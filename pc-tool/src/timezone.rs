//! Time zone names -> POSIX TZ strings for the device (which only knows the
//! POSIX form: no zone database on a microcontroller).
//!
//! The list covers common zones; anything else can be given as a POSIX TZ
//! string directly, e.g. `<+0530>-5:30` (note: POSIX offsets are west-positive,
//! so UTC+2 is written `-2`).
//!
//! Also the calendar arithmetic (days to a date) the studio and the tests use.

/// (name, POSIX TZ)
pub const ZONES: &[(&str, &str)] = &[
    ("UTC", "UTC0"),
    // Europe
    ("Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Tallinn", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Riga", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Vilnius", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"),
    ("Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"),
    ("Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"),
    ("Atlantic/Reykjavik", "GMT0"),
    ("Europe/Istanbul", "<+03>-3"),
    ("Europe/Moscow", "MSK-3"),
    // Americas
    ("America/New_York", "EST5EDT,M3.2.0,M11.1.0"),
    ("America/Toronto", "EST5EDT,M3.2.0,M11.1.0"),
    ("America/Chicago", "CST6CDT,M3.2.0,M11.1.0"),
    ("America/Denver", "MST7MDT,M3.2.0,M11.1.0"),
    ("America/Phoenix", "MST7"),
    ("America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"),
    ("America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"),
    ("America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"),
    ("Pacific/Honolulu", "HST10"),
    ("America/Mexico_City", "CST6"),
    ("America/Sao_Paulo", "<-03>3"),
    ("America/Buenos_Aires", "<-03>3"),
    // Asia, Oceania, Africa
    ("Asia/Dubai", "<+04>-4"),
    ("Asia/Kolkata", "IST-5:30"),
    ("Asia/Bangkok", "<+07>-7"),
    ("Asia/Singapore", "<+08>-8"),
    ("Asia/Shanghai", "CST-8"),
    ("Asia/Hong_Kong", "HKT-8"),
    ("Asia/Tokyo", "JST-9"),
    ("Asia/Seoul", "KST-9"),
    ("Australia/Perth", "AWST-8"),
    ("Australia/Brisbane", "AEST-10"),
    ("Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"),
    ("Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"),
    ("Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"),
    ("Africa/Johannesburg", "SAST-2"),
    ("Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"),
    ("Africa/Lagos", "WAT-1"),
    ("Africa/Nairobi", "EAT-3"),
];

/// A zone name ("Europe/Helsinki", or just the city: "helsinki", "new york")
/// to its POSIX TZ string. Case and spaces/underscores don't matter.
pub fn lookup(name: &str) -> Option<&'static str> {
    let norm = |s: &str| s.to_ascii_lowercase().replace([' ', '-'], "_");
    let want = norm(name);
    ZONES
        .iter()
        .find(|(n, _)| {
            let n = norm(n);
            n == want || n.rsplit('/').next() == Some(want.as_str())
        })
        .map(|(_, tz)| *tz)
}

/// The name for a POSIX TZ string, if it's one of ours (first match).
pub fn name_of(posix: &str) -> Option<&'static str> {
    ZONES.iter().find(|(_, tz)| *tz == posix).map(|(n, _)| *n)
}

/// Days since 1970-01-01 to (year, month, day), proleptic Gregorian.
pub fn civil(days: i64) -> (i64, u32, u32) {
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_and_cities() {
        assert_eq!(lookup("Europe/Helsinki"), Some("EET-2EEST,M3.5.0/3,M10.5.0/4"));
        assert_eq!(lookup("helsinki"), Some("EET-2EEST,M3.5.0/3,M10.5.0/4"));
        assert_eq!(lookup("new york"), Some("EST5EDT,M3.2.0,M11.1.0"));
        assert_eq!(lookup("utc"), Some("UTC0"));
        assert_eq!(lookup("Nowhere/Atlantis"), None);
        assert_eq!(name_of("CET-1CEST,M3.5.0,M10.5.0/3"), Some("Europe/Stockholm"));
    }
}
