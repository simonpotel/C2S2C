/* test_simd.c - the CRC-32C implementations: known values, chaining, hardware against portable.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "../check.h"
#include "C2S2C/c2s2c.h"
#include "c2s2c_simd.h"

/* The standard check value of CRC-32C, and a few other published ones. */
static void test_known_values(void)
{
    CHECK(c2s2c_crc32c(0, (const uint8_t*)"123456789", 9) == 0xE3069283u);
    CHECK(c2s2c_crc32c(0, NULL, 0) == 0);
    uint8_t zeros[32] = {0};
    CHECK(c2s2c_crc32c(0, zeros, 32) == 0x8A9136AAu); /* iSCSI test vector: 32 zero bytes */
    uint8_t ones[32];
    memset(ones, 0xFF, sizeof ones);
    CHECK(c2s2c_crc32c(0, ones, 32) == 0x62A8AB43u); /* iSCSI test vector: 32 bytes of 0xFF */
    CHECK(c2s2c_crc32c_generic(0, (const uint8_t*)"123456789", 9) == 0xE3069283u);
}

/* Checksumming in pieces gives the same result as in one go, wherever the cut is. */
static void test_chaining(void)
{
    uint8_t data[300];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(i * 31u + 7u);
    uint32_t whole = c2s2c_crc32c(0, data, sizeof data);
    for (size_t cut = 0; cut <= sizeof data; cut++) {
        uint32_t part = c2s2c_crc32c(c2s2c_crc32c(0, data, cut), data + cut, sizeof data - cut);
        CHECK(part == whole);
    }
}

/* Hardware must equal the portable code for every length and every alignment of the start. */
static void test_hardware_equals_portable(void)
{
    c2s2c_crc32c_fn hw = c2s2c_cpu_has_crc32c() ? c2s2c_crc32c_hardware() : NULL;
    if (!hw) {
        printf("no hardware CRC-32C on this CPU: only the portable code is tested\n");
        return;
    }
    uint8_t buf[600];
    uint64_t seed = 12345;
    for (size_t i = 0; i < sizeof buf; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        buf[i] = (uint8_t)(seed >> 56);
    }
    int bad = 0;
    for (size_t offset = 0; offset < 16; offset++)
        for (size_t len = 0; len + offset <= 520; len++)
            bad +=
                hw(0x1234u, buf + offset, len) != c2s2c_crc32c_generic(0x1234u, buf + offset, len);
    CHECK(bad == 0);
    CHECK(c2s2c_crc32c(0, (const uint8_t*)"123456789", 9) ==
          0xE3069283u); /* through the dispatcher */
}

int main(void)
{
    test_known_values();
    test_chaining();
    test_hardware_equals_portable();
    return c2s2c_test_done();
}
