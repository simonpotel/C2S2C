// build.rs - tells cargo where libc2s2c is.
// SPDX-License-Identifier: MIT
//
// Set C2S2C_LIB_DIR to the folder that contains libc2s2c.so / .dylib / c2s2c.dll.
// Without it, the checkout's Build/release and Build/debug folders are tried.
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=C2S2C_LIB_DIR");
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.join("../../..");
    let mut candidates = Vec::new();
    if let Ok(dir) = std::env::var("C2S2C_LIB_DIR") {
        candidates.push(PathBuf::from(dir));
    }
    candidates.push(root.join("Build/release"));
    candidates.push(root.join("Build/debug"));

    let dir = candidates
        .into_iter()
        .find(|d| d.join("libc2s2c.dylib").exists() || d.join("libc2s2c.so").exists() || d.join("c2s2c.dll").exists())
        .expect("libc2s2c not found: build it and set C2S2C_LIB_DIR");
    let dir = dir.canonicalize().unwrap();
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=dylib=c2s2c");
    // Tell the crates that depend on this one (DEP_C2S2C_LIB_DIR), so their executables can
    // find the shared library at run time.
    println!("cargo:lib_dir={}", dir.display());
    // Same for this crate's own test binary.
    if cfg!(unix) {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir.display());
    }
}
