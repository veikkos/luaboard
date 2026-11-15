//! Lua syntax check, with the same parser (Lua 5.4) as the firmware, so the
//! messages match what the device would say: "main.lua:12: '=' expected near 'x'".

use std::ffi::{c_char, c_int, c_void, CStr, CString};

#[allow(non_camel_case_types)]
type lua_State = c_void;

extern "C" {
    fn luaL_newstate() -> *mut lua_State;
    fn lua_close(l: *mut lua_State);
    fn luaL_loadbufferx(
        l: *mut lua_State,
        buf: *const c_char,
        sz: usize,
        name: *const c_char,
        mode: *const c_char,
    ) -> c_int;
    fn lua_tolstring(l: *mut lua_State, idx: c_int, len: *mut usize) -> *const c_char;
}

/// Parse (don't run) `src`. Err is the Lua message, e.g. "main.lua:3: unexpected symbol near ')'".
pub fn check(name: &str, src: &[u8]) -> Result<(), String> {
    let chunk = CString::new(format!("@{name}")).map_err(|_| "bad file name".to_string())?;
    unsafe {
        let l = luaL_newstate();
        if l.is_null() {
            return Err("out of memory".into());
        }
        let rc = luaL_loadbufferx(l, src.as_ptr().cast(), src.len(), chunk.as_ptr(), c"t".as_ptr());
        let res = if rc == 0 {
            Ok(())
        } else {
            let p = lua_tolstring(l, -1, std::ptr::null_mut());
            Err(if p.is_null() { "syntax error".into() } else { CStr::from_ptr(p).to_string_lossy().into_owned() })
        };
        lua_close(l);
        res
    }
}

/// Line number from a Lua error message for `name` ("name:LINE: ...").
pub fn error_line(name: &str, msg: &str) -> Option<usize> {
    msg.strip_prefix(name)?.strip_prefix(':')?.split(':').next()?.parse().ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn valid_code_passes() {
        assert!(check("main.lua", b"local x = 1\nfunction loop() x = x + 1 end\n").is_ok());
    }

    #[test]
    fn error_has_file_and_line() {
        let e = check("main.lua", b"local x = 1\nif x then\n  print(x\nend\n").unwrap_err();
        assert!(e.starts_with("main.lua:4:"), "{e}");
        assert_eq!(error_line("main.lua", &e), Some(4));
    }

    #[test]
    fn lua54_features_accepted() {
        assert!(check("a.lua", b"local x <const> = 1 // 2\ngoto done\n::done::\n").is_ok());
    }

    #[test]
    fn code_is_not_run() {
        assert!(check("a.lua", b"error('boom')").is_ok());
    }
}
