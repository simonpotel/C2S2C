/* registry.c - lookup of packets by id, and payload validation by packet id.
 * SPDX-License-Identifier: MIT */
#include "Duplex/duplex.h"
#include "dx_packets.h"

/*
 * In plain words: the table of known packets (dx_registry) is generated from
 * Registry/protocol.toml, so C, C++, Python, Rust and TypeScript all agree on ids and layouts.
 * This file only searches that table. It is sorted by id, so a binary search is enough.
 */
static int find_index(uint16_t id)
{
    unsigned lo = 0, hi = dx_registry_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (dx_registry[mid].id == id) return (int)mid;
        if (dx_registry[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

int dx_registry_index(uint16_t packet_id)
{
    return find_index(packet_id);
}

static const dx_packet_info* find_packet(uint16_t id)
{
    int i = find_index(id);
    return i < 0 ? NULL : &dx_registry[i];
}

int dx_payload_validate(uint16_t packet_id, const uint8_t* in, uint32_t len)
{
    const dx_packet_info* p = find_packet(packet_id);
    if (!p) return DX_ERR_UNKNOWN_ID; /* never guess the layout of a packet we do not know */
    return dx_payload_validate_schema(&p->schema, in, len);
}

const char* dx_packet_name(uint16_t packet_id)
{
    const dx_packet_info* p = find_packet(packet_id);
    return p ? p->name : NULL;
}
