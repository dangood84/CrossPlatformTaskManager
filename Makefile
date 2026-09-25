# Cross-Platform Task Manager — console dashboard (C99)
#
# macOS:   make
# Linux:   make            (or: make linux)
# Windows: make            (w64devkit / MinGW / MSYS)  or: make windows
#
# Same idea as the Resource Monitor: one tree, compile-time host.

CC       ?= gcc
SRC      := src
BUILD    := build
CFLAGS   ?= -std=c99 -Wall -Wextra -O2 -Isrc
LDFLAGS  ?=
BIN      := $(BUILD)/task-manager

UNAME_S  := $(shell uname -s 2>/dev/null || echo Unknown)

ifeq ($(UNAME_S),Darwin)
  COLLECT  := $(SRC)/collect_darwin.c
  LDLIBS   :=
endif
ifeq ($(UNAME_S),Linux)
  COLLECT  := $(SRC)/collect_linux.c
  # glibc hid POSIX under -std=c99; posix_features.h also sets these.
  CFLAGS   += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
  LDLIBS   := -lrt
endif

# WORKING: w64devkit's uname prints "Windows", not MINGW64_NT-*.
# cmd.exe / PowerShell always set OS=Windows_NT. The old MINGW/MSYS
# substring miss meant `make` linked no collector and ld died on
# collect_init. Detect all of those, then still allow `make windows`.
ifeq ($(COLLECT),)
  ifeq ($(OS),Windows_NT)
    ON_WINDOWS := 1
  endif
  ifneq (,$(findstring Windows,$(UNAME_S)))
    ON_WINDOWS := 1
  endif
  ifneq (,$(findstring MINGW,$(UNAME_S)))
    ON_WINDOWS := 1
  endif
  ifneq (,$(findstring MSYS,$(UNAME_S)))
    ON_WINDOWS := 1
  endif
  ifneq (,$(findstring CYGWIN,$(UNAME_S)))
    ON_WINDOWS := 1
  endif
  ifdef ON_WINDOWS
    COLLECT  := $(SRC)/collect_win.c
    LDLIBS   := -lpsapi -ladvapi32
    BIN      := $(BUILD)/task-manager.exe
  endif
endif

COMMON   := $(SRC)/util.c $(SRC)/term.c $(SRC)/render.c $(COLLECT)

.PHONY: all run linux windows test once clean

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BIN): $(BUILD) $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/taskman.c $(COMMON) $(LDFLAGS) $(LDLIBS)

$(BUILD)/taskmantest: $(BUILD) $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/taskmantest.c $(SRC)/util.c $(COLLECT) $(LDFLAGS) $(LDLIBS)

run: $(BIN)
	$(BIN)

once: $(BIN)
	$(BIN) --once

test: $(BUILD)/taskmantest
	$(BUILD)/taskmantest

# Explicit host targets (run these on that OS; they do not cross-compile).
linux: $(BUILD)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	    -o $(BUILD)/task-manager \
	    $(SRC)/taskman.c $(SRC)/util.c $(SRC)/term.c $(SRC)/render.c \
	    $(SRC)/collect_linux.c -lrt

windows: $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/TaskManager.exe \
	    $(SRC)/taskman.c $(SRC)/util.c $(SRC)/term.c $(SRC)/render.c \
	    $(SRC)/collect_win.c -lpsapi -ladvapi32

clean:
	rm -rf $(BUILD)
