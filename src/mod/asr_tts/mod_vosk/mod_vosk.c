/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2013, Anthony Minessale II <anthm@freeswitch.org>
 *
 * mod_vosk - Speech recognition using Vosk server (Windows x64 port)
 Threaded version - 2026-11-05 - eliminates clicks - but uses a bigger buffer  not deployed
 */

static char *version_date = "2026-11-23";

#define __PRETTY_FUNCTION__ __func__
#include <switch.h>
#ifndef _MSC_VER
#include <netinet/tcp.h>
#endif

#ifdef _MSC_VER
// #pragma comment(lib, "C:/Development/GitHub/GERRAudio/SIPServer/x64/Release/FreeSwitchCore.lib")
#endif

#define WS_OP_CONTINUATION 0x00
#define WS_OP_TEXT 0x01
#define WS_OP_BINARY 0x02
#define WS_OP_CLOSE 0x08
#define WS_OP_PING 0x09
#define WS_OP_PONG 0x0A

#define WS_MAX_PAYLOAD (1024 * 1024)
#define WS_IO_TIMEOUT_US 250000

#define VOSK_IS_EAGAIN(s) ((s) == 11 || (s) == 35 || (s) == 120007 || (s) == 730035)

SWITCH_MODULE_LOAD_FUNCTION(mod_vosk_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vosk_shutdown);
SWITCH_MODULE_DEFINITION(mod_vosk, mod_vosk_load, mod_vosk_shutdown, NULL);

static switch_mutex_t *MUTEX = NULL;
static switch_event_node_t *NODE = NULL;

// configs
static struct {
	char *server_url;
	char *server_ip;
	switch_port_t server_port;
	int return_json;
	int auto_reload;
	int endpoint_silence_ms;
	/* Longer forced-final timeout while the phrase is clearly unfinished (see vosk_phrase_incomplete) */
	int endpoint_incomplete_ms;
	char *endpoint_incomplete_words; /* " w1 w2 ... " (padded with spaces, lower case) */
	int speech_complete_timeout;
	char *recognition_words;
	float min_confidence;
	int vad_mode;
	int debug_pcm;

	switch_memory_pool_t *pool;
} globals;

typedef struct {
	switch_socket_t *ws;
	char *result;
	switch_mutex_t *mutex;
	int sample_rate;
	int handshake_done;
	int16_t *out_buffer;
	switch_size_t out_max_len;
	char *last_partial;
	switch_time_t last_partial_time;

	int16_t fir_hist[12];

	int16_t remainder[2];
	int remainder_len;

	/* Despike (5-point Hampel-style) state: last 4 RAW input samples, plus counters */
	int16_t desp_hist[4];
	unsigned long long desp_replaced;
	unsigned long long desp_total;

	char *rx_buf;
	switch_size_t rx_cap;
	char *frag_buf;
	switch_size_t frag_len;
	switch_size_t frag_cap;
	int frag_active;

	switch_thread_t *read_thread;
	volatile int running;

	/* Extra grammar words supplied per session through the asr_json_param_vosk_extra_words
	 * variable ('|' separated), and whether the config JSON has been sent to the server yet */
	char *extra_words;
	int config_sent;
} vosk_t;

/* -------------------------------------------------------------------------
 * WebSocket frame helpers
 * ---------------------------------------------------------------------- */

static switch_status_t ws_send_binary(switch_socket_t *sock, const char *payload, switch_size_t payload_len)
{
	unsigned char hdr[8];
	unsigned char mask[4];
	char stack_masked[2048];
	char *masked = stack_masked;
	switch_size_t hlen, slen;

	if (!sock || !payload || payload_len == 0) return SWITCH_STATUS_FALSE;
	if (payload_len > 65535) return SWITCH_STATUS_FALSE;

	if (payload_len > sizeof(stack_masked)) {
		masked = (char *)malloc(payload_len);
		if (!masked) return SWITCH_STATUS_MEMERR;
	}

	mask[0] = (unsigned char)(rand() & 0xFF);
	mask[1] = (unsigned char)(rand() & 0xFF);
	mask[2] = (unsigned char)(rand() & 0xFF);
	mask[3] = (unsigned char)(rand() & 0xFF);

	hdr[0] = 0x82;
	hdr[1] = 0xFE;
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

	if (tlen > 65535) return SWITCH_STATUS_FALSE;

	mask[0] = (unsigned char)(rand() & 0xFF);
	mask[1] = (unsigned char)(rand() & 0xFF);
	mask[2] = (unsigned char)(rand() & 0xFF);
	mask[3] = (unsigned char)(rand() & 0xFF);

	hdr[0] = 0x81;
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

static switch_status_t ws_send_pong(switch_socket_t *sock, const char *payload, switch_size_t plen)
{
	unsigned char frame[6 + 125];
	unsigned char mask[4];
	switch_size_t flen;

	if (!sock || plen > 125) return SWITCH_STATUS_FALSE;

	mask[0] = (unsigned char)(rand() & 0xFF);
	mask[1] = (unsigned char)(rand() & 0xFF);
	mask[2] = (unsigned char)(rand() & 0xFF);
	mask[3] = (unsigned char)(rand() & 0xFF);

	frame[0] = (unsigned char)(0x80 | WS_OP_PONG);
	frame[1] = (unsigned char)(0x80 | plen);
	frame[2] = mask[0];
	frame[3] = mask[1];
	frame[4] = mask[2];
	frame[5] = mask[3];
	for (switch_size_t i = 0; i < plen; i++) frame[6 + i] = (unsigned char)payload[i] ^ mask[i % 4];

	flen = 6 + plen;
	return (switch_socket_send(sock, (char *)frame, &flen) == SWITCH_STATUS_SUCCESS) ? SWITCH_STATUS_SUCCESS
																					 : SWITCH_STATUS_GENERR;
}

static switch_status_t ws_recv_exact(switch_socket_t *sock, char *buf, switch_size_t want)
{
	switch_size_t got = 0;
	switch_time_t deadline = switch_micro_time_now() + WS_IO_TIMEOUT_US;

	while (got < want) {
		switch_size_t n = want - got;
		switch_status_t st = switch_socket_recv(sock, buf + got, &n);

		if (st == SWITCH_STATUS_SUCCESS) {
			if (n == 0) return SWITCH_STATUS_GENERR;
			got += n;
			continue;
		}
		if (VOSK_IS_EAGAIN(st)) {
			if (switch_micro_time_now() > deadline) return SWITCH_STATUS_TIMEOUT;
			switch_sleep(1000);
			continue;
		}
		return SWITCH_STATUS_GENERR;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t vosk_buf_reserve(char **buf, switch_size_t *cap, switch_size_t need)
{
	char *p;
	switch_size_t ncap;

	if (need <= *cap) return SWITCH_STATUS_SUCCESS;

	ncap = *cap ? *cap : 1024;
	while (ncap < need) ncap *= 2;

	p = (char *)realloc(*buf, ncap);
	if (!p) return SWITCH_STATUS_MEMERR;

	*buf = p;
	*cap = ncap;
	return SWITCH_STATUS_SUCCESS;
}

static void vosk_handle_text(vosk_t *vosk, const char *text)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_vosk: rx text frame: %s\n", text);
	switch_safe_free(vosk->result);
	vosk->result = switch_safe_strdup(text);
	if (strstr(text, "\"partial\"")) {
		if (!vosk->last_partial || strcmp(vosk->last_partial, text) != 0) {
			switch_safe_free(vosk->last_partial);
			vosk->last_partial = switch_safe_strdup(text);
			vosk->last_partial_time = switch_micro_time_now();
		}
	}
}

/* -------------------------------------------------------------------------
 * Background Read Thread
 * ---------------------------------------------------------------------- */
static void *SWITCH_THREAD_FUNC vosk_read_thread(switch_thread_t *thread, void *obj)
{
	vosk_t *vosk = (vosk_t *)obj;
	switch_pollfd_t pollfd = {0};
	int32_t num_fds = 0;
	unsigned char hdr[2];
	unsigned char fin, opcode, masked_flag;
	unsigned char mask_key[4] = {0, 0, 0, 0};
	uint64_t payload_len;
	switch_size_t n;

	pollfd.desc_type = SWITCH_POLL_SOCKET;
	pollfd.desc.s = vosk->ws;
	pollfd.reqevents = SWITCH_POLLIN | SWITCH_POLLERR;
	pollfd.client_data = vosk;

	while (vosk->running && vosk->ws) {
		/* Poll with a 50ms timeout. If nothing arrives, loop and check running flag again. */
		if (switch_poll(&pollfd, 1, &num_fds, 50000) != SWITCH_STATUS_SUCCESS || num_fds <= 0) { continue; }
		if (!(pollfd.rtnevents & SWITCH_POLLIN)) { continue; }

		if (ws_recv_exact(vosk->ws, (char *)hdr, 2) != SWITCH_STATUS_SUCCESS) break;

		fin = (hdr[0] >> 7) & 0x01;
		opcode = hdr[0] & 0x0F;
		masked_flag = (hdr[1] >> 7) & 0x01;
		payload_len = hdr[1] & 0x7F;

		if (payload_len == 126) {
			unsigned char ext[2];
			if (ws_recv_exact(vosk->ws, (char *)ext, 2) != SWITCH_STATUS_SUCCESS) break;
			payload_len = ((uint64_t)ext[0] << 8) | ext[1];
		} else if (payload_len == 127) {
			unsigned char ext[8];
			if (ws_recv_exact(vosk->ws, (char *)ext, 8) != SWITCH_STATUS_SUCCESS) break;
			payload_len = 0;
			for (int b = 0; b < 8; b++) payload_len = (payload_len << 8) | ext[b];
		}

		if (payload_len > WS_MAX_PAYLOAD || (opcode >= 0x08 && payload_len > 125)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_vosk: implausible WS frame, ending recognition\n");
			break;
		}

		if (masked_flag && ws_recv_exact(vosk->ws, (char *)mask_key, 4) != SWITCH_STATUS_SUCCESS) break;

		n = (switch_size_t)payload_len;
		if (vosk_buf_reserve(&vosk->rx_buf, &vosk->rx_cap, n + 1) != SWITCH_STATUS_SUCCESS) break;

		if (n > 0) {
			if (ws_recv_exact(vosk->ws, vosk->rx_buf, n) != SWITCH_STATUS_SUCCESS) break;
			if (masked_flag) {
				for (switch_size_t i = 0; i < n; i++) vosk->rx_buf[i] ^= mask_key[i % 4];
			}
		}
		vosk->rx_buf[n] = '\0';

		switch (opcode) {
		case WS_OP_TEXT:
		case WS_OP_CONTINUATION:
			if (opcode == WS_OP_TEXT) {
				vosk->frag_len = 0;
				vosk->frag_active = 0;
				if (fin) {
					if (n > 0) {
						/* Lock mutex ONLY when updating shared strings */
						switch_mutex_lock(vosk->mutex);
						vosk_handle_text(vosk, vosk->rx_buf);
						switch_mutex_unlock(vosk->mutex);
					}
					break;
				}
				vosk->frag_active = 1;
			} else if (!vosk->frag_active) {
				break;
			}

			if (vosk->frag_len + n > WS_MAX_PAYLOAD ||
				vosk_buf_reserve(&vosk->frag_buf, &vosk->frag_cap, vosk->frag_len + n + 1) != SWITCH_STATUS_SUCCESS) {
				vosk->frag_len = 0;
				vosk->frag_active = 0;
				break;
			}
			memcpy(vosk->frag_buf + vosk->frag_len, vosk->rx_buf, n);
			vosk->frag_len += n;

			if (fin) {
				vosk->frag_buf[vosk->frag_len] = '\0';
				if (vosk->frag_len > 0) {
					switch_mutex_lock(vosk->mutex);
					vosk_handle_text(vosk, vosk->frag_buf);
					switch_mutex_unlock(vosk->mutex);
				}
				vosk->frag_len = 0;
				vosk->frag_active = 0;
			}
			break;

		case WS_OP_PING:
			/* Sending PONG is fast enough to do without locking, ws_send_pong is self-contained */
			ws_send_pong(vosk->ws, vosk->rx_buf, n);
			break;

		case WS_OP_CLOSE:
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "mod_vosk: server sent WS CLOSE\n");
			vosk->running = 0;
			break;

		default:
			break;
		}
	}

	return NULL;
}

/* -------------------------------------------------------------------------
 * vosk_asr_open
 * ---------------------------------------------------------------------- */
static switch_status_t vosk_asr_open(switch_asr_handle_t *ah, const char *codec, int rate, const char *dest,
									 switch_asr_flag_t *flags)
{
	vosk_t *vosk = NULL;
	switch_socket_t *socket = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_threadattr_t *thd_attr = NULL;

	if (!(vosk = (vosk_t *)switch_core_alloc(ah->memory_pool, sizeof(*vosk)))) { return SWITCH_STATUS_MEMERR; }
	ah->private_info = vosk;

	switch_mutex_init(&vosk->mutex, SWITCH_MUTEX_NESTED, ah->memory_pool);
	vosk->handshake_done = 0;
	vosk->result = NULL;
	vosk->read_thread = NULL;
	vosk->running = 0;

	vosk->last_partial = NULL;
	vosk->last_partial_time = 0;

	vosk->rx_buf = NULL;
	vosk->rx_cap = 0;
	vosk->frag_buf = NULL;
	vosk->frag_len = 0;
	vosk->frag_cap = 0;
	vosk->frag_active = 0;
	memset(vosk->fir_hist, 0, sizeof(vosk->fir_hist));

	vosk->extra_words = NULL;
	vosk->config_sent = 0;

	vosk->remainder_len = 0;
	memset(vosk->desp_hist, 0, sizeof(vosk->desp_hist));
	vosk->desp_replaced = 0;
	vosk->desp_total = 0;

	ah->rate = 48000;
	vosk->sample_rate = 48000;
	ah->codec = switch_core_strdup(ah->memory_pool, "L16");
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
					  "mod_vosk: opened at %dHz (will downsample to 16kHz via FIR)\n", ah->rate);

	if (switch_sockaddr_info_get(&sa, globals.server_ip, SWITCH_UNSPEC, globals.server_port, 0, ah->memory_pool) !=
		SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk: sockaddr_info_get failed for %s:%d\n",
						  globals.server_ip, globals.server_port);
		return SWITCH_STATUS_GENERR;
	}

	if (switch_socket_create(&socket, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, ah->memory_pool) !=
		SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk: socket_create failed\n");
		return SWITCH_STATUS_GENERR;
	}

	switch_socket_timeout_set(socket, 5000000);
	if (switch_socket_connect(socket, sa) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk: connect to %s:%d failed\n",
						  globals.server_ip, globals.server_port);
		switch_socket_close(socket);
		return SWITCH_STATUS_GENERR;
	}

	if (switch_socket_opt_set(socket, SWITCH_SO_TCP_NODELAY, 1) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "mod_vosk: Could not set TCP_NODELAY\n");
	}

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

		switch_socket_timeout_set(socket, 2000000);
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
		switch_socket_timeout_set(socket, 0);

		if (!strstr(resp, "101")) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "mod_vosk: WS upgrade failed (no 101). Response:\n%s\n", resp);
			switch_socket_close(socket);
			return SWITCH_STATUS_GENERR;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_vosk: WS upgrade OK\n");
	}

	/* The config JSON is no longer sent here: FreeSWITCH delivers asr_json_param_* variables
	 * after open, so it is sent from vosk_asr_load_grammar (or the first feed) instead */

	vosk->out_max_len = 16000 * sizeof(int16_t);
	vosk->out_buffer = (int16_t *)switch_core_alloc(ah->memory_pool, vosk->out_max_len);

	vosk->ws = socket;
	vosk->handshake_done = 1;

	/* IGNITE THE BACKGROUND READ THREAD */
	vosk->running = 1;
	switch_threadattr_create(&thd_attr, ah->memory_pool);
	switch_threadattr_detach_set(thd_attr, 0);
	switch_thread_create(&vosk->read_thread, thd_attr, vosk_read_thread, vosk, ah->memory_pool);

	return SWITCH_STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Grammar / config JSON
 * ---------------------------------------------------------------------- */

/* Appends each '|' separated entry of extra (trimmed, lowercased, quoted) to the grammar in buf,
 * skipping empty entries, entries containing quotes or backslashes, and entries already present. */
static void vosk_append_extra_words(char *buf, switch_size_t cap, const char *extra)
{
	const char *p = extra;

	while (p && *p) {
		const char *end = strchr(p, '|');
		switch_size_t len = end ? (switch_size_t)(end - p) : strlen(p);
		char word[256];
		char quoted[260];
		switch_size_t i, w = 0;
		int bad = 0;

		while (len > 0 && (*p == ' ' || *p == '\t')) {
			p++;
			len--;
		}
		while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;

		if (len > 0 && len < sizeof(word)) {
			for (i = 0; i < len; i++) {
				char c = p[i];
				if (c == '"' || c == '\\' || (unsigned char)c < 0x20) {
					bad = 1;
					break;
				}
				word[w++] = (char)tolower((unsigned char)c);
			}
			word[w] = '\0';

			if (!bad && w > 0) {
				switch_snprintf(quoted, sizeof(quoted), "\"%s\"", word);
				if (!strstr(buf, quoted) && strlen(buf) + strlen(quoted) + 3 < cap) {
					if (buf[0]) strcat(buf, ", ");
					strcat(buf, quoted);
				}
			}
		}

		p = end ? end + 1 : NULL;
	}
}

/* Sends the config JSON once per session: the vosk.conf grammar plus any extra words */
static switch_status_t vosk_send_config(vosk_t *vosk)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	char *words = NULL;
	char *cfg_json = NULL;
	switch_size_t cap;

	switch_mutex_lock(vosk->mutex);

	if (vosk->config_sent) goto done;

	if (!vosk->ws || !vosk->handshake_done) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	switch_mutex_lock(MUTEX);
	cap = (globals.recognition_words ? strlen(globals.recognition_words) : 0) +
		  (vosk->extra_words ? strlen(vosk->extra_words) * 2 + 64 : 0) + 64;
	words = (char *)malloc(cap);
	if (words) {
		switch_snprintf(words, cap, "%s", globals.recognition_words ? globals.recognition_words : "");
		/* strip trailing whitespace from the CDATA block so appended entries follow cleanly */
		{
			switch_size_t wl = strlen(words);
			while (wl > 0 &&
				   (words[wl - 1] == ' ' || words[wl - 1] == '\n' || words[wl - 1] == '\r' || words[wl - 1] == '\t')) {
				words[--wl] = '\0';
			}
		}
		if (vosk->extra_words) vosk_append_extra_words(words, cap, vosk->extra_words);

		cfg_json = switch_mprintf("{\"config\":{\"sample_rate\":16000,\"min_confidence\":%f,\"vad_mode\":%d,"
								  "\"speech_complete_timeout\":%d,\"grammar\":[%s]}}",
								  globals.min_confidence, globals.vad_mode, globals.speech_complete_timeout, words);
	}
	switch_mutex_unlock(MUTEX);

	if (!cfg_json) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk: failed to allocate config JSON memory\n");
		status = SWITCH_STATUS_MEMERR;
		goto done;
	}

	// Log the final JSON string to verify CDATA parsed correctly
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_vosk: final JSON config payload:\n%s\n", cfg_json);

	if (ws_send_text(vosk->ws, cfg_json) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_vosk: failed to send config JSON\n");
		status = SWITCH_STATUS_GENERR;
		goto done;
	}

	vosk->config_sent = 1;

done:
	switch_safe_free(cfg_json);
	switch_safe_free(words);
	switch_mutex_unlock(vosk->mutex);
	return status;
}

/* -------------------------------------------------------------------------
 * vosk_asr_close
 * ---------------------------------------------------------------------- */
static switch_status_t vosk_asr_close(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;

	if (vosk->desp_total > 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						  "mod_vosk: despike replaced %llu of %llu input samples (%.4f%%)\n", vosk->desp_replaced,
						  vosk->desp_total, 100.0 * (double)vosk->desp_replaced / (double)vosk->desp_total);
	}

	/* SIGNAL BACKGROUND THREAD TO STOP AND WAIT FOR IT */
	vosk->running = 0;
	if (vosk->read_thread) {
		switch_status_t st;
		switch_thread_join(&st, vosk->read_thread);
		vosk->read_thread = NULL;
	}

	switch_mutex_lock(vosk->mutex);
	if (vosk->ws) {
		if (vosk->handshake_done) {
			unsigned char close_hdr[6] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
			switch_size_t cl = 6;
			switch_socket_send(vosk->ws, (char *)close_hdr, &cl);
		}
		switch_socket_close(vosk->ws);
		vosk->ws = NULL;
	}

	switch_set_flag(ah, SWITCH_ASR_FLAG_CLOSED);
	switch_safe_free(vosk->result);

	switch_safe_free(vosk->last_partial);

	switch_safe_free(vosk->rx_buf);
	vosk->rx_cap = 0;
	switch_safe_free(vosk->frag_buf);
	vosk->frag_cap = 0;
	vosk->frag_len = 0;
	vosk->frag_active = 0;

	switch_mutex_unlock(vosk->mutex);
	return SWITCH_STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Despike helpers
 * ---------------------------------------------------------------------- */
#ifndef DESPIKE_THRESH
#define DESPIKE_THRESH 6000
#endif

static int16_t median5_s16(int16_t a, int16_t b, int16_t c, int16_t d, int16_t e)
{
	int16_t v[5];
	int i, j;

	v[0] = a;
	v[1] = b;
	v[2] = c;
	v[3] = d;
	v[4] = e;
	for (i = 1; i < 5; i++) {
		int16_t key = v[i];
		for (j = i - 1; j >= 0 && v[j] > key; j--) { v[j + 1] = v[j]; }
		v[j + 1] = key;
	}
	return v[2];
}

/* -------------------------------------------------------------------------
 * vosk_asr_feed (NOW STRICTLY NON-BLOCKING)
 * ---------------------------------------------------------------------- */
static switch_status_t vosk_asr_feed(switch_asr_handle_t *ah, void *data, unsigned int len, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;

	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) return SWITCH_STATUS_BREAK;
	if (!vosk->handshake_done) return SWITCH_STATUS_BREAK;
	if (!vosk->running) return SWITCH_STATUS_BREAK; /* Thread failed or closed */

	/* Normally sent by load_grammar; this covers audio arriving before it */
	if (!vosk->config_sent && vosk_send_config(vosk) != SWITCH_STATUS_SUCCESS) return SWITCH_STATUS_BREAK;

	switch_mutex_lock(vosk->mutex);

	if (!vosk->ws) {
		switch_mutex_unlock(vosk->mutex);
		return SWITCH_STATUS_BREAK;
	}

	/* ---- Audio → Vosk -------------------------------------------------- */
	if (len > 0) {
		if (len % sizeof(int16_t) != 0) {
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_SUCCESS;
		}

		int in_sample_count = (int)(len / sizeof(int16_t));
		int16_t *in_samples = (int16_t *)data;
		if (globals.debug_pcm) {
			char fname_in[256];
			sprintf(fname_in, "C:/inetpub/SIPServer/sounds/debug_in_%p.pcm", (void *)ah);
			FILE *f_in = fopen(fname_in, "ab");
			if (f_in) {
				fwrite(data, 1, len, f_in);
				fclose(f_in);
			}
		}

		/* Robust despike: conditional 5-point median, in place, state carried across frames. */
		{
			int16_t h0 = vosk->desp_hist[0], h1 = vosk->desp_hist[1];
			int16_t h2 = vosk->desp_hist[2], h3 = vosk->desp_hist[3];

			for (int i = 0; i < in_sample_count; i++) {
				int16_t x = in_samples[i];
				int16_t c = h2;
				int16_t m = median5_s16(h0, h1, h2, h3, x);
				int d = (int)c - (int)m;

				if (d > DESPIKE_THRESH || d < -DESPIKE_THRESH) {
					c = m;
					vosk->desp_replaced++;
				}
				in_samples[i] = c;
				h0 = h1;
				h1 = h2;
				h2 = h3;
				h3 = x;
			}

			vosk->desp_hist[0] = h0;
			vosk->desp_hist[1] = h1;
			vosk->desp_hist[2] = h2;
			vosk->desp_hist[3] = h3;
			vosk->desp_total += (unsigned long long)in_sample_count;
		}

		int total_in_samples = vosk->remainder_len + in_sample_count;

		if (total_in_samples < 3) {
			for (int i = 0; i < in_sample_count; i++) { vosk->remainder[vosk->remainder_len++] = in_samples[i]; }
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_SUCCESS;
		}

		int out_sample_count = total_in_samples / 3;
		int processed_in_samples = out_sample_count * 3;
		switch_size_t out_bytes = (switch_size_t)(out_sample_count * sizeof(int16_t));
		int16_t new_hist[12];

		int16_t *out_samples = vosk->out_buffer;

		static const float FILTER_TAPS[13] = {-0.0030f, -0.0090f, -0.0124f, 0.0220f,  0.1187f,	0.2375f, 0.2924f,
											  0.2375f,	0.1187f,  0.0220f,	-0.0124f, -0.0090f, -0.0030f};

#define GET_IN_SAMPLE(idx)                                                                                             \
	((idx) < vosk->remainder_len ? vosk->remainder[(idx)] : in_samples[(idx) - vosk->remainder_len])

		if (out_bytes > vosk->out_max_len) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "mod_vosk: frame of %u bytes exceeds output buffer, dropped\n", len);
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_SUCCESS;
		}

		for (int s = 0; s < out_sample_count; s++) {
			float filtered = 0.0f;
			int base = s * 3;

			for (int tap = 0; tap < 13; tap++) {
				int k = base + tap;
				int16_t sample = (k < 12) ? vosk->fir_hist[k] : GET_IN_SAMPLE(k - 12);
				filtered += (float)sample * FILTER_TAPS[tap];
			}

			if (filtered > 32767.0f)
				filtered = 32767.0f;
			else if (filtered < -32768.0f)
				filtered = -32768.0f;
			out_samples[s] = (int16_t)filtered;
		}

		for (int k = 0; k < 12; k++) {
			int src = processed_in_samples + k;
			new_hist[k] = (src < 12) ? vosk->fir_hist[src] : GET_IN_SAMPLE(src - 12);
		}
		memcpy(vosk->fir_hist, new_hist, sizeof(new_hist));

		int new_remainder_len = total_in_samples - processed_in_samples;
		for (int i = 0; i < new_remainder_len; i++) { vosk->remainder[i] = GET_IN_SAMPLE(processed_in_samples + i); }
		vosk->remainder_len = new_remainder_len;

#undef GET_IN_SAMPLE

		if (globals.debug_pcm) {
			char fname_out[256];
			sprintf(fname_out, "C:/inetpub/SIPServer/sounds/debug_out_%p.pcm", (void *)ah);
			FILE *f_out = fopen(fname_out, "ab");
			if (f_out) {
				fwrite(out_samples, 1, out_bytes, f_out);
				fclose(f_out);
			}
		}

		switch_status_t sr = ws_send_binary(vosk->ws, (const char *)out_samples, out_bytes);

		if (sr != SWITCH_STATUS_SUCCESS) {
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_BREAK;
		}
	}

	switch_mutex_unlock(vosk->mutex);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t vosk_asr_pause(switch_asr_handle_t *ah) { return SWITCH_STATUS_SUCCESS; }
static switch_status_t vosk_asr_resume(switch_asr_handle_t *ah) { return SWITCH_STATUS_SUCCESS; }
static switch_status_t vosk_asr_start_input_timers(switch_asr_handle_t *ah) { return SWITCH_STATUS_SUCCESS; }

static switch_status_t vosk_asr_load_grammar(switch_asr_handle_t *ah, const char *grammar, const char *name)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;

	/* FreeSWITCH applies asr_json_param_* variables just before this call, so the extra words are known now */
	if (!vosk) return SWITCH_STATUS_FALSE;
	return vosk_send_config(vosk);
}

/* Receives asr_json_param_<name> channel or global variables, applied by FreeSWITCH after open */
static void vosk_asr_text_param(switch_asr_handle_t *ah, char *param, const char *val)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;

	if (!vosk || zstr(param)) return;

	if (!strcasecmp(param, "vosk_extra_words")) {
		switch_mutex_lock(vosk->mutex);
		vosk->extra_words = zstr(val) ? NULL : switch_core_strdup(ah->memory_pool, val);
		switch_mutex_unlock(vosk->mutex);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_vosk: extra grammar words: %s\n",
						  vosk->extra_words ? vosk->extra_words : "(none)");
	}
}
static switch_status_t vosk_asr_unload_grammar(switch_asr_handle_t *ah, const char *name)
{
	return SWITCH_STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * vosk_asr_check_results
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Context-aware endpoint: a partial that obviously continues gets more time
 * ---------------------------------------------------------------------- */
static const char *VOSK_TENS[] = {"twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety", NULL};
static const char *VOSK_UNITS[] = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", NULL};

static int vosk_word_in(const char *w, const char *const *list)
{
	for (; *list; list++)
		if (!strcasecmp(w, *list)) return 1;
	return 0;
}

static int vosk_word_in_config(const char *w)
{
	char key[64];
	if (!globals.endpoint_incomplete_words || !*w || strlen(w) > sizeof(key) - 3) return 0;
	switch_snprintf(key, sizeof(key), " %s ", w);
	return strstr(globals.endpoint_incomplete_words, key) != NULL;
}

/* 1 when the phrase cannot be finished yet, so a short pause must not end it:
 *   - it ends with a tens word ("conference thirty" -> "six" may follow; conferences are 26-99),
 *   - it ends with a units word straight after a lead word ("conference two" -> "seven" may follow;
 *     single-digit conferences do not exist),
 *   - it ends with a lead/command word ("conference", "join", "set volume" ...: endpoint-incomplete-words). */
static int vosk_phrase_incomplete(const char *phrase)
{
	char buf[512], *tok, *save = NULL;
	char last[64] = "", prev[64] = "";

	if (zstr(phrase)) return 0;
	switch_copy_string(buf, phrase, sizeof(buf));
	for (tok = strtok_s(buf, " \t", &save); tok; tok = strtok_s(NULL, " \t", &save)) {
		if (!strcmp(tok, "[unk]")) continue;
		switch_copy_string(prev, last, sizeof(prev));
		switch_copy_string(last, tok, sizeof(last));
	}
	if (!*last) return 0;

	if (vosk_word_in(last, VOSK_TENS)) return 1;
	if (vosk_word_in(last, VOSK_UNITS) && *prev && vosk_word_in_config(prev)) return 1;
	if (vosk_word_in_config(last)) return 1;
	return 0;
}

static switch_status_t vosk_check_results_locked(vosk_t *vosk)
{
	if (!vosk->result) return SWITCH_STATUS_FALSE;
	if (strstr(vosk->result, "\"text\":\"\"")) return SWITCH_STATUS_FALSE;

	if (!strstr(vosk->result, "\"partial\"")) { return SWITCH_STATUS_SUCCESS; }

	if (strstr(vosk->result, "\"partial\":\"\"")) { return SWITCH_STATUS_FALSE; }

	if (vosk->last_partial_time > 0) {
		switch_time_t now = switch_micro_time_now();
		cJSON *root = cJSON_Parse(vosk->result);
		const char *partial_phrase =
			(root && cJSON_HasObjectItem(root, "partial")) ? cJSON_GetObjectCstr(root, "partial") : NULL;

		/* Short timeout for a complete phrase; longer while the phrase obviously continues
		 * ("conference thirty" ... "six"), so a natural pause no longer cuts the number short.
		 * A real final from the Vosk server (with word confidences) still wins whenever it comes first. */
		int incomplete = partial_phrase && globals.endpoint_incomplete_ms > globals.endpoint_silence_ms &&
						 vosk_phrase_incomplete(partial_phrase);
		int wait_ms = incomplete ? globals.endpoint_incomplete_ms : globals.endpoint_silence_ms;

		if ((now - vosk->last_partial_time) > (switch_time_t)wait_ms * 1000) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "mod_vosk: %dms endpoint (%s phrase) forced final: '%s'\n", wait_ms,
							  incomplete ? "unfinished" : "complete", partial_phrase ? partial_phrase : "");
			if (partial_phrase) {
				char *forced_final = switch_mprintf("{\"text\":\"%s\"}", partial_phrase);
				if (forced_final) {
					switch_safe_free(vosk->result);
					vosk->result = forced_final;
					cJSON_Delete(root);
					return SWITCH_STATUS_SUCCESS;
				}
			}
		}
		if (root) cJSON_Delete(root);
	}

	return SWITCH_STATUS_FALSE;
}

static switch_status_t vosk_asr_check_results(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;
	switch_status_t ret;

	switch_mutex_lock(vosk->mutex);
	ret = vosk_check_results_locked(vosk);
	switch_mutex_unlock(vosk->mutex);

	return ret;
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

	switch_safe_free(vosk->last_partial);
	vosk->last_partial = NULL;
	vosk->last_partial_time = 0;

	switch_mutex_unlock(vosk->mutex);
	return ret;
}

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

static void set_global_str(char **dst, const char *val)
{
	char *copy = switch_safe_strdup(val);
	if (!copy) return;
	switch_safe_free(*dst);
	*dst = copy;
}

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
			if (!strcasecmp(var, "debug-pcm")) globals.debug_pcm = atoi(val);
			if (!strcasecmp(var, "server-url")) set_global_str(&globals.server_url, val);
			if (!strcasecmp(var, "return-json")) globals.return_json = atoi(val);
			if (!strcasecmp(var, "auto-reload")) globals.auto_reload = atoi(val);
			if (!strcasecmp(var, "endpoint-silence-ms")) globals.endpoint_silence_ms = atoi(val);
			if (!strcasecmp(var, "endpoint-incomplete-ms")) globals.endpoint_incomplete_ms = atoi(val);
			if (!strcasecmp(var, "endpoint-incomplete-words")) {
				char *padded = switch_mprintf(" %s ", val);
				char *c;
				for (c = padded; c && *c; c++) *c = (*c == ',') ? ' ' : (char)tolower((unsigned char)*c);
				set_global_str(&globals.endpoint_incomplete_words, padded);
				switch_safe_free(padded);
			}
			if (!strcasecmp(var, "speech-complete-timeout")) globals.speech_complete_timeout = atoi(val);
			if (!strcasecmp(var, "min-confidence")) globals.min_confidence = (float)atof(val);
			if (!strcasecmp(var, "vad-mode")) globals.vad_mode = atoi(val);
		}
	}

	// Read the raw CDATA block directly
	switch_xml_t grammar_node = switch_xml_child(cfg, "grammar");
	if (grammar_node) { set_global_str(&globals.recognition_words, switch_xml_txt(grammar_node)); }

done:
	if (!globals.server_url) set_global_str(&globals.server_url, "ws://127.0.0.1:2700");
	if (globals.endpoint_silence_ms <= 0) { globals.endpoint_silence_ms = 400; }
	if (globals.endpoint_incomplete_ms <= 0) { globals.endpoint_incomplete_ms = 1800; }
	if (!globals.endpoint_incomplete_words) {
		set_global_str(&globals.endpoint_incomplete_words,
					   " conference channel connect join switch go to with in set volume mic mike ear current ");
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					  "mod_vosk: endpoint %dms, %dms while unfinished (lead words:%s)\n", globals.endpoint_silence_ms,
					  globals.endpoint_incomplete_ms, globals.endpoint_incomplete_words);
	if (!globals.recognition_words) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "mod_vosk: 'grammar' missing from XML. Using hardcoded production defaults.\n");

		set_global_str(&globals.recognition_words,
					   "\"up\", \"down\", \"louder\", \"softer\", \"volume\", \"go\", \"to\", \"go to\", "
					   "\"conference\", \"switch\", \"switch to\", "
					   "\"connect\", \"connect to\", \"connect with\", \"join\", \"in\", \"with\", \"join in\", "
					   "\"join with\", \"disconnect\", \"leave\", "
					   "\"no\", \"no conference\", \"twenty\", \"thirty\", \"forty\", \"fifty\", \"sixty\", "
					   "\"seventy\", \"eighty\", \"ninety\", \"pre\",\"vault\",\"pre-vault\","
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

	if (!globals.server_ip) globals.server_ip = switch_core_strdup(globals.pool, "127.0.0.1");
	globals.server_port = 2700;
	if (xml) switch_xml_free(xml);
	return status;
}

static void do_load(void)
{
	switch_mutex_lock(MUTEX);
	globals.debug_pcm = 0; // default
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
	asr_interface->asr_text_param = vosk_asr_text_param;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_vosk loaded. Server: %s\n", globals.server_url);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vosk_shutdown)
{
	switch_event_unbind(&NODE);

	switch_mutex_lock(MUTEX);
	switch_safe_free(globals.server_url);
	switch_safe_free(globals.recognition_words);
	switch_safe_free(globals.endpoint_incomplete_words);
	switch_mutex_unlock(MUTEX);

	return SWITCH_STATUS_UNLOAD;
}