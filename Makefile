# BXChat - needs libsodium (https://libsodium.org)
#   Debian/Ubuntu: sudo apt install libsodium-dev
#   macOS:         brew install libsodium
#   Windows:       MSYS2 MinGW shell: pacman -S mingw-w64-x86_64-libsodium make gcc

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99
CFLAGS  += $(shell pkg-config --cflags libsodium 2>/dev/null)
LIBS    := $(shell pkg-config --libs libsodium 2>/dev/null || echo -lsodium)

ifeq ($(OS),Windows_NT)
  LIBS += -lws2_32
  EXE   = .exe
endif

all: bxserver$(EXE) bxclient$(EXE)

bxserver$(EXE): src/server.c src/common.c src/common.h
	$(CC) $(CFLAGS) -o $@ src/server.c src/common.c $(LIBS)

bxclient$(EXE): src/client.c src/common.c src/common.h
	$(CC) $(CFLAGS) -o $@ src/client.c src/common.c $(LIBS)

clean:
	rm -f bxserver bxclient bxserver.exe bxclient.exe

.PHONY: all clean
