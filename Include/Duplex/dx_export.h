/* dx_export.h - symbol visibility for the public ABI.
 * SPDX-License-Identifier: MIT */
#ifndef DX_EXPORT_H
#define DX_EXPORT_H

/* The library is built with -fvisibility=hidden, so only functions marked DX_API are exported.
 * This keeps the ABI surface small and deliberate (checked by check_abi.py). */
#if defined(_WIN32)
#if defined(DX_BUILDING_SHARED)
#define DX_API __declspec(dllexport)
#elif defined(DX_USING_SHARED)
#define DX_API __declspec(dllimport)
#else
#define DX_API
#endif
#else
#define DX_API __attribute__((visibility("default")))
#endif

#endif
