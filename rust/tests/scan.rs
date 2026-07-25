//! End-to-end scan tests over real fixture trees.
//!
//! Fixtures are created under the crate directory rather than the OS temp
//! directory: on Windows the temp path lives under AppData, which the safety
//! model treats as protected, so a temp fixture would correctly yield nothing.

use std::fs;
use std::path::{Path, PathBuf};

use twintidy::{find_duplicates, scan, surface_scan, Category, Flow, Options, ScanError};

/// A fixture directory that removes itself when the test ends.
struct Fixture {
    root: PathBuf,
}

impl Fixture {
    fn new(name: &str) -> Fixture {
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("test-fixture-{name}"));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create fixture root");
        Fixture { root }
    }

    fn write(&self, relative: &str, contents: &[u8]) -> PathBuf {
        let path = self.root.join(relative);
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).expect("create fixture directory");
        }
        fs::write(&path, contents).expect("write fixture file");
        path
    }

    fn path(&self) -> &Path {
        &self.root
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

fn payload(seed: u8, length: usize) -> Vec<u8> {
    (0..length).map(|i| seed.wrapping_add(i as u8)).collect()
}

#[test]
fn finds_exact_duplicates_with_different_names() {
    let fixture = Fixture::new("exact");
    let content = payload(3, 9000);
    fixture.write("docs/invoice.txt", &content);
    fixture.write("copies/renamed.txt", &content);
    fixture.write("docs/unique.txt", &payload(9, 9000));

    let result = scan(fixture.path(), &Options::default(), |_, _, _| {
        Flow::Continue
    })
    .expect("scan succeeds");

    assert_eq!(result.groups.len(), 1, "expected one duplicate group");
    assert_eq!(result.groups[0].files.len(), 2);
    assert_eq!(result.reclaimable, 9000);
    // A same-size file with different content must not be grouped.
    assert!(!result.groups[0]
        .files
        .iter()
        .any(|file| file.path.ends_with("unique.txt")));
}

#[test]
fn same_size_different_content_is_not_a_duplicate() {
    let fixture = Fixture::new("samesize");
    fixture.write("a.txt", &payload(1, 5000));
    fixture.write("b.txt", &payload(2, 5000));

    let result = scan(fixture.path(), &Options::default(), |_, _, _| {
        Flow::Continue
    })
    .expect("scan succeeds");
    assert!(result.groups.is_empty());
}

#[test]
fn protected_directories_and_extensions_are_excluded() {
    let fixture = Fixture::new("safety");
    let content = payload(7, 6000);
    // Duplicates that the safety model must hide:
    fixture.write("node_modules/pkg/a.js", &content);
    fixture.write("node_modules/pkg/b.js", &content);
    fixture.write("docs/tool1.exe", &content);
    fixture.write("docs/tool2.exe", &content);
    // A duplicate pair that must survive:
    let keep = payload(8, 6000);
    fixture.write("docs/keep1.txt", &keep);
    fixture.write("docs/keep2.txt", &keep);

    let result = scan(fixture.path(), &Options::default(), |_, _, _| {
        Flow::Continue
    })
    .expect("scan succeeds");

    assert_eq!(result.groups.len(), 1, "only the .txt pair may be reported");
    for file in &result.groups[0].files {
        let text = file.path.to_string_lossy();
        assert!(
            !text.contains("node_modules"),
            "node_modules leaked: {text}"
        );
        assert!(
            !text.ends_with(".exe"),
            "protected extension leaked: {text}"
        );
    }
}

#[test]
fn surface_scan_reports_category_statistics() {
    let fixture = Fixture::new("categories");
    fixture.write("a.txt", &payload(1, 100));
    fixture.write("b.pdf", &payload(2, 200));
    fixture.write("c.jpg", &payload(3, 300));

    let surface = surface_scan(fixture.path(), &Options::default(), |_, _, _| {
        Flow::Continue
    })
    .expect("surface scan succeeds");

    assert_eq!(surface.files.len(), 3);
    assert_eq!(surface.total_bytes, 600);
    assert_eq!(surface.stats_for(Category::Text).files, 1);
    assert_eq!(surface.stats_for(Category::Pdf).files, 1);
    assert_eq!(surface.stats_for(Category::Images).files, 1);
    assert_eq!(surface.stats_for(Category::Video).files, 0);
}

#[test]
fn category_filter_restricts_duplicate_matching() {
    let fixture = Fixture::new("catfilter");
    let text = payload(4, 4000);
    fixture.write("a.txt", &text);
    fixture.write("b.txt", &text);
    let image = payload(5, 4000);
    fixture.write("a.png", &image);
    fixture.write("b.png", &image);

    let surface = surface_scan(fixture.path(), &Options::default(), |_, _, _| {
        Flow::Continue
    })
    .expect("surface scan succeeds");

    let images_only = Options {
        categories: vec![Category::Images],
        ..Default::default()
    };
    let result = find_duplicates(&surface, &images_only, |_, _, _| Flow::Continue)
        .expect("duplicate scan succeeds");

    assert_eq!(result.groups.len(), 1);
    assert!(result.groups[0]
        .files
        .iter()
        .all(|file| file.path.extension().is_some_and(|ext| ext == "png")));
}

#[test]
fn min_size_and_extension_filters_apply() {
    let fixture = Fixture::new("filters");
    let small = payload(6, 100);
    fixture.write("small1.txt", &small);
    fixture.write("small2.txt", &small);
    let big = payload(7, 20_000);
    fixture.write("big1.log", &big);
    fixture.write("big2.log", &big);

    let surface = surface_scan(fixture.path(), &Options::default(), |_, _, _| {
        Flow::Continue
    })
    .expect("surface scan succeeds");

    // The size floor removes the small pair.
    let floored = Options {
        min_file_size: 1000,
        ..Default::default()
    };
    let result = find_duplicates(&surface, &floored, |_, _, _| Flow::Continue).expect("scan");
    assert_eq!(result.groups.len(), 1);

    // Excluding .log then removes the remaining pair too.
    let excluded = Options {
        min_file_size: 1000,
        excluded_extensions: vec!["log".into()],
        ..Default::default()
    }
    .normalized();
    let result = find_duplicates(&surface, &excluded, |_, _, _| Flow::Continue).expect("scan");
    assert!(result.groups.is_empty());
}

#[test]
fn cancellation_stops_the_scan() {
    let fixture = Fixture::new("cancel");
    for index in 0..20 {
        fixture.write(&format!("file-{index}.txt"), &payload(index as u8, 3000));
    }

    let outcome = scan(fixture.path(), &Options::default(), |_, _, _| Flow::Cancel);
    assert_eq!(outcome.unwrap_err(), ScanError::Cancelled);
}

#[test]
fn protected_root_is_refused() {
    // A root whose own path is protected must be rejected outright rather
    // than silently returning an empty result.
    let protected: PathBuf = ["some", "node_modules"].iter().collect();
    let outcome = surface_scan(&protected, &Options::default(), |_, _, _| Flow::Continue);
    assert_eq!(outcome.unwrap_err(), ScanError::ProtectedRoot);
}

#[test]
fn group_hash_is_the_sha256_of_the_content() {
    let fixture = Fixture::new("hash");
    // "abc" is the canonical FIPS 180-4 vector.
    fixture.write("a.txt", b"abc");
    fixture.write("b.txt", b"abc");

    let result = scan(fixture.path(), &Options::default(), |_, _, _| {
        Flow::Continue
    })
    .expect("scan succeeds");

    assert_eq!(result.groups.len(), 1);
    assert_eq!(
        result.groups[0].hash,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
}
