-- Legends Lua: the world side of the mod API (docs/lua-api.md from "The world"). Built into the plugin (world.c embeds
-- this file); it runs right after api.lua in each fresh state, given the primitives of the game's runtime/worldapi.h
-- (wt) and api.lua's own parts (__world_parts), and adds: net / players / chat (scripts talking between players over the
-- shared world), the player held, walked and sped up, the party and boxes (party / box: PKHeX's format decoded and
-- edited), battle events, world.trace / world.ground, weather. The same in both games; where they differ, it says so.
local wt = ...
local G = _G
local P = G.__world_parts
G.__world_parts = nil
if not P then return end
local rt, vec, player, ui = P.rt, P.vec, P.player, P.ui
local function timer_after(sec, fn) return P.timer_add(P.shared, sec, false, fn) end
local on, emit, has, run, report = P.on, P.emit, P.has, P.run, P.report
local fmt, spack, sunpack = string.format, string.pack, string.unpack
local ZA = game.name == "za"
math.randomseed(math.floor(rt.now() * 1e6) % 2147483647)

local function need(what)
  if not wt.have then error(what .. ": this game build has no world API - update the game", 3) end
end

-- ---------------------------------------------------------------- values <-> bytes (net messages)
-- nil, booleans, numbers, strings, vec and tables of them (8 deep at most; no functions, no cycles)
local function enc(v, out, depth)
  local t = type(v)
  if t == "nil" then out[#out + 1] = "z"
  elseif t == "boolean" then out[#out + 1] = v and "t" or "f"
  elseif t == "number" then
    if math.type(v) == "integer" then out[#out + 1] = spack("<Bj", 105, v) else out[#out + 1] = spack("<Bd", 100, v) end
  elseif t == "string" then out[#out + 1] = spack("<Bs2", 115, v)
  elseif t == "table" then
    if depth >= 8 then error("net.send: a table nested over 8 deep", 4) end
    if getmetatable(v) and getmetatable(v) == getmetatable(vec(0, 0, 0)) then
      out[#out + 1] = spack("<Bfff", 118, v.x, v.y, v.z)  -- ('v': a vec comes back a vec)
      return
    end
    out[#out + 1] = "{"
    for k, x in pairs(v) do
      local kt = type(k)
      if kt ~= "string" and kt ~= "number" and kt ~= "boolean" then error("net.send: a table key must be a string, number or boolean", 4) end
      enc(k, out, depth + 1)
      enc(x, out, depth + 1)
    end
    out[#out + 1] = "}"
  else error("net.send: can't send a " .. t, 4) end
end
local function dec(s, i, depth)
  if depth > 9 then error("too deep") end
  local c = s:byte(i)
  if c == 122 then return nil, i + 1           -- z
  elseif c == 116 then return true, i + 1      -- t
  elseif c == 102 then return false, i + 1     -- f
  elseif c == 105 then return sunpack("<j", s, i + 1)
  elseif c == 100 then return sunpack("<d", s, i + 1)
  elseif c == 115 then return sunpack("<s2", s, i + 1)
  elseif c == 118 then local x, y, z, j = sunpack("<fff", s, i + 1) return vec(x, y, z), j
  elseif c == 123 then                         -- {
    local t, k, v = {}, nil, nil
    i = i + 1
    while s:byte(i) ~= 125 do
      if i > #s then error("cut short") end
      k, i = dec(s, i, depth + 1)
      v, i = dec(s, i, depth + 1)
      if k == nil or k ~= k then error("bad key") end
      t[k] = v
    end
    return t, i + 1
  end
  error("bad byte " .. tostring(c))
end

-- ---------------------------------------------------------------- net: messages between players
local receivers = {}  -- name -> list of {fn, mod}
local function net_send(mod, name, data, to)
  need("net.send")
  if type(name) ~= "string" or #name == 0 or #name > 32 then error("net.send(name, data [, to]): name is 1-32 characters", 2) end
  local out = {spack("<s1", name)}
  enc(data, out, 0)
  local b = table.concat(out)
  if #b > 1024 then error(fmt("net.send: the message is %d bytes, over 1024", #b), 2) end
  if to ~= nil and type(to) == "table" then to = to.id end
  return wt.net_send(b, to)
end
local function net_receive(mod, name, fn)
  if type(name) ~= "string" or type(fn) ~= "function" then error("net.receive(name, fn(from, data))", 2) end
  local l = receivers[name]
  if not l then l = {} receivers[name] = l end
  l[#l + 1] = {fn = fn, mod = mod}
end
local function net_poll()
  for _ = 1, 64 do
    local b, from = wt.net_recv()
    if not b then return end
    local ok, name, data = pcall(function()
      local n, i = sunpack("<s1", b, 1)
      local v, j = dec(b, i, 0)
      if j ~= #b + 1 then error("bytes left over") end
      return n, v
    end)
    if not ok then rt.log("net: a message from " .. tostring(from) .. " didn't decode (" .. tostring(name) .. ")")
    else
      local l = receivers[name]
      if l then for _, h in ipairs(l) do run(h, "net.receive " .. name, from, data) end end
    end
  end
end

-- ---------------------------------------------------------------- players: the others in the shared world
local players = {}
function players.list()
  if not wt.have then return {} end
  local t = {}
  for i, p in ipairs(wt.players()) do
    t[i] = {id = p.id, pos = vec(p.x, p.y, p.z), yaw = p.yaw, standin = p.standin, dist = p.dist >= 0 and p.dist or nil}
  end
  return t
end
function players.me() return wt.have and wt.net_self() or nil end
function players.online() return wt.have and wt.net_up() or false end
function players.find(id) for _, p in ipairs(players.list()) do if p.id == id then return p end end end

-- ---------------------------------------------------------------- chat: a line over the sender's stand-in, everyone
local chat = {}
function chat.say(text)
  need("chat.say")
  text = tostring(text)
  if #text == 0 then return false end
  return wt.chat_send(text)
end
local function chat_poll()
  for _ = 1, 16 do
    local text, from = wt.chat_recv()
    if not text then return end
    emit("chat", from, text)
  end
end

-- the chat key: T on the keyboard, L + ZL held together on a controller - while the shared world is on - opens the
-- runtime's text dialog (input.text: the on-screen keyboard with a controller) and says what was typed. chat.key(false)
-- switches it off (a mod with a T of its own), chat.key(true) back on.
local chat_key_on, chat_prompting = true, false
function chat.key(on_) chat_key_on = on_ ~= false end
function chat.prompt()
  local inp = G.input
  if chat_prompting or not (inp and inp.text) or not players.online() then return false end
  chat_prompting = true
  inp.text("Chat", "", 100, function(t)
    chat_prompting = false
    if t and #t > 0 then chat.say(t) end
  end)
  return true
end
on(P.shared, "key", "T", function(e)
  if chat_key_on and e.down and not e.rep and not G.__gui_open and not chat_prompting then chat.prompt() end
end)
on(P.shared, "button", function(b)
  if chat_key_on and (b == "L" or b == "ZL") and player.button("L") and player.button("ZL") and not G.__gui_open then chat.prompt() end
end)

-- ---------------------------------------------------------------- the player held, walked, sped up
local BUTTONS = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"}
local BIT = {}
for i, n in ipairs(BUTTONS) do BIT[n] = 1 << (i - 1) end
local held = {buttons = 0, sticks = 0}
-- player.freeze(true) holds the player still: every button but + and - kept from the game, both sticks at rest (the
-- game's menus too: what it shows stays up). opts.camera = true lets the camera stick through. Scripts still see the
-- buttons (on("button"), player.button), so a frozen player can drive a script's own controls.
function player.freeze(on_, opts)
  need("player.freeze")
  if on_ == false then held.buttons, held.sticks = 0, 0
  else
    held.buttons = 0xffff & ~(BIT.PLUS | BIT.MINUS)
    held.sticks = (type(opts) == "table" and opts.camera) and 1 or 3
  end
  wt.input_hold(held.buttons, held.sticks)
  return true
end
function player.frozen() return held.buttons ~= 0 or held.sticks ~= 0 end
-- player.hold({"A", "B"}, sticks): just these buttons kept from the game (sticks: "left", "right", "both"); hold() lets go
function player.hold(list, sticks)
  need("player.hold")
  local m = 0
  for _, n in ipairs(list or {}) do
    local b = BIT[string.upper(tostring(n))]
    if not b then error("player.hold: one of " .. table.concat(BUTTONS, " "), 2) end
    m = m | b
  end
  held.buttons = m
  held.sticks = sticks == "left" and 1 or sticks == "right" and 2 or (sticks == "both" or sticks == true) and 3 or 0
  wt.input_hold(held.buttons, held.sticks)
end
-- player.walk(x, y): the left stick held there for the game (-1..1, y forward: as the player's own stick - relative to
-- the camera), player.walk(false) gives it back. player.walk_dir(yaw): toward a world direction (needs the camera's yaw;
-- a mod usually steers by player.yaw instead)
local walking = false
function player.walk(x, y)
  need("player.walk")
  if x == false or x == nil then walking = false wt.input_stick(false, 0, 0) return end
  walking = true
  wt.input_stick(true, tonumber(x) or 0, tonumber(y) or 0)
end
-- player.speed(k): how fast the player moves, times k (1 normal). Arceus: the player's animation controller's master
-- speed (SetAnimationMasterSpeed) - its walk and run carry it by their root motion, so faster steps go further (2x:
-- 2.3x the way in the same time, checked 2026-11-19); every animation of the player's plays that much faster. Z-A: the
-- runtime's (worldapi player_speed) - false where it isn't found.
local speed_k = 1
local PLA_SPEED = [[Global.GetFieldObjectForHaxe():__haxe_export_FindCharactorComponent(FnvHash64.new("PlayerObject")):__haxe_export_GetCharacterController():__haxe_export_GetAnimationController():SetAnimationMasterSpeed(%s)]]
function player.speed(k)
  need("player.speed")
  k = tonumber(k) or 1
  if k <= 0 or k > 10 then error("player.speed(k): 0 < k <= 10", 2) end
  if ZA then return wt.speed(k) end
  speed_k = k
  P.script(nil, fmt(PLA_SPEED, fmt("%.6g", k)))
  return true
end

-- ---------------------------------------------------------------- Pokémon: the party and the boxes (PKHeX's formats)
-- Z-A: PA9 (the party format in boxes too: level and stats there), species stored as Gen 9's internal numbers (from 917
-- on they differ from the national dex: PKHeX's SpeciesConverter tables). Arceus: PA8 (boxes hold the stored part: no
-- level there - the EXP is).
local T9 = {65, -1, -1, -1, -1, 31, 31, 47, 47, 29, 29, 53, 31, 31, 46, 44, 30, 30, -7, -7, -7, 13, 13, -2, -2, 23, 23, 24,
  -21, -21, 27, 27, 47, 47, 47, 26, 14, -33, -33, -33, -17, -17, 3, -29, 12, -12, -31, -31, -31, 3, 3, -24, -24, -44, -44, -30,
  -30, -28, -28, 23, 23, 6, 7, 29, 8, 3, 4, 4, 20, 4, 23, 6, 3, 3, 4, -1, 13, 9, 7, 5, 7, 9, 9, -43, -43, -43, -68, -68, -68,
  -58, -58, -25, -29, -31, 6, -1, 6, 0, 0, 0, 3, 3, 4, 2, 3, 3, -5, -12, -12}
local function national9(raw) local d = T9[raw - 917 + 1] return d and raw + d or raw end
local function internal9(nat)
  if nat < 917 then return nat end
  for i, d in ipairs(T9) do if 917 + i - 1 + d == nat then return 917 + i - 1 end end
  return nat
end
-- the formats' fields: {offset, kind} (u8 / u16 / u32 / s16 = UTF-16 text of 13 units)
local F = ZA and {
  species = {0x08, "u16"}, item = {0x0A, "u16"}, tid = {0x0C, "u16"}, sid = {0x0E, "u16"}, exp = {0x10, "u32"},
  ability = {0x14, "u16"}, pid = {0x1C, "u32"}, nature = {0x20, "u8"}, stat_nature = {0x21, "u8"}, form = {0x24, "u8"},
  nickname = {0x58, "s16"}, moves = {0x72, "u16x4"}, pp = {0x7A, "u8x4"}, hp = {0x8A, "u16"}, iv32 = {0x8C, "u32"},
  ot = {0xF8, "s16"}, ball = {0x124, "u8"}, level = {0x148, "u8"}, stats = {0x14A, "u16x6"}, ec = {0x00, "u32"},
} or {
  species = {0x08, "u16"}, item = {0x0A, "u16"}, tid = {0x0C, "u16"}, sid = {0x0E, "u16"}, exp = {0x10, "u32"},
  ability = {0x14, "u16"}, pid = {0x1C, "u32"}, nature = {0x20, "u8"}, stat_nature = {0x21, "u8"}, form = {0x24, "u8"},
  nickname = {0x60, "s16"}, moves = {0x54, "u16x4"}, pp = {0x5C, "u8x4"}, hp = {0x92, "u16"}, iv32 = {0x94, "u32"},
  ot = {0x110, "s16"}, ball = {0x137, "u8"}, level = {0x168, "u8"}, stats = {0x16A, "u16x6"}, ec = {0x00, "u32"},
}
local function utf16_get(b, o)
  local t = {}
  for k = 0, 12 do
    local c = sunpack("<I2", b, o + 1 + k * 2)
    if c == 0 then break end
    t[#t + 1] = utf8.char(c)
  end
  return table.concat(t)
end
local function utf16_put(s, maxu)
  local t, n = {}, 0
  for _, c in utf8.codes(s) do
    if n >= maxu then break end
    if c > 0xffff then c = 0x3f end
    t[#t + 1] = spack("<I2", c)
    n = n + 1
  end
  for _ = n, 12 do t[#t + 1] = spack("<I2", 0) end
  return table.concat(t)
end
local function get(b, f)
  local o, k = f[1], f[2]
  if o + 1 > #b then return nil end
  if k == "u8" then return b:byte(o + 1)
  elseif k == "u16" then return (sunpack("<I2", b, o + 1))
  elseif k == "u32" then return (sunpack("<I4", b, o + 1))
  elseif k == "s16" then return utf16_get(b, o)
  elseif k == "u16x4" then local a, c, d, e = sunpack("<I2I2I2I2", b, o + 1) return {a, c, d, e}
  elseif k == "u8x4" then return {b:byte(o + 1, o + 4)}
  elseif k == "u16x6" then if o + 12 > #b then return nil end local a, c, d, e, f, g = sunpack("<I2I2I2I2I2I2", b, o + 1) return {a, c, d, e, f, g}
  end
end
local function put(b, f, v)
  local o, k = f[1], f[2]
  local s
  if k == "u8" then s = spack("<I1", v)
  elseif k == "u16" then s = spack("<I2", v)
  elseif k == "u32" then s = spack("<I4", v)
  elseif k == "s16" then s = utf16_put(v, 12)
  elseif k == "u16x4" then s = spack("<I2I2I2I2", v[1] or 0, v[2] or 0, v[3] or 0, v[4] or 0)
  elseif k == "u8x4" then s = spack("<I1I1I1I1", v[1] or 0, v[2] or 0, v[3] or 0, v[4] or 0)
  else error("can't set that") end
  return b:sub(1, o) .. s .. b:sub(o + #s + 1)
end
local party_used, party_job, party_t, party_seen = false, false, -1e9, false  -- (the party's asking: below)
local function curve(g, n)
  if g == 0 then return n ^ 3
  elseif g == 4 then return math.floor(4 * n ^ 3 / 5)
  elseif g == 5 then return math.floor(5 * n ^ 3 / 4)
  elseif g == 3 then return math.max(0, math.floor(6 * n ^ 3 / 5 - 15 * n * n + 100 * n - 140))
  elseif g == 1 then
    if n <= 50 then return math.floor(n ^ 3 * (100 - n) / 50)
    elseif n <= 68 then return math.floor(n ^ 3 * (150 - n) / 100)
    elseif n <= 98 then return math.floor(n ^ 3 * ((1911 - 10 * n) // 3) / 500)
    else return math.floor(n ^ 3 * (160 - n) / 100) end
  else
    if n <= 15 then return math.floor(n ^ 3 * ((n + 1) // 3 + 24) / 50)
    elseif n <= 36 then return math.floor(n ^ 3 * (n + 14) / 50)
    else return math.floor(n ^ 3 * (n // 2 + 32) / 50) end
  end
end
local function level_of(g, exp)
  local lv = 1
  while lv < 100 and curve(g, lv + 1) <= exp do lv = lv + 1 end
  return lv
end
local Mon = {}
Mon.__index = Mon
Mon.__tostring = function(m) return fmt("pokemon #%d%s lv %s \"%s\"", m.species, m.form ~= 0 and ("-" .. m.form) or "", tostring(m.level), m.nickname) end
local function decode(b, box, slot)
  local m = setmetatable({box = box, slot = slot, _b = b}, Mon)
  for k, f in pairs(F) do m[k] = get(b, f) end
  if ZA then m.species = national9(m.species) end
  local iv = m.iv32
  m.ivs = {iv & 31, (iv >> 5) & 31, (iv >> 10) & 31, (iv >> 15) & 31, (iv >> 20) & 31, (iv >> 25) & 31}  -- (HP Atk Def Spe SpA SpD)
  m.egg = (iv >> 30) & 1 == 1
  m.nicknamed = (iv >> 31) & 1 == 1
  m.gender = ZA and (b:byte(0x22 + 1) >> 1) & 3 or (b:byte(0x22 + 1) >> 2) & 3   -- (0 male, 1 female, 2 none)
  m.alpha = ZA and b:byte(0x23 + 1) ~= 0 or (b:byte(0x16 + 1) & 32) ~= 0
  local x = ((m.pid >> 16) ~ (m.pid & 0xffff) ~ m.tid ~ m.sid)
  m.shiny = x < 16
  if box ~= -1 and not ZA then  -- (Arceus' boxes: the stored part only - the level is its EXP on the species' growth
    m.level, m.stats = nil, nil  --  curve, read from the game's own personal table)
    local g = wt.growth and wt.growth(m.species, m.form)
    if g then m.level = level_of(g, m.exp) m.growth = g end
  end
  m.iv32 = nil
  return m
end
local function slot_read(box, slot)
  local b = wt.pkm_read(box, slot)
  return b and decode(b, box, slot) or nil
end
-- m:set{nickname =, moves =, item =, form =, nature =, ivs =, shiny =, exp =, level = (with exp), hp =, ball =}: written
-- into its slot at once (checksum and encryption as the game's), its table updated. The game shows the change when it
-- next reads the slot (the box screen, the party screen); its own copies (a battle running) aren't touched.
function Mon:set(t)
  if not ZA and self.box == -1 then  -- (Arceus' party: the game's own members - HP and form through its commands)
    for k in pairs(t) do if k ~= "hp" and k ~= "form" then error("pokemon:set: Arceus' party takes hp and form only (its members are the game's; put one in a box to change more)", 2) end end
    local i = self.slot
    if t.hp then P.script(nil, fmt("PokemonCommand.SetHp(-1, %d, %d)", i, math.floor(t.hp))) self.hp = math.floor(t.hp) end
    if t.form then P.script(nil, fmt("PokemonCommand.ChangeFormNo(-1, %d, %d)", i, math.floor(t.form))) self.form = math.floor(t.form) end
    party_t = -1e9  -- (asked again soon)
    return true
  end
  local b = self._b
  local function want(k, check)
    local v = t[k]
    if v == nil then return nil end
    if check and not check(v) then error("pokemon:set: bad " .. k, 3) end
    return v
  end
  local function n16(v) return math.type(v) == "integer" and v >= 0 and v < 65536 end
  local function n8(v) return math.type(v) == "integer" and v >= 0 and v < 256 end
  local v
  v = want("nickname", function(s) return type(s) == "string" and #s > 0 end)
  if v then
    b = put(b, F.nickname, v)
    local o = (ZA and 0x8C or 0x94) + 1
    b = b:sub(1, o - 1) .. spack("<I4", sunpack("<I4", b, o) | 0x80000000) .. b:sub(o + 4)
  end
  v = want("moves", function(x) return type(x) == "table" end)
  if v then
    for i = 1, 4 do if v[i] ~= nil and not n16(v[i]) then error("pokemon:set: moves are numbers", 2) end end
    b = put(b, F.moves, v)
    local pp = get(b, F.pp)  -- (a new move with no PP left gets 20 - the game tops it up at a Pokémon Center)
    for i = 1, 4 do if (v[i] or 0) ~= 0 and pp[i] == 0 then pp[i] = 20 elseif (v[i] or 0) == 0 then pp[i] = 0 end end
    b = put(b, F.pp, pp)
  end
  v = want("item", n16) if v then b = put(b, F.item, v) end
  v = want("form", n8) if v then b = put(b, F.form, v) end
  v = want("nature", function(x) return n8(x) and x < 25 end) if v then b = put(b, F.nature, v) b = put(b, F.stat_nature, v) end
  v = want("ball", n8) if v then b = put(b, F.ball, v) end
  v = want("exp", function(x) return math.type(x) == "integer" and x >= 0 and x <= 1640000 end) if v then b = put(b, F.exp, v) end
  v = want("level", function(x) return math.type(x) == "integer" and x >= 1 and x <= 100 end)
  if v then
    if t.exp == nil then error("pokemon:set: level needs exp too (the game counts the level from it)", 2) end
    if self.box == -1 or ZA then b = put(b, F.level, v) end
  end
  v = want("hp", n16) if v then b = put(b, F.hp, v) end
  v = want("ivs", function(x) return type(x) == "table" end)
  if v then
    local o = (ZA and 0x8C or 0x94) + 1
    local old = sunpack("<I4", b, o)
    local iv = old & 0xC0000000
    for i = 1, 6 do local x = math.max(0, math.min(31, math.floor(v[i] or 0))) iv = iv | (x << (5 * (i - 1))) end
    b = b:sub(1, o - 1) .. spack("<I4", iv) .. b:sub(o + 4)
  end
  if t.shiny ~= nil then  -- (the PID's top half made to match - or not - the trainer's ids, as PKHeX's SetShiny / SetIsShiny)
    local pid = sunpack("<I4", b, 0x1C + 1)
    local tid, sid = sunpack("<I2I2", b, 0x0C + 1)
    local low = pid & 0xffff
    local hi = t.shiny and (tid ~ sid ~ low) or ((tid ~ sid ~ low) ~ 0x10)
    pid = (hi << 16) | low
    b = b:sub(1, 0x1C) .. spack("<I4", pid) .. b:sub(0x1C + 5)
  end
  if t.species ~= nil then
    local s = t.species
    if not (math.type(s) == "integer" and s >= 1 and s <= 1025) then error("pokemon:set: species 1-1025", 2) end
    b = put(b, F.species, ZA and internal9(s) or s)
  end
  if not wt.pkm_write(self.box, self.slot, b) then return false end
  local fresh = slot_read(self.box, self.slot)
  if fresh then for k, x in pairs(fresh) do self[k] = x end end
  return true
end
function Mon:bytes() return self._b end  -- (the decrypted data, PKHeX's .pa9 / .pa8)
local party, box = {}, {}
-- Z-A: where the party's members are is asked of the game's own Lua (its Player object's party: each member's script
-- object -> [ud] -> [+8] -> [+0x50] -> [+0x30] = its data) and handed to the runtime, which checks it - every 3 s once a
-- script has used the party (a swap, a catch, a release moves them), and at once with party.refresh()
local party_lv = {}  -- (Z-A: the party's levels, as the game's members say - its party data is the stored part only)
local PARTY_JOB = [[local p = Commands.SCDED1A922C1D919F("Player") if p == nil then return "" end
local party = p:fA1DE81A4() if party == nil then return "" end
local t = {} for i = 0, party:f5BFC43FB() - 1 do local m = party:f701A6236(i) t[#t + 1] = m and (tostring(m):match("0x%x+") .. ":" .. tostring(m:fD2EF1BBB())) or "0:0" end
return table.concat(t, " ")]]
-- Arceus: the party the game plays with is its own objects - the save's party block ([[main+42BA6B0]+200]+68, decoded
-- here: names, IVs, trainer...) is written when the game saves. What changes in play (level, HP, moves, form, a new
-- member) is asked of the game's PokemonCommand (its scripts' reads) every 3 s once used, and laid over the block's.
local live = {}  -- (Arceus: {species, form, level, hp, max hp, shiny, alpha, moves} per member, as the game says)
local PLA_PARTY_JOB = [[local P, t = PokemonCommand, {}
for i = 0, 5 do
  local s = P.GetMonsNo(-1, i)
  if not s or s == 0 then break end
  t[#t + 1] = table.concat({s, P.GetFormNo(-1, i), P.GetLevel(-1, i), P.GetHp(-1, i), P.GetMaxHp(-1, i), P.IsRare(-1, i) and 1 or 0,
    P.IsOybn(-1, i) and 1 or 0, P.GetWaza(-1, i, 0), P.GetWaza(-1, i, 1), P.GetWaza(-1, i, 2), P.GetWaza(-1, i, 3)}, ":")
end
return table.concat(t, " ")]]
local function party_resolve(fn)
  if party_job then return end
  party_job = true
  if not ZA then
    P.script(P.shared, PLA_PARTY_JOB, function(r)
      party_job = false
      party_t = rt.now()
      live = {}
      for w in (r or ""):gmatch("%S+") do
        local v = {}
        for x in w:gmatch("[^:]+") do v[#v + 1] = tonumber(x) or 0 end
        live[#live + 1] = {species = v[1], form = v[2], level = v[3], hp = v[4], hp_max = v[5], shiny = v[6] == 1, alpha = v[7] == 1,
                           moves = {v[8], v[9], v[10], v[11]}}
      end
      party_seen = true
      if fn then fn() end
    end)
    return
  end
  P.script(P.shared, PARTY_JOB, function(r)
    party_job = false
    party_t = rt.now()
    local n = 0
    for a, lv in (r or ""):gmatch("(%S+):(%S+)") do
      party_lv[n + 1] = tonumber(lv)
      local ud = tonumber(a) or 0
      local o = ud ~= 0 and mem.u64(ud)
      local x = o and mem.u64(o + 8)
      local w = x and mem.u64(x + 0x50)
      local d = w and mem.u64(w + 0x30)
      wt.party_at(n, d or 0)
      n = n + 1
    end
    for k = n, 5 do wt.party_at(k, 0) end
    wt.party_at(-1, n)
    party_seen = true
    if fn then fn() end
  end)
end
local function party_poll() if party_used and rt.now() - party_t > 3 then party_resolve() end end
-- party.refresh([fn]): the party asked of the game again now (fn() once it answered); party.ready(): known yet
function party.refresh(fn) party_used = true party_resolve(fn) end
function party.ready() party_used = true if not party_seen then party_resolve() end return party_seen end
function party.get(i)
  need("party.get")
  party.ready()
  i = math.floor(i)
  if not ZA then
    local l = live[i]
    if party_seen and not l then return nil end
    local m = slot_read(-1, i - 1)
    if l then
      if not m or m.species ~= l.species then m = setmetatable({box = -1, slot = i - 1, _b = nil, nickname = "", stale = true}, Mon) end
      for k, v in pairs(l) do m[k] = v end
      m.stats = m.stats or {}
      m.stats[1] = l.hp_max
    end
    if m then m.live = l ~= nil end
    return m
  end
  local m = slot_read(-1, i - 1)
  if m then m.level = party_lv[i] end
  return m
end
function party.list()
  need("party.list")
  local t = {}
  for i = 1, 6 do local m = party.get(i) if m then t[#t + 1] = m end end
  return t
end
function party.count() party.ready() if not ZA then return #live end local ok, st, pa, n = wt.pkm_info() return n end
function box.get(b, s)
  need("box.get")
  return slot_read(math.floor(b) - 1, math.floor(s) - 1)
end
function box.list(b)
  need("box.list")
  local t = {}
  for s = 1, 30 do local m = box.get(b, s) if m then t[#t + 1] = m end end
  return t
end
-- box.put(b, s, bytes): a whole Pokémon (PKHeX's decrypted .pa9 / .pa8 data, e.g. another's m:bytes()) into a slot
function box.put(b, s, bytes)
  need("box.put")
  if type(bytes) ~= "string" then error("box.put(box, slot, bytes)", 2) end
  return wt.pkm_write(math.floor(b) - 1, math.floor(s) - 1, bytes)
end
function box.empty_slot()
  for b = 1, 32 do for s = 1, 30 do if not wt.pkm_read(b - 1, s - 1) then return b, s end end end
end

local pokemon = G.pokemon  -- (api.lua's: pokemon.spawn ...; give added here)
-- ---- a new Pokémon into the first empty box slot (pokemon.give): made from one of the player's own (their trainer
-- data, the game's format) with its species, form, level, moves and colours changed - as PKHeX would. Its EXP is the
-- level's on the species' growth curve, which is found from another of that species the player has (its level and
-- EXP); without one, the medium-fast curve (most Pokémon): the game then shows the level its EXP makes.
local function growth_of(species, form)
  local gr = wt.growth and wt.growth(species, form or 0)
  if gr then return gr end
  local function fit(m)
    if not (m and m.species == species and m.level and m.level > 1 and m.level < 100) then return nil end
    for g = 0, 5 do if curve(g, m.level) <= m.exp and m.exp < curve(g, m.level + 1) then return g end end
  end
  for i = 1, 6 do local g = fit(party.get(i)) if g then return g end end
  for b = 1, 32 do for s = 1, 30 do local g = fit(box.get(b, s)) if g then return g end end end
  return 0
end
-- pokemon.give{species =, level = 5, form = 0, shiny = false, name =, moves = {..}, ball =, alpha = false}: into the first
-- empty box slot; returns box, slot (nil + why). name: what the box shows (the game stores a Pokémon's name with it; a
-- nameless one is called "Pokémon") - set nickname = true to make it count as a nickname.
function pokemon.give(t)
  need("pokemon.give")
  if type(t) ~= "table" then t = {species = t} end
  local sp = t.species
  if not (math.type(sp) == "integer" and sp >= 1 and sp <= 1025) then error("pokemon.give{species = 1-1025, level = ...}", 2) end
  local lv = math.max(1, math.min(100, math.floor(tonumber(t.level) or 5)))
  local tmpl = nil
  for i = 1, 6 do local m = party.get(i) if m and not m.egg then tmpl = m break end end
  if not tmpl then for b = 1, 32 do for s = 1, 30 do local m = box.get(b, s) if m and not m.egg then tmpl = m break end end if tmpl then break end end end
  if not tmpl then return nil, "no Pokémon of the player's to make it from (yet)" end
  local bn, sn = box.empty_slot()
  if not bn then return nil, "the boxes are full" end
  local b = tmpl:bytes()
  local stored = ZA and 0x148 or 0x168
  if #b < stored then return nil, "unexpected data" end
  b = b:sub(1, stored)
  if ZA then b = b .. string.rep("\0", 0x10) end  -- (Z-A's boxes keep the party part: level, stats - below)
  local function w(off, fmt_, ...) local s = spack(fmt_, ...) b = b:sub(1, off) .. s .. b:sub(off + #s + 1) end
  local ec = math.random(0, 0xffffffff)
  local tid, sid = sunpack("<I2I2", b, 0x0C + 1)
  local low = math.random(0, 0xffff)
  local hi = t.shiny and (tid ~ sid ~ low) or ((tid ~ sid ~ low ~ math.random(16, 0xffff)) & 0xffff)
  if not t.shiny and ((hi ~ tid ~ sid ~ low) < 16) then hi = hi ~ 0x8000 end
  w(0x00, "<I4", ec)
  w(0x08, "<I2", ZA and internal9(sp) or sp)
  w(0x0A, "<I2", 0)                                 -- (no held item)
  w(0x1C, "<I4", (hi << 16) | low)
  w(0x24, "<I1", math.floor(tonumber(t.form) or 0))
  local g = growth_of(sp, math.floor(tonumber(t.form) or 0))
  w(0x10, "<I4", curve(g, lv))
  local mv = t.moves or {33}
  local moves_off, pp_off, relearn_off, iv_off, hp_off = ZA and 0x72 or 0x54, ZA and 0x7A or 0x5C, ZA and 0x82 or 0x8A, ZA and 0x8C or 0x94, ZA and 0x8A or 0x92
  w(moves_off, "<I2I2I2I2", mv[1] or 0, mv[2] or 0, mv[3] or 0, mv[4] or 0)
  w(pp_off, "<I1I1I1I1", mv[1] and 20 or 0, mv[2] and 20 or 0, mv[3] and 20 or 0, mv[4] and 20 or 0)
  w(relearn_off, "<I2I2I2I2", 0, 0, 0, 0)
  local iv = 0
  for i = 0, 5 do iv = iv | (math.random(0, 31) << (5 * i)) end
  if t.nickname then iv = iv | 0x80000000 end
  w(iv_off, "<I4", iv)
  if ZA then b = b:sub(1, 0x23) .. string.char(t.alpha and 1 or 0) .. b:sub(0x25)
  else b = b:sub(1, 0x16) .. string.char((b:byte(0x17) & ~0x60) | (t.alpha and 0x20 or 0)) .. b:sub(0x18) end
  b = put(b, F.nickname, tostring(t.name or "Pokémon"))
  if t.ball then w(F.ball[1], "<I1", math.floor(t.ball)) end
  local hp = math.floor(10 + lv * 2.5)                -- (rough stats: the game works out its own when it uses it)
  w(hp_off, "<I2", hp)
  if ZA then w(0x148, "<I1I1I2I2I2I2I2I2", lv, 0, hp, lv * 2 + 5, lv * 2 + 5, lv * 2 + 5, lv * 2 + 5, lv * 2 + 5) end
  if not wt.pkm_write(bn - 1, sn - 1, b) then return nil, "not written" end
  rt.log(fmt("pokemon.give: #%d lv %d into box %d slot %d (growth %d)", sp, lv, bn, sn, g))
  return bn, sn
end

-- ---------------------------------------------------------------- battles: battle_start / battle_end events
local battle = {}
local b_on, b_count = 0, nil
local b_snap, b_last = nil, nil
local KINDS = {[0] = "unknown", "wild", "trainer"}
-- (what a battle changed, seen from outside it: a Pokémon more (party or boxes) -> "caught"; the whole party fainted ->
-- "lost"; else "other" (won or ran - the game keeps no result a script can read for a battle it didn't start from an
-- event)
local function snapshot(fn)
  party.refresh(function()
    local s = {party = 0, hp = 0, boxed = 0}
    for i = 1, 6 do local m = party.get(i) if m then s.party = s.party + 1 s.hp = s.hp + (m.hp or 0) end end
    for b = 0, 31 do for k = 0, 29 do if wt.pkm_read(b, k) then s.boxed = s.boxed + 1 end end end
    fn(s)
  end)
end
function battle.active() if not wt.have then return false end local on_ = wt.battle() return on_ == 1 end
function battle.last() return b_last end
local function battle_poll()
  local on_, kind, res, count = wt.battle()
  if on_ ~= b_on then
    b_on = on_
    if on_ == 1 then
      b_snap = nil
      snapshot(function(s) b_snap = s end)
      emit("battle_start", {kind = KINDS[kind] or "unknown"})
    end
  end
  if b_count == nil then b_count = count
  elseif count ~= b_count then
    b_count = count
    local before = b_snap
    timer_after(1.5, function()  -- (the game puts a caught one away / heals after its end screen: looked at a little later)
      snapshot(function(s)
        local r = "other"
        if before and (s.party + s.boxed > before.party + before.boxed) then r = "caught"
        elseif s.party > 0 and s.hp == 0 then r = "lost" end
        b_last = r
        emit("battle_end", {result = r, party_hp = s.hp})
      end)
    end)
  end
end

-- ---------------------------------------------------------------- the world through the game's own scripts
-- (traces, objects, the camera, the player's animation, rides, a wild battle: the games' own script commands, run in
-- their Lua with game.script / game.run - so their answers come a frame later, to a callback)
local world, camera = {}, {}
local S, RUN, lit = P.script, P.run_co, P.lit
local function num(v) return fmt("%.9g", v) end
local function xyz3(p) p = vec(p) return num(p.x) .. ", " .. num(p.y) .. ", " .. num(p.z) end
local function parse_nums(r)
  local t = {}
  for w in tostring(r or ""):gmatch("%S+") do t[#t + 1] = tonumber(w) or w end
  return t
end
-- (Arceus: a Vector3 of the game's, made from x, y, z; its parts read back - .x or :GetX())
local PLA_PRE = [[local function V(x, y, z) local v = Vector3.new() v:Set(x, y, z) return v end
local function XYZ(v) if type(v.x) == "number" then return v.x, v.y, v.z end return v:GetX(), v:GetY(), v:GetZ() end
local H = FnvHash64.new
local fo = Global.GetFieldObjectForHaxe()
]]
local function pla(code) return PLA_PRE .. code end

-- world.trace(from, to, fn): the first thing of the world's collision between the two points: fn(hit) a frame later -
-- hit = {pos, normal (Arceus), what (Arceus: the collision's id), dist} or nil (nothing there)
function world.trace(a, b, fn)
  a, b = vec(a), vec(b)
  if type(fn) ~= "function" then error("world.trace(from, to, fn(hit)): the answer comes a frame later", 2) end
  local code
  if ZA then
    code = fmt("local r = Commands.SED24081F313B33A9({%s}, {%s}) if r then return r[1] .. ' ' .. r[2] .. ' ' .. r[3] end return 'none'", xyz3(a), xyz3(b))
  else
    code = pla(fmt([[local cc = fo:__haxe_export_FindCharactorComponent(H("PlayerObject")):__haxe_export_GetCharacterController()
local r = RayResult.new()
if cc:GetFieldPhysics():CastRayToLayerMask(V(%s), V(%s), FieldPhysics.GetGroundLayersBitField(), r) then
  local x, y, z = XYZ(r.position) local nx, ny, nz = XYZ(r.normal)
  return x .. ' ' .. y .. ' ' .. z .. ' ' .. nx .. ' ' .. ny .. ' ' .. nz .. ' ' .. tostring(r.rawId)
end
return 'none']], xyz3(a), xyz3(b)))
  end
  return S(nil, code, function(r, err)
    local t = parse_nums(r)
    local hit = nil
    if type(t[1]) == "number" and type(t[3]) == "number" then
      local p = vec(t[1], t[2], t[3])
      hit = {pos = p, dist = a:dist(p), normal = type(t[6]) == "number" and vec(t[4], t[5], t[6]) or nil, what = t[7] and tostring(t[7]) or "world"}
    elseif err then rt.log("world.trace: " .. tostring(err)) end
    run({fn = fn}, "world.trace", hit)
  end)
end
-- world.ground(pos, fn): the ground's height under pos (a trace from 2 m above it to 50 m below): fn(y) or fn(nil)
function world.ground(p, fn)
  p = vec(p)
  return world.trace(p + vec(0, 2, 0), p - vec(0, 50, 0), function(hit) fn(hit and hit.pos.y or nil, hit) end)
end

-- ---- objects: effects (both games) and props (Z-A: the game's object templates)
local Obj = {}
Obj.__index = Obj
Obj.__tostring = function(o) return fmt("%s %s (%s)", o.kind, o.name, o.path) end
local objs, obj_n = {}, 0
local function obj_name(prefix) obj_n = obj_n + 1 return fmt("%s_%d", prefix, obj_n) end
-- world.effect{path =, pos =, rot = {pitch, yaw, roll} | yaw =, scale =, on = target (Arceus: a character's name), joint =}
-- Z-A: path under ik_effect/data/ without .ptcl ("field/item/kira/ef_item_kira", "field/weather/et001/et001_rain_wide_range");
-- Arceus: a .ptcl's path ("bin/effect/battle/particle/ew399/ew399_hit.ptcl"). Returns a handle: o:move(pos), o:rotate(p, y, r),
-- o:scale(s), o:remove(). The game's events may clear effects when they end (Z-A: RemoveAllTagEffect after each event).
function world.effect(t)
  if type(t) ~= "table" or type(t.path) ~= "string" then error("world.effect{path = ..., pos = {x, y, z}}", 2) end
  local pos = t.pos and vec(t.pos) or player.ahead(2)
  if not pos then error("world.effect: pos needed (no player yet)", 2) end
  local r = t.rot or {0, tonumber(t.yaw) or 0, 0}
  local o = setmetatable({kind = "effect", name = t.name or obj_name("luafx"), path = t.path, pos = pos, scale_ = tonumber(t.scale) or 1}, Obj)
  local code
  if ZA then
    code = fmt([[Commands.CreateEffect(%q, %q)
local n = 0 while not Commands.IsSetUpEffect(%q) and n < 600 do n = n + 1 Commands.Suspend() end
Commands.SB2937945F48A7BD5(%q, %s)
Commands.SAA880CEABEE19158(%q, %s, %s, %s)
Commands.SD3F56D7A1041E840(%q, %s)
Commands.PlayEffect(%q)]], o.path, o.name, o.name, o.name, xyz3(pos), o.name, num(r[1]), num(r[2]), num(r[3]), o.name, num(o.scale_), o.name)
  else
    code = pla(fmt([[fo:__haxe_export_PlayParticleVfx(%q, V(%s), V(%s, %s, %s), %s, %q, H(%q))
local n = 0 while not fo:__haxe_export_IsLoadedParticle(H(%q)) and n < 600 do n = n + 1 coroutine.yield() end
fo:__haxe_export_EmitParticle(H(%q))]], o.path, xyz3(pos), num(math.rad(r[1])), num(math.rad(r[2])), num(math.rad(r[3])),
      num(o.scale_), o.name, o.name, o.name, o.name))
  end
  RUN(nil, code)
  objs[o.name] = o
  return o
end
-- world.prop{model =, pos =, yaw =, anim = false} (Z-A): one of the game's object templates - a model with its look and
-- collision (world/ik_obj_template/...: "world/ik_obj_template/effect/field/gimmick/field_item303a"), or a .trsot by its
-- whole path (trsot =). Made hidden, shown once loaded. Props go away all together: world.props_clear() (and when a game
-- event ends: its end clears the event objects).
function world.prop(t)
  if not ZA then error("world.prop: Z-A only (Arceus: world.effect, pokemon.spawn, npc.spawn)", 2) end
  if type(t) ~= "table" or (type(t.model) ~= "string" and type(t.trsot) ~= "string") then error("world.prop{model = ..., pos = {x, y, z}, yaw = 0}", 2) end
  local pos = t.pos and vec(t.pos) or player.ahead(2)
  if not pos then error("world.prop: pos needed (no player yet)", 2) end
  local o = setmetatable({kind = "prop", name = t.name or obj_name("luaprop"), path = t.model or t.trsot, pos = pos, yaw = tonumber(t.yaw) or 0}, Obj)
  local code
  if t.trsot then
    o.object = o.name  -- (the café system's own name)
    code = fmt([[Commands.S2939C514FD3C987D(%q, %q, {%s}, %s)
local n = 0
while n < 900 do
  n = n + 1
  Commands.Update()
  if n > 2 and CF8BF2A0E1DDFCAEB.S3E916EB37B6A48C8 == 0 then break end
  Commands.Suspend()
end
Commands.SE7448E48A195BB70()
return n]], o.path, o.name, xyz3(pos), num(o.yaw))
  else
    o.object = o.name .. "_CreateObjectTemplate"
    code = fmt([[Commands.S40ACEF14A8745A74(%q, %q, {%s}, %s, %s)
local n = 0
while n < 900 do
  n = n + 1
  Commands.Update()
  if CE3A3A45F0BDA7426.S81BA08EE110AAE62() then break end
  Commands.Suspend()
end
Commands.S5AED1F1B5B2A7020()
return n]], o.path, o.name, xyz3(pos), num(o.yaw), t.anim and "true" or "false")
  end
  RUN(nil, code)
  objs[o.name] = o
  return o
end
function world.props_clear()
  if not ZA then return end
  S(nil, "Commands.S3E36C5EB9EFAF0F8() Commands.S729E9F8C776DCA95()")
  for k, o in pairs(objs) do if o.kind == "prop" then o.removed = true objs[k] = nil end end
end
function Obj:move(p)
  self.pos = vec(p)
  if ZA and self.kind == "effect" then S(nil, fmt("Commands.SB2937945F48A7BD5(%q, %s)", self.name, xyz3(self.pos)))
  elseif ZA then S(nil, fmt("Commands.FObj_Set_Pos_Ang(%q, %s, %s)", self.object, xyz3(self.pos), num(self.yaw or 0)))
  else S(nil, pla(fmt("fo:__haxe_export_SetPosParticle(H(%q), V(%s))", self.name, xyz3(self.pos)))) end
  return self
end
function Obj:rotate(p, y, r)
  if ZA and self.kind == "effect" then S(nil, fmt("Commands.SAA880CEABEE19158(%q, %s, %s, %s)", self.name, num(p or 0), num(y or 0), num(r or 0)))
  elseif ZA then self.yaw = y or p or 0 S(nil, fmt("Commands.FObj_Set_Ang(%q, %s)", self.object, num(self.yaw))) end
  return self
end
function Obj:scale(s) if ZA and self.kind == "effect" then S(nil, fmt("Commands.SD3F56D7A1041E840(%q, %s)", self.name, num(s))) end return self end
-- (Z-A props: the game's own physics on it - collision and gravity on / off, a drop onto the ground below)
function Obj:physics(on_) if ZA and self.kind == "prop" then S(nil, fmt("Commands.S10A25E1BCEA2D263(%q, %s)", self.object, on_ and "true" or "false")) end return self end
function Obj:fall(d) if ZA and self.kind == "prop" then RUN(nil, fmt("Commands.FObj_Falling(%q, %s) for i = 1, 120 do Commands.Update() Commands.Suspend() end", self.object, num(d or 5))) end return self end
function Obj:show(v) if ZA and self.kind == "prop" then S(nil, fmt("Commands.FObj_Set_Visibility(%q, %s)", self.object, v == false and "false" or "true")) end return self end
function Obj:remove()
  if self.removed then return end
  self.removed = true
  objs[self.name] = nil
  if self.kind == "effect" then
    if ZA then S(nil, fmt("Commands.RemoveEffect(%q)", self.name)) else S(nil, pla(fmt("fo:__haxe_export_KillParticle(H(%q))", self.name))) end
  else self:show(false) end  -- (Z-A props: hidden - the game removes props only all together, world.props_clear)
end
function world.objects() local t = {} for _, o in pairs(objs) do t[#t + 1] = o end return t end

-- ---- the camera: a look at a spot, a path of shots, back to the game's
-- camera.look(target, opts): the event camera put at opts.from (default: 4 m behind the player, 2 m up) looking at target
-- over opts.time seconds (0: at once), opts.fov degrees (50). camera.path({{from =, at =, time =}, ...}): each shot in
-- turn (fn() at the end). camera.reset([seconds]): the game's own camera back, eased.
local function cam_from(opts)
  if opts.from then return vec(opts.from) end
  local p, f = player.pos(), player.forward()
  if not p then error("camera: from = {x, y, z} needed (no player yet)", 3) end
  return p - f * 4 + vec(0, 2.2, 0)
end
local function shot_code(from, at, time, fov)
  if ZA then
    -- (its angles: pitch up from level, yaw 180 off the way it looks - checked 2026-11-19, za10-cam-front)
    local d = at - from
    local pitch, yaw = math.deg(math.atan(d.y, math.sqrt(d.x * d.x + d.z * d.z))), math.deg(math.atan(d.x, d.z)) + 180
    return fmt("Commands.Camera_Move_Angle(%s, {%s}, {%s, %s, 0}, %s, 0, 3)\nCommands.Wait_Camera()", num(time), xyz3(from), num(pitch), num(yaw), num(fov))
  end
  local frames = math.max(1, math.floor(time * 30 + 0.5))
  return fmt([[local cm = Global.GetCameraManager()
if not cm:IsEventCameraEnable() then cm:EventCameraStart() end
local p = CameraInterpolationParameter.new()
p.start = cm:GetEventCameraParameter()
p["end"] = cm:GetEventCameraParameter()
p["end"].cameraPos:Set(%s) p["end"].lookAtPos:Set(%s) p["end"].fovy = math.rad(%s) p["end"].roll = 0
p.type = 2 p.addRate = %s p.rate = %s p.frame = 0
cm:EventCameraMove(p)
local n = 0 while cm:IsEventCameraMoving() and n < 3000 do n = n + 1 coroutine.yield() end]], xyz3(from), xyz3(at), num(fov),
    num(1 / frames), time <= 0 and "1" or "0")
end
function camera.look(at, opts)
  opts = opts or {}
  local target = at
  if type(at) == "table" and getmetatable(at) and at.pos and type(at.pos) == "function" then target = at:pos() end
  if at == "player" then target = player.pos() + vec(0, 1.4, 0) end
  if not target then error("camera.look(target): a spot, \"player\" or a character", 2) end
  return RUN(nil, shot_code(cam_from(opts), vec(target), tonumber(opts.time) or 1, tonumber(opts.fov) or 50), opts.done)
end
function camera.path(shots, done)
  if type(shots) ~= "table" or #shots == 0 then error("camera.path({{from =, at =, time =}, ...})", 2) end
  local parts = {}
  for i, s in ipairs(shots) do parts[i] = shot_code(vec(s.from), vec(s.at), tonumber(s.time) or 2, tonumber(s.fov) or 50) end
  return RUN(nil, table.concat(parts, "\n"), done)
end
function camera.reset(sec)
  sec = tonumber(sec) or 0.5
  if ZA then
    return RUN(nil, fmt([[Commands.S060E4139F53705A4(%s)
local n = 0 while not c56DE81E7.f7C10447B() and n < 600 do n = n + 1 Commands.Suspend() end
Commands.S87C02A63ACAD9232(false)
Commands.SC818B7D84F7486E2(true)]], num(sec)))
  end
  return S(nil, fmt("local cm = Global.GetCameraManager() if cm:IsEventCameraEnable() then cm:EventCameraEnd(0, %d, 0, 0) end", math.max(0, math.floor(sec * 30))))
end

-- ---- the player's animation, facing, rides
-- player.anim(n): one of the game's player animations by number (Z-A: FObj_Set_Animation's - 1 standing, 74 receiving,
-- 174 talking; Arceus: the player animation controller's Action(n) - 16 yes, 17 no, 12 passing an item);
-- player.anim_loop(n, on) (Arceus: LoopAction - 9 looking at the phone), player.pose(n, on) (Arceus: PhotoPoseAction 1-15),
-- player.face_expression(name) (Z-A: "smile", "serious", "sad"...)
function player.anim(n)
  n = math.floor(n)
  if ZA then return S(nil, fmt("Commands.FObj_Set_Animation(\"Player\", %d)", n)) end
  return S(nil, fmt("Global.GetPlayerComponent():__haxe_export_GetPlayerAnimationController():Action(%d)", n))
end
function player.anim_loop(n, on_)
  if ZA then return S(nil, on_ == false and "Commands.FObj_Reset_AnimationAll(\"Player\")" or fmt("Commands.FObj_Set_Animation(\"Player\", %d)", math.floor(n))) end
  return S(nil, fmt("Global.GetPlayerComponent():__haxe_export_GetPlayerAnimationController():LoopAction(%d, %s)", math.floor(n), on_ == false and "false" or "true"))
end
function player.pose(n, on_)
  if ZA then return player.anim_loop(n, on_) end
  return S(nil, fmt("Global.GetPlayerComponent():__haxe_export_GetPlayerAnimationController():PhotoPoseAction(%d, %s)", math.floor(n), on_ == false and "false" or "true"))
end
function player.face_expression(name) if ZA then return S(nil, fmt("Commands.S09650F7513B02189(\"Player\", %q)", tostring(name))) end end
-- player.dismount() (Arceus: off the ride Pokémon, as its events do), player.riding(fn(yes)). (Mounting: the one script
-- path, the ride manager's StartOdoshishiRide - Wyrdeer - hung the game from the field script's state, 2026-11-19: left out)
function player.dismount() if not ZA then return S(nil, "local r = Global.GetRideManager() if r:IsRiding() then r:StopRide() end") end end
function player.riding(fn) if ZA then fn(false) return end return S(nil, "return Global.GetRideManager():IsRiding()", function(r) fn(r == "true") end) end

-- ---- a wild battle started by a script (Arceus: one of the game's event encounters - bin/pokemon/data/poke_event_encount.bin:
-- "aipom01" Aipom lv 30, "yukimenoko01" Froslass lv 55...; as its events start theirs)
function battle.wild(enc, opts)
  if ZA then return false, "Z-A's battles start from its C++ triggers, not its scripts" end
  opts = opts or {}
  local p = opts.pos and vec(opts.pos) or player.pos()
  if not p then return false, "no player yet" end
  local args = {tostring(enc), opts.field or "ee350", num(p.x), num(p.y), num(p.z), num(opts.yaw or 0), tostring(opts.a or "1"), "none", "TRUE", "TRUE", tostring(opts.mode or 0)}
  for i, a in ipairs(args) do args[i] = fmt("%q", a) end
  S(nil, fmt("AppCommand.ExecuteEventCommand(FnvHash64.new(\"call_wild_battle\"), FnvHash64.new(\"\"), %s)", table.concat(args, ", ")))
  return true
end

-- ---------------------------------------------------------------- the weather
local weather = {}
-- weather.get(fn): fn(id, name) - the weather the game says it has now (Z-A: its weather getter, 0 clear, 2 rain...;
-- Arceus: nil - its scripts have no weather read). weather.is(name, fn): Z-A's own check ("rain", "storm"): fn(true/false).
-- weather.set(id): the runtime's (worldapi weather_set) - false where it isn't found yet (both games, 2026-11-19: Z-A's
-- index changes what it reports, not the sky; Arceus has no setter in its scripts). A rain look a script can have:
-- world.effect{path = "field/weather/et001/et001_rain_wide_range"} (Z-A)
function weather.get(fn)
  if type(fn) ~= "function" then error("weather.get(fn(id)): the answer comes a frame later", 2) end
  local w = wt.have and wt.weather() or -1
  if w >= 0 or not ZA then run({fn = fn}, "weather.get", w >= 0 and w or nil, w >= 0 and wt.weather_name(w) or nil) return end
  S(nil, "return c4B6212B8.f6362F6BB()", function(r) run({fn = fn}, "weather.get", tonumber(r), nil) end)
end
function weather.is(name, fn)
  if not ZA then run({fn = fn}, "weather.is", nil) return end
  S(nil, fmt("return Commands.IsCheckWeather(%q)", tostring(name)), function(r) run({fn = fn}, "weather.is", r == "true") end)
end
function weather.set(w)
  need("weather.set")
  if type(w) ~= "number" then return false end
  return wt.weather(math.floor(w))
end
function weather.reset() if wt.have then return wt.weather(-1) end end

-- ---------------------------------------------------------------- into each mod's globals, the frame, a reload
local base_api_for = P.api_for()
local function world_for(mod, env)
  env.net = {
    send = function(name, data, to) return net_send(mod, name, data, to) end,
    receive = function(name, fn) return net_receive(mod, name, fn) end,
  }
  env.players = players
  env.chat = chat
  env.party = party
  env.box = box
  env.battle = battle
  env.world = world
  env.camera = camera
  env.weather = weather
  return env
end
P.api_for(function(mod, env) base_api_for(mod, env) return world_for(mod, env) end)
world_for(P.shared, G)

local base_tick = G.__tick
local wframe = 0
G.__tick = function(frame)
  base_tick(frame)
  if not wt.have then return end
  wframe = wframe + 1
  net_poll()
  chat_poll()
  if wframe % 4 == 0 then battle_poll() end
  if wframe % 30 == 0 then party_poll() end
end
local base_unload = G.__unload
G.__unload = function()
  base_unload()
  if wt.have then  -- (a reload lets the player go: what the old scripts held ends with them)
    if held.buttons ~= 0 or held.sticks ~= 0 then wt.input_hold(0, 0) end
    if walking then wt.input_stick(false, 0, 0) end
    wt.speed(1)
    if not ZA and speed_k ~= 1 then P.script(nil, fmt(PLA_SPEED, "1")) end
  end
end

-- ---------------------------------------------------------------- console commands
P.cmd_add(P.shared, "chat", function(...)
  local t = {...}
  for i = 1, #t do t[i] = tostring(t[i]) end
  local line = table.concat(t, " ")
  print(chat.say(line) and ("said: " .. line) or "not sent (no shared world, or more than one line a second)")
end, "chat <text>: a chat line to the other players (over your head)")
P.cmd_add(P.shared, "players", function()
  print(fmt("me %s, shared world %s", tostring(players.me()), players.online() and "on" or "off"))
  for _, p in ipairs(players.list()) do
    print(fmt("%s at %s%s%s", p.id, tostring(p.pos), p.standin and " (stand-in up)" or "", p.dist and fmt(", %.1f m", p.dist) or ""))
  end
end, "the other players heard")
P.cmd_add(P.shared, "party", function()
  for i = 1, 6 do local m = party.get(i) if m then print(i .. ": " .. tostring(m) .. (m.shiny and " shiny" or "")) end end
end, "the party")
P.cmd_add(P.shared, "box", function(b)
  b = b or 1
  for s = 1, 30 do local m = box.get(b, s) if m then print(fmt("%d/%d: %s%s", b, s, tostring(m), m.shiny and " shiny" or "")) end end
end, "box [n]: what box n holds")
