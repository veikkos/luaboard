-- prelude.lua: built into the firmware, runs before every script.
-- Timers, tasks (coroutines) and network helpers. Returns the tick function
-- the firmware calls at sys.rate() Hz; it returns whether the script still has
-- anything to run (loop(), timers or tasks).

---------------------------------------------------------------- timers
local timers, next_id = {}, 1
timer = {}

local function add_timer(ms, fn, every)
  assert(type(fn) == 'function', 'timer callback must be a function')
  local id = next_id; next_id = id + 1
  timers[id] = { at = sys.millis() + ms, every = every and ms or nil, fn = fn }
  return id
end
function timer.after(ms, fn) return add_timer(ms, fn, false) end
function timer.every(ms, fn) assert(ms >= 10, 'interval must be >= 10 ms') return add_timer(ms, fn, true) end
function timer.cancel(id) timers[id] = nil end

----------------------------------------------------------------- tasks
-- task(fn, ...) runs fn as a coroutine. Inside a task, sleep(ms) and yield()
-- give the device back (no busy waiting), so network code can be written
-- top to bottom. Each tick resumes every task that is due.
local tasks, current = {}, nil

function task(fn, ...)
  assert(type(fn) == 'function', 'task() needs a function')
  local args = table.pack(...)
  local t = { co = coroutine.create(function() return fn(table.unpack(args, 1, args.n)) end), wake = 0 }
  tasks[#tasks + 1] = t
end

function in_task() return current ~= nil end

function sleep(ms)
  assert(current, 'sleep() only works inside task()')
  current.wake = sys.millis() + (ms or 0)
  coroutine.yield()
end

function yield()
  assert(current, 'yield() only works inside task()')
  current.wake = 0
  coroutine.yield()
end

---------------------------------------------------------- socket methods
-- The waiting ones, on top of the non-blocking ones in C.
local sock = net._methods
net._methods = nil

-- Wait until a socket from net.connect() is connected.
function sock:wait_connected(timeout_ms)
  local deadline = sys.millis() + (timeout_ms or 5000)
  while true do
    local st, err = self:status()
    if st == 'connected' then return true end
    if st == 'closed' then return nil, 'cannot connect to ' .. self:peer() .. ': ' .. (err or 'failed') end
    if sys.millis() >= deadline then self:close(); return nil, 'cannot connect to ' .. self:peer() .. ': timeout' end
    if current then yield() else return nil, 'wait_connected() needs task()' end
  end
end

-- Send everything. Inside a task it waits for buffer space; outside it
-- gives up if the socket buffer is full.
function sock:write_all(data)
  local s = self
  local pos, tries = 1, 0
  while pos <= #data do
    local n, err = s:send(data:sub(pos))
    if not n then return nil, err end
    pos = pos + n
    if n == 0 then
      if current then yield()
      else
        tries = tries + 1
        if tries > 50 then return nil, 'socket buffer full' end
        sys.delay(1)
      end
    end
  end
  return true
end

-- Buffered reader: r:read(n, [timeout_ms]) returns exactly n bytes,
-- r:line([timeout_ms]) one line without "\n", r:chunk([timeout_ms]) whatever
-- has arrived. They wait (inside a task) and return nil, "closed" at the end
-- or nil, "timeout".
function sock:reader()
  local s = self
  -- Unread data: head from pos on, then the chunks received since (joined
  -- into head only when needed, so a big body isn't copied for every chunk).
  local head, pos, parts, size = '', 1, {}, 0
  local nl = false                  -- one of parts has a "\n"
  local r = {}
  local function fill(deadline)
    local d, err = s:recv(4096)
    if not d then return nil, err end
    if d == '' then
      if not current then return nil, 'reader() needs task()' end
      if deadline and sys.millis() >= deadline then return nil, 'timeout' end
      yield()
    else
      parts[#parts + 1] = d
      size = size + #d
      nl = nl or d:find('\n', 1, true) ~= nil
    end
    return true
  end
  local function join()
    if #parts == 0 then return end
    if pos <= #head then table.insert(parts, 1, pos == 1 and head or head:sub(pos)) end
    head, pos, parts, nl = net._join(parts), 1, {}, false
  end
  local function take(n)              -- n bytes from head (joined), n <= its unread part
    local out = (pos == 1 and n == #head) and head or head:sub(pos, pos + n - 1)
    pos, size = pos + n, size - n
    if pos > #head then head, pos = '', 1 end
    return out
  end
  function r:read(n, timeout_ms)
    local deadline = timeout_ms and sys.millis() + timeout_ms
    while size < n do
      local ok, err = fill(deadline)
      if not ok then return nil, err end
    end
    join()
    return take(n)
  end
  -- Whatever has arrived (at least 1 byte), for reading until the peer closes.
  function r:chunk(timeout_ms)
    local deadline = timeout_ms and sys.millis() + timeout_ms
    while size == 0 do
      local ok, err = fill(deadline)
      if not ok then return nil, err end
    end
    join()
    return take(size)
  end
  function r:line(timeout_ms)
    local deadline = timeout_ms and sys.millis() + timeout_ms
    while true do
      local i = head:find('\n', pos, true)
      if i then return (take(i - pos + 1):sub(1, -2):gsub('\r$', '')) end
      if nl then join()
      else
        local ok, err = fill(deadline)
        if not ok then return nil, err end
      end
    end
  end
  return r
end

------------------------------------------------------------ ble helpers
-- ble.scan(fn, [opts]) calls fn(adv) for each device heard from, again
-- whenever it advertises something new, until ble.stop_scan(). Connections
-- get read/write/subscribe here, on top of the non-blocking ones in C.
local ble_scan_fn
local ble_subs = setmetatable({}, { __mode = 'k' })   -- connection -> { ["svc/chr"] = fn }

function ble.scan(fn, opts)
  assert(type(fn) == 'function', 'ble.scan() needs a function')
  local ok, err = ble._scan(type(opts) == 'table' and opts.active == true)
  if not ok then return nil, err end
  ble_scan_fn = fn
  return true
end

function ble.stop_scan()
  ble_scan_fn = nil
  ble._stop_scan()
end

local function ble_wait(c, what, token, err)
  if not token then return nil, what .. ': ' .. err end
  while true do
    local st, res = c:_poll(token)
    if st == 'done' then return res end
    if st == 'error' then return nil, what .. ': ' .. res end
    yield()
  end
end

local function ble_key(svc, chr)
  local s, c = ble.uuid(svc), ble.uuid(chr)
  return s and c and s .. '/' .. c
end

local conn = ble._methods
ble._methods = nil

-- Wait until a connection from ble.connect() is up (and its services known).
function conn:wait_connected(timeout_ms)
  local deadline = sys.millis() + (timeout_ms or 20000)
  while true do
    local st, err = self:status()
    if st == 'connected' then return true end
    if st == 'closed' then
      return nil, 'cannot connect to ' .. self:addr() .. ': ' .. (err or 'failed'):gsub('^cannot connect: ', '')
    end
    if sys.millis() >= deadline then self:close() return nil, 'cannot connect to ' .. self:addr() .. ': timeout' end
    if current then yield() else return nil, 'wait_connected() needs task()' end
  end
end

function conn:read(svc, chr)
  assert(current, 'read() only works inside task()')
  return ble_wait(self, 'read ' .. tostring(chr), self:_read(svc, chr))
end

function conn:write(svc, chr, data)
  assert(current, 'write() only works inside task()')
  local ok, err = ble_wait(self, 'write ' .. tostring(chr), self:_write(svc, chr, data))
  if ok then return true end
  return nil, err
end

-- fn(data, chr, svc) for each notification (or indication) of the value.
function conn:subscribe(svc, chr, fn)
  assert(type(fn) == 'function', 'subscribe() needs a function')
  assert(current, 'subscribe() only works inside task()')
  local key = ble_key(svc, chr)
  local subs = ble_subs[self] or {}
  ble_subs[self] = subs
  if key then subs[key] = fn end          -- before asking: the first value can come at once
  local ok, err = ble_wait(self, 'subscribe ' .. tostring(chr), self:_subscribe(svc, chr, true))
  if ok then return true end
  if key then subs[key] = nil end
  return nil, err
end

function conn:unsubscribe(svc, chr)
  local key, subs = ble_key(svc, chr), ble_subs[self]
  if subs and key then subs[key] = nil end
  if not current then return true end     -- (outside a task the device keeps sending; it's ignored)
  local ok, err = ble_wait(self, 'unsubscribe ' .. tostring(chr), self:_subscribe(svc, chr, false))
  if ok then return true end
  return nil, err
end

--------------------------------------------------------- browser login
-- browser.login(url, redirect, [title], [timeout_ms]) -> query string | nil, err
-- Asks the connected studio / CLI to open `url` in the computer's browser and
-- to catch the redirect to `redirect` (http://127.0.0.1:<port>/...). Waits
-- (inside a task) until the user has logged in, declined, or the timeout
-- (default 10 minutes) passes. See oauth.lua for the usual use.
function browser.login(url, redirect, title, timeout_ms)
  assert(current, 'browser.login() only works inside task()')
  local id = browser._start(url, redirect, title)
  local deadline = sys.millis() + (timeout_ms or 600000)
  while true do
    local st, res = browser._poll(id)
    if st == 'done' then return res end
    if st ~= 'pending' then return nil, 'login cancelled' end
    if sys.millis() >= deadline then browser._cancel(id) return nil, 'login timed out' end
    sleep(250)
  end
end

---------------------------------------------------------- touch events
-- touch.on(kind, fn): the system tells presses from long presses (one
-- timing for every app and the menu), and each touch is one event, decided
-- by where it went down (the strip: a button; the screen: tap/hold/drag):
--   "press" fn("A"|"B"|"C")   let go before the long-press time
--   "long"  fn("A"|"C")       held that long (no "press" follows); holding
--                             B always leaves the app instead
--   "tap"   fn(x, y)          on the screen, let go before that time
--   "hold"  fn(x, y)          held in place on the screen that long
--   "drag"  fn(x, y, x0, y0, ended)  the finger went further than a tap's
--                             wiggle room: at each new (x, y), and once more
--                             where it was let go (ended true)
--   "swipe" fn(dir, x, y)     "up" "down" "left" "right", from (x, y): a drag
--                             let go away from where it started
-- fn nil removes the handler.
local touch_kinds = { press = true, long = true, tap = true, hold = true, drag = true, swipe = true }
local touch_handlers = {}

function touch.on(kind, fn)
  assert(touch_kinds[kind], 'touch.on: kind is "press", "long", "tap", "hold", "drag" or "swipe"')
  assert(fn == nil or type(fn) == 'function', 'touch.on: the handler must be a function (or nil)')
  touch_handlers[kind] = fn
end

------------------------------------------------------------------ tick
return function(now, dt)
  for _ = 1, 16 do
    local kind, a, b, c, d, e = touch._event()
    if not kind then break end
    local fn = touch_handlers[kind]
    if fn then fn(a, b, c, d, e) end
  end

  if ble_scan_fn then
    for _ = 1, 32 do                      -- each device at most once a tick
      local a = ble._adv()
      if not a or not ble_scan_fn then break end
      ble_scan_fn(a)
    end
  end
  for c, subs in pairs(ble_subs) do
    for _ = 1, 16 do
      local svc, chr, data = c:_notified()
      if not svc then break end
      local fn = subs[svc .. '/' .. chr]
      if fn then fn(data, chr, svc) end
    end
  end

  local due = {}
  for id, t in pairs(timers) do if now >= t.at then due[#due + 1] = id end end
  table.sort(due)
  for _, id in ipairs(due) do
    local t = timers[id]
    if t then
      if t.every then t.at = math.max(t.at + t.every, now + 1) else timers[id] = nil end
      t.fn()
    end
  end

  local i = 1
  while i <= #tasks do
    local t = tasks[i]
    if now >= t.wake then
      current = t
      local ok, err = coroutine.resume(t.co)
      current = nil
      if not ok then
        table.remove(tasks, i)
        error(err, 0)                     -- stops the script, shown on screen
      elseif coroutine.status(t.co) == 'dead' then
        table.remove(tasks, i)
      else
        i = i + 1
      end
    else
      i = i + 1
    end
  end

  if type(loop) == 'function' then loop(dt) return true end
  -- false: the script has nothing left to do
  return next(timers) ~= nil or #tasks > 0 or ble_scan_fn ~= nil or next(ble_subs) ~= nil
    or next(touch_handlers) ~= nil
end
