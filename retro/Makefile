# pixelado-retro: the frontend, built into one program with one libretro core (a static archive).
#   make SYS=gba NAME=gpsp CORE_LIB=/path/to/gpsp_libretro.a VERSION=0.1.0   # -> build/pixelado-core-gpsp
#   make fake                                                          # the frontend with tests/fake_core.c
#   make test                                                          # the link's loopback test (two processes)
# SYS is the system (ds, gba, snes: it names the settings file and the Party's room); NAME is the
# core's own name (the program it builds: pixelado-core-<name>).
# On Linux the handheld's parts (KMS, ALSA, evdev) are in by default; elsewhere (a Mac) only the null ones:
#   KMS=0 ALSA=0 EVDEV=0 turn them off. DRM_CFLAGS/DRM_LIBS for a cross build (pkg-config otherwise).
CC ?= cc
SYS ?= retro
NAME ?= $(SYS)
VERSION ?= 0.0.0
OUT ?= build
CORE_LIB ?=
DEFAULT_OPTIONS ?=
OPT ?= -O2
UNAME := $(shell uname -s)
ifeq ($(UNAME),Linux)
KMS ?= 1
ALSA ?= 1
EVDEV ?= 1
else
KMS ?= 0
ALSA ?= 0
EVDEV ?= 0
endif

SRC := src/main.c src/core.c src/video.c src/audio.c src/input.c src/link.c src/card.c src/system.c src/log.c \
       ../common/src/line.c ../common/src/partyctl.c ../common/src/roomctl.c
HDR := $(wildcard src/*.h include/*.h ../common/include/pixelado/*.h)
# (CFLAGS and LDFLAGS are the caller's, added after ours)
CFLAGS ?=
PXFLAGS = $(OPT) -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra \
          -Wno-unused-parameter -Wno-missing-field-initializers -Iinclude -I../common/include \
          -DPX_SYS='"$(SYS)"' -DPX_VERSION='"$(VERSION)"' -DPX_DEFAULT_OPTIONS='"$(DEFAULT_OPTIONS)"' \
          -ffile-prefix-map=$(CURDIR)/=retro/ -ffile-prefix-map=$(abspath $(CURDIR)/..)/=
LDFLAGS ?=
LIBS := -lm -lpthread
ifeq ($(KMS),1)
DRM_CFLAGS ?= $(shell pkg-config --cflags libdrm 2>/dev/null)
DRM_LIBS ?= -ldrm
PXFLAGS += -DPX_HAVE_KMS $(DRM_CFLAGS)
LIBS += $(DRM_LIBS)
endif
ifeq ($(ALSA),1)
PXFLAGS += -DPX_HAVE_ALSA
LIBS += -lasound
endif
ifeq ($(EVDEV),1)
PXFLAGS += -DPX_HAVE_EVDEV
endif

.PHONY: all fake test clean
all: $(OUT)/pixelado-core-$(NAME)

$(OUT)/pixelado-core-$(NAME): $(SRC) $(HDR) $(CORE_LIB)
	@test -n "$(CORE_LIB)" || { echo "CORE_LIB: the libretro core's static archive" >&2; exit 1; }
	@mkdir -p $(OUT)
	$(CC) $(PXFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(CORE_LIB) $(LIBS)

fake: $(OUT)/pixelado-core-fake
$(OUT)/pixelado-core-fake: SYS = fake
$(OUT)/pixelado-core-fake: NAME = fake
$(OUT)/pixelado-core-fake: $(SRC) $(HDR) tests/fake_core.c
	@mkdir -p $(OUT)
	$(CC) $(PXFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) tests/fake_core.c $(LIBS)

test: $(OUT)/pixelado-core-fake
	sh tests/loopback.sh $(OUT)/pixelado-core-fake
	sh tests/scale.sh $(OUT)/pixelado-core-fake

clean:
	rm -rf $(OUT)
