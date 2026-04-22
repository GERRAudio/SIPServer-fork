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
 *   0xF5  DialInfo           → log DTMF digits
 *
 * Reference: tools/IVCore/Devices/IvcDeviceBase.cs
 *            tools/IVCore/Protocol/DpiMessages.cs
 */

#include "ivp_dpi.h"

#include <switch.h>
#include <string.h>

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
	uint8_t frame[512];
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
		"mod_ivcore: DPI 0x80 PanelTypeRequest — sending full init sequence\n");

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

/* 0xF4 DisconnectOutboundReply: [0xF4][bSuccess] */
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
			size_t cur = strlen(ch->dpi_dial_buffer);
			size_t add = strlen(dial);
			if (cur + add < sizeof(ch->dpi_dial_buffer))
				memcpy(ch->dpi_dial_buffer + cur, dial, add + 1);
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"mod_ivcore: DPI <- 0xF1 DialOut dial='%s' cont=%u accumulated='%s'\n",
			dial, (unsigned)cont, ch->dpi_dial_buffer);

		if (cont == 0) {
			/* Final segment — acknowledge success so the matrix marks the
			 * call as connecting-out.  Signal the exchange_media loop that
			 * a dialplan transfer is needed; it will call
			 * switch_ivr_session_transfer() using dpi_dial_buffer. */
			ch->dpi_state = (uint8_t)IVP_SIP_STATE_CONNECTING_OUT;
			send_connect_reply(ch,
				/*success*/ 1,
				/*reason */ (uint8_t)IVP_SIP_REASON_NOT_SET,
				/*state  */ ch->dpi_state,
				send_cb);
			/* dpi_dial_buffer is intentionally NOT cleared here — the
			 * exchange_media loop reads it and clears it after transfer. */
			ch->dpi_dial_pending = SWITCH_TRUE;
		}
		break;
	}

	case IVP_DPI_DISCONNECT_OUTGOING: {
		uint8_t reason = (dpi_len >= 2) ? dpi[1] : 0;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"mod_ivcore: DPI <- 0xF4 DisconnectOutgoing reason=%u\n",
			(unsigned)reason);
		ch->dpi_state = (uint8_t)IVP_SIP_STATE_ON_HOOK_ALLOCATED;
		ch->dpi_dial_buffer[0] = '\0';
		send_disconnect_outbound_reply(ch, /*success*/ 1, send_cb);
		/* Only hang up the FreeSWITCH session if the SIP call was
		 * actually in progress (ConnectedOut or ConnectingOut).
		 * If the matrix sends 0xF4 to reset state before a new dial
		 * (while we are still in exchange_media / OnHookAllocated),
		 * do NOT hang up — the IVP session must stay alive so the
		 * next 0xF1 DialOut can route through it. */
		if (ch->session &&
			(reason == (uint8_t)IVP_SIP_REASON_FAR_END ||
			 reason == (uint8_t)IVP_SIP_REASON_LOCAL_END ||
			 ch->call_state == IVC_STATE_UP ||
			 ch->call_state == IVC_STATE_RINGING)) {
			switch_channel_hangup(
				switch_core_session_get_channel(ch->session),
				SWITCH_CAUSE_NORMAL_CLEARING);
		}
		break;
	}

	case IVP_DPI_DIAL_INFO: {
		/* [msgId][len][80 ASCII] */
		char    digits[96];
		uint8_t len      = (dpi_len >= 2) ? dpi[1] : 0;
		int     copy_len = len;
		if (copy_len > 80) copy_len = 80;
		if (copy_len > dpi_len - 2) copy_len = dpi_len - 2;
		if (copy_len < 0) copy_len = 0;
		copy_ascii(digits, (int)sizeof(digits), dpi + 2, copy_len);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"mod_ivcore: DPI <- 0xF5 DialInfo len=%u digits='%s'\n",
			(unsigned)len, digits);
		/* TODO: switch_channel_queue_dtmf for each digit. */
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
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"mod_ivcore: DPI <- id=0x%02X len=%d bytes=%s%s\n",
			(unsigned)id, dpi_len, preview, dpi_len > 16 ? " ..." : "");
		break;
	}
	}

	return SWITCH_STATUS_SUCCESS;
}
