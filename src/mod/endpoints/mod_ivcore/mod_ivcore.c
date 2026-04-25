/**
 * mod_ivcore.c
 *
 * FreeSWITCH endpoint module for IVC/IVP intercom protocol.
 *
 * Architecture mirrors mod_portaudio:
 *   - One ring buffer per channel direction (rx_ring / tx_ring)
 *   - A per-channel receive thread (ivp_recv_loop) writes encoded bytes
 *     into rx_ring
 *   - read_frame() drains rx_ring and hands the encoded frame to FreeSWITCH
 *   - write_frame() accepts an encoded frame from FreeSWITCH and sends
 *     it over UDP
 *
 * Dial string format:
 *   ivcore/<card-name>/<port-name>/<called_number>
 *   ivcore/<card-name>/<called_number>   -- uses first port on the card
 *   ivcore/<called_number>               -- uses card "default", first port
 *
 * Config file: $FS_ROOT/conf/autoload_configs/ivcore.conf.xml
 */

#include "mod_ivcore.h"
#include "ivp_transport.h"
#include "ivp_dpi.h"

#include <switch.h>
#include <stdlib.h>
#include <string.h>

/* =====================================================================
 * Module globals
 * ===================================================================*/

ivcore_globals_t ivcore_globals;

SWITCH_MODULE_LOAD_FUNCTION(mod_ivcore_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ivcore_shutdown);
SWITCH_MODULE_DEFINITION(mod_ivcore, mod_ivcore_load, mod_ivcore_shutdown, NULL);
SWITCH_STANDARD_API(ivc_cmd);

/* =====================================================================
 * Card / port lookup
 * ===================================================================*/

const ivcore_card_t *ivcore_card_find(const char *name)
{
	int i;
	if (!name || !*name) name = "default";
	for (i = 0; i < ivcore_globals.card_count; i++) {
		if (!strcasecmp(ivcore_globals.cards[i].name, name))
			return &ivcore_globals.cards[i];
	}
	return NULL;
}

const ivcore_port_t *ivcore_port_find(const char *card_name, const char *port_name,
									   const ivcore_card_t **card_out)
{
	const ivcore_card_t *card = ivcore_card_find(card_name);
	int i;
	if (card_out) *card_out = card;
	if (!card) return NULL;

	if (!port_name || !*port_name)
		return (card->port_count > 0) ? &card->ports[0] : NULL;

	for (i = 0; i < card->port_count; i++) {
		if (!strcasecmp(card->ports[i].name, port_name))
			return &card->ports[i];
	}
	return NULL;
}

/* Apply card + port settings to a channel's connection params. */
static void apply_card_port(ivcore_channel_t *ch,
							 const ivcore_card_t *c, const ivcore_port_t *p)
{
	switch_copy_string(ch->params.server_ip,      c->server_ip,      sizeof(ch->params.server_ip));
	switch_copy_string(ch->params.username,       p->username,       sizeof(ch->params.username));
	switch_copy_string(ch->params.account,        p->username,       sizeof(ch->params.account));
	switch_copy_string(ch->params.password,       p->password,       sizeof(ch->params.password));
	switch_copy_string(ch->params.calling_name,   "sessiax2",        sizeof(ch->params.calling_name));
	switch_copy_string(ch->params.display_name,   p->username,       sizeof(ch->params.display_name));
	switch_copy_string(ch->params.called_context, c->context,        sizeof(ch->params.called_context));
	switch_copy_string(ch->params.encryption_key, c->encryption_key, sizeof(ch->params.encryption_key));
	switch_copy_string(ch->params.auth_key,       c->auth_key,       sizeof(ch->params.auth_key));

	if (p->device_type[0])
		switch_copy_string(ch->params.device_type, p->device_type, sizeof(ch->params.device_type));
	else
		switch_copy_string(ch->params.device_type, "lqsip",        sizeof(ch->params.device_type));

	/* Card-level TX pacing timer (mod_sofia-style "timer-name"). */
	switch_copy_string(ch->params.timer_name,
		(c->timer_name[0] ? c->timer_name : "soft"),
		sizeof(ch->params.timer_name));

	ch->params.tcp_port = c->tcp_port;
	ch->params.udp_port = c->udp_port;
	ch->ptime_ms        = (uint32_t)c->ptime_ms;

	switch (c->codec) {
	case 'a':
		ch->active_codec         = IVP_CODEC_G711A;
		ch->sample_rate          = 8000;
		ch->params.codec_family  = IVP_CODEC_G711A;
		ch->params.codec_format  = IVP_CODEC_G711A;
		ch->params.sampling_rate = 8000;
		ch->params.frame_size    = 8;   /* samples */
		ch->params.frame_time    = 1;   /* ms per frame */
		ch->params.frames_per_packet = 20;
		break;
	case 'g':
		ch->active_codec         = IVP_CODEC_G722;
		ch->sample_rate          = 16000;
		ch->params.codec_family  = IVP_CODEC_G722;
		ch->params.codec_format  = IVP_CODEC_G722;
		ch->params.sampling_rate = 16000;
		/* 160 bytes / 20 ms: matches FS G.722 native delivery and fits within
		 * the matrix LAN jitter buffer (max 64 ms).  The dedicated ivp_tx_loop
		 * paces these at exactly 20 ms intervals. */
		ch->params.frame_size    = 160;
		ch->params.frame_time    = 20;
		ch->params.frames_per_packet = 1;
		break;
	case 'p':
		ch->active_codec         = IVP_CODEC_PCM;
		ch->sample_rate          = 8000;
		ch->params.codec_family  = IVP_CODEC_PCM;
		ch->params.codec_format  = IVP_CODEC_PCM;
		ch->params.sampling_rate = 8000;
		ch->params.frame_size    = 8;
		ch->params.frame_time    = 1;
		ch->params.frames_per_packet = 20;
		break;
	default: /* 'u' = G.711 µ-law */
		ch->active_codec         = IVP_CODEC_G711U;
		ch->sample_rate          = 8000;
		ch->params.codec_family  = IVP_CODEC_G711U;
		ch->params.codec_format  = IVP_CODEC_G711U;
		ch->params.sampling_rate = 8000;
		ch->params.frame_size    = 8;
		ch->params.frame_time    = 1;
		ch->params.frames_per_packet = 20;
		break;
	}

	ch->params.protection_level  = 0;
	ch->params.user_id           = 0;

	/* LQ-SIP ports on IVC cards only support G.722.  If the XML config
	 * specified anything else (or left it at the µ-law default), force
	 * G.722 here so we don't waste time advertising a codec the card
	 * will silently ignore.  Other device types keep whatever the XML
	 * said. */
	if (!strcasecmp(ch->params.device_type, "lqsip") &&
		ch->active_codec != IVP_CODEC_G722) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"mod_ivcore: card '%s' port '%s' is LQ-SIP — forcing codec to G.722\n",
			c->name, p->name);
		ch->active_codec         = IVP_CODEC_G722;
		ch->sample_rate          = 16000;
		ch->params.codec_family  = IVP_CODEC_G722;
		ch->params.codec_format  = IVP_CODEC_G722;
		ch->params.sampling_rate = 16000;
		ch->params.frame_size    = 160;
		ch->params.frame_time    = 20;
		ch->params.frames_per_packet = 1;
	}

	/* ptime used for the FreeSWITCH codec init must match the IVP packet
	 * cadence (frame_time * frames_per_packet). */
	ch->ptime_ms = (uint32_t)(ch->params.frame_time * ch->params.frames_per_packet);
	if (ch->ptime_ms == 0) ch->ptime_ms = (uint32_t)c->ptime_ms;

	/* Derive the IVP media key (lowercase hex MD5 of username||password)
	 * if the card config did not provide one explicitly.  This is the
	 * value the IVC card validates against; an empty/wrong value causes
	 * the card to silently drop the NEW frame (no ACCEPT, no REJECT). */
	if (!ch->params.encryption_key[0] && p->password[0]) {
		char hash_input[256];
		char hash_out[SWITCH_MD5_DIGEST_STRING_SIZE];
		int  ulen = (int)strlen(p->username);
		int  plen = (int)strlen(p->password);
		if (ulen + plen <= (int)sizeof(hash_input)) {
			memcpy(hash_input,        p->username, (size_t)ulen);
			memcpy(hash_input + ulen, p->password, (size_t)plen);
			if (switch_md5_string(hash_out, (const void *)hash_input,
								   (switch_size_t)(ulen + plen)) == SWITCH_STATUS_SUCCESS) {
				switch_copy_string(ch->params.encryption_key, hash_out,
								   sizeof(ch->params.encryption_key));
				if (!ch->params.auth_key[0]) {
					switch_copy_string(ch->params.auth_key, hash_out,
									   sizeof(ch->params.auth_key));
				}
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
					"mod_ivcore: derived media key for port '%s' user '%s'\n",
					p->name, p->username);
			}
		}
	}
}

ivcore_channel_t *ivcore_channel_alloc(switch_core_session_t *session,
										const ivcore_card_t *card,
										const ivcore_port_t *port)
{
	ivcore_channel_t *ch = (ivcore_channel_t *)
		switch_core_session_alloc(session, sizeof(*ch));
	if (!ch) return NULL;

	/* switch_core_session_alloc already zeroes the memory — no memset needed */
	ch->session  = session;
	ch->channel  = switch_core_session_get_channel(session);
	ch->tcp_sock = -1;
	ch->udp_sock = -1;
	ch->running  = SWITCH_FALSE;

	ring_reset(&ch->rx_ring);

	switch_copy_string(ch->params.version_string, "3.0.0", sizeof(ch->params.version_string));

	if (card && port) {
		apply_card_port(ch, card, port);
	} else {
		switch_copy_string(ch->params.server_ip,      "127.0.0.1", sizeof(ch->params.server_ip));
		switch_copy_string(ch->params.called_context, "default",   sizeof(ch->params.called_context));
		switch_copy_string(ch->params.calling_name,   "sessiax2",  sizeof(ch->params.calling_name));
		switch_copy_string(ch->params.device_type,    "lqsip",     sizeof(ch->params.device_type));
		switch_copy_string(ch->params.timer_name,     "soft",      sizeof(ch->params.timer_name));
		ch->params.tcp_port          = IVC_TCP_PORT;
		ch->params.udp_port          = IVC_UDP_PORT;
		ch->active_codec             = IVP_CODEC_G722;
		ch->sample_rate              = 16000;
		ch->params.codec_family      = IVP_CODEC_G722;
		ch->params.codec_format      = IVP_CODEC_G722;
		ch->params.sampling_rate     = 16000;
		ch->params.frame_size        = 160;
		ch->params.frame_time        = 20;
		ch->params.frames_per_packet = 1;
		ch->ptime_ms                 = 20;	}

	return ch;
}

void ivcore_channel_free(ivcore_channel_t *ch)
{
	int i;
	if (!ch) return;
	ch->running = SWITCH_FALSE;

	if (ch->rx_thread) {
		switch_status_t st;
		switch_thread_join(&st, ch->rx_thread);
		ch->rx_thread = NULL;
	}

	ivp_transport_close(ch);

	switch_mutex_lock(ivcore_globals.mutex);
	for (i = 0; i < MAX_IVC_CHANNELS; i++) {
		if (ivcore_globals.channels[i] == ch) {
			ivcore_globals.channels[i] = NULL;
			ivcore_globals.channel_count--;
			break;
		}
	}
	switch_mutex_unlock(ivcore_globals.mutex);
}

/* =====================================================================
 * Codec setup — shared by outbound and autoconnect paths
 * ===================================================================*/

static switch_status_t ivcore_setup_codecs(switch_core_session_t *session,
                                            ivcore_channel_t *ch)
{
    const char *codec_name;
    int rate;
    int codec_ms;

    switch (ch->active_codec) {
    case IVP_CODEC_G711A: codec_name = "PCMA"; rate = 8000;  break;
    /* G.722 is registered in FreeSWITCH at the legacy RTP clock rate of
     * 8000 Hz even though it actually samples at 16 kHz. */
    case IVP_CODEC_G722:  codec_name = "G722"; rate = 8000;  break;
    case IVP_CODEC_PCM:   codec_name = "L16";  rate = 8000;  break;
    default:              codec_name = "PCMU"; rate = 8000;  break;
    }

    /* Fixed 20 ms — matches the IVP wire cadence exactly (160 bytes/frame for
     * G.722, 160 bytes/frame for G.711).  write_frame therefore delivers one
     * complete IVP packet per callback with no accumulation needed.
     *
     * G.722 NOTE: FreeSWITCH (following RFC 3551) registers G.722 at 8000 Hz
     * even though the codec internally runs at 16 kHz.  read_frame must
     * therefore set .rate=8000 and .samples=160 (not 16000/320) so that the
     * media core does not treat each 20 ms frame as a 10 ms frame and run the
     * audio at double speed on the bridged leg. */
    codec_ms = 20;
    if (switch_core_codec_init(&ch->read_codec, codec_name, NULL,
            NULL, rate, codec_ms, 1,
            SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
            NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "mod_ivcore: read codec init failed for %s at 20 ms\n", codec_name);
        return SWITCH_STATUS_FALSE;
    }

    if (switch_core_codec_init(&ch->write_codec, codec_name, NULL,
            NULL, rate, codec_ms, 1,
            SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
            NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "mod_ivcore: write codec init failed for %s at 20 ms\n", codec_name);
        switch_core_codec_destroy(&ch->read_codec);
        return SWITCH_STATUS_FALSE;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
        "mod_ivcore: codec %s initialized at 20 ms (%d bytes/frame)\n",
        codec_name, codec_ms * 8);

    switch_core_session_set_read_codec(session,  &ch->read_codec);
    switch_core_session_set_write_codec(session, &ch->write_codec);

    /* TX pacer — mirrors mod_sofia's <param name="timer-name"> behaviour.
     * "none" disables pacing entirely (channel_write_frame sends as fast as
     * FreeSWITCH delivers).  "soft" (default) gates each write to the
     * negotiated ptime, eliminating the "hurried/distorted" symptom for
     * tone_stream / playback sources that have no read-leg back-pressure.
     * Any other value is passed through to switch_core_timer_init() so the
     * full set of installed FreeSWITCH timer modules can be selected. */
    if (!ch->write_timer_inited) {
        const char *tname = ch->params.timer_name[0] ? ch->params.timer_name : "soft";
        if (!strcasecmp(tname, "none")) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                "mod_ivcore: TX pacing disabled (timer-name=none)\n");
        } else {
            int samples_per_tick = (rate / 1000) * codec_ms;  /* 8000/1000 × 20 = 160 */
            if (switch_core_timer_init(&ch->write_timer, tname, codec_ms,
                    samples_per_tick,
                    switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS) {
                ch->write_timer_inited = SWITCH_TRUE;
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                    "mod_ivcore: TX pacer '%s' init at %d ms / %d samples\n",
                    tname, codec_ms, samples_per_tick);
            } else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                    "mod_ivcore: TX pacer '%s' init failed — falling back to unpaced\n",
                    tname);
            }
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

/* =====================================================================
 * Dial string parser
 * ===================================================================*/

static switch_bool_t parse_dialstring(ivcore_channel_t *ch, const char *dest)
{
	char tmp[256];
	const char *card_name;
	const char *port_name;
	const char *called;
	char *slash1;
	char *slash2;
	const ivcore_card_t *card = NULL;
	const ivcore_port_t *port;

	switch_copy_string(tmp, dest, sizeof(tmp));

	card_name = "default";
	port_name = NULL;
	called    = tmp;

	slash1 = strchr(tmp, '/');
	if (slash1) {
		*slash1   = '\0';
		card_name = tmp;
		called    = slash1 + 1;

		slash2 = strchr(slash1 + 1, '/');
		if (slash2) {
			*slash2   = '\0';
			port_name = slash1 + 1;
			called    = slash2 + 1;
		}
	}

	port = ivcore_port_find(card_name, port_name, &card);

	if (!card) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_ivcore: card '%s' not found, using defaults\n", card_name);
	} else if (!port) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_ivcore: card '%s' has no port '%s', using defaults\n",
			card_name, port_name ? port_name : "(first)");
	} else {
		apply_card_port(ch, card, port);
	}

	switch_copy_string(ch->params.called_number, called, sizeof(ch->params.called_number));
	return SWITCH_TRUE;
}

/* =====================================================================
 * FreeSWITCH I/O routines
 * ===================================================================*/

static switch_call_cause_t channel_outgoing_channel(
	switch_core_session_t *session,
	switch_event_t *var_event,
	switch_caller_profile_t *outbound_profile,
	switch_core_session_t **new_session,
	switch_memory_pool_t **pool,
	switch_originate_flag_t flags,
	switch_call_cause_t *cancel_cause)
{
	switch_call_cause_t cause = SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	switch_core_session_t *nsession = NULL;
	ivcore_channel_t *ch;
	int i;

	(void)session;
	(void)cancel_cause;

	if (!(nsession = switch_core_session_request_uuid(
			  ivcore_globals.endpoint_interface,
			  SWITCH_CALL_DIRECTION_OUTBOUND,
			  flags, pool,
			  switch_event_get_header(var_event, "origination_uuid")))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
			"mod_ivcore: session request failed\n");
		goto fail;
	}

	ch = ivcore_channel_alloc(nsession, NULL, NULL);
	if (!ch) goto fail;

	switch_core_session_set_private(nsession, ch);

	if (outbound_profile && outbound_profile->destination_number) {
		parse_dialstring(ch, outbound_profile->destination_number);
	}

	if (ivcore_setup_codecs(nsession, ch) != SWITCH_STATUS_SUCCESS) {
		goto fail;
	}

	{
		switch_caller_profile_t *caller_profile =
			switch_caller_profile_clone(nsession, outbound_profile);
		switch_channel_set_caller_profile(ch->channel, caller_profile);
	}

	switch_channel_set_name(ch->channel, "ivcore");
	switch_channel_set_flag(ch->channel, CF_AUDIO);

	switch_mutex_lock(ivcore_globals.mutex);
	for (i = 0; i < MAX_IVC_CHANNELS; i++) {
		if (!ivcore_globals.channels[i]) {
			ivcore_globals.channels[i] = ch;
			ivcore_globals.channel_count++;
			break;
		}
	}
	switch_mutex_unlock(ivcore_globals.mutex);

	if (ivp_udp_open(ch) != SWITCH_STATUS_SUCCESS) {
		cause = SWITCH_CAUSE_NETWORK_OUT_OF_ORDER;
		goto fail;
	}
	if (ivp_tcp_login(ch) != SWITCH_STATUS_SUCCESS) {
		cause = SWITCH_CAUSE_NETWORK_OUT_OF_ORDER;
		goto fail;
	}

	/* Start recv thread BEFORE sending NEW. */
	ch->running = SWITCH_TRUE;
	{
		switch_threadattr_t *tattr;
		switch_threadattr_create(&tattr, switch_core_session_get_pool(nsession));
		switch_threadattr_detach_set(tattr, 0);
		switch_thread_create(&ch->rx_thread, tattr, ivp_recv_loop, ch,
							 switch_core_session_get_pool(nsession));
	}

	if (ivp_send_new(ch) != SWITCH_STATUS_SUCCESS) {
		ch->running = SWITCH_FALSE;
		cause = SWITCH_CAUSE_NETWORK_OUT_OF_ORDER;
		goto fail;
	}

	switch_channel_mark_pre_answered(ch->channel);
	switch_channel_set_state(ch->channel, CS_INIT);
	*new_session = nsession;
	cause = SWITCH_CAUSE_SUCCESS;
	return cause;

fail:
	if (nsession) {
		switch_core_session_destroy(&nsession);
	}
	return cause;
}

static switch_status_t channel_read_frame(switch_core_session_t *session,
										   switch_frame_t **frame,
										   switch_io_flag_t flags,
										   int stream_id)
{
	uint32_t frame_bytes;
	uint32_t avail;
	ivcore_channel_t *ch = (ivcore_channel_t *)
		switch_core_session_get_private(session);

	(void)flags;
	(void)stream_id;

	if (!ch || ch->call_state == IVC_STATE_HANGUP)
		return SWITCH_STATUS_FALSE;

	/* While waiting for ACCEPT, yield one frame interval and return CNG. */
	if (ch->call_state == IVC_STATE_CONNECTING) {
		switch_yield(20 * 1000);
		memset(ch->read_frame_data, 0, 160);
		ch->read_frame.data    = ch->read_frame_data;
		ch->read_frame.datalen = 160;
		ch->read_frame.buflen  = (uint32_t)sizeof(ch->read_frame_data);
		ch->read_frame.samples = 160;   /* G.722: 20 ms × 8 kHz RTP clock (RFC 3551) */
		ch->read_frame.rate    = 8000;
		ch->read_frame.codec   = &ch->read_codec;
		ch->read_frame.flags   = SFF_CNG;
		*frame = &ch->read_frame;
		return SWITCH_STATUS_SUCCESS;
	}

	/* Fixed 160 bytes / 20 ms / 320 samples — matches the negotiated
	 * G.722 provisioning (frameSize=160, frameTime=20, fpp=1). */
	frame_bytes = 160;

	avail = ring_available(&ch->rx_ring);
	if (avail >= frame_bytes) {
		ring_read(&ch->rx_ring, ch->read_frame_data, frame_bytes);
		ch->read_frame.flags = 0;
	} else {
		/* No packet ready yet — yield one frame interval and send CNG. */
		switch_yield(20 * 1000);
		memset(ch->read_frame_data, 0, frame_bytes);
		ch->read_frame.flags = SFF_CNG;
	}

	ch->read_frame.data    = ch->read_frame_data;
	ch->read_frame.datalen = frame_bytes;
	ch->read_frame.buflen  = (uint32_t)sizeof(ch->read_frame_data);
	ch->read_frame.samples = 160;   /* G.722: 20 ms × 8 kHz RTP clock (RFC 3551) */
	ch->read_frame.rate    = 8000;
	ch->read_frame.codec   = &ch->read_codec;

	*frame = &ch->read_frame;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_write_frame(switch_core_session_t *session,
											switch_frame_t *frame,
											switch_io_flag_t flags,
											int stream_id)
{
	ivcore_channel_t *ch = (ivcore_channel_t *)
		switch_core_session_get_private(session);

	(void)flags;
	(void)stream_id;

	if (!ch || ch->call_state != IVC_STATE_UP)
		return SWITCH_STATUS_SUCCESS;

	/* Skip comfort-noise/silence marker frames — they are FS-internal
	 * placeholders and must not be forwarded as real G.722 audio. */
	if (frame->flags & SFF_CNG)
		return SWITCH_STATUS_SUCCESS;

	/* Pace to the negotiated 20 ms wire cadence.  Without this, sources
	 * like tone_stream/playback (no read-leg back-pressure) blast frames
	 * back-to-back and the matrix renders them "hurried/distorted".
	 *
	 * When `ivc debug on`, capture three numbers per write:
	 *   arr_us   = wall-clock between this write_frame and the previous one
	 *              BEFORE the pacer runs.  Small (<5 ms) means FS is firing
	 *              writes back-to-back — exactly the "hurried" symptom.
	 *   sleep_us = how long switch_core_timer_next() actually blocked.
	 *              Should be ≈ (ptime - arr_us) when FS is too fast, ≈0 when
	 *              FS is already paced upstream (e.g. bridged RTP).
	 *   datalen / call_state / codec — sanity context.
	 *
	 * Aggregated min/avg/max for arr & sleep are dumped once per second so the
	 * log isn't drowned at 50 frames/s. */
	{
		switch_bool_t dbg = (ivcore_globals.debug == SWITCH_TRUE) ? SWITCH_TRUE : SWITCH_FALSE;
		switch_time_t entry_us = dbg ? switch_micro_time_now() : 0;
		switch_time_t after_pace_us;
		uint32_t arr_us = 0;
		uint32_t sleep_us = 0;

		if (dbg && ch->tx_dbg_last_entry_us != 0) {
			arr_us = (uint32_t)(entry_us - ch->tx_dbg_last_entry_us);
		}
		if (dbg) {
			ch->tx_dbg_last_entry_us = entry_us;
		}

		if (ch->write_timer_inited) {
			switch_core_timer_next(&ch->write_timer);
		}

		if (dbg) {
			after_pace_us = switch_micro_time_now();
			sleep_us = (uint32_t)(after_pace_us - entry_us);

			/* Update rolling stats. */
			if (ch->tx_dbg_frames == 0) {
				ch->tx_dbg_arr_min_us   = arr_us;
				ch->tx_dbg_arr_max_us   = arr_us;
				ch->tx_dbg_sleep_min_us = sleep_us;
				ch->tx_dbg_sleep_max_us = sleep_us;
				ch->tx_dbg_arr_sum_us   = 0;
				ch->tx_dbg_sleep_sum_us = 0;
				ch->tx_dbg_last_log_us  = after_pace_us;
			} else {
				if (arr_us   < ch->tx_dbg_arr_min_us)   ch->tx_dbg_arr_min_us   = arr_us;
				if (arr_us   > ch->tx_dbg_arr_max_us)   ch->tx_dbg_arr_max_us   = arr_us;
				if (sleep_us < ch->tx_dbg_sleep_min_us) ch->tx_dbg_sleep_min_us = sleep_us;
				if (sleep_us > ch->tx_dbg_sleep_max_us) ch->tx_dbg_sleep_max_us = sleep_us;
			}
			ch->tx_dbg_arr_sum_us   += arr_us;
			ch->tx_dbg_sleep_sum_us += sleep_us;
			ch->tx_dbg_frames++;

			/* Per-frame trace (DEBUG level so it's only seen when both
			 * `ivc debug on` AND the FS log level is at DEBUG). */
			IVC_LOG_DEBUG("mod_ivcore: TX pacer frame#%u arr=%u us sleep=%u us "
				"datalen=%u flags=0x%X\n",
				(unsigned)ch->tx_dbg_frames,
				(unsigned)arr_us, (unsigned)sleep_us,
				(unsigned)frame->datalen, (unsigned)frame->flags);

			/* Once-per-second summary at INFO level so it shows up in the
			 * normal log without spamming. */
			if (after_pace_us - ch->tx_dbg_last_log_us >= 1000000) {
				uint32_t n = ch->tx_dbg_frames;
				uint32_t arr_avg   = (uint32_t)(ch->tx_dbg_arr_sum_us   / (n ? n : 1));
				uint32_t sleep_avg = (uint32_t)(ch->tx_dbg_sleep_sum_us / (n ? n : 1));
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					"mod_ivcore: TX pacer 1s stats  frames=%u  "
					"arr min/avg/max=%u/%u/%u us  "
					"sleep min/avg/max=%u/%u/%u us  "
					"timer=%s\n",
					(unsigned)n,
					(unsigned)ch->tx_dbg_arr_min_us, (unsigned)arr_avg, (unsigned)ch->tx_dbg_arr_max_us,
					(unsigned)ch->tx_dbg_sleep_min_us, (unsigned)sleep_avg, (unsigned)ch->tx_dbg_sleep_max_us,
					ch->write_timer_inited ? (ch->params.timer_name[0] ? ch->params.timer_name : "soft") : "none");
				ch->tx_dbg_frames      = 0;
				ch->tx_dbg_last_log_us = after_pace_us;
			}
		}
	}

	if (frame->datalen > 0) {
		static uint32_t s_write_count = 0;
		if (s_write_count < 20) {
			/* Full diagnostic on first 20 frames: show flags, size, rate,
			 * and a hex dump of the first 32 bytes so we can verify the
			 * codec bytes look like G.722 (not PCM, not silence, not junk). */
			char hexbuf[128] = {0};
			const uint8_t *d = (const uint8_t *)frame->data;
			uint32_t dump = frame->datalen < 32 ? frame->datalen : 32;
			uint32_t hi;
			for (hi = 0; hi < dump; hi++)
				switch_snprintf(hexbuf + hi * 3, sizeof(hexbuf) - hi * 3,
					"%02X ", d[hi]);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
				"mod_ivcore: TX #%u datalen=%u samples=%u rate=%u flags=0x%X "
				"codec_impl=%s  [%s...]\n",
				(unsigned)s_write_count,
				(unsigned)frame->datalen,
				(unsigned)frame->samples,
				(unsigned)frame->rate,
				(unsigned)frame->flags,
				ch->write_codec.implementation
					? ch->write_codec.implementation->iananame : "?",
				hexbuf);
		} else if ((s_write_count % 200) == 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
				"mod_ivcore: TX #%u datalen=%u samples=%u rate=%u flags=0x%X\n",
				(unsigned)s_write_count, (unsigned)frame->datalen,
				(unsigned)frame->samples, (unsigned)frame->rate,
				(unsigned)frame->flags);
		}
		s_write_count++;

		/* FS is initialised at 20 ms so write_frame always delivers exactly
		 * 160 bytes — one complete IVP packet.  Send it directly. */
		ch->last_write_us = switch_micro_time_now();
		ivp_send_media(ch, (const uint8_t *)frame->data, (int)frame->datalen);
	}

	return SWITCH_STATUS_SUCCESS;
}

/* Forward declarations — defined after the config loader. */
static void spawn_autoconnect_session(int ci, int pi);
static void schedule_autoconnect_retry(int ci, int pi);

static switch_status_t channel_on_hangup(switch_core_session_t *session)
{
	/* channel_on_hangup runs in the session's own thread (CS_HANGUP state handler).
	 * ivcore_channel_free() joins rx_thread before returning, so the recv loop
	 * will not outlive the session — no rwunlock needed here. */
	ivcore_channel_t *ch = (ivcore_channel_t *)
		switch_core_session_get_private(session);
	switch_bool_t respawn      = SWITCH_FALSE;
	int           respawn_ci   = 0;
	int           respawn_pi   = 0;

	if (!ch) return SWITCH_STATUS_SUCCESS;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: channel hangup\n");

	/* Signal the matrix BEFORE sending IVP HANGUP.  The IVC card stops
	 * forwarding Data frames once it processes a HANGUP, so the 0x93
	 * KeyStatusUpdate must go out first or CPUApp never sees it.
	 *
	 * Always send the panel-initiated 0xF4 disconnect request whenever
	 * dpi_state indicates the port was in any telephony-active state.
	 * This clears CPUApp's dial buffer so the next dial is clean.
	 * Note: we send the panel-request form [0xF4][reason], NOT the reply
	 * form [0xF4][success] — the latter is only processed by CPUApp when
	 * it was the one that originated the disconnect. */
	if (ch->dpi_send_cb) {
		if (ch->dpi_state == (uint8_t)IVP_SIP_STATE_CONNECTED_OUT ||
			ch->dpi_state == (uint8_t)IVP_SIP_STATE_CONNECTING_OUT) {
			ivp_dpi_send_key_status_update(ch, /*press*/  1, ch->dpi_send_cb);
			ivp_dpi_send_key_status_update(ch, /*release*/0, ch->dpi_send_cb);
		}
		/* Send in all non-idle telephony states so CPUApp always resets. */
		if (ch->dpi_state != (uint8_t)IVP_SIP_STATE_ON_HOOK) {
			ivp_dpi_send_disconnect_reply(ch,
				(uint8_t)IVP_SIP_REASON_LOCAL_END,
				ch->dpi_send_cb);
		}
	}

	if (ch->call_state == IVC_STATE_UP || ch->call_state == IVC_STATE_CONNECTING)
		ivp_send_hangup(ch);

	/* Capture respawn info before ivcore_channel_free() zeroes the struct. */
	if (ch->is_autoconnect) {
		respawn    = SWITCH_TRUE;
		respawn_ci = ch->autoconnect_card_idx;
		respawn_pi = ch->autoconnect_port_idx;
	}

	ivcore_channel_free(ch);

	switch_core_codec_destroy(&ch->read_codec);
	switch_core_codec_destroy(&ch->write_codec);

	if (ch->write_timer_inited) {
		switch_core_timer_destroy(&ch->write_timer);
		ch->write_timer_inited = SWITCH_FALSE;
	}

	/* Re-establish the standing IVP session so the matrix can dial again
	 * without a module reload.  A brief delay lets the IVC card fully
	 * process the HANGUP before we send a new NEW frame. */
	if (respawn) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"mod_ivcore: respawning autoconnect session (card=%d port=%d)\n",
			respawn_ci, respawn_pi);
		switch_yield(500000); /* 500 ms */
		spawn_autoconnect_session(respawn_ci, respawn_pi);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_init(switch_core_session_t *session)
{
	switch_channel_t *chan = switch_core_session_get_channel(session);
	/* Skip CS_ROUTING for the initial IVP call setup — the IVP leg has no
	 * inbound dialplan to run; it sits in CS_EXCHANGE_MEDIA until the matrix
	 * sends a 0xF1 DialOut, at which point switch_ivr_session_transfer() will
	 * re-enter CS_ROUTING and the standard routing handler will run the
	 * dialplan using the new caller profile.  If we pushed to CS_ROUTING here
	 * our channel_on_routing would hijack that second routing pass too. */
	switch_channel_set_state(chan, CS_EXCHANGE_MEDIA);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_exchange_media(switch_core_session_t *session)
{
	switch_channel_t *chan = switch_core_session_get_channel(session);
	ivcore_channel_t *ch = (ivcore_channel_t *)switch_core_session_get_private(session);
	switch_frame_t *read_frame = NULL;

	/* Block here for the lifetime of the IVP connection.
	 * Without this loop the session thread falls through to CS_HANGUP
	 * immediately, causing channel_kill_channel to fire and stop the
	 * recv thread before any ACCEPT can arrive from the IVC card.
	 *
	 * When the matrix sends a 0xF1 DialOut message, ivp_dpi.c sets
	 * ch->dpi_dial_pending = TRUE and leaves the number in ch->dpi_dial_buffer.
	 * We detect that here and call switch_ivr_session_transfer() to hand
	 * this IVP session into the dialplan — exactly the same mechanism used
	 * by the 'transfer' dialplan app and mod_skinny.  The dialplan can then
	 * bridge to a SIP endpoint using ch->params.called_context. */
	while (switch_channel_ready(chan) && ch && ch->running == SWITCH_TRUE) {
		switch_status_t status;

		if (ch->dpi_dial_pending && ch->dpi_dial_buffer[0]) {
			char dest[sizeof(ch->dpi_dial_buffer)];
			switch_copy_string(dest, ch->dpi_dial_buffer, sizeof(dest));
			ch->dpi_dial_buffer[0] = '\0';
			ch->dpi_dial_pending   = SWITCH_FALSE;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"mod_ivcore: DPI dial → transferring session to dialplan "
				"dest='%s' dialplan='XML' context='%s'\n",
				dest, ch->params.called_context[0]
					? ch->params.called_context : "default");

			switch_ivr_session_transfer(session, dest, "XML",
				ch->params.called_context[0]
					? ch->params.called_context : "default");
			/* switch_ivr_session_transfer() changes the channel state;
			 * the session is no longer ours to drive — exit the loop. */
			break;
		}

		status = switch_core_session_read_frame(session, &read_frame,
											   SWITCH_IO_FLAG_NONE, 0);
		if (!SWITCH_READ_ACCEPTABLE(status)) break;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_soft_execute(switch_core_session_t *session)
{
	(void)session;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_kill_channel(switch_core_session_t *session,
											 int sig)
{
	ivcore_channel_t *ch = (ivcore_channel_t *)
		switch_core_session_get_private(session);

	if (!ch) return SWITCH_STATUS_SUCCESS;

	/* SWITCH_SIG_BREAK is sent on every state transition just to wake
	 * blocking I/O — it must NOT terminate the recv thread, otherwise
	 * the very first CS_INIT -> CS_ROUTING transition would kill the
	 * channel before any ACCEPT can arrive from the IVC card.
	 * Only SWITCH_SIG_KILL means the channel is really going away.
	 * Same convention used by mod_portaudio / mod_rtc / mod_khomp. */
	switch (sig) {
	case SWITCH_SIG_KILL:
		ch->running = SWITCH_FALSE;
		break;
	case SWITCH_SIG_BREAK:
	default:
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_send_dtmf(switch_core_session_t *session,
										  const switch_dtmf_t *dtmf)
{
	(void)session;
	(void)dtmf;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_message(switch_core_session_t *session,
												switch_core_session_message_t *msg)
{
	ivcore_channel_t *ch = (ivcore_channel_t *)
		switch_core_session_get_private(session);

	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_ANSWER:
		if (ch) {
			ch->call_state = IVC_STATE_UP;
			/* The bridged SIP call was answered — tell the matrix the
			 * outbound call connected.  Without this 0xF1 ConnectReply
			 * CPUApp keeps the port in ConnectingOut state indefinitely. */
			if (ch->dpi_send_cb) {
				ch->dpi_state = (uint8_t)IVP_SIP_STATE_CONNECTED_OUT;
				ivp_dpi_send_connect_reply(ch,
					/*success*/ 1,
					/*reason */ (uint8_t)IVP_SIP_REASON_NOT_SET,
					/*state  */ ch->dpi_state,
					ch->dpi_send_cb);
			}
		}
		break;
	case SWITCH_MESSAGE_INDICATE_RINGING:
		if (ch) ch->call_state = IVC_STATE_RINGING;
		break;
	default:
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

/* =====================================================================
 * State handlers and I/O routine tables
 * ===================================================================*/

static switch_state_handler_table_t channel_event_handlers = {
	/*.on_init*/            channel_on_init,
	/*.on_routing*/         NULL,  /* standard routing runs the dialplan on transfer */
	/*.on_execute*/         NULL,
	/*.on_hangup*/          channel_on_hangup,
	/*.on_exchange_media*/  channel_on_exchange_media,
	/*.on_soft_execute*/    channel_on_soft_execute,
	/*.on_consume_media*/   NULL,
	/*.on_hibernate*/       NULL,
	/*.on_reset*/           NULL,
	/*.on_park*/            NULL,
	/*.on_reporting*/       NULL,
	/*.on_destroy*/         NULL,
};

static switch_io_routines_t channel_io_routines = {
	/*.outgoing_channel*/  channel_outgoing_channel,
	/*.read_frame*/        channel_read_frame,
	/*.write_frame*/       channel_write_frame,
	/*.kill_channel*/      channel_kill_channel,
	/*.send_dtmf*/         channel_send_dtmf,
	/*.receive_message*/   channel_receive_message,
	/*.receive_event*/     NULL,
	/*.state_change*/      NULL,
	/*.read_video_frame*/  NULL,
	/*.write_video_frame*/ NULL,
};

/* =====================================================================
 * Config loader
 * ===================================================================*/

static switch_status_t load_config(void)
{
	switch_xml_t cfg, xml, cards_node, card_node, port_node, param;
	switch_xml_t settings_node;

	ivcore_globals.card_count = 0;

	if (!(xml = switch_xml_open_cfg("ivcore.conf", &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_ivcore: ivcore.conf.xml not found - no cards loaded\n");
		return SWITCH_STATUS_SUCCESS;
	}

	/* Optional <settings> section for module-level params. */
	if ((settings_node = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings_node, "param"); param; param = param->next) {
			const char *n = switch_xml_attr_soft(param, "name");
			const char *v = switch_xml_attr_soft(param, "value");
			if (!n || !v) continue;
			if (!strcasecmp(n, "debug"))
				ivcore_globals.debug = switch_true(v) ? SWITCH_TRUE : SWITCH_FALSE;
		}
	}

	if (!(cards_node = switch_xml_child(cfg, "cards"))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_ivcore: no <cards> section in ivcore.conf.xml\n");
		switch_xml_free(xml);
		return SWITCH_STATUS_SUCCESS;
	}

	for (card_node = switch_xml_child(cards_node, "card");
		 card_node;
		 card_node = card_node->next)
	{
		const char *cname;
		ivcore_card_t *c;

		if (ivcore_globals.card_count >= MAX_IVC_PROFILES) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: MAX_IVC_PROFILES (%d) reached, skipping remaining cards\n",
				MAX_IVC_PROFILES);
			break;
		}

		cname = switch_xml_attr_soft(card_node, "name");
		if (!cname || !*cname) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: <card> missing name attribute, skipping\n");
			continue;
		}

		c = &ivcore_globals.cards[ivcore_globals.card_count];
		memset(c, 0, sizeof(*c));

		switch_copy_string(c->name, cname, sizeof(c->name));

		c->tcp_port = IVC_TCP_PORT;
		c->udp_port = IVC_UDP_PORT;
		c->codec    = 'u';
		c->ptime_ms = 20;
		switch_copy_string(c->timer_name, "soft",     sizeof(c->timer_name));
		switch_copy_string(c->context,   "default",   sizeof(c->context));
		switch_copy_string(c->server_ip, "127.0.0.1", sizeof(c->server_ip));

		for (param = switch_xml_child(card_node, "param"); param; param = param->next) {
			const char *n = switch_xml_attr_soft(param, "name");
			const char *v = switch_xml_attr_soft(param, "value");
			if (!n || !v) continue;

			if      (!strcasecmp(n, "login-ip"))       switch_copy_string(c->server_ip,      v, sizeof(c->server_ip));
			else if (!strcasecmp(n, "tcp-port"))       c->tcp_port = atoi(v);
			else if (!strcasecmp(n, "udp-port"))       c->udp_port = atoi(v);
			else if (!strcasecmp(n, "context"))        switch_copy_string(c->context,        v, sizeof(c->context));
			else if (!strcasecmp(n, "codec"))          c->codec    = v[0];
			else if (!strcasecmp(n, "ptime"))          c->ptime_ms = atoi(v);
			else if (!strcasecmp(n, "timer-name"))     switch_copy_string(c->timer_name,     v, sizeof(c->timer_name));
			else if (!strcasecmp(n, "encryption-key")) switch_copy_string(c->encryption_key, v, sizeof(c->encryption_key));
			else if (!strcasecmp(n, "auth-key"))       switch_copy_string(c->auth_key,       v, sizeof(c->auth_key));
			else if (!strcasecmp(n, "autoconnect"))    c->autoconnect = switch_true(v) ? SWITCH_TRUE : SWITCH_FALSE;
		}

		for (port_node = switch_xml_child(card_node, "port");
			 port_node;
			 port_node = port_node->next)
		{
			const char *pname;
			const char *u, *w, *t;
			ivcore_port_t *p;

			if (c->port_count >= MAX_PORTS_PER_CARD) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					"mod_ivcore: card '%s' MAX_PORTS_PER_CARD (%d) reached\n",
					c->name, MAX_PORTS_PER_CARD);
				break;
			}

			pname = switch_xml_attr_soft(port_node, "name");
			if (!pname || !*pname) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					"mod_ivcore: <port> in card '%s' missing name, skipping\n", c->name);
				continue;
			}

			p = &c->ports[c->port_count];
			memset(p, 0, sizeof(*p));
			switch_copy_string(p->name, pname, sizeof(p->name));

			u = switch_xml_attr_soft(port_node, "username");
			w = switch_xml_attr_soft(port_node, "password");
			t = switch_xml_attr_soft(port_node, "type");
			if (u) switch_copy_string(p->username,    u,                sizeof(p->username));
			if (w) switch_copy_string(p->password,    w,                sizeof(p->password));
			switch_copy_string(p->device_type, (t && *t) ? t : "lqsip", sizeof(p->device_type));

			{
				const char *ac  = switch_xml_attr_soft(port_node, "autoconnect");
				const char *acn = switch_xml_attr_soft(port_node, "autoconnect-number");
				/* Inherit card default; port attribute overrides if present. */
				if (ac && *ac)
					p->autoconnect = switch_true(ac) ? SWITCH_TRUE : SWITCH_FALSE;
				else
					p->autoconnect = c->autoconnect;
				switch_copy_string(p->autoconnect_number,
					(acn && *acn) ? acn : "1",
					sizeof(p->autoconnect_number));
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"mod_ivcore:   port '%s' username='%s'\n", p->name, p->username);

			c->port_count++;
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"mod_ivcore: loaded card '%s' - %s:%d codec=%c ptime=%d context=%s ports=%d\n",
			c->name, c->server_ip, c->tcp_port, c->codec, c->ptime_ms,
			c->context, c->port_count);

		ivcore_globals.card_count++;
	}

	switch_xml_free(xml);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: %d card(s) loaded\n", ivcore_globals.card_count);

	return SWITCH_STATUS_SUCCESS;
}

/* =====================================================================
 * API command helpers
 * ===================================================================*/

static const char *call_state_str(ivcore_call_state_t s)
{
    switch (s) {
    case IVC_STATE_IDLE:       return "idle";
    case IVC_STATE_CONNECTING: return "connecting";
    case IVC_STATE_RINGING:    return "ringing";
    case IVC_STATE_UP:         return "up";
    case IVC_STATE_HANGUP:     return "hangup";
    default:                   return "unknown";
    }
}

static const char *codec_str(ivp_audio_codec_t c)
{
    switch (c) {
    case IVP_CODEC_G711U: return "PCMU";
    case IVP_CODEC_G711A: return "PCMA";
    case IVP_CODEC_G722:  return "G722";
    case IVP_CODEC_PCM:   return "L16";
    default:              return "unknown";
    }
}

/* ivc list  — plain-text table */
static void cmd_list(switch_stream_handle_t *stream)
{
    int i;
    int found = 0;

    stream->write_function(stream,
        "%-4s %-38s %-16s %-12s %-6s %-10s %-8s %-16s\n",
        "#", "UUID", "Dest", "Server", "State", "Codec", "Ptime", "IVP-User");
    stream->write_function(stream,
        "---- "
        "-------------------------------------- "
        "---------------- "
        "------------ "
        "------ "
        "---------- "
        "-------- "
        "----------------\n");

    switch_mutex_lock(ivcore_globals.mutex);
    for (i = 0; i < MAX_IVC_CHANNELS; i++) {
        ivcore_channel_t *ch = ivcore_globals.channels[i];
        const char *uuid;
        if (!ch) continue;
        found++;
        uuid = ch->session ? switch_core_session_get_uuid(ch->session) : "N/A";
        stream->write_function(stream,
            "%-4d %-38s %-16s %-12s %-6s %-10s %-8u %-16s\n",
            i,
            uuid,
            ch->params.called_number[0] ? ch->params.called_number : "-",
            ch->params.server_ip,
            call_state_str(ch->call_state),
            codec_str(ch->active_codec),
            ch->ptime_ms,
            ch->params.username[0] ? ch->params.username : "-");
    }
    switch_mutex_unlock(ivcore_globals.mutex);

    stream->write_function(stream, "\n%d active channel%s\n\n", found, found == 1 ? "" : "s");

    stream->write_function(stream, "Cards loaded: %d\n", ivcore_globals.card_count);
    for (i = 0; i < ivcore_globals.card_count; i++) {
        const ivcore_card_t *c = &ivcore_globals.cards[i];
        stream->write_function(stream,
            "  [%d] %-20s  %s:%d  codec=%c  ptime=%d  context=%s  ports=%d\n",
            i, c->name, c->server_ip, c->tcp_port,
            c->codec, c->ptime_ms, c->context, c->port_count);
    }
}

/* ivc list xml  — XML fragment */
static void cmd_list_xml(switch_stream_handle_t *stream)
{
    int i;

    stream->write_function(stream, "<ivcore>\n");

    stream->write_function(stream, "  <channels count=\"%d\">\n", ivcore_globals.channel_count);
    switch_mutex_lock(ivcore_globals.mutex);
    for (i = 0; i < MAX_IVC_CHANNELS; i++) {
        ivcore_channel_t *ch = ivcore_globals.channels[i];
        const char *uuid;
        if (!ch) continue;
        uuid = ch->session ? switch_core_session_get_uuid(ch->session) : "";
        stream->write_function(stream,
            "    <channel index=\"%d\" uuid=\"%s\" dest=\"%s\" server=\"%s\""
            " state=\"%s\" codec=\"%s\" ptime=\"%u\" ivp-user=\"%s\"/>\n",
            i, uuid,
            ch->params.called_number,
            ch->params.server_ip,
            call_state_str(ch->call_state),
            codec_str(ch->active_codec),
            ch->ptime_ms,
            ch->params.username);
    }
    switch_mutex_unlock(ivcore_globals.mutex);
    stream->write_function(stream, "  </channels>\n");

    stream->write_function(stream, "  <cards count=\"%d\">\n", ivcore_globals.card_count);
    for (i = 0; i < ivcore_globals.card_count; i++) {
        const ivcore_card_t *c = &ivcore_globals.cards[i];
        int j;
        stream->write_function(stream,
            "    <card index=\"%d\" name=\"%s\" server=\"%s\" tcp-port=\"%d\""
            " udp-port=\"%d\" codec=\"%c\" ptime=\"%d\" context=\"%s\" ports=\"%d\">\n",
            i, c->name, c->server_ip, c->tcp_port,
            c->udp_port, c->codec, c->ptime_ms, c->context, c->port_count);
        for (j = 0; j < c->port_count; j++) {
            stream->write_function(stream,
                "      <port index=\"%d\" name=\"%s\" username=\"%s\" type=\"%s\"/>\n",
                j, c->ports[j].name, c->ports[j].username, c->ports[j].device_type);
        }
        stream->write_function(stream, "    </card>\n");
    }
    stream->write_function(stream, "  </cards>\n");

    stream->write_function(stream, "</ivcore>\n");
}

/* =====================================================================
 * SWITCH_STANDARD_API: ivc
 *
 *   ivc list           -- plain-text channel + card table
 *   ivc list xml       -- XML fragment
 *   ivc rescan         -- reload ivcore.conf.xml (only when no calls active)
 *   ivc help           -- usage
 * ===================================================================*/

static const char *ivc_api_usage =
    "USAGE:\n"
    "-----------------------------------------------------------------------\n"
    "ivc list             List active channels and loaded cards (text)\n"
    "ivc list xml         Same as above, as an XML fragment\n"
    "ivc rescan           Reload ivcore.conf.xml (requires no active calls)\n"
    "ivc debug on|off     Enable/disable per-frame HDLC/DPI/transport traces\n"
    "ivc help             Show this message\n"
    "-----------------------------------------------------------------------\n";

SWITCH_STANDARD_API(ivc_cmd)
{
    char *mycmd = NULL;
    char *argv[8] = {0};
    int argc = 0;

    if (zstr(cmd)) {
        stream->write_function(stream, "%s", ivc_api_usage);
        return SWITCH_STATUS_SUCCESS;
    }

    mycmd = strdup(cmd);
    argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));

    if (!argc || !argv[0]) {
        stream->write_function(stream, "%s", ivc_api_usage);
        goto done;
    }

    if (!strcasecmp(argv[0], "help")) {
        stream->write_function(stream, "%s", ivc_api_usage);

    } else if (!strcasecmp(argv[0], "list")) {
        if (argv[1] && !strcasecmp(argv[1], "xml")) {
            cmd_list_xml(stream);
        } else {
            cmd_list(stream);
        }

    } else if (!strcasecmp(argv[0], "rescan")) {
        if (ivcore_globals.channel_count > 0) {
            stream->write_function(stream,
                "-ERR Cannot rescan while %d channel%s active\n",
                ivcore_globals.channel_count,
                ivcore_globals.channel_count == 1 ? " is" : "s are");
        } else {
            switch_mutex_lock(ivcore_globals.mutex);
            memset(ivcore_globals.cards, 0, sizeof(ivcore_globals.cards));
            ivcore_globals.card_count = 0;
            switch_mutex_unlock(ivcore_globals.mutex);
            load_config();
            stream->write_function(stream, "+OK %d card%s loaded\n",
                ivcore_globals.card_count,
                ivcore_globals.card_count == 1 ? "" : "s");
        }

    } else if (!strcasecmp(argv[0], "debug")) {
        if (!argv[1]) {
            stream->write_function(stream, "ivc debug is currently %s\nUsage: ivc debug on|off\n",
                ivcore_globals.debug == SWITCH_TRUE ? "ON" : "OFF");
        } else if (!strcasecmp(argv[1], "on")) {
            ivcore_globals.debug = SWITCH_TRUE;
            stream->write_function(stream, "+OK IVC debug logging enabled\n");
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                "mod_ivcore: per-frame debug logging ENABLED\n");
        } else if (!strcasecmp(argv[1], "off")) {
            ivcore_globals.debug = SWITCH_FALSE;
            stream->write_function(stream, "+OK IVC debug logging disabled\n");
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                "mod_ivcore: per-frame debug logging DISABLED\n");
        } else {
            stream->write_function(stream, "-ERR Usage: ivc debug on|off\n");
        }

    } else {
        stream->write_function(stream, "-ERR Unknown command [%s]\n\n%s", argv[0], ivc_api_usage);
    }

done:
    switch_safe_free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

/* =====================================================================
 * Autoconnect session spawner (shared by module load and post-call respawn)
 * ===================================================================*/

#define IVP_AUTOCONNECT_RETRY_US  10000000  /* 10 seconds */

typedef struct {
    int ci;
    int pi;
} autoconnect_retry_args_t;

static void *autoconnect_retry_thread(switch_thread_t *thread, void *obj)
{
    autoconnect_retry_args_t *args = (autoconnect_retry_args_t *)obj;
    int ci = args->ci;
    int pi = args->pi;
    (void)thread;
    free(args);

    while (ivcore_globals.running == SWITCH_TRUE) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
            "mod_ivcore: autoconnect retry in 10 s (card=%d port=%d)\n", ci, pi);
        switch_yield(IVP_AUTOCONNECT_RETRY_US);
        if (ivcore_globals.running == SWITCH_FALSE) break;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
            "mod_ivcore: retrying autoconnect (card=%d port=%d)\n", ci, pi);
        spawn_autoconnect_session(ci, pi);
        break; /* spawn_autoconnect_session will start another retry if it fails */
    }
    return NULL;
}

static void schedule_autoconnect_retry(int ci, int pi)
{
    autoconnect_retry_args_t *args;
    switch_thread_t    *t;
    switch_threadattr_t *tattr;

    if (ivcore_globals.running == SWITCH_FALSE) return;

    args = (autoconnect_retry_args_t *)malloc(sizeof(*args));
    if (!args) return;
    args->ci = ci;
    args->pi = pi;

    switch_threadattr_create(&tattr, ivcore_globals.pool);
    switch_threadattr_detach_set(tattr, 1);
    switch_thread_create(&t, tattr, autoconnect_retry_thread, args,
                         ivcore_globals.pool);
}

static void spawn_autoconnect_session(int ci, int pi)
{
    ivcore_card_t  *c = &ivcore_globals.cards[ci];
    ivcore_port_t  *p = &c->ports[pi];
    switch_core_session_t *ac_session;
    ivcore_channel_t *ch;
    int slot;

    if (!(ac_session = switch_core_session_request(
            ivcore_globals.endpoint_interface,
            SWITCH_CALL_DIRECTION_OUTBOUND,
            SOF_NONE, NULL))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "mod_ivcore: autoconnect card='%s' port='%s' - session request failed\n",
            c->name, p->name);
        return;
    }

    ch = ivcore_channel_alloc(ac_session, c, p);
    if (!ch) {
        switch_core_session_destroy(&ac_session);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "mod_ivcore: autoconnect card='%s' port='%s' - channel alloc failed\n",
            c->name, p->name);
        return;
    }

    switch_core_session_set_private(ac_session, ch);
    switch_copy_string(ch->params.called_number, p->autoconnect_number,
                       sizeof(ch->params.called_number));

    ch->is_autoconnect       = SWITCH_TRUE;
    ch->autoconnect_card_idx = ci;
    ch->autoconnect_port_idx = pi;

    switch_channel_set_name(ch->channel, "ivcore-autoconnect");
    switch_channel_set_flag(ch->channel, CF_AUDIO);

    {
        switch_caller_profile_t *cp =
            switch_caller_profile_new(
                switch_core_session_get_pool(ac_session),
                "ivcore",
                "XML",
                p->username,
                p->username,
                NULL, NULL, NULL, NULL,
                "mod_ivcore",
                c->context[0] ? c->context : "default",
                "ivcore"
            );
        switch_channel_set_caller_profile(ch->channel, cp);
    }

    if (ivcore_setup_codecs(ac_session, ch) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "mod_ivcore: autoconnect card='%s' port='%s' - codec init failed\n",
            c->name, p->name);
        ivcore_channel_free(ch);
        switch_core_session_destroy(&ac_session);
        return;
    }

    switch_mutex_lock(ivcore_globals.mutex);
    for (slot = 0; slot < MAX_IVC_CHANNELS; slot++) {
        if (!ivcore_globals.channels[slot]) {
            ivcore_globals.channels[slot] = ch;
            ivcore_globals.channel_count++;
            break;
        }
    }
    switch_mutex_unlock(ivcore_globals.mutex);

    if (ivp_udp_open(ch)  != SWITCH_STATUS_SUCCESS ||
        ivp_tcp_login(ch) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "mod_ivcore: autoconnect card='%s' port='%s' - connect failed, will retry\n",
            c->name, p->name);
        ivcore_channel_free(ch);
        switch_core_session_destroy(&ac_session);
        schedule_autoconnect_retry(ci, pi);
        return;
    }

    ch->running = SWITCH_TRUE;
    {
        switch_threadattr_t *tattr;
        switch_threadattr_create(&tattr, switch_core_session_get_pool(ac_session));
        switch_threadattr_detach_set(tattr, 0);
        switch_thread_create(&ch->rx_thread, tattr, ivp_recv_loop, ch,
                             switch_core_session_get_pool(ac_session));
    }

    if (ivp_send_new(ch) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "mod_ivcore: autoconnect card='%s' port='%s' - send NEW failed, will retry\n",
            c->name, p->name);
        ch->running = SWITCH_FALSE;
        ivcore_channel_free(ch);
        switch_core_session_destroy(&ac_session);
        schedule_autoconnect_retry(ci, pi);
        return;
    }

    switch_channel_mark_pre_answered(ch->channel);
    switch_channel_set_state(ch->channel, CS_INIT);
    switch_core_session_thread_launch(ac_session);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
        "mod_ivcore: autoconnect card='%s' port='%s' connected to %s:%d\n",
        c->name, p->name, c->server_ip, c->tcp_port);
}

/* =====================================================================
 * Module load / shutdown
 * ===================================================================*/

SWITCH_MODULE_LOAD_FUNCTION(mod_ivcore_load)
{
	switch_endpoint_interface_t *endpoint_interface;

	memset(&ivcore_globals, 0, sizeof(ivcore_globals));
	ivcore_globals.pool = pool;

	switch_mutex_init(&ivcore_globals.mutex, SWITCH_MUTEX_NESTED, pool);
	ivcore_globals.running = SWITCH_TRUE;

	load_config();

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	endpoint_interface = (switch_endpoint_interface_t *)
		switch_loadable_module_create_interface(*module_interface,
												SWITCH_ENDPOINT_INTERFACE);
	endpoint_interface->interface_name = "ivcore";
	endpoint_interface->io_routines    = &channel_io_routines;
	endpoint_interface->state_handler  = &channel_event_handlers;

	ivcore_globals.endpoint_interface = endpoint_interface;

	{
		switch_api_interface_t *api_interface;
		SWITCH_ADD_API(api_interface, "ivc",
					   "IVCore control: ivc [list [xml]|rescan|help]",
					   ivc_cmd, "[list [xml]|rescan|help]");
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore loaded - IVC/IVP endpoint ready\n");

	/* ------------------------------------------------------------------
	 * Autoconnect: establish TCP/UDP for every port flagged autoconnect.
	 * We do this after the module interface is fully registered so that
	 * session_request() can succeed.
	 * ------------------------------------------------------------------ */
	{
		int ci, pi;
		for (ci = 0; ci < ivcore_globals.card_count; ci++) {
			ivcore_card_t *c = &ivcore_globals.cards[ci];
			for (pi = 0; pi < c->port_count; pi++) {
				if (!c->ports[pi].autoconnect) continue;
				spawn_autoconnect_session(ci, pi);
			}
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ivcore_shutdown)
{
	int i;

	ivcore_globals.running = SWITCH_FALSE;

	switch_mutex_lock(ivcore_globals.mutex);
	for (i = 0; i < MAX_IVC_CHANNELS; i++) {
		if (ivcore_globals.channels[i]) {
			ivcore_globals.channels[i]->running = SWITCH_FALSE;
		}
	}
	switch_mutex_unlock(ivcore_globals.mutex);

	switch_sleep(200000); /* 200 ms — give receive threads time to exit */

	switch_mutex_destroy(ivcore_globals.mutex);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore unloaded\n");

	return SWITCH_STATUS_SUCCESS;
}
