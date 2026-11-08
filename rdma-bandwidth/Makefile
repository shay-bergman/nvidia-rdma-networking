CC      := gcc
CFLAGS  := -std=gnu11 -O2 -Wall -D_GNU_SOURCE -D_POSIX_C_SOURCE=200112L
LDFLAGS := -libverbs

OBJS := ib_verbs.o tcp_exchange.o bw_bench.o

all: server client

server: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o server

# The client is the same binary; it acts as a client when given a host.
client: server
	@ln -sf server client

%.o: %.c pingpong.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f server client *.o

.PHONY: all clean
