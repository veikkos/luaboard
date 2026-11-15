-- json.lua: JSON decode/encode, built into the firmware (require("json")).
--
--   local t = json.decode('{"a": [1, 2, null], "b": "x"}')   -- t.a[3] == json.null
--   local s = json.encode({ name = "x", list = { 1, 2 } })
--
-- Objects become tables, arrays become sequences. JSON null decodes to
-- json.null (so arrays keep their length); encode accepts json.null and nil
-- values in arrays. An empty table encodes as {} unless marked with
-- json.array(t). Integers stay integers; other numbers are floats.

local M = {}

M.null = setmetatable({}, { __tostring = function() return 'null' end })
local array_mt = {}
function M.array(t) return setmetatable(t or {}, array_mt) end

---------------------------------------------------------------- decode

local byte, sub, find, char = string.byte, string.sub, string.find, string.char
local tonumber, concat = tonumber, table.concat

local function fail(s, pos, msg)
  local line = 1
  for _ in sub(s, 1, pos - 1):gmatch('\n') do line = line + 1 end
  error(string.format('json: %s at line %d (position %d)', msg, line, pos), 0)
end

local function skip(s, pos)
  return find(s, '[^ \t\r\n]', pos) or #s + 1
end

local function utf8char(cp)
  if cp < 0x80 then return char(cp) end
  return utf8.char(cp)
end

local escapes = { ['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t' }

local function decode_string(s, pos)       -- pos at the opening quote
  local i = pos + 1
  local j = find(s, '["\\]', i)
  if j and byte(s, j) == 34 then return sub(s, i, j - 1), j + 1 end   -- fast path: no escapes
  local parts = {}
  while true do
    j = find(s, '["\\]', i)
    if not j then fail(s, pos, 'unterminated string') end
    parts[#parts + 1] = sub(s, i, j - 1)
    if byte(s, j) == 34 then return concat(parts), j + 1 end
    local e = sub(s, j + 1, j + 1)
    if e == 'u' then
      local hex = sub(s, j + 2, j + 5)
      local cp = tonumber(hex, 16)
      if not cp or #hex < 4 then fail(s, j, 'bad \\u escape') end
      i = j + 6
      if cp >= 0xD800 and cp <= 0xDBFF and sub(s, i, i + 1) == '\\u' then   -- surrogate pair
        local lo = tonumber(sub(s, i + 2, i + 5), 16)
        if lo and lo >= 0xDC00 and lo <= 0xDFFF then
          cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00)
          i = i + 6
        end
      end
      parts[#parts + 1] = utf8char(cp)
    else
      local c = escapes[e]
      if not c then fail(s, j, 'bad escape') end
      parts[#parts + 1] = c
      i = j + 2
    end
  end
end

local decode_value

local function decode_array(s, pos)
  local t, n = {}, 0
  pos = skip(s, pos + 1)
  if byte(s, pos) == 93 then return M.array(t), pos + 1 end            -- ]
  while true do
    local v
    v, pos = decode_value(s, pos)
    n = n + 1
    t[n] = v
    pos = skip(s, pos)
    local c = byte(s, pos)
    if c == 93 then return t, pos + 1 end
    if c ~= 44 then fail(s, pos, "',' or ']' expected") end
    pos = skip(s, pos + 1)
  end
end

local function decode_object(s, pos)
  local t = {}
  pos = skip(s, pos + 1)
  if byte(s, pos) == 125 then return t, pos + 1 end                   -- }
  while true do
    if byte(s, pos) ~= 34 then fail(s, pos, 'object key expected') end
    local k
    k, pos = decode_string(s, pos)
    pos = skip(s, pos)
    if byte(s, pos) ~= 58 then fail(s, pos, "':' expected") end
    local v
    v, pos = decode_value(s, skip(s, pos + 1))
    t[k] = v
    pos = skip(s, pos)
    local c = byte(s, pos)
    if c == 125 then return t, pos + 1 end
    if c ~= 44 then fail(s, pos, "',' or '}' expected") end
    pos = skip(s, pos + 1)
  end
end

function decode_value(s, pos)
  pos = skip(s, pos)
  local c = byte(s, pos)
  if c == 123 then return decode_object(s, pos) end
  if c == 91 then return decode_array(s, pos) end
  if c == 34 then return decode_string(s, pos) end
  if c == 116 and sub(s, pos, pos + 3) == 'true' then return true, pos + 4 end
  if c == 102 and sub(s, pos, pos + 4) == 'false' then return false, pos + 5 end
  if c == 110 and sub(s, pos, pos + 3) == 'null' then return M.null, pos + 4 end
  local num = s:match('^-?%d+%.?%d*[eE]?[-+]?%d*', pos)
  if num and #num > 0 then
    local v = tonumber(num)
    if v then return math.tointeger(v) or v, pos + #num end
  end
  if not c then fail(s, pos, 'unexpected end') end
  fail(s, pos, "unexpected '" .. char(c) .. "'")
end

function M.decode(s)
  assert(type(s) == 'string', 'json.decode needs a string')
  local v, pos = decode_value(s, 1)
  pos = skip(s, pos)
  if pos <= #s then fail(s, pos, 'trailing characters') end
  return v
end

---------------------------------------------------------------- encode

local escape_map = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
                     ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
local function encode_string(s)
  return '"' .. s:gsub('[%c"\\]', function(c)
    return escape_map[c] or string.format('\\u%04x', byte(c))
  end) .. '"'
end

local function is_array(t)
  if getmetatable(t) == array_mt then return true end
  local n = #t
  if n == 0 then return false end
  local count = 0
  for _ in pairs(t) do count = count + 1 end
  return count == n
end

local encode_value
function encode_value(v, out, seen)
  local tv = type(v)
  if v == nil or v == M.null then out[#out + 1] = 'null'
  elseif tv == 'boolean' then out[#out + 1] = tostring(v)
  elseif tv == 'number' then
    if v ~= v or v == math.huge or v == -math.huge then error('json: cannot encode ' .. tostring(v), 0) end
    out[#out + 1] = math.type(v) == 'integer' and tostring(v) or string.format('%.14g', v)
  elseif tv == 'string' then out[#out + 1] = encode_string(v)
  elseif tv == 'table' then
    if seen[v] then error('json: cannot encode a table that contains itself', 0) end
    seen[v] = true
    if is_array(v) then
      out[#out + 1] = '['
      for i = 1, #v do
        if i > 1 then out[#out + 1] = ',' end
        encode_value(v[i], out, seen)
      end
      out[#out + 1] = ']'
    else
      out[#out + 1] = '{'
      local first = true
      for k, val in pairs(v) do
        if type(k) ~= 'string' and type(k) ~= 'number' then error('json: object keys must be strings', 0) end
        if not first then out[#out + 1] = ',' end
        first = false
        out[#out + 1] = encode_string(tostring(k))
        out[#out + 1] = ':'
        encode_value(val, out, seen)
      end
      out[#out + 1] = '}'
    end
    seen[v] = nil
  else
    error('json: cannot encode a ' .. tv, 0)
  end
end

function M.encode(v)
  local out = {}
  encode_value(v, out, {})
  return concat(out)
end

return M
