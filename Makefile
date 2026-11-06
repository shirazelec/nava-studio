# Nava Studio - native C/GTK4 media player.
#
#   make           build FFmpeg (if needed) and the player
#   make ffmpeg    build the vendored FFmpeg 8.0 only
#   make run       build and launch
#   make test      build the headless engine test
#   make install   install into $(PREFIX) (default /usr/local)
#   make deb       build a self-contained .deb under dist/
#   make clean / distclean
#
# Cross compiling: make CROSS_COMPILE=aarch64-linux-gnu-
# The target's GTK4/PulseAudio development packages must be visible to
# pkg-config (e.g. via PKG_CONFIG_LIBDIR or a sysroot).

VERSION       := 1.0.0
PREFIX        ?= /usr/local
DESTDIR       ?=
CROSS_COMPILE ?=

CC          := $(CROSS_COMPILE)gcc
PKG_CONFIG  ?= pkg-config

BUILD    := build
FF_SRC   := third_party/ffmpeg
FF_BUILD := $(FF_SRC)
FF_DIST  := $(BUILD)/ffmpeg-dist
OUT      := $(BUILD)/obj

UNAME_S := $(shell uname -s)

# FFmpeg SIMD assembly detection
HAS_X86ASM := $(shell command -v nasm >/dev/null 2>&1 && echo yes || command -v yasm >/dev/null 2>&1 && echo yes)
X86ASM_FLAG := $(if $(HAS_X86ASM),,--disable-x86asm)

FF_ARGS := --disable-doc --disable-debug --enable-shared --disable-static \
           --enable-gpl --enable-version3 \
           --disable-programs --disable-avdevice \
           --disable-autodetect $(X86ASM_FLAG)

ifeq ($(UNAME_S),Darwin)
  # macOS (CoreAudio)
  PKGS := gtk4 graphene-1.0
  AUDIO_FLAGS := -DNAVA_AUDIO_COREAUDIO=1
  PLATFORM_LIBS := -framework CoreAudio -framework AudioToolbox -lpthread -lm
else ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
  # Windows MinGW (WASAPI)
  PKGS := gtk4 graphene-1.0
  AUDIO_FLAGS := -DNAVA_AUDIO_WASAPI=1
  PLATFORM_LIBS := -lole32 -lmmdevapi -lavrt -lpthread -lm
else
  # Linux (PulseAudio)
  PKGS := gtk4 graphene-1.0 vulkan libpulse-simple
  AUDIO_FLAGS := -DNAVA_AUDIO_PULSE=1
  PLATFORM_LIBS := -lm -lpthread -ldl
endif

CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-misleading-indentation -Wno-deprecated-declarations -Wno-unused-parameter
CPPFLAGS += -Isrc -I$(FF_DIST)/include $(AUDIO_FLAGS) $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDFLAGS  += -L$(FF_DIST)/lib
LDFLAGS  += '-Wl,-rpath,$$ORIGIN/lib' '-Wl,-rpath,$$ORIGIN/../lib' '-Wl,-rpath,$$ORIGIN/build/ffmpeg-dist/lib'
LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS))
LDLIBS   += -lavformat -lavcodec -lavutil -lswscale -lswresample $(PLATFORM_LIBS)

SRCS   := src/main.c src/app.c src/engine.c src/dsp.c src/vis.c src/audio_out.c
OBJS   := $(patsubst src/%.c,$(OUT)/%.o,$(SRCS)) $(OUT)/resources.o
TARGET := nava-studio

.PHONY: all ffmpeg app run test install deb clean distclean
all: app

ffmpeg: $(FF_DIST)/lib/libavcodec.so

# FFmpeg's configure refuses out-of-tree builds when the source path
# contains whitespace, so the vendored
# tree is built in place and only the installed result lives under build/.
$(FF_DIST)/lib/libavcodec.so:
	@if [ ! -f $(FF_BUILD)/ffbuild/config.mak ]; then \
	  echo "==> configuring FFmpeg"; \
	  cd $(FF_BUILD) && ./configure --prefix='$(abspath $(FF_DIST))' $(FF_ARGS) > configure.log 2>&1 || { tail -30 $(FF_BUILD)/configure.log; exit 1; }; \
	fi
	@echo "==> building FFmpeg (this can take a few minutes)"
	@$(MAKE) -C $(FF_BUILD) -j$$(nproc) > build.log 2>&1 || { tail -40 $(FF_BUILD)/build.log; exit 1; }
	@$(MAKE) -C $(FF_BUILD) install > install.log 2>&1 || { tail -30 $(FF_BUILD)/install.log; exit 1; }
	@echo "==> FFmpeg installed to $(FF_DIST)"

app: ffmpeg $(TARGET)

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/%.o: src/%.c | $(OUT) $(FF_DIST)/lib/libavcodec.so
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/resources.c: resources/style.css resources/nava.gresource.xml | $(OUT)
	glib-compile-resources --sourcedir=resources --generate-source --target=$@ resources/nava.gresource.xml

$(OUT)/resources.o: $(OUT)/resources.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

test: app $(BUILD)/nava-engine-test

$(BUILD)/nava-engine-test: $(OUT)/engine_test.o $(OUT)/engine.o $(OUT)/dsp.o $(OUT)/audio_out.o
	$(CC) $^ $(LDFLAGS) $(LDLIBS) -o $@

run: app
	./scripts/run.sh

install: app
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/share/nava-studio/lib
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/share/nava-studio/nava-studio
	cp -a $(FF_DIST)/lib/. $(DESTDIR)$(PREFIX)/share/nava-studio/lib/
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps
	install -m 644 data/nava-studio.desktop $(DESTDIR)$(PREFIX)/share/applications/
	install -m 644 data/icons/nava-studio.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/
	printf '#!/bin/sh\nexec "%s/share/nava-studio/nava-studio" "$$@"\n' "$(PREFIX)" > $(DESTDIR)$(PREFIX)/bin/nava-studio
	chmod 755 $(DESTDIR)$(PREFIX)/bin/nava-studio
	@# Register every supported audio/video type with the desktop environment.
	NAVA_DESKTOP_FILE=$(DESTDIR)$(PREFIX)/share/applications/nava-studio.desktop \
		sh scripts/register-mime.sh register || true

deb: app
	./scripts/make-deb.sh

clean:
	rm -rf $(BUILD) $(TARGET)

distclean: clean
	@if [ -f $(FF_BUILD)/ffbuild/config.mak ]; then $(MAKE) -C $(FF_BUILD) distclean >/dev/null 2>&1 || true; fi
	rm -rf $(FF_DIST) $(BUILD)
