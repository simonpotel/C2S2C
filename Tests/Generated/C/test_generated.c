/* test_generated.c - the generated C registry must match Generated/registry.txt.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "../../check.h"
#include "c2s2c_packets.h"

/*
 * Why: Generated/registry.txt is the language-neutral listing ("0x8002 chat_message
 * u32,string,u64", one packet per line). Every language has a test that reads the same file and
 * compares it with its own generated code, so a broken generator output shows up per language.
 * Usage: test_generated <path to registry.txt>
 */
static const char* const type_names[] = {"",    "u8",  "u16", "u32", "u64",  "i8",     "i16",
                                         "i32", "i64", "f32", "f64", "bool", "string", "bytes"};

/* Writes the schema as text, e.g. "u32,string,array<u16>". */
static void schema_text(const c2s2c_schema* s, char* out, size_t cap)
{
    out[0] = 0;
    for (uint16_t i = 0; i < s->count; i++) {
        const c2s2c_field* f = &s->fields[i];
        if (i) strncat(out, ",", cap - strlen(out) - 1);
        if (f->type == C2S2C_T_ARRAY) {
            strncat(out, "array<", cap - strlen(out) - 1);
            strncat(out, type_names[f->elem], cap - strlen(out) - 1);
            strncat(out, ">", cap - strlen(out) - 1);
        } else {
            strncat(out, type_names[f->type], cap - strlen(out) - 1);
        }
    }
}

int main(int argc, char** argv)
{
    FILE* f = argc == 2 ? fopen(argv[1], "r") : NULL;
    if (!f) return 2;

    char line[256];
    unsigned lines = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned id;
        char name[64], schema[128] = "", got[128];
        int fields = sscanf(line, "%x %63s %127s", &id, name, schema);
        CHECK(fields >= 2);

        /* Find the entry with that id and compare name and schema text. */
        const c2s2c_packet_info* p = NULL;
        for (unsigned i = 0; i < c2s2c_registry_count; i++)
            if (c2s2c_registry[i].id == id) p = &c2s2c_registry[i];
        CHECK(p != NULL);
        if (!p) continue;
        schema_text(&p->schema, got, sizeof got);
        CHECK(strcmp(p->name, name) == 0 && strcmp(got, schema) == 0);
        lines++;
    }
    fclose(f);
    CHECK(lines == c2s2c_registry_count); /* nothing missing, nothing extra */
    return c2s2c_test_done();
}
