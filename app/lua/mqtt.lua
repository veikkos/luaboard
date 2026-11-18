-- mqtt.lua: MQTT 3.1.1 client, built into the firmware (require("mqtt")).
-- Upload your own mqtt.lua to replace it.
--
--   task(function()
--     local mqtt = require("mqtt")
--     local c = assert(mqtt.connect("mqtt://192.168.1.10:1883", {   -- or mqtts:// (TLS)
--       client_id = "weather-1", username = "u", password = "p",
--       will = { topic = "weather-1/status", payload = "offline", retain = true },
--     }))
--     c.on_connect = function() c:publish("weather-1/status", "online", { retain = true }) end
--     c:subscribe("weather-1/cmd/#", function(topic, msg) print(topic, msg) end)
--     c:publish("weather-1/temp", "21.5", { qos = 1 })
--   end)
--
-- The client reconnects by itself (1, 2, 4 ... 30 s) and keeps its
-- subscriptions. publish() while disconnected queues the message; the queue
-- is sent in order after reconnecting. QoS 1 messages that weren't confirmed
-- when the connection dropped are sent again (at-least-once: the backend may
-- see a duplicate). The queue lives in RAM: a script restart or reboot loses it.
--
-- connect() and waiting for QoS 1 confirmations need task(); QoS 0 publish
-- (and queueing) also works from timers and loop().

local M = {}

local CONNECT, CONNACK, PUBLISH, PUBACK = 0x10, 0x20, 0x30, 0x40
local SUBSCRIBE, SUBACK, PINGREQ, PINGRESP, DISCONNECT = 0x82, 0x90, 0xC0, 0xD0, 0xE0

local CONNACK_ERRORS = {
  [1] = 'unacceptable protocol version', [2] = 'client id rejected',
  [3] = 'server unavailable', [4] = 'bad user name or password', [5] = 'not authorized',
}

-------------------------------------------------------------- encoding

local function enc_len(n)                 -- remaining-length varint
  local out = {}
  repeat
    local b = n % 128
    n = n // 128
    if n > 0 then b = b | 0x80 end
    out[#out + 1] = string.char(b)
  until n == 0
  return table.concat(out)
end

local function packet(first, body) return string.char(first) .. enc_len(#body) .. body end
local function str(s) return string.pack('>s2', s) end

-- MQTT topic filter matching with + and #.
local function matches(filter, topic)
  local f, t = {}, {}
  for p in (filter .. '/'):gmatch('([^/]*)/') do f[#f + 1] = p end
  for p in (topic .. '/'):gmatch('([^/]*)/') do t[#t + 1] = p end
  for i = 1, #f do
    if f[i] == '#' then return true end
    if t[i] == nil then return false end
    if f[i] ~= '+' and f[i] ~= t[i] then return false end
  end
  return #t == #f
end
M.matches = matches

local fail = sys._error   -- a wrong call: the error points at the app's line

-- A flag option: true, false or left out.
local function flag(opts, k, where)
  local v = opts[k]
  if v ~= nil and type(v) ~= 'boolean' then fail(where .. ': opts.' .. k .. ' must be true or false') end
  return v
end

-- "mqtt://host[:port]", "mqtts://host[:port]" (TLS) or "host[:port]".
local function parse_url(url, opts)
  local scheme, host, port = tostring(url):match('^(mqtts?)://([^:/]+):?(%d*)$')
  if not host then host, port = tostring(url):match('^([^:/]+):?(%d*)$') end
  if not host then fail('bad broker address: ' .. tostring(url)) end
  local tls = scheme == 'mqtts' or opts.tls or opts.ca ~= nil
  return host, tonumber(port) or opts.port or (tls and 8883 or 1883), tls
end

local function read_packet(r, timeout_ms, max)
  local first, err = r:read(1, timeout_ms)
  if not first then return nil, err end
  local len, mult, n = 0, 1, 0
  repeat
    local b = r:read(1)
    if not b then return nil, 'closed' end
    b = b:byte()
    len = len + (b & 0x7F) * mult
    mult = mult * 128
    n = n + 1
    if n == 4 and b & 0x80 ~= 0 then return nil, 'bad packet length from the broker' end
  until b & 0x80 == 0
  if len > max then return nil, 'packet too big (' .. len .. ' bytes, max_size ' .. max .. ')' end
  local body = len > 0 and r:read(len) or ''
  if not body then return nil, 'closed' end
  return first:byte(), body
end

---------------------------------------------------------------- client

local Client = {}
Client.__index = Client

function Client:_next_id()
  local id = self.pid
  self.pid = self.pid % 65535 + 1
  return id
end

function Client:_raw_send(bytes)
  local ok, err = self.sock:write_all(bytes)
  if not ok then self:_lost(err) return nil, err end
  self.last_send = sys.millis()
  return true
end

-- Put a message in the offline queue (bounded: the oldest is dropped).
function Client:_enqueue(msg, front)
  if self.max_queue <= 0 then return nil, 'not connected' end
  if front then table.insert(self.queue, 1, msg) else self.queue[#self.queue + 1] = msg end
  while #self.queue > self.max_queue do
    table.remove(self.queue, 1)
    self.dropped = self.dropped + 1
  end
  return true, 'queued'
end

-- Send one message now. QoS 1 messages stay in `pending` until PUBACK.
function Client:_send_msg(msg)
  local first = PUBLISH | (msg.qos << 1) | (msg.retain and 1 or 0)
  local body = str(msg.topic)
  local id
  if msg.qos == 1 then
    id = self:_next_id()
    body = body .. string.pack('>I2', id)
    self.pending[id] = msg
  end
  local ok, err = self:_raw_send(packet(first, body .. msg.payload))
  if not ok then return nil, err end
  return true, id
end

-- Send queued messages in order while connected.
function Client:_flush()
  while self.connected and #self.queue > 0 do
    local msg = table.remove(self.queue, 1)
    local ok = self:_send_msg(msg)
    if not ok then return end                     -- _lost() re-queued pending ones
  end
end

-- Connection gone (error, broker closed, keepalive timeout).
function Client:_lost(err)
  if not self.connected then return end
  self.connected = false
  self.gen = self.gen + 1                         -- stops this connection's reader
  self.sock:close()
  -- Unconfirmed QoS 1 messages go back to the front of the queue, in order.
  local ids = {}
  for id in pairs(self.pending) do ids[#ids + 1] = id end
  table.sort(ids, function(a, b) return a > b end)
  for _, id in ipairs(ids) do
    local m = self.pending[id]
    if m ~= true then self:_enqueue(m, true) end
  end
  self.pending = {}
  if self.on_close then self.on_close(err) end
end

-- One connection attempt: TCP, CONNECT (with will), CONNACK.
function Client:_open()
  local o = self.opts
  local s, err = net.connect(self.host, self.port, self.tls and { tls = true, ca = o.ca } or nil)
  if not s then return nil, err end
  local ok
  ok, err = s:wait_connected(o.timeout)
  if not ok then return nil, err end

  local flags = (o.clean == false) and 0 or 0x02
  local tail = str(self.client_id)
  if o.will then
    local w = o.will
    flags = flags | 0x04 | ((w.qos or 0) << 3) | (w.retain and 0x20 or 0)
    tail = tail .. str(w.topic) .. str(tostring(w.payload or ''))
  end
  if o.username then flags = flags | 0x80; tail = tail .. str(o.username) end
  if o.password then flags = flags | 0x40; tail = tail .. str(o.password) end
  local body = str('MQTT') .. string.char(4, flags) .. string.pack('>I2', self.keepalive) .. tail
  ok, err = s:write_all(packet(CONNECT, body))
  if not ok then s:close() return nil, err end

  -- A half-dead broker may accept TCP and never answer: don't wait forever.
  local r = s:reader()
  local kind, ack = read_packet(r, o.timeout or 5000, 16)
  if not kind or kind ~= CONNACK or #ack < 2 then s:close() return nil, 'no CONNACK from broker' end
  local rc = ack:byte(2)
  if rc ~= 0 then s:close() return nil, 'broker refused: ' .. (CONNACK_ERRORS[rc] or rc) end

  self.sock, self.reader = s, r
  self.connected = true
  self.gen = self.gen + 1
  self.last_send, self.last_recv = sys.millis(), sys.millis()
  local gen = self.gen
  task(function() self:_read_loop(gen) end)

  -- Restore subscriptions, then send what was queued while offline.
  for _, sub in ipairs(self.subs) do
    local id = self:_next_id()
    self.pending[id] = true
    self:_raw_send(packet(SUBSCRIBE, string.pack('>I2', id) .. str(sub.filter) .. string.char(sub.qos)))
  end
  self:_flush()
  if self.connected and self.on_connect then self.on_connect() end
  return true
end

-- A PUBLISH body: qos, topic, packet id (qos > 0), payload. Errors on a
-- body too short for its own lengths.
local function parse_publish(kind, body)
  local qos = (kind >> 1) & 3
  local topic, pos = string.unpack('>s2', body)
  local id
  if qos > 0 then id, pos = string.unpack('>I2', body, pos) end
  return qos, topic, id, body:sub(pos)
end

-- Incoming packets for one connection (generation `gen`).
function Client:_read_loop(gen)
  local r = self.reader
  while self.connected and self.gen == gen do
    local kind, body = read_packet(r, nil, self.max_size)
    if self.gen ~= gen then return end            -- a newer connection took over
    if not kind then self:_lost(body or 'closed') return end
    self.last_recv = sys.millis()
    local t = kind & 0xF0
    if t == PUBLISH then
      local ok, qos, topic, id, payload = pcall(parse_publish, kind, body)
      if not ok then self:_lost('bad PUBLISH from the broker') return end
      if qos == 1 then self:_raw_send(string.char(PUBACK, 2) .. string.pack('>I2', id)) end
      for _, s in ipairs(self.subs) do
        if matches(s.filter, topic) then s.fn(topic, payload) end
      end
    elseif t == PUBACK or t == SUBACK then
      if #body < 2 then self:_lost('bad ack from the broker') return end
      self.pending[string.unpack('>I2', body)] = nil
    end                                           -- PINGRESP: last_recv updated above
  end
end

-- Keepalive pings, dead-connection detection, reconnecting with back-off.
function Client:_supervise()
  local backoff = 1000
  while not self.closing do
    if self.connected then
      local now = sys.millis()
      if now - self.last_recv > self.keepalive * 1500 then
        self:_lost('keepalive timeout (broker silent)')
      elseif now - self.last_send >= self.keepalive * 500 then
        self:_raw_send(string.char(PINGREQ, 0))
      end
      sleep(500)
    elseif self.opts.reconnect == false then
      return
    else
      sleep(backoff)
      if self.closing then return end
      local ok = self:_open()
      backoff = ok and 1000 or math.min(backoff * 2, 30000)
    end
  end
end

-------------------------------------------------------------- public API

function Client:publish(topic, payload, opts)
  opts = opts or {}
  local qos = opts.qos or 0
  if qos ~= 0 and qos ~= 1 then fail('qos must be 0 or 1') end
  local msg = { topic = topic, payload = tostring(payload or ''), qos = qos, retain = flag(opts, 'retain', 'publish') or false }
  if self.closing then return nil, 'closed' end
  if not self.connected or #self.queue > 0 then   -- keep order behind queued messages
    local ok, err = self:_enqueue(msg)
    self:_flush()
    return ok, err
  end
  local ok, id = self:_send_msg(msg)
  if not ok then return self:_enqueue(msg) end    -- lost while sending: keep it
  if qos == 1 and in_task() then                  -- wait for PUBACK
    local deadline = sys.millis() + (opts.timeout or 5000)
    while self.pending[id] do
      if not self.connected then return true, 'queued' end   -- re-queued by _lost()
      if sys.millis() >= deadline then return nil, 'timeout' end
      yield()
    end
  end
  return true
end

function Client:subscribe(filter, fn, qos)
  if type(fn) ~= 'function' then fail('subscribe needs a callback') end
  qos = qos or 0
  self.subs[#self.subs + 1] = { filter = filter, fn = fn, qos = qos }
  if not self.connected then return true, 'queued' end      -- sent on (re)connect
  local id = self:_next_id()
  self.pending[id] = true
  local ok, err = self:_raw_send(packet(SUBSCRIBE, string.pack('>I2', id) .. str(filter) .. string.char(qos)))
  if not ok then return true, 'queued' end
  if in_task() then
    local deadline = sys.millis() + 5000
    while self.pending[id] and self.connected do
      if sys.millis() >= deadline then return nil, 'timeout' end
      yield()
    end
  end
  return true
end

-- Disconnect cleanly (the broker does NOT publish the will) and stop reconnecting.
function Client:close()
  self.closing = true
  if self.connected then
    self.sock:write_all(string.char(DISCONNECT, 0))
    self.connected = false
    self.gen = self.gen + 1
    self.sock:close()
    if self.on_close then self.on_close('closed by script') end
  end
end

function Client:queued() return #self.queue end

-- Connect to a broker. Call inside task(). Returns a client or nil, error.
-- opts: client_id, username, password, keepalive (s, default 60),
--       clean (default true), timeout (ms, default 5000),
--       will = { topic, payload, qos, retain }, tls, ca (PEM),
--       reconnect (default true), queue (max queued messages, default 100; 0 = off),
--       max_size (biggest packet taken from the broker, default 64 KiB)
function M.connect(url, opts)
  if not in_task() then fail('mqtt.connect() must be called inside task()') end
  opts = opts or {}
  for _, k in ipairs({ 'tls', 'clean', 'reconnect' }) do flag(opts, k, 'mqtt.connect') end
  local host, port, tls = parse_url(url, opts)
  -- The default id is the board's own, so two boards running the same app
  -- don't push each other off the broker.
  local c = setmetatable({
    host = host, port = port, tls = tls, opts = opts,
    client_id = opts.client_id or ('luaboard-' .. sys.info().id),
    keepalive = opts.keepalive or 60,
    max_queue = opts.queue or 100,
    max_size = opts.max_size or 64 * 1024,
    connected = false, closing = false, gen = 0, pid = 1,
    subs = {}, pending = {}, queue = {}, dropped = 0,
  }, Client)
  local ok, err = c:_open()
  if not ok then return nil, err end
  task(function() c:_supervise() end)
  return c
end

return M
