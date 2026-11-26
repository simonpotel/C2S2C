/* string.h - the three memory functions the codec needs, for the WebAssembly build.
 * SPDX-License-Identifier: MIT */
#ifndef C2S2C_WASM_STRING_H
#define C2S2C_WASM_STRING_H

#include <stddef.h>

/* There is no C library in the browser build: wasm_glue.c provides these. */
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int value, size_t n);
int memcmp(const void* a, const void* b, size_t n);

#endif
