-- A stand-in for the gcal example's Google Calendar client, for the showcase
-- GIF: one calendar with a few meetings around the device's clock, the next
-- one starting at the coming half hour, so its reminder (10 min) is due.
-- Uploaded into the app, it replaces marketplace/gcal/gcal.lua.

local M = {}
local Cal = {}
Cal.__index = Cal

function M.new() return setmetatable({}, Cal) end

function Cal:calendars()
  return { { id = "me", name = "Me", primary = true, defaults = { 10 } } }
end

function Cal:events()
  local now = sys.time() or 0
  local half = now - now % 1800 + 1800               -- the next :00 or :30
  local function ev(id, title, start, mins)
    return { id = "me/" .. id, title = title, start = start, stop = start + mins * 60, reminders = { 10 } }
  end
  return {
    ev("review", "Design review", half, 30),
    ev("anna", "1:1 with Anna", half + 5400, 30),
    ev("planning", "Sprint planning", half + 3 * 3600, 60),
    ev("retro", "Team retro", half + 24 * 3600, 45),
  }
end

function Cal:logged_in() return true end
function Cal:logout() end

return M
