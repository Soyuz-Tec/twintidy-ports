# TwinTidy Ports

C and Rust ports of the duplicate-detection core of [TwinTidy](https://github.com/Soyuz-Tec/TwinTidy), the safety-first Windows duplicate-file finder by Kayilan Inc.

These are **read-only command-line scanners**: they find and report duplicate files but never delete, move, or modify anything. The full Windows GUI application, its safety model, and its signed releases live in the main TwinTidy repository — these ports replicate the detection engine only.

## Detection strategy

Both ports use the same pipeline as the Go engine:

1. Walk the tree, collecting regular files (symlinks skipped, so cycles are impossible).
2. Group by exact size — files of different sizes can never be duplicates.
3. Within a size group, hash each file with streamed FNV-1a 64.
4. Within a hash group, confirm with a full byte-for-byte comparison, so a hash collision can never produce a false duplicate.

Unreadable files are warned about and skipped, never guessed at.

## Layout

| Directory | Language | Build |
| --- | --- | --- |
| [`c/`](c/) | C11, no dependencies | `make` (or a single `gcc` command) |
| [`rust/`](rust/) | Rust, standard library only | `cargo build --release` |

## Usage (both ports)

```text
twintidy <folder>
```

Prints each duplicate group with per-file paths and a keep-one-copy reclaimable-bytes estimate.

## License

MIT, Copyright (c) 2026 Kayilan Inc. See [LICENSE](LICENSE).
