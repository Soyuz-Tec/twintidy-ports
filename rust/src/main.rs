//! twintidy.rs — minimal Rust port of TwinTidy's duplicate-detection core.
//!
//! Strategy (same as the Go engine):
//!   1. Walk the tree, collect regular files (symlinks skipped, so no cycles).
//!   2. Group by exact size (different size => not duplicates).
//!   3. Within a size group, hash each file (FNV-1a 64, streamed).
//!   4. Within a hash group, confirm with a full byte-for-byte compare,
//!      so a hash collision can never produce a false duplicate.
//!
//! Read-only: this program never deletes, moves, or modifies anything.
//!
//! Standard library only — no external crates.
//!
//! Build:  rustc -O twintidy.rs        (or drop into a cargo bin crate)
//! Usage:  twintidy <folder>

use std::collections::HashMap;
use std::env;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

struct Entry {
    path: PathBuf,
    size: u64,
}

/// Recursively walk `dir`, collecting regular files. Symlinks are
/// skipped via `symlink_metadata` so cycles cannot occur. Unreadable
/// directories are warned about and skipped, never fatal.
fn walk(dir: &Path, out: &mut Vec<Entry>) {
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => {
            eprintln!("warn: cannot open {}", dir.display());
            return;
        }
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let meta = match fs::symlink_metadata(&path) {
            Ok(m) => m,
            Err(_) => continue,
        };
        if meta.file_type().is_symlink() {
            continue;
        } else if meta.is_dir() {
            walk(&path, out);
        } else if meta.is_file() {
            out.push(Entry {
                path,
                size: meta.len(),
            });
        }
    }
}

/// Streamed FNV-1a 64-bit hash of a whole file.
fn fnv1a_file(path: &Path) -> io::Result<u64> {
    let mut f = File::open(path)?;
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    let mut buf = [0u8; 1 << 16];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        for &b in &buf[..n] {
            h ^= u64::from(b);
            h = h.wrapping_mul(0x100_0000_01b3);
        }
    }
    Ok(h)
}

/// Byte-for-byte comparison; the final arbiter, immune to collisions.
fn same_bytes(a: &Path, b: &Path) -> io::Result<bool> {
    let mut fa = File::open(a)?;
    let mut fb = File::open(b)?;
    let mut ba = [0u8; 1 << 16];
    let mut bb = [0u8; 1 << 16];
    loop {
        let na = fa.read(&mut ba)?;
        let nb = read_exactly(&mut fb, &mut bb[..na.max(1)], na)?;
        if na != nb || ba[..na] != bb[..nb] {
            return Ok(false);
        }
        if na == 0 {
            return Ok(true);
        }
    }
}

/// Read up to `want` bytes, retrying short reads, so both sides of the
/// comparison advance in lockstep even if the OS returns partial reads.
fn read_exactly(f: &mut File, buf: &mut [u8], want: usize) -> io::Result<usize> {
    let mut got = 0;
    while got < want {
        let n = f.read(&mut buf[got..want])?;
        if n == 0 {
            break;
        }
        got += n;
    }
    Ok(got)
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: {} <folder>", args[0]);
        return ExitCode::from(2);
    }

    let mut files = Vec::new();
    walk(Path::new(&args[1]), &mut files);
    if files.is_empty() {
        println!("No files found.");
        return ExitCode::SUCCESS;
    }

    // 1) group by exact size, skipping empty files
    let mut by_size: HashMap<u64, Vec<usize>> = HashMap::new();
    for (i, e) in files.iter().enumerate() {
        if e.size > 0 {
            by_size.entry(e.size).or_default().push(i);
        }
    }

    let mut sizes: Vec<u64> = by_size
        .iter()
        .filter(|(_, v)| v.len() >= 2)
        .map(|(&s, _)| s)
        .collect();
    sizes.sort_unstable();

    let mut groups = 0u32;
    let mut reclaimable = 0u64;

    for size in sizes {
        let candidates = &by_size[&size];

        // 2) hash candidates within the size group
        let mut by_hash: HashMap<u64, Vec<usize>> = HashMap::new();
        for &i in candidates {
            match fnv1a_file(&files[i].path) {
                Ok(h) => by_hash.entry(h).or_default().push(i),
                Err(_) => eprintln!("warn: cannot read {} (skipped)", files[i].path.display()),
            }
        }

        // 3) confirm byte-for-byte within each hash group
        for bucket in by_hash.values() {
            if bucket.len() < 2 {
                continue;
            }
            let mut claimed = vec![false; bucket.len()];
            for k in 0..bucket.len() {
                if claimed[k] {
                    continue;
                }
                let mut members = vec![bucket[k]];
                for l in (k + 1)..bucket.len() {
                    if claimed[l] {
                        continue;
                    }
                    if let Ok(true) = same_bytes(&files[bucket[k]].path, &files[bucket[l]].path) {
                        claimed[l] = true;
                        members.push(bucket[l]);
                    }
                }
                if members.len() >= 2 {
                    groups += 1;
                    reclaimable += size * (members.len() as u64 - 1);
                    println!(
                        "\nDuplicate group {}  ({} bytes each, {} files)",
                        groups,
                        size,
                        members.len()
                    );
                    for &m in &members {
                        println!("  {}", files[m].path.display());
                    }
                }
            }
        }
    }

    if groups == 0 {
        println!("No duplicates found.");
    } else {
        println!(
            "\n{} duplicate group(s); keeping one copy of each would reclaim {} bytes.",
            groups, reclaimable
        );
    }
    ExitCode::SUCCESS
}
