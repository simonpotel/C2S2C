//! Cost of the Rust binding: pipelined ping/pong round trips per second.
//! SPDX-License-Identifier: MIT
use duplex::registry::*;
use duplex::{Context, Writer};
use std::cell::Cell;
use std::rc::Rc;
use std::time::{Duration, Instant};

fn main() -> duplex::Result<()> {
    let (server, client) = (Context::new()?, Context::new()?);
    server.listen("127.0.0.1", 0)?;
    let conn = client.connect("127.0.0.1", server.port()?)?;
    server.on_packet(PING, |h, p| {
        let _ = h.send(p.conn, PONG, p.data());
    })?;
    let pongs = Rc::new(Cell::new(0u64));
    let c = pongs.clone();
    client.on_packet(PONG, move |_, _| c.set(c.get() + 1))?;
    let payload = Writer::new().u32(1).finish()?;
    let mut inflight = 0u64;
    let t0 = Instant::now();
    while t0.elapsed() < Duration::from_secs(1) {
        while inflight < 32 && client.try_send(conn, PING, &payload)? {
            inflight += 1;
        }
        server.dispatch(0)?;
        let before = pongs.get();
        client.dispatch(0)?;
        inflight -= pongs.get() - before;
    }
    println!("rust_roundtrips {:.0} packets/s", pongs.get() as f64 / t0.elapsed().as_secs_f64());
    Ok(())
}
