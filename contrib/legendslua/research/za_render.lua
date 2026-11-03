-- Z-A research (Legends Lua): the renderer - render passes and their queues, who asks for the performance mode,
-- render targets and viewports of a given size (with hardware watchpoints on who sets them). Copy into
-- <game folder>/lua/ with Legends Lua switched on; set what you want below (saving reloads it). Was LARECOMP_ZA_PASSQ /
-- PERFLOG / RTLOG / RTWATCH / VPWATCH in the runtime. Addresses: Z-A 2.0.2.
local PASSQ = false     -- each render pass's queue walk 0x700180ee10: the pass and its queue length (each pass once / 600 frames)
local PERFLOG = false   -- who asks nn::oe::GetPerformanceMode (sdk 0x70083f70e0), each caller once; the pipeline's per-mode sizes
local RTLOG = false     -- render target creation 0x700044e390 (?, desc, ..): the descriptions holding 1920
local RTWATCH = nil     -- a width (e.g. 1920): the first render target that wide gets a watchpoint on its width
local VPWATCH = nil     -- a width: the first viewport that wide (state + 0xc4 + 8) gets a watchpoint

local function inguest(a) return a and a >= 0x7000000000 and a < 0x8000000000 end

if PASSQ then
  local seen, last = {}, -1
  hook(0x700180ec2c, function(c)
    local f, p = game.frame(), c:x(0)
    if f % 600 == 0 and f ~= last then last = f; seen = {} end
    if seen[p] then return end
    seen[p] = true
    local b, e, nm = mem.u64(p + 0x168), mem.u64(p + 0x170), mem.u64(p + 0x40)
    log(string.format("f%d pass %x %s queue %d items (lr %x)", f, p, inguest(nm) and mem.str(nm, 40) or "?", (e - b) // 16, c.lr))
  end)
end

if PERFLOG then
  local seen = {}
  hook(0x70083f70e0, function(c)
    if seen[c.lr] then return end
    seen[c.lr] = true
    log(string.format("GetPerformanceMode from %x (its caller %x)", c.lr, c:bt(1)[1] or 0))
  end)
  local n = 0
  hook(0x7000ed2dc0, function(c)  -- (the pipeline: +0x288 + mode * 4 width, +0x290 + mode * 4 height, mode at +0x2b4)
    n = n + 1
    if n > 4 then return end
    local p = c:x(0)
    log(string.format("pipeline %x sizes: mode 0 %dx%d, mode 1 %dx%d (mode %d)", p, mem.u32(p + 0x288), mem.u32(p + 0x290),
      mem.u32(p + 0x28c), mem.u32(p + 0x294), mem.i32(p + 0x2b4)))
  end)
end

if RTLOG then
  local n = 0
  hook(0x700044e390, function(c)
    if n >= 30 then return end
    local d = c:x(1)
    for i = 0, 23 do
      if mem.u32(d + 4 * i) == 1920 then
        n = n + 1
        local w = {}
        for k = 0, 11 do w[#w + 1] = string.format("%x", mem.u32(d + 4 * k)) end
        log(string.format("rt create from %x: desc %x, 1920 at +0x%x: %s", c.lr, d, i * 4, table.concat(w, " ")))
        break
      end
    end
  end)
end

if RTWATCH then
  -- the render target lookups: 0x70018a4da4 (.., handle) and 0x70018b7088 (state, slot, handle): the handle
  -- {u8 valid, u64 object}, the object's +0x68 / +0x70 the size the viewport is set to
  local armed, nlog = false, 0
  local function bind(c)
    local hnd = c.addr == 0x70018b7088 and c:x(2) or c:x(1)
    local o = inguest(hnd) and mem.u8(hnd) ~= 0 and mem.u64(hnd + 8) or nil
    if not inguest(o) then return end
    if nlog < 30 then nlog = nlog + 1; log(string.format("bind %x: target %x size %d x %d", c.addr, o, mem.u64(o + 0x68), mem.u64(o + 0x70))) end
    if not armed and mem.u64(o + 0x68) == RTWATCH then
      armed = true
      log(string.format("render target %x: size %d x %d - watching its width", o, mem.u64(o + 0x68), mem.u64(o + 0x70)))
      watch(o + 0x68, 8)
    end
  end
  hook(0x70018b7088, bind)
  hook(0x70018b71a4, bind)
end

if VPWATCH then
  local armed = false
  hook(0x70018b7390, function(c)  -- (the render state's viewport flush: viewports at x0 + 0xc4)
    local w = mem.f32(c:x(0) + 0xc4 + 8)
    if not armed and w == VPWATCH then
      armed = true
      log(string.format("viewport %g in state %x: watching", w, c:x(0)))
      watch(c:x(0) + 0xc4 + 8, 4)
    end
  end)
end

log("za_render research hooks on")
