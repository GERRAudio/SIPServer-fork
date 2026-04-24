/*
 * ravenna_ring.h
 *
 * Single-producer / multi-consumer (SPMC) sample ring with fan-out
 * cursors. The producer is the RX reactor (or, for TX, the FS write
 * path). Each consumer has its own read cursor; cursors do not
 * influence one another.
 *
 * Power-of-two capacity, indexed via a 64-bit monotonic counter.
 *
 * Producer never blocks. If a cursor falls more than `capacity`
 * samples behind, the cursor is flagged `overrun` — it is the
 * caller's responsibility (mod_ravenna.c read path) to react,
 * typically by hanging up the FS leg.
 */

#ifndef RAVENNA_RING_H
#define RAVENNA_RING_H

#include "mod_ravenna.h"

struct ravenna_cursor_s {
	ravenna_ring_t *ring;
	uint64_t        read_pos;     /* next sample to read */
	switch_bool_t   overrun;
	switch_bool_t   in_use;
};

struct ravenna_ring_s {
	ravenna_sample_t *buf;        /* size = capacity                   */
	uint32_t          capacity;   /* power of two, samples             */
	uint32_t          mask;       /* capacity - 1                      */
	volatile uint64_t write_pos;  /* monotonic write index (samples)   */

	/* Fan-out cursors */
	switch_mutex_t   *cursor_mtx; /* protects cursor add/remove        */
	ravenna_cursor_t  cursors[RAVENNA_MAX_FANOUT];
};

switch_status_t ravenna_ring_create(ravenna_ring_t **out,
									switch_memory_pool_t *pool,
									uint32_t capacity_samples);

void            ravenna_ring_destroy(ravenna_ring_t **r);

/* Producer: copy `n` samples in. Cursors that fall behind get
 * marked overrun (the producer never blocks). */
void            ravenna_ring_write(ravenna_ring_t *r,
								   const ravenna_sample_t *src, uint32_t n);

/* Consumer: attach/detach a cursor. Returns NULL if RAVENNA_MAX_FANOUT
 * is exhausted. New cursor starts at the current write position. */
ravenna_cursor_t *ravenna_ring_attach(ravenna_ring_t *r);
void              ravenna_ring_detach(ravenna_cursor_t **c);

/* Consumer: read up to `n` samples. Returns number of samples
 * actually copied. If overrun, returns -1 (and the caller should
 * detach + hang up). */
int  ravenna_cursor_read(ravenna_cursor_t *c, ravenna_sample_t *dst,
						 uint32_t n);

uint64_t ravenna_cursor_available(ravenna_cursor_t *c);

#endif /* RAVENNA_RING_H */
