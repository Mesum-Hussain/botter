# botcore build.
#   make            dev build with the host compiler (dynamic)      -> ./botcore
#   make release    static musl build, stripped (what botter ships) -> ./botcore-static
#   make clean
# The agent packer (botter_pack) lives outside this repo.

CC       ?= gcc
CFLAGS   ?= -Os -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables
LDFLAGS  ?= -Wl,--gc-sections
WARN     = -std=gnu11 -Wall -Wextra -Wno-unused-parameter
INC      = -Isrc -Ilib/cjson

SRCS     = $(wildcard src/*.c) lib/cjson/cJSON.c
OBJS     = $(patsubst %.c,obj/%.o,$(SRCS))
OBJS_REL = $(patsubst %.c,obj-musl/%.o,$(SRCS))
HDRS     = $(wildcard src/*.h)

botcore: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) -lm

obj/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) $(INC) -c -o $@ $<

release: botcore-static

botcore-static: $(OBJS_REL)
	musl-gcc $(CFLAGS) $(LDFLAGS) -static -o $@ $(OBJS_REL) -lm
	strip $@

obj-musl/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	musl-gcc $(CFLAGS) $(WARN) $(INC) -c -o $@ $<

clean:
	rm -rf obj obj-musl botcore botcore-static

.PHONY: clean release
