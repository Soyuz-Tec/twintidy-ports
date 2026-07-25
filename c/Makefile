# make predefines CC=cc, so `?=` would never take effect. Override only the
# built-in default, leaving an explicit CC from the environment or command
# line untouched. MinGW-w64 toolchains ship gcc but no cc.
ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
TARGET  := twintidy
GUI     :=

ENGINE  := src/scanner.c src/safety.c src/sha256.c
HEADERS := src/scanner.h src/safety.h src/sha256.h

ifeq ($(OS),Windows_NT)
TARGET  := twintidy.exe
GUI     := twintidy-gui.exe
LDFLAGS += -static
endif

.PHONY: all cli gui clean test

all: cli gui

cli: $(TARGET)

# The GUI is Windows-only; on other platforms this target is a no-op.
gui: $(GUI)

$(TARGET): src/main.c $(ENGINE) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ src/main.c $(ENGINE) $(LDFLAGS)

# -municode selects the wide wWinMain entry point so the window uses the
# Unicode Win32 API throughout.
twintidy-gui.exe: src/gui.c $(ENGINE) $(HEADERS)
	$(CC) $(CFLAGS) -mwindows -municode -o $@ src/gui.c $(ENGINE) $(LDFLAGS) -lcomctl32 -lole32 -lshell32

test: $(TARGET)
	sh test/smoke.sh ./$(TARGET)

clean:
	rm -f twintidy twintidy.exe twintidy-gui.exe
