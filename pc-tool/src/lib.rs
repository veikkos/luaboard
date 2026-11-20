//! Client library for the ESP32 Lua devboard serial protocol.
//!
//! The protocol is line based (see `docs/PROTOCOL.md`):
//! the host sends one command per line, the device answers with zero or more
//! data lines (`FILE ...`) and exactly one final `OK ...` or `ERR ...` line.
//! At any time the device may also emit asynchronous `LOG ...` (script
//! `print()` output) and `EVT ...` (state changes, runtime errors) lines.

pub mod crash;
mod device;
pub mod firmware;
pub mod keys;
mod link;
pub mod login;
pub mod ota;
mod project;
pub mod simctl;
pub mod syntax;
pub mod tether;
pub mod timezone;
pub mod wifi;

pub use device::*;
pub use link::*;
pub use project::*;
