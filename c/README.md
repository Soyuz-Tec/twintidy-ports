# TwinTidy — C port

C11 port of TwinTidy's duplicate-detection core. Single translation unit, no dependencies beyond the C standard library and `dirent.h` (POSIX; provided by MinGW-w64 on Windows).

## Build

```sh
make
```

Or directly:

```sh
gcc -std=c11 -O2 -o twintidy src/twintidy.c
```

On Windows with MinGW-w64, `make` links `-static` so the resulting `twintidy.exe` is fully self-contained.

## Test

```sh
make test
```

## Usage

```text
twintidy <folder>
```
