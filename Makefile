PKG_NAME = ddb_jack

PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib

CC     ?= gcc
CFLAGS += -I$(shell pkg-config --variable=includedir jack)
LDLIBS += $(shell pkg-config --libs jack)

all:
	$(CC) -std=c99 -shared $(CFLAGS) -o $(PKG_NAME).so $(LDLIBS) $(PKG_NAME).c -fPIC -Wall $(LDFLAGS)

install:
	install -D -m 755 $(PKG_NAME).so $(DESTDIR)$(LIBDIR)/deadbeef/$(PKG_NAME).so

