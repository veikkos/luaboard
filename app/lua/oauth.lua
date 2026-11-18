-- oauth.lua: OAuth 2.0 login (authorization code + PKCE), built into the
-- firmware (require("oauth")). The login happens once in the browser of the
-- computer running luaboard studio (or `luaboard browser`); after that the
-- device refreshes its access by itself.
--
--   local oauth = require("oauth")
--   local login = oauth.new{
--     name = "spotify",                       -- secret holding the refresh token
--     title = "Spotify",                      -- shown in the studio's login prompt
--     client_id = "...",                      -- public with PKCE; no client secret
--     client_secret = nil,                    -- optional: sent with token requests
--                                             -- (Google "Desktop app" clients need it)
--     scope = "user-read-playback-state",
--     params = nil,                           -- optional: extra authorize URL parameters,
--                                             -- e.g. { access_type = "offline" } (Google)
--     authorize_url = "https://accounts.spotify.com/authorize",
--     token_url = "https://accounts.spotify.com/api/token",
--     redirect = "http://127.0.0.1:8888/callback",
--     on_login = function(waiting) end,       -- true: the user must log in now
--   }
--   task(function()
--     local status, body = login:request{ url = "https://api.spotify.com/v1/me" }
--   end)
--
-- Re-login: when the stored login stops working (access removed, token
-- revoked or expired), the refresh token is forgotten and the next request
-- starts a new browser login, calling on_login(true) so the script can tell
-- the user. login:logout() (or `luaboard secrets rm <app> <name>`) logs out on
-- purpose. Network errors keep the stored login and just fail the request.

local http, json = require('http'), require('json')

local M = {}
local Login = {}
Login.__index = Login

function M.new(o)
  for _, k in ipairs({ 'name', 'client_id', 'authorize_url', 'token_url', 'redirect' }) do
    if not o[k] or o[k] == '' then sys._error('oauth.new: ' .. k .. ' is required') end
  end
  return setmetatable({ o = o, access = nil, expires = 0, busy = false }, Login)
end

local function b64url(s) return crypto.base64(s, true) end

function Login:_save(tok)
  self.access = tok.access_token
  self.expires = sys.millis() + math.max(60, (tonumber(tok.expires_in) or 3600) - 60) * 1000
  if tok.refresh_token and tok.refresh_token ~= json.null then
    secrets.set(self.o.name, tok.refresh_token)   -- providers may rotate it
  end
end

-- POST to the token endpoint. Returns the token table, or nil, message, error code.
function Login:_token_request(fields)
  fields.client_id = self.o.client_id
  fields.client_secret = self.o.client_secret
  local st, body = http.post(self.o.token_url, http.form(fields), {
    ['Content-Type'] = 'application/x-www-form-urlencoded', Accept = 'application/json' })
  if not st then return nil, body end                         -- network: no code
  local t = json.decode(body)
  if st == 200 and type(t) == 'table' and t.access_token then return t end
  local code = type(t) == 'table' and t.error or nil
  local msg = type(t) == 'table' and (t.error_description or t.error) or ('HTTP ' .. st)
  if st >= 500 then code = nil end                            -- server trouble: retry later
  return nil, tostring(msg), code
end

function Login:_browser_login()
  local verifier = b64url(crypto.random(48))
  local state = b64url(crypto.random(16))
  local params = {
    client_id = self.o.client_id, response_type = 'code', redirect_uri = self.o.redirect,
    state = state, code_challenge = b64url(crypto.sha256(verifier)), code_challenge_method = 'S256',
  }
  if self.o.scope then params.scope = self.o.scope end
  for k, v in pairs(self.o.params or {}) do params[k] = v end
  local url = self.o.authorize_url .. (self.o.authorize_url:find('?', 1, true) and '&' or '?') .. http.form(params)

  print(self.o.name .. ': log in from luaboard studio (or run: luaboard browser)')
  if self.o.on_login then self.o.on_login(true) end
  local q, err = browser.login(url, self.o.redirect, self.o.title or self.o.name, self.o.login_timeout)
  if self.o.on_login then self.o.on_login(false) end
  if not q then return nil, err end

  local p = http.parse_query(q)
  if p.error then return nil, 'login refused: ' .. (p.error_description or p.error) end
  if p.state ~= state then return nil, 'login answer does not belong to this login (state)' end
  if not p.code then return nil, 'no code in the login answer' end
  local tok, e = self:_token_request{ grant_type = 'authorization_code', code = p.code,
                                      redirect_uri = self.o.redirect, code_verifier = verifier }
  if not tok then return nil, 'token exchange failed: ' .. e end
  self:_save(tok)
  print(self.o.name .. ': logged in')
  return true
end

function Login:_obtain()
  local refresh = secrets.get(self.o.name)
  if refresh then
    local tok, err, code = self:_token_request{ grant_type = 'refresh_token', refresh_token = refresh }
    if tok then self:_save(tok) return self.access end
    if not code then return nil, err end                      -- offline etc.: keep the login
    -- invalid_grant & co: the stored login is dead. Forget it, log in again.
    print(self.o.name .. ': stored login no longer works (' .. err .. '), logging in again')
    secrets.set(self.o.name, nil)
  end
  local ok, err = self:_browser_login()
  if not ok then return nil, err end
  return self.access
end

-- A valid access token; logs in or refreshes when needed. Inside task().
function Login:token()
  if not in_task() then sys._error('oauth: call inside task()') end
  while self.busy do sleep(100) end                -- one login / refresh at a time
  if self.access and sys.millis() < self.expires then return self.access end
  self.busy = true
  local ok, tok, err = pcall(self._obtain, self)
  self.busy = false
  if not ok then error(tok, 0) end
  return tok, err
end

-- http.request() with the access token. A 401 refreshes once (and logs in
-- again if the refresh is refused).
function Login:request(opts)
  for attempt = 1, 2 do
    local tok, err = self:token()
    if not tok then return nil, err end
    local o, h = {}, {}
    for k, v in pairs(opts) do o[k] = v end
    for k, v in pairs(opts.headers or {}) do h[k] = v end
    h.Authorization = 'Bearer ' .. tok
    o.headers = h
    local st, body, headers = http.request(o)
    if st ~= 401 or attempt == 2 then return st, body, headers end
    self.access = nil
  end
end

function Login:logged_in() return secrets.get(self.o.name) ~= nil end

function Login:logout()
  secrets.set(self.o.name, nil)
  self.access, self.expires = nil, 0
end

return M
