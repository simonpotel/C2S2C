// test_cpp.cpp - the C++ binding: errors, RAII release, payload helpers, handlers and hooks.
// SPDX-License-Identifier: MIT
#include <chrono>
#include <cstring>
#include <string>

#include "../../check.h"
#include "c2s2c_packets.hpp"
#include <c2s2c.hpp>

namespace
{

using namespace c2s2c;
namespace pkt = c2s2c::pkt;

template <class F> void spin(Context& a, Context& b, F done)
{
    auto t0 = std::chrono::steady_clock::now();
    while (!done() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
        a.dispatch(1);
        b.dispatch(1);
    }
}

c2s2c_config small_config(uint32_t queue = 0, uint32_t max_payload = 0)
{
    c2s2c_config cfg;
    c2s2c_config_default(&cfg);
    cfg.max_conns = 4;
    if (queue) cfg.rx_queue = queue;
    if (max_payload) cfg.max_payload = max_payload;
    cfg.rx_ring_bytes = 1u << 20;
    cfg.tx_bytes = 0;
    return cfg;
}

// Library errors become exceptions that carry the code and its message.
void test_errors()
{
    Context ctx;
    try {
        ctx.send(12345, pkt::PING, nullptr, 0);
        CHECK(false);
    } catch (const Error& e) {
        CHECK(e.code() == C2S2C_ERR_BAD_CONN && std::string(e.what()) == "invalid connection");
    }
    c2s2c_config bad;
    c2s2c_config_default(&bad);
    bad.rx_ring_bytes = 100; // smaller than two maximal packets: refused
    try {
        Context c(&bad);
        CHECK(false);
    } catch (const Error& e) {
        CHECK(e.code() == C2S2C_ERR_NO_MEMORY);
    }
}

// Writer and Reader: round trip, and exceptions instead of silent failure.
void test_codec()
{
    uint8_t buf[64];
    Writer w(buf, sizeof buf);
    w.u32(7).string("héllo").u64(99);
    Reader r(buf, w.size());
    CHECK(r.u32() == 7 && r.string() == "héllo" && r.u64() == 99);
    try {
        r.u8();
        CHECK(false);
    } catch (const Error& e) {
        CHECK(e.code() == C2S2C_ERR_TRUNCATED);
    }
    uint8_t tiny[2];
    Writer small(tiny, sizeof tiny);
    small.u32(1);
    try {
        small.size();
        CHECK(false);
    } catch (const Error& e) {
        CHECK(e.code() == C2S2C_ERR_OVERFLOW);
    }
    Writer bad(buf, sizeof buf);
    bad.string(std::string_view("\xC0\x80", 2));
    try {
        bad.size();
        CHECK(false);
    } catch (const Error& e) {
        CHECK(e.code() == C2S2C_ERR_BAD_UTF8);
    }
}

// A Batch gives its packets back when it goes out of scope. The server's queue holds only 8
// packets, so receiving 200 pings proves they are released (otherwise reading would stall).
void test_batch_releases()
{
    auto cfg = small_config(8);
    Context server(&cfg), client(&cfg);
    server.listen("127.0.0.1");
    uint32_t conn = client.connect("127.0.0.1", static_cast<uint16_t>(server.port()));

    int got = 0, sent = 0;
    uint8_t b[4];
    auto t0 = std::chrono::steady_clock::now();
    while (got < 200 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
        if (sent < 200) {
            Writer(b, 4).u32(static_cast<uint32_t>(sent));
            try {
                if (client.try_send(conn, pkt::PING, b, 4)) sent++;
            } catch (const Error&) { // not connected yet cannot happen here, but never hide it
                CHECK(false);
            }
        }
        client.dispatch(0);
        Batch batch = server.poll(1);
        Batch moved = std::move(batch); // moving must not release twice
        for (size_t i = 0; i < moved.size(); i++)
            if (moved[i].id() == pkt::PING) got++;
    }
    CHECK(got == 200);
}

// Handlers and hooks written as lambdas, and a flow-controlled send.
void test_handlers_and_hooks()
{
    auto cfg = small_config();
    Context server(&cfg), client(&cfg);
    server.listen("127.0.0.1");

    int pings = 0, pongs = 0, connects = 0, rejected = 0, dropped = 0;
    server.on_packet(pkt::PING, [&](Context& c, const Packet& p) {
        pings++;
        c.send(p.conn(), pkt::PONG, p.data(), p.size()); // reply from inside a handler
    });
    client.on_packet(pkt::PONG, [&](Context&, const Packet&) { pongs++; });
    Hooks hooks;
    hooks.on_connect = [&](uint32_t) { connects++; };
    hooks.after_recv = [&](const Packet& p) {
        if (p.id() != pkt::CHAT_SEND) return false;
        dropped++;
        return true;
    };
    server.set_hooks(hooks);
    Hooks chook;
    chook.before_send = [&](uint32_t, uint16_t id, const uint8_t*, uint32_t) {
        if (id != pkt::LOGIN) return false;
        rejected++;
        return true;
    };
    client.set_hooks(chook);

    uint32_t conn = client.connect("127.0.0.1", static_cast<uint16_t>(server.port()));
    spin(server, client, [&] { return connects == 1; });
    CHECK(connects == 1);

    uint8_t b[16];
    uint32_t n = Writer(b, sizeof b).u32(5).size();
    client.send(conn, pkt::PING, b, n);
    n = Writer(b, sizeof b).string("hello").size();
    client.send(conn, pkt::CHAT_SEND, b, n);
    try {
        client.send(conn, pkt::LOGIN, b, n); // vetoed by the before_send hook
        CHECK(false);
    } catch (const Error& e) {
        CHECK(e.code() == C2S2C_ERR_REJECTED);
    }
    spin(server, client, [&] { return pongs == 1 && dropped == 1; });
    CHECK(pings == 1 && pongs == 1 && dropped == 1 && rejected == 1);

    // try_send reports a full buffer as false (nobody reads on the server side meanwhile).
    std::string big(1000, 'x');
    uint8_t payload[1100];
    n = Writer(payload, sizeof payload).string(big).size();
    bool full = false;
    for (int i = 0; i < 200000 && !full; i++)
        full = !client.try_send(conn, pkt::CHAT_SEND, payload, n);
    CHECK(full);
}

} // namespace

int main()
{
    test_errors();
    test_codec();
    test_batch_releases();
    test_handlers_and_hooks();
    return c2s2c_test_done();
}
