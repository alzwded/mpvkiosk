VERSION = 1.3.2
CC ?= gcc
CFLAGS ?= -std=c99 -O2
PREFIX ?= /usr/local

jakserver: jakserver.c
	$(CC) $(CFLAGS) '-DVERSION="$(VERSION)"' -o jakserver jakserver.c

install: jakserver
	install -m 755 -D jakserver $(PREFIX)/bin/jakserver
	install -m 644 -D jakserver.1 $(PREFIX)/share/man/man1/jakserver.1

clean:
	rm -rf jakserver *.o
