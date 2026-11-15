-- A stand-in for the built-in http, for the showcase GIF: the weather app's
-- forecast request gets a made-up November midday in Helsinki, around the
-- device's clock. Uploaded into the app, it replaces require("http").

local json = require("json")

local M = {}

function M.urlencode(s) return (tostring(s):gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end)) end

local function forecast(now)
  local hour = now - now % 3600
  local times, temps, rain = {}, {}, {}
  for i = 0, 23 do
    local t = hour + i * 3600
    local h = (t + 7200) % 86400 // 3600            -- Finnish time (November: UTC+2)
    times[#times + 1] = t
    -- warmest at 14, coldest at 06; a front with showers in the evening
    temps[#temps + 1] = math.floor((4.1 + 2.3 * math.cos((h - 14) * math.pi / 12) + (i % 3) * 0.1) * 10 + 0.5) / 10
    rain[#rain + 1] = ({ [18] = 0.2, [19] = 0.8, [20] = 1.4, [21] = 0.9, [22] = 0.3 })[h] or 0
  end
  local midnight = now - (now + 7200) % 86400
  local days = {}
  for i = 0, 6 do days[#days + 1] = midnight + i * 86400 end
  return {
    latitude = 60.17, longitude = 24.94, utc_offset_seconds = 7200, timezone = "Europe/Helsinki",
    current = { time = now - now % 900, interval = 900, temperature_2m = 6.2, apparent_temperature = 3.1,
                weather_code = 1, wind_speed_10m = 4.6, wind_direction_10m = 235, is_day = 1 },
    hourly = { time = times, temperature_2m = temps, precipitation = rain },
    daily = { time = days, weather_code = { 1, 61, 3, 80, 71, 2, 0 },
              temperature_2m_max = { 6.8, 7.4, 5.9, 4.2, 1.6, 2.8, 3.5 },
              temperature_2m_min = { 1.9, 3.8, 2.2, -0.4, -2.7, -3.9, -5.1 },
              precipitation_sum = { 3.6, 8.2, 0.4, 5.1, 2.3, 0.0, 0.0 } },
  }
end

function M.get(url)
  if url:find("/forecast?", 1, true) then
    return 200, json.encode(forecast(sys.time() or 0)), {}
  end
  return 404, "{}", {}
end

return M
