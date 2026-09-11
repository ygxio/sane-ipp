# sane-ipp -- IPP backend for SANE
#
# Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
# Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
# SPDX-License-Identifier: BSD-2-Clause
# See LICENSE for license terms and conditions

CC      = gcc
CFLAGS  = -O2 -g -Wall -Wextra -std=gnu99 -D_GNU_SOURCE -MMD -MP

# Avahi supplies DNS-SD discovery.
AVAHI_CFLAGS := $(shell pkg-config --cflags avahi-client)
AVAHI_LIBS   := $(shell pkg-config --libs avahi-client)

CUPS_CFLAGS := $(shell cups-config --cflags)
CUPS_LIBS   := $(shell cups-config --libs)

CFLAGS  += $(AVAHI_CFLAGS) $(CUPS_CFLAGS)

TOOLS    = ipp-discover ipp-probe
OBJS     = ipp-discover.o ipp-mdns.o ipp-probe.o ipp-proto.o
DEPS     = $(OBJS:.o=.d)

all: $(TOOLS)

ipp-discover: ipp-discover.o ipp-mdns.o
	$(CC) $(CFLAGS) -o $@ $^ $(AVAHI_LIBS)

ipp-probe: ipp-probe.o ipp-proto.o
	$(CC) $(CFLAGS) -o $@ $^ $(CUPS_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TOOLS) $(OBJS) $(DEPS)

-include $(DEPS)

.PHONY: all clean
