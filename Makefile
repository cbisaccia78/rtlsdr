CC=gcc
PKG_CONFIG=pkg-config
GTK_CFLAGS=$(shell $(PKG_CONFIG) --cflags gtk4)
GTK_LIBS=$(shell $(PKG_CONFIG) --libs gtk4)
RTLSDR_CFLAGS=$(shell $(PKG_CONFIG) --cflags librtlsdr)
RTLSDR_LIBS=$(shell $(PKG_CONFIG) --libs librtlsdr)
ACCELERATE_LIBS=-framework Accelerate
CORE_AUDIO_LIBS=-framework AudioToolbox -framework CoreFoundation
CFLAGS=-Wall -Wextra -O2 $(GTK_CFLAGS) $(RTLSDR_CFLAGS)
LDFLAGS=$(GTK_LIBS) $(RTLSDR_LIBS) $(ACCELERATE_LIBS) $(CORE_AUDIO_LIBS)

all: radio_gui

radio_gui: main.c radio_engine.c radio_engine.h demodulator.c demodulator.h audio_buffer.c audio_buffer.h audio_output_mac.c audio_output_mac.h
	$(CC) $(CFLAGS) -o radio_gui main.c radio_engine.c demodulator.c audio_buffer.c audio_output_mac.c $(LDFLAGS)

clean:
	rm -f radio_gui
