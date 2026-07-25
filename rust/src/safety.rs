//! The file-selection policy ported from TwinTidy's Go engine.
//!
//! Two independent rules keep a scan on user-created content:
//! - protected directories: system, application, and build-output folders are
//!   never traversed;
//! - protected extensions: executables, drivers, installers, and shortcuts are
//!   never treated as duplicate candidates.
//!
//! Without this policy a scan reports build artefacts and repository
//! internals that a user must not act on.

use std::path::Path;

/// Directory names that are never traversed, at any depth.
const PROTECTED_DIRECTORIES: &[&str] = &[
    "$recycle.bin",
    "system volume information",
    "windows",
    "program files",
    "program files (x86)",
    "programdata",
    "recovery",
    "perflogs",
    "config.msi",
    "msocache",
    "appdata",
    "application data",
    "temporary internet files",
    "inetcache",
    "cache",
    ".cache",
    ".git",
    ".hg",
    ".svn",
    "node_modules",
    "__pycache__",
    ".pytest_cache",
    ".mypy_cache",
    ".ruff_cache",
    ".venv",
    "venv",
    "env",
    "site-packages",
    "packages",
    "bin",
    "obj",
    "target",
    "build",
    "dist",
];

/// Extensions that are never duplicate candidates.
const PROTECTED_EXTENSIONS: &[&str] = &[
    ".386",
    ".appx",
    ".appxbundle",
    ".cab",
    ".com",
    ".cpl",
    ".cur",
    ".dll",
    ".drv",
    ".efi",
    ".exe",
    ".gadget",
    ".hta",
    ".icl",
    ".icns",
    ".ico",
    ".inf",
    ".ins",
    ".iso",
    ".job",
    ".lnk",
    ".msi",
    ".msix",
    ".msixbundle",
    ".msp",
    ".mst",
    ".ocx",
    ".pif",
    ".scr",
    ".sys",
    ".theme",
    ".themepack",
];

/// File categories, matching the Go engine's classification.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Category {
    Pdf,
    Text,
    Word,
    Excel,
    PowerPoint,
    Images,
    Audio,
    Video,
    Archives,
    Other,
}

impl Category {
    /// Every category, in display order.
    pub const ALL: [Category; 10] = [
        Category::Pdf,
        Category::Text,
        Category::Word,
        Category::Excel,
        Category::PowerPoint,
        Category::Images,
        Category::Audio,
        Category::Video,
        Category::Archives,
        Category::Other,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Category::Pdf => "PDF",
            Category::Text => "Text",
            Category::Word => "Word",
            Category::Excel => "Excel",
            Category::PowerPoint => "PowerPoint",
            Category::Images => "Images",
            Category::Audio => "Audio",
            Category::Video => "Video",
            Category::Archives => "Archives",
            Category::Other => "Other",
        }
    }

    /// Parse a lower-case CLI name such as `powerpoint`.
    pub fn from_name(name: &str) -> Option<Category> {
        Category::ALL
            .iter()
            .copied()
            .find(|category| category.label().eq_ignore_ascii_case(name))
    }

    fn extensions(self) -> &'static [&'static str] {
        match self {
            Category::Pdf => &[".pdf"],
            Category::Text => &[
                ".txt", ".md", ".csv", ".tsv", ".json", ".xml", ".html", ".css", ".js", ".go",
                ".py", ".log", ".ini", ".yaml", ".yml", ".sql", ".ps1",
            ],
            Category::Word => &[".doc", ".docx", ".docm", ".rtf"],
            Category::Excel => &[".xls", ".xlsx", ".xlsm", ".xlsb"],
            Category::PowerPoint => &[".ppt", ".pptx", ".pptm"],
            Category::Images => &[
                ".bmp", ".dib", ".gif", ".jpg", ".jpeg", ".jpe", ".png", ".tif", ".tiff", ".ico",
                ".heic", ".heif", ".webp", ".raw", ".cr2", ".nef", ".arw",
            ],
            Category::Audio => &[
                ".mp3", ".m4a", ".aac", ".wav", ".wma", ".flac", ".ogg", ".aiff",
            ],
            Category::Video => &[
                ".mp4", ".m4v", ".mov", ".avi", ".mkv", ".wmv", ".webm", ".mpeg", ".mpg", ".3gp",
            ],
            Category::Archives => &[".zip", ".7z", ".rar", ".tar", ".gz", ".bz2", ".xz"],
            Category::Other => &[],
        }
    }
}

/// Extension including its dot, lower-cased; empty when there is none.
fn extension_of(path: &Path) -> String {
    path.extension()
        .map(|value| format!(".{}", value.to_string_lossy().to_ascii_lowercase()))
        .unwrap_or_default()
}

/// Classify a path by extension; unknown extensions map to [`Category::Other`].
pub fn category_for_path(path: &Path) -> Category {
    let extension = extension_of(path);
    if extension.is_empty() {
        return Category::Other;
    }
    for category in Category::ALL {
        if category.extensions().contains(&extension.as_str()) {
            return category;
        }
    }
    Category::Other
}

/// Report whether a directory must not be traversed. Any path segment that
/// matches a protected name disqualifies the whole subtree, so a nested
/// `node_modules` or `.git` is skipped wherever it appears.
pub fn should_skip_directory(path: &Path) -> bool {
    path.components().any(|component| {
        let segment = component.as_os_str().to_string_lossy().to_ascii_lowercase();
        // A bare drive prefix such as "c:" is never a meaningful segment.
        if segment.len() == 2 && segment.ends_with(':') {
            return false;
        }
        PROTECTED_DIRECTORIES.contains(&segment.as_str())
    })
}

/// Report whether a file may be treated as a duplicate candidate. Files with
/// protected extensions and files inside protected directories are excluded.
pub fn is_user_created_file(path: &Path) -> bool {
    let extension = extension_of(path);
    if PROTECTED_EXTENSIONS.contains(&extension.as_str()) {
        return false;
    }
    match path.parent() {
        Some(parent) => !should_skip_directory(parent),
        None => true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn protected_directories_are_skipped_at_any_depth() {
        assert!(should_skip_directory(&PathBuf::from(
            r"C:\code\project\node_modules"
        )));
        assert!(should_skip_directory(&PathBuf::from(r"C:\code\.git\refs")));
        assert!(!should_skip_directory(&PathBuf::from(
            r"C:\code\project\src"
        )));
    }

    #[test]
    fn drive_prefix_is_not_a_segment() {
        // "c:" must not be mistaken for a protected name.
        assert!(!should_skip_directory(&PathBuf::from(r"C:\Users\example")));
    }

    #[test]
    fn protected_extensions_are_never_candidates() {
        assert!(!is_user_created_file(&PathBuf::from(
            r"C:\Users\a\setup.exe"
        )));
        assert!(!is_user_created_file(&PathBuf::from(r"C:\Users\a\lib.DLL")));
        assert!(is_user_created_file(&PathBuf::from(
            r"C:\Users\a\notes.txt"
        )));
    }

    #[test]
    fn files_inherit_directory_protection() {
        assert!(!is_user_created_file(&PathBuf::from(
            r"C:\code\node_modules\pkg\index.js"
        )));
    }

    #[test]
    fn categories_classify_by_extension() {
        assert_eq!(category_for_path(&PathBuf::from("a.PDF")), Category::Pdf);
        assert_eq!(
            category_for_path(&PathBuf::from("a.jpeg")),
            Category::Images
        );
        assert_eq!(
            category_for_path(&PathBuf::from("a.unknown")),
            Category::Other
        );
        assert_eq!(category_for_path(&PathBuf::from("noext")), Category::Other);
    }

    #[test]
    fn category_names_round_trip() {
        assert_eq!(
            Category::from_name("powerpoint"),
            Some(Category::PowerPoint)
        );
        assert_eq!(Category::from_name("nope"), None);
    }
}
