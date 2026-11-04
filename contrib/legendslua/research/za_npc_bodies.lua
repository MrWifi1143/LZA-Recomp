-- Z-A research (Legends Lua): character bodies as they appear and go, with the player's distance to them then. Copy
-- into <game folder>/lua/ with Legends Lua switched on. Addresses: Z-A 2.0.2.
--
-- A character's body (vtable 0x7003be6218, position at +0x90 / +0x98) is updated through 0x7000081ce0 every frame
-- while it exists. Found with this (2026-11-04): people are created ~48 m away (their spawner's OuterPlayerTrigger,
-- 50 m) and kept to ~75 m; at Max People ~58 m (ZARecomp 9219604). Some bodies appear at ~30 m and go at ~31 m (or
-- ~58 / ~58 m): a near-only body, not the person's spawn - check with screenshots before trusting a distance.
local START = nil      -- {x, z}: the player's start (Legends Studio `zasave <save>/main` prints it); nil: the first body seen
local LOG_NEW = true   -- each new body: frame, position, distance
local LOG_GONE = true  -- each body no longer updated (checked every 600 frames): first and last frame and distance

local player, bodies = nil, {}
hook(0x7000081ce0, function(c)
  local o = c:x(0)
  if mem.u64(o) ~= 0x7003be6218 then return end
  local x, y, z = mem.f32(o + 0x90), mem.f32(o + 0x94), mem.f32(o + 0x98)
  if not x then return end
  if not player and (not START or (math.abs(x - START[1]) < 2 and math.abs(z - START[2]) < 2)) then
    player = o
    log(string.format("BODY player %x", o))
  end
  local px, pz = player and mem.f32(player + 0x90), player and mem.f32(player + 0x98)
  local d = px and math.sqrt((x - px) ^ 2 + (z - pz) ^ 2) or -1
  local e = bodies[o]
  if not e then
    bodies[o] = {first = game.frame(), d0 = d, last = game.frame(), d1 = d, x = x, z = z}
    if LOG_NEW then log(string.format("BODY new f%d %x at %.1f %.1f %.1f  dist %.1f", game.frame(), o, x, y, z, d)) end
  else
    e.last, e.d1, e.x, e.z = game.frame(), d, x, z
  end
end)

on_frame(function(f)
  if not LOG_GONE or f % 600 ~= 0 then return end
  for o, e in pairs(bodies) do
    if f - e.last > 60 then
      log(string.format("BODY gone %x: first f%d at %.1f m, last f%d at %.1f m (at %.1f %.1f)", o, e.first, e.d0, e.last, e.d1, e.x, e.z))
      bodies[o] = nil
    end
  end
end)

log("za_npc_bodies on")
