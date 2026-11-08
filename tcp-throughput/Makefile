CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2
LDFLAGS =

all: client server

client: client.o shared.o
	$(CC) $(LDFLAGS) -o $@ $^

server: server.o shared.o
	$(CC) $(LDFLAGS) -o $@ $^

client.o: client.c shared.h
	$(CC) $(CFLAGS) -c client.c

server.o: server.c shared.h
	$(CC) $(CFLAGS) -c server.c

shared.o: shared.c shared.h
	$(CC) $(CFLAGS) -c shared.c

clean:
	rm -f client server *.o

.PHONY: all clean
