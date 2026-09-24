include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_license

mod_LTLIBRARIES = mod_license.la
mod_license_la_SOURCES  = mod_license.c
mod_skel_la_CFLAGS   = $(AM_CFLAGS)
mod_skel_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_skel_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
