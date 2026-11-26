/* c2s2c_version.h - ABI and protocol versions.
 * SPDX-License-Identifier: MIT */
#ifndef C2S2C_VERSION_H
#define C2S2C_VERSION_H

/* Two independent versions: the ABI version changes when the C interface changes (bindings
 * check it at load time); the protocol version changes when the bytes on the wire change. */

/** ABI version: (major << 16) | (minor << 8) | patch. Bump on any public layout change. */
#define C2S2C_ABI_VERSION 0x00020000u
#define C2S2C_PROTOCOL_VERSION 1

#endif
