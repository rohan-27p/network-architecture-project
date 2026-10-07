# ─────────────────────────────────────────────────────────────────────
#  Makefile — Binary HTTP Protocol  (bserve + bcurl)
#
#  On Linux / macOS:   make
#  On Windows (MinGW): mingw32-make
#  Clean:              make clean
# ─────────────────────────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c99 -O2
SRCS    = protocol.c

# Windows needs Winsock
ifeq ($(OS),Windows_NT)
  LDFLAGS = -lws2_32
  RM      = del /Q
  EXT     = .exe
else
  LDFLAGS =
  RM      = rm -f
  EXT     =
endif

.PHONY: all clean

all: bserve$(EXT) bcurl$(EXT)

bserve$(EXT): bserve.c $(SRCS) protocol.h
	$(CC) $(CFLAGS) -o $@ bserve.c $(SRCS) $(LDFLAGS)

bcurl$(EXT): bcurl.c $(SRCS) protocol.h
	$(CC) $(CFLAGS) -o $@ bcurl.c $(SRCS) $(LDFLAGS)

clean:
	$(RM) bserve$(EXT) bcurl$(EXT)
