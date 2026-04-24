/*
 * ravenna_net.c — UDP / multicast helpers, Linux + Windows.
 *
 * No allocations on the hot path; all buffers belong to the caller.
 */

#include "ravenna_net.h"

#include <string.h>

#ifndef _WIN32
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <net/if.h>
  #ifndef SOCK_NONBLOCK
	#define SOCK_NONBLOCK 0
  #endif
#else
  /* in_addr_t is a POSIX typedef; Windows uses ULONG for IPv4 in_addr.s_addr. */
  typedef ULONG in_addr_t;
#endif

/* ------------------------------------------------------------------
 *  Internal helpers
 * ------------------------------------------------------------------ */

static int ravenna_set_nonblock(ravenna_socket_t s)
{
#ifdef _WIN32
	u_long nb = 1;
	return (ioctlsocket(s, FIONBIO, &nb) == 0) ? 0 : -1;
#else
	int fl = fcntl(s, F_GETFL, 0);
	if (fl < 0) return -1;
	return fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

static int ravenna_set_int_opt(ravenna_socket_t s, int level, int name, int val)
{
	return setsockopt(s, level, name, (const char *)&val, sizeof(val));
}

/* Resolve interface IP. iface may be:
 *   - empty       => INADDR_ANY
 *   - dotted IP   => use as-is
 *   - ifname (Linux) => look up via if_nametoindex+SIOCGIFADDR via getifaddrs
 *
 * For simplicity (and to keep this file dependency-free) we accept
 * a dotted IP on Windows and either a dotted IP or an interface name
 * on Linux. Interface names on Windows would require GetAdaptersAddresses;
 * leaving that for a follow-up.
 */
static in_addr_t ravenna_iface_addr(const char *iface)
{
	if (zstr(iface)) return htonl(INADDR_ANY);
#ifndef _WIN32
	{
		struct in_addr a;
		if (inet_pton(AF_INET, iface, &a) == 1) return a.s_addr;
		/* Fall through to ifname lookup */
		{
			struct ifreq ifr;
			int s = socket(AF_INET, SOCK_DGRAM, 0);
			if (s < 0) return htonl(INADDR_ANY);
			memset(&ifr, 0, sizeof(ifr));
			switch_snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface);
			if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
				in_addr_t r = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
				close(s);
				return r;
			}
			close(s);
		}
	}
#else
	{
		struct in_addr a;
		if (inet_pton(AF_INET, iface, &a) == 1) return a.s_addr;
	}
#endif
	return htonl(INADDR_ANY);
}

/* ------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------ */

switch_status_t ravenna_net_open_rx(ravenna_socket_t *sock_out,
									const char *mcast_addr, int port,
									const char *iface)
{
	ravenna_socket_t s;
	struct sockaddr_in addr;
	struct ip_mreq mreq;
	int rcvbuf = 4 * 1024 * 1024;

	*sock_out = RAVENNA_INVALID_SOCKET;

	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == RAVENNA_INVALID_SOCKET) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "ravenna: socket() failed for rx %s:%d\n", mcast_addr, port);
		return SWITCH_STATUS_GENERR;
	}

	ravenna_set_int_opt(s, SOL_SOCKET, SO_REUSEADDR, 1);
#ifdef SO_REUSEPORT
	ravenna_set_int_opt(s, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "ravenna: bind(:%d) failed for rx %s\n", port, mcast_addr);
		ravenna_net_close(&s);
		return SWITCH_STATUS_GENERR;
	}

	memset(&mreq, 0, sizeof(mreq));
	if (inet_pton(AF_INET, mcast_addr, &mreq.imr_multiaddr) != 1) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "ravenna: bad mcast addr '%s'\n", mcast_addr);
		ravenna_net_close(&s);
		return SWITCH_STATUS_GENERR;
	}
	mreq.imr_interface.s_addr = ravenna_iface_addr(iface);

	if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				   (const char *)&mreq, sizeof(mreq)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "ravenna: IP_ADD_MEMBERSHIP %s on iface '%s' failed\n",
						  mcast_addr, iface ? iface : "");
		ravenna_net_close(&s);
		return SWITCH_STATUS_GENERR;
	}

	if (ravenna_set_nonblock(s) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ravenna: failed to set non-blocking on rx socket\n");
	}

	*sock_out = s;
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t ravenna_net_open_tx(ravenna_socket_t *sock_out,
									struct sockaddr_in *dest,
									const char *mcast_addr, int port,
									const char *iface, int ttl)
{
	ravenna_socket_t s;
	struct in_addr ifa;
	int sndbuf = 4 * 1024 * 1024;
	unsigned char ttl_byte = (unsigned char)(ttl > 0 ? ttl : 16);
	unsigned char loop = 0;

	*sock_out = RAVENNA_INVALID_SOCKET;

	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == RAVENNA_INVALID_SOCKET) return SWITCH_STATUS_GENERR;

	setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));

	ifa.s_addr = ravenna_iface_addr(iface);
	if (setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,
				   (const char *)&ifa, sizeof(ifa)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ravenna: IP_MULTICAST_IF on '%s' failed\n", iface ? iface : "");
	}
	setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL,
			   (const char *)&ttl_byte, sizeof(ttl_byte));
	setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP,
			   (const char *)&loop, sizeof(loop));

	if (ravenna_set_nonblock(s) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ravenna: failed to set non-blocking on tx socket\n");
	}

	memset(dest, 0, sizeof(*dest));
	dest->sin_family = AF_INET;
	dest->sin_port   = htons((uint16_t)port);
	if (inet_pton(AF_INET, mcast_addr, &dest->sin_addr) != 1) {
		ravenna_net_close(&s);
		return SWITCH_STATUS_GENERR;
	}

	*sock_out = s;
	return SWITCH_STATUS_SUCCESS;
}

void ravenna_net_close(ravenna_socket_t *sock)
{
	if (!sock || *sock == RAVENNA_INVALID_SOCKET) return;
#ifdef _WIN32
	closesocket(*sock);
#else
	close(*sock);
#endif
	*sock = RAVENNA_INVALID_SOCKET;
}

int ravenna_net_recv_batch(ravenna_socket_t s, ravenna_pkt_t *pkts, int max)
{
	int n = 0;
	for (; n < max; n++) {
		int r;
#ifdef _WIN32
		r = recv(s, (char *)pkts[n].buf, pkts[n].cap, 0);
		if (r < 0) {
			if (WSAGetLastError() == WSAEWOULDBLOCK) break;
			break;
		}
#else
		r = (int)recv(s, pkts[n].buf, pkts[n].cap, 0);
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			break;
		}
#endif
		pkts[n].len = r;
	}
	return n;
}

int ravenna_net_send(ravenna_socket_t s, const struct sockaddr_in *dest,
					 const void *buf, int len)
{
#ifdef _WIN32
	return sendto(s, (const char *)buf, len, 0,
				  (const struct sockaddr *)dest, sizeof(*dest));
#else
	return (int)sendto(s, buf, len, 0,
					   (const struct sockaddr *)dest, sizeof(*dest));
#endif
}

switch_status_t ravenna_net_open_wake(ravenna_socket_t *r, ravenna_socket_t *w)
{
	/* Cross-platform: a connected loopback UDP pair. */
	struct sockaddr_in la;
	socklen_t alen = sizeof(la);
	ravenna_socket_t rs, ws;

	*r = *w = RAVENNA_INVALID_SOCKET;

	rs = socket(AF_INET, SOCK_DGRAM, 0);
	ws = socket(AF_INET, SOCK_DGRAM, 0);
	if (rs == RAVENNA_INVALID_SOCKET || ws == RAVENNA_INVALID_SOCKET) {
		ravenna_net_close(&rs); ravenna_net_close(&ws);
		return SWITCH_STATUS_GENERR;
	}

	memset(&la, 0, sizeof(la));
	la.sin_family = AF_INET;
	la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	la.sin_port = 0;
	if (bind(rs, (struct sockaddr *)&la, sizeof(la)) < 0 ||
		getsockname(rs, (struct sockaddr *)&la, &alen) < 0 ||
		connect(ws, (struct sockaddr *)&la, sizeof(la)) < 0) {
		ravenna_net_close(&rs); ravenna_net_close(&ws);
		return SWITCH_STATUS_GENERR;
	}

	ravenna_set_nonblock(rs);
	ravenna_set_nonblock(ws);

	*r = rs;
	*w = ws;
	return SWITCH_STATUS_SUCCESS;
}

void ravenna_net_wake(ravenna_socket_t w)
{
	const char b = 1;
#ifdef _WIN32
	send(w, &b, 1, 0);
#else
	(void)!send(w, &b, 1, 0);
#endif
}

void ravenna_net_drain_wake(ravenna_socket_t r)
{
	char buf[64];
	for (;;) {
#ifdef _WIN32
		int n = recv(r, buf, sizeof(buf), 0);
		if (n <= 0) break;
#else
		ssize_t n = recv(r, buf, sizeof(buf), 0);
		if (n <= 0) break;
#endif
	}
}
