-- Spawn Menu: an in-game menu (M on the keyboard, L + R on a controller, or the console's "spawnmenu") that makes
-- characters and Pokémon where you stand, sets the time of day and puts a small panel on the HUD. It remembers its
-- settings for each save. Shows the overlay (draw), menus (gui), the text dialog, images, sounds and storage.
local logo = image.load("images/logo.png")
local click = sound.load("sounds/click.wav")
local pop = sound.load("sounds/pop.ogg")

local SPECIES = {{25, "Pikachu"}, {133, "Eevee"}, {448, "Lucario"}}
local made = {}   -- what this menu made: {thing, label}

local function blip() if click then click:play{volume = 0.6} end end

local menu = gui.window{title = "Spawn Menu", w = 600}
local list_start  -- (the "Made" list is rebuilt after each spawn)

local function rebuild()
  menu:clear()
  if logo then menu:image(logo, 80) end
  menu:label("Characters")
  menu:input("Name", storage.get("name", "Bob"), function(v) storage.set("name", v) blip() end, {max = 24})
  menu:button("Make a villager here", function()
    local n = npc.spawn{pos = player.ahead(3), yaw = player.yaw() + 180, name = storage.get("name", "Bob")}
    made[#made + 1] = {n, "Character " .. n.name}
    n:on("up", function() n:say("Hi, I'm " .. n.name .. "!") end)
    if pop then pop:play{pos = n.home, range = 25} end
    storage.set("made", storage.get("made", 0) + 1)
    rebuild()
  end)
  menu:button("Make one dressed like you", function()
    local n = npc.spawn{pos = player.ahead(3), yaw = player.yaw() + 180, name = storage.get("name", "Bob"), outfit = "player"}
    made[#made + 1] = {n, "Dressed " .. n.name}
    n:on("up", function() n:say("Same outfit!") end)
    if pop then pop:play{pos = n.home, range = 25} end
    storage.set("made", storage.get("made", 0) + 1)
    rebuild()
  end)
  menu:separator()
  menu:label("Pokémon")
  local names = {}
  for i, s in ipairs(SPECIES) do names[i] = s[2] end
  menu:choice("Species", names, storage.get("species", 1), function(i) storage.set("species", i) blip() end)
  menu:checkbox("Shiny", storage.get("shiny", false), function(v) storage.set("shiny", v) blip() end)
  menu:button("Make it", function()
    local s = SPECIES[storage.get("species", 1)]
    local p, err = pokemon.spawn{species = s[1], pos = player.ahead(3.5), yaw = player.yaw() + 180, shiny = storage.get("shiny", false), name = s[2]}
    if not p then ui.notify("No " .. s[2] .. ": " .. tostring(err)) return end
    made[#made + 1] = {p, (storage.get("shiny", false) and "Shiny " or "") .. s[2]}
    if pop then pop:play{pos = player.ahead(3.5), range = 25, pitch = 1.3} end
    storage.set("made", storage.get("made", 0) + 1)
    rebuild()
  end)
  menu:separator()
  menu:label("World")
  local pending
  menu:slider("Time of day", 0, 24, storage.get("hour", 12), function(v)
    storage.set("hour", v)
    if pending then timer.cancel(pending) end  -- (set once the slider rests: Z-A's city reloads for a new time)
    pending = timer.after(0.6, function() pending = nil game.set_time(v) end)
  end, {step = 0.5, format = "%.1f h"})
  menu:checkbox("Panel on the HUD", storage.get("panel", true), function(v) storage.set("panel", v) blip() end)
  menu:separator()
  menu:label(function() return ("Made: %d here, %d in this save"):format(#made, storage.get("made", 0)) end)
  local labels = {}
  for i, m in ipairs(made) do labels[i] = m[2] end
  if #labels > 0 then
    menu:list(labels, function(i)
      local m = table.remove(made, i)
      if m then m[1]:remove() end
      blip()
      rebuild()
    end)
    menu:button("Remove them all", function()
      for _, m in ipairs(made) do m[1]:remove() end
      made = {}
      blip()
      rebuild()
    end)
  end
  menu:button("Close", function() menu:close() end)
end
rebuild()

local function toggle() rebuild() menu:toggle() blip() end
on("key", "M", function(e) if e.down and not e.rep then toggle() end end)
on("button", "R", function() if player.button("L") then toggle() end end)
cmd.add("spawnmenu", toggle, "spawnmenu: the Spawn Menu")

-- the HUD panel: top right of the game's interface box
hud.add(function()
  if not storage.get("panel", true) then return end
  local bx, by, bw = draw.box()
  local s = draw.scale()
  local w, h = 300 * s, 92 * s
  local x, y = bx + bw - w - 24 * s, by + 120 * s
  draw.rect(x, y, w, h, 0x14171fc8, 14 * s)
  draw.rect_outline(x, y, w, h, 0xeec25480, 1.5 * s, 14 * s)
  draw.text("Spawn Menu", x + 16 * s, y + 12 * s, {size = 24 * s, color = "yellow"})
  draw.text(("%d made · M / L+R"):format(#made), x + 16 * s, y + 48 * s, {size = 22 * s, color = 0xd8dbe4ff})
end)
