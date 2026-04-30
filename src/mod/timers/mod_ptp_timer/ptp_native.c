/*
 * ptp_native.c
 *
 * Built-in PTPv2 (IEEE 1588-2008) slave client.  See ptp_native.h for
 * scope and design choices.
 *
 * Wire format references throughout cite IEEE 1588-2008 sections.
 */

#include "ptp_native.h"
#include "mod_ptp_timer.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  typedef SOCKET ptpn_sock_t;
  #define PTPN_INVALID_SOCK   INVALID_SOCKET
  #define PTPN_CLOSESOCK(s)   closesocket(s)
  #define ptpn_errno          WSAGetLastError()
#else
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <sys/ioctl.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <net/if.h>
  #include <ifaddrs.h>
  #include <unistd.h>
  #include <errno.h>
  typedef int ptpn_sock_t;
  #define PTPN_INVALID_SOCK   (-1)
  #define PTPN_CLOSESOCK(s)   close(s)
  #define ptpn_errno          errno
#endif

/* =====================================================================
 *  Wire constants
 * =================================================================== */

#define PTP_EVENT_PORT      319
#define PTP_GENERAL_PORT    320
#define PTP_MCAST_DEFAULT   "224.0.1.129"   /* default scope            */
#define PTP_MCAST_PEER      "224.0.0.107"   /* peer-delay (unused here) */

/* PTP message types (header.msg_type_transport low nibble) */
#define PTP_MSG_SYNC                0x0
#define PTP_MSG_DELAY_REQ           0x1
#define PTP_MSG_PDELAY_REQ          0x2
#define PTP_MSG_PDELAY_RESP         0x3
#define PTP_MSG_FOLLOW_UP           0x8
#define PTP_MSG_DELAY_RESP          0x9
#define PTP_MSG_PDELAY_RESP_FU      0xA
#define PTP_MSG_ANNOUNCE            0xB
#define PTP_MSG_SIGNALING           0xC
#define PTP_MSG_MANAGEMENT          0xD

#define PTP_VERSION                 2
#define PTP_HEADER_LEN              34

/* flagField bits */
#define PTP_FLAG_TWO_STEP           0x0200    /* byte1 bit1 in network order */

/* Tunables */
#define PTPN_MAX_MASTERS            8
#define PTPN_RX_BUFLEN              1500
#define PTPN_DELAY_REQ_INTERVAL_US  1000000   /* 1 Hz fallback           */
#define PTPN_HOUSEKEEP_INTERVAL_US  1000000
#define PTPN_GM_TIMEOUT_FACTOR      3         /* expire after 3× announce */
#define PTPN_DEFAULT_LOG_ANNOUNCE   1         /* 2^1 = 2 s                */
#define PTPN_SERVO_LOCK_THRESH_NS   500000    /* legacy |Δoffset| bound (unused) */
#define PTPN_SERVO_DELAY_STABLE_NS  250000    /* |Δpath_delay| < 250 µs ⇒ stable */
#define PTPN_SERVO_DELAY_MAX_NS     10000000  /* path_delay sanity ceiling: 10 ms */
#define PTPN_SERVO_LOCK_COUNT       3         /* successive stable samples       */
#define PTPN_SERVO_HOLDOVER_NS      100000000 /* hard sanity reset clamp  */
#define PTPN_DREQ_AUTO_UNICAST_AFTER 30       /* unanswered TX before AUTO->UNICAST (~30s @ 1Hz) */

/* =====================================================================
 *  Wire structs (all multi-byte fields are big-endian on the wire)
 * =================================================================== */

#pragma pack(push, 1)

typedef struct {
	uint8_t  v[8];
} ptp_clock_id_t;

typedef struct {
	ptp_clock_id_t clock;
	uint16_t       port_number;       /* be */
} ptp_port_id_t;

typedef struct {
	uint8_t  msg_type_transport;       /* hi nibble = transportSpecific */
	uint8_t  version_ptp;              /* lo nibble = versionPTP        */
	uint16_t message_length;           /* be                            */
	uint8_t  domain_number;
	uint8_t  reserved1;
	uint16_t flag_field;               /* be                            */
	uint64_t correction_field;         /* be, ns × 2^16                 */
	uint32_t reserved2;
	ptp_port_id_t source_port;
	uint16_t sequence_id;              /* be                            */
	uint8_t  control_field;
	uint8_t  log_message_interval;
} ptp_header_t;

/* PTP timestamp = secondsField (6 B) + nanosecondsField (4 B), big endian */
typedef struct {
	uint8_t  seconds[6];               /* be */
	uint32_t nanoseconds;              /* be */
} ptp_timestamp_t;

/* Announce body (after header) — IEEE 1588-2008 §13.5 */
typedef struct {
	ptp_timestamp_t origin_ts;
	int16_t  current_utc_offset;       /* be */
	uint8_t  reserved;
	uint8_t  grandmaster_priority1;
	uint8_t  gm_clock_quality[4];      /* class, accuracy, varianceHi, varianceLo */
	uint8_t  grandmaster_priority2;
	ptp_clock_id_t grandmaster_identity;
	uint16_t steps_removed;            /* be */
	uint8_t  time_source;
} ptp_announce_t;

/* Sync / Follow_Up body */
typedef struct {
	ptp_timestamp_t origin_ts;
} ptp_sync_t;

/* Delay_Req body */
typedef struct {
	ptp_timestamp_t origin_ts;
} ptp_delay_req_t;

/* Delay_Resp body */
typedef struct {
	ptp_timestamp_t receive_ts;
	ptp_port_id_t   requesting_port;
} ptp_delay_resp_t;

#pragma pack(pop)

/* =====================================================================
 *  Master tracking
 * =================================================================== */

typedef struct {
	switch_bool_t   in_use;
	ptp_port_id_t   port;              /* network-order */
	uint8_t         priority1;
	uint8_t         clock_class;
	uint8_t         clock_accuracy;
	uint16_t        offset_scaled_log_variance;  /* host order */
	uint8_t         priority2;
	ptp_clock_id_t  grandmaster_id;
	uint16_t        steps_removed;
	int8_t          log_announce_interval;
	uint8_t         transport_specific;        /* hi nibble of msg_type_transport */
	uint8_t         domain;
	switch_time_t   last_announce_us;
	uint32_t        source_addr;               /* IPv4 of last RX from this master, network order */
} ptpn_master_t;

/* =====================================================================
 *  Engine
 * =================================================================== */

struct ptp_native_s {
	switch_memory_pool_t *pool;
	ptp_native_cfg_t      cfg;
	uint32_t              iface_addr;     /* network order, IPv4         */
	char                  iface_desc[64]; /* for source_desc             */

	ptpn_sock_t           ev_sock;        /* UDP/319                     */
	ptpn_sock_t           gen_sock;       /* UDP/320                     */
	ptpn_sock_t           tx_sock;        /* TX-only, bound to iface_addr:0 for Delay_Req egress */
	ptp_clock_id_t        local_clock_id;
	uint16_t              seq_delay_req;

	switch_thread_t      *thread;
	switch_bool_t         running;
	switch_mutex_t       *mutex;          /* protects everything below   */

	/* Master table */
	ptpn_master_t         masters[PTPN_MAX_MASTERS];
	int                   active_idx;     /* -1 = none                   */

	/* Live timing — set under mutex */
	switch_bool_t         locked;
	int                   lock_consecutive;
	int64_t               offset_ns;          /* master - local          */
	int64_t               path_delay_ns;
	uint32_t              servo_steps;
	switch_time_t         last_sync_us;
	switch_time_t         last_delay_req_us;
	switch_bool_t         servo_have_prev;    /* servo seen at least one sample */
	int64_t               prev_offset_ns;     /* legacy, kept for diagnostics    */
	int64_t               prev_delay_ns;      /* for path-delay stability lock   */

	/* Two-step Sync state per active master */
	switch_bool_t         pending_sync;
	uint16_t              pending_sync_seq;
	int64_t               pending_t2_ns;      /* RX time of Sync         */
	int64_t               pending_t1_ns;      /* preciseOriginTimestamp  */
	switch_bool_t         sync_profile_logged; /* one-shot NOTICE per active GM */

	/* Delay_Resp matching */
	uint16_t              outstanding_dreq_seq;
	int64_t               outstanding_dreq_t3_ns;
	int64_t               outstanding_t1_ns;   /* snapshot of t1 at send time */
	int64_t               outstanding_t2_ns;   /* snapshot of t2 at send time */

	/* Delay_Req delivery state (for AUTO fallback) */
	switch_bool_t         effective_dreq_unicast;
	uint32_t              dreq_unanswered;
	switch_bool_t         dreq_proven;        /* at least one Delay_Resp matched */
	switch_bool_t         dreq_hex_dumped;    /* one-shot raw dump of first TX */

	/* Diagnostics counters (debug visibility into the message flow). */
	uint64_t              rx_announce;
	uint64_t              rx_sync;
	uint64_t              rx_follow_up;
	uint64_t              rx_delay_resp;
	uint64_t              rx_other;
	uint64_t              rx_dropped_domain;
	uint64_t              rx_dropped_self;
	uint64_t              rx_dropped_no_active;
	uint64_t              rx_dropped_wrong_master;       /* Sync wrong source */
	uint64_t              rx_dropped_dresp_wrong_master; /* Delay_Resp wrong source */
	uint64_t              rx_dropped_dreq_seq;
	uint64_t              rx_dropped_dreq_port;
	uint64_t              tx_delay_req;
	uint64_t              tx_delay_req_failed;
	uint64_t              rx_pdelay_req;
	uint64_t              rx_pdelay_resp;

	/* Raw socket-level counters (before any PTP parsing/filtering).
	 * These tell us whether traffic is even reaching us on each port. */
	uint64_t              rx_raw_event_pkts;     /* UDP/319 packets recv'd */
	uint64_t              rx_raw_event_bytes;
	uint64_t              rx_raw_general_pkts;   /* UDP/320 packets recv'd */
	uint64_t              rx_raw_general_bytes;
	/* Per-message-type counters seen on the general socket only.
	 * If rx_gen_delay_resp stays 0 while rx_gen_follow_up climbs, the
	 * GM is not emitting Delay_Resp toward us (or it's being filtered). */
	uint64_t              rx_gen_announce;
	uint64_t              rx_gen_follow_up;
	uint64_t              rx_gen_delay_resp;
	uint64_t              rx_gen_other;

	switch_time_t         dbg_next_summary_us;
};

/* =====================================================================
 *  Helpers
 * =================================================================== */

static inline uint16_t be16(uint16_t v)
{
	return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t be32(uint32_t v)
{
	return ((v & 0xFF000000U) >> 24) |
		   ((v & 0x00FF0000U) >> 8)  |
		   ((v & 0x0000FF00U) << 8)  |
		   ((v & 0x000000FFU) << 24);
}

static inline uint64_t be48_to_u64(const uint8_t b[6])
{
	return ((uint64_t)b[0] << 40) | ((uint64_t)b[1] << 32) |
		   ((uint64_t)b[2] << 24) | ((uint64_t)b[3] << 16) |
		   ((uint64_t)b[4] << 8)  |  (uint64_t)b[5];
}

static int64_t ptp_ts_to_ns(const ptp_timestamp_t *ts)
{
	uint64_t s  = be48_to_u64(ts->seconds);
	uint32_t ns = be32(ts->nanoseconds);
	return (int64_t)(s * 1000000000ULL + ns);
}

static int64_t local_now_ns(void)
{
	return (int64_t)switch_micro_time_now() * 1000;
}

static const char *fmt_clock_id(const ptp_clock_id_t *id, char *buf, size_t cap)
{
	switch_snprintf(buf, cap, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
					id->v[0], id->v[1], id->v[2], id->v[3],
					id->v[4], id->v[5], id->v[6], id->v[7]);
	return buf;
}

/* IEEE 1588-2008 §9.3.4 dataset comparison.
 * Returns <0 if A is better, >0 if B is better, 0 if equivalent.
 * Lower priority1, then better clockQuality, then lower priority2,
 * then numerically lower clockIdentity wins. */
static int bmca_compare(const ptpn_master_t *a, const ptpn_master_t *b)
{
	if (a->priority1 != b->priority1) {
		return (int)a->priority1 - (int)b->priority1;
	}
	if (a->clock_class != b->clock_class) {
		return (int)a->clock_class - (int)b->clock_class;
	}
	if (a->clock_accuracy != b->clock_accuracy) {
		return (int)a->clock_accuracy - (int)b->clock_accuracy;
	}
	if (a->offset_scaled_log_variance != b->offset_scaled_log_variance) {
		return (int)a->offset_scaled_log_variance - (int)b->offset_scaled_log_variance;
	}
	if (a->priority2 != b->priority2) {
		return (int)a->priority2 - (int)b->priority2;
	}
	return memcmp(a->grandmaster_id.v, b->grandmaster_id.v, 8);
}

/* =====================================================================
 *  Network helpers
 * =================================================================== */

static uint32_t resolve_iface_ipv4(const char *iface)
{
	struct in_addr ina;
	if (zstr(iface)) return htonl(INADDR_ANY);

	/* Try dotted-quad first. */
	if (inet_pton(AF_INET, iface, &ina) == 1) {
		return ina.s_addr;
	}

#ifndef _WIN32
	{
		struct ifreq ifr;
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s >= 0) {
			memset(&ifr, 0, sizeof(ifr));
			switch_snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
			if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
				close(s);
				return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
			}
			close(s);
		}
	}
#else
	{
		/* Look up by adapter friendly-name or description. */
		ULONG cb = 16 * 1024;
		IP_ADAPTER_ADDRESSES *aa = (IP_ADAPTER_ADDRESSES *)malloc(cb);
		if (aa && GetAdaptersAddresses(AF_INET,
				GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST |
				GAA_FLAG_SKIP_FRIENDLY_NAME, NULL, aa, &cb) == NO_ERROR) {
			IP_ADAPTER_ADDRESSES *p;
			for (p = aa; p; p = p->Next) {
				char fname[256];
				WideCharToMultiByte(CP_UTF8, 0, p->FriendlyName, -1,
									fname, sizeof(fname), NULL, NULL);
				if (!_stricmp(fname, iface) ||
					(p->Description && WideCharToMultiByte(CP_UTF8, 0,
						p->Description, -1, fname, sizeof(fname), NULL, NULL),
					 !_stricmp(fname, iface))) {
					IP_ADAPTER_UNICAST_ADDRESS *u;
					for (u = p->FirstUnicastAddress; u; u = u->Next) {
						if (u->Address.lpSockaddr->sa_family == AF_INET) {
							uint32_t v = ((struct sockaddr_in *)u->Address.lpSockaddr)
											->sin_addr.s_addr;
							free(aa);
							return v;
						}
					}
				}
			}
		}
		if (aa) free(aa);
	}
#endif
	return htonl(INADDR_ANY);
}

/* Look up the MAC address (6 bytes) of the local interface whose IPv4
 * address matches `iface_addr` (network order). Returns SWITCH_STATUS_SUCCESS
 * and fills `mac[0..5]` on success; SWITCH_STATUS_FALSE on any failure
 * (loopback, missing adapter, all-zero MAC, etc.). */
static switch_status_t resolve_iface_mac(uint32_t iface_addr, uint8_t mac[6])
{
	if (iface_addr == 0 || iface_addr == htonl(INADDR_ANY)) {
		return SWITCH_STATUS_FALSE;
	}

#ifdef _WIN32
	{
		ULONG cb = 16 * 1024;
		IP_ADAPTER_ADDRESSES *aa = (IP_ADAPTER_ADDRESSES *)malloc(cb);
		IP_ADAPTER_ADDRESSES *p;
		switch_status_t status = SWITCH_STATUS_FALSE;
		if (!aa) return SWITCH_STATUS_FALSE;
		if (GetAdaptersAddresses(AF_INET,
				GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST |
				GAA_FLAG_SKIP_FRIENDLY_NAME, NULL, aa, &cb) != NO_ERROR) {
			free(aa);
			return SWITCH_STATUS_FALSE;
		}
		for (p = aa; p && status != SWITCH_STATUS_SUCCESS; p = p->Next) {
			IP_ADAPTER_UNICAST_ADDRESS *u;
			for (u = p->FirstUnicastAddress; u; u = u->Next) {
				if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
				if (((struct sockaddr_in *)u->Address.lpSockaddr)
						->sin_addr.s_addr == iface_addr) {
					if (p->PhysicalAddressLength >= 6) {
						memcpy(mac, p->PhysicalAddress, 6);
						if (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) {
							status = SWITCH_STATUS_SUCCESS;
						}
					}
					break;
				}
			}
		}
		free(aa);
		return status;
	}
#else
	{
		struct ifaddrs *ifap = NULL, *ifa;
		char ifname[IFNAMSIZ] = "";
		struct ifreq ifr;
		int s;

		if (getifaddrs(&ifap) != 0) return SWITCH_STATUS_FALSE;
		for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
			if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET &&
				((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr == iface_addr) {
				switch_snprintf(ifname, sizeof(ifname), "%s", ifa->ifa_name);
				break;
			}
		}
		freeifaddrs(ifap);
		if (!ifname[0]) return SWITCH_STATUS_FALSE;

		s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s < 0) return SWITCH_STATUS_FALSE;
		memset(&ifr, 0, sizeof(ifr));
		switch_snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
#ifdef SIOCGIFHWADDR
		if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0) {
			memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
			close(s);
			if (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) {
				return SWITCH_STATUS_SUCCESS;
			}
		} else
#endif
		{
			close(s);
		}
		return SWITCH_STATUS_FALSE;
	}
#endif
}

static switch_status_t open_mcast_socket(ptpn_sock_t *out, uint16_t port,
										 uint32_t iface_addr)
{
	ptpn_sock_t s;
	int yes = 1;
	struct sockaddr_in sa;
	struct ip_mreq mreq;
	struct in_addr ifa;

	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == PTPN_INVALID_SOCK) {
		return SWITCH_STATUS_FALSE;
	}

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#ifdef SO_REUSEPORT
	setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&yes, sizeof(yes));
#endif

	memset(&sa, 0, sizeof(sa));
	sa.sin_family      = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port        = htons(port);
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "ptp_native: bind UDP/%u failed (%d)\n", port, ptpn_errno);
		PTPN_CLOSESOCK(s);
		return SWITCH_STATUS_FALSE;
	}

	memset(&mreq, 0, sizeof(mreq));
	mreq.imr_multiaddr.s_addr = inet_addr(PTP_MCAST_DEFAULT);
	mreq.imr_interface.s_addr = iface_addr;
	if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				   (const char *)&mreq, sizeof(mreq)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "ptp_native: IP_ADD_MEMBERSHIP UDP/%u failed (%d)\n",
						  port, ptpn_errno);
		PTPN_CLOSESOCK(s);
		return SWITCH_STATUS_FALSE;
	}

	/* TX side: bind multicast egress to the chosen iface. */
	ifa.s_addr = iface_addr;
	setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,
			   (const char *)&ifa, sizeof(ifa));

	{
		unsigned char loop = 0;
		setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP,
				   (const char *)&loop, sizeof(loop));
	}
	{
		unsigned char ttl = 1;
		setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL,
				   (const char *)&ttl, sizeof(ttl));
	}

	*out = s;
	return SWITCH_STATUS_SUCCESS;
}

/* =====================================================================
 *  Master table operations (caller holds mutex)
 * =================================================================== */

static int find_master(ptp_native_t *p, const ptp_port_id_t *port)
{
	int i;
	for (i = 0; i < PTPN_MAX_MASTERS; i++) {
		if (!p->masters[i].in_use) continue;
		if (memcmp(&p->masters[i].port, port, sizeof(*port)) == 0) return i;
	}
	return -1;
}

static int alloc_master(ptp_native_t *p)
{
	int i;
	for (i = 0; i < PTPN_MAX_MASTERS; i++) {
		if (!p->masters[i].in_use) return i;
	}
	return -1;
}

/* Recompute the active master per BMCA / pinned policy. */
static void recompute_active(ptp_native_t *p)
{
	int i, best = -1;

	if (p->cfg.prio_mode == PTP_NATIVE_PRIO_LOCKED) {
		for (i = 0; i < PTPN_MAX_MASTERS; i++) {
			if (!p->masters[i].in_use) continue;
			if (memcmp(p->masters[i].grandmaster_id.v,
					   p->cfg.locked_clock_id, 8) == 0) {
				best = i;
				break;
			}
		}
	} else if (p->cfg.prio_mode == PTP_NATIVE_PRIO_FIRST) {
		for (i = 0; i < PTPN_MAX_MASTERS; i++) {
			if (p->masters[i].in_use) { best = i; break; }
		}
	} else {
		for (i = 0; i < PTPN_MAX_MASTERS; i++) {
			if (!p->masters[i].in_use) continue;
			if (best < 0 || bmca_compare(&p->masters[i], &p->masters[best]) < 0) {
				best = i;
			}
		}
	}

	if (best != p->active_idx) {
		char a[32], b[32];
		const ptpn_master_t *prev_m = (p->active_idx >= 0) ?
									  &p->masters[p->active_idx] : NULL;
		const ptpn_master_t *new_m  = (best >= 0) ? &p->masters[best] : NULL;

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "ptp_native: grandmaster %s -> %s\n",
						  prev_m ? fmt_clock_id(&prev_m->grandmaster_id, a, sizeof(a)) : "(none)",
						  new_m  ? fmt_clock_id(&new_m->grandmaster_id,  b, sizeof(b)) : "(none)");
		p->active_idx = best;
		p->lock_consecutive = 0;
		p->locked = SWITCH_FALSE;
		/* Drop stale two-step / delay-resp state — sequence numbers
		 * belong to the previous master. */
		p->pending_sync = SWITCH_FALSE;
		p->outstanding_dreq_seq = 0;
		p->outstanding_t1_ns = 0;
		p->outstanding_t2_ns = 0;
		p->effective_dreq_unicast = SWITCH_FALSE;
		p->dreq_unanswered = 0;
		p->dreq_proven = SWITCH_FALSE;
		p->sync_profile_logged = SWITCH_FALSE;
	}
}

/* =====================================================================
 *  Message handlers (caller holds mutex)
 * =================================================================== */

static void handle_announce(ptp_native_t *p, const ptp_header_t *hdr,
							const ptp_announce_t *a)
{
	int idx = find_master(p, &hdr->source_port);
	ptpn_master_t *m;

	if (idx < 0) {
		char sp[32], gm[32];
		idx = alloc_master(p);
		if (idx < 0) return;       /* table full — drop                  */
		memset(&p->masters[idx], 0, sizeof(p->masters[idx]));
		p->masters[idx].in_use = SWITCH_TRUE;
		p->masters[idx].port   = hdr->source_port;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"ptp_native: new master idx=%d sourcePort=%s/%u GM=%s "
			"prio1=%u class=%u acc=0x%02x prio2=%u steps=%u domain=%u ts=%u\n",
			idx,
			fmt_clock_id(&hdr->source_port.clock, sp, sizeof(sp)),
			(unsigned)be16(hdr->source_port.port_number),
			fmt_clock_id(&a->grandmaster_identity, gm, sizeof(gm)),
			(unsigned)a->grandmaster_priority1,
			(unsigned)a->gm_clock_quality[0],
			(unsigned)a->gm_clock_quality[1],
			(unsigned)a->grandmaster_priority2,
			(unsigned)be16(a->steps_removed),
			(unsigned)hdr->domain_number,
			(unsigned)((hdr->msg_type_transport >> 4) & 0x0F));
	}
	m = &p->masters[idx];

	m->priority1                  = a->grandmaster_priority1;
	m->clock_class                = a->gm_clock_quality[0];
	m->clock_accuracy             = a->gm_clock_quality[1];
	m->offset_scaled_log_variance = ((uint16_t)a->gm_clock_quality[2] << 8) |
									 (uint16_t)a->gm_clock_quality[3];
	m->priority2                  = a->grandmaster_priority2;
	m->grandmaster_id             = a->grandmaster_identity;
	m->steps_removed              = be16(a->steps_removed);
	m->log_announce_interval      = (int8_t)hdr->log_message_interval;
	m->transport_specific         = (uint8_t)((hdr->msg_type_transport >> 4) & 0x0F);
	m->domain                     = hdr->domain_number;
	m->last_announce_us           = switch_micro_time_now();

	if (mod_ptp_globals.debug) {
		uint8_t ts = (uint8_t)((hdr->msg_type_transport >> 4) & 0x0F);
		char gid[32];
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"ptp_native: Announce GM=%s class=%u acc=0x%02x prio1=%u prio2=%u "
			"steps=%u logAnn=%d transportSpecific=%u (%s)\n",
			fmt_clock_id(&m->grandmaster_id, gid, sizeof(gid)),
			(unsigned)m->clock_class, (unsigned)m->clock_accuracy,
			(unsigned)m->priority1, (unsigned)m->priority2,
			(unsigned)m->steps_removed, (int)m->log_announce_interval,
			(unsigned)ts,
			ts == 0 ? "default/SMPTE/AES67 (E2E)" :
			ts == 1 ? "802.1AS/gPTP (P2P only)"   : "unknown");
	}

	recompute_active(p);
}

static void handle_sync(ptp_native_t *p, const ptp_header_t *hdr,
						const ptp_sync_t *s, int64_t rx_ns)
{
	p->rx_sync++;
	if (p->active_idx < 0) {
		p->rx_dropped_no_active++;
		return;
	}
	/* Match on clockIdentity only — SMPTE-2059 / Lab X GMs commonly send
	 * Announce, Sync and Delay_Resp from different portNumber values within
	 * the same clockIdentity. */
	if (memcmp(hdr->source_port.clock.v,
			   p->masters[p->active_idx].port.clock.v, 8) != 0) {
		p->rx_dropped_wrong_master++;
		if (mod_ptp_globals.debug) {
			char a[32], b[32];
			fmt_clock_id(&hdr->source_port.clock, a, sizeof(a));
			fmt_clock_id(&p->masters[p->active_idx].port.clock, b, sizeof(b));
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"ptp_native: Sync from %s (port %u) ignored — active GM is %s\n",
				a, be16(hdr->source_port.port_number), b);
		}
		return;
	}

	p->pending_sync       = SWITCH_TRUE;
	p->pending_sync_seq   = be16(hdr->sequence_id);
	p->pending_t2_ns      = rx_ns;
	p->last_sync_us       = switch_micro_time_now();

	if (!p->sync_profile_logged) {
		uint16_t flags = be16(hdr->flag_field);
		uint8_t  ts    = (uint8_t)((hdr->msg_type_transport >> 4) & 0x0F);
		char gid[32];
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"ptp_native: first Sync from active GM %s — %s, transportSpecific=%u (%s), domain=%u, logSync=%d\n",
			fmt_clock_id(&p->masters[p->active_idx].grandmaster_id, gid, sizeof(gid)),
			(flags & PTP_FLAG_TWO_STEP) ? "two-step" : "one-step",
			(unsigned)ts,
			ts == 0 ? "default/SMPTE/AES67 E2E" :
			ts == 1 ? "802.1AS/gPTP P2P-only — Delay_Resp will NOT come" : "unknown",
			(unsigned)hdr->domain_number,
			(int)(int8_t)hdr->log_message_interval);
		p->sync_profile_logged = SWITCH_TRUE;
	}

	if (mod_ptp_globals.debug) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"ptp_native: Sync seq=%u t2=%lldns flags=0x%04x %s\n",
			(unsigned)be16(hdr->sequence_id), (long long)rx_ns,
			(unsigned)be16(hdr->flag_field),
			(be16(hdr->flag_field) & PTP_FLAG_TWO_STEP) ? "(two-step)" : "(one-step)");
	}

	/* If one-step Sync, originTimestamp is already authoritative t1.
	 * If two-step, t1 will arrive in Follow_Up. */
	if ((be16(hdr->flag_field) & PTP_FLAG_TWO_STEP) == 0) {
		p->pending_t1_ns = ptp_ts_to_ns(&s->origin_ts);
	} else {
		p->pending_t1_ns = 0;
	}
}

static void handle_follow_up(ptp_native_t *p, const ptp_header_t *hdr,
							 const ptp_sync_t *fu)
{
	p->rx_follow_up++;
	if (!p->pending_sync) {
		if (mod_ptp_globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"ptp_native: Follow_Up seq=%u dropped — no pending Sync\n",
				(unsigned)be16(hdr->sequence_id));
		}
		return;
	}
	if (be16(hdr->sequence_id) != p->pending_sync_seq) {
		if (mod_ptp_globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"ptp_native: Follow_Up seq=%u != pending Sync seq=%u\n",
				(unsigned)be16(hdr->sequence_id), (unsigned)p->pending_sync_seq);
		}
		return;
	}
	p->pending_t1_ns = ptp_ts_to_ns(&fu->origin_ts);
	if (mod_ptp_globals.debug) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"ptp_native: Follow_Up seq=%u t1=%lldns\n",
			(unsigned)be16(hdr->sequence_id), (long long)p->pending_t1_ns);
	}
}

/* Update servo with one (offset, delay) pair.
 *
 * Note on "offset" in this implementation:
 *   We never discipline the OS clock — `t1`/`t4` are PTP/TAI epoch ns from
 *   the GM and `t2`/`t3` are local CLOCK_MONOTONIC / QPC ns. The numerical
 *   `offset_ns = ((t2-t1) - (t4-t3)) / 2` therefore contains the entire
 *   epoch difference (decades) plus continuous local-vs-GM drift. Offset
 *   is therefore *not* a useful lock signal in this design.
 *
 *   The stable, observable metric is `path_delay_ns = ((t2-t1)+(t4-t3))/2`.
 *   The epoch difference cancels symmetrically in that sum, leaving the
 *   one-way wire delay. When successive path_delay samples agree to within
 *   a small bound, the round-trip math is consistent and Sync/Delay_Resp
 *   pairs are correctly correlated → lock.
 */
static void servo_update(ptp_native_t *p, int64_t offset_ns, int64_t delay_ns)
{
	int64_t ddelay_ns;

	/* Path delay sanity: must be non-negative and within a reasonable LAN
	 * bound. Anything larger means t3/t4 are from a mis-paired exchange. */
	if (delay_ns < 0 || delay_ns > PTPN_SERVO_DELAY_MAX_NS) {
		if (mod_ptp_globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"ptp_native: servo skip — implausible path_delay=%lldns\n",
				(long long)delay_ns);
		}
		return;
	}

	p->offset_ns     = offset_ns;
	p->path_delay_ns = delay_ns;
	p->servo_steps++;

	if (!p->servo_have_prev) {
		p->servo_have_prev  = SWITCH_TRUE;
		p->prev_offset_ns   = offset_ns;
		p->prev_delay_ns    = delay_ns;
		p->lock_consecutive = 0;
		return;
	}

	ddelay_ns = delay_ns - p->prev_delay_ns;
	if (ddelay_ns < 0) ddelay_ns = -ddelay_ns;
	p->prev_offset_ns = offset_ns;
	p->prev_delay_ns  = delay_ns;

	if (ddelay_ns <= PTPN_SERVO_DELAY_STABLE_NS) {
		if (p->lock_consecutive < 0xFFFF) p->lock_consecutive++;
		if (p->lock_consecutive >= PTPN_SERVO_LOCK_COUNT && !p->locked) {
			char id[32];
			p->locked = SWITCH_TRUE;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
							  "ptp_native: locked to GM %s (path_delay=%lldns, Δdelay=%lldns over %d samples)\n",
							  fmt_clock_id(&p->masters[p->active_idx].grandmaster_id,
										   id, sizeof(id)),
							  (long long)delay_ns, (long long)ddelay_ns,
							  PTPN_SERVO_LOCK_COUNT);
		}
	} else {
		if (mod_ptp_globals.debug && p->lock_consecutive > 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"ptp_native: stability broken (Δpath_delay=%lldns) — lock_consecutive reset\n",
				(long long)ddelay_ns);
		}
		p->lock_consecutive = 0;
	}
}

static void handle_delay_resp(ptp_native_t *p, const ptp_header_t *hdr,
							  const ptp_delay_resp_t *r)
{
	int64_t t1, t2, t3, t4, offset, delay;

	p->rx_delay_resp++;
	if (p->active_idx < 0) { p->rx_dropped_no_active++; return; }
	if (memcmp(hdr->source_port.clock.v,
			   p->masters[p->active_idx].port.clock.v, 8) != 0) {
		p->rx_dropped_dresp_wrong_master++;
		if (mod_ptp_globals.debug) {
			char a[32], b[32];
			fmt_clock_id(&hdr->source_port.clock, a, sizeof(a));
			fmt_clock_id(&p->masters[p->active_idx].port.clock, b, sizeof(b));
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"ptp_native: Delay_Resp from %s ignored — active GM is %s\n", a, b);
		}
		return;
	}
	/* Identity check FIRST: on a multicast segment we receive every
	 * other slave's Delay_Resp too. Anything not addressed to us is
	 * not a "drop" worth alarming about — silently ignore. */
	if (memcmp(r->requesting_port.clock.v, p->local_clock_id.v, 8) != 0) {
		/* Not for us — not a real drop, no counter bump. */
		return;
	}
	if (be16(hdr->sequence_id) != p->outstanding_dreq_seq) {
		p->rx_dropped_dreq_seq++;
		if (mod_ptp_globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"ptp_native: Delay_Resp seq=%u != outstanding=%u\n",
				(unsigned)be16(hdr->sequence_id),
				(unsigned)p->outstanding_dreq_seq);
		}
		return;
	}

	if (p->outstanding_dreq_seq == 0 || p->outstanding_t1_ns == 0) {
		if (mod_ptp_globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"ptp_native: Delay_Resp seq=%u dropped — no outstanding Delay_Req snapshot\n",
				(unsigned)be16(hdr->sequence_id));
		}
		return;
	}

	t1 = p->outstanding_t1_ns;        /* master TX of Sync (snapshot)   */
	t2 = p->outstanding_t2_ns;        /* local RX of Sync (snapshot)    */
	t3 = p->outstanding_dreq_t3_ns;   /* local TX of Delay_Req          */
	t4 = ptp_ts_to_ns(&r->receive_ts);/* master RX of Delay_Req         */

	offset = ((t2 - t1) - (t4 - t3)) / 2;
	delay  = ((t2 - t1) + (t4 - t3)) / 2;

	servo_update(p, offset, delay);

	p->outstanding_dreq_seq = 0;
	p->outstanding_t1_ns    = 0;
	p->outstanding_t2_ns    = 0;
	p->dreq_unanswered      = 0;
	p->dreq_proven          = SWITCH_TRUE;
}

/* =====================================================================
 *  TX
 * =================================================================== */

static void send_delay_req(ptp_native_t *p)
{
	uint8_t buf[PTP_HEADER_LEN + sizeof(ptp_delay_req_t)];
	ptp_header_t   *h = (ptp_header_t *)buf;
	ptp_delay_req_t *d = (ptp_delay_req_t *)(buf + PTP_HEADER_LEN);
	struct sockaddr_in dst;
	int64_t t3;
	switch_bool_t want_unicast;
	uint32_t master_addr;

	if (p->active_idx < 0 || p->pending_sync == SWITCH_FALSE) return;

	/* For two-step we must have already received Follow_Up (t1 != 0).
	 * For one-step, handle_sync set pending_t1_ns directly. Either way,
	 * sending without a valid t1 would render any Delay_Resp unusable. */
	if (p->pending_t1_ns == 0) return;

	/* If a previous Delay_Req is still outstanding when we get here, it
	 * means we never received its Delay_Resp — count it for AUTO fallback. */
	if (p->outstanding_dreq_seq != 0) {
		if (p->dreq_unanswered < 0xFFFFFFFFu) p->dreq_unanswered++;
		if (p->cfg.dreq_mode == PTP_NATIVE_DREQ_AUTO &&
			!p->effective_dreq_unicast &&
			!p->dreq_proven &&
			p->dreq_unanswered >= PTPN_DREQ_AUTO_UNICAST_AFTER &&
			p->masters[p->active_idx].source_addr != 0) {
			p->effective_dreq_unicast = SWITCH_TRUE;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"ptp_native: AUTO fallback — switching Delay_Req to unicast "
				"(%u multicast attempts unanswered)\n",
				(unsigned)p->dreq_unanswered);
		}
	}

	master_addr = p->masters[p->active_idx].source_addr;
	want_unicast =
		(p->cfg.dreq_mode == PTP_NATIVE_DREQ_UNICAST) ? SWITCH_TRUE :
		(p->cfg.dreq_mode == PTP_NATIVE_DREQ_MULTICAST) ? SWITCH_FALSE :
		p->effective_dreq_unicast;

	if (want_unicast && master_addr == 0) {
		/* Cannot do unicast without an address yet — fall back to multicast. */
		want_unicast = SWITCH_FALSE;
	}

	/* If unicast is also being ignored for a long time, surface a diagnostic
	 * once every ~30 attempts.  Most likely cause: the GM requires unicast
	 * negotiation (IEEE 1588 §16.1, REQUEST_UNICAST_TRANSMISSION) before
	 * accepting Delay_Req, or it is configured one-way / Sync-only. */
	if (want_unicast && p->dreq_unanswered &&
		(p->dreq_unanswered % 30) == 0) {
		char ip[INET_ADDRSTRLEN] = "?";
		struct in_addr ia; ia.s_addr = master_addr;
#ifdef _WIN32
		switch_snprintf(ip, sizeof(ip), "%s", inet_ntoa(ia));
#else
		inet_ntop(AF_INET, &ia, ip, sizeof(ip));
#endif
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"ptp_native: %u unicast Delay_Req to %s still unanswered — "
			"GM may require IEEE 1588-2008 §16.1 unicast negotiation "
			"(REQUEST_UNICAST_TRANSMISSION via Signaling) or be configured "
			"one-way/Sync-only.  Verify with packet capture on UDP/319.\n",
			(unsigned)p->dreq_unanswered, ip);
	}

	memset(buf, 0, sizeof(buf));
	{
		uint8_t ts = (p->active_idx >= 0) ? p->masters[p->active_idx].transport_specific : 0;
		h->msg_type_transport  = (uint8_t)((ts << 4) | PTP_MSG_DELAY_REQ);
	}
	h->version_ptp         = PTP_VERSION;
	h->message_length      = be16((uint16_t)sizeof(buf));
	h->domain_number       = p->cfg.domain;
	h->flag_field          = want_unicast ? be16(0x0400) : 0;  /* unicastFlag */
	h->correction_field    = 0;
	h->source_port.clock   = p->local_clock_id;
	h->source_port.port_number = be16(1);
	h->sequence_id         = be16(++p->seq_delay_req);
	h->control_field       = 0x01;                      /* Delay_Req */
	h->log_message_interval = 0x7F;                     /* unspecified */

	/* originTimestamp can be zeroed; t3 is captured locally. */

	memset(&dst, 0, sizeof(dst));
	dst.sin_family      = AF_INET;
	dst.sin_addr.s_addr = want_unicast ? master_addr : inet_addr(PTP_MCAST_DEFAULT);
	dst.sin_port        = htons(PTP_EVENT_PORT);

	t3 = local_now_ns();
	if (sendto(p->tx_sock, (const char *)buf, sizeof(buf), 0,
			   (struct sockaddr *)&dst, sizeof(dst)) > 0) {
		p->outstanding_dreq_seq = p->seq_delay_req;
		p->outstanding_dreq_t3_ns = t3;
		p->outstanding_t1_ns = p->pending_t1_ns;
		p->outstanding_t2_ns = p->pending_t2_ns;
		p->last_delay_req_us = switch_micro_time_now();
		p->tx_delay_req++;
		if (mod_ptp_globals.debug) {
			char dstbuf[INET_ADDRSTRLEN] = "?";
#ifdef _WIN32
			{
				struct in_addr ia; ia.s_addr = dst.sin_addr.s_addr;
				switch_snprintf(dstbuf, sizeof(dstbuf), "%s", inet_ntoa(ia));
			}
#else
			inet_ntop(AF_INET, &dst.sin_addr, dstbuf, sizeof(dstbuf));
#endif
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"ptp_native: TX Delay_Req seq=%u t3=%lldns dst=%s (%s)\n",
				(unsigned)p->seq_delay_req, (long long)t3, dstbuf,
				want_unicast ? "unicast" : "multicast");
		}

		/* One-shot raw hex dump of the first Delay_Req we transmit, plus
		 * a decoded header summary, so it can be diffed against a Wireshark
		 * capture taken on the dumb hub.  If this packet is well-formed and
		 * the GM still does not respond, the GM is one-way / Sync-only or
		 * requires unicast negotiation. */
		if (!p->dreq_hex_dumped) {
			char hex[3 * sizeof(buf) + 1];
			size_t i;
			char myid[32], dstbuf2[INET_ADDRSTRLEN] = "?";
			struct in_addr ia2; ia2.s_addr = dst.sin_addr.s_addr;
#ifdef _WIN32
			switch_snprintf(dstbuf2, sizeof(dstbuf2), "%s", inet_ntoa(ia2));
#else
			inet_ntop(AF_INET, &ia2, dstbuf2, sizeof(dstbuf2));
#endif
			for (i = 0; i < sizeof(buf); i++) {
				switch_snprintf(hex + i * 3, 4, "%02X ", buf[i]);
			}
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"ptp_native: first Delay_Req TX dump (%u bytes -> %s:%u %s)\n"
				"  msgType=0x%02X (transportSpecific=%u, type=%u) versionPTP=%u "
				"messageLength=%u domainNumber=%u flagField=0x%04X\n"
				"  sourcePortIdentity=%s/%u sequenceId=%u controlField=0x%02X "
				"logMessageInterval=0x%02X\n"
				"  raw: %s\n",
				(unsigned)sizeof(buf), dstbuf2, (unsigned)PTP_EVENT_PORT,
				want_unicast ? "unicast" : "multicast",
				(unsigned)h->msg_type_transport,
				(unsigned)((h->msg_type_transport >> 4) & 0x0F),
				(unsigned)(h->msg_type_transport & 0x0F),
				(unsigned)(h->version_ptp & 0x0F),
				(unsigned)be16(h->message_length),
				(unsigned)h->domain_number,
				(unsigned)be16(h->flag_field),
				fmt_clock_id(&h->source_port.clock, myid, sizeof(myid)),
				(unsigned)be16(h->source_port.port_number),
				(unsigned)be16(h->sequence_id),
				(unsigned)h->control_field,
				(unsigned)h->log_message_interval,
				hex);
			p->dreq_hex_dumped = SWITCH_TRUE;
		}
	} else {
		p->tx_delay_req_failed++;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"ptp_native: sendto Delay_Req failed (%d)\n", ptpn_errno);
	}
}

/* =====================================================================
 *  Housekeeping
 * =================================================================== */

static void housekeeping(ptp_native_t *p)
{
	int i;
	switch_time_t now = switch_micro_time_now();

	for (i = 0; i < PTPN_MAX_MASTERS; i++) {
		ptpn_master_t *m = &p->masters[i];
		switch_time_t timeout_us;
		int8_t la;

		if (!m->in_use) continue;

		la = m->log_announce_interval;
		if (la < -7 || la > 7) la = PTPN_DEFAULT_LOG_ANNOUNCE;
		timeout_us = (switch_time_t)(1 << (la < 0 ? 0 : la)) *
					 1000000 * PTPN_GM_TIMEOUT_FACTOR;

		if (now - m->last_announce_us > timeout_us) {
			char id[32];
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "ptp_native: master %s timed out after %lldms\n",
							  fmt_clock_id(&m->grandmaster_id, id, sizeof(id)),
							  (long long)((now - m->last_announce_us) / 1000));
			m->in_use = SWITCH_FALSE;
		}
	}

	recompute_active(p);

	if (p->active_idx < 0 && p->locked) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ptp_native: sync lost — entering holdover\n");
		p->locked = SWITCH_FALSE;
		p->lock_consecutive = 0;
	}

	/* Periodic flow summary while debug is on. */
	if (mod_ptp_globals.debug && now >= p->dbg_next_summary_us) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"ptp_native flow: ann=%llu sync=%llu fup=%llu dresp=%llu "
			"pdreq=%llu pdresp=%llu tx_dreq=%llu (fail=%llu) "
			"drop[noact=%llu sync_wm=%llu dresp_wm=%llu dseq=%llu "
			"dport=%llu dom=%llu self=%llu] active=%d locked=%d steps=%u%s\n",
			(unsigned long long)p->rx_announce,
			(unsigned long long)p->rx_sync,
			(unsigned long long)p->rx_follow_up,
			(unsigned long long)p->rx_delay_resp,
			(unsigned long long)p->rx_pdelay_req,
			(unsigned long long)p->rx_pdelay_resp,
			(unsigned long long)p->tx_delay_req,
			(unsigned long long)p->tx_delay_req_failed,
			(unsigned long long)p->rx_dropped_no_active,
			(unsigned long long)p->rx_dropped_wrong_master,
			(unsigned long long)p->rx_dropped_dresp_wrong_master,
			(unsigned long long)p->rx_dropped_dreq_seq,
			(unsigned long long)p->rx_dropped_dreq_port,
			(unsigned long long)p->rx_dropped_domain,
			(unsigned long long)p->rx_dropped_self,
			p->active_idx, (int)p->locked, (unsigned)p->servo_steps,
			(p->rx_pdelay_req || p->rx_pdelay_resp) ?
				" [P2P traffic seen — GM may require peer-delay; E2E Delay_Req will not get a Delay_Resp]" : "");

		/* Raw socket-level visibility — does the GM/network even reach UDP/320? */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"ptp_native sockets: ev[pkts=%llu bytes=%llu] gen[pkts=%llu bytes=%llu] "
			"gen_mtypes[ann=%llu fup=%llu dresp=%llu other=%llu]%s\n",
			(unsigned long long)p->rx_raw_event_pkts,
			(unsigned long long)p->rx_raw_event_bytes,
			(unsigned long long)p->rx_raw_general_pkts,
			(unsigned long long)p->rx_raw_general_bytes,
			(unsigned long long)p->rx_gen_announce,
			(unsigned long long)p->rx_gen_follow_up,
			(unsigned long long)p->rx_gen_delay_resp,
			(unsigned long long)p->rx_gen_other,
			(p->rx_raw_general_pkts == 0)
				? " [no UDP/320 traffic at all — IGMP snooping or filter blocking general port]"
				: (p->rx_gen_delay_resp == 0 && p->tx_delay_req > 0)
					? " [UDP/320 alive but GM is not emitting Delay_Resp toward us — check GM unicast/multicast policy and switch ACLs]"
					: "");
		p->dbg_next_summary_us = now + 5000000; /* 5 s */
	}
}

/* =====================================================================
 *  RX dispatch
 * =================================================================== */

static void rx_handle(ptp_native_t *p, const uint8_t *buf, int len, int64_t rx_ns, uint32_t src_addr, int from_general)
{
	const ptp_header_t *h;
	uint8_t mtype;

	if (len < (int)PTP_HEADER_LEN) return;
	h = (const ptp_header_t *)buf;

	if ((h->version_ptp & 0x0F) != PTP_VERSION) return;

	switch_mutex_lock(p->mutex);

	if (h->domain_number != p->cfg.domain) {
		p->rx_dropped_domain++;
		switch_mutex_unlock(p->mutex);
		return;
	}

	mtype = h->msg_type_transport & 0x0F;

	/* Per-socket per-mtype tally — visible regardless of master/self/etc. filters. */
	if (from_general) {
		switch (mtype) {
		case PTP_MSG_ANNOUNCE:    p->rx_gen_announce++;   break;
		case PTP_MSG_FOLLOW_UP:   p->rx_gen_follow_up++;  break;
		case PTP_MSG_DELAY_RESP:  p->rx_gen_delay_resp++; break;
		default:                  p->rx_gen_other++;      break;
		}
	}

	/* Drop our own Delay_Req loopback (clock id match). */
	if (memcmp(h->source_port.clock.v, p->local_clock_id.v, 8) == 0) {
		p->rx_dropped_self++;
		switch_mutex_unlock(p->mutex);
		return;
	}

	/* Update source_addr for the master matching this clock identity, so
	 * unicast Delay_Req has a valid destination even when ports differ. */
	if (src_addr) {
		int i;
		for (i = 0; i < PTPN_MAX_MASTERS; i++) {
			if (p->masters[i].in_use &&
				memcmp(p->masters[i].port.clock.v,
					   h->source_port.clock.v, 8) == 0) {
				p->masters[i].source_addr = src_addr;
			}
		}
	}

	switch (mtype) {
	case PTP_MSG_ANNOUNCE:
		p->rx_announce++;
		if (len >= (int)(PTP_HEADER_LEN + sizeof(ptp_announce_t))) {
			handle_announce(p, h, (const ptp_announce_t *)(buf + PTP_HEADER_LEN));
			/* handle_announce may have allocated a new master entry — record
			 * its source IPv4 immediately so unicast Delay_Req can target it. */
			if (src_addr) {
				int i;
				for (i = 0; i < PTPN_MAX_MASTERS; i++) {
					if (p->masters[i].in_use &&
						memcmp(p->masters[i].port.clock.v,
							   h->source_port.clock.v, 8) == 0) {
						p->masters[i].source_addr = src_addr;
					}
				}
			}
		}
		break;
	case PTP_MSG_SYNC:
		if (len >= (int)(PTP_HEADER_LEN + sizeof(ptp_sync_t))) {
			handle_sync(p, h, (const ptp_sync_t *)(buf + PTP_HEADER_LEN), rx_ns);
		}
		break;
	case PTP_MSG_FOLLOW_UP:
		if (len >= (int)(PTP_HEADER_LEN + sizeof(ptp_sync_t))) {
			handle_follow_up(p, h, (const ptp_sync_t *)(buf + PTP_HEADER_LEN));
		}
		break;
	case PTP_MSG_DELAY_RESP:
		if (len >= (int)(PTP_HEADER_LEN + sizeof(ptp_delay_resp_t))) {
			handle_delay_resp(p, h, (const ptp_delay_resp_t *)(buf + PTP_HEADER_LEN));
		}
		break;
	case PTP_MSG_PDELAY_REQ:
		p->rx_pdelay_req++;
		break;
	case PTP_MSG_PDELAY_RESP:
	case PTP_MSG_PDELAY_RESP_FU:
		p->rx_pdelay_resp++;
		break;
	default:
		p->rx_other++;
		break;
	}

	switch_mutex_unlock(p->mutex);
}

/* =====================================================================
 *  RX thread
 * =================================================================== */

static void *SWITCH_THREAD_FUNC ptp_native_thread(switch_thread_t *thread, void *obj)
{
	ptp_native_t *p = (ptp_native_t *)obj;
	uint8_t buf[PTPN_RX_BUFLEN];
	switch_time_t next_dreq_us = 0;
	switch_time_t next_house_us = switch_micro_time_now() + PTPN_HOUSEKEEP_INTERVAL_US;

	(void)thread;

	while (p->running) {
		fd_set rfds;
		struct timeval tv;
		ptpn_sock_t maxfd;
		int rc;

		FD_ZERO(&rfds);
		FD_SET(p->ev_sock,  &rfds);
		FD_SET(p->gen_sock, &rfds);
		maxfd = (p->ev_sock > p->gen_sock) ? p->ev_sock : p->gen_sock;

		tv.tv_sec  = 0;
		tv.tv_usec = 100000;       /* 100 ms wakeup for housekeeping     */

		rc = select((int)(maxfd + 1), &rfds, NULL, NULL, &tv);
		if (rc < 0) {
#ifdef _WIN32
			if (ptpn_errno == WSAEINTR) continue;
#else
			if (errno == EINTR) continue;
#endif
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "ptp_native: select() failed (%d)\n", ptpn_errno);
			switch_yield(100000);
			continue;
		}

		if (rc > 0) {
			int64_t rx_ns = local_now_ns();
			struct sockaddr_in from;
			socklen_t fromlen;
			if (FD_ISSET(p->ev_sock, &rfds)) {
				int n;
				fromlen = sizeof(from);
				memset(&from, 0, sizeof(from));
				n = recvfrom(p->ev_sock, (char *)buf, sizeof(buf), 0,
							 (struct sockaddr *)&from, &fromlen);
				if (n > 0) {
					switch_mutex_lock(p->mutex);
					p->rx_raw_event_pkts++;
					p->rx_raw_event_bytes += (uint64_t)n;
					switch_mutex_unlock(p->mutex);
					rx_handle(p, buf, n, rx_ns, from.sin_addr.s_addr, 0);
				}
			}
			if (FD_ISSET(p->gen_sock, &rfds)) {
				int n;
				fromlen = sizeof(from);
				memset(&from, 0, sizeof(from));
				n = recvfrom(p->gen_sock, (char *)buf, sizeof(buf), 0,
							 (struct sockaddr *)&from, &fromlen);
				if (n > 0) {
					switch_mutex_lock(p->mutex);
					p->rx_raw_general_pkts++;
					p->rx_raw_general_bytes += (uint64_t)n;
					switch_mutex_unlock(p->mutex);
					rx_handle(p, buf, n, rx_ns, from.sin_addr.s_addr, 1);
				}
			}
		}

		/* Periodic Delay_Req. */
		{
			switch_time_t now = switch_micro_time_now();
			if (now >= next_dreq_us) {
				uint64_t before = p->tx_delay_req;
				switch_mutex_lock(p->mutex);
				send_delay_req(p);
				switch_mutex_unlock(p->mutex);
				/* If the send was skipped (e.g. waiting on Follow_Up so t1
				 * is not yet valid), retry quickly instead of losing a full
				 * 1 Hz interval. Otherwise pace at the configured rate. */
				if (p->tx_delay_req == before) {
					next_dreq_us = now + 50000;            /* 50 ms */
				} else {
					next_dreq_us = now + PTPN_DELAY_REQ_INTERVAL_US;
				}
			}
			if (now >= next_house_us) {
				switch_mutex_lock(p->mutex);
				housekeeping(p);
				switch_mutex_unlock(p->mutex);
				next_house_us = now + PTPN_HOUSEKEEP_INTERVAL_US;
			}
		}
	}

	return NULL;
}

/* =====================================================================
 *  Public API
 * =================================================================== */

switch_status_t ptp_native_create(ptp_native_t **out,
								  switch_memory_pool_t *pool,
								  const ptp_native_cfg_t *cfg)
{
	ptp_native_t *p;
	switch_threadattr_t *thd_attr = NULL;
	struct in_addr ina;

	if (!out || !pool || !cfg) return SWITCH_STATUS_FALSE;

	p = switch_core_alloc(pool, sizeof(*p));
	memset(p, 0, sizeof(*p));
	p->pool       = pool;
	p->cfg        = *cfg;
	p->ev_sock    = PTPN_INVALID_SOCK;
	p->gen_sock   = PTPN_INVALID_SOCK;
	p->tx_sock    = PTPN_INVALID_SOCK;
	p->active_idx = -1;
	p->dbg_next_summary_us = switch_micro_time_now() + 5000000;
	switch_mutex_init(&p->mutex, SWITCH_MUTEX_NESTED, pool);

	p->iface_addr = resolve_iface_ipv4(cfg->iface);
	ina.s_addr    = p->iface_addr;
	switch_snprintf(p->iface_desc, sizeof(p->iface_desc),
					"native-ptpv2:%s:dom%u",
					cfg->iface ? cfg->iface : "0.0.0.0", (unsigned)cfg->domain);

	if (open_mcast_socket(&p->ev_sock,  PTP_EVENT_PORT,   p->iface_addr) != SWITCH_STATUS_SUCCESS ||
		open_mcast_socket(&p->gen_sock, PTP_GENERAL_PORT, p->iface_addr) != SWITCH_STATUS_SUCCESS) {
		if (p->ev_sock  != PTPN_INVALID_SOCK) PTPN_CLOSESOCK(p->ev_sock);
		if (p->gen_sock != PTPN_INVALID_SOCK) PTPN_CLOSESOCK(p->gen_sock);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "ptp_native: failed to open multicast sockets on %s\n",
						  cfg->iface ? cfg->iface : "(any)");
		return SWITCH_STATUS_FALSE;
	}

	/* Dedicated TX socket bound to the chosen iface IP, so Delay_Req
	 * always egresses with the correct source address even on multi-homed
	 * hosts (Windows in particular ignores IP_MULTICAST_IF for source
	 * selection when the socket is bound to INADDR_ANY). */
	{
		struct sockaddr_in tx_bind;
		unsigned char ttl = 1;
		unsigned char loop = 0;
		struct in_addr ifa;
		p->tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (p->tx_sock == PTPN_INVALID_SOCK) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"ptp_native: TX socket() failed (%d)\n", ptpn_errno);
			PTPN_CLOSESOCK(p->ev_sock);
			PTPN_CLOSESOCK(p->gen_sock);
			return SWITCH_STATUS_FALSE;
		}
		memset(&tx_bind, 0, sizeof(tx_bind));
		tx_bind.sin_family      = AF_INET;
		tx_bind.sin_addr.s_addr = p->iface_addr;
		tx_bind.sin_port        = 0;
		if (bind(p->tx_sock, (struct sockaddr *)&tx_bind, sizeof(tx_bind)) < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"ptp_native: TX bind to iface failed (%d)\n", ptpn_errno);
			PTPN_CLOSESOCK(p->tx_sock);
			PTPN_CLOSESOCK(p->ev_sock);
			PTPN_CLOSESOCK(p->gen_sock);
			return SWITCH_STATUS_FALSE;
		}
		ifa.s_addr = p->iface_addr;
		setsockopt(p->tx_sock, IPPROTO_IP, IP_MULTICAST_IF,
				   (const char *)&ifa, sizeof(ifa));
		setsockopt(p->tx_sock, IPPROTO_IP, IP_MULTICAST_TTL,
				   (const char *)&ttl, sizeof(ttl));
		setsockopt(p->tx_sock, IPPROTO_IP, IP_MULTICAST_LOOP,
				   (const char *)&loop, sizeof(loop));
	}

	/* Build the local clockIdentity per IEEE 1588-2008 §7.5.2.2:
	 * EUI-64 from the bound interface's MAC (MAC[0..2] | FF FE | MAC[3..5]).
	 * The clockIdentity must be stable across restarts so the GM does not
	 * treat us as a brand-new slave on every launch (some grandmasters,
	 * Audinate Brooklyn II included, gate Delay_Resp on slave registration
	 * state and rate-limit unknown identities). If the MAC cannot be read
	 * (loopback, no adapter match, etc.), fall back to an IPv4-derived
	 * identity that is at least stable per host. */
	{
		uint8_t mac[6];
		if (resolve_iface_mac(p->iface_addr, mac) == SWITCH_STATUS_SUCCESS) {
			p->local_clock_id.v[0] = mac[0];
			p->local_clock_id.v[1] = mac[1];
			p->local_clock_id.v[2] = mac[2];
			p->local_clock_id.v[3] = 0xFF;
			p->local_clock_id.v[4] = 0xFE;
			p->local_clock_id.v[5] = mac[3];
			p->local_clock_id.v[6] = mac[4];
			p->local_clock_id.v[7] = mac[5];
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"ptp_native: clockIdentity (EUI-64 from MAC) = "
				"%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
				p->local_clock_id.v[0], p->local_clock_id.v[1],
				p->local_clock_id.v[2], p->local_clock_id.v[3],
				p->local_clock_id.v[4], p->local_clock_id.v[5],
				p->local_clock_id.v[6], p->local_clock_id.v[7]);
		} else {
			uint32_t a = ntohl(p->iface_addr);
			p->local_clock_id.v[0] = (uint8_t)(a >> 24);
			p->local_clock_id.v[1] = (uint8_t)(a >> 16);
			p->local_clock_id.v[2] = (uint8_t)(a >> 8);
			p->local_clock_id.v[3] = 0xFF;
			p->local_clock_id.v[4] = 0xFE;
			p->local_clock_id.v[5] = (uint8_t)(a & 0xFF);
			p->local_clock_id.v[6] = 0x00;
			p->local_clock_id.v[7] = 0x01;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"ptp_native: could not read iface MAC, using IPv4-derived "
				"clockIdentity (stable per host but not IEEE 1588 ideal)\n");
		}
	}

	p->running = SWITCH_TRUE;
	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_detach_set(thd_attr, 0);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&p->thread, thd_attr, ptp_native_thread, p, pool);

	{
		char ipbuf[INET_ADDRSTRLEN] = "?";
		struct in_addr ia2; ia2.s_addr = p->iface_addr;
#ifdef _WIN32
		switch_snprintf(ipbuf, sizeof(ipbuf), "%s", inet_ntoa(ia2));
#else
		inet_ntop(AF_INET, &ia2, ipbuf, sizeof(ipbuf));
#endif
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "ptp_native: started on %s (bound %s), domain %u, dreq_mode=%s\n",
						  cfg->iface ? cfg->iface : "(any)", ipbuf,
						  (unsigned)cfg->domain,
						  cfg->dreq_mode == PTP_NATIVE_DREQ_MULTICAST ? "multicast" :
						  cfg->dreq_mode == PTP_NATIVE_DREQ_UNICAST   ? "unicast"   : "auto");
	}

	*out = p;
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t ptp_native_poll(ptp_native_t *p, ptp_status_t *out)
{
	if (!p || !out) return SWITCH_STATUS_FALSE;
	memset(out, 0, sizeof(*out));
	out->sample_us = switch_micro_time_now();

	switch_mutex_lock(p->mutex);

	switch_snprintf(out->source_desc, sizeof(out->source_desc), "%s", p->iface_desc);

	if (p->active_idx < 0) {
		out->state = PTP_SYNC_NONE;
		switch_snprintf(out->port_state, sizeof(out->port_state), "LISTENING");
		switch_snprintf(out->grandmaster_id, sizeof(out->grandmaster_id),
						"00:00:00:00:00:00:00:00");
	} else {
		const ptpn_master_t *m = &p->masters[p->active_idx];
		fmt_clock_id(&m->grandmaster_id, out->grandmaster_id,
					 sizeof(out->grandmaster_id));
		out->master_offset_ns = p->offset_ns;
		out->path_delay_ns    = p->path_delay_ns;
		out->servo_steps      = p->servo_steps;
		switch_snprintf(out->port_state, sizeof(out->port_state),
						p->locked ? "SLAVE" : "UNCALIBRATED");
		out->state = p->locked ? PTP_SYNC_LOCKED : PTP_SYNC_HOLDOVER;
	}

	switch_mutex_unlock(p->mutex);
	return SWITCH_STATUS_SUCCESS;
}

void ptp_native_destroy(ptp_native_t **p_io)
{
	ptp_native_t *p;
	switch_status_t st;

	if (!p_io || !*p_io) return;
	p = *p_io;

	p->running = SWITCH_FALSE;
	if (p->thread) switch_thread_join(&st, p->thread);

	if (p->ev_sock  != PTPN_INVALID_SOCK) PTPN_CLOSESOCK(p->ev_sock);
	if (p->gen_sock != PTPN_INVALID_SOCK) PTPN_CLOSESOCK(p->gen_sock);
	if (p->tx_sock  != PTPN_INVALID_SOCK) PTPN_CLOSESOCK(p->tx_sock);

	*p_io = NULL;
}

int64_t ptp_native_now_ns(ptp_native_t *p)
{
	int64_t off, now;
	if (!p) return 0;
	switch_mutex_lock(p->mutex);
	if (!p->locked) {
		switch_mutex_unlock(p->mutex);
		return 0;
	}
	off = p->offset_ns;
	switch_mutex_unlock(p->mutex);
	now = local_now_ns();
	return now + off;
}

switch_bool_t ptp_native_is_locked(ptp_native_t *p)
{
	switch_bool_t r;
	if (!p) return SWITCH_FALSE;
	switch_mutex_lock(p->mutex);
	r = p->locked;
	switch_mutex_unlock(p->mutex);
	return r;
}
