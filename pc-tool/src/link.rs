//! Links to the device: a serial port, the simulator's pseudo-terminal or
//! named pipe, or the Wi-Fi link (wifi::TcpLink), behind `Link`.

use anyhow::{Context, Result};
use std::io::{Read, Write};
#[cfg(not(windows))]
use std::path::Path;
use std::time::Duration;

use crate::wifi;

pub const DEFAULT_BAUD: u32 = 115_200;

/// The simulator's serial port as `run` starts it: a symlink to its pty, or
/// on Windows its named pipe.
#[cfg(not(windows))]
pub const SIM_LINK: &str = "/tmp/esp32-sim";
#[cfg(windows)]
pub const SIM_LINK: &str = r"\\.\pipe\esp32-sim";

/// What a device's serial port is called here, for hints.
pub const PORT_EXAMPLE: &str = if cfg!(windows) {
    "COM3"
} else if cfg!(target_os = "macos") {
    "/dev/cu.usbmodem..."
} else {
    "/dev/ttyACM0"
};

/// True if `port` is the simulator's pseudo-terminal (or named pipe) rather than a device.
pub fn is_simulator_port(port: &str) -> bool {
    if pipe::is_pipe(port) {
        return true;
    }
    let target = std::fs::canonicalize(port).map(|p| p.to_string_lossy().into_owned()).unwrap_or_default();
    port == SIM_LINK || target.starts_with("/dev/ttys") || target.starts_with("/dev/pts/")
}

/// Is the simulator's serial port there (a simulator running)?
fn sim_link_exists() -> bool {
    #[cfg(windows)]
    {
        pipe::exists(SIM_LINK)
    }
    #[cfg(not(windows))]
    {
        Path::new(SIM_LINK).exists()
    }
}

/// Serial ports worth offering to the user: real ports plus the simulator link.
pub fn list_ports() -> Vec<String> {
    let mut out = Vec::new();
    if sim_link_exists() {
        out.push(SIM_LINK.to_string());
    }
    if let Ok(ports) = serialport::available_ports() {
        for p in ports {
            // On macOS every device appears as /dev/tty.* and /dev/cu.*; cu.* is the right one.
            if p.port_name.starts_with("/dev/tty.") {
                continue;
            }
            out.push(p.port_name);
        }
    }
    out
}

/// Byte transport to the device: a real serial port, or a raw PTY (simulator).
pub trait Link: Read + Write + Send {}
impl<T: Read + Write + Send> Link for T {}

/// Open a serial link. Real ports (ESP32-S3 USB CDC, CP2102/CH340/FTDI) go through `serialport`;
/// if that fails on Unix - typically because the path is a pseudo-terminal from
/// the simulator, where macOS rejects the baud-rate ioctl - fall back to a raw
/// termios file handle.
pub fn open_link(path: &str, baud: u32) -> Result<Box<dyn Link>> {
    open_link_poll(path, baud, Duration::from_millis(20))
}

/// open_link, with how long a read waits for data before it gives up
/// (TimedOut). Short, for a program that also has something to send
/// meanwhile and only this thread to do it (`luaboard raw`).
pub fn open_link_poll(path: &str, baud: u32, wait: Duration) -> Result<Box<dyn Link>> {
    if let Some((host, port)) = wifi::parse_tcp_port(path) {
        return Ok(Box::new(wifi::TcpLink::open(&host, port, wait)?));
    }
    #[cfg(windows)]
    if pipe::is_pipe(path) {
        return Ok(Box::new(pipe::PipeLink::open(path, wait)?));
    }
    // The simulator's serial port is a pseudo-terminal: no baud rate to
    // set, and nothing for the serial port library to do.
    #[cfg(unix)]
    if pty::is_pty(path) {
        return Ok(Box::new(pty::RawTty::open(path, wait)?));
    }
    match serialport::new(path, baud).timeout(wait).open() {
        Ok(mut p) => {
            // Both lines up is "run" for the ESP32-S3's USB serial and for the
            // reset circuit of USB-UART boards; RTS alone resets the chip. So
            // DTR first, and an open never resets the device (an update that
            // is just booting would be rolled back by the bootloader).
            let _ = p.write_data_terminal_ready(true);
            let _ = p.write_request_to_send(true);
            Ok(Box::new(p))
        }
        Err(e) => {
            #[cfg(unix)]
            match pty::RawTty::open(path, wait) {
                Ok(p) => return Ok(Box::new(p)),
                Err(busy) if busy.kind() == std::io::ErrorKind::ResourceBusy => return Err(busy.into()),
                Err(_) => {}
            }
            Err(e).with_context(|| format!("cannot open serial port {path}"))
        }
    }
}

/// The simulator's serial port on Windows: a named pipe, `\\.\pipe\<name>`.
mod pipe {
    /// Does `port` name a pipe (`\\.\pipe\...`, or with forward slashes)?
    pub fn is_pipe(port: &str) -> bool {
        let p = port.to_ascii_lowercase().replace('/', "\\");
        p.starts_with(r"\\.\pipe\")
    }

    /// Is the pipe there? Looked up in the pipe folder: opening it to see
    /// would take the simulator's only connection.
    #[cfg(windows)]
    pub fn exists(port: &str) -> bool {
        let name = port.replace('/', "\\");
        let Some(name) = name.get(9..) else { return false };
        std::fs::read_dir(r"\\.\pipe\")
            .is_ok_and(|mut d| d.any(|e| e.is_ok_and(|e| e.file_name().to_string_lossy().eq_ignore_ascii_case(name))))
    }

    #[cfg(windows)]
    pub use link::PipeLink;

    #[cfg(windows)]
    mod link {
        use std::fs::{File, OpenOptions};
        use std::io::{self, Read, Write};
        use std::os::windows::io::AsRawHandle;
        use std::time::{Duration, Instant};
        use windows_sys::Win32::Foundation::{ERROR_BROKEN_PIPE, ERROR_PIPE_BUSY, HANDLE};
        use windows_sys::Win32::System::Pipes::{PeekNamedPipe, WaitNamedPipeW};

        /// A connection to the simulator's pipe, with a short read timeout
        /// (`wait`) like the other links. Reads wait by peeking, as a pipe
        /// opened for plain reads would block until data comes.
        pub struct PipeLink {
            file: File,
            wait: Duration,
        }

        impl PipeLink {
            pub fn open(path: &str, wait: Duration) -> io::Result<Self> {
                let name = path.replace('/', "\\");
                // Busy also for a moment after the last client left, until
                // the simulator listens again: waited for, a second at most.
                let until = Instant::now() + Duration::from_secs(1);
                loop {
                    match OpenOptions::new().read(true).write(true).open(&name) {
                        Ok(file) => return Ok(PipeLink { file, wait }),
                        Err(e) if e.raw_os_error() == Some(ERROR_PIPE_BUSY as i32) => {
                            if Instant::now() >= until {
                                return Err(io::Error::new(
                                    io::ErrorKind::ResourceBusy,
                                    format!(
                                        "{path} is in use (the simulator takes one connection at a time, like a serial port)"
                                    ),
                                ));
                            }
                            let wide: Vec<u16> = name.encode_utf16().chain([0]).collect();
                            unsafe { WaitNamedPipeW(wide.as_ptr(), 100) };
                        }
                        Err(e) => return Err(e),
                    }
                }
            }

            /// Bytes waiting, or an error when the simulator closed its end.
            fn available(&self) -> io::Result<u32> {
                let mut n = 0u32;
                let ok = unsafe {
                    PeekNamedPipe(
                        self.file.as_raw_handle() as HANDLE,
                        std::ptr::null_mut(),
                        0,
                        std::ptr::null_mut(),
                        &mut n,
                        std::ptr::null_mut(),
                    )
                };
                if ok == 0 {
                    let e = io::Error::last_os_error();
                    return Err(if e.raw_os_error() == Some(ERROR_BROKEN_PIPE as i32) {
                        io::Error::new(io::ErrorKind::BrokenPipe, "device disconnected")
                    } else {
                        e
                    });
                }
                Ok(n)
            }
        }

        impl Read for PipeLink {
            fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
                let until = Instant::now() + self.wait;
                loop {
                    let n = self.available()?;
                    if n > 0 {
                        let want = buf.len().min(n as usize);
                        return match self.file.read(&mut buf[..want])? {
                            0 => Err(io::Error::new(io::ErrorKind::BrokenPipe, "device disconnected")),
                            got => Ok(got),
                        };
                    }
                    if Instant::now() >= until {
                        return Err(io::ErrorKind::TimedOut.into());
                    }
                    std::thread::sleep(Duration::from_millis(1));
                }
            }
        }

        impl Write for PipeLink {
            fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
                self.file.write(buf).map_err(|e| {
                    if e.raw_os_error() == Some(ERROR_BROKEN_PIPE as i32) || e.raw_os_error() == Some(232) {
                        io::Error::new(io::ErrorKind::BrokenPipe, "device disconnected")
                    } else {
                        e
                    }
                })
            }
            fn flush(&mut self) -> io::Result<()> {
                self.file.flush()
            }
        }
    }

    #[cfg(test)]
    mod tests {
        #[test]
        fn pipe_names() {
            assert!(super::is_pipe(r"\\.\pipe\esp32-sim"));
            assert!(super::is_pipe("//./pipe/esp32-sim"));
            assert!(!super::is_pipe("COM3"));
            assert!(!super::is_pipe("/tmp/esp32-sim"));
        }
    }
}

#[cfg(unix)]
mod pty {
    use std::fs::{File, OpenOptions};
    use std::io::{self, Read, Write};
    use std::os::unix::fs::OpenOptionsExt;
    use std::os::unix::io::AsRawFd;

    /// A tty opened in raw mode with a short read timeout (`wait`), mirroring
    /// how the serialport-based link behaves.
    pub struct RawTty {
        file: File,
        wait_ms: i32,
    }

    /// Is `path` (or what it links to) a pseudo-terminal: /dev/pts/N on
    /// Linux, /dev/ttysNNN on macOS?
    pub fn is_pty(path: &str) -> bool {
        let Ok(real) = std::fs::canonicalize(path) else { return false };
        let real = real.to_string_lossy();
        let number = |rest: &str| !rest.is_empty() && rest.bytes().all(|b| b.is_ascii_digit());
        real.strip_prefix("/dev/pts/").is_some_and(number) || real.strip_prefix("/dev/ttys").is_some_and(number)
    }

    impl RawTty {
        pub fn open(path: &str, wait: std::time::Duration) -> io::Result<Self> {
            let file = OpenOptions::new().read(true).write(true).custom_flags(libc::O_NOCTTY).open(path)?;
            let fd = file.as_raw_fd();
            unsafe {
                if libc::isatty(fd) == 0 {
                    return Err(io::Error::other("not a tty"));
                }
                // One program at a time, as on a serial port (serialport
                // opens those exclusively) and the simulator's pipe on
                // Windows: two readers would each get part of the replies.
                // The lock goes with the file, also when a program dies.
                if libc::flock(fd, libc::LOCK_EX | libc::LOCK_NB) != 0 {
                    let e = io::Error::last_os_error();
                    if e.raw_os_error() == Some(libc::EWOULDBLOCK) {
                        return Err(io::Error::new(
                            io::ErrorKind::ResourceBusy,
                            format!(
                                "{path} is in use by another program, such as the studio \
                                 (the simulator takes one connection at a time, like a serial port)"
                            ),
                        ));
                    }
                }
                let mut t: libc::termios = std::mem::zeroed();
                if libc::tcgetattr(fd, &mut t) != 0 {
                    return Err(io::Error::last_os_error());
                }
                libc::cfmakeraw(&mut t);
                if libc::tcsetattr(fd, libc::TCSANOW, &t) != 0 {
                    return Err(io::Error::last_os_error());
                }
            }
            Ok(RawTty { file, wait_ms: wait.as_millis().clamp(1, 60_000) as i32 })
        }
    }

    impl Drop for RawTty {
        /// Let go of the link by name, not only by closing the file: a
        /// process this program is starting just then (between its fork
        /// and its exec) still has the file open, and with it the lock.
        fn drop(&mut self) {
            unsafe {
                libc::flock(self.file.as_raw_fd(), libc::LOCK_UN);
            }
        }
    }

    impl Read for RawTty {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            let mut pfd = libc::pollfd { fd: self.file.as_raw_fd(), events: libc::POLLIN, revents: 0 };
            let r = unsafe { libc::poll(&mut pfd, 1, self.wait_ms) };
            if r < 0 {
                return Err(io::Error::last_os_error());
            }
            if r == 0 || pfd.revents & libc::POLLIN == 0 {
                if pfd.revents & (libc::POLLHUP | libc::POLLERR) != 0 {
                    return Err(io::Error::new(io::ErrorKind::BrokenPipe, "device disconnected"));
                }
                return Err(io::ErrorKind::TimedOut.into());
            }
            // After a hang-up (simulator exited), macOS reports POLLIN|POLLHUP and
            // read() returns 0 = end of file. Callers treat Ok(0) as "no data yet",
            // so report it as a disconnect instead. Pending data is still returned first.
            match self.file.read(buf)? {
                0 => Err(io::Error::new(io::ErrorKind::BrokenPipe, "device disconnected")),
                n => Ok(n),
            }
        }
    }

    impl Write for RawTty {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            self.file.write(buf)
        }
        fn flush(&mut self) -> io::Result<()> {
            self.file.flush()
        }
    }

    #[cfg(test)]
    mod tests {
        use super::RawTty;
        use std::io::{ErrorKind, Read};

        /// A PTY pair like the simulator's: returns (master fd, slave path).
        fn openpty() -> (i32, String) {
            unsafe {
                let m = libc::posix_openpt(libc::O_RDWR | libc::O_NOCTTY);
                assert!(m >= 0 && libc::grantpt(m) == 0 && libc::unlockpt(m) == 0);
                let name = std::ffi::CStr::from_ptr(libc::ptsname(m)).to_string_lossy().into_owned();
                (m, name)
            }
        }

        #[test]
        fn idle_read_times_out() {
            let (m, path) = openpty();
            let mut t = RawTty::open(&path, std::time::Duration::from_millis(20)).unwrap();
            let mut buf = [0u8; 16];
            assert_eq!(t.read(&mut buf).unwrap_err().kind(), ErrorKind::TimedOut);
            unsafe { libc::close(m) };
        }

        #[test]
        fn hangup_is_reported_as_disconnect() {
            // The simulator exiting (e.g. window closed) closes the PTY master.
            let (m, path) = openpty();
            let mut t = RawTty::open(&path, std::time::Duration::from_millis(20)).unwrap();
            unsafe { libc::close(m) };
            let mut buf = [0u8; 16];
            let err = t.read(&mut buf).expect_err("a closed PTY must not look like an idle link");
            assert_eq!(err.kind(), ErrorKind::BrokenPipe);
        }

        #[test]
        fn one_program_at_a_time() {
            // Two programs on one link would each read part of the replies.
            let (m, path) = openpty();
            let first = RawTty::open(&path, std::time::Duration::from_millis(20)).unwrap();
            let err = RawTty::open(&path, std::time::Duration::from_millis(20))
                .err()
                .expect("a second open while the first holds the link");
            assert_eq!(err.kind(), ErrorKind::ResourceBusy);
            drop(first);
            RawTty::open(&path, std::time::Duration::from_millis(20)).expect("free again once the first let go");
            unsafe { libc::close(m) };
        }
    }
}
