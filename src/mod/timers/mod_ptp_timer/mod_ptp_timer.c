/*
 * mod_ptp_timer.c
 *
 * FreeSWITCH timer module that delivers ticks paced against a
 * PTP-disciplined wall clock.
 *
 * Design summary (per user decisions):
 *   - Single timer name: "ptp"
 *   - Linux: ptp4l management socket directly (ptp_source_linux.c)
 *   - Windows: native ptpprov.dll first, then PTPSync fallback,
 *     otherwise refuse to load (ptp_source_windows.c)
 *   - Status thread polls once per PTP_STATUS_POLL_MS, fires events
 *     on transitions, and updates a shared snapshot
 *   - "ptp status" API exposes the snapshot as JSON or plain text
 *   - Transition events: ptp::sync_acquired, ptp::sync_lost,
 *     ptp::grandmaster_changed, plus a periodic ptp::status
 *
 * The hot tick path uses switch_micro_time_now() (which on every
 * supported platform is satisfied by a high-resolution monotonic
 * clock; on Linux it tracks CLOCK_MONOTONIC, on Windows it uses
 * QueryPerformanceCounter).  When the OS clock is PTP-disciplined,
 * deltas computed from the wall clock are themselves PTP-disciplined.
 */

#include <switch.h>
#include "mod_ptp_timer.h"
#include "ptp_source.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_ptp_timer_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ptp_timer_shutdown);
SWITCH_MODULE_DEFINITION(mod_ptp_timer, mod_ptp_timer_load, mod_ptp_timer_shutdown, NULL);

mod_ptp_globals_t mod_ptp_globals;

static ptp_source_t *g_source = NULL;

/* =====================================================================
 *  Timer interface
 * =================================================================== */

typedef struct {
	switch_time_t    reference_us;   /* anchor wall-clock micros */
	switch_size_t    tick;           /* number of ticks delivered */
	switch_time_t    interval_us;    /* tick period in micros    */
} ptp_private_t;

static switch_status_t ptp_timer_init(switch_timer_t *timer)
{
	ptp_private_t *p;

	if (timer->interval < 1 || timer->interval > PTP_MAX_INTERVAL_MS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "ptp: invalid interval %d ms\n", timer->interval);
		return SWITCH_STATUS_GENERR;
	}

	p = switch_core_alloc(timer->memory_pool, sizeof(*p));
	p->interval_us  = (switch_time_t)timer->interval * 1000;
	p->reference_us = switch_micro_time_now();
	p->tick         = 0;

	timer->private_info = p;
	timer->start = p->reference_us;
	timer->tick  = 0;

	PTP_LOG_DEBUG("ptp: timer init interval=%dms samples=%d\n",
				  timer->interval, timer->samples);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t ptp_timer_step(switch_timer_t *timer)
{
	ptp_private_t *p = (ptp_private_t *)timer->private_info;
	p->tick++;
	timer->tick = p->tick;
	timer->samplecount = (uint32_t)(p->tick * timer->samples);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t ptp_timer_next(switch_timer_t *timer)
{
	ptp_private_t *p = (ptp_private_t *)timer->private_info;
	switch_time_t now, target, delta;

	p->tick++;
	target = p->reference_us + (p->tick * p->interval_us);
	now    = switch_micro_time_now();

	if (target > now) {
		delta = target - now;
		/* Cap any single sleep to one full second so that a wall-clock
		 * step (PTP just acquired sync, etc.) cannot stall the thread. */
		if (delta > 1000000) delta = 1000000;
		switch_yield(delta);
	} else if (now - target > p->interval_us * 4) {
		/* We have fallen badly behind — re-anchor so we don't burst. */
		PTP_LOG_DEBUG("ptp: tick re-anchor (drift %lldus)\n",
					  (long long)(now - target));
		p->reference_us = switch_micro_time_now() - p->interval_us;
		p->tick = 1;
	}

	timer->tick = p->tick;
	timer->samplecount = (uint32_t)(p->tick * timer->samples);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t ptp_timer_sync(switch_timer_t *timer)
{
	ptp_private_t *p = (ptp_private_t *)timer->private_info;
	switch_time_t now = switch_micro_time_now();
	p->reference_us = now;
	p->tick = 0;
	timer->tick = 0;
	timer->samplecount = 0;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t ptp_timer_check(switch_timer_t *timer, switch_bool_t step)
{
	ptp_private_t *p = (ptp_private_t *)timer->private_info;
	switch_time_t now = switch_micro_time_now();
	switch_time_t target = p->reference_us + ((p->tick + 1) * p->interval_us);

	if (now >= target) {
		if (step == SWITCH_TRUE) {
			p->tick++;
			timer->tick = p->tick;
			timer->samplecount = (uint32_t)(p->tick * timer->samples);
		}
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_FALSE;
}

static switch_status_t ptp_timer_destroy(switch_timer_t *timer)
{
	timer->private_info = NULL;
	return SWITCH_STATUS_SUCCESS;
}

/* =====================================================================
 *  Status thread + events
 * =================================================================== */

static void fire_event(const char *subclass, const ptp_status_t *st)
{
	switch_event_t *event = NULL;
	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, subclass) != SWITCH_STATUS_SUCCESS) {
		return;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "PTP-State",          ptp_sync_state_str(st->state));
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "PTP-Grandmaster-ID", st->grandmaster_id);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "PTP-Port-State",     st->port_state);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "PTP-Source",         st->source_desc);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "PTP-Master-Offset-NS",      "%lld", (long long)st->master_offset_ns);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "PTP-Path-Delay-NS",         "%lld", (long long)st->path_delay_ns);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "PTP-Servo-Steps",           "%u",   (unsigned)st->servo_steps);
	switch_event_fire(&event);
}

static void *SWITCH_THREAD_FUNC status_thread_run(switch_thread_t *thread, void *obj)
{
	ptp_status_t prev;
	(void)thread; (void)obj;

	memset(&prev, 0, sizeof(prev));
	prev.state = PTP_SYNC_UNKNOWN;

	while (mod_ptp_globals.running == SWITCH_TRUE) {
		ptp_status_t now;

		if (g_source && ptp_source_poll(g_source, &now) == SWITCH_STATUS_SUCCESS) {

			switch_mutex_lock(mod_ptp_globals.mutex);
			mod_ptp_globals.last_status = now;
			switch_mutex_unlock(mod_ptp_globals.mutex);

			if (prev.state != PTP_SYNC_LOCKED && now.state == PTP_SYNC_LOCKED) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
								  "ptp: sync acquired (GM=%s port=%s)\n",
								  now.grandmaster_id, now.port_state);
				fire_event(PTP_EVENT_SYNC_ACQUIRED, &now);
			} else if (prev.state == PTP_SYNC_LOCKED && now.state != PTP_SYNC_LOCKED) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "ptp: sync lost (now %s)\n",
								  ptp_sync_state_str(now.state));
				fire_event(PTP_EVENT_SYNC_LOST, &now);
			}

			if (now.state == PTP_SYNC_LOCKED &&
				prev.grandmaster_id[0] &&
				strcmp(prev.grandmaster_id, now.grandmaster_id) != 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
								  "ptp: grandmaster changed %s -> %s\n",
								  prev.grandmaster_id, now.grandmaster_id);
				fire_event(PTP_EVENT_GRANDMASTER_CHANGED, &now);
			}

			fire_event(PTP_EVENT_STATUS, &now);
			prev = now;
		}

		switch_yield(PTP_STATUS_POLL_MS * 1000);
	}
	return NULL;
}

/* =====================================================================
 *  Configuration
 * =================================================================== */

static switch_status_t load_config(void)
{
	switch_xml_t cfg, xml, settings, param;
	const char *cf = "ptp_timer.conf";

	switch_set_string(mod_ptp_globals.ptp4l_socket, "/var/run/ptp4l");
	switch_set_string(mod_ptp_globals.ptpsync_dll,  "PTPSyncNative.dll");
	mod_ptp_globals.debug = SWITCH_FALSE;

	if ((xml = switch_xml_open_cfg(cf, &cfg, NULL)) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						  "ptp: %s not found, using defaults\n", cf);
		return SWITCH_STATUS_SUCCESS;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *var = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "ptp4l-socket") && !zstr(val)) {
				switch_set_string(mod_ptp_globals.ptp4l_socket, val);
			} else if (!strcasecmp(var, "ptpsync-dll") && !zstr(val)) {
				switch_set_string(mod_ptp_globals.ptpsync_dll, val);
			} else if (!strcasecmp(var, "debug")) {
				mod_ptp_globals.debug = switch_true(val) ? SWITCH_TRUE : SWITCH_FALSE;
			}
		}
	}

	switch_xml_free(xml);
	return SWITCH_STATUS_SUCCESS;
}

/* =====================================================================
 *  API: "ptp status [json]" / "ptp debug on|off"
 * =================================================================== */

#define PTP_API_SYNTAX "status [json] | debug on|off"

SWITCH_STANDARD_API(ptp_api_function)
{
	char *argv[4] = { 0 };
	char *mycmd = NULL;
	int   argc  = 0;

	if (!zstr(cmd)) {
		mycmd = strdup(cmd);
		argc  = switch_separate_string(mycmd, ' ', argv, switch_arraylen(argv));
	}

	if (argc < 1) {
		stream->write_function(stream, "-USAGE: %s\n", PTP_API_SYNTAX);
		goto done;
	}

	if (!strcasecmp(argv[0], "debug")) {
		if (argc >= 2 && (!strcasecmp(argv[1], "on") || !strcasecmp(argv[1], "off"))) {
			mod_ptp_globals.debug = !strcasecmp(argv[1], "on") ? SWITCH_TRUE : SWITCH_FALSE;
		}
		stream->write_function(stream, "+OK ptp debug %s\n",
							   mod_ptp_globals.debug == SWITCH_TRUE ? "on" : "off");
		goto done;
	}

	if (!strcasecmp(argv[0], "status")) {
		ptp_status_t s;
		switch_bool_t want_json = (argc >= 2 && !strcasecmp(argv[1], "json"));

		switch_mutex_lock(mod_ptp_globals.mutex);
		s = mod_ptp_globals.last_status;
		switch_mutex_unlock(mod_ptp_globals.mutex);

		if (want_json) {
			stream->write_function(stream,
				"{"
				"\"state\":\"%s\","
				"\"grandmaster_id\":\"%s\","
				"\"port_state\":\"%s\","
				"\"master_offset_ns\":%lld,"
				"\"path_delay_ns\":%lld,"
				"\"servo_steps\":%u,"
				"\"source\":\"%s\","
				"\"sample_us\":%lld"
				"}\n",
				ptp_sync_state_str(s.state),
				s.grandmaster_id, s.port_state,
				(long long)s.master_offset_ns, (long long)s.path_delay_ns,
				(unsigned)s.servo_steps, s.source_desc,
				(long long)s.sample_us);
		} else {
			stream->write_function(stream,
				"PTP state       : %s\n"
				"Grandmaster     : %s\n"
				"Port state      : %s\n"
				"Master offset   : %lld ns\n"
				"Mean path delay : %lld ns\n"
				"Servo steps     : %u\n"
				"Source          : %s\n",
				ptp_sync_state_str(s.state),
				s.grandmaster_id[0] ? s.grandmaster_id : "(none)",
				s.port_state[0]     ? s.port_state     : "(unknown)",
				(long long)s.master_offset_ns,
				(long long)s.path_delay_ns,
				(unsigned)s.servo_steps,
				s.source_desc[0]    ? s.source_desc    : "(none)");
		}
		goto done;
	}

	stream->write_function(stream, "-USAGE: %s\n", PTP_API_SYNTAX);

done:
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

/* =====================================================================
 *  Module load / shutdown
 * =================================================================== */

SWITCH_MODULE_LOAD_FUNCTION(mod_ptp_timer_load)
{
	switch_timer_interface_t *timer_interface;
	switch_api_interface_t   *api_interface;
	switch_threadattr_t      *thd_attr = NULL;

	memset(&mod_ptp_globals, 0, sizeof(mod_ptp_globals));
	mod_ptp_globals.pool = pool;
	switch_mutex_init(&mod_ptp_globals.mutex, SWITCH_MUTEX_NESTED, pool);

	load_config();

	if (ptp_source_create(&g_source, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
						  "ptp: failed to initialise PTP source, refusing to load\n");
		return SWITCH_STATUS_GENERR;
	}

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	timer_interface = (switch_timer_interface_t *)
		switch_loadable_module_create_interface(*module_interface, SWITCH_TIMER_INTERFACE);
	timer_interface->interface_name = PTP_TIMER_NAME;
	timer_interface->timer_init     = ptp_timer_init;
	timer_interface->timer_next     = ptp_timer_next;
	timer_interface->timer_step     = ptp_timer_step;
	timer_interface->timer_sync     = ptp_timer_sync;
	timer_interface->timer_check    = ptp_timer_check;
	timer_interface->timer_destroy  = ptp_timer_destroy;

	SWITCH_ADD_API(api_interface, "ptp", "PTP timer status / debug",
				   ptp_api_function, PTP_API_SYNTAX);

	switch_event_reserve_subclass(PTP_EVENT_STATUS);
	switch_event_reserve_subclass(PTP_EVENT_SYNC_ACQUIRED);
	switch_event_reserve_subclass(PTP_EVENT_SYNC_LOST);
	switch_event_reserve_subclass(PTP_EVENT_GRANDMASTER_CHANGED);

	mod_ptp_globals.running = SWITCH_TRUE;
	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_detach_set(thd_attr, 0);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&mod_ptp_globals.status_thread, thd_attr,
						 status_thread_run, NULL, pool);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "mod_ptp_timer loaded (timer name '%s')\n", PTP_TIMER_NAME);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ptp_timer_shutdown)
{
	switch_status_t st;

	mod_ptp_globals.running = SWITCH_FALSE;
	if (mod_ptp_globals.status_thread) {
		switch_thread_join(&st, mod_ptp_globals.status_thread);
		mod_ptp_globals.status_thread = NULL;
	}

	switch_event_free_subclass(PTP_EVENT_STATUS);
	switch_event_free_subclass(PTP_EVENT_SYNC_ACQUIRED);
	switch_event_free_subclass(PTP_EVENT_SYNC_LOST);
	switch_event_free_subclass(PTP_EVENT_GRANDMASTER_CHANGED);

	if (g_source) {
		ptp_source_destroy(&g_source);
	}

	return SWITCH_STATUS_SUCCESS;
}
