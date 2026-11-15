-- http.lua: HTTP/1.1 client, built into the firmware (require("http")).
--
--   task(function()
--     local http = require("http")
--     local status, body, headers = http.get("https://api.example.com/v1/data")
--     local status2 = http.request{ method = "POST", url = "...",
--                                   headers = { ["Content-Type"] = "application/json" },
--                                   body = json.encode(t), timeout = 10000 }
--   end)
--
-- request() returns status, body, headers (lowercase names), or nil, err when
-- the request could not be made. Call it inside task(). https:// uses TLS with
-- certificate checks (opts.ca = PEM to trust your own CA). Connections are
-- kept open and reused per host, so polling an API doesn't pay for a new TLS
-- handshake every time. Bodies: Content-Length, chunked, or until close.

local M = {}

local DEFAULT_TIMEOUT = 15000
local DEFAULT_MAX = 256 * 1024

local idle = {}           -- "https://host:port" -> { sock, reader } kept for reuse

function M.urlencode(s)
  return (tostring(s):gsub('[^%w%-%._~]', function(c) return string.format('%%%02X', c:byte()) end))
end

function M.urldecode(s)
  return (s:gsub('+', ' '):gsub('%%(%x%x)', function(h) return string.char(tonumber(h, 16)) end))
end

-- { a = 1, b = "x y" } -> "a=1&b=x%20y" (sorted, so it's predictable)
function M.form(t)
  local keys = {}
  for k in pairs(t) do keys[#keys + 1] = k end
  table.sort(keys)
  local out = {}
  for _, k in ipairs(keys) do out[#out + 1] = M.urlencode(k) .. '=' .. M.urlencode(t[k]) end
  return table.concat(out, '&')
end

-- "a=1&b=x%20y" -> { a = "1", b = "x y" }
function M.parse_query(q)
  local t = {}
  for k, v in (q or ''):gmatch('([^&=?]+)=?([^&]*)') do t[M.urldecode(k)] = M.urldecode(v) end
  return t
end

function M.parse_url(url)
  local scheme, rest = url:match('^(%a+)://(.*)$')
  assert(scheme == 'http' or scheme == 'https', 'url must start with http:// or https://: ' .. tostring(url))
  local hostport, path = rest:match('^([^/?#]+)(.*)$')
  assert(hostport, 'bad url: ' .. url)
  local host, port = hostport:match('^(.-):(%d+)$')
  host = host or hostport
  port = tonumber(port) or (scheme == 'https' and 443 or 80)
  if path == '' or path:sub(1, 1) ~= '/' then path = '/' .. path end
  return scheme, host, port, (path:gsub('#.*$', ''))
end

local function remaining(deadline)
  return math.max(1, deadline - sys.millis())
end

local function open(scheme, host, port, opts, deadline)
  local s, err = net.connect(host, port, scheme == 'https' and { tls = true, ca = opts.ca } or nil)
  if not s then return nil, err end
  local ok
  ok, err = net.wait_connected(s, remaining(deadline))
  if not ok then return nil, err end
  return { sock = s, reader = net.reader(s) }
end

-- Read one response. Returns status, body, headers, keep (connection reusable).
local function read_response(c, method, deadline, max)
  local r = c.reader
  local line, err = r:line(remaining(deadline))
  if not line then return nil, err end
  local status = tonumber(line:match('^HTTP/%d%.%d (%d%d%d)'))
  if not status then return nil, 'bad response: ' .. line:sub(1, 60) end
  local headers = {}
  while true do
    line, err = r:line(remaining(deadline))
    if not line then return nil, err end
    if line == '' then break end
    local k, v = line:match('^([^:]+):%s*(.-)%s*$')
    if k then
      k = k:lower()
      headers[k] = headers[k] and (headers[k] .. ', ' .. v) or v
    end
  end
  local keep = not (headers['connection'] or ''):lower():find('close')
  if status == 100 then return read_response(c, method, deadline, max) end

  local body = ''
  if method == 'HEAD' or status == 204 or status == 304 then
    -- no body
  elseif (headers['transfer-encoding'] or ''):lower():find('chunked') then
    local parts, total = {}, 0
    while true do
      line, err = r:line(remaining(deadline))
      if not line then return nil, err end
      local hex = line:match('^%x+')
      local n = hex and #hex <= 8 and tonumber(hex, 16)
      if not n then return nil, 'bad chunk header' end
      if n == 0 then
        repeat line, err = r:line(remaining(deadline)) until not line or line == ''   -- trailers
        break
      end
      total = total + n
      if total > max then return nil, 'response too big (max ' .. max .. ' bytes)' end
      local d
      d, err = r:read(n, remaining(deadline))
      if not d then return nil, err end
      parts[#parts + 1] = d
      r:line(remaining(deadline))                    -- CRLF after the chunk
    end
    body = net._join(parts)
  elseif headers['content-length'] then
    local digits = headers['content-length']:match('^%d+$')
    local n = digits and #digits <= 9 and tonumber(digits)
    if not n then return nil, 'bad Content-Length' end
    if n > max then return nil, 'response too big (' .. n .. ' bytes, max ' .. max .. ')' end
    if n > 0 then
      body, err = r:read(n, remaining(deadline))
      if not body then return nil, err end
    end
  else                                               -- body until the server closes
    local parts, total = {}, 0
    while true do
      local d
      d, err = r:chunk(remaining(deadline))
      if not d and err ~= 'closed' then return nil, err end   -- (a timeout isn't its end)
      if not d then break end
      parts[#parts + 1] = d
      total = total + #d
      if total > max then return nil, 'response too big (max ' .. max .. ' bytes)' end
    end
    body, keep = net._join(parts), false
  end
  return status, body, headers, keep
end

function M.request(opts)
  if type(opts) == 'string' then opts = { url = opts } end
  assert(in_task(), 'http.request() must be called inside task()')
  local method = (opts.method or 'GET'):upper()
  local scheme, host, port, path = M.parse_url(opts.url)
  local deadline = sys.millis() + (opts.timeout or DEFAULT_TIMEOUT)
  local max = opts.max_size or DEFAULT_MAX
  local body = opts.body or ''

  local lines = { method .. ' ' .. path .. ' HTTP/1.1' }
  local has = {}
  for k, v in pairs(opts.headers or {}) do
    lines[#lines + 1] = k .. ': ' .. tostring(v)
    has[k:lower()] = true
  end
  local default_port = (scheme == 'https' and port == 443) or (scheme == 'http' and port == 80)
  if not has['host'] then lines[#lines + 1] = 'Host: ' .. host .. (default_port and '' or ':' .. port) end
  if not has['user-agent'] then lines[#lines + 1] = 'User-Agent: luaboard' end
  if not has['content-length'] and (#body > 0 or method == 'POST' or method == 'PUT' or method == 'PATCH') then
    lines[#lines + 1] = 'Content-Length: ' .. #body
  end
  local req = table.concat(lines, '\r\n') .. '\r\n\r\n' .. body

  -- Connections verified against a custom CA are only reused with that CA.
  local key = scheme .. '://' .. host .. ':' .. port
  if scheme == 'https' and opts.ca then key = key .. '#' .. crypto.hex(crypto.sha256(opts.ca)):sub(1, 16) end
  for attempt = 1, 2 do
    local c = idle[key]
    idle[key] = nil
    local reused = c ~= nil
    if c and c.sock:status() ~= 'connected' then c.sock:close(); c, reused = nil, false end
    if not c then
      local err
      c, err = open(scheme, host, port, opts, deadline)
      if not c then return nil, err end
    end
    local ok, err = net.write_all(c.sock, req)
    local status, rbody, headers, keep
    if ok then status, rbody, headers, keep = read_response(c, method, deadline, max) end
    if status then
      if keep and not idle[key] then idle[key] = c else c.sock:close() end
      return status, rbody, headers
    end
    c.sock:close()
    err = err or rbody
    -- A kept connection the server already closed: try once more on a new one.
    if not (reused and attempt == 1 and (err == 'closed' or tostring(err):find('closed'))) then
      return nil, err
    end
  end
end

function M.get(url, headers, opts)
  local o = { url = url, headers = headers }
  for k, v in pairs(opts or {}) do o[k] = v end
  return M.request(o)
end

function M.post(url, body, headers, opts)
  local o = { method = 'POST', url = url, body = body, headers = headers }
  for k, v in pairs(opts or {}) do o[k] = v end
  return M.request(o)
end

-- Close kept connections (they're also closed when the script ends).
function M.close_idle()
  for k, c in pairs(idle) do c.sock:close(); idle[k] = nil end
end

return M
