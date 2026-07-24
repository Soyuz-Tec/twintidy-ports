CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
TARGET  := twintidy
ifeq ($(OS),Windows_NT)
TARGET  := twintidy.exe
LDFLAGS += -static
endif

.PHONY: all clean test

all: $(TARGET)

$(TARGET): src/twintidy.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test: $(TARGET)
	sh test/smoke.sh ./$(TARGET)

clean:
	rm -f twintidy twintidy.exe
