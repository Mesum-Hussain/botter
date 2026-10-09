# botter TUI (front end for the botter agent only; not part of botcore).
#   make        -> ./botter-tui (static musl)
#   make dev    -> ./botter-tui-dev (host gcc, debug)

# also drop compiler notes (Fedora annobin adds ~15KB) that plain strip keeps
STRIP_NOTES ?= -R .comment -R .note.gnu.property -R .annobin.notes -R .gnu.build.attributes
CJSON  = ../tools/src/botcore/lib/cjson
SRCS   = main.c lay.c $(CJSON)/cJSON.c
HDRS   = tui.h logo.h
VERSION ?= $(shell cat ../VERSION 2>/dev/null || echo dev)
WARN   = -std=gnu11 -Wall -Wextra -Wno-unused-parameter -DBOTTER_VERSION='"$(VERSION)"'

botter-tui: $(SRCS) $(HDRS)
	musl-gcc -Os -static $(WARN) -I$(CJSON) -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o $@ $(SRCS) -lm
	strip $(STRIP_NOTES) $@

dev: botter-tui-dev

botter-tui-dev: $(SRCS) $(HDRS)
	gcc -g -O1 $(WARN) -I$(CJSON) -o $@ $(SRCS) -lm

clean:
	rm -f botter-tui botter-tui-dev

.PHONY: dev clean
