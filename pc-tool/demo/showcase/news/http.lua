-- A stand-in for the built-in http, for the showcase GIF and the news app's
-- screenshot: a made-up feed, "The Board Gazette", with stories from the
-- last few hours (around the device's clock) and pictures uploaded with the
-- app. Uploaded into the app, it replaces require("http").
--
-- The pictures are NASA's, in the public domain (images.nasa.gov), cropped
-- to 640x360:
--   pic1.jpg  iss058e005282    View of the Earth's limb with an aurora (JSC)
--   pic2.jpg  NHQ202211160030  Artemis I Launch (NASA/Bill Ingalls)
--   pic3.jpg  iss040e092688    Moon setting (JSC)
--   pic4.jpg  PIA24542         Perseverance's Selfie with Ingenuity (JPL)
--   pic5.jpg  iss030e119777    Earth Observations by Expedition 30 (JSC)

local M = {}

local STORIES = {
  { 25, "pic2", "Moon rocket lifts off at night on its first crewed test flight",
    "The crew of four will fly around the Moon and back over ten days, testing the ship's life support on the way." },
  { 70, "pic5", "Strong solar storm brings bright auroras far to the south",
    "The northern lights may be seen again tonight wherever the sky is clear, forecasters say." },
  { 130, "pic4", "Mars rover finds layered rocks in an old river delta",
    "The layers formed in standing water and are the best place yet to look for signs of past life." },
  { 200, "pic3", "The year's biggest full moon rises on Thursday",
    "It will look about a seventh larger than the smallest full moon of the year." },
  { 310, "pic1", "City lights seen from orbit show how fast the coast has grown",
    "Pictures taken ten years apart show new towns along the whole shoreline." },
  { 420, nil, "Library opens a new reading room for children",
    "The room has two thousand books and a corner for reading aloud." },
  { 540, nil, "Weekend weather: clear skies and frost in the morning",
    "Days stay dry, nights cold, with the first ice on the lakes in the north." },
}

local MON = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" }

local function rfc822(t)
  local d = sys.date(t, "UTC")
  return string.format("%02d %s %04d %02d:%02d:%02d GMT", d.day, MON[d.month], d.year, d.hour, d.min, d.sec)
end

local function rss()
  local now = sys.time() or 1794306245
  local out = { '<?xml version="1.0" encoding="UTF-8"?>',
    '<rss version="2.0" xmlns:media="http://search.yahoo.com/mrss/"><channel>',
    "<title>The Board Gazette</title><link>https://example.com/</link>" }
  for i, s in ipairs(STORIES) do
    out[#out + 1] = string.format(
      "<item><title>%s</title><link>https://example.com/%d</link><guid>gazette-%d</guid>"
        .. "<description>%s</description><pubDate>%s</pubDate>%s</item>",
      s[3], i, i, s[4], rfc822(now - s[1] * 60),
      s[2] and string.format('<media:content url="https://example.com/%s.jpg" medium="image" width="640"/>', s[2]) or "")
  end
  out[#out + 1] = "</channel></rss>"
  return table.concat(out, "\n")
end

function M.get(url)
  if url:match("/feed%.xml$") then return 200, rss(), {} end
  local pic = url:match("/(pic%d)%.jpg$")
  if pic then return 200, assert(asset(pic .. ".jpg")), {} end
  return 404, "", {}
end

function M.close_idle() end

function M.request(t)
  return M.get(type(t) == "string" and t or t.url)
end

return M
