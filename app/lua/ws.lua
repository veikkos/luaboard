-- WebSocket client (RFC 6455) on the net sockets, for text and binary
-- messages. Call connect and recv inside task().
--
--   local ws = require("ws")
--   local w, err = ws.connect("wss://host[:port]/path", opts)
--     opts: ca, cert_name (as net.connect's, for wss://), headers (a table,
--     sent with the request), timeout (ms for connecting, default 8000)
--   w:send(data, [binary])   -> true | nil, err
--   w:recv([timeout_ms])     -> data, binary | nil, "timeout" | nil, err
--   w:close()
--
-- The server's pings are answered while recv waits, a message that comes in
-- several frames is put together, and one longer than 256 KiB closes the
-- connection. A message sent is one frame of at most 64 KiB.
--
-- What goes out is queued whole frames, sent from the front: a pong and a
-- message sent from a touch handler meanwhile never mix, and a send outside
-- a task that doesn't fit the socket at once leaves the rest queued (it goes
-- with the next send or recv), never half a frame.

local M = {}
local W = {}
W.__index = W

local fail = sys._error   -- a wrong call: the error points at the app's line

local MAX_RECV = 256 * 1024
local MAX_SEND = 64 * 1024
local MAX_QUEUED = 256 * 1024                       -- sent but not out yet: more is refused
local GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'   -- (the protocol's: hashed with the key into the server's answer)
local TEXT, BINARY, CLOSE, PING, PONG = 1, 2, 8, 9, 10

-- "ws://host[:port][/path]" or "wss://..." (TLS).
local function parse_url(url)
  if type(url) ~= 'string' then fail('ws.connect: the url must be a string, not ' .. type(url)) end
  local scheme, rest = url:match('^(%a+)://(.*)$')
  scheme = scheme and scheme:lower()
  if scheme ~= 'ws' and scheme ~= 'wss' then fail('ws.connect: the url must start with ws:// or wss://: ' .. url) end
  local hostport, path = rest:match('^([^/?#]+)(.*)$')
  if not hostport then fail('ws.connect: bad url: ' .. url) end
  local host, port = hostport:match('^(.-):(%d+)$')
  host = host or hostport
  port = tonumber(port) or (scheme == 'wss' and 443 or 80)
  if path == '' or path:sub(1, 1) ~= '/' then path = '/' .. path end
  return scheme == 'wss', host, port, (path:gsub('#.*$', ''))
end

-- One frame from this end: masked, as a client's have to be.
local function frame(op, payload)
  local n = #payload
  local head
  if n < 126 then head = string.char(0x80 | op, 0x80 | n)
  elseif n < 65536 then head = string.char(0x80 | op, 0x80 | 126, n >> 8, n & 255)
  else head = string.char(0x80 | op, 0x80 | 127) .. string.pack('>I8', n) end
  local mask = crypto.random(4)
  return head .. mask .. net._xor(payload, mask)
end

function M.connect(url, opts)
  if not in_task() then fail('ws.connect() must be called inside task()') end
  opts = opts or {}
  if type(opts) ~= 'table' then fail('ws.connect: opts must be a table') end
  local tls, host, port, path = parse_url(url)
  local timeout = opts.timeout or 8000
  local deadline = sys.millis() + timeout
  local s, err = net.connect(host, port, tls and { tls = true, ca = opts.ca, cert_name = opts.cert_name } or nil)
  if not s then return nil, err end
  local ok
  ok, err = s:wait_connected(timeout)
  if not ok then return nil, err end

  local key = crypto.base64(crypto.random(16))
  local default_port = port == (tls and 443 or 80)
  local lines = {
    'GET ' .. path .. ' HTTP/1.1', 'Host: ' .. host .. (default_port and '' or ':' .. port),
    'Upgrade: websocket', 'Connection: Upgrade', 'Sec-WebSocket-Key: ' .. key, 'Sec-WebSocket-Version: 13',
  }
  for k, v in pairs(opts.headers or {}) do lines[#lines + 1] = k .. ': ' .. tostring(v) end
  lines[#lines + 1], lines[#lines + 2] = '', ''
  ok, err = s:write_all(table.concat(lines, '\r\n'))

  -- Its answer: 101, and the key hashed back, which only a WebSocket server does.
  local r = s:reader()
  local function left() return math.max(1, deadline - sys.millis()) end
  local line, accept
  if ok then line, err = r:line(left(), 1024) end
  if line and not line:match('^HTTP/1%.1 101') then line, err = nil, 'not a WebSocket: ' .. line:sub(1, 60) end
  while line and line ~= '' do
    line, err = r:line(left(), 8192)
    local name, value = (line or ''):match('^([^:]+):%s*(.-)%s*$')
    if name and name:lower() == 'sec-websocket-accept' then accept = value end
  end
  if line and accept ~= crypto.base64(crypto.sha1(key .. GUID)) then line, err = nil, 'not a WebSocket: the wrong answer to the key' end
  if not line then s:close() return nil, err end
  return setmetatable({ s = s, r = r, url = url, out = '' }, W)
end

function W:close()
  if not self.s then return end
  if self.out == '' then self.s:send(frame(CLOSE, '')) end   -- (if it goes: nobody waits for it)
  self.s:close()
  self.s, self.out = nil, ''
end

-- Send what is queued, as much as the socket takes now; with `wait` (in a
-- task) until all of it has gone. True, or nil, err (the connection closed).
local function flush(self, wait)
  while self.out ~= '' do
    local n, err = self.s:send(self.out)
    if not n then
      self:close()
      return nil, err
    end
    self.out = self.out:sub(n + 1)               -- (no yield between the send and this: writers can't mix)
    if n == 0 then
      if not wait then return true end
      yield()
      if not self.s then return nil, 'closed' end
    end
  end
  return true
end

local function queue(self, f)
  if #self.out + #f > MAX_QUEUED then return nil, 'too much waiting to be sent' end
  self.out = self.out .. f
  return flush(self, in_task())
end

function W:send(data, binary)
  if type(data) ~= 'string' then fail('ws send: the message must be a string, not ' .. type(data)) end
  if #data > MAX_SEND then fail('ws send: a message takes at most ' .. MAX_SEND .. ' bytes') end
  if not self.s then return nil, 'closed' end
  return queue(self, frame(binary and BINARY or TEXT, data))
end

-- The next message: data, and true if it's a binary one. timeout_ms is for
-- the whole call, the pings answered meanwhile included. A frame that has
-- started is read whole (its parts follow at once). A message whose first
-- frames have come is kept when the time runs out, and the next call goes on
-- with it: nothing is lost by a timeout.
function W:recv(timeout_ms)
  if not in_task() then fail('ws recv() must be called inside task()') end
  if not self.s then return nil, 'closed' end
  local r = self.r
  local deadline = timeout_ms and sys.millis() + timeout_ms
  local function give_up(why) self:close() return nil, why end
  while true do
    local ok, err = flush(self, false)             -- (what a send outside a task left queued)
    if not ok then return nil, err end
    local h
    h, err = r:read(2, deadline and math.max(1, deadline - sys.millis()))
    if not h then
      if err == 'timeout' then return nil, err end
      return give_up(err or 'closed')
    end
    local b1, b2 = h:byte(1, 2)
    local op, n = b1 & 15, b2 & 127
    if n == 126 then
      h, err = r:read(2, 10000)
      n = h and string.unpack('>I2', h)
    elseif n == 127 then
      h, err = r:read(8, 10000)
      n = h and string.unpack('>I8', h)
    end
    if not n then return give_up(err or 'closed') end
    if n < 0 or n > MAX_RECV then return give_up('message too long') end
    local mask, p = false, ''
    if b2 & 128 ~= 0 then                          -- (a server doesn't mask, but may)
      mask, err = r:read(4, 10000)
      if not mask then return give_up(err or 'closed') end
    end
    if n > 0 then
      p, err = r:read(n, 10000)
      if not p then return give_up(err or 'closed') end
    end
    if mask then p = net._xor(p, mask) end
    if op == CLOSE then return give_up('closed')
    elseif op == PING then
      ok, err = queue(self, frame(PONG, p))
      if not ok then return nil, err end
    elseif op == TEXT or op == BINARY then self.parts, self.kind, self.size = { p }, op, n
    elseif op == 0 and self.parts then
      self.parts[#self.parts + 1] = p
      self.size = self.size + n
      if self.size > MAX_RECV then return give_up('message too long') end
    end
    if self.parts and op <= BINARY and b1 & 128 ~= 0 then   -- the message's last frame
      local data, binary = net._join(self.parts), self.kind == BINARY
      self.parts = nil
      return data, binary
    end
  end
end

return M
