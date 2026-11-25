/* pool.c - fixed-size slot pool with a free list.
 * SPDX-License-Identifier: MIT */
#include "pool.h"

#include <string.h>

/* Free slots store the index of the next free slot in their first 4 bytes. */
static void set_next(const dx_pool* p, uint32_t index, uint32_t next)
{
    memcpy(dx_pool_at(p, index), &next, sizeof next);
}

static uint32_t get_next(const dx_pool* p, uint32_t index)
{
    uint32_t next;
    memcpy(&next, dx_pool_at(p, index), sizeof next);
    return next;
}

uint8_t* dx_pool_at(const dx_pool* p, uint32_t index)
{
    return p->mem + (size_t)index * p->slot_size;
}

void dx_pool_init(dx_pool* p, uint8_t* mem, uint32_t slot_size, uint32_t count)
{
    p->mem = mem;
    p->slot_size = slot_size;
    p->count = count;
    p->in_use = 0;
    p->free_head = count ? 0 : DX_POOL_NONE;
    /* Chain every slot to the next one; the last ends the chain. */
    for (uint32_t i = 0; i < count; i++)
        set_next(p, i, i + 1 < count ? i + 1 : DX_POOL_NONE);
}

uint32_t dx_pool_get(dx_pool* p)
{
    uint32_t index = p->free_head;
    if (index == DX_POOL_NONE) return DX_POOL_NONE;
    p->free_head = get_next(p, index);
    p->in_use++;
    return index;
}

void dx_pool_put(dx_pool* p, uint32_t index)
{
    set_next(p, index, p->free_head);
    p->free_head = index;
    p->in_use--;
}
