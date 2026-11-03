-- Z-A research (Legends Lua): what streams in where - collision shapes made, NPC spawn triggers, area streaming offsets,
-- streaming groups, the realize jobs. Copy into <game folder>/lua/ with Legends Lua switched on; set what you want below
-- (saving reloads it). Was LARECOMP_ZA_COLLOG / COLBT / TRIG / AREALOG / REALIZE / SGDIST / SGFLOOR / JOBLOG in the
-- runtime. Addresses: Z-A 2.0.2. How the rock pop-in was found with these: ZARecomp 14d2447.
local COLLOG = true      -- each collision shape made: its name, type, params, frame
local COLBT_FROM = nil   -- a frame number: the call stack of the first 12 shapes made from then on
local TRIG = nil         -- {"OuterPlayerTrigger", 3}: that trigger's radius times 3 (its scene value kept)
local AREALOG = true     -- each area's streaming offsets (grid / sensor / conditional realize, unrealize)
local REALIZE = nil      -- a number of metres added to all six offsets
local SGLOG = true       -- each streaming group's realize / unrealize distances
local SGDIST = nil       -- a factor for every group's distances
local SGFLOOR = nil      -- metres: no group realizes nearer than this (unrealize 15 m further)
local JOBLOG_FROM = nil  -- a frame number: the realize jobs run from then on (their task's vtable and strings)

-- FlatBuffers in game memory: a table's field address (nil when absent), what a reference points to
local function fb_field(t, idx)
  if not t then return nil end
  local vt = t - (mem.i32(t) or 0)
  local vsize = mem.u16(vt) or 0
  if 4 + 2 * idx + 2 > vsize then return nil end
  local off = mem.u16(vt + 4 + 2 * idx)
  return off ~= 0 and t + off or nil
end
local function fb_ref(f) return f and f + mem.u32(f) or nil end

-- The collision component factory 0x7001108570 (.., x2 -> its FlatBuffer: field 1 shape {0 type, 1 params
-- {1 radius, 2 height}, 9 name})
local made, bts, trig_orig = 0, 0, {}
hook(0x7001108570, function(c)
  local t = fb_ref(mem.u64(c:x(2)))
  local shape = fb_ref(fb_field(t, 1))
  local nm = fb_ref(fb_field(shape, 9))
  local name = nm and mem.str(nm + 4, 80) or "-"
  local pr = fb_ref(fb_field(shape, 1))
  if COLLOG then
    local v = {}
    for i = 0, 3 do local f = fb_field(pr, i); v[i + 1] = f and mem.f32(f) or 0 end
    local ty = fb_field(shape, 0)
    log(string.format("col %s type %d params %g %g %g %g (frame %d)", name, ty and mem.u8(ty) or 255, v[1], v[2], v[3], v[4], game.frame()))
  end
  if COLBT_FROM and game.frame() >= COLBT_FROM and bts < 12 then
    bts = bts + 1
    local s = {}
    for _, a in ipairs(c:bt(16)) do s[#s + 1] = string.format("%x", a) end
    log(string.format("col bt lr %x: %s", c.lr, table.concat(s, " ")))
  end
  if TRIG and name == TRIG[1] then
    local r = fb_field(pr, 1)
    if r then
      trig_orig[r] = trig_orig[r] or mem.f32(r)
      mem.wf32(r, trig_orig[r] * TRIG[2])
    end
  end
end)

-- 0x70008f0680 reads an area's FieldAreaInfo: its streamingConfig lands at +0x108..+0x11c
local areas = 0
hook(0x70008f0680, function(c)
  local a = c:x(0)
  c:original()
  local o = a + 0x108
  if AREALOG and areas < 64 then
    areas = areas + 1
    log(string.format("area %x streaming offsets: grid %g / %g, sensor %g / %g, conditional %g / %g", a,
      mem.f32(o), mem.f32(o + 4), mem.f32(o + 8), mem.f32(o + 12), mem.f32(o + 16), mem.f32(o + 20)))
  end
  if REALIZE then for i = 0, 5 do mem.wf32(o + 4 * i, mem.f32(o + 4 * i) + REALIZE) end end
  return true
end)

-- A scene's GridStreamingGroup: the factory 0x70010e4ae0 (result via x8: u8 made, the group at +8) sets +0x240
-- realize, +0x244 unrealize. (What Lumiose's props really follow is the OverrideSensorData next to it - the runtime's
-- Detail factor handles that: za_fixes.c sensor_make.)
local groups = 0
hook(0x70010e4ae0, function(c)
  local res = c:x(8)
  c:original()
  if res == 0 or mem.u8(res) == 0 then return true end
  local g = mem.u64(res + 8)
  if not g or g == 0 then return true end
  local d0, d1 = mem.f32(g + 0x240), mem.f32(g + 0x244)
  if SGLOG and groups < 40 then groups = groups + 1; log(string.format("streaming group %x: realize %g m, unrealize %g m", g, d0, d1)) end
  if SGDIST then d0, d1 = d0 * SGDIST, d1 * SGDIST end
  if SGFLOOR then d0 = math.max(d0, SGFLOOR); d1 = math.max(d1, d0 + 15) end
  mem.wf32(g + 0x240, d0); mem.wf32(g + 0x244, d1)
  return true
end)

-- The realize jobs 0x70010e1540 runs (x0: +0x10 the task): its vtable and the C strings its first words point to
local jobs = 0
hook(0x70010e1540, function(c)
  if not JOBLOG_FROM or game.frame() < JOBLOG_FROM or jobs >= 40 then return end
  local t = mem.u64(c:x(0) + 0x10)
  if not t or t == 0 then return end
  jobs = jobs + 1
  local s = {string.format("task %x vt %x:", t, mem.u64(t))}
  for i = 1, 23 do
    local q = mem.u64(t + 8 * i)
    if q and q >= 0x7000000000 and q < 0x7400000000 and q % 8 == 0 then
      local str = mem.str(q, 40)
      if #str >= 4 and str:match("^[%g ]+$") then s[#s + 1] = string.format('+%x "%s"', i * 8, str) end
    end
  end
  log(string.format("job (frame %d) %s", game.frame(), table.concat(s, " ")))
end)

log("za_streaming research hooks on")
