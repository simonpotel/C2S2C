/* dx_version.h - ABI and protocol versions.
 * SPDX-License-Identifier: MIT */
#ifndef DX_VERSION_H
#define DX_VERSION_H

/* Two independent versions: the ABI version changes when the C interface changes (bindings
 * check it at load time); the protocol version changes when the bytes on the wire change. */

/** ABI version: (major << 16) | (minor << 8) | patch. Bump on any public layout change. */
#define DX_ABI_VERSION 0x00020000u
#define DX_PROTOCOL_VERSION 1

#endif
