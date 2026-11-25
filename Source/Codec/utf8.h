/* utf8.h - strict UTF-8 validation.
 * SPDX-License-Identifier: MIT */
#ifndef DX_UTF8_H
#define DX_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Returns 1 if s[0..n) is well-formed UTF-8 (RFC 3629), else 0. */
int dx_utf8_valid(const uint8_t* s, size_t n);

#endif
