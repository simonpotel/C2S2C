/* encode.c - payload writer: appends fields to a caller-owned buffer.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "Duplex/duplex.h"
#include "endian.h"
#include "utf8.h"

/*
 * A writer is a cursor over a buffer you provide. Each dx_w_* call appends
 * one field. If something goes wrong (buffer full, bad text) the error is remembered and
 * every later call becomes a no-op, so callers write all fields and check once at the end.
 * Nothing here allocates.
 */

/* Reserves n bytes; returns where to write them, or NULL (and records why) if impossible. */
static uint8_t* w_take(dx_writer* w, uint32_t n)
{
    if (w->err) return NULL;
    if (w->cap - w->len < n) {
        w->err = DX_ERR_OVERFLOW;
        return NULL;
    }
    uint8_t* p = w->data + w->len;
    w->len += n;
    return p;
}

void dx_writer_init(dx_writer* w, uint8_t* buf, uint32_t cap)
{
    w->data = buf;
    w->cap = cap;
    w->len = 0;
    w->err = 0;
}

void dx_w_u8(dx_writer* w, uint8_t v)
{
    uint8_t* p = w_take(w, 1);
    if (p) p[0] = v;
}

void dx_w_u16(dx_writer* w, uint16_t v)
{
    uint8_t* p = w_take(w, 2);
    if (p) dx_wr16(p, v);
}

void dx_w_u32(dx_writer* w, uint32_t v)
{
    uint8_t* p = w_take(w, 4);
    if (p) dx_wr32(p, v);
}

void dx_w_u64(dx_writer* w, uint64_t v)
{
    uint8_t* p = w_take(w, 8);
    if (p) dx_wr64(p, v);
}

/* Floats are sent as their IEEE-754 bit pattern; memcpy is the defined way to get the bits. */
void dx_w_f32(dx_writer* w, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    dx_w_u32(w, bits);
}

void dx_w_f64(dx_writer* w, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    dx_w_u64(w, bits);
}

void dx_w_string(dx_writer* w, const char* s, uint16_t n)
{
    if (w->err) return;
    if (n > 0 && !s) {
        w->err = DX_ERR_INVALID_ARG;
        return;
    }
    /* Refuse to emit text that a receiver would reject. */
    if (!dx_utf8_valid((const uint8_t*)s, n)) {
        w->err = DX_ERR_BAD_UTF8;
        return;
    }
    uint8_t* p = w_take(w, (uint32_t)2 + n);
    if (!p) return;
    dx_wr16(p, n);
    if (n) memcpy(p + 2, s, n);
}

void dx_w_bytes(dx_writer* w, const uint8_t* src, uint32_t n)
{
    if (w->err) return;
    if (n > 0 && !src) {
        w->err = DX_ERR_INVALID_ARG;
        return;
    }
    if (n > w->cap - w->len || w->cap - w->len - n < 4) { /* 4 = length prefix, no overflow */
        w->err = DX_ERR_OVERFLOW;
        return;
    }
    uint8_t* p = w_take(w, 4 + n);
    dx_wr32(p, n);
    if (n) memcpy(p + 4, src, n);
}
