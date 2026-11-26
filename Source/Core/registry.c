/* registry.c - lookup of packets by id, and payload validation by packet id.
 * SPDX-License-Identifier: MIT */
#include "C2S2C/c2s2c.h"
#include "c2s2c_packets.h"

/*
 * The table of known packets (c2s2c_registry) is generated from
 * Registry/protocol.toml, so C, C++, Python, Rust and TypeScript all agree on ids and layouts.
 * This file only searches that table. It is sorted by id, so a binary search is enough.
 */
static int find_index(uint16_t id)
{
    unsigned lo = 0, hi = c2s2c_registry_size;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (c2s2c_registry[mid].id == id) return (int)mid;
        if (c2s2c_registry[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

static const c2s2c_packet_info* find_packet(uint16_t id)
{
    int i = find_index(id);
    return i < 0 ? NULL : &c2s2c_registry[i];
}

int c2s2c_payload_validate(uint16_t packet_id, const uint8_t* in, uint32_t len)
{
    const c2s2c_packet_info* p = find_packet(packet_id);
    if (!p) return C2S2C_ERR_UNKNOWN_ID; /* never guess the layout of a packet we do not know */
    return c2s2c_payload_validate_schema(&p->schema, in, len);
}

uint32_t c2s2c_registry_count(void)
{
    return c2s2c_registry_size;
}

int c2s2c_registry_get(uint32_t index, uint16_t* id, const char** name, const char** layout)
{
    if (index >= c2s2c_registry_size || !id || !name || !layout) return C2S2C_ERR_INVALID_ARG;
    *id = c2s2c_registry[index].id;
    *name = c2s2c_registry[index].name;
    *layout = c2s2c_registry[index].layout;
    return C2S2C_OK;
}

const char* c2s2c_packet_name(uint16_t packet_id)
{
    const c2s2c_packet_info* p = find_packet(packet_id);
    return p ? p->name : NULL;
}
