CC       = gcc
CFLAGS   = -O2 -g -Wall -Wextra -std=gnu99 -D_GNU_SOURCE
AVAHI_CFLAGS = $(shell pkg-config --cflags avahi-client)
AVAHI_LIBS   = $(shell pkg-config --libs avahi-client)

all: ipp-discover

ipp-discover: ipp-discover.c ipp-mdns.c ipp-mdns.h
	$(CC) $(CFLAGS) $(AVAHI_CFLAGS) -o $@ ipp-discover.c ipp-mdns.c $(AVAHI_LIBS)

clean:
	rm -f ipp-discover

.PHONY: all clean
