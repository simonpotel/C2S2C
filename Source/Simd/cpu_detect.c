/* cpu_detect.c - which CPU features are available, and the public c2s2c_crc32c entry point.
 * SPDX-License-Identifier: MIT */
#include <stdatomic.h>

#include "C2S2C/c2s2c.h"
#include "c2s2c_simd.h"

#if defined(__x86_64__)
#include <cpuid.h>
#elif defined(__aarch64__) && defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

/*
 * The checksum has one portable version and, on CPUs that support it, a much
 * faster hardware version. We ask the CPU once what it supports (at run time, so one binary
 * works everywhere) and remember the best function in `chosen`. Two threads asking at the same
 * time just compute the same answer twice, which is harmless, so a plain atomic pointer is enough.
 */
int c2s2c_cpu_has_crc32c(void)
{
#if defined(__x86_64__)
    unsigned eax, ebx, ecx, edx;
    return __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1u << 20)); /* SSE4.2 */
#elif defined(__aarch64__) && defined(__APPLE__)
    int has = 0;
    size_t size = sizeof has;
    return sysctlbyname("hw.optional.armv8_crc32", &has, &size, NULL, 0) == 0 && has;
#elif defined(__aarch64__) && defined(__linux__)
    return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#else
    return 0;
#endif
}

static _Atomic(c2s2c_crc32c_fn) chosen;

uint32_t c2s2c_crc32c(uint32_t crc, const uint8_t* data, size_t len)
{
    c2s2c_crc32c_fn fn = atomic_load_explicit(&chosen, memory_order_relaxed);
    if (!fn) {
        fn = c2s2c_cpu_has_crc32c() ? c2s2c_crc32c_hardware() : NULL;
        if (!fn) fn = c2s2c_crc32c_generic;
        atomic_store_explicit(&chosen, fn, memory_order_relaxed);
    }
    return fn(crc, data, len);
}
