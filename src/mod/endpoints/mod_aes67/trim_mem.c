#include "aes67_api.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h> // For GetProcessHeaps if needed

/*
Optional memory cleansing - call be called programmatically from aes67 CLI
or alternately from CLI script in Freeswitch 
	"fsctl reclaim_mem"

For heap-specific cleanup, enumerate private heaps with GetProcessHeaps and call HeapCompact(hHeap, 0) on each
during idle periods; it coalesces free blocks but rarely shrinks the committed virtual address space and is mainly for
convenience as Windows auto-compacts on HeapFree

Call TrimWorkingSetIdle() periodically (e.g., every 30-60 seconds of idle) or on telephony idle detection; avoid during
active RTP/audio processing to prevent latency spikes from page faults. ​

Performance Considerations
Working set trimming works best for telephony DLLs with bursty allocations, as it reduces RSS (resident set size) by up
to 2/3 during idle without affecting virtual commit size.

Monitor with GetProcessMemoryInfo before/after to tune
frequency; excessive calls hurt perf. 
HeapCompact adds minimal overhead but offers little footprint reduction unless fragmented. ​
*/

volatile BOOL memcheck_active = TRUE;			//default is on


void CompactHeaps(void)
{
	// NOTE: GetProcessHeaps() is called twice - once to get the current heap
	// count, then again to actually fill a buffer sized for that count. If any
	// thread creates a new private heap (GStreamer/GLib and the CRT do this
	// dynamically, e.g. while pipelines are being built/torn down) in the window
	// between those two calls, the second call's return value ("filled") comes
	// back LARGER than the buffer we allocated, and per the Win32 docs the buffer
	// is not guaranteed to have been filled with valid handles in that case.
	// Blindly looping "filled" times over a buffer sized for the old, smaller
	// count reads past the end of the allocation and hands HeapCompact() garbage
	// HANDLE values - undefined behavior, and a very plausible source of the
	// intermittent crash during memory cleanup, especially under Dante
	// reconnect churn where pipelines (and their heaps) are being created/torn
	// down around the same time the cleanup timer fires.
	//
	// Fix: loop until a fill actually fits the buffer we sized for it, growing
	// the buffer and retrying if the heap count grew out from under us.
	DWORD capacity = 0;
	HANDLE *heaps = NULL;
	DWORD filled = 0;

	do {
		DWORD needed = GetProcessHeaps(0, NULL);
		if (needed == 0) {
			if (heaps) HeapFree(GetProcessHeap(), 0, heaps);
			return;
		}

		if (needed > capacity) {
			HANDLE *resized = heaps
				? (HANDLE *)HeapReAlloc(GetProcessHeap(), 0, heaps, needed * sizeof(HANDLE))
				: (HANDLE *)HeapAlloc(GetProcessHeap(), 0, needed * sizeof(HANDLE));
			if (!resized) {
				if (heaps) HeapFree(GetProcessHeap(), 0, heaps);
				return;
			}
			heaps = resized;
			capacity = needed;
		}

		filled = GetProcessHeaps(capacity, heaps);
		// If filled > capacity, another heap was created in the race window
		// between the two calls above - retry with a bigger buffer instead of
		// trusting a stale count.
	} while (filled > capacity);

	for (DWORD i = 0; i < filled; ++i) { HeapCompact(heaps[i], 0); }

	HeapFree(GetProcessHeap(), 0, heaps);
}




/*
Windows treats both parameters as special when set to(SIZE_T)− 1(SIZE_T)−1
	: it attempts to remove as many pages as possible from the process working set,
	  effectively “emptying” it without changing virtual allocations
		  or destroying heap contents
				 .This is equivalent to calling EmptyWorkingSet on the process and is safe to trigger during genuine
			 idle periods to reduce resident memory pressure.
*/

void TrimCurrentProcessWorkingSet(void)
{
	HANDLE hProcess = GetCurrentProcess();

	// Optional: ensure the call succeeds; you might log or collect stats.
	if (!SetProcessWorkingSetSize(hProcess, (SIZE_T)-1, (SIZE_T)-1)) {
		// handle error if desired: GetLastError();
	}
}
#else
void CompactHeaps(void) { ; }
void TrimCurrentProcessWorkingSet(void) { ; }
#endif


volatile gint interval_min = INTERVAL_MIN > 0 ? INTERVAL_MIN : 1; // guard at declaration

void heartbeat_callback(switch_event_t *event)
{
	static volatile gint call_count = 0;
	g_atomic_int_add(&call_count, 1);

	gint threshold = (gint)(((3600L / 20L) * g_atomic_int_get(&interval_min)) / 60L);

	if (g_atomic_int_get(&call_count) >= threshold) {
		if (periodic_mem_check(TRUE)) {
			g_atomic_int_set(&call_count, 0);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "AES67: Cleaning memory ---\n");
		}
		// else: periodic_mem_check skipped this cycle (e.g. streams are mid
		// create/teardown right now) - leave call_count where it is so we
		// retry on the very next heartbeat tick instead of waiting a full
		// interval_min again.
	}
}
