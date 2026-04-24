/*
 * ravenna_net.c — UDP / multicast helpers, Linux + Windows.
 *
 * IPv4 ONLY.  AES67 (AES67-2018) and the RAVENNA transport specification
 * define the session layer exclusively over IPv4 multicast (239.x.x.x,
 * RFC 2365 administratively-scoped space).  IPv6 multicast is not used
 * by any shipping AES67 or RAVENNA device and is therefore a deliberate
 * non-goal for this module.  All sockets use AF_INET / struct sockaddr_in;
 * iface-primary and iface-secondary accept an IPv4 address (all platforms)
 * or a Linux interface name (Linux only).
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
  /* GetAdaptersAddresses is in iphlpapi. */
  #include <iphlpapi.h>
  #pragma comment(lib, "iphlpapi.lib")
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

/* Resolve interface IP from the iface-primary / iface-secondary config value.
 *
 * Linux  : interface name  (e.g. "ens19", "eth0")  — preferred; resolved via
 *          SIOCGIFADDR.  A dotted IPv4 address is also accepted as a fallback.
 * Windows: interface name (e.g. "Ethernet 2", "Local Area Connection") OR
 *          dotted IPv4 address (e.g. "192.168.10.5") — both accepted.
 *          Name lookup uses GetAdaptersAddresses(); dotted IP tried first.
 * Empty  : INADDR_ANY (OS picks the interface — usually wrong for multicast).
 *
 * On Linux, prefer the ifname in config files so the mapping is explicit even
 * when multiple IPs share the same adapter.
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
		/* Fast path: dotted IPv4 address */
		if (inet_pton(AF_INET, iface, &a) == 1) return a.s_addr;

		/* Slow path: adapter friendly-name or adapter description lookup.
		 * We walk GetAdaptersAddresses() looking for an IPv4 unicast address
		 * on an adapter whose FriendlyName or Description matches `iface`.
		 * The comparison is case-insensitive. */
		{
			IP_ADAPTER_ADDRESSES *list = NULL, *cur;
			ULONG buflen = 16 * 1024;
			ULONG ret;
			in_addr_t result = htonl(INADDR_ANY);

			/* GetAdaptersAddresses may need more space on systems with many
			 * adapters; retry once with the suggested size. */
			list = (IP_ADAPTER_ADDRESSES *)malloc(buflen);
			if (!list) return htonl(INADDR_ANY);

			ret = GetAdaptersAddresses(AF_INET,
				GAA_FLAG_SKIP_ANYCAST |
				GAA_FLAG_SKIP_MULTICAST |
				GAA_FLAG_SKIP_DNS_SERVER,
				NULL, list, &buflen);

			if (ret == ERROR_BUFFER_OVERFLOW) {
				free(list);
				list = (IP_ADAPTER_ADDRESSES *)malloc(buflen);
				if (!list) return htonl(INADDR_ANY);
				ret = GetAdaptersAddresses(AF_INET,
					GAA_FLAG_SKIP_ANYCAST |
					GAA_FLAG_SKIP_MULTICAST |
					GAA_FLAG_SKIP_DNS_SERVER,
					NULL, list, &buflen);
			}

			if (ret == NO_ERROR) {
				for (cur = list; cur; cur = cur->Next) {
					/* Convert wide FriendlyName / AdapterName to narrow for
					 * comparison.  We use WideCharToMultiByte rather than
					 * wcstombs so it works regardless of locale. */
					char fname[256] = {0};
					char aname[256] = {0};
					WideCharToMultiByte(CP_UTF8, 0,
						cur->FriendlyName, -1, fname, sizeof(fname) - 1, NULL, NULL);
					WideCharToMultiByte(CP_UTF8, 0,
						cur->Description,  -1, aname, sizeof(aname) - 1, NULL, NULL);

					if (_stricmp(iface, fname) == 0 || _stricmp(iface, aname) == 0) {
						IP_ADAPTER_UNICAST_ADDRESS *ua = cur->FirstUnicastAddress;
						if (ua) {
							result = ((struct sockaddr_in *)
								ua->Address.lpSockaddr)->sin_addr.s_addr;
						}
						break;
					}
				}
			}

			free(list);
			return result;
		}
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
