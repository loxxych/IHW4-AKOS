CC=gcc
CFLAGS=-Wall -Wextra -pthread -std=c11

all: clinic_io

clinic_io: main.c
	$(CC) $(CFLAGS) -o clinic_io main.c

clean:
	rm -f clinic_io

.PHONY: all clean
