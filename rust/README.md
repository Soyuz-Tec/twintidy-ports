# TwinTidy — Rust port

Rust port of TwinTidy's duplicate-detection core, with a command-line scanner and a native Win32 GUI.

| File | Role |
| --- | --- |
| `src/lib.rs` | detection engine, shared by both front ends; standard library only |
| `src/main.rs` | command-line front end |
| `src/bin/gui.rs` | Win32 GUI front end (Windows only; a stub elsewhere) |

## Build

```sh
cargo build --release
```

Binaries land at `target/release/twintidy` and `target/release/twintidy-gui.exe`.

## Test

```sh
cargo test
cargo fmt --check
cargo clippy --all-targets -- -D warnings
```

## Usage

```text
twintidy <folder>          # command line
twintidy-gui.exe           # window: select a folder, scan, review
```

Both are read-only and never modify the scanned tree.

## Dependencies

The library and CLI use the standard library only. The GUI uses [`windows-sys`](https://crates.io/crates/windows-sys) — Microsoft's official Win32 bindings — declared under a `cfg(windows)` target dependency, so non-Windows builds pull in nothing.
