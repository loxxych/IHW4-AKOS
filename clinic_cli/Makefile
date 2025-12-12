CC=gcc
CFLAGS=-Wall -Wextra -std=c11 -pthread

all: clinic_cli

clinic_cli: main.c
	$(CC) $(CFLAGS) -o clinic_cli main.c

clean:
	rm -f clinic_cli

.PHONY: all clean
