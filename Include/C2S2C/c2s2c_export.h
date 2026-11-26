/* c2s2c_export.h - symbol visibility for the public ABI.
 * SPDX-License-Identifier: MIT */
#ifndef C2S2C_EXPORT_H
#define C2S2C_EXPORT_H

/* The library is built with -fvisibility=hidden, so only functions marked C2S2C_API are exported.
 * This keeps the ABI surface small and deliberate (checked by check_abi.py). */
#if defined(_WIN32)
#if defined(C2S2C_BUILDING_SHARED)
#define C2S2C_API __declspec(dllexport)
#elif defined(C2S2C_USING_SHARED)
#define C2S2C_API __declspec(dllimport)
#else
#define C2S2C_API
#endif
#else
#define C2S2C_API __attribute__((visibility("default")))
#endif

#endif
