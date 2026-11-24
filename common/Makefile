# What the cores share with Pixelado Party, as a small C library (C99, POSIX). The cores build these sources into
# themselves (retro/ does); this Makefile only runs the tests on a computer:
#   make -C common test
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Wno-unused-parameter -Iinclude
SRC := src/line.c src/partyctl.c src/roomctl.c
OUT ?= build

.PHONY: test clean
test: $(OUT)/test_partyctl
	$(OUT)/test_partyctl

$(OUT)/test_partyctl: tests/test_partyctl.c $(SRC) include/pixelado/*.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_partyctl.c $(SRC)

clean:
	rm -rf $(OUT)
