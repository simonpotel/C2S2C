/* test_property.c - randomized round trips: write -> validate -> read must give back the input.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "../check.h"
#include "Duplex/duplex.h"
#include "dx_packets.h"

/*
 * Why: hand-written cases only cover what we thought of. Here every type is exercised with
 * 100 000 random values each. The seed is printed so any failure can be replayed exactly:
 *   DX_SEED=<number> ./test_property
 * By default the seed is fixed, so runs are deterministic (no flaky CI).
 */
#define ITERATIONS 100000

static uint64_t rng_state;

/* xorshift64*: tiny, fast, good enough for test data. */
static uint64_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}

/* Encodes a random Unicode scalar value (never a surrogate) as UTF-8; returns bytes written. */
static size_t rnd_utf8_char(uint8_t* out)
{
    static const uint32_t max_for_len[] = {0x7F, 0x7FF, 0xFFFF, 0x10FFFF};
    size_t len = rnd() % 4 + 1;
    uint32_t cp = (uint32_t)(rnd() % (max_for_len[len - 1] + 1));
    if (len == 3 && cp >= 0xD800 && cp <= 0xDFFF) cp = 0x20AC;                /* skip surrogates */
    if (len > 1 && cp <= max_for_len[len - 2]) cp = max_for_len[len - 2] + 1; /* avoid overlong */
    if (len == 1) {
        out[0] = (uint8_t)cp;
    } else if (len == 2) {
        out[0] = (uint8_t)(0xC0 | cp >> 6);
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
    } else if (len == 3) {
        out[0] = (uint8_t)(0xE0 | cp >> 12);
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
    } else {
        out[0] = (uint8_t)(0xF0 | cp >> 18);
        out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    return len;
}

/* Random valid string of up to 16 characters; returns its byte length. */
static uint16_t rnd_string(uint8_t* out)
{
    size_t n = 0, chars = rnd() % 17;
    for (size_t i = 0; i < chars; i++)
        n += rnd_utf8_char(out + n);
    return (uint16_t)n;
}

/* One scalar type: write a random value, validate with a 1-field schema, read it back. */
static void scalar_roundtrip(uint8_t type, int size)
{
    const dx_field f[] = {{type, 0}};
    const dx_schema s = {1, f};
    int bad = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t v = rnd();
        if (size < 8) v &= (1ull << (size * 8)) - 1;
        if (type == DX_T_BOOL) v &= 1;
        uint8_t buf[8];
        dx_writer w;
        dx_writer_init(&w, buf, sizeof buf);
        if (size == 1)
            dx_w_u8(&w, (uint8_t)v);
        else if (size == 2)
            dx_w_u16(&w, (uint16_t)v);
        else if (size == 4)
            dx_w_u32(&w, (uint32_t)v);
        else
            dx_w_u64(&w, v);

        dx_reader r;
        dx_reader_init(&r, buf, w.len);
        uint64_t back = size == 1   ? dx_r_u8(&r)
                        : size == 2 ? dx_r_u16(&r)
                        : size == 4 ? dx_r_u32(&r)
                                    : dx_r_u64(&r);
        bad += w.err || w.len != (uint32_t)size || r.err || back != v ||
               dx_payload_validate_schema(&s, buf, w.len) != DX_OK;
    }
    CHECK(bad == 0);
}

/* Floats: compare bit patterns so NaN and -0.0 are covered too. */
static void float_roundtrip(void)
{
    int bad = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        uint32_t b32 = (uint32_t)rnd();
        uint64_t b64 = rnd();
        float f;
        double d;
        memcpy(&f, &b32, 4);
        memcpy(&d, &b64, 8);
        uint8_t buf[12];
        dx_writer w;
        dx_writer_init(&w, buf, sizeof buf);
        dx_w_f32(&w, f);
        dx_w_f64(&w, d);
        dx_reader r;
        dx_reader_init(&r, buf, w.len);
        float f2 = dx_r_f32(&r);
        double d2 = dx_r_f64(&r);
        uint32_t c32;
        uint64_t c64;
        memcpy(&c32, &f2, 4);
        memcpy(&c64, &d2, 8);
        bad += w.err || r.err || c32 != b32 || c64 != b64;
    }
    CHECK(bad == 0);
}

/* Random strings, byte blobs, and arrays of strings. */
static void composite_roundtrip(void)
{
    const dx_field f[] = {{DX_T_STRING, 0}, {DX_T_BYTES, 0}, {DX_T_ARRAY, DX_T_STRING}};
    const dx_schema s = {3, f};
    int bad = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        uint8_t str[80], blob[40], buf[1024], tmp[80];
        uint16_t slen = rnd_string(str);
        uint32_t blen = (uint32_t)(rnd() % 40);
        for (uint32_t k = 0; k < blen; k++)
            blob[k] = (uint8_t)rnd();
        uint16_t count = (uint16_t)(rnd() % 6);

        dx_writer w;
        dx_writer_init(&w, buf, sizeof buf);
        dx_w_string(&w, (const char*)str, slen);
        dx_w_bytes(&w, blob, blen);
        dx_w_u16(&w, count);
        for (uint16_t k = 0; k < count; k++)
            dx_w_string(&w, (const char*)tmp, rnd_string(tmp));

        dx_reader r;
        dx_reader_init(&r, buf, w.len);
        uint16_t sl2;
        uint32_t bl2;
        const char* s2 = dx_r_string(&r, &sl2);
        const uint8_t* b2 = dx_r_bytes(&r, &bl2);
        bad += w.err || r.err || sl2 != slen || memcmp(s2, str, slen) != 0 || bl2 != blen ||
               (blen && memcmp(b2, blob, blen) != 0) ||
               dx_payload_validate_schema(&s, buf, w.len) != DX_OK;
    }
    CHECK(bad == 0);
}

/* Garbage in: validation must always return one of the documented codes (and never crash;
 * the sanitizer builds turn any out-of-bounds read into a failure). */
static void garbage_never_crashes(void)
{
    const dx_field f[] = {
        {DX_T_U16, 0}, {DX_T_STRING, 0}, {DX_T_BOOL, 0}, {DX_T_ARRAY, DX_T_BYTES}};
    const dx_schema s = {4, f};
    int bad = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        uint8_t buf[64];
        uint32_t len = (uint32_t)(rnd() % sizeof buf);
        for (uint32_t k = 0; k < len; k++)
            buf[k] = (uint8_t)rnd();
        int rc = dx_payload_validate_schema(&s, buf, len);
        bad += !(rc == DX_OK || (rc <= DX_ERR_TRUNCATED && rc >= DX_ERR_OVERFLOW));
    }
    CHECK(bad == 0);
}

/* Writes one random value of a non-array type. */
static void write_value(dx_writer* w, uint8_t type)
{
    uint8_t tmp[80], blob[40];
    switch (type) {
    case DX_T_U8:
    case DX_T_I8:
        dx_w_u8(w, (uint8_t)rnd());
        break;
    case DX_T_U16:
    case DX_T_I16:
        dx_w_u16(w, (uint16_t)rnd());
        break;
    case DX_T_U32:
    case DX_T_I32:
        dx_w_u32(w, (uint32_t)rnd());
        break;
    case DX_T_U64:
    case DX_T_I64:
        dx_w_u64(w, rnd());
        break;
    case DX_T_F32:
        dx_w_u32(w, (uint32_t)rnd());
        break;
    case DX_T_F64:
        dx_w_u64(w, rnd());
        break;
    case DX_T_BOOL:
        dx_w_u8(w, (uint8_t)(rnd() & 1));
        break;
    case DX_T_STRING:
        dx_w_string(w, (const char*)tmp, rnd_string(tmp));
        break;
    default: {
        uint32_t n = (uint32_t)(rnd() % sizeof blob);
        for (uint32_t k = 0; k < n; k++)
            blob[k] = (uint8_t)rnd();
        dx_w_bytes(w, blob, n);
    }
    }
}

/* Every packet of the registry: a random valid payload must validate by id, and cutting its
 * last byte must be rejected. This is what keeps each generated layout honest. */
static void registry_roundtrip(void)
{
    for (unsigned i = 0; i < dx_registry_count; i++) {
        const dx_packet_info* p = &dx_registry[i];
        int bad = 0;
        for (int it = 0; it < ITERATIONS; it++) {
            uint8_t buf[2048];
            dx_writer w;
            dx_writer_init(&w, buf, sizeof buf);
            for (uint16_t f = 0; f < p->schema.count; f++) {
                const dx_field* fld = &p->schema.fields[f];
                if (fld->type == DX_T_ARRAY) {
                    uint16_t count = (uint16_t)(rnd() % 6);
                    dx_w_u16(&w, count);
                    for (uint16_t k = 0; k < count; k++)
                        write_value(&w, fld->elem);
                } else {
                    write_value(&w, fld->type);
                }
            }
            bad += w.err || dx_payload_validate(p->id, buf, w.len) != DX_OK ||
                   (w.len > 0 && dx_payload_validate(p->id, buf, w.len - 1) == DX_OK);
        }
        if (bad) printf("packet %s failed %d times\n", p->name, bad);
        CHECK(bad == 0);
    }
}

int main(void)
{
    const char* env = getenv("DX_SEED");
    rng_state = env ? strtoull(env, NULL, 10) : 0x9E3779B97F4A7C15ull;
    if (!rng_state) rng_state = 1; /* xorshift must not start at 0 */
    printf("seed = %llu (replay with DX_SEED)\n", (unsigned long long)rng_state);

    scalar_roundtrip(DX_T_U8, 1);
    scalar_roundtrip(DX_T_I8, 1);
    scalar_roundtrip(DX_T_BOOL, 1);
    scalar_roundtrip(DX_T_U16, 2);
    scalar_roundtrip(DX_T_I16, 2);
    scalar_roundtrip(DX_T_U32, 4);
    scalar_roundtrip(DX_T_I32, 4);
    scalar_roundtrip(DX_T_U64, 8);
    scalar_roundtrip(DX_T_I64, 8);
    float_roundtrip();
    composite_roundtrip();
    garbage_never_crashes();
    registry_roundtrip();
    return dx_test_done();
}
