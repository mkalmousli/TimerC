CC      = gcc
TARGET  = timer
SRCS    = timer.c draw.c parse.c audio.c db.c config.c lock.c

# Tiny setuid-root helper (lockhelper.c) so `timer` itself never runs as
# root — only it can flip a USB device's sysfs 'authorized' attribute,
# and only after validating the path strictly. See lock.h/lock.c.
LOCK_HELPER         = timer-lock-helper
LOCK_HELPER_INSTALL = /usr/local/bin/$(LOCK_HELPER)

CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter \
          $(shell pkg-config --cflags x11 xft xinerama xrandr 2>/dev/null) \
          -I/usr/include

LDFLAGS = $(shell pkg-config --libs x11 xft xinerama xrandr 2>/dev/null) \
          -lsqlite3 -lm -lpthread
ifeq ($(USE_GL),1)
SRCS += gl.c
CFLAGS += -DUSE_GL
LDFLAGS += -lGL -lGLX
endif

LDFLAGS += -lX11 -lXft -lXinerama -lXrandr -lXrender -lfontconfig

OBJS    = $(SRCS:.c=.o)

all: $(TARGET) $(LOCK_HELPER)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(LOCK_HELPER): lockhelper.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

%.o: %.c timer.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Single command: installs the (unprivileged) timer binary, then — the
# only privileged step anywhere in this project — installs the tiny
# setuid-root lock helper via sudo, prompting for your password once.
install: $(TARGET) $(LOCK_HELPER)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)
	install -Dm644 timer.desktop $(HOME)/.local/share/applications/timer.desktop
	install -Dm644 timer.svg $(HOME)/.local/share/icons/hicolor/scalable/apps/timer.svg
	@test -f $(HOME)/.local/share/icons/hicolor/index.theme || \
		install -Dm644 index.theme $(HOME)/.local/share/icons/hicolor/index.theme
	@update-desktop-database $(HOME)/.local/share/applications >/dev/null 2>&1 || true
	@gtk-update-icon-cache $(HOME)/.local/share/icons/hicolor >/dev/null 2>&1 || true
	@echo "Installed to $(HOME)/.local/bin/$(TARGET)"
	@echo "Installed desktop entry and icon — launch via app menu or run 'timer' anywhere (ensure ~/.local/bin is on PATH)"
	@echo ""
	@echo "Installing the lock helper for '-l' (needs sudo once)..."
	@sudo install -Dm4755 -o root -g root $(LOCK_HELPER) $(LOCK_HELPER_INSTALL) && \
		echo "Installed $(LOCK_HELPER_INSTALL) (setuid root) — 'timer -l ...' now fully" && \
		echo "disconnects USB keyboard/mouse, no sudo needed at run time." || \
		( echo "Skipped — couldn't get sudo. '-l' will fall back to a plain X11 grab" && \
		  echo "until you run: sudo make install-lock-helper" )

# Same lock-helper install step on its own, if you skipped it above or
# need to re-run it after `make clean` rebuilds the helper binary.
install-lock-helper: $(LOCK_HELPER)
	install -Dm4755 -o root -g root $(LOCK_HELPER) $(LOCK_HELPER_INSTALL)
	@echo "Installed $(LOCK_HELPER_INSTALL) (setuid root, owned by root)"
	@echo "'timer -l ...' (run as your normal user) can now fully disconnect"
	@echo "USB keyboard/mouse for the timer's duration, no sudo needed."

uninstall:
	rm -f $(HOME)/.local/bin/$(TARGET)
	rm -f $(HOME)/.local/share/applications/timer.desktop
	rm -f $(HOME)/.local/share/icons/hicolor/scalable/apps/timer.svg
	@update-desktop-database $(HOME)/.local/share/applications >/dev/null 2>&1 || true
	@gtk-update-icon-cache $(HOME)/.local/share/icons/hicolor >/dev/null 2>&1 || true
	@echo "Uninstalled $(TARGET)"
	@echo "Note: $(LOCK_HELPER_INSTALL) (setuid root) was left in place — remove it with:"
	@echo "    sudo make uninstall-lock-helper"

uninstall-lock-helper:
	rm -f $(LOCK_HELPER_INSTALL)
	@echo "Removed $(LOCK_HELPER_INSTALL)"

clean:
	rm -f $(TARGET) $(LOCK_HELPER) *.o

.PHONY: all install uninstall install-lock-helper uninstall-lock-helper clean
