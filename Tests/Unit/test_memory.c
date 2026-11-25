/* test_memory.c - unit tests: ring buffer and slot pool.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "../check.h"
#include "Memory/pool.h"
#include "Memory/ring.h"

/* Fill a block with a byte derived from its id, to detect overlap or corruption later. */
static void fill(uint8_t* p, uint32_t n, uint8_t id)
{
    memset(p, id, n);
}

static int intact(const uint8_t* p, uint32_t n, uint8_t id)
{
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != id) return 0;
    return 1;
}

/* Push and pop in order; the ring reports full and recovers after a pop. */
static void test_ring_basic(void)
{
    uint8_t mem[16];
    dx_ring r;
    uint32_t a1, a2, a3;
    dx_ring_init(&r, mem, sizeof mem);

    uint8_t* b1 = dx_ring_push(&r, 6, &a1);
    uint8_t* b2 = dx_ring_push(&r, 6, &a2);
    CHECK(b1 == mem && b2 == mem + 6 && a1 == 6 && a2 == 6);
    CHECK(dx_ring_free(&r) == 4);
    CHECK(dx_ring_push(&r, 5, &a3) == NULL); /* only 4 free */
    CHECK(dx_ring_push(&r, 4, &a3) != NULL); /* exactly full */
    CHECK(dx_ring_free(&r) == 0 && dx_ring_push(&r, 1, &a3) == NULL);
    dx_ring_pop(&r, a1);
    CHECK(dx_ring_free(&r) == 6);
    CHECK(dx_ring_push(&r, 0, &a3) != NULL); /* zero-length blocks are allowed */
}

/* A block that does not fit before the end skips the rest and restarts at 0. */
static void test_ring_wrap(void)
{
    uint8_t mem[16];
    dx_ring r;
    uint32_t a1, a2, a3;
    dx_ring_init(&r, mem, sizeof mem);

    dx_ring_push(&r, 10, &a1);             /* [0,10) */
    dx_ring_push(&r, 4, &a2);              /* [10,14) */
    dx_ring_pop(&r, a1);                   /* free [0,10) and [14,16) */
    uint8_t* b = dx_ring_push(&r, 5, &a3); /* 2 bytes left at the end: padding, then at 0 */
    CHECK(b == mem && a3 == 2 + 5);
    CHECK(dx_ring_push(&r, 6, &a1) == NULL); /* only [5,10) free = 5 contiguous */
    dx_ring_pop(&r, a2);
    dx_ring_pop(&r, a3); /* releasing the padded block frees its padding too */
    CHECK(dx_ring_free(&r) == 16);
    CHECK(dx_ring_push(&r, 16, &a1) == mem); /* empty ring restarts at 0: full capacity usable */
}

/* Random push/pop (model check): live blocks never overlap or get corrupted. */
static void test_ring_random(void)
{
    enum { CAP = 256, LIVE = 64 };
    uint8_t mem[CAP];
    struct {
        uint8_t* p;
        uint32_t n, adv;
        uint8_t id;
    } q[LIVE];
    unsigned head = 0, count = 0;
    unsigned long seed = 12345;
    int bad = 0;
    dx_ring r;
    dx_ring_init(&r, mem, CAP);

    for (int i = 0; i < 200000; i++) {
        seed = seed * 6364136223846793005ul + 1442695040888963407ul;
        unsigned op = (unsigned)(seed >> 33) % 3;
        if (op < 2 && count < LIVE) { /* push (twice as likely) */
            uint32_t n = (uint32_t)(seed >> 40) % 40, adv;
            uint8_t* p = dx_ring_push(&r, n, &adv);
            if (p) {
                unsigned slot = (head + count++) % LIVE;
                q[slot].p = p;
                q[slot].n = n;
                q[slot].adv = adv;
                q[slot].id = (uint8_t)i;
                fill(p, n, q[slot].id);
            }
        } else if (count) { /* pop the oldest after checking its bytes are untouched */
            bad += !intact(q[head].p, q[head].n, q[head].id);
            dx_ring_pop(&r, q[head].adv);
            head = (head + 1) % LIVE;
            count--;
        }
        for (unsigned k = 0; k < count; k += 7) { /* spot-check other live blocks */
            unsigned s = (head + k) % LIVE;
            bad += !intact(q[s].p, q[s].n, q[s].id);
        }
    }
    CHECK(bad == 0);
}

/* Pool: exhaust, free, reuse; slots do not overlap. */
static void test_pool(void)
{
    uint32_t mem[8 * 4]; /* 8 slots of 16 bytes, 4-byte aligned */
    dx_pool p;
    uint32_t idx[8];
    dx_pool_init(&p, (uint8_t*)mem, 16, 8);

    for (int i = 0; i < 8; i++) {
        idx[i] = dx_pool_get(&p);
        CHECK(idx[i] != DX_POOL_NONE);
        fill(dx_pool_at(&p, idx[i]), 16, (uint8_t)(i + 1));
    }
    CHECK(p.in_use == 8 && dx_pool_get(&p) == DX_POOL_NONE); /* exhausted */
    for (int i = 0; i < 8; i++)
        CHECK(intact(dx_pool_at(&p, idx[i]), 16, (uint8_t)(i + 1)));

    dx_pool_put(&p, idx[3]);
    CHECK(p.in_use == 7 && dx_pool_get(&p) == idx[3]); /* the freed slot is reused */

    dx_pool_init(&p, (uint8_t*)mem, 16, 0); /* empty pool */
    CHECK(dx_pool_get(&p) == DX_POOL_NONE);
}

int main(void)
{
    test_ring_basic();
    test_ring_wrap();
    test_ring_random();
    test_pool();
    return dx_test_done();
}
