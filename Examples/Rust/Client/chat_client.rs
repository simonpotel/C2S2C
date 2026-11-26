//! Chat client (see Examples/C/Client) written with the Rust binding.
//! SPDX-License-Identifier: MIT
//!
//! Usage: chat_client_rust <port> <nickname> <message>...
//! Logs in, sends each message, prints the broadcasts it receives until its own messages came back.
use c2s2c::registry::*;
use c2s2c::{Context, Writer};
use std::cell::Cell;
use std::io::Write;
use std::rc::Rc;
use std::time::{Duration, Instant};

fn main() -> c2s2c::Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 4 {
        eprintln!("usage: chat_client_rust <port> <nickname> <message>...");
        std::process::exit(2);
    }
    let port: u16 = args[1].parse().expect("port");
    let ctx = Context::new()?;

    let logged_in = Rc::new(Cell::new(false));
    let received = Rc::new(Cell::new(0usize));
    let l = logged_in.clone();
    ctx.on_packet(LOGIN_OK, move |_, _| l.set(true))?;
    let r = received.clone();
    ctx.on_packet(CHAT_MESSAGE, move |_, p| {
        let mut rd = p.reader();
        if let (Ok(id), Ok(text)) = (rd.u32(), rd.string()) {
            println!("[{id}] {text}");
            let _ = std::io::stdout().flush();
            r.set(r.get() + 1);
        }
    })?;

    let conn = ctx.connect("127.0.0.1", port)?;
    let wait_until = |done: &dyn Fn() -> bool| {
        let t0 = Instant::now();
        while !done() && t0.elapsed() < Duration::from_secs(10) {
            let _ = ctx.dispatch(50);
        }
        done()
    };

    ctx.send(conn, LOGIN, &Writer::new().string(&args[2]).finish()?)?;
    if !wait_until(&|| logged_in.get()) {
        std::process::exit(1);
    }
    for text in &args[3..] {
        ctx.send(conn, CHAT_SEND, &Writer::new().string(text).finish()?)?;
    }
    let want = args.len() - 3;
    let ok = wait_until(&|| received.get() >= want);
    ctx.close(conn)?;
    for _ in 0..20 {
        ctx.dispatch(10)?; // let the close go out
    }
    std::process::exit(if ok { 0 } else { 1 });
}
