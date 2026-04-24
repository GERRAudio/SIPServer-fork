/*
 * ravenna_ring.c — SPMC sample ring + fan-out cursors.
 */

#include "ravenna_ring.h"

#include <string.h>

static uint32_t round_pow2(uint32_t v)
{
	uint32_t p = 1;
	while (p < v) p <<= 1;
	return p;
}

switch_status_t ravenna_ring_create(ravenna_ring_t **out,
									switch_memory_pool_t *pool,
									uint32_t capacity_samples)
{
	ravenna_ring_t *r;
	uint32_t cap;

	if (!out || !pool || capacity_samples == 0) return SWITCH_STATUS_GENERR;

	cap = round_pow2(capacity_samples);

	r = switch_core_alloc(pool, sizeof(*r));
	memset(r, 0, sizeof(*r));
	r->buf      = switch_core_alloc(pool, cap * sizeof(ravenna_sample_t));
	r->capacity = cap;
	r->mask     = cap - 1;
	r->write_pos = 0;
	switch_mutex_init(&r->cursor_mtx, SWITCH_MUTEX_NESTED, pool);

	*out = r;
	return SWITCH_STATUS_SUCCESS;
}

void ravenna_ring_destroy(ravenna_ring_t **r)
{
	if (!r || !*r) return;
	*r = NULL;
}

void ravenna_ring_write(ravenna_ring_t *r,
						const ravenna_sample_t *src, uint32_t n)
{
	uint64_t w = r->write_pos;
	uint32_t off = (uint32_t)(w & r->mask);
	uint32_t first = r->capacity - off;
	int i;

	if (n == 0) return;
	if (n > r->capacity) {
		/* Drop everything older than the last `capacity` samples. */
		src += (n - r->capacity);
		n    = r->capacity;
		off  = 0;
		first = r->capacity;
	}

	if (n <= first) {
		memcpy(r->buf + off, src, n * sizeof(ravenna_sample_t));
	} else {
		memcpy(r->buf + off, src,         first * sizeof(ravenna_sample_t));
		memcpy(r->buf,       src + first, (n - first) * sizeof(ravenna_sample_t));
	}

	/* Publish */
	r->write_pos = w + n;

	/* Mark any cursor that now lags by more than capacity. We don't
	 * lock here — the cursor flags are only inspected by the
	 * corresponding reader, who treats `overrun` as terminal. */
	for (i = 0; i < RAVENNA_MAX_FANOUT; i++) {
		ravenna_cursor_t *c = &r->cursors[i];
		if (c->in_use && (r->write_pos - c->read_pos) > r->capacity) {
			c->overrun = SWITCH_TRUE;
		}
	}
}

ravenna_cursor_t *ravenna_ring_attach(ravenna_ring_t *r)
{
	int i;
	ravenna_cursor_t *got = NULL;

	switch_mutex_lock(r->cursor_mtx);
	for (i = 0; i < RAVENNA_MAX_FANOUT; i++) {
		if (!r->cursors[i].in_use) {
			got = &r->cursors[i];
			memset(got, 0, sizeof(*got));
			got->ring     = r;
			got->in_use   = SWITCH_TRUE;
			got->overrun  = SWITCH_FALSE;
			got->read_pos = r->write_pos;
			break;
		}
	}
	switch_mutex_unlock(r->cursor_mtx);
	return got;
}

void ravenna_ring_detach(ravenna_cursor_t **c)
{
	ravenna_cursor_t *cur;
	ravenna_ring_t   *r;
	if (!c || !*c) return;
	cur = *c;
	r   = cur->ring;
	if (r) switch_mutex_lock(r->cursor_mtx);
	cur->in_use = SWITCH_FALSE;
	cur->ring   = NULL;
	if (r) switch_mutex_unlock(r->cursor_mtx);
	*c = NULL;
}

uint64_t ravenna_cursor_available(ravenna_cursor_t *c)
{
	if (!c || !c->ring || !c->in_use) return 0;
	return c->ring->write_pos - c->read_pos;
}

int ravenna_cursor_read(ravenna_cursor_t *c, ravenna_sample_t *dst, uint32_t n)
{
	ravenna_ring_t *r;
	uint64_t avail;
	uint32_t off, first;

	if (!c || !c->ring || !c->in_use) return -1;
	if (c->overrun) return -1;
	r = c->ring;

	avail = r->write_pos - c->read_pos;
	if (avail == 0) return 0;
	if ((uint64_t)n > avail) n = (uint32_t)avail;

	off   = (uint32_t)(c->read_pos & r->mask);
	first = r->capacity - off;
	if (n <= first) {
		memcpy(dst, r->buf + off, n * sizeof(ravenna_sample_t));
	} else {
		memcpy(dst,         r->buf + off, first * sizeof(ravenna_sample_t));
		memcpy(dst + first, r->buf,       (n - first) * sizeof(ravenna_sample_t));
	}
	c->read_pos += n;

	if ((r->write_pos - c->read_pos) > r->capacity) {
		c->overrun = SWITCH_TRUE;
		return -1;
	}
	return (int)n;
}
