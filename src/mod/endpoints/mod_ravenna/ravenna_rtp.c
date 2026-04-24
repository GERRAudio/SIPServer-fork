/*
 * ravenna_rtp.c — RTP header + AES67 sample codecs.
 *
 * Wire formats:
 *   L16: signed 16-bit big-endian, interleaved per RFC 3551.
 *   L24: signed 24-bit big-endian, interleaved per RFC 3190.
 *   L32: signed 32-bit big-endian, interleaved (non-standard for RTP
 *        but supported here for higher-precision routing). For
 *        AES67 interop, prefer L16 or L24.
 *
 * S16 native conversions:
 *   L16 → S16 : direct
 *   L24 → S16 : drop the LSB (>> 8)
 *   L32 → S16 : drop the low 16 bits (>> 16)
 *
 *   S16 → L24 : sample << 8
 *   S16 → L32 : sample << 16
 */

#include "ravenna_rtp.h"

#include <string.h>

switch_status_t ravenna_rtp_parse(const uint8_t *buf, int len,
								  ravenna_rtp_hdr_t *out)
{
	int csrc_count, has_ext, off;

	if (len < RAVENNA_RTP_HDR_SIZE) return SWITCH_STATUS_GENERR;

	out->version      = (buf[0] >> 6) & 0x03;
	if (out->version != RAVENNA_RTP_VERSION) return SWITCH_STATUS_GENERR;

	csrc_count        = buf[0] & 0x0F;
	has_ext           = (buf[0] >> 4) & 0x01;
	out->marker       = (buf[1] >> 7) & 0x01;
	out->payload_type =  buf[1] & 0x7F;
	out->seq          = ((uint16_t)buf[2] << 8) | buf[3];
	out->timestamp    = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
						((uint32_t)buf[6] << 8)  |  (uint32_t)buf[7];
	out->ssrc         = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
						((uint32_t)buf[10] << 8) |  (uint32_t)buf[11];

	off = RAVENNA_RTP_HDR_SIZE + csrc_count * 4;
	if (has_ext) {
		if (off + 4 > len) return SWITCH_STATUS_GENERR;
		{
			int ext_words = ((int)buf[off + 2] << 8) | buf[off + 3];
			off += 4 + ext_words * 4;
		}
	}
	if (off > len) return SWITCH_STATUS_GENERR;

	out->payload_off = off;
	out->payload_len = len - off;
	return SWITCH_STATUS_SUCCESS;
}

void ravenna_rtp_write_hdr(uint8_t *buf, uint8_t pt, uint16_t seq,
						   uint32_t ts, uint32_t ssrc, uint8_t marker)
{
	buf[0] = (RAVENNA_RTP_VERSION << 6);                      /* V=2, P=0, X=0, CC=0 */
	buf[1] = (uint8_t)((marker & 0x01) << 7) | (pt & 0x7F);
	buf[2] = (uint8_t)(seq >> 8);
	buf[3] = (uint8_t)(seq & 0xFF);
	buf[4] = (uint8_t)((ts  >> 24) & 0xFF);
	buf[5] = (uint8_t)((ts  >> 16) & 0xFF);
	buf[6] = (uint8_t)((ts  >>  8) & 0xFF);
	buf[7] = (uint8_t)( ts        & 0xFF);
	buf[8] = (uint8_t)((ssrc >> 24) & 0xFF);
	buf[9] = (uint8_t)((ssrc >> 16) & 0xFF);
	buf[10]= (uint8_t)((ssrc >>  8) & 0xFF);
	buf[11]= (uint8_t)( ssrc       & 0xFF);
}

/* ------------------------------------------------------------------
 *  Decode
 * ------------------------------------------------------------------ */

int ravenna_rtp_decode(const uint8_t *p, int plen,
					   ravenna_codec_t codec, int channels,
					   ravenna_sample_t **out, int out_caps)
{
	int bps = ravenna_codec_bytes(codec);
	int frame, samples;
	int ch;

	if (channels <= 0 || bps <= 0) return -1;
	frame   = bps * channels;
	if (frame == 0) return -1;
	samples = plen / frame;
	if (samples <= 0) return 0;
	if (samples > out_caps) samples = out_caps;

	switch (codec) {
	case RAV_CODEC_L16: {
		int s;
		for (s = 0; s < samples; s++) {
			for (ch = 0; ch < channels; ch++) {
				const uint8_t *q = p + (s * channels + ch) * 2;
				out[ch][s] = (int16_t)(((uint16_t)q[0] << 8) | q[1]);
			}
		}
		break;
	}
	case RAV_CODEC_L24: {
		int s;
		for (s = 0; s < samples; s++) {
			for (ch = 0; ch < channels; ch++) {
				const uint8_t *q = p + (s * channels + ch) * 3;
				int32_t v = ((int32_t)(int8_t)q[0] << 16) |
							((int32_t)q[1] << 8)         |
							 (int32_t)q[2];
				/* Sign-extend 24-bit into 32-bit, then >> 8 for S16. */
				out[ch][s] = (int16_t)(v >> 8);
			}
		}
		break;
	}
	case RAV_CODEC_L32: {
		int s;
		for (s = 0; s < samples; s++) {
			for (ch = 0; ch < channels; ch++) {
				const uint8_t *q = p + (s * channels + ch) * 4;
				int32_t v = ((int32_t)q[0] << 24) | ((int32_t)q[1] << 16) |
							((int32_t)q[2] << 8)  |  (int32_t)q[3];
				out[ch][s] = (int16_t)(v >> 16);
			}
		}
		break;
	}
	default: return -1;
	}
	return samples;
}

/* ------------------------------------------------------------------
 *  Encode
 * ------------------------------------------------------------------ */

int ravenna_rtp_encode(const ravenna_sample_t * const *in, int samples,
					   ravenna_codec_t codec, int channels,
					   uint8_t *payload, int payload_cap)
{
	int bps = ravenna_codec_bytes(codec);
	int need;
	int ch, s;

	if (channels <= 0 || bps <= 0 || samples <= 0) return -1;
	need = bps * channels * samples;
	if (need > payload_cap) return -1;

	switch (codec) {
	case RAV_CODEC_L16:
		for (s = 0; s < samples; s++) {
			for (ch = 0; ch < channels; ch++) {
				int16_t v = in[ch] ? in[ch][s] : 0;
				uint8_t *q = payload + (s * channels + ch) * 2;
				q[0] = (uint8_t)((uint16_t)v >> 8);
				q[1] = (uint8_t)((uint16_t)v & 0xFF);
			}
		}
		break;
	case RAV_CODEC_L24:
		for (s = 0; s < samples; s++) {
			for (ch = 0; ch < channels; ch++) {
				int32_t v = in[ch] ? ((int32_t)in[ch][s]) << 8 : 0;
				uint8_t *q = payload + (s * channels + ch) * 3;
				q[0] = (uint8_t)((v >> 16) & 0xFF);
				q[1] = (uint8_t)((v >>  8) & 0xFF);
				q[2] = (uint8_t)( v        & 0xFF);
			}
		}
		break;
	case RAV_CODEC_L32:
		for (s = 0; s < samples; s++) {
			for (ch = 0; ch < channels; ch++) {
				int32_t v = in[ch] ? ((int32_t)in[ch][s]) << 16 : 0;
				uint8_t *q = payload + (s * channels + ch) * 4;
				q[0] = (uint8_t)((v >> 24) & 0xFF);
				q[1] = (uint8_t)((v >> 16) & 0xFF);
				q[2] = (uint8_t)((v >>  8) & 0xFF);
				q[3] = (uint8_t)( v        & 0xFF);
			}
		}
		break;
	default: return -1;
	}
	return need;
}
