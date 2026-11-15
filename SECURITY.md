# Security

luaboard tries to be secure, but it's a spare-time project and comes with no
warranty (see [LICENSE](LICENSE)).

## Reporting an issue

Please report security issues privately through the repository's Security
tab (Report a vulnerability) rather than as a public issue. Say what you
found and, if you can, how to reproduce it. The simulator is enough. You'll
be credited in the fix and its release notes, unless you'd rather not be.

## What it protects

Apps from each other. Scripts run in a sandboxed Lua VM with no file or OS
access, source code only (no bytecode), a memory cap and a watchdog. An app
can read only its own secrets, files and Bluetooth pairings, can't talk to
the device's own control port, and a browser login always shows which app
is asking.

The device from your network. The Wi-Fi link takes no commands until your
computer is paired with the device over USB, which gives it a random 256-bit
key. Every connection is encrypted and authenticated with that key in both
directions: others on the network can't read or change what goes over it,
can't pose as the device, and can't guess the key from what they record. A
connection without the key can't push out a client that has logged in.

Your computer from the device. The PC tools only open http(s) links for a
login and only save files under plain names.

## What it doesn't

Someone holding the device. USB is trusted, so you can never lock yourself
out, and the flash isn't encrypted. Whoever has the board can read the Wi-Fi
password, the apps' secrets and the Wi-Fi pairing key.

Someone with your computer's keys. The pairing keys are kept in a
[file](docs/PROTOCOL.md#wi-fi-link) readable only by you. Anyone who can read
that file can use your devices over Wi-Fi, including updating their
firmware (images aren't signed). If it leaks, flash the firmware again over
USB (`luaboard flash`), in case it was replaced, then pair again with
`luaboard pair --new` over USB, which unpairs every other computer.

Code you install into an existing app. It can read that app's secrets, like
a Spotify login (`luaboard install` and `put` and the studio tell you when
there are any). Install other people's apps under a name of their own.

An app hanging the device. Lua code is stopped after 1 s (10 s counting
slow calls like `sys.delay`), and if the firmware itself hangs, a hardware
watchdog restarts the device after 30 s. An autostart app that keeps causing
that is skipped after three tries. To skip a broken autostart app yourself,
keep a finger on the screen while it boots.

## Losing the device or giving it away

Before giving it away, run `luaboard erase` over USB. It clears the apps,
secrets, Wi-Fi and pairing key from the flash itself and keeps only the
firmware. `luaboard format` and `wifi off` just mark them deleted, so they
could still be read back from the chip. If it's lost, revoke the logins
its apps used on each service's account page (Manage apps on Spotify,
Third-party apps & services on Google), and maybe change the Wi-Fi password.
