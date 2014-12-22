plugin_LTLIBRARIES = libgsttmpfile.la

libgsttmpfile_la_SOURCES = \
	gsttmpfile.c
libgsttmpfile_la_CFLAGS = \
	$(GST_PLUGINS_BAD_CFLAGS) \
	$(GST_BASE_CFLAGS) \
	$(GST_ALLOCATORS_CFLAGS) \
	$(GST_UNIXFD_CFLAGS) \
	$(GST_CFLAGS)
libgsttmpfile_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgsttmpfile_la_LIBADD = \
	$(GST_LIBS) \
	$(GST_BASE_LIBS) \
	$(GST_ALLOCATORS_LIBS) \
	$(GST_UNIXFD_LIBS) -lgstallocators-1.0

libgsttmpfile_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

noinst_HEADERS =
