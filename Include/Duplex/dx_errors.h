/* dx_errors.h - error codes (0 = success, < 0 = failure).
 * SPDX-License-Identifier: MIT */
#ifndef DX_ERRORS_H
#define DX_ERRORS_H

/* Convention for the whole API: 0 = success, negative = error. Callers test `rc < 0`, and
 * dx_strerror() turns a code into text. Never renumber an existing code (ABI). */
#define DX_OK 0
#define DX_ERR_INVALID_ARG (-1)
#define DX_ERR_TRUNCATED (-2)
#define DX_ERR_BAD_MAGIC (-3)
#define DX_ERR_BAD_VERSION (-4)
#define DX_ERR_BAD_FLAGS (-5)
#define DX_ERR_BAD_ID (-6)
#define DX_ERR_TOO_LARGE (-7)
#define DX_ERR_BAD_UTF8 (-8)    /* a string is not valid UTF-8 */
#define DX_ERR_BAD_VALUE (-9)   /* a value breaks its type's rule (e.g. bool not 0/1) */
#define DX_ERR_TRAILING (-10)   /* bytes left over after the last field */
#define DX_ERR_BAD_SCHEMA (-11) /* the field description itself is invalid */
#define DX_ERR_OVERFLOW (-12)   /* writer buffer too small */
#define DX_ERR_UNKNOWN_ID (-13) /* packet id not in the registry */

#endif
