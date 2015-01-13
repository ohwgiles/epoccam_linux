CC=gcc
CFLAGS=-Wall -Werror -std=c99 -g $(shell pkg-config --cflags gtk+-2.0)
TARGET=epoccam
SRC=epoccam_linux.c
LIBS=-lavcodec -lavutil -lasound -lavformat $(shell pkg-config --libs gtk+-2.0)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

all: $(TARGET)

.PHONY: clean

clean:
	rm $(TARGET)
