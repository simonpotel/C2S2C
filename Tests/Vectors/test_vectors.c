/* test_vectors.c - checks one reference vector: <base>.bin against the outcome in <base>.json.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Duplex/duplex.h"

/*
 * What a vector is: a file of raw bytes (.bin) plus the expected outcome (.json). The same
 * pairs are meant to be reused by every language binding, so they all prove they agree with C.
 *   header_*.json : {"error": "DX_ERR_..."}  or  {"header": {"magic": ..., ...}}
 *   payload_*.json: {"schema": "u8,string,array<u16>", "error": "DX_OK" or "DX_ERR_..."}
 * Usage: test_vectors <path without extension>   (one ctest entry per vector, see CMake)
 */

/* Error names used in the .json files, mapped to their numeric codes. */
#define ERR(x) {#x, x}
static const struct {
    const char* name;
    int code;
} errors[] = {ERR(DX_OK),
              ERR(DX_ERR_TRUNCATED),
              ERR(DX_ERR_BAD_MAGIC),
              ERR(DX_ERR_BAD_VERSION),
              ERR(DX_ERR_BAD_FLAGS),
              ERR(DX_ERR_BAD_ID),
              ERR(DX_ERR_TOO_LARGE),
              ERR(DX_ERR_BAD_UTF8),
              ERR(DX_ERR_BAD_VALUE),
              ERR(DX_ERR_TRAILING)};

/* Type names used in "schema", mapped to DX_T_* codes. */
static const struct {
    const char* name;
    uint8_t type;
} types[] = {{"u8", DX_T_U8},      {"u16", DX_T_U16}, {"u32", DX_T_U32},   {"u64", DX_T_U64},
             {"i8", DX_T_I8},      {"i16", DX_T_I16}, {"i32", DX_T_I32},   {"i64", DX_T_I64},
             {"f32", DX_T_F32},    {"f64", DX_T_F64}, {"bool", DX_T_BOOL}, {"string", DX_T_STRING},
             {"bytes", DX_T_BYTES}};

/* Reads a whole file into buf (NUL-terminated). Returns the size, or -1 on failure. */
static long read_file(const char* path, unsigned char* buf, size_t cap)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (long)n;
}

/* Finds `"key": <number>` in the flat JSON text and returns the number. */
static unsigned long json_number(const char* json, const char* key)
{
    char pattern[32];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char* p = strstr(json, pattern);
    return p ? strtoul(p + strlen(pattern), NULL, 10) : (unsigned long)-1;
}

/* Looks a type name up; returns its DX_T_* code or 0. Stops at n characters. */
static uint8_t type_of(const char* name, size_t n)
{
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strlen(types[i].name) == n && strncmp(types[i].name, name, n) == 0)
            return types[i].type;
    return 0;
}

/* Parses "u8,string,array<u16>" into fields. Returns the field count, or -1 if malformed. */
static int parse_schema(const char* s, dx_field* out, int max)
{
    int n = 0;
    while (*s && *s != '"') {
        if (n == max) return -1;
        size_t len = strcspn(s, ",\"");
        if (strncmp(s, "array<", 6) == 0) {
            out[n].type = DX_T_ARRAY;
            out[n].elem = type_of(s + 6, len - 7); /* strip "array<" and ">" */
        } else {
            out[n].type = type_of(s, len);
            out[n].elem = 0;
        }
        n++;
        s += len + (s[len] == ',');
    }
    return n;
}

/* Header vector: decode and compare either the error code or every field. */
static int check_header(const unsigned char* bin, long len, const char* json)
{
    dx_header h;
    int rc = dx_header_decode(bin, (size_t)len, &h);
    const char* err = strstr(json, "\"error\": \"");
    if (err) {
        for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++)
            if (strncmp(err + 10, errors[i].name, strlen(errors[i].name)) == 0)
                return rc == errors[i].code;
        return 0;
    }
    return rc == DX_OK && h.magic == json_number(json, "magic") &&
           h.version == json_number(json, "version") && h.flags == json_number(json, "flags") &&
           h.packet_id == json_number(json, "packet_id") &&
           h.length == json_number(json, "length") && h.sequence == json_number(json, "sequence");
}

/* Payload vector: validate against the json schema and compare the error code. */
static int check_payload(const unsigned char* bin, long len, const char* json)
{
    dx_field fields[8];
    const char* schema = strstr(json, "\"schema\": \"");
    const char* err = strstr(json, "\"error\": \"");
    if (!schema || !err) return 0;
    int n = parse_schema(schema + 11, fields, 8);
    if (n < 0) return 0;
    dx_schema s = {(uint16_t)n, fields};
    int rc = dx_payload_validate_schema(&s, bin, (uint32_t)len);
    for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++)
        if (strncmp(err + 10, errors[i].name, strlen(errors[i].name)) == 0 &&
            err[10 + strlen(errors[i].name)] == '"')
            return rc == errors[i].code;
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;

    /* Load the bytes and the expectation. */
    char path[1024];
    unsigned char bin[256], json[512];
    snprintf(path, sizeof path, "%s.bin", argv[1]);
    long len = read_file(path, bin, sizeof bin);
    snprintf(path, sizeof path, "%s.json", argv[1]);
    if (len < 0 || read_file(path, json, sizeof json) < 0) {
        puts("FAIL: cannot read vector files");
        return 1;
    }

    /* A "schema" key means payload vector, otherwise header vector. */
    int ok = strstr((const char*)json, "\"schema\"") ? check_payload(bin, len, (const char*)json)
                                                     : check_header(bin, len, (const char*)json);
    if (!ok) puts("FAIL: result differs from the vector");
    return ok ? 0 : 1;
}
