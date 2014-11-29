CC=gcc
CFLAGS=-Wall -Werror -std=c99
TARGET=epoccam
SRCS=epoccam_linux.c

OBJS = $(SRCS:.c=.o)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) -o $(TARGET) $(OBJS)

all: $(TARGET)

.PHONY: clean

clean:
	rm *.o $(TARGET)
