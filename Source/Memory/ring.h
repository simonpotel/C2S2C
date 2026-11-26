/* ring.h - byte ring buffer that hands out contiguous blocks (internal).
 * SPDX-License-Identifier: MIT */
#ifndef C2S2C_RING_H
#define C2S2C_RING_H

#include <stdint.h>

/*
 * A circular area of memory where received packets are stored one after the
 * other. The oldest is freed first (pop), the newest is added at the tail (push). A block is
 * always contiguous, so a packet can be handed out as one pointer + length. When a block does
 * not fit before the end of the area, the leftover bytes are skipped ("padding") and the block
 * is placed at the start. The ring never allocates: the caller gives it the memory.
 */
typedef struct c2s2c_ring {
    uint8_t* buf;
    uint32_t cap;
    uint32_t head; /* start of the oldest block */
    uint32_t tail; /* where the next block goes */
    uint32_t used; /* bytes in use, padding included (tells "full" from "empty") */
} c2s2c_ring;

/** Uses @p buf (@p cap bytes) as ring memory. */
void c2s2c_ring_init(c2s2c_ring* r, uint8_t* buf, uint32_t cap);

/**
 * Reserves @p n contiguous bytes. Returns their address, or NULL if there is no room.
 * *adv receives how many ring bytes the block consumes (n, plus skipped padding); the caller
 * must remember it and give it back to c2s2c_ring_pop when the block is released.
 */
uint8_t* c2s2c_ring_push(c2s2c_ring* r, uint32_t n, uint32_t* adv);

/** Releases the OLDEST block, whose push returned @p adv. Blocks are freed strictly in order. */
void c2s2c_ring_pop(c2s2c_ring* r, uint32_t adv);

/** Number of bytes not in use. A block of this size may still fail if it must wrap. */
uint32_t c2s2c_ring_free(const c2s2c_ring* r);

#endif
