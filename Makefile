# The default target of this Makefile is:
all:

-include config.mk

PKG_DEPS=gstreamer-1.0 gstreamer-base-1.0 gio-2.0 gio-unix-2.0 gstreamer-video-1.0

prefix?=/usr/local
exec_prefix?=$(prefix)
bindir?=$(exec_prefix)/bin
libexecdir?=$(exec_prefix)/libexec
datarootdir?=$(prefix)/share
mandir?=$(datarootdir)/man
man1dir?=$(mandir)/man1
sysconfdir?=$(prefix)/etc

user_name?=$(shell git config user.name || \
                   getent passwd `whoami` | cut -d : -f 5 | cut -d , -f 1)
user_email?=$(shell git config user.email || echo "$$USER@$$(hostname)")

INSTALL?=install
TAR ?= $(shell which gnutar >/dev/null 2>&1 && echo gnutar || echo tar)
MKTAR = $(TAR) --format=gnu --owner=root --group=root \
    --mtime="$(shell git show -s --format=%ci HEAD)"
GZIP ?= gzip

# Support installing GStreamer elements under $HOME
gsthomepluginsdir=$(if $(XDG_DATA_HOME),$(XDG_DATA_HOME),$(HOME)/.local/share)/gstreamer-1.0/plugins
gstsystempluginsdir=$(shell pkg-config --variable=pluginsdir gstreamer-1.0)
gstpluginsdir?=$(if $(filter $(HOME)%,$(prefix)),$(gsthomepluginsdir),$(gstsystempluginsdir))

VALA_PKGDEPS = gstreamer-1.0 gio-2.0 posix gio-unix-2.0 gio-2.0-workaround
VALAFLAGS = $(patsubst %,--pkg %,$(VALA_PKGDEPS)) 

# Generate version from 'git describe' when in git repository, and from
# VERSION file included in the dist tarball otherwise.
generate_version := $(shell \
	GIT_DIR=.git git describe --always --dirty > VERSION.now 2>/dev/null && \
	{ cmp VERSION.now VERSION 2>/dev/null || mv VERSION.now VERSION; }; \
	rm -f VERSION.now)
VERSION?=$(shell cat VERSION)
ESCAPED_VERSION=$(subst -,_,$(VERSION))

.DELETE_ON_ERROR:


all : pulsevideo gst/gstpulsevideo.so

% : %.vala
	valac --vapidir=vapi -o $@ $(VALAFLAGS) $<

%.c : %.vala
	valac --vapidir=vapi -C -o $@ $(VALAFLAGS) $<

install : pulsevideo gst/gstpulsevideo.so VERSION
	$(INSTALL) -m 0755 -d \
	    $(DESTDIR)$(bindir) \
	    $(DESTDIR)$(gstpluginsdir)
	$(INSTALL) -m 0755 pulsevideo $(DESTDIR)$(bindir)
	$(INSTALL) -m 0644 gst/gstpulsevideo.so \
	    $(DESTDIR)$(gstpluginsdir)

clean:
	git clean -fX

check:
	true

# Can only be run from within a git clone of pulsevideo or VERSION (and the
# list of files) won't be set correctly.
dist: pulsevideo-$(VERSION).tar.gz

DIST = $(shell git ls-files)
DIST += VERSION

pulsevideo-$(VERSION).tar.gz: $(DIST)
	@$(TAR) --version 2>/dev/null | grep -q GNU || { \
	    printf 'Error: "make dist" requires GNU tar ' >&2; \
	    printf '(use "make dist TAR=gnutar").\n' >&2; \
	    exit 1; }
	# Separate tar and gzip so we can pass "-n" for more deterministic tarball
	# generation
	$(MKTAR) -c --transform='s,^,pulsevideo-$(VERSION)/,' \
	         -f pulsevideo-$(VERSION).tar $^ && \
	$(GZIP) -9fn pulsevideo-$(VERSION).tar


# Force rebuild if installation directories change
sq = $(subst ','\'',$(1)) # function to escape single quotes (')
.buildcfg: flags = libexecdir=$(call sq,$(libexecdir)):\
                   sysconfdir=$(call sq,$(sysconfdir))
.buildcfg: FORCE
	@if [ '$(flags)' != "$$(cat $@ 2>/dev/null)" ]; then \
	    [ -f $@ ] && echo "*** new $@" >&2; \
	    echo '$(flags)' > $@; \
	fi

gst/gstpulsevideo.so : \
		gst/gstdbusvideosourcesrc.h \
		gst/gstdbusvideosourcesrc.c \
		gst/gstpulsevideoplugin.c \
		gst/gstsocketsrc.h \
		gst/gstsocketsrc.c \
		gst/gstvideosource1.c \
		gst/gstvideosource1.h \
		VERSION
	@if ! pkg-config --exists $(PKG_DEPS); then \
		printf "Please install packages $(PKG_DEPS)"; exit 1; fi
	gcc -shared -o $@ $(filter %.c %.o,$^) -fPIC  -Wall -Werror $(CFLAGS) \
		$(LDFLAGS) $$(pkg-config --libs --cflags $(PKG_DEPS)) \
		-DVERSION=\"$(VERSION)\" -DPACKAGE="\"pulsevideo\""

gst/gstvideosource1.c gst/gstvideosource1.h : \
		dbus-xml/com.stbtester.VideoSource1.xml
	cd $(dir $@) && \
	gdbus-codegen \
		--generate-c-code $(basename $(notdir $@)) \
		--interface-prefix=com.stbtester. \
		--c-namespace Gst ../$<

.PHONY: all clean check dist doc install uninstall
.PHONY: FORCE TAGS
