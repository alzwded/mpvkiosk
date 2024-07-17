VERSION = 1.0
CC ?= gcc
CFLAGS ?= -std=gnu99 -O2
PREFIX ?= /usr/local

jakserver: jakserver.c
	$(CC) $(CFLAGS) '-DVERSION="$(VERSION)"' -o jakserver jakserver.c

install: jakserver
	install -m 755 jakserver $(PREFIX)/bin/jakserver
	install -m 644 jakserver.1 $(PREFIX)/share/man/man1/jakserver.1

clean:
	rm -rf jakserver *.o
