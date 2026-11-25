/* ring.c - byte ring buffer that hands out contiguous blocks.
 * SPDX-License-Identifier: MIT */
#include "ring.h"

#include <stddef.h>

void dx_ring_init(dx_ring* r, uint8_t* buf, uint32_t cap)
{
    r->buf = buf;
    r->cap = cap;
    r->head = r->tail = r->used = 0;
}

uint32_t dx_ring_free(const dx_ring* r)
{
    return r->cap - r->used;
}

uint8_t* dx_ring_push(dx_ring* r, uint32_t n, uint32_t* adv)
{
    if (n > r->cap - r->used) return NULL;
    if (r->used == 0) r->head = r->tail = 0; /* empty: restart at 0 for the longest free run */

    uint32_t at;
    if (r->tail >= r->head && r->used < r->cap) {
        /* Data sits in [head, tail): free space is [tail, cap) then [0, head). */
        if (r->cap - r->tail >= n) {
            at = r->tail;
            *adv = n;
        } else if (r->head >= n) {
            /* Not enough room before the end: skip it as padding and restart at 0. */
            at = 0;
            *adv = (r->cap - r->tail) + n;
        } else {
            return NULL;
        }
    } else {
        /* Wrapped (tail < head): the only free space is [tail, head). */
        if (r->head - r->tail < n) return NULL;
        at = r->tail;
        *adv = n;
    }
    if (*adv > r->cap - r->used) return NULL; /* padding must fit in the free bytes too */

    r->tail = (at + n) % r->cap;
    r->used += *adv;
    return r->buf + at;
}

void dx_ring_pop(dx_ring* r, uint32_t adv)
{
    r->head = (r->head + adv) % r->cap;
    r->used -= adv;
}
