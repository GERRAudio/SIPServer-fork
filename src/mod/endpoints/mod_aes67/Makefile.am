include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_aes67

if HAVE_GST
mod_LTLIBRARIES = mod_aes67.la
aes67_la_SOURCES = mod_aes67.c aes67_api.c
aes67_la_CFLAGS = $(AM_CFLAGS)
aes67_la_CPPFLAGS = -I. $(GST_CFLAGS) $(AM_CPPFLAGS)
aes67_la_LIBADD = $(GST_LIBS) $(switch_builddir)/libfreeswitch.la
aes67_la_LDFLAGS = -avoid-version -module -no-undefined -shared
if ISMAC
#mod_aes67_la_LDFLAGS += 
endif
else
install: error
all: error
error:
	$(error You must install gstreamer-1.0 and its subprojects to build $(MODNAME))
endif