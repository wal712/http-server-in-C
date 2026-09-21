CFLAGS = -Wall -Wextra -pedantic -lpthread

all: server

server: server.c
	gcc $(CFLAGS) server.c -o server

clean:
	rm -f server

.PHONY: all clean