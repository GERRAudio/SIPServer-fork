/*
 * mod_ravenna.h
 *
 * Shared types for the Ravenna / AES67 endpoint module.
 *
 * Design highlights:
 *   - One global RX reactor thread polls every stream socket via
 *     epoll (Linux) or WSAPoll (Windows).
 *   - One global TX pacer thread, ticked by mod_ptp_timer, walks a
 *     min-heap of streams and emits RTP packets at each stream's
 *     ptime. Sub-ms ptimes are handled by emitting N packets per
 *     1 ms tick.
 *   - Per-channel SPSC sample ring + fan-out cursors let us "fork"
 *     a single received stream into many FS sessions with no extra
 *     packets and no extra threads. A slow consumer is flagged
 *     overrun and the corresponding FS leg is hung up.
 *   - Codec support: L16, L24, L32 (AES67-compatible for L16/L24).
 */

#ifndef MOD_RAVENNA_H
#define MOD_RAVENNA_H

#include <switch.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET ravenna_socket_t;
  #define RAVENNA_INVALID_SOCKET INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  typedef int ravenna_socket_t;
  #define RAVENNA_INVALID_SOCKET (-1)
#endif

#define RAVENNA_MOD_NAME            "mod_ravenna"
#define RAVENNA_MAX_NAME_LEN        128
#define RAVENNA_MAX_IFACE_LEN       64
#define RAVENNA_MAX_IP_LEN          48
#define RAVENNA_MAX_CHANNELS        64        /* per stream */
#define RAVENNA_MAX_STREAMS         256
#define RAVENNA_MAX_ENDPOINTS       1024
#define RAVENNA_MAX_FANOUT          16        /* readers per channel */

/* Native FS sample type — int16 mono frames of `samples_per_packet`. */
typedef int16_t ravenna_sample_t;

/* Default ring depth in *packets*. With 1 ms / 48 kHz that is 48
 * samples per packet; 1024 packets ≈ 1 s of audio. */
#define RAVENNA_RING_PACKETS_DEFAULT 1024

/* PTP timer name we depend on. */
#define RAVENNA_PTP_TIMER_NAME      "ptp"

/* Logging gates */
#define RAV_LOG_DEBUG(fmt, ...) \
	do { if (mod_ravenna_globals.debug) \
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, fmt, ##__VA_ARGS__); \
	} while (0)

/* ------------------------------------------------------------------
 *  Codec
 * ------------------------------------------------------------------ */
typedef enum {
	RAV_CODEC_L16 = 0,
	RAV_CODEC_L24,
	RAV_CODEC_L32
} ravenna_codec_t;

static inline const char *ravenna_codec_str(ravenna_codec_t c)
{
	switch (c) {
	case RAV_CODEC_L16: return "L16";
	case RAV_CODEC_L24: return "L24";
	case RAV_CODEC_L32: return "L32";
	default:            return "?";
	}
}

static inline int ravenna_codec_bytes(ravenna_codec_t c)
{
	switch (c) {
	case RAV_CODEC_L16: return 2;
	case RAV_CODEC_L24: return 3;
	case RAV_CODEC_L32: return 4;
	default:            return 0;
	}
}

/* ------------------------------------------------------------------
 *  Forward decls
 * ------------------------------------------------------------------ */
typedef struct ravenna_ring_s     ravenna_ring_t;
typedef struct ravenna_cursor_s   ravenna_cursor_t;
typedef struct ravenna_stream_s   ravenna_stream_t;
typedef struct ravenna_endpoint_s ravenna_endpoint_t;
typedef struct ravenna_session_s  ravenna_session_t;

/* ------------------------------------------------------------------
 *  SMPTE 2022-7 deduplication window
 *
 *  We keep a circular bitmap of recently-seen RTP sequence numbers.
 *  Window size must be a power of two. 2048 covers the standard
 *  2022-7 reorder depth (>>100 packets) with room to spare.
 * ------------------------------------------------------------------ */
#define RAVENNA_ST2022_WIN   2048     /* power of two */
#define RAVENNA_ST2022_MASK  (RAVENNA_ST2022_WIN - 1)

/* ------------------------------------------------------------------
 *  Stream — one multicast RTP flow (RX, TX, or both).
 * ------------------------------------------------------------------ */
struct ravenna_stream_s {
	char                 name[RAVENNA_MAX_NAME_LEN];

	/* Geometry */
	int                  channels;          /* 1..RAVENNA_MAX_CHANNELS */
	int                  sample_rate;       /* 48000, 96000, ...       */
	double               ptime_ms;          /* 0.125, 0.25, 1.0, ...    */
	int                  samples_per_packet;/* derived                  */

	/* Codec */
	ravenna_codec_t      rx_codec;
	ravenna_codec_t      tx_codec;
	int                  rtp_payload_type;  /* dynamic PT, e.g. 98     */

	/* RX side */
	switch_bool_t        rx_enabled;
	char                 rx_addr[RAVENNA_MAX_IP_LEN];
	int                  rx_port;
	ravenna_socket_t     rx_sock;
	ravenna_ring_t      *rx_rings[RAVENNA_MAX_CHANNELS]; /* one per ch */
	uint32_t             rx_last_ts;        /* RTP TS of last packet   */
	uint16_t             rx_last_seq;
	switch_bool_t        rx_seq_inited;
	uint64_t             rx_packets;
	uint64_t             rx_drops;
	uint64_t             rx_seq_gaps;

	/* TX side */
	switch_bool_t        tx_enabled;
	char                 tx_addr[RAVENNA_MAX_IP_LEN];
	int                  tx_port;
	ravenna_socket_t     tx_sock;
	struct sockaddr_in   tx_dest;
	ravenna_ring_t      *tx_rings[RAVENNA_MAX_CHANNELS]; /* per channel */
	uint32_t             tx_ssrc;
	uint16_t             tx_seq;
	uint32_t             tx_ts;
	switch_time_t        tx_next_due_us;    /* heap key                */
	uint64_t             tx_packets;

	/* Interface (multicast iface IP / Linux ifname) — path A (primary) */
	char                 iface[RAVENNA_MAX_IFACE_LEN];

	/* ------ SMPTE ST 2022-7 redundant path (optional) ------------- */
	switch_bool_t        st2022_7;             /* feature enabled       */

	/* Secondary interface — path B.  Falls back to `iface` if unset,
	 * but real 2022-7 protection requires a physically separate NIC. */
	char                 iface2[RAVENNA_MAX_IFACE_LEN];

	/* RX path 2 */
	char                 rx2_addr[RAVENNA_MAX_IP_LEN];
	int                  rx2_port;
	ravenna_socket_t     rx2_sock;

	/* TX path 2 */
	char                 tx2_addr[RAVENNA_MAX_IP_LEN];
	int                  tx2_port;
	ravenna_socket_t     tx2_sock;
	struct sockaddr_in   tx2_dest;

	/* Dedup table: tracks last RAVENNA_ST2022_WIN seq numbers.
	 * Slot [seq & MASK] holds the seq value seen (0xFF…FF = empty). */
	uint16_t             dedup_win[RAVENNA_ST2022_WIN];

	/* Stats for path 2 */
	uint64_t             rx2_packets;
	uint64_t             rx2_drops;
	uint64_t             rx_dedup_dropped;  /* packets discarded as dups */

	/* Mutex for config-time mutation only; hot path is lock-free. */
	switch_mutex_t      *mutex;
};

/* ------------------------------------------------------------------
 *  Endpoint — what gets dialled as ravenna/endpoint/<name>
 * ------------------------------------------------------------------ */
struct ravenna_endpoint_s {
	char               name[RAVENNA_MAX_NAME_LEN];

	ravenna_stream_t  *in_stream;
	int                inchan;        /* index into in_stream->rx_rings */

	ravenna_stream_t  *out_stream;
	int                outchan;       /* index into out_stream->tx_rings */

	switch_bool_t      multiple_listen; /* allow multiple RX sessions */
	int                active_rx_sessions;

	switch_mutex_t    *mutex;
};

/* ------------------------------------------------------------------
 *  Per-session state hung off a switch_core_session_t.
 * ------------------------------------------------------------------ */
struct ravenna_session_s {
	switch_core_session_t   *session;
	switch_channel_t        *channel;
	ravenna_endpoint_t      *endpoint;

	/* RX read cursor (NULL if endpoint is TX-only) */
	ravenna_cursor_t        *rx_cursor;

	/* Codec / framing as negotiated with FS core */
	int                      sample_rate;
	int                      codec_ms;
	int                      samples_per_frame;
	switch_codec_t           read_codec;
	switch_codec_t           write_codec;
	switch_frame_t           read_frame;
	uint8_t                  read_buf[SWITCH_RECOMMENDED_BUFFER_SIZE];

	/* Timer for read-pacing (when no RX stream attached) */
	switch_timer_t           read_timer;
	switch_bool_t            read_timer_inited;

	uint32_t                 flags;
};

/* ------------------------------------------------------------------
 *  Global module state
 * ------------------------------------------------------------------ */
typedef struct {
	switch_memory_pool_t       *pool;
	switch_endpoint_interface_t*endpoint_interface;
	switch_mutex_t             *mutex;

	/* Config defaults (overridden per-stream) */
	int                         default_sample_rate;
	double                      default_ptime_ms;
	int                         default_channels;
	ravenna_codec_t             default_rx_codec;
	ravenna_codec_t             default_tx_codec;
	int                         default_payload_type;
	char                        default_iface[RAVENNA_MAX_IFACE_LEN];
	char                        dialplan[64];
	char                        cid_name[64];
	char                        cid_num[64];
	char                        hold_file[256];

	/* Streams + endpoints (config-loaded; stable until reload) */
	switch_hash_t              *streams;     /* name -> ravenna_stream_t* */
	switch_hash_t              *endpoints;   /* name -> ravenna_endpoint_t* */

	/* Threads */
	switch_thread_t            *rx_thread;
	switch_thread_t            *tx_thread;
	volatile switch_bool_t      running;

	/* Cross-thread wake for RX (self-pipe / loopback UDP) */
	ravenna_socket_t            rx_wake_r;
	ravenna_socket_t            rx_wake_w;

	/* Diagnostics */
	switch_bool_t               debug;
} mod_ravenna_globals_t;

extern mod_ravenna_globals_t mod_ravenna_globals;

/* ------------------------------------------------------------------
 *  Cross-file entry points (RX/TX threads, config loader)
 * ------------------------------------------------------------------ */
void *SWITCH_THREAD_FUNC ravenna_rx_thread_run(switch_thread_t *t, void *obj);
void *SWITCH_THREAD_FUNC ravenna_tx_thread_run(switch_thread_t *t, void *obj);

switch_status_t ravenna_load_config(void);
void            ravenna_unload_config(void);
<<<<<<< Updated upstream
=======
switch_status_t ravenna_reload(switch_stream_handle_t *stream);
>>>>>>> Stashed changes

/* Stream lifecycle */
switch_status_t ravenna_stream_open_sockets(ravenna_stream_t *s);
void            ravenna_stream_close_sockets(ravenna_stream_t *s);

#endif /* MOD_RAVENNA_H */
