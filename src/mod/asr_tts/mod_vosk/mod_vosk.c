/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2013, Anthony Minessale II <anthm@freeswitch.org>
 *
 * mod_vosk - Speech recognition using Vosk server (Windows x64 port)
 *
 * High-Accuracy FIR Filter Version:
 * Natively downsamples 48kHz audio streams down to 16kHz using a 13-tap
 * windowed-sinc anti-aliasing filter before passing data to the ASR engine.
 *
 *  WebSocket handshake + config JSON moved from vosk_asr_feed into
 *            vosk_asr_open so that no audio frames are lost and there is no
 *            race between the config send and the first binary frame.
 *  Resampling path uses dynamic sample counts derived from `len`
 *            rather than the hardcoded 1920/960/320/640 constants, so any
 *            ptime (10ms, 20ms, 30ms) is handled correctly.
 *  FIR filter boundary uses zero-padding instead of clamping to
 *            index 0, eliminating the DC transient at the start of each frame.
 *  vosk_asr_check_results no longer signals SUCCESS on partial
 *            results; only a non-empty final {"text":"..."} result returns
 *            SUCCESS, preventing premature termination of the listen window.
 */

#define __PRETTY_FUNCTION__ __func__
#include <switch.h>
#ifndef _MSC_VER
#include <netinet/tcp.h>
#endif

#ifdef _MSC_VER
#pragma comment(lib, "C:/Development/GitHub/GERRAudio/SIPServer/x64/Release/FreeSwitchCore.lib")
#endif

#define WS_OP_CONTINUATION 0x00
#define WS_OP_TEXT 0x01
#define WS_OP_BINARY 0x02
#define WS_OP_CLOSE 0x08
#define WS_OP_PING 0x09
#define WS_OP_PONG 0x0A

SWITCH_MODULE_LOAD_FUNCTION(mod_vosk_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vosk_shutdown);
SWITCH_MODULE_DEFINITION(mod_vosk, mod_vosk_load, mod_vosk_shutdown, NULL);

static switch_mutex_t *MUTEX = NULL;
static switch_event_node_t *NODE = NULL;


static struct {
	char *server_url;
	char *server_ip;
	switch_port_t server_port;
	int return_json;
	int auto_reload;
	int endpoint_silence_ms;	 
	int speech_complete_timeout;
	char *recognition_words; 
	float min_confidence;
	int vad_mode;

	switch_memory_pool_t *pool;
} globals;

typedef struct {
	switch_socket_t *ws;
	char *result;
	switch_mutex_t *mutex;
	int sample_rate;
	int handshake_done;
	int16_t *out_buffer;	   /* Permanent pre-allocated buffer */
	switch_size_t out_max_len; /* Max sample capacity tracker */
	// Added for local endpointing silence detection
	char *last_partial;		   //Cache to store the previous partial phrase text 
	switch_time_t last_partial_time; // 64-bit integer tracking the microsecond timestamp */
} vosk_t;



/* -------------------------------------------------------------------------
 * WebSocket frame helpers
 * ---------------------------------------------------------------------- */

static switch_status_t ws_send_binary(switch_socket_t *sock, const char *payload, switch_size_t payload_len)
{
	unsigned char hdr[8];
	unsigned char mask[4];
	char stack_masked[2048]; /* OPTIMIZATION: Use stack memory instead of heap malloc */
	char *masked = stack_masked;
	switch_size_t hlen, slen;

	if (!sock || !payload || payload_len == 0) return SWITCH_STATUS_FALSE;

	/* If a frame somehow exceeds our 2048 safe stack buffer, fallback safely to heap */
	if (payload_len > sizeof(stack_masked)) {
		masked = (char *)malloc(payload_len);
		if (!masked) return SWITCH_STATUS_MEMERR;
	}

	mask[0] = (unsigned char)(rand() & 0xFF);
	mask[1] = (unsigned char)(rand() & 0xFF);
	mask[2] = (unsigned char)(rand() & 0xFF);
	mask[3] = (unsigned char)(rand() & 0xFF);

	hdr[0] = 0x82; /* FIN + opcode BINARY */
	hdr[1] = 0xFE; /* MASK bit + 126 */
	hdr[2] = (unsigned char)((payload_len >> 8) & 0xFF);
	hdr[3] = (unsigned char)(payload_len & 0xFF);
	hdr[4] = mask[0];
	hdr[5] = mask[1];
	hdr[6] = mask[2];
	hdr[7] = mask[3];

	for (switch_size_t i = 0; i < payload_len; i++) masked[i] = payload[i] ^ mask[i % 4];

	hlen = 8;
	if (switch_socket_send(sock, (char *)hdr, &hlen) != SWITCH_STATUS_SUCCESS) {
		if (masked != stack_masked) free(masked);
		return SWITCH_STATUS_GENERR;
	}

	slen = payload_len;
	if (switch_socket_send(sock, masked, &slen) != SWITCH_STATUS_SUCCESS) {
		if (masked != stack_masked) free(masked);
		return SWITCH_STATUS_GENERR;
	}

	if (masked != stack_masked) free(masked);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t ws_send_text(switch_socket_t *sock, const char *text)
{
	switch_size_t tlen = strlen(text);
	unsigned char hdr[8];
	unsigned char mask[4];
	char *masked = NULL;
	switch_size_t hlen, slen;

	mask[0] = (unsigned char)(rand() & 0xFF);
	mask[1] = (unsigned char)(rand() & 0xFF);
	mask[2] = (unsigned char)(rand() & 0xFF);
	mask[3] = (unsigned char)(rand() & 0xFF);

	hdr[0] = 0x81; /* FIN + opcode TEXT */
	if (tlen <= 125) {
		hdr[1] = (unsigned char)(0x80 | tlen);
		hdr[2] = mask[0];
		hdr[3] = mask[1];
		hdr[4] = mask[2];
		hdr[5] = mask[3];
		hlen = 6;
	} else {
		hdr[1] = 0xFE;
		hdr[2] = (unsigned char)((tlen >> 8) & 0xFF);
		hdr[3] = (unsigned char)(tlen & 0xFF);
		hdr[4] = mask[0];
		hdr[5] = mask[1];
		hdr[6] = mask[2];
		hdr[7] = mask[3];
		hlen = 8;
	}

	masked = (char *)malloc(tlen);
	if (!masked) return SWITCH_STATUS_MEMERR;
	for (switch_size_t i = 0; i < tlen; i++) masked[i] = text[i] ^ mask[i % 4];

	slen = tlen;
	if (switch_socket_send(sock, (char *)hdr, &hlen) != SWITCH_STATUS_SUCCESS ||
		switch_socket_send(sock, masked, &slen) != SWITCH_STATUS_SUCCESS) {
		free(masked);
		return SWITCH_STATUS_GENERR;
	}
	free(masked);
	return SWITCH_STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * vosk_asr_open
 *
 * WebSocket upgrade handshake and Vosk config JSON are sent here,
 * synchronously, before any audio frames can arrive.  This guarantees:
 *   a) No audio frame is lost to the handshake branch.
 *   b) The server has processed the sample_rate config before the first
 *      binary frame arrives.
 * ---------------------------------------------------------------------- */
static switch_status_t vosk_asr_open(switch_asr_handle_t *ah, const char *codec, int rate, const char *dest,
									 switch_asr_flag_t *flags)
{
	vosk_t *vosk = NULL;
	switch_socket_t *socket = NULL;
	switch_sockaddr_t *sa = NULL;

	if (!(vosk = (vosk_t *)switch_core_alloc(ah->memory_pool, sizeof(*vosk)))) { return SWITCH_STATUS_MEMERR; }
	ah->private_info = vosk;

	switch_mutex_init(&vosk->mutex, SWITCH_MUTEX_NESTED, ah->memory_pool);
	vosk->handshake_done = 0;
	vosk->result = NULL;

	vosk->last_partial = NULL;	 /* Initialize to safe NULL pointer state */
	vosk->last_partial_time = 0; /* Clear the stopwatch state to 0 */


	ah->rate = 48000;
	vosk->sample_rate = 48000;
	ah->codec = switch_core_strdup(ah->memory_pool, "L16");
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
					  "mod_vosk: opened at %dHz (will downsample to 16kHz via FIR)\n", ah->rate);
	/* --- TCP connect ---------------------------------------------------- */
	if (switch_sockaddr_info_get(&sa, globals.server_ip, SWITCH_UNSPEC, globals.server_port, 0, globals.pool) !=
		SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk: sockaddr_info_get failed for %s:%d\n",
						  globals.server_ip, globals.server_port);
		return SWITCH_STATUS_GENERR;
	}

	if (switch_socket_create(&socket, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, globals.pool) !=
		SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk: socket_create failed\n");
		return SWITCH_STATUS_GENERR;
	}

	switch_socket_timeout_set(socket, 5000000); /* 5 s connect timeout */
	if (switch_socket_connect(socket, sa) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk: connect to %s:%d failed\n",
						  globals.server_ip, globals.server_port);
		switch_socket_close(socket);
		return SWITCH_STATUS_GENERR;
	}

	/* Try to disable Nagle's algo for speed*/
	if (switch_socket_opt_set(socket, SWITCH_SO_TCP_NODELAY, 1) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "mod_vosk: Could not set TCP_NODELAY\n");
	}

	/* --- WebSocket HTTP upgrade ----------------------------------------- */
	{
		const char *hs = "GET / HTTP/1.1\r\n"
						 "Host: 127.0.0.1:2700\r\n"
						 "Upgrade: websocket\r\n"
						 "Connection: Upgrade\r\n"
						 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
						 "Sec-WebSocket-Version: 13\r\n"
						 "\r\n";
		switch_size_t hs_len = strlen(hs);

		if (switch_socket_send(socket, hs, &hs_len) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk: failed to send WS upgrade request\n");
			switch_socket_close(socket);
			return SWITCH_STATUS_GENERR;
		}

		/* Read until we have the complete HTTP response header */
		switch_socket_timeout_set(socket, 2000000); /* 2 s */
		char resp[4096] = {0};
		int total = 0;
		while (total < (int)sizeof(resp) - 1) {
			switch_size_t chunk = sizeof(resp) - total - 1;
			switch_status_t rs = switch_socket_recv(socket, resp + total, &chunk);
			if (rs != SWITCH_STATUS_SUCCESS || chunk == 0) break;
			total += (int)chunk;
			resp[total] = '\0';
			if (strstr(resp, "\r\n\r\n")) break;
		}
		switch_socket_timeout_set(socket, 0); /* restore non-blocking */

		if (!strstr(resp, "101")) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "mod_vosk: WS upgrade failed (no 101). Response:\n%s\n", resp);
			switch_socket_close(socket);
			return SWITCH_STATUS_GENERR;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_vosk: WS upgrade OK\n");
	}

	/* --- Send Vosk configuration JSON ------------------------------------ */
	{
		/* Dynamically format the JSON using  custom XML configuration array */
		char *cfg_json = switch_mprintf("{\"config\":{\"sample_rate\":16000,\"min_confidence\":%f,\"vad_mode\":%d,"
										"\"speech_complete_timeout\":%d,\"words\":[%s]}}",
										globals.min_confidence, globals.vad_mode, globals.speech_complete_timeout,
										globals.recognition_words);
		if (!cfg_json) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "mod_vosk: failed to allocate config JSON memory\n");
			switch_socket_close(socket);
			return SWITCH_STATUS_MEMERR;
		}

		if (ws_send_text(socket, cfg_json) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk: failed to send config JSON\n");
			switch_safe_free(cfg_json);
			switch_socket_close(socket);
			return SWITCH_STATUS_GENERR;
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_vosk: config JSON sent: %s\n", cfg_json);
		switch_safe_free(cfg_json); /* Safely release heap memory before processing audio frames */
	}




	vosk->out_max_len = 16000 * sizeof(int16_t); /* Safe 1-second headroom allocation */
	vosk->out_buffer = (int16_t *)switch_core_alloc(ah->memory_pool, vosk->out_max_len);

	vosk->ws = socket;
	vosk->handshake_done = 1;
	return SWITCH_STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * vosk_asr_close
 * ---------------------------------------------------------------------- */
static switch_status_t vosk_asr_close(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;

	switch_mutex_lock(vosk->mutex);
	if (vosk->ws) {
		if (vosk->handshake_done) {
			/* RFC 6455 close frame, masked, no payload */
			unsigned char close_hdr[6] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
			switch_size_t cl = 6;
			switch_socket_send(vosk->ws, (char *)close_hdr, &cl);
		}
		switch_socket_close(vosk->ws);
		vosk->ws = NULL;
	}

	switch_set_flag(ah, SWITCH_ASR_FLAG_CLOSED);
	switch_safe_free(vosk->result);

	// local endpointing mem cleanup
	switch_safe_free(vosk->last_partial); /* Safely releases the string memory from heap */

	switch_mutex_unlock(vosk->mutex);
	return SWITCH_STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * vosk_asr_feed
 *
 * Dynamic frame sizing — sample counts are derived from `len` so that
 *         10ms, 20ms, and 30ms ptimes all produce correctly sized output.
 *         A frame is treated as 48kHz when its byte count is exactly 3x a
 *         16kHz frame at the same sample width (i.e. len % 6 == 0 &&
 *         len > 640).  All other frames are passed through as 16kHz native.
 *
 * FIR boundary uses zero-padding (out-of-range input_idx → 0.0f)
 *         instead of clamping to index 0, removing the DC transient at the
 *         leading edge of each frame.
 * ---------------------------------------------------------------------- */
static switch_status_t vosk_asr_feed(switch_asr_handle_t *ah, void *data, unsigned int len, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;
	switch_pollfd_t pollfd = {0};
	int32_t num_fds = 0;

	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) return SWITCH_STATUS_BREAK;
	if (!vosk->handshake_done) return SWITCH_STATUS_BREAK;

	switch_mutex_lock(vosk->mutex);

	/* ---- Audio → Vosk -------------------------------------------------- */
	if (len > 0) {
		/*
		 * Detect a 48kHz frame: byte count is an exact 3:1 multiple of a
		 * 16kHz frame.  For 20ms: 48k→1920 bytes, 16k→640 bytes.
		 * Guard: len > 640 avoids misidentifying a 16kHz 20ms frame.
		 */
		int is_48k = (len > 640 && (len % 6) == 0);

		if (is_48k) {
			int in_sample_count = (int)(len / sizeof(int16_t));
			int out_sample_count = in_sample_count / 3;
			switch_size_t out_bytes = (switch_size_t)(out_sample_count * sizeof(int16_t));

			int16_t *in_samples = (int16_t *)data;

			/*  Use the pre-allocated pool buffer for speed */
			int16_t *out_samples = vosk->out_buffer;

			/* 13-tap windowed-sinc low-pass FIR, cut-off at 8kHz */
			static const float FILTER_TAPS[13] = {-0.0118f, -0.0253f, -0.0129f, 0.0573f,  0.1755f,	0.2647f, 0.3046f,
												  0.2647f,	0.1755f,  0.0573f,	-0.0129f, -0.0253f, -0.0118f};

			for (int i = 0; i < out_sample_count; i++) {
				float filtered = 0.0f;
				int center = i * 3;

				for (int tap = 0; tap < 13; tap++) {
					int idx = center + (tap - 6);
					if (idx >= 0 && idx < in_sample_count) { filtered += (float)in_samples[idx] * FILTER_TAPS[tap]; }
				}

				if (filtered > 32767.0f)
					filtered = 32767.0f;
				else if (filtered < -32768.0f)
					filtered = -32768.0f;
				out_samples[i] = (int16_t)filtered;
			}

			switch_status_t sr = ws_send_binary(vosk->ws, (const char *)out_samples, out_bytes);


			if (sr != SWITCH_STATUS_SUCCESS) {
				switch_mutex_unlock(vosk->mutex);
				return SWITCH_STATUS_BREAK;
			}
		
		} else {
			/* Native 16kHz frame — pass through unchanged */
			if (ws_send_binary(vosk->ws, (const char *)data, len) != SWITCH_STATUS_SUCCESS) {
				switch_mutex_unlock(vosk->mutex);
				return SWITCH_STATUS_BREAK;
			}
		}
	}

	/* ---- Non-blocking poll for a response frame ------------------------ */
	pollfd.desc_type = SWITCH_POLL_SOCKET;
	pollfd.desc.s = vosk->ws;
	pollfd.reqevents = SWITCH_POLLIN | SWITCH_POLLERR;
	pollfd.client_data = vosk;

	if (switch_poll(&pollfd, 1, &num_fds, 0) != SWITCH_STATUS_SUCCESS || num_fds <= 0) {
		switch_mutex_unlock(vosk->mutex);
		return SWITCH_STATUS_SUCCESS;
	}
	if (!(pollfd.rtnevents & SWITCH_POLLIN)) {
		switch_mutex_unlock(vosk->mutex);
		return SWITCH_STATUS_SUCCESS;
	}

	/* ---- Read WS frame header (2 bytes) -------------------------------- */
	char hdr[2];
	switch_size_t hdr_len = 2;
	if (switch_socket_recv(vosk->ws, hdr, &hdr_len) != SWITCH_STATUS_SUCCESS || hdr_len < 2) {
		switch_mutex_unlock(vosk->mutex);
		return SWITCH_STATUS_BREAK;
	}

	unsigned char opcode = (unsigned char)hdr[0] & 0x0F;
	unsigned char masked_flag = ((unsigned char)hdr[1] >> 7) & 0x01;
	unsigned char raw_len = (unsigned char)hdr[1] & 0x7F;
	switch_size_t payload_len = raw_len;

	if (raw_len == 126) {
		char ext[2];
		switch_size_t el = 2;
		if (switch_socket_recv(vosk->ws, ext, &el) != SWITCH_STATUS_SUCCESS) {
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_BREAK;
		}
		payload_len = ((unsigned char)ext[0] << 8) | (unsigned char)ext[1];
	}

	char mask_key[4] = {0, 0, 0, 0};
	if (masked_flag) {
		switch_size_t ml = 4;
		switch_socket_recv(vosk->ws, mask_key, &ml);
	}

	char *payload = NULL;
	if (payload_len > 0) {
		payload = (char *)switch_core_alloc(globals.pool, payload_len + 1);
		switch_size_t recv_len = payload_len;
		if (switch_socket_recv(vosk->ws, payload, &recv_len) != SWITCH_STATUS_SUCCESS) {
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_BREAK;
		}
		payload[recv_len] = '\0';
		if (masked_flag) {
			for (switch_size_t i = 0; i < recv_len; i++) payload[i] ^= mask_key[i % 4];
		}
	}

	switch (opcode) {

	case WS_OP_TEXT:
			if (payload) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_vosk: rx text frame: %s\n", payload);
				switch_safe_free(vosk->result);
				vosk->result = switch_safe_strdup(payload);
				// endpoint updates
				if (strstr(payload, "\"partial\"")) {
					/* If the phrase text has changed, reset the microsecond timer */
					if (!vosk->last_partial || strcmp(vosk->last_partial, payload) != 0) {
						switch_safe_free(vosk->last_partial);
						vosk->last_partial = switch_safe_strdup(payload);
						vosk->last_partial_time = switch_micro_time_now(); /* Stamp current OS microsecond */
					}
				}
			}
			break;

	case WS_OP_PING: {
		unsigned char pong[2] = {(unsigned char)(0x80 | WS_OP_PONG), (unsigned char)(payload_len & 0x7F)};
		switch_size_t pl = 2;
		switch_socket_send(vosk->ws, (char *)pong, &pl);
		if (payload_len > 0 && payload) {
			switch_size_t pd = payload_len;
			switch_socket_send(vosk->ws, payload, &pd);
		}
	} break;

	case WS_OP_CLOSE:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "mod_vosk: server sent WS CLOSE\n");
		switch_mutex_unlock(vosk->mutex);
		return SWITCH_STATUS_BREAK;

	default:
		break;
	}

	switch_mutex_unlock(vosk->mutex);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t vosk_asr_pause(switch_asr_handle_t *ah) { return SWITCH_STATUS_SUCCESS; }
static switch_status_t vosk_asr_resume(switch_asr_handle_t *ah) { return SWITCH_STATUS_SUCCESS; }
static switch_status_t vosk_asr_start_input_timers(switch_asr_handle_t *ah) { return SWITCH_STATUS_SUCCESS; }

static switch_status_t vosk_asr_load_grammar(switch_asr_handle_t *ah, const char *grammar, const char *name)
{
	return SWITCH_STATUS_SUCCESS;
}
static switch_status_t vosk_asr_unload_grammar(switch_asr_handle_t *ah, const char *name)
{
	return SWITCH_STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * vosk_asr_check_results
 *
 * Only signal SUCCESS (result ready for collection) on a non-empty
 * final result.  Partial results keep returning FALSE so FreeSWITCH
 * continues feeding audio rather than terminating the listen window early.
 *
 * Vosk final result:   {"text":"hello world"}
 * Vosk silent result:  {"text":""}
 * Vosk partial result: {"partial":"hello"}
 * ---------------------------------------------------------------------- */

static switch_status_t vosk_asr_check_results(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;

	if (!vosk->result) return SWITCH_STATUS_FALSE;
	if (strstr(vosk->result, "\"text\":\"\"")) return SWITCH_STATUS_FALSE;

	if (!strstr(vosk->result, "\"partial\"")) { return SWITCH_STATUS_SUCCESS; }

	if (strstr(vosk->result, "\"partial\"")) {
		if (strstr(vosk->result, "\"partial\":\"\"")) { return SWITCH_STATUS_FALSE; }

		if (vosk->last_partial_time > 0) {
			switch_time_t now = switch_micro_time_now();

			/* TARGET OPTIMIZATION: Dynamically read from the XML configuration value */
			switch_time_t silence_threshold_us = (switch_time_t)(globals.endpoint_silence_ms * 1000);

			if ((now - vosk->last_partial_time) > silence_threshold_us) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
								  "mod_vosk: Dynamic %dms endpoint triggered via XML config.\n",
								  globals.endpoint_silence_ms);

				cJSON *root = cJSON_Parse(vosk->result);
				if (root && cJSON_HasObjectItem(root, "partial")) {
					const char *partial_phrase = cJSON_GetObjectCstr(root, "partial");






					char *forced_final = switch_mprintf("{\"text\":\"%s\"}", partial_phrase);

					switch_safe_free(vosk->result);
					vosk->result = forced_final;

					cJSON_Delete(root);
					return SWITCH_STATUS_SUCCESS;
				}
				if (root) cJSON_Delete(root);
			}
		}
	}

	return SWITCH_STATUS_FALSE;
}



/* -------------------------------------------------------------------------
 * vosk_asr_get_results
 * ---------------------------------------------------------------------- */
static switch_status_t vosk_asr_get_results(switch_asr_handle_t *ah, char **xmlstr, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;
	switch_status_t ret;

	switch_mutex_lock(vosk->mutex);
	if (!vosk->result) {
		switch_mutex_unlock(vosk->mutex);
		return SWITCH_STATUS_FALSE;
	}

	if (globals.return_json) {
		*xmlstr = switch_safe_strdup(vosk->result);
		ret = strstr(vosk->result, "\"partial\"") ? SWITCH_STATUS_MORE_DATA : SWITCH_STATUS_SUCCESS;
	} else {
		cJSON *root = cJSON_Parse(vosk->result);
		if (root && cJSON_HasObjectItem(root, "text")) {
			*xmlstr = switch_safe_strdup(cJSON_GetObjectCstr(root, "text"));
			ret = SWITCH_STATUS_SUCCESS;
		} else if (root && cJSON_HasObjectItem(root, "partial")) {
			*xmlstr = switch_safe_strdup(cJSON_GetObjectCstr(root, "partial"));
			ret = SWITCH_STATUS_MORE_DATA;
		} else {
			ret = SWITCH_STATUS_GENERR;
		}
		if (root) cJSON_Delete(root);
	}

	switch_safe_free(vosk->result);
	vosk->result = NULL;

	switch_safe_free(vosk->last_partial); /* Clear tracking text cache for the next sentence */
	vosk->last_partial = NULL;
	vosk->last_partial_time = 0; /* Clear stopwatch */


	switch_mutex_unlock(vosk->mutex);
	return ret;
}

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */
static switch_status_t load_config(void)
{
	char *cf = "vosk.conf";
	switch_xml_t cfg, xml = NULL, param, settings;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "mod_vosk: vosk.conf not found, using defaults\n");
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *)switch_xml_attr_soft(param, "name");
			char *val = (char *)switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "server-url")) globals.server_url = switch_core_strdup(globals.pool, val);
			if (!strcasecmp(var, "return-json")) globals.return_json = atoi(val);
			if (!strcasecmp(var, "auto-reload")) globals.auto_reload = atoi(val);
			if (!strcasecmp(var, "recognition-words"))
				globals.recognition_words = switch_core_strdup(globals.pool, val);
			if (!strcasecmp(var, "endpoint-silence-ms")) globals.endpoint_silence_ms = atoi(val);
			if (!strcasecmp(var, "speech-complete-timeout")) globals.speech_complete_timeout = atoi(val);
			if (!strcasecmp(var, "recognition-words"))
				globals.recognition_words = switch_core_strdup(globals.pool, val);
			if (!strcasecmp(var, "speech-complete-timeout")) globals.endpoint_silence_ms = atoi(val);

			if (!strcasecmp(var, "min-confidence")) globals.min_confidence = (float)atof(val);
			if (!strcasecmp(var, "vad-mode")) globals.vad_mode = atoi(val);
			if (!strcasecmp(var, "speech-complete-timeout")) globals.speech_complete_timeout = atoi(val);

		}
	}
done:
	if (!globals.server_url) 
		globals.server_url = switch_core_strdup(globals.pool, "ws://127.0.0.1:2700");
	/* Fallback configuration if parameter is missing from the XML file */
	if (globals.endpoint_silence_ms <= 0) {
		globals.endpoint_silence_ms = 400; // High-concurrency performance fallback default
	}
	if (!globals.recognition_words) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "mod_vosk: 'recognition-words' missing from XML. Using hardcoded production defaults.\n");

		globals.recognition_words = switch_core_strdup(
			globals.pool, "\"up\", \"down\", \"louder\", \"softer\", \"volume\", \"go\", \"to\", \"go to\", "
						  "\"conference\", \"switch\", \"switch to\", "
						  "\"connect\", \"connect to\", \"connect with\", \"join\", \"in\", \"with\", \"join in\", "
						  "\"join with\", \"disconnect\", \"leave\", "
						  "\"no\", \"no conference\", \"twenty\", \"thirty\", \"forty\", \"fifty\", \"sixty\", "
						  "\"seventy\", \"eighty\", \"ninety\", "
						  "\"one\", \"two\", \"three\", \"four\", \"five\", \"six\", \"seven\", \"eight\", \"nine\", "
						  "\"27\", \"28\", \"29\", \"30\", \"31\", \"32\", \"33\", \"34\", \"35\", \"36\", \"37\", "
						  "\"38\", \"39\", \"40\", "
						  "\"41\", \"42\", \"43\", \"44\", \"45\", \"46\", \"47\", \"48\", \"49\", \"50\", \"51\", "
						  "\"52\", \"53\", \"54\", "
						  "\"55\", \"56\", \"57\", \"58\", \"59\", \"60\", \"61\", \"62\", \"63\", \"64\", \"65\", "
						  "\"66\", \"67\", \"68\", "
						  "\"69\", \"70\", \"71\", \"72\", \"73\", \"74\", \"75\", \"76\", \"77\", \"78\", \"79\", "
						  "\"80\", \"81\", \"82\", "
						  "\"83\", \"84\", \"85\", \"86\", \"87\", \"88\", \"89\", \"90\", \"91\", \"92\", \"93\", "
						  "\"94\", \"95\", \"96\", "
						  "\"97\", \"98\", \"99\", \"[unk]\"");
	}

	globals.server_ip = switch_core_strdup(globals.pool, "127.0.0.1");
	globals.server_port = 2700;
	if (xml) switch_xml_free(xml);
	return status;
}




static void do_load(void)
{
	switch_mutex_lock(MUTEX);
	load_config();
	switch_mutex_unlock(MUTEX);
}

static void event_handler(switch_event_t *event)
{
	if (globals.auto_reload) do_load();
}

/* -------------------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------------- */
SWITCH_MODULE_LOAD_FUNCTION(mod_vosk_load)
{
	switch_asr_interface_t *asr_interface;

	globals.pool = pool;
	switch_mutex_init(&MUTEX, SWITCH_MUTEX_NESTED, globals.pool);

	if (switch_event_bind_removable(modname, SWITCH_EVENT_RELOADXML, NULL, event_handler, NULL, &NODE) !=
		SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk_load: event bind failed\n");
	}

	do_load();
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	asr_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ASR_INTERFACE);
	asr_interface->interface_name = "vosk";
	asr_interface->asr_open = vosk_asr_open;
	asr_interface->asr_close = vosk_asr_close;
	asr_interface->asr_load_grammar = vosk_asr_load_grammar;
	asr_interface->asr_unload_grammar = vosk_asr_unload_grammar;
	asr_interface->asr_resume = vosk_asr_resume;
	asr_interface->asr_pause = vosk_asr_pause;
	asr_interface->asr_feed = vosk_asr_feed;
	asr_interface->asr_check_results = vosk_asr_check_results;
	asr_interface->asr_get_results = vosk_asr_get_results;
	asr_interface->asr_start_input_timers = vosk_asr_start_input_timers;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_vosk loaded. Server: %s\n", globals.server_url);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vosk_shutdown)
{
	switch_event_unbind(&NODE);
	return SWITCH_STATUS_UNLOAD;
}
