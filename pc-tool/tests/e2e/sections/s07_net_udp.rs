//! UDP from Lua: net.udp, datagrams out and in, the waiting helper, limits.

use std::net::UdpSocket;
use std::time::Duration;

use crate::harness::T;

/// A UDP port on this computer that is free right now.
fn free_udp_port() -> u16 {
    UdpSocket::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
}

pub fn run(t: &mut T) {
    t.say("== UDP from Lua (net.udp)");
    t.wipe();
    t.boot();
    let host = UdpSocket::bind("127.0.0.1:0").unwrap();
    host.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let hp = host.local_addr().unwrap().port();
    let dp = free_udp_port();

    // Out and back: the device sends from a port of its own, the answer comes to it.
    t.eval("u = net.udp()");
    let sent = t.eval(&format!(r#"u:send("ping", "127.0.0.1", {hp})"#));
    let mut buf = [0u8; 64];
    let got = host.recv_from(&mut buf).ok();
    let from = got.map(|(_, a)| a);
    let ok = sent == "true" && got.map(|(n, _)| &buf[..n] == b"ping").unwrap_or(false);
    t.check(ok, "a datagram goes out", |_| format!("send: {sent}, the host got {got:?}"));
    let own = t.eval("u:port()");
    let nothing = t.eval("select('#', u:recv()), u:recv()");
    if let Some(a) = from {
        host.send_to(b"pong", a).unwrap();
    }
    let ok = t
        .until(2.0, |t| t.eval("local d, ip, p = u:recv() if d then R = { d, ip, p } end return R and R[1]") == "pong");
    let v = t.eval("R[1], R[2], R[3]");
    let ok = ok
        && v == format!("pong\t127.0.0.1\t{hp}")
        && nothing == "1\tnil"
        && from.map(|a| a.port().to_string()) == Some(own);
    t.check(ok, "one comes back to its port, with the sender's address; nothing waiting is nil", |_| {
        format!("recv: {v}, before it: {nothing}")
    });

    // A port of its choice; an empty datagram is data, not "nothing".
    let bound = t.eval(&format!("b = net.udp({dp}) return b:port()"));
    host.send_to(b"", ("127.0.0.1", dp)).unwrap();
    let ok =
        t.until(2.0, |t| t.eval("local d, ip, p = b:recv() if d then E = { d, ip, p } end return E ~= nil") == "true");
    let v = t.eval("#E[1], E[2], E[3]");
    let taken = t.eval(&format!("select(2, net.udp({dp}))"));
    let ok = ok && bound == dp.to_string() && v == format!("0\t127.0.0.1\t{hp}") && taken.starts_with("bind: ");
    t.check(ok, "net.udp(port) receives on that port, an empty datagram too; a port in use is an error value", |_| {
        format!("port {bound}, empty: {v}, the port twice: {taken}")
    });

    // What isn't taken, and a closed socket.
    let name = t.eval(r#"pcall(u.send, u, "x", "example.com", 9)"#);
    // (each call's message: the second of what pcall returns)
    let short = t.eval(
        r#"select(2, pcall(u.send, u, "x", "192.168.1", 9)), select(2, pcall(u.send, u, "x", 1, 9)), select(2, pcall(u.send, u, "x", "1.2.3.4.5", 9))"#,
    );
    let own = t.eval("select(2, pcall(net.udp, 68)), select(2, pcall(net.udp, 5353))");
    let big = t.eval(r#"pcall(u.send, u, string.rep("x", 1473), "127.0.0.1", 9)"#);
    let fits = t.eval(&format!(r#"u:send(string.rep("x", 1472), "127.0.0.1", {hp})"#));
    let n = host.recv_from(&mut [0u8; 2048]).map(|(n, _)| n).unwrap_or(0);
    t.eval("u:close()");
    let closed = t.eval(r#"select(2, u:recv()), select(2, u:send("x", "127.0.0.1", 9))"#);
    let ok = name.contains("IPv4 address")
        && big.contains("1472")
        && fits == "true"
        && n == 1472
        && closed == "closed\tclosed";
    t.check(ok, "an address not a name, 1472 bytes at most; a closed socket says so", |_| {
        format!("name: {name} / big: {big} / 1472 bytes: {fits} ({n} got) / closed: {closed}")
    });
    let ok = short.matches("IPv4 address").count() == 3 && own.matches("device's own ports").count() == 2;
    t.check(ok, "an address is four numbers with dots; the device's own ports (DHCP, mDNS) aren't an app's", |_| {
        format!("not addresses: {short} / ports 68 and 5353: {own}")
    });

    // Sockets are the app's: four at most (b is one), and closed when it ends.
    let many = t.eval("local n = 0 K = {} for i = 1, 20 do local s = net.udp() if s then n = n + 1 K[i] = s end end return n, select(2, net.udp())");
    t.write(
        "udp/main.lua",
        format!(
            "S = assert(net.udp({dp}))\n\
             task(function()\n\
               T1 = table.pack(S:wait(200)) T2 = table.pack(S:wait(5000))\n\
               T3 = #(S:wait(5000)) T4 = #(S:wait(5000, 4096))\n\
             end)\n\
             function loop() end\n"
        ),
    );
    let first = t.e(&["put", &t.path("udp/main.lua"), "--app", "udp", "--run", "main.lua"]);
    // The waiting helper, in the app's task: nothing in time, then a datagram.
    t.until(2.0, |t| t.eval("T1 ~= nil") == "true");
    let timeout = t.eval("T1[1], T1[2]");
    host.send_to(b"late", ("127.0.0.1", dp)).unwrap();
    let late = t.until(2.0, |t| t.eval("T2 and T2[1]") == "late");
    // A datagram longer than a packet: cut at 1472 unless more is asked for.
    host.send_to(&[b'x'; 3000], ("127.0.0.1", dp)).unwrap();
    t.until(2.0, |t| t.eval("T3 ~= nil") == "true");
    host.send_to(&[b'x'; 3000], ("127.0.0.1", dp)).unwrap();
    t.until(2.0, |t| t.eval("T4 ~= nil") == "true");
    let cut = t.eval("T3, T4");
    let outside = t.eval("S:wait(10)");
    let ok = late && timeout == "nil\ttimeout" && outside.contains("needs task()") && cut == "1472\t3000";
    t.check(ok, "u:wait() in a task: a datagram, or nil and \"timeout\"", |t| {
        format!(
            "timeout: {timeout}, then: {}, outside a task: {outside}, 3000 bytes read as: {cut}",
            t.eval("T2 and T2[1]")
        )
    });
    let again = t.e(&["run", "udp"]);
    t.until(2.0, |t| t.e(&["info"]).contains("running: udp/main.lua"));
    let ok = many.starts_with("3\ttoo many UDP sockets (max 4)")
        && first.contains("RUNNING")
        && again.contains("RUNNING")
        && t.eval("S:port()") == dp.to_string();
    t.check(ok, "four UDP sockets at most, and they are closed when the app ends", |t| {
        format!("opened: {many} / runs: {first} / {again} / port {}", t.eval("S:port()"))
    });
    t.stop();
}
