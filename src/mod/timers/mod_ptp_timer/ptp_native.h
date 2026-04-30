/*
 * ptp_native.h
 *
 * Built-in PTPv2 (IEEE 1588-2008) slave client.
 *
 * Cross-platform (Linux + Windows) software-timestamped multicast PTP
 * slave.  Implements:
 *
 *   - Announce / Sync / Follow_Up / Delay_Req / Delay_Resp
 *   - End-to-End delay mechanism (no P2P)
 *   - BMCA (Best Master Clock Algorithm) per IEEE 1588-2008 §9.3.4 over
 *     the set of masters seen on the wire; failover when the active
 *     grandmaster misses 3× its announceInterval
 *   - Internal PI servo on master offset; we DO NOT steer the OS clock
 *   - Snapshot reported via the existing ptp_status_t structure
 *
 * Design choice: PTP time is exposed as an *internal* offset that the
 * caller adds to switch_micro_time_now() when it needs a PTP-aligned
 * wall clock.  The OS clock is never touched.  This avoids fights with
 * w32time / NTP and removes the need for elevated privileges beyond
 * binding UDP/319 + UDP/320.
 *
 * Slave-only.  Single iface + single domain per process.
 */

#ifndef PTP_NATIVE_H
#define PTP_NATIVE_H

#include "mod_ptp_timer.h"
#include <stdint.h>

/* Forward */
typedef struct ptp_native_s ptp_native_t;

/* Master selection policy */
typedef enum {
	PTP_NATIVE_PRIO_BMCA = 0,   /**< full IEEE 1588-2008 BMCA              */
	PTP_NATIVE_PRIO_FIRST,      /**< first master seen wins (node-ptpv2)   */
	PTP_NATIVE_PRIO_LOCKED      /**< pinned to cfg.locked_clock_id         */
} ptp_native_prio_t;

/* Delay_Req delivery mode.
 *  MULTICAST — always send to 224.0.1.129 (default IEEE 1588 behavior)
 *  UNICAST   — always send unicast to the master's source IPv4
 *  AUTO      — start multicast; if no Delay_Resp arrives after a few
 *              attempts, switch automatically to unicast.  This is the
 *              right default for SMPTE-2059-2 deployments where many
 *              GMs ignore multicast Delay_Req. */
typedef enum {
	PTP_NATIVE_DREQ_AUTO = 0,
	PTP_NATIVE_DREQ_MULTICAST,
	PTP_NATIVE_DREQ_UNICAST
} ptp_native_dreq_mode_t;

/* Configuration — all fields are caller-owned, copied at create() time. */
typedef struct {
	const char           *iface;       /**< NIC name (Linux) or dotted IPv4 */
	uint8_t               domain;      /**< PTP domain number (0-127)       */
	ptp_native_prio_t     prio_mode;
	uint8_t               locked_clock_id[8];   /**< only used when PRIO_LOCKED */
	ptp_native_dreq_mode_t dreq_mode;
} ptp_native_cfg_t;

/* -----------------------------------------------------------------------
 *  Lifecycle
 * --------------------------------------------------------------------- */

/**
 * Construct a native PTP client.  Opens the multicast sockets, starts
 * the RX thread.  Returns SWITCH_STATUS_FALSE if sockets cannot be
 * opened (e.g. UDP/319 already bound by another process or insufficient
 * privileges).
 */
switch_status_t ptp_native_create(ptp_native_t **out,
								  switch_memory_pool_t *pool,
								  const ptp_native_cfg_t *cfg);

/**
 * Fill the standard ptp_status_t snapshot.  Cheap (microseconds): just
 * copies the latest under the snapshot mutex.
 */
switch_status_t ptp_native_poll(ptp_native_t *p, ptp_status_t *out);

/**
 * Stop the RX thread and release sockets.
 */
void ptp_native_destroy(ptp_native_t **p_io);

/* -----------------------------------------------------------------------
 *  Time access (internal-offset model)
 * --------------------------------------------------------------------- */

/**
 * Return the current PTP time in nanoseconds since the PTP epoch
 * (1970-01-01 TAI), computed as the local monotonic clock plus the
 * servo's internal offset.  Returns 0 if not yet locked.
 */
int64_t ptp_native_now_ns(ptp_native_t *p);

/**
 * Returns SWITCH_TRUE iff the servo currently considers itself locked
 * to a grandmaster.
 */
switch_bool_t ptp_native_is_locked(ptp_native_t *p);

#endif /* PTP_NATIVE_H */
