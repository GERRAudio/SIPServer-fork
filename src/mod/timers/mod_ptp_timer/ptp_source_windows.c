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
#include "ptp_native.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
#include <wchar.h>

/* -- W32Time late-bound API contract ---------------------------------
 *
 * w32time.dll exports (documented in the Windows SDK header w32time.h
 * but not declared in <windows.h>):
 *
 *   HRESULT WINAPI W32TimeQuerySource(LPWSTR *ppwszSource);
 *   HRESULT WINAPI W32TimeQueryStatus(W32TIME_STATUS_INFO_LEVEL Level,
 *                                     PVOID pInfo);
 *
 * We only need W32TimeStatusBasicInfo (= 0).  The struct layout below
 * matches w32time.h; w32time fills in *ulSize* with sizeof so a
 * mismatched build is detectable.
 * --------------------------------------------------------------------*/

typedef enum {
	W32TimeStatusBasicInfo = 0
} W32TIME_STATUS_INFO_LEVEL;

#pragma pack(push, 8)
typedef struct {
	ULONG    ulSize;
	UCHAR    eLeapIndicator;       /* 0 = OK, 3 = LI_ALARM (no sync) */
	UCHAR    nStratum;             /* 0/16 = unsynchronised          */
	CHAR     nPollInterval;
	LARGE_INTEGER qwLastSyncTicks;
	LARGE_INTEGER qwPhaseOffset;   /* 100ns units, signed            */
	ULONG    ulRootDelay;
	ULONG    ulRootDispersion;
	GUID     uuidSourceClockId;
	ULONG    ulClockRate;
	ULONG    eState;
	ULONG    ulLastSyncError;      /* 0 = ERROR_SUCCESS              */
	WCHAR    wszLastSyncErrorMsg[256];
	ULONG    eMonitorReturnCode;
	GUID     uuidMonitorClockId;
} W32TIME_STATUS_INFO;
#pragma pack(pop)

typedef HRESULT (WINAPI *w32t_query_source_fn)(LPWSTR *ppwszSource);
typedef HRESULT (WINAPI *w32t_query_status_fn)(W32TIME_STATUS_INFO_LEVEL, PVOID);
typedef VOID    (WINAPI *w32t_free_string_fn)(LPWSTR);

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
	WIN_PTP_NATIVE_CLIENT,    /* built-in PTPv2 client (ptp_native.c)   */
	WIN_PTP_NATIVE,           /* w32time + ptpprov.dll                  */
	WIN_PTP_FALLBACK,         /* PTPSync DLL                            */
} win_ptp_kind_t;

struct ptp_source_s {
	switch_memory_pool_t *pool;
	win_ptp_kind_t        kind;

	/* Built-in PTPv2 client */
	ptp_native_t         *native_client;

	/* Native — late-bound w32time.dll exports */
	HMODULE               native_mod;
	w32t_query_source_fn  w32t_source;
	w32t_query_status_fn  w32t_status;
	w32t_free_string_fn   w32t_free;     /* may be NULL — uses LocalFree fallback */

	/* Fallback */
	HMODULE               fallback_mod;
	ptpsync_init_fn       fb_init;
	ptpsync_shutdown_fn   fb_shutdown;
	ptpsync_status_fn     fb_status;
	char                  fb_dll_name[260];
};

/* -------- detection helpers -------- */

/* Parse "bmca" | "first" | "locked:AA:BB:..:HH" into a ptp_native_cfg_t.
 * Returns SWITCH_STATUS_SUCCESS even on bad input — falls back to BMCA. */
static switch_status_t parse_native_priority(const char *s,
											 ptp_native_prio_t *mode_out,
											 uint8_t locked_id_out[8])
{
	memset(locked_id_out, 0, 8);
	*mode_out = PTP_NATIVE_PRIO_BMCA;

	if (zstr(s) || !strcasecmp(s, "bmca")) {
		return SWITCH_STATUS_SUCCESS;
	}
	if (!strcasecmp(s, "first")) {
		*mode_out = PTP_NATIVE_PRIO_FIRST;
		return SWITCH_STATUS_SUCCESS;
	}
	if (!strncasecmp(s, "locked:", 7)) {
		const char *q = s + 7;
		unsigned int b[8];
		if (sscanf(q, "%2x:%2x:%2x:%2x:%2x:%2x:%2x:%2x",
				   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) == 8) {
			int i;
			for (i = 0; i < 8; i++) locked_id_out[i] = (uint8_t)(b[i] & 0xFF);
			*mode_out = PTP_NATIVE_PRIO_LOCKED;
			return SWITCH_STATUS_SUCCESS;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ptp: bad ptp-priority '%s' — falling back to BMCA\n", s);
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t try_native_client(ptp_source_t *src)
{
	ptp_native_cfg_t cfg;
	memset(&cfg, 0, sizeof(cfg));

	cfg.iface  = mod_ptp_globals.ptp_iface[0] ? mod_ptp_globals.ptp_iface : NULL;
	cfg.domain = mod_ptp_globals.ptp_domain;
	parse_native_priority(mod_ptp_globals.ptp_priority,
						  &cfg.prio_mode, cfg.locked_clock_id);

	cfg.dreq_mode = PTP_NATIVE_DREQ_AUTO;
	if (!strcasecmp(mod_ptp_globals.ptp_dreq_mode, "multicast")) {
		cfg.dreq_mode = PTP_NATIVE_DREQ_MULTICAST;
	} else if (!strcasecmp(mod_ptp_globals.ptp_dreq_mode, "unicast")) {
		cfg.dreq_mode = PTP_NATIVE_DREQ_UNICAST;
	}

	if (ptp_native_create(&src->native_client, src->pool, &cfg) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	src->kind = WIN_PTP_NATIVE_CLIENT;
	return SWITCH_STATUS_SUCCESS;
}

static switch_bool_t native_ptpprov_present(void)
{
	char windir[MAX_PATH];
	char path[MAX_PATH];
	UINT n = GetWindowsDirectoryA(windir, sizeof(windir));
	if (!n || n >= sizeof(windir)) return SWITCH_FALSE;
	switch_snprintf(path, sizeof(path), "%s\\system32\\ptpprov.dll", windir);
	return (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) ? SWITCH_TRUE : SWITCH_FALSE;
}

static switch_bool_t w32time_service_running(void)
{
	SC_HANDLE scm, svc;
	SERVICE_STATUS_PROCESS ssp;
	DWORD needed = 0;
	switch_bool_t running = SWITCH_FALSE;

	scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
	if (!scm) return SWITCH_FALSE;

	svc = OpenServiceA(scm, "W32Time", SERVICE_QUERY_STATUS);
	if (svc) {
		if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
								 (LPBYTE)&ssp, sizeof(ssp), &needed)) {
			running = (ssp.dwCurrentState == SERVICE_RUNNING) ? SWITCH_TRUE : SWITCH_FALSE;
		}
		CloseServiceHandle(svc);
	}
	CloseServiceHandle(scm);
	return running;
}

static switch_bool_t ptpclient_provider_enabled(void)
{
	HKEY hk;
	DWORD val = 0, type = 0, cb = sizeof(val);
	LONG r;

	r = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
		"SYSTEM\\CurrentControlSet\\Services\\W32Time\\TimeProviders\\PtpClient",
		0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hk);
	if (r != ERROR_SUCCESS) return SWITCH_FALSE;

	r = RegQueryValueExA(hk, "Enabled", NULL, &type, (LPBYTE)&val, &cb);
	RegCloseKey(hk);

	return (r == ERROR_SUCCESS && type == REG_DWORD && val != 0) ? SWITCH_TRUE : SWITCH_FALSE;
}

static switch_status_t try_native(ptp_source_t *src)
{
	if (!native_ptpprov_present()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						  "ptp: native provider unavailable — ptpprov.dll not found\n");
		return SWITCH_STATUS_FALSE;
	}
	if (!ptpclient_provider_enabled()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						  "ptp: native provider unavailable — "
						  "W32Time\\TimeProviders\\PtpClient\\Enabled != 1\n");
		return SWITCH_STATUS_FALSE;
	}
	if (!w32time_service_running()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ptp: native provider unavailable — W32Time service not running\n");
		return SWITCH_STATUS_FALSE;
	}

	src->native_mod = LoadLibraryA("w32time.dll");
	if (!src->native_mod) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ptp: native provider unavailable — LoadLibrary(w32time.dll) failed (%lu)\n",
						  GetLastError());
		return SWITCH_STATUS_FALSE;
	}

	src->w32t_source = (w32t_query_source_fn)GetProcAddress(src->native_mod, "W32TimeQuerySource");
	src->w32t_status = (w32t_query_status_fn)GetProcAddress(src->native_mod, "W32TimeQueryStatus");
	src->w32t_free   = (w32t_free_string_fn) GetProcAddress(src->native_mod, "W32TimeFreeString");

	if (!src->w32t_source || !src->w32t_status) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ptp: native provider unavailable — w32time.dll missing required exports\n");
		FreeLibrary(src->native_mod);
		src->native_mod = NULL;
		return SWITCH_STATUS_FALSE;
	}

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

	if (mod_ptp_globals.use_native &&
		try_native_client(src) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "ptp: using built-in PTPv2 client\n");
		*src_out = src;
		return SWITCH_STATUS_SUCCESS;
	}

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
					  "(no built-in client started, no %%windir%%\\system32\\ptpprov.dll, "
					  "and PTPSync DLL not loadable)\n");
	*src_out = NULL;
	return SWITCH_STATUS_FALSE;
}

static switch_status_t poll_native(ptp_source_t *src, ptp_status_t *st)
{
	LPWSTR src_name = NULL;
	HRESULT hr;
	W32TIME_STATUS_INFO info;
	switch_bool_t is_ptp = SWITCH_FALSE;

	switch_snprintf(st->source_desc, sizeof(st->source_desc), "windows-ptpprov");

	/* 1) Which provider is w32time currently using? */
	hr = src->w32t_source(&src_name);
	if (SUCCEEDED(hr) && src_name) {
		/* "PTP" is what the PtpClient provider reports.  Anything else
		 * (NTP, NT5DS, Local CMOS Clock, Free-running System Clock, VMIC)
		 * means the system clock is NOT PTP-disciplined right now. */
		is_ptp = (_wcsicmp(src_name, L"PTP") == 0) ? SWITCH_TRUE : SWITCH_FALSE;

		/* Stash a short ASCII tag so 'ptp status' tells the operator
		 * what w32time is actually doing. */
		WideCharToMultiByte(CP_UTF8, 0, src_name, -1,
							st->port_state, sizeof(st->port_state) - 1,
							NULL, NULL);

		if (src->w32t_free) {
			src->w32t_free(src_name);
		} else {
			LocalFree(src_name);
		}
	} else {
		switch_snprintf(st->port_state, sizeof(st->port_state), "QUERY_SOURCE_FAIL");
	}

	if (!is_ptp) {
		st->state = PTP_SYNC_NONE;
		st->master_offset_ns = 0;
		st->path_delay_ns    = 0;
		st->servo_steps      = 0;
		switch_snprintf(st->grandmaster_id, sizeof(st->grandmaster_id),
						"00:00:00:00:00:00:00:00");
		return SWITCH_STATUS_SUCCESS;
	}

	/* 2) PTP is selected — query servo health. */
	memset(&info, 0, sizeof(info));
	info.ulSize = sizeof(info);
	hr = src->w32t_status(W32TimeStatusBasicInfo, &info);
	if (FAILED(hr)) {
		st->state = PTP_SYNC_HOLDOVER;
		switch_snprintf(st->grandmaster_id, sizeof(st->grandmaster_id),
						"00:00:00:00:00:00:00:00");
		return SWITCH_STATUS_SUCCESS;
	}

	/* qwPhaseOffset is in 100ns units, signed. */
	st->master_offset_ns = (int64_t)info.qwPhaseOffset.QuadPart * 100;
	st->path_delay_ns    = (int64_t)info.ulRootDelay * 100;
	st->servo_steps      = (uint32_t)info.nStratum;

	/* uuidSourceClockId — render as the EUI-64 we promise. Take 8 bytes. */
	{
		const unsigned char *b = (const unsigned char *)&info.uuidSourceClockId;
		switch_snprintf(st->grandmaster_id, sizeof(st->grandmaster_id),
						"%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
						b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
	}

	/* Map to coarse state.
	 *   eLeapIndicator == 3 (LI_ALARM) => not yet locked / no GM
	 *   nStratum 0 or 16              => unsynchronised
	 *   ulLastSyncError != 0          => last servo step failed (holdover)
	 */
	if (info.eLeapIndicator == 3 || info.nStratum == 0 || info.nStratum == 16) {
		st->state = PTP_SYNC_HOLDOVER;
	} else if (info.ulLastSyncError != 0) {
		st->state = PTP_SYNC_HOLDOVER;
	} else {
		st->state = PTP_SYNC_LOCKED;
	}

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
	case WIN_PTP_NATIVE_CLIENT: return ptp_native_poll(src->native_client, st);
	case WIN_PTP_NATIVE:        return poll_native(src, st);
	case WIN_PTP_FALLBACK:      return poll_fallback(src, st);
	default:                    return SWITCH_STATUS_SUCCESS;
	}
}

void ptp_source_destroy(ptp_source_t **src_io)
{
	ptp_source_t *src;
	if (!src_io || !*src_io) return;
	src = *src_io;
	if (src->kind == WIN_PTP_NATIVE_CLIENT) {
		ptp_native_destroy(&src->native_client);
	}
	if (src->kind == WIN_PTP_FALLBACK) {
		if (src->fb_shutdown) src->fb_shutdown();
		if (src->fallback_mod) FreeLibrary(src->fallback_mod);
	}
	if (src->native_mod) FreeLibrary(src->native_mod);
	*src_io = NULL;
}

#endif /* _WIN32 */
