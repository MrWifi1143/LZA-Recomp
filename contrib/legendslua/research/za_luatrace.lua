-- Z-A research (Legends Lua): who drives the game's own Lua 5.4 - lua_resume / lua_pcallk / luaL_loadbufferx counts, callers
-- and states every 600 frames (2026-11-19: one lua_pcallk a frame on the main state from 0x7000111560 - hle/za_script.c
-- runs mods' game scripts there). Copy into <game folder>/lua/ with Legends Lua switched on. Addresses: Z-A 2.0.2.
local RESUME, PCALL, LOADBUF = 0x70017ae970, 0x7000111bd0, 0x70017ab5c0
local st = {resume = {}, pcall = {}, load = {}}
local function bump(t, c)
  local k = string.format("%x", c.lr)
  local e = t[k] or {n = 0, L = {}}
  e.n = e.n + 1
  e.L[string.format("%x", c:x(0))] = true
  if not e.bt then local b = c:bt(6) local s = {} for i, a in ipairs(b) do s[i] = string.format("%x", a) end e.bt = table.concat(s, " ") end
  if c.addr == RESUME then e.from = string.format("%x", c:x(1)) end
  t[k] = e
end
hook(RESUME, function(c) bump(st.resume, c) end)
hook(PCALL, function(c) bump(st.pcall, c) end)
hook(LOADBUF, function(c)
  bump(st.load, c)
  local nm = mem.str(c:x(3), 80) or "?"
  log(string.format("LOADBUF f%d L %x len %d name %s lr %x", game.frame(), c:x(0), c:x(2), nm, c.lr))
end)
local last = 0
on_frame(function(f)
  if f - last < 600 then return end
  last = f
  for kind, t in pairs(st) do
    for k, e in pairs(t) do
      local ls = {} for l in pairs(e.L) do ls[#ls + 1] = l end
      log(string.format("%s f%d lr %s n %d states %d (%s) from %s bt %s", kind, f, k, e.n, #ls, table.concat(ls, ","):sub(1, 60), e.from or "-", e.bt or ""))
    end
    st[kind] = {}
  end
end)
log("za_luatrace on")
