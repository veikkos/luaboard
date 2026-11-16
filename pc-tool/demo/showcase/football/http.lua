-- A stand-in for the built-in http, for the showcase GIF and the football
-- app's screenshot: made-up clubs and games around the device's clock (two
-- in progress, one at half time, one to come, three finished), in the shape
-- of football-data.org's answers. The crests are made-up shields uploaded
-- with the app (<TLA>.jpg). Uploaded into the app, it replaces require("http").

local json = require("json")

local M = {}

local TEAMS = {
  NBR = { 1, "Northbridge FC", "Northbridge", "Northbridge Park" },
  HBC = { 2, "Harbor City FC", "Harbor City", "The Quay" },
  KFU = { 3, "Kingsford United", "Kingsford Utd", "Kingsford Road" },
  RIV = { 4, "Riverside Athletic", "Riverside", "Riverside Ground" },
  ASH = { 5, "Ashby Rovers", "Ashby", "Ashby Lane" },
  WST = { 6, "Westmoor Town", "Westmoor", "Moor Park" },
  EVA = { 7, "Eastvale FC", "Eastvale", "Vale Stadium" },
  BFA = { 8, "Brookfield Athletic", "Brookfield", "Brook Field" },
}

-- minutes from now, competition, home, away, status, score, half-time score
local GAMES = {
  { -82, "PL", "NBR", "HBC", "IN_PLAY", { 2, 1 }, { 1, 1 } },
  { -52, "BL1", "KFU", "RIV", "PAUSED", { 1, 1 }, { 1, 1 } },
  { -21, "PL", "ASH", "WST", "IN_PLAY", { 0, 0 }, { nil, nil } },
  { 75, "CL", "EVA", "BFA", "TIMED", { nil, nil }, { nil, nil } },
  { -240, "PL", "HBC", "KFU", "FINISHED", { 3, 0 }, { 2, 0 } },
  { -300, "BL1", "WST", "EVA", "FINISHED", { 1, 2 }, { 0, 1 } },
  { -1500, "CL", "BFA", "ASH", "FINISHED", { 2, 2 }, { 1, 0 } },
}

local COMP = { PL = "Premier League", BL1 = "Bundesliga", CL = "UEFA Champions League" }

local function iso(t)
  local d = sys.date(t, "UTC")
  return string.format("%04d-%02d-%02dT%02d:%02d:00Z", d.year, d.month, d.day, d.hour, d.min)
end

local function team(tla)
  local t = TEAMS[tla]
  return { id = t[1], name = t[2], shortName = t[3], tla = tla, crest = "https://example.com/crests/" .. tla .. ".jpg" }
end

local function match(i, g, now)
  return {
    id = 9000 + i, utcDate = iso(now + g[1] * 60), status = g[5], matchday = 12,
    competition = { code = g[2], name = COMP[g[2]] },
    homeTeam = team(g[3]), awayTeam = team(g[4]),
    score = { fullTime = { home = g[6][1], away = g[6][2] }, halfTime = { home = g[7][1], away = g[7][2] } },
    venue = TEAMS[g[3]][4],
  }
end

function M.get(url)
  local now = sys.time() or 1794306245
  if url:match("/v4/matches%?") then
    local list = {}
    for i, g in ipairs(GAMES) do list[i] = match(i, g, now) end
    return 200, json.encode({ matches = list }), {}
  end
  local id = tonumber(url:match("/v4/matches/(%d+)$"))
  if id and GAMES[id - 9000] then return 200, json.encode(match(id - 9000, GAMES[id - 9000], now)), {} end
  local tla = url:match("/crests/(%u+)%.jpg$")
  if tla then return 200, assert(asset(tla .. ".jpg")), {} end
  return 404, "", {}
end

function M.close_idle() end

function M.request(t)
  return M.get(type(t) == "string" and t or t.url)
end

return M
