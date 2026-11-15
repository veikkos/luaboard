//! The simulator's control channel (`--control PORT`): a TCP port on
//! 127.0.0.1 taking `KEY=VALUE` lines (the simulator's `--set` keys,
//! `advance=MS`, `quit=1`). Each is answered on the same connection, in
//! order, with `ok LINE` or `error LINE` once it has taken effect. One client
//! at a time: the next one waits until it leaves.

use std::io::{BufRead, BufReader, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

use anyhow::{bail, Context, Result};

/// Where `luaboard sim-ctl` looks without --control: the port the `run`
/// target gives the simulator (its Wi-Fi link's 3333, plus one).
pub const DEFAULT: &str = "127.0.0.1:3334";

/// How long one answer may take: an advance answers once the device has run
/// through its time, a day of it in some seconds.
const ANSWER_TIMEOUT: Duration = Duration::from_secs(60);

/// "PORT" or "HOST:PORT"; a port alone is on 127.0.0.1.
pub fn address(s: &str) -> String {
    if s.contains(':') {
        s.to_string()
    } else {
        format!("127.0.0.1:{s}")
    }
}

/// Whether the simulator took a line: its answer, `ok LINE` or `error LINE`.
#[derive(Debug, PartialEq, Eq)]
pub enum Answer {
    Ok,
    Error,
}

/// Send `lines` to the control channel at `addr` in one write, so no real
/// time passes between them while the device's clock runs, and wait for each
/// answer.
pub fn send(addr: &str, lines: &[String]) -> Result<Vec<Answer>> {
    if let Some(l) = lines.iter().find(|l| l.contains('\n') || l.contains('\r')) {
        bail!("a control line is one line: {l:?}");
    }
    let to = address(addr);
    let sock = to.to_socket_addrs().with_context(|| format!("control channel {to}"))?.next();
    let Some(sock) = sock else { bail!("control channel {to}: no such address") };
    let mut s = TcpStream::connect_timeout(&sock, Duration::from_secs(2)).with_context(|| {
        format!("no simulator control channel at {to} (the simulator's --control PORT; the run target's is 3334)")
    })?;
    let _ = s.set_nodelay(true);
    s.set_read_timeout(Some(ANSWER_TIMEOUT))?;
    let mut all = String::new();
    for l in lines {
        all.push_str(l);
        all.push('\n');
    }
    s.write_all(all.as_bytes())?;
    let mut answers = Vec::new();
    let mut r = BufReader::new(s);
    for l in lines {
        let mut a = String::new();
        match r.read_line(&mut a) {
            Ok(0) => bail!("the simulator closed the control channel before answering {l}"),
            Ok(_) => {}
            Err(e) => bail!("no answer to {l} from {to} ({e}; one client at a time: is another one connected?)"),
        }
        let a = a.trim_end();
        answers.push(match a.strip_suffix(l.as_str()) {
            Some("ok ") => Answer::Ok,
            Some("error ") => Answer::Error,
            _ => bail!("unexpected answer to {l}: {a}"),
        });
    }
    Ok(answers)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    /// A fake simulator: answers each line as `judge` says.
    fn fake(judge: fn(&str) -> &'static str) -> String {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = l.local_addr().unwrap().to_string();
        std::thread::spawn(move || {
            let (s, _) = l.accept().unwrap();
            let mut w = s.try_clone().unwrap();
            for line in BufReader::new(s).lines() {
                let line = line.unwrap();
                writeln!(w, "{} {line}", judge(&line)).unwrap();
            }
        });
        addr
    }

    #[test]
    fn sends_lines_and_reads_answers() {
        let addr = fake(|l| if l.contains('=') { "ok" } else { "error" });
        let lines = vec!["button=B".to_string(), "bogus".to_string(), "advance=1000".to_string()];
        assert_eq!(send(&addr, &lines).unwrap(), vec![Answer::Ok, Answer::Error, Answer::Ok]);
    }

    #[test]
    fn port_alone_is_loopback() {
        assert_eq!(address("3334"), "127.0.0.1:3334");
        assert_eq!(address("localhost:9"), "localhost:9");
    }

    #[test]
    fn refuses_line_breaks() {
        assert!(send("127.0.0.1:1", &["a=1\nquit=1".to_string()]).is_err());
    }
}
