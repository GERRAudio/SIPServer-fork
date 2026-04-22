/**
 * ivp_hdlc.h
 *
 * Minimal HDLC data-link layer for the IVP Data channel (FrameType=7,
 * Subclass=1) used by mod_ivcore.  Only what is needed to keep the
 * card happy: respond to U-SABME with U-UA, and emit S-frame RR every
 * ~3 seconds as a keep-alive.  DPI payloads are not yet delivered
 * upward — they are only acknowledged so the card's HDLC watchdog
 * (4 s keepalive / 10 s inactive) does not tear the IVP call down.
 *
 * Reference: E:\Development\IVCore\PROTOCOL.md §"HDLC Data Link Layer".
 */

#ifndef IVP_HDLC_H
#define IVP_HDLC_H

#include "mod_ivcore.h"

/* HDLC framing constants */
#define IVP_HDLC_FLAG    0x7E
#define IVP_HDLC_ESCAPE  0x7D
#define IVP_HDLC_XOR     0x20

/* Address byte values */
#define IVP_HDLC_ADDR_CMD   0x01   /* server -> client (command)  */
#define IVP_HDLC_ADDR_RESP  0x03   /* client -> server (response) */

/* Send callback used by ivp_hdlc_on_data() to push a stuffed HDLC
 * frame back to the card wrapped in an IVP type=7 sub=1 protocol
 * frame.  Implemented in ivp_transport.c. */
typedef switch_status_t (*ivp_hdlc_send_data_cb)(ivcore_channel_t *ch,
												  const uint8_t *frame,
												  int frame_len);

/**
 * Reset HDLC state.  Call once when the IVP call enters CONNECTING.
 */
void ivp_hdlc_reset(ivcore_channel_t *ch);

/**
 * Process one inbound IVP Data frame payload (the bytes that follow
 * the 14-byte IVP protocol header).  Detects U-SABME and responds
 * with U-UA via send_cb; updates ch->hdlc.v_r for I-frames and ACKs
 * them with an immediate RR so the card's HDLC window does not stall.
 */
switch_status_t ivp_hdlc_on_data(ivcore_channel_t *ch,
								  const uint8_t *payload, int payload_len,
								  ivp_hdlc_send_data_cb send_cb);

/**
 * Build an S-frame RR (Receive Ready) that acknowledges everything
 * received so far (uses ch->hdlc.v_r) into out_buf.  Returns the
 * number of bytes written (HDLC-stuffed, with leading/trailing
 * 0x7E flags).
 */
int ivp_hdlc_build_rr(const ivcore_channel_t *ch,
					   uint8_t *out_buf, int out_cap);

/**
 * Build a U-UA frame (response to SABME) into out_buf.  Returns the
 * number of bytes written.
 */
int ivp_hdlc_build_ua(uint8_t *out_buf, int out_cap);

/**
 * Build an HDLC I-frame carrying the given DPI payload.  Uses
 * ch->hdlc.v_s for the send sequence (and increments it) and
 * ch->hdlc.v_r for the piggy-backed ack sequence.  Returns total
 * bytes written (HDLC-stuffed, with leading/trailing 0x7E flags),
 * or -1 on overflow.
 */
int ivp_hdlc_build_iframe(ivcore_channel_t *ch,
                           const uint8_t *payload, int payload_len,
                           uint8_t *out_buf, int out_cap);

#endif /* IVP_HDLC_H */
