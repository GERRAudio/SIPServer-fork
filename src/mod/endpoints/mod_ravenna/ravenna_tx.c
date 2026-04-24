/*
 * ravenna_tx.c — single-thread TX pacer driven by mod_ptp_timer.
 *
 * The pacer ticks at the finest interval the PTP timer supports
 * (1 ms, since mod_ptp_timer's interval is in ms). Each iteration we
 * walk the streams hash and emit any RTP packets that are due.
 *
 * For sub-ms ptimes (e.g. 125 µs), we emit ceil(1ms / ptime) packets
 * per tick for that stream, advancing tx_next_due_us appropriately.
 *
 * Per stream we read `samples_per_packet` samples from each TX channel
 * ring (zero-fill if a channel is empty), interleave-encode them
 * directly into the RTP payload, prepend the 12-byte RTP header in
 * place, and sendto() once.
 */

#include "mod_ravenna.h"
#include "ravenna_net.h"
#include "ravenna_rtp.h"
#include "ravenna_ring.h"

#define RAV_TX_TIMER_NAME       RAVENNA_PTP_TIMER_NAME
#define RAV_TX_TIMER_INTERVAL_MS 1
#define RAV_TX_PKT_BUF_SIZE     (RAVENNA_RTP_HDR_SIZE + 8192)

/* For each TX channel, allocate a fan-out cursor on its ring so that
 * the pacer can keep an independent read position. Allocated lazily
 * the first time we send for that channel. */
typedef struct {
	ravenna_cursor_t *cur[RAVENNA_MAX_CHANNELS];
} tx_state_t;

static switch_status_t ensure_tx_cursors(ravenna_stream_t *s, tx_state_t *st)
{
	int ch;
	for (ch = 0; ch < s->channels; ch++) {
		if (!st->cur[ch] && s->tx_rings[ch]) {
			st->cur[ch] = ravenna_ring_attach(s->tx_rings[ch]);
			if (!st->cur[ch]) return SWITCH_STATUS_GENERR;
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

/* Pull samples_per_packet samples per channel into per-channel scratch.
 * Missing data is zero-filled. */
static void pull_channels(ravenna_stream_t *s, tx_state_t *st,
						  ravenna_sample_t **scratch, int samples)
{
	int ch;
	for (ch = 0; ch < s->channels; ch++) {
		int got = 0;
		if (st->cur[ch]) {
			got = ravenna_cursor_read(st->cur[ch], scratch[ch], samples);
			if (got < 0) got = 0;
		}
		if (got < samples) {
			memset(scratch[ch] + got, 0,
				   (samples - got) * sizeof(ravenna_sample_t));
		}
	}
}

static void emit_one_packet(ravenna_stream_t *s, tx_state_t *st,
							uint8_t *pktbuf, int pktbuf_cap,
							ravenna_sample_t **scratch)
{
	int payload_cap = pktbuf_cap - RAVENNA_RTP_HDR_SIZE;
	int n;

	pull_channels(s, st, scratch, s->samples_per_packet);

	n = ravenna_rtp_encode((const ravenna_sample_t * const *)scratch,
						   s->samples_per_packet, s->tx_codec,
						   s->channels,
						   pktbuf + RAVENNA_RTP_HDR_SIZE, payload_cap);
	if (n <= 0) return;

	ravenna_rtp_write_hdr(pktbuf,
						  (uint8_t)s->rtp_payload_type,
						  s->tx_seq, s->tx_ts, s->tx_ssrc, 0);

	ravenna_net_send(s->tx_sock, &s->tx_dest,
					 pktbuf, RAVENNA_RTP_HDR_SIZE + n);

	/* ST 2022-7 — identical packet (same seq/ts/ssrc) to redundant path */
	if (s->st2022_7 && s->tx2_sock != RAVENNA_INVALID_SOCKET) {
		ravenna_net_send(s->tx2_sock, &s->tx2_dest,
						 pktbuf, RAVENNA_RTP_HDR_SIZE + n);
	}

	s->tx_seq++;
	s->tx_ts += (uint32_t)s->samples_per_packet;
	s->tx_packets++;
}

void *SWITCH_THREAD_FUNC ravenna_tx_thread_run(switch_thread_t *t, void *obj)
{
	switch_timer_t timer;
	switch_bool_t  timer_inited = SWITCH_FALSE;
	uint8_t        pktbuf[RAV_TX_PKT_BUF_SIZE];

	static ravenna_sample_t  scratch_storage[RAVENNA_MAX_CHANNELS][8192];
	static ravenna_sample_t *scratch_ptrs[RAVENNA_MAX_CHANNELS];

	/* Per-stream pacer cursors are kept in a hash keyed by stream pointer
	 * value. To avoid pulling in another data structure we attach a small
	 * tx_state_t to the stream itself via a side hash. For the first
	 * cut, we look up by name. */
	switch_hash_t *tx_state_hash = NULL;
	int i;

	(void)t; (void)obj;

	for (i = 0; i < RAVENNA_MAX_CHANNELS; i++) scratch_ptrs[i] = scratch_storage[i];

	switch_core_hash_init(&tx_state_hash);

	if (switch_core_timer_init(&timer, RAV_TX_TIMER_NAME,
							   RAV_TX_TIMER_INTERVAL_MS, 8 /* dummy */,
							   mod_ravenna_globals.pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
						  "ravenna: tx pacer cannot start — timer '%s' not available\n",
						  RAV_TX_TIMER_NAME);
		switch_core_hash_destroy(&tx_state_hash);
		return NULL;
	}
	timer_inited = SWITCH_TRUE;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "ravenna: tx pacer started (timer %s, %d ms tick)\n",
					  RAV_TX_TIMER_NAME, RAV_TX_TIMER_INTERVAL_MS);

	while (mod_ravenna_globals.running) {
		switch_time_t now_us;
		switch_hash_index_t *hi;

		switch_core_timer_next(&timer);
		now_us = switch_micro_time_now();

		switch_mutex_lock(mod_ravenna_globals.mutex);
		for (hi = switch_core_hash_first(mod_ravenna_globals.streams);
			 hi;
			 hi = switch_core_hash_next(&hi)) {
			const void *k; void *v; switch_ssize_t klen;
			ravenna_stream_t *s;
			tx_state_t *st;
			switch_time_t period_us;
			int max_emit_per_tick = 16;
			int emitted = 0;

			switch_core_hash_this(hi, &k, &klen, &v);
			s = (ravenna_stream_t *)v;
			if (!s->tx_enabled || s->tx_sock == RAVENNA_INVALID_SOCKET) continue;

			st = (tx_state_t *)switch_core_hash_find(tx_state_hash, s->name);
			if (!st) {
				st = switch_core_alloc(mod_ravenna_globals.pool, sizeof(*st));
				memset(st, 0, sizeof(*st));
				switch_core_hash_insert(tx_state_hash, s->name, st);
			}
			ensure_tx_cursors(s, st);

			period_us = (switch_time_t)(s->ptime_ms * 1000.0);
			if (period_us == 0) period_us = 1000;
			if (s->tx_next_due_us == 0) s->tx_next_due_us = now_us;

			while (s->tx_next_due_us <= now_us && emitted < max_emit_per_tick) {
				emit_one_packet(s, st, pktbuf, sizeof(pktbuf), scratch_ptrs);
				s->tx_next_due_us += period_us;
				emitted++;
			}
		}
		switch_mutex_unlock(mod_ravenna_globals.mutex);
	}

	if (timer_inited) switch_core_timer_destroy(&timer);
	switch_core_hash_destroy(&tx_state_hash);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "ravenna: tx pacer stopped\n");
	return NULL;
}
