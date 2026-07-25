//! Command-line front end for the TwinTidy Rust port.
//!
//! Read-only: reports duplicate groups and never modifies the scanned tree.
//! The detection engine lives in `lib.rs` and is shared with the Windows GUI.
//!
//! Usage: twintidy <folder>

use std::env;
use std::path::Path;
use std::process::ExitCode;

use twintidy::{scan, Flow, ScanError};

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: {} <folder>", args[0]);
        return ExitCode::from(2);
    }

    let result = match scan(Path::new(&args[1]), |_, _, _| Flow::Continue) {
        Ok(result) => result,
        Err(ScanError::Cancelled) => {
            eprintln!("scan cancelled");
            return ExitCode::from(2);
        }
    };

    if result.files_scanned == 0 {
        println!("No files found.");
        return ExitCode::SUCCESS;
    }

    for (index, group) in result.groups.iter().enumerate() {
        println!(
            "\nDuplicate group {}  ({} bytes each, {} files)",
            index + 1,
            group.size,
            group.files.len()
        );
        for file in &group.files {
            println!("  {}", file.path.display());
        }
    }

    if result.groups.is_empty() {
        println!("No duplicates found.");
    } else {
        println!(
            "\n{} duplicate group(s); keeping one copy of each would reclaim {} bytes.",
            result.groups.len(),
            result.reclaimable
        );
    }
    if result.files_unreadable > 0 {
        eprintln!(
            "warn: {} item(s) could not be read and were skipped",
            result.files_unreadable
        );
    }
    ExitCode::SUCCESS
}
