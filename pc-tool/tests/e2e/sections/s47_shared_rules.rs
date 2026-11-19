//! Numbers the marketplace's index checks (marketplace/tools/build_index.py)
//! and the CLI copy from the firmware's app_config.h: still the same. No
//! simulator: the sources are read.

use crate::harness::T;
use std::fs;

pub fn run(t: &mut T) {
    t.say("== rules copied from app_config.h");
    let config = fs::read_to_string(t.root.join("app/src/app_config.h")).unwrap_or_default();
    let index = fs::read_to_string(t.root.join("marketplace/tools/build_index.py")).unwrap_or_default();
    // "#define NAME 31" -> 31
    let d = |name: &str| -> usize {
        let line = config.lines().find(|l| l.split_whitespace().take(2).eq(["#define", name])).unwrap_or("");
        line.split_whitespace().nth(2).and_then(|v| v.parse().ok()).unwrap_or(0)
    };

    let name_max = d("VFS_APP_NAME_MAX").saturating_sub(1); // the NUL takes one
    let exts: Vec<String> = luaboard::ASSET_EXTENSIONS.iter().map(|e| format!("\".{e}\"")).collect();
    let lines = [
        format!("DESC_ROOM = ({}, {})", d("APP_DESC_LINES"), d("APP_DESC_COLS")),
        format!("DESC_QR_ROOM = ({}, {})", d("APP_DESC_QR_LINES"), d("APP_DESC_QR_COLS")),
        format!(
            r#"TEXT_MAX = {{"title": {}, "description": DESC_ROOM[0] * DESC_ROOM[1] * 3, "author": {}, "license": {}}}"#,
            d("APP_TITLE_MAX"),
            d("APP_AUTHOR_MAX"),
            d("APP_LICENSE_MAX")
        ),
        format!("{{1,{name_max}}}"),
        format!("BOARD_EXTS = ({})", exts.join(", ")),
    ];
    let missing: Vec<&String> = lines.iter().filter(|l| !index.contains(l.as_str())).collect();
    t.check(missing.is_empty(), "build_index.py has app_config.h's numbers and the CLI's file types", |_| {
        format!("not in build_index.py: {missing:?}")
    });

    let cli = luaboard::valid_app_name(&"a".repeat(name_max)) && !luaboard::valid_app_name(&"a".repeat(name_max + 1));
    t.check(cli, "the CLI's app names are as long as the board's", |_| format!("not {name_max} characters at most"));
}
