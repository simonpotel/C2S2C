//! Raw declarations of the Duplex C API (`duplex.h`). Use the `duplex` crate instead of this one
//! unless you need the unsafe functions directly.
//! SPDX-License-Identifier: MIT
#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_void};

/// Opaque context.
#[repr(C)]
pub struct dx_ctx {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct dx_alloc {
    pub alloc: Option<unsafe extern "C" fn(usize, *mut c_void) -> *mut c_void>,
    pub release: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    pub user: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct dx_config {
    pub size: u32,
    pub max_conns: u32,
    pub max_payload: u32,
    pub rx_ring_bytes: u32,
    pub rx_queue: u32,
    pub tx_bytes: u32,
    pub flags: u32,
    pub idle_timeout_ms: u32,
    pub backend: u32,
    pub alloc: dx_alloc,
    pub max_tunnels: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct dx_packet {
    pub conn: u32,
    pub sequence: u32,
    pub data: *const u8,
    pub length: u32,
    pub packet_id: u16,
    pub flags: u8,
    pub reserved: u8,
}

/// Interface version this crate was written for: major 2, minor 0.
pub const EXPECTED_ABI: u32 = 0x0002_0000;

pub const DX_ERR_WOULD_BLOCK: c_int = -14;
pub const DX_SYS_CONNECTED: u16 = 0x7F01;
pub const DX_SYS_DISCONNECTED: u16 = 0x7F02;

extern "C" {
    pub fn dx_strerror(err: c_int) -> *const c_char;
    pub fn dx_abi_version() -> u32;
    pub fn dx_config_default(cfg: *mut dx_config);
    pub fn dx_create(cfg: *const dx_config) -> *mut dx_ctx;
    pub fn dx_destroy(ctx: *mut dx_ctx);
    pub fn dx_listen(ctx: *mut dx_ctx, addr: *const c_char, port: u16) -> c_int;
    pub fn dx_listen_port(ctx: *const dx_ctx) -> c_int;
    pub fn dx_connect(ctx: *mut dx_ctx, addr: *const c_char, port: u16, out_conn: *mut u32) -> c_int;
    pub fn dx_poll(ctx: *mut dx_ctx, out: *mut dx_packet, max: c_int, timeout_ms: c_int) -> c_int;
    pub fn dx_release(ctx: *mut dx_ctx, count: c_int);
    pub fn dx_send(ctx: *mut dx_ctx, conn: u32, id: u16, data: *const u8, len: u32) -> c_int;
    pub fn dx_close(ctx: *mut dx_ctx, conn: u32) -> c_int;
    pub fn dx_flush(ctx: *mut dx_ctx) -> c_int;
}
