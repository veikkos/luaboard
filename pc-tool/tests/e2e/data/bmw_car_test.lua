-- Checks for marketplace/bmw_cardata/car.lua, run on the simulator by the e2e suite (40-bmw):
-- prints "PASS name" / "FAIL name: got ..." per check and "DONE" at the end.
-- The device's time zone is the default (Finland, UTC+2 in November).
local car = require("car")

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

local T = 1793971800                      -- Fri 6.11.2026 13:30 UTC, 15:30 in Finland
local SOC = "vehicle.drivetrain.batteryManagement.header"
local SOC_NK = "vehicle.powertrain.electric.battery.stateOfCharge.displayed"
local STATUS = "vehicle.drivetrain.electricEngine.charging.status"
local HV = "vehicle.drivetrain.electricEngine.charging.hvStatus"
local POWER = "vehicle.powertrain.electric.battery.charging.power"
local LEFT = "vehicle.drivetrain.electricEngine.charging.timeRemaining"
local PLUGGED = "vehicle.powertrain.tractionBattery.charging.port.anyPosition.isPlugged"
local LOCK = "vehicle.cabin.door.lock.status"

local function iso(t)
  local d = sys.date(t, true)
  return string.format("%04d-%02d-%02dT%02d:%02d:%02d.000Z", d.year, d.month, d.day, d.hour, d.min, d.sec)
end
local function e(v, t) return { value = v, unit = "x", timestamp = iso(t or T - 120) } end

check("parse_time", car.parse_time("2026-11-06T13:30:00.000Z"), T)
check("parse_time no fraction", car.parse_time("2026-11-06T13:30:00Z"), T)
check("parse_time garbage", car.parse_time("x"), nil)
check("parse_time nil", car.parse_time(nil), nil)

local s = {}
car.merge(s, { [SOC] = e(78) })
local v = car.view(s, T)
check("soc", v.soc, 78)
check("age", v.age, "2 min ago")
check("age without clock", car.view(s, nil).age, nil)
check("never received is nil", v.range_km, nil)

s = {}
car.merge(s, { [SOC] = e("78"), [PLUGGED] = e("true") })
v = car.view(s, T)
check("REST string number", v.soc, 78)
check("REST string boolean", v.plug, "Plugged in")

s = {}
car.merge(s, { [SOC_NK] = e(64) })
check("soc fallback", car.view(s, T).soc, 64)
car.merge(s, { [SOC] = e(78) })
check("soc first descriptor wins a tie", car.view(s, T).soc, 78)

-- The car updates some descriptors and leaves others behind: the newest wins.
local LEVEL = "vehicle.drivetrain.electricEngine.charging.level"
local KOMBI = "vehicle.drivetrain.electricEngine.kombiRemainingElectricRange"
local RANGE2 = "vehicle.drivetrain.electricEngine.remainingElectricRange"
s = {}
car.merge(s, { [SOC] = e(68, T - 4200), [LEVEL] = e(73, T - 60), [KOMBI] = e(229, T - 4200), [RANGE2] = e(244, T - 60) })
v = car.view(s, T)
check("soc newest wins", v.soc, 73)
check("range newest wins", v.range_km, 244)
car.merge(s, { [STATUS] = e("CHARGINGACTIVE", T - 4200), [HV] = e("FINISHED_FULLY_CHARGED", T - 60) })
check("charging newest wins", car.view(s, T).charging, "Charged")
s = {}
car.merge(s, { [SOC] = { value = 68, unit = "percent" }, [LEVEL] = e(73, T - 60) })
check("a value without a time loses to a dated one", car.view(s, T).soc, 73)
s = {}
car.merge(s, { [PLUGGED] = e(false) })
check("isPlugged false", car.view(s, T).plug, "Unplugged")

s = {}
car.merge(s, { [STATUS] = e("CHARGINGACTIVE"), [POWER] = e(7200), [LEFT] = e(130, T) })
v = car.view(s, T)
check("charging", v.charging, "Charging")
check("is_charging", v.is_charging, true)
check("power_kw", v.power_kw, 7.2)
check("full at", v.left, "full at 17:40")
check("left without clock", car.view(s, nil).left, "2 h 10 min left")
car.merge(s, { [STATUS] = e("CHARGINGENDED") })
v = car.view(s, T)
check("charged", v.charging, "Charged")
check("no power when not charging", v.power_kw, nil)
check("no time when not charging", v.left, nil)

s = {}
car.merge(s, { [HV] = e("FINISHED_NOT_FULL") })
check("hvStatus", car.view(s, T).charging, "Stopped")
s = {}
car.merge(s, { [STATUS] = e("SOMETHINGNEW") })
check("unknown status", car.view(s, T).charging, nil)

s = {}
car.merge(s, { [LOCK] = e("SELECTIVELOCKED") })
check("partly locked", car.view(s, T).lock, "partly locked")
car.merge(s, { [LOCK] = e("SECURED") })
check("secured is locked", car.view(s, T).lock, "locked")
car.merge(s, { ["vehicle.body.trunk.isOpen"] = e(false, T - 120 + 5 * 60) })
check("lock as new as the rest is shown", car.view(s, T).lock, "locked")
car.merge(s, { ["vehicle.body.trunk.isOpen"] = e(false, T - 120 + 5 * 60 + 1) })
check("lock older than the rest is hidden", car.view(s, T).lock, nil)
car.merge(s, { ["vehicle.body.chargingPort.status"] = e("DISCONNECTED") })
check("unplugged", car.view(s, T).plug, "Unplugged")

check("fmt_km", car.fmt_km(12345), "12 345 km")
check("fmt_km small", car.fmt_km(999), "999 km")
check("fmt_km million", car.fmt_km(1234567.4), "1 234 567 km")

s = {}
check("unknown key once", car.merge(s, { ["vehicle.foo"] = e(1) }), { "vehicle.foo" })
check("unknown key not again", car.merge(s, { ["vehicle.foo"] = e(2) }), {})
check("bad data", pcall(car.merge, s, "x"), true)
check("bad entry", pcall(car.merge, s, { k = 5 }), true)

check("needs_rest empty", car.needs_rest({}, T), true)
s = {}
car.merge(s, { [SOC] = e(78, T - 600) })
check("needs_rest fresh", car.needs_rest(s, T), false)
check("needs_rest no clock", car.needs_rest(s, nil), false)
s = {}
car.merge(s, { [SOC] = e(78, T - 4000) })
check("needs_rest old", car.needs_rest(s, T), true)
check("not stale", car.view(s, T).stale, false)
s = {}
car.merge(s, { [SOC] = e(78, T - 90000) })
check("stale", car.view(s, T).stale, true)

local rec, ok = nil, nil
local oks = 0
for _ = 1, 11 do
  ok, rec = car.budget_take(rec, T, 10)
  if ok then oks = oks + 1 end
end
check("budget cap", oks, 10)
check("budget day", rec.day, "2026-11-06")
ok, rec = car.budget_take(rec, T + 86400, 10)
check("budget new day", ok, true)
check("budget new day count", rec.n, 1)

s = {}
car.merge(s, { [SOC] = e(81), ["vehicle.foo"] = e(1) })
local back = car.decode(car.encode(s))
check("encode keeps soc", car.view(back, T).soc, 81)
check("encode leaves unknown keys out", back["vehicle.foo"], nil)
check("decode garbage", next(car.decode("not json")), nil)
check("descriptors", #car.DESCRIPTORS >= 9, true)

-- crop: a 6x4 picture of background 0xFFFF with one dark pixel at (2, 1)
local function pic(w, h, dots)
  local t = {}
  for y = 0, h - 1 do for x = 0, w - 1 do
    local c = dots[y * w + x] or 0xFFFF
    t[#t + 1] = string.char(c >> 8, c & 255)
  end end
  return table.concat(t)
end
local px, cw, ch = car.crop(pic(6, 4, { [1 * 6 + 2] = 0x0000 }), 6, 4, 0xFFFF, 1)
check("crop size", { cw, ch }, { 3, 3 })
check("crop keeps the dot in the middle", px and px:sub(9, 10) == string.char(0, 0), true)
px, cw, ch = car.crop(pic(6, 4, { [0] = 0x0000 }), 6, 4, 0xFFFF, 10)
check("crop margin clamped", { cw, ch }, { 6, 4 })
check("crop of only background", car.crop(pic(4, 2, {}), 4, 2, 0xFFFF, 2), nil)
check("utc_day", car.utc_day(T), "2026-11-06")

-- Page 2's rows
local function D(k) return "vehicle." .. k end
s = {}
check("all closed needs a known value", car.view(s, T).openings, nil)
car.merge(s, { [D("cabin.door.row1.driver.isOpen")] = e("CLOSED"), [D("cabin.window.row2.passenger.status")] = e("CLOSED"),
  [D("body.trunk.isOpen")] = e("CLOSED"), [D("body.hood.isOpen")] = e(false) })
check("all closed", car.view(s, T).openings, {})
car.merge(s, { [D("cabin.door.row1.driver.isOpen")] = e("OPEN"), [D("cabin.window.row2.passenger.status")] = e("INTERMEDIATE"),
  [D("body.trunk.isOpen")] = e(true) })
check("what is open", car.view(s, T).openings, { "Front left door open", "Rear right window half open", "Trunk open" })
v = car.view(s, T)
check("each opening's state", { v.open.door_fl, v.open.window_rr, v.open.trunk, v.open.hood, v.open.door_fr },
  { "open", "half", "open", "closed", nil })

s = {}
local function tyre(pos, kpa, target)
  car.merge(s, { [D("chassis.axle." .. pos .. ".tire.pressure")] = e(kpa), [D("chassis.axle." .. pos .. ".tire.pressureTarget")] = e(target) })
end
tyre("row1.wheel.left", 250, 250) tyre("row1.wheel.right", 248, 250) tyre("row2.wheel.left", 250, 270) tyre("row2.wheel.right", 270, 270)
v = car.view(s, T)
check("tyres in bar", v.tyres, { "2.5", "2.5", "2.5", "2.7" })
check("tyres fine", v.tyres_low, false)
tyre("row2.wheel.left", 230, 270)
check("a tyre 10 % under its target is low", car.view(s, T).tyres_low, true)
check("which tyre is low", car.view(s, T).tyre_low, { false, false, true, false })
check("targets once per axle", { v.target_front, v.target_rear }, { "2.5", "2.7" })

s = {}
car.merge(s, { [D("vehicle.preConditioning.activity")] = e("INACTIVE"), [D("vehicle.preConditioning.remainingTime")] = e(15) })
check("climate off", car.view(s, T).climate, nil)
car.merge(s, { [D("cabin.hvac.preconditioning.status.comfortState")] = e("COMFORT_HEATING", T - 30),
  [D("cabin.hvac.preconditioning.status.remainingRunningTime")] = e(720, T - 30) })
check("climate on, newest", car.view(s, T).climate, "Climate on, 12 min left")

s = {}
car.merge(s, { [D("status.serviceDistance.next")] = e(12000),
  [D("drivetrain.electricEngine.charging.method")] = e("AC_TYPE2PLUG"),
  [D("body.trunk.isLocked")] = e(true) })
v = car.view(s, T)
check("service", v.service_km, 12000)
check("AC", v.method, "AC")
check("trunk lock counts as locked", v.lock, "locked")
car.merge(s, { [D("drivetrain.electricEngine.charging.method")] = e("DC") })
v = car.view(s, T)
check("DC", v.method, "DC")

s = {}
car.merge(s, { [STATUS] = e("CHARGINGACTIVE"), [D("drivetrain.electricEngine.charging.timeToFullyCharged")] = e(60, T) })
check("time to full from timeToFullyCharged", car.view(s, T).left, "full at 16:30")
check("container name follows the list", car.container_name():match("^luaboard%-%x%x%x%x%x%x%x%x$") ~= nil, true)

print("DONE")
