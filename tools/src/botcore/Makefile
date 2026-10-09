# botcore build.
#   make            dev build with the host compiler (dynamic)      -> ./botcore
#   make release    static musl build, stripped (what botter ships) -> ./botcore-static
#   make clean
# The agent packer (botter_pack) lives outside this repo.
# TLS: vendored BearSSL (lib/bearssl, MIT) built as a static archive; CA anchors in lib/ca.

# also drop compiler notes (Fedora annobin adds ~15KB) that plain strip keeps
STRIP_NOTES ?= -R .comment -R .note.gnu.property -R .annobin.notes -R .gnu.build.attributes

CC       ?= gcc
CFLAGS   ?= -Os -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables
LDFLAGS  ?= -Wl,--gc-sections
WARN     = -std=gnu11 -Wall -Wextra -Wno-unused-parameter -DBC_VERSION='"$(VERSION)"'
VERSION  ?= $(shell cat $(dir $(lastword $(MAKEFILE_LIST)))../../../VERSION 2>/dev/null || echo dev)
INC      = -Isrc -Ilib/cjson -Ilib/bearssl/inc -Ilib/ca
BR_SRCS  = $(wildcard lib/bearssl/src/*.c lib/bearssl/src/*/*.c)
BR_FLAGS = -Os -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -Ilib/bearssl/inc -Ilib/bearssl/src

SRCS     = $(wildcard src/*.c) lib/cjson/cJSON.c
OBJS     = $(patsubst %.c,obj/%.o,$(SRCS))
OBJS_REL = $(patsubst %.c,obj-musl/%.o,$(SRCS))
HDRS     = $(wildcard src/*.h)

botcore: $(OBJS) obj/libbearssl.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) obj/libbearssl.a -lm

obj/libbearssl.a: $(patsubst %.c,obj/%.o,$(BR_SRCS))
	ar rcs $@ $^

obj/lib/bearssl/%.o: lib/bearssl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BR_FLAGS) -c -o $@ $<

obj/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) $(INC) -c -o $@ $<

release: botcore-static

botcore-static: $(OBJS_REL) obj-musl/libbearssl.a
	musl-gcc $(CFLAGS) $(LDFLAGS) -static -o $@ $(OBJS_REL) obj-musl/libbearssl.a -lm
	strip $(STRIP_NOTES) $@

obj-musl/libbearssl.a: $(patsubst %.c,obj-musl/%.o,$(BR_SRCS))
	ar rcs $@ $^

obj-musl/lib/bearssl/%.o: lib/bearssl/%.c
	@mkdir -p $(dir $@)
	musl-gcc $(BR_FLAGS) -c -o $@ $<

obj-musl/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	musl-gcc $(CFLAGS) $(WARN) $(INC) -c -o $@ $<

clean:
	rm -rf obj obj-musl botcore botcore-static

.PHONY: clean release
