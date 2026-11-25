// chat_client.cpp - the chat client (see Examples/C/Client) written with the C++ binding.
// SPDX-License-Identifier: MIT
//
// Usage: chat_client <port> <nickname> <message>...
// Logs in, sends each message, prints the broadcasts it receives until its own messages came back.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "dx_packets.hpp"
#include <duplex.hpp>

int main(int argc, char** argv)
{
    namespace pkt = duplex::pkt;
    if (argc < 4) {
        std::fprintf(stderr, "usage: chat_client <port> <nickname> <message>...\n");
        return 2;
    }
    try {
        duplex::Context ctx;
        bool logged_in = false;
        int received = 0;
        ctx.on_packet(pkt::LOGIN_OK,
                      [&](duplex::Context&, const duplex::Packet&) { logged_in = true; });
        ctx.on_packet(pkt::CHAT_MESSAGE, [&](duplex::Context&, const duplex::Packet& p) {
            auto r = p.reader();
            uint32_t id = r.u32();
            auto text = r.string();
            std::printf("[%u] %.*s\n", id, static_cast<int>(text.size()), text.data());
            std::fflush(stdout);
            received++;
        });

        uint32_t conn = ctx.connect("127.0.0.1", static_cast<uint16_t>(std::atoi(argv[1])));
        auto send_string = [&](uint16_t id, const std::string& s) {
            uint8_t buf[512];
            ctx.send(conn, id, buf, duplex::Writer(buf, sizeof buf).string(s).size());
        };
        auto wait_until = [&](auto done) { // run the loop for at most ~10 seconds
            auto t0 = std::chrono::steady_clock::now();
            while (!done() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
                ctx.dispatch(50);
            return done();
        };

        send_string(pkt::LOGIN, argv[2]);
        if (!wait_until([&] { return logged_in; })) return 1;
        for (int i = 3; i < argc; i++)
            send_string(pkt::CHAT_SEND, argv[i]);
        bool ok = wait_until([&] { return received >= argc - 3; });

        ctx.close(conn);
        for (int i = 0; i < 20; i++)
            ctx.dispatch(10); // let the close go out
        return ok ? 0 : 1;
    } catch (const duplex::Error& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
