/*
 * ptp_source_linux.c
 *
 * PTP status source backed by ptp4l's management socket.
 *
 * ptp4l (linuxptp) exposes a UDS management socket at the path given by
 * its `uds_address` option (default: /var/run/ptp4l).  Clients connect
 * with their own UDS, then exchange PTP management messages framed in
 * the same wire format as on-the-wire IEEE 1588 management.
 *
 * We send three GET requests once per poll:
 *   - CURRENT_DATA_SET     -> stepsRemoved, offsetFromMaster, meanPathDelay
 *   - PARENT_DATA_SET      -> grandmasterIdentity
 *   - PORT_DATA_SET (port 1) -> portState
 *
 * The wire layout matches IEEE 1588-2008 §15.4.  We only parse the
 * fields we care about and tolerate ptp4l versions that pad differently.
 *
 * On non-Linux builds this file compiles to an empty translation unit.
 */

#ifdef __linux__

#include "ptp_source.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

/* PTP message types */
#define PTP_MSG_MANAGEMENT          0x0D

/* Management actions */
#define PTP_MGMT_ACTION_GET         0
#define PTP_MGMT_ACTION_RESPONSE    3

/* Management TLV ids we care about */
#define PTP_MGMT_ID_CURRENT_DATA_SET 0x2001
#define PTP_MGMT_ID_PARENT_DATA_SET  0x2002
#define PTP_MGMT_ID_PORT_DATA_SET    0x2004

#define PTP_TLV_TYPE_MANAGEMENT     0x0001

#pragma pack(push, 1)

struct ptp_clock_id {
	uint8_t v[8];
};

struct ptp_port_id {
	struct ptp_clock_id clock;
	uint16_t            port_number;   /* network order */
};

struct ptp_header {
	uint8_t  msg_type_transport;       /* hi nibble = transportSpecific, lo = msgType */
	uint8_t  version_ptp;              /* lo nibble = versionPTP */
	uint16_t message_length;           /* network order */
	uint8_t  domain_number;
	uint8_t  reserved1;
	uint16_t flag_field;
	uint64_t correction_field;
	uint32_t reserved2;
	struct ptp_port_id source_port;
	uint16_t sequence_id;              /* network order */
	uint8_t  control_field;
	uint8_t  log_message_interval;
};

struct ptp_management_msg {
	struct ptp_header  hdr;
	struct ptp_port_id target_port;
	uint8_t            starting_boundary_hops;
	uint8_t            boundary_hops;
	uint8_t            action;          /* lo nibble */
	uint8_t            reserved;
	/* TLV follows: 2B type, 2B length, 2B managementId, [data...] */
};

struct ptp_tlv_hdr {
	uint16_t tlv_type;     /* network order */
	uint16_t length;       /* network order, length of value (incl. mgmtId) */
	uint16_t management_id;/* network order */
};

/* CURRENT_DATA_SET payload */
struct ptp_current_ds {
	uint16_t steps_removed;     /* network order */
	int64_t  offset_from_master;/* network order, scaled-ns (1/2^16 ns) */
	int64_t  mean_path_delay;   /* network order, scaled-ns */
};

/* PARENT_DATA_SET payload (only the leading bytes we need) */
struct ptp_parent_ds_head {
	struct ptp_port_id parent_port_identity;
	uint8_t            parent_stats;
	uint8_t            reserved;
	int16_t            observed_offset_scaled_log_variance;
	int32_t            observed_phase_change_rate;
	uint8_t            grandmaster_priority1;
	uint8_t            gm_clock_quality[4];
	uint8_t            grandmaster_priority2;
	struct ptp_clock_id grandmaster_identity;
};

/* PORT_DATA_SET payload (only what we need) */
struct ptp_port_ds_head {
	struct ptp_port_id port_identity;
	uint8_t            port_state;
	/* ... more fields we ignore ... */
};

#pragma pack(pop)

struct ptp_source_s {
	switch_memory_pool_t *pool;
	int                   sockfd;
	char                  client_path[108];
	char                  server_path[108];
	uint16_t              seq;
	struct ptp_clock_id   client_clock_id;

	/* sticky bits for cross-poll consistency */
	switch_bool_t         have_current;
	switch_bool_t         have_parent;
	switch_bool_t         have_port;
};

/* -------- helpers -------- */

static const char *port_state_name(uint8_t s)
{
	switch (s) {
	case 1:  return "INITIALIZING";
	case 2:  return "FAULTY";
	case 3:  return "DISABLED";
	case 4:  return "LISTENING";
	case 5:  return "PRE_MASTER";
	case 6:  return "MASTER";
	case 7:  return "PASSIVE";
	case 8:  return "UNCALIBRATED";
	case 9:  return "SLAVE";
	default: return "UNKNOWN";
	}
}

static int64_t be64_to_cpu_signed(uint64_t v_be)
{
	uint64_t v = ((uint64_t)ntohl((uint32_t)(v_be & 0xffffffffULL)) << 32) |
				  (uint64_t)ntohl((uint32_t)(v_be >> 32));
	return (int64_t)v;
}

static void format_clock_id(const struct ptp_clock_id *id, char *out, size_t outlen)
{
	switch_snprintf(out, outlen, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
					id->v[0], id->v[1], id->v[2], id->v[3],
					id->v[4], id->v[5], id->v[6], id->v[7]);
}

static switch_status_t open_uds(ptp_source_t *src)
{
	struct sockaddr_un addr;
	const char *uds = mod_ptp_globals.ptp4l_socket;

	if (zstr(uds)) {
		uds = "/var/run/ptp4l";
	}

	src->sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (src->sockfd < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "ptp: socket() failed: %s\n", strerror(errno));
		return SWITCH_STATUS_FALSE;
	}

	switch_snprintf(src->client_path, sizeof(src->client_path),
					"/tmp/mod_ptp_timer.%d.sock", (int)getpid());
	unlink(src->client_path);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	switch_snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", src->client_path);
	if (bind(src->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "ptp: bind(%s) failed: %s\n", src->client_path, strerror(errno));
		close(src->sockfd);
		src->sockfd = -1;
		return SWITCH_STATUS_FALSE;
	}

	switch_snprintf(src->server_path, sizeof(src->server_path), "%s", uds);

	{
		struct timeval tv = { 0, 200 * 1000 }; /* 200 ms */
		setsockopt(src->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t send_get(ptp_source_t *src, uint16_t mgmt_id)
{
	uint8_t buf[64];
	struct ptp_management_msg *m = (struct ptp_management_msg *)buf;
	struct ptp_tlv_hdr *tlv = (struct ptp_tlv_hdr *)(buf + sizeof(*m));
	struct sockaddr_un to;
	size_t total = sizeof(*m) + sizeof(*tlv);

	memset(buf, 0, total);

	m->hdr.msg_type_transport = PTP_MSG_MANAGEMENT;
	m->hdr.version_ptp = 0x02;
	m->hdr.message_length = htons((uint16_t)total);
	m->hdr.domain_number = 0;
	m->hdr.source_port.clock = src->client_clock_id;
	m->hdr.source_port.port_number = htons(1);
	m->hdr.sequence_id = htons(++src->seq);
	m->hdr.control_field = 0x04; /* management */
	m->hdr.log_message_interval = 0x7F;

	memset(m->target_port.clock.v, 0xff, 8);
	m->target_port.port_number = htons(0xffff);
	m->starting_boundary_hops = 0;
	m->boundary_hops = 0;
	m->action = PTP_MGMT_ACTION_GET;

	tlv->tlv_type = htons(PTP_TLV_TYPE_MANAGEMENT);
	tlv->length = htons(2); /* just the managementId */
	tlv->management_id = htons(mgmt_id);

	memset(&to, 0, sizeof(to));
	to.sun_family = AF_UNIX;
	switch_snprintf(to.sun_path, sizeof(to.sun_path), "%s", src->server_path);

	if (sendto(src->sockfd, buf, total, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
		PTP_LOG_DEBUG("ptp: sendto(%s) failed: %s\n", src->server_path, strerror(errno));
		return SWITCH_STATUS_FALSE;
	}
	return SWITCH_STATUS_SUCCESS;
}

/* Receives one management response and dispatches it into status. */
static switch_status_t recv_one(ptp_source_t *src, ptp_status_t *st)
{
	uint8_t buf[1500];
	ssize_t n = recv(src->sockfd, buf, sizeof(buf), 0);
	struct ptp_management_msg *m;
	struct ptp_tlv_hdr *tlv;
	uint16_t mgmt_id;
	uint8_t  *payload;
	size_t   plen;

	if (n < (ssize_t)(sizeof(*m) + sizeof(*tlv))) {
		return SWITCH_STATUS_FALSE;
	}

	m = (struct ptp_management_msg *)buf;
	if ((m->hdr.msg_type_transport & 0x0F) != PTP_MSG_MANAGEMENT) {
		return SWITCH_STATUS_FALSE;
	}
	if ((m->action & 0x0F) != PTP_MGMT_ACTION_RESPONSE) {
		return SWITCH_STATUS_FALSE;
	}

	tlv = (struct ptp_tlv_hdr *)(buf + sizeof(*m));
	mgmt_id = ntohs(tlv->management_id);
	payload = (uint8_t *)tlv + sizeof(*tlv);
	plen    = (size_t)n - (sizeof(*m) + sizeof(*tlv));

	switch (mgmt_id) {
	case PTP_MGMT_ID_CURRENT_DATA_SET: {
		struct ptp_current_ds *cd = (struct ptp_current_ds *)payload;
		if (plen < sizeof(*cd)) break;
		st->servo_steps      = (uint32_t)ntohs(cd->steps_removed);
		st->master_offset_ns = be64_to_cpu_signed(cd->offset_from_master) >> 16;
		st->path_delay_ns    = be64_to_cpu_signed(cd->mean_path_delay)    >> 16;
		src->have_current    = SWITCH_TRUE;
		break;
	}
	case PTP_MGMT_ID_PARENT_DATA_SET: {
		struct ptp_parent_ds_head *pd = (struct ptp_parent_ds_head *)payload;
		if (plen < sizeof(*pd)) break;
		format_clock_id(&pd->grandmaster_identity, st->grandmaster_id, sizeof(st->grandmaster_id));
		src->have_parent = SWITCH_TRUE;
		break;
	}
	case PTP_MGMT_ID_PORT_DATA_SET: {
		struct ptp_port_ds_head *pds = (struct ptp_port_ds_head *)payload;
		if (plen < sizeof(*pds)) break;
		switch_snprintf(st->port_state, sizeof(st->port_state), "%s",
						port_state_name(pds->port_state));
		src->have_port = SWITCH_TRUE;
		break;
	}
	default:
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

/* -------- public API -------- */

switch_status_t ptp_source_create(ptp_source_t **src_out, switch_memory_pool_t *pool)
{
	ptp_source_t *src = switch_core_alloc(pool, sizeof(*src));
	switch_uuid_t uuid;

	memset(src, 0, sizeof(*src));
	src->pool   = pool;
	src->sockfd = -1;

	/* Derive a stable-ish "clock identity" for our client port. */
	switch_uuid_get(&uuid);
	memcpy(src->client_clock_id.v, uuid.data, 8);

	if (open_uds(src) != SWITCH_STATUS_SUCCESS) {
		/* The daemon may simply not be running yet; we still load. */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ptp: ptp4l socket not reachable yet, will retry on poll\n");
	}

	switch_snprintf(src->client_path, sizeof(src->client_path),
					"%s", src->client_path);

	*src_out = src;
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t ptp_source_poll(ptp_source_t *src, ptp_status_t *st)
{
	int i;

	memset(st, 0, sizeof(*st));
	st->state     = PTP_SYNC_NONE;
	st->sample_us = switch_micro_time_now();
	switch_snprintf(st->source_desc, sizeof(st->source_desc),
					"ptp4l@%s", mod_ptp_globals.ptp4l_socket);

	if (src->sockfd < 0 && open_uds(src) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_SUCCESS;
	}

	src->have_current = src->have_parent = src->have_port = SWITCH_FALSE;

	send_get(src, PTP_MGMT_ID_CURRENT_DATA_SET);
	send_get(src, PTP_MGMT_ID_PARENT_DATA_SET);
	send_get(src, PTP_MGMT_ID_PORT_DATA_SET);

	for (i = 0; i < 6; i++) {
		if (recv_one(src, st) != SWITCH_STATUS_SUCCESS) break;
		if (src->have_current && src->have_parent && src->have_port) break;
	}

	if (!src->have_port) {
		st->state = PTP_SYNC_NONE;
	} else if (!strcmp(st->port_state, "SLAVE")) {
		st->state = PTP_SYNC_LOCKED;
	} else if (!strcmp(st->port_state, "UNCALIBRATED") ||
			   !strcmp(st->port_state, "PRE_MASTER")   ||
			   !strcmp(st->port_state, "LISTENING")) {
		st->state = PTP_SYNC_HOLDOVER;
	} else if (!strcmp(st->port_state, "MASTER")) {
		/* We are the GM — treat as locked to ourselves. */
		st->state = PTP_SYNC_LOCKED;
	} else {
		st->state = PTP_SYNC_NONE;
	}

	return SWITCH_STATUS_SUCCESS;
}

void ptp_source_destroy(ptp_source_t **src_io)
{
	ptp_source_t *src;
	if (!src_io || !*src_io) return;
	src = *src_io;
	if (src->sockfd >= 0) {
		close(src->sockfd);
		src->sockfd = -1;
	}
	if (src->client_path[0]) {
		unlink(src->client_path);
	}
	*src_io = NULL;
}

#endif /* __linux__ */
