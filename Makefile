CC=gcc
CFLAGS=-Wall -Werror -std=c99 -g
TARGET=epoccam
SRC=epoccam_linux.c
LIBS=-lavcodec -lavutil -lasound -lavformat

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

all: $(TARGET)

.PHONY: clean

clean:
	rm $(TARGET)
