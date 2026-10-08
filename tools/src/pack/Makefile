# botter_pack: standalone, depends on nothing from botcore.
#   make         -> ./botter_pack
CC     ?= gcc
CFLAGS ?= -Os

botter_pack: botter_pack.c
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -o $@ $<

clean:
	rm -f botter_pack

.PHONY: clean
