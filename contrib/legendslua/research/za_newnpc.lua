-- Z-A research (Legends Lua): how townspeople are made and go - the traces behind runtime/hle/newnpc.c (LARECOMP_NEWNPC).
-- Copy into <game folder>/lua/ with Legends Lua switched on. Addresses: Z-A 2.0.2. Found 2026-11-16/17:
--
--   station NPCs, crowd walkers, pairs, NPCs' Pokemon all come from ONE call, the character placement spawn
--   0x70005e8644(x0 spawner component, x1 NPC data {name struct...}, x2 the new object's name struct {hash, char *, len,
--   record}, x3 = S+8, x4 = S+0x40 a transform {+0x10 scale, +0x20 quaternion, +0x30 position}, x5 = S, x6 a hash).
--   Callers: 0x7000654c30 placement NPC spawner (station), 0x70006541a4 its NPC's Pokemon, 0x70009ce1e8 traffic spawner
--   (crowd), 0x700049e7b4 / 0x700049e994 pair spawner, 0x70005e61c4 outzone effects. For a station spawn x3+0xc8 is the
--   spawner object's name (e.g. spawner_npc_talk_A04_z02_03_0006_010) and x1 the NPC data (npc_A04_z02_03_0006).
--   The object it makes is a node (vtable 0x7003be0d28, name at +0x2b8, translation +0xe0); the character manager
--   (0x70000af150) realises it later (live node from it, "active_<name>_old" destroyed, then the CharacterCreationMaster
--   make 0x70011164d0, whose request +0x30 -> +0xc8 -> +0x38 is the live node). Nodes are destroyed through 0x7001147330
--   (their destructor), from the world's deferred destroy list (0x7001ce2e90) - an area unloading, or one put away.
local LOG_SPAWNS = true    -- every placement spawn: caller kind, NPC data, name, spawner, position
local LOG_MAKES = true     -- every character model made: the node's name and its .ccdatam
local LOG_GONE = true      -- every named character node destroyed

local kinds = {[0x7000654c30] = "station", [0x70006541a4] = "station-pokemon", [0x70009ce1e8] = "crowd",
  [0x700049e7b4] = "pair", [0x700049e994] = "pair-pokemon", [0x70005e61c4] = "outzone"}
local function name_of(ns)
  local p, n = mem.u64(ns + 8), mem.u64(ns + 0x10)
  return (p and n and n < 200) and mem.str(p, n) or "?"
end

if LOG_SPAWNS then
  hook(0x70005e8644, function(c)
    local kind = kinds[c.lr] or string.format("%x", c.lr)
    local sp = ""
    for i = 0, 0x100, 8 do  -- (the spawner object's name: a char * in S, e.g. x3+0xc8)
      local v = mem.u64(c:x(3) + i)
      local s = v and v > 0x7000000000 and v < 0x7400000000 and mem.str(v, 48)
      if s and s:find("^spawner") then sp = "spawner " .. s break end
    end
    local t = c:x(4) + 0x30
    log(string.format("SPAWN f%d %s data %s name %s %s at %.2f %.2f %.2f", game.frame(), kind, name_of(c:x(1)), name_of(c:x(2)),
      sp, mem.f32(t) or 0, mem.f32(t + 4) or 0, mem.f32(t + 8) or 0))
  end)
end

if LOG_MAKES then
  hook(0x70011164d0, function(c)
    local req = c:x(2)
    local fb = mem.u64(req) or 0
    local b = fb ~= 0 and mem.bytes(fb, 4096) or ""
    local model = b:match("ik_chara/([%w_/]+)%.ccdatam") or "?"
    local cb = mem.u64((mem.u64(req + 0x30) or 0) + 0xc8)
    local node = cb and mem.u64(cb + 0x38)
    local nm = node and mem.str(mem.u64(node + 0x2b8) or 0, 80) or "?"
    log(string.format("MAKE f%d %s %s", game.frame(), nm, model))
  end)
end

if LOG_GONE then
  hook(0x7001147330, function(c)
    local nm = mem.str(mem.u64(c:x(0) + 0x2b8) or 0, 80) or ""
    if nm:find("npc") then log(string.format("GONE f%d %s", game.frame(), nm)) end
  end)
end

log("za_newnpc on")
