CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -pedantic -pthread
TARGET=clinic_alt

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
