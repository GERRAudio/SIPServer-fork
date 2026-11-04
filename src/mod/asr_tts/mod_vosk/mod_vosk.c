/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2013, Anthony Minessale II <anthm@freeswitch.org>
 *
 * mod_vosk - Speech recognition using Vosk server (Windows x64 port)
 */

static char *version_date = "2026-11-04";

#define __PRETTY_FUNCTION__ __func__
#include <switch.h>
#ifndef _MSC_VER
#include <netinet/tcp.h>
#endif

#ifdef _MSC_VER
// #pragma comment(lib, "C:/Development/GitHub/GERRAudio/SIPServer/x64/Release/FreeSwitchCore.lib")
#endif

/* Set to 1 to write debug_in and debug_out PCM files. Set to 0 for production. */
#define VOSK_DEBUG_PCM 1

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

	int16_t last_in_sample;

	char *rx_buf;
	switch_size_t rx_cap;
	char *frag_buf;
	switch_size_t frag_len;
	switch_size_t frag_cap;
	int frag_active;
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
 * vosk_asr_open
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

	vosk->last_partial = NULL;
	vosk->last_partial_time = 0;

	vosk->rx_buf = NULL;
	vosk->rx_cap = 0;
	vosk->frag_buf = NULL;
	vosk->frag_len = 0;
	vosk->frag_cap = 0;
	vosk->frag_active = 0;
	memset(vosk->fir_hist, 0, sizeof(vosk->fir_hist));

	vosk->remainder_len = 0;
	vosk->last_in_sample = 0;

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

	{
		char *cfg_json;

		switch_mutex_lock(MUTEX);
		cfg_json = switch_mprintf("{\"config\":{\"sample_rate\":16000,\"min_confidence\":%f,\"vad_mode\":%d,"
								  "\"speech_complete_timeout\":%d,\"grammar\":[%s]}}",
								  globals.min_confidence, globals.vad_mode, globals.speech_complete_timeout,
								  globals.recognition_words ? globals.recognition_words : "");
		switch_mutex_unlock(MUTEX);

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
		switch_safe_free(cfg_json);
	}

	vosk->out_max_len = 16000 * sizeof(int16_t);
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
 * vosk_asr_feed
 * ---------------------------------------------------------------------- */
static switch_status_t vosk_asr_feed(switch_asr_handle_t *ah, void *data, unsigned int len, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;
	switch_pollfd_t pollfd = {0};
	int32_t num_fds = 0;
	unsigned char hdr[2];
	unsigned char fin, opcode, masked_flag;
	unsigned char mask_key[4] = {0, 0, 0, 0};
	uint64_t payload_len;
	switch_size_t n;

	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) return SWITCH_STATUS_BREAK;
	if (!vosk->handshake_done) return SWITCH_STATUS_BREAK;

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

#if VOSK_DEBUG_PCM
		/* 1. DUMP RAW INPUT AUDIO FROM FREESWITCH (48kHz) BEFORE ANY PROCESSING */
		/* Placed explicitly at the top so it captures the unmodified network stream */
		char fname_in[256];
		sprintf(fname_in, "C:/inetpub/SIPServer/sounds/debug_in_%p.pcm", (void *)ah);
		FILE *f_in = fopen(fname_in, "ab");
		if (f_in) {
			fwrite(data, 1, len, f_in);
			fclose(f_in);
		}
#endif

		/* SOFTWARE DESPIKER: Limit extreme sample-to-sample voltage jumps (slew rate) */
		for (int i = 0; i < in_sample_count; i++) {
			int prev = (i == 0) ? vosk->last_in_sample : in_samples[i - 1];
			int delta = in_samples[i] - prev;

			if (delta > 8000 || delta < -8000) {
				/* Slew rate exceeded. Hold the previous sample's voltage */
				in_samples[i] = prev;
			}
		}

		if (in_sample_count > 0) { vosk->last_in_sample = in_samples[in_sample_count - 1]; }

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

#if VOSK_DEBUG_PCM
		/* 2. DUMP OUTPUT AUDIO GOING TO VOSK (16kHz) AFTER DESPIKING */
		char fname_out[256];
		sprintf(fname_out, "C:/inetpub/SIPServer/sounds/debug_out_%p.pcm", (void *)ah);
		FILE *f_out = fopen(fname_out, "ab");
		if (f_out) {
			fwrite(out_samples, 1, out_bytes, f_out);
			fclose(f_out);
		}
#endif

		switch_status_t sr = ws_send_binary(vosk->ws, (const char *)out_samples, out_bytes);

		if (sr != SWITCH_STATUS_SUCCESS) {
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_BREAK;
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
	if (ws_recv_exact(vosk->ws, (char *)hdr, 2) != SWITCH_STATUS_SUCCESS) goto ws_fail;

	fin = (hdr[0] >> 7) & 0x01;
	opcode = hdr[0] & 0x0F;
	masked_flag = (hdr[1] >> 7) & 0x01;
	payload_len = hdr[1] & 0x7F;

	if (payload_len == 126) {
		unsigned char ext[2];
		if (ws_recv_exact(vosk->ws, (char *)ext, 2) != SWITCH_STATUS_SUCCESS) goto ws_fail;
		payload_len = ((uint64_t)ext[0] << 8) | ext[1];
	} else if (payload_len == 127) {
		unsigned char ext[8];
		if (ws_recv_exact(vosk->ws, (char *)ext, 8) != SWITCH_STATUS_SUCCESS) goto ws_fail;
		payload_len = 0;
		for (int b = 0; b < 8; b++) payload_len = (payload_len << 8) | ext[b];
	}

	if (payload_len > WS_MAX_PAYLOAD || (opcode >= 0x08 && payload_len > 125)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "mod_vosk: implausible WS frame (opcode 0x%02x, length %llu)\n", opcode,
						  (unsigned long long)payload_len);
		goto ws_fail;
	}

	if (masked_flag && ws_recv_exact(vosk->ws, (char *)mask_key, 4) != SWITCH_STATUS_SUCCESS) goto ws_fail;

	n = (switch_size_t)payload_len;
	if (vosk_buf_reserve(&vosk->rx_buf, &vosk->rx_cap, n + 1) != SWITCH_STATUS_SUCCESS) goto ws_fail;

	if (n > 0) {
		if (ws_recv_exact(vosk->ws, vosk->rx_buf, n) != SWITCH_STATUS_SUCCESS) goto ws_fail;
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
				if (n > 0) vosk_handle_text(vosk, vosk->rx_buf);
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
			if (vosk->frag_len > 0) vosk_handle_text(vosk, vosk->frag_buf);
			vosk->frag_len = 0;
			vosk->frag_active = 0;
		}
		break;

	case WS_OP_PING:
		ws_send_pong(vosk->ws, vosk->rx_buf, n);
		break;

	case WS_OP_CLOSE:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "mod_vosk: server sent WS CLOSE\n");
		switch_mutex_unlock(vosk->mutex);
		return SWITCH_STATUS_BREAK;

	default:
		break;
	}

	switch_mutex_unlock(vosk->mutex);
	return SWITCH_STATUS_SUCCESS;

ws_fail:
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					  "mod_vosk: WebSocket read failed or lost sync, ending recognition\n");
	switch_mutex_unlock(vosk->mutex);
	return SWITCH_STATUS_BREAK;
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
 * ---------------------------------------------------------------------- */

static switch_status_t vosk_check_results_locked(vosk_t *vosk)
{
	if (!vosk->result) return SWITCH_STATUS_FALSE;
	if (strstr(vosk->result, "\"text\":\"\"")) return SWITCH_STATUS_FALSE;

	if (!strstr(vosk->result, "\"partial\"")) { return SWITCH_STATUS_SUCCESS; }

	if (strstr(vosk->result, "\"partial\"")) {
		if (strstr(vosk->result, "\"partial\":\"\"")) { return SWITCH_STATUS_FALSE; }

		if (vosk->last_partial_time > 0) {
			switch_time_t now = switch_micro_time_now();

			switch_time_t silence_threshold_us = (switch_time_t)(globals.endpoint_silence_ms * 1000);

			if ((now - vosk->last_partial_time) > silence_threshold_us) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
								  "mod_vosk: Dynamic %dms endpoint triggered via XML config.\n",
								  globals.endpoint_silence_ms);

				cJSON *root = cJSON_Parse(vosk->result);
				if (root && cJSON_HasObjectItem(root, "partial")) {
					const char *partial_phrase = cJSON_GetObjectCstr(root, "partial");

					char *forced_final = switch_mprintf("{\"text\":\"%s\"}", partial_phrase ? partial_phrase : "");

					if (forced_final) {
						switch_safe_free(vosk->result);
						vosk->result = forced_final;
						cJSON_Delete(root);
						return SWITCH_STATUS_SUCCESS;
					}
				}
				if (root) cJSON_Delete(root);
			}
		}
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
	vosk->last_partial_time = 0; /* CRASH FIX APPLIED HERE */

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
			if (!strcasecmp(var, "recognition-words")) set_global_str(&globals.recognition_words, val);
			if (!strcasecmp(var, "endpoint-silence-ms")) globals.endpoint_silence_ms = atoi(val);
			if (!strcasecmp(var, "speech-complete-timeout")) globals.speech_complete_timeout = atoi(val);
			if (!strcasecmp(var, "min-confidence")) globals.min_confidence = (float)atof(val);
			if (!strcasecmp(var, "vad-mode")) globals.vad_mode = atoi(val);
		}
	}
done:
	if (!globals.server_url) set_global_str(&globals.server_url, "ws://127.0.0.1:2700");
	if (globals.endpoint_silence_ms <= 0) { globals.endpoint_silence_ms = 400; }
	if (!globals.recognition_words) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "mod_vosk: 'recognition-words' missing from XML. Using hardcoded production defaults.\n");

		set_global_str(&globals.recognition_words,
					   "\"set\", \"time\", \"set mic\", \"set mike\", \"set volume\", \"operator\", \"call operator\", \"what time is it\", "
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

	/* Reset variables to prevent caching old config states on reload */
	globals.debug_pcm = 0;
	globals.min_confidence = 0.0;
	globals.endpoint_silence_ms = 0;
	globals.vad_mode = 0;

	switch_safe_free(globals.recognition_words);
	globals.recognition_words = NULL;
	switch_safe_free(globals.server_url);
	globals.server_url = NULL;

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

	switch_mutex_lock(MUTEX);
	switch_safe_free(globals.server_url);
	switch_safe_free(globals.recognition_words);
	switch_mutex_unlock(MUTEX);

	return SWITCH_STATUS_UNLOAD;
}