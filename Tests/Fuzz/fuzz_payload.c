/* fuzz_payload.c - libFuzzer target: header decode + payload validation on arbitrary bytes.
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>

#include "C2S2C/c2s2c.h"

/*
 * What this does: libFuzzer feeds mutated byte strings here, steered by code coverage, to
 * find inputs that crash the parser. Any out-of-bounds access, undefined behaviour or leak is
 * reported by the sanitizers it is built with. Returning normally means "no problem found".
 * The schema below uses every type so all decoder paths are reachable.
 */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const c2s2c_field fields[] = {{C2S2C_T_U8, 0},    {C2S2C_T_I16, 0},
                                         {C2S2C_T_U32, 0},   {C2S2C_T_F64, 0},
                                         {C2S2C_T_BOOL, 0},  {C2S2C_T_STRING, 0},
                                         {C2S2C_T_BYTES, 0}, {C2S2C_T_ARRAY, C2S2C_T_STRING}};
    static const c2s2c_schema schema = {8, fields};

    c2s2c_header h;
    (void)c2s2c_header_decode(data, size, &h);
    (void)c2s2c_payload_validate_schema(&schema, data, (uint32_t)size);
    return 0;
}
