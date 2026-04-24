/*
 * mod_ptp_timer.h
 *
 * Shared types for mod_ptp_timer — a FreeSWITCH timer module that drives
 * its tick from a PTP-disciplined system clock and exposes the PTP
 * grandmaster / offset / sync-state from either ptp4l (Linux) or the
 * Windows native PTP provider (ptpprov.dll) with a PTPSync fallback.
 *
 * All public constants used by both the core and the platform sources
 * live here so the source files do not have circular dependencies.
 */

#ifndef MOD_PTP_TIMER_H
#define MOD_PTP_TIMER_H

#include <switch.h>
#include <stdint.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Module identity
 * --------------------------------------------------------------------- */

#define PTP_TIMER_NAME            "ptp"   /**< switch_core_timer_init() name */

/* Default 1 ms tick — finest cadence FreeSWITCH actually uses. */
#define PTP_DEFAULT_INTERVAL_MS   1
#define PTP_MAX_INTERVAL_MS       120

/* Status poll cadence — 1 s is enough; PTP state rarely flips faster. */
#define PTP_STATUS_POLL_MS        1000

/* -----------------------------------------------------------------------
 * Sync state — coarse summary the upper layers care about
 * --------------------------------------------------------------------- */

typedef enum {
	PTP_SYNC_UNKNOWN = 0,   /**< source not initialised yet */
	PTP_SYNC_NONE,          /**< no PTP source available    */
	PTP_SYNC_HOLDOVER,      /**< had sync, lost grandmaster */
	PTP_SYNC_LOCKED,        /**< actively tracking GM       */
} ptp_sync_state_t;

static inline const char *ptp_sync_state_str(ptp_sync_state_t s)
{
	switch (s) {
	case PTP_SYNC_NONE:     return "none";
	case PTP_SYNC_HOLDOVER: return "holdover";
	case PTP_SYNC_LOCKED:   return "locked";
	default:                return "unknown";
	}
}

/* -----------------------------------------------------------------------
 * Status snapshot — populated by the platform PTP source
 *
 * All fields are populated best-effort.  Sources that cannot read a
 * particular value leave it zero / empty.
 * --------------------------------------------------------------------- */

#define PTP_GM_ID_STR_LEN     32   /**< IEEE EUI-64 hex with separators */
#define PTP_PORT_STATE_LEN    24
#define PTP_SOURCE_DESC_LEN   64

typedef struct {
	ptp_sync_state_t state;

	/** Grandmaster clock identity — formatted as "AA:BB:CC:FF:FE:DD:EE:FF". */
	char     grandmaster_id[PTP_GM_ID_STR_LEN];

	/** Offset from master in nanoseconds. Negative if local is ahead. */
	int64_t  master_offset_ns;

	/** Mean path delay to grandmaster in nanoseconds. */
	int64_t  path_delay_ns;

	/** Number of clock servos completed; 0 means not steady-state yet. */
	uint32_t servo_steps;

	/** Local PTP port state, e.g. "SLAVE", "MASTER", "PASSIVE". */
	char     port_state[PTP_PORT_STATE_LEN];

	/** Free-form description of the source (e.g. "ptp4l@/var/run/ptp4l",
	 *  "windows-ptpprov", "ptpsync-fallback"). */
	char     source_desc[PTP_SOURCE_DESC_LEN];

	/** Wall-clock microseconds (switch_micro_time_now) when this snapshot
	 *  was produced — useful for staleness checks. */
	switch_time_t sample_us;
} ptp_status_t;

/* -----------------------------------------------------------------------
 * FreeSWITCH event subclasses (CUSTOM)
 *
 * Subscribers can register against any of these to react to PTP changes.
 *   ptp::status              fired every poll (low rate)
 *   ptp::sync_acquired       UNKNOWN/NONE/HOLDOVER -> LOCKED
 *   ptp::sync_lost           LOCKED -> HOLDOVER/NONE
 *   ptp::grandmaster_changed grandmaster_id changed while LOCKED
 * --------------------------------------------------------------------- */

#define PTP_EVENT_STATUS              "ptp::status"
#define PTP_EVENT_SYNC_ACQUIRED       "ptp::sync_acquired"
#define PTP_EVENT_SYNC_LOST           "ptp::sync_lost"
#define PTP_EVENT_GRANDMASTER_CHANGED "ptp::grandmaster_changed"

/* -----------------------------------------------------------------------
 * Module-global runtime state — defined in mod_ptp_timer.c
 * --------------------------------------------------------------------- */

typedef struct {
	switch_memory_pool_t *pool;
	switch_mutex_t       *mutex;        /**< protects status snapshot */
	switch_bool_t         debug;        /**< per-tick traces          */

	ptp_status_t          last_status;  /**< most recent snapshot     */

	/** Status thread */
	switch_thread_t      *status_thread;
	switch_bool_t         running;

	/** Linux source: path to ptp4l UDS management socket
	 *  (config: <param name="ptp4l-socket" value="/var/run/ptp4l"/>) */
	char                  ptp4l_socket[256];

	/** Windows source: filename of the PTPSync fallback DLL.
	 *  (config: <param name="ptpsync-dll" value="PTPSyncNative.dll"/>) */
	char                  ptpsync_dll[256];
} mod_ptp_globals_t;

extern mod_ptp_globals_t mod_ptp_globals;

/* -----------------------------------------------------------------------
 * Conditional per-tick debug log (suppressed unless 'ptp debug on')
 * --------------------------------------------------------------------- */
#define PTP_LOG_DEBUG(fmt, ...) \
	do { \
		if (mod_ptp_globals.debug == SWITCH_TRUE) { \
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, fmt, ##__VA_ARGS__); \
		} \
	} while (0)

#endif /* MOD_PTP_TIMER_H */
