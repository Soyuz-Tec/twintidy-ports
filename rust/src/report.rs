//! Duplicate-report export, matching the Go engine's formats.
//!
//! CSV cells that spreadsheet software would evaluate as formulas are
//! neutralized, so an exported report cannot execute anything when opened.

use std::fmt::Write as _;
use std::fs;
use std::io;
use std::path::Path;

use crate::{format_timestamp, ScanResult};

pub const SCHEMA: &str = "twintidy.duplicate-report/v1";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format {
    Csv,
    Json,
}

impl Format {
    pub fn extension(self) -> &'static str {
        match self {
            Format::Csv => ".csv",
            Format::Json => ".json",
        }
    }

    pub fn from_name(name: &str) -> Option<Format> {
        match name.trim().to_ascii_lowercase().as_str() {
            "csv" => Some(Format::Csv),
            "json" => Some(Format::Json),
            _ => None,
        }
    }
}

/// Spreadsheet software evaluates a cell beginning with one of these as a
/// formula or command. Prefixing with an apostrophe keeps the text visible
/// while preventing evaluation.
fn needs_formula_guard(value: &str) -> bool {
    matches!(
        value.chars().next(),
        Some('=') | Some('+') | Some('-') | Some('@') | Some('\t') | Some('\r')
    )
}

fn csv_cell(value: &str) -> String {
    let mut cell = String::with_capacity(value.len() + 2);
    cell.push('"');
    if needs_formula_guard(value) {
        cell.push('\'');
    }
    for character in value.chars() {
        if character == '"' {
            cell.push('"'); // RFC 4180 doubling
        }
        cell.push(character);
    }
    cell.push('"');
    cell
}

fn json_string(value: &str) -> String {
    let mut out = String::with_capacity(value.len() + 2);
    out.push('"');
    for character in value.chars() {
        match character {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// Render the report for `result`. `folder` labels the scan root.
/// `generated_at` is a Unix timestamp so callers control the clock.
pub fn render(format: Format, folder: &str, result: &ScanResult, generated_at: i64) -> String {
    match format {
        Format::Csv => render_csv(folder, result, generated_at),
        Format::Json => render_json(folder, result, generated_at),
    }
}

fn render_csv(folder: &str, result: &ScanResult, generated_at: i64) -> String {
    let stamp = format_timestamp(generated_at);
    let mut out = String::new();
    out.push_str(
        "generatedAt,scanFolder,group,sha256,groupSize,groupReclaimableBytes,\
         reportReclaimableBytes,path,fileSize,modifiedAt,category\r\n",
    );

    let mut report_total_written = false;
    for (index, group) in result.groups.iter().enumerate() {
        let group_reclaimable = if group.files.len() >= 2 {
            group.size * (group.files.len() as u64 - 1)
        } else {
            0
        };
        for (position, file) in group.files.iter().enumerate() {
            // Per-group and per-report totals are emitted once each, so a
            // reader can sum the file rows without double counting.
            let group_cell = if position == 0 {
                group_reclaimable.to_string()
            } else {
                String::new()
            };
            let report_cell = if report_total_written {
                String::new()
            } else {
                report_total_written = true;
                result.reclaimable.to_string()
            };
            let _ = writeln!(
                out,
                "{},{},{},{},{},{},{},{},{},{},{}\r",
                csv_cell(&stamp),
                csv_cell(folder),
                index + 1,
                csv_cell(&group.hash),
                group.size,
                group_cell,
                report_cell,
                csv_cell(&file.path.to_string_lossy()),
                file.size,
                csv_cell(&format_timestamp(file.modified_at)),
                csv_cell(file.category.label()),
            );
        }
    }
    out
}

fn render_json(folder: &str, result: &ScanResult, generated_at: i64) -> String {
    let file_count: usize = result.groups.iter().map(|group| group.files.len()).sum();
    let mut out = String::new();
    let _ = write!(
        out,
        "{{\n  \"schema\": {},\n  \"generatedAt\": {}",
        json_string(SCHEMA),
        json_string(&format_timestamp(generated_at))
    );
    if !folder.is_empty() {
        let _ = write!(out, ",\n  \"folder\": {}", json_string(folder));
    }
    let _ = write!(
        out,
        ",\n  \"groupCount\": {},\n  \"fileCount\": {},\n  \"reclaimableBytes\": {},\n  \"groups\": [",
        result.groups.len(),
        file_count,
        result.reclaimable
    );

    for (index, group) in result.groups.iter().enumerate() {
        out.push_str(if index > 0 { ",\n    {" } else { "\n    {" });
        let _ = write!(
            out,
            "\n      \"size\": {},\n      \"sha256\": {},\n      \"files\": [",
            group.size,
            json_string(&group.hash)
        );
        for (position, file) in group.files.iter().enumerate() {
            out.push_str(if position > 0 {
                ",\n        {"
            } else {
                "\n        {"
            });
            let _ = write!(
                out,
                "\"path\": {}, \"size\": {}, \"modifiedAt\": {}, \"category\": {}}}",
                json_string(&file.path.to_string_lossy()),
                file.size,
                json_string(&format_timestamp(file.modified_at)),
                json_string(file.category.label())
            );
        }
        out.push_str(if group.files.is_empty() {
            "]"
        } else {
            "\n      ]"
        });
        out.push_str("\n    }");
    }
    out.push_str(if result.groups.is_empty() {
        "]\n}\n"
    } else {
        "\n  ]\n}\n"
    });
    out
}

/// Write atomically: the report is built beside `path` and renamed into
/// place only once complete, so a failure never leaves a truncated report
/// at the destination.
pub fn write_file(
    path: &Path,
    format: Format,
    folder: &str,
    result: &ScanResult,
    generated_at: i64,
) -> io::Result<()> {
    let staging = path.with_extension(format!(
        "{}tmp",
        path.extension()
            .map(|value| format!("{}.", value.to_string_lossy()))
            .unwrap_or_default()
    ));
    fs::write(&staging, render(format, folder, result, generated_at))?;
    match fs::rename(&staging, path) {
        Ok(()) => Ok(()),
        Err(error) => {
            let _ = fs::remove_file(&staging);
            Err(error)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Category, DuplicateGroup, FileRecord};
    use std::path::PathBuf;

    fn sample() -> ScanResult {
        ScanResult {
            groups: vec![DuplicateGroup {
                size: 10,
                hash: "abc123".into(),
                files: vec![
                    FileRecord {
                        path: PathBuf::from("=danger.txt"),
                        size: 10,
                        modified_at: 1_784_937_600,
                        category: Category::Text,
                    },
                    FileRecord {
                        path: PathBuf::from("plain.txt"),
                        size: 10,
                        modified_at: 1_784_937_600,
                        category: Category::Text,
                    },
                ],
            }],
            reclaimable: 10,
            files_considered: 2,
            files_unreadable: 0,
        }
    }

    #[test]
    fn csv_neutralizes_formula_cells() {
        let csv = render(Format::Csv, "scan", &sample(), 1_784_937_600);
        assert!(
            csv.contains("\"'=danger.txt\""),
            "formula-leading path was not neutralized:\n{csv}"
        );
        // No cell may begin with a raw formula character.
        assert!(!csv.contains(",=") && !csv.contains("\n="));
    }

    #[test]
    fn csv_emits_totals_once() {
        let csv = render(Format::Csv, "scan", &sample(), 1_784_937_600);
        let rows: Vec<&str> = csv
            .lines()
            .skip(1)
            .filter(|line| !line.is_empty())
            .collect();
        assert_eq!(rows.len(), 2);
        // The group and report totals appear on the first row only.
        assert!(rows[0].contains(",10,10,10,"));
        assert!(rows[1].contains(",10,,,"));
    }

    #[test]
    fn json_is_well_formed_and_escaped() {
        let json = render(Format::Json, "scan", &sample(), 1_784_937_600);
        assert!(json.starts_with('{') && json.trim_end().ends_with('}'));
        assert!(json.contains("\"schema\": \"twintidy.duplicate-report/v1\""));
        assert!(json.contains("\"groupCount\": 1"));
        assert!(json.contains("\"fileCount\": 2"));
        // Backslashes in Windows paths must be escaped.
        let escaped = render(Format::Json, r"C:\scan", &sample(), 1_784_937_600);
        assert!(escaped.contains(r#""folder": "C:\\scan""#));
    }

    #[test]
    fn empty_result_still_renders_valid_documents() {
        let empty = ScanResult::default();
        let json = render(Format::Json, "", &empty, 1_784_937_600);
        assert!(json.contains("\"groups\": []"));
        let csv = render(Format::Csv, "", &empty, 1_784_937_600);
        assert_eq!(csv.lines().count(), 1, "header only");
    }

    #[test]
    fn format_names_round_trip() {
        assert_eq!(Format::from_name("JSON"), Some(Format::Json));
        assert_eq!(Format::from_name("csv"), Some(Format::Csv));
        assert_eq!(Format::from_name("xml"), None);
        assert_eq!(Format::Json.extension(), ".json");
    }
}
