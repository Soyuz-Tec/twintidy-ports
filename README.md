# TwinTidy Ports

C and Rust ports of the duplicate-detection core of [TwinTidy](https://github.com/Soyuz-Tec/TwinTidy), the safety-first Windows duplicate-file finder by Kayilan Inc.

Each port ships a **command-line scanner** and a **native Windows GUI**. Both are **read-only**: they find and report duplicate files but never delete, move, or modify anything. The full TwinTidy application — its safety model, hardlink consolidation, previews, and signed releases — lives in the main repository; these ports replicate the detection engine and a review front end only.

## Detection strategy

Both ports use the same pipeline as the Go engine:

1. Walk the tree, collecting regular files (symlinks skipped, so cycles are impossible).
2. Group by exact size — files of different sizes can never be duplicates.
3. Within a size group, hash each file with streamed FNV-1a 64.
4. Within a hash group, confirm with a full byte-for-byte comparison, so a hash collision can never produce a false duplicate.

Unreadable files are counted and skipped, never guessed at.

## Layout

| Directory | Language | CLI | GUI |
| --- | --- | --- | --- |
| [`c/`](c/) | C11 | `twintidy` | `twintidy-gui.exe` (Win32) |
| [`rust/`](rust/) | Rust | `twintidy` | `twintidy-gui.exe` (Win32) |

In both ports the detection engine is separate from the front ends, so the CLI and the GUI run exactly the same code: `c/src/scanner.c` and `rust/src/lib.rs`.

## Usage

Command line, on either port:

```text
twintidy <folder>
```

It prints each duplicate group with per-file paths and a keep-one-copy reclaimable-bytes estimate. Exit codes: `0` success, `2` bad invocation or scan failure.

The GUI opens a window where you select a folder, run a scan with live progress and a working Cancel, and review the results in a sortable table (group, size, file, folder). It offers no destructive action.

## Building

C (needs a C11 compiler; MinGW-w64 on Windows):

```sh
cd c && make
```

Rust:

```sh
cd rust && cargo build --release
```

## Dependencies

The Rust library and CLI use the **standard library only**. The Rust GUI additionally uses [`windows-sys`](https://crates.io/crates/windows-sys), Microsoft's official Win32 bindings, pulled in on Windows targets alone. The C port has no dependencies beyond the C standard library, POSIX `dirent.h`, and — for the GUI — the Win32 API that ships with Windows.

## Known limitations

- The C GUI converts engine paths for display using the active code page, so paths containing characters outside it may render with substitutions. This affects display only, never which files the engine compares.
- Neither port implements TwinTidy's safety model. Pointed at a development folder they will report duplicates inside `node_modules`, `.git`, and build output that the real application deliberately skips.

## License

MIT, Copyright (c) 2026 Kayilan Inc. See [LICENSE](LICENSE).
