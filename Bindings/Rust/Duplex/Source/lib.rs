//! Safe Rust binding of the Duplex packet library.
//! SPDX-License-Identifier: MIT
//!
//! The C library hands out raw pointers that must be given back in order and
//! reports errors as negative numbers. This crate turns that into Rust habits:
//!
//! * [`Context`] owns the engine and frees it on `Drop`;
//! * [`Batch`] owns the packets of one poll and releases them on `Drop`; a [`Packet`] borrows
//!   from its batch, so the compiler refuses any use after the release;
//! * errors are `Result<_, Error>`;
//! * handlers and hooks are closures, run in Rust over batched polls (no per-packet FFI callback).
//!
//! A context must be used by one thread at a time: it can be moved to another thread but not
//! shared (it is `Send`, not `Sync`).

use duplex_sys as sys;
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::fmt;
use std::marker::PhantomData;
use std::os::raw::c_int;

/// Packet ids generated from `Registry/protocol.toml`.
#[path = "../../../../Generated/Rust/registry.rs"]
pub mod registry;

/// Error code of a library that does not match this binding's interface version.
pub const ERR_ABI_MISMATCH: i32 = -1000;
const ERR_TRUNCATED: i32 = -2;
const ERR_BAD_UTF8: i32 = -8;
const ERR_OVERFLOW: i32 = -12;
const ERR_NO_MEMORY: i32 = -18;
const ERR_REJECTED: i32 = -22;

/// A library error: the `DX_ERR_*` code (negative) and its message.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(pub i32);

impl Error {
    pub fn code(&self) -> i32 {
        self.0
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.0 == ERR_ABI_MISMATCH {
            return write!(f, "incompatible libduplex version");
        }
        // SAFETY: dx_strerror returns a static NUL-terminated string, never NULL.
        let text = unsafe { CStr::from_ptr(sys::dx_strerror(self.0)) };
        write!(f, "{}", text.to_string_lossy())
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

fn check(rc: c_int) -> Result<i32> {
    if rc < 0 {
        Err(Error(rc))
    } else {
        Ok(rc)
    }
}

// ---------------------------------------------------------------- payload codec

/// Builds a payload. Strings must fit in 65535 bytes and are UTF-8 by construction.
#[derive(Default)]
pub struct Writer {
    buf: Vec<u8>,
    err: Option<Error>,
}

impl Writer {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn u8(&mut self, v: u8) -> &mut Self {
        self.buf.push(v);
        self
    }
    pub fn u16(&mut self, v: u16) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn u32(&mut self, v: u32) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn u64(&mut self, v: u64) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn f32(&mut self, v: f32) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn f64(&mut self, v: f64) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn string(&mut self, s: &str) -> &mut Self {
        match u16::try_from(s.len()) {
            Ok(n) => {
                self.u16(n);
                self.buf.extend_from_slice(s.as_bytes());
            }
            Err(_) => self.err = Some(Error(ERR_OVERFLOW)),
        }
        self
    }
    pub fn bytes(&mut self, b: &[u8]) -> &mut Self {
        match u32::try_from(b.len()) {
            Ok(n) => {
                self.u32(n);
                self.buf.extend_from_slice(b);
            }
            Err(_) => self.err = Some(Error(ERR_OVERFLOW)),
        }
        self
    }
    /// The finished payload, or the first error that happened while writing.
    pub fn finish(&self) -> Result<Vec<u8>> {
        match self.err {
            Some(e) => Err(e),
            None => Ok(self.buf.clone()),
        }
    }
}

/// Reads a payload. Every call is bounds-checked and strings are validated as UTF-8.
pub struct Reader<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Reader { data, pos: 0 }
    }
    fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        if self.data.len() - self.pos < n {
            return Err(Error(ERR_TRUNCATED));
        }
        let s = &self.data[self.pos..self.pos + n];
        self.pos += n;
        Ok(s)
    }
    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.take(1)?[0])
    }
    pub fn u16(&mut self) -> Result<u16> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }
    pub fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    pub fn u64(&mut self) -> Result<u64> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    pub fn f32(&mut self) -> Result<f32> {
        Ok(f32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    pub fn f64(&mut self) -> Result<f64> {
        Ok(f64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    pub fn string(&mut self) -> Result<&'a str> {
        let n = self.u16()? as usize;
        std::str::from_utf8(self.take(n)?).map_err(|_| Error(ERR_BAD_UTF8))
    }
    pub fn bytes(&mut self) -> Result<&'a [u8]> {
        let n = self.u32()? as usize;
        self.take(n)
    }
}

// ---------------------------------------------------------------- packets and batches

/// A received packet. It borrows from the [`Batch`] (or handler call) that produced it.
pub struct Packet<'a> {
    pub conn: u32,
    pub id: u16,
    pub sequence: u32,
    data: &'a [u8],
}

impl<'a> Packet<'a> {
    /// SAFETY: `raw` must come from the library and stay valid for 'a (it does: see Batch).
    unsafe fn from_raw(raw: &sys::dx_packet) -> Packet<'a> {
        let data = if raw.length == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(raw.data, raw.length as usize)
        };
        Packet { conn: raw.conn, id: raw.packet_id, sequence: raw.sequence, data }
    }
    pub fn data(&self) -> &'a [u8] {
        self.data
    }
    pub fn reader(&self) -> Reader<'a> {
        Reader::new(self.data)
    }
    /// Reason of a `DX_SYS_DISCONNECTED` event: 0 for a normal close, negative for an error.
    pub fn reason(&self) -> Result<i32> {
        Ok(self.reader().u32()? as i32)
    }
}

/// The packets of one [`Handle::poll`]. They are given back to the library when it is dropped.
pub struct Batch<'a> {
    handle: &'a Handle,
    raw: Vec<sys::dx_packet>,
}

impl<'a> Batch<'a> {
    pub fn len(&self) -> usize {
        self.raw.len()
    }
    pub fn is_empty(&self) -> bool {
        self.raw.is_empty()
    }
    pub fn iter(&self) -> impl Iterator<Item = Packet<'_>> {
        // SAFETY: the memory stays valid until Drop, and the packets borrow `self`.
        self.raw.iter().map(|r| unsafe { Packet::from_raw(r) })
    }
}

impl Drop for Batch<'_> {
    fn drop(&mut self) {
        if !self.raw.is_empty() {
            // SAFETY: releases exactly the entries this poll handed out.
            unsafe { sys::dx_release(self.handle.raw, self.raw.len() as c_int) };
        }
    }
}

// ---------------------------------------------------------------- context

/// Settings for [`Context::with_config`]; start from `Config::default()`.
pub struct Config(sys::dx_config);

impl Default for Config {
    fn default() -> Self {
        let mut cfg = std::mem::MaybeUninit::<sys::dx_config>::uninit();
        // SAFETY: dx_config_default fills the whole structure.
        unsafe { sys::dx_config_default(cfg.as_mut_ptr()) };
        Config(unsafe { cfg.assume_init() })
    }
}

impl Config {
    pub fn max_conns(mut self, n: u32) -> Self {
        self.0.max_conns = n;
        self
    }
    pub fn max_payload(mut self, n: u32) -> Self {
        self.0.max_payload = n;
        self.0.tx_bytes = 4 * (n + 14);
        self
    }
    pub fn rx_queue(mut self, n: u32) -> Self {
        self.0.rx_queue = n;
        self
    }
    pub fn idle_timeout_ms(mut self, n: u32) -> Self {
        self.0.idle_timeout_ms = n;
        self
    }
    pub fn rx_ring_bytes(mut self, n: u32) -> Self {
        self.0.rx_ring_bytes = n;
        self
    }
}

/// Callbacks for events; every field is optional.
#[derive(Default)]
pub struct Hooks {
    pub on_connect: Option<Box<dyn FnMut(u32)>>,
    pub on_disconnect: Option<Box<dyn FnMut(u32, i32)>>,
    pub on_error: Option<Box<dyn FnMut(u32, i32)>>,
    /// Return true to reject the packet (`send` then fails with code -22).
    pub before_send: Option<Box<dyn FnMut(u32, u16, &[u8]) -> bool>>,
    /// Return true to drop the packet before any handler sees it.
    pub after_recv: Option<Box<dyn FnMut(&Packet) -> bool>>,
}

/// What a context can do. Handlers receive a `&Handle`, and a [`Context`] dereferences to one.
pub struct Handle {
    raw: *mut sys::dx_ctx,
    inner: *const Inner,
}

type HandlerFn = Box<dyn FnMut(&Handle, &Packet)>;

struct Inner {
    handlers: RefCell<HashMap<u16, HandlerFn>>,
    hooks: RefCell<Hooks>,
}

/// One network engine; frees itself on drop. Not `Sync`.
/// Handlers and hooks run here, in Rust, over one batched poll: the C library never calls back.
pub struct Context {
    handle: Handle,
    _inner: Box<Inner>,
    _single_thread: PhantomData<*const ()>,
}

// SAFETY: a context may move between threads as long as one thread uses it at a time (the C API's rule).
unsafe impl Send for Context {}

impl std::ops::Deref for Context {
    type Target = Handle;
    fn deref(&self) -> &Handle {
        &self.handle
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        // SAFETY: we own the context; nothing can use it after this.
        unsafe { sys::dx_destroy(self.handle.raw) };
    }
}

impl Context {
    pub fn new() -> Result<Context> {
        Self::create(std::ptr::null())
    }

    pub fn with_config(cfg: &Config) -> Result<Context> {
        Self::create(&cfg.0)
    }

    fn create(cfg: *const sys::dx_config) -> Result<Context> {
        // SAFETY: plain function calls; cfg is NULL or points to a valid config.
        let abi = unsafe { sys::dx_abi_version() };
        if abi >> 16 != sys::EXPECTED_ABI >> 16 || abi < sys::EXPECTED_ABI {
            return Err(Error(ERR_ABI_MISMATCH));
        }
        let raw = unsafe { sys::dx_create(cfg) };
        if raw.is_null() {
            return Err(Error(ERR_NO_MEMORY));
        }
        let inner = Box::new(Inner { handlers: RefCell::new(HashMap::new()), hooks: RefCell::new(Hooks::default()) });
        Ok(Context { handle: Handle { raw, inner: &*inner }, _inner: inner, _single_thread: PhantomData })
    }

    /// Calls `f` for packets with this id (0 = default handler) during `dispatch`.
    pub fn on_packet(&self, id: u16, f: impl FnMut(&Handle, &Packet) + 'static) -> Result<()> {
        self.inner().handlers.borrow_mut().insert(id, Box::new(f));
        Ok(())
    }

    /// Installs hooks (replacing the previous ones).
    pub fn set_hooks(&self, hooks: Hooks) -> Result<()> {
        *self.inner().hooks.borrow_mut() = hooks;
        Ok(())
    }

    /// Polls once, then runs hooks and handlers for the whole batch and releases it.
    /// Returns the number of entries handled.
    pub fn dispatch(&self, timeout_ms: i32) -> Result<usize> {
        let batch = self.handle.poll(timeout_ms)?;
        let inner = self.inner();
        for p in batch.iter() {
            match p.id {
                sys::DX_SYS_CONNECTED => {
                    if let Some(f) = inner.hooks.borrow_mut().on_connect.as_mut() {
                        f(p.conn);
                    }
                }
                sys::DX_SYS_DISCONNECTED => {
                    let reason = p.reason()?;
                    let mut hooks = inner.hooks.borrow_mut();
                    if reason < 0 {
                        if let Some(f) = hooks.on_error.as_mut() {
                            f(p.conn, reason);
                        }
                    }
                    if let Some(f) = hooks.on_disconnect.as_mut() {
                        f(p.conn, reason);
                    }
                }
                _ => {
                    let dropped = inner.hooks.borrow_mut().after_recv.as_mut().map(|f| f(&p)).unwrap_or(false);
                    if dropped {
                        continue;
                    }
                    // try_borrow_mut: a handler cannot reach on_packet (it only gets a Handle).
                    if let Ok(mut map) = inner.handlers.try_borrow_mut() {
                        if map.contains_key(&p.id) {
                            map.get_mut(&p.id).unwrap()(&self.handle, &p);
                        } else if let Some(f) = map.get_mut(&0) {
                            f(&self.handle, &p);
                        }
                    }
                }
            }
        }
        Ok(batch.len())
    }

    fn inner(&self) -> &Inner {
        // SAFETY: points into `_inner`, which lives as long as `self`.
        unsafe { &*self.handle.inner }
    }
}

impl Handle {
    pub fn listen(&self, addr: &str, port: u16) -> Result<()> {
        let addr = CString::new(addr).map_err(|_| Error(-1))?;
        check(unsafe { sys::dx_listen(self.raw, addr.as_ptr(), port) }).map(|_| ())
    }

    pub fn port(&self) -> Result<u16> {
        check(unsafe { sys::dx_listen_port(self.raw) }).map(|p| p as u16)
    }

    pub fn connect(&self, addr: &str, port: u16) -> Result<u32> {
        let addr = CString::new(addr).map_err(|_| Error(-1))?;
        let mut conn = 0u32;
        check(unsafe { sys::dx_connect(self.raw, addr.as_ptr(), port, &mut conn) })?;
        Ok(conn)
    }

    pub fn close(&self, conn: u32) -> Result<()> {
        check(unsafe { sys::dx_close(self.raw, conn) }).map(|_| ())
    }

    /// Queues a packet (written at the next poll/dispatch, or by `flush`).
    /// `Err(Error(-14))` means the send buffer is full (see `try_send`); `Err(Error(-22))` means
    /// the `before_send` hook refused it.
    pub fn send(&self, conn: u32, id: u16, data: &[u8]) -> Result<()> {
        // SAFETY: `inner` lives as long as the context this handle belongs to.
        let inner = unsafe { &*self.inner };
        if let Ok(mut hooks) = inner.hooks.try_borrow_mut() {
            if let Some(f) = hooks.before_send.as_mut() {
                if f(conn, id, data) {
                    return Err(Error(ERR_REJECTED));
                }
            }
        }
        check(unsafe { sys::dx_send(self.raw, conn, id, data.as_ptr(), data.len() as u32) }).map(|_| ())
    }

    /// Like `send`, but returns `Ok(false)` instead of an error when the send buffer is full.
    pub fn try_send(&self, conn: u32, id: u16, data: &[u8]) -> Result<bool> {
        match self.send(conn, id, data) {
            Ok(()) => Ok(true),
            Err(Error(sys::DX_ERR_WOULD_BLOCK)) => Ok(false),
            Err(e) => Err(e),
        }
    }

    /// Writes queued packets now (only needed when no poll/dispatch follows).
    pub fn flush(&self) -> Result<()> {
        check(unsafe { sys::dx_flush(self.raw) }).map(|_| ())
    }

    /// Waits up to `timeout_ms` and returns up to 64 packets, released when the batch is dropped.
    pub fn poll(&self, timeout_ms: i32) -> Result<Batch<'_>> {
        let mut raw = vec![
            sys::dx_packet { conn: 0, sequence: 0, data: std::ptr::null(), length: 0, packet_id: 0, flags: 0, reserved: 0 };
            64
        ];
        let n = check(unsafe { sys::dx_poll(self.raw, raw.as_mut_ptr(), 64, timeout_ms) })?;
        raw.truncate(n as usize);
        Ok(Batch { handle: self, raw })
    }
}
