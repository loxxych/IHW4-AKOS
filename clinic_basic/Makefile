CC=gcc
CFLAGS=-Wall -Wextra -std=c11 -pthread

all: clinic

clinic: main.c
	$(CC) $(CFLAGS) -o clinic main.c

clean:
	rm -f clinic

.PHONY: all clean
