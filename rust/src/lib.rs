//! Duplicate-detection engine shared by the CLI and the Windows GUI.
//!
//! Strategy (same as the Go engine):
//!   1. Walk the tree, collecting regular files (symlinks skipped, so no cycles).
//!   2. Group by exact size — different size means not duplicates.
//!   3. Within a size group, hash each file (FNV-1a 64, streamed).
//!   4. Within a hash group, confirm byte-for-byte, so a hash collision can
//!      never produce a false duplicate.
//!
//! Read-only: nothing here deletes, moves, or modifies a file.
//!
//! Standard library only — no external crates.

use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};

/// One member of a confirmed duplicate group.
#[derive(Clone, Debug)]
pub struct DuplicateFile {
    pub path: PathBuf,
    pub size: u64,
}

/// Files whose complete contents matched.
#[derive(Clone, Debug)]
pub struct DuplicateGroup {
    pub size: u64,
    pub files: Vec<DuplicateFile>,
}

/// Outcome of one scan.
#[derive(Clone, Debug, Default)]
pub struct ScanResult {
    pub groups: Vec<DuplicateGroup>,
    /// Bytes freed by keeping one copy per group.
    pub reclaimable: u64,
    pub files_scanned: usize,
    pub files_unreadable: usize,
}

/// Progress stage reported to the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    /// Walking directories; the total is not yet known.
    Walking,
    /// Hashing and comparing candidates.
    Comparing,
    Done,
}

impl Stage {
    pub fn label(self) -> &'static str {
        match self {
            Stage::Walking => "Scanning folders",
            Stage::Comparing => "Comparing files",
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

#[derive(Debug)]
pub enum ScanError {
    Cancelled,
}

struct Entry {
    path: PathBuf,
    size: u64,
}

/// Scan `root` recursively and return confirmed duplicate groups.
///
/// `progress` is called with the current stage, work done, and total work
/// (zero while walking). Returning [`Flow::Cancel`] stops the scan promptly
/// and yields [`ScanError::Cancelled`].
pub fn scan<F>(root: &Path, mut progress: F) -> Result<ScanResult, ScanError>
where
    F: FnMut(Stage, usize, usize) -> Flow,
{
    let mut files = Vec::new();
    let mut unreadable = 0usize;
    walk(root, &mut files, &mut unreadable, &mut progress)?;

    let mut result = ScanResult {
        files_scanned: files.len(),
        ..Default::default()
    };
    if files.is_empty() {
        result.files_unreadable = unreadable;
        return Ok(result);
    }

    // 1) group by exact size, ignoring empty files
    let mut by_size: HashMap<u64, Vec<usize>> = HashMap::new();
    for (index, entry) in files.iter().enumerate() {
        if entry.size > 0 {
            by_size.entry(entry.size).or_default().push(index);
        }
    }
    let mut sizes: Vec<u64> = by_size
        .iter()
        .filter(|(_, group)| group.len() >= 2)
        .map(|(&size, _)| size)
        .collect();
    sizes.sort_unstable();

    let candidate_total: usize = sizes.iter().map(|size| by_size[size].len()).sum();
    let mut processed = 0usize;

    for size in sizes {
        let candidates = &by_size[&size];

        // 2) hash candidates within the size group
        let mut by_hash: HashMap<u64, Vec<usize>> = HashMap::new();
        for &index in candidates {
            if progress(Stage::Comparing, processed, candidate_total) == Flow::Cancel {
                return Err(ScanError::Cancelled);
            }
            processed += 1;
            match fnv1a_file(&files[index].path) {
                Ok(hash) => by_hash.entry(hash).or_default().push(index),
                Err(_) => unreadable += 1,
            }
        }

        // 3) confirm byte-for-byte within each hash bucket
        for bucket in by_hash.values() {
            if bucket.len() < 2 {
                continue;
            }
            let mut claimed = vec![false; bucket.len()];
            for position in 0..bucket.len() {
                if claimed[position] {
                    continue;
                }
                let mut members = vec![bucket[position]];
                for other in (position + 1)..bucket.len() {
                    if claimed[other] {
                        continue;
                    }
                    if let Ok(true) =
                        same_bytes(&files[bucket[position]].path, &files[bucket[other]].path)
                    {
                        claimed[other] = true;
                        members.push(bucket[other]);
                    }
                }
                if members.len() >= 2 {
                    result.reclaimable += size * (members.len() as u64 - 1);
                    result.groups.push(DuplicateGroup {
                        size,
                        files: members
                            .iter()
                            .map(|&index| DuplicateFile {
                                path: files[index].path.clone(),
                                size,
                            })
                            .collect(),
                    });
                }
            }
        }
    }

    result.groups.sort_by_key(|group| group.size);
    result.files_unreadable = unreadable;
    progress(Stage::Done, candidate_total, candidate_total);
    Ok(result)
}

/// Recursively collect regular files. Symlinks are skipped via
/// `symlink_metadata`, so traversal cycles are impossible. Unreadable
/// directories are counted and skipped rather than aborting the scan.
fn walk<F>(
    dir: &Path,
    out: &mut Vec<Entry>,
    unreadable: &mut usize,
    progress: &mut F,
) -> Result<(), ScanError>
where
    F: FnMut(Stage, usize, usize) -> Flow,
{
    let entries = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(_) => {
            *unreadable += 1;
            return Ok(());
        }
    };
    for entry in entries.flatten() {
        if progress(Stage::Walking, out.len(), 0) == Flow::Cancel {
            return Err(ScanError::Cancelled);
        }
        let path = entry.path();
        let meta = match fs::symlink_metadata(&path) {
            Ok(meta) => meta,
            Err(_) => {
                *unreadable += 1;
                continue;
            }
        };
        if meta.file_type().is_symlink() {
            continue;
        } else if meta.is_dir() {
            walk(&path, out, unreadable, progress)?;
        } else if meta.is_file() {
            out.push(Entry {
                path,
                size: meta.len(),
            });
        }
    }
    Ok(())
}

/// Streamed FNV-1a 64-bit hash of a whole file.
fn fnv1a_file(path: &Path) -> io::Result<u64> {
    let mut file = File::open(path)?;
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    let mut buffer = [0u8; 1 << 16];
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        for &byte in &buffer[..read] {
            hash ^= u64::from(byte);
            hash = hash.wrapping_mul(0x100_0000_01b3);
        }
    }
    Ok(hash)
}

/// Byte-for-byte comparison: the final arbiter, immune to hash collisions.
fn same_bytes(left: &Path, right: &Path) -> io::Result<bool> {
    let mut left_file = File::open(left)?;
    let mut right_file = File::open(right)?;
    let mut left_buffer = [0u8; 1 << 16];
    let mut right_buffer = [0u8; 1 << 16];
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
