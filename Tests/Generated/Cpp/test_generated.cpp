// test_generated.cpp - the generated C++ registry must match Generated/registry.txt.
// SPDX-License-Identifier: MIT
//
// Why: see Tests/Generated/C/test_generated.c. Usage: test_generated <path to registry.txt>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "dx_packets.hpp"

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    std::ifstream in(argv[1]);
    int pass = 0, fail = 0;
    unsigned lines = 0;
    std::string id, name, schema;
    while (in >> id >> name >> schema) {
        bool found = false;
        for (const auto& p : duplex::pkt::all)
            if (p.id == std::stoul(id, nullptr, 16) && name == p.name && schema == p.schema)
                found = true;
        (found ? pass : fail)++;
        lines++;
    }
    // The table must have exactly as many entries as the listing.
    (lines == sizeof(duplex::pkt::all) / sizeof(duplex::pkt::all[0]) ? pass : fail)++;
    std::printf("%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
