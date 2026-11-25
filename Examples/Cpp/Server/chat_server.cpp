// chat_server.cpp - the chat room server (see Examples/C/Server) written with the C++ binding.
// SPDX-License-Identifier: MIT
//
// Same protocol and command line as the C version: clients log in with a nickname, send chat
// messages, and every message is broadcast to all logged-in clients. Usage: chat_server [port]
// (the chosen port is printed on the first line).
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>

#include "dx_packets.hpp"
#include <duplex.hpp>

namespace
{
std::atomic<bool> stop{false};
}

int main(int argc, char** argv)
{
    namespace pkt = duplex::pkt;
    try {
        duplex::Context ctx;
        ctx.listen("127.0.0.1", argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 0);

        std::map<uint32_t, std::string> users; // connection handle -> nickname

        ctx.on_packet(pkt::LOGIN, [&](duplex::Context& c, const duplex::Packet& p) {
            users[p.conn()] = std::string(p.reader().string());
            uint8_t buf[4];
            c.send(p.conn(), pkt::LOGIN_OK, buf,
                   duplex::Writer(buf, sizeof buf).u32(p.conn()).size());
        });
        ctx.on_packet(pkt::CHAT_SEND, [&](duplex::Context& c, const duplex::Packet& p) {
            if (!users.count(p.conn())) return; // must log in first
            uint8_t buf[512];
            auto text = p.reader().string();
            uint32_t n = duplex::Writer(buf, sizeof buf)
                             .u32(p.conn())
                             .string(text)
                             .u64(static_cast<uint64_t>(std::time(nullptr)))
                             .size();
            for (auto& user : users)
                c.send(user.first, pkt::CHAT_MESSAGE, buf, n); // broadcast
        });
        ctx.on_packet(pkt::PING, [](duplex::Context& c, const duplex::Packet& p) {
            c.send(p.conn(), pkt::PONG, p.data(), p.size()); // same nonce back
        });
        duplex::Hooks hooks;
        hooks.on_disconnect = [&](uint32_t conn, int) { users.erase(conn); };
        ctx.set_hooks(hooks);

        std::signal(SIGINT, [](int) { stop = true; });
        std::signal(SIGTERM, [](int) { stop = true; });
        std::printf("%d\n", ctx.port());
        std::fflush(stdout);
        while (!stop)
            ctx.dispatch(200);
    } catch (const duplex::Error& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
