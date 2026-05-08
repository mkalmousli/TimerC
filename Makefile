CC      = gcc
TARGET  = timer
SRC     = timer.c

# Build flags
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter \
          $(shell pkg-config --cflags x11 xft xrandr 2>/dev/null) \
          -I/usr/include

LDFLAGS = $(shell pkg-config --libs x11 xft xrandr 2>/dev/null) \
          -lsqlite3 -lm -lpthread

# Fallback if pkg-config doesn't find things
LDFLAGS += -lX11 -lXft -lXrandr -lfontconfig

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)
	@echo "Installed to $(HOME)/.local/bin/$(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all install clean
