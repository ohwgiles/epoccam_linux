PREFIX=/usr
CC=gcc
CFLAGS=-DPREFIX=\"$(PREFIX)\" -Wall -Werror -std=c99 -g $(shell pkg-config --cflags gtk+-2.0)
TARGET=epoccam
SRC=epoccam_linux.c
LIBS=-lavcodec -lavutil -lasound -lavformat $(shell pkg-config --libs gtk+-2.0)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

all: $(TARGET)

.PHONY: clean install

clean:
	rm $(TARGET)

install:
	install -m 0755 $(TARGET) $(PREFIX)/bin
	mkdir -p $(PREFIX)/share/epoccam
	install -m 0644 icon_default.png $(PREFIX)/share/epoccam
	install -m 0644 icon_available.png $(PREFIX)/share/epoccam
	install -m 0644 icon_recording.png $(PREFIX)/share/epoccam
	install -m 0644 epoccam.svg $(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 0644 epoccam.desktop $(PREFIX)/share/applications
