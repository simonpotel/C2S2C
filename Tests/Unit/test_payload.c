/* test_payload.c - unit tests: payload writer/reader, schema validation, UTF-8.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "../check.h"
#include "C2S2C/c2s2c.h"
#include "Codec/utf8.h"

#define N(a) ((uint16_t)(sizeof(a) / sizeof((a)[0])))

/* One field of every kind, so a single payload exercises every type. */
static const c2s2c_field all_fields[] = {{C2S2C_T_U8, 0},    {C2S2C_T_U16, 0},
                                         {C2S2C_T_U32, 0},   {C2S2C_T_U64, 0},
                                         {C2S2C_T_F32, 0},   {C2S2C_T_F64, 0},
                                         {C2S2C_T_BOOL, 0},  {C2S2C_T_STRING, 0},
                                         {C2S2C_T_BYTES, 0}, {C2S2C_T_ARRAY, C2S2C_T_U16}};
static const c2s2c_schema all = {N(all_fields), all_fields};

/* Writes a valid payload for `all`; returns its length. */
static uint32_t make_all(uint8_t* buf, uint32_t cap)
{
    c2s2c_writer w;
    c2s2c_writer_init(&w, buf, cap);
    c2s2c_w_u8(&w, 0xAB);
    c2s2c_w_u16(&w, 0xBEEF);
    c2s2c_w_u32(&w, 0xDEADBEEFu);
    c2s2c_w_u64(&w, 0x0123456789ABCDEFull);
    c2s2c_w_f32(&w, 1.5f);
    c2s2c_w_f64(&w, -2.25);
    c2s2c_w_u8(&w, 1); /* bool */
    c2s2c_w_string(&w, "h\xC3\xA9llo \xE2\x82\xAC", 10);
    c2s2c_w_bytes(&w, (const uint8_t*)"\x01\x02\x03", 3);
    c2s2c_w_u16(&w, 3); /* array count */
    c2s2c_w_u16(&w, 10);
    c2s2c_w_u16(&w, 20);
    c2s2c_w_u16(&w, 30);
    return w.err ? 0 : w.len;
}

/* Values written must come back identical, field by field, and validate. */
static void test_roundtrip(void)
{
    uint8_t buf[256];
    uint32_t len = make_all(buf, sizeof buf);
    CHECK(len == 1 + 2 + 4 + 8 + 4 + 8 + 1 + (2 + 10) + (4 + 3) + (2 + 6));
    CHECK(c2s2c_payload_validate_schema(&all, buf, len) == C2S2C_OK);

    c2s2c_reader r;
    c2s2c_reader_init(&r, buf, len);
    uint16_t slen;
    uint32_t blen;
    CHECK(c2s2c_r_u8(&r) == 0xAB && c2s2c_r_u16(&r) == 0xBEEF && c2s2c_r_u32(&r) == 0xDEADBEEFu);
    CHECK(c2s2c_r_u64(&r) == 0x0123456789ABCDEFull && c2s2c_r_f32(&r) == 1.5f &&
          c2s2c_r_f64(&r) == -2.25);
    CHECK(c2s2c_r_u8(&r) == 1);
    const char* s = c2s2c_r_string(&r, &slen);
    CHECK(slen == 10 && memcmp(s, "h\xC3\xA9llo \xE2\x82\xAC", 10) == 0);
    const uint8_t* b = c2s2c_r_bytes(&r, &blen);
    CHECK(blen == 3 && b[2] == 3);
    CHECK(c2s2c_r_u16(&r) == 3 && c2s2c_r_u16(&r) == 10 && c2s2c_r_u16(&r) == 20 &&
          c2s2c_r_u16(&r) == 30);
    CHECK(r.err == 0 && r.pos == len);

    /* Exact wire bytes of the first fields: little-endian, no padding. */
    CHECK(buf[0] == 0xAB && buf[1] == 0xEF && buf[2] == 0xBE && buf[3] == 0xEF && buf[6] == 0xDE);
}

/* Every shorter prefix is truncated; one extra byte is trailing data. */
static void test_length_errors(void)
{
    uint8_t buf[256];
    uint32_t len = make_all(buf, sizeof buf);
    for (uint32_t n = 0; n < len; n++)
        CHECK(c2s2c_payload_validate_schema(&all, buf, n) == C2S2C_ERR_TRUNCATED);
    buf[len] = 0;
    CHECK(c2s2c_payload_validate_schema(&all, buf, len + 1) == C2S2C_ERR_TRAILING);
}

/* bool must be 0 or 1. */
static void test_bool(void)
{
    const c2s2c_field f[] = {{C2S2C_T_BOOL, 0}};
    const c2s2c_schema s = {1, f};
    uint8_t v;
    for (v = 0; v < 2; v++)
        CHECK(c2s2c_payload_validate_schema(&s, &v, 1) == C2S2C_OK);
    v = 2;
    CHECK(c2s2c_payload_validate_schema(&s, &v, 1) == C2S2C_ERR_BAD_VALUE);
}

/* Well-formed and malformed UTF-8, including the classic attack forms. */
static void test_utf8(void)
{
    static const struct {
        const char* bytes;
        size_t n;
        int valid;
    } cases[] = {
        {"", 0, 1},
        {"a", 1, 1},
        {"\xC3\xA9", 2, 1},         /* e-acute */
        {"\xE2\x82\xAC", 3, 1},     /* euro sign */
        {"\xF0\x9F\x98\x80", 4, 1}, /* emoji */
        {"\xED\x9F\xBF", 3, 1},     /* U+D7FF, last before surrogates */
        {"\xF4\x8F\xBF\xBF", 4, 1}, /* U+10FFFF, the maximum */
        {"\xC0\x80", 2, 0},         /* overlong NUL */
        {"\xC1\xBF", 2, 0},         /* overlong */
        {"\xE0\x80\x80", 3, 0},     /* overlong 3 bytes */
        {"\xF0\x80\x80\x80", 4, 0}, /* overlong 4 bytes */
        {"\xED\xA0\x80", 3, 0},     /* surrogate U+D800 */
        {"\xF4\x90\x80\x80", 4, 0}, /* above U+10FFFF */
        {"\xF5\x80\x80\x80", 4, 0}, /* invalid lead byte */
        {"\x80", 1, 0},             /* stray continuation */
        {"\xC3", 1, 0},             /* cut sequence */
        {"\xE2\x82", 2, 0},         /* cut sequence */
        {"\xC3\x28", 2, 0},         /* bad continuation */
        {"\xE2\x28\xA1", 3, 0},     /* bad continuation */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        CHECK(c2s2c_utf8_valid((const uint8_t*)cases[i].bytes, cases[i].n) == cases[i].valid);

    /* The same rule applies when writing and when reading. */
    uint8_t buf[16];
    c2s2c_writer w;
    c2s2c_writer_init(&w, buf, sizeof buf);
    c2s2c_w_string(&w, "\xC0\x80", 2);
    CHECK(w.err == C2S2C_ERR_BAD_UTF8 && w.len == 0);

    const uint8_t wire[] = {2, 0, 0xC0, 0x80};
    c2s2c_reader r;
    uint16_t n;
    c2s2c_reader_init(&r, wire, sizeof wire);
    CHECK(c2s2c_r_string(&r, &n) == NULL && r.err == C2S2C_ERR_BAD_UTF8 && n == 0);
}

/* Writer: overflow at every capacity, sticky error, NULL arguments. */
static void test_writer(void)
{
    uint8_t big[256], small[256];
    uint32_t len = make_all(big, sizeof big);
    for (uint32_t cap = 0; cap < len; cap++)
        CHECK(make_all(small, cap) == 0);

    c2s2c_writer w;
    c2s2c_writer_init(&w, small, 3);
    c2s2c_w_u32(&w, 1); /* needs 4: overflows */
    CHECK(w.err == C2S2C_ERR_OVERFLOW && w.len == 0);
    c2s2c_w_u8(&w, 1); /* sticky: ignored although there is room */
    CHECK(w.err == C2S2C_ERR_OVERFLOW && w.len == 0);

    c2s2c_writer_init(&w, small, sizeof small);
    c2s2c_w_string(&w, NULL, 1);
    CHECK(w.err == C2S2C_ERR_INVALID_ARG);
    c2s2c_writer_init(&w, small, sizeof small);
    c2s2c_w_bytes(&w, NULL, 1);
    CHECK(w.err == C2S2C_ERR_INVALID_ARG);
    c2s2c_writer_init(&w, small, 4); /* a blob of 0xFFFFFFFF bytes must not wrap the size check */
    c2s2c_w_bytes(&w, small, 0xFFFFFFFFu);
    CHECK(w.err == C2S2C_ERR_OVERFLOW);
    c2s2c_writer_init(&w, small, sizeof small); /* empty string and blob are fine */
    c2s2c_w_string(&w, "", 0);
    c2s2c_w_bytes(&w, NULL, 0);
    CHECK(w.err == 0 && w.len == 6);
}

/* Reader: reading past the end is an error and later reads stay safe. */
static void test_reader(void)
{
    const uint8_t b[] = {1, 2, 3};
    c2s2c_reader r;
    c2s2c_reader_init(&r, b, sizeof b);
    CHECK(c2s2c_r_u32(&r) == 0 && r.err == C2S2C_ERR_TRUNCATED);
    CHECK(c2s2c_r_u8(&r) == 0 && r.pos == 0); /* sticky, nothing consumed */

    /* A blob whose declared length exceeds the data. */
    const uint8_t liar[] = {0xFF, 0xFF, 0xFF, 0xFF, 1};
    uint32_t n;
    c2s2c_reader_init(&r, liar, sizeof liar);
    CHECK(c2s2c_r_bytes(&r, &n) == NULL && n == 0 && r.err == C2S2C_ERR_TRUNCATED);
}

/* Bad schemas and bad arguments are reported as such, never as data errors. */
static void test_schema_errors(void)
{
    uint8_t x = 0;
    const c2s2c_field bad0[] = {{0, 0}}, bad99[] = {{99, 0}},
                      nested[] = {{C2S2C_T_ARRAY, C2S2C_T_ARRAY}}, noelem[] = {{C2S2C_T_ARRAY, 0}},
                      ok[] = {{C2S2C_T_U8, 0}};
    CHECK(c2s2c_payload_validate_schema(&(c2s2c_schema){1, bad0}, &x, 1) == C2S2C_ERR_BAD_SCHEMA);
    CHECK(c2s2c_payload_validate_schema(&(c2s2c_schema){1, bad99}, &x, 1) == C2S2C_ERR_BAD_SCHEMA);
    CHECK(c2s2c_payload_validate_schema(&(c2s2c_schema){1, nested}, &x, 1) == C2S2C_ERR_BAD_SCHEMA);
    CHECK(c2s2c_payload_validate_schema(&(c2s2c_schema){1, noelem}, &x, 1) == C2S2C_ERR_BAD_SCHEMA);
    CHECK(c2s2c_payload_validate_schema(NULL, &x, 1) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_payload_validate_schema(&(c2s2c_schema){1, NULL}, &x, 1) == C2S2C_ERR_INVALID_ARG);
    CHECK(c2s2c_payload_validate_schema(&(c2s2c_schema){1, ok}, NULL, 1) == C2S2C_ERR_INVALID_ARG);

    /* Empty schema = empty payload only. */
    const c2s2c_schema empty = {0, NULL};
    CHECK(c2s2c_payload_validate_schema(&empty, NULL, 0) == C2S2C_OK);
    CHECK(c2s2c_payload_validate_schema(&empty, &x, 1) == C2S2C_ERR_TRAILING);
}

/* Largest array count (65535) and a count that promises more than the data holds. */
static void test_array_limits(void)
{
    static uint8_t buf[2 + 65535];
    const c2s2c_field f[] = {{C2S2C_T_ARRAY, C2S2C_T_U8}};
    const c2s2c_schema s = {1, f};
    buf[0] = 0xFF;
    buf[1] = 0xFF;
    CHECK(c2s2c_payload_validate_schema(&s, buf, sizeof buf) == C2S2C_OK);
    CHECK(c2s2c_payload_validate_schema(&s, buf, sizeof buf - 1) == C2S2C_ERR_TRUNCATED);
}

int main(void)
{
    test_roundtrip();
    test_length_errors();
    test_bool();
    test_utf8();
    test_writer();
    test_reader();
    test_schema_errors();
    test_array_limits();
    return c2s2c_test_done();
}
