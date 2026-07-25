//! Duplicate-detection engine shared by the CLI and the Windows GUI.
//!
//! Staged pipeline, matching TwinTidy's Go engine:
//!
//! ```text
//! surface scan  -> inventory of user-created files, with category stats
//! size mapping  -> only same-size files can be duplicates
//! boundary hash -> cheap head+tail comparison rejects most candidates
//! full hash     -> SHA-256 over the whole file
//! confirmation  -> byte-for-byte compare, so a hash collision cannot
//!                  produce a false duplicate
//! ```
//!
//! Read-only: nothing here deletes, moves, or modifies a file.
//! Standard library only — no external crates.

pub mod safety;
pub mod sha256;

pub use safety::{category_for_path, is_user_created_file, should_skip_directory, Category};

use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

const BOUNDARY_READ_SIZE: usize = 4096;
const HASH_BUFFER_SIZE: usize = 65536;
const DEFAULT_MAX_DIRECTORIES: usize = 250_000;
const DEFAULT_MAX_FILES: usize = 500_000;

/// One file in the surface inventory.
#[derive(Clone, Debug)]
pub struct FileRecord {
    pub path: PathBuf,
    pub size: u64,
    /// Seconds since the Unix epoch; zero when unavailable.
    pub modified_at: i64,
    pub category: Category,
}

/// Files whose complete contents matched.
#[derive(Clone, Debug)]
pub struct DuplicateGroup {
    pub size: u64,
    pub hash: String,
    pub files: Vec<FileRecord>,
}

/// Per-category totals produced by the surface scan.
#[derive(Clone, Copy, Debug, Default)]
pub struct CategoryStats {
    pub files: usize,
    pub bytes: u64,
}

/// Inventory of candidate files, produced before any hashing.
#[derive(Clone, Debug, Default)]
pub struct SurfaceReport {
    pub files: Vec<FileRecord>,
    pub total_bytes: u64,
    pub directories_scanned: usize,
    pub skipped_system_items: usize,
    pub errors_ignored: usize,
    pub category_stats: Vec<(Category, CategoryStats)>,
}

impl SurfaceReport {
    pub fn stats_for(&self, category: Category) -> CategoryStats {
        self.category_stats
            .iter()
            .find(|(candidate, _)| *candidate == category)
            .map(|(_, stats)| *stats)
            .unwrap_or_default()
    }
}

/// Outcome of duplicate detection.
#[derive(Clone, Debug, Default)]
pub struct ScanResult {
    pub groups: Vec<DuplicateGroup>,
    /// Bytes freed by keeping one copy per group.
    pub reclaimable: u64,
    pub files_considered: usize,
    pub files_unreadable: usize,
}

/// Caller-supplied scan policy. [`Options::default`] applies no extra filtering.
#[derive(Clone, Debug)]
pub struct Options {
    /// Categories to include. Empty means every category.
    pub categories: Vec<Category>,
    pub min_file_size: u64,
    pub excluded_paths: Vec<PathBuf>,
    /// Lower-case, dot-prefixed extensions.
    pub excluded_extensions: Vec<String>,
    pub max_directories: usize,
    pub max_files: usize,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            categories: Vec::new(),
            min_file_size: 0,
            excluded_paths: Vec::new(),
            excluded_extensions: Vec::new(),
            max_directories: DEFAULT_MAX_DIRECTORIES,
            max_files: DEFAULT_MAX_FILES,
        }
    }
}

impl Options {
    /// Canonicalize filter inputs so later matching is deterministic.
    pub fn normalized(mut self) -> Self {
        self.excluded_extensions = self
            .excluded_extensions
            .iter()
            .filter_map(|value| {
                let trimmed = value.trim().to_ascii_lowercase();
                if trimmed.is_empty() || trimmed == "." {
                    return None;
                }
                Some(if trimmed.starts_with('.') {
                    trimmed
                } else {
                    format!(".{trimmed}")
                })
            })
            .collect();
        if self.max_directories == 0 {
            self.max_directories = DEFAULT_MAX_DIRECTORIES;
        }
        if self.max_files == 0 {
            self.max_files = DEFAULT_MAX_FILES;
        }
        self
    }

    fn includes_category(&self, category: Category) -> bool {
        self.categories.is_empty() || self.categories.contains(&category)
    }

    fn passes_filters(&self, record: &FileRecord) -> bool {
        if self.min_file_size > 0 && record.size < self.min_file_size {
            return false;
        }
        if !self.excluded_extensions.is_empty() {
            let extension = record
                .path
                .extension()
                .map(|value| format!(".{}", value.to_string_lossy().to_ascii_lowercase()))
                .unwrap_or_default();
            if self.excluded_extensions.contains(&extension) {
                return false;
            }
        }
        !self
            .excluded_paths
            .iter()
            .any(|prefix| record.path.starts_with(prefix))
    }
}

/// Progress stage reported to the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    Surface,
    BoundaryHashing,
    FullHashing,
    Done,
}

impl Stage {
    pub fn label(self) -> &'static str {
        match self {
            Stage::Surface => "Surface scan",
            Stage::BoundaryHashing => "Boundary hashing",
            Stage::FullHashing => "Full hashing",
            Stage::Done => "Done",
        }
    }
}

/// Returned by a progress callback to continue or stop the scan.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Flow {
    Continue,
    Cancel,
}

#[derive(Debug, PartialEq, Eq)]
pub enum ScanError {
    Cancelled,
    /// The chosen root is a protected system or build location.
    ProtectedRoot,
    /// A cardinality cap was reached; narrow the scan and retry.
    LimitExceeded,
}

/// Phase one: inventory the user-created files under `root`. Protected
/// directories and protected extensions are excluded here, so later phases
/// never see them.
pub fn surface_scan<F>(
    root: &Path,
    options: &Options,
    mut progress: F,
) -> Result<SurfaceReport, ScanError>
where
    F: FnMut(Stage, usize, usize) -> Flow,
{
    if should_skip_directory(root) {
        // The user explicitly chose this root, but it is a protected system
        // or build location; refuse rather than silently returning nothing.
        return Err(ScanError::ProtectedRoot);
    }

    let mut report = SurfaceReport::default();
    let mut stats: HashMap<Category, CategoryStats> = HashMap::new();
    walk(root, options, &mut report, &mut stats, &mut progress)?;

    let mut ordered: Vec<(Category, CategoryStats)> = Category::ALL
        .iter()
        .filter_map(|category| stats.get(category).map(|value| (*category, *value)))
        .collect();
    ordered.sort_by_key(|(category, _)| *category);
    report.category_stats = ordered;

    let total = report.files.len();
    progress(Stage::Surface, total, total);
    Ok(report)
}

fn walk<F>(
    dir: &Path,
    options: &Options,
    report: &mut SurfaceReport,
    stats: &mut HashMap<Category, CategoryStats>,
    progress: &mut F,
) -> Result<(), ScanError>
where
    F: FnMut(Stage, usize, usize) -> Flow,
{
    if report.directories_scanned >= options.max_directories {
        return Err(ScanError::LimitExceeded);
    }
    let entries = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(_) => {
            report.errors_ignored += 1;
            return Ok(());
        }
    };
    report.directories_scanned += 1;

    for entry in entries.flatten() {
        if progress(Stage::Surface, report.files.len(), 0) == Flow::Cancel {
            return Err(ScanError::Cancelled);
        }
        let path = entry.path();
        let meta = match fs::symlink_metadata(&path) {
            Ok(meta) => meta,
            Err(_) => {
                report.errors_ignored += 1;
                continue;
            }
        };
        if meta.file_type().is_symlink() {
            // Symlinks are never followed: they can escape the scan root and
            // create traversal cycles.
            report.skipped_system_items += 1;
        } else if meta.is_dir() {
            if should_skip_directory(&path) {
                report.skipped_system_items += 1;
            } else {
                walk(&path, options, report, stats, progress)?;
            }
        } else if meta.is_file() {
            if !is_user_created_file(&path) {
                report.skipped_system_items += 1;
                continue;
            }
            if report.files.len() >= options.max_files {
                return Err(ScanError::LimitExceeded);
            }
            let category = category_for_path(&path);
            let size = meta.len();
            let modified_at = meta
                .modified()
                .ok()
                .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
                .map(|value| value.as_secs() as i64)
                .unwrap_or(0);

            report.total_bytes += size;
            let slot = stats.entry(category).or_default();
            slot.files += 1;
            slot.bytes += size;

            report.files.push(FileRecord {
                path,
                size,
                modified_at,
                category,
            });
        }
    }
    Ok(())
}

/// Phase two: find duplicates among an inventory, honouring the category
/// selection and filters in `options`.
pub fn find_duplicates<F>(
    surface: &SurfaceReport,
    options: &Options,
    mut progress: F,
) -> Result<ScanResult, ScanError>
where
    F: FnMut(Stage, usize, usize) -> Flow,
{
    let candidates: Vec<&FileRecord> = surface
        .files
        .iter()
        .filter(|record| {
            options.includes_category(record.category) && options.passes_filters(record)
        })
        .collect();

    let mut result = ScanResult {
        files_considered: candidates.len(),
        ..Default::default()
    };
    if candidates.is_empty() {
        progress(Stage::Done, 0, 0);
        return Ok(result);
    }

    // 1) group by exact size, ignoring empty files
    let mut by_size: HashMap<u64, Vec<&FileRecord>> = HashMap::new();
    for record in &candidates {
        if record.size > 0 {
            by_size.entry(record.size).or_default().push(record);
        }
    }
    let mut sizes: Vec<u64> = by_size
        .iter()
        .filter(|(_, group)| group.len() >= 2)
        .map(|(&size, _)| size)
        .collect();
    sizes.sort_unstable();

    let total: usize = sizes.iter().map(|size| by_size[size].len()).sum();
    let mut processed = 0usize;

    for size in sizes {
        let group = &by_size[&size];

        // 2) boundary hashing rejects most same-size candidates cheaply
        let mut by_boundary: HashMap<u64, Vec<&FileRecord>> = HashMap::new();
        for record in group {
            if progress(Stage::BoundaryHashing, processed, total) == Flow::Cancel {
                return Err(ScanError::Cancelled);
            }
            processed += 1;
            match boundary_hash(&record.path, record.size) {
                Ok(hash) => by_boundary.entry(hash).or_default().push(record),
                Err(_) => result.files_unreadable += 1,
            }
        }

        // 3) full SHA-256 only for files sharing a boundary hash
        for bucket in by_boundary.values() {
            if bucket.len() < 2 {
                continue;
            }
            let mut by_hash: HashMap<String, Vec<&FileRecord>> = HashMap::new();
            for record in bucket {
                if progress(Stage::FullHashing, processed, total) == Flow::Cancel {
                    return Err(ScanError::Cancelled);
                }
                match full_hash(&record.path) {
                    Ok(hash) => by_hash.entry(hash).or_default().push(record),
                    Err(_) => result.files_unreadable += 1,
                }
            }

            // 4) confirm byte-for-byte, then record the group
            for (hash, matches) in by_hash {
                if matches.len() < 2 {
                    continue;
                }
                let mut claimed = vec![false; matches.len()];
                for position in 0..matches.len() {
                    if claimed[position] {
                        continue;
                    }
                    let mut members = vec![matches[position]];
                    for other in (position + 1)..matches.len() {
                        if claimed[other] {
                            continue;
                        }
                        if let Ok(true) = same_bytes(&matches[position].path, &matches[other].path)
                        {
                            claimed[other] = true;
                            members.push(matches[other]);
                        }
                    }
                    if members.len() >= 2 {
                        result.reclaimable += size * (members.len() as u64 - 1);
                        result.groups.push(DuplicateGroup {
                            size,
                            hash: hash.clone(),
                            files: members.into_iter().cloned().collect(),
                        });
                    }
                }
            }
        }
    }

    result.groups.sort_by(|left, right| {
        left.size
            .cmp(&right.size)
            .then_with(|| left.hash.cmp(&right.hash))
    });
    progress(Stage::Done, total, total);
    Ok(result)
}

/// Convenience: surface scan followed by duplicate detection.
pub fn scan<F>(root: &Path, options: &Options, mut progress: F) -> Result<ScanResult, ScanError>
where
    F: FnMut(Stage, usize, usize) -> Flow,
{
    let surface = surface_scan(root, options, &mut progress)?;
    find_duplicates(&surface, options, &mut progress)
}

/// FNV-1a over the head and tail of a file: a cheap pre-filter that rejects
/// most same-size candidates without reading whole files.
fn boundary_hash(path: &Path, size: u64) -> io::Result<u64> {
    let mut file = File::open(path)?;
    let want = size.min(BOUNDARY_READ_SIZE as u64) as usize;
    let mut head = vec![0u8; want];
    file.read_exact(&mut head)?;

    let mut tail = vec![0u8; want];
    if size > BOUNDARY_READ_SIZE as u64 {
        file.seek(SeekFrom::Start(size - want as u64))?;
    } else {
        file.seek(SeekFrom::Start(0))?;
    }
    file.read_exact(&mut tail)?;

    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    let mut mix = |byte: u8| {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x100_0000_01b3);
    };
    for byte in size.to_le_bytes() {
        mix(byte);
    }
    for &byte in &head {
        mix(byte);
    }
    for &byte in &tail {
        mix(byte);
    }
    Ok(hash)
}

/// Streamed SHA-256 of a whole file, as lower-case hex.
fn full_hash(path: &Path) -> io::Result<String> {
    let mut file = File::open(path)?;
    let mut hasher = sha256::Sha256::new();
    let mut buffer = vec![0u8; HASH_BUFFER_SIZE];
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(sha256::to_hex(&hasher.finish()))
}

/// Byte-for-byte comparison: the final arbiter, immune to hash collisions.
fn same_bytes(left: &Path, right: &Path) -> io::Result<bool> {
    let mut left_file = File::open(left)?;
    let mut right_file = File::open(right)?;
    let mut left_buffer = vec![0u8; HASH_BUFFER_SIZE];
    let mut right_buffer = vec![0u8; HASH_BUFFER_SIZE];
    loop {
        let left_read = read_chunk(&mut left_file, &mut left_buffer)?;
        let right_read = read_chunk(&mut right_file, &mut right_buffer)?;
        if left_read != right_read || left_buffer[..left_read] != right_buffer[..right_read] {
            return Ok(false);
        }
        if left_read == 0 {
            return Ok(true);
        }
    }
}

/// Fill `buffer` as far as the file allows, retrying short reads so both
/// sides of a comparison advance in lockstep.
fn read_chunk(file: &mut File, buffer: &mut [u8]) -> io::Result<usize> {
    let mut filled = 0;
    while filled < buffer.len() {
        let read = file.read(&mut buffer[filled..])?;
        if read == 0 {
            break;
        }
        filled += read;
    }
    Ok(filled)
}

/// Human-readable byte size, e.g. `1.5 MB`.
pub fn format_bytes(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    let mut value = bytes as f64;
    let mut unit = 0;
    while value >= 1024.0 && unit + 1 < UNITS.len() {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 {
        format!("{bytes} B")
    } else {
        format!("{value:.1} {}", UNITS[unit])
    }
}

/// Format a Unix timestamp as `YYYY-MM-DD HH:MM`, or an empty string when
/// unavailable. Implemented locally so the crate stays dependency-free.
pub fn format_timestamp(seconds: i64) -> String {
    if seconds <= 0 {
        return String::new();
    }
    let time = UNIX_EPOCH + std::time::Duration::from_secs(seconds as u64);
    let total = match time.duration_since(UNIX_EPOCH) {
        Ok(value) => value.as_secs() as i64,
        Err(_) => return String::new(),
    };
    let days = total / 86_400;
    let seconds_of_day = total % 86_400;
    let (year, month, day) = civil_from_days(days);
    format!(
        "{year:04}-{month:02}-{day:02} {:02}:{:02}",
        seconds_of_day / 3600,
        (seconds_of_day % 3600) / 60
    )
}

/// Howard Hinnant's days-from-civil inverse, for calendar formatting.
fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let day_of_era = z - era * 146_097;
    let year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let year = year_of_era + era * 400;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let mp = (5 * day_of_year + 2) / 153;
    let day = (day_of_year - (153 * mp + 2) / 5 + 1) as u32;
    let month = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if month <= 2 { year + 1 } else { year }, month, day)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn format_bytes_scales_units() {
        assert_eq!(format_bytes(512), "512 B");
        assert_eq!(format_bytes(2048), "2.0 KB");
        assert_eq!(format_bytes(5 * 1024 * 1024), "5.0 MB");
    }

    #[test]
    fn format_timestamp_renders_known_epoch() {
        // 2026-07-25 00:00 UTC
        assert_eq!(format_timestamp(1_784_937_600), "2026-07-25 00:00");
        assert_eq!(format_timestamp(0), "");
    }

    #[test]
    fn options_normalize_extensions() {
        let options = Options {
            excluded_extensions: vec![" TMP ".into(), ".log".into(), "".into(), ".".into()],
            ..Default::default()
        }
        .normalized();
        assert_eq!(options.excluded_extensions, vec![".tmp", ".log"]);
    }
}
