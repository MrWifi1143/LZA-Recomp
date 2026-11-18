-- Z-A research (Legends Lua): why a Link Play join over a lagged online server sometimes ends in "No one could be
-- found!" (2026-11-17, za-mp17). Copy into <game folder>/lua/ on both copies (with za_pia_steps.lua for Pia's jobs).
--
--   Both games run a connect task (vtable 0x7003d88680, made by 0x7000c9fd60; +0xb8 the players it needs: 2 for a
--   trade). Its update 0x7000b8c4e8: +0xb4 state 0 waits for its child (+0x40, vtable 0x7003d871b0: the search / host
--   cycle and the Pia join) to report 1 in child+0x48; state 1 then needs the session's player count
--   ([*0x70040fc390] + 0x110, read by 0x700253e990) >= +0xb8 - and ends with result 3 AT ONCE when it isn't (the only
--   writer of 3: 0x7000b8c650), which makes the game leave (0x70009628fc ... 0x70019a72a0: LeaveSession).
--   That give-up is not impatience: the count drops 2 -> 1 because the HOST left. The host keeps each room up ~2.3-3.4 s
--   and then leaves to search again (LEAVEASK from 0x70019b7360, x1 = the name of Pia's
--   ResultConst<HandlingType_Cleanup, ..., Description_SessionIsNotFound>), even while a station that joined the ldn
--   network late in that window is still joining the session (its Pia join request lost / late). The joiner completes
--   JoinSession during the host's LeaveMeshWithHostMigration (count 2), then the host is gone (count 1 -> 0).
--   Tried and dropped: letting the joiner's state 1 wait up to 10 s for the count - it never came back (0 of 2 after
--   10 s, the trade 69 s instead of ~41 s with the immediate give-up and a rejoin). The fix is on the host: its room
--   window is the "wait for players" job 0x700199b0d0 (za_room_window.lua), held while an ldn station is still joining
--   (runtime/hle/za_fixes.c, 2026-11-17).
local last = {}
local function count() local g = mem.u64(0x70040fc390) or 0; if g == 0 then return -1 end return mem.u32(g + 0x110) or -2 end
hook(0x7000b8c4e8, function(c)
  local t = c:x(0)
  if mem.u64(t) ~= 0x7003d88680 then return end
  local ch = mem.u64(t + 0x40) or 0
  local s = string.format("state %d result %d need %d count %d child result %d", mem.u32(t + 0xb4) or -1, mem.u32(t + 0x48) or -1,
    mem.u16(t + 0xb8) or -1, count(), ch ~= 0 and (mem.u32(ch + 0x48) or -1) or -1)
  if last[t] ~= s then last[t] = s; log(string.format("JOINTASK %x f%d %s", t, game.frame(), s)) end
  c:original()
  if (mem.u32(t + 0x48) or -1) == 3 then log(string.format("GIVEUP %x f%d need %d count %d", t, game.frame(), mem.u16(t + 0xb8) or -1, count())) end
  return true
end)
for _, a in ipairs({0x70019a72a0, 0x7001a2a930}) do  -- a leave being queued: who (lr 0x70019b7368 = the host's room ending)
  hook(a, function(c)
    local b = {}
    for _, v in ipairs(c:bt(12)) do b[#b + 1] = string.format("%x", v) end
    log(string.format("LEAVEASK %x f%d x0 %x x1 %x lr %x bt %s", a, game.frame(), c:x(0), c:x(1), c.lr, table.concat(b, " ")))
  end)
end
local lc = -9
on_frame(function(f) local n = count(); if n ~= lc then lc = n; log(string.format("SESSCOUNT f%d %d", f, n)) end end)
log("za_join_task.lua on")
