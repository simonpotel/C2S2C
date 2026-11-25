/* utf8.c - strict UTF-8 validation.
 * SPDX-License-Identifier: MIT */
#include "utf8.h"

/*
 * Why validate: strings come from the network. Accepting malformed UTF-8 lets attackers hide
 * data from filters (overlong forms such as a 2-byte "/") and breaks languages that assume
 * valid text (Python, Rust, JS). So we accept exactly what RFC 3629 allows:
 *   - no bytes 0xC0, 0xC1, 0xF5..0xFF (they can never start a valid sequence);
 *   - no overlong encodings, no surrogates (U+D800..DFFF), nothing above U+10FFFF.
 * The rules are encoded as "how many continuation bytes follow" and "allowed range of the
 * FIRST continuation byte" (the other continuations are always 0x80..0xBF).
 */
int dx_utf8_valid(const uint8_t* s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t b = s[i];
        if (b < 0x80) { /* plain ASCII */
            i++;
            continue;
        }

        /* Work out the sequence length and the legal range of the 2nd byte. */
        size_t need;
        uint8_t lo = 0x80, hi = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            need = 1;
        } else if (b >= 0xE0 && b <= 0xEF) {
            need = 2;
            if (b == 0xE0) lo = 0xA0; /* forbid overlong 3-byte */
            if (b == 0xED) hi = 0x9F; /* forbid surrogates */
        } else if (b >= 0xF0 && b <= 0xF4) {
            need = 3;
            if (b == 0xF0) lo = 0x90; /* forbid overlong 4-byte */
            if (b == 0xF4) hi = 0x8F; /* forbid > U+10FFFF */
        } else {
            return 0;
        }

        /* The whole sequence must fit in the input. */
        if (n - i <= need) return 0;
        if (s[i + 1] < lo || s[i + 1] > hi) return 0;
        for (size_t k = 2; k <= need; k++)
            if ((s[i + k] & 0xC0) != 0x80) return 0;
        i += need + 1;
    }
    return 1;
}
