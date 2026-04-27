/**
 * ivp_dpi.c
 *
 * DPI (Dumb Panel Interface) decoder/encoder for the IVP data channel.
 * Carried inside HDLC I-frame payloads on top of IVP type=7 sub=1 frames.
 *
 * Wire framing (both directions):
 *   [CSO header: 6 bytes][DPI message ID: 1 byte][payload...]
 *
 *   CSO header = [MsgType: 4 bytes big-endian = 1][RouteDst: 1 = 0][RouteSrc: 1 = 0]
 *
 * Startup handshake (must match IvcDeviceBase.cs exactly):
 *   1. On HDLC link-up  → send PanelReset (0x92)
 *   2. On PanelTypeRequest (0x80) → send full init sequence
 *   3. On KeyStatusRequest (0x8B)  → send KeyStatusReply (0x94) per key
 *   4. After 6 key replies         → send NetworkPortStatus + PotPositions
 *
 * SIP telephony (bidirectional, 0xF0–0xF6):
 *   0xF0  GetState          → reply with current call state + cliInfo
 *   0xF1  DialOut request   → log dial number, reply connect success (stub)
 *   0xF4  DisconnectOutbound → reply success (stub)
 *   0xF5  DialInfo           → treat as definitive dial string, reply 0xF1 success, trigger dialplan
 *
 * Reference: tools/IVCore/Devices/IvcDeviceBase.cs
 *            tools/IVCore/Protocol/DpiMessages.cs
 */

#include "ivp_dpi.h"

#include <switch.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define IVC_DBGOUT(buf) OutputDebugStringA(buf)
#else
#define IVC_DBGOUT(buf) ((void)0)
#endif

/* =====================================================================
 * SIP telephone panel identity constants
 * These match SipTelephoneDevice in DeviceTypes.cs:
 *   PanelTypeCode  = 0x8106 (PanelTypes.Telephony)
 *   PanelSubType   = 0x11   (PANEL_SUB_TYPE_TELEPHONY_SIP_CLIENT)
 *   NumberOfKeys   = 0
 *   NumberOfPots   = 0
 *   ModuleCount    = 0
 * ===================================================================*/
#define DPI_PANEL_TYPE_HI    0x81
#define DPI_PANEL_TYPE_LO    0x06
#define DPI_PANEL_SUBTYPE    0x11
#define DPI_PANEL_NUM_KEYS   0
#define DPI_PANEL_NUM_POTS   0
#define DPI_PANEL_VERSION    3    /* >= 2 required for Unicode labels */
#define DPI_PANEL_MODULES    0

/* =====================================================================
 * CSO envelope helpers
 * ===================================================================*/

/* Prepend the 6-byte CSO envelope to a DPI message and send it as an
 * HDLC I-frame.  dpi[] must begin with the DPI message ID byte. */
static switch_status_t send_dpi_msg(ivcore_channel_t *ch,
									 const uint8_t *dpi, int dpi_len,
									 ivp_hdlc_send_data_cb send_cb)
{
	/* CSO header: MsgType(4 BE)=1, RouteDst=0, RouteSrc=0 */
	uint8_t buf[IVP_DPI_CSO_HEADER_SIZE + 256];
	/* Worst-case HDLC stuffing of a max-size DPI payload:
	 *   input  = IVP_DPI_CSO_HEADER_SIZE(6) + dpi_len(max 256) = 262 bytes
	 *   raw    = addr(1) + ctl(1) + ext(1) + 262 + crc(2)      = 267 bytes
	 *   stuffed = 0x7E + 2*267 + 0x7E                          = 536 bytes
	 * 512 was too small by 24 bytes, causing a stack buffer overflow that
	 * corrupted the return address and produced the 0xC0000005 AV at
	 * 0x00007FFC00000000 (torn/overwritten instruction pointer). */
	uint8_t frame[600];
	int     total, n;

	if (!ch || !dpi || dpi_len <= 0 || !send_cb) return SWITCH_STATUS_FALSE;
	if (dpi_len > 256) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_ivcore: DPI message too large (%d bytes)\n", dpi_len);
		return SWITCH_STATUS_FALSE;
	}

	/* [0..3] MsgType = 1 big-endian, [4] RouteDst = 0, [5] RouteSrc = 0 */
	buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x01;
	buf[4] = 0x00; buf[5] = 0x00;
	memcpy(buf + IVP_DPI_CSO_HEADER_SIZE, dpi, (size_t)dpi_len);
	total = IVP_DPI_CSO_HEADER_SIZE + dpi_len;

	n = ivp_hdlc_build_iframe(ch, buf, total, frame, (int)sizeof(frame));
	if (n <= 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_ivcore: DPI I-frame build failed (total=%d)\n", total);
		return SWITCH_STATUS_FALSE;
	}
	return send_cb(ch, frame, n);
}

/* =====================================================================
 * Panel init sequence senders
 * Mirror of IvcDeviceBase.SendPanelInitSequence() in C#.
 * ===================================================================*/

/* 0x92 PanelReset: [0x92][action=0][type=0] */
switch_status_t ivp_dpi_send_panel_reset(ivcore_channel_t *ch,
										  ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[3] = { (uint8_t)IVP_DPI_PANEL_RESET, 0x00, 0x00 };
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: DPI -> 0x92 PanelReset (HDLC link up)\n");
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* 0xF1 ConnectReply (public): call when FreeSWITCH answers the SIP call. */
switch_status_t ivp_dpi_send_connect_reply(ivcore_channel_t *ch,
											uint8_t success, uint8_t reason,
											uint8_t state,
											ivp_hdlc_send_data_cb send_cb)
{
	return send_connect_reply(ch, success, reason, state, send_cb);
}

/* 0x93 KeyStatusUpdate: [0x93][key=0][keyState][0][0][0][0][0]
 * Used to signal a panel-side hangup to CPUApp via the key handler.
 * key_state = 1 for press, 0 for release. */
switch_status_t ivp_dpi_send_key_status_update(ivcore_channel_t *ch,
												uint8_t key_state,
												ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[9] = { (uint8_t)IVP_DPI_KEY_STATUS_UPDATE,
					   0x00,       /* key = 0 (call signaling key) */
					   key_state,  /* 1 = press, 0 = release */
					   0, 0, 0, 0, 0, 0 };
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: DPI -> 0x93 KeyStatusUpdate key=0 state=%u\n",
		(unsigned)key_state);
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* 0x90 PanelTypeReply:
 *   [0x90][typeHi][typeLo][numKeys][numPots][subType][extHW][region][version]
 */
static switch_status_t send_panel_type_reply(ivcore_channel_t *ch,
											  ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[9];
	msg[0] = (uint8_t)IVP_DPI_PANEL_TYPE_REPLY;
	msg[1] = DPI_PANEL_TYPE_HI;
	msg[2] = DPI_PANEL_TYPE_LO;
	msg[3] = DPI_PANEL_NUM_KEYS;
	msg[4] = DPI_PANEL_NUM_POTS;
	msg[5] = DPI_PANEL_SUBTYPE;
	msg[6] = 0x00; /* ExtHardware */
	msg[7] = 0x00; /* Region */
	msg[8] = DPI_PANEL_VERSION;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: DPI -> 0x90 PanelTypeReply type=0x%02X%02X subType=0x%02X "
		"keys=%u pots=%u version=%u\n",
		DPI_PANEL_TYPE_HI, DPI_PANEL_TYPE_LO, DPI_PANEL_SUBTYPE,
		DPI_PANEL_NUM_KEYS, DPI_PANEL_NUM_POTS, DPI_PANEL_VERSION);
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* 0x91 PanelSwVersion:
 *   [0x91][major][minor][buildHi][buildLo][0][0][0][0]
 *   Matches C#: Major=3, Minor=0, Build=100
 */
static switch_status_t send_panel_sw_version(ivcore_channel_t *ch,
											  ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[9] = { (uint8_t)IVP_DPI_PANEL_SW_VERSION,
					   3, 0, 0, 100, 0, 0, 0, 0 };
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: DPI -> 0x91 PanelSwVersion 3.0.100\n");
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* 0xAC ChecksumMessage: [0xAC][01 00 01 F3 74 00 00 00]
 *   Matches the firmware checksum hardcoded in C# IvcDeviceBase.
 */
static switch_status_t send_checksum_message(ivcore_channel_t *ch,
											  ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[9] = { 0xAC, 0x01, 0x00, 0x01, 0xF3, 0x74, 0x00, 0x00, 0x00 };
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: DPI -> 0xAC ChecksumMessage\n");
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* 0x9E VPanelInfo: [0x9E][87 bytes]
 *   Wire layout (from IvcDeviceBase.SendPanelInfo):
 *   [0..1] PanelType BE, [2] ModuleCount, [3..4] PanelPost=0x0001,
 *   [5..8] UBootVersion, [9..12] KernelVersion, [13..16] AppVersion,
 *   [17..86] KernelVersionString (70 bytes)
 */
static switch_status_t send_vpanel_info(ivcore_channel_t *ch,
										 ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[88];
	const char *ver_str = "v.3.0.0-b100;";
	size_t      ver_len;
	memset(msg, 0, sizeof(msg));
	msg[0]  = 0x9E;
	msg[1]  = DPI_PANEL_TYPE_HI;
	msg[2]  = DPI_PANEL_TYPE_LO;
	msg[3]  = DPI_PANEL_MODULES;
	msg[4]  = 0x00; msg[5] = 0x01; /* PanelPost = 0x0001 */
	/* UBoot: 0x00 0x07 0x04 0x04 */
	msg[6]  = 0x00; msg[7] = 0x07; msg[8] = 0x04; msg[9] = 0x04;
	/* Kernel: 0x00 0x20 0x06 0x52 */
	msg[10] = 0x00; msg[11] = 0x20; msg[12] = 0x06; msg[13] = 0x52;
	/* App: 3.0.0 */
	msg[14] = 3; msg[15] = 0; msg[16] = 0; msg[17] = 0;
	/* Kernel version string at offset 18 */
	ver_len = strlen(ver_str);
	if (ver_len > 70) ver_len = 70;
	memcpy(msg + 18, ver_str, ver_len);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: DPI -> 0x9E VPanelInfo\n");
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* 0xB4 MiscPanelSettings: [0xB4][31 bytes serial string]
 *   Matches IvcDeviceBase.SendMiscPanelSettings().
 */
static switch_status_t send_misc_panel_settings(ivcore_channel_t *ch,
												  ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[32];
	const char *serial = "IVCore-Panel";
	size_t      slen;
	memset(msg, 0, sizeof(msg));
	msg[0] = 0xB4;
	slen = strlen(serial);
	if (slen > 31) slen = 31;
	memcpy(msg + 1, serial, slen);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: DPI -> 0xB4 MiscPanelSettings serial='%s'\n", serial);
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* Full panel init sequence sent in response to PanelTypeRequest (0x80).
 * On re-init (dpi_init_sent already set) only PanelTypeReply is sent again.
 * Mirrors IvcDeviceBase.SendPanelInitSequence() exactly.
 */
static void send_panel_init_sequence(ivcore_channel_t *ch,
									  ivp_hdlc_send_data_cb send_cb)
{
	if (ch->dpi_init_sent) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"mod_ivcore: DPI 0x80 PanelTypeRequest (re-init) — sending PanelTypeReply only\n");
		send_panel_type_reply(ch, send_cb);
		return;
	}

	ch->dpi_init_sent = SWITCH_TRUE;
	ch->dpi_key_status_replies = 0;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: DPI 0x80 PanelTypeRequest - sending full init sequence\n");

	send_panel_type_reply(ch, send_cb);    /* 0x90 */
	send_panel_sw_version(ch, send_cb);    /* 0x91 */
	send_checksum_message(ch, send_cb);    /* 0xAC */
	send_vpanel_info(ch, send_cb);         /* 0x9E */
	send_misc_panel_settings(ch, send_cb); /* 0xB4 */
	/* 0x9F PanelModuleInfo: DPI_PANEL_MODULES=0 so nothing to send */
}

/* 0x94 KeyStatusReply:
 *   [0x94][region][key][0 0 0 0 0 0]  (8 bytes + msgId = 9 total)
 *   Matches DpiKeyStatusReply.Serialize() in C#.
 */
static switch_status_t send_key_status_reply(ivcore_channel_t *ch,
											  uint8_t key, uint8_t region,
											  ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[9] = { (uint8_t)IVP_DPI_KEY_STATUS_REPLY,
					   region, key, 0, 0, 0, 0, 0, 0 };
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: DPI -> 0x94 KeyStatusReply key=%u region=%u\n",
		(unsigned)key, (unsigned)region);
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* NetworkPortStatus (0x98) and PotPositions (0x95) sent after 6 key replies.
 * Both are zero-payload stubs — the SIP telephone has no pots or network ports.
 */
static void send_network_port_status(ivcore_channel_t *ch,
									  ivp_hdlc_send_data_cb send_cb)
{
	/* 0x98 MultiplePotPosition used as NetworkPortStatus stub (1 byte body = 0) */
	uint8_t msg[2] = { 0x98, 0x00 };
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: DPI -> 0x98 NetworkPortStatus (stub)\n");
	send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

static void send_pot_positions(ivcore_channel_t *ch,
								ivp_hdlc_send_data_cb send_cb)
{
	/* 0x95 PotPosition stub — no pots on SIP telephone */
	uint8_t msg[2] = { 0x95, 0x00 };
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: DPI -> 0x95 PotPositions (stub)\n");
	send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* =====================================================================
 * SIP telephony reply senders
 * ===================================================================*/

/* 0xF0 GetStateReply: [0xF0][state][80 bytes cliInfo] */
static switch_status_t send_get_state_reply(ivcore_channel_t *ch,
											 ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[82];
	memset(msg, 0, sizeof(msg));
	msg[0] = (uint8_t)IVP_DPI_GET_STATE;
	msg[1] = ch->dpi_state;
	if (ch->params.calling_name[0])
		strncpy((char *)(msg + 2), ch->params.calling_name, 80);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"mod_ivcore: DPI -> 0xF0 GetStateReply state=%u\n",
		(unsigned)ch->dpi_state);
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* 0xF1 ConnectReply: [0xF1][bSuccess][reason][state] */
static switch_status_t send_connect_reply(ivcore_channel_t *ch,
										   uint8_t success, uint8_t reason,
										   uint8_t state,
										   ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[4] = { (uint8_t)IVP_DPI_CONNECT_OUTGOING,
					   success, reason, state };
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: DPI -> 0xF1 ConnectReply success=%u reason=%u state=%u\n",
		(unsigned)success, (unsigned)reason, (unsigned)state);
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* 0xF4 DisconnectOutboundReply: [0xF4][bSuccess]
 * Sent in response to a rack-initiated 0xF4 request. */
static switch_status_t send_disconnect_outbound_reply(ivcore_channel_t *ch,
											   uint8_t success,
											   ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[2] = { (uint8_t)IVP_DPI_DISCONNECT_OUTGOING, success };
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: DPI -> 0xF4 DisconnectOutboundReply success=%u\n",
		(unsigned)success);
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* 0xF4 panel-initiated disconnect: [0xF4][reason]
 * This is the SipDisconnectOutboundPanelRequest form — sent panel→rack when
 * the PANEL wants to signal to CPUApp that the call has ended.  CPUApp
 * processes this and clears its internal dial buffer.  This is distinct from
 * send_disconnect_outbound_reply() ([0xF4][success]) which is a reply to a
 * rack-initiated request and is silently ignored if no request was pending. */
static switch_status_t send_disconnect_outbound_request(ivcore_channel_t *ch,
													   uint8_t reason,
													   ivp_hdlc_send_data_cb send_cb)
{
	uint8_t msg[2] = { (uint8_t)IVP_DPI_DISCONNECT_OUTGOING, reason };
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"mod_ivcore: DPI -> 0xF4 DisconnectOutboundRequest reason=%u\n",
		(unsigned)reason);
	return send_dpi_msg(ch, msg, (int)sizeof(msg), send_cb);
}

/* Public: send a panel-initiated 0xF4 disconnect to CPUApp.
 * Uses the request form [0xF4][reason] so CPUApp processes it and clears
 * its dial buffer — not the reply form [0xF4][success] which CPUApp ignores
 * when no rack-initiated request was pending. */
switch_status_t ivp_dpi_send_disconnect_reply(ivcore_channel_t *ch,
											  uint8_t reason,
											  ivp_hdlc_send_data_cb send_cb)
{
	ch->dpi_state            = (uint8_t)IVP_SIP_STATE_ON_HOOK_ALLOCATED;
	ch->dpi_dial_buffer[0]   = '\0';
	ch->dpi_dial_cont_active = SWITCH_FALSE;
	ch->dpi_dial_pending     = SWITCH_FALSE;
	return send_disconnect_outbound_request(ch, reason, send_cb);
}

/* =====================================================================
 * Helpers
 * ===================================================================*/

/* Copy up to max_len bytes from src into dst as a null-terminated
 * ASCII string, stopping at the first NUL or non-printable byte. */
static void copy_ascii(char *dst, int dst_cap,
					   const uint8_t *src, int max_len)
{
	int i, o = 0;
	if (dst_cap <= 0) return;
	for (i = 0; i < max_len && o < dst_cap - 1; i++) {
		uint8_t b = src[i];
		if (b == 0x00) break;
		if (b < 0x20 || b > 0x7E) break;
		dst[o++] = (char)b;
	}
	dst[o] = '\0';
}

/* =====================================================================
 * Public entry point
 * ===================================================================*/

switch_status_t ivp_dpi_on_message(ivcore_channel_t *ch,
								   const uint8_t *msg, int msg_len,
								   ivp_hdlc_send_data_cb send_cb)
{
	const uint8_t *dpi;
	int            dpi_len;
	uint8_t        id;

	if (!ch || !msg || msg_len < 1) return SWITCH_STATUS_FALSE;

	/* Strip the 6-byte CSO envelope.  Every message from the matrix
	 * (rack → panel) is wrapped as [00 00 00 01 00 00][DPI ID][payload].
	 * Without stripping this, byte 0 is always 0x00, which is why the
	 * old code logged "unknown id=0x00" for every frame. */
	if (msg_len <= IVP_DPI_CSO_HEADER_SIZE) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_ivcore: DPI frame too short to contain CSO header (%d bytes)\n",
			msg_len);
		return SWITCH_STATUS_FALSE;
	}
	dpi     = msg + IVP_DPI_CSO_HEADER_SIZE;
	dpi_len = msg_len - IVP_DPI_CSO_HEADER_SIZE;
	id      = dpi[0];

	switch ((ivp_dpi_msg_id_t)id) {

	/* ── Panel init handshake ──────────────────────────────────────── */

	case IVP_DPI_PANEL_TYPE_REQUEST:
		/* 0x80: matrix is asking "what panel are you?" */
		send_panel_init_sequence(ch, send_cb);
		break;

	case IVP_DPI_KEY_STATUS_REQUEST: {
		/* 0x8B: matrix queries one key's current state.
		 * Payload after msgId: [key][page][?][?][region] (5 bytes typical). */
		uint8_t key    = (dpi_len >= 2) ? dpi[1] : 0;
		uint8_t region = (dpi_len >= 6) ? dpi[5] : 0;
		send_key_status_reply(ch, key, region, send_cb);
		ch->dpi_key_status_replies++;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"mod_ivcore: DPI <- 0x8B KeyStatusRequest key=%u region=%u "
			"(reply #%u)\n",
			(unsigned)key, (unsigned)region,
			(unsigned)ch->dpi_key_status_replies);
		if (ch->dpi_key_status_replies == 6) {
			send_network_port_status(ch, send_cb);
			send_pot_positions(ch, send_cb);
		}
		break;
	}

	case IVP_DPI_MATRIX_VERSION:
		/* 0xE7: matrix announces its firmware version — log and ignore. */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			"mod_ivcore: DPI <- 0xE7 MatrixVersion (%d bytes)\n", dpi_len);
		break;

	/* ── SIP telephony ─────────────────────────────────────────────── */

	case IVP_DPI_GET_STATE:
		/* Self-healing state sync: if our DPI state claims the call is
		 * connecting or connected but the IVP call is no longer UP,
		 * reset to OnHookAllocated before replying.  This unsticks the
		 * matrix when a previous call ended without a clean 0xF4 exchange
		 * (e.g. a crash, a retransmit race, or a stuck ConnectingOut). */
		if ((ch->dpi_state == (uint8_t)IVP_SIP_STATE_CONNECTING_OUT ||
			 ch->dpi_state == (uint8_t)IVP_SIP_STATE_CONNECTED_OUT  ||
			 ch->dpi_state == (uint8_t)IVP_SIP_STATE_CONNECTING_IN  ||
			 ch->dpi_state == (uint8_t)IVP_SIP_STATE_CONNECTED_IN)  &&
			ch->call_state != IVC_STATE_UP &&
			ch->call_state != IVC_STATE_RINGING) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: DPI 0xF0 GetState — dpi_state=%u but call_state=%d, "
				"resetting to OnHookAllocated\n",
				(unsigned)ch->dpi_state, (int)ch->call_state);
			ch->dpi_state = (uint8_t)IVP_SIP_STATE_ON_HOOK_ALLOCATED;
		}
		IVC_DBGOUT("[IVC-SIP] 0xF0 GetState\n");
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"mod_ivcore: DPI <- 0xF0 GetState\n");
		send_get_state_reply(ch, send_cb);
		break;

	case IVP_DPI_CONNECT_OUTGOING: {
		/* Request: [msgId][80 ASCII dial buffer][1 byte continuation] */
		char    dial[96];
		uint8_t cont = 0;
		if (dpi_len < 1 + 80) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: DPI 0xF1 truncated (%d bytes)\n", dpi_len);
			break;
		}
		copy_ascii(dial, (int)sizeof(dial), dpi + 1, 80);
		if (dpi_len >= 1 + 80 + 1) cont = dpi[1 + 80];
		{
			char _dbg[256];
			switch_snprintf(_dbg, sizeof(_dbg),
				"[IVC-SIP] 0xF1 DialOut dial='%s' cont=%u ch=0x%lX pending=%d\n",
				dial, (unsigned)cont, (unsigned long)(uintptr_t)ch,
				(int)ch->dpi_dial_pending);
			IVC_DBGOUT(_dbg);
		}

		/* Each 0xF1 packet contains the COMPLETE dial string accumulated
		 * so far (not just new incremental digits).  cont=1 means the
		 * user is still entering digits; cont=0 is the final number.
		 * Always REPLACE the buffer — never append — so that intermediate
		 * "still-typing" packets cannot concatenate with the final one
		 * and produce a doubled number (e.g. "919891898" for "9198"). */
		/* Reject an empty dial string — this is the matrix echoing back a
		 * spurious 0xF4 that was sent while CPUApp had no pending dial state.
		 * Accepting it would route an empty destination and leave the channel
		 * stuck in ConnectingOut.
		 *
		 * We MUST still reply (success=0) even for empty strings.  The 0xF1
		 * request/reply is a paired protocol handshake: if the panel never
		 * replies, CPUApp stays in "waiting for ConnectReply" state and will
		 * NOT send a real 0xF1 when the user actually dials, permanently
		 * blocking dial-out on this port. */
		if (dial[0] == '\0') {
			/* Empty 0xF1 is an off-hook notification: the matrix signals that
			 * the user has gone off-hook but hasn't finished dialing yet.
			 * The actual number will arrive in a 0xF5 DialInfo.
			 *
			 * We must reply success=1 (ConnectingOut) — NOT success=0 —
			 * because success=0 causes CPUApp to treat this as a REJECTED
			 * call attempt.  It then sends a second ConnectReply internally
			 * when 0xF5 arrives, resulting in the matrix displaying the number
			 * twice (e.g. "91989198" for a single dial of "9198").
			 *
			 * By replying success=1 here AND setting dpi_dial_pending=TRUE with
			 * an empty buffer, the 0xF5 path will update the buffer and skip
			 * its own ConnectReply — guaranteeing exactly one ConnectReply per
			 * dial session.  exchange_media won't route because it checks
			 * dpi_dial_buffer[0] before transferring. */
			{
				char _dbg[128];
				switch_snprintf(_dbg, sizeof(_dbg),
					"[IVC] 0xF1 empty dial (off-hook) — success=1 pending=TRUE ch=0x%lX\n",
					(unsigned long)(uintptr_t)ch);
				IVC_DBGOUT(_dbg);
			}
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"mod_ivcore: DPI <- 0xF1 empty dial (off-hook notification) — "
				"replying success=1 ConnectingOut, waiting for 0xF5\n");
			/* Reset dial diagnostics so a subsequent 0xF5 starts fresh. */
			ch->diag_dial_cont_packets = 0;
			ch->diag_dial_first_us     = switch_micro_time_now();
			ch->diag_dial_final_us     = 0;
			ch->diag_dial_raw[0]       = '\0';
			ch->diag_dial_source       = 0xF1;
			ch->dpi_dial_buffer[0]     = '\0';   /* empty — 0xF5 will fill it */
			ch->dpi_state = (uint8_t)IVP_SIP_STATE_CONNECTING_OUT;
			send_connect_reply(ch,
				/*success*/ 1,
				/*reason */ (uint8_t)IVP_SIP_REASON_NOT_SET,
				/*state  */ ch->dpi_state,
				send_cb);
			ch->dpi_dial_pending = SWITCH_TRUE;  /* suppresses 0xF5 ConnectReply */
			break;
		}

		switch_copy_string(ch->dpi_dial_buffer, dial, sizeof(ch->dpi_dial_buffer));
		ch->dpi_dial_cont_active = (cont != 0) ? SWITCH_TRUE : SWITCH_FALSE;

		/* --- Dial diagnostics ----------------------------------------
		 * Track the full lifecycle of each dial sequence so that
		 * channel_on_exchange_media() can stamp ivc_* channel variables
		 * before handing the session to the FreeSWITCH dialplan.
		 * diag_dial_first_us is set only on the very first packet of a
		 * new dial sequence (when cont_packets was 0 before this one). */
		if (ch->diag_dial_cont_packets == 0) {
			/* First packet of this dial sequence. */
			ch->diag_dial_source     = 0xF1;
			ch->diag_dial_first_us   = switch_micro_time_now();
			switch_copy_string(ch->diag_dial_raw, dial, sizeof(ch->diag_dial_raw));
		}
		if (cont != 0) {
			/* Continuation packet — matrix is still accumulating digits. */
			ch->diag_dial_cont_packets++;
			/* Keep diag_dial_raw up to date with the latest complete
			 * string the matrix has sent so far. */
			switch_copy_string(ch->diag_dial_raw, dial, sizeof(ch->diag_dial_raw));
		} else {
			/* Final/only packet — stamp completion time. */
			ch->diag_dial_final_us = switch_micro_time_now();
			switch_copy_string(ch->diag_dial_raw, dial, sizeof(ch->diag_dial_raw));
		}
		/* -------------------------------------------------------------- */

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"mod_ivcore: DPI <- 0xF1 DialOut dial='%s' cont=%u buffer='%s' "
			"cont_packets=%u first_us=%" SWITCH_TIME_T_FMT " final_us=%" SWITCH_TIME_T_FMT "\n",
			dial, (unsigned)cont, ch->dpi_dial_buffer,
			(unsigned)ch->diag_dial_cont_packets,
			ch->diag_dial_first_us, ch->diag_dial_final_us);

		if (cont == 0) {
			/* Final segment — acknowledge success and signal the
			 * exchange_media loop.
			 *
			 * LOCKING NOTE: switch_core_session_run holds the session
			 * write-lock for the entire duration of channel_on_exchange_media.
			 * Any cross-thread call that tries to acquire even a read-lock
			 * on the session (e.g. switch_core_session_locate) will block
			 * indefinitely, freezing this recv thread.  Therefore we MUST NOT
			 * call any switch_core_session_locate / switch_ivr_session_transfer
			 * from here.
			 *
			 * The correct pattern is:
			 *   1. Set dpi_dial_pending (volatile — seen by session thread).
			 *   2. Call switch_channel_set_flag(CF_BREAK) — uses only the
			 *      channel's flag-mutex, not the session rwlock — so it is
			 *      safe from any thread.
			 *   3. switch_core_session_read_frame returns SWITCH_STATUS_BREAK,
			 *      which SWITCH_READ_ACCEPTABLE treats as success.
			 *   4. The session thread loops, sees dpi_dial_pending, and calls
			 *      switch_ivr_session_transfer on its own stack — no deadlock. */
			ch->dpi_state        = (uint8_t)IVP_SIP_STATE_CONNECTING_OUT;
			ch->dpi_dial_pending = SWITCH_TRUE;
			{
				char _dbg[256];
				switch_snprintf(_dbg, sizeof(_dbg),
					"[IVC] 0xF1 sending ConnectReply success=1 ch=0x%lX dial='%s'\n",
					(unsigned long)(uintptr_t)ch, ch->dpi_dial_buffer);
				IVC_DBGOUT(_dbg);
			}
			send_connect_reply(ch,
				/*success*/ 1,
				/*reason */ (uint8_t)IVP_SIP_REASON_NOT_SET,
				/*state  */ ch->dpi_state,
				send_cb);
			if (ch->channel) {
				switch_channel_state_t cs =
					switch_channel_get_state(ch->channel);
				switch_bool_t ready = switch_channel_ready(ch->channel);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					"mod_ivcore: DPI 0xF1 pending set — channel state=%d ready=%d "
					"running=%d\n",
					(int)cs, (int)ready, (int)ch->running);
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					"mod_ivcore: DPI 0xF1 pending set but ch->channel is NULL\n");
			}
		}
		break;
	}

	case IVP_DPI_DISCONNECT_OUTGOING: {
		uint8_t reason = (dpi_len >= 2) ? dpi[1] : 0;
		{
			char _dbg[128];
			switch_snprintf(_dbg, sizeof(_dbg),
				"[IVC-SIP] 0xF4 DisconnectOutgoing reason=%u\n", (unsigned)reason);
			IVC_DBGOUT(_dbg);
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"mod_ivcore: DPI <- 0xF4 DisconnectOutgoing reason=%u\n",
			(unsigned)reason);

		/* If a dial is already queued and waiting for the exchange_media loop
		 * to route it, do NOT clear the buffer or hang up.  Some matrix firmware
		 * versions send a 0xF4 reason=NOT_SET immediately after 0xF5 DialInfo as
		 * a CPUApp state-reset.  Honouring it here would wipe dpi_dial_buffer
		 * before channel_on_exchange_media's ~20 ms poll can consume it, silently
		 * aborting the pending dial-out.  We already committed to the call by
		 * replying 0xF1 success=1 — ack the 0xF4 and let the dial proceed. */
		if (ch->dpi_dial_pending && ch->dpi_dial_buffer[0]) {
			{
				char _dbg[192];
				switch_snprintf(_dbg, sizeof(_dbg),
					"[IVC] 0xF4 reason=%u SUPPRESSED dial='%s' ch=0x%lX\n",
					(unsigned)reason, ch->dpi_dial_buffer, (unsigned long)(uintptr_t)ch);
				IVC_DBGOUT(_dbg);
			}
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"mod_ivcore: DPI 0xF4 reason=%u suppressed — dial pending for '%s', "
				"acking without clearing state\n",
				(unsigned)reason, ch->dpi_dial_buffer);
			send_disconnect_outbound_reply(ch, /*success*/ 1, send_cb);
			break;
		}
		{
			char _dbg[128];
			switch_snprintf(_dbg, sizeof(_dbg),
				"[IVC] 0xF4 reason=%u NORMAL teardown ch=0x%lX call_state=%d\n",
				(unsigned)reason, (unsigned long)(uintptr_t)ch, (int)ch->call_state);
			IVC_DBGOUT(_dbg);
		}

		ch->dpi_state = (uint8_t)IVP_SIP_STATE_ON_HOOK_ALLOCATED;
		ch->dpi_dial_buffer[0]     = '\0';
		ch->dpi_dial_cont_active   = SWITCH_FALSE;
		/* Reset dial diagnostics so next dial sequence starts clean. */
		ch->diag_dial_cont_packets = 0;
		ch->diag_dial_first_us     = 0;
		ch->diag_dial_final_us     = 0;
		ch->diag_dial_raw[0]       = '\0';
		ch->diag_dial_source       = 0;
		/* Rack initiated: reply with the reply form [0xF4][success=1] */
		send_disconnect_outbound_reply(ch, /*success*/ 1, send_cb);
		/* Only hang up the FreeSWITCH session on an explicit disconnect reason
		 * (FarEnd = remote SIP hung up, LocalEnd = panel user hung up).
		 *
		 * Do NOT hang up when reason=NOT_SET (0).  The matrix sends 0xF4
		 * reason=0 as a benign pre-dial state-reset while the IVP session is
		 * sitting idle in exchange_media (IVC_STATE_UP / OnHookAllocated).
		 * Treating that as a teardown kills the session before the user's
		 * 0xF1/0xF5 dial arrives, leaving the port dead until the next
		 * autoconnect respawn (500 ms later).
		 *
		 * The dpi_state is the authoritative indicator of an active SIP call;
		 * IVC_STATE_UP only reflects the IVP transport connection, not the
		 * SIP leg. */
		if (ch->session &&
			(reason == (uint8_t)IVP_SIP_REASON_FAR_END ||
			 reason == (uint8_t)IVP_SIP_REASON_LOCAL_END)) {
			char _dbg2[128];
			switch_snprintf(_dbg2, sizeof(_dbg2),
				"[IVC] 0xF4 reason=%u triggering hangup ch=0x%lX\n",
				(unsigned)reason, (unsigned long)(uintptr_t)ch);
			IVC_DBGOUT(_dbg2);
			switch_channel_hangup(
				switch_core_session_get_channel(ch->session),
				SWITCH_CAUSE_NORMAL_CLEARING);
		}
		break;
	}

	case IVP_DPI_DIAL_INFO: {
		/* [msgId][len][80 ASCII]
		 * Some matrices use a two-step dial flow:
		 *   1. 0xF1 DialOut with empty string (off-hook notification only)
		 *   2. 0xF5 DialInfo carrying the complete dialed number
		 * When 0xF5 arrives with a non-empty number we treat it as the
		 * definitive dial string, reply 0xF1 ConnectReply success=1 so the
		 * matrix advances to ConnectingOut, then signal the exchange_media
		 * loop to run the dialplan transfer. */
		char    digits[96];
		uint8_t len      = (dpi_len >= 2) ? dpi[1] : 0;
		int     copy_len = len;
		if (copy_len > 80) copy_len = 80;
		if (copy_len > dpi_len - 2) copy_len = dpi_len - 2;
		if (copy_len < 0) copy_len = 0;
		copy_ascii(digits, (int)sizeof(digits), dpi + 2, copy_len);
		{
			char _dbg[256];
			switch_snprintf(_dbg, sizeof(_dbg),
				"[IVC-SIP] 0xF5 DialInfo len=%u digits='%s'\n",
				(unsigned)len, digits);
			IVC_DBGOUT(_dbg);
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"mod_ivcore: DPI <- 0xF5 DialInfo len=%u digits='%s'\n",
			(unsigned)len, digits);

		if (digits[0] != '\0') {
			/* Always update the buffer with 0xF5's authoritative number.
			 * If 0xF1 already sent ConnectReply (dpi_dial_pending==TRUE),
			 * do NOT send a second ConnectReply — that would make CPUApp
			 * concatenate the number ("91989198").  Just update the buffer
			 * and re-kick the exchange_media loop via CF_BREAK. */
			switch_copy_string(ch->dpi_dial_buffer, digits, sizeof(ch->dpi_dial_buffer));
			ch->dpi_dial_cont_active = SWITCH_FALSE;
			if (ch->diag_dial_first_us == 0)
				ch->diag_dial_first_us = switch_micro_time_now();
			ch->diag_dial_final_us = switch_micro_time_now();
			ch->diag_dial_source   = 0xF5;
			switch_copy_string(ch->diag_dial_raw, digits, sizeof(ch->diag_dial_raw));
			ch->dpi_state = (uint8_t)IVP_SIP_STATE_CONNECTING_OUT;

			if (!ch->dpi_dial_pending) {
				/* 0xF1 did not already send ConnectReply — send it now. */
				send_connect_reply(ch,
					/*success*/ 1,
					/*reason */ (uint8_t)IVP_SIP_REASON_NOT_SET,
					/*state  */ ch->dpi_state,
					send_cb);
			}

			/* Signal the exchange_media loop — same safe pattern as 0xF1. */
			ch->dpi_dial_pending = SWITCH_TRUE;
			{
				char _dbg[128];
				switch_snprintf(_dbg, sizeof(_dbg),
					"[IVC] 0xF5 dpi_dial_pending=TRUE CF_BREAK set buffer='%s' ch=0x%lX\n",
					ch->dpi_dial_buffer, (unsigned long)(uintptr_t)ch);
				IVC_DBGOUT(_dbg);
			}
		}
		break;
	}

	case IVP_DPI_CONNECT_INCOMING:
	case IVP_DPI_DISCONNECT_INCOMING:
	case IVP_DPI_CLI_INFO:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"mod_ivcore: DPI <- 0x%02X (panel-originated, ignored)\n",
			(unsigned)id);
		break;

	default: {
		char preview[3 * 16 + 1];
		int  i, pos = 0;
		int  show = dpi_len < 16 ? dpi_len : 16;
		for (i = 0; i < show; i++)
			pos += switch_snprintf(preview + pos,
				(int)sizeof(preview) - pos, "%02X ", dpi[i]);
		if (pos > 0) preview[pos - 1] = '\0';
		IVC_LOG_DEBUG("mod_ivcore: DPI <- id=0x%02X len=%d bytes=%s%s\n",
			(unsigned)id, dpi_len, preview, dpi_len > 16 ? " ..." : "");
		break;
	}
	}

	return SWITCH_STATUS_SUCCESS;
}
