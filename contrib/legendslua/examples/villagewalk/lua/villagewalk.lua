-- Village Walk - a sample mod for Legends: Arceus (Legends Lua, docs/lua-api.md).
-- Three new townspeople walk a loop in front of where you stand when the game loads (Jubilife Village, the Fieldlands -
-- anywhere with people). Come near one and it stops, turns to you and says hello; walk away and it carries on. Talk to
-- one (A, facing it) for a line of its own.
-- Install: copy this folder to <game folder>/mods/villagewalk/ and switch the mod on (Mods page) - or Legends Studio's Play.

local SIZE = 2                 -- (the loop: a square this many metres from its middle)
local CENTER = nil             -- (a fixed spot: vec(x, y, z); nil: 6 m in front of where the player first stands)
local lines = {
  "Good day! The Survey Corps keeps us busy.",
  "Have you seen the Galaxy Hall up close?",
  "I walk this square every day. Good for the legs!",
}

local walkers = {}

local function make(c)
  local loop = {c + vec(SIZE, 0, -SIZE), c + vec(SIZE, 0, SIZE), c + vec(-SIZE, 0, SIZE), c + vec(-SIZE, 0, -SIZE)}
  for i = 1, 3 do
    local n = npc.spawn{pos = loop[i], name = "Walker " .. i, yaw = loop[i]:yaw_to(loop[i % 4 + 1])}
    n.near_radius = 2.5
    n.data.line = lines[i]
    n:walk_route(loop, {loop = true, wait = 1, start = i % 4 + 1})  -- (each starts toward the next corner)
    walkers[#walkers + 1] = n
  end
  log(string.format("three walkers around %s", tostring(c)))
end

-- the player near one: it stops and greets them; gone: on its way again
on("near", function(n)
  if not n.data.line then return end
  n:pause()
  n:face_player()
  n:say("Oh, hello there!", 2)
end)
on("far", function(n)
  if n.data.line then n:resume() end
end)
on("talk", function(n)
  if n.data.line then n:face_player() n:say(n.data.line) end
end)

-- made once the player is in an area (tried every second until then)
local t
t = timer.every(1, function()
  local p = player.pos()
  if not p then return end
  timer.cancel(t)
  make(CENTER or player.ahead(6))
end)

cmd.add("walkers", function()
  for _, n in ipairs(walkers) do print(n, n:pos(), n:dist() and string.format("%.1f m away", n:dist()) or "") end
end, "where the Village Walk walkers are")
