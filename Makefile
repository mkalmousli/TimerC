CC      = gcc
TARGET  = timer
SRCS    = timer.c draw.c parse.c audio.c db.c config.c

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

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c timer.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)
	install -Dm644 timer.desktop $(HOME)/.local/share/applications/timer.desktop
	install -Dm644 timer.svg $(HOME)/.local/share/icons/hicolor/scalable/apps/timer.svg
	-update-desktop-database $(HOME)/.local/share/applications 2>/dev/null
	-gtk-update-icon-cache $(HOME)/.local/share/icons/hicolor 2>/dev/null
	@echo "Installed to $(HOME)/.local/bin/$(TARGET)"
	@echo "Installed desktop entry and icon — launch via app menu or run 'timer' anywhere (ensure ~/.local/bin is on PATH)"

clean:
	rm -f $(TARGET) *.o

.PHONY: all install clean
