//! Tests of the Rust binding: errors, codec, release (via back-pressure), handlers and hooks.
//! SPDX-License-Identifier: MIT
use duplex::registry::*;
use duplex::{Config, Context, Error, Hooks, Reader, Writer};
use std::cell::RefCell;
use std::rc::Rc;
use std::time::{Duration, Instant};

/// Runs both contexts until `done()` or 10 s (no fixed sleeps).
fn spin(a: &Context, b: &Context, done: impl Fn() -> bool) -> bool {
    let t0 = Instant::now();
    while !done() && t0.elapsed() < Duration::from_secs(10) {
        a.dispatch(1).unwrap();
        b.dispatch(1).unwrap();
    }
    done()
}

fn pair(cfg: impl Fn() -> Config) -> (Context, Context, u32) {
    let server = Context::with_config(&cfg()).unwrap();
    let client = Context::with_config(&cfg()).unwrap();
    server.listen("127.0.0.1", 0).unwrap();
    let conn = client.connect("127.0.0.1", server.port().unwrap()).unwrap();
    (server, client, conn)
}

fn small() -> Config {
    Config::default().max_conns(4).rx_ring_bytes(1 << 20)
}

#[test]
fn errors_carry_code_and_message() {
    let ctx = Context::new().unwrap();
    let err = ctx.send(12345, PING, &[0, 0, 0, 0]).unwrap_err();
    assert_eq!(err.code(), -20);
    assert_eq!(err.to_string(), "invalid connection");
    // Settings the library refuses: a ring smaller than two maximal packets.
    assert_eq!(Context::with_config(&Config::default().rx_ring_bytes(100)).err(), Some(Error(-18)));
}

#[test]
fn codec_roundtrip_and_failures() {
    let data = Writer::new().u32(7).string("héllo €").u64(1 << 40).finish().unwrap();
    let mut r = Reader::new(&data);
    assert_eq!((r.u32().unwrap(), r.string().unwrap(), r.u64().unwrap()), (7, "héllo €", 1 << 40));
    assert_eq!(r.u8(), Err(Error(-2))); // truncated
    assert_eq!(Writer::new().u32(0x01020304).finish().unwrap(), vec![4, 3, 2, 1]); // little-endian
    // Invalid UTF-8 on the wire is refused when reading.
    assert_eq!(Reader::new(&[2, 0, 0xC0, 0x80]).string(), Err(Error(-8)));
    // A string that does not fit the 16-bit length is an error, not a silent truncation.
    assert_eq!(Writer::new().string(&"x".repeat(70000)).finish(), Err(Error(-12)));
}

#[test]
fn small_queue_keeps_flowing_because_batches_release() {
    let (server, client, conn) = pair(|| small().rx_queue(8));
    let (mut sent, mut got) = (0u32, 0u32);
    let t0 = Instant::now();
    while got < 300 && t0.elapsed() < Duration::from_secs(15) {
        if sent < 300 && client.try_send(conn, PING, &Writer::new().u32(sent).finish().unwrap()).unwrap() {
            sent += 1;
        }
        client.dispatch(0).unwrap();
        let batch = server.poll(1).unwrap();
        got += batch.iter().filter(|p| p.id == PING).count() as u32;
        // `batch` is dropped here, which releases its packets; packets cannot outlive it.
    }
    assert_eq!(got, 300);
}

#[test]
fn handlers_hooks_and_flow_control() {
    let (server, client, conn) = pair(small);
    let counts = Rc::new(RefCell::new([0u32; 6])); // pings, pongs, connects, dropped, rejected, chat
    let c = counts.clone();
    server
        .on_packet(PING, move |h, p| {
            c.borrow_mut()[0] += 1;
            h.send(p.conn, PONG, p.data()).unwrap(); // reply from inside a handler
        })
        .unwrap();
    let c = counts.clone();
    client.on_packet(PONG, move |_, _| c.borrow_mut()[1] += 1).unwrap();
    let c = counts.clone();
    server.on_packet(CHAT_SEND, move |_, _| c.borrow_mut()[5] += 1).unwrap();

    let (c1, c2) = (counts.clone(), counts.clone());
    server
        .set_hooks(Hooks {
            on_connect: Some(Box::new(move |_| c1.borrow_mut()[2] += 1)),
            after_recv: Some(Box::new(move |p| {
                let spam = p.id == CHAT_SEND && p.reader().string().map(|s| s.starts_with("spam")).unwrap_or(false);
                if spam {
                    c2.borrow_mut()[3] += 1;
                }
                spam
            })),
            ..Default::default()
        })
        .unwrap();
    let c3 = counts.clone();
    client
        .set_hooks(Hooks {
            before_send: Some(Box::new(move |_, id, _| {
                if id == LOGIN {
                    c3.borrow_mut()[4] += 1;
                }
                id == LOGIN
            })),
            ..Default::default()
        })
        .unwrap();

    assert!(spin(&server, &client, || counts.borrow()[2] == 1));
    client.send(conn, PING, &Writer::new().u32(1).finish().unwrap()).unwrap();
    client.send(conn, CHAT_SEND, &Writer::new().string("spam spam").finish().unwrap()).unwrap();
    client.send(conn, CHAT_SEND, &Writer::new().string("hello").finish().unwrap()).unwrap();
    assert_eq!(client.send(conn, LOGIN, &Writer::new().string("me").finish().unwrap()), Err(Error(-22)));
    assert!(spin(&server, &client, || counts.borrow()[1] == 1 && counts.borrow()[5] == 1));
    assert_eq!(*counts.borrow(), [1, 1, 1, 1, 1, 1]);

    // A full send buffer is Ok(false) from try_send (nobody serves the other side).
    let payload = Writer::new().string(&"x".repeat(1000)).finish().unwrap();
    let mut full = false;
    for _ in 0..200_000 {
        if !client.try_send(conn, CHAT_SEND, &payload).unwrap() {
            full = true;
            break;
        }
    }
    assert!(full);
}

#[test]
fn disconnect_reason_and_hook() {
    let (server, client, conn) = pair(small);
    let connected = Rc::new(RefCell::new(0));
    let reasons = Rc::new(RefCell::new(Vec::new()));
    let (c, r) = (connected.clone(), reasons.clone());
    server
        .set_hooks(Hooks {
            on_connect: Some(Box::new(move |_| *c.borrow_mut() += 1)),
            on_disconnect: Some(Box::new(move |_, reason| r.borrow_mut().push(reason))),
            ..Default::default()
        })
        .unwrap();
    assert!(spin(&server, &client, || *connected.borrow() == 1));
    client.close(conn).unwrap();
    assert!(spin(&server, &client, || *reasons.borrow() == vec![0])); // 0 = normal close
}
