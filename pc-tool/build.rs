//! Compiles the Lua parser from ../third_party/lua (the same Lua the firmware
//! runs) so the tools can syntax-check scripts before uploading them.
fn main() {
    let src = std::path::Path::new("../third_party/lua/src");
    let core = [
        "lapi", "lauxlib", "lcode", "lctype", "ldebug", "ldo", "ldump", "lfunc", "lgc", "llex", "lmem", "lobject",
        "lopcodes", "lparser", "lstate", "lstring", "ltable", "ltm", "lundump", "lvm", "lzio",
    ];
    let mut b = cc::Build::new();
    for f in core {
        b.file(src.join(format!("{f}.c")));
    }
    b.include(src).warnings(false).compile("lua54");
    println!("cargo:rerun-if-changed=../third_party/lua/src");
}
