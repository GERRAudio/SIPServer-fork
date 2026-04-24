/*
 * ravenna_rtp.h — minimal RTP header + L16/L24/L32 sample codec.
 *
 * No allocations. Caller provides all buffers.
 */

#ifndef RAVENNA_RTP_H
#define RAVENNA_RTP_H

#include "mod_ravenna.h"

#define RAVENNA_RTP_HDR_SIZE 12
#define RAVENNA_RTP_VERSION  2

typedef struct {
	uint8_t  version;       /* 2 */
	uint8_t  payload_type;
	uint16_t seq;
	uint32_t timestamp;
	uint32_t ssrc;
	uint8_t  marker;
	int      payload_off;   /* offset of payload in src buffer */
	int      payload_len;   /* length of payload in src buffer */
} ravenna_rtp_hdr_t;

/* Parse an RTP header from `buf`/`len`. Returns SWITCH_STATUS_SUCCESS
 * on a valid v2 packet, or _GENERR. CSRC list and extensions are
 * skipped over (payload_off accounts for them). */
switch_status_t ravenna_rtp_parse(const uint8_t *buf, int len,
								  ravenna_rtp_hdr_t *out);

/* Write a 12-byte RTP header into `buf` (must be at least 12 bytes). */
void ravenna_rtp_write_hdr(uint8_t *buf, uint8_t pt, uint16_t seq,
						   uint32_t ts, uint32_t ssrc, uint8_t marker);

/* Decode payload (interleaved channels, big-endian samples) into
 * per-channel S16 sample arrays.
 *
 *   payload      : pointer to payload bytes
 *   payload_len  : length in bytes
 *   codec        : RAV_CODEC_L16 / L24 / L32
 *   channels     : number of interleaved channels
 *   out          : array of `channels` pointers, each capable of holding
 *                  `samples_per_channel` int16 samples
 *   out_caps     : sample capacity per channel (for safety)
 *
 * Returns number of samples per channel actually decoded, or -1.
 */
int ravenna_rtp_decode(const uint8_t *payload, int payload_len,
					   ravenna_codec_t codec, int channels,
					   ravenna_sample_t **out, int out_caps);

/* Encode per-channel S16 samples into an interleaved RTP payload.
 *
 *   in           : array of `channels` pointers, each holding `samples` ints
 *   samples      : samples per channel
 *   payload      : output buffer (caller-sized)
 *   payload_cap  : capacity in bytes
 *
 * Returns number of bytes written, or -1.
 */
int ravenna_rtp_encode(const ravenna_sample_t * const *in, int samples,
					   ravenna_codec_t codec, int channels,
					   uint8_t *payload, int payload_cap);

/* Bytes per packetised payload for given parameters. */
static inline int ravenna_rtp_payload_bytes(ravenna_codec_t c,
											int channels, int samples)
{
	return ravenna_codec_bytes(c) * channels * samples;
}

#endif /* RAVENNA_RTP_H */
