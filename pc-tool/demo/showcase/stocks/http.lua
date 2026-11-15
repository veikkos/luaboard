-- A stand-in for the built-in http, for the showcase GIF and the stocks
-- example's screenshot: made-up but typical days for SPY, QQQ, DIA and IWM
-- (a busy opening, a quiet midday, a push into the close), around the
-- device's clock. During a session the bars run up to now; otherwise the
-- last session is whole, with an after-hours trade. Uploaded into the app,
-- it replaces require("http").

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
  return string.format("%04d-%02d-%02dT%02d:%02d:%02dZ", y, m, d, s // 3600, s % 3600 // 60, s % 60)
end

local ET = -5 * 3600                                -- New York in November

-- UTC of New York midnight of the weekday session at or before `t`.
local function session_of(t)
  local mid = t - (t + ET) % 86400
  local function weekday(m) return ((m + ET + 12 * 3600) // 86400 + 4) % 7 end  -- 0 = Sunday
  if t < mid + 9 * 3600 + 1800 then mid = mid - 86400 end    -- before today's opening
  while weekday(mid) == 0 or weekday(mid) == 6 do mid = mid - 86400 end
  return mid
end

-- symbol = { previous close, the day's change in %, seed }
local STOCKS = { SPY = { 683.10, 0.82, 11 }, QQQ = { 621.40, -0.91, 23 },
                 DIA = { 476.20, 0.38, 37 }, IWM = { 247.80, 1.64, 51 } }

-- The 78 five-minute closes of a session: a random walk, livelier at the
-- opening and the close, bent to end at the day's change.
local function closes(s)
  local prev, change, r = STOCKS[s][1], STOCKS[s][2], STOCKS[s][3]
  local walk, p = {}, 0
  for i = 1, 78 do
    r = (r * 16807) % 2147483647
    local busy = 0.6 + 1.6 * math.exp(-(i - 1) / 8) + 0.8 * math.exp(-(78 - i) / 6)
    p = p + (r / 2147483647 - 0.5) * busy * 0.0021 + math.sin(i / 7 + r % 5) * 0.0002
    walk[i] = p
  end
  local list = {}
  for i = 1, 78 do
    local f = walk[i] + (change / 100 - walk[78]) * i / 78
    list[i] = prev * (1 + f)
  end
  return prev, list
end

function M.form(t)
  local keys = {}
  for k in pairs(t) do keys[#keys + 1] = k end
  table.sort(keys)
  local out = {}
  for _, k in ipairs(keys) do out[#out + 1] = k .. "=" .. tostring(t[k]) end
  return table.concat(out, "&")
end

function M.get(url)
  local now = sys.time()
  local mid = session_of(now)
  local open, close = mid + 9 * 3600 + 1800, mid + 16 * 3600
  local n = math.min(78, (math.min(now, close) - open) // 300)
  local path, syms = url:match("/v2/([^?]+)%?.*symbols=([^&]+)")
  local out = {}
  for s in syms:gmatch("[^,]+") do
    if STOCKS[s] then
      local prev, list = closes(s)
      if path == "stocks/snapshots" then
        local last, t = list[math.max(n, 1)], now - 3
        if now >= close then last, t = list[78] * 0.9988, close + 3 * 3600 + 1234 end  -- after hours
        local before = session_of(mid - 1)
        out[s] = { latestTrade = { t = iso(math.min(t, now)), p = last },
                   dailyBar = { t = iso(mid), c = list[n] },
                   prevDailyBar = { t = iso(before), c = prev } }
      else
        local bars = {}
        for i = 1, n do
          local o = i == 1 and prev * 1.0012 or list[i - 1]
          bars[i] = { t = iso(open + (i - 1) * 300), o = o, c = list[i] }
        end
        out[s] = bars
      end
    end
  end
  if path == "stocks/bars" then out = { bars = out, next_page_token = json.null } end
  return 200, json.encode(out), {}
end

return M
