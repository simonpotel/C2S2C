/* test_vectors.c - checks one reference vector: <base>.bin must decode as described by <base>.json.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Duplex/duplex.h"

/*
 * What a vector is: a file of raw bytes (.bin) plus the expected outcome (.json). The same
 * pairs are meant to be reused by every language binding, so they all prove they agree with C.
 * Usage: test_vectors <path without extension>   (one ctest entry per vector, see CMake)
 */

/* Error names used in the .json files, mapped to their numeric codes. */
#define ERR(x) {#x, x}
static const struct {
    const char* name;
    int code;
} errors[] = {ERR(DX_ERR_TRUNCATED), ERR(DX_ERR_BAD_MAGIC), ERR(DX_ERR_BAD_VERSION),
              ERR(DX_ERR_BAD_FLAGS), ERR(DX_ERR_BAD_ID),    ERR(DX_ERR_TOO_LARGE)};

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

int main(int argc, char** argv)
{
    if (argc != 2) return 2;

    /* 1. Load the bytes and the expectation. */
    char path[1024];
    unsigned char bin[64], json[512];
    snprintf(path, sizeof path, "%s.bin", argv[1]);
    long len = read_file(path, bin, sizeof bin);
    snprintf(path, sizeof path, "%s.json", argv[1]);
    if (len < 0 || read_file(path, json, sizeof json) < 0) {
        puts("FAIL: cannot read vector files");
        return 1;
    }

    /* 2. Decode with the library. */
    dx_header h;
    int rc = dx_header_decode(bin, (size_t)len, &h);

    /* 3a. Expected failure: the return code must be the named error. */
    const char* err = strstr((const char*)json, "\"error\": \"");
    if (err) {
        for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++)
            if (strncmp(err + 10, errors[i].name, strlen(errors[i].name)) == 0)
                return rc == errors[i].code ? 0 : (printf("FAIL: rc=%d\n", rc), 1);
        puts("FAIL: unknown error name in json");
        return 1;
    }

    /* 3b. Expected success: every field must match the json. */
    int ok = rc == DX_OK && h.magic == json_number((const char*)json, "magic") &&
             h.version == json_number((const char*)json, "version") &&
             h.flags == json_number((const char*)json, "flags") &&
             h.packet_id == json_number((const char*)json, "packet_id") &&
             h.length == json_number((const char*)json, "length") &&
             h.sequence == json_number((const char*)json, "sequence");
    if (!ok) printf("FAIL: rc=%d or a field differs\n", rc);
    return ok ? 0 : 1;
}
