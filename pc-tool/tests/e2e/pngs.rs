//! PNG files for the tests, made here: any colour type and bit depth, each
//! row with a chosen filter, compressed with zlib (flate2).

use std::io::Write;

use flate2::write::ZlibEncoder;
use flate2::Compression;

fn chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    out.extend((data.len() as u32).to_be_bytes());
    let mut body = kind.to_vec();
    body.extend_from_slice(data);
    out.extend(&body);
    out.extend(luaboard::crc32(&body).to_be_bytes());
}

fn paeth(a: i16, b: i16, c: i16) -> u8 {
    let p = a + b - c;
    let (pa, pb, pc) = ((p - a).abs(), (p - b).abs(), (p - c).abs());
    (if pa <= pb && pa <= pc {
        a
    } else if pb <= pc {
        b
    } else {
        c
    }) as u8
}

/// Filter one row (`row`, the unfiltered bytes) against the one above.
fn filter(kind: u8, row: &[u8], prev: &[u8], bpp: usize) -> Vec<u8> {
    let mut out = vec![kind];
    for i in 0..row.len() {
        let a = if i >= bpp { row[i - bpp] } else { 0 };
        let b = prev[i];
        let c = if i >= bpp { prev[i - bpp] } else { 0 };
        let pred = match kind {
            0 => 0,
            1 => a,
            2 => b,
            3 => ((a as u16 + b as u16) / 2) as u8,
            _ => paeth(a as i16, b as i16, c as i16),
        };
        out.push(row[i].wrapping_sub(pred));
    }
    out
}

pub struct Png<'a> {
    pub w: u32,
    pub h: u32,
    pub color_type: u8,
    pub depth: u8,
    /// Each row's raw (unfiltered) bytes.
    pub rows: &'a [Vec<u8>],
    /// Chunks before IDAT, e.g. PLTE and tRNS.
    pub extra: &'a [(&'a [u8; 4], Vec<u8>)],
    /// Row r uses filter `filters[r % filters.len()]`.
    pub filters: &'a [u8],
    pub interlace: bool,
}

pub fn encode(p: &Png) -> Vec<u8> {
    let channels = match p.color_type {
        0 | 3 => 1,
        4 => 2,
        2 => 3,
        _ => 4,
    };
    let bpp = ((channels * p.depth as usize) / 8).max(1);
    let mut raw = Vec::new();
    let mut prev = vec![0u8; p.rows.first().map_or(0, |r| r.len())];
    for (i, row) in p.rows.iter().enumerate() {
        let kind = if p.filters.is_empty() { 0 } else { p.filters[i % p.filters.len()] };
        raw.extend(filter(kind, row, &prev, bpp));
        prev = row.clone();
    }
    let mut z = ZlibEncoder::new(Vec::new(), Compression::default());
    z.write_all(&raw).unwrap();
    let idat = z.finish().unwrap();
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut ihdr = Vec::new();
    ihdr.extend(p.w.to_be_bytes());
    ihdr.extend(p.h.to_be_bytes());
    ihdr.extend([p.depth, p.color_type, 0, 0, p.interlace as u8]);
    chunk(&mut out, b"IHDR", &ihdr);
    for (kind, data) in p.extra {
        chunk(&mut out, kind, data);
    }
    // Two IDAT chunks when there is enough data: the stream goes across them.
    let half = idat.len() / 2;
    chunk(&mut out, b"IDAT", &idat[..half]);
    chunk(&mut out, b"IDAT", &idat[half..]);
    chunk(&mut out, b"IEND", &[]);
    out
}

/// An 8-bit RGBA image from a pixel function.
pub fn rgba(w: u32, h: u32, f: impl Fn(u32, u32) -> [u8; 4]) -> Vec<u8> {
    let rows: Vec<Vec<u8>> = (0..h).map(|y| (0..w).flat_map(|x| f(x, y)).collect()).collect();
    encode(&Png { w, h, color_type: 6, depth: 8, rows: &rows, extra: &[], filters: &[0, 1, 2, 3, 4], interlace: false })
}
