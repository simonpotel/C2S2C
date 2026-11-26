/* pool.h - fixed-size slot pool with a free list (internal).
 * SPDX-License-Identifier: MIT */
#ifndef DX_POOL_H
#define DX_POOL_H

#include <stdint.h>

#define DX_POOL_NONE 0xFFFFFFFFu

/*
 * N equally sized boxes in one block of memory. Taking a box and giving it
 * back are O(1) and never allocate. Free boxes form a chain: the first 4 bytes of a free box
 * store the index of the next free one, so no extra bookkeeping memory is needed.
 */
typedef struct dx_pool {
    uint8_t* mem;
    uint32_t slot_size; /* at least 4 */
    uint32_t count;
    uint32_t free_head; /* index of the first free slot, or DX_POOL_NONE */
    uint32_t in_use;
} dx_pool;

/** Uses @p mem (slot_size * count bytes, 4-byte aligned) as slots; all start free. */
void dx_pool_init(dx_pool* p, uint8_t* mem, uint32_t slot_size, uint32_t count);

/** Takes a free slot and returns its index, or DX_POOL_NONE if the pool is exhausted. */
uint32_t dx_pool_get(dx_pool* p);

/** Gives slot @p index back. The caller must not use it afterwards. */
void dx_pool_put(dx_pool* p, uint32_t index);

/** Address of slot @p index (must be < count). */
uint8_t* dx_pool_at(const dx_pool* p, uint32_t index);

#endif
