/**
 * ivp_dpi.h
 *
 * SIP-telephony DPI (Dumb Panel Interface) decoder/encoder for the
 * IVP data channel.  Carried inside HDLC I-frame payloads (see
 * ivp_hdlc.c) on top of IVP type=7 sub=1 protocol frames.
 *
 * Wire format (inbound from matrix):
 *   [CSO header: 6 bytes][DPI message ID: 1 byte][payload...]
 *
 *   CSO header = [MsgType:4 bytes big-endian = 1][RouteDst:1][RouteSrc:1]
 *
 *   This 6-byte CSO envelope must be stripped before parsing the DPI ID.
 *   Outgoing DPI messages must be wrapped with the same envelope.
 *
 * Reference: E:\Development\IVCore\SIP_TELEPHONY_DPI.md
 *            E:\Development\IVCore\Devices\IvcDeviceBase.cs
 */

#ifndef IVP_DPI_H
#define IVP_DPI_H

#include "mod_ivcore.h"
#include "ivp_hdlc.h"

/* CSO envelope size (precedes every DPI message in HDLC I-frames) */
#define IVP_DPI_CSO_HEADER_SIZE  6

/* Message IDs.  Same byte for request and reply; direction implied by sender. */
typedef enum {
	/* Rack → Panel */
	IVP_DPI_PANEL_TYPE_REQUEST    = 0x80,
	IVP_DPI_KEY_STATUS_REQUEST    = 0x8B,
	IVP_DPI_MATRIX_VERSION        = 0xE7,
	/* Panel → Rack */
	IVP_DPI_PANEL_RESET           = 0x92,
	IVP_DPI_KEY_STATUS_UPDATE     = 0x93,
	IVP_DPI_PANEL_TYPE_REPLY      = 0x90,
	IVP_DPI_PANEL_SW_VERSION      = 0x91,
	IVP_DPI_KEY_STATUS_REPLY      = 0x94,
	/* SIP telephony (bidirectional) */
	IVP_DPI_GET_STATE             = 0xF0,
	IVP_DPI_CONNECT_OUTGOING      = 0xF1,
	IVP_DPI_CONNECT_INCOMING      = 0xF2,
	IVP_DPI_DISCONNECT_INCOMING   = 0xF3,
	IVP_DPI_DISCONNECT_OUTGOING   = 0xF4,
	IVP_DPI_DIAL_INFO             = 0xF5,
	IVP_DPI_CLI_INFO              = 0xF6,
} ivp_dpi_msg_id_t;

/* SipCallState values (see SIP_TELEPHONY_DPI.md). */
typedef enum {
	IVP_SIP_STATE_ON_HOOK            = 0,
	IVP_SIP_STATE_ON_HOOK_ALLOCATED  = 1,
	IVP_SIP_STATE_CONNECTING_OUT     = 2,
	IVP_SIP_STATE_CONNECTED_OUT      = 3,
	IVP_SIP_STATE_CONNECTING_IN      = 4,
	IVP_SIP_STATE_CONNECTED_IN       = 5,
} ivp_sip_call_state_t;

/* SipDisconnectReason values. */
typedef enum {
	IVP_SIP_REASON_NOT_SET        = 0,
	IVP_SIP_REASON_FAR_END        = 1,
	IVP_SIP_REASON_LOCAL_END      = 2,
	IVP_SIP_REASON_NOT_CONFIGURED = 3,
	IVP_SIP_REASON_IDLE           = 4,
	IVP_SIP_REASON_IN_USE         = 5,
} ivp_sip_disconnect_reason_t;

/**
 * Send PanelReset (0x92) wrapped in the CSO envelope as an HDLC I-frame.
 * This is the first thing the device sends after the HDLC link comes up.
 * It tells the matrix "I just came online — please push all label/state data."
 * Without this the matrix never marks the port as online.
 */
switch_status_t ivp_dpi_send_panel_reset(ivcore_channel_t *ch,
										  ivp_hdlc_send_data_cb send_cb);

/**
 * Send a 0xF1 ConnectReply back to the matrix.
 * Call this when FreeSWITCH signals the outbound SIP call was answered
 * (SWITCH_MESSAGE_INDICATE_ANSWER).
 *   success  1 = connected, 0 = failed
 *   reason   SipDisconnectReason (0 = NotSet for a successful answer)
 *   state    New SipCallState (IVP_SIP_STATE_CONNECTED_OUT = 3)
 */
switch_status_t ivp_dpi_send_connect_reply(ivcore_channel_t *ch,
											uint8_t success, uint8_t reason,
											uint8_t state,
											ivp_hdlc_send_data_cb send_cb);

/**
 * Send a 0x93 KeyStatusUpdate for key 0 to CPUApp.
 * This is the correct way to initiate a panel-side hangup — CPUApp's key
 * handler detects the key press on a telephony port and drives teardown.
 *   key_state  1 = press, 0 = release
 * Typically called twice: once with key_state=1, then key_state=0.
 */
switch_status_t ivp_dpi_send_key_status_update(ivcore_channel_t *ch,
												uint8_t key_state,
												ivp_hdlc_send_data_cb send_cb);

/**
 * Decode one inbound CSO+DPI message from an HDLC I-frame payload.
 *
 *   ch          per-channel context (dpi_state, dpi_init_sent, etc.).
 *   msg         raw HDLC I-frame payload — includes the 6-byte CSO header.
 *   msg_len     total byte count (must be > IVP_DPI_CSO_HEADER_SIZE).
 *   send_cb     callback to push reply HDLC frames back to the card.
 */
switch_status_t ivp_dpi_on_message(ivcore_channel_t *ch,
								   const uint8_t *msg, int msg_len,
								   ivp_hdlc_send_data_cb send_cb);

#endif /* IVP_DPI_H */
