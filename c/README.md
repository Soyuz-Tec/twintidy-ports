# TwinTidy — C port

C11 port of TwinTidy's duplicate-detection core, with a command-line scanner and a native Win32 GUI. No dependencies beyond the C standard library and POSIX `dirent.h`; the GUI additionally uses the Win32 API that ships with Windows.

| File | Role |
| --- | --- |
| `src/scanner.c`, `src/scanner.h` | detection engine, shared by both front ends |
| `src/main.c` | command-line front end |
| `src/gui.c` | Win32 GUI front end (Windows only) |

## Build

```sh
make            # CLI, plus the GUI on Windows
make cli        # command-line scanner only
make gui        # GUI only (no-op on non-Windows)
```

Or build the CLI directly:

```sh
gcc -std=c11 -O2 -o twintidy src/main.c src/scanner.c
```

On Windows with MinGW-w64, `make` links `-static` so the executables are self-contained. The GUI target adds `-mwindows -municode` to select the windowed subsystem and the wide `wWinMain` entry point.

## Test

```sh
make test
```

## Usage

```text
twintidy <folder>          # command line
twintidy-gui.exe           # window: select a folder, scan, review
```

Both are read-only and never modify the scanned tree.
