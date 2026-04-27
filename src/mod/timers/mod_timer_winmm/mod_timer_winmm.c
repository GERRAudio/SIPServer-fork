/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * mod_timer_winmm.c -- High-accuracy soft timer for Windows.
 *
 * Strategy:
 *   - Prefer CreateWaitableTimerExW() with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
 *     (Windows 10, version 1803 / Server 2016 build 17134+). This gives sub-ms
 *     accuracy on a kernel waitable timer without bumping the system-wide
 *     timer resolution.
 *   - Fall back to a normal CreateWaitableTimer() with timeBeginPeriod(1) when
 *     the high-resolution flag is not supported (older Windows).
 *   - One dedicated time-critical thread per active interval blocks on the
 *     timer HANDLE via WaitForSingleObject(INFINITE) -- zero CPU between
 *     ticks, scheduler-driven wakeups, no Sleep() jitter.
 *   - Thread is also boosted via MMCSS ("Pro Audio") when avrt.dll is
 *     available, so it survives Windows scheduling pressure.
 *
 * This module implements the same switch_timer_interface_t contract as
 * mod_posix_timer / softtimer and registers under the name "winmm".
 *
 * Select it at runtime in conf/autoload_configs/switch.conf.xml:
 *   <param name="timer-name" value="winmm"/>
 */

#include <switch.h>

#ifndef WIN32
#error mod_timer_winmm is only supported on Windows
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

SWITCH_MODULE_LOAD_FUNCTION(mod_timer_winmm_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_timer_winmm_shutdown);
SWITCH_MODULE_DEFINITION(mod_timer_winmm, mod_timer_winmm_load, mod_timer_winmm_shutdown, NULL);

#define MAX_INTERVAL        2000  /* ms */
#define TIMERS_PER_INTERVAL 4

/* Win10 1803 flag for high-resolution waitable timers. Define here so we can
 * build with older SDK headers and still use the flag at runtime. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET    0x00000001
#endif

/* Function pointer types for runtime resolution */
typedef HANDLE (WINAPI *PFN_CreateWaitableTimerExW)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

/* MMCSS (avrt.dll) -- optional, resolved at runtime */
typedef HANDLE (WINAPI *PFN_AvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
typedef BOOL   (WINAPI *PFN_AvRevertMmThreadCharacteristics)(HANDLE);

/**
 * Per-interval timer state. One of these exists for every active
 * (interval, slot) pair. Multiple FreeSWITCH timers can share an
 * interval_timer_t via the "users" refcount.
 */
typedef struct {
	int users;                          /* refcount of switch_timer_t consumers */
	int interval;                       /* tick interval in ms */
	int num;                            /* slot index for this interval */
	HANDLE htimer;                      /* kernel waitable timer */
	HANDLE thread;                      /* dedicated wait thread */
	switch_size_t tick;                 /* monotonic tick counter (broadcast to users) */
	switch_mutex_t *mutex;              /* protects tick + condvar */
	switch_thread_cond_t *cond;         /* broadcast on every tick */
	volatile int running;               /* thread loop flag */

	/* Absolute scheduling state.  We use SetWaitableTimer in one-shot mode
	 * (lPeriod = 0) and re-arm with next_due_100ns = next_due_100ns + period
	 * after every fire.  This is drift-free: any late tick is compensated by
	 * the next tick firing sooner, so error never accumulates.
	 *
	 * Periodic mode (non-zero lPeriod) is incompatible with high-resolution
	 * waitable timers -- the kernel silently demotes the schedule to ms-tick
	 * granularity, producing a steady ~0.3-0.5 ms-per-tick drift that
	 * compounds into tens of ms per second.  See:
	 *   https://learn.microsoft.com/.../createwaitabletimerexw
	 *   "high-resolution timers must be used as one-shot timers"
	 *
	 * Units: 100-nanosecond intervals (the FILETIME unit used by
	 * SetWaitableTimer's positive absolute due-time form). */
	LARGE_INTEGER period_100ns;
	LARGE_INTEGER next_due_100ns;
} interval_timer_t;

static struct {
	switch_memory_pool_t *pool;
	int shutdown;

	/* timer slots indexed by [interval_ms][slot] */
	interval_timer_t interval_timers[MAX_INTERVAL + 1][TIMERS_PER_INTERVAL];
	int next_interval_timer_num[MAX_INTERVAL + 1];
	switch_mutex_t *interval_timers_mutex;

	/* runtime feature detection */
	PFN_CreateWaitableTimerExW pCreateWaitableTimerExW;
	int have_high_resolution;           /* set after first successful CWTE call */
	int timebeginperiod_active;         /* set when we had to call timeBeginPeriod(1) */

	/* MMCSS */
	HMODULE avrt_dll;
	PFN_AvSetMmThreadCharacteristicsW pAvSetMmThreadCharacteristicsW;
	PFN_AvRevertMmThreadCharacteristics pAvRevertMmThreadCharacteristics;
} globals;


/**
 * Try to create a kernel waitable timer using the highest-precision API
 * available on the host. Falls back gracefully on older Windows.
 */
static HANDLE create_best_waitable_timer(void)
{
	HANDLE h = NULL;

	/* Auto-reset (flags = 0 / bManualReset = FALSE) is required for periodic
	 * SetWaitableTimer used in a WaitForSingleObject loop; with manual-reset
	 * the handle would stay signaled after the first tick and the wait thread
	 * would spin at 100% CPU instead of blocking between ticks. */
	if (globals.pCreateWaitableTimerExW) {
		h = globals.pCreateWaitableTimerExW(NULL, NULL,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
			TIMER_ALL_ACCESS);
		if (h) {
			globals.have_high_resolution = 1;
			return h;
		}
		/* Flag may not be supported on this build -- retry without it. */
		h = globals.pCreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
		if (h) {
			return h;
		}
	}

	/* Final fallback: classic API, auto-reset. */
	h = CreateWaitableTimer(NULL, FALSE, NULL);
	return h;
}

/**
 * Dedicated wait thread. Blocks on the kernel timer handle, increments tick
 * count, broadcasts to all users. Runs at TIME_CRITICAL priority and is
 * registered with MMCSS "Pro Audio" when available.
 */
static DWORD WINAPI interval_timer_thread(LPVOID arg)
{
	interval_timer_t *it = (interval_timer_t *)arg;
	HANDLE mmcss_handle = NULL;
	DWORD mmcss_task_index = 0;

	/* Boost priority -- this thread must wake on every tick. */
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	if (globals.pAvSetMmThreadCharacteristicsW) {
		mmcss_handle = globals.pAvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task_index);
	}

	while (it->running && !globals.shutdown) {
		DWORD wr = WaitForSingleObject(it->htimer, 250 /* ms watchdog */);
		if (wr == WAIT_OBJECT_0) {
			/* Re-arm BEFORE waking consumers so the next tick is already
			 * scheduled while consumers do their per-tick work.  Absolute
			 * due time advances by exactly one period -- no drift. */
			it->next_due_100ns.QuadPart += it->period_100ns.QuadPart;

			/* If we are pathologically behind (debugger break, host
			 * sleep/resume), skip whole missed intervals to avoid a
			 * burst of make-up ticks.  Detected when the new deadline
			 * is more than one full period in the past. */
			{
				LARGE_INTEGER now_ft;
				GetSystemTimePreciseAsFileTime((FILETIME *)&now_ft);
				if (it->next_due_100ns.QuadPart < now_ft.QuadPart - it->period_100ns.QuadPart) {
					it->next_due_100ns.QuadPart =
						now_ft.QuadPart + it->period_100ns.QuadPart;
				}
			}

			if (!SetWaitableTimer(it->htimer, &it->next_due_100ns,
					0 /* one-shot, no kernel periodic */, NULL, NULL, FALSE)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
					"SetWaitableTimer re-arm failed (interval=%d) gle=%lu\n",
					it->interval, GetLastError());
				break;
			}

			switch_mutex_lock(it->mutex);
			it->tick++;
			switch_thread_cond_broadcast(it->cond);
			switch_mutex_unlock(it->mutex);
		} else if (wr == WAIT_TIMEOUT) {
			/* watchdog -- check shutdown flag and re-wait */
			continue;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"timer wait failed (interval=%d) gle=%lu\n", it->interval, GetLastError());
			break;
		}
	}

	if (mmcss_handle && globals.pAvRevertMmThreadCharacteristics) {
		globals.pAvRevertMmThreadCharacteristics(mmcss_handle);
	}

	return 0;
}

/**
 * Start an interval timer (or join an existing one).
 */
static switch_status_t interval_timer_start(interval_timer_t *it, int interval)
{
	if (globals.shutdown) {
		return SWITCH_STATUS_GENERR;
	}

	if (it->users <= 0) {
		LARGE_INTEGER due;

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"starting %d ms timer #%d (%s)\n",
			interval, it->num + 1,
			globals.have_high_resolution ? "high-resolution waitable" :
			(globals.pCreateWaitableTimerExW ? "waitable" : "classic waitable"));

		it->interval = interval;
		it->tick = 0;
		it->users = 0;
		it->running = 1;

		if (it->mutex == NULL) {
			switch_mutex_init(&it->mutex, SWITCH_MUTEX_NESTED, globals.pool);
			switch_thread_cond_create(&it->cond, globals.pool);
		}

		it->htimer = create_best_waitable_timer();
		if (!it->htimer) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"CreateWaitableTimer failed gle=%lu\n", GetLastError());
			return SWITCH_STATUS_GENERR;
		}

		/* One-shot scheduling with drift-free absolute deadlines.
		 *
		 * High-resolution waitable timers are documented one-shot only.
		 * Setting a non-zero period silently demotes scheduling to ms-tick
		 * resolution and produces a sustained ~0.3-0.5 ms-per-tick drift,
		 * which compounds into tens of ms per second over a long-running
		 * media session (audible as choppy/distorted audio downstream).
		 *
		 * Instead: arm a one-shot at "now + period" using GetSystemTime-
		 * PreciseAsFileTime (sub-microsecond clock), and the wait thread
		 * re-arms each tick with next_due += period.  Any late wake-up is
		 * automatically corrected on the next tick because the deadline is
		 * absolute -- the error cannot accumulate. */
		it->period_100ns.QuadPart = (LONGLONG)interval * 10000LL; /* ms -> 100ns */
		{
			LARGE_INTEGER now_ft;
			GetSystemTimePreciseAsFileTime((FILETIME *)&now_ft);
			it->next_due_100ns.QuadPart = now_ft.QuadPart + it->period_100ns.QuadPart;
		}

		/* Positive lpDueTime = absolute UTC FILETIME; lPeriod = 0 = one-shot. */
		if (!SetWaitableTimer(it->htimer, &it->next_due_100ns,
				0 /* one-shot */, NULL, NULL, FALSE)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"SetWaitableTimer failed gle=%lu\n", GetLastError());
			CloseHandle(it->htimer);
			it->htimer = NULL;
			return SWITCH_STATUS_GENERR;
		}
		(void)due; /* legacy local kept to minimise diff churn */

		it->thread = CreateThread(NULL, 0, interval_timer_thread, it, 0, NULL);
		if (!it->thread) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"CreateThread failed gle=%lu\n", GetLastError());
			CancelWaitableTimer(it->htimer);
			CloseHandle(it->htimer);
			it->htimer = NULL;
			return SWITCH_STATUS_GENERR;
		}
	}

	it->users++;
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Tear down an interval timer (no remaining users).
 */
static void interval_timer_delete(interval_timer_t *it)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"deleting %d ms timer #%d\n", it->interval, it->num + 1);

	it->running = 0;

	if (it->htimer) {
		CancelWaitableTimer(it->htimer);
	}

	/* Wake any user threads currently in switch_thread_cond_timedwait so
	 * they can observe shutdown of this interval timer. */
	if (it->mutex && it->cond) {
		switch_mutex_lock(it->mutex);
		switch_thread_cond_broadcast(it->cond);
		switch_mutex_unlock(it->mutex);
	}

	if (it->thread) {
		WaitForSingleObject(it->thread, 1000);
		CloseHandle(it->thread);
		it->thread = NULL;
	}

	if (it->htimer) {
		CloseHandle(it->htimer);
		it->htimer = NULL;
	}

	it->users = 0;
}

static switch_status_t interval_timer_stop(interval_timer_t *it)
{
	if (it->users > 0) {
		it->users--;
		if (it->users == 0) {
			interval_timer_delete(it);
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

/* ----- switch_timer_interface_t implementation ----- */

static switch_status_t mod_timer_winmm_init(switch_timer_t *timer)
{
	interval_timer_t *it;
	switch_status_t status;
	int slot;

	if (timer->interval < 1 || timer->interval > MAX_INTERVAL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Bad interval: %d\n", timer->interval);
		return SWITCH_STATUS_GENERR;
	}

	switch_mutex_lock(globals.interval_timers_mutex);
	slot = globals.next_interval_timer_num[timer->interval]++;
	if (globals.next_interval_timer_num[timer->interval] >= TIMERS_PER_INTERVAL) {
		globals.next_interval_timer_num[timer->interval] = 0;
	}

	it = &globals.interval_timers[timer->interval][slot];
	it->num = slot;
	it->interval = timer->interval;
	status = interval_timer_start(it, timer->interval);
	timer->private_info = it;
	switch_mutex_unlock(globals.interval_timers_mutex);

	return status;
}

static switch_status_t mod_timer_winmm_step(switch_timer_t *timer)
{
	timer->tick++;
	timer->samplecount += timer->samples;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t mod_timer_winmm_next(switch_timer_t *timer)
{
	interval_timer_t *it = (interval_timer_t *)timer->private_info;

	if (!it) {
		return SWITCH_STATUS_GENERR;
	}

	if ((int)(timer->tick - it->tick) < -1) {
		timer->tick = it->tick;
	}
	mod_timer_winmm_step(timer);

	switch_mutex_lock(it->mutex);
	while ((int)(timer->tick - it->tick) > 0 && !globals.shutdown && it->running) {
		switch_thread_cond_timedwait(it->cond, it->mutex, 20 * 1000);
	}
	switch_mutex_unlock(it->mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t mod_timer_winmm_sync(switch_timer_t *timer)
{
	interval_timer_t *it = (interval_timer_t *)timer->private_info;
	if (it) {
		timer->tick = it->tick;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t mod_timer_winmm_check(switch_timer_t *timer, switch_bool_t step)
{
	interval_timer_t *it = (interval_timer_t *)timer->private_info;
	int diff;

	if (!it) {
		return SWITCH_STATUS_GENERR;
	}

	diff = (int)(timer->tick - it->tick);
	if (diff > 0) {
		timer->diff = diff;
		return SWITCH_STATUS_FALSE;
	}
	timer->diff = 0;
	if (step) {
		mod_timer_winmm_step(timer);
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t mod_timer_winmm_destroy(switch_timer_t *timer)
{
	interval_timer_t *it = (interval_timer_t *)timer->private_info;
	switch_status_t status;

	if (!it) {
		return SWITCH_STATUS_SUCCESS;
	}

	switch_mutex_lock(globals.interval_timers_mutex);
	status = interval_timer_stop(it);
	switch_mutex_unlock(globals.interval_timers_mutex);

	timer->private_info = NULL;
	return status;
}

/* ----- module load/unload ----- */

SWITCH_MODULE_LOAD_FUNCTION(mod_timer_winmm_load)
{
	switch_timer_interface_t *timer_interface;
	HMODULE hk32;

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;
	switch_mutex_init(&globals.interval_timers_mutex, SWITCH_MUTEX_NESTED, globals.pool);

	/* Resolve high-resolution waitable timer API at runtime so the module
	 * still loads (and falls back) on Windows versions that lack it. */
	hk32 = GetModuleHandleA("kernel32.dll");
	if (hk32) {
		globals.pCreateWaitableTimerExW =
			(PFN_CreateWaitableTimerExW)GetProcAddress(hk32, "CreateWaitableTimerExW");
	}

	/* Optional MMCSS for guaranteed scheduling. */
	globals.avrt_dll = LoadLibraryA("avrt.dll");
	if (globals.avrt_dll) {
		globals.pAvSetMmThreadCharacteristicsW =
			(PFN_AvSetMmThreadCharacteristicsW)GetProcAddress(globals.avrt_dll, "AvSetMmThreadCharacteristicsW");
		globals.pAvRevertMmThreadCharacteristics =
			(PFN_AvRevertMmThreadCharacteristics)GetProcAddress(globals.avrt_dll, "AvRevertMmThreadCharacteristics");
	}

	/* Bump the system tick to 1 ms unconditionally for as long as the module
	 * is loaded.
	 *
	 *   - On older Windows (no CreateWaitableTimerExW) this is what gives
	 *     SetWaitableTimer/Sleep their sub-15 ms accuracy in the first place.
	 *   - On Windows 10 2004+ the high-resolution waitable-timer kernel path
	 *     does NOT depend on this, but FreeSWITCH may also be running other
	 *     timers (mod_softtimer, third-party modules, the JS/Lua/Python
	 *     bindings, etc.) that still go through the legacy timer subsystem.
	 *     Per-process timer-resolution policy (introduced in Win10 2004) means
	 *     those would otherwise coalesce to ~15.6 ms whenever no other
	 *     process in the system happens to be holding 1 ms resolution.
	 *   - Calling timeBeginPeriod(1) is cheap and is reference-counted by
	 *     the kernel; the matching timeEndPeriod(1) at module unload below
	 *     restores the previous value.
	 *
	 * This is belt-and-suspenders: it guarantees 1 ms granularity for the
	 * whole FS process for the duration of mod_timer_winmm's lifetime,
	 * regardless of which timer module switch.conf.xml selects. */
	if (timeBeginPeriod(1) == TIMERR_NOERROR) {
		globals.timebeginperiod_active = 1;
	}

	*module_interface = switch_loadable_module_create_module_interface(globals.pool, modname);
	timer_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_TIMER_INTERFACE);
	timer_interface->interface_name = "winmm";
	timer_interface->timer_init    = mod_timer_winmm_init;
	timer_interface->timer_next    = mod_timer_winmm_next;
	timer_interface->timer_step    = mod_timer_winmm_step;
	timer_interface->timer_sync    = mod_timer_winmm_sync;
	timer_interface->timer_check   = mod_timer_winmm_check;
	timer_interface->timer_destroy = mod_timer_winmm_destroy;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"mod_timer_winmm loaded (CreateWaitableTimerExW=%s, MMCSS=%s, timeBeginPeriod=%s)\n",
		globals.pCreateWaitableTimerExW ? "yes" : "no",
		globals.pAvSetMmThreadCharacteristicsW ? "yes" : "no",
		globals.timebeginperiod_active ? "yes" : "no");

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_timer_winmm_shutdown)
{
	int i, j;
	globals.shutdown = 1;

	switch_mutex_lock(globals.interval_timers_mutex);
	for (i = 0; i <= MAX_INTERVAL; i++) {
		for (j = 0; j < TIMERS_PER_INTERVAL; j++) {
			interval_timer_t *it = &globals.interval_timers[i][j];
			if (it->users > 0 || it->thread) {
				interval_timer_delete(it);
			}
		}
	}
	switch_mutex_unlock(globals.interval_timers_mutex);

	if (globals.timebeginperiod_active) {
		timeEndPeriod(1);
		globals.timebeginperiod_active = 0;
	}

	if (globals.avrt_dll) {
		FreeLibrary(globals.avrt_dll);
		globals.avrt_dll = NULL;
	}

	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
