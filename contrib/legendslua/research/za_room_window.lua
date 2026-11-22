-- Z-A research (Legends Lua): the Link Play host's room window (2026-11-17, za-mp18). Copy into <game folder>/lua/.
--
--   Looking for a partner the game looks for a room (a session browse - its continuation 0x70019b71e0 gets Pia's
--   SessionIsNotFound and goes on: "NotMatching") and, finding none, opens its own and waits for the others in a job:
--   0x700199b0d0(job) - +0x50 u16 players needed, +0x58 double ms left (starts at ~2.9-4 s), +0x60 double ms added per
--   player who comes (5000), +0x68 u16 players counted (Pia's session count, 0x700253e990), +0x6a set once this copy
--   hosts (its clock runs only then). At 0 it fails with Pia's Timeout: the continuation leaves (0x70019a72a0 from
--   0x70019b7368 -> LeaveSession, LeaveMeshWithHostMigration) and the cycle starts over. A station in our ldn network
--   whose Pia join is still under way isn't counted: the host left under it (runtime/hle/za_fixes.c now holds the clock
--   while one is; LARECOMP_ROOM_HOLD=0 = the game's own window).
--   WAITJOB = the job's fields when they change, ERRTYPE = the error each continuation got (0x7000c0fc7c compares the
--   error's type name through 0x7003160740), LEAVEASK = a leave being queued.
local last = {}
hook(0x700199b0d0, function(c)
  local j = c:x(0)
  local s = string.format("host %d need %d counted %d", mem.u8(j + 0x6a) or -1, mem.u16(j + 0x50) or -1, mem.u16(j + 0x68) or -1)
  if last[j] ~= s then last[j] = s; log(string.format("WAITJOB %x f%d %s left %.0f ms per %.0f", j, game.frame(), s, mem.f64(j + 0x58) or -1, mem.f64(j + 0x60) or -1)) end
end)
hook(0x7003160740, function(c)
  if c.lr == 0x7000c0fcc0 then log(string.format("ERRTYPE f%d got '%s'", game.frame(), (mem.str(c:x(0), 300) or "?"):match("Description_(%w+)") or "?")) end
end)
for _, a in ipairs({0x70019a72a0, 0x7001a2a930}) do
  hook(a, function(c)
    local b = {}
    for _, v in ipairs(c:bt(6)) do b[#b + 1] = string.format("%x", v) end
    log(string.format("LEAVEASK %x f%d lr %x bt %s", a, game.frame(), c.lr, table.concat(b, " ")))
  end)
end
log("za_room_window.lua on")
