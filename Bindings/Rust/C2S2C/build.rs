// build.rs - lets the binaries and tests of this crate find libc2s2c at run time.
// SPDX-License-Identifier: MIT
fn main() {
    // DEP_C2S2C_LIB_DIR is published by the c2s2c-sys build script (the folder of libc2s2c).
    if let Ok(dir) = std::env::var("DEP_C2S2C_LIB_DIR") {
        if cfg!(unix) {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
    }
}
