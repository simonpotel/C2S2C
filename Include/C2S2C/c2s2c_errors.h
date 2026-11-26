/* c2s2c_errors.h - error codes (0 = success, < 0 = failure).
 * SPDX-License-Identifier: MIT */
#ifndef C2S2C_ERRORS_H
#define C2S2C_ERRORS_H

/* Convention for the whole API: 0 = success, negative = error. Callers test `rc < 0`, and
 * c2s2c_strerror() turns a code into text. Never renumber an existing code (ABI). */
#define C2S2C_OK 0
#define C2S2C_ERR_INVALID_ARG (-1)
#define C2S2C_ERR_TRUNCATED (-2)
#define C2S2C_ERR_BAD_MAGIC (-3)
#define C2S2C_ERR_BAD_VERSION (-4)
#define C2S2C_ERR_BAD_FLAGS (-5)
#define C2S2C_ERR_BAD_ID (-6)
#define C2S2C_ERR_TOO_LARGE (-7)
#define C2S2C_ERR_BAD_UTF8 (-8)       /* a string is not valid UTF-8 */
#define C2S2C_ERR_BAD_VALUE (-9)      /* a value breaks its type's rule (e.g. bool not 0/1) */
#define C2S2C_ERR_TRAILING (-10)      /* bytes left over after the last field */
#define C2S2C_ERR_BAD_SCHEMA (-11)    /* the field description itself is invalid */
#define C2S2C_ERR_OVERFLOW (-12)      /* writer buffer too small */
#define C2S2C_ERR_UNKNOWN_ID (-13)    /* packet id not in the registry */
#define C2S2C_ERR_WOULD_BLOCK (-14)   /* send buffer full: poll, then retry */
#define C2S2C_ERR_IO (-15)            /* socket error */
#define C2S2C_ERR_BAD_SEQUENCE (-16)  /* packet sequence number is not the expected one */
#define C2S2C_ERR_BAD_DIRECTION (-17) /* packet travels the wrong way (CTOS vs STOC) */
#define C2S2C_ERR_NO_MEMORY (-18)     /* allocation failed */
#define C2S2C_ERR_LIMIT (-19)         /* a configured limit is reached (connections, listener) */
#define C2S2C_ERR_BAD_CONN (-20)      /* unknown, closed or stale connection handle */
#define C2S2C_ERR_TIMEOUT (-21)       /* connection idle for too long */
#define C2S2C_ERR_REJECTED (-22)      /* a hook refused the packet */

#endif
