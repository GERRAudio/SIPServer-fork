/*
 * ravenna_net.h — socket helpers (multicast join, send/recv batching).
 */

#ifndef RAVENNA_NET_H
#define RAVENNA_NET_H

#include "mod_ravenna.h"

/* Bind a UDP socket on (bind_addr, port) and join the multicast group
 * (mcast_addr) on the given interface. iface may be empty to use the
 * system default. Returns SWITCH_STATUS_SUCCESS or _GENERR. */
switch_status_t ravenna_net_open_rx(ravenna_socket_t *sock_out,
									const char *mcast_addr, int port,
									const char *iface);

/* Open a TX UDP socket bound to the given interface (for routing) and
 * fill *dest with mcast_addr:port. */
switch_status_t ravenna_net_open_tx(ravenna_socket_t *sock_out,
									struct sockaddr_in *dest,
									const char *mcast_addr, int port,
									const char *iface, int ttl);

void            ravenna_net_close(ravenna_socket_t *sock);

/* Drain a receive socket into batched (buf, len, addr) tuples until
 * EWOULDBLOCK or `max` packets received. Returns number of packets. */
typedef struct {
	uint8_t  *buf;
	int       cap;
	int       len;
} ravenna_pkt_t;

int  ravenna_net_recv_batch(ravenna_socket_t s,
							ravenna_pkt_t *pkts, int max);

int  ravenna_net_send(ravenna_socket_t s, const struct sockaddr_in *dest,
					  const void *buf, int len);

/* Self-pipe used to wake the RX reactor when streams are added/removed. */
switch_status_t ravenna_net_open_wake(ravenna_socket_t *r, ravenna_socket_t *w);
void            ravenna_net_wake(ravenna_socket_t w);
void            ravenna_net_drain_wake(ravenna_socket_t r);

#endif /* RAVENNA_NET_H */
