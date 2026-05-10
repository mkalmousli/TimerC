CC      = gcc
TARGET  = timer
SRCS    = timer.c draw.c parse.c audio.c db.c

CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter \
          $(shell pkg-config --cflags x11 xft xinerama xrandr 2>/dev/null) \
          -I/usr/include

LDFLAGS = $(shell pkg-config --libs x11 xft xinerama xrandr 2>/dev/null) \
          -lsqlite3 -lm -lpthread
LDFLAGS += -lX11 -lXft -lXinerama -lXrandr -lXrender -lfontconfig

OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c timer.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)
	@echo "Installed to $(HOME)/.local/bin/$(TARGET)"

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all install clean
