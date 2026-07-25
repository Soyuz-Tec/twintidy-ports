//! Interface preferences persisted between sessions.
//!
//! Loading is fail-open: a missing, unreadable, or malformed file yields
//! defaults rather than an error, so a corrupt preference file can never
//! prevent the application from starting.
//!
//! The format is a small line-oriented `key=value` file rather than JSON: it
//! is trivially parseable without a dependency, and a malformed line is
//! ignored instead of invalidating the whole file.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Settings {
    /// Window placement in screen coordinates. A zero width or height means
    /// "no stored placement"; the caller should use its own default.
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
    pub maximized: bool,
    /// Last folder the user scanned, if any.
    pub last_folder: Option<PathBuf>,
}

impl Settings {
    /// Whether a usable window placement is stored.
    pub fn has_placement(&self) -> bool {
        self.width > 0 && self.height > 0
    }
}

/// Resolve the settings file path beside the user's local application data.
pub fn default_path() -> Option<PathBuf> {
    let base = std::env::var_os("LOCALAPPDATA")
        .or_else(|| std::env::var_os("HOME"))
        .or_else(|| std::env::var_os("XDG_CONFIG_HOME"))?;
    let directory = PathBuf::from(base).join("TwinTidyRustPort");
    // Creating the directory here keeps save() a single fallible step.
    let _ = fs::create_dir_all(&directory);
    Some(directory.join("settings.ini"))
}

/// Load settings, returning defaults on any failure.
pub fn load(path: &Path) -> Settings {
    let Ok(text) = fs::read_to_string(path) else {
        return Settings::default(); // fail open: no stored preferences yet
    };
    let mut value = Settings::default();
    for line in text.lines() {
        let Some((key, raw)) = line.split_once('=') else {
            continue; // ignore malformed lines rather than failing
        };
        let raw = raw.trim_end_matches(['\r', '\n']);
        match key.trim() {
            "x" => value.x = raw.parse().unwrap_or(0),
            "y" => value.y = raw.parse().unwrap_or(0),
            "width" => value.width = raw.parse().unwrap_or(0),
            "height" => value.height = raw.parse().unwrap_or(0),
            "maximized" => value.maximized = raw.trim() == "1",
            "lastFolder" if !raw.is_empty() => value.last_folder = Some(PathBuf::from(raw)),
            _ => {}
        }
    }
    // A stored placement with no extent is meaningless; discard it so the
    // caller falls back to its own default size.
    if !value.has_placement() {
        value.x = 0;
        value.y = 0;
        value.width = 0;
        value.height = 0;
    }
    value
}

/// Save settings atomically: written to a staging file and renamed into
/// place, so an interrupted write cannot corrupt the stored preferences.
pub fn save(path: &Path, value: &Settings) -> io::Result<()> {
    let folder = value
        .last_folder
        .as_ref()
        .map(|path| path.to_string_lossy().into_owned())
        .unwrap_or_default();
    let body = format!(
        "x={}\ny={}\nwidth={}\nheight={}\nmaximized={}\nlastFolder={}\n",
        value.x,
        value.y,
        value.width,
        value.height,
        i32::from(value.maximized),
        folder
    );

    let staging = path.with_extension("ini.tmp");
    fs::write(&staging, body)?;
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

    fn fixture_path(name: &str) -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("test-settings-{name}.ini"))
    }

    #[test]
    fn missing_file_yields_defaults() {
        let value = load(&fixture_path("does-not-exist"));
        assert_eq!(value, Settings::default());
        assert!(!value.has_placement());
    }

    #[test]
    fn round_trips_through_save_and_load() {
        let path = fixture_path("roundtrip");
        let original = Settings {
            x: 100,
            y: 200,
            width: 1180,
            height: 720,
            maximized: true,
            last_folder: Some(PathBuf::from("C:\\Users\\example\\Documents")),
        };
        save(&path, &original).expect("save succeeds");
        let restored = load(&path);
        let _ = fs::remove_file(&path);
        assert_eq!(restored, original);
    }

    #[test]
    fn malformed_lines_are_ignored() {
        let path = fixture_path("malformed");
        fs::write(
            &path,
            "this line has no separator\nwidth=800\n=novalue\nheight=600\ngarbage\n",
        )
        .expect("write fixture");
        let value = load(&path);
        let _ = fs::remove_file(&path);
        assert_eq!(value.width, 800);
        assert_eq!(value.height, 600);
        assert!(value.has_placement());
    }

    #[test]
    fn placement_without_extent_is_discarded() {
        let path = fixture_path("noextent");
        fs::write(&path, "x=50\ny=60\nwidth=0\nheight=0\n").expect("write fixture");
        let value = load(&path);
        let _ = fs::remove_file(&path);
        assert!(!value.has_placement());
        assert_eq!(value.x, 0, "a meaningless placement is cleared entirely");
    }

    #[test]
    fn a_saved_file_leaves_no_staging_behind() {
        let path = fixture_path("staging");
        save(&path, &Settings::default()).expect("save succeeds");
        let staging = path.with_extension("ini.tmp");
        let leftover = staging.exists();
        let _ = fs::remove_file(&path);
        assert!(!leftover, "staging file must be renamed, not left behind");
    }
}
