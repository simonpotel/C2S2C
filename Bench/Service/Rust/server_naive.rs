//! Benchmark service written from scratch in Rust (no C2S2C, std only): what you write when you
//! recode the protocol yourself. One thread per connection, the same checks as the C2S2C library
//! for this packet, and connections idle for one second are dropped.
//! SPDX-License-Identifier: MIT
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::time::Duration;

const MAGIC: u16 = 0xC0DE;
const PING: u16 = 3;
const PONG: u16 = 0x8003;
const MAX_PAYLOAD: u32 = 1024;

fn serve(mut s: TcpStream) -> std::io::Result<()> {
    s.set_nodelay(true)?;
    s.set_read_timeout(Some(Duration::from_secs(1)))?; // idle connections are dropped
    let (mut rx, mut tx) = (0u32, 0u32);
    loop {
        let mut h = [0u8; 14];
        s.read_exact(&mut h)?;
        let magic = u16::from_le_bytes([h[0], h[1]]);
        let (flags, id) = (h[3], u16::from_le_bytes([h[4], h[5]]));
        let length = u32::from_le_bytes([h[6], h[7], h[8], h[9]]);
        let seq = u32::from_le_bytes([h[10], h[11], h[12], h[13]]);
        let bad = magic != MAGIC || h[2] != 1 || flags & 0xFC != 0 || flags & 1 != 0 || id & 0x8000 != 0
            || length > MAX_PAYLOAD || seq != rx;
        if bad || id != PING || length != 4 {
            return Ok(()); // close the connection
        }
        let mut payload = [0u8; 4];
        s.read_exact(&mut payload)?;
        let mut out = [0u8; 18];
        out[0..2].copy_from_slice(&MAGIC.to_le_bytes());
        out[2] = 1;
        out[3] = 1;
        out[4..6].copy_from_slice(&PONG.to_le_bytes());
        out[6..10].copy_from_slice(&4u32.to_le_bytes());
        out[10..14].copy_from_slice(&tx.to_le_bytes());
        out[14..18].copy_from_slice(&payload);
        s.write_all(&out)?;
        rx += 1;
        tx += 1;
    }
}

fn main() -> std::io::Result<()> {
    let listener = TcpListener::bind("127.0.0.1:0")?;
    println!("{}", listener.local_addr()?.port());
    for stream in listener.incoming().flatten() {
        std::thread::spawn(move || {
            let _ = serve(stream);
        });
    }
    Ok(())
}
