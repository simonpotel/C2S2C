/* test_registry.c - unit tests: packet lookup and payload validation by packet id.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "../check.h"
#include "Duplex/duplex.h"
#include "dx_packets.h"

/* The generated table must be strictly sorted (binary search relies on it) with unique names. */
static void test_table_shape(void)
{
    CHECK(dx_registry_count > 0);
    for (unsigned i = 1; i < dx_registry_count; i++) {
        CHECK(dx_registry[i - 1].id < dx_registry[i].id);
        for (unsigned j = 0; j < i; j++)
            CHECK(strcmp(dx_registry[i].name, dx_registry[j].name) != 0);
    }
    /* Direction is encoded in the id: every id's bit 15 must be set only for server packets. */
    CHECK((DX_PKT_LOGIN & DX_ID_STOC_BIT) == 0 && (DX_PKT_LOGIN_OK & DX_ID_STOC_BIT) != 0);
}

/* Every registered id is found by name; unknown ids (just around the known ones) are not. */
static void test_lookup(void)
{
    for (unsigned i = 0; i < dx_registry_count; i++)
        CHECK(strcmp(dx_packet_name(dx_registry[i].id), dx_registry[i].name) == 0);
    CHECK(strcmp(dx_packet_name(DX_PKT_CHAT_MESSAGE), "chat_message") == 0);
    CHECK(dx_packet_name(0x0000) == NULL && dx_packet_name(0x0006) == NULL);
    CHECK(dx_packet_name(0x8006) == NULL && dx_packet_name(0xFFFF) == NULL);
}

/* Validation by id: right layout passes, wrong layout and unknown id are rejected. */
static void test_validate(void)
{
    uint8_t buf[64];
    dx_writer w;

    dx_writer_init(&w, buf, sizeof buf); /* chat_message = u32, string, u64 */
    dx_w_u32(&w, 7);
    dx_w_string(&w, "hi", 2);
    dx_w_u64(&w, 1234);
    CHECK(dx_payload_validate(DX_PKT_CHAT_MESSAGE, buf, w.len) == DX_OK);
    CHECK(dx_payload_validate(DX_PKT_CHAT_MESSAGE, buf, w.len - 1) == DX_ERR_TRUNCATED);
    CHECK(dx_payload_validate(DX_PKT_PONG, buf, w.len) == DX_ERR_TRAILING); /* other layout */
    CHECK(dx_payload_validate(0x1234, buf, w.len) == DX_ERR_UNKNOWN_ID);

    dx_writer_init(&w, buf, sizeof buf); /* user_list = array<string> */
    dx_w_u16(&w, 2);
    dx_w_string(&w, "ann", 3);
    dx_w_string(&w, "bob", 3);
    CHECK(dx_payload_validate(DX_PKT_USER_LIST, buf, w.len) == DX_OK);
}

int main(void)
{
    test_table_shape();
    test_lookup();
    test_validate();
    return dx_test_done();
}
