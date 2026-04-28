/**
 * mod_ivcore.h
 *
 * FreeSWITCH endpoint module for IVC/IVP intercom protocol.
 * Mirrors the IVCore C# stack (IvcConstants, IvpFrames, AudioPipeline)
 * in C, with a lock-free SPSC ring buffer per channel direction —
 * the same pattern used by mod_portaudio.
 *
 * Build target: FreeSWITCH 1.10+, C11, POSIX/Winsock sockets.
 */

#ifndef MOD_IVCORE_H
#define MOD_IVCORE_H

#include <switch.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Build-time limits
 * --------------------------------------------------------------------- */

#define MAX_IVC_CHANNELS     32      /**< Simultaneous IVC channel slots  */
#define MAX_IVC_PROFILES     64      /**< Named client profile slots       */
#define IVC_RING_BUFFER_SIZE 65536   /**< Per-channel ring buffer, must be power of 2 */
#define IVC_TCP_PORT         6001    /**< Default IVC card TCP login port  */
#define IVC_UDP_PORT         6001    /**< Default IVC card UDP media port  */
#define IVC_MAX_FRAME_BYTES  4096    /**< Largest possible IVP frame      */
#define IVC_PANEL_NAME_LEN   10      /**< Panel login name field length    */
#define IVC_MAC_ADDR_LEN     6       /**< MAC address field length         */
#define IVC_IPV4_LEN         4       /**< IPv4 address field length        */

/* -----------------------------------------------------------------------
 * IVC Network type  (matches Eclipse/IVC network quality setting)
 *
 * Controls G.722 packet duration and encryption/protection level.
 * Per CODEC.txt §1 (IVC32Port::ApplyNetworkType):
 *   SUPERLAN  → 8  ms packets, protection 0, no encryption
 *   LAN       → 16 ms packets, protection 0, no encryption  (default)
 *   WAN       → 24 ms packets, protection 0, AES encryption
 *   INTERNET  → 40 ms packets, protection 1, AES encryption
 * --------------------------------------------------------------------- */

typedef enum {
    IVC_NET_SUPERLAN  = 0,   /**< fpp=8,  protection=0 */
    IVC_NET_LAN       = 1,   /**< fpp=16, protection=0 (default) */
    IVC_NET_WAN       = 2,   /**< fpp=24, protection=0 */
    IVC_NET_INTERNET  = 3,   /**< fpp=40, protection=1 */
} ivcore_network_type_t;

/* -----------------------------------------------------------------------
 * IVP Codec flags  (matches IvpAudioCodec enum in IvcConstants.cs)
 * --------------------------------------------------------------------- */

typedef enum {
	IVP_CODEC_G7231   = 0x001,
	IVP_CODEC_GSM     = 0x002,
	IVP_CODEC_G711U   = 0x004,   /**< G.711 µ-law  8 kHz */
	IVP_CODEC_G711A   = 0x008,   /**< G.711 A-law  8 kHz */
	IVP_CODEC_MP3     = 0x010,
	IVP_CODEC_G722    = 0x020,   /**< G.722 wideband 16 kHz */
	IVP_CODEC_PCM     = 0x040,   /**< Raw 16-bit PCM */
	IVP_CODEC_LPC10   = 0x080,
	IVP_CODEC_G729    = 0x100,
	IVP_CODEC_SPEEX   = 0x200,
	IVP_CODEC_ILBC    = 0x400,
	IVP_CODEC_OPUS    = 0x800,
} ivp_audio_codec_t;

/* -----------------------------------------------------------------------
 * IVP Frame types  (matches IvpFrameType / IvpProtocolSubclass in IvcConstants.cs)
 * --------------------------------------------------------------------- */

typedef enum {
	IVP_FRAME_UNDEFINED = 0,
	IVP_FRAME_CONTROL   = 4,
	IVP_FRAME_PROTOCOL  = 6,
	IVP_FRAME_DATA      = 7,
} ivp_frame_type_t;

typedef enum {
	IVP_PROTO_NEW        = 1,
	IVP_PROTO_PING       = 2,
	IVP_PROTO_PONG       = 3,
	IVP_PROTO_ACK        = 4,
	IVP_PROTO_HANGUP     = 5,
	IVP_PROTO_REJECT     = 6,
	IVP_PROTO_ACCEPT     = 7,
	IVP_PROTO_LAG_REQ    = 11,
	IVP_PROTO_LAG_REPLY  = 12,
} ivp_proto_subclass_t;

typedef enum {
	IVP_CTRL_RINGING = 3,
	IVP_CTRL_ANSWER  = 4,
	IVP_CTRL_BUSY    = 5,
} ivp_ctrl_subclass_t;

/* -----------------------------------------------------------------------
 * IVP Information Element keys  (matches IvpIeKey in IvcConstants.cs)
 * --------------------------------------------------------------------- */

typedef enum {
	IVP_IE_CALLED_NUMBER    = 1,
	IVP_IE_CALLING_NUMBER   = 2,
	IVP_IE_CALLING_NAME     = 4,
	IVP_IE_CALLED_CONTEXT   = 5,
	IVP_IE_USERNAME         = 6,
	IVP_IE_CAPABILITY       = 8,
	IVP_IE_FORMAT           = 9,
	IVP_IE_VERSION          = 12,
	IVP_IE_DNID             = 13,
	IVP_IE_CAUSE            = 22,
	IVP_IE_PROVISIONING     = 29,
	IVP_IE_SAMPLING_RATE    = 41,
	IVP_IE_CAUSE_CODE       = 42,
	IVP_IE_ENCRYPTION_KEY   = 44,
	IVP_IE_AUTH_KEY         = 45,
	IVP_IE_PROTECTION_LEVEL = 52,
	IVP_IE_USER_ID          = 53,
	IVP_IE_BIT_RATE         = 54,
	IVP_IE_DISPLAY_NAME     = 55,
	IVP_IE_HTTP_PORT        = 56,
	IVP_IE_IS_HTTPS         = 57,
} ivp_ie_key_t;

/* -----------------------------------------------------------------------
 * IVP Protocol Frame Header  (14 bytes, big-endian wire format)
 * Matches IvpProtocolFrameHeader in IvpFrames.cs
 * --------------------------------------------------------------------- */

typedef struct {
	uint16_t src_call_number;   /**< 15-bit; bit 15 = protocol flag (always 1) */
	uint16_t dst_call_number;   /**< 15-bit; bit 15 = resent flag              */
	bool     is_resent;
	uint16_t src_call_number2;  /**< Mirror of src_call_number                 */
	uint32_t timestamp;
	uint8_t  out_sequence;
	uint8_t  in_sequence;
	ivp_frame_type_t   frame_type;
	uint8_t            subclass;
} ivp_proto_header_t;

#define IVP_PROTO_HEADER_SIZE 14

/* -----------------------------------------------------------------------
 * IVP Media Frame Header  (variable, big-endian)
 * Matches IvpMediaFrameHeader in IvpFrames.cs
 * --------------------------------------------------------------------- */

typedef struct {
	uint16_t src_call_number;       /**< 15-bit; bit 15 = 0 for media frames  */
	uint8_t  recovery_sequence;
	uint8_t  redundancy_layers;     /**< 0-15                                  */
	uint16_t media_length;          /**< payload bytes (0-4095)                */
	uint16_t media_sequence_number;
	uint8_t  src_nq;
	uint8_t  src_type;
	uint16_t src_free;
	uint32_t src_user_id;
	uint16_t signal_level;
	uint8_t  dst_call_number_high;
	uint8_t  codec;                 /**< ivp_audio_codec_t bitmask (1 bit set) */
} ivp_media_header_t;

#define IVP_MEDIA_HEADER_SIZE 17

/* -----------------------------------------------------------------------
 * TCP Login structures  (matches LoginMessages.cs)
 * --------------------------------------------------------------------- */

typedef struct {
	uint8_t  mac_address[IVC_MAC_ADDR_LEN];
	uint8_t  panel_name[IVC_PANEL_NAME_LEN];
	uint8_t  network_connection_type;   /**< NetworkConnectionType enum */
	uint8_t  version_major;
	uint8_t  version_minor;
	uint8_t  version_patch;
	uint16_t local_udp_port;            /**< big-endian on wire         */
} ivc_login_message_t;

typedef struct {
	uint8_t  server_ip[IVC_IPV4_LEN];
	uint16_t server_udp_port;           /**< big-endian on wire         */
	uint8_t  result_code;
} ivc_login_response_t;

/* -----------------------------------------------------------------------
 * Lock-free SPSC ring buffer (matches AudioPipeline.cs bounded channel)
 *
 * Uses a power-of-2 size so masking replaces modulo.
 * Only one thread writes (IVP receive loop) and one thread reads
 * (FreeSWITCH read_frame callback) — no mutex required.
 * volatile uint32_t provides sufficient ordering on x86/x64 for SPSC.
 * --------------------------------------------------------------------- */

typedef struct {
	uint8_t  data[IVC_RING_BUFFER_SIZE];
	volatile uint32_t head;             /**< Write position (producer)   */
	volatile uint32_t tail;             /**< Read  position (consumer)   */
} ivcore_ring_buffer_t;

/** Returns number of bytes available to read. */
static inline uint32_t ring_available(const ivcore_ring_buffer_t *rb)
{
	uint32_t h = rb->head;
	uint32_t t = rb->tail;
	return (h - t) & (IVC_RING_BUFFER_SIZE - 1);
}

/** Returns free space available to write. */
static inline uint32_t ring_free(const ivcore_ring_buffer_t *rb)
{
	return (IVC_RING_BUFFER_SIZE - 1) - ring_available(rb);
}

/**
 * Write up to len bytes into the ring buffer.
 * Drops oldest bytes (advances tail) when full, matching
 * BoundedChannelFullMode.DropOldest from AudioPipeline.cs.
 * Returns bytes actually written (always == len unless len > buffer size).
 */
static inline uint32_t ring_write(ivcore_ring_buffer_t *rb,
								  const uint8_t *src, uint32_t len)
{
	uint32_t free_space, h, mask, first_chunk;
	if (len == 0) return 0;
	if (len > IVC_RING_BUFFER_SIZE - 1) len = IVC_RING_BUFFER_SIZE - 1;

	free_space = ring_free(rb);
	if (len > free_space) {
		uint32_t drop = len - free_space;
		rb->tail = (rb->tail + drop) & (IVC_RING_BUFFER_SIZE - 1);
	}

	h = rb->head;
	mask = IVC_RING_BUFFER_SIZE - 1;
	first_chunk = IVC_RING_BUFFER_SIZE - (h & mask);
	if (first_chunk >= len) {
		memcpy(&rb->data[h & mask], src, len);
	} else {
		memcpy(&rb->data[h & mask], src, first_chunk);
		memcpy(&rb->data[0], src + first_chunk, len - first_chunk);
	}
	rb->head = h + len;
	return len;
}

/**
 * Read up to len bytes from the ring buffer.
 * Returns number of bytes actually read.
 */
static inline uint32_t ring_read(ivcore_ring_buffer_t *rb,
								 uint8_t *dst, uint32_t len)
{
	uint32_t avail, t, mask, first_chunk;
	avail = ring_available(rb);
	if (len > avail) len = avail;
	if (len == 0) return 0;

	t = rb->tail;
	mask = IVC_RING_BUFFER_SIZE - 1;
	first_chunk = IVC_RING_BUFFER_SIZE - (t & mask);
	if (first_chunk >= len) {
		memcpy(dst, &rb->data[t & mask], len);
	} else {
		memcpy(dst, &rb->data[t & mask], first_chunk);
		memcpy(dst + first_chunk, &rb->data[0], len - first_chunk);
	}
	rb->tail = t + len;
	return len;
}

/** Reset ring buffer to empty. */
static inline void ring_reset(ivcore_ring_buffer_t *rb)
{
	rb->head = 0;
	rb->tail = 0;
}

/* -----------------------------------------------------------------------
 * IVP call state
 * --------------------------------------------------------------------- */

typedef enum {
	IVC_STATE_IDLE        = 0,
	IVC_STATE_CONNECTING  = 1,
	IVC_STATE_RINGING     = 2,
	IVC_STATE_UP          = 3,
	IVC_STATE_HANGUP      = 4,
} ivcore_call_state_t;

/* -----------------------------------------------------------------------
 * Connection parameters (fed to ivp_transport_connect)
 * --------------------------------------------------------------------- */

typedef struct {
	char     server_ip[64];
	int      tcp_port;
	int      udp_port;

	char     username[128];
	char     calling_name[128];
	char     display_name[128];
	char     called_number[64];
	char     called_context[64];
	char     version_string[32];

	uint32_t codec_family;       /**< ivp_audio_codec_t bitmask capability */
	uint32_t codec_format;       /**< Negotiated single codec flag          */
	uint16_t sampling_rate;
	uint16_t frame_size;          /**< Provisioning IE: samples per frame    */
	uint16_t frame_time;          /**< Provisioning IE: ms per frame         */
	uint16_t frames_per_packet;   /**< Provisioning IE: frames per packet    */
	uint8_t  protection_level;
	uint32_t user_id;

	char     encryption_key[256];
	char     auth_key[256];
	char     password[128];      /**< Port password, used to derive media key */
	char     account[128];       /**< Bare account name (no prefix) for key derivation */

	/**
	 * Device type — controls TCP login format and IVP username prefix.
	 * Copied from ivcore_port_t.device_type; defaults to "lqsip".
	 */
	char     device_type[32];

	} ivcore_conn_params_t;

/* -----------------------------------------------------------------------
 * Per-channel structure
 *
 * Each active FreeSWITCH channel that routes through IVCore owns one of
 * these.  The IVP receive thread writes encoded audio into rx_ring;
 * read_frame drains it.  write_frame calls ivp_send_media() directly.
 * --------------------------------------------------------------------- */

typedef struct ivcore_channel_s {
	/* FreeSWITCH handle */
	switch_core_session_t *session;
	switch_channel_t      *channel;
	char                   session_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1]; /**< Copy of session UUID for safe cross-thread lookup */
	switch_codec_t         read_codec;
	switch_codec_t         write_codec;
	switch_frame_t         read_frame;
	uint8_t                read_frame_data[IVC_MAX_FRAME_BYTES];

	/* Network */
	ivcore_conn_params_t   params;
	int                    tcp_sock;     /**< TCP login socket (closed after login) */
	int                    udp_sock;     /**< UDP media socket                      */
	struct sockaddr_in     remote_addr;

	/* IVP call state */
	ivcore_call_state_t    call_state;
	uint16_t               local_call_number;
	uint16_t               local_udp_port;      /**< Bound local UDP port, sent in TCP login */
	uint16_t               remote_call_number;
	uint8_t                out_sequence;
	uint8_t                in_sequence;
	uint32_t               timestamp_base;
	uint16_t               media_sequence_out;

	/* Active codec */
	ivp_audio_codec_t      active_codec;
	int                    sample_rate;  /**< 8000 or 16000 Hz */

	/* Receive ring buffer — ivp_recv_loop writes, channel_read_frame reads. */
	ivcore_ring_buffer_t   rx_ring;      /**< IVP → FreeSWITCH (inbound audio)  */

	/* Silence frame sent by the TX keepalive thread when FS is not writing. */
	uint8_t                tx_silence_buf[IVC_MAX_FRAME_BYTES];

	/* Receive / Transmit threads */
	switch_thread_t       *rx_thread;
	switch_thread_t       *tx_thread;  /**< Dedicated TX pacer thread — owns pacing & ivp_send_media */
	volatile switch_bool_t running;

	/* Silence padding when rx_ring is starved */
	uint32_t               ptime_ms;     /**< IVP wire cadence in ms (e.g. 8 for lqsip G.722) */

	/* Soft timer used to pace channel_read_frame at exactly ptime_ms per tick.
	 * Without this the session thread drains a buffered rx_ring at CPU speed,
	 * causing audio to play back many times faster than real-time. */
	switch_timer_t         read_timer;
	switch_bool_t          timer_started; /**< TRUE once read_timer is initialised */

	/** Timestamp (switch_micro_time_now) of the last real audio packet sent by
	 *  channel_write_frame.  The recv-loop silence keep-alive checks this to
	 *  avoid doubling the media rate when FreeSWITCH is actively sending audio. */
	volatile switch_time_t last_write_us;


	/* HDLC data-link state for the IVP Data channel (type=7 sub=1).
	 * Forward-declared; defined in ivp_hdlc.h. */
	struct {
		switch_bool_t link_up;
		uint8_t       v_s;
		uint8_t       v_r;
		switch_time_t last_rr_us;
		switch_time_t last_sabme_us; /**< timestamp of most recent SABME for debounce */
		uint32_t      sabme_count;
		uint32_t      iframe_count;
	} hdlc;

	/* SIP-telephony DPI state (carried inside HDLC I-frames).
	 * Mirrors SipCallState in SIP_TELEPHONY_DPI.md:
	 *   0=OnHook 1=OnHookAllocated 2=ConnectingOut 3=ConnectedOut
	 *   4=ConnectingIn 5=ConnectedIn
	 * Default to OnHookAllocated once the IVP call is UP so 0xF0 replies
	 * are sensible before any dial activity. */
	uint8_t                dpi_state;
	char                   dpi_dial_buffer[160];   /**< Last 0xF1 dial string seen */
	switch_bool_t          dpi_init_sent;          /**< TRUE after PanelTypeReply init sequence sent */
	uint8_t                dpi_key_status_replies; /**< Count of 0x8B KeyStatusReply messages sent */
	volatile switch_bool_t dpi_dial_pending;       /**< TRUE when a complete 0xF1 dial string is ready to route */
	switch_bool_t          dpi_dial_cont_active;   /**< TRUE while accumulating a multi-packet 0xF1 sequence (cont=1) */
	volatile switch_bool_t hdlc_reset_pending;     /**< Set by HDLC SABME handler; cleared by recv loop to re-sync IVP in_sequence */

	/* ---------------------------------------------------------------
	 * Dial diagnostics — populated by ivp_dpi.c as dial packets arrive.
	 * Stamped as channel variables (ivc_*) by channel_on_exchange_media()
	 * immediately before switch_ivr_session_transfer() so the FreeSWITCH
	 * dialplan, CDR, and ESL event listeners can inspect the full
	 * context of every dial-out without needing to parse log files.
	 * --------------------------------------------------------------- */
	uint8_t        diag_dial_source;        /**< DPI msg ID that triggered routing: 0xF1 or 0xF5 */
	char           diag_dial_raw[160];      /**< Raw dial string exactly as received from matrix  */
	uint32_t       diag_dial_cont_packets;  /**< Number of cont=1 0xF1 packets accumulated        */
	switch_time_t  diag_dial_first_us;      /**< switch_micro_time_now() of first 0xF1 received   */
	switch_time_t  diag_dial_final_us;      /**< switch_micro_time_now() of the routing packet     */

	/* Stored HDLC send callback — set in ivp_transport.c when the first IVP
	 * Data (HDLC) frame is received inside ivp_recv_loop.  Allows proactive
	 * DPI sends (e.g. 0xF1 ConnectReply on SIP answer, 0x93 KeyStatusUpdate
	 * on hangup) from mod_ivcore.c without access to the static
	 * ivp_send_data_frame.
	 *
	 * MUST be initialised to NULL on channel allocation (ivcore_channel_alloc).
	 * Every call site in mod_ivcore.c MUST guard with a NULL check — this
	 * pointer is not valid until the HDLC link is established, and SIP
	 * state-machine callbacks (answer, hangup) can fire before that point.
	 * Calling an uninitialised pointer causes an 0xC0000005 access violation
	 * in debug builds (MSVC fills unset memory with 0xCC). */
	switch_status_t      (*dpi_send_cb)(struct ivcore_channel_s *ch,
										const uint8_t *frame, int frame_len);

	/* Autoconnect respawn tracking.
	 * When is_autoconnect is TRUE, channel_on_hangup will spawn a fresh
	 * standing IVP session for the same card/port so the matrix can dial
	 * out again without a module reload. */
	switch_bool_t          is_autoconnect;
	int                    autoconnect_card_idx;
	int                    autoconnect_port_idx;

	/* Set by 0xF4 DisconnectOutgoing handler when the matrix initiates the
	 * hangup.  channel_on_hangup checks this flag to skip sending
	 * IVP_PROTO_HANGUP back to the card — the IVP link stays alive so the
	 * respawn can inherit the existing sockets without a TCP re-login. */
	switch_bool_t          dpi_initiated_hangup;

	/* Linked list for global channel table */
	struct ivcore_channel_s *next;
} ivcore_channel_t;

/* -----------------------------------------------------------------------
 * IVC Port  (one login credential set on a card)
 *
 * Maps to a <port name="..."> element inside a <card>.
 * A port has a name used in the dial string and the username/password
 * used for the IVC login handshake.
 * --------------------------------------------------------------------- */

#define MAX_PORTS_PER_CARD 64

typedef struct {
	char name[64];          /**< Port name used in the dial string, e.g. "intercom1" */
	char username[128];     /**< IVC account username, e.g. "test"                   */
	char password[128];     /**< IVC account password, e.g. "test"                   */
	/**
	 * Device type string controlling login format and IVP username prefix.
	 * Supported values:
	 *   "lqsip"  — LQ SIP telephony interface (default)
	 *   "lq4"   — LQ 4-wire interface
	 *   "panel" — Standard V-Series / softpanel login
	 *   "mobile"— Station-IC / Agent-IC mobile panel login
	 */
	char device_type[32];

	/** If true, TCP/UDP connection is established at module load. */
	switch_bool_t autoconnect;

	/** Number to pass in the IVP NEW frame when autoconnecting (default "0"). */
	char autoconnect_number[64];
} ivcore_port_t;

/* -----------------------------------------------------------------------
 * IVC Card  (one physical IVC card / Eclipse matrix)
 *
 * Maps to a <card name="..."> element in ivcore.conf.xml.
 * Holds the network address, audio settings, and all ports on that card.
 * --------------------------------------------------------------------- */

typedef struct {
	char name[64];              /**< Card name, e.g. "main", "studio-a"       */

	/* Network — set once per card */
	char server_ip[64];         /**< IVC card IP address                      */
	int  tcp_port;              /**< TCP login port (default 6001)            */
	int  udp_port;              /**< UDP media port hint (default 6001)       */

	/* Audio — set once per card */
	char                  codec;          /**< 'u'=µ-law 'a'=A-law 'g'=G.722 'p'=PCM  */
	int                   ptime_ms;       /**< Packetisation time in ms (default 20)    */
	ivcore_network_type_t network_type;   /**< superlan/lan/wan/internet (default: lan) */

	/* FreeSWITCH routing */
	char context[64];           /**< Dialplan context for inbound calls       */

	/* Optional security (card-level; applies to all ports) */
	char encryption_key[256];
	char auth_key[256];

	/** Default autoconnect setting inherited by ports that do not override it. */
	switch_bool_t autoconnect;

	/* Ports on this card */
	ivcore_port_t ports[MAX_PORTS_PER_CARD];
	int           port_count;
} ivcore_card_t;

/* -----------------------------------------------------------------------
 * Module globals  (defined in mod_ivcore.cpp)
 * --------------------------------------------------------------------- */

typedef struct {
	switch_memory_pool_t        *pool;
	switch_endpoint_interface_t *endpoint_interface;
	switch_mutex_t              *mutex;
	ivcore_channel_t            *channels[MAX_IVC_CHANNELS];
	int                          channel_count;

	/* Card table — loaded from ivcore.conf.xml <cards> */
	ivcore_card_t                cards[MAX_IVC_PROFILES];  /* reuse limit constant */
	int                          card_count;

	/** FALSE once mod_ivcore_shutdown begins — retry threads check this. */
	switch_bool_t                running;

	/** TRUE to emit per-frame HDLC/DPI/transport DEBUG traces.
	 *  Toggled at runtime via "ivc debug on|off".
	 *  Also readable from ivcore.conf.xml: <param name="debug" value="true"/> */
	switch_bool_t                debug;
} ivcore_globals_t;

extern ivcore_globals_t ivcore_globals;

/* -----------------------------------------------------------------------
 * Function prototypes (implemented across the .cpp files)
 * --------------------------------------------------------------------- */

/* ivp_transport.cpp */
switch_status_t ivp_tcp_login(ivcore_channel_t *ch);
switch_status_t ivp_udp_open(ivcore_channel_t *ch);
switch_status_t ivp_send_new(ivcore_channel_t *ch);
switch_status_t ivp_send_ack(ivcore_channel_t *ch, uint16_t dst_call_number,
							  uint8_t in_seq, uint8_t out_seq, uint32_t echo_ts);
switch_status_t ivp_send_hangup(ivcore_channel_t *ch);
switch_status_t ivp_send_media(ivcore_channel_t *ch,
								const uint8_t *payload, int payload_len);
void            ivp_transport_close(ivcore_channel_t *ch);

/**
 * ivp_transport_steal: extract the live TCP and UDP socket descriptors from
 * a channel without closing them, then zero the channel's socket fields.
 * Used by the autoconnect respawn path so a new session can inherit an
 * existing IVP link without a full TCP re-login.
 *
 * @param ch       Channel to steal sockets from.
 * @param tcp_out  Receives the TCP socket fd (-1 if not open).
 * @param udp_out  Receives the UDP socket fd (-1 if not open).
 */
void            ivp_transport_steal(ivcore_channel_t *ch, int *tcp_out, int *udp_out);

/**
 * ivcore_inherit_t: carries the stolen sockets and card/port identity across
 * the session-teardown boundary so spawn_autoconnect_session_inherited() can
 * re-use them in the new FreeSWITCH session.
 */
typedef struct {
	int tcp_sock;           /**< Stolen TCP socket (or -1) */
	int udp_sock;           /**< Stolen UDP socket (or -1) */
	int card_idx;           /**< ivcore_globals.cards[] index */
	int port_idx;           /**< card->ports[] index */
} ivcore_inherit_t;

void           *ivp_recv_loop(switch_thread_t *thread, void *obj);
void           *ivp_tx_loop(switch_thread_t *thread, void *obj);

/* mod_ivcore.c — card/port lookup */
const ivcore_card_t *ivcore_card_find(const char *card_name);
const ivcore_port_t *ivcore_port_find(const char *card_name, const char *port_name,
                                       const ivcore_card_t **card_out);

/* mod_ivcore.c — channel lifecycle */
ivcore_channel_t *ivcore_channel_alloc(switch_core_session_t *session,
                                        const ivcore_card_t *card,
                                        const ivcore_port_t *port);
void              ivcore_channel_free(ivcore_channel_t *ch);

/* -----------------------------------------------------------------------
 * Conditional per-frame debug logging
 *
 * Use IVC_LOG_DEBUG(...) for high-volume traces (every HDLC frame, every
 * ACK, every DPI message).  They are suppressed unless ivcore_globals.debug
 * is TRUE so they don't pollute the FreeSWITCH log during normal operation.
 * Toggle at runtime: "ivc debug on" / "ivc debug off"
 * --------------------------------------------------------------------- */
#define IVC_LOG_DEBUG(fmt, ...) \
	do { \
		if (ivcore_globals.debug == SWITCH_TRUE) { \
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, fmt, ##__VA_ARGS__); \
		} \
	} while (0)

#endif /* MOD_IVCORE_H */
