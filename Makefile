# Cross-Platform Task Manager — console dashboard (C99)
#
# macOS:   make
# Linux:   make            (or: make linux)
# Windows: from MinGW/MSVC: make windows
#
# Same idea as the Resource Monitor: one tree, compile-time host.

CC       ?= gcc
SRC      := src
BUILD    := build
CFLAGS   ?= -std=c99 -Wall -Wextra -O2 -Isrc
LDFLAGS  ?=

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
ifneq (,$(findstring MINGW,$(UNAME_S)))
  COLLECT  := $(SRC)/collect_win.c
  LDLIBS   := -lpsapi -ladvapi32
endif
ifneq (,$(findstring MSYS,$(UNAME_S)))
  COLLECT  := $(SRC)/collect_win.c
  LDLIBS   := -lpsapi -ladvapi32
endif

COMMON   := $(SRC)/util.c $(SRC)/term.c $(SRC)/render.c $(COLLECT)

.PHONY: all run linux windows test once clean

all: $(BUILD)/task-manager

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/task-manager: $(BUILD) $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/taskman.c $(COMMON) $(LDFLAGS) $(LDLIBS)

$(BUILD)/taskmantest: $(BUILD) $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/taskmantest.c $(SRC)/util.c $(COLLECT) $(LDFLAGS) $(LDLIBS)

run: $(BUILD)/task-manager
	$(BUILD)/task-manager

once: $(BUILD)/task-manager
	$(BUILD)/task-manager --once

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
