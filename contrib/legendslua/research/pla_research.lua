-- Arceus research (Legends Lua): thrown balls, the wild Pokemon's flick check and hit events, who asks a clock for its
-- frames, call stacks of any function, the event scripts' "arrived" state, watchpoints on AI components or any address.
-- Copy into <game folder>/lua/ with Legends Lua switched on; set what you want below (saving reloads it). Was
-- LARECOMP_PLA_BALL_LOG / FLICK_LOG / FRAMES_USERS / BT / STATE_WATCH / AI_WATCH and <live>/rwatch in the runtime.
-- Addresses: Legends: Arceus 1.1.1. How the catch bug was found with these: PLARecomp 17a25a9.
local BALL_LOG = false      -- a thrown object's flight step: the frames it was given, drag, lift, the velocity read
local FLICK_LOG = false     -- a wild Pokemon's "flick the ball away" answer as it changes; the hit events reaching it
local FRAMES_USERS = nil    -- a number of frames: then list who asked a clock for its frames, and how often
local BT = {}               -- addresses, e.g. {0x70019ecbd8}: the call stack of the first 3 calls of each
local STATE_WATCH = false   -- a watchpoint on the first event-script object's "arrived" state ([obj + 0x3e4])
local AI_WATCH = nil        -- a path in the first behaviour tree component, e.g. {0x2a8, 0x84}: [comp + 0x2a8] + 0x84 watched
local WATCH = {}            -- addresses watched now, e.g. {0x7251831440}

local function bt_line(c, n)
  local s = {}
  for _, a in ipairs(c:bt(n or 14)) do s[#s + 1] = string.format("%x", a) end
  return table.concat(s, " ")
end

if BALL_LOG then
  -- the flight step (f_7001a7191c state 1) at its velocity read f_7002adbce0 (from 0x7001a71b98)
  local n = 0
  hook(0x7002adbce0, function(c)
    if c.lr ~= 0x7001a71b9c then return end
    local fr, s8, s9 = mem.f32(c.sp), c:s(8), c:s(9)
    c:original()
    n = n + 1
    if n <= 400 then
      log(string.format("ball present %d frames %g drag %g lift %g velocity %g %g %g", game.frame(), fr, s8, s9, c:vf(0, 0), c:vf(0, 1), c:vf(0, 2)))
    end
    return true
  end)
  -- the clock f_7001a7236c hands that step: its vtable, and the frames getter at +0x30
  local k = 0
  hook(0x7001a7236c, function(c)
    if c.lr ~= 0x7001a71a28 then return end
    local out = c:x(8)
    c:original()
    k = k + 1
    if k <= 3 then
      local o = mem.u64(out) or 0
      local vt = o ~= 0 and mem.u64(o) or 0
      log(string.format("ball clock %x vtable %x frames getter %x", o, vt, vt ~= 0 and mem.u64(vt + 0x30) or 0))
    end
    return true
  end)
end

if FLICK_LOG then
  local last_o, last_r
  hook(0x7001aa085c, function(c)  -- (x0 = the Pokemon's component)
    local o = c:x(0)
    c:original()
    local r = c:x(0) & 1
    if o ~= last_o or r ~= last_r then
      log(string.format("flick %x -> %d at present %d (8d9 %d 821 %d 81d %d 956 %d a90 %d | 8b0 %d 8b4 %d)", o, r, game.frame(),
        mem.u8(o + 0x8d9), mem.u8(o + 0x821), mem.u8(o + 0x81d), mem.u8(o + 0x956), mem.u8(o + 0xa90), mem.i32(o + 0x8b0), mem.i32(o + 0x8b4)))
      last_o, last_r = o, r
    end
    return true
  end)
  local n = 0
  hook(0x7001911760, function(c)  -- (x1 = the Pokemon, x2 = the event, its type at +0xf0: 5 = a ball's hit)
    n = n + 1
    if n > 400 then return end
    local ev, obj = c:x(2), c:x(1)
    log(string.format("hitev obj %x event %x type %d at present %d (event vtable %x, obj vtable %x)", obj, ev,
      ev ~= 0 and mem.i32(ev + 0xf0) or -1, game.frame(), ev ~= 0 and mem.u64(ev) or 0, obj ~= 0 and mem.u64(obj) or 0))
  end)
end

if FRAMES_USERS then
  -- the clock's getters (vtable +0x30 f_70024a84fc, +0x38, +0x40): the callers and how often
  local users, from, done = {}, nil, false
  local function note(c)
    if done then return end
    local now = game.frame()
    from = from or now
    local key = string.format("f_%x from %x", c.addr, c.lr)
    users[key] = (users[key] or 0) + 1
    if now - from > FRAMES_USERS then
      done = true
      for k, n in pairs(users) do log(string.format("frames users %s: %d calls over %d presents", k, n, now - from)) end
    end
  end
  hook(0x70024a84fc, note)
  hook(0x70024a87d4, note)
  hook(0x70024a8cc4, note)
end

for _, a in ipairs(BT) do
  local n = 0
  hook(a, function(c)
    n = n + 1
    if n <= 3 then log(string.format("bt %x: lr %x x0 %x: %s", a, c.lr, c:x(0), bt_line(c))) end
  end)
end

if STATE_WATCH then
  local armed = false
  hook(0x70019b24dc, function(c)  -- (the event scripts' "arrived" check: [obj + 0x3e4] == 1)
    if armed or c:x(0) == 0 then return end
    armed = true
    log(string.format("statewatch obj %x state %d", c:x(0), mem.u32(c:x(0) + 0x3e4)))
    watch(c:x(0) + 0x3e4, 4)
  end)
end

if AI_WATCH then
  local armed = false
  hook(0x7000a6bc64, function(c)  -- (a behaviour tree component's update, x0 = the component)
    if armed or c:x(0) == 0 then return end
    local a = c:x(0)
    for i = 1, #AI_WATCH - 1 do a = mem.u64(a + AI_WATCH[i]); if not a or a == 0 then return end end
    a = a + AI_WATCH[#AI_WATCH]
    armed = true
    log(string.format("aiwatch component %x: watching %x", c:x(0), a))
    watch(a, 4)
  end)
end

for _, a in ipairs(WATCH) do watch(a, 4) end

log("pla_research hooks on")
