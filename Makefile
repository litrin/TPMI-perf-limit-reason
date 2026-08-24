# plr-tpmi - Intel Performance Limit Reasons via TPMI
#
# Default target builds a fully static, dependency-free binary.

CC      ?= gcc
CFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

TARGET  := plr-tpmi
SRC     := src/plr_tpmi.c

.PHONY: all static dynamic install clean

all: static

# Fully static link: the resulting binary has no runtime dependencies.
static: CFLAGS += -static
static: LDFLAGS += -static
static: $(TARGET)

dynamic: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
