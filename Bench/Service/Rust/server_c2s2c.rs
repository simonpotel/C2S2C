//! Benchmark service on the Rust binding. Usage: bench_service_c2s2c poll|dispatch
//! poll: a plain loop over batches; dispatch: Rust handlers (on_packet) over the same batches.
//! SPDX-License-Identifier: MIT
use c2s2c::registry::*;
use c2s2c::{Config, Context};

fn main() -> c2s2c::Result<()> {
    let ctx = Context::with_config(&Config::default().max_conns(4096).max_payload(1024).idle_timeout_ms(1000))?;
    ctx.listen("127.0.0.1", 0)?;
    println!("{}", ctx.port()?);
    if std::env::args().nth(1).as_deref() == Some("dispatch") {
        ctx.on_packet(PING, |h, p| {
            let _ = h.send(p.conn, PONG, p.data());
        })?;
        loop {
            ctx.dispatch(100)?;
        }
    }
    loop {
        let batch = ctx.poll(100)?;
        for p in batch.iter() {
            if p.id == PING {
                let _ = ctx.send(p.conn, PONG, p.data());
            }
        }
    }
}
