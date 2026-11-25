/* ring.h - byte ring buffer that hands out contiguous blocks (internal).
 * SPDX-License-Identifier: MIT */
#ifndef DX_RING_H
#define DX_RING_H

#include <stdint.h>

/*
 * In plain words: a circular area of memory where received packets are stored one after the
 * other. The oldest is freed first (pop), the newest is added at the tail (push). A block is
 * always contiguous, so a packet can be handed out as one pointer + length. When a block does
 * not fit before the end of the area, the leftover bytes are skipped ("padding") and the block
 * is placed at the start. The ring never allocates: the caller gives it the memory.
 */
typedef struct dx_ring {
    uint8_t* buf;
    uint32_t cap;
    uint32_t head; /* start of the oldest block */
    uint32_t tail; /* where the next block goes */
    uint32_t used; /* bytes in use, padding included (tells "full" from "empty") */
} dx_ring;

/** Uses @p buf (@p cap bytes) as ring memory. */
void dx_ring_init(dx_ring* r, uint8_t* buf, uint32_t cap);

/**
 * Reserves @p n contiguous bytes. Returns their address, or NULL if there is no room.
 * *adv receives how many ring bytes the block consumes (n, plus skipped padding); the caller
 * must remember it and give it back to dx_ring_pop when the block is released.
 */
uint8_t* dx_ring_push(dx_ring* r, uint32_t n, uint32_t* adv);

/** Releases the OLDEST block, whose push returned @p adv. Blocks are freed strictly in order. */
void dx_ring_pop(dx_ring* r, uint32_t adv);

/** Number of bytes not in use. A block of this size may still fail if it must wrap. */
uint32_t dx_ring_free(const dx_ring* r);

#endif
