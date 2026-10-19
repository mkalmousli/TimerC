CC      = gcc
VERSION ?= 1.2.0
MANPAGES = timerc.1 timerc-bg.1 timerc-config.1 timerc-insights.1 timerc-uninstall.1
TARGET  = timerc

CORE_SRCS     = src/core/app.c src/core/config.c src/core/parse.c src/core/db.c
PLATFORM_SRCS = src/platform/linux/lock.c src/platform/linux/linux_audio.c \
                src/platform/linux/linux_fs.c
UI_SRCS       = src/ui/draw_common.c src/ui/draw_timer.c src/ui/draw_alert.c
CLI_SRCS      = src/cli/tui.c src/cli/uninstall.c
AUDIO_SRCS    = src/audio/synth.c

SRCS = $(CORE_SRCS) $(PLATFORM_SRCS) $(UI_SRCS) $(AUDIO_SRCS) $(CLI_SRCS)

# Tiny setuid-root helper (lockhelper.c) so `timerc` itself never runs as
# root — only it can flip a USB device's sysfs 'authorized' attribute,
# and only after validating the path strictly. See include/platform/lock.h
# and src/platform/linux/lock.c.
LOCK_HELPER_SRC     = src/platform/linux/lockhelper.c
LOCK_HELPER         = timerc-lock-helper
LOCK_HELPER_INSTALL = /usr/local/bin/$(LOCK_HELPER)

INCLUDES = -Iinclude/core -Iinclude/platform -Iinclude/ui -Iinclude/audio

CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter \
          $(shell pkg-config --cflags x11 xft xinerama xrandr 2>/dev/null) \
          $(INCLUDES) -DTIMERC_VERSION=\"$(VERSION)\"

LDFLAGS = $(shell pkg-config --libs x11 xft xinerama xrandr 2>/dev/null) \
          -lsqlite3 -lm -lpthread
ifeq ($(USE_GL),1)
SRCS += src/platform/linux/gl.c
CFLAGS += -DUSE_GL
LDFLAGS += -lGL -lGLX
endif

LDFLAGS += -lX11 -lXft -lXinerama -lXrandr -lXrender -lfontconfig

OBJS    = $(SRCS:.c=.o)

# Headers every .c file transitively depends on, for a simple/blunt but
# correct rebuild trigger (this project's headers are cheap to re-scan).
HEADERS = $(wildcard include/core/*.h include/platform/*.h include/ui/*.h \
                     include/audio/*.h src/ui/*.h)

all: $(TARGET) $(TARGET)-bg $(LOCK_HELPER)

# Same binary under another name: argv[0] == timerc-bg detaches from the terminal.
$(TARGET)-bg: $(TARGET)
	ln -sf $(TARGET) $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(LOCK_HELPER): $(LOCK_HELPER_SRC)
	$(CC) -O2 -Wall -Wextra -o $@ $<

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

PREFIX ?= $(HOME)/.local
BINDIR  = $(PREFIX)/bin
DATADIR = $(PREFIX)/share

# Installs `timerc` (blocking, foreground) and `timerc-bg` (same binary,
# detaches into the background), plus desktop entry and icon. No root needed;
# the optional setuid lock helper is a separate target (install-lock-helper).
install: $(TARGET)
	install -Dm755 $(TARGET) $(BINDIR)/$(TARGET)
	ln -sf $(TARGET) $(BINDIR)/$(TARGET)-bg
	ln -sf $(TARGET) $(BINDIR)/$(TARGET)-uninstall
	for m in $(MANPAGES); do install -Dm644 man/$$m $(DATADIR)/man/man1/$$m; done
	install -Dm644 timerc.desktop $(DATADIR)/applications/timerc.desktop
	install -Dm644 timerc.svg $(DATADIR)/icons/hicolor/scalable/apps/timerc.svg
	@update-desktop-database $(DATADIR)/applications >/dev/null 2>&1 || true
	@gtk-update-icon-cache -f $(DATADIR)/icons/hicolor >/dev/null 2>&1 || true
	@echo "Installed timerc, timerc-bg and timerc-uninstall to $(BINDIR)"
	@echo "Optional: 'sudo make install-lock-helper' enables 'timerc -l' (USB input lock)."

# Same lock-helper install step on its own, if you skipped it above or
# need to re-run it after `make clean` rebuilds the helper binary.
install-lock-helper: $(LOCK_HELPER)
	install -Dm4755 -o root -g root $(LOCK_HELPER) $(LOCK_HELPER_INSTALL)
	@echo "Installed $(LOCK_HELPER_INSTALL) (setuid root, owned by root)"
	@echo "'timerc -l ...' (run as your normal user) can now fully disconnect"
	@echo "USB keyboard/mouse for the timerc's duration, no sudo needed."

uninstall:
	rm -f $(BINDIR)/$(TARGET) $(BINDIR)/$(TARGET)-bg $(BINDIR)/$(TARGET)-uninstall
	for m in $(MANPAGES); do rm -f $(DATADIR)/man/man1/$$m; done
	rm -f $(DATADIR)/applications/timerc.desktop
	rm -f $(DATADIR)/icons/hicolor/scalable/apps/timerc.svg
	@echo "Uninstalled $(TARGET). If installed, remove the lock helper with: sudo make uninstall-lock-helper"

uninstall-lock-helper:
	rm -f $(LOCK_HELPER_INSTALL)
	@echo "Removed $(LOCK_HELPER_INSTALL)"

clean:
	rm -f $(TARGET) $(TARGET)-bg $(LOCK_HELPER) $(OBJS) src/platform/linux/gl.o


.PHONY: all install uninstall install-lock-helper uninstall-lock-helper clean
