/* test_registry.c - unit tests: packet lookup and payload validation by packet id.
 * SPDX-License-Identifier: MIT */
#include <string.h>

#include "../check.h"
#include "C2S2C/c2s2c.h"
#include "c2s2c_packets.h"

/* The generated table must be strictly sorted (binary search relies on it) with unique names. */
static void test_table_shape(void)
{
    CHECK(c2s2c_registry_size > 0);
    for (unsigned i = 1; i < c2s2c_registry_size; i++) {
        CHECK(c2s2c_registry[i - 1].id < c2s2c_registry[i].id);
        for (unsigned j = 0; j < i; j++)
            CHECK(strcmp(c2s2c_registry[i].name, c2s2c_registry[j].name) != 0);
    }
    /* Direction is encoded in the id: every id's bit 15 must be set only for server packets. */
    CHECK((C2S2C_PKT_LOGIN & C2S2C_ID_STOC_BIT) == 0 &&
          (C2S2C_PKT_LOGIN_OK & C2S2C_ID_STOC_BIT) != 0);
}

/* Every registered id is found by name; unknown ids (just around the known ones) are not. */
static void test_lookup(void)
{
    for (unsigned i = 0; i < c2s2c_registry_size; i++)
        CHECK(strcmp(c2s2c_packet_name(c2s2c_registry[i].id), c2s2c_registry[i].name) == 0);
    CHECK(strcmp(c2s2c_packet_name(C2S2C_PKT_CHAT_MESSAGE), "chat_message") == 0);
    CHECK(c2s2c_packet_name(0x0000) == NULL && c2s2c_packet_name(0x0006) == NULL);
    CHECK(c2s2c_packet_name(0x8006) == NULL && c2s2c_packet_name(0xFFFF) == NULL);
}

/* Validation by id: right layout passes, wrong layout and unknown id are rejected. */
static void test_validate(void)
{
    uint8_t buf[64];
    c2s2c_writer w;

    c2s2c_writer_init(&w, buf, sizeof buf); /* chat_message = u32, string, u64 */
    c2s2c_w_u32(&w, 7);
    c2s2c_w_string(&w, "hi", 2);
    c2s2c_w_u64(&w, 1234);
    CHECK(c2s2c_payload_validate(C2S2C_PKT_CHAT_MESSAGE, buf, w.len) == C2S2C_OK);
    CHECK(c2s2c_payload_validate(C2S2C_PKT_CHAT_MESSAGE, buf, w.len - 1) == C2S2C_ERR_TRUNCATED);
    CHECK(c2s2c_payload_validate(C2S2C_PKT_PONG, buf, w.len) ==
          C2S2C_ERR_TRAILING); /* other layout */
    CHECK(c2s2c_payload_validate(0x1234, buf, w.len) == C2S2C_ERR_UNKNOWN_ID);

    c2s2c_writer_init(&w, buf, sizeof buf); /* user_list = array<string> */
    c2s2c_w_u16(&w, 2);
    c2s2c_w_string(&w, "ann", 3);
    c2s2c_w_string(&w, "bob", 3);
    CHECK(c2s2c_payload_validate(C2S2C_PKT_USER_LIST, buf, w.len) == C2S2C_OK);
}

int main(void)
{
    test_table_shape();
    test_lookup();
    test_validate();
    return c2s2c_test_done();
}
