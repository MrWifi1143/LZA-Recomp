-- Legends Lua: the mod API (docs/lua-api.md). Built into the plugin (legendslua.c embeds this file); it runs first in
-- each fresh Lua state, given the C side's primitives (rt), and makes what the scripts see: npc, player, on / off /
-- emit, timer, cmd, ui, vec, include. The same in both games. Everything a script hands in here is checked; a script's
-- error is reported (log + a notice for one that keeps failing), never passed on to the game.
local rt = ...
local G = _G
local sqrt, atan, sin, cos, rad, deg, floor = math.sqrt, math.atan, math.sin, math.cos, math.rad, math.deg, math.floor
local fmt, insert, remove = string.format, table.insert, table.remove

-- ---------------------------------------------------------------- vectors
local Vec = {}
Vec.__index = Vec
local function vec(x, y, z)
  if type(x) == "table" then
    local t = x
    x, y, z = t.x or t[1], t.y or t[2], t.z or t[3]
  end
  if type(x) ~= "number" or type(y) ~= "number" or type(z) ~= "number" then
    error("a position is {x, y, z} (or x, y, z): three numbers", 3)
  end
  return setmetatable({x = x, y = y, z = z}, Vec)
end
Vec.__add = function(a, b) return vec(a.x + b.x, a.y + b.y, a.z + b.z) end
Vec.__sub = function(a, b) return vec(a.x - b.x, a.y - b.y, a.z - b.z) end
Vec.__mul = function(a, k) if type(a) == "number" then a, k = k, a end return vec(a.x * k, a.y * k, a.z * k) end
Vec.__unm = function(a) return vec(-a.x, -a.y, -a.z) end
Vec.__eq = function(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end
Vec.__tostring = function(a) return fmt("(%.2f, %.2f, %.2f)", a.x, a.y, a.z) end
function Vec:len() return sqrt(self.x * self.x + self.y * self.y + self.z * self.z) end
function Vec:dist(b) b = vec(b) local dx, dy, dz = self.x - b.x, self.y - b.y, self.z - b.z return sqrt(dx * dx + dy * dy + dz * dz) end
function Vec:dist2d(b) b = vec(b) local dx, dz = self.x - b.x, self.z - b.z return sqrt(dx * dx + dz * dz) end
function Vec:unpack() return self.x, self.y, self.z end
function Vec:copy() return vec(self.x, self.y, self.z) end
function Vec:yaw_to(b) b = vec(b) return deg(atan(b.x - self.x, b.z - self.z)) end  -- (degrees, 0 = +z, 90 = +x)
function Vec:ahead(yaw, d) return vec(self.x + sin(rad(yaw)) * d, self.y, self.z + cos(rad(yaw)) * d) end
function Vec:lerp(b, t) b = vec(b) return vec(self.x + (b.x - self.x) * t, self.y + (b.y - self.y) * t, self.z + (b.z - self.z) * t) end
local function is_vec(v) return type(v) == "table" and (v.x or v[1]) ~= nil end

-- ---------------------------------------------------------------- reporting
local mods = {}       -- name -> mod {name, dir, env, failed}
local mod_list = {}   -- in load order
local shared = {name = "lua", dir = (game and game.root or "") .. "/lua"}  -- (the game folder's lua/: the shared globals)
local told = {}
local function report(mod, what, err)
  local name = mod and mod.name or "lua"
  rt.log(fmt("%s: %s: %s", name, what, tostring(err)))
end
local function tell_once(key, text)
  if told[key] then return end
  told[key] = true
  rt.notify(text, 8)
end

-- a handler {fn, mod, errors}: run, errors reported; after 10 it is switched off (one broken handler can't flood the
-- log every frame) and the player told once
local function run(h, what, ...)
  if h.dead then return false end
  local ok, err = xpcall(h.fn, rt.trace, ...)
  if not ok then
    h.errors = (h.errors or 0) + 1
    if h.errors <= 3 then report(h.mod, what, err) end
    if h.errors >= 10 then
      h.dead = true
      report(h.mod, what, "failed 10 times: switched off")
      tell_once((h.mod and h.mod.name or "lua") .. ":" .. what,
        fmt("Mod \"%s\": a script error switched part of it off (%s) - see the log", h.mod and h.mod.name or "lua", what))
    end
  end
  return ok, err
end

-- ---------------------------------------------------------------- events
-- on(event, fn) / on(event, filter, fn): filter = a button name ("button"), a character ("talk", "npc_*"), ...
local handlers = {}
local next_id = 0
local function on(mod, ev, a, b)
  if type(ev) ~= "string" then error("on(event, fn): the event is a name, e.g. \"tick\"", 3) end
  local filter, fn = nil, a
  if b ~= nil then filter, fn = a, b end
  if type(fn) ~= "function" then error("on(\"" .. ev .. "\", fn): fn must be a function", 3) end
  next_id = next_id + 1
  local list = handlers[ev]
  if not list then list = {} handlers[ev] = list end
  list[#list + 1] = {fn = fn, mod = mod, id = next_id, filter = filter}
  return next_id
end
local function off(id)
  for _, list in pairs(handlers) do
    for i = #list, 1, -1 do if list[i].id == id then list[i].dead = true remove(list, i) end end
  end
end
local function has(ev) local l = handlers[ev] return l and #l > 0 end
-- (subject: what a handler's filter is compared with - the button, the character)
local function emit_to(ev, subject, ...)
  local list = handlers[ev]
  if not list then return end
  local n = #list
  for i = 1, n do
    local h = list[i]
    if h and not h.dead and (h.filter == nil or h.filter == subject) then run(h, ev, ...) end
  end
end
local function emit(ev, ...) return emit_to(ev, nil, ...) end

-- ---------------------------------------------------------------- timers
local timers = {}
local now = rt.now
local function timer_add(mod, sec, every, fn)
  if type(sec) ~= "number" or sec < 0 then error("timer: seconds must be a number >= 0", 3) end
  if type(fn) ~= "function" then error("timer: fn must be a function", 3) end
  next_id = next_id + 1
  timers[#timers + 1] = {at = now() + sec, every = every and math.max(sec, 0.001) or nil, fn = fn, mod = mod, id = next_id}
  return next_id
end
local function timer_cancel(id)
  for i = #timers, 1, -1 do if timers[i].id == id then timers[i].dead = true remove(timers, i) end end
end
local function timers_run()
  if #timers == 0 then return end
  local t = now()
  local due = nil
  for i = 1, #timers do
    local tm = timers[i]
    if not tm.dead and tm.at <= t then due = due or {} due[#due + 1] = tm end
  end
  if not due then return end
  for _, tm in ipairs(due) do
    if not tm.dead then
      if tm.every then
        tm.at = tm.at + tm.every
        if tm.at < t then tm.at = t + tm.every end
        run(tm, "timer")
      else
        tm.at = math.huge  -- (one shot: never due again - gone once it has run)
        run(tm, "timer")
        tm.dead = true
      end
    end
  end
  for i = #timers, 1, -1 do if timers[i].dead then remove(timers, i) end end
end

-- ---------------------------------------------------------------- console commands
local commands = {}
local function cmd_add(mod, name, fn, help)
  if type(name) ~= "string" or not name:match("^[%w_%.%-]+$") then error("cmd.add(name, fn): name is one word", 3) end
  if type(fn) ~= "function" then error("cmd.add(name, fn): fn must be a function", 3) end
  commands[name] = {fn = fn, mod = mod, help = help or ""}
end
local function words(line)
  local t = {}
  for w in line:gmatch("%S+") do t[#t + 1] = tonumber(w) or w end
  return t
end
local function cmd_run(line)
  local w = words(line)
  local name = remove(w, 1)
  if not name then return end
  local c = commands[name]
  if not c then print("no command \"" .. tostring(name) .. "\" (try: help)") return end
  local ok, err = xpcall(c.fn, rt.trace, table.unpack(w))
  if not ok then report(c.mod, "command " .. name, err) print("error: " .. tostring(err)) end
end
G.__cmd_line = cmd_run

-- ---------------------------------------------------------------- ui
local ui = {}
function ui.notify(text, seconds) rt.notify(tostring(text), tonumber(seconds) or 4) end
function ui.say(who, text, seconds)
  local name = type(who) == "table" and who.name or tostring(who)
  rt.notify(name .. ": " .. tostring(text), tonumber(seconds) or 4)
end

-- ---------------------------------------------------------------- the game's own scripts (Z-A)
-- Lua text run in the game's own Lua 5.4 (its event framework) at the game's next frame; fn(result, err) gets what it
-- returned as text. Each job starts with `local Commands = C0C062DE5AED06885` (the framework's Commands class).
local jobs = {}  -- id -> {fn, mod}
local function lit(v)  -- (a value as Lua source, for the game's Lua)
  local t = type(v)
  if t == "number" then return math.type(v) == "integer" and tostring(v) or fmt("%.9g", v) end
  if t == "string" then return fmt("%q", v) end
  if t == "boolean" or t == "nil" then return tostring(v) end
  if t == "table" then
    if v.x and v.y and v.z then return fmt("{%.9g, %.9g, %.9g}", v.x, v.y, v.z) end
    local p = {}
    for i = 1, #v do p[i] = lit(v[i]) end
    return "{" .. table.concat(p, ", ") .. "}"
  end
  error("game.call: can't pass a " .. t, 3)
end
-- (the framework's names are hashed: Commands.AddItem is Commands.S<FNV-1a 64 of "AddItem">)
local function fnv64(str)
  local h = 0xcbf29ce484222645
  for i = 1, #str do h = (h ~ str:byte(i)) * 0x100000001b3 end
  return fmt("%016X", h)
end
local function hashed(code)
  return (code:gsub("Commands%.([%a_][%w_]*)", function(n)
    if #n == 17 and n:match("^S%x+$") then return nil end
    return "Commands.S" .. fnv64(n)
  end))
end
-- (Z-A: the framework's own object lookup - every FObj_* command, its party's Player - takes the object into the
-- framework's list of event objects, as an event does (SCDED1A922C1D919F: SEED5C57DB4A3CB5D:push), and only an event's end
-- lets them go. Held, the Player can't send its Pokemon out (D-pad Up threw nothing - 2026-11-19). So each job lets go
-- what it took: the objects in that list after it that weren't before, but the scripts' own (luanpc_, luapoke_, luaprop_)
-- - S38F7587912F54E9A, the framework's own release of one, and its priority tag. A game.run coroutine does it at its end)
local ZA_HELD = [[if not __LZA_HELD then
  __LZA_HELD = function() local l, h = C0C062DE5AED06885.SEED5C57DB4A3CB5D, {} for i = 0, l.length - 1 do h[l[i]:fE9C29DA1()] = true end return h end
  __LZA_LETGO = function(h) local l, n = C0C062DE5AED06885.SEED5C57DB4A3CB5D, {}
    for i = 0, l.length - 1 do local nm = l[i]:fE9C29DA1() if not h[nm] and not tostring(nm):match("^lua") then n[#n + 1] = nm end end
    for _, nm in ipairs(n) do C0C062DE5AED06885.S38F7587912F54E9A(nm) C0C062DE5AED06885.SB54DBC4F41BBBCDD(nm) end end
end
]]
local function script(mod, code, fn, keep)
  if (rt.api_version or 1) < 3 then return nil, "this game build is too old" end
  -- (its first result as text: the game's lua_tolstring takes strings and numbers only)
  -- (Z-A: the framework's Commands at hand, its hashed names; Arceus: plain names)
  local za = game.name == "za"
  local text
  if za and not keep then
    text = "local Commands = C0C062DE5AED06885\n" .. ZA_HELD .. "local __h = __LZA_HELD()\nlocal __ok, __r = pcall(function()\n" ..
      hashed(code) .. "\nend)\n__LZA_LETGO(__h)\nif not __ok then error(__r, 0) end\nreturn tostring(__r)"
  else
    text = (za and "local Commands = C0C062DE5AED06885\n" or "") .. "local __r = (function()\n" .. (za and hashed(code) or code) ..
      "\nend)()\nreturn tostring(__r)"
  end
  local id = rt.game_script(text)
  if id == 0 then return nil, "not taken (too many waiting, or not in this game)" end
  jobs[id] = {fn = fn, mod = mod}
  return id
end
local function jobs_poll()
  for id, j in pairs(jobs) do
    local st, out = rt.script_result(id)
    if st ~= 0 then
      jobs[id] = nil
      if st == 2 and not j.fn then report(j.mod, "game.script", out) end
      if j.fn then run({fn = j.fn, mod = j.mod}, "game.script", st == 1 and out or nil, st ~= 1 and out or nil) end
    end
  end
end
-- a job that waits (message windows, Wait_Time: the framework's Suspend yields): run as a coroutine in the game's Lua,
-- resumed once a frame until it ends (game.run)
local co_alive = false
local co_job = nil
local CO_TICK = "local n, t = 0, __LZA_CO or {} for i = #t, 1, -1 do local co = t[i] local ok, e = coroutine.resume(co) " ..
  "if coroutine.status(co) == 'dead' then table.remove(t, i) if not ok then __LZA_ERR = tostring(e) end else n = n + 1 end end " ..
  "local e = __LZA_ERR __LZA_ERR = nil return n .. (e and (' ' .. e) or '')"
local function run_co(mod, code, fn)
  local za = game.name == "za"
  local body = za and (ZA_HELD .. "__LZA_CO = __LZA_CO or {} local co = coroutine.create(function()\nlocal __h = __LZA_HELD()\n" ..
    "local __ok, __e = pcall(function()\n" .. code .. "\nend)\n__LZA_LETGO(__h)\nif not __ok then error(__e, 0) end\nend)")
    or ("__LZA_CO = __LZA_CO or {} local co = coroutine.create(function()\n" .. code .. "\nend)")
  local id, err = script(mod, body .. " local ok, e = coroutine.resume(co) if coroutine.status(co) ~= 'dead' then __LZA_CO[#__LZA_CO + 1] = co end " ..
    "if not ok then error(e, 0) end return 'started'", fn, true)
  if id then co_alive = true end
  return id, err
end
local function co_tick()
  if not co_alive or co_job then return end
  co_job = script(nil, CO_TICK, function(r, e)
    co_job = nil
    local n, msg = (r or ""):match("^(%d+)%s*(.*)$")
    co_alive = (tonumber(n) or 0) > 0
    if msg and msg ~= "" then rt.log("game.run: " .. msg) end
    if e then rt.log("game.run: " .. e) co_alive = false end
  end, true)  -- (keep: the coroutines let go of what they took at their own end)
end
local function call_code(fname, args)
  local a = {}
  for i = 1, args.n do a[i] = lit(args[i]) end
  return fmt("local r = %s(%s) if type(r) == 'table' then local t = {} for i = 1, #r do t[i] = tostring(r[i]) end return table.concat(t, ' ') end return r", fname, table.concat(a, ", "))
end

-- ---------------------------------------------------------------- the player
local BUTTONS = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"}
local BIT = {}
for i, n in ipairs(BUTTONS) do BIT[n] = 1 << (i - 1) end
local player = {}
function player.pos()
  if not rt.have_api then return nil end
  local x, y, z = rt.player()
  if not x then return nil end
  return vec(x, y, z)
end
function player.yaw()
  if not rt.have_api then return nil end
  local x, y, z, yaw = rt.player()
  return yaw
end
function player.forward()
  local yaw = player.yaw()
  if not yaw then return nil end
  return vec(sin(rad(yaw)), 0, cos(rad(yaw)))
end
function player.ahead(d)
  local p, yaw = player.pos(), player.yaw()
  if not p then return nil end
  return p:ahead(yaw, d or 2)
end
function player.teleport(x, y, z)
  local p = vec(x, y, z)
  rt.teleport(p.x, p.y, p.z)
end
function player.button(name)
  local b = BIT[string.upper(tostring(name))]
  if not b then error("player.button: one of " .. table.concat(BUTTONS, " "), 2) end
  return rt.buttons() & b ~= 0
end

-- ---------------------------------------------------------------- characters (npc)
local STATES = {[0] = "removed", "waiting", "coming", "up", "walking"}
local Npc = {}
Npc.__index = Npc
Npc.__tostring = function(n) return fmt("npc %s (#%d, %s)", n.name, n.id, n:state()) end
local npcs = {}  -- handle -> Npc (every mod's)
local function check_npc(n)
  if getmetatable(n) ~= Npc then error("not a character made by npc.spawn", 3) end
end
local function npc_spawn(mod, t)
  if not rt.have_api then error("npc.spawn: this game build has no mod API - update the game", 3) end
  if type(t) ~= "table" then error("npc.spawn{pos = {x, y, z}, ...}: one table", 3) end
  local pos = t.pos and vec(t.pos) or player.ahead(2)
  if not pos then error("npc.spawn: pos = {x, y, z} needed (the player isn't in a level yet)", 3) end
  local yaw = tonumber(t.yaw) or 0
  local name = t.name and tostring(t.name) or nil
  local model = t.model or t.template or ""
  if type(model) ~= "string" then error("npc.spawn: model is a name (npc.models() lists them)", 3) end
  local h = rt.npc_spawn(model, pos.x, pos.y, pos.z, yaw, name or "")
  if h == 0 then error("npc.spawn: too many characters at once (remove some first)", 3) end
  if t.outfit ~= nil or t.body ~= nil then  -- (what it wears: "player", or {[slot] = item, ...} over the player's (Arceus))
    local spec
    if type(t.outfit) == "table" then
      local parts = {"player"}
      for slot, item in pairs(t.outfit) do parts[#parts + 1] = tostring(slot) .. "=" .. tostring(item) end
      spec = table.concat(parts, ",")
    else spec = t.outfit and tostring(t.outfit) or "player" end
    if t.body then spec = spec .. ",body=" .. tostring(t.body) end
    if (rt.api_version or 1) < 2 or not rt.npc_outfit(h, spec) then report(mod, "npc.spawn", "outfit \"" .. spec .. "\" not taken - its look's own") end
  end
  local n = setmetatable({id = h, name = name or ("npc" .. h), mod = mod, data = {}, home = pos, _st = 1, talk = t.talk}, Npc)
  npcs[h] = n
  return n
end
function Npc:state()
  if self.removed then return "removed" end
  local st = rt.npc_state(self.id)
  return STATES[st] or "removed"
end
function Npc:valid() return not self.removed and rt.npc_state(self.id) ~= 0 end
function Npc:is_up() local st = rt.npc_state(self.id) return st == 3 or st == 4 end
function Npc:is_walking() return rt.npc_state(self.id) == 4 end
function Npc:pos()
  local st, x, y, z = rt.npc_state(self.id)
  if st == 0 then return nil end
  return vec(x, y, z)
end
function Npc:yaw()
  local st, x, y, z, yaw = rt.npc_state(self.id)
  if st == 0 then return nil end
  return yaw
end
function Npc:dist(to)
  local p = self:pos()
  local q = to and vec(to) or player.pos()
  if not p or not q then return math.huge end
  return p:dist2d(q)
end
local function za_obj(n)  -- (Z-A: the name the game knows it by, while it is up)
  return game.name == "za" and (rt.api_version or 1) >= 3 and rt.npc_object(n.id) or nil
end
local function go(n, p, run)
  n._goal, n._req_t, n._seen_walk = p, now(), false
  local obj = za_obj(n)
  if obj then  -- (Z-A: the game's own navigation walks it - around things, on the ground)
    rt.npc_walk(n.id, p.x, p.y, p.z, (run and 1 or 0) | 2)
    script(n.mod, fmt("Commands.FObj_Move_To_PosXYZ_Navi(%q, %.9g, %.9g, %.9g, %s)", obj, p.x, p.y, p.z, run and "6.0" or "2.0"))
    n._navi = obj
    return
  end
  rt.npc_walk(n.id, p.x, p.y, p.z, run and true or false)
end
local function halt(n)  -- (stands: held where it is by the runtime - the game's walk, if it was walking it, has no say)
  rt.npc_stop(n.id)
  n._navi = nil
end
function Npc:walk_to(x, y, z, run)
  check_npc(self)
  local p
  if is_vec(x) then p, run = vec(x), y else p = vec(x, y, z) end
  self._route = nil
  go(self, p, run)
  return self
end
function Npc:run_to(x, y, z) if is_vec(x) then return self:walk_to(x, true) end return self:walk_to(x, y, z, true) end
-- walk a list of points in turn; opts: loop (start again), run, wait (seconds at each point), start (the point to go
-- to first, default 1), on_point(npc, i)
function Npc:walk_route(points, opts)
  check_npc(self)
  if type(points) ~= "table" or #points == 0 then error("walk_route: a list of points", 2) end
  local pts = {}
  for i, p in ipairs(points) do pts[i] = vec(p) end
  opts = opts or {}
  local i = math.max(1, math.min(#pts, math.floor(tonumber(opts.start) or 1)))
  self._route = {pts = pts, i = i, opts = opts}
  go(self, pts[i], opts.run)
  return self
end
-- a route held (it stands) and taken up again where it was going
function Npc:pause()
  check_npc(self)
  if self._route then self._route.paused = true self._route.wait_until = nil end
  self._goal = nil
  halt(self)
  return self
end
function Npc:resume()
  check_npc(self)
  local r = self._route
  if r and r.paused then r.paused = false go(self, r.pts[r.i], r.opts.run) end
  return self
end
function Npc:stop() check_npc(self) self._route, self._goal = nil, nil halt(self) return self end  -- (its route dropped too: pause keeps it)
function Npc:set_pos(x, y, z, yaw)
  check_npc(self)
  local p
  if is_vec(x) then p, yaw = vec(x), y else p = vec(x, y, z) end
  self._route, self._goal = nil, nil
  if self._navi then halt(self) end
  rt.npc_set(self.id, p.x, p.y, p.z, tonumber(yaw) or 1e9)
  return self
end
function Npc:face(yaw_or_pos)
  check_npc(self)
  local p = self:pos()
  if not p then return self end
  local yaw = type(yaw_or_pos) == "number" and yaw_or_pos or p:yaw_to(yaw_or_pos)
  rt.npc_set(self.id, p.x, p.y, p.z, yaw)
  return self
end
function Npc:face_player() local p = player.pos() if p then self:face(p) end return self end
function Npc:remove()
  if self.removed then return end
  rt.npc_remove(self.id)
  npcs[self.id] = nil
  self.removed = true
end
-- a line over its head in a speech bubble (seconds: default by its length); a notice where the game build has none
function Npc:say(text, seconds)
  check_npc(self)
  ui.bubble(self, text, seconds)
  return self
end
-- one of its animation's parameters, by the game's name: true / false a bool, a whole number an int, else a float;
-- n:anim(name) (or kind "trigger") fires a trigger. Z-A: "sit_bool" + "sit_int" (1, 2) sit it down, "speak_bool" talks,
-- "once_action_trigger" a gesture; Arceus (dressed ones: the player's net) "set_squat_bool" crouches
function Npc:anim(name, value, kind)
  check_npc(self)
  if type(name) ~= "string" then error("anim(name, value): the parameter's name", 2) end
  local k = kind and kind:sub(1, 1) or (value == nil and "t") or (type(value) == "boolean" and "b")
    or (math.type(value) == "integer" and "i") or "f"
  local v = type(value) == "boolean" and (value and 1 or 0) or tonumber(value) or 0
  return rt.npc_anim(self.id, name, k, v)
end
function Npc:trigger(name) return self:anim(name, nil, "t") end
-- its once-action n: once_action_int n, then once_action_trigger (Z-A characters also wait_int 1 - their route goes
-- through their default wait)
function Npc:play_once(n)
  check_npc(self)
  if game.name == "za" then self:anim("wait_int", 1) end
  return self:anim("once_action_int", math.floor(n)) and self:trigger("once_action_trigger")
end
-- Z-A: the name the game knows it by (for game.call's FObj_* functions), nil until it is up
function Npc:object() check_npc(self) return za_obj(self) end
-- Z-A: one of the game's own animations by number (FObj_Set_Animation: 1 standing ... the events' numbers)
function Npc:play_anim(id)
  check_npc(self)
  local obj = za_obj(self)
  if not obj then return false end
  script(self.mod, fmt("Commands.FObj_Set_Animation(%q, %d)", obj, math.floor(id)))
  return true
end
-- this character's events: on("talk"), on("near"), on("arrived"), on("up"), on("gone") - the same as on(event, npc, fn)
function Npc:on(ev, fn)
  local map = {talk = "talk", near = "near", far = "far", arrived = "npc_arrived", up = "npc_up", gone = "npc_gone"}
  return on(self.mod, map[ev] or ev, self, fn)
end

local npc = {}
function npc.models() return rt.have_api and rt.npc_models() or "" end
function npc.find(name) for _, n in pairs(npcs) do if n.name == name then return n end end end
function npc.all() local t = {} for _, n in pairs(npcs) do t[#t + 1] = n end table.sort(t, function(a, b) return a.id < b.id end) return t end

-- ---------------------------------------------------------------- the game's own message window (Z-A)
-- a line of the script's own text in the window the game's people talk in: reserved labels lza_say_<n> answered by
-- the runtime (hle/za_script.c), shown from a coroutine as the game's own item window does (its message file loaded,
-- the window, closed with A). Elsewhere: a notice.
local say_slot = 0
local MSG_SHOW = [[local f = CF74EC46ADA8DC783.S3567CE4CD45AC7D3("script/fld_item")
CF74EC46ADA8DC783.S092ACF6F9CAC2BA2(f)
while not CF74EC46ADA8DC783.S8EE12659B6952775(f) do Commands.Suspend() end
Commands.ShowMessageWindow(2, "lza_say_%d", "fld_item")
Commands.HideMessageWindow()
CF74EC46ADA8DC783.S34645180A2F21B29("script/fld_item")]]
function ui.message(text, fn)
  text = tostring(text)
  if game.name == "za" and (rt.api_version or 1) >= 3 then
    say_slot = (say_slot + 1) % 16
    if rt.message_text(say_slot, text) then
      local id = run_co(nil, fmt(MSG_SHOW, say_slot), fn)
      if id then return true end
    end
  end
  rt.notify(text, 4)
  if fn then fn() end
  return true
end
function ui.say(who, text)
  local name = type(who) == "table" and who.name or tostring(who)
  return ui.message(name .. ":\n" .. tostring(text))
end

-- ---------------------------------------------------------------- items
local item = {}
function item.give(id, n, show)
  if (rt.api_version or 1) < 3 then error("item.give: this game build is too old", 2) end
  id, n = math.floor(id), math.floor(n or 1)
  local ok = rt.item_give(id, n)
  if ok and show and game.name == "za" then  -- (Z-A: the game's own "You got ...!" window too - A closes it)
    run_co(nil, fmt("Commands.S6DCE2895AC79F359(Commands.S5DF104B25CC642C7(%d), %d)", id, n))
  end
  return ok
end

-- ---------------------------------------------------------------- Pokémon of a script's own
-- (Arceus: made by the runtime like the shared world's mounts. Z-A: the event framework's own make, by name)
local Poke = {}
Poke.__index = Poke
Poke.__tostring = function(p) return fmt("pokemon %s (#%d %d)", p.name, p.species, p.id) end
local pokemon = {}
local pokes = {}
local za_poke_n = 0
function pokemon.spawn(t)
  if type(t) ~= "table" or not tonumber(t.species) then error("pokemon.spawn{species = n, pos = {x, y, z}, ...}", 2) end
  if (rt.api_version or 1) < 2 then error("pokemon.spawn: this game build is too old", 2) end
  local pos = t.pos and vec(t.pos) or player.ahead(3)
  if not pos then error("pokemon.spawn: pos needed (no player yet)", 2) end
  if game.name == "za" then
    if (rt.api_version or 1) < 3 then return nil, "this game build is too old" end
    za_poke_n = za_poke_n + 1
    local name = fmt("luapoke_%d", za_poke_n)
    local p = setmetatable({id = -za_poke_n, species = math.floor(t.species), name = t.name or name, object = name, data = {},
                            _at = pos, _yaw = tonumber(t.yaw) or 0, _st = 2}, Poke)
    local code = fmt("return Commands.S0D43FC7826F5A0FA(%q, {%.9g, %.9g, %.9g}, %.9g, %d, %d, %d, nil, nil, true, %s) ~= nil",
      name, pos.x, pos.y, pos.z, p._yaw, p.species, math.floor(tonumber(t.form) or 0), math.floor(tonumber(t.gender) or 0), t.shiny and "true" or "false")
    local id, err = script(nil, code, function(r, e)
      if p.removed then return end
      p._st = r == "true" and 3 or 0
      if p._st == 0 then rt.log(fmt("pokemon.spawn: %s not made (%s)", name, e or r or "?")) end
    end)
    if not id then return nil, err end
    pokes[p.id] = p
    return p
  end
  local h = rt.poke_spawn(math.floor(t.species), pos.x, pos.y, pos.z, tonumber(t.yaw) or 0, math.floor(tonumber(t.form) or 0), t.shiny and true or false, t.name or "")
  if h == 0 then return nil, "not made (" .. (game.name == "za" and "not in Z-A yet" or "none free") .. ")" end
  local p = setmetatable({id = h, species = math.floor(t.species), name = t.name or ("pokemon" .. h), data = {},
                          object = "luapoke_" .. (h >> 8)}, Poke)  -- (Arceus: its name, luapoke_<k>)
  pokes[h] = p
  return p
end
local function za_poke(p) return p.id < 0 end
-- (Z-A) its animation's parameters, as a character's n:anim; p:play_once(n): its once-action n (once_action_int n,
-- then once_action_trigger) - the motion its set has there, or a new one put there with Studio's motionadd
function Poke:anim(name, value, kind)
  if self.removed or not self.object or (rt.api_version or 1) < 3 then return false end
  local k = kind and kind:sub(1, 1) or (value == nil and "t") or (type(value) == "boolean" and "b")
    or (math.type(value) == "integer" and "i") or "f"
  local v = type(value) == "boolean" and (value and 1 or 0) or tonumber(value) or 0
  return rt.object_anim(self.object, tostring(name), k, v)
end
function Poke:trigger(name) return self:anim(name, nil, "t") end
-- (Arceus: its emotion n held `seconds` (default 3), then let go - loop_emotion_int n + loop_emotion_bool, entered by
-- one_emotion_trigger: 1 sleep, 2 eat for most (Studio motionstates <model> emotion). Its one-shot branch (glad) needs
-- loop_emotion_bool true to enter and false at the entry: not reached from here - 2026-11-19)
function Poke:play_once(n, seconds)
  if game.name == "za" then return self:anim("once_action_int", math.floor(n)) and self:trigger("once_action_trigger") end
  if not (self:anim("loop_emotion_int", math.floor(n)) and self:anim("loop_emotion_bool", true) and self:trigger("one_emotion_trigger")) then
    return false
  end
  timer_add(shared, tonumber(seconds) or 3, false, function() if not self.removed then self:anim("loop_emotion_bool", false) end end)
  return true
end
function Poke:state()
  if self.removed then return "removed" end
  if za_poke(self) then return STATES[self._st] or "removed" end
  local st = rt.poke_state(self.id) return STATES[st] or "removed"
end
function Poke:valid() return not self.removed and (za_poke(self) and self._st ~= 0 or not za_poke(self) and rt.poke_state(self.id) ~= 0) end
function Poke:pos()
  if self.removed then return nil end
  if za_poke(self) then return self._st ~= 0 and self._at or nil end  -- (Z-A: where it was made; game.call FObj_Get_Pos for now)
  local st, x, y, z = rt.poke_state(self.id) if st == 0 then return nil end return vec(x, y, z)
end
function Poke:remove()
  if self.removed then return end
  self.removed = true
  pokes[self.id] = nil
  if za_poke(self) then script(nil, fmt("Commands.SB1734BFAFC3FB69D(%q)", self.object)) else rt.poke_remove(self.id) end  -- (Z-A: the framework's delete of a Pokémon it made)
  if ui.bubble_hide then ui.bubble_hide(self) end  -- (its speech bubble with it)
end
function pokemon.all() local t = {} for _, p in pairs(pokes) do t[#t + 1] = p end return t end
G.pokemon = pokemon

-- ---------------------------------------------------------------- speech bubbles
-- ui.bubble(target, text [, seconds [, opts]]): a speech bubble over target's head (the runtime draws it - white box,
-- tail - and it follows the target): a character (npc.spawn), a Pokémon (pokemon.spawn), "player", a game object's
-- name (Z-A: "luanpc_001", a townsperson's), or a spot {x, y, z}. seconds: how long (default: by the text's length,
-- 3 to 12 s); opts.height: metres over its feet. Text is cut at 100 characters; "\n" breaks a line, long ones wrap.
-- The same target's bubble is replaced. Returns its id (nil: this game build has no bubbles - a notice instead).
local BUBBLE = rt.have_api and (rt.api_version or 1) >= 5
local bubble_of = setmetatable({}, {__mode = "k"})  -- (target -> its last bubble's id)
function ui.bubble(target, text, seconds, opts)
  text = tostring(text)
  if not BUBBLE then
    local who = type(target) == "table" and target.name
    rt.notify(who and (who .. ": " .. text) or text, tonumber(seconds) or 4)
    return nil
  end
  local kind, handle, object, p = 0, 0, "", nil
  local mt = getmetatable(target)
  if mt == Npc then
    if target.removed then return nil end
    kind, handle = 2, target.id
  elseif mt == Poke then
    if target.removed then return nil end
    if target.id < 0 then kind, object = 4, target.object else kind, handle = 3, target.id end
  elseif target == "player" or target == player then kind = 1
  elseif type(target) == "string" then kind, object = 4, target
  else p = vec(target) end
  local h = type(opts) == "table" and tonumber(opts.height) or (mt == Poke and 1.0) or 0  -- (a Pokémon: lower than a head)
  local id = rt.bubble(kind, handle, object, p and p.x or 0, p and p.y or 0, p and p.z or 0, h, text, tonumber(seconds) or 0)
  if id == 0 then return nil end
  if type(target) == "table" then bubble_of[target] = id end
  return id
end
-- ui.bubble_hide(id or target): that bubble fades out
function ui.bubble_hide(x)
  if not BUBBLE then return false end
  local id = type(x) == "number" and x or bubble_of[x]
  return id ~= nil and id > 0 and rt.bubble_hide(id) == 1
end
-- a story work (Z-A "SYS_WORK_PHASE", Arceus "WPHA_STORY"; Arceus flags as "flag:<name>"): its value, or nil when the
-- game has no such work (yet); game.work(name, value) writes it
game.work = function(name, value)
  if (rt.api_version or 1) < 2 then error("game.work: this game build is too old", 2) end
  return rt.work(tostring(name), value ~= nil and math.floor(value) or nil)
end
G.item = item
-- the time of day: the game's clock set to that hour (0-24) at its next frame (Z-A)
game.set_time = function(hour)
  if (rt.api_version or 1) < 3 then error("game.set_time: this game build is too old", 2) end
  return rt.time_set(tonumber(hour) or error("game.set_time(hour)", 2))
end

-- each frame: the characters' states -> npc_up / npc_gone / npc_arrived; routes; near / far; talk (A by one)
local NEAR = 4.0        -- (near: within this many metres - a character's own near_radius overrides it)
local TALK = 2.2        -- (talk where the game has no Talk prompt (Z-A): A pressed this close, the character in front)
-- (Arceus: the game's own Talk prompt - "A <name>" over a character a talk handler is for, its A the talk)
local PROMPT = rt.have_api and (rt.api_version or 1) >= 4 and rt.npc_talk(0, 0) == 1
local function talks_to(n)
  if n.talk == false then return false end
  local l = handlers.talk
  if not l then return false end
  for _, h in ipairs(l) do if not h.dead and (h.filter == nil or h.filter == n) then return true end end
  return false
end
local function arrive(n, t)
  n._goal = nil
  local r = n._route
  if r then
    if r.opts.on_point then run({fn = r.opts.on_point, mod = n.mod}, "walk_route on_point", n, r.i) end
    r.wait_until = t + (tonumber(r.opts.wait) or 0)
  else
    emit_to("npc_arrived", n, n)
  end
end
local function npcs_poll()
  local t = now()
  for h, n in pairs(npcs) do
    local st = rt.npc_state(h)
    if st == 0 then
      npcs[h] = nil
      n.removed = true
    else
      local was = n._st or 1
      local up, wasup = st >= 3, was >= 3
      if up and not wasup then n._up_t = t emit_to("npc_up", n, n) end
      if wasup and not up then emit_to("npc_gone", n, n) n._near = false end
      if st == 4 then n._seen_walk = true end
      if PROMPT and up then  -- (the prompt over it while a talk handler is for it)
        local want = talks_to(n)
        if want ~= (n._prompt or false) then rt.npc_talk(h, want and 1 or 0) n._prompt = want end
      end
      -- (there: it walked and stands again - or it never had to walk, its goal a second old and it up)
      if n._goal and st == 3 and (n._seen_walk or t - math.max(n._req_t or 0, n._up_t or 0) > 1.0) then arrive(n, t) end
      local r = n._route
      if r and not r.paused and r.wait_until and t >= r.wait_until and up then
        r.wait_until = nil
        r.i = r.i + 1
        if r.i > #r.pts then
          if r.opts.loop then r.i = 1 else n._route = nil emit_to("npc_arrived", n, n) end
        end
        if n._route then go(n, r.pts[r.i], r.opts.run) end
      end
      n._st = st
    end
  end
end
local function near_poll()
  if not has("near") and not has("far") then return end
  local p = player.pos()
  if not p then return end
  for h, n in pairs(npcs) do
    if n._st and n._st >= 3 then
      local d = n:dist(p)
      local r = n.near_radius or NEAR
      if not n._near and d <= r then n._near = true emit_to("near", n, n, d)
      elseif n._near and d > r + 0.5 then n._near = false emit_to("far", n, n, d) end
    end
  end
end
local function talk_check()
  if PROMPT then  -- (the game's prompt: who was talked to)
    local h = rt.npc_talked()
    while h and h ~= 0 do
      local n = npcs[h]
      if n then emit_to("talk", n, n) end
      h = rt.npc_talked()
    end
    return
  end
  if not has("talk") then return end
  local p, yaw = player.pos(), player.yaw()
  if not p then return end
  local best, bd = nil, TALK
  for h, n in pairs(npcs) do
    if n._st and n._st >= 3 then
      local q = n:pos()
      if q then
        local d = p:dist2d(q)
        local a = math.abs(((p:yaw_to(q) - yaw) + 180) % 360 - 180)
        if d <= bd and (a <= 70 or d < 1.0) then best, bd = n, d end
      end
    end
  end
  if best then emit_to("talk", best, best) end
end

-- ---------------------------------------------------------------- the frame
local last_buttons, last_t, area_id, frame_n = 0, nil, nil, 0
local game_t = {}
G.__tick = function(frame)
  local step = frame - (G.__last_frame or frame - 1)
  G.__last_frame = frame
  frame_n = frame_n + (step > 0 and step or 1)
  local t = now()
  local dt = last_t and t - last_t or 0
  last_t = t
  game.dt = dt
  if rt.have_api then
    local b = rt.buttons()
    if b ~= last_buttons then
      local down = b & ~last_buttons
      local up = last_buttons & ~b
      last_buttons = b
      for i, name in ipairs(BUTTONS) do
        local bit = 1 << (i - 1)
        if down & bit ~= 0 then
          emit_to("button", name, name)
          if name == "A" and not PROMPT then talk_check() end
        end
        if up & bit ~= 0 then emit_to("button_up", name, name) end
      end
    end
    if frame_n % 15 < (G.__tick_every or 1) then
      local id, name = rt.area()
      if area_id ~= nil and id ~= area_id then emit("area_loaded", {id = id, name = name}) end
      area_id = id
    end
    local e = G.__tick_every or 1
    if frame_n % 6 < e and next(npcs) then npcs_poll() end
    if (frame_n + 3) % 10 < e and next(npcs) then near_poll() end
    if PROMPT and next(npcs) then talk_check() end
  end
  if next(jobs) then jobs_poll() end
  if co_alive then co_tick() end
  timers_run()
  if has("tick") then emit("tick", dt) end
  -- (how often the plugin calls this: every frame while something needs each one - tick handlers, buttons, talk -
  -- else every 4th, the rest needs no more: timers a few frames late at most)
  G.__tick_every = (has("tick") or has("button") or has("button_up") or has("talk") or co_alive) and 1 or 4
end

-- ---------------------------------------------------------------- each mod's globals
-- A mod's scripts see their own table, which falls back on the shared globals: two mods never overwrite each other's
-- variables; what a mod makes (handlers, timers, commands, characters) is known as its.
local function api_for(mod, env)
  env.MOD = mod.name
  env.vec = vec
  env.on = function(ev, a, b) return on(mod, ev, a, b) end
  env.off = off
  env.emit = function(ev, ...) return emit(ev, ...) end
  env.timer = {
    after = function(sec, fn) return timer_add(mod, sec, false, fn) end,
    every = function(sec, fn) return timer_add(mod, sec, true, fn) end,
    cancel = timer_cancel,
  }
  env.cmd = {
    add = function(name, fn, help) return cmd_add(mod, name, fn, help) end,
    run = cmd_run,
    list = function() local t = {} for k in pairs(commands) do t[#t + 1] = k end table.sort(t) return t end,
  }
  env.npc = setmetatable({
    spawn = function(t) return npc_spawn(mod, t) end,
    list = function() local t = {} for _, n in pairs(npcs) do if n.mod == mod then t[#t + 1] = n end end
                      table.sort(t, function(a, b) return a.id < b.id end) return t end,
  }, {__index = npc})
  env.player = player
  env.pokemon = pokemon
  env.item = item
  env.game = setmetatable({
    script = function(code, fn) return script(mod, tostring(code), fn) end,
    run = function(code, fn) return run_co(mod, tostring(code), fn) end,  -- (a coroutine: may wait - message windows)
    hash = function(name) return "S" .. fnv64(tostring(name)) end,
    call = function(fname, ...)  -- game.call("Commands.FObj_Get_Pos", "Player", function(r) ... end)
      local args = table.pack(...)
      local fn = args.n > 0 and type(args[args.n]) == "function" and args[args.n] or nil
      if fn then args[args.n] = nil args.n = args.n - 1 end
      return script(mod, call_code(tostring(fname), args), fn)
    end,
  }, {__index = game})
  env.ui = ui
  env.include = function(file)  -- (another script of this mod's own folder - "lib/x.lua" - run in its globals: its results)
    if type(file) ~= "string" or file:find("%.%.") or file:find("^[/\\]") or file:find(":") then
      error("include(\"lib/file.lua\"): a file in this mod's lua folder", 2)
    end
    local chunk, err = rt.loadfile(mod.dir .. "/" .. file, env)
    if not chunk then error(err, 2) end
    return chunk()
  end
  env.log = function(...) local t = {...} for i = 1, select("#", ...) do t[i] = tostring(t[i]) end
                          rt.log((mod == shared and "" or mod.name .. ": ") .. table.concat(t, "\t")) end
  env.print = env.log
  return env
end
api_for(shared, G)

G.mods = {}  -- (the console: mods.<name> is that mod's globals)
G.__mod_env = function(name, dir)
  if name == "" then shared.dir = dir return G end
  local mod = {name = name, dir = dir}
  local env = setmetatable({}, {__index = G})
  api_for(mod, env)
  mod.env = env
  mods[name] = mod
  mod_list[#mod_list + 1] = mod
  G.mods[name] = env
  return env
end
G.__mod_run = function(env, path, file)
  local mod = mods[env.MOD] or shared
  local chunk, err = rt.loadfile(path, env)
  if not chunk then
    report(mod, file, err)
    tell_once(mod.name .. ":load", fmt("Mod \"%s\": %s has an error and didn't run - see the log", mod.name, file))
    return
  end
  local ok, e = xpcall(chunk, rt.trace)
  if not ok then
    report(mod, file, e)
    tell_once(mod.name .. ":run", fmt("Mod \"%s\": %s stopped with an error - see the log", mod.name, file))
  end
end
G.__loaded = function()
  emit("loaded")
end
G.__unload = function()
  emit("unload")
  for h, n in pairs(npcs) do rt.npc_remove(h) end
  for h, p in pairs(pokes) do p:remove() end
end

-- ---------------------------------------------------------------- built-in console commands
cmd_add(shared, "help", function()
  local t = {}
  for k, c in pairs(commands) do t[#t + 1] = fmt("%-12s %s%s", k, c.help, c.mod ~= shared and " (" .. c.mod.name .. ")" or "") end
  table.sort(t)
  for _, l in ipairs(t) do print(l) end
end, "these commands")
cmd_add(shared, "npcs", function()
  for _, n in ipairs(npc.all()) do
    local p = n:pos()
    print(fmt("#%d %-16s %-8s %s  (%s)", n.id, n.name, n:state(), p and tostring(p) or "-", n.mod.name))
  end
end, "the characters scripts made")
cmd_add(shared, "models", function() print(npc.models()) end, "what new characters can look like here")
cmd_add(shared, "where", function()
  local p = player.pos()
  print(p and fmt("player at %s facing %.0f, area %s", tostring(p), player.yaw(), select(2, rt.area()) or "") or "no player yet")
end, "the player's position")
cmd_add(shared, "stats", function()
  local s = rt.stats()
  print(fmt("scripts: %.1f us a frame (max %.1f) over %d frames; hooks %d calls %.1f us each; Lua memory %d KB",
    s.frame_us, s.frame_max_us, s.frames, s.hook_calls, s.hook_us, s.memory_kb))
end, "what the scripts cost")
cmd_add(shared, "tp", function(x, y, z) player.teleport(x, y, z) end, "tp x y z: the player there")
cmd_add(shared, "bench", function(n)
  n = n or 200
  local function t(name, f)
    local a = rt.cpu()
    for i = 1, n do f() end
    print(fmt("%-16s %.2f us a call", name, (rt.cpu() - a) / n * 1e6))
  end
  local any = next(npcs)
  t("buttons", function() return rt.buttons() end)
  t("player.pos", function() return player.pos() end)
  t("player.yaw", function() return player.yaw() end)
  t("area", function() return rt.area() end)
  if any then t("npc_state", function() return rt.npc_state(any) end) end
  t("vec", function() return vec(1, 2, 3) end)
  t("now", function() return rt.now() end)
end, "bench [n]: what the mod API's calls cost here (CPU)")

G.vec = vec
G.lua = {stats = rt.stats, stats_reset = rt.stats_reset}
game.time = rt.now   -- (seconds, a steady clock)
game.cpu = rt.cpu    -- (this thread's CPU seconds: profiling a script)

-- ---------------------------------------------------------------- the game's interface: gameui
-- The game's own layouts (runtime/gameui.h): any pane moved, scaled, hidden, faded, anchored to the screen's edges, its
-- text changed. gameui.layouts() / gameui.panes(layout) show what is on screen now (layout and pane names), gameui.set
-- edits, gameui.reset takes a script's edits back. A reload takes them all back. (The mod API's version 7.)
local gameui = {}
local function ui_have() return rt.api_version and rt.api_version >= 7 end
function gameui.layouts()
  local out = {}
  local t = ui_have() and rt.ui_layouts() or nil
  for line in (t or ""):gmatch("[^\n]+") do
    local name, n, w, h = line:match("^(%S+) (%d+) ([%d.]+)x([%d.]+)")
    if name then out[#out + 1] = {name = name, panes = tonumber(n), w = tonumber(w), h = tonumber(h)} end
  end
  table.sort(out, function(a, b) return a.name < b.name end)
  return out
end
function gameui.panes(layout)
  if type(layout) ~= "string" then error("gameui.panes(layout): a layout's name (gameui.layouts())", 2) end
  local t = ui_have() and rt.ui_panes(layout) or nil
  if not t then return nil end
  local out = {}
  for line in t:gmatch("[^\n]+") do
    local d, name, ty, x, y, w, h, shown = line:match("^(%d+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%d)")
    if d then
      out[#out + 1] = {depth = tonumber(d), name = name, type = ty, x = tonumber(x), y = tonumber(y), w = tonumber(w),
                       h = tonumber(h), shown = shown == "1"}
    end
  end
  return out
end
local ui_keys = {x = 1, y = 1, sx = 1, sy = 1, scale = 1, w = 1, h = 1, alpha = 1, hide = 1, show = 1, anchor = 1, mode = 1,
                 fill = 1, follow = 1, text = 1, image = 1}
-- gameui.set(layout, pane, {x=, y=, scale=, sx=, sy=, w=, h=, alpha=, hide=, anchor=, mode=, fill=, follow=, text=, image=})
-- (pane nil or "": the layout itself)
function gameui.set(layout, pane, t)
  if type(layout) ~= "string" then error("gameui.set(layout, pane, {...}): a layout's name first", 2) end
  if type(pane) == "table" and t == nil then pane, t = "", pane end
  if type(t) ~= "table" then error("gameui.set(layout, pane, {...}): what to change, as a table", 2) end
  if not ui_have() then return false end
  local ok = true
  for k, v in pairs(t) do
    if not ui_keys[k] then error("gameui.set: no key \"" .. tostring(k) .. "\" (x y sx sy scale w h alpha hide show anchor mode fill follow text image)", 2) end
    if type(v) == "boolean" then v = v and "1" or "0" end
    if type(v) == "table" then v = table.concat(v, " ") end  -- (anchor = {"left", "top"})
    ok = rt.ui_set(layout, pane or "", k, tostring(v)) and ok
  end
  return ok
end
function gameui.reset(layout, pane) if ui_have() then rt.ui_unset(layout, pane) end end
function gameui.text(layout, pane) return ui_have() and rt.ui_text(layout, pane) or nil end
function gameui.clone(layout, src, name, t)
  if not ui_have() or not rt.ui_clone(layout, src, name) then return false end
  if t then gameui.set(layout, name, t) end
  return true
end
cmd_add(shared, "ui", function(layout)
  if not layout then
    for _, l in ipairs(gameui.layouts()) do print(fmt("%-36s %4d panes  %gx%g", l.name, l.panes, l.w, l.h)) end
    return
  end
  local ps = gameui.panes(layout)
  if not ps then print("no layout \"" .. layout .. "\" on screen (ui: the layouts)") return end
  for _, p in ipairs(ps) do
    print(fmt("%s%-24s %-8s %7.1f %7.1f  %6.1f x %-6.1f%s", string.rep("  ", p.depth), p.name, p.type, p.x, p.y, p.w, p.h, p.shown and "" or "  (hidden)"))
  end
end, "ui [layout]: the game's interface layouts on screen, or one's panes")
G.gameui = gameui

-- ---------------------------------------------------------------- the world side (world.lua: net, party, battles...)
-- api.lua's own parts handed to world.lua, which runs right after this in the same state and takes them (it adds its
-- tables to each mod's globals by wrapping api_for, its work to __tick / __unload)
G.__world_parts = {
  rt = rt, vec = vec, is_vec = is_vec, player = player, ui = ui, on = on, off = off, emit = emit, emit_to = emit_to,
  has = has, run = run, report = report, tell_once = tell_once, timer_add = timer_add, script = script, run_co = run_co,
  lit = lit, fnv64 = fnv64, Npc = Npc, Poke = Poke, npcs = npcs, pokes = pokes, shared = shared, cmd_add = cmd_add,
  api_for = function(f) local was = api_for if f then api_for = f end return was end,
}

-- ---------------------------------------------------------------- for the extensions (hud.lua, gui.lua: run next)
-- (the mod API's version 6 - the overlay, input, sounds, storage, menus - lives in files of its own; they reach what
-- this file keeps local through this table)
G.__api = {rt = rt, on = on, off = off, emit = emit, emit_to = emit_to, has = has, run = run, report = report,
           tell_once = tell_once, vec = vec, is_vec = is_vec, Npc = Npc, Poke = Poke, npcs = npcs, pokes = pokes,
           mods = mods, shared = shared, ui = ui, player = player, game = game, cmd_add = cmd_add, now = now}
