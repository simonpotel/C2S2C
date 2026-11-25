// bench_ffi.cpp - cost of the C++ binding: pipelined ping/pong round trips per second.
// SPDX-License-Identifier: MIT
#include <chrono>
#include <cstdio>

#include "dx_packets.hpp"
#include <duplex.hpp>

int main()
{
    namespace pkt = duplex::pkt;
    duplex::Context server, client;
    server.listen("127.0.0.1");
    uint32_t conn = client.connect("127.0.0.1", static_cast<uint16_t>(server.port()));
    server.on_packet(pkt::PING, [](duplex::Context& c, const duplex::Packet& p) {
        c.send(p.conn(), pkt::PONG, p.data(), p.size());
    });
    uint64_t pongs = 0;
    client.on_packet(pkt::PONG, [&](duplex::Context&, const duplex::Packet&) { pongs++; });
    uint8_t payload[4];
    duplex::Writer(payload, 4).u32(1);
    uint64_t inflight = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1)) {
        while (inflight < 32 && client.try_send(conn, pkt::PING, payload, 4))
            inflight++;
        server.dispatch(0);
        uint64_t before = pongs;
        client.dispatch(0);
        inflight -= pongs - before;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("cpp_roundtrips %.0f packets/s\n", static_cast<double>(pongs) / secs);
}
