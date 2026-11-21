-- Legends Lua: quests (lib/quest.lua - docs/lua-api.md "Quests"). Built into the plugin and run once after the rest of
-- the API: every mod gets `quest` (and include("lib/quest.lua") returns it when the mod has no file of that name).
-- A quest is a table - a giver, lines, steps from templates, rewards, prerequisites - that quest.define() runs: the
-- giver made in the world, its progress kept per save (storage), a tracker panel with the objective and a marker over
-- where to go, a journal (J / "quests" in the console), events quest_started / quest_step / quest_done. The same in
-- both games; the few steps one game can't do say so. Legends Studio's Quest Builder writes these tables.
local rt = ...
local G = _G
rt = rt or G.__api.rt
local A = G.__api
local fmt, floor, min, max, abs, sqrt = string.format, math.floor, math.min, math.max, math.abs, math.sqrt
local vec, player, emit, now = A.vec, A.player, A.emit, A.now
local ZA = G.game.name == "za"

local quest = {}          -- (the shared table; each mod gets one bound to it: quest_for)
local list = {}           -- (every quest defined, in order)
local by_key = {}         -- ("<mod>/<id>" -> quest)
local busy_until = 0      -- (a dialogue, a window or a battle start going on: talks wait)
local busy = false
local tracked             -- (the quest the tracker shows)
local show_tracker = true
local draw_id, poll_id, talk_tick_id
local function every(sec, fn) return G.timer.every(sec, fn) end

local C = {panel = 0x101420d8, edge = 0xeec25499, title = 0xeec254ff, text = 0xeef0f5ff, dim = 0xaab1c3ff,
           mark = 0xeec254ff, mark_dark = 0x1a1d27ff, done = 0x6fd27aff, spot = 0x7fd8ffff}

-- ---------------------------------------------------------------- small helpers
local function lines_of(v)  -- (a line or a list of them -> a list)
  if v == nil then return {} end
  if type(v) == "string" then return v == "" and {} or {v} end
  local t = {}
  for _, l in ipairs(v) do if tostring(l) ~= "" then t[#t + 1] = tostring(l) end end
  return t
end
local function wrap(text, width)  -- (words to lines of at most width characters - UTF-8 counted as bytes, close enough)
  local out, line = {}, ""
  for para in (text .. "\n"):gmatch("([^\n]*)\n") do
    for w in para:gmatch("%S+") do
      if line == "" then line = w elseif #line + 1 + #w <= width then line = line .. " " .. w else out[#out + 1] = line line = w end
    end
    out[#out + 1] = line
    line = ""
  end
  while #out > 0 and out[#out] == "" do out[#out] = nil end
  return out
end
local function copy(t) if type(t) ~= "table" then return t end local o = {} for k, v in pairs(t) do o[k] = copy(v) end return o end
local function area_matches(want)  -- (Arceus: a quest's area, "ha_area01" / "area01", against the one the player is in)
  if not want or want == "" or ZA then return true end
  local _, name = rt.area()
  if not name or name == "" then return false end
  local a = tostring(want):match("(area%d%d)") or want
  return name:find(a, 1, true) ~= nil
end
local function tmp(q) q.tmp = q.tmp or {} return q.tmp end  -- (a step's passing values: not kept)
local function report(q, what, err) A.report(q.mod, "quest " .. q.id .. " " .. what, err) end
local function safe(q, what, fn, ...)
  local ok, e = pcall(fn, ...)
  if not ok then report(q, what, e) end
  return ok, e
end

-- ---------------------------------------------------------------- what a quest keeps (per save: its mod's storage)
local function rec(q)
  local r = q.storage.get("quest." .. q.id)
  if type(r) ~= "table" then r = {s = "none"} end
  return r
end
local function put(q, r) q.storage.set("quest." .. q.id, r) q.cache = r end
local function state(q) if not q.cache then q.cache = rec(q) end return q.cache end
function quest.state(q) return state(q).s end

-- ---------------------------------------------------------------- talking: Z-A's own window, Arceus an overlay box
local talk = nil  -- {name, lines, i, fn} while the overlay box is up
local talk_input
local function said_by(name, text) return (name and name ~= "" and (name .. ":\n") or "") .. text end
local function overlay_say(name, lines_, fn)
  talk = {name = name, lines = lines_, i = 1, fn = fn, armed = false}
  if G.player.freeze then pcall(G.player.freeze, true, {camera = true}) end
  if not talk_tick_id then talk_tick_id = G.on("tick", function() talk_input() end) end  -- (A read every frame while it's up)
end
-- Z-A: the game's own message window, every page of a talk in one coroutine of the game's (its text in the runtime's
-- reserved labels lza_say_8..15; ui.message keeps to 0..7); game.run's callback comes once A closed the last page
local ZA_TALK = [[local f = CF74EC46ADA8DC783.S3567CE4CD45AC7D3("script/fld_item")
CF74EC46ADA8DC783.S092ACF6F9CAC2BA2(f)
while not CF74EC46ADA8DC783.S8EE12659B6952775(f) do Commands.Suspend() end
%s
Commands.HideMessageWindow()
CF74EC46ADA8DC783.S34645180A2F21B29("script/fld_item")]]
local za_slot = 0
local function za_talk(q, pages, fn)
  local shows = {}
  for i, page in ipairs(pages) do
    if i > 8 then break end
    local slot = 8 + za_slot
    za_slot = (za_slot + 1) % 8
    rt.message_text(slot, page)
    shows[#shows + 1] = fmt("Commands.ShowMessageWindow(2, \"lza_say_%d\", \"fld_item\")", slot)
  end
  if not q.env.game.run(fmt(ZA_TALK, table.concat(shows, "\n")), function() fn() end) then fn() end
end
-- say(q, who, lines, fn): each line in turn (A after each), then fn()
local function say(q, who, lines_, fn)
  lines_ = lines_of(lines_)
  local name = type(who) == "table" and who.name or who
  if #lines_ == 0 then if fn then fn() end return end
  busy = true
  local function finish() busy = false busy_until = now() + 0.7 if fn then safe(q, "after a line", fn) end end
  if ZA and q.def.dialogue ~= "overlay" then
    local pages = {}
    for _, l in ipairs(lines_) do  -- (the window shows two lines: the name and the first, then two at a time)
      local w = wrap(l, 42)
      pages[#pages + 1] = said_by(name, w[1] or "")
      for i = 2, #w, 2 do pages[#pages + 1] = w[i] .. (w[i + 1] and ("\n" .. w[i + 1]) or "") end
    end
    za_talk(q, pages, finish)
  else
    overlay_say(name, lines_, finish)
  end
end
local function talk_draw(x, y, w, h, s)
  if not talk then return end
  local bw, bh = 1100 * s, 210 * s
  local bx, by = x + (w - bw) / 2, y + h - bh - 70 * s
  G.draw.rect(bx, by, bw, bh, 0x0f1219f0, 18 * s)
  G.draw.rect_outline(bx, by, bw, bh, 0xeec25488, 2 * s, 18 * s)
  if talk.name and talk.name ~= "" then
    local tw = G.draw.text_size(talk.name, 30 * s)
    G.draw.rect(bx + 30 * s, by - 26 * s, tw + 40 * s, 48 * s, 0xeec254ff, 12 * s)
    G.draw.text(talk.name, bx + 50 * s, by - 2 * s, {size = 30 * s, color = 0x1a1d27ff, valign = "middle"})
  end
  G.draw.text(talk.lines[talk.i] or "", bx + 44 * s, by + 46 * s, {size = 34 * s, color = C.text, wrap = bw - 88 * s})
  local blink = floor(now() * 2) % 2 == 0
  G.draw.text(blink and "A  \u{25BC}" or "A", bx + bw - 40 * s, by + bh - 26 * s, {size = 26 * s, color = C.title, align = "right", valign = "middle"})
end
talk_input = function()  -- (A: the next line - armed once A was let go, so the A that talked doesn't skip one)
  if not talk then if talk_tick_id then G.off(talk_tick_id) talk_tick_id = nil end return end
  local a = player.button("A")
  if not talk.armed then if not a then talk.armed = true end return end
  if a then
    talk.armed = false
    talk.i = talk.i + 1
    if talk.i > #talk.lines then
      local fn = talk.fn
      talk = nil
      if talk_tick_id then G.off(talk_tick_id) talk_tick_id = nil end
      if G.player.freeze then pcall(G.player.freeze, false) end
      if fn then fn() end
    end
  end
end

-- ---------------------------------------------------------------- the characters a quest makes
local function npc_spec(q, spec)  -- (a step's npc: "giver", a name of one of the quest's own, a table, or nil)
  if spec == nil or spec == "giver" then return q.giver_spec end
  if type(spec) == "string" then
    for _, s in ipairs(q.npc_specs) do if s.name == spec then return s end end
    return {name = spec, existing = true}
  end
  return spec
end
local function npc_of(q, spec)  -- (its handle, made when wanted)
  spec = npc_spec(q, spec)
  if not spec then return nil end
  local h = q.npcs[spec.name]
  if h and h:valid() then return h end
  if spec.existing then  -- (a character another script made: Studio's townspeople, ...)
    h = G.npc.find(spec.name)
    if h then q.npcs[spec.name] = h q.hooked = q.hooked or {} if not q.hooked[h] then q.hooked[h] = true h:on("talk", function(n) q:talked(n) end) end end
    return h
  end
  if not spec.pos or not area_matches(spec.area or q.def.area) then return nil end
  local ok, n = pcall(q.env.npc.spawn, {pos = spec.pos, yaw = spec.yaw, name = spec.name, model = spec.model, outfit = spec.outfit, body = spec.body})
  if not ok then report(q, "npc " .. tostring(spec.name), n) return nil end
  q.npcs[spec.name] = n
  n.data.quest = q
  n:on("talk", function(n_) q:talked(n_) end)
  if spec.route and #spec.route > 1 then n:walk_route(spec.route, {loop = true, wait = 2}) end
  return n
end
local function npc_pos(q, spec)  -- (where it is - or will be)
  spec = npc_spec(q, spec)
  if not spec then return nil end
  local h = q.npcs[spec.name]
  local p = h and h:valid() and h:pos()
  if p then return p end
  return spec.pos and vec(spec.pos) or nil
end

-- ---------------------------------------------------------------- the steps
-- each kind: text(q, st, r) the objective's default, start(q, st, r), poll(q, st, r) -> true when done, talk(q, st, r, n)
-- (a character of its was talked to) -> handled, target(q, st, r) -> where the marker goes
local K = {}
local function target_is(q, st, n) local s = npc_spec(q, st.npc) return s and n.name == s.name end
K.talk = {
  text = function(q, st) local s = npc_spec(q, st.npc) return "Talk to " .. (s and s.name or "them") end,
  talk = function(q, st, r, n) if not target_is(q, st, n) then return false end n:face_player() say(q, n, st.lines, function() q:advance() end) return true end,
  target = function(q, st) return npc_pos(q, st.npc) end,
}
K.go = {
  text = function(q, st) return st.place and ("Go to " .. st.place) or "Go to the marked place" end,
  poll = function(q, st)
    local p = player.pos()
    if not p or not st.pos then return false end
    local t = vec(st.pos)
    return p:dist2d(t) <= (st.radius or 5) and abs(p.y - t.y) <= max(6, st.radius or 5)
  end,
  target = function(q, st) return st.pos and vec(st.pos) end,
}
K.collect = {
  text = function(q, st) return "Find " .. (st.what or "the items") end,
  progress = function(q, st, r) local n = 0 for i = 1, #(st.spots or {}) do if r.got and r.got[i] then n = n + 1 end end return n, #(st.spots or {}) end,
  poll = function(q, st, r)
    local p = player.pos()
    if not p then return false end
    r.got = r.got or {}
    local all = true
    for i, s in ipairs(st.spots or {}) do
      if not r.got[i] then
        local t = vec(s)
        if p:dist2d(t) <= (st.radius or 1.6) and abs(p.y - t.y) < 4 then
          r.got[i] = true
          put(q, r)
          local n, of = K.collect.progress(q, st, r)
          G.ui.notify(fmt("Found %s (%d/%d)", st.one or "one", n, of), 3)
          emit("quest_progress", q.id, n, of)
        else all = false end
      end
    end
    return all
  end,
  target = function(q, st, r)  -- (the nearest left)
    local p = player.pos()
    local best, bd
    for i, s in ipairs(st.spots or {}) do
      if not (r.got and r.got[i]) then local d = p and p:dist(vec(s)) or i if not bd or d < bd then best, bd = vec(s), d end end
    end
    return best
  end,
}
-- catch: more of a species in the party and the boxes than when the step began (counted a box at a time)
local scan = {box = 0, counts = {}, party = {}, cycle = 0, want = {}}
local function owned(species)
  local n = scan.party[species] or 0
  for _, c in pairs(scan.counts) do n = n + (c[species] or 0) end
  return n
end
local function scan_tick()
  if not next(scan.want) or not G.box or not G.party then return end
  local ok = pcall(function()
    G.party.ready()
    local pc = {}
    for _, m in ipairs(G.party.list()) do if m.species and scan.want[m.species] then pc[m.species] = (pc[m.species] or 0) + 1 end end
    scan.party = pc
    scan.box = scan.box % 32 + 1
    local c = {}
    for _, m in ipairs(G.box.list(scan.box)) do if m.species and scan.want[m.species] then c[m.species] = (c[m.species] or 0) + 1 end end
    scan.counts[scan.box] = c
    if scan.box == 32 then scan.cycle = scan.cycle + 1 end
  end)
  if not ok then scan.want = {} end  -- (no party / boxes in this build: the step can't be counted)
end
K.catch = {
  text = function(q, st) return fmt("Catch %s%s", (st.count or 1) > 1 and ((st.count or 1) .. " x ") or "", st.what or ("#" .. tostring(st.species))) end,
  start = function(q, st, r) r.base = nil tmp(q).since = scan.cycle end,
  progress = function(q, st, r) if not r.base then return 0, st.count or 1 end return min(st.count or 1, max(0, owned(st.species) - r.base)), st.count or 1 end,
  poll = function(q, st, r)
    scan.want[st.species] = true
    local k = tmp(q)
    if not k.since then k.since = scan.cycle end
    if not r.base then  -- (a whole round of the boxes first: what the player had)
      if scan.cycle >= k.since + 1 then r.base = owned(st.species) put(q, r) end
      return false
    end
    if scan.cycle < k.since + 1 then return false end  -- (after a reload: counted again first)
    local n, of = K.catch.progress(q, st, r)
    if n ~= (k.shown or 0) then k.shown = n if n > 0 and n < of then G.ui.notify(fmt("%s (%d/%d)", q.def.title, n, of), 3) end end
    return n >= of
  end,
}
local function party_has(species)
  if not G.party then return false end
  local ok, l = pcall(G.party.list)
  if not ok then return false end
  for _, m in ipairs(l) do if m.species == species then return true end end
  return false
end
K.show = {
  text = function(q, st) local s = npc_spec(q, st.npc) return fmt("Show %s %s", s and s.name or "them", st.what or ("#" .. tostring(st.species))) end,
  start = function() if G.party then pcall(G.party.refresh) end end,
  poll = function() if G.party then pcall(G.party.ready) end return false end,  -- (the party known by the time they're talked to)
  talk = function(q, st, r, n)
    if not target_is(q, st, n) then return false end
    n:face_player()
    if party_has(st.species) then say(q, n, st.lines, function() q:advance() end)
    else say(q, n, st.missing or ("Come back with " .. (st.what or "it") .. " in your party.")) end
    return true
  end,
  target = function(q, st) return npc_pos(q, st.npc) end,
}
local GET_ITEM = "return Global.GetSaveSystem():Bag():GetItemNum(%d)"
local SUB_ITEM = "Global.GetSaveSystem():Bag():SubItem(%d, %d, -1) return Global.GetSaveSystem():Bag():GetItemNum(%d)"
K.item = {
  text = function(q, st) local s = npc_spec(q, st.npc) return fmt("Bring %s %d %s", s and s.name or "them", st.count or 1, st.what or ("of item " .. tostring(st.item))) end,
  talk = function(q, st, r, n)
    if not target_is(q, st, n) then return false end
    n:face_player()
    if ZA then  -- (Z-A: no reader of the bag's counts found - the step can't check; it takes the player's word)
      say(q, n, st.lines, function() q:advance() end)
      return true
    end
    busy = true
    q.env.game.script(fmt(GET_ITEM, st.item), function(res)
      busy = false
      local have = tonumber(res) or 0
      if have >= (st.count or 1) then
        if st.take ~= false then q.env.game.script(fmt(SUB_ITEM, st.item, st.count or 1, st.item)) end
        say(q, n, st.lines, function() q:advance() end)
      else
        say(q, n, st.missing or fmt("You have %d. Bring me %d.", have, st.count or 1))
      end
    end)
    return true
  end,
  target = function(q, st) return npc_pos(q, st.npc) end,
}
K.battle = {
  text = function(q, st) local s = npc_spec(q, st.npc) return st.what and ("Defeat " .. st.what) or ("Battle with " .. (s and s.name or "them")) end,
  talk = function(q, st, r, n)
    if not target_is(q, st, n) then return false end
    n:face_player()
    say(q, n, st.lines, function()
      if not (G.battle and G.battle.wild and G.battle.wild(st.encounter, {})) then
        report(q, "battle", "battle.wild(\"" .. tostring(st.encounter) .. "\") didn't start (Arceus only)")
        return
      end
      r.fighting = true
      put(q, r)
      busy = true
      busy_until = now() + 5
    end)
    return true
  end,
  battle_end = function(q, st, r, res)
    if not r.fighting then return end
    r.fighting = nil
    busy = false
    if res and res.result == "lost" then put(q, r) G.ui.notify("Try again: talk to them when you're ready", 4) return end
    q:advance()
  end,
  target = function(q, st) return npc_pos(q, st.npc) end,
}
K.time = {
  text = function(q, st) return st.when == "day" and "Wait until it is day" or "Wait until night" end,
  poll = function(q, st, r)
    if not ZA then return true end  -- (Arceus: no clock reader yet - the step passes; Studio doesn't offer it there)
    local t = tmp(q)
    if t.asking or (t.asked and now() - t.asked < 2) then return t.is_time end
    t.asking, t.asked = true, now()
    q.env.game.script("return Commands.IsDayTime()", function(res)
      t.asking = false
      if res == nil then return end
      t.is_time = (st.when == "day") == (tostring(res) == "true")
    end)
    return t.is_time
  end,
}
K.wait = {  -- (seconds of play: counted down while the game runs, kept with the rest)
  text = function(q, st) return st.text or "Wait a while" end,
  start = function(q, st, r) r.left = st.seconds or 60 end,
  poll = function(q, st, r)
    local t, k = now(), tmp(q)
    local dt = k.at and min(t - k.at, 1) or 0
    k.at = t
    r.left = (r.left or st.seconds or 60) - dt
    if floor(r.left) % 5 == 0 and floor(r.left) ~= k.saved then k.saved = floor(r.left) put(q, r) end
    return r.left <= 0
  end,
}
K.custom = {text = function(q, st) return st.text or "..." end}  -- (done by quest.advance(id) from the mod's own code)

-- ---------------------------------------------------------------- a quest
local Q = {}
Q.__index = Q
local function step_of(q, r) return q.def.steps[r.step or 1] end
function Q:objective()
  local r = state(self)
  if r.s == "none" then return self.def.summary or self.def.description or "" end
  if r.s == "ready" then return self.def.return_text or ("Return to " .. (self.giver_spec and self.giver_spec.name or "the client")) end
  if r.s == "done" then return "Complete" end
  local st = step_of(self, r)
  if not st then return "" end
  local k = K[st.kind] or K.custom
  local text = st.text or (k.text and k.text(self, st, r)) or ""
  if k.progress then local n, of = k.progress(self, st, r) if of and of > 1 then text = fmt("%s (%d/%d)", text, n, of) end end
  return text
end
function Q:target()  -- (where the marker goes, and whether it's a character's head)
  local r = state(self)
  if r.s == "ready" then return npc_pos(self, "giver"), true end
  if r.s ~= "active" then return nil end
  local st = step_of(self, r)
  local k = st and K[st.kind]
  local p = k and k.target and k.target(self, st, r) or nil
  return p, st.kind ~= "go" and st.kind ~= "collect"
end
function Q:available()
  local req = self.def.requires or {}
  if req.story then
    local v = G.game.work(ZA and "SYS_WORK_PHASE" or "WPHA_STORY")
    if v and v < req.story then return false end
  end
  for _, id in ipairs(req.quests or {}) do
    local o = by_key[id] or by_key[self.mod.name .. "/" .. id]
    if not o or state(o).s ~= "done" then return false end
  end
  return true
end
local NATIVE = "Global.GetEventScriptManager():GetEventProgressManager():GetQuestListManager():GetQuestList():SetProgress(FnvHash64.new(%q), %d)"
local function native(q, v)  -- (Arceus: the game's own request list entry Studio made for it shows this progress)
  if ZA or not q.def.native then return end
  q.env.game.script(fmt(NATIVE, q.def.native, v), function(_, err) if err then report(q, "native request", err) end end)
end
local function begin_step(q, r)
  local st = step_of(q, r)
  if not st then return end
  local k = K[st.kind]
  if not k then report(q, "step", "no step kind \"" .. tostring(st.kind) .. "\"") end
  if k and k.start then safe(q, "step start", k.start, q, st, r) end
  for _, s in ipairs(q.npc_specs) do npc_of(q, s) end
  put(q, r)
end
function Q:start()
  local r = {s = "active", step = 1}
  self.cache, self.tmp = r, nil
  tracked = self
  begin_step(self, r)
  native(self, 10)
  G.ui.notify("New quest: " .. self.def.title, 5)
  emit("quest_started", self.id, self)
  if #self.def.steps == 0 then self:advance() end
end
local function give_rewards(q)
  local rw = q.def.rewards or {}
  for _, it in ipairs(rw.items or {}) do
    local ok, e = pcall(G.item.give, it.id, it.n or 1, ZA)
    if not ok then report(q, "reward item", e) elseif not ZA then G.ui.notify(fmt("Got %s x%d", it.name or ("item " .. it.id), it.n or 1), 4) end
  end
  local pokes = rw.pokemon or (rw.species and {rw}) or {}
  if #pokes > 0 and G.party then  -- (made from one of the player's own: the party asked for first)
    local function give(tries)
      for _, p in ipairs(pokes) do
        local ok, b, why = pcall(G.pokemon.give, {species = p.species, level = p.level or 5, form = p.form, shiny = p.shiny, name = p.name})
        if ok and b then G.ui.notify(fmt("%s was sent to box %d", p.name or ("Pokémon #" .. p.species), b), 5)
        elseif ok and tries > 0 and tostring(why):find("yet") then G.timer.after(1, function() give(tries - 1) end) return
        else report(q, "reward Pokémon", ok and why or b) end
      end
    end
    pcall(G.party.refresh, function() give(5) end)
  end
  if rw.money and rw.money > 0 then
    if ZA then report(q, "reward money", "Z-A: no money command found - left out")
    else q.env.game.script(fmt("Global.GetSaveSystem():Misc():AddGold(%d)", floor(rw.money))) G.ui.notify(fmt("Got %d money", floor(rw.money)), 4) end
  end
end
function Q:finish()
  local r = {s = "done"}
  put(self, r)
  give_rewards(self)
  native(self, 255)
  G.ui.notify("Quest complete: " .. self.def.title, 6)
  emit("quest_done", self.id, self)
  if tracked == self then tracked = nil end
  for name, n in pairs(self.npcs) do  -- (the step characters go; the giver stays unless it asked to go too)
    if n.data and n.data.quest == self and name ~= self.giver_spec.name and not (npc_spec(self, name) or {}).stay then n:remove() self.npcs[name] = nil end
  end
end
function Q:advance()
  local r = state(self)
  if r.s ~= "active" then return end
  local st = step_of(self, r)
  if st and st.kind ~= "talk" and st.kind ~= "show" and st.kind ~= "item" and st.kind ~= "battle" and #lines_of(st.lines) > 0 then
    for _, l in ipairs(lines_of(st.lines)) do G.ui.notify(l, 5) end
  end
  emit("quest_step", self.id, r.step, self)
  r.step = (r.step or 1) + 1
  r.got, r.base, r.fighting, r.left = nil, nil, nil, nil
  self.tmp = nil
  tracked = self
  if r.step > #self.def.steps then
    if self.def.turn_in == false then self:finish() return end
    r.s = "ready"
    put(self, r)
    G.ui.notify(self.def.title .. ": " .. self:objective(), 5)
    return
  end
  begin_step(self, r)
  G.ui.notify(self.def.title .. ": " .. self:objective(), 5)
end
function Q:talked(n)
  if busy or talk or now() < busy_until then return end
  local r = state(self)
  local giver = self.giver_spec and n.name == self.giver_spec.name
  if r.s == "active" then
    local st = step_of(self, r)
    local k = st and K[st.kind]
    if k and k.talk then
      local ok, handled = safe(self, "talk", k.talk, self, st, r, n)
      if ok and handled then return end
    end
    if giver then n:face_player() say(self, n, self.def.lines.progress or "How is it going?") end
    return
  end
  if not giver then return end
  n:face_player()
  if r.s == "done" then say(self, n, self.def.lines.after or self.def.lines.done) return end
  if r.s == "ready" then
    say(self, n, self.def.lines.done or "Thank you!", function() self:finish() end)
    return
  end
  -- not started: offered (if it can be)
  if not self:available() then say(self, n, self.def.lines.locked) return end
  say(self, n, self.def.lines.offer or self.def.description, function() self:offer() end)
end
-- the offer: the quest's window (Accept / Not now), opened once A is let go
function Q:offer()
  if self.def.auto_accept then self:start() say(self, self.giver_spec, self.def.lines.accept) return end
  busy = true
  local q = self
  local id
  id = every(0.05, function()
    if player.button("A") then return end
    G.timer.cancel(id)
    local w = G.gui.window{title = q.def.title, w = 640, on_close = function() if busy then busy = false busy_until = now() + 0.7 end end}
    if q.def.description and q.def.description ~= "" then for _, l in ipairs(wrap(q.def.description, 58)) do w:label(l) end w:separator() end
    w:label("First: " .. ((q.def.steps[1] and (q.def.steps[1].text or (K[q.def.steps[1].kind] or K.custom).text(q, q.def.steps[1], {}))) or "-"))
    local rw = q.def.rewards or {}
    local parts = {}
    for _, it in ipairs(rw.items or {}) do parts[#parts + 1] = fmt("%s x%d", it.name or ("item " .. it.id), it.n or 1) end
    for _, p in ipairs(rw.pokemon or {}) do parts[#parts + 1] = p.name or ("Pokémon #" .. p.species) end
    if rw.money and rw.money > 0 and not ZA then parts[#parts + 1] = fmt("%d money", rw.money) end
    if #parts > 0 then w:label("Reward: " .. table.concat(parts, ", ")) end
    w:separator()
    w:button("Accept", function()
      w:close()
      busy = false
      q:start()
      say(q, q.giver_spec, q.def.lines.accept)
    end)
    w:button("Not now", function() w:close() say(q, q.giver_spec, q.def.lines.decline) end)
    w:open()
  end)
end

-- ---------------------------------------------------------------- the tracker, markers, the journal
local function marker(p, label, col, s, x0, y0, w0, h0)  -- (a diamond over a spot, the metres under it; at the edge when off screen)
  local x, y, d = G.draw.to_screen(p)
  local off = not x or x < x0 or y < y0 or x > x0 + w0 or y > y0 + h0
  if not x then x, y = x0 + w0 / 2, y0 + h0 - 60 * s end
  local cx, cy = min(max(x, x0 + 40 * s), x0 + w0 - 40 * s), min(max(y, y0 + 40 * s), y0 + h0 - 40 * s)
  local r = 16 * s
  G.draw.triangle(cx, cy - r, cx + r, cy, cx - r, cy, C.mark_dark)
  G.draw.triangle(cx - r, cy, cx + r, cy, cx, cy + r, C.mark_dark)
  local k = r - 4 * s
  G.draw.triangle(cx, cy - k, cx + k, cy, cx - k, cy, col)
  G.draw.triangle(cx - k, cy, cx + k, cy, cx, cy + k, col)
  if label then G.draw.text(label, cx, cy - r - 4 * s, {size = 22 * s, color = col, align = "center", valign = "bottom", shadow = true}) end
  if d then G.draw.text(fmt("%d m", floor(d + 0.5)), cx, cy + r + 4 * s, {size = 20 * s, color = C.text, align = "center", shadow = true}) end
  return off
end
local function over_head(p, glyph, col, s)
  local x, y, d = G.draw.to_screen(p)
  if not x or d > 40 then return end
  local r = 18 * s
  G.draw.circle(x, y, r + 3 * s, C.mark_dark)
  G.draw.circle(x, y, r, col)
  G.draw.text(glyph, x, y, {size = 28 * s, color = C.mark_dark, align = "center", valign = "middle"})
end
local function tracker_draw()
  local x, y, w, h = G.draw.box()
  local s = G.draw.scale()
  -- the givers' marks: ! a quest to take, ? one to hand in
  for _, q in ipairs(list) do
    local r = state(q)
    local gp = q.giver_spec and npc_pos(q, "giver")
    if gp then
      local up = gp + vec(0, ZA and 2.05 or 2.0, 0)
      if busy or talk then -- (not over a talk)
      elseif r.s == "none" and q.def.mark ~= false and q:available() then over_head(up, "!", C.mark, s)
      elseif r.s == "ready" then over_head(up, "?", C.mark, s) end
    end
  end
  local q = tracked
  if q and show_tracker and not talk then
    local r = state(q)
    if r.s == "active" or r.s == "ready" then
      local t, person = q:target()
      local pp = player.pos()
      local near = t and pp and r.s == "ready" and pp:dist(t) < 40  -- (handing in near the giver: their ? says it)
      if t and not near then marker(t + vec(0, person and (ZA and 2.15 or 2.1) or 1.0, 0), nil, C.mark, s, x, y, w, h) end
      local pw = 400 * s
      local px, py = x + w - pw - 36 * s, y + 250 * s
      local obj = q:objective()
      local tw = pw - 36 * s
      local _, th = G.draw.text_size(obj, 26 * s, tw)
      local _, hh = G.draw.text_size(q.def.title, 28 * s, tw)
      local ph = 30 * s + hh + 10 * s + th + 20 * s
      G.draw.rect(px, py, pw, ph, C.panel, 14 * s)
      G.draw.rect(px, py, 6 * s, ph, C.title, 3 * s)
      G.draw.text(q.def.title, px + 22 * s, py + 14 * s, {size = 28 * s, color = C.title, wrap = tw})
      G.draw.text(obj, px + 22 * s, py + 24 * s + hh, {size = 26 * s, color = C.text, wrap = tw})
      if r.s == "active" then  -- (the spots of a collect step: each one left)
        local st = step_of(q, r)
        if st and st.kind == "collect" then
          for i, sp in ipairs(st.spots or {}) do
            if not (r.got and r.got[i]) then
              local sx, sy, d = G.draw.to_screen(vec(sp) + vec(0, 0.6, 0))
              if sx and d < 45 then  -- (a glow and a twinkle; bigger near)
                local k = max(0.6, min(1.6, 12 / max(d, 1)))
                local pulse = (1 + 0.25 * math.sin(now() * 5)) * k
                G.draw.circle(sx, sy, 22 * s * pulse, 0x7fd8ff40)
                G.draw.circle(sx, sy, 12 * s * pulse, 0x9fe4ffa0)
                G.draw.circle(sx, sy, 6 * s * k, 0xffffffff)
                local r2 = 18 * s * pulse
                G.draw.line(sx - r2, sy, sx + r2, sy, 0xffffffc0, 2 * s)
                G.draw.line(sx, sy - r2, sx, sy + r2, 0xffffffc0, 2 * s)
              end
            end
          end
        end
      end
    end
  end
  talk_draw(x, y, w, h, s)
end

local journal
local function journal_build()
  journal:clear()
  local act, done = {}, {}
  for _, q in ipairs(list) do
    local s = state(q).s
    if s == "active" or s == "ready" then act[#act + 1] = q elseif s == "done" then done[#done + 1] = q end
  end
  journal:label(#act == 0 and "No quests on now." or "On now (pick one to track it):")
  if #act > 0 then
    local rows, sel = {}, nil
    for i, q in ipairs(act) do rows[i] = q.def.title .. "  -  " .. q:objective() if q == tracked then sel = i end end
    journal:list(rows, function(i) tracked = act[i] show_tracker = true journal_build() end, {selected = sel})
  end
  journal:separator()
  journal:label(#done == 0 and "Done: none yet." or "Done:")
  for _, q in ipairs(done) do journal:label("  \u{00B7} " .. q.def.title) end
  journal:separator()
  journal:checkbox("Show the tracker", show_tracker, function(on_) show_tracker = on_ end)
  journal:button("Close", function() journal:close() end)
end
function quest.journal(open_)
  if not journal then journal = G.gui.window{title = "Quests", w = 760} end
  if open_ == false or (open_ == nil and journal:is_open()) then journal:close() return end
  journal_build()
  journal:open()
end

-- ---------------------------------------------------------------- a few times a second: steps, spawns, drawing
local function want_draw()
  if talk then return true end
  if tracked and show_tracker and (state(tracked).s == "active" or state(tracked).s == "ready") then return true end
  local p = player.pos()
  if not p then return false end
  for _, q in ipairs(list) do  -- (a ! or ? to show near)
    local s = state(q).s
    if s == "none" or s == "ready" then
      local gp = q.giver_spec and npc_pos(q, "giver")
      if gp and p:dist(gp) < 40 then return true end
    end
  end
  return false
end
local tick_n, combo = 0, false
local function poll()
  tick_n = tick_n + 1
  local p = player.pos()
  local c = player.button("R") and player.button("DOWN")  -- (R + d-pad down: the journal)
  if c and not combo then quest.journal() end
  combo = c
  if p then
    for _, q in ipairs(list) do
      local r = state(q)
      if not q.synced and tick_n > 8 then q.synced = true if q.def.native and r.s ~= "none" then native(q, r.s == "done" and 255 or 10) end end
      if r.s ~= "done" or (q.giver_spec and not q.giver_spec.leave) then
        if q.giver_spec and not q.npcs[q.giver_spec.name] then npc_of(q, "giver") end
      end
      if r.s == "active" then
        if tick_n % 8 == 1 then for _, s in ipairs(q.npc_specs) do if not q.npcs[s.name] then npc_of(q, s) end end end
        local st = step_of(q, r)
        local k = st and K[st.kind]
        if k and k.poll and not busy then
          local ok, isdone = safe(q, "step", k.poll, q, st, r)
          if ok and isdone then q:advance() end
        end
      end
      if not tracked and (r.s == "active" or r.s == "ready") then tracked = q end
    end
  end
  if tick_n % 2 == 0 then scan_tick() end
  local want = want_draw()
  if want and not draw_id then draw_id = G.on("draw", tracker_draw)
  elseif not want and draw_id then G.off(draw_id) draw_id = nil end
end

-- ---------------------------------------------------------------- quest.define (bound to the mod that calls it)
local function check_def(d)
  if type(d) ~= "table" then error("quest.define{id = ..., title = ..., steps = {...}}", 3) end
  if type(d.id) ~= "string" or d.id == "" then error("quest.define: id = \"a_name\" (unique in the mod)", 3) end
  d.title = d.title or d.id
  d.lines = d.lines or {}
  d.steps = d.steps or {}
  for i, st in ipairs(d.steps) do
    if not K[st.kind] then error(fmt("quest %s: step %d's kind \"%s\" - talk, go, collect, catch, show, item, battle, time, wait or custom", d.id, i, tostring(st.kind)), 3) end
    if (st.kind == "catch" or st.kind == "show") and not tonumber(st.species) then error(fmt("quest %s: step %d (%s) needs species = <dex number>", d.id, i, st.kind), 3) end
    if st.kind == "go" and not st.pos then error(fmt("quest %s: step %d (go) needs pos = {x, y, z}", d.id, i), 3) end
    if st.kind == "collect" and not (st.spots and #st.spots > 0) then error(fmt("quest %s: step %d (collect) needs spots = {{x, y, z}, ...}", d.id, i), 3) end
    if st.kind == "item" and not tonumber(st.item) then error(fmt("quest %s: step %d (item) needs item = <id>", d.id, i), 3) end
    if st.kind == "battle" and ZA then rt.log(fmt("[lua] quest %s: step %d is a battle - Z-A can't start one from a script: it is skipped", d.id, i)) end
  end
end
local function define(mod, env, storage, d)
  check_def(d)
  local key = mod.name .. "/" .. d.id
  if by_key[key] then error("quest.define: \"" .. d.id .. "\" is defined twice in this mod", 2) end
  local q = setmetatable({id = d.id, key = key, def = d, mod = mod, env = env, storage = storage, npcs = {}, npc_specs = {}}, Q)
  if d.giver then
    if type(d.giver) == "string" then q.giver_spec = {name = d.giver, existing = true}
    else q.giver_spec = d.giver q.giver_spec.name = q.giver_spec.name or "Client" end
  end
  for _, st in ipairs(d.steps) do
    if type(st.npc) == "table" then st.npc.name = st.npc.name or ("Quest NPC " .. (#q.npc_specs + 1)) q.npc_specs[#q.npc_specs + 1] = st.npc st.npc = st.npc.name end
    if ZA and st.kind == "battle" then st.kind = "custom" st.text = st.text or "(a battle: Z-A can't start one)" end
  end
  for _, s in ipairs(d.npcs or {}) do q.npc_specs[#q.npc_specs + 1] = s end
  list[#list + 1] = q
  by_key[key] = q
  if not poll_id then  -- (the first quest: the checks a few times a second, J for the journal)
    poll_id = every(0.25, poll)
    G.on("key", "J", function(e) if e.down and not e.rep then quest.journal() end end)
  end
  local r = state(q)
  if r.s == "active" and step_of(q, r) and (step_of(q, r).kind == "catch") then scan.want[step_of(q, r).species] = true end
  return q
end
local function find(mod, id)
  if type(id) == "table" then return id end
  return by_key[mod.name .. "/" .. tostring(id)] or by_key[tostring(id)]
end
local function quest_for(mod, env)
  local storage = env.storage
  local t = {}
  t.define = function(d) return define(mod, env, storage, d) end
  t.get = function(id) return find(mod, id) end
  t.state = function(id) local q = find(mod, id) return q and state(q).s or nil end
  t.step = function(id) local q = find(mod, id) return q and state(q).step or nil end
  t.start = function(id) local q = find(mod, id) if q and state(q).s ~= "active" then q:start() return true end return false end
  t.advance = function(id) local q = find(mod, id) if q then q:advance() return true end return false end
  t.complete = function(id) local q = find(mod, id) if q then q:finish() return true end return false end
  t.reset = function(id) local q = find(mod, id) if q then put(q, {s = "none"}) native(q, 1) if tracked == q then tracked = nil end return true end return false end
  t.track = function(id) tracked = find(mod, id) show_tracker = true end
  t.list = function() local o = {} for _, q in ipairs(list) do o[#o + 1] = q end return o end
  t.journal = quest.journal
  t.objective = function(id) local q = find(mod, id) return q and q:objective() end
  return t
end

-- each mod's `quest`; include("lib/quest.lua") gives it too when the mod has no such file
local base_env = G.__mod_env
G.__mod_env = function(name, dir)
  local env = base_env(name, dir)
  if name ~= "" then
    local mod = A.mods[name]
    env.quest = quest_for(mod, env)
    local inc = env.include
    env.include = function(file)
      if file == "lib/quest.lua" and not env.files.exists("lua/lib/quest.lua") then return env.quest end
      return inc(file)
    end
  end
  return env
end
G.quest = quest_for(A.shared, G)

G.on("battle_end", function(res)
  for _, q in ipairs(list) do
    local r = state(q)
    local st = r.s == "active" and step_of(q, r)
    if st and st.kind == "battle" then K.battle.battle_end(q, st, r, res) end
  end
end)

A.cmd_add(A.shared, "quests", function()
  if #list == 0 then print("no quests defined") return end
  for _, q in ipairs(list) do
    local r = state(q)
    print(fmt("%-24s %-6s step %s/%d  %s  (%s)", q.key, r.s, tostring(r.step or "-"), #q.def.steps, q:objective(), q.def.title))
  end
end, "the quests and where each one is")
A.cmd_add(A.shared, "quest", function(what, id, n)
  if what == "journal" then quest.journal() return end
  local q = id and (by_key[tostring(id)] or (function() for _, x in ipairs(list) do if x.id == tostring(id) then return x end end end)())
  if not q then print("quest journal | quest start|reset|next|done|track <id> | quest step <id> <n>") return end
  if what == "start" then q:start()
  elseif what == "reset" then put(q, {s = "none"}) native(q, 1) if tracked == q then tracked = nil end
  elseif what == "next" then q:advance()
  elseif what == "done" then q:finish()
  elseif what == "track" then tracked = q show_tracker = true
  elseif what == "step" then local r = state(q) r.s, r.step = "active", tonumber(n) or 1 begin_step(q, r) end
  print(fmt("%s: %s, step %s - %s", q.key, state(q).s, tostring(state(q).step or "-"), q:objective()))
end, "quest start|reset|next|done|track <id> | quest step <id> <n> | quest journal: tests")

-- a reload: the overlay box and a held player let go
local base_unload = G.__unload
G.__unload = function()
  if talk and G.player.freeze then pcall(G.player.freeze, false) end
  talk = nil  -- (the state goes with the reload; what was kept is in storage)
  base_unload()
end
