/*
 * ptp_source.h
 *
 * Abstract interface for a PTP status source.  Two concrete
 * implementations exist:
 *
 *   ptp_source_linux.c    — talks to ptp4l over its UDS management socket
 *   ptp_source_windows.c  — uses the Windows native PTP provider
 *                           (%windir%\system32\ptpprov.dll) when present,
 *                           falls back to GridProtectionAlliance/PTPSync,
 *                           otherwise reports PTP_SYNC_NONE.
 *
 * mod_ptp_timer.c picks the right factory at load time and treats the
 * source as an opaque handle.
 */

#ifndef PTP_SOURCE_H
#define PTP_SOURCE_H

#include "mod_ptp_timer.h"

typedef struct ptp_source_s ptp_source_t;

/**
 * Create the platform PTP source.  The implementation reads the
 * configured ptp4l socket path or PTPSync DLL name from
 * mod_ptp_globals as needed.
 *
 * Returns SWITCH_STATUS_SUCCESS even if the PTP daemon is not yet
 * running — the source will report PTP_SYNC_NONE until it can connect.
 * Returns SWITCH_STATUS_FALSE only if no PTP support is available on
 * this host at all (e.g. Windows without ptpprov.dll and without a
 * PTPSync DLL), so the caller can refuse to load the module.
 */
switch_status_t ptp_source_create(ptp_source_t **src_out, switch_memory_pool_t *pool);

/**
 * Poll the source for the current PTP status.  Implementations are
 * expected to be cheap (microseconds) — mod_ptp_timer.c calls this
 * once per PTP_STATUS_POLL_MS from a dedicated thread.
 *
 * status_out->sample_us is filled by the implementation.
 */
switch_status_t ptp_source_poll(ptp_source_t *src, ptp_status_t *status_out);

/**
 * Release any OS resources (sockets, DLL handles, threads) the source
 * acquired.  Safe to call with NULL.
 */
void ptp_source_destroy(ptp_source_t **src);

#endif /* PTP_SOURCE_H */
