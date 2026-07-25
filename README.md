# TwinTidy Ports

C and Rust ports of the duplicate-detection core of [TwinTidy](https://github.com/Soyuz-Tec/TwinTidy), the safety-first Windows duplicate-file finder by Kayilan Inc.

Each port ships a **command-line scanner** and a **native Windows GUI**. Both are **read-only**: they find and report duplicate files but never delete, move, or modify anything. The full TwinTidy application — its safety model, hardlink consolidation, previews, and signed releases — lives in the main repository; these ports replicate the detection engine and a review front end only.

## Detection strategy

Both ports run the same staged pipeline as the Go engine:

1. **Surface scan** — walk the tree collecting user files, with per-category statistics. Symlinks are skipped, so cycles are impossible.
2. **Size mapping** — files of different sizes can never be duplicates.
3. **Boundary hash** — a cheap FNV-1a over each file's head and tail rejects most same-size candidates without reading them whole.
4. **Full hash** — SHA-256 over the complete file, matching the group identifiers the Go engine publishes.
5. **Confirmation** — a full byte-for-byte comparison, so a hash collision can never produce a false duplicate.

Unreadable files are counted and skipped, never guessed at.

## Safety model

Both ports apply the same file-selection policy as TwinTidy, so a scan stays on user-created content:

- **Protected directories** are never traversed at any depth — system and application folders (`Windows`, `AppData`, `ProgramData`), version-control internals (`.git`, `.svn`), dependency trees (`node_modules`, `site-packages`, `.venv`), and build output (`target`, `build`, `dist`, `bin`, `obj`).
- **Protected extensions** are never duplicate candidates — executables, libraries, drivers, installers, and shortcuts.
- A scan root that is itself protected is refused rather than silently returning nothing.

Without this policy a scan of a development folder reports thousands of build artefacts a user must not act on.

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

Options: `--surface` (inventory only), `--category NAME` (repeatable), `--min-size BYTES`, `--exclude PATH`, `--exclude-ext EXT`, `--export PATH`, `--format csv|json`.

It prints each duplicate group with its SHA-256, per-file paths, and a keep-one-copy reclaimable-bytes estimate. Exit codes: `0` no duplicates, `1` duplicates found, `2` bad invocation or scan failure.

The GUI follows the same two-phase workflow as the Go application:

1. **Select Folder**, then **Surface Scan** builds an inventory of user files.
2. The **file-type focus** checkboxes fill in with the per-category counts found; untick what you do not care about.
3. **Find Duplicates** hashes only the selected types, with live stage, elapsed time, and a working Cancel.
4. Review results with row checkboxes, **Keep Newest** / **Keep Oldest** / **Clear Selection**, **Show In Explorer**, and **Export Report** (CSV or JSON).

Keep Newest and Keep Oldest always leave one copy of every group unchecked. Selection is planning only: neither window offers any action that deletes, moves, or modifies a file.

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
- Neither port implements TwinTidy's Windows file-identity capture or hardlink consolidation, so they can plan cleanup but never perform it.

## License

MIT, Copyright (c) 2026 Kayilan Inc. See [LICENSE](LICENSE).
