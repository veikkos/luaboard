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
-- certificate checks (opts.ca = PEM to trust your own CA, opts.cert_name = the
-- name its certificate is for when that isn't the URL's host). Redirects are
-- followed (opts.redirects, default 5; 0 returns the 3xx), without the
-- credentials when to another server (Authorization, Cookie and the names in
-- opts.private_headers, such as an API key's own header). Connections are
-- kept open and reused per host, so polling an API doesn't pay for a new TLS
-- handshake every time; when the device is out of sockets, the one kept
-- longest is let go of. Bodies: Content-Length, chunked, or until close.

local M = {}

local DEFAULT_TIMEOUT = 15000
local DEFAULT_MAX = 256 * 1024
local DEFAULT_REDIRECTS = 5

local idle = {}           -- "https://host:port" -> { sock, reader, used } kept for reuse

local fail = sys._error   -- a wrong call: the error points at the app's line

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
  if type(url) ~= 'string' then fail('url must be a string, not ' .. type(url)) end
  local scheme, rest = url:match('^(%a+)://(.*)$')
  scheme = scheme and scheme:lower()
  if scheme ~= 'http' and scheme ~= 'https' then fail('url must start with http:// or https://: ' .. url) end
  local hostport, path = rest:match('^([^/?#]+)(.*)$')
  if not hostport then fail('bad url: ' .. url) end
  local host, port = hostport:match('^(.-):(%d+)$')
  host = host or hostport
  port = tonumber(port) or (scheme == 'https' and 443 or 80)
  if path == '' or path:sub(1, 1) ~= '/' then path = '/' .. path end
  return scheme, host, port, (path:gsub('#.*$', ''))
end

local function remaining(deadline)
  return math.max(1, deadline - sys.millis())
end

-- net.connect, letting go of kept connections (oldest first) while the
-- device is out of sockets: of TLS ones when it's out of those.
local function connect(host, port, o)
  while true do
    local s, err = net.connect(host, port, o)
    if s or not tostring(err):find('^too many') then return s, err end
    local tls_only, oldest = err:find('TLS') ~= nil, nil
    for k, c in pairs(idle) do
      if (not tls_only or k:find('^https:')) and (not oldest or c.used < idle[oldest].used) then oldest = k end
    end
    if not oldest then return nil, err end
    idle[oldest].sock:close()
    idle[oldest] = nil
  end
end

local function open(scheme, host, port, ca, cert_name, deadline)
  local s, err = connect(host, port, scheme == 'https' and { tls = true, ca = ca, cert_name = cert_name } or nil)
  if not s then return nil, err end
  local ok
  ok, err = s:wait_connected(remaining(deadline))
  if not ok then return nil, err end
  return { sock = s, reader = s:reader() }
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

-- url of a redirect's Location, relative to the url it came from.
local function resolve(loc, base)
  if loc:match('^%a[%w+.-]*://') then return loc end
  local scheme, root = base:match('^(%a+:)(//[^/?#]+)')
  if loc:sub(1, 2) == '//' then return scheme .. loc end
  if loc:sub(1, 1) == '/' then return scheme .. root .. loc end
  local dir = base:gsub('[?#].*$', '')
  if loc:sub(1, 1) == '?' then return dir .. loc end
  if not dir:match('^%a+://[^/]+/') then dir = dir .. '/' end
  return (dir:gsub('[^/]*$', '')) .. loc
end

-- Methods that can be sent twice without harm (RFC 9110): only these are
-- sent again when a kept connection breaks under them. A POST may have
-- arrived already.
local IDEMPOTENT = { GET = true, HEAD = true, PUT = true, DELETE = true, OPTIONS = true, TRACE = true }

-- A kept connection is still good: open, and nothing waiting on it (a
-- server that closed it sent its end, which recv sees now).
local function alive(c)
  return c.sock:status() == 'connected' and c.sock:recv(1) == ''
end

-- One request and its response, no redirects. cert_name: the name this
-- server's certificate is for, if not its host.
local function request1(opts, cert_name, method, url, headers, body, deadline, max)
  local scheme, host, port, path = M.parse_url(url)
  local lines = { method .. ' ' .. path .. ' HTTP/1.1' }
  local has = {}
  for k, v in pairs(headers) do
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

  -- Connections verified against a custom CA, or for another name, are only
  -- reused with that CA and that name (the name last: it may hold any character).
  local key = scheme .. '://' .. host .. ':' .. port
  if scheme == 'https' and (opts.ca or cert_name) then
    key = key .. '#ca=' .. (opts.ca and crypto.hex(crypto.sha256(opts.ca)):sub(1, 16) or '') .. '#name=' .. (cert_name or '')
  end
  for attempt = 1, 2 do
    local c = idle[key]
    idle[key] = nil
    local reused = c ~= nil
    if c and not alive(c) then c.sock:close(); c, reused = nil, false end
    if not c then
      local err
      c, err = open(scheme, host, port, opts.ca, cert_name, deadline)
      if not c then return nil, err end
    end
    local ok, err = c.sock:write_all(req)
    local status, rbody, rheaders, keep
    if ok then status, rbody, rheaders, keep = read_response(c, method, deadline, max) end
    if status then
      c.used = sys.millis()
      if keep and not idle[key] then idle[key] = c else c.sock:close() end
      return status, rbody, rheaders
    end
    c.sock:close()
    err = err or rbody
    -- A kept connection the server closed just now: try once more on a new
    -- one, if the request can be sent twice.
    if not (reused and attempt == 1 and IDEMPOTENT[method] and (err == 'closed' or tostring(err):find('closed'))) then
      return nil, err
    end
  end
end

-- Headers that say who the user is (and the old Host) don't go on to another server.
local PRIVATE = { authorization = true, cookie = true, ['proxy-authorization'] = true, host = true }

function M.request(opts)
  if type(opts) == 'string' then opts = { url = opts } end
  if type(opts) ~= 'table' then fail('http.request() takes a table or a url') end
  if not in_task() then fail('http.request() must be called inside task()') end
  local method = (opts.method or 'GET'):upper()
  local url = opts.url
  M.parse_url(url)                                   -- (a bad url is an error before anything is sent)
  local deadline = sys.millis() + (opts.timeout or DEFAULT_TIMEOUT)
  local max = opts.max_size or DEFAULT_MAX
  local body = opts.body or ''
  local headers = {}
  for k, v in pairs(opts.headers or {}) do headers[k] = v end
  local redirects = opts.redirects or DEFAULT_REDIRECTS
  local private = {}
  for k in pairs(PRIVATE) do private[k] = true end
  local cert_name = opts.cert_name
  if cert_name ~= nil and (type(cert_name) ~= 'string' or #cert_name == 0 or #cert_name > 253 or cert_name:find('\0', 1, true)) then
    fail('http.request: cert_name must be a name of 1 to 253 characters')
  end
  local extra = opts.private_headers
  if extra ~= nil and type(extra) ~= 'table' then fail('http.request: private_headers must be a list of header names') end
  for _, name in ipairs(extra or {}) do
    if type(name) ~= 'string' then fail('http.request: private_headers must be a list of header names') end
    private[name:lower()] = true
  end

  while true do
    local status, rbody, rheaders = request1(opts, cert_name, method, url, headers, body, deadline, max)
    if not status then return nil, rbody end
    local loc = rheaders.location
    if not (loc and (status == 301 or status == 302 or status == 303 or status == 307 or status == 308)) then
      return status, rbody, rheaders
    end
    if redirects <= 0 then
      if (opts.redirects or DEFAULT_REDIRECTS) == 0 then return status, rbody, rheaders end
      return nil, 'too many redirects'
    end
    redirects = redirects - 1
    local next_url = resolve(loc, url)
    if not next_url:match('^[hH][tT][tT][pP][sS]?://[^/?#]') then
      return nil, 'redirect to an url that is not http(s): ' .. loc:sub(1, 80)
    end
    -- To another server: without the user's credentials, and its
    -- certificate is checked as its own (cert_name was the first one's).
    local function origin(u) local sc, hp = u:match('^(%a+)://([^/?#]+)') return sc:lower() .. '://' .. hp:lower() end
    if origin(next_url) ~= origin(url) then
      for k in pairs(headers) do if private[k:lower()] then headers[k] = nil end end
      cert_name = nil
    end
    -- 303, and 301/302 after a POST (as browsers do), go on as a GET without the body.
    if status == 303 or ((status == 301 or status == 302) and method == 'POST') then
      if method ~= 'HEAD' then method = 'GET' end
      body = ''
      for k in pairs(headers) do
        local l = k:lower()
        if l == 'content-type' or l == 'content-length' then headers[k] = nil end
      end
    end
    url = next_url
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
