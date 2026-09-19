CFLAGS = -Wall -Wextra -pedantic -lpthread

all: server

server: server.c
	$(CC) $(CFLAGS) server.c -o server

clean:
	rm -f server

.PHONY: all clean