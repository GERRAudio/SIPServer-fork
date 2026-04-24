/*
 * ravenna_rx.c — single-thread RX reactor for all Ravenna streams.
 *
 * One thread, one poll set, one batched recv loop per ready socket.
 * Decoded samples are written into per-channel rings; FS sessions
 * read from them via fan-out cursors.
 *
 * Implementation note: we use poll()/WSAPoll() for portability.
 * If profiling shows poll-rebuild overhead is significant we can
 * switch the Linux build to epoll without disturbing the rest of
 * the module.
 */

#include "mod_ravenna.h"
#include "ravenna_net.h"
#include "ravenna_rtp.h"
#include "ravenna_ring.h"

#ifdef _WIN32
  #include <winsock2.h>
#else
  #include <poll.h>
#endif

#define RAV_RX_BATCH    32         /* packets drained per ready socket */
#define RAV_RX_MTU      2048       /* per-packet buffer */
#define RAV_POLL_MS     100        /* wake to allow stream churn */

/* Build the pollfd array from current streams + the wake pipe.
 * Returns number of fds written. The wake pipe is always at index 0. */
static int build_pollset(
#ifdef _WIN32
						 WSAPOLLFD *pfds,
#else
						 struct pollfd *pfds,
#endif
						 int max,
						 ravenna_stream_t **stream_for_idx)
{
	int n = 0;
	switch_hash_index_t *hi;

	pfds[n].fd      = mod_ravenna_globals.rx_wake_r;
	pfds[n].events  = POLLIN;
	pfds[n].revents = 0;
	stream_for_idx[n] = NULL;
	n++;

	for (hi = switch_core_hash_first(mod_ravenna_globals.streams);
		 hi && n < max;
		 hi = switch_core_hash_next(&hi)) {
		const void *k; void *v; switch_ssize_t klen;
		ravenna_stream_t *s;
		switch_core_hash_this(hi, &k, &klen, &v);
		s = (ravenna_stream_t *)v;
		if (!s->rx_enabled || s->rx_sock == RAVENNA_INVALID_SOCKET) continue;
		pfds[n].fd      = s->rx_sock;
		pfds[n].events  = POLLIN;
		pfds[n].revents = 0;
		stream_for_idx[n] = s;
		n++;
	}
	return n;
}

/* Decode a single packet into the stream's per-channel rings.
 * Returns SWITCH_STATUS_SUCCESS or _GENERR. */
static switch_status_t handle_packet(ravenna_stream_t *s,
									 const uint8_t *pkt, int len,
									 ravenna_sample_t **scratch,
									 int scratch_cap)
{
	ravenna_rtp_hdr_t h;
	int samples;
	int ch;

	if (ravenna_rtp_parse(pkt, len, &h) != SWITCH_STATUS_SUCCESS) {
		s->rx_drops++;
		return SWITCH_STATUS_GENERR;
	}

	if (s->rx_seq_inited) {
		uint16_t expected = (uint16_t)(s->rx_last_seq + 1);
		if (h.seq != expected) s->rx_seq_gaps++;
	}
	s->rx_last_seq   = h.seq;
	s->rx_last_ts    = h.timestamp;
	s->rx_seq_inited = SWITCH_TRUE;
	s->rx_packets++;

	samples = ravenna_rtp_decode(pkt + h.payload_off, h.payload_len,
								 s->rx_codec, s->channels,
								 scratch, scratch_cap);
	if (samples <= 0) {
		s->rx_drops++;
		return SWITCH_STATUS_GENERR;
	}

	for (ch = 0; ch < s->channels; ch++) {
		if (s->rx_rings[ch]) {
			ravenna_ring_write(s->rx_rings[ch], scratch[ch], (uint32_t)samples);
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

void *SWITCH_THREAD_FUNC ravenna_rx_thread_run(switch_thread_t *t, void *obj)
{
#ifdef _WIN32
	WSAPOLLFD     pfds[RAVENNA_MAX_STREAMS + 1];
#else
	struct pollfd pfds[RAVENNA_MAX_STREAMS + 1];
#endif
	ravenna_stream_t *map[RAVENNA_MAX_STREAMS + 1];

	uint8_t pktbuf[RAV_RX_MTU];
	ravenna_pkt_t pkts[RAV_RX_BATCH];
	int i;

	/* Per-channel scratch (samples per packet for the largest config). */
	static ravenna_sample_t  scratch_storage[RAVENNA_MAX_CHANNELS][8192];
	static ravenna_sample_t *scratch_ptrs[RAVENNA_MAX_CHANNELS];
	for (i = 0; i < RAVENNA_MAX_CHANNELS; i++) scratch_ptrs[i] = scratch_storage[i];

	for (i = 0; i < RAV_RX_BATCH; i++) {
		pkts[i].buf = pktbuf;     /* recv loop is single-buffered: we
									 process each packet immediately */
		pkts[i].cap = sizeof(pktbuf);
		pkts[i].len = 0;
	}

	(void)t; (void)obj;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "ravenna: rx reactor started\n");

	while (mod_ravenna_globals.running) {
		int nfds, ready, j;

		switch_mutex_lock(mod_ravenna_globals.mutex);
		nfds = build_pollset(pfds, RAVENNA_MAX_STREAMS + 1, map);
		switch_mutex_unlock(mod_ravenna_globals.mutex);

#ifdef _WIN32
		ready = WSAPoll(pfds, nfds, RAV_POLL_MS);
#else
		ready = poll(pfds, nfds, RAV_POLL_MS);
#endif
		if (ready <= 0) continue;

		if (pfds[0].revents & POLLIN) {
			ravenna_net_drain_wake(mod_ravenna_globals.rx_wake_r);
		}

		for (j = 1; j < nfds; j++) {
			ravenna_stream_t *s = map[j];
			if (!s || !(pfds[j].revents & POLLIN)) continue;

			/* Drain socket. */
			for (;;) {
				int r = ravenna_net_recv_batch(s->rx_sock, pkts, 1);
				if (r <= 0) break;
				handle_packet(s, pkts[0].buf, pkts[0].len,
							  scratch_ptrs, 8192);
			}
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "ravenna: rx reactor stopped\n");
	return NULL;
}
