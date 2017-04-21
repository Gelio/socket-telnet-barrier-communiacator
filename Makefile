CC=gcc
CFLAGS=-Wall -std=gnu99
LDLIBS= -lpthread

.PHONY: clean

all: server

clean:
	rm server

server: server.c
	$(CC) server.c -o server $(CFLAGS) $(LDLIBS)
