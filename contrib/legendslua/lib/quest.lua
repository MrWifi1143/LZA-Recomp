-- The quest library is NOT in this release (Z-A 1.3.0 / Arceus 1.4.0): quests come with Legends Studio, whose Quest
-- Builder makes them. Here every mod's `quest` (and include("lib/quest.lua")) is a stand-in: anything called on it stops
-- the script with "quest.<name>: not available in this release", the way net.* does.
local G = _G
local MSG = "not available in this release (quests come with Legends Studio)"
local stub = setmetatable({}, {
  __index = function(_, k) return function() error("quest." .. tostring(k) .. ": " .. MSG, 2) end end,
  __newindex = function() error("quest: " .. MSG, 2) end,
})
local base_env = G.__mod_env
G.__mod_env = function(name, dir)
  local env = base_env(name, dir)
  if name ~= "" then
    env.quest = stub
    local inc = env.include
    env.include = function(file)
      if file == "lib/quest.lua" and not env.files.exists("lua/lib/quest.lua") then return stub end
      return inc(file)
    end
  end
  return env
end
G.quest = stub
