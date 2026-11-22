-- The Scout's Lost Supplies: a quest made in Legends Studio's Quest Builder (its source: scout_supplies.json beside this file - edit
-- that in the Studio; this file is written anew from it). Run by the recomp's Legends Lua quest library
-- (lib/quest.lua, built into the plugin): docs/lua-api.md "Quests".
local quest = include("lib/quest.lua")

quest.define{
  id = "scout_supplies",
  title = "The Scout's Lost Supplies",
  description = "Tomas of the Survey Corps dropped his supplies crossing the Fieldlands. Help him gather them and restock.",
  area = "ha_area01",
  giver = {name = "Tomas", pos = {566.700, 27.280, 106.200}, yaw = 140},
  npcs = {
    {name = "Hana", pos = {563.500, 27.700, 109.500}, yaw = 230, outfit = "player"},
  },
  lines = {
    offer = {"You there! You're with the Survey Corps too, aren't you?", "A Bidoof spooked me and I dropped my whole pack. Could you lend a hand?"},
    accept = {"Much obliged! Hana over there saw where things rolled."},
    decline = {"I'll manage... somehow."},
    progress = {"Any luck finding my things?"},
    done = {"My supplies, and a Bidoof to keep an eye on the others!", "Here, take these for your trouble."},
    after = {"Thanks again. I'll watch my step around Bidoof from now on."},
  },
  steps = {
    {kind = "talk", text = "Ask Hana where the supplies went", npc = "Hana", lines = {"Tomas's pack? Things rolled all the way to the big rock.", "Look there first."}},
    {kind = "go", text = "Search the tall grass by the big rock", pos = {541.600, 30.300, 126.600}, radius = 7, place = "the big rock", lines = {"There's a trail of dropped supplies here!"}},
    {kind = "collect", text = "Gather the dropped supplies", spots = {{543.900, 29.630, 121.400}, {555, 28.290, 112}, {550, 29.390, 104}}, radius = 1.800, one = "a bundle of supplies", lines = {"That's everything that fell out."}},
    {kind = "catch", text = "Catch a Bidoof", species = 399, count = 1, what = "a Bidoof", lines = {"Caught one! Tomas will be relieved."}},
    {kind = "item", text = "Bring Tomas 3 Poké Balls", npc = "giver", count = 3, item = 1710, what = "Poké Balls", lines = {"Three Poké Balls - just what I needed to restock. Thank you!"}, missing = {"I need three Poké Balls to restock. Come back when you have them."}},
  },
  rewards = {items = {{id = 1710, n = 5, name = "Poké Ball"}}, pokemon = {{species = 133, level = 5, name = "Eevee"}}, money = 500},
}
