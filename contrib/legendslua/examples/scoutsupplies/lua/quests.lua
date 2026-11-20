-- The mod's quests (Legends Studio's Quest Builder writes this file anew): each one is lua/quests/<id>.lua.
-- (Files in a folder below lua/ run only when included - this does it, in this order.)
for _, id in ipairs({"scout_supplies"}) do
  local ok, err = pcall(include, "quests/" .. id .. ".lua")
  if not ok then log("quest " .. id .. ": " .. tostring(err)) end
end
