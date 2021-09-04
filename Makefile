package = continual-sync
version = 0.0.5

CC = gcc
LDFLAGS = -r
LINKFLAGS = 
DEFS = -DVERSION=\""$(version)"\"
CFLAGS = -Wall -I. -g
#CFLAGS = -O2 -g -pipe -Wall -Wp,-D_FORTIFY_SOURCE=2 -fexceptions -fstack-protector --param=ssp-buffer-size=4 -m64 -mtune=generic
CPPFLAGS = $(DEFS)

INSTALL = /usr/bin/install -c
DO_GZIP = gzip -f9

srcdir = .
VPATH = $(srcdir)

prefix = /usr/local
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
infodir = ${prefix}/share/info
mandir = ${prefix}/share/man
etcdir = /usr/local/etc
datadir = ${prefix}/share
sbindir = ${exec_prefix}/sbin

ALLTARGETS=watchdir continual-sync

.PHONY: all indent todo clean install
all: $(ALLTARGETS)

.SUFFIXES: .c .o

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

watchdir: watchdir.o watch.o common.o
	$(CC) $(LINKFLAGS) $(CFLAGS) -o $@ $+

continual-sync: continual-sync.o sync.o watch.o common.o
	$(CC) $(LINKFLAGS) $(CFLAGS) -o $@ $+

indent:
	cd $(srcdir) && indent -npro -kr -i8 -cd42 -c45 *.c

todo:
	grep -F TODO *.c *.h NEWS

clean:
	-rm -f *.o *~ $(ALLTARGETS) $(package)-$(version).tar.gz

install: all
	mkdir -p $(DESTDIR)$(bindir)
	mkdir -p $(DESTDIR)$(mandir)/man1
	mkdir -p $(DESTDIR)$(mandir)/man5
	$(INSTALL) -m 755 watchdir $(DESTDIR)$(bindir)/watchdir
	$(INSTALL) -m 755 continual-sync $(DESTDIR)$(bindir)/continual-sync
	$(INSTALL) -m 644 watchdir.1 $(DESTDIR)$(mandir)/man1/watchdir.1
	$(INSTALL) -m 644 continual-sync.1 $(DESTDIR)$(mandir)/man1/continual-sync.1
	$(INSTALL) -m 644 continual-sync.conf.5 $(DESTDIR)$(mandir)/man5/continual-sync.conf.5
	-$(DO_GZIP) $(DESTDIR)$(mandir)/man1/watchdir.1
	-$(DO_GZIP) $(DESTDIR)$(mandir)/man1/continual-sync.1
	-$(DO_GZIP) $(DESTDIR)$(mandir)/man5/continual-sync.conf.5

dist:
	rm -rf $(package)-$(version)
	mkdir $(package)-$(version)
	cp -dpf README NEWS COPYING Makefile continual-sync.spec continual-sync.init *.c *.h watchdir.1 continual-sync.1 continual-sync.conf.5 example.cf example-large.cf defaults.cf $(package)-$(version)/
	sed -i 's/^Version:.*$$/Version:	'"$(version)"'/' $(package)-$(version)/continual-sync.spec
	chmod 644 `find $(package)-$(version) -type f -print`
	chmod 755 `find $(package)-$(version) -type d -print`
	tar cf $(package)-$(version).tar $(package)-$(version)
	rm -rf $(package)-$(version)
	$(DO_GZIP) $(package)-$(version).tar

common.o: common.c common.h
watch.o: watch.c common.h
sync.o: sync.c sync.h common.h
watchdir.o: watchdir.c common.h
continual-sync.o: continual-sync.c sync.h common.h
