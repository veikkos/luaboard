//! Lua syntax highlighting for the editor.
//!
//! A small hand-written tokenizer (no regex / syntect dependency) that knows
//! Lua 5.4 lexical rules well enough for coloring: short and long strings,
//! short and long comments, numbers (incl. hex/exponents), keywords, the
//! standard library and the device API modules of this firmware.

use eframe::egui::{text::LayoutJob, Color32, FontId, TextFormat};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tok {
    Plain,
    Keyword,
    Literal, // true false nil
    Builtin, // print, pairs, string, math ...
    Device,  // disp, adc, gpio, sys, timer
    Call,    // identifier followed by '('
    Number,
    Str,
    Comment,
}

const KEYWORDS: &[&str] = &[
    "and", "break", "do", "else", "elseif", "end", "for", "function", "goto", "if", "in", "local", "not", "or",
    "repeat", "return", "then", "until", "while",
];
const LITERALS: &[&str] = &["true", "false", "nil"];
const BUILTINS: &[&str] = &[
    "assert",
    "collectgarbage",
    "error",
    "getmetatable",
    "ipairs",
    "load",
    "next",
    "pairs",
    "pcall",
    "print",
    "rawequal",
    "rawget",
    "rawlen",
    "rawset",
    "require",
    "select",
    "setmetatable",
    "tonumber",
    "tostring",
    "type",
    "xpcall",
    "_G",
    "_VERSION",
    "string",
    "table",
    "math",
    "utf8",
    "coroutine",
    "self",
];
const DEVICE: &[&str] = &["disp", "adc", "gpio", "sys", "timer"];

/// Split source into (byte range, token kind) spans covering the whole text.
pub fn tokenize(src: &str) -> Vec<(std::ops::Range<usize>, Tok)> {
    let b = src.as_bytes();
    let n = b.len();
    let mut out: Vec<(std::ops::Range<usize>, Tok)> = Vec::new();
    let mut push = |r: std::ops::Range<usize>, t: Tok| {
        if r.is_empty() {
            return;
        }
        // merge adjacent plain spans to keep the layout job small
        if let Some((last, lt)) = out.last_mut() {
            if *lt == t && last.end == r.start {
                last.end = r.end;
                return;
            }
        }
        out.push((r, t));
    };

    // Long bracket at i: "[" "="* "[" -> Some(level)
    let long_open = |i: usize| -> Option<usize> {
        if b.get(i) != Some(&b'[') {
            return None;
        }
        let mut j = i + 1;
        while b.get(j) == Some(&b'=') {
            j += 1;
        }
        (b.get(j) == Some(&b'[')).then_some(j - i - 1)
    };
    // End index (exclusive) of a long bracket body started at `from` with `level`.
    let long_close = |from: usize, level: usize| -> usize {
        let mut k = from;
        while k < n {
            if b[k] == b']' {
                let mut j = k + 1;
                let mut eq = 0;
                while j < n && b[j] == b'=' {
                    eq += 1;
                    j += 1;
                }
                if eq == level && j < n && b[j] == b']' {
                    return j + 1;
                }
            }
            k += 1;
        }
        n
    };

    let mut i = 0;
    while i < n {
        let c = b[i];
        // comments
        if c == b'-' && b.get(i + 1) == Some(&b'-') {
            let end = match long_open(i + 2) {
                Some(level) => long_close(i + 2 + level + 2, level),
                None => src[i..].find('\n').map_or(n, |p| i + p),
            };
            push(i..end, Tok::Comment);
            i = end;
            continue;
        }
        // long strings
        if let Some(level) = long_open(i) {
            let end = long_close(i + level + 2, level);
            push(i..end, Tok::Str);
            i = end;
            continue;
        }
        // short strings (unterminated ones stop at end of line)
        if c == b'"' || c == b'\'' {
            let mut j = i + 1;
            while j < n && b[j] != c && b[j] != b'\n' {
                j += if b[j] == b'\\' { 2 } else { 1 };
            }
            let end = (j + 1).min(n);
            push(i..end, Tok::Str);
            i = end;
            continue;
        }
        // numbers
        if c.is_ascii_digit() || (c == b'.' && b.get(i + 1).is_some_and(u8::is_ascii_digit)) {
            let mut j = i;
            let hex = c == b'0' && matches!(b.get(i + 1), Some(b'x' | b'X'));
            if hex {
                j += 2;
            }
            while j < n {
                let d = b[j];
                let exp = if hex { matches!(d, b'p' | b'P') } else { matches!(d, b'e' | b'E') };
                if exp && matches!(b.get(j + 1), Some(b'+' | b'-')) {
                    j += 2;
                } else if d.is_ascii_alphanumeric() || d == b'.' {
                    j += 1;
                } else {
                    break;
                }
            }
            push(i..j, Tok::Number);
            i = j;
            continue;
        }
        // identifiers / keywords
        if c.is_ascii_alphabetic() || c == b'_' {
            let mut j = i;
            while j < n && (b[j].is_ascii_alphanumeric() || b[j] == b'_') {
                j += 1;
            }
            let word = &src[i..j];
            let after_dot = i > 0 && b[i - 1] == b'.' && !(i > 1 && b[i - 2] == b'.');
            let mut k = j;
            while k < n && (b[k] == b' ' || b[k] == b'\t') {
                k += 1;
            }
            let is_call = k < n && (b[k] == b'(' || b[k] == b'"' || b[k] == b'{');
            let t = if KEYWORDS.contains(&word) {
                Tok::Keyword
            } else if LITERALS.contains(&word) {
                Tok::Literal
            } else if !after_dot && DEVICE.contains(&word) {
                Tok::Device
            } else if !after_dot && BUILTINS.contains(&word) {
                Tok::Builtin
            } else if is_call {
                Tok::Call
            } else {
                Tok::Plain
            };
            push(i..j, t);
            i = j;
            continue;
        }
        // anything else: one (UTF-8) character of plain text
        let len = src[i..].chars().next().map_or(1, char::len_utf8);
        push(i..i + len, Tok::Plain);
        i += len;
    }
    out
}

struct Palette {
    plain: Color32,
    keyword: Color32,
    literal: Color32,
    builtin: Color32,
    device: Color32,
    call: Color32,
    number: Color32,
    string: Color32,
    comment: Color32,
}

fn palette(dark: bool) -> Palette {
    let c = Color32::from_rgb;
    if dark {
        Palette {
            plain: c(0xd4, 0xd4, 0xd4),
            keyword: c(0xc5, 0x86, 0xc0),
            literal: c(0x56, 0x9c, 0xd6),
            builtin: c(0x4e, 0xc9, 0xb0),
            device: c(0xff, 0xb8, 0x6c),
            call: c(0xdc, 0xdc, 0xaa),
            number: c(0xb5, 0xce, 0xa8),
            string: c(0xce, 0x91, 0x78),
            comment: c(0x6a, 0x99, 0x55),
        }
    } else {
        Palette {
            plain: c(0x1f, 0x1f, 0x1f),
            keyword: c(0xaf, 0x00, 0xdb),
            literal: c(0x00, 0x00, 0xff),
            builtin: c(0x26, 0x7f, 0x99),
            device: c(0xc0, 0x5a, 0x00),
            call: c(0x79, 0x5e, 0x26),
            number: c(0x09, 0x86, 0x58),
            string: c(0xa3, 0x15, 0x15),
            comment: c(0x00, 0x80, 0x00),
        }
    }
}

/// Build a colored layout job for the editor.
pub fn layout_job(src: &str, font: FontId, dark: bool) -> LayoutJob {
    let p = palette(dark);
    let mut job = LayoutJob::default();
    for (range, tok) in tokenize(src) {
        let color = match tok {
            Tok::Plain => p.plain,
            Tok::Keyword => p.keyword,
            Tok::Literal => p.literal,
            Tok::Builtin => p.builtin,
            Tok::Device => p.device,
            Tok::Call => p.call,
            Tok::Number => p.number,
            Tok::Str => p.string,
            Tok::Comment => p.comment,
        };
        let mut fmt = TextFormat::simple(font.clone(), color);
        if tok == Tok::Comment {
            fmt.italics = true;
        }
        job.append(&src[range], 0.0, fmt);
    }
    job
}

/// Memoizes the last highlighted text so unchanged frames cost nothing.
#[derive(Default)]
pub struct Highlighter {
    key: Option<(String, bool)>,
    job: LayoutJob,
}

impl Highlighter {
    pub fn job(&mut self, src: &str, font: &FontId, dark: bool) -> LayoutJob {
        let fresh = self.key.as_ref().is_some_and(|(s, d)| s == src && *d == dark);
        if !fresh {
            self.job = layout_job(src, font.clone(), dark);
            self.key = Some((src.to_string(), dark));
        }
        self.job.clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn kinds(src: &str) -> Vec<(&str, Tok)> {
        tokenize(src).into_iter().map(|(r, t)| (&src[r], t)).filter(|(s, _)| !s.trim().is_empty()).collect()
    }

    #[test]
    fn covers_whole_input() {
        let src = "local x = [==[ a ]] b ]==] -- hi\nprint('é')";
        let toks = tokenize(src);
        assert_eq!(toks.first().unwrap().0.start, 0);
        assert_eq!(toks.last().unwrap().0.end, src.len());
        for w in toks.windows(2) {
            assert_eq!(w[0].0.end, w[1].0.start);
        }
    }

    #[test]
    fn classifies_tokens() {
        let k = kinds("local n = 0x1F + 2.5e-3 -- c\nfunction loop(dt) disp.text(0, 0, \"a\\\"b\") end");
        assert!(k.contains(&("local", Tok::Keyword)));
        assert!(k.contains(&("0x1F", Tok::Number)));
        assert!(k.contains(&("2.5e-3", Tok::Number)));
        assert!(k.contains(&("-- c", Tok::Comment)));
        assert!(k.contains(&("loop", Tok::Call)));
        assert!(k.contains(&("disp", Tok::Device)));
        assert!(k.contains(&("text", Tok::Call)));
        assert!(k.contains(&("\"a\\\"b\"", Tok::Str)));
    }

    #[test]
    fn long_comment_and_field_names() {
        let k = kinds("--[[ multi\nline ]] x.print = t.sys");
        assert_eq!(k[0], ("--[[ multi\nline ]]", Tok::Comment));
        // field names after '.' are not builtins/device modules
        assert!(!k.iter().any(|(_, t)| matches!(t, Tok::Builtin | Tok::Device)), "{k:?}");
        // ...but the same names on their own are
        assert!(kinds("print(sys)").contains(&("print", Tok::Builtin)));
        assert!(kinds("print(sys)").contains(&("sys", Tok::Device)));
    }
}
