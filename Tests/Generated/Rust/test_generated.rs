// test_generated.rs - the generated Rust registry must match Generated/registry.txt.
// SPDX-License-Identifier: MIT
//
// Why: see Tests/Generated/C/test_generated.c. Usage: test_generated <path to registry.txt>
#[path = "../../../Generated/Rust/registry.rs"]
mod registry;

fn main() {
    let path = std::env::args().nth(1).expect("path to registry.txt");
    let text = std::fs::read_to_string(path).unwrap();
    let (mut pass, mut fail, mut lines) = (0, 0, 0);
    for line in text.lines().filter(|l| !l.is_empty()) {
        let mut it = line.split_whitespace();
        let id = u16::from_str_radix(it.next().unwrap().trim_start_matches("0x"), 16).unwrap();
        let (name, schema) = (it.next().unwrap(), it.next().unwrap_or(""));
        let found = registry::PACKETS.iter().any(|p| p.0 == id && p.1 == name && p.2 == schema);
        if found { pass += 1 } else { fail += 1 }
        lines += 1;
    }
    // The table must have exactly as many entries as the listing.
    if lines == registry::PACKETS.len() { pass += 1 } else { fail += 1 }
    println!("{pass} passed, {fail} failed");
    std::process::exit(if fail == 0 { 0 } else { 1 });
}
