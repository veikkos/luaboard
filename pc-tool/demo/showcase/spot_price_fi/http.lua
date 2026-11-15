-- A stand-in for the built-in http, for the showcase GIF: the spot price app
-- gets a made-up but typical November day (cheap night, morning and evening
-- peaks) and a windy, cheap tomorrow, around the device's clock. Uploaded
-- into the app, it replaces require("http").

local json = require("json")

local M = {}

local function civil_from_days(z)
  z = z + 719468
  local era = z // 146097
  local doe = z - era * 146097
  local yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
  local doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
  local mp = (5 * doy + 2) // 153
  local d = doy - (153 * mp + 2) // 5 + 1
  local m = mp < 10 and mp + 3 or mp - 9
  return yoe + era * 400 + (m <= 2 and 1 or 0), m, d
end

local function iso(t)
  local y, m, d = civil_from_days(t // 86400)
  local s = t % 86400
  return string.format("%04d-%02d-%02dT%02d:%02d:%02d.000Z", y, m, d, s // 3600, s % 3600 // 60, s % 60)
end

-- c/kWh at local hour h (0..24, fractional) of day 0 (today) or 1 (tomorrow).
local function price(h, day, k)
  local wobble = math.sin(k * 1.7) * 0.9 + math.sin(k * 0.37) * 0.6
  if day == 1 then return math.max(0.3, 2.2 + 2.5 * math.exp(-((h - 8.5) / 2) ^ 2) + wobble * 0.5) end
  local p = 2.1
    + 19 * math.exp(-((h - 8.25) / 1.4) ^ 2)        -- the morning peak
    + 10 * math.exp(-((h - 12.5) / 2.5) ^ 2)        -- a busy midday
    + 24 * math.exp(-((h - 17.75) / 1.6) ^ 2)       -- the evening peak
  return math.max(0.4, p + wobble * (p > 8 and 1.6 or 0.5))
end

local function prices(now)
  local start = now - (now + 7200) % 86400          -- today 00:00 Finnish time (UTC+2 in November)
  local list = {}
  for i = 0, 191 do
    local t = start + i * 900
    local p = price(i % 96 / 4, i // 96, i)
    list[#list + 1] = { price = math.floor(p * 1000 + 0.5) / 1000, startDate = iso(t), endDate = iso(t + 899) }
  end
  return list
end

function M.get(url)
  local now = sys.time() or 0
  if url:find("latest-prices.json", 1, true) then
    return 200, json.encode({ prices = prices(now) }), {}
  end
  if url:find("price.json", 1, true) then
    return 200, json.encode({ price = 5.0 }), {}
  end
  return 404, "{}", {}
end

return M
