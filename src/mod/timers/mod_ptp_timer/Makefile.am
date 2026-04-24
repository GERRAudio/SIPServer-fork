include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_ptp_timer

mod_LTLIBRARIES = mod_ptp_timer.la
mod_ptp_timer_la_SOURCES = mod_ptp_timer.c ptp_source_linux.c ptp_source_windows.c
mod_ptp_timer_la_CFLAGS  = $(AM_CFLAGS)
mod_ptp_timer_la_CPPFLAGS = -I.
mod_ptp_timer_la_LIBADD  = $(switch_builddir)/libfreeswitch.la
mod_ptp_timer_la_LDFLAGS = -avoid-version -module -no-undefined -shared
