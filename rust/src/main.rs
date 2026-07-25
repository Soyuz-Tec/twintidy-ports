//! Command-line front end for the TwinTidy Rust port.
//!
//! Read-only: reports duplicate groups and never modifies the scanned tree.
//! The detection engine lives in `lib.rs` and is shared with the Windows GUI.

use std::env;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use twintidy::{find_duplicates, surface_scan, Category, Flow, Options, ScanError};

fn print_usage(program: &str) {
    eprintln!("usage: {program} [options] <folder>\n");
    eprintln!("Find exact duplicate files. The scan is read-only.\n");
    eprintln!("Options:");
    eprintln!("  --min-size BYTES     ignore files smaller than BYTES");
    eprintln!("  --exclude PATH       skip this path subtree (repeatable)");
    eprintln!("  --exclude-ext EXT    skip this extension, e.g. .tmp (repeatable)");
    eprintln!("  --category NAME      restrict to a category (repeatable);");
    eprintln!("                       pdf text word excel powerpoint images");
    eprintln!("                       audio video archives other");
    eprintln!("  --surface            report the surface inventory and exit");
    eprintln!("  --help               show this help\n");
    eprintln!("Exit codes: 0 no duplicates, 1 duplicates found, 2 error.");
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    let mut options = Options::default();
    let mut root: Option<PathBuf> = None;
    let mut surface_only = false;

    let mut index = 1;
    while index < args.len() {
        let argument = args[index].as_str();
        let take_value = |index: &mut usize| -> Option<String> {
            if *index + 1 < args.len() {
                *index += 1;
                Some(args[*index].clone())
            } else {
                None
            }
        };
        match argument {
            "--help" | "-h" => {
                print_usage(&args[0]);
                return ExitCode::SUCCESS;
            }
            "--surface" => surface_only = true,
            "--min-size" => match take_value(&mut index).and_then(|v| v.parse::<u64>().ok()) {
                Some(value) => options.min_file_size = value,
                None => {
                    eprintln!("--min-size requires a byte count");
                    return ExitCode::from(2);
                }
            },
            "--exclude" => match take_value(&mut index) {
                Some(value) => options.excluded_paths.push(PathBuf::from(value)),
                None => {
                    eprintln!("--exclude requires a path");
                    return ExitCode::from(2);
                }
            },
            "--exclude-ext" => match take_value(&mut index) {
                Some(value) => options.excluded_extensions.push(value),
                None => {
                    eprintln!("--exclude-ext requires an extension");
                    return ExitCode::from(2);
                }
            },
            "--category" => match take_value(&mut index) {
                Some(value) => match Category::from_name(&value) {
                    Some(category) => options.categories.push(category),
                    None => {
                        eprintln!("unknown category: {value}");
                        return ExitCode::from(2);
                    }
                },
                None => {
                    eprintln!("--category requires a name");
                    return ExitCode::from(2);
                }
            },
            other if other.starts_with('-') => {
                eprintln!("unknown option: {other}");
                print_usage(&args[0]);
                return ExitCode::from(2);
            }
            other => {
                if root.is_some() {
                    eprintln!("only one folder may be scanned");
                    return ExitCode::from(2);
                }
                root = Some(PathBuf::from(other));
            }
        }
        index += 1;
    }

    let Some(root) = root else {
        print_usage(&args[0]);
        return ExitCode::from(2);
    };
    let options = options.normalized();

    let surface = match surface_scan(Path::new(&root), &options, |_, _, _| Flow::Continue) {
        Ok(report) => report,
        Err(ScanError::ProtectedRoot) => {
            eprintln!(
                "cannot scan {}: the folder is a protected system or build location",
                root.display()
            );
            return ExitCode::from(2);
        }
        Err(ScanError::LimitExceeded) => {
            eprintln!("scan limit exceeded; select a smaller folder");
            return ExitCode::from(2);
        }
        Err(ScanError::Cancelled) => return ExitCode::from(2),
    };

    if surface_only {
        println!("Surface inventory for {}", root.display());
        println!(
            "  {} user file(s), {} byte(s) in {} folder(s)",
            surface.files.len(),
            surface.total_bytes,
            surface.directories_scanned
        );
        println!(
            "  {} protected item(s) skipped, {} unreadable\n",
            surface.skipped_system_items, surface.errors_ignored
        );
        for (category, stats) in &surface.category_stats {
            println!(
                "  {:<12} {:>6} file(s)  {} byte(s)",
                category.label(),
                stats.files,
                stats.bytes
            );
        }
        return ExitCode::SUCCESS;
    }

    let result = match find_duplicates(&surface, &options, |_, _, _| Flow::Continue) {
        Ok(result) => result,
        Err(_) => {
            eprintln!("duplicate scan failed");
            return ExitCode::from(2);
        }
    };

    for (index, group) in result.groups.iter().enumerate() {
        println!(
            "\nDuplicate group {}  ({} bytes each, {} files)",
            index + 1,
            group.size,
            group.files.len()
        );
        println!("  sha256 {}", group.hash);
        for file in &group.files {
            println!("  {}", file.path.display());
        }
    }

    let exit_code = if result.groups.is_empty() {
        println!(
            "No duplicates found among {} considered file(s).",
            result.files_considered
        );
        ExitCode::SUCCESS
    } else {
        println!(
            "\n{} duplicate group(s); keeping one copy of each would reclaim {} bytes.",
            result.groups.len(),
            result.reclaimable
        );
        ExitCode::from(1)
    };
    if result.files_unreadable > 0 {
        eprintln!(
            "warn: {} file(s) could not be read and were skipped",
            result.files_unreadable
        );
    }
    exit_code
}
