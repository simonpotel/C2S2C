/* decode.c - payload reader and schema validation.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "Duplex/duplex.h"
#include "endian.h"
#include "utf8.h"

/*
 * In plain words: a reader is a cursor over received bytes. Every read first checks that
 * enough bytes remain (network data is never trusted). On failure the error is remembered and
 * later reads return 0/NULL. dx_payload_validate_schema() uses the very same reader to walk a
 * packet field by field, so "valid" always means "readable with these exact functions".
 */

/* Takes n bytes; returns where they start, or NULL (recording TRUNCATED) if not enough remain. */
static const uint8_t* r_take(dx_reader* r, uint32_t n)
{
    if (r->err) return NULL;
    if (r->len - r->pos < n) {
        r->err = DX_ERR_TRUNCATED;
        return NULL;
    }
    const uint8_t* p = r->data + r->pos;
    r->pos += n;
    return p;
}

void dx_reader_init(dx_reader* r, const uint8_t* data, uint32_t len)
{
    r->data = data;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

uint8_t dx_r_u8(dx_reader* r)
{
    const uint8_t* p = r_take(r, 1);
    return p ? p[0] : 0;
}

uint16_t dx_r_u16(dx_reader* r)
{
    const uint8_t* p = r_take(r, 2);
    return p ? dx_rd16(p) : 0;
}

uint32_t dx_r_u32(dx_reader* r)
{
    const uint8_t* p = r_take(r, 4);
    return p ? dx_rd32(p) : 0;
}

uint64_t dx_r_u64(dx_reader* r)
{
    const uint8_t* p = r_take(r, 8);
    return p ? dx_rd64(p) : 0;
}

float dx_r_f32(dx_reader* r)
{
    uint32_t bits = dx_r_u32(r);
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

double dx_r_f64(dx_reader* r)
{
    uint64_t bits = dx_r_u64(r);
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

const char* dx_r_string(dx_reader* r, uint16_t* len)
{
    uint16_t n = dx_r_u16(r);
    const uint8_t* p = r_take(r, n);
    if (!p) {
        *len = 0;
        return NULL;
    }
    if (!dx_utf8_valid(p, n)) {
        r->err = DX_ERR_BAD_UTF8;
        *len = 0;
        return NULL;
    }
    *len = n;
    return (const char*)p;
}

const uint8_t* dx_r_bytes(dx_reader* r, uint32_t* len)
{
    uint32_t n = dx_r_u32(r);
    const uint8_t* p = r_take(r, n);
    *len = p ? n : 0;
    return p;
}

/* Schema validation: walks a payload with the reader above and checks every field. */

/* Byte size of the fixed-size types, indexed by DX_T_* (0 = not fixed-size). */
static const uint8_t fixed_size[] = {0, 1, 2, 4, 8, 1, 2, 4, 8, 4, 8, 1, 0, 0, 0};

/* Skips (and checks) one value of a non-array type. */
static void skip_value(dx_reader* r, int type)
{
    uint16_t slen;
    uint32_t blen;
    if (type == DX_T_STRING) {
        dx_r_string(r, &slen);
    } else if (type == DX_T_BYTES) {
        dx_r_bytes(r, &blen);
    } else if (type == DX_T_BOOL) {
        if (dx_r_u8(r) > 1 && !r->err) r->err = DX_ERR_BAD_VALUE;
    } else {
        r_take(r, fixed_size[type]);
    }
}

/* A schema type is valid if it is a known non-array type (1..13). */
static int plain_type(int t)
{
    return t >= DX_T_U8 && t <= DX_T_BYTES;
}

int dx_payload_validate_schema(const dx_schema* s, const uint8_t* in, uint32_t len)
{
    if (!s || (s->count > 0 && !s->fields) || (len > 0 && !in)) return DX_ERR_INVALID_ARG;

    /* 1. Check the schema itself first, so a bad schema is never blamed on the data. */
    for (uint16_t i = 0; i < s->count; i++) {
        const dx_field* f = &s->fields[i];
        if (!plain_type(f->type) && !(f->type == DX_T_ARRAY && plain_type(f->elem)))
            return DX_ERR_BAD_SCHEMA;
    }

    /* 2. Walk the data field by field. Arrays are a u16 count then that many elements. */
    dx_reader r;
    dx_reader_init(&r, in, len);
    for (uint16_t i = 0; i < s->count && !r.err; i++) {
        const dx_field* f = &s->fields[i];
        if (f->type == DX_T_ARRAY) {
            uint16_t count = dx_r_u16(&r);
            for (uint32_t k = 0; k < count && !r.err; k++)
                skip_value(&r, f->elem);
        } else {
            skip_value(&r, f->type);
        }
    }
    if (r.err) return r.err;

    /* 3. Exactly one payload: leftover bytes mean sender and receiver disagree on the format. */
    return r.pos == r.len ? DX_OK : DX_ERR_TRAILING;
}
