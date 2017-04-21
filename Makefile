CC=gcc
CFLAGS=-Wall

.PHONY: clean

all: server

clean:
	rm server

server: server.c
	$(CC) $(CFLAGS) $(LDLIBS) server.c -o server
