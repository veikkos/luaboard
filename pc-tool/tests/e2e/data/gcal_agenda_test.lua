-- Checks for examples/gcal/agenda.lua, run on the simulator by the e2e suite (13-gcal-api):
-- prints "PASS name" / "FAIL name: got ..." per check and "DONE" at the end.
-- The device's time zone is the default (Finland, UTC+2 in November).
local agenda = require("agenda")

local function show(v)
  if type(v) ~= "table" then return tostring(v) end
  local t = {}
  for i, x in ipairs(v) do t[i] = show(x) end
  return "{" .. table.concat(t, ",") .. "}"
end

local function check(name, got, want)
  if show(got) == show(want) then print("PASS " .. name)
  else print("FAIL " .. name .. ": got " .. show(got) .. ", want " .. show(want)) end
end

local T = 1793970000                      -- Fri 6.11.2026 15:00 Finnish time (13:00 UTC)
local H, M = 3600, 60

check("parse_time Z", agenda.parse_time("2026-11-06T13:00:00Z"), T)
check("parse_time offset", agenda.parse_time("2026-11-06T15:00:00+02:00"), T)
check("parse_time negative offset", agenda.parse_time("2026-11-06T08:00:00-05:00"), T)
check("parse_time fraction", agenda.parse_time("2026-11-06T13:00:00.250Z"), T)
check("parse_time date only", agenda.parse_time("2026-11-06"), nil)
check("parse_time garbage", agenda.parse_time(nil), nil)

check("day_start today", agenda.day_start(T, 0), 1793916000)
check("day_start +2", agenda.day_start(T, 2), 1794088800)

check("fit long", agenda.fit("Quarterly planning session", 11), "Quarterly..")
check("fit counts characters", agenda.fit("Päivä ja yö", 9), "Päivä j..")
check("fit UTF-8 that fits", agenda.fit("Älä unohda", 10), "Älä unohda")
check("fit short", agenda.fit("short", 10), "short")
check("hhmm", agenda.hhmm(T), "15:00")
check("day_name", agenda.day_name(T), "Fri 6.11.")

check("until min", agenda.until_text(T, T + 18 * M), "in 18 min")
check("until h min", agenda.until_text(T, T + 80 * M), "in 1 h 20 min")
check("until h", agenda.until_text(T, T + 2 * H), "in 2 h")
check("until seconds", agenda.until_text(T, T + 10), "in 1 min")
check("until rounds up", agenda.until_text(T, T + 17 * M + 1), "in 18 min")

-- normalize: what is kept, and which reminders count
local function dt(t) return { dateTime = string.format("2026-11-06T%02d:%02d:00+02:00", t // 60, t % 60) } end
local items = {
  { id = "a", summary = "Älä unohda", start = dt(16 * 60), ["end"] = dt(17 * 60), reminders = { useDefault = true } },
  { id = "b", summary = "Design review", start = dt(17 * 60), ["end"] = dt(18 * 60),
    reminders = { useDefault = false, overrides = { { method = "email", minutes = 30 }, { method = "popup", minutes = 5 } } } },
  { id = "x1", summary = "cancelled", status = "cancelled", start = dt(16 * 60), ["end"] = dt(17 * 60) },
  { id = "x2", summary = "all day", start = { date = "2026-11-06" }, ["end"] = { date = "2026-11-07" } },
  { id = "x3", summary = "free", transparency = "transparent", start = dt(16 * 60), ["end"] = dt(17 * 60) },
  { id = "x4", summary = "declined", start = dt(16 * 60), ["end"] = dt(17 * 60),
    attendees = { { email = "o@x", responseStatus = "accepted" }, { self = true, responseStatus = "declined" } } },
  { id = "c", start = dt(18 * 60), ["end"] = dt(19 * 60), reminders = { useDefault = false } },
}
local evs = agenda.normalize(items, "cal", { 10 })
local ids, rem = {}, {}
for i, e in ipairs(evs) do ids[i] = e.id; rem[i] = e.reminders end
check("normalize keeps", ids, { "cal/a", "cal/b", "cal/c" })
check("normalize reminders", rem, { { 10 }, { 5 }, {} })
check("normalize title as given", evs[1].title, "Älä unohda")
check("normalize no title", evs[3].title, "(no title)")
check("normalize times", { evs[2].start, evs[2].stop }, { T + 2 * H, T + 3 * H })

-- views
local function ev(id, s, e) return { id = id, title = id, start = s, stop = e, reminders = {} } end
local A = ev("A", T - 30 * M, T + 30 * M)                    -- 14:30-15:30
local B = ev("B", T - 15 * M, T + 1 * H)                     -- 14:45-16:00
local C = ev("C", T + 150 * M, T + 3 * H)                    -- 17:30-18:00
local D = ev("D", 1794034800, 1794034800 + 30 * M)           -- tomorrow 09:00-09:30
local function list_ids(v) local t = {} for i, e in ipairs(v.list) do t[i] = e.id end return t end

local v = agenda.view(agenda.merge({ { A, C }, { B, D } }), T, 0)
check("view now: kind", v.main.kind, "now")
check("view now: latest started", v.main.event and v.main.event.id, "B")
check("view now: label", v.main.label, "now - until 16:00")
check("view now: list", list_ids(v), { "A", "C" })

v = agenda.view({ C, D }, T, 0)
check("view next: kind", v.main.kind, "next")
check("view next: label", v.main.label, "next - in 2 h 30 min")
check("view next: list", list_ids(v), {})

v = agenda.view({ D }, T, 0)
check("view none: label", v.main.label, "nothing more today")
check("view none: kind", v.main.kind, "none")
check("view none: tomorrow listed", { v.list_label, list_ids(v) }, { "tomorrow", { "D" } })

v = agenda.view({ C, D }, T, 1)
check("view tomorrow: kind", v.main.kind, "first")
check("view tomorrow: label", v.main.label, "tomorrow - 09:00")
v = agenda.view({ C }, T, 1)
check("view tomorrow empty", v.main.label, "nothing tomorrow")

local started_yesterday = ev("Y", T - 26 * H, T + H)         -- a long event that began yesterday
v = agenda.view({ started_yesterday }, T, 0)
check("view: event from yesterday still running", { v.main.kind, v.main.event and v.main.event.id }, { "now", "Y" })

-- alerts
local E = { id = "E", title = "E", start = T + 600, stop = T + 1200, reminders = { 10, 5 } }
local function key(a) return a and a.key end
check("alert: not yet", key(agenda.alert_due({ E }, T - 60, {})), nil)
check("alert: first reminder", key(agenda.alert_due({ E }, T, {})), "E@10")
check("alert: minutes", agenda.alert_due({ E }, T, {}).minutes, 10)
check("alert: dismissed", key(agenda.alert_due({ E }, T, { ["E@10"] = E.start })), nil)
check("alert: second reminder", key(agenda.alert_due({ E }, T + 300, { ["E@10"] = E.start })), "E@5")
check("alert: dismissed, then the meeting moved", key(agenda.alert_due({ E }, T, { ["E@10"] = E.start - 3600 })), "E@10")
check("alert: meeting started", key(agenda.alert_due({ E }, T + 600, {})), nil)
local F = { id = "F", title = "F", start = T + 300, stop = T + 900, reminders = { 15 } }
check("alert: earliest meeting first", key(agenda.alert_due({ E, F }, T, {})), "F@15")
check("alert: no reminders", key(agenda.alert_due({ ev("G", T + 60, T + 120) }, T, {})), nil)

local Z0 = { id = "Z", title = "Z", start = T + 600, stop = T + 1200, reminders = { 10, 0 } }
check("alert: 0 min reminder not before its time", key(agenda.alert_due({ Z0 }, T + 599, {})), "Z@10")
check("alert: 0 min reminder at the start", key(agenda.alert_due({ Z0 }, T + 600, {})), "Z@0")
check("alert: 0 min reminder for a while", key(agenda.alert_due({ Z0 }, T + 899, {})), "Z@0")
check("alert: 0 min reminder ends", key(agenda.alert_due({ Z0 }, T + 900, {})), nil)
check("until: started", agenda.until_text(T, T), "now")

check("retry delays", { agenda.retry_delay(1), agenda.retry_delay(2), agenda.retry_delay(4), agenda.retry_delay(9) },
      { 30, 60, 240, 300 })
check("login retry delays", { agenda.login_retry_delay(1), agenda.login_retry_delay(2), agenda.login_retry_delay(9) },
      { 900, 1800, 14400 })

print("DONE")
