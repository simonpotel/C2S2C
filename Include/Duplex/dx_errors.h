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

#endif
