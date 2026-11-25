// build.rs - lets the binaries and tests of this crate find libduplex at run time.
// SPDX-License-Identifier: MIT
fn main() {
    // DEP_DUPLEX_LIB_DIR is published by the duplex-sys build script (the folder of libduplex).
    if let Ok(dir) = std::env::var("DEP_DUPLEX_LIB_DIR") {
        if cfg!(unix) {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
    }
}
