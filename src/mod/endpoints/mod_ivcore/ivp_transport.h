/**
 * ivp_transport.h
 *
 * TCP login handshake and IVP UDP media transport for mod_ivcore.
 *
 * Mirrors TcpLoginClient.cs and IvpUdpTransport.cs from the IVCore
 * C# stack:
 *   - ivp_tcp_login()   → TcpLoginClient.LoginAsync()
 *   - ivp_udp_open()    → IvpUdpTransport.Open()
 *   - ivp_send_new()    → IvpUdpTransport.SendNew()
 *   - ivp_send_media()  → IvpUdpTransport.SendMediaFrame()
 *   - ivp_send_hangup() → IvpUdpTransport.SendHangup()
 *   - ivp_recv_loop()   → IvpUdpTransport.ReceiveLoop()  (runs in its own thread)
 */

#ifndef IVP_TRANSPORT_H
#define IVP_TRANSPORT_H

#include "mod_ivcore.h"

/**
 * Perform the TCP login handshake with the IVC card.
 *
 * Connects to params->server_ip:params->tcp_port, sends a
 * PanelLoginAttempt message, and waits for PanelLoginResponse.
 * On success, fills ch->remote_addr with the UDP server address
 * returned by the IVC card and returns SWITCH_STATUS_SUCCESS.
 *
 * Matches TcpLoginClient.LoginAsync() in TcpLoginClient.cs.
 */
switch_status_t ivp_tcp_login(ivcore_channel_t *ch);

/**
 * Open the UDP socket and bind a local port.
 * Must be called after ivp_tcp_login() so remote_addr is known.
 *
 * Matches IvpUdpTransport.Open() in IvpUdpTransport.cs.
 */
switch_status_t ivp_udp_open(ivcore_channel_t *ch);

/**
 * Send an IVP NEW protocol frame to initiate a call.
 * Appends all IEs from ch->params (Username, CallingName, Format,
 * Capability, SamplingRate, Provisioning, EncryptionKey, etc.).
 *
 * Matches IvpUdpTransport.SendNew() in IvpUdpTransport.cs.
 */
switch_status_t ivp_send_new(ivcore_channel_t *ch);

/**
 * Send an IVP ACK for a received frame.
 */
switch_status_t ivp_send_ack(ivcore_channel_t *ch,
							  uint16_t dst_call_number,
							  uint8_t  in_seq,
							  uint8_t  out_seq);

/**
 * Send an IVP HANGUP protocol frame.
 * Matches IvpUdpTransport.SendHangup() in IvpUdpTransport.cs.
 */
switch_status_t ivp_send_hangup(ivcore_channel_t *ch);

/**
 * Send a media (audio) frame over UDP.
 * payload should contain codec-encoded bytes (G.711, G.722, or PCM).
 *
 * Matches IvpUdpTransport.SendMediaFrame() / SendRawPayload() in
 * IvpUdpTransport.cs.
 */
switch_status_t ivp_send_media(ivcore_channel_t *ch,
								const uint8_t *payload, int payload_len);

/**
 * Close the UDP (and any lingering TCP) sockets for this channel.
 * Matches IvpUdpTransport.Dispose() in IvpUdpTransport.cs.
 */
void ivp_transport_close(ivcore_channel_t *ch);

/**
 * UDP receive loop — runs in its own switch_thread.
 * Reads incoming IVP frames, decodes audio into ch->rx_ring, and
 * updates call state.  Thread exits when ch->running is SWITCH_FALSE.
 *
 * Matches IvpUdpTransport.ReceiveLoop() in IvpUdpTransport.cs.
 */
void *ivp_recv_loop(switch_thread_t *thread, void *obj);

/* -----------------------------------------------------------------------
 * Wire serialisation helpers (used internally and by mod_ivcore.c)
 * --------------------------------------------------------------------- */

/**
 * Write a 14-byte IVP protocol frame header into buf (big-endian).
 * Matches IvpProtocolFrameHeader.WriteTo() in IvpFrames.cs.
 */
void ivp_write_proto_header(uint8_t *buf, const ivp_proto_header_t *hdr);

/**
 * Parse a 14-byte IVP protocol frame header from buf (big-endian).
 * Matches IvpProtocolFrameHeader.ReadFrom() in IvpFrames.cs.
 */
void ivp_read_proto_header(const uint8_t *buf, ivp_proto_header_t *hdr);

/**
 * Write an IVP media frame header into buf (big-endian).
 * Returns number of bytes written.
 */
int ivp_write_media_header(uint8_t *buf, const ivp_media_header_t *hdr);

/**
 * Parse an IVP media frame header from buf.
 * Returns number of bytes consumed.
 */
int ivp_read_media_header(const uint8_t *buf, ivp_media_header_t *hdr);

/**
 * Append an IE (Information Element) of type STRING to buf.
 * Matches IvpIeHelper.AppendString() in IvpUdpTransport.cs.
 */
int ivp_ie_append_string(uint8_t *buf, ivp_ie_key_t key, const char *value);

/**
 * Append an IE of type DWORD (4 bytes, big-endian) to buf.
 * Matches IvpIeHelper.AppendDword().
 */
int ivp_ie_append_dword(uint8_t *buf, ivp_ie_key_t key, uint32_t value);

/**
 * Append an IE of type WORD (2 bytes, big-endian) to buf.
 */
int ivp_ie_append_word(uint8_t *buf, ivp_ie_key_t key, uint16_t value);

/**
 * Append an IE of type BYTE (1 byte) to buf.
 */
int ivp_ie_append_byte(uint8_t *buf, ivp_ie_key_t key, uint8_t value);

/**
 * Append the Provisioning IE (frameSize, frameTime, framesPerPacket).
 * Matches IvpIeHelper.AppendProvisioning() in IvpUdpTransport.cs.
 */
int ivp_ie_append_provisioning(uint8_t *buf,
								uint16_t frame_size,
								uint16_t frame_time,
								uint16_t frames_per_packet);

#endif /* IVP_TRANSPORT_H */
