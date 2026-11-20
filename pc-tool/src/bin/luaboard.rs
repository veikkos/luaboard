//! `luaboard` - command line client for the ESP32 Lua devboard.
//!
//!   luaboard ports
//!   luaboard install marketplace/dashboard --run
//!   luaboard -p /tmp/esp32-sim put marketplace/dashboard/*.lua --run main.lua
//!   luaboard eval "adc.read(adc.PORTB_IN)"
//!   luaboard monitor
//!   luaboard update build/luaboard-m5stack-cores3.bin

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use luaboard::{Device, Msg, DEFAULT_BAUD, SIM_LINK};
use std::io::{IsTerminal, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

#[derive(Parser)]
#[command(version, about = "Upload and control Lua scripts on the ESP32 Lua devboard")]
struct Cli {
    /// Serial port or tcp://host:port for Wi-Fi (default: $LUABOARD_PORT, else the first running
    /// simulator: /tmp/esp32-sim, /tmp/esp32-sim-2, ..., on Windows \\.\pipe\esp32-sim, ...)
    #[arg(short, long, global = true)]
    port: Option<String>,
    #[arg(short, long, global = true, default_value_t = DEFAULT_BAUD)]
    baud: u32,
    /// After starting a script, or handing over a browser login: keep showing
    /// the device's output this many seconds (default 0.3; 1.5 after a login)
    #[arg(long, global = true, value_name = "SECS")]
    linger: Option<f64>,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum WifiAction {
    /// Store the network name and password on the device and connect
    Set { ssid: String, password: Option<String> },
    /// Forget the network; Wi-Fi off
    Off,
}

#[derive(Subcommand)]
enum SecretAction {
    /// Store a secret (an API key, a token) for the app's scripts: secrets.get(NAME)
    Set { app: String, name: String, value: String },
    /// Delete one secret of an app (e.g. log it out of a service), or without NAME all of them
    Rm { app: String, name: Option<String> },
    /// Store the secrets.env of an app folder, or of every app in a folder of apps (such as
    /// marketplace), for the apps the device has; the others are skipped. Other secrets stay.
    Push { folder: PathBuf },
}

#[derive(Subcommand)]
enum PairAction {
    /// Unpair: the device forgets its key and takes no commands over Wi-Fi
    Off,
}

#[derive(Subcommand)]
enum Cmd {
    /// List serial ports and devices found on the network (mDNS)
    Ports,
    /// Show Wi-Fi status, or set/forget the network the device joins
    Wifi {
        #[command(subcommand)]
        action: Option<WifiAction>,
    },
    /// Check the device answers
    Ping,
    /// Do the browser login a script asks for (OAuth): open the browser, hand the result to the device
    Browser {
        /// Wait this many seconds for a script to ask, if none is waiting yet
        #[arg(long, default_value_t = 0)]
        wait: u64,
        /// Don't open the browser; print the URL to open by hand
        #[arg(long)]
        no_open: bool,
    },
    /// List the apps' secrets on the device (names only, never values), or set / delete them
    Secrets {
        #[command(subcommand)]
        action: Option<SecretAction>,
    },
    /// Firmware / storage / heap information
    Info,
    /// The last crash, if the device restarted after one: its cause and stack
    /// trace, with functions and lines when the build's ELF is at hand
    Crash {
        /// The firmware's ELF from that build (default: build/idf-<board>/luaboard.elf)
        #[arg(long)]
        elf: Option<PathBuf>,
    },
    /// List the files on the device (<app>/<file>)
    Ls,
    /// List the apps on the device: installed (flash) or only uploaded (RAM)
    Apps,
    /// Install a project folder as an app (its *.lua files; app name = folder name),
    /// or write an app already uploaded to the device's RAM to flash
    Install {
        /// Project folder, or the name of an app on the device
        dir: PathBuf,
        /// App name (default: the folder's name)
        #[arg(long)]
        app: Option<String>,
        /// Start it afterwards
        #[arg(long)]
        run: bool,
        /// Start it at boot instead of the menu
        #[arg(long)]
        autorun: bool,
        /// Install even if a file has a syntax error
        #[arg(long)]
        no_check: bool,
    },
    /// Delete an app from the device (RAM and flash)
    Uninstall { app: String },
    /// Check .lua files for syntax errors (no device needed)
    Check {
        #[arg(required = true)]
        files: Vec<PathBuf>,
    },
    /// Upload files into an app, to RAM (try it out; `install` keeps it)
    Put {
        #[arg(required = true)]
        files: Vec<PathBuf>,
        /// App to put them in (default: the name of the first file's folder)
        #[arg(long)]
        app: Option<String>,
        /// Upload even if a file has a syntax error
        #[arg(long)]
        no_check: bool,
        /// Start this script of the app after uploading (e.g. main.lua)
        #[arg(long)]
        run: Option<String>,
        /// Install the app (write it to flash) after uploading
        #[arg(long)]
        save: bool,
    },
    /// Download a file from the device (<app>/<file>)
    Get {
        name: String,
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    /// Delete a file (<app>/<file>)
    Rm { name: String },
    /// (Re)start an app (<app>), or one of its scripts (<app>/<file>.lua)
    Run { name: Option<String> },
    /// Stop the running script
    Stop,
    /// Install every app that has uploads in RAM
    Save,
    /// Delete all apps (RAM and flash) and secrets
    Format,
    /// Show or set the app started at boot instead of the menu (<app> | off)
    Autorun { app: Option<String> },
    /// Show the device's clock (set from the network once Wi-Fi is up)
    Time {
        /// Ask the time server now (it's also refreshed every 6 h by itself)
        #[arg(long)]
        sync: bool,
    },
    /// Show or set the device's time zone: a name (Europe/Helsinki, helsinki,
    /// new york, UTC, ...) or a POSIX TZ string (EET-2EEST,M3.5.0/3,M10.5.0/4)
    Tz {
        zone: Option<String>,
        /// List the zone names this tool knows
        #[arg(long)]
        list: bool,
    },
    /// Evaluate a Lua expression/statement on the device
    Eval { code: Vec<String> },
    /// Pair this computer with the device for Wi-Fi (over USB), or show the pairing (any link)
    Pair {
        /// Make a new key: every other paired computer has to pair again
        #[arg(long)]
        new: bool,
        #[command(subcommand)]
        action: Option<PairAction>,
    },
    /// Flash a firmware image (the merged .bin from the `fw` build) over USB
    Flash {
        image: PathBuf,
        /// Only check the image, don't touch the device
        #[arg(long)]
        check: bool,
    },
    /// Update the firmware over the device's link: USB, Wi-Fi, or a running
    /// simulator. Apps, settings and secrets stay. If the new firmware keeps
    /// crashing, the device goes back to the old one.
    Update {
        /// The merged .bin from the `fw` build (a device), or the simulator
        /// executable (build/<board>/esp32-sim) for the simulator
        image: PathBuf,
        /// Only check the image against the device, send nothing
        #[arg(long)]
        check: bool,
    },
    /// Erase all data on the device over USB: apps, secrets, Wi-Fi, pairing,
    /// settings. Clears the flash itself (FORMAT only marks things deleted);
    /// the firmware stays. Use before giving the device away.
    Erase {
        /// Don't ask for confirmation
        #[arg(long)]
        yes: bool,
    },
    /// Save the device's screen as an image (PPM)
    Screenshot { file: PathBuf },
    /// Touch the device from here: X Y (a point on the screen, 320x240),
    /// A, B or C (a touch button), or power (a short press of the power key)
    Touch {
        #[arg(num_args = 1..=2, value_name = "X Y | A | B | C | power")]
        at: Vec<String>,
        /// Keep the finger down this many milliseconds (a long press: 800)
        #[arg(long, default_value_t = 100)]
        hold: u64,
    },
    /// Print device output (LOG/EVT) until Ctrl-C or --secs
    Monitor {
        #[arg(long)]
        secs: Option<u64>,
    },
    /// Lend the device this computer's network over USB, for when it has no
    /// Wi-Fi, and print its output (LOG/EVT) until Ctrl-C
    Tether {
        /// Let the device's apps connect to this computer's own addresses
        /// (127.0.0.1, localhost), not only to other computers
        #[arg(long)]
        allow_local: bool,
    },
    /// Send control lines to a simulator started with --control (the run
    /// target's is on): its --set keys, advance=MS to fast-forward its clock,
    /// quit=1 to power it off. Each waits until it has taken effect.
    ///
    /// Example: luaboard sim-ctl button=B advance=1000 button=-
    SimCtl {
        /// The simulator's control port, [HOST:]PORT
        #[arg(long, env = "LUABOARD_SIM_CONTROL", default_value = luaboard::simctl::DEFAULT)]
        control: String,
        /// KEY=VALUE lines, one per argument (quote one with spaces)
        #[arg(required = true)]
        lines: Vec<String>,
    },
    /// Pass bytes between stdin/stdout and the device's serial link, unchanged
    /// (protocol debugging, tests). Ends when stdin closes, once the device
    /// has been quiet for --linger seconds (default 0.3)
    Raw,
}

/// `raw`: stdin to the link and the link to stdout, byte for byte.
fn raw(port: &str, baud: u32, linger: f64) -> Result<()> {
    use std::io::{Read, Write};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{mpsc, Arc};
    // (a short wait in each read: what comes on stdin goes out between them)
    let mut link = luaboard::open_link_poll(port, baud, Duration::from_millis(2))?;
    let done = Arc::new(AtomicBool::new(false));
    let (tx, rx) = mpsc::channel::<Vec<u8>>();
    {
        let done = done.clone();
        std::thread::spawn(move || {
            let mut buf = [0u8; 4096];
            let mut stdin = std::io::stdin().lock();
            while let Ok(n) = stdin.read(&mut buf) {
                if n == 0 || tx.send(buf[..n].to_vec()).is_err() {
                    break;
                }
            }
            done.store(true, Ordering::SeqCst);
        });
    }
    let mut out = std::io::stdout().lock();
    let mut buf = [0u8; 4096];
    let mut quiet_since = Instant::now();
    loop {
        // Read before draining: once stdin has ended, everything it sent is queued.
        let stdin_done = done.load(Ordering::SeqCst);
        while let Ok(data) = rx.try_recv() {
            // (what comes meanwhile is printed: the device may wait for that)
            luaboard::write_all_reading(&mut *link, &data, Duration::from_secs(3), &mut |b| {
                let _ = out.write_all(b);
            })?;
            quiet_since = Instant::now();
        }
        match link.read(&mut buf) {
            Ok(0) => {}
            Ok(n) => {
                out.write_all(&buf[..n])?;
                out.flush()?;
                quiet_since = Instant::now();
            }
            Err(e) if matches!(e.kind(), std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock) => {}
            Err(e) => return Err(e).context("device link"),
        }
        if stdin_done && quiet_since.elapsed().as_secs_f64() >= linger {
            return Ok(());
        }
    }
}

fn print_async(m: Msg) {
    match m {
        Msg::Log(s) => println!("{s}"),
        Msg::Evt(s) => eprintln!("[evt] {s}"),
        Msg::Other(s) if !s.is_empty() => eprintln!("[dev] {s}"),
        _ => {}
    }
}

fn resolve_port(p: Option<String>) -> String {
    p.or_else(|| std::env::var("LUABOARD_PORT").ok())
        .or_else(|| luaboard::sim_ports().into_iter().next())
        .unwrap_or_else(|| SIM_LINK.to_string())
}

fn file_name(p: &Path) -> Result<String> {
    Ok(p.file_name().context("not a file")?.to_string_lossy().into_owned())
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let port = resolve_port(cli.port);
    let open = || Device::open(&port, cli.baud);
    let a = &mut print_async;
    // Show what the device prints for a while (--linger, else `default` seconds).
    let linger = |dev: &mut Device, default: f64| -> Result<()> {
        let secs = cli.linger.unwrap_or(default);
        for m in dev.poll(Duration::try_from_secs_f64(secs).unwrap_or_default())? {
            print_async(m);
        }
        Ok(())
    };

    match cli.cmd {
        // Commands that don't talk to the device through Device first.
        Cmd::Ports => {
            for p in luaboard::list_ports() {
                println!("{p}");
            }
            for d in luaboard::wifi::discover(Duration::from_millis(1500)) {
                println!(
                    "{:<28} {} (Wi-Fi, {} {})",
                    d.port,
                    d.name,
                    d.board.as_deref().unwrap_or("?"),
                    d.fw.as_deref().unwrap_or("")
                );
            }
        }
        Cmd::Check { files } => check_files(&files)?,
        Cmd::SimCtl { control, lines } => {
            let answers = luaboard::simctl::send(&control, &lines)?;
            let refused: Vec<&str> = lines
                .iter()
                .zip(&answers)
                .filter(|(_, a)| **a == luaboard::simctl::Answer::Error)
                .map(|(l, _)| l.as_str())
                .collect();
            if !refused.is_empty() {
                bail!("the simulator refused: {}", refused.join(", "));
            }
        }
        Cmd::Tz { list: true, .. } => {
            for (n, tz) in luaboard::timezone::ZONES {
                println!("{n:<22} {tz}");
            }
        }
        Cmd::Flash { image, check } => flash(&port, &image, check)?,
        Cmd::Erase { yes } => erase(&port, yes)?,
        Cmd::Update { image, check } => update(&port, cli.baud, &image, check)?,
        Cmd::Raw => raw(&port, cli.baud, cli.linger.unwrap_or(0.3))?,

        // The rest open the device.
        Cmd::Tz { zone, list: false } => {
            // A name we know, else a POSIX TZ string (it has an offset, so a
            // digit; its rules may have '/' too: EET-2EEST,M3.5.0/3,M10.5.0/4).
            let posix = match zone.as_deref() {
                None => None,
                Some(z) => match luaboard::timezone::lookup(z) {
                    Some(tz) => Some(tz.to_string()),
                    None if z.chars().any(|c| c.is_ascii_digit()) => Some(z.to_string()),
                    None => {
                        bail!("unknown time zone '{z}': see `luaboard tz --list`, or give a POSIX TZ string")
                    }
                },
            };
            let mut dev = open()?;
            let tz = dev.timezone(posix.as_deref(), a)?;
            match luaboard::timezone::name_of(&tz) {
                Some(n) => println!("{n} ({tz})"),
                None => println!("{tz}"),
            }
        }
        Cmd::Browser { wait, no_open } => {
            let mut dev = open()?;
            browser_login(&mut dev, wait, no_open)?;
            linger(&mut dev, 1.5)?;
        }
        Cmd::Secrets { action } => {
            let mut dev = open()?;
            match action {
                None => {
                    let list = dev.secrets(a)?;
                    for (app, n, size) in &list {
                        println!("{app:<23} {n:<16} {size:>5} B");
                    }
                    println!("{} secret(s)", list.len());
                }
                Some(SecretAction::Set { app, name, value }) => {
                    dev.secret_set(&app, &name, value.as_bytes(), a)?;
                    println!("stored {app} {name}");
                }
                Some(SecretAction::Rm { app, name: Some(name) }) => {
                    dev.secret_rm(&app, &name, a)?;
                    println!("deleted {app} {name}");
                }
                Some(SecretAction::Rm { app, name: None }) => {
                    dev.secrets_clear(&app, a)?;
                    println!("deleted all secrets of {app}");
                }
                Some(SecretAction::Push { folder }) => secrets_push(&mut dev, &folder, a)?,
            }
        }
        Cmd::Pair { new, action } => {
            let mut dev = open()?;
            let over_wifi = luaboard::wifi::parse_tcp_port(&port).is_some();
            match action {
                Some(PairAction::Off) => {
                    dev.unpair(a)?;
                    println!("unpaired: the device takes no commands over Wi-Fi until it is paired again");
                }
                None if over_wifi && !new => {}
                None => {
                    if over_wifi {
                        bail!("pairing only works over USB");
                    }
                    let id = dev.pair(new, a)?;
                    println!("paired: key {id} saved in {}", luaboard::keys::path().display());
                }
            }
            let st = dev.pair_status(a)?;
            match (&st.id, st.here) {
                (Some(id), true) => println!("Wi-Fi: paired (key {id}), and this computer has the key"),
                (Some(id), false) => {
                    println!("Wi-Fi: paired (key {id}), but not with this computer: run `luaboard pair` over USB")
                }
                (None, _) => {
                    println!("Wi-Fi: not paired (the Wi-Fi link takes no commands): run `luaboard pair` over USB")
                }
            }
        }
        Cmd::Wifi { action } => {
            let mut dev = open()?;
            let st = match action {
                None => dev.wifi_status(a)?,
                Some(WifiAction::Set { ssid, password }) => {
                    dev.wifi_set(&ssid, password.as_deref().unwrap_or(""), a)?
                }
                Some(WifiAction::Off) => dev.wifi_off(a)?,
            };
            println!("   state: {}", st.state);
            println!("    ssid: {}", st.ssid.as_deref().unwrap_or("-"));
            if let Some(ip) = &st.ip {
                println!("      ip: {ip}  (connect with: -p tcp://{ip}:{})", st.port);
                println!("    rssi: {} dBm", st.rssi);
            }
            if st.state != "off" && !dev.pair_status(a)?.here {
                println!("note: to use the device over Wi-Fi, pair this computer with it: luaboard pair");
            }
        }
        Cmd::Ping => {
            let mut dev = open()?;
            dev.ping(a)?;
            println!("PONG from {port}");
        }
        Cmd::Info => {
            let mut dev = open()?;
            for kv in dev.info(a)?.split_whitespace() {
                let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
                println!("{k:>8}: {v}");
            }
        }
        Cmd::Crash { elf } => {
            let mut dev = open()?;
            match dev.crash(a)? {
                None => println!(
                    "no crash: the last reset was {}",
                    luaboard::kv_field(&dev.info(a)?, "reset").unwrap_or("?")
                ),
                Some(c) => luaboard::crash::describe(&c, elf.as_deref()).iter().for_each(|l| println!("{l}")),
            }
        }
        Cmd::Ls => {
            let mut dev = open()?;
            let files = dev.list(a)?;
            for f in &files {
                println!("{:<24} {:>6} B  crc {:08x}", f.name, f.size, f.crc);
            }
            println!("{} file(s)", files.len());
        }
        Cmd::Apps => {
            let mut dev = open()?;
            let apps = dev.apps(a)?;
            for p in &apps {
                let state = match p.state {
                    luaboard::AppState::Installed => "installed",
                    luaboard::AppState::Ram => "in RAM only (not installed)",
                    luaboard::AppState::Modified => "installed, newer uploads in RAM",
                };
                println!("{:<24} {:>3} file(s) {:>7} B  {state}", p.name, p.files, p.bytes);
            }
            println!("{} app(s)", apps.len());
        }
        Cmd::Install { dir, app, run, autorun, no_check } => {
            if !no_check && dir.is_dir() {
                check_files(&project_files(&dir)?).context("nothing installed (use --no-check to install anyway)")?;
            }
            let mut dev = open()?;
            let app = if dir.is_dir() {
                let app = app_arg(app, &dir)?;
                let mut files = Vec::new();
                for f in project_files(&dir)? {
                    let data = std::fs::read(&f).with_context(|| format!("reading {}", f.display()))?;
                    files.push((file_name(&f)?, data));
                }
                if files.is_empty() {
                    bail!("no .lua files in {}", dir.display());
                }
                luaboard::stamp_dev_version(&dir, &mut files);
                for (n, d) in &files {
                    println!("{app}/{n} ({} B)", d.len());
                }
                push_secrets(&mut dev, &app, &dir, a)?;
                println!("{}", dev.install_project(&app, &files, a)?);
                app
            } else {
                // Not a folder here: an app already uploaded to the device.
                let name = dir.to_string_lossy().into_owned();
                if !luaboard::valid_app_name(&name) {
                    bail!("{name}: no such folder");
                }
                println!(
                    "{}",
                    dev.install(&name, a)
                        .with_context(|| format!("{name}: no such folder, nor an app on the device"))?
                );
                name
            };
            if autorun {
                println!("{}", dev.autorun(Some(&app), a)?);
            }
            if run {
                // Installing has already started an app that was on the
                // screen. Starting it once more would end that start in the
                // middle of what it does first: a login it was renewing
                // would be lost with the reply.
                let main = format!("{app}/main.lua");
                let info = dev.info(a)?;
                if luaboard::kv_field(&info, "running") == Some(main.as_str()) {
                    println!("RUNNING {main}");
                } else {
                    println!("{}", dev.run(Some(&app), a)?);
                }
                linger(&mut dev, 0.3)?;
            }
        }
        Cmd::Uninstall { app } => {
            let mut dev = open()?;
            println!("{}", dev.uninstall(&app, a)?);
        }
        Cmd::Touch { at, hold } => {
            let mut dev = open()?;
            let point = match at.iter().map(|s| s.to_ascii_lowercase()).collect::<Vec<_>>().as_slice() {
                [p] if p == "power" => None,
                [b] if b == "a" => Some((65, 260)),
                [b] if b == "b" => Some((165, 260)),
                [b] if b == "c" => Some((260, 260)),
                [x, y] => match (x.parse::<u32>(), y.parse::<u32>()) {
                    (Ok(x), Ok(y)) => Some((x, y)),
                    _ => bail!("touch: X and Y are numbers"),
                },
                _ => bail!("touch: X Y, A, B, C or power"),
            };
            if let Some(p) = point {
                dev.touch(Some(p), a)?;
                // Held: say so again before the device's 1 s timeout lets go.
                let until = Instant::now() + Duration::from_millis(hold);
                while let Some(left) = until.checked_duration_since(Instant::now()) {
                    std::thread::sleep(left.min(Duration::from_millis(300)));
                    if Instant::now() < until {
                        dev.touch(Some(p), a)?;
                    }
                }
                dev.touch(None, a)?;
            } else {
                dev.key_power(a)?;
            }
        }
        Cmd::Screenshot { file } => {
            let mut dev = open()?;
            let f = dev.framebuffer(a)?;
            let mut out = format!("P6\n{} {}\n255\n", f.width, f.height).into_bytes();
            for y in 0..f.height {
                for x in 0..f.width {
                    let (r, g, b) = f.rgb(x, y);
                    out.extend_from_slice(&[r, g, b]);
                }
            }
            std::fs::write(&file, out).with_context(|| format!("writing {}", file.display()))?;
            println!("saved {} ({}x{})", file.display(), f.width, f.height);
        }
        Cmd::Put { files, app, run, save, no_check } => {
            if !no_check {
                check_files(&files).context("nothing uploaded (use --no-check to upload anyway)")?;
            }
            let mut dev = open()?;
            let dir = luaboard::folder_of(&files[0]);
            let app = app_arg(app, dir)?;
            push_secrets(&mut dev, &app, dir, a)?;
            for f in &files {
                let data = std::fs::read(f).with_context(|| format!("reading {}", f.display()))?;
                let name = format!("{app}/{}", file_name(f)?);
                dev.put(&name, &data, a)?;
                println!("uploaded {name} ({} B)", data.len());
            }
            if save {
                println!("{}", dev.install(&app, a)?);
            }
            if let Some(entry) = run {
                let entry = if entry.contains('/') { entry } else { format!("{app}/{entry}") };
                println!("{}", dev.run(Some(&entry), a)?);
                linger(&mut dev, 0.3)?; // the script's first output
            }
        }
        Cmd::Get { name, output } => {
            let mut dev = open()?;
            let data = dev.get(&name, a)?;
            match output {
                Some(p) => std::fs::write(&p, &data)?,
                // To a terminal: as text, without control characters (a file
                // could rewrite the terminal); into a pipe: the bytes as they are.
                None if std::io::stdout().is_terminal() => {
                    for line in String::from_utf8_lossy(&data).split_inclusive('\n') {
                        let (text, nl) = line.strip_suffix('\n').map_or((line, ""), |t| (t, "\n"));
                        print!("{}{nl}", luaboard::printable(text.trim_end_matches('\r')));
                    }
                }
                None => std::io::stdout().write_all(&data)?,
            }
        }
        Cmd::Rm { name } => {
            let mut dev = open()?;
            dev.remove(&name, a)?;
            println!("removed {name}");
        }
        Cmd::Run { name } => {
            let mut dev = open()?;
            println!("{}", dev.run(name.as_deref(), a)?);
            linger(&mut dev, 0.3)?;
        }
        Cmd::Stop => {
            let mut dev = open()?;
            dev.stop(a)?;
            println!("stopped");
        }
        Cmd::Save => {
            let mut dev = open()?;
            println!("{}", dev.save(a)?);
        }
        Cmd::Format => {
            let mut dev = open()?;
            eprintln!("formatting: erasing the apps partition can take a few minutes");
            dev.format(a)?;
            println!("formatted");
        }
        Cmd::Autorun { app } => {
            let mut dev = open()?;
            println!("{}", dev.autorun(app.as_deref(), a)?);
        }
        Cmd::Time { sync } => {
            let mut dev = open()?;
            if sync {
                let syncs = |t: &str| luaboard::kv_field(t, "syncs").map(str::to_string);
                let before = syncs(&dev.time(a)?);
                dev.time_sync(a)?;
                let deadline = std::time::Instant::now() + Duration::from_secs(10);
                loop {
                    if syncs(&dev.time(a)?) != before {
                        println!("synced");
                        break;
                    }
                    if std::time::Instant::now() > deadline {
                        bail!("no answer from the time server (pool.ntp.org) in 10 s");
                    }
                    std::thread::sleep(Duration::from_millis(50));
                }
            }
            let reply = dev.time(a)?;
            let get = |k: &str| luaboard::kv_field(&reply, k).unwrap_or("-");
            let tz = get("tz");
            let zone = luaboard::timezone::name_of(tz).map(|n| format!("{n}, ")).unwrap_or_default();
            if get("synced") == "1" {
                let from = match get("source") {
                    "rtc" => ", from the RTC chip: no NTP yet",
                    "usb" => ", from the computer over USB: no NTP yet",
                    _ => "",
                };
                println!("{}  ({zone}{tz}{from})", get("local").replacen('T', " ", 1));
            } else {
                println!("not set yet: the device takes the time from the network once Wi-Fi is up ({zone}{tz})");
            }
        }
        Cmd::Eval { code } => {
            let mut dev = open()?;
            let r = dev.eval(&code.join(" "), a)?;
            if !r.is_empty() {
                println!("{r}");
            }
        }
        Cmd::Monitor { secs } => {
            let mut dev = open()?;
            let end = secs.map(|s| Instant::now() + Duration::from_secs(s));
            eprintln!("monitoring {port} (Ctrl-C to quit)");
            loop {
                for m in dev.poll(Duration::from_millis(200))? {
                    print_async(m);
                }
                if end.is_some_and(|e| Instant::now() >= e) {
                    break;
                }
            }
        }
        Cmd::Tether { allow_local } => {
            if luaboard::wifi::parse_tcp_port(&port).is_some() {
                bail!("the tether works only over USB (the device has Wi-Fi when it's reached over it)");
            }
            let mut dev = open()?;
            let mut relay = luaboard::tether::Relay::new(allow_local);
            eprintln!("lending {port} this computer's network (Ctrl-C to quit)");
            loop {
                // (a turn at once while bytes move, else every 30 ms)
                let wait = Duration::from_millis(if relay.busy() { 2 } else { 30 });
                for m in relay.pump(&mut dev, wait)? {
                    print_async(m);
                }
            }
        }
    }
    Ok(())
}

/// Handle a script's browser-login request from the terminal.
fn browser_login(dev: &mut Device, wait: u64, no_open: bool) -> Result<()> {
    use luaboard::login;
    let mut req = dev.browser_pending(&mut print_async)?;
    let end = Instant::now() + Duration::from_secs(wait);
    while req.is_none() && Instant::now() < end {
        for m in dev.poll(Duration::from_millis(50))? {
            if let Msg::Evt(e) = &m {
                if let Some(r) = login::parse_event(e) {
                    req = Some(r);
                }
            }
            print_async(m);
        }
    }
    let Some(req) = req else { bail!("no script is asking for a login") };
    println!("The app '{}' on the device asks you to log in to {} at {}", req.app, req.title, req.host());
    if no_open {
        println!("Open this in a browser on this computer:\n  {}", req.url);
    } else {
        println!("Opening the browser... (if it doesn't open: {})", req.url);
    }
    let stop = std::sync::atomic::AtomicBool::new(false);
    match login::catch_redirect(&req, !no_open, &stop, Duration::from_secs(600)) {
        Ok(q) => {
            dev.browser_answer(req.id, &q, &mut print_async)?;
            println!("Login handed to the device.");
            Ok(())
        }
        Err(e) => {
            let _ = dev.browser_cancel(req.id, &mut print_async);
            Err(e)
        }
    }
}

/// The app name: given, or the folder's name.
fn app_arg(app: Option<String>, dir: &Path) -> Result<String> {
    let app = app.unwrap_or_else(|| luaboard::app_name_for(dir));
    if !luaboard::valid_app_name(&app) {
        bail!("'{app}' can't be an app name: 1..23 characters, A-Z a-z 0-9 _ . - (use --app NAME)");
    }
    Ok(app)
}

/// `secrets push`: each app folder's secrets.env into its app on the device,
/// for the installed apps. One line per app (names, never values); a broken
/// file is reported and the rest go on, failing at the end.
fn secrets_push(dev: &mut Device, folder: &Path, a: &mut dyn FnMut(Msg)) -> Result<()> {
    let dirs = luaboard::app_folders(folder)?;
    let installed: Vec<String> =
        dev.apps(a)?.into_iter().filter(|p| !matches!(p.state, luaboard::AppState::Ram)).map(|p| p.name).collect();
    let mut failed = 0;
    for dir in &dirs {
        if !dir.join(luaboard::SECRETS_FILE).is_file() {
            continue;
        }
        let app = luaboard::app_name_for(dir);
        let secrets = match luaboard::project_secrets(dir) {
            Ok(s) => s,
            Err(e) => {
                println!("{app}: {e:#}");
                failed += 1;
                continue;
            }
        };
        if !installed.contains(&app) {
            println!("{app}: not installed, skipped");
            continue;
        }
        if let Err(e) = dev.set_secrets(&app, &secrets, a) {
            println!("{app}: {e:#}");
            failed += 1;
            continue;
        }
        let names: Vec<&str> = secrets.iter().map(|(n, _)| n.as_str()).collect();
        let s = if names.len() == 1 { "" } else { "s" };
        println!("{app}: {} secret{s} set ({})", names.len(), names.join(", "));
    }
    if failed > 0 {
        bail!("{failed} app(s) failed");
    }
    Ok(())
}

/// Put the project folder's secrets file (if any) into the app's secrets on the device.
fn push_secrets(dev: &mut Device, app: &str, dir: &Path, a: &mut dyn FnMut(Msg)) -> Result<()> {
    let secrets = luaboard::project_secrets(dir)?;
    if let Some(note) = dev.push_secrets(app, &secrets, a)? {
        println!("{note}");
    }
    for (name, _) in &secrets {
        println!("secret {app} {name} (from {})", luaboard::SECRETS_FILE);
    }
    Ok(())
}

/// The files of a project folder that go on the device.
fn project_files(dir: &Path) -> Result<Vec<PathBuf>> {
    let mut files: Vec<PathBuf> = std::fs::read_dir(dir)
        .with_context(|| format!("reading folder {}", dir.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| luaboard::is_project_file(p))
        .collect();
    files.sort();
    Ok(files)
}

/// Syntax-check Lua files with the firmware's parser. Prints each error.
fn check_files(files: &[PathBuf]) -> Result<()> {
    let mut bad = 0;
    for f in files {
        let data = std::fs::read(f).with_context(|| format!("reading {}", f.display()))?;
        let name = file_name(f)?;
        if !name.ends_with(".lua") {
            continue;
        }
        if let Err(e) = luaboard::syntax::check(&name, &data) {
            eprintln!("{e}");
            bad += 1;
        }
    }
    if bad > 0 {
        bail!("{bad} file(s) with syntax errors");
    }
    Ok(())
}

fn erase(port: &str, yes: bool) -> Result<()> {
    use luaboard::firmware::{self, FlashEvent};
    use std::io::{BufRead, Write};

    if !yes {
        print!(
            "This erases everything stored on the device at {port}: apps, secrets, Wi-Fi, pairing and settings.\n\
             The firmware stays. Type 'erase' to go on: "
        );
        std::io::stdout().flush()?;
        let mut answer = String::new();
        std::io::stdin().lock().read_line(&mut answer)?;
        if answer.trim() != "erase" {
            bail!("nothing erased");
        }
    }
    firmware::erase_data(
        port,
        &mut |ev| match ev {
            FlashEvent::Connecting => println!("connecting to {port} (entering download mode)..."),
            FlashEvent::Connected { chip } => println!("connected: {chip}"),
            FlashEvent::Done => println!("done, the device restarts as new"),
            _ => {}
        },
        &mut |label, size| println!("erasing {label} ({} KB)...", size / 1024),
    )
}

fn update(port: &str, baud: u32, path: &Path, check_only: bool) -> Result<()> {
    use luaboard::ota::{self, Event};

    let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    let img = ota::image(&bytes)?;
    println!("{}: {}", path.display(), img.describe());
    let mut dev = Device::open(port, baud)?;
    let a = &mut print_async;
    let info = dev.info(a)?;
    let field = |info: &str, k: &str| luaboard::kv_field(info, k).unwrap_or("?").to_string();
    println!("device: luaboard {} on {}", field(&info, "ver"), field(&info, "slot"));
    ota::check_for(&img, &info)?;
    if check_only {
        println!("image OK for this device");
        return Ok(());
    }
    let mut last_pct = usize::MAX;
    let tty = std::io::stdout().is_terminal();
    ota::update(
        dev,
        port,
        baud,
        &img,
        &mut |ev| match ev {
            Event::Sending { done, total } => {
                // A terminal gets a counter; a log every 25 %.
                let pct = done * 100 / total.max(1);
                let step = if tty { pct } else { pct / 25 * 25 };
                if step != last_pct {
                    last_pct = step;
                    if tty {
                        print!("\rsending {pct:3}%");
                    } else {
                        println!("sending {step:3}%");
                    }
                    let _ = std::io::stdout().flush();
                }
            }
            Event::Checking => println!("{}the device checks the image...", if tty { "\rsending 100%\n" } else { "" }),
            Event::Installed(v) => println!("installed {v}, the device reboots"),
            Event::Rebooted(info) => println!("done: running {} on {}", field(&info, "ver"), field(&info, "slot")),
        },
        a,
    )?;
    Ok(())
}

fn flash(port: &str, path: &Path, check_only: bool) -> Result<()> {
    use luaboard::firmware::{self, FlashEvent};
    use std::io::Write;

    let image = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    let info = firmware::inspect(&image)?;
    println!(
        "{}: {} {} for {} ({} KB, built {}, ESP-IDF {})",
        path.display(),
        info.project,
        info.version,
        info.chip,
        info.size / 1024,
        info.build_date,
        info.idf_version
    );
    firmware::check_for_device(&info)?;
    for r in firmware::flash_regions(&image)? {
        println!("  writes {:<16} at 0x{:06x}  {:>5} KB", r.name, r.offset, r.len.div_ceil(1024));
    }
    println!("  leaves data partitions untouched (installed apps, settings, Wi-Fi, pairing, secrets)");
    if check_only {
        println!("image OK");
        return Ok(());
    }
    let mut last_pct = usize::MAX;
    firmware::flash(port, &image, &mut |ev| match ev {
        FlashEvent::Connecting => println!("connecting to {port} (entering download mode)..."),
        FlashEvent::Connected { chip } => println!("connected: {chip}"),
        FlashEvent::Writing { done, total } => {
            let pct = done * 100 / total.max(1);
            if pct != last_pct {
                last_pct = pct;
                print!("\rwriting {pct:3}%");
                let _ = std::io::stdout().flush();
            }
        }
        FlashEvent::Verifying => println!("\nverifying..."),
        FlashEvent::Done => println!("done, device reset into the new firmware"),
    })
}
