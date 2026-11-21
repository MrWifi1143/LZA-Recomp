-- Legends Lua: menus for scripts (gui.window - docs/lua-api.md), drawn on the overlay with the draw API (hud.lua).
-- Windows hold a column of widgets (labels, buttons, checkboxes, sliders, choices, lists, text fields, separators). The
-- top window has the input while it's open - the game's is held back (input.grab) - and works with a controller alone
-- (d-pad / arrows move, A / Enter / Space activate, left / right change a slider or choice, B / Backspace / Esc close),
-- with a mouse (point, click, drag a slider, wheel scrolls) and a keyboard. A text field opens the runtime's text dialog
-- (typed, or its on-screen keyboard with a controller).
local rt = ...
local G = _G
local A = G.__api
local draw = G.draw
local fmt, floor, min, max, abs = string.format, math.floor, math.min, math.max, math.abs

local gui = {}
-- (straight to the C side, colours already numbers: no tables made per call - less garbage each frame)
local R, RO, T, CI, IMG, CLIP, UNCLIP = rt.hud_rect, rt.hud_rect_outline, rt.hud_text, rt.hud_circle, rt.hud_image, rt.hud_clip, rt.hud_unclip
local MID, MIDC, MIDR = 4, 5, 6  -- (text flags: vertically centred; + centred / right)
local stack = {}  -- (open windows, the last on top)
local Window = {}
Window.__index = Window

local C = {
  panel = 0x14171fe8, title = 0x232836ff, edge = 0x3a4152ff, text = 0xeef0f5ff, dim = 0x9aa1b3ff, gold = 0xeec254ff,
  focus = 0x2f3a52ff, focus_edge = 0xeec254ff, track = 0x2a3040ff, fill = 0xeec254ff, check = 0x4cc35aff, sep = 0x2e3442ff,
}

local function call(win, what, fn, ...)
  if not fn then return end
  local ok, err = pcall(fn, ...)
  if not ok then A.report(win.mod, "gui " .. what, err) end
end
local function text_of(v) if type(v) == "function" then local ok, r = pcall(v) return ok and tostring(r) or "?" end return tostring(v) end

-- gui.window{title = "...", w = 560 (px at 1080 lines), h = max height, x / y (default: centred in the interface box)}
function gui.window(t, mod)
  t = t or {}
  local w = setmetatable({title = t.title or "", w = t.w or 560, maxh = t.h or 760, x = t.x, y = t.y, items = {},
                          focus = 1, scroll = 0, open_ = false, mod = mod, on_close = t.on_close}, Window)
  return w
end
local function add(w, it) it.win = w w.items[#w.items + 1] = it return it end
function Window:label(text) return add(self, {kind = "label", text = text}) end
function Window:separator() return add(self, {kind = "sep"}) end
-- image(img, height): a picture (image.load), centred, that tall (px at 1080 lines; default its own height)
function Window:image(img, h) return add(self, {kind = "image", img = img, ih = h}) end
function Window:button(text, fn) return add(self, {kind = "button", text = text, fn = fn, focusable = true}) end
function Window:checkbox(text, value, fn) return add(self, {kind = "check", text = text, value = value and true or false, fn = fn, focusable = true}) end
-- slider(text, min, max, value, fn(v), {step = 1, format = "%g"})
function Window:slider(text, lo, hi, value, fn, o)
  o = o or {}
  return add(self, {kind = "slider", text = text, lo = lo, hi = hi, value = value or lo, fn = fn, step = o.step or (hi - lo) / 20,
                    format = o.format or "%g", focusable = true})
end
-- choice(text, options, index, fn(i, option)): left / right (or a click) go through the options
function Window:choice(text, options, index, fn)
  return add(self, {kind = "choice", text = text, options = options, index = index or 1, fn = fn, focusable = true})
end
-- list(options, fn(i, option), {selected = i}): one row each; A / a click picks
function Window:list(options, fn, o)
  local first
  for i, opt in ipairs(options) do
    local it = add(self, {kind = "row", text = opt, index = i, fn = fn, focusable = true, list = options})
    first = first or it
  end
  if o and o.selected and first then self.selected_row = o.selected end
  return first
end
-- input(text, value, fn(v), {max = 60}): the runtime's text dialog
function Window:input(text, value, fn, o)
  return add(self, {kind = "input", text = text, value = value or "", fn = fn, max = o and o.max or 60, focusable = true})
end
function Window:clear() self.items, self.focus, self.scroll = {}, 1, 0 return self end
function Window:is_open() return self.open_ end
function Window:open()
  if self.open_ then return self end
  self.open_ = true
  stack[#stack + 1] = self
  self.focus = 1
  while self.items[self.focus] and not self.items[self.focus].focusable do self.focus = self.focus + 1 end
  self.mx, self.my = rt.input_mouse()  -- (only the pointer moving afterwards picks what's under it)
  self.scroll = 0
  G.__gui_open = true
  rt.input_grab(true)
  return self
end
function Window:close()
  if not self.open_ then return self end
  self.open_ = false
  for i = #stack, 1, -1 do if stack[i] == self then table.remove(stack, i) end end
  G.__gui_open = #stack > 0
  if #stack == 0 then rt.input_grab(false) end
  call(self, "on_close", self.on_close, self)
  return self
end
function Window:toggle() if self.open_ then self:close() else self:open() end return self end

-- ---------------------------------------------------------------- layout and drawing
local function metrics()
  local bx, by, bw, bh = draw.box()
  local s = bh / 1080
  return bx, by, bw, bh, s
end
local function item_h(it, s)
  if it.kind == "sep" then return 14 * s end
  if it.kind == "label" then return 40 * s end
  if it.kind == "row" then return 44 * s end
  if it.kind == "image" then return ((it.ih or (it.img and it.img.h) or 64) + 12) * s end
  return 52 * s
end
local function layout(w)
  local bx, by, bw, bh, s = metrics()
  local W = min(w.w * s, bw - 40 * s)
  local head, pad = 58 * s, 14 * s
  local content = 0
  for _, it in ipairs(w.items) do content = content + item_h(it, s) end
  local H = min(head + pad * 2 + content, w.maxh * s, bh - 40 * s)
  local X = w.x and bx + w.x * s or bx + (bw - W) / 2
  local Y = w.y and by + w.y * s or by + (bh - H) / 2
  local L = w.L or {}
  L.X, L.Y, L.W, L.H, L.head, L.pad, L.s, L.view, L.content = X, Y, W, H, head, pad, s, H - head - pad * 2, content
  w.L = L
  return L
end
local function keep_focus_visible(w)
  local L = w.L
  if not L then return end
  local first = 1
  while w.items[first] and not w.items[first].focusable do first = first + 1 end
  if w.focus == first then w.scroll = 0 return end  -- (back at the top: what's above the first one shows too)
  local y = 0
  for i, it in ipairs(w.items) do
    local h = item_h(it, L.s)
    if i == w.focus then
      if y < w.scroll then w.scroll = y end
      if y + h > w.scroll + L.view then w.scroll = y + h - L.view end
      break
    end
    y = y + h
  end
  w.scroll = max(0, min(w.scroll, max(0, L.content - L.view)))
end
local function draw_window(w, top)
  local L = layout(w)
  local s = L.s
  R(L.X + 4 * s, L.Y + 6 * s, L.W, L.H, 0x00000066, 16 * s)
  R(L.X, L.Y, L.W, L.H, C.panel, 16 * s)
  R(L.X, L.Y, L.W, L.head, C.title, 16 * s)
  R(L.X, L.Y + L.head - 16 * s, L.W, 16 * s, C.title, 0)
  T(w.title, L.X + 22 * s, L.Y + L.head / 2, 30 * s, C.gold, MID, 0)
  local cb = w.close_box or {}
  cb[1], cb[2], cb[3], cb[4] = L.X + L.W - L.head, L.Y, L.head, L.head
  w.close_box = cb
  T("×", L.X + L.W - L.head / 2, L.Y + L.head / 2, 34 * s, C.dim, MIDC, 0)
  RO(L.X, L.Y, L.W, L.H, top and C.edge or 0x2a2f3cff, 16 * s, 1.5 * s)
  local cx, cy, cw = L.X + L.pad, L.Y + L.head + L.pad, L.W - L.pad * 2
  CLIP(L.X, cy, L.W, L.view)
  local y = cy - w.scroll
  local rects = w.rects or {}
  w.rects = rects
  for i = #w.items + 1, #rects do rects[i] = nil end
  local fs = 26 * s
  for i, it in ipairs(w.items) do
    local h = item_h(it, s)
    local r = rects[i] or {}
    r[1], r[2], r[3], r[4] = cx, y, cw, h
    rects[i] = r
    if y + h >= cy and y <= cy + L.view then
      local f = top and i == w.focus and it.focusable
      if f then
        R(cx, y + 3 * s, cw, h - 6 * s, C.focus, 10 * s)
        RO(cx, y + 3 * s, cw, h - 6 * s, C.focus_edge, 10 * s, 2 * s)
      end
      local mid, k = y + h / 2, it.kind
      if k == "label" then
        T(text_of(it.text), cx + 8 * s, mid, 24 * s, C.dim, MID, 0)
      elseif k == "image" and it.img then
        local ih = (it.ih or it.img.h) * s
        local iw = ih * it.img.w / it.img.h
        IMG(it.img.id, cx + (cw - iw) / 2, y + 6 * s, iw, ih, 0xffffffff, 0, 0, 1, 1, 0)
      elseif k == "sep" then
        R(cx + 8 * s, mid - 1 * s, cw - 16 * s, 2 * s, C.sep, 0)
      elseif k == "button" then
        T(text_of(it.text), cx + cw / 2, mid, fs, C.text, MIDC, 0)
      elseif k == "check" then
        local b = 26 * s
        RO(cx + 14 * s, mid - b / 2, b, b, C.dim, 5 * s, 2 * s)
        if it.value then R(cx + 19 * s, mid - b / 2 + 5 * s, b - 10 * s, b - 10 * s, C.check, 3 * s) end
        T(text_of(it.text), cx + 54 * s, mid, fs, C.text, MID, 0)
      elseif k == "slider" then
        T(text_of(it.text), cx + 14 * s, mid, fs, C.text, MID, 0)
        local tx, tw = cx + cw * 0.45, cw * 0.55 - 90 * s
        local f01 = (it.value - it.lo) / (it.hi - it.lo)
        R(tx, mid - 4 * s, tw, 8 * s, C.track, 4 * s)
        R(tx, mid - 4 * s, tw * f01, 8 * s, C.fill, 4 * s)
        CI(tx + tw * f01, mid, 11 * s, 0xffffffff)
        T(fmt(it.format, it.value), cx + cw - 12 * s, mid, fs, C.gold, MIDR, 0)
        local tr = it.track or {}
        tr[1], tr[2] = tx, tw
        it.track = tr
      elseif k == "choice" then
        T(text_of(it.text), cx + 14 * s, mid, fs, C.text, MID, 0)
        T("‹  " .. text_of(it.options[it.index] or "") .. "  ›", cx + cw - 14 * s, mid, fs, C.gold, MIDR, 0)
      elseif k == "row" then
        local sel = w.selected_row == it.index
        T(text_of(it.text), cx + 22 * s, mid, fs, sel and C.gold or C.text, MID, 0)
        if sel then CI(cx + 10 * s, mid, 4 * s, C.gold) end
      elseif k == "input" then
        T(text_of(it.text), cx + 14 * s, mid, fs, C.text, MID, 0)
        local bx0 = cx + cw * 0.45
        R(bx0, mid - 18 * s, cw * 0.55 - 12 * s, 36 * s, 0x0d0f15ff, 6 * s)
        T(it.value ~= "" and it.value or "...", bx0 + 10 * s, mid, 24 * s, it.value ~= "" and C.text or C.dim, MID, 0)
      end
    end
    y = y + h
  end
  UNCLIP()
  if L.content > L.view then  -- (a scroll bar)
    local bh = L.view * L.view / L.content
    local by = cy + (L.view - bh) * (w.scroll / (L.content - L.view))
    R(L.X + L.W - 8 * s, by, 4 * s, bh, 0xffffff40, 2 * s)
  end
end

-- ---------------------------------------------------------------- input
local function step_focus(w, d)
  local n = #w.items
  if n == 0 then return end
  local i = w.focus
  for _ = 1, n do
    i = i + d
    if i < 1 then i = n elseif i > n then i = 1 end
    if w.items[i].focusable then w.focus = i break end
  end
  keep_focus_visible(w)
end
local function activate(w, it)
  if not it then return end
  if it.kind == "button" then call(w, it.text, it.fn, w)
  elseif it.kind == "check" then it.value = not it.value call(w, it.text, it.fn, it.value)
  elseif it.kind == "choice" then
    it.index = it.index % #it.options + 1
    call(w, it.text, it.fn, it.index, it.options[it.index])
  elseif it.kind == "row" then w.selected_row = it.index call(w, "list", it.fn, it.index, it.list[it.index])
  elseif it.kind == "input" then
    if rt.text_prompt(text_of(it.text), it.value, it.max) then
      it.waiting = true
      w.waiting = it
    end
  end
end
local function adjust(w, it, d)
  if not it then return end
  if it.kind == "slider" then
    local v = max(it.lo, min(it.hi, it.value + d * it.step))
    if v ~= it.value then it.value = v call(w, it.text, it.fn, v) end
  elseif it.kind == "choice" then
    it.index = (it.index - 1 + d) % #it.options + 1
    call(w, it.text, it.fn, it.index, it.options[it.index])
  end
end
local BIT = {A = 0, B = 1, LEFT = 12, UP = 13, RIGHT = 14, DOWN = 15}
local held, held_since, held_rep = 0, {}, {}
local pad_b, pad_t = 0, 0
local function edge(name)  -- (pressed now - or held: repeating after 0.35 s, the d-pad only)
  local bit = 1 << BIT[name]
  local down, was = pad_b & bit ~= 0, held & bit ~= 0
  if down and not was then held_since[name], held_rep[name] = pad_t, pad_t return true end
  if down and pad_t - held_since[name] > 0.35 and pad_t - held_rep[name] > 0.075 and name ~= "A" and name ~= "B" then held_rep[name] = pad_t return true end
  return false
end
local function pad_nav(w)
  pad_b, pad_t = rt.buttons(), rt.now()
  local up, down, left, right, a, back = edge("UP"), edge("DOWN"), edge("LEFT"), edge("RIGHT"), edge("A"), edge("B")
  held = pad_b
  if up then step_focus(w, -1) end
  if down then step_focus(w, 1) end
  local it = w.items[w.focus]
  if left then adjust(w, it, -1) end
  if right then adjust(w, it, 1) end
  if a then activate(w, it) end
  if back then w:close() end
end
local function in_rect(x, y, r) return r and x >= r[1] and y >= r[2] and x < r[1] + r[3] and y < r[2] + r[4] end
local drag
local function mouse_nav(w)
  local x, y = rt.input_mouse()
  if drag and drag.win == w then
    local it = drag.it
    if rt.input_mouse and select(3, rt.input_mouse()) & 1 == 0 then drag = nil
    elseif it.track then
      local f = max(0, min(1, (x - it.track[1]) / it.track[2]))
      local v = it.lo + f * (it.hi - it.lo)
      v = it.lo + floor((v - it.lo) / it.step + 0.5) * it.step
      v = max(it.lo, min(it.hi, v))
      if v ~= it.value then it.value = v call(w, it.text, it.fn, v) end
    end
  end
  if w.mx ~= x or w.my ~= y then  -- (the pointer moved: what's under it has the focus)
    w.mx, w.my = x, y
    for i, r in ipairs(w.rects or {}) do
      if w.items[i].focusable and in_rect(x, y, r) and y >= w.L.Y + w.L.head then w.focus = i break end
    end
  end
end
G.__gui_mouse = function(e)
  local w = stack[#stack]
  if not w then return false end
  if e.type == "wheel" then
    if w.L then w.scroll = max(0, min(w.scroll - e.wheel * 40 * w.L.s, max(0, w.L.content - w.L.view))) end
    return true
  end
  if e.type == "mouse_down" and e.button == "left" then
    if in_rect(e.x, e.y, w.close_box) then w:close() return true end
    for i, r in ipairs(w.rects or {}) do
      local it = w.items[i]
      if it.focusable and in_rect(e.x, e.y, r) and e.y >= w.L.Y + w.L.head and e.y <= w.L.Y + w.L.H then
        w.focus = i
        if it.kind == "slider" then drag = {win = w, it = it} else activate(w, it) end
        break
      end
    end
    return true
  end
  if e.type == "mouse_down" and e.button == "right" then w:close() return true end
  return true  -- (the menu's: not the scripts' or the game's)
end
G.__gui_key = function(e)
  local w = stack[#stack]
  if not w then return false end
  if e.down and e.key == "Escape" then w:close() end
  return e.key == "Escape" or e.key == "Tab"
end
G.__gui_draw = function()
  for i, w in ipairs(stack) do draw_window(w, i == #stack) end
  local w = stack[#stack]
  if not w then return end
  if w.waiting then  -- (the text dialog is up: its answer)
    local st, text = rt.text_prompt_poll()
    if st ~= 0 then
      local it = w.waiting
      w.waiting, it.waiting = nil, false
      if st == 1 then it.value = text call(w, it.text, it.fn, text) end
    end
    return
  end
  pad_nav(w)
  mouse_nav(w)
end

-- each mod's gui (its windows report errors as its)
local base_env = G.__mod_env
G.__mod_env = function(name, dir)
  local env = base_env(name, dir)
  if name ~= "" then
    local mod = A.mods[name]
    env.gui = {window = function(t) return gui.window(t, mod) end, close_all = function() for i = #stack, 1, -1 do if stack[i].mod == mod then stack[i]:close() end end end}
  end
  return env
end
G.gui = {window = function(t) return gui.window(t, A.shared) end, close_all = function() for i = #stack, 1, -1 do stack[i]:close() end end}
local base_unload = G.__unload
G.__unload = function() for i = #stack, 1, -1 do stack[i].open_ = false end stack = {} G.__gui_open = false base_unload() end
