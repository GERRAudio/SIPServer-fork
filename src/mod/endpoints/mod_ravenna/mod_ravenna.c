/*
 * mod_ravenna.c
 *
 * Lean AES67/RAVENNA endpoint module: shared RX reactor + PTP-paced
 * shared TX pacer + per-channel SPMC fan-out rings, all driven by
 * mod_ptp_timer.
 *
 * Config schema mirrors mod_aes67 so it is a drop-in replacement.
 *
 *   <configuration name="ravenna.conf" ...>
 *     <settings>...</settings>
 *     <streams>
 *       <stream name="...">
 *         <param name="rx-address" .../>
 *         <param name="tx-address" .../>
 *         <param name="channels" .../>
 *         <param name="rx-codec" value="L16|L24|L32"/>
 *         <param name="tx-codec" value="L16|L24|L32"/>
 *         <param name="ptime-ms" value="1.0"/>
 *         ...
 *       </stream>
 *     </streams>
 *     <endpoints>
 *       <endpoint name="...">
 *         <param name="instream"  value="udp1:0"/>
 *         <param name="outstream" value="udp1:0"/>
 *       </endpoint>
 *     </endpoints>
 *   </configuration>
 */

#include "mod_ravenna.h"
#include "ravenna_net.h"
#include "ravenna_rtp.h"
#include "ravenna_ring.h"

#include <string.h>

mod_ravenna_globals_t mod_ravenna_globals;

SWITCH_MODULE_LOAD_FUNCTION(mod_ravenna_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ravenna_shutdown);
SWITCH_MODULE_DEFINITION(mod_ravenna, mod_ravenna_load, mod_ravenna_shutdown, NULL);

/* ==================================================================
 *  Helpers
 * ================================================================== */

static uint32_t hash_count(switch_hash_t *h)
{
	switch_hash_index_t *hi;
	uint32_t n = 0;
	for (hi = switch_core_hash_first(h); hi; hi = switch_core_hash_next(&hi)) n++;
	return n;
}

static ravenna_codec_t parse_codec(const char *s, ravenna_codec_t def)
{
	if (zstr(s)) return def;
	if (!strcasecmp(s, "L16")) return RAV_CODEC_L16;
	if (!strcasecmp(s, "L24")) return RAV_CODEC_L24;
	if (!strcasecmp(s, "L32")) return RAV_CODEC_L32;
	return def;
}

/* "udp1:3" => stream "udp1", channel 3 */
static switch_bool_t parse_stream_ref(const char *spec,
									  char *name_out, size_t name_cap,
									  int *chan_out)
{
	const char *colon;
	size_t nlen;

	if (zstr(spec)) return SWITCH_FALSE;
	colon = strchr(spec, ':');
	if (!colon) return SWITCH_FALSE;

	nlen = (size_t)(colon - spec);
	if (nlen == 0 || nlen >= name_cap) return SWITCH_FALSE;

	memcpy(name_out, spec, nlen);
	name_out[nlen] = '\0';
	*chan_out = atoi(colon + 1);
	return SWITCH_TRUE;
}

/* ==================================================================
 *  Stream / endpoint helpers
 * ================================================================== */

switch_status_t ravenna_stream_open_sockets(ravenna_stream_t *s)
{
	switch_status_t st = SWITCH_STATUS_SUCCESS;

	if (s->rx_enabled && s->rx_sock == RAVENNA_INVALID_SOCKET) {
		st = ravenna_net_open_rx(&s->rx_sock, s->rx_addr, s->rx_port, s->iface);
		if (st != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "ravenna: stream '%s' rx open failed\n", s->name);
			return st;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "ravenna: stream '%s' rx %s:%d (codec=%s ch=%d ptime=%.3fms)\n",
						  s->name, s->rx_addr, s->rx_port,
						  ravenna_codec_str(s->rx_codec),
						  s->channels, s->ptime_ms);
	}

	/* ST 2022-7 RX path 2 */
	if (s->st2022_7 && s->rx_enabled && s->rx2_addr[0] && s->rx2_port &&
		s->rx2_sock == RAVENNA_INVALID_SOCKET) {
		const char *iface2 = s->iface2[0] ? s->iface2 : s->iface;
		st = ravenna_net_open_rx(&s->rx2_sock, s->rx2_addr, s->rx2_port, iface2);
		if (st != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "ravenna: stream '%s' rx2 open failed (single-path fallback)\n",
							  s->name);
			/* Non-fatal: degrade to single path */
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
							  "ravenna: stream '%s' rx2 (ST2022-7) %s:%d iface=%s\n",
							  s->name, s->rx2_addr, s->rx2_port, iface2);
		}
	}

	if (s->tx_enabled && s->tx_sock == RAVENNA_INVALID_SOCKET) {
		st = ravenna_net_open_tx(&s->tx_sock, &s->tx_dest,
								 s->tx_addr, s->tx_port, s->iface, 16);
		if (st != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "ravenna: stream '%s' tx open failed\n", s->name);
			return st;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "ravenna: stream '%s' tx %s:%d (codec=%s ch=%d ptime=%.3fms)\n",
						  s->name, s->tx_addr, s->tx_port,
						  ravenna_codec_str(s->tx_codec),
						  s->channels, s->ptime_ms);
	}

	/* ST 2022-7 TX path 2 */
	if (s->st2022_7 && s->tx_enabled && s->tx2_addr[0] && s->tx2_port &&
		s->tx2_sock == RAVENNA_INVALID_SOCKET) {
		const char *iface2 = s->iface2[0] ? s->iface2 : s->iface;
		st = ravenna_net_open_tx(&s->tx2_sock, &s->tx2_dest,
								 s->tx2_addr, s->tx2_port, iface2, 16);
		if (st != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "ravenna: stream '%s' tx2 open failed (single-path fallback)\n",
							  s->name);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
							  "ravenna: stream '%s' tx2 (ST2022-7) %s:%d iface=%s\n",
							  s->name, s->tx2_addr, s->tx2_port, iface2);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

void ravenna_stream_close_sockets(ravenna_stream_t *s)
{
	ravenna_net_close(&s->rx_sock);
	ravenna_net_close(&s->rx2_sock);
	ravenna_net_close(&s->tx_sock);
	ravenna_net_close(&s->tx2_sock);
}

/* ==================================================================
 *  Stream comparison — used by reload diff
 * ================================================================== */

/* Returns SWITCH_TRUE if all socket-affecting parameters are identical.
 * Internal state (stats, rings, sequence numbers) is intentionally
 * excluded — those are runtime, not config. */
static switch_bool_t stream_equal(const ravenna_stream_t *a,
								  const ravenna_stream_t *b)
{
	if (a->channels    != b->channels)    return SWITCH_FALSE;
	if (a->sample_rate != b->sample_rate) return SWITCH_FALSE;
	if (a->ptime_ms    != b->ptime_ms)    return SWITCH_FALSE;
	if (a->rx_enabled  != b->rx_enabled)  return SWITCH_FALSE;
	if (a->tx_enabled  != b->tx_enabled)  return SWITCH_FALSE;
	if (a->rx_codec    != b->rx_codec)    return SWITCH_FALSE;
	if (a->tx_codec    != b->tx_codec)    return SWITCH_FALSE;
	if (a->rtp_payload_type != b->rtp_payload_type) return SWITCH_FALSE;
	if (a->st2022_7    != b->st2022_7)    return SWITCH_FALSE;
	if (strcmp(a->rx_addr,  b->rx_addr))  return SWITCH_FALSE;
	if (strcmp(a->tx_addr,  b->tx_addr))  return SWITCH_FALSE;
	if (strcmp(a->rx2_addr, b->rx2_addr)) return SWITCH_FALSE;
	if (strcmp(a->tx2_addr, b->tx2_addr)) return SWITCH_FALSE;
	if (a->rx_port  != b->rx_port)        return SWITCH_FALSE;
	if (a->tx_port  != b->tx_port)        return SWITCH_FALSE;
	if (a->rx2_port != b->rx2_port)       return SWITCH_FALSE;
	if (a->tx2_port != b->tx2_port)       return SWITCH_FALSE;
	if (strcmp(a->iface,  b->iface))      return SWITCH_FALSE;
	if (strcmp(a->iface2, b->iface2))     return SWITCH_FALSE;
	return SWITCH_TRUE;
}

/* ==================================================================
 *  Configuration loader
 * ================================================================== */

static switch_status_t load_settings(switch_xml_t settings)
{
	switch_xml_t param;

	for (param = switch_xml_child(settings, "param"); param; param = param->next) {

		const char *var = switch_xml_attr_soft(param, "name");
		const char *val = switch_xml_attr_soft(param, "value");
		if (!strcasecmp(var, "sample-rate"))      mod_ravenna_globals.default_sample_rate  = atoi(val);
		else if (!strcasecmp(var, "ptime-ms"))    mod_ravenna_globals.default_ptime_ms     = atof(val);
		else if (!strcasecmp(var, "channels"))    mod_ravenna_globals.default_channels     = atoi(val);
		else if (!strcasecmp(var, "rx-codec"))    mod_ravenna_globals.default_rx_codec     = parse_codec(val, RAV_CODEC_L16);
		else if (!strcasecmp(var, "tx-codec"))    mod_ravenna_globals.default_tx_codec     = parse_codec(val, RAV_CODEC_L16);
		else if (!strcasecmp(var, "rtp-payload-type")) mod_ravenna_globals.default_payload_type = atoi(val);
		else if (!strcasecmp(var, "iface-primary")) switch_set_string(mod_ravenna_globals.default_iface, val);
		else if (!strcasecmp(var, "dialplan"))    switch_set_string(mod_ravenna_globals.dialplan,    val);
		else if (!strcasecmp(var, "cid-name"))    switch_set_string(mod_ravenna_globals.cid_name,    val);
		else if (!strcasecmp(var, "cid-num"))     switch_set_string(mod_ravenna_globals.cid_num,     val);
		else if (!strcasecmp(var, "hold-file"))   switch_set_string(mod_ravenna_globals.hold_file,   val);
		else if (!strcasecmp(var, "debug"))       mod_ravenna_globals.debug = switch_true(val) ? SWITCH_TRUE : SWITCH_FALSE;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t load_streams(switch_xml_t streams_xml)
{
	switch_xml_t stream_xml;

	for (stream_xml = switch_xml_child(streams_xml, "stream"); stream_xml; stream_xml = stream_xml->next) {
		const char *name = switch_xml_attr_soft(stream_xml, "name");
		ravenna_stream_t *s;
		switch_xml_t param;
		int ch;

		if (zstr(name)) continue;
		if (switch_core_hash_find(mod_ravenna_globals.streams, name)) continue;

		s = switch_core_alloc(mod_ravenna_globals.pool, sizeof(*s));
		memset(s, 0, sizeof(*s));
		switch_set_string(s->name, name);
		s->rx_sock          = RAVENNA_INVALID_SOCKET;
		s->rx2_sock         = RAVENNA_INVALID_SOCKET;
		s->tx_sock          = RAVENNA_INVALID_SOCKET;
		s->tx2_sock         = RAVENNA_INVALID_SOCKET;

		/* Pre-fill dedup window so seq 0 is not a false positive */
		memset(s->dedup_win, 0xFF, sizeof(s->dedup_win));

		s->channels         = mod_ravenna_globals.default_channels;
		s->sample_rate      = mod_ravenna_globals.default_sample_rate;
		s->ptime_ms         = mod_ravenna_globals.default_ptime_ms;
		s->rx_codec         = mod_ravenna_globals.default_rx_codec;
		s->tx_codec         = mod_ravenna_globals.default_tx_codec;
		s->rtp_payload_type = mod_ravenna_globals.default_payload_type;
		switch_set_string(s->iface, mod_ravenna_globals.default_iface);
		s->tx_ssrc          = (uint32_t)switch_micro_time_now();

		for (param = switch_xml_child(stream_xml, "param"); param; param = param->next) {
			const char *var = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "rx-address"))         { switch_set_string(s->rx_addr, val); s->rx_enabled = SWITCH_TRUE; }
			else if (!strcasecmp(var, "rx-port"))       { s->rx_port = atoi(val); s->rx_enabled = SWITCH_TRUE; }
			else if (!strcasecmp(var, "rx2-address"))   { switch_set_string(s->rx2_addr, val); }
			else if (!strcasecmp(var, "rx2-port"))      { s->rx2_port = atoi(val); }
			else if (!strcasecmp(var, "tx-address"))    { switch_set_string(s->tx_addr, val); s->tx_enabled = SWITCH_TRUE; }
			else if (!strcasecmp(var, "tx-port"))       { s->tx_port = atoi(val); s->tx_enabled = SWITCH_TRUE; }
			else if (!strcasecmp(var, "tx2-address"))   { switch_set_string(s->tx2_addr, val); }
			else if (!strcasecmp(var, "tx2-port"))      { s->tx2_port = atoi(val); }
			else if (!strcasecmp(var, "st2022-7"))      { s->st2022_7 = switch_true(val) ? SWITCH_TRUE : SWITCH_FALSE; }
			else if (!strcasecmp(var, "channels"))      s->channels    = atoi(val);
			else if (!strcasecmp(var, "sample-rate"))   s->sample_rate = atoi(val);
			else if (!strcasecmp(var, "ptime-ms"))      s->ptime_ms    = atof(val);
			else if (!strcasecmp(var, "rx-codec"))      s->rx_codec    = parse_codec(val, s->rx_codec);
			else if (!strcasecmp(var, "tx-codec"))      s->tx_codec    = parse_codec(val, s->tx_codec);
			else if (!strcasecmp(var, "rtp-payload-type")) s->rtp_payload_type = atoi(val);
			else if (!strcasecmp(var, "iface-primary"))    switch_set_string(s->iface, val);
			else if (!strcasecmp(var, "iface-secondary")) switch_set_string(s->iface2, val);
		}

		if (s->channels < 1 || s->channels > RAVENNA_MAX_CHANNELS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "ravenna: stream '%s' bad channels %d\n", name, s->channels);
			continue;
		}
		if (s->ptime_ms <= 0) s->ptime_ms = 1.0;

		s->samples_per_packet = (int)((s->ptime_ms * s->sample_rate) / 1000.0 + 0.5);
		if (s->samples_per_packet < 1) s->samples_per_packet = 1;

		switch_mutex_init(&s->mutex, SWITCH_MUTEX_NESTED, mod_ravenna_globals.pool);

		for (ch = 0; ch < s->channels; ch++) {
			uint32_t cap = (uint32_t)s->samples_per_packet * RAVENNA_RING_PACKETS_DEFAULT;
			if (s->rx_enabled) ravenna_ring_create(&s->rx_rings[ch], mod_ravenna_globals.pool, cap);
			if (s->tx_enabled) ravenna_ring_create(&s->tx_rings[ch], mod_ravenna_globals.pool, cap);
		}

		switch_core_hash_insert(mod_ravenna_globals.streams, s->name, s);
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t load_endpoints(switch_xml_t endpoints_xml)
{
	switch_xml_t ep_xml;

	for (ep_xml = switch_xml_child(endpoints_xml, "endpoint"); ep_xml; ep_xml = ep_xml->next) {
		const char *name = switch_xml_attr_soft(ep_xml, "name");
		ravenna_endpoint_t *e;
		switch_xml_t param;

		if (zstr(name)) continue;
		if (switch_core_hash_find(mod_ravenna_globals.endpoints, name)) continue;

		e = switch_core_alloc(mod_ravenna_globals.pool, sizeof(*e));
		memset(e, 0, sizeof(*e));
		switch_set_string(e->name, name);
		switch_mutex_init(&e->mutex, SWITCH_MUTEX_NESTED, mod_ravenna_globals.pool);

		for (param = switch_xml_child(ep_xml, "param"); param; param = param->next) {
			const char *var = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");
			char sname[RAVENNA_MAX_NAME_LEN]; int ch = 0;

			if (!strcasecmp(var, "instream") &&
				parse_stream_ref(val, sname, sizeof(sname), &ch)) {
				e->in_stream = switch_core_hash_find(mod_ravenna_globals.streams, sname);
				e->inchan    = ch;
			} else if (!strcasecmp(var, "outstream") &&
					   parse_stream_ref(val, sname, sizeof(sname), &ch)) {
				e->out_stream = switch_core_hash_find(mod_ravenna_globals.streams, sname);
				e->outchan    = ch;
			} else if (!strcasecmp(var, "multiple-listen")) {
				e->multiple_listen = switch_true(val) ? SWITCH_TRUE : SWITCH_FALSE;
			}
		}

		if (!e->in_stream && !e->out_stream) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "ravenna: endpoint '%s' has neither instream nor outstream\n", name);
			continue;
		}

		switch_core_hash_insert(mod_ravenna_globals.endpoints, e->name, e);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "ravenna: endpoint '%s' in=%s:%d out=%s:%d\n",
						  name,
						  e->in_stream  ? e->in_stream->name  : "(none)", e->inchan,
						  e->out_stream ? e->out_stream->name : "(none)", e->outchan);
	}
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t ravenna_load_config(void)
{
	switch_xml_t cfg, xml, settings, streams, endpoints;
	const char *cf = "ravenna.conf";

	/* Defaults */
	mod_ravenna_globals.default_sample_rate  = 48000;
	mod_ravenna_globals.default_ptime_ms     = 1.0;
	mod_ravenna_globals.default_channels     = 1;
	mod_ravenna_globals.default_rx_codec     = RAV_CODEC_L16;
	mod_ravenna_globals.default_tx_codec     = RAV_CODEC_L16;
	mod_ravenna_globals.default_payload_type = 98;
	mod_ravenna_globals.default_iface[0]     = 0;
	switch_set_string(mod_ravenna_globals.dialplan, "XML");

	if ((xml = switch_xml_open_cfg(cf, &cfg, NULL)) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ravenna: %s not found, no streams loaded\n", cf);
		return SWITCH_STATUS_SUCCESS;
	}

	if ((settings  = switch_xml_child(cfg, "settings")))  load_settings(settings);
	if ((streams   = switch_xml_child(cfg, "streams")))   load_streams(streams);
	if ((endpoints = switch_xml_child(cfg, "endpoints"))) load_endpoints(endpoints);

	switch_xml_free(xml);
	return SWITCH_STATUS_SUCCESS;
}

void ravenna_unload_config(void)
{
	switch_hash_index_t *hi;

	for (hi = switch_core_hash_first(mod_ravenna_globals.streams); hi;
		 hi = switch_core_hash_next(&hi)) {
		const void *k; void *v; switch_ssize_t klen;
		ravenna_stream_t *s;
		switch_core_hash_this(hi, &k, &klen, &v);
		s = (ravenna_stream_t *)v;
		ravenna_stream_close_sockets(s);
	}
}

/* ==================================================================
 *  FreeSWITCH endpoint glue
 * ================================================================== */

static switch_status_t channel_on_init     (switch_core_session_t *session);
static switch_status_t channel_on_hangup   (switch_core_session_t *session);
static switch_status_t channel_on_destroy  (switch_core_session_t *session);
static switch_status_t channel_on_routing  (switch_core_session_t *session);
static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig);
static switch_status_t channel_read_frame  (switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id);
static switch_status_t channel_write_frame (switch_core_session_t *session, switch_frame_t *frame,  switch_io_flag_t flags, int stream_id);
static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session, switch_memory_pool_t **pool,
													switch_originate_flag_t flags, switch_call_cause_t *cancel_cause);

static switch_state_handler_table_t ravenna_state_handlers = {
	/*.on_init        = */ channel_on_init,
	/*.on_routing     = */ channel_on_routing,
	/*.on_execute     = */ NULL,
	/*.on_hangup      = */ channel_on_hangup,
	/*.on_exchange_media = */ NULL,
	/*.on_soft_execute   = */ NULL,
	/*.on_consume_media  = */ NULL,
	/*.on_hibernate      = */ NULL,
	/*.on_reset          = */ NULL,
	/*.on_park           = */ NULL,
	/*.on_reporting      = */ NULL,
	/*.on_destroy        = */ channel_on_destroy
};

static switch_io_routines_t ravenna_io_routines = {
	/*.outgoing_channel = */ channel_outgoing_channel,
	/*.read_frame       = */ channel_read_frame,
	/*.write_frame      = */ channel_write_frame,
	/*.kill_channel     = */ channel_kill_channel
};

/* ------------------------------------------------------------------
 *  Session lifecycle
 * ------------------------------------------------------------------ */

static switch_status_t setup_codecs(ravenna_session_t *rs)
{
	ravenna_stream_t *ref =
		rs->endpoint->in_stream ? rs->endpoint->in_stream : rs->endpoint->out_stream;
	int rate = ref ? ref->sample_rate : 48000;
	int ms   = ref ? (int)(ref->ptime_ms < 1.0 ? 1.0 : ref->ptime_ms) : 20;

	rs->sample_rate       = rate;
	rs->codec_ms          = ms;
	rs->samples_per_frame = (rate * ms) / 1000;

	if (switch_core_codec_init(&rs->read_codec, "L16", "Ravenna", NULL,
							   rate, ms, 1,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
							   NULL, switch_core_session_get_pool(rs->session)) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	if (switch_core_codec_init(&rs->write_codec, "L16", "Ravenna", NULL,
							   rate, ms, 1,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
							   NULL, switch_core_session_get_pool(rs->session)) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	switch_core_session_set_read_codec (rs->session, &rs->read_codec);
	switch_core_session_set_write_codec(rs->session, &rs->write_codec);

	rs->read_frame.codec = &rs->read_codec;
	rs->read_frame.data  = rs->read_buf;
	rs->read_frame.buflen= sizeof(rs->read_buf);

	if (switch_core_timer_init(&rs->read_timer, RAVENNA_PTP_TIMER_NAME,
							   ms, rs->samples_per_frame,
							   switch_core_session_get_pool(rs->session)) == SWITCH_STATUS_SUCCESS) {
		rs->read_timer_inited = SWITCH_TRUE;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_init(switch_core_session_t *session)
{
	ravenna_session_t *rs = switch_core_session_get_private(session);
	switch_channel_t *ch  = switch_core_session_get_channel(session);

	if (!rs || !rs->endpoint) return SWITCH_STATUS_FALSE;

	if (setup_codecs(rs) != SWITCH_STATUS_SUCCESS) {
		switch_channel_hangup(ch, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
		return SWITCH_STATUS_FALSE;
	}

	if (rs->endpoint->in_stream) {
		ravenna_ring_t *ring = rs->endpoint->in_stream->rx_rings[rs->endpoint->inchan];
		if (ring) rs->rx_cursor = ravenna_ring_attach(ring);
		switch_mutex_lock(rs->endpoint->mutex);
		rs->endpoint->active_rx_sessions++;
		switch_mutex_unlock(rs->endpoint->mutex);
	}

	switch_channel_set_state(ch, CS_ROUTING);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_routing(switch_core_session_t *session)
{
	(void)session;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_hangup(switch_core_session_t *session)
{
	ravenna_session_t *rs = switch_core_session_get_private(session);
	if (!rs) return SWITCH_STATUS_SUCCESS;

	if (rs->rx_cursor) ravenna_ring_detach(&rs->rx_cursor);
	if (rs->endpoint) {
		switch_mutex_lock(rs->endpoint->mutex);
		if (rs->endpoint->active_rx_sessions > 0) rs->endpoint->active_rx_sessions--;
		switch_mutex_unlock(rs->endpoint->mutex);
	}
	if (rs->read_timer_inited) {
		switch_core_timer_destroy(&rs->read_timer);
		rs->read_timer_inited = SWITCH_FALSE;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_destroy(switch_core_session_t *session)
{
	ravenna_session_t *rs = switch_core_session_get_private(session);
	if (rs) {
		if (rs->read_codec.implementation)  switch_core_codec_destroy(&rs->read_codec);
		if (rs->write_codec.implementation) switch_core_codec_destroy(&rs->write_codec);
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig)
{
	(void)session; (void)sig;
	return SWITCH_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------
 *  Read / write
 * ------------------------------------------------------------------ */

static switch_status_t channel_read_frame(switch_core_session_t *session,
										  switch_frame_t **frame,
										  switch_io_flag_t flags, int stream_id)
{
	ravenna_session_t *rs = switch_core_session_get_private(session);
	switch_channel_t  *ch = switch_core_session_get_channel(session);
	int got = 0;

	(void)flags; (void)stream_id;

	if (!rs) return SWITCH_STATUS_FALSE;

	if (rs->read_timer_inited) switch_core_timer_next(&rs->read_timer);

	if (rs->rx_cursor) {
		got = ravenna_cursor_read(rs->rx_cursor,
								  (ravenna_sample_t *)rs->read_buf,
								  (uint32_t)rs->samples_per_frame);
		if (got < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "ravenna: cursor overrun on session %s — hanging up\n",
							  switch_channel_get_name(ch));
			switch_channel_hangup(ch, SWITCH_CAUSE_MEDIA_TIMEOUT);
			return SWITCH_STATUS_FALSE;
		}
	}

	if (got < rs->samples_per_frame) {
		memset((uint8_t *)rs->read_buf + got * sizeof(ravenna_sample_t), 0,
			   (rs->samples_per_frame - got) * sizeof(ravenna_sample_t));
	}

	rs->read_frame.datalen = rs->samples_per_frame * (int)sizeof(ravenna_sample_t);
	rs->read_frame.samples = rs->samples_per_frame;
	*frame = &rs->read_frame;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_write_frame(switch_core_session_t *session,
										   switch_frame_t *frame,
										   switch_io_flag_t flags, int stream_id)
{
	ravenna_session_t *rs = switch_core_session_get_private(session);
	ravenna_endpoint_t *e;
	ravenna_stream_t   *s;
	ravenna_ring_t     *ring;
	int samples;

	(void)flags; (void)stream_id;

	if (!rs || !rs->endpoint) return SWITCH_STATUS_FALSE;
	e = rs->endpoint;
	s = e->out_stream;
	if (!s) return SWITCH_STATUS_SUCCESS;          /* RX-only endpoint */
	if (e->outchan < 0 || e->outchan >= s->channels) return SWITCH_STATUS_SUCCESS;
	ring = s->tx_rings[e->outchan];
	if (!ring) return SWITCH_STATUS_SUCCESS;

	samples = frame->datalen / (int)sizeof(ravenna_sample_t);
	if (samples > 0) {
		ravenna_ring_write(ring, (const ravenna_sample_t *)frame->data, (uint32_t)samples);
	}
	return SWITCH_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------
 *  Outgoing call
 * ------------------------------------------------------------------ */

static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session,
													switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session,
													switch_memory_pool_t **pool,
													switch_originate_flag_t flags,
													switch_call_cause_t *cancel_cause)
{
	switch_core_session_t *nsession;
	switch_channel_t      *nch;
	switch_caller_profile_t *cp;
	ravenna_session_t     *rs;
	ravenna_endpoint_t    *e;
	const char            *dest;
	const char            *epname;
	const char            *prefix = "endpoint/";

	(void)session; (void)var_event; (void)flags; (void)cancel_cause;

	if (!outbound_profile || zstr(outbound_profile->destination_number)) {
		return SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
	}

	dest = outbound_profile->destination_number;
	if (strncasecmp(dest, prefix, strlen(prefix)) != 0) {
		return SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
	}
	epname = dest + strlen(prefix);

	e = switch_core_hash_find(mod_ravenna_globals.endpoints, epname);
	if (!e) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ravenna: outgoing for unknown endpoint '%s'\n", epname);
		return SWITCH_CAUSE_NO_ROUTE_DESTINATION;
	}

	if (e->in_stream && !e->multiple_listen && e->active_rx_sessions > 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ravenna: endpoint '%s' busy\n", epname);
		return SWITCH_CAUSE_USER_BUSY;
	}

	nsession = switch_core_session_request(mod_ravenna_globals.endpoint_interface,
										   SWITCH_CALL_DIRECTION_OUTBOUND,
										   flags, pool);
	if (!nsession) return SWITCH_CAUSE_NORMAL_TEMPORARY_FAILURE;

	nch = switch_core_session_get_channel(nsession);
	switch_channel_set_name(nch, switch_core_session_sprintf(nsession, "ravenna/%s", epname));

	cp = switch_caller_profile_clone(nsession, outbound_profile);
	switch_channel_set_caller_profile(nch, cp);

	rs = switch_core_session_alloc(nsession, sizeof(*rs));
	memset(rs, 0, sizeof(*rs));
	rs->session  = nsession;
	rs->channel  = nch;
	rs->endpoint = e;
	switch_core_session_set_private(nsession, rs);

	switch_channel_set_state(nch, CS_INIT);
	switch_channel_set_flag(nch, CF_OUTBOUND);

	if (switch_core_session_thread_launch(nsession) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_destroy(&nsession);
		return SWITCH_CAUSE_NORMAL_TEMPORARY_FAILURE;
	}

	*new_session = nsession;
	return SWITCH_CAUSE_SUCCESS;
}

/* ==================================================================
 *  Reload — live config diff and stream reconciliation
 * ================================================================== */

/*
 * ravenna_reload() performs a non-destructive live reload:
 *
 *  1. Ask FreeSWITCH to refresh its XML source (switch_xml_reload).
 *     This is the same mechanism used by 'reloadxml' on the console
 *     and works regardless of whether config comes from disk, XML
 *     curl, XML LDAP, or any other bound XML source.
 *
 *  2. Parse ravenna.conf into a TEMPORARY set of stream/endpoint
 *     hashes using the existing load_streams()/load_endpoints()
 *     helpers, but pointed at a fresh pool so we never touch live
 *     data until the diff is complete.
 *
 *  3. Diff new vs current:
 *       - Streams present in current but absent in new  → REMOVE
 *       - Streams present in both but config changed    → RESTART
 *       - Streams present in new but absent in current  → ADD
 *       - Streams identical in both                     → KEEP (no-op)
 *     Endpoints are always replaced wholesale after streams settle,
 *     because they are purely config references (no sockets, no rings).
 *
 *  4. For REMOVE/RESTART streams: close sockets.  Active FS sessions
 *     that hold cursors continue to run; their cursors will drain to
 *     zero and read silence, which is the same behaviour as a network
 *     outage — the session is not forcibly hung up here.
 *
 *  5. For ADD/RESTART streams: copy the new ravenna_stream_t into
 *     the module pool, open its sockets, and insert into the live
 *     hash.
 *
 *  6. Wake the RX reactor so it rebuilds its pollset.
 */
switch_status_t ravenna_reload(switch_stream_handle_t *stream)
{
    switch_xml_t         cfg, xml, settings_xml, streams_xml, endpoints_xml;
    switch_hash_t       *new_streams   = NULL;
    switch_hash_t       *new_endpoints = NULL;
    switch_memory_pool_t *tmp_pool     = NULL;
    const char          *cf            = "ravenna.conf";
    const char          *xml_err       = NULL;
    switch_hash_index_t *hi;
    int added = 0, removed = 0, changed = 0, kept = 0;

    /* --- Step 1: refresh FS XML source -------------------------------- */
    switch_xml_reload(&xml_err);
    if (stream) {
        stream->write_function(stream, "XML reload: %s\n",
            xml_err ? xml_err : "OK");
    }

    /* --- Step 2: parse new config into a temporary pool/hashes -------- */
    if (switch_core_new_memory_pool(&tmp_pool) != SWITCH_STATUS_SUCCESS) {
        if (stream) stream->write_function(stream, "-ERR failed to create temp pool\n");
        return SWITCH_STATUS_GENERR;
    }
    switch_core_hash_init(&new_streams);
    switch_core_hash_init(&new_endpoints);

    if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "ravenna reload: %s not found\n", cf);
        /* Treat as "all streams removed" — fall through with empty hashes */
    } else {
        /* Temporarily point globals at the tmp pool/hashes so the existing
         * loaders write into them. */
        switch_hash_t       *save_streams   = mod_ravenna_globals.streams;
        switch_hash_t       *save_endpoints = mod_ravenna_globals.endpoints;
        switch_memory_pool_t *save_pool     = mod_ravenna_globals.pool;

        mod_ravenna_globals.streams   = new_streams;
        mod_ravenna_globals.endpoints = new_endpoints;
        mod_ravenna_globals.pool      = tmp_pool;

        if ((settings_xml  = switch_xml_child(cfg, "settings")))  load_settings(settings_xml);
        if ((streams_xml   = switch_xml_child(cfg, "streams")))    load_streams(streams_xml);
        if ((endpoints_xml = switch_xml_child(cfg, "endpoints")))  load_endpoints(endpoints_xml);

        mod_ravenna_globals.streams   = save_streams;
        mod_ravenna_globals.endpoints = save_endpoints;
        mod_ravenna_globals.pool      = save_pool;

        switch_xml_free(xml);
    }

    /* --- Step 3+4: diff and reconcile under the global mutex ----------- */
    switch_mutex_lock(mod_ravenna_globals.mutex);

    /* REMOVE or RESTART: walk current streams */
    for (hi = switch_core_hash_first(mod_ravenna_globals.streams); hi;
         hi = switch_core_hash_next(&hi)) {
        const void *k; void *v; switch_ssize_t klen;
        ravenna_stream_t *cur, *nw;
        switch_core_hash_this(hi, &k, &klen, &v);
        cur = (ravenna_stream_t *)v;
        nw  = (ravenna_stream_t *)switch_core_hash_find(new_streams, cur->name);

        if (!nw) {
            /* REMOVE */
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                              "ravenna reload: removing stream '%s'\n", cur->name);
            ravenna_stream_close_sockets(cur);
            switch_core_hash_delete(mod_ravenna_globals.streams, cur->name);
            removed++;
        } else if (!stream_equal(cur, nw)) {
            /* RESTART — close old sockets; the new version is added below */
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                              "ravenna reload: restarting stream '%s' (config changed)\n",
                              cur->name);
            ravenna_stream_close_sockets(cur);
            switch_core_hash_delete(mod_ravenna_globals.streams, cur->name);
            changed++;
        } else {
            kept++;
        }
    }

    /* ADD or complete RESTART: walk new streams */
    for (hi = switch_core_hash_first(new_streams); hi;
         hi = switch_core_hash_next(&hi)) {
        const void *k; void *v; switch_ssize_t klen;
        ravenna_stream_t *nw, *live;
        switch_core_hash_this(hi, &k, &klen, &v);
        nw = (ravenna_stream_t *)v;

        if (switch_core_hash_find(mod_ravenna_globals.streams, nw->name)) {
            continue; /* still present (kept) */
        }

        /* Copy into the module's permanent pool */
        live = switch_core_alloc(mod_ravenna_globals.pool, sizeof(*live));
        memcpy(live, nw, sizeof(*live));
        /* Reset runtime state that was copied from the template */
        live->rx_sock = RAVENNA_INVALID_SOCKET;
        live->rx2_sock = RAVENNA_INVALID_SOCKET;
        live->tx_sock = RAVENNA_INVALID_SOCKET;
        live->tx2_sock = RAVENNA_INVALID_SOCKET;
        live->rx_packets = 0; live->rx_drops = 0; live->rx_seq_gaps = 0;
        live->tx_packets = 0;
        live->rx2_packets = 0; live->rx2_drops = 0; live->rx_dedup_dropped = 0;
        live->rx_seq_inited = SWITCH_FALSE;
        memset(live->dedup_win, 0xFF, sizeof(live->dedup_win));
        switch_mutex_init(&live->mutex, SWITCH_MUTEX_NESTED, mod_ravenna_globals.pool);

        /* Recreate rings — ring pointers from the tmp pool are invalid */
        {
            int ch;
            for (ch = 0; ch < live->channels; ch++) {
                uint32_t cap = (uint32_t)live->samples_per_packet * RAVENNA_RING_PACKETS_DEFAULT;
                live->rx_rings[ch] = NULL;
                live->tx_rings[ch] = NULL;
                if (live->rx_enabled) ravenna_ring_create(&live->rx_rings[ch], mod_ravenna_globals.pool, cap);
                if (live->tx_enabled) ravenna_ring_create(&live->tx_rings[ch], mod_ravenna_globals.pool, cap);
            }
        }

        ravenna_stream_open_sockets(live);
        switch_core_hash_insert(mod_ravenna_globals.streams, live->name, live);

        if (changed > 0 &&
            !switch_core_hash_find(new_streams, live->name)) {
            /* came from a restart */
        } else {
            added++;
        }
    }

    /* Replace endpoints wholesale */
    switch_core_hash_destroy(&mod_ravenna_globals.endpoints);
    switch_core_hash_init(&mod_ravenna_globals.endpoints);
    for (hi = switch_core_hash_first(new_endpoints); hi;
         hi = switch_core_hash_next(&hi)) {
        const void *k; void *v; switch_ssize_t klen;
        ravenna_endpoint_t *ne, *live_ep;
        switch_core_hash_this(hi, &k, &klen, &v);
        ne = (ravenna_endpoint_t *)v;

        live_ep = switch_core_alloc(mod_ravenna_globals.pool, sizeof(*live_ep));
        memcpy(live_ep, ne, sizeof(*live_ep));
        /* Re-resolve stream pointers to the live hash */
        live_ep->in_stream  = ne->in_stream  ?
            switch_core_hash_find(mod_ravenna_globals.streams, ne->in_stream->name)  : NULL;
        live_ep->out_stream = ne->out_stream ?
            switch_core_hash_find(mod_ravenna_globals.streams, ne->out_stream->name) : NULL;
        switch_mutex_init(&live_ep->mutex, SWITCH_MUTEX_NESTED, mod_ravenna_globals.pool);
        switch_core_hash_insert(mod_ravenna_globals.endpoints, live_ep->name, live_ep);
    }

    switch_mutex_unlock(mod_ravenna_globals.mutex);

    /* Wake RX reactor to rebuild its pollset */
    ravenna_net_wake(mod_ravenna_globals.rx_wake_w);

    /* --- Cleanup temp resources --------------------------------------- */
    switch_core_hash_destroy(&new_streams);
    switch_core_hash_destroy(&new_endpoints);
    switch_core_destroy_memory_pool(&tmp_pool);

    if (stream) {
        stream->write_function(stream,
            "+OK reload complete: %d added, %d removed, %d restarted, %d unchanged\n",
            added, removed, changed, kept);
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "ravenna reload: +%d -%d ~%d =%d streams; endpoints replaced\n",
                      added, removed, changed, kept);
    return SWITCH_STATUS_SUCCESS;
}

/* ==================================================================
 *  API: ravenna status [json] | streams | debug on|off
 * ================================================================== */

#define RAVENNA_API_SYNTAX "status [json] | streams | endpoints | reload | debug on|off"

SWITCH_STANDARD_API(ravenna_api_function)
{
	char *argv[4] = { 0 };
	char *mycmd   = NULL;
	int   argc    = 0;

	if (!zstr(cmd)) {
		mycmd = strdup(cmd);
		argc  = switch_separate_string(mycmd, ' ', argv, switch_arraylen(argv));
	}

	if (argc < 1) {
		stream->write_function(stream, "-USAGE: %s\n", RAVENNA_API_SYNTAX);
		goto done;
	}

if (!strcasecmp(argv[0], "reload")) {
	return ravenna_reload(stream);
}

if (!strcasecmp(argv[0], "debug")) {
	if (argc >= 2) {
		mod_ravenna_globals.debug =
			!strcasecmp(argv[1], "on") ? SWITCH_TRUE : SWITCH_FALSE;
	}
	stream->write_function(stream, "+OK ravenna debug %s\n",
						   mod_ravenna_globals.debug ? "on" : "off");
	goto done;
}

	if (!strcasecmp(argv[0], "streams")) {
		switch_hash_index_t *hi;
		stream->write_function(stream,
			"%-20s %-7s %-18s %-5s %-18s %-5s %-3s %-7s %-12s %-12s %-8s\n",
			"name", "rx/tx", "rx-addr", "rx-pt", "tx-addr", "tx-pt",
			"ch", "ptime", "rx-pkts", "tx-pkts", "drops");
		for (hi = switch_core_hash_first(mod_ravenna_globals.streams); hi;
			 hi = switch_core_hash_next(&hi)) {
			const void *k; void *v; switch_ssize_t klen;
			ravenna_stream_t *s;
			switch_core_hash_this(hi, &k, &klen, &v);
			s = (ravenna_stream_t *)v;
			stream->write_function(stream,
				"%-20s %s/%s   %-18s %-5d %-18s %-5d %-3d %-7.3f %-12llu %-12llu %-8llu\n",
				s->name,
				s->rx_enabled ? "Y" : "-",
				s->tx_enabled ? "Y" : "-",
				s->rx_addr[0] ? s->rx_addr : "-", s->rx_port,
				s->tx_addr[0] ? s->tx_addr : "-", s->tx_port,
				s->channels, s->ptime_ms,
				(unsigned long long)s->rx_packets,
				(unsigned long long)s->tx_packets,
				(unsigned long long)s->rx_drops);
		}
		goto done;
	}

	if (!strcasecmp(argv[0], "endpoints")) {
		switch_hash_index_t *hi;
		stream->write_function(stream, "%-32s %-24s %-24s %-3s\n",
							   "name", "in", "out", "rx");
		for (hi = switch_core_hash_first(mod_ravenna_globals.endpoints); hi;
			 hi = switch_core_hash_next(&hi)) {
			const void *k; void *v; switch_ssize_t klen;
			ravenna_endpoint_t *e;
			char ib[64], ob[64];
			switch_core_hash_this(hi, &k, &klen, &v);
			e = (ravenna_endpoint_t *)v;
			switch_snprintf(ib, sizeof(ib), "%s:%d",
				e->in_stream  ? e->in_stream->name  : "-", e->inchan);
			switch_snprintf(ob, sizeof(ob), "%s:%d",
				e->out_stream ? e->out_stream->name : "-", e->outchan);
			stream->write_function(stream, "%-32s %-24s %-24s %-3d\n",
				e->name, ib, ob, e->active_rx_sessions);
		}
		goto done;
	}

	if (!strcasecmp(argv[0], "status")) {
		stream->write_function(stream,
			"ravenna: streams=%u endpoints=%u running=%s debug=%s ptp-timer=%s\n",
				hash_count(mod_ravenna_globals.streams),
				hash_count(mod_ravenna_globals.endpoints),
			mod_ravenna_globals.running ? "yes" : "no",
			mod_ravenna_globals.debug   ? "on"  : "off",
			RAVENNA_PTP_TIMER_NAME);
		goto done;
	}

	stream->write_function(stream, "-USAGE: %s\n", RAVENNA_API_SYNTAX);

done:
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

/* ==================================================================
 *  Module load/shutdown
 * ================================================================== */

SWITCH_MODULE_LOAD_FUNCTION(mod_ravenna_load)
{
	switch_api_interface_t  *api_interface;
	switch_threadattr_t     *thd_attr = NULL;

	memset(&mod_ravenna_globals, 0, sizeof(mod_ravenna_globals));
	mod_ravenna_globals.pool = pool;
	switch_mutex_init(&mod_ravenna_globals.mutex, SWITCH_MUTEX_NESTED, pool);
	switch_core_hash_init(&mod_ravenna_globals.streams);
	switch_core_hash_init(&mod_ravenna_globals.endpoints);

	/* Ensure mod_ptp_timer is loaded — it provides the "ptp" timer that the
	 * TX pacer requires.  If it is already loaded this is a cheap no-op.
	 * If the load fails we refuse to start rather than silently falling back
	 * to a non-PTP clock. */
	if (switch_loadable_module_exists("mod_ptp_timer") != SWITCH_STATUS_SUCCESS) {
		const char *err = NULL;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "mod_ravenna: mod_ptp_timer not loaded — attempting to load it\n");
		if (switch_loadable_module_load_module((char *)SWITCH_GLOBAL_dirs.mod_dir,
											   "mod_ptp_timer",
											   SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
							  "mod_ravenna: failed to load mod_ptp_timer (%s) — "
							  "a working PTP timer is required\n",
							  err ? err : "unknown error");
			return SWITCH_STATUS_GENERR;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "mod_ravenna: mod_ptp_timer loaded successfully\n");
	}

	if (ravenna_load_config() != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_GENERR;
	}

	/* Open all configured sockets up front. */
	{
		switch_hash_index_t *hi;
		for (hi = switch_core_hash_first(mod_ravenna_globals.streams); hi;
			 hi = switch_core_hash_next(&hi)) {
			const void *k; void *v; switch_ssize_t klen;
			ravenna_stream_t *s;
			switch_core_hash_this(hi, &k, &klen, &v);
			s = (ravenna_stream_t *)v;
			ravenna_stream_open_sockets(s);
		}
	}

	/* Wake pipe for the RX reactor. */
	if (ravenna_net_open_wake(&mod_ravenna_globals.rx_wake_r,
							  &mod_ravenna_globals.rx_wake_w) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
						  "ravenna: failed to create wake pipe\n");
		return SWITCH_STATUS_GENERR;
	}

	/* Endpoint registration */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	mod_ravenna_globals.endpoint_interface = (switch_endpoint_interface_t *)
		switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE);
	mod_ravenna_globals.endpoint_interface->interface_name = "ravenna";
	mod_ravenna_globals.endpoint_interface->io_routines    = &ravenna_io_routines;
	mod_ravenna_globals.endpoint_interface->state_handler  = &ravenna_state_handlers;

	SWITCH_ADD_API(api_interface, "ravenna",
				   "Ravenna/AES67 endpoint control",
				   ravenna_api_function, RAVENNA_API_SYNTAX);

	mod_ravenna_globals.running = SWITCH_TRUE;
	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_detach_set(thd_attr, 0);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&mod_ravenna_globals.rx_thread, thd_attr,
						 ravenna_rx_thread_run, NULL, pool);
	switch_thread_create(&mod_ravenna_globals.tx_thread, thd_attr,
						 ravenna_tx_thread_run, NULL, pool);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "mod_ravenna loaded: %u streams, %u endpoints\n",
					  hash_count(mod_ravenna_globals.streams),
					  hash_count(mod_ravenna_globals.endpoints));
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ravenna_shutdown)
{
	switch_status_t st;

	mod_ravenna_globals.running = SWITCH_FALSE;
	ravenna_net_wake(mod_ravenna_globals.rx_wake_w);

	if (mod_ravenna_globals.rx_thread) switch_thread_join(&st, mod_ravenna_globals.rx_thread);
	if (mod_ravenna_globals.tx_thread) switch_thread_join(&st, mod_ravenna_globals.tx_thread);

	ravenna_unload_config();
	ravenna_net_close(&mod_ravenna_globals.rx_wake_r);
	ravenna_net_close(&mod_ravenna_globals.rx_wake_w);

	switch_core_hash_destroy(&mod_ravenna_globals.streams);
	switch_core_hash_destroy(&mod_ravenna_globals.endpoints);
	return SWITCH_STATUS_SUCCESS;
}
