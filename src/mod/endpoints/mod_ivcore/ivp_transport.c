/**
 * ivp_transport.c
 *
 * IVP TCP login handshake and UDP media transport for mod_ivcore.
 *
 * Wire format translations are derived directly from the IVCore C# source:
 *   - TCP login:    TcpLoginClient.cs / LoginMessages.cs
 *   - IVP frames:   IvpFrames.cs (IvpProtocolFrameHeader, IvpMediaFrameHeader,
 *                   IvpIeHelper)
 *   - UDP transport: IvpUdpTransport.cs
 *
 * Sockets are POSIX (Linux/FreeBSD) or Winsock2 (Windows).
 */

#include "mod_ivcore.h"
#include "ivp_transport.h"
#include "ivp_hdlc.h"

#include <switch.h>
#include <inttypes.h>

/* Cross-platform socket compat ------------------------------------------
 * On Windows, switch.h / APR pulls in WinSock2.h which provides the BSD
 * socket API via the Winsock2 layer.  We still need a few POSIX->Winsock
 * name-compatibility shims and type definitions.
 */
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef int      ssize_t;
   typedef int      socklen_t;
#  define sock_close(s)  closesocket(s)
#  define sock_errno()   WSAGetLastError()
#  define SOCK_EAGAIN    WSAEWOULDBLOCK
#  define SOCK_EWOULDBLOCK WSAEWOULDBLOCK
   static const char *sock_strerror(int e) {
	   static char buf[128];
	   FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					  NULL, (DWORD)e, 0, buf, sizeof(buf), NULL);
	   return buf;
   }
#  define IVC_INET_PTON(af, src, dst)  InetPtonA((af), (src), (dst))
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  define sock_close(s)       close(s)
#  define sock_errno()        errno
#  define SOCK_EAGAIN         EAGAIN
#  define SOCK_EWOULDBLOCK    EWOULDBLOCK
#  define sock_strerror(e)    strerror(e)
#  define IVC_INET_PTON(af, src, dst) inet_pton((af), (src), (dst))
#endif

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

/* =====================================================================
 * Debug hex-dump helper
 * Prints up to 'len' bytes as "XX XX XX ..." on a single LOG_DEBUG line.
 * ===================================================================*/
static void ivp_hexdump(const char *label, const uint8_t *buf, int len)
{
#define IVP_HEXDUMP_COLS 32
	char line[IVP_HEXDUMP_COLS * 3 + 4];
	int  i, pos;
	for (i = 0; i < len; i += IVP_HEXDUMP_COLS) {
		int chunk = len - i;
		int c;
		if (chunk > IVP_HEXDUMP_COLS) chunk = IVP_HEXDUMP_COLS;
		pos = 0;
		for (c = 0; c < chunk; c++) {
			pos += switch_snprintf(line + pos, (int)sizeof(line) - pos,
				"%02X ", buf[i + c]);
		}
		if (pos > 0) line[pos - 1] = '\0';
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"mod_ivcore:   %s [%03d] %s\n", label, i, line);
	}
#undef IVP_HEXDUMP_COLS
}

/* =====================================================================
 * Byte-order helpers (big-endian <-> host)
 * ===================================================================*/

static void put_u16_be(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xFF);
}

static void put_u32_be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >>  8);
	p[3] = (uint8_t)(v  & 0xFF);
}

static uint16_t get_u16_be(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get_u32_be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
		 | ((uint32_t)p[2] <<  8) | (uint32_t)p[3];
}

static void put_u16_le(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)(v >> 8);
}

/* =====================================================================
 * IVP Information Element serialisers
 * Matches IvpIeHelper in IvpFrames.cs
 * ===================================================================*/

int ivp_ie_append_string(uint8_t *buf, ivp_ie_key_t key, const char *value)
{
	int len;
	if (!value) return 0;
	len = (int)strlen(value);
	buf[0] = (uint8_t)key;
	buf[1] = (uint8_t)len;
	memcpy(buf + 2, value, (size_t)len);
	return 2 + len;
}

int ivp_ie_append_dword(uint8_t *buf, ivp_ie_key_t key, uint32_t value)
{
	buf[0] = (uint8_t)key;
	buf[1] = 4;
	put_u32_be(buf + 2, value);
	return 6;
}

int ivp_ie_append_word(uint8_t *buf, ivp_ie_key_t key, uint16_t value)
{
	buf[0] = (uint8_t)key;
	buf[1] = 2;
	put_u16_be(buf + 2, value);
	return 4;
}

int ivp_ie_append_byte(uint8_t *buf, ivp_ie_key_t key, uint8_t value)
{
	buf[0] = (uint8_t)key;
	buf[1] = 1;
	buf[2] = value;
	return 3;
}

int ivp_ie_append_provisioning(uint8_t *buf,
								uint16_t frame_size,
								uint16_t frame_time,
								uint16_t frames_per_packet)
{
	buf[0] = (uint8_t)IVP_IE_PROVISIONING;
	buf[1] = 6;
	put_u16_le(buf + 2, frame_size);
	put_u16_le(buf + 4, frame_time);
	put_u16_le(buf + 6, frames_per_packet);
	return 8;
}

/* =====================================================================
 * IVP Protocol Frame Header serialisation
 * ===================================================================*/

void ivp_write_proto_header(uint8_t *buf, const ivp_proto_header_t *hdr)
{
	put_u16_be(buf,     (uint16_t)(hdr->src_call_number | 0x8000));
	put_u16_be(buf + 2, (uint16_t)(hdr->dst_call_number | (hdr->is_resent ? 0x8000 : 0)));
	put_u16_be(buf + 4, (uint16_t)(hdr->src_call_number2 | 0x8000));
	put_u32_be(buf + 6, hdr->timestamp);
	buf[10] = hdr->out_sequence;
	buf[11] = hdr->in_sequence;
	buf[12] = (uint8_t)hdr->frame_type;
	buf[13] = hdr->subclass;
}

void ivp_read_proto_header(const uint8_t *buf, ivp_proto_header_t *hdr)
{
	uint16_t r0 = get_u16_be(buf);
	uint16_t r1 = get_u16_be(buf + 2);
	uint16_t r2 = get_u16_be(buf + 4);
	hdr->src_call_number  = r0 & 0x7FFF;
	hdr->dst_call_number  = r1 & 0x7FFF;
	hdr->is_resent        = (r1 & 0x8000) != 0;
	hdr->src_call_number2 = r2 & 0x7FFF;
	hdr->timestamp        = get_u32_be(buf + 6);
	hdr->out_sequence     = buf[10];
	hdr->in_sequence      = buf[11];
	hdr->frame_type       = (ivp_frame_type_t)buf[12];
	hdr->subclass         = buf[13];
}

/* =====================================================================
 * IVP Media Frame Header serialisation
 * ===================================================================*/

int ivp_write_media_header(uint8_t *buf, const ivp_media_header_t *hdr)
{
	uint16_t red_len;
	buf[0]  = (uint8_t)((hdr->src_call_number >> 8) & 0x7F);
	buf[1]  = (uint8_t)(hdr->src_call_number & 0xFF);
	buf[2]  = hdr->recovery_sequence;
	red_len = (uint16_t)(((hdr->redundancy_layers & 0x0F) << 12)
						  | (hdr->media_length & 0x0FFF));
	put_u16_be(buf + 3, red_len);
	put_u16_be(buf + 5, hdr->media_sequence_number);
	buf[7]  = hdr->src_nq;
	buf[8]  = hdr->src_type;
	put_u16_be(buf + 9,  hdr->src_free);
	put_u32_be(buf + 11, hdr->src_user_id);
	put_u16_be(buf + 15, hdr->signal_level);
	return 17;
}

int ivp_read_media_header(const uint8_t *buf, ivp_media_header_t *hdr)
{
	uint16_t red_len;
	hdr->src_call_number     = (uint16_t)(((buf[0] & 0x7F) << 8) | buf[1]);
	hdr->recovery_sequence   = buf[2];
	red_len                  = get_u16_be(buf + 3);
	hdr->redundancy_layers   = (uint8_t)((red_len >> 12) & 0x0F);
	hdr->media_length        = (uint16_t)(red_len & 0x0FFF);
	hdr->media_sequence_number = get_u16_be(buf + 5);
	hdr->src_nq              = buf[7];
	hdr->src_type            = buf[8];
	hdr->src_free            = get_u16_be(buf + 9);
	hdr->src_user_id         = get_u32_be(buf + 11);
	hdr->signal_level        = get_u16_be(buf + 15);
	return 17;
}

/* =====================================================================
 * TCP Login Handshake
 * ===================================================================*/

#define IVC_LOGIN_MSG_ID        0x0B
#define IVC_LOGIN_RESP_ID       0x0A
#define IVC_LQ_LOGIN_MSG_ID     0x1B
#define IVC_LQ_LOGIN_RESP_ID    0x1A
#define IVC_LOGIN_NO_MSG        0xFF
#define IVC_PANEL_TYPE_LQ       0x8110
#define IVC_PANEL_TYPE_STDPANEL 0x8012

static const char *ivp_username_prefix(const char *device_type)
{
	if (!device_type || !*device_type) return "lqsip";
	if (!strcasecmp(device_type, "lqsip"))  return "lqsip";
	if (!strcasecmp(device_type, "lq4"))    return "lq4";
	if (!strcasecmp(device_type, "lq2"))    return "lq2";
	if (!strcasecmp(device_type, "lqivc"))  return "lqivc";
	if (!strcasecmp(device_type, "aoip"))   return "aoip";
	if (!strcasecmp(device_type, "mobile")) return "aic";
	if (!strcasecmp(device_type, "panel"))  return "client_ivr";
	return "client_ivr";
}

static switch_bool_t is_lq_login(const char *dtype)
{
	return (!dtype || !*dtype
		|| strcasecmp(dtype, "lqsip")  == 0
		|| strcasecmp(dtype, "lq4")    == 0
		|| strcasecmp(dtype, "mobile") == 0)
		? SWITCH_TRUE : SWITCH_FALSE;
}

static int build_base_login(uint8_t *tx, const ivcore_conn_params_t *p,
							  uint8_t msg_id, uint8_t payload_len, uint16_t panel_type,
							  uint16_t local_udp_port)
{
	int off = 0;
	int nlen;
	memset(tx, 0, 56);

	tx[off++] = msg_id;
	tx[off++] = payload_len;

	/* PanelName (10 bytes, null-padded) */
	nlen = (int)strlen(p->username);
	if (nlen > IVC_PANEL_NAME_LEN) nlen = IVC_PANEL_NAME_LEN;
	memcpy(tx + off, p->username, (size_t)nlen);
	off += IVC_PANEL_NAME_LEN;

	/* PanelIpAddress — 0.0.0.0 */
	off += 4;

	/* ServerPort: the LOCAL UDP port we are listening on.
	 * The IPA uses this to know where to send media back to us. */
	put_u16_be(tx + off, local_udp_port);
	off += 2;

	/* ServerIpAddress */
	{
		uint32_t ip4 = inet_addr(p->server_ip);
		memcpy(tx + off, &ip4, 4);
		off += 4;
	}

	/* NetworkMask, Gateway, DNS — 0.0.0.0 each */
	off += 12;

	/* HardwareId / MAC — zeroed */
	off += IVC_MAC_ADDR_LEN;

	/* NetOptions=0, ConnectionType=0 (LAN) */
	off += 2;

	/* LoginStatus=0, TransactionNumber=0 */
	off += 4;

	/* PanelType */
	put_u16_be(tx + off, panel_type);
	off += 2;

	/* UpTime=0, UpConnectionTime=0 */
	off += 8;  /* total = 56 */
	return off;
}

switch_status_t ivp_tcp_login(ivcore_channel_t *ch)
{
	struct sockaddr_in addr;
	int sock = -1;
	uint8_t tx[80];
	uint8_t rx[128];
	ssize_t n;
	int tx_len;
	switch_bool_t lq_mode;
	uint8_t resp_id;

	memset(&addr, 0, sizeof(addr));
	memset(tx, 0, sizeof(tx));
	memset(rx, 0, sizeof(rx));

	lq_mode = is_lq_login(ch->params.device_type);

	addr.sin_family = AF_INET;
	addr.sin_port   = htons((uint16_t)ch->params.tcp_port);
	if (IVC_INET_PTON(AF_INET, ch->params.server_ip, &addr.sin_addr) != 1) {
		struct hostent *he = gethostbyname(ch->params.server_ip);
		if (!he) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"mod_ivcore: cannot resolve host '%s'\n", ch->params.server_ip);
			return SWITCH_STATUS_FALSE;
		}
		memcpy(&addr.sin_addr, he->h_addr_list[0], 4);
	}

	sock = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"mod_ivcore: socket() failed: %s\n", sock_strerror(sock_errno()));
		return SWITCH_STATUS_FALSE;
	}

#ifdef _WIN32
	{
		DWORD tv_ms = 5000;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
		setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
	}
#else
	{
		struct timeval tv;
		tv.tv_sec = 5; tv.tv_usec = 0;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
#endif

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"mod_ivcore: TCP connect to %s:%d failed: %s\n",
			ch->params.server_ip, ch->params.tcp_port, sock_strerror(sock_errno()));
		sock_close(sock);
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: TCP connected to %s:%d (sock=%d)\n",
		ch->params.server_ip, ch->params.tcp_port, sock);

	if (lq_mode) {
		tx_len = build_base_login(tx, &ch->params,
			IVC_LQ_LOGIN_MSG_ID, 71, IVC_PANEL_TYPE_LQ, ch->local_udp_port);
		tx_len += 16; /* 16-byte unique client ID at [56..71] — zeroed */
		tx_len += 2;  /* 2-byte pad at [72..73] */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"mod_ivcore: sending LQ login (%s) to %s:%d localUdpPort=%u\n",
			ch->params.device_type, ch->params.server_ip, ch->params.tcp_port,
			(unsigned)ch->local_udp_port);
	} else {
		tx_len = build_base_login(tx, &ch->params,
			IVC_LOGIN_MSG_ID, 55, IVC_PANEL_TYPE_STDPANEL, ch->local_udp_port);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"mod_ivcore: sending standard login (%s) to %s:%d localUdpPort=%u\n",
			ch->params.device_type, ch->params.server_ip, ch->params.tcp_port,
			(unsigned)ch->local_udp_port);
	}

	n = send(sock, (const char *)tx, (int)tx_len, 0);
	if (n != (ssize_t)tx_len) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"mod_ivcore: TCP send failed: %s\n", sock_strerror(sock_errno()));
		sock_close(sock);
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: TCP login TX %d bytes (msgId=0x%02X mode=%s username='%s' localUdpPort=%u)\n",
		tx_len, tx[0], lq_mode ? "LQ" : "STD", ch->params.username,
		(unsigned)ch->local_udp_port);
	ivp_hexdump("TX", tx, tx_len);

	n = recv(sock, (char *)rx, (int)sizeof(rx), 0);
	if (n < 1) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"mod_ivcore: TCP recv failed or empty response (n=%d): %s\n",
			(int)n, sock_strerror(sock_errno()));
		sock_close(sock);
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: TCP login RX %d bytes (respId=0x%02X)\n", (int)n, rx[0]);
	ivp_hexdump("RX", rx, (int)n);

	resp_id = rx[0];
	if (resp_id == IVC_LOGIN_NO_MSG) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_ivcore: Login rejected by IVC card (NoMessages)\n");
		sock_close(sock);
		return SWITCH_STATUS_FALSE;
	}

	if (lq_mode) {
		uint8_t  udp_ip[4];
		uint16_t udp_port;
		char     connect_user[IVC_PANEL_NAME_LEN + 1];
		const char *ivp_user;
		uint16_t failure_code;

		if (resp_id != IVC_LQ_LOGIN_RESP_ID) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: unexpected LQ login response 0x%02X\n", resp_id);
			sock_close(sock);
			return SWITCH_STATUS_FALSE;
		}
		if (n < 22) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"mod_ivcore: LQ login response too short (%d bytes)\n", (int)n);
			sock_close(sock);
			return SWITCH_STATUS_FALSE;
		}

		failure_code = get_u16_be(rx + 20);
		if (failure_code != 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: LQ login failure code %u\n", (unsigned)failure_code);
			sock_close(sock);
			return SWITCH_STATUS_FALSE;
		}

		memcpy(udp_ip, rx + 8, 4);
		udp_port = get_u16_be(rx + 12);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"mod_ivcore: LQ resp parsed — udpIp=%d.%d.%d.%d udpPort=%u failureCode=%u rx[2..7]=%02X %02X %02X %02X %02X %02X\n",
			udp_ip[0], udp_ip[1], udp_ip[2], udp_ip[3], (unsigned)udp_port,
			(unsigned)failure_code,
			rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);

		memset(connect_user, 0, sizeof(connect_user));
		if (n >= 32) {
			memcpy(connect_user, rx + 22, IVC_PANEL_NAME_LEN);
			connect_user[IVC_PANEL_NAME_LEN] = '\0';
		}

		ivp_user = (connect_user[0]) ? connect_user : ch->params.username;
		switch_snprintf(ch->params.username, sizeof(ch->params.username),
			"%s.%s", ivp_username_prefix(ch->params.device_type), ivp_user);

		memset(&ch->remote_addr, 0, sizeof(ch->remote_addr));
		ch->remote_addr.sin_family = AF_INET;
		memcpy(&ch->remote_addr.sin_addr, udp_ip, 4);
		ch->remote_addr.sin_port = htons(udp_port);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"mod_ivcore: LQ TCP login OK — UDP %d.%d.%d.%d:%d ivpUser='%s'\n",
			udp_ip[0], udp_ip[1], udp_ip[2], udp_ip[3], (int)udp_port,
			ch->params.username);
	} else {
		uint8_t  udp_ip[4];
		uint16_t udp_port;
		char     orig_user[128];

		if (resp_id != IVC_LOGIN_RESP_ID) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: unexpected login response msgId=0x%02X\n", resp_id);
			sock_close(sock);
			return SWITCH_STATUS_FALSE;
		}
		if (n < 14) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"mod_ivcore: Login response too short (%d bytes)\n", (int)n);
			sock_close(sock);
			return SWITCH_STATUS_FALSE;
		}

		memcpy(udp_ip, rx + 2, 4);
		udp_port = get_u16_be(rx + 6);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"mod_ivcore: STD resp parsed — udpIp=%d.%d.%d.%d udpPort=%u rx[1]=%02X rx[8..13]=%02X %02X %02X %02X %02X %02X\n",
			udp_ip[0], udp_ip[1], udp_ip[2], udp_ip[3], (unsigned)udp_port,
			rx[1], rx[8], rx[9], rx[10], rx[11], rx[12], rx[13]);

		switch_copy_string(orig_user, ch->params.username, sizeof(orig_user));
		switch_snprintf(ch->params.username, sizeof(ch->params.username),
			"%s.%s", ivp_username_prefix(ch->params.device_type), orig_user);

		memset(&ch->remote_addr, 0, sizeof(ch->remote_addr));
		ch->remote_addr.sin_family = AF_INET;
		memcpy(&ch->remote_addr.sin_addr, udp_ip, 4);
		ch->remote_addr.sin_port = htons(udp_port);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"mod_ivcore: TCP login OK — UDP server %d.%d.%d.%d:%d ivpUser='%s'\n",
			udp_ip[0], udp_ip[1], udp_ip[2], udp_ip[3], (int)udp_port,
			ch->params.username);
	}

	ch->tcp_sock = sock;
	return SWITCH_STATUS_SUCCESS;
}

/* =====================================================================
 * UDP Socket
 * ===================================================================*/

switch_status_t ivp_udp_open(ivcore_channel_t *ch)
{
	struct sockaddr_in local;
	int sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"mod_ivcore: UDP socket() failed: %s\n", sock_strerror(sock_errno()));
		return SWITCH_STATUS_FALSE;
	}

	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port = 0;
	if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"mod_ivcore: UDP bind failed: %s\n", sock_strerror(sock_errno()));
		sock_close(sock);
		return SWITCH_STATUS_FALSE;
	}

	/* Capture the ephemeral port the OS assigned so we can send it in the TCP login. */
	{
		struct sockaddr_in bound;
		socklen_t blen = sizeof(bound);
		if (getsockname(sock, (struct sockaddr *)&bound, &blen) == 0) {
			ch->local_udp_port = ntohs(bound.sin_port);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"mod_ivcore: UDP socket bound on local port %u (sock=%d)\n",
				(unsigned)ch->local_udp_port, sock);
		} else {
			ch->local_udp_port = 0;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: getsockname failed: %s — local_udp_port will be 0\n",
				sock_strerror(sock_errno()));
		}
	}

#ifdef _WIN32
	{
		DWORD tv_ms = 4;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
	}
#else
	{
		struct timeval tv;
		tv.tv_sec = 0; tv.tv_usec = 4000;  /* 4 ms — tight enough for 8 ms IVP cadence */
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
#endif

	ch->udp_sock      = sock;
	ch->local_call_number = (uint16_t)(rand() & 0x7FFF);
	if (ch->local_call_number == 0) ch->local_call_number = 1;
	ch->timestamp_base = (uint32_t)time(NULL);
	ch->out_sequence   = 0;
	ch->in_sequence    = 0;
	ch->media_sequence_out = 0;
	ch->call_state     = IVC_STATE_CONNECTING;
	ivp_hdlc_reset(ch);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: UDP socket opened, localCallNo=0x%04X\n",
		(unsigned)ch->local_call_number);
	return SWITCH_STATUS_SUCCESS;
}

/* =====================================================================
 * Internal: send a raw protocol frame (header + IE payload)
 * ===================================================================*/

/* Per IAX2 (which IVP mirrors) the following protocol subclasses do NOT
 * consume an outgoing sequence number: ACK, PING, PONG, LAG_REQ, LAG_REPLY.
 * Everything else (NEW, ACCEPT, REJECT, HANGUP, Control, Data) does. */
static switch_bool_t proto_consumes_oseq(ivp_frame_type_t ft, uint8_t sub)
{
	if (ft != IVP_FRAME_PROTOCOL) return SWITCH_TRUE;
	switch (sub) {
	case IVP_PROTO_ACK:
	case IVP_PROTO_PING:
	case IVP_PROTO_PONG:
	case IVP_PROTO_LAG_REQ:
	case IVP_PROTO_LAG_REPLY:
		return SWITCH_FALSE;
	default:
		return SWITCH_TRUE;
	}
}

static switch_status_t send_proto_frame(ivcore_channel_t *ch,
										 ivp_frame_type_t frame_type,
										 uint8_t subclass,
										 const uint8_t *payload, int payload_len)
{
	uint8_t buf[IVC_MAX_FRAME_BYTES];
	ivp_proto_header_t hdr;
	int total;
	ssize_t sent;
	switch_bool_t consume = proto_consumes_oseq(frame_type, subclass);

	if (IVP_PROTO_HEADER_SIZE + payload_len > (int)sizeof(buf))
		return SWITCH_STATUS_FALSE;

	memset(&hdr, 0, sizeof(hdr));
	hdr.src_call_number  = ch->local_call_number;
	hdr.src_call_number2 = ch->local_call_number;
	hdr.dst_call_number  = ch->remote_call_number;
	hdr.timestamp        = (uint32_t)time(NULL) - ch->timestamp_base;
	hdr.out_sequence     = ch->out_sequence;
	hdr.in_sequence      = ch->in_sequence;
	hdr.frame_type       = frame_type;
	hdr.subclass         = subclass;
	hdr.is_resent        = 0;
	if (consume) ch->out_sequence++;

	ivp_write_proto_header(buf, &hdr);
	if (payload && payload_len > 0)
		memcpy(buf + IVP_PROTO_HEADER_SIZE, payload, (size_t)payload_len);

	total = IVP_PROTO_HEADER_SIZE + payload_len;
	sent = (ssize_t)sendto(ch->udp_sock, (const char *)buf, total, 0,
						  (struct sockaddr *)&ch->remote_addr,
						  (socklen_t)sizeof(ch->remote_addr));
	return (sent == (ssize_t)total) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/* =====================================================================
 * IVP NEW frame
 * ===================================================================*/

switch_status_t ivp_send_new(ivcore_channel_t *ch)
{
	uint8_t payload[1024];
	int off = 0;

	off += ivp_ie_append_string (payload + off, IVP_IE_USERNAME,       ch->params.username);
	off += ivp_ie_append_string (payload + off, IVP_IE_CALLING_NAME,   ch->params.calling_name);
	off += ivp_ie_append_string (payload + off, IVP_IE_DISPLAY_NAME,   ch->params.display_name);
	off += ivp_ie_append_dword  (payload + off, IVP_IE_FORMAT,         ch->params.codec_format);
	off += ivp_ie_append_dword  (payload + off, IVP_IE_CAPABILITY,     ch->params.codec_family);
	off += ivp_ie_append_dword  (payload + off, IVP_IE_USER_ID,        ch->params.user_id);
	off += ivp_ie_append_byte   (payload + off, IVP_IE_PROTECTION_LEVEL, ch->params.protection_level);
	off += ivp_ie_append_word   (payload + off, IVP_IE_SAMPLING_RATE,  ch->params.sampling_rate);
	off += ivp_ie_append_provisioning(payload + off,
									  ch->params.frame_size,
									  ch->params.frame_time,
									  ch->params.frames_per_packet);
	off += ivp_ie_append_string (payload + off, IVP_IE_VERSION,        ch->params.version_string);

	if (ch->params.called_number[0])
		off += ivp_ie_append_string(payload + off, IVP_IE_CALLED_NUMBER, ch->params.called_number);

	if (ch->params.called_context[0])
		off += ivp_ie_append_string(payload + off, IVP_IE_CALLED_CONTEXT, ch->params.called_context);

	if (ch->params.encryption_key[0])
		off += ivp_ie_append_string(payload + off, IVP_IE_ENCRYPTION_KEY, ch->params.encryption_key);

	if (ch->params.auth_key[0])
		off += ivp_ie_append_string(payload + off, IVP_IE_AUTH_KEY, ch->params.auth_key);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: ivp_send_new — username='%s' callingName='%s' displayName='%s' "
		"calledNum='%s' context='%s' codec=0x%04X rate=%u frameSize=%u frameTime=%u "
		"remoteAddr=%d.%d.%d.%d:%u localCallNo=0x%04X payloadBytes=%d\n",
		ch->params.username, ch->params.calling_name, ch->params.display_name,
		ch->params.called_number, ch->params.called_context,
		(unsigned)ch->params.codec_format, (unsigned)ch->params.sampling_rate,
		(unsigned)ch->params.frame_size, (unsigned)ch->params.frame_time,
		(uint8_t)((ntohl(ch->remote_addr.sin_addr.s_addr) >> 24) & 0xFF),
		(uint8_t)((ntohl(ch->remote_addr.sin_addr.s_addr) >> 16) & 0xFF),
		(uint8_t)((ntohl(ch->remote_addr.sin_addr.s_addr) >>  8) & 0xFF),
		(uint8_t)( ntohl(ch->remote_addr.sin_addr.s_addr)        & 0xFF),
		(unsigned)ntohs(ch->remote_addr.sin_port),
		(unsigned)ch->local_call_number, off);
	ivp_hexdump("NEW-payload", payload, off);

	return send_proto_frame(ch, IVP_FRAME_PROTOCOL, IVP_PROTO_NEW, payload, off);
}

/* =====================================================================
 * IVP ACK frame
 * ===================================================================*/

/* Per IAX2/IVP, an ACK echoes the OSEQ/ISEQ of the frame being
 * acknowledged: ack.oseq = received.iseq, ack.iseq = received.oseq + 1.
 * The ACK must also echo the timestamp of the acknowledged frame so the
 * sender's reliable layer can match it to its retransmit queue.
 * It does NOT consume our own sequence space. */
switch_status_t ivp_send_ack(ivcore_channel_t *ch,
							  uint16_t dst_call_number,
							  uint8_t  in_seq,
							  uint8_t  out_seq,
							  uint32_t echo_ts)
{
	uint8_t buf[IVP_PROTO_HEADER_SIZE];
	ivp_proto_header_t hdr;
	ssize_t sent;
	int err;

	memset(&hdr, 0, sizeof(hdr));
	hdr.src_call_number  = ch->local_call_number;
	hdr.src_call_number2 = ch->local_call_number;
	hdr.dst_call_number  = dst_call_number;
	hdr.timestamp        = echo_ts;
	hdr.out_sequence     = in_seq;              /* echo their iseq         */
	hdr.in_sequence      = (uint8_t)(out_seq + 1); /* next expected        */
	hdr.frame_type       = IVP_FRAME_PROTOCOL;
	hdr.subclass         = IVP_PROTO_ACK;
	hdr.is_resent        = 0;

	ivp_write_proto_header(buf, &hdr);
	sent = (ssize_t)sendto(ch->udp_sock, (const char *)buf,
						   IVP_PROTO_HEADER_SIZE, 0,
						   (struct sockaddr *)&ch->remote_addr,
						   (socklen_t)sizeof(ch->remote_addr));
	err = (sent == (ssize_t)IVP_PROTO_HEADER_SIZE) ? 0 : sock_errno();
	IVC_LOG_DEBUG("mod_ivcore: TX ACK dstCallNo=0x%04X oseq=%u iseq=%u ts=%u sent=%d err=%d\n",
		(unsigned)dst_call_number, (unsigned)hdr.out_sequence,
		(unsigned)hdr.in_sequence, (unsigned)echo_ts, (int)sent, err);
	return (sent == (ssize_t)IVP_PROTO_HEADER_SIZE)
		? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/* =====================================================================
 * IVP HANGUP frame
 * ===================================================================*/

switch_status_t ivp_send_hangup(ivcore_channel_t *ch)
{
	return send_proto_frame(ch, IVP_FRAME_PROTOCOL, IVP_PROTO_HANGUP, NULL, 0);
}

/* =====================================================================
 * IVP Media Frame
 * ===================================================================*/

switch_status_t ivp_send_media(ivcore_channel_t *ch,
								const uint8_t *payload, int payload_len)
{
	uint8_t buf[IVC_MAX_FRAME_BYTES];
	ivp_media_header_t mhdr;
	int hdr_len;
	int total;
	ssize_t sent;

	memset(&mhdr, 0, sizeof(mhdr));
	mhdr.src_call_number       = ch->local_call_number;
	mhdr.recovery_sequence     = 0;
	mhdr.redundancy_layers     = 0;
	/* media_length includes 12 bytes of meta overhead, matching the matrix's
	 * own TX packets (observed: 64-byte audio → media_length=76). */
	mhdr.media_length          = (uint16_t)(payload_len + 12);
	mhdr.media_sequence_number = ch->media_sequence_out++;
	mhdr.src_nq                = 0;
	mhdr.src_type              = 0;
	mhdr.src_free              = 0;
	mhdr.src_user_id           = ch->params.user_id;
	mhdr.signal_level          = 0;

	hdr_len = ivp_write_media_header(buf, &mhdr);
	if (hdr_len + payload_len > (int)sizeof(buf))
		return SWITCH_STATUS_FALSE;

	memcpy(buf + hdr_len, payload, (size_t)payload_len);
	total = hdr_len + payload_len;

	sent = (ssize_t)sendto(ch->udp_sock, (const char *)buf, total, 0,
						  (struct sockaddr *)&ch->remote_addr,
						  (socklen_t)sizeof(ch->remote_addr));
	return (sent == (ssize_t)total) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/* =====================================================================
 * Close transport sockets
 * ===================================================================*/

void ivp_transport_steal(ivcore_channel_t *ch, int *tcp_out, int *udp_out)
{
    if (tcp_out) *tcp_out = ch->tcp_sock;
    if (udp_out) *udp_out = ch->udp_sock;
    /* Zero out the channel's socket fields so ivp_transport_close() (called
     * during the normal hangup teardown) will not close the sockets we
     * just handed off. */
    ch->tcp_sock = -1;
    ch->udp_sock = -1;
}

void ivp_transport_close(ivcore_channel_t *ch)
{
	if (ch->udp_sock >= 0) {
		sock_close(ch->udp_sock);
		ch->udp_sock = -1;
	}
	if (ch->tcp_sock >= 0) {
		sock_close(ch->tcp_sock);
		ch->tcp_sock = -1;
	}
	ch->call_state = IVC_STATE_IDLE;
}

/* =====================================================================
 * UDP Receive Loop
 * ===================================================================*/

static void handle_media_payload(ivcore_channel_t *ch,
								  const uint8_t *payload, int len)
{
	if (len > 0)
		ring_write(&ch->rx_ring, payload, (uint32_t)len);
}

/* Wrap a stuffed HDLC frame in an IVP type=7 sub=1 protocol frame and
 * send it.  Used as the ivp_hdlc_send_data_cb. */
static switch_status_t ivp_send_data_frame(ivcore_channel_t *ch,
										const uint8_t *frame, int frame_len)
{
	IVC_LOG_DEBUG("mod_ivcore: TX HDLC frame %d bytes (oseq=%u)\n",
		frame_len, (unsigned)ch->out_sequence);
	return send_proto_frame(ch, IVP_FRAME_DATA, 0x01, frame, frame_len);
}

void *ivp_recv_loop(switch_thread_t *thread, void *obj)
{
	ivcore_channel_t *ch = (ivcore_channel_t *)obj;
	uint8_t buf[IVC_MAX_FRAME_BYTES];
	struct sockaddr_in from;
	socklen_t from_len;

	/* NEW retransmit: resend every 500 ms until ACCEPT arrives or we give up. */
	switch_time_t last_new_sent = switch_micro_time_now();
	int new_retries = 1; /* 1 = first send already done before the loop */
#define IVP_NEW_RETRY_INTERVAL_US  500000   /* 500 ms */
#define IVP_NEW_MAX_RETRIES        10       /* give up after 5 s */

	/* Silence keep-alive: per PROTOCOL.md, the IVR's media-activity
	 * watchdog will hang up the call if no media frames arrive within
	 * a few seconds.  Once the call is UP, send a zero-payload media
	 * frame every ptime_ms.  ch->last_write_us is the single "last media
	 * sent" clock, stamped by write_frame for real audio AND by this loop
	 * for silence, guaranteeing exactly one packet per interval. */
	switch_bool_t have_in_seq = SWITCH_FALSE;

	(void)thread;

	/* dpi_send_cb is explicitly NULLed in ivcore_channel_alloc() before this
	 * thread is created, so no assignment is needed here.  It is set to
	 * ivp_send_data_frame the first time an IVP Data (HDLC) frame arrives. */
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: recv loop started for channel %p\n", (void *)ch);

	while (ch->running == SWITCH_TRUE) {
		ssize_t n;

		/* If the HDLC layer performed a SABME reset, clear our IVP-level
		 * in_sequence so the dedup window re-syncs.  Without this the IVP
		 * layer sees the freshly-reset HDLC I-frames (send_seq starting at 0
		 * again) as already-processed retransmits and either drops or
		 * double-delivers them, causing doubled dial strings. */
		if (ch->hdlc_reset_pending) {
			ch->in_sequence     = 0;
			have_in_seq         = SWITCH_FALSE;
			ch->hdlc_reset_pending = SWITCH_FALSE;
		}

		/* Retransmit NEW while we are still in CONNECTING state. */
		if (ch->call_state == IVC_STATE_CONNECTING) {
			switch_time_t now = switch_micro_time_now();
			if (now - last_new_sent >= IVP_NEW_RETRY_INTERVAL_US) {
				last_new_sent = now;
				if (new_retries >= IVP_NEW_MAX_RETRIES) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							"mod_ivcore: no ACCEPT after %d retries, hanging up\n",
							IVP_NEW_MAX_RETRIES);
						ch->call_state = IVC_STATE_HANGUP;
						ch->running    = SWITCH_FALSE;
						{
							switch_core_session_t *s = switch_core_session_locate(ch->session_uuid);
							if (s) {
								switch_channel_hangup(switch_core_session_get_channel(s),
									SWITCH_CAUSE_NETWORK_OUT_OF_ORDER);
								switch_core_session_rwunlock(s);
							}
						}
						break;
					}
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
						"mod_ivcore: retransmitting NEW (attempt %d/%d)\n",
						new_retries + 1, IVP_NEW_MAX_RETRIES);
					ivp_send_new(ch);
					new_retries++;
			}
		}

		/* Silence keepalive is now owned by ivp_tx_loop — skip here. */

		/* HDLC keep-alive: once the link is up, send an S-frame RR
		 * every ~3 s so the card's HDLC processor (4 s keepalive,
		 * 10 s inactive timeout) does not tear the call down. */
		if (ch->call_state == IVC_STATE_UP && ch->hdlc.link_up) {
			switch_time_t now = switch_micro_time_now();
			if (now - ch->hdlc.last_rr_us >= 3000000) {
				uint8_t rr[16];
				int rr_len = ivp_hdlc_build_rr(ch, rr, (int)sizeof(rr));
				if (rr_len > 0) {
					ivp_send_data_frame(ch, rr, rr_len);
					ch->hdlc.last_rr_us = now;
				}
			}
		}

		from_len = sizeof(from);
		n = (ssize_t)recvfrom(ch->udp_sock, (char *)buf, (int)sizeof(buf), 0,
							 (struct sockaddr *)&from, &from_len);

		if (n < 0) {
			int e = sock_errno();
			if (e == SOCK_EAGAIN || e == SOCK_EWOULDBLOCK)
				continue;
#ifdef _WIN32
			/* On Windows, SO_RCVTIMEO expiry returns WSAETIMEDOUT (10060),
			 * not WSAEWOULDBLOCK.  Treat it as a normal poll timeout. */
					if (e == WSAETIMEDOUT)
							continue;
					/* WSAECONNRESET (10054) is delivered to UDP sockets on Windows when
					 * the remote host responds with ICMP port-unreachable.  It does NOT
					 * mean our socket is broken — just skip it and keep waiting. */
					if (e == WSAECONNRESET)
							continue;
			#endif
			if (ch->running == SWITCH_TRUE)
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					"mod_ivcore: recvfrom error %d: %s\n", e, sock_strerror(e));
			break;
		}

		if (n < 2) continue;

		if (buf[0] & 0x80) {
			/* --- Protocol frame --- */
			ivp_proto_header_t hdr;
			switch_bool_t consumes;
			if (n < IVP_PROTO_HEADER_SIZE) continue;

			ivp_read_proto_header(buf, &hdr);

			/* Per-frame trace: gated behind ivc debug on */
			IVC_LOG_DEBUG("mod_ivcore: recv proto frame type=%d sub=%d srcCallNo=0x%04X "
				"dstCallNo=0x%04X outSeq=%u inSeq=%u ts=%u bytes=%d resent=%d\n",
				(int)hdr.frame_type, (int)hdr.subclass,
				(unsigned)hdr.src_call_number, (unsigned)hdr.dst_call_number,
				(unsigned)hdr.out_sequence, (unsigned)hdr.in_sequence,
				(unsigned)hdr.timestamp, (int)n, (int)hdr.is_resent);

			consumes = proto_consumes_oseq(hdr.frame_type, hdr.subclass);

			/* Re-ACK retransmits but do not advance our in_sequence and
			 * do not re-process the payload. */
			if (consumes && have_in_seq &&
				(uint8_t)hdr.out_sequence != ch->in_sequence) {
				if ((uint8_t)(hdr.out_sequence + 1) == ch->in_sequence) {
					/* Already-seen frame: re-ACK so the card stops resending. */
					ivp_send_ack(ch, hdr.src_call_number,
								 hdr.in_sequence, hdr.out_sequence,
								 hdr.timestamp);
				}
				continue;
			}

			if (consumes) {
				ch->in_sequence = (uint8_t)(hdr.out_sequence + 1);
				have_in_seq = SWITCH_TRUE;
				/* ACK every seq-consuming frame: NEW, ACCEPT, REJECT,
				 * HANGUP, Control (Ringing/Answer/Busy), Data (HDLC). */
				ivp_send_ack(ch, hdr.src_call_number,
							 hdr.in_sequence, hdr.out_sequence,
							 hdr.timestamp);
			}

			if (hdr.frame_type == IVP_FRAME_PROTOCOL) {
				switch ((ivp_proto_subclass_t)hdr.subclass) {

				case IVP_PROTO_ACCEPT: {
					int accept_payload_len = (int)n - IVP_PROTO_HEADER_SIZE;
					ch->remote_call_number = hdr.src_call_number;
					ch->call_state = IVC_STATE_UP;
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						"mod_ivcore: ACCEPT received, remoteCallNo=0x%04X payloadLen=%d\n",
						(unsigned)hdr.src_call_number, accept_payload_len);
					/* Parse IEs from ACCEPT to honour the matrix's negotiated
					 * provisioning (frameSize / frameTime / framesPerPacket).
					 * Mirrors ApplyNegotiatedProvisioning() in IvpUdpTransport.cs. */
					if (accept_payload_len > 2) {
						const uint8_t *ie = buf + IVP_PROTO_HEADER_SIZE;
						const uint8_t *ie_end = ie + accept_payload_len;
						while (ie + 2 <= ie_end) {
							uint8_t ie_key = ie[0];
							uint8_t ie_len = ie[1];
							ie += 2;
							if (ie + ie_len > ie_end) break;
							if (ie_key == (uint8_t)IVP_IE_PROVISIONING && ie_len >= 6) {
								uint16_t fs  = (uint16_t)(ie[0] | (ie[1] << 8)); /* LE */
								uint16_t ft  = (uint16_t)(ie[2] | (ie[3] << 8));
								uint16_t fpp = (uint16_t)(ie[4] | (ie[5] << 8));
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
									"mod_ivcore: ACCEPT provisioning: frameSize=%u frameTime=%u fpp=%u "
									"(was frameSize=%u frameTime=%u)\n",
									(unsigned)fs, (unsigned)ft, (unsigned)fpp,
									(unsigned)ch->params.frame_size,
									(unsigned)ch->params.frame_time);
								if (fs > 0)  ch->params.frame_size        = fs;
								if (ft > 0)  ch->params.frame_time        = ft;
								if (fpp > 0) ch->params.frames_per_packet = fpp;
								ch->ptime_ms = (uint32_t)(ch->params.frame_time
													* ch->params.frames_per_packet);
								if (ch->ptime_ms == 0) ch->ptime_ms = 20;
							}
							ie += ie_len;
						}
					}
					break;
				}

				case IVP_PROTO_HANGUP:
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						"mod_ivcore: HANGUP received from IVC card\n");
					ch->call_state = IVC_STATE_HANGUP;
					ch->running    = SWITCH_FALSE;
					{
						switch_core_session_t *s = switch_core_session_locate(ch->session_uuid);
						if (s) {
							switch_channel_hangup(switch_core_session_get_channel(s),
								SWITCH_CAUSE_NORMAL_CLEARING);
							switch_core_session_rwunlock(s);
						}
					}
					break;

				case IVP_PROTO_NEW:
					ch->remote_call_number = hdr.src_call_number;
					break;

				case IVP_PROTO_PING:
					send_proto_frame(ch, IVP_FRAME_PROTOCOL, IVP_PROTO_PONG, NULL, 0);
					break;

				case IVP_PROTO_PONG:
				case IVP_PROTO_ACK:
					break;

				case IVP_PROTO_REJECT:
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						"mod_ivcore: REJECT received from IVC card\n");
					ch->call_state = IVC_STATE_HANGUP;
					ch->running    = SWITCH_FALSE;
					{
						switch_core_session_t *s = switch_core_session_locate(ch->session_uuid);
						if (s) {
							switch_channel_hangup(switch_core_session_get_channel(s),
								SWITCH_CAUSE_CALL_REJECTED);
							switch_core_session_rwunlock(s);
						}
					}
					break;

				default:
					break;
				}
			} else if (hdr.frame_type == IVP_FRAME_DATA && hdr.subclass == 0x01) {
				/* IVP Data frame carries an HDLC-framed DPI payload.
				 * The card sends U-SABME first; if we don't reply with
				 * U-UA (and keep the link alive with S-frame RR every
				 * ~3 s), it tears the IVP call down after ~4 s. */
				int hdlc_len = (int)n - IVP_PROTO_HEADER_SIZE;
				if (hdlc_len > 0) {
					/* Store send_cb so mod_ivcore.c can send DPI messages
					 * proactively (e.g. 0xF1 ConnectReply on SIP answer,
					 * 0x93 KeyStatusUpdate on hangup).
					 * Guard the write with the global mutex so that readers
					 * in channel_on_hangup / channel_receive_message (which
					 * run on a different thread) never observe a torn pointer. */
					switch_mutex_lock(ivcore_globals.mutex);
					ch->dpi_send_cb = ivp_send_data_frame;
					switch_mutex_unlock(ivcore_globals.mutex);
					ivp_hdlc_on_data(ch, buf + IVP_PROTO_HEADER_SIZE,
								 hdlc_len, ivp_send_data_frame);
				}
			} else if (hdr.frame_type == IVP_FRAME_CONTROL) {
				int ctrl_len = (int)n - IVP_PROTO_HEADER_SIZE;
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
					"mod_ivcore: Control frame sub=%u payloadLen=%d\n",
					(unsigned)hdr.subclass, ctrl_len);
				if (ctrl_len > 0)
					ivp_hexdump("CTRL-RX", buf + IVP_PROTO_HEADER_SIZE, ctrl_len);
			}

		} else {
			/* --- Media frame --- */
			ivp_media_header_t mhdr;
			int hdr_len;
			int payload_len;

			if (n < IVP_MEDIA_HEADER_SIZE) continue;

			hdr_len = ivp_read_media_header(buf, &mhdr);
			/* media_length on the wire includes 12 bytes of meta overhead
			 * (matching what ivp_send_media adds on TX).  Subtract it to
			 * get the actual audio payload length. */
			payload_len = (int)mhdr.media_length - 12;
			if (payload_len < 0) payload_len = 0;

			if (hdr_len + payload_len > (int)n)
				payload_len = (int)n - hdr_len;
			if (payload_len <= 0) continue;

			handle_media_payload(ch, buf + hdr_len, payload_len);
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: recv loop exited for channel %p\n", (void *)ch);
	return NULL;
}

/* =====================================================================
 * ivp_tx_loop — dedicated TX pacer thread
 *
 * Owns all outbound media pacing and sending, completely decoupled from
 * the FreeSWITCH session thread.  channel_write_frame() only pushes
 * encoded bytes into tx_ring; this thread drains them at exactly the
 * right wire cadence using a QPC hybrid-sleep pacer.
 *
 * When tx_ring is empty (FS has not delivered a frame yet), a silence
 * frame is sent to keep the matrix media watchdog alive.
 * ===================================================================*/
void *ivp_tx_loop(switch_thread_t *thread, void *obj)
{
	ivcore_channel_t *ch = (ivcore_channel_t *)obj;
	int frame_bytes;
	uint32_t ptime_ms;

	/* Jitter diagnostics (only accumulated when ivcore_globals.debug == TRUE).
	 * We track how late/early each timer tick fires vs. the ideal deadline.
	 * Positive jitter_us = late, negative = early. */
	switch_time_t dbg_deadline_us  = 0;     /* expected fire time of next tick    */
	int64_t       dbg_jitter_min   = 0;
	int64_t       dbg_jitter_max   = 0;
	int64_t       dbg_jitter_sum   = 0;
	uint32_t      dbg_frames       = 0;
	switch_time_t dbg_last_log_us  = 0;
	uint32_t      dbg_silence_run  = 0;    /* consecutive silence-substitution frames */

#ifdef _WIN32
	/* On Windows each TX thread gets its own private high-resolution waitable
	 * timer.  This avoids the shared condvar thundering-herd that occurs when
	 * mod_timer_winmm broadcasts to ALL waiting TX threads simultaneously:
	 * with N channels they all unblock at once, compete for the CPU, and the
	 * last one to run is delayed by (N-1) × dispatch latency — which empties
	 * the TX ring and causes silence substitution / audible stutter.
	 *
	 * A private timer per channel means each thread wakes independently at its
	 * own absolute deadline.  A per-process sequence number staggers the
	 * initial phase so channels started at the same time do NOT converge on the
	 * same fire instant even when they all use the same ptime.  The FS condvar
	 * path is still used on non-Windows platforms. */
	HANDLE  win_timer  = NULL;
	int     timer_ok   = 0;
	LARGE_INTEGER due;
	/* Global sequence counter — each TX thread claims one slot so the initial
	 * fire time is spread evenly across one ptime window. */
	static volatile LONG s_tx_seq = 0;

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	{
		HMODULE avrt = GetModuleHandleW(L"avrt.dll");
		if (!avrt) avrt = LoadLibraryW(L"avrt.dll");
		if (avrt) {
			typedef HANDLE (WINAPI *PFN_AvSet)(LPCWSTR, LPDWORD);
			PFN_AvSet fn = (PFN_AvSet)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
			if (fn) {
				DWORD task_idx = 0;
				fn(L"Pro Audio", &task_idx);
			}
		}
	}
#else
	switch_timer_t timer = { 0 };
	switch_memory_pool_t *pool = NULL;
	int timer_ok = 0;
#endif

	/* Wait until codec setup has run so frame_size and ptime are valid. */
	while (ch->running && ch->params.frame_size == 0)
		switch_sleep(1000);

	frame_bytes = (ch->params.frame_size > 0) ? (int)ch->params.frame_size : 320;
	if (frame_bytes > IVC_MAX_FRAME_BYTES) frame_bytes = IVC_MAX_FRAME_BYTES;
	ptime_ms = ch->ptime_ms ? ch->ptime_ms : 20;

	/* Pre-fill the silence buffer. */
	{
		uint8_t fill = (ch->active_codec == IVP_CODEC_G711U) ? 0xFF : 0x00;
		memset(ch->tx_silence_buf, fill, (size_t)frame_bytes);
	}

#ifdef _WIN32
	/* Create a private synchronisation-timer (non-manual-reset, so it
	 * auto-resets after WaitForSingleObject returns — no spurious wakeups). */
	win_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
	if (win_timer) {
		/* Stagger the initial phase: claim a slot in [0, ptime_ms) so that
		 * channels started together fire at different points in the ptime
		 * window.  With 10 channels at 20 ms ptime the slots are 2 ms apart.
		 * The modulus wraps every ptime_ms channels, which is fine — even with
		 * many channels a 2ms stagger window is enough to avoid contention. */
		LONG my_seq = InterlockedIncrement(&s_tx_seq) - 1;
		LONGLONG stagger_100ns = (LONGLONG)(my_seq % (LONG)ptime_ms) * 1000LL * 10LL; /* ms→100ns */
		LONGLONG first_100ns   = (LONGLONG)ptime_ms * 10000LL + stagger_100ns;
		due.QuadPart = -first_100ns;   /* negative = relative */
		if (SetWaitableTimer(win_timer, &due, 0, NULL, NULL, FALSE)) {
			timer_ok = 1;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
				"mod_ivcore: TX ch=%p using private waitable timer, "
				"ptime=%u ms frame=%d bytes phase_slot=%ld (+%lld ms)\n",
				(void *)ch, ptime_ms, frame_bytes,
				(long)(my_seq % (LONG)ptime_ms),
				(long long)(stagger_100ns / 10000LL));
		} else {
			CloseHandle(win_timer);
			win_timer = NULL;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: TX ch=%p SetWaitableTimer failed (%lu), "
				"falling back to switch_sleep\n",
				(void *)ch, GetLastError());
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_ivcore: TX ch=%p CreateWaitableTimerExW failed (%lu), "
			"falling back to switch_sleep\n",
			(void *)ch, GetLastError());
	}
#else
	/* Non-Windows: use the FS timer subsystem (posix timer / softtimer). */
	switch_core_new_memory_pool(&pool);
	{
		uint32_t samples = ptime_ms * 8;
		const char *requested = "soft";

		if (switch_core_timer_init(&timer, requested,
								   (int)ptime_ms, (int)samples, pool)
				== SWITCH_STATUS_SUCCESS) {
			timer_ok = 1;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
				"mod_ivcore: TX thread using '%s' timer, ptime=%u ms frame=%d bytes\n",
				requested, ptime_ms, frame_bytes);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: '%s' timer init failed, falling back to switch_sleep. "
				"ptime=%u ms frame=%d bytes\n",
				requested, ptime_ms, frame_bytes);
		}
	}
#endif

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: TX thread started, frame=%d bytes ptime=%u ms\n",
		frame_bytes, ptime_ms);

	/* Initialise jitter baseline.  The deadline is anchored on the first
	 * timer fire (see below), not on entry to the loop, so the startup
	 * cost of timer init / first wait is not counted as jitter.
	 * dbg_deadline_us == 0 means "anchor on next tick". */
	dbg_last_log_us = switch_micro_time_now();
	dbg_deadline_us = 0;

	while (ch->running) {
		uint32_t avail;
		const uint8_t *send_buf;
		switch_time_t now_us;

		/* --- Pace to the negotiated ptime --- */
#ifdef _WIN32
		if (timer_ok) {
			/* Block until the private per-channel waitable timer fires. */
			WaitForSingleObject(win_timer, (DWORD)(ptime_ms * 3));

			/* Re-arm for the next absolute deadline.  We use the ideal
			 * deadline (dbg_deadline_us) when the baseline has been set so
			 * that any late wakeup is automatically corrected on the next
			 * tick without accumulating drift.  Before the first tick the
			 * deadline is not yet set, so use a relative period instead. */
			if (dbg_deadline_us != 0) {
				/* Convert FS microsecond absolute time to FILETIME 100-ns units.
				 * switch_micro_time_now() is QPC-based; FILETIME epoch is
				 * 1601-01-01.  We approximate the next absolute fire time as
				 * (deadline_us + ptime_us) expressed as a relative offset from
				 * now so we do not need to know the epoch offset. */
				LONGLONG next_rel_100ns =
					-(LONGLONG)((switch_time_t)ptime_ms * 1000 -
								(int64_t)(switch_micro_time_now() - dbg_deadline_us)) * 10LL;
				/* Clamp: if we are already past the deadline, fire immediately. */
				if (next_rel_100ns > -1000LL) next_rel_100ns = -1000LL;
				due.QuadPart = next_rel_100ns;
			} else {
				due.QuadPart = -(LONGLONG)ptime_ms * 10000LL;
			}
			SetWaitableTimer(win_timer, &due, 0, NULL, NULL, FALSE);
		} else {
			switch_sleep(ptime_ms * 1000);
		}
#else
		if (timer_ok) {
			if (switch_core_timer_next(&timer) != SWITCH_STATUS_SUCCESS) {
				timer_ok = 0;
			}
		}
		if (!timer_ok) {
			switch_sleep(ptime_ms * 1000);
		}
#endif

		now_us = switch_micro_time_now();

		/* --- Jitter accounting (under debug flag to avoid cache-line churn) --- */
		if (ivcore_globals.debug == SWITCH_TRUE) {
			int64_t jitter;

			/* Anchor the deadline on the *first* tick we observe so that
			 * timer-init / thread-startup latency is not counted as jitter. */
			if (dbg_deadline_us == 0) {
				dbg_deadline_us = now_us;
			}

			jitter = (int64_t)(now_us - dbg_deadline_us);
			if (dbg_frames == 0) {
				dbg_jitter_min = jitter;
				dbg_jitter_max = jitter;
			} else {
				if (jitter < dbg_jitter_min) dbg_jitter_min = jitter;
				if (jitter > dbg_jitter_max) dbg_jitter_max = jitter;
			}
			dbg_jitter_sum += jitter;
			dbg_frames++;

			/* Log once per second. */
			if (now_us - dbg_last_log_us >= 1000000) {
				int64_t avg = dbg_frames ? dbg_jitter_sum / (int64_t)dbg_frames : 0;
				IVC_LOG_DEBUG(
					"[IVC-TX] ch=%p timer jitter over %u frames: "
					"min=%+" PRId64 " avg=%+" PRId64 " max=%+" PRId64 " us"
					" silence=%u\n",
					(void *)ch, dbg_frames,
					dbg_jitter_min, avg, dbg_jitter_max,
					dbg_silence_run);
				dbg_jitter_min = 0;
				dbg_jitter_max = 0;
				dbg_jitter_sum = 0;
				dbg_frames      = 0;
				dbg_silence_run = 0;
				dbg_last_log_us = now_us;
			}
			/* Advance ideal deadline by one ptime interval.  This is the
			 * correct, drift-free formula: real lateness accumulates into
			 * the jitter readout instead of being silently absorbed. */
			dbg_deadline_us += (switch_time_t)ptime_ms * 1000;

			/* Last-resort resync: only trip on truly pathological gaps
			 * (debugger break, host sleep/resume, watchdog stall).  In
			 * normal operation -- including 50-100 ms of drift caused by
			 * Windows timer-resolution coalescing -- we do NOT snap, so
			 * the jitter readout keeps showing the true drift instead of
			 * papering over it.  When we do snap, log a WARNING (not
			 * DEBUG) so the event is visible even with debug disabled. */
			{
				const switch_time_t resync_threshold_us = 250000; /* 250 ms */
				int64_t lateness_us = (int64_t)(now_us - dbg_deadline_us);
				if (lateness_us > (int64_t)resync_threshold_us) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						"[IVC-TX] ch=%p timer drift %" PRId64 " us exceeds "
						"%" PRId64 " us resync threshold; rebasing deadline\n",
						(void *)ch, lateness_us,
						(int64_t)resync_threshold_us);
					dbg_deadline_us = now_us;
				}
			}
		}

		if (!ch->running) break;

		/* Drain one frame from tx_ring, or use silence. */
			avail = ring_available(&ch->tx_ring);
			if (avail >= (uint32_t)frame_bytes) {
				/* Read directly from ring into send buffer.
				 * ring_read handles wrap-around. */
				uint8_t frame_buf[IVC_MAX_FRAME_BYTES];
				ring_read(&ch->tx_ring, frame_buf, (uint32_t)frame_bytes);
				send_buf = frame_buf;
				dbg_silence_run = 0; /* reset consecutive-silence counter */
			} else {
				/* Ring empty or partial — send silence.
				 * Log under debug to diagnose stutter: consecutive silence frames
				 * are the audible glitch, not HDLC or jitter. */
				send_buf = ch->tx_silence_buf;
				dbg_silence_run++;
				if (ivcore_globals.debug == SWITCH_TRUE && dbg_silence_run == 1) {
					IVC_LOG_DEBUG("[IVC-TX] ch=%p ring underrun: silence substituted "
						"(avail=%u need=%d jitter=%" PRId64 " us)\n",
						(void *)ch, avail, frame_bytes,
						(dbg_deadline_us ? (int64_t)(now_us - (dbg_deadline_us - (switch_time_t)ptime_ms * 1000)) : 0));
				}
			}

		if (ch->call_state == IVC_STATE_UP) {
			ch->last_write_us = switch_micro_time_now();
			ivp_send_media(ch, send_buf, frame_bytes);
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: TX thread exited for channel %p\n", (void *)ch);

#ifdef _WIN32
	if (win_timer) {
		CancelWaitableTimer(win_timer);
		CloseHandle(win_timer);
		win_timer = NULL;
	}
#else
	if (timer_ok) {
		switch_core_timer_destroy(&timer);
	}
	if (pool) {
		switch_core_destroy_memory_pool(&pool);
	}
#endif
	return NULL;
}
