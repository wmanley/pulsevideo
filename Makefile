# The default target of this Makefile is:
all:

-include config.mk

PKG_DEPS=gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0 gio-2.0 gio-unix-2.0

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
	rm -f VERSION.now; \
	[ -e VERSION ] || echo UNKNOWN >VERSION; )
VERSION?=$(shell cat VERSION)
ESCAPED_VERSION=$(subst -,_,$(VERSION))

.DELETE_ON_ERROR:


all : pulsevideo build/libgstpulsevideo.so

% : %.vala
	valac --vapidir=vapi -o $@ $(VALAFLAGS) $<

%.c : %.vala
	valac --vapidir=vapi -C -o $@ $(VALAFLAGS) $<

server : pulsevideo
client : build/libgstpulsevideo.so

install : install-client install-server

install-server : pulsevideo VERSION
	$(INSTALL) -m 0755 -d \
	    $(DESTDIR)$(bindir) && \
	$(INSTALL) -m 0755 pulsevideo $(DESTDIR)$(bindir)

install-client : build/libgstpulsevideo.so
	$(INSTALL) -m 0755 -d \
	    $(DESTDIR)$(gstpluginsdir) && \
	$(INSTALL) -m 0644 build/libgstpulsevideo.so \
	    $(DESTDIR)$(gstpluginsdir)

clean:
	git clean -fdX

tests/socketintegrationtest : tests/socketintegrationtest.c build/gstnetcontrolmessagemeta.h build/libgstpulsevideo.so
	gcc -o$@ $< -Wall -Werror $(CFLAGS) $$(pkg-config --cflags --libs $(PKG_DEPS) gstreamer-check-1.0 gstreamer-app-1.0) -Lbuild/ -lgstpulsevideo

check: check-pytest check-gst check-gst-valgrind

TESTS=tests/

check-pytest : pulsevideo build/libgstpulsevideo.so
	GST_PLUGIN_PATH=$(CURDIR)/build LD_LIBRARY_PATH=$(CURDIR)/build \
	py.test --boxed -vv $(TESTS)

check-gst: ./tests/socketintegrationtest
	GST_PLUGIN_PATH=$(CURDIR)/build LD_LIBRARY_PATH=$(CURDIR)/build ./tests/socketintegrationtest

check-gst-gdb: ./tests/socketintegrationtest
	GST_PLUGIN_PATH=$(CURDIR)/build LD_LIBRARY_PATH=$(CURDIR)/build CK_FORK=no G_DEBUG=fatal_warnings gdb ./tests/socketintegrationtest

check-gst-valgrind: ./tests/socketintegrationtest common/
	GST_PLUGIN_PATH=$(CURDIR)/build \
	G_SLICE=always-malloc \
	LD_LIBRARY_PATH=$(CURDIR)/build \
	G_DEBUG=fatal_warnings \
	valgrind --tool=memcheck \
	         --suppressions=./common/gst.supp \
	         --leak-check=full \
	         --trace-children=yes \
	         --show-possibly-lost=no \
	         --leak-resolution=high \
	         --num-callers=20 \
	         ./tests/socketintegrationtest

shell:
	GST_PLUGIN_PATH=$(GST_PLUGIN_PATH):$(CURDIR)/build \
	    LD_LIBRARY_PATH=$(CURDIR)/build:$(LD_LIBRARY_PATH) \
	    PATH=$(CURDIR):$(PATH) \
	    /bin/bash

common/ :
	git clone git://anongit.freedesktop.org/gstreamer/common common

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

build/tcp build/tmpfile build/debugutils build/gst-plugins-base/gst-libs/gst/allocators :
	mkdir -p $@

build/%.c : gst/%.c Makefile | build/tcp build/tmpfile build/gst-plugins-base/gst-libs/gst/allocators
	@echo GEN $@ && \
	sed -e 's/GstMulti/PvMulti/g' \
	    -e 's/GST_MULTI/PV_MULTI/g' \
	    -e 's/gst_multi/pv_multi/g' \
	    -e 's/GST_TYPE_MULTI/PV_TYPE_MULTI/g' \
	    -e 's/gstmulti/pvmulti/g' \
	    -e 's/GstSocket/PvSocket/g' \
	    -e 's/gst_socket/pv_socket/g' \
	    -e 's/GST_SOCKET/PV_SOCKET/g' \
	    -e 's/GstWatchdog/PvWatchdog/g' \
	    -e 's/gst_watchdog/pv_watchdog/g' \
	    -e 's/GST_WATCHDOG/PV_WATCHDOG/g' \
	    -e 's/GST_TYPE_WATCHDOG/PV_TYPE_WATCHDOG/g' \
	    -e 's/GstFdMemory/PvFdMemory/g' \
	    -e 's/gstfdmemory/pvfdmemory/g' \
	    -e 's/gst_fd_mem/pv_fd_mem/g' \
	    -e 's/GST_FD_MEMORY/PV_FD_MEMORY/g' \
	    -e 's/GstFdMemory/PvFdMemory/g' \
	    -e 's/GstFdAllocator/PvFdAllocator/g' \
	    -e 's/GST_FD_ALLOCATOR/PV_FD_ALLOCATOR/g' \
	    -e 's/gst_fd_allocator/pv_fd_allocator/g' \
	    -e 's/gst_is_fd_memory/pv_is_fd_memory/g' \
	    $< \
	| sed -e 's/include "pv/include "gst/g' \
	      -e 's,include "tcp/pv,include "tcp/gst,g' \
	      -e 's,include "tmpfile/pv,include "tmpfile/gst,g' \
	      -e 's,include <gst/allocators/pv,include <gst-plugins-base/gst-libs/gst/allocators/gst,' \
	      >$@
build/%.h : gst/%.h Makefile | build/debugutils build/tcp build/tmpfile build/gst-plugins-base/gst-libs/gst/allocators
	@echo GEN $@ && \
	sed -e 's/GstMulti/PvMulti/g' \
	    -e 's/GST_MULTI/PV_MULTI/g' \
	    -e 's/gst_multi/pv_multi/g' \
	    -e 's/GST_TYPE_MULTI/PV_TYPE_MULTI/g' \
	    -e 's/gstmulti/pvmulti/g' \
	    -e 's/GstSocket/PvSocket/g' \
	    -e 's/gst_socket/pv_socket/g' \
	    -e 's/GST_SOCKET/PV_SOCKET/g' \
	    -e 's/GstWatchdog/PvWatchdog/g' \
	    -e 's/gst_watchdog/pv_watchdog/g' \
	    -e 's/GST_WATCHDOG/PV_WATCHDOG/g' \
	    -e 's/GST_TYPE_WATCHDOG/PV_TYPE_WATCHDOG/g' \
	    -e 's/GstFdMemory/PvFdMemory/g' \
	    -e 's/gstfdmemory/pvfdmemory/g' \
	    -e 's/gst_fd_mem/pv_fd_mem/g' \
	    -e 's/GST_FD_MEMORY/PV_FD_MEMORY/g' \
	    -e 's/GstFdMemory/PvFdMemory/g' \
	    -e 's/GstFdMemory/PvFdMemory/g' \
	    -e 's/GstFdAllocator/PvFdAllocator/g' \
	    -e 's/GST_FD_ALLOCATOR/PV_FD_ALLOCATOR/g' \
	    -e 's/gst_fd_allocator/pv_fd_allocator/g' \
	    -e 's/gst_is_fd_memory/pv_is_fd_memory/g' \
	    $< \
	| sed -e 's/include "pv/include "gst/g' \
	      -e 's,include "tcp/pv,include "tcp/gst,g' \
	      -e 's,include "tmpfile/pv,include "tmpfile/gst,g' \
	      -e 's,include <gst/allocators/pv,include <gst-plugins-base/gst-libs/gst/allocators/gst,' \
	      >$@

build/libgstpulsevideo.so : \
		build/gst-plugins-base/gst-libs/gst/allocators/gstfdmemory.h \
		build/gst-plugins-base/gst-libs/gst/allocators/gstfdmemory.c \
		build/gstpulsevideosrc.h \
		build/gstpulsevideosrc.c \
		build/gstnetcontrolmessagemeta.c \
		build/gstnetcontrolmessagemeta.h \
		build/gstpulsevideoplugin.c \
		build/gstsocketsrc.h \
		build/gstsocketsrc.c \
		build/gstrawvideovalidate.h \
		build/gstrawvideovalidate.c \
		build/gstvideosource1.c \
		build/gstvideosource1.h \
		build/debugutils/gstwatchdog.h \
		build/debugutils/gstwatchdog.c \
		build/tcp/gstmultihandlesink.h \
		build/tcp/gstmultihandlesink.c \
		build/tcp/gstmultisocketsink.h \
		build/tcp/gstmultisocketsink.c \
		build/tmpfile/gstfddepay.c \
		build/tmpfile/gstfddepay.h \
		build/tmpfile/gstfdpay.c \
		build/tmpfile/gstfdpay.h \
		build/tmpfile/gsttmpfileallocator.c \
		build/tmpfile/gsttmpfileallocator.h \
		build/tmpfile/wire-protocol.h \
		build/tcp/gstmultihandlesink.c \
		build/tcp/gstmultisocketsink.h \
		build/tcp/gstmultisocketsink.c \
		VERSION
	@if ! pkg-config --exists $(PKG_DEPS); then \
		printf "Please install packages $(PKG_DEPS)"; exit 1; fi
	gcc -shared -o $@ $(filter %.c %.o,$^) -fPIC  -Wall -Werror \
		-I build -DHAVE_MMAP=1 $(CFLAGS) $(LDFLAGS) \
		$$(pkg-config --libs --cflags $(PKG_DEPS)) \
		-DVERSION=\"$(VERSION)\" -DPACKAGE="\"pulsevideo\""

build/gstvideosource1.c build/gstvideosource1.h : \
		dbus-xml/com.stbtester.VideoSource1.xml
	cd $(dir $@) && \
	gdbus-codegen \
		--generate-c-code $(basename $(notdir $@)) \
		--interface-prefix=com.stbtester. \
		--c-namespace Gst ../$<

TAGS:
	git ls-files | xargs etags

.PHONY: all clean check dist doc install uninstall
.PHONY: FORCE TAGS
