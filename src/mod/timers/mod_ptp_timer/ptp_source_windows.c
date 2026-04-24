/*
 * ptp_source_windows.c
 *
 * PTP status source for Windows.  Detection order, per design:
 *
 *   1. Native Windows PTP provider:
 *        %windir%\system32\ptpprov.dll
 *      If present, we treat the system clock as PTP-disciplined and
 *      query offset/leap/state via the public time-provider APIs
 *      exposed by w32time (GetSystemTimeAdjustmentPrecise +
 *      whatever ptpprov publishes through performance counters).
 *
 *   2. PTPSync fallback:
 *        Configurable DLL name (default: "PTPSyncNative.dll") loaded
 *        from the FreeSWITCH process directory or PATH.
 *        We resolve a small set of named exports and call them; if any
 *        are missing we treat the fallback as unavailable.
 *
 *   3. Fail:
 *        ptp_source_create() returns SWITCH_STATUS_FALSE so the module
 *        load is aborted.
 *
 * This file compiles to nothing on non-Windows builds.
 */

#ifdef _WIN32

#include "ptp_source.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* -- PTPSync optional export contract --------------------------------
 *
 *   BOOL  PTPSync_Initialize(void);
 *   void  PTPSync_Shutdown(void);
 *   BOOL  PTPSync_GetStatus(PTPSYNC_STATUS *out);
 *
 * Where PTPSYNC_STATUS matches the layout below.  This is a thin
 * binding contract; the actual GridProtectionAlliance/PTPSync managed
 * library can be wrapped by a small native shim that exports these
 * three entry points.
 * --------------------------------------------------------------------*/

#pragma pack(push, 1)
typedef struct {
	int      locked;            /* 0/1 */
	long long offset_ns;        /* signed nanoseconds from GM */
	long long path_delay_ns;    /* signed nanoseconds */
	unsigned char gm_id[8];     /* IEEE EUI-64 */
	char     port_state[24];    /* ASCII */
} PTPSYNC_STATUS;
#pragma pack(pop)

typedef BOOL (WINAPI *ptpsync_init_fn)(void);
typedef void (WINAPI *ptpsync_shutdown_fn)(void);
typedef BOOL (WINAPI *ptpsync_status_fn)(PTPSYNC_STATUS *out);

typedef enum {
	WIN_PTP_NONE = 0,
	WIN_PTP_NATIVE,
	WIN_PTP_FALLBACK,
} win_ptp_kind_t;

struct ptp_source_s {
	switch_memory_pool_t *pool;
	win_ptp_kind_t        kind;

	/* Native */
	HMODULE               native_mod;

	/* Fallback */
	HMODULE               fallback_mod;
	ptpsync_init_fn       fb_init;
	ptpsync_shutdown_fn   fb_shutdown;
	ptpsync_status_fn     fb_status;
	char                  fb_dll_name[260];
};

/* -------- detection helpers -------- */

static switch_bool_t native_ptpprov_present(void)
{
	char windir[MAX_PATH];
	char path[MAX_PATH];
	UINT n = GetWindowsDirectoryA(windir, sizeof(windir));
	if (!n || n >= sizeof(windir)) return SWITCH_FALSE;
	switch_snprintf(path, sizeof(path), "%s\\system32\\ptpprov.dll", windir);
	return (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) ? SWITCH_TRUE : SWITCH_FALSE;
}

static switch_status_t try_native(ptp_source_t *src)
{
	if (!native_ptpprov_present()) {
		return SWITCH_STATUS_FALSE;
	}
	/* We do not LoadLibrary ptpprov.dll directly — w32time owns it.
	 * Its presence is enough to trust GetSystemTimePreciseAsFileTime
	 * and GetSystemTimeAdjustmentPrecise as PTP-disciplined sources. */
	src->kind = WIN_PTP_NATIVE;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t try_fallback(ptp_source_t *src)
{
	const char *dll = mod_ptp_globals.ptpsync_dll;

	if (zstr(dll)) dll = "PTPSyncNative.dll";

	switch_snprintf(src->fb_dll_name, sizeof(src->fb_dll_name), "%s", dll);

	src->fallback_mod = LoadLibraryA(dll);
	if (!src->fallback_mod) {
		return SWITCH_STATUS_FALSE;
	}

	src->fb_init     = (ptpsync_init_fn)    GetProcAddress(src->fallback_mod, "PTPSync_Initialize");
	src->fb_shutdown = (ptpsync_shutdown_fn)GetProcAddress(src->fallback_mod, "PTPSync_Shutdown");
	src->fb_status   = (ptpsync_status_fn)  GetProcAddress(src->fallback_mod, "PTPSync_GetStatus");

	if (!src->fb_init || !src->fb_status) {
		FreeLibrary(src->fallback_mod);
		src->fallback_mod = NULL;
		return SWITCH_STATUS_FALSE;
	}

	if (!src->fb_init()) {
		if (src->fb_shutdown) src->fb_shutdown();
		FreeLibrary(src->fallback_mod);
		src->fallback_mod = NULL;
		return SWITCH_STATUS_FALSE;
	}

	src->kind = WIN_PTP_FALLBACK;
	return SWITCH_STATUS_SUCCESS;
}

/* -------- public API -------- */

switch_status_t ptp_source_create(ptp_source_t **src_out, switch_memory_pool_t *pool)
{
	ptp_source_t *src = switch_core_alloc(pool, sizeof(*src));

	memset(src, 0, sizeof(*src));
	src->pool = pool;
	src->kind = WIN_PTP_NONE;

	if (try_native(src) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "ptp: using native Windows PTP provider (ptpprov.dll)\n");
		*src_out = src;
		return SWITCH_STATUS_SUCCESS;
	}

	if (try_fallback(src) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "ptp: using PTPSync fallback (%s)\n", src->fb_dll_name);
		*src_out = src;
		return SWITCH_STATUS_SUCCESS;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
					  "ptp: no PTP provider available "
					  "(no %%windir%%\\system32\\ptpprov.dll and PTPSync DLL not loadable)\n");
	*src_out = NULL;
	return SWITCH_STATUS_FALSE;
}

static switch_status_t poll_native(ptp_source_t *src, ptp_status_t *st)
{
	DWORD adj_ms = 0, incr_ms = 0;
	BOOL  disabled = TRUE;

	(void)src;

	switch_snprintf(st->source_desc, sizeof(st->source_desc), "windows-ptpprov");

	/* We do not get a fine-grained offset from public Win32 APIs; what
	 * we can authoritatively report is whether time adjustment is
	 * disabled (FALSE => clock is being disciplined). */
	if (GetSystemTimeAdjustment(&adj_ms, &incr_ms, &disabled)) {
		st->state = disabled ? PTP_SYNC_HOLDOVER : PTP_SYNC_LOCKED;
	} else {
		st->state = PTP_SYNC_HOLDOVER;
	}

	switch_snprintf(st->port_state, sizeof(st->port_state),
					disabled ? "FREE_RUN" : "DISCIPLINED");
	switch_snprintf(st->grandmaster_id, sizeof(st->grandmaster_id),
					"00:00:00:00:00:00:00:00");
	st->master_offset_ns = 0;
	st->path_delay_ns    = 0;
	st->servo_steps      = 0;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t poll_fallback(ptp_source_t *src, ptp_status_t *st)
{
	PTPSYNC_STATUS s;
	memset(&s, 0, sizeof(s));

	switch_snprintf(st->source_desc, sizeof(st->source_desc),
					"ptpsync:%s", src->fb_dll_name);

	if (!src->fb_status(&s)) {
		st->state = PTP_SYNC_NONE;
		return SWITCH_STATUS_SUCCESS;
	}

	st->master_offset_ns = (int64_t)s.offset_ns;
	st->path_delay_ns    = (int64_t)s.path_delay_ns;
	switch_snprintf(st->port_state, sizeof(st->port_state), "%.*s",
					(int)sizeof(s.port_state), s.port_state);
	switch_snprintf(st->grandmaster_id, sizeof(st->grandmaster_id),
					"%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
					s.gm_id[0], s.gm_id[1], s.gm_id[2], s.gm_id[3],
					s.gm_id[4], s.gm_id[5], s.gm_id[6], s.gm_id[7]);

	st->state = s.locked ? PTP_SYNC_LOCKED : PTP_SYNC_HOLDOVER;
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t ptp_source_poll(ptp_source_t *src, ptp_status_t *st)
{
	memset(st, 0, sizeof(*st));
	st->state     = PTP_SYNC_NONE;
	st->sample_us = switch_micro_time_now();

	switch (src->kind) {
	case WIN_PTP_NATIVE:   return poll_native(src, st);
	case WIN_PTP_FALLBACK: return poll_fallback(src, st);
	default:               return SWITCH_STATUS_SUCCESS;
	}
}

void ptp_source_destroy(ptp_source_t **src_io)
{
	ptp_source_t *src;
	if (!src_io || !*src_io) return;
	src = *src_io;
	if (src->kind == WIN_PTP_FALLBACK) {
		if (src->fb_shutdown) src->fb_shutdown();
		if (src->fallback_mod) FreeLibrary(src->fallback_mod);
	}
	if (src->native_mod) FreeLibrary(src->native_mod);
	*src_io = NULL;
}

#endif /* _WIN32 */
