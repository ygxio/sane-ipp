# sane-ipp -- IPP backend for SANE
#
# Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
# Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
# SPDX-License-Identifier: BSD-2-Clause
# See LICENSE for license terms and conditions

CC      = gcc
CFLAGS  = -O2 -g -Wall -Wextra -std=gnu99 -D_GNU_SOURCE -MMD -MP -fPIC

# Avahi supplies DNS-SD discovery.
AVAHI_CFLAGS := $(shell pkg-config --cflags avahi-client)
AVAHI_LIBS   := $(shell pkg-config --libs avahi-client)

CUPS_CFLAGS := $(shell cups-config --cflags)
CUPS_LIBS   := $(shell cups-config --libs)

CFLAGS  += $(AVAHI_CFLAGS) $(CUPS_CFLAGS)

# Installation directories.
#
# libdir is asked of sane-backends rather than guessed, because the SANE
# loader only searches the directory its own libraries were installed
# into, and distributions disagree about where that is. sysconfdir
# follows prefix, except for a system install, where the loader reads
# /etc/sane.d and nowhere else.
#
# All of them, and DESTDIR, can be overridden from the command line.
PKG_CONFIG  ?= pkg-config

ifeq "$(shell uname -s)" "Linux"
    prefix  ?= /usr
else
    prefix  ?= /usr/local
endif

ifeq "$(prefix)" "/usr"
    sysconfdir ?= /etc
else
    sysconfdir ?= $(prefix)/etc
endif

exec_prefix ?= $(prefix)
libdir      ?= $(shell $(PKG_CONFIG) --variable=libdir sane-backends)
sanedir      = $(libdir)/sane
confdir      = $(sysconfdir)/sane.d

INSTALL  = install

BACKEND  = libsane-ipp.so.1
TOOLS    = ipp-discover ipp-probe
OBJS     = ipp-discover.o ipp-mdns.o ipp-probe.o ipp-proto.o \
           sane-ipp.o ipp-zeroconf.o
DEPS     = $(OBJS:.o=.d)

all: $(TOOLS) $(BACKEND)

ipp-discover: ipp-discover.o ipp-mdns.o ipp-zeroconf.o
	$(CC) $(CFLAGS) -o $@ $^ $(AVAHI_LIBS)

ipp-probe: ipp-probe.o ipp-proto.o
	$(CC) $(CFLAGS) -o $@ $^ $(CUPS_LIBS)

$(BACKEND): sane-ipp.o ipp-mdns.o ipp-proto.o ipp-zeroconf.o ipp.sym
	$(CC) $(CFLAGS) -shared -o $@ sane-ipp.o ipp-mdns.o ipp-proto.o ipp-zeroconf.o \
		-Wl,-soname,$(BACKEND) \
		-Wl,--version-script=ipp.sym \
		-Wl,--no-undefined \
		$(AVAHI_LIBS) $(CUPS_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	$(INSTALL) -d $(DESTDIR)$(sanedir)
	$(INSTALL) -m 755 $(BACKEND) $(DESTDIR)$(sanedir)
	$(INSTALL) -d $(DESTDIR)$(confdir)/dll.d
	[ -e $(DESTDIR)$(confdir)/dll.d/ipp ] || \
		$(INSTALL) -m 644 dll.conf $(DESTDIR)$(confdir)/dll.d/ipp

uninstall:
	rm -f $(DESTDIR)$(sanedir)/$(BACKEND)
	rm -f $(DESTDIR)$(confdir)/dll.d/ipp

clean:
	rm -f $(TOOLS) $(BACKEND) $(OBJS) $(DEPS)

-include $(DEPS)

.PHONY: all clean install uninstall
