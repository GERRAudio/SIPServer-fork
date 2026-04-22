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
		DWORD tv_ms = 50;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
	}
#else
	{
		struct timeval tv;
		tv.tv_sec = 0; tv.tv_usec = 50000;
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
	mhdr.media_length          = (uint16_t)payload_len;
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
	 * frame every ptime_ms.  Bytes per packet for G.722 is
	 * frame_size * frames_per_packet (= 8 * 8 = 64). */
	switch_time_t last_media_sent = 0;
	switch_bool_t have_in_seq = SWITCH_FALSE;

	(void)thread;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: recv loop started for channel %p\n", (void *)ch);

	while (ch->running == SWITCH_TRUE) {
		ssize_t n;

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
						if (ch->session)
							switch_channel_hangup(
								switch_core_session_get_channel(ch->session),
								SWITCH_CAUSE_NETWORK_OUT_OF_ORDER);
						break;
					}
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
						"mod_ivcore: retransmitting NEW (attempt %d/%d)\n",
						new_retries + 1, IVP_NEW_MAX_RETRIES);
					ivp_send_new(ch);
					new_retries++;
			}
		}

		/* Silence media keep-alive while the call is UP.  Without this
		 * the IVR's media-activity watchdog tears the call down a few
		 * seconds after ACCEPT.  Skip the keep-alive entirely for the
		 * current interval when channel_write_frame has already sent a
		 * real audio packet — sending both would double the media rate
		 * causing audio to sound slow and repeated on the matrix. */
		if (ch->call_state == IVC_STATE_UP) {
			switch_time_t now = switch_micro_time_now();
			uint32_t pkt_us = (ch->ptime_ms ? ch->ptime_ms : 20) * 1000;
			if (last_media_sent == 0 || now - last_media_sent >= pkt_us) {
				/* Only send silence if write_frame hasn't sent real audio
				 * in the last packet interval (with a 2× window for jitter). */
				switch_bool_t real_audio_active =
					(ch->last_write_us != 0 &&
					 now - ch->last_write_us < (switch_time_t)pkt_us * 2);
				if (!real_audio_active) {
					uint8_t silence[256];
					int bytes;
					if (ch->active_codec == IVP_CODEC_G722 ||
						ch->active_codec == IVP_CODEC_G711U ||
						ch->active_codec == IVP_CODEC_G711A) {
						bytes = (int)ch->params.frame_size *
								(int)ch->params.frames_per_packet;
					} else {
						bytes = (int)((ch->ptime_ms ? ch->ptime_ms : 20) * 8);
					}
					if (bytes <= 0) bytes = 64;
					if (bytes > (int)sizeof(silence)) bytes = (int)sizeof(silence);
					memset(silence, 0, (size_t)bytes);
					{
						switch_status_t mst = ivp_send_media(ch, silence, bytes);
						if (last_media_sent == 0) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
								"mod_ivcore: starting silence keep-alive (%d bytes / %u ms)\n",
								bytes, (unsigned)(ch->ptime_ms ? ch->ptime_ms : 20));
						} else if ((ch->media_sequence_out & 0x1F) == 0) {
							IVC_LOG_DEBUG("mod_ivcore: TX silence mediaSeq=%u status=%d\n",
								(unsigned)ch->media_sequence_out, (int)mst);
						}
					}
					last_media_sent = now;
				}
			}
		}

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

				case IVP_PROTO_ACCEPT:
					ch->remote_call_number = hdr.src_call_number;
					ch->call_state = IVC_STATE_UP;
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						"mod_ivcore: ACCEPT received, remoteCallNo=0x%04X\n",
						(unsigned)hdr.src_call_number);
					break;

				case IVP_PROTO_HANGUP:
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						"mod_ivcore: HANGUP received from IVC card\n");
					ch->call_state = IVC_STATE_HANGUP;
					ch->running    = SWITCH_FALSE;
					if (ch->session) {
						switch_channel_hangup(
							switch_core_session_get_channel(ch->session),
							SWITCH_CAUSE_NORMAL_CLEARING);
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
					if (ch->session) {
						switch_channel_hangup(
							switch_core_session_get_channel(ch->session),
							SWITCH_CAUSE_CALL_REJECTED);
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
					 * 0x93 KeyStatusUpdate on hangup). */
					ch->dpi_send_cb = ivp_send_data_frame;
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
			payload_len = (int)mhdr.media_length;

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
