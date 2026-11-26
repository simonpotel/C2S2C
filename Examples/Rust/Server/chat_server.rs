//! Chat room server (see Examples/C/Server) written with the Rust binding.
//! SPDX-License-Identifier: MIT
//!
//! Same protocol and command line as the C version. Usage: chat_server_rust [port]
//! The chosen port is printed on the first line.
use c2s2c::registry::*;
use c2s2c::{Context, Hooks, Writer};
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::time::{SystemTime, UNIX_EPOCH};

fn main() -> c2s2c::Result<()> {
    let ctx = Context::new()?;
    let port = std::env::args().nth(1).and_then(|p| p.parse().ok()).unwrap_or(0);
    ctx.listen("127.0.0.1", port)?;

    let users: Rc<RefCell<HashMap<u32, String>>> = Rc::default(); // connection handle -> nickname

    let u = users.clone();
    ctx.on_packet(LOGIN, move |h, p| {
        if let Ok(nick) = p.reader().string() {
            u.borrow_mut().insert(p.conn, nick.to_string());
            let _ = h.send(p.conn, LOGIN_OK, &Writer::new().u32(p.conn).finish().unwrap()); // handle = user id
        }
    })?;
    let u = users.clone();
    ctx.on_packet(CHAT_SEND, move |h, p| {
        if !u.borrow().contains_key(&p.conn) {
            return; // must log in first
        }
        let text = match p.reader().string() {
            Ok(t) => t,
            Err(_) => return,
        };
        let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs();
        let message = Writer::new().u32(p.conn).string(text).u64(now).finish().unwrap();
        for conn in u.borrow().keys() {
            let _ = h.send(*conn, CHAT_MESSAGE, &message); // broadcast
        }
    })?;
    ctx.on_packet(PING, |h, p| {
        let _ = h.send(p.conn, PONG, p.data()); // same nonce back
    })?;
    let u = users.clone();
    ctx.set_hooks(Hooks { on_disconnect: Some(Box::new(move |conn, _| { u.borrow_mut().remove(&conn); })), ..Default::default() })?;

    println!("{}", ctx.port()?);
    loop {
        ctx.dispatch(200)?;
    }
}
