-- orbit: a ring of dots around a title

local TITLE = "Hello"
local COLOR = disp.ORANGE
local DOTS = 12

function loop()
  local t = sys.millis() / 1600
  disp.clear(disp.rgb(10, 14, 30))
  for i = 1, DOTS do
    local a = t + i * 2 * math.pi / DOTS
    local x = 160 + math.cos(a) * 96
    local y = 120 + math.sin(a) * 96
    disp.circle(x // 1, y // 1, 9, COLOR, true)
  end
  local w = disp.text_width(TITLE, 3)
  disp.text(160 - w // 2, 110, TITLE, disp.WHITE, 3)
end
