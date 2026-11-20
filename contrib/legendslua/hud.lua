-- Legends Lua: the mod API's version 6 for scripts - the overlay (draw, image, hud), keyboard and mouse (input, on("key")
-- / on("mouse") / on("text")), sounds, files of a mod's folder, storage per save (docs/lua-api.md). Built into the
-- plugin; runs right after api.lua with the same C primitives (rt), and reaches api.lua's own pieces through G.__api.
local rt = ...
local G = _G
local A = G.__api
local on, emit, emit_to, has, run, report, vec, is_vec = A.on, A.emit, A.emit_to, A.has, A.run, A.report, A.vec, A.is_vec
local fmt, floor, min, max = string.format, math.floor, math.min, math.max
local V6 = rt.have_api and (rt.api_version or 1) >= 6

-- ---------------------------------------------------------------- colours
-- a colour is 0xRRGGBBAA, or a table {r, g, b, a} (0..255, a 255 by default): color(r, g, b, a) makes the number
local function color(r, g, b, a)
  if type(r) == "table" then r, g, b, a = r.r or r[1], r.g or r[2], r.b or r[3], r.a or r[4] end
  local function c(v) v = floor(tonumber(v) or 0) return v < 0 and 0 or v > 255 and 255 or v end
  return c(r) << 24 | c(g) << 16 | c(b) << 8 | c(a or 255)
end
-- color.hex("#ff8800" / "#ff880080"), color.alpha(c, 0..255), color.lerp(a, b, t)
local function hex(h)
  h = tostring(h):gsub("^#", "")
  if #h == 6 then h = h .. "ff" end
  local v = tonumber(h, 16)
  if not v or #h ~= 8 then error("color.hex(\"#rrggbb\" or \"#rrggbbaa\")", 2) end
  return v
end
local colors = {
  white = 0xffffffff, black = 0x000000ff, red = 0xe8414bff, green = 0x4cc35aff, blue = 0x3d7fe0ff, yellow = 0xf2c84bff,
  gray = 0x8c8c94ff, dark = 0x1c1f26e6, clear = 0x00000000,
}
local function col(c, def)
  if c == nil then return def or 0xffffffff end
  if type(c) == "string" then return colors[c] or error("no colour \"" .. c .. "\" (white black red green blue yellow gray dark)", 3) end
  return c
end

-- ---------------------------------------------------------------- draw (immediate: inside a draw handler)
local draw = {}
local drawing = false
local ALIGN = {left = 0, center = 1, right = 2}
local function size_now() return rt.hud_size() end
function draw.size() local w, h = rt.hud_size() return w, h end
-- the game's interface box: 16:9 in the middle (the whole screen at 16:9; a wider one - ultrawide - keeps it centred)
function draw.box() local _, _, x, y, w, h = rt.hud_size() return x, y, w, h end
-- the box's height over 1080: what a layout made for 1920x1080 is multiplied by
function draw.scale() local _, _, _, _, _, h = rt.hud_size() return h / 1080 end
function draw.rect(x, y, w, h, c, rounding) rt.hud_rect(x, y, w, h, col(c), rounding or 0) end
function draw.rect_outline(x, y, w, h, c, thickness, rounding) rt.hud_rect_outline(x, y, w, h, col(c), rounding or 0, thickness or 1) end
function draw.line(x1, y1, x2, y2, c, thickness) rt.hud_line(x1, y1, x2, y2, col(c), thickness or 1) end
function draw.circle(x, y, r, c) rt.hud_circle(x, y, r, col(c)) end
function draw.circle_outline(x, y, r, c, thickness) rt.hud_circle(x, y, r, col(c), thickness or 1) end
function draw.triangle(x1, y1, x2, y2, x3, y3, c) rt.hud_tri(x1, y1, x2, y2, x3, y3, col(c)) end
function draw.gradient(x, y, w, h, top, bottom) rt.hud_grad(x, y, w, h, col(top), col(bottom, 0x000000ff)) end
-- draw.text(text, x, y, opts) - opts: size (22), color, align ("left" / "center" / "right"), valign ("top" / "middle" /
-- "bottom"), wrap (a width: lines wrapped in it; align then within it), shadow; or draw.text(text, x, y, size, color, align)
function draw.text(text, x, y, o, c, align)
  text = tostring(text)
  local size, wrap, flags = 22, 0, 0
  if type(o) == "table" then
    size, c, align, wrap = o.size or 22, o.color, o.align, o.wrap or 0
    if o.valign == "middle" then flags = flags | 4 elseif o.valign == "bottom" then flags = flags | 8 end
    if o.shadow then flags = flags | 16 end
  elseif o ~= nil then size = o end
  flags = flags | (ALIGN[align or "left"] or 0)
  rt.hud_text(text, x, y, size, col(c), flags, wrap)
end
function draw.text_size(text, size, wrap) return rt.hud_measure(tostring(text), size or 22, wrap or 0) end
-- an image (image.load): x, y, w, h (default: its size); opts tint, uv {u0, v0, u1, v1} (0..1: a part of it), rounding
function draw.image(img, x, y, w, h, o)
  if type(img) ~= "table" or not img.id then error("draw.image(img, ...): an image from image.load", 2) end
  o = o or {}
  local uv = o.uv or {0, 0, 1, 1}
  rt.hud_image(img.id, x, y, w or img.w, h or img.h, col(o.tint), uv[1], uv[2], uv[3], uv[4], o.rounding or 0)
end
-- clip: what's drawn by fn (or until draw.unclip) is cut to the rectangle
function draw.clip(x, y, w, h, fn)
  rt.hud_clip(x, y, w, h)
  if fn then local ok, e = pcall(fn) rt.hud_unclip() if not ok then error(e, 0) end end
end
function draw.unclip() rt.hud_unclip() end
-- the alpha (0..1) everything drawn after it is multiplied by (until the next draw.alpha; each frame starts at 1)
function draw.alpha(a) return rt.hud_alpha(a or 1) end
-- where a point of the world is on the screen: x, y, metres away - nil behind the camera
function draw.to_screen(p) p = vec(p) return rt.hud_to_screen(p.x, p.y, p.z) end
local color_mt = {__call = function(_, ...) return color(...) end}
local color_api = setmetatable({hex = hex,
  alpha = function(c, a) c = col(c) return (c & 0xffffff00) | (floor(a) & 255) end,
  lerp = function(a, b, t)
    a, b = col(a), col(b)
    local o = 0
    for sh = 0, 24, 8 do
      local x, y = (a >> sh) & 255, (b >> sh) & 255
      o = o | (floor(x + (y - x) * t + 0.5) & 255) << sh
    end
    return o
  end}, color_mt)
for k, v in pairs(colors) do color_api[k] = v end
draw.color = color_api

-- ---------------------------------------------------------------- the frame: input in, draw handlers, the list out
local input = {}
local keys_down, mouse_down = {}, {}
local function want_input() return has("key") or has("mouse") or has("text") or G.__gui_open end
local last_submit = 0
local function input_poll()
  if not V6 or not want_input() then return end
  local evs = rt.input_events()
  if not evs then return end
  for _, e in ipairs(evs) do
    local t = e.type
    if t == "key_down" or t == "key_up" then
      local down = t == "key_down"
      keys_down[e.key] = down or nil
      e.down = down
      if G.__gui_key and G.__gui_key(e) then goto next end  -- (an open menu took it)
      emit_to("key", e.key, e)
    elseif t == "mouse_down" or t == "mouse_up" or t == "wheel" then
      if e.button then mouse_down[e.button] = t == "mouse_down" or nil end
      e.down = t == "mouse_down"
      if G.__gui_mouse and G.__gui_mouse(e) then goto next end
      emit_to("mouse", e.button or "wheel", e)
    elseif t == "text" then
      emit("text", e.text)
    end
    ::next::
  end
end
local function draw_frame()
  local want = has("draw") or G.__gui_open
  if not want then
    if last_submit > 0 then rt.hud_begin() rt.hud_end() last_submit = 0 end  -- (nothing left to draw: the overlay cleared)
    return
  end
  rt.hud_begin()
  drawing = true
  emit("draw")
  if G.__gui_draw then G.__gui_draw() end
  drawing = false
  last_submit = rt.hud_end()
end
local base_tick = G.__tick
G.__tick = function(frame)
  input_poll()
  base_tick(frame)
  draw_frame()
  if has("draw") or want_input() then G.__tick_every = 1 end
  if G.__storage_tick then G.__storage_tick() end
end

function input.mouse_pos() local x, y = rt.input_mouse() return x, y end
function input.mouse_down(b) if b then return mouse_down[b] == true end local _, _, k = rt.input_mouse() return k end
function input.key_down(name) return keys_down[name] == true end
-- the game's controller and keyboard held back from the game (a menu of the script's); the controller still reaches
-- scripts (player.button, on("button"))
function input.grab(on_) rt.input_grab(on_ and true or false) end

-- ---------------------------------------------------------------- per mod: images, sounds, files, storage, hud
local function mod_root(mod)  -- (mods/<name>/lua -> mods/<name>; the game folder's lua -> the game folder)
  return (mod.dir or ""):gsub("[/\\]lua$", "")
end
local function rel_ok(p)
  return type(p) == "string" and p ~= "" and not p:find("%.%.") and not p:find("^[/\\]") and not p:find(":")
end
local function mod_path(mod, p, what)
  if not rel_ok(p) then error(what .. ": a file of the mod's own folder (\"images/x.png\")", 3) end
  return mod_root(mod) .. "/" .. p
end

local Sound, Voice = {}, {}
Sound.__index, Voice.__index = Sound, Voice
Sound.__tostring = function(s) return fmt("sound %s (#%d)", s.file, s.id) end
local function play(s, o)
  o = o or {}
  local p = o.pos and vec(o.pos)
  local v = rt.sound_play(s.id, o.volume or 1, o.pitch or 1, p and p.x, p and p.y, p and p.z, o.range or 30, o.loop)
  if v == 0 then return nil end
  return setmetatable({id = v, sound = s}, Voice)
end
function Sound:play(o) return play(self, o) end
function Voice:stop() rt.sound_stop(self.id) end
function Voice:playing() return rt.sound_playing(self.id) end
function Voice:set_volume(v) rt.sound_set(self.id, v) end
function Voice:set_pos(p) p = vec(p) rt.sound_set(self.id, nil, p.x, p.y, p.z) end

-- storage: a mod's values kept per save (the save the game has: <game folder>/moddata/<mod>/<save>.lua), and for the
-- whole mod (storage.global); a Lua table written half a second after a change (strings, numbers, true/false, tables)
local stores = {}
local function ser(v, depth, seen)
  local t = type(v)
  if t == "string" then return fmt("%q", v)
  elseif t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "0" end
    return math.type(v) == "integer" and tostring(v) or fmt("%.17g", v)
  elseif t == "boolean" then return tostring(v)
  elseif t == "table" then
    if depth > 32 or seen[v] then error("storage: a table inside itself, or nested more than 32 deep", 0) end
    seen[v] = true
    local parts = {}
    for k, x in pairs(v) do
      local kt = type(k)
      if (kt == "string" or kt == "number") and (type(x) ~= "function" and type(x) ~= "userdata" and type(x) ~= "thread") then
        parts[#parts + 1] = "[" .. ser(k, depth + 1, seen) .. "]=" .. ser(x, depth + 1, seen)
      end
    end
    seen[v] = nil
    table.sort(parts)
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return "nil"
end
local save_id, save_id_at = nil, -10
local function current_save()  -- (looked up once a second at most)
  local t = rt.now()
  if t - save_id_at > 1 then save_id_at, save_id = t, rt.save_id() end
  return save_id ~= "" and save_id or "default"
end
local function store_for(mod, global)
  local slot = global and "_mod" or current_save()
  local key = mod.name .. "/" .. slot
  local s = stores[key]
  if s then return s end
  local path = rt.data_root .. "/moddata/" .. mod.name .. "/" .. slot .. ".lua"
  local data = {}
  local text = rt.file_read(path, 16 << 20)
  if text then
    local chunk = load("return " .. text, "=" .. path, "t", {})
    local ok, v = false, nil
    if chunk then ok, v = pcall(chunk) end
    if ok and type(v) == "table" then data = v else report(mod, "storage", "the saved values in " .. path .. " couldn't be read: started empty") end
  end
  s = {path = path, data = data, dirty = false, mod = mod}
  stores[key] = s
  return s
end
G.__storage_tick = function()
  local t = rt.now()
  for _, s in pairs(stores) do
    if s.dirty and t - s.dirty_at > 0.5 then
      s.dirty = false
      local ok, text = pcall(ser, s.data, 0, {})
      if ok then rt.file_write_later(s.path, text) else report(s.mod, "storage", text) end
    end
  end
end
local function storage_api(mod, global)
  local st = {}
  local function me() return store_for(mod, global) end
  function st.get(k, default) local v = me().data[k] if v == nil then return default end return v end
  function st.set(k, v)
    if type(k) ~= "string" and type(k) ~= "number" then error("storage.set(key, value): the key is a string or a number", 2) end
    local s = me()
    s.data[k] = v
    s.dirty, s.dirty_at = true, rt.now()
  end
  function st.all() return me().data end
  function st.save() local s = me() s.dirty, s.dirty_at = true, 0 end  -- (a table changed in place: written)
  function st.slot() return global and "" or current_save() end
  return st
end

local images = {}
local function ext_env(mod, env)
  env.draw = draw
  env.color = color_api
  env.input = setmetatable({
    -- the runtime's text dialog (typed, or the on-screen keyboard with a controller); fn(text) on OK, fn(nil) cancelled
    text = function(title, initial, maxlen, fn)
      if type(initial) == "function" then fn, initial, maxlen = initial, nil, nil end
      if type(maxlen) == "function" then fn, maxlen = maxlen, nil end
      if not rt.text_prompt(tostring(title or ""), tostring(initial or ""), tonumber(maxlen) or 60) then return false end
      local id
      id = env.timer.every(0.05, function()
        local st, text = rt.text_prompt_poll()
        if st == 0 then return end
        env.timer.cancel(id)
        if fn then fn(st == 1 and text or nil) end
      end)
      return true
    end,
  }, {__index = input})
  env.image = {
    load = function(file)
      local p = mod_path(mod, file, "image.load")
      if images[p] then return images[p] end
      local id, w, h = rt.hud_load_image(p)
      if not id then return nil, "not read: " .. p end
      local img = {id = id, w = w, h = h, file = file}
      images[p] = img
      return img
    end,
  }
  env.sound = {
    load = function(file)
      local id = rt.sound_load(mod_path(mod, file, "sound.load"))
      if not id then return nil, "not read: " .. file end
      return setmetatable({id = id, file = file}, Sound)
    end,
    play = function(s, o)
      if type(s) == "string" then s = env.sound.load(s) end
      if not s then return nil end
      return play(s, o)
    end,
    stop_all = function() rt.sound_stop(0) end,
  }
  env.files = {
    read = function(file) return rt.file_read(mod_path(mod, file, "files.read")) end,
    exists = function(file) return rel_ok(file) and rt.file_exists(mod_root(mod) .. "/" .. file) end,
  }
  env.storage = storage_api(mod, false)
  env.storage.global = storage_api(mod, true)
  env.hud = {
    add = function(fn) return env.on("draw", fn) end,   -- (hud.add(fn): fn() each frame, drawing; off(id) stops it)
    remove = function(id) return env.off(id) end,
  }
end
ext_env(A.shared, G)
local base_env = G.__mod_env
G.__mod_env = function(name, dir)
  local env = base_env(name, dir)
  if name ~= "" then ext_env(A.mods[name], env) end
  return env
end
G.__unload_ext = function() if V6 then rt.sound_stop(0) rt.input_grab(false) end end
local base_unload = G.__unload
G.__unload = function() base_unload() G.__unload_ext() end
