include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_ravenna

mod_LTLIBRARIES = mod_ravenna.la
mod_ravenna_la_SOURCES = \
    mod_ravenna.c \
    ravenna_net.c \
    ravenna_rtp.c \
    ravenna_ring.c \
    ravenna_rx.c \
    ravenna_tx.c

mod_ravenna_la_CFLAGS   = $(AM_CFLAGS)
mod_ravenna_la_CPPFLAGS = -I.
mod_ravenna_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_ravenna_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
