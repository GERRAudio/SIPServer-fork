/**
 * ivp_hdlc.c
 *
 * Minimal HDLC data-link layer for the IVP Data channel.  See
 * ivp_hdlc.h for scope and references.
 */

#include "ivp_hdlc.h"
#include "ivp_dpi.h"
#include <switch.h>
#include <string.h>

/* =====================================================================
 * CRC-16/X.25 (FCS)
 *   poly = 0x1021, init = 0xFFFF, refin = true, refout = true,
 *   xorout = 0xFFFF
 * ===================================================================*/

static uint16_t crc16_x25_update(uint16_t crc, uint8_t b)
{
	int i;
	crc ^= (uint16_t)b;
	for (i = 0; i < 8; i++) {
		if (crc & 1) crc = (crc >> 1) ^ 0x8408; /* 0x1021 reflected */
		else         crc >>= 1;
	}
	return crc;
}

static uint16_t crc16_x25(const uint8_t *buf, int len)
{
	uint16_t crc = 0xFFFF;
	int i;
	for (i = 0; i < len; i++) crc = crc16_x25_update(crc, buf[i]);
	return (uint16_t)(crc ^ 0xFFFF);
}

/* =====================================================================
 * Byte stuffing / unstuffing
 * ===================================================================*/

/* Stuff 'src' (raw frame: address, control..., crc-lo, crc-hi) into
 * 'dst' wrapped in 0x7E flags.  Returns total bytes written. */
static int hdlc_stuff(uint8_t *dst, int dst_cap,
					   const uint8_t *src, int src_len)
{
	int o = 0;
	int i;
	if (o + 1 > dst_cap) return -1;
	dst[o++] = IVP_HDLC_FLAG;
	for (i = 0; i < src_len; i++) {
		uint8_t b = src[i];
		if (b == IVP_HDLC_FLAG || b == IVP_HDLC_ESCAPE) {
			if (o + 2 > dst_cap) return -1;
			dst[o++] = IVP_HDLC_ESCAPE;
			dst[o++] = (uint8_t)(b ^ IVP_HDLC_XOR);
		} else {
			if (o + 1 > dst_cap) return -1;
			dst[o++] = b;
		}
	}
	if (o + 1 > dst_cap) return -1;
	dst[o++] = IVP_HDLC_FLAG;
	return o;
}

/* Unstuff one HDLC frame starting at 'src' (which may include leading
 * flag(s)).  Writes the raw frame (address..crc-hi) into 'dst'.
 * Returns the number of raw bytes written, or -1 if no valid frame
 * was found.  *consumed is set to the number of input bytes consumed
 * (up to and including the trailing flag). */
static int hdlc_unstuff_one(const uint8_t *src, int src_len,
							 uint8_t *dst, int dst_cap, int *consumed)
{
	int i = 0;
	int o = 0;
	int started = 0;
	*consumed = 0;

	/* skip leading flags */
	while (i < src_len && src[i] == IVP_HDLC_FLAG) i++;

	for (; i < src_len; i++) {
		uint8_t b = src[i];
		if (b == IVP_HDLC_FLAG) {
			*consumed = i + 1;
			return started ? o : 0;
		}
		started = 1;
		if (b == IVP_HDLC_ESCAPE) {
			if (i + 1 >= src_len) return -1;
			i++;
			b = (uint8_t)(src[i] ^ IVP_HDLC_XOR);
		}
		if (o >= dst_cap) return -1;
		dst[o++] = b;
	}
	/* No trailing flag — frame may be split across IVP packets, treat
	 * as not-yet-complete. */
	*consumed = src_len;
	return -1;
}

/* =====================================================================
 * Frame builders
 * ===================================================================*/

/* Build a frame: [addr][ctl][crc-lo][crc-hi] then stuff & flag. */
static int build_simple_frame(uint8_t *out, int out_cap,
							   uint8_t addr, uint8_t ctl)
{
	uint8_t raw[4];
	uint16_t crc;
	raw[0] = addr;
	raw[1] = ctl;
	crc = crc16_x25(raw, 2);
	raw[2] = (uint8_t)(crc & 0xFF);
	raw[3] = (uint8_t)(crc >> 8);
	return hdlc_stuff(out, out_cap, raw, 4);
}

/* Build a 5-byte frame: [addr][ctl][ext][crc-lo][crc-hi]. */
static int build_ext_frame(uint8_t *out, int out_cap,
							uint8_t addr, uint8_t ctl, uint8_t ext)
{
	uint8_t raw[5];
	uint16_t crc;
	raw[0] = addr;
	raw[1] = ctl;
	raw[2] = ext;
	crc = crc16_x25(raw, 3);
	raw[3] = (uint8_t)(crc & 0xFF);
	raw[4] = (uint8_t)(crc >> 8);
	return hdlc_stuff(out, out_cap, raw, 5);
}

int ivp_hdlc_build_ua(uint8_t *out_buf, int out_cap)
{
	/* U-UA: Control = 0x73 with P/F=1 (0b01110011).  Address = response. */
	return build_simple_frame(out_buf, out_cap, IVP_HDLC_ADDR_RESP, 0x73);
}

int ivp_hdlc_build_rr(const ivcore_channel_t *ch,
					   uint8_t *out_buf, int out_cap)
{
	/* S-frame RR control = 0x01.  Extended byte = ackSeq<<1 | P/F. */
	uint8_t ext = (uint8_t)(((ch->hdlc.v_r & 0x7F) << 1) | 0x01);
	return build_ext_frame(out_buf, out_cap, IVP_HDLC_ADDR_RESP, 0x01, ext);
}

int ivp_hdlc_build_iframe(ivcore_channel_t *ch,
						   const uint8_t *payload, int payload_len,
						   uint8_t *out_buf, int out_cap)
{
	/* I-frame: [addr][ctl][ext][payload...][crc-lo][crc-hi]
	 *   ctl = (v_s<<1) | 0  (LSB 0 = I-frame)
	 *   ext = (v_r<<1) | P/F   (P/F=1)
	 */
	uint8_t raw[256];
	int     raw_len = 3 + payload_len + 2;
	uint16_t crc;
	if (!ch || !payload || payload_len < 0) return -1;
	if (raw_len > (int)sizeof(raw)) return -1;

	raw[0] = IVP_HDLC_ADDR_RESP;
	raw[1] = (uint8_t)((ch->hdlc.v_s & 0x7F) << 1);
	raw[2] = (uint8_t)(((ch->hdlc.v_r & 0x7F) << 1) | 0x01);
	if (payload_len > 0) memcpy(raw + 3, payload, (size_t)payload_len);
	crc = crc16_x25(raw, 3 + payload_len);
	raw[3 + payload_len + 0] = (uint8_t)(crc & 0xFF);
	raw[3 + payload_len + 1] = (uint8_t)(crc >> 8);

	ch->hdlc.v_s = (uint8_t)((ch->hdlc.v_s + 1) & 0x7F);

	return hdlc_stuff(out_buf, out_cap, raw, raw_len);
}

/* =====================================================================
 * State management
 * ===================================================================*/

void ivp_hdlc_reset(ivcore_channel_t *ch)
{
	if (!ch) return;
	memset(&ch->hdlc, 0, sizeof(ch->hdlc));
	ch->hdlc.link_up    = SWITCH_FALSE;
	ch->hdlc.v_s        = 0;
	ch->hdlc.v_r        = 0;
	ch->hdlc.last_rr_us = 0;
}

/* Decode a raw (unstuffed) HDLC frame: address, control, [extended]
 * for I/S frames, then payload, then 2-byte CRC.  Validates CRC.
 * Returns 1 if valid, 0 otherwise. */
static int decode_raw(const uint8_t *raw, int raw_len,
					   uint8_t *out_addr, uint8_t *out_ctl,
					   uint8_t *out_ext, int *out_has_ext,
					   const uint8_t **out_payload, int *out_payload_len)
{
	uint16_t crc_calc;
	uint16_t crc_wire;
	int has_ext;
	if (raw_len < 4) return 0;

	crc_calc = crc16_x25(raw, raw_len - 2);
	crc_wire = (uint16_t)(raw[raw_len - 2] | (raw[raw_len - 1] << 8));
	if (crc_calc != crc_wire) return 0;

	*out_addr = raw[0];
	*out_ctl  = raw[1];

	/* Determine HDLC frame type from control byte:
	 *   bit0 = 0          -> I-frame  (has extended ackSeq byte + payload)
	 *   bits 0-1 = 01     -> S-frame  (has extended ackSeq byte, no payload)
	 *   bits 0-1 = 11     -> U-frame  (no extended byte, no payload typically)
	 */
	if ((raw[1] & 0x01) == 0x00) {
		/* I-frame */
		has_ext = 1;
	} else if ((raw[1] & 0x03) == 0x01) {
		/* S-frame */
		has_ext = 1;
	} else {
		/* U-frame */
		has_ext = 0;
	}

	if (has_ext) {
		if (raw_len < 5) return 0;
		*out_ext = raw[2];
		*out_payload = raw + 3;
		*out_payload_len = raw_len - 3 - 2; /* minus ext omitted? no: raw_len - addr(1) - ctl(1) - ext(1) - crc(2) */
		/* Wait — payload starts after addr+ctl+ext (3 bytes). */
		*out_payload_len = raw_len - 3 - 2;
		if (*out_payload_len < 0) *out_payload_len = 0;
	} else {
		*out_ext = 0;
		*out_payload = raw + 2;
		*out_payload_len = raw_len - 2 - 2;
		if (*out_payload_len < 0) *out_payload_len = 0;
	}
	*out_has_ext = has_ext;
	return 1;
}

switch_status_t ivp_hdlc_on_data(ivcore_channel_t *ch,
								  const uint8_t *payload, int payload_len,
								  ivp_hdlc_send_data_cb send_cb)
{
	int off = 0;

	if (!ch || !payload || payload_len <= 0) return SWITCH_STATUS_FALSE;

	while (off < payload_len) {
		uint8_t  raw[256];
		int      consumed = 0;
		int      raw_len;
		uint8_t  addr, ctl, ext;
		int      has_ext;
		const uint8_t *frame_payload;
		int      frame_payload_len;

		raw_len = hdlc_unstuff_one(payload + off, payload_len - off,
									raw, (int)sizeof(raw), &consumed);
		if (raw_len <= 0) {
			/* Not a complete frame in this packet — stop. */
			IVC_LOG_DEBUG("mod_ivcore: HDLC unstuff no complete frame "
				"(off=%d payload_len=%d consumed=%d)\n",
				off, payload_len, consumed);
			break;
		}
		off += consumed;

		if (!decode_raw(raw, raw_len, &addr, &ctl, &ext, &has_ext,
						 &frame_payload, &frame_payload_len)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_ivcore: HDLC CRC/format error (rawLen=%d)\n", raw_len);
			continue;
		}

		if ((ctl & 0x03) == 0x03) {
			/* U-frame.  Mask off P/F bit (0x10) for type compare. */
			uint8_t u = (uint8_t)(ctl & ~0x10);

			if (u == 0x6F) {
				/* SABME (0b01101111).  Reply with UA. */
				uint8_t ua[16];
				int n;
				ch->hdlc.sabme_count++;
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					"mod_ivcore: HDLC SABME #%u — resetting v_r=%u v_s=%u "
					"dpi_state=%u dpi_dial_pending=%d dpi_dial_buffer='%s'\n",
					(unsigned)ch->hdlc.sabme_count,
					(unsigned)ch->hdlc.v_r, (unsigned)ch->hdlc.v_s,
					(unsigned)ch->dpi_state,
					(int)ch->dpi_dial_pending, ch->dpi_dial_buffer);
				n = ivp_hdlc_build_ua(ua, (int)sizeof(ua));
				if (n > 0 && send_cb) send_cb(ch, ua, n);

				ch->hdlc.v_s = 0;
					ch->hdlc.v_r = 0;
					ch->hdlc.link_up = SWITCH_TRUE;
					ch->hdlc.last_rr_us = switch_micro_time_now();
					ch->dpi_init_sent = SWITCH_FALSE;
					ch->dpi_key_status_replies = 0;
					/* Signal the IVP recv loop to reset its in_sequence counter
					 * so that the IVP-level dedup re-syncs with the fresh HDLC
					 * link.  Without this, HDLC v_r resets to 0 but the IVP
					 * layer still thinks it has already seen those sequence
					 * numbers and silently drops the re-sent I-frames — OR the
					 * IVP layer re-processes them and doubles the dial string. */
					ch->hdlc_reset_pending = SWITCH_TRUE;

				/* Set allocated-but-idle so a 0xF0 GetState reply is
				 * meaningful before the first 0x80 PanelTypeRequest. */
				if (ch->dpi_state == 0)
					ch->dpi_state = 1; /* IVP_SIP_STATE_ON_HOOK_ALLOCATED */

				/* PanelReset (0x92) is the first thing a real device sends
					 * after the HDLC link comes up.  Without it the matrix never
					 * marks the port online or sends 0xF1 dial-out commands. */
					ivp_dpi_send_panel_reset(ch, send_cb);

					/* Proactively clear any leftover SIP telephony state in
					 * CPUApp when the HDLC link was re-established mid-call.
					 * Only send 0xF4 when dpi_state indicates an active call;
					 * sending it in OnHookAllocated (state=1) would cause CPUApp
					 * to echo back an empty 0xF1 DialOut that stalls dial-out. */
						if (ch->dpi_state == (uint8_t)IVP_SIP_STATE_CONNECTING_OUT ||
							ch->dpi_state == (uint8_t)IVP_SIP_STATE_CONNECTED_OUT  ||
							ch->dpi_state == (uint8_t)IVP_SIP_STATE_CONNECTING_IN  ||
							ch->dpi_state == (uint8_t)IVP_SIP_STATE_CONNECTED_IN) {
							ivp_dpi_send_disconnect_reply(ch,
								(uint8_t)IVP_SIP_REASON_LOCAL_END, send_cb);
						}
			} else if (u == 0x63) {
				/* UA — server acknowledged our SABME (we did not send one
				 * but log for completeness). */
				IVC_LOG_DEBUG("mod_ivcore: HDLC UA received\n");
			} else if (u == 0x43) {
				/* DISC */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					"mod_ivcore: HDLC DISC received\n");
				ch->hdlc.link_up = SWITCH_FALSE;
			} else {
				IVC_LOG_DEBUG("mod_ivcore: HDLC U-frame ctl=0x%02X (unhandled)\n", ctl);
			}
		} else if ((ctl & 0x03) == 0x01) {
			/* S-frame from server (rare for us but possible). */
				IVC_LOG_DEBUG("mod_ivcore: HDLC S-frame ctl=0x%02X ext=0x%02X\n", ctl, ext);
		} else {
			/* I-frame.  Update v_r so our next RR/UA ackSeq advances. */
			uint8_t send_seq = (uint8_t)((ctl >> 1) & 0x7F);
			ch->hdlc.iframe_count++;
			IVC_LOG_DEBUG("mod_ivcore: HDLC I-frame #%u send_seq=%u v_r=%u payloadLen=%d %s\n",
				(unsigned)ch->hdlc.iframe_count,
				(unsigned)send_seq, (unsigned)ch->hdlc.v_r,
				frame_payload_len,
				(send_seq == ch->hdlc.v_r) ? "ACCEPTED" : "SEQ-MISMATCH-DROPPED");

			/* Accept if it matches v_r; advance v_r for ack. */
			if (send_seq == ch->hdlc.v_r) {
				ch->hdlc.v_r = (uint8_t)((ch->hdlc.v_r + 1) & 0x7F);

				/* Hand the I-frame payload to the DPI decoder so we
					 * can see dial strings, DTMF, state queries, etc. */
					if (frame_payload_len > 0) {
						IVC_LOG_DEBUG("mod_ivcore: HDLC I-frame -> ivp_dpi_on_message payloadLen=%d "
							"firstByte=0x%02X\n",
							frame_payload_len,
							(frame_payload_len > 0) ? frame_payload[0] : 0);
						ivp_dpi_on_message(ch, frame_payload,
										   frame_payload_len, send_cb);
					}
			}

			/* Acknowledge with an immediate RR so the server's window
			 * does not stall. */
			{
				uint8_t rr[16];
				int n = ivp_hdlc_build_rr(ch, rr, (int)sizeof(rr));
				if (n > 0 && send_cb) send_cb(ch, rr, n);
				ch->hdlc.last_rr_us = switch_micro_time_now();
			}
		}
	}

	return SWITCH_STATUS_SUCCESS;
}
