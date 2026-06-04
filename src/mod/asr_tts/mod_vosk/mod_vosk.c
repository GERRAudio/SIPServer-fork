/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2013, Anthony Minessale II <anthm@freeswitch.org>
 *
 * mod_vosk - Speech recognition using Vosk server (Windows x64 port)
 *
 * Changes from original Linux version:
 *  - Uses raw APR TCP sockets instead of libwebsockets
 *  - Manual WebSocket framing with RFC-6455 client masking
 *  - 8000Hz -> 16000Hz linear upsampling (duplicate-sample method)
 *    so FreeSWITCH telephony audio works with 16kHz Vosk models
 *  - Handshake deferred to first vosk_asr_feed call to avoid blocking
 *    the FreeSWITCH dialplan thread during vosk_asr_open
 */

#define __PRETTY_FUNCTION__ __func__
#include <switch.h>
#ifndef _MSC_VER
#include <netinet/tcp.h>
#endif

/* ---------------------------------------------------------------
 * Link the FreeSWITCH core import library.
 * Remove this pragma and add it to the VS project linker settings
 * if you want the build to be portable across machines.
 * --------------------------------------------------------------- */
#ifdef _MSC_VER
#pragma comment(lib, "C:/Development/GitHub/GERRAudio/SIPServer/x64/Release/FreeSwitchCore.lib")
#endif

/* Audio block size in bytes at 8000Hz (what FreeSWITCH delivers).
 * 3200 bytes = 1600 int16 samples = 200ms of 8kHz mono audio.
 * After 2x upsampling this becomes 6400 bytes at 16kHz. */
#define AUDIO_BLOCK_SIZE 3200
#define AUDIO_BLOCK_SIZE_16K (AUDIO_BLOCK_SIZE * 2) /* after upsampling */

/* WebSocket opcodes (RFC 6455) */
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
	switch_memory_pool_t *pool;
} globals;

typedef struct {
	switch_socket_t *ws; /* TCP socket to Vosk server        */
	char *result;		 /* latest JSON result string         */
	switch_mutex_t *mutex;
	switch_buffer_t *audio_buffer; /* accumulates 8kHz samples          */
	int handshake_done;			   /* WS upgrade complete?              */
} vosk_t;

/* ==============================================================
 * Helper: send a masked WebSocket binary frame (RFC 6455 §5.3).
 * Client MUST mask; server must NOT mask.  payload > 125 bytes
 * requires the 16-bit extended length encoding.
 * ==============================================================*/
static switch_status_t ws_send_binary(switch_socket_t *sock, const char *payload, switch_size_t payload_len)
{
	/* Frame layout for payload 126..65535 bytes:
	 * [0x82][0xFE][len_hi][len_lo][mk0][mk1][mk2][mk3][masked payload...] */
	unsigned char hdr[8];
	unsigned char mask[4];
	char *masked = NULL;
	switch_size_t hlen, slen;

	if (!sock || !payload || payload_len == 0) return SWITCH_STATUS_FALSE;

	/* Random 32-bit masking key */
	mask[0] = (unsigned char)(rand() & 0xFF);
	mask[1] = (unsigned char)(rand() & 0xFF);
	mask[2] = (unsigned char)(rand() & 0xFF);
	mask[3] = (unsigned char)(rand() & 0xFF);

	hdr[0] = 0x82; /* FIN=1, opcode=binary  */
	hdr[1] = 0xFE; /* MASK=1, len=126 (ext) */
	hdr[2] = (unsigned char)((payload_len >> 8) & 0xFF);
	hdr[3] = (unsigned char)(payload_len & 0xFF);
	hdr[4] = mask[0];
	hdr[5] = mask[1];
	hdr[6] = mask[2];
	hdr[7] = mask[3];

	/* Copy and mask payload */
	masked = (char *)malloc(payload_len);
	if (!masked) return SWITCH_STATUS_MEMERR;
	for (switch_size_t i = 0; i < payload_len; i++) { masked[i] = payload[i] ^ mask[i % 4]; }

	hlen = 8;
	if (switch_socket_send(sock, (char *)hdr, &hlen) != SWITCH_STATUS_SUCCESS) {
		free(masked);
		return SWITCH_STATUS_GENERR;
	}

	slen = payload_len;
	if (switch_socket_send(sock, masked, &slen) != SWITCH_STATUS_SUCCESS) {
		free(masked);
		return SWITCH_STATUS_GENERR;
	}

	free(masked);
	return SWITCH_STATUS_SUCCESS;
}

/* ==============================================================
 * Helper: send a masked WebSocket text frame (for JSON).
 * ==============================================================*/
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

	hdr[0] = 0x81; /* FIN=1, opcode=text */
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

	if (switch_socket_send(sock, (char *)hdr, &hlen) != SWITCH_STATUS_SUCCESS ||
		switch_socket_send(sock, masked, (slen = tlen, &slen)) != SWITCH_STATUS_SUCCESS) {
		free(masked);
		return SWITCH_STATUS_GENERR;
	}
	free(masked);
	return SWITCH_STATUS_SUCCESS;
}

/* ==============================================================
 * Helper: upsample 8kHz int16 PCM to 16kHz by duplicating each
 * sample (nearest-neighbour).  Simple but good enough for ASR.
 *
 * in_bytes  - input buffer  (8kHz,  int16, mono)
 * in_len    - byte count of input
 * out_buf   - caller-supplied output buffer (must be 2x in_len)
 * returns   - number of bytes written to out_buf
 * ==============================================================*/
static switch_size_t upsample_8k_to_16k(const char *in_bytes, switch_size_t in_len, char *out_buf)
{
	const int16_t *in = (const int16_t *)in_bytes;
	int16_t *out = (int16_t *)out_buf;
	switch_size_t n = in_len / sizeof(int16_t);

	for (switch_size_t i = 0; i < n; i++) {
		out[i * 2] = in[i];
		out[i * 2 + 1] = in[i];
	}
	return n * 2 * sizeof(int16_t);
}

/* ==============================================================
 * vosk_asr_open
 * Just opens the TCP connection.  WebSocket upgrade is deferred
 * to the first vosk_asr_feed call so the dialplan thread is not
 * blocked waiting for a network round-trip.
 * ==============================================================*/
static switch_status_t vosk_asr_open(switch_asr_handle_t *ah, const char *codec, int rate, const char *dest,
									 switch_asr_flag_t *flags)
{
	vosk_t *vosk;
	switch_socket_t *socket = NULL;
	switch_sockaddr_t *sa = NULL;

	if (!(vosk = (vosk_t *)switch_core_alloc(ah->memory_pool, sizeof(*vosk)))) { return SWITCH_STATUS_MEMERR; }
	ah->private_info = vosk;

	switch_mutex_init(&vosk->mutex, SWITCH_MUTEX_NESTED, ah->memory_pool);
	vosk->handshake_done = 0;
	vosk->result = NULL;

	if (switch_buffer_create_dynamic(&vosk->audio_buffer, AUDIO_BLOCK_SIZE, AUDIO_BLOCK_SIZE, 0) !=
		SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "vosk_asr_open: audio buffer create failed\n");
		return SWITCH_STATUS_MEMERR;
	}

	/* Force L16 (raw PCM) codec — FreeSWITCH will transcode for us */
	ah->codec = switch_core_strdup(ah->memory_pool, "L16");

	/* Resolve server address */
	if (switch_sockaddr_info_get(&sa, globals.server_ip, SWITCH_UNSPEC, globals.server_port, 0, globals.pool) !=
		SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "vosk_asr_open: failed to resolve %s\n",
						  globals.server_ip);
		return SWITCH_STATUS_GENERR;
	}

	if (switch_socket_create(&socket, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, globals.pool) !=
		SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "vosk_asr_open: socket create failed\n");
		return SWITCH_STATUS_GENERR;
	}

	/* 5-second connect timeout */
	switch_socket_timeout_set(socket, 5000000);

	if (switch_socket_connect(socket, sa) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "vosk_asr_open: TCP connect to %s:%u failed\n",
						  globals.server_ip, globals.server_port);
		switch_socket_close(socket);
		return SWITCH_STATUS_GENERR;
	}

	/* Non-blocking from here on */
	switch_socket_timeout_set(socket, 0);

	vosk->ws = socket;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "vosk_asr_open: TCP connected to %s:%u\n", globals.server_ip,
					  globals.server_port);
	return SWITCH_STATUS_SUCCESS;
}

/* ==============================================================
 * vosk_asr_close
 * ==============================================================*/
static switch_status_t vosk_asr_close(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;

	switch_mutex_lock(vosk->mutex);

	if (vosk->ws) {
		/* Send WebSocket close frame before closing TCP */
		if (vosk->handshake_done) {
			unsigned char close_hdr[6] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
			switch_size_t cl = 6;
			switch_socket_send(vosk->ws, (char *)close_hdr, &cl);
		}
		switch_socket_close(vosk->ws);
		vosk->ws = NULL;
	}

	switch_set_flag(ah, SWITCH_ASR_FLAG_CLOSED);
	switch_buffer_destroy(&vosk->audio_buffer);
	switch_safe_free(vosk->result);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "vosk_asr_close: done\n");
	switch_mutex_unlock(vosk->mutex);
	return SWITCH_STATUS_SUCCESS;
}

/* ==============================================================
 * vosk_asr_feed
 * Called continuously by FreeSWITCH with 8kHz PCM audio.
 * On the first call: performs the WebSocket HTTP upgrade.
 * Thereafter: accumulates audio, upsamples 8k->16k, sends to
 * Vosk, and polls for JSON responses.
 * ==============================================================*/
static switch_status_t vosk_asr_feed(switch_asr_handle_t *ah, void *data, unsigned int len, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;
	switch_pollfd_t pollfd = {0};
	int32_t num_fds = 0;

	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) return SWITCH_STATUS_BREAK;

	switch_mutex_lock(vosk->mutex);

	/* ---- WebSocket HTTP upgrade (first call only) ---- */
	if (!vosk->handshake_done) {
		const char *hs = "GET / HTTP/1.1\r\n"
						 "Host: 127.0.0.1:2700\r\n"
						 "Upgrade: websocket\r\n"
						 "Connection: Upgrade\r\n"
						 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
						 "Sec-WebSocket-Version: 13\r\n"
						 "\r\n";

		switch_size_t hs_len = strlen(hs);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "vosk_feed: sending WS upgrade (%u bytes)\n",
						  (unsigned)hs_len);

		if (switch_socket_send(vosk->ws, hs, &hs_len) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "vosk_feed: WS upgrade send failed\n");
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_BREAK;
		}

		/* Read HTTP 101 response — 500ms timeout */
		switch_socket_timeout_set(vosk->ws, 500000);

		char resp[4096] = {0};
		int total = 0;
		while (total < (int)sizeof(resp) - 1) {
			switch_size_t chunk = sizeof(resp) - total - 1;
			switch_status_t rs = switch_socket_recv(vosk->ws, resp + total, &chunk);
			if (rs != SWITCH_STATUS_SUCCESS || chunk == 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "vosk_feed: WS upgrade recv ended early (read %d bytes)\n", total);
				break;
			}
			total += (int)chunk;
			resp[total] = '\0';
			if (strstr(resp, "\r\n\r\n")) break;
		}

		switch_socket_timeout_set(vosk->ws, 0); /* back to non-blocking */

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "vosk_feed: WS upgrade response (%d bytes):\n%.200s\n",
						  total, resp);

		if (!strstr(resp, "101")) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "vosk_feed: server did not return 101, got: %.80s\n", resp);
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_BREAK;
		}

		/* Send Vosk config JSON telling it to expect 16kHz audio.
		 * This must arrive before any audio frames. */
		const char *cfg_json = "{\"config\":{\"sample_rate\":16000}}";
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "vosk_feed: sending config %s\n", cfg_json);
		if (ws_send_text(vosk->ws, cfg_json) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "vosk_feed: failed to send config JSON\n");
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_BREAK;
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "vosk_feed: WebSocket handshake and config complete\n");
		vosk->handshake_done = 1;
	}

	/* ---- Accumulate 8kHz audio ---- */
	switch_buffer_write(vosk->audio_buffer, data, len);

	/* ---- When we have a full block, upsample and send ---- */
	if (switch_buffer_inuse(vosk->audio_buffer) >= AUDIO_BLOCK_SIZE) {
		char in_buf[AUDIO_BLOCK_SIZE];
		char out_buf[AUDIO_BLOCK_SIZE_16K];

		switch_size_t in_len = switch_buffer_read(vosk->audio_buffer, in_buf, AUDIO_BLOCK_SIZE);

		/* 8kHz -> 16kHz by sample duplication */
		switch_size_t out_len = upsample_8k_to_16k(in_buf, in_len, out_buf);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
						  "vosk_feed: sending %u bytes at 8kHz -> %u bytes at 16kHz\n", (unsigned)in_len,
						  (unsigned)out_len);

		if (ws_send_binary(vosk->ws, out_buf, out_len) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "vosk_feed: ws_send_binary failed\n");
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_BREAK;
		}
	}

	/* ---- Non-blocking poll for incoming response frames ---- */
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

	/* ---- Read a WebSocket frame header (2 bytes) ---- */
	char hdr[2];
	switch_size_t hdr_len = 2;
	if (switch_socket_recv(vosk->ws, hdr, &hdr_len) != SWITCH_STATUS_SUCCESS || hdr_len < 2) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "vosk_feed: failed to read response frame header\n");
		switch_mutex_unlock(vosk->mutex);
		return SWITCH_STATUS_BREAK;
	}

	unsigned char opcode = (unsigned char)hdr[0] & 0x0F;
	unsigned char masked_flag = ((unsigned char)hdr[1] >> 7) & 0x01;
	unsigned char raw_len = (unsigned char)hdr[1] & 0x7F;

	switch_size_t payload_len = raw_len;

	/* Extended length */
	if (raw_len == 126) {
		char ext[2];
		switch_size_t el = 2;
		if (switch_socket_recv(vosk->ws, ext, &el) != SWITCH_STATUS_SUCCESS) {
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_BREAK;
		}
		payload_len = ((unsigned char)ext[0] << 8) | (unsigned char)ext[1];
	} else if (raw_len == 127) {
		/* 64-bit length — extremely unlikely for JSON, skip frame */
		char ext[8];
		switch_size_t el = 8;
		switch_socket_recv(vosk->ws, ext, &el);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "vosk_feed: oversized server frame (64-bit len), skipping\n");
		switch_mutex_unlock(vosk->mutex);
		return SWITCH_STATUS_SUCCESS;
	}

	/* Server frames should NOT be masked, but handle if they are */
	char mask_key[4] = {0, 0, 0, 0};
	if (masked_flag) {
		switch_size_t ml = 4;
		switch_socket_recv(vosk->ws, mask_key, &ml);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "vosk_feed: rx frame opcode=0x%02X masked=%d len=%u\n",
					  opcode, masked_flag, (unsigned)payload_len);

	/* ---- Read payload ---- */
	char *payload = NULL;
	if (payload_len > 0) {
		payload = (char *)switch_core_alloc(globals.pool, payload_len + 1);
		switch_size_t recv_len = payload_len;
		if (switch_socket_recv(vosk->ws, payload, &recv_len) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "vosk_feed: failed to read payload (%u bytes)\n",
							  (unsigned)payload_len);
			switch_mutex_unlock(vosk->mutex);
			return SWITCH_STATUS_BREAK;
		}
		payload[recv_len] = '\0';

		/* Unmask if server oddly masked (shouldn't happen per RFC) */
		if (masked_flag) {
			for (switch_size_t i = 0; i < recv_len; i++) payload[i] ^= mask_key[i % 4];
		}
	}

	/* ---- Dispatch on opcode ---- */
	switch (opcode) {
	case WS_OP_TEXT:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "vosk_feed: JSON result (%u bytes): %s\n",
						  (unsigned)payload_len, payload ? payload : "");
		if (payload) {
			switch_safe_free(vosk->result);
			vosk->result = switch_safe_strdup(payload);
		}
		break;

	case WS_OP_BINARY:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
						  "vosk_feed: unexpected binary frame from server (%u bytes)\n", (unsigned)payload_len);
		break;

	case WS_OP_PING:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "vosk_feed: PING received, sending PONG\n");
		{
			/* PONG: unmasked (client->server PONG should be masked per RFC,
			 * but in practice servers accept unmasked PONG) */
			unsigned char pong[2] = {(unsigned char)(0x80 | WS_OP_PONG), (unsigned char)(payload_len & 0x7F)};
			switch_size_t pl = 2;
			switch_socket_send(vosk->ws, (char *)pong, &pl);
			if (payload_len > 0 && payload) {
				switch_size_t pd = payload_len;
				switch_socket_send(vosk->ws, payload, &pd);
			}
		}
		break;

	case WS_OP_CLOSE:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "vosk_feed: server sent CLOSE frame\n");
		switch_mutex_unlock(vosk->mutex);
		return SWITCH_STATUS_BREAK;

	default:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "vosk_feed: unknown opcode 0x%02X len=%u payload=%.40s\n", opcode, (unsigned)payload_len,
						  payload ? payload : "");
		break;
	}

	switch_mutex_unlock(vosk->mutex);
	return SWITCH_STATUS_SUCCESS;
}

/* ==============================================================
 * Stub implementations required by the ASR interface
 * ==============================================================*/
static switch_status_t vosk_asr_pause(switch_asr_handle_t *ah) { return SWITCH_STATUS_SUCCESS; }

static switch_status_t vosk_asr_resume(switch_asr_handle_t *ah) { return SWITCH_STATUS_SUCCESS; }

static switch_status_t vosk_asr_load_grammar(switch_asr_handle_t *ah, const char *grammar, const char *name)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t vosk_asr_unload_grammar(switch_asr_handle_t *ah, const char *name)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t vosk_asr_start_input_timers(switch_asr_handle_t *ah) { return SWITCH_STATUS_SUCCESS; }

/* ==============================================================
 * vosk_asr_check_results
 * Returns SUCCESS if a non-empty result is available.
 * ==============================================================*/
static switch_status_t vosk_asr_check_results(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	vosk_t *vosk = (vosk_t *)ah->private_info;
	/* Non-empty result AND not just {"text":""} */
	return (vosk->result && strstr(vosk->result, "\"\"") == NULL) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/* ==============================================================
 * vosk_asr_get_results
 * Extracts text from the JSON and returns it to FreeSWITCH.
 * Returns SUCCESS for final results, MORE_DATA for partials.
 * ==============================================================*/
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
		/* Return raw JSON to the dialplan */
		*xmlstr = switch_safe_strdup(vosk->result);
		ret = strstr(vosk->result, "\"partial\"") ? SWITCH_STATUS_MORE_DATA : SWITCH_STATUS_SUCCESS;
	} else {
		/* Extract just the text string */
		cJSON *root = cJSON_Parse(vosk->result);

		if (root && cJSON_HasObjectItem(root, "text")) {
			*xmlstr = switch_safe_strdup(cJSON_GetObjectCstr(root, "text"));
			ret = SWITCH_STATUS_SUCCESS;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "vosk_get_results: FINAL text=\"%s\"\n", *xmlstr);
		} else if (root && cJSON_HasObjectItem(root, "partial")) {
			*xmlstr = switch_safe_strdup(cJSON_GetObjectCstr(root, "partial"));
			ret = SWITCH_STATUS_MORE_DATA;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "vosk_get_results: partial=\"%s\"\n", *xmlstr);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "vosk_get_results: JSON parse failed for: %s\n",
							  vosk->result);
			ret = SWITCH_STATUS_GENERR;
		}
		if (root) cJSON_Delete(root);
	}

	switch_safe_free(vosk->result);
	vosk->result = NULL;
	switch_mutex_unlock(vosk->mutex);
	return ret;
}

/* ==============================================================
 * Configuration loader
 * ==============================================================*/
static switch_status_t load_config(void)
{
	char *cf = "vosk.conf";
	switch_xml_t cfg, xml = NULL, param, settings;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "load_config: cannot open %s\n", cf);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *)switch_xml_attr_soft(param, "name");
			char *val = (char *)switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "server-url")) globals.server_url = switch_core_strdup(globals.pool, val);
			if (!strcasecmp(var, "return-json")) globals.return_json = atoi(val);
		}
	}

done:
	if (!globals.server_url) globals.server_url = switch_core_strdup(globals.pool, "ws://127.0.0.1:2700");

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
	if (globals.auto_reload) {
		do_load();
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Vosk config reloaded\n");
	}
}

/* ==============================================================
 * Module load / shutdown
 * ==============================================================*/
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

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_vosk loaded — server=%s:%u, return_json=%d\n",
					  globals.server_ip, globals.server_port, globals.return_json);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vosk_shutdown)
{
	switch_event_unbind(&NODE);
	return SWITCH_STATUS_UNLOAD;
}
