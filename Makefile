CC=gcc
PKG_CONFIG=pkg-config
GTK_CFLAGS=$(shell $(PKG_CONFIG) --cflags gtk4)
GTK_LIBS=$(shell $(PKG_CONFIG) --libs gtk4)
RTLSDR_CFLAGS=$(shell $(PKG_CONFIG) --cflags librtlsdr)
RTLSDR_LIBS=$(shell $(PKG_CONFIG) --libs librtlsdr)
ACCELERATE_LIBS=-framework Accelerate
CFLAGS=-Wall -Wextra -O2 $(GTK_CFLAGS) $(RTLSDR_CFLAGS)
LDFLAGS=$(GTK_LIBS) $(RTLSDR_LIBS) $(ACCELERATE_LIBS)

all: radio_gui

radio_gui: main.c radio_engine.c radio_engine.h
	$(CC) $(CFLAGS) -o radio_gui main.c radio_engine.c $(LDFLAGS)

clean:
	rm -f radio_gui
