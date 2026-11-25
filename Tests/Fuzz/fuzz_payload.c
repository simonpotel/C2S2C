/* fuzz_payload.c - libFuzzer target: header decode + payload validation on arbitrary bytes.
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>

#include "Duplex/duplex.h"

/*
 * What this does: libFuzzer feeds mutated byte strings here, steered by code coverage, to
 * find inputs that crash the parser. Any out-of-bounds access, undefined behaviour or leak is
 * reported by the sanitizers it is built with. Returning normally means "no problem found".
 * The schema below uses every type so all decoder paths are reachable.
 */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const dx_field fields[] = {
        {DX_T_U8, 0},   {DX_T_I16, 0},    {DX_T_U32, 0},   {DX_T_F64, 0},
        {DX_T_BOOL, 0}, {DX_T_STRING, 0}, {DX_T_BYTES, 0}, {DX_T_ARRAY, DX_T_STRING}};
    static const dx_schema schema = {8, fields};

    dx_header h;
    (void)dx_header_decode(data, size, &h);
    (void)dx_payload_validate_schema(&schema, data, (uint32_t)size);
    return 0;
}
