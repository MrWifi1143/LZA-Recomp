-- Z-A research (Legends Lua): Pia's (Link Play's network library) jobs, step by step - how the walk-up trade's "joined, then
-- No one could be found!" was traced (2026-11-17). Copy into <game folder>/lua/ with Legends Lua switched on, on both
-- copies; best with LARECOMP_BSD_TRACE=1 (every game packet with its time and FNV-1a) and LARECOMP_LDN_LOG=1. Z-A 2.0.2.
--
--   A Pia job runs its current step - a member function at job+0x38 (+0x40 its this-adjustment), its name string at
--   job+0x48, the step before at +0x58/+0x68 - from 0x70024f4eb0(job), until a step returns done: w0 low byte 1 done,
--   4 sleep (w0 >> 16 = ms: 0x01f40004 = 500 ms), 5 again next frame. The joiner's session comes up as
--   NetBackgroundProcessJob::StartConnectNetwork -> JoinMeshJob::WaitJoinResponse -> ::WaitUpdateSessionMessage ->
--   ::CompleteProcess -> JoinSessionJobBase::CompleteProcess; the host then UpdatePlayerNum -> ChangeNetworkState ->
--   NetNetworkStateJob::WaitSendUpdateSessionProperty -> OpenCloseParticipationJob::WaitP2pStable. Lost steps are sent
--   again only after 0.5-1 s (NetResendMessageJob sleeps 500 ms).
--   The game itself leaves a session that isn't up in time: LeaveSessionJob's start 0x70025419d4 (and LeaveMeshJob's,
--   0x7002557da8) called from its event queue (0x7001918a10 -> 0x7001a1ef80 -> 0x7001a1f040 -> vtable +0x68 ->
--   0x700253e4a0) - the decision is queued earlier, not found.
local LOG_EVERY_DONE = false  -- also each "done" of a step that didn't change (busy: periodic jobs finish every frame)
local last = {}
hook(0x70024f4eb0, function(c)
  local job = c:x(0)
  local before = mem.u64(job + 0x48)
  c:original()
  local after = mem.u64(job + 0x48)
  local r = c:x(0)
  local lo = r & 0xff
  if before ~= after or lo == 1 then
    local s = string.format("%s -> %s%s", mem.str(before) or "?", mem.str(after) or "?", lo == 1 and " [done]" or "")
    if last[job] ~= s or LOG_EVERY_DONE then
      last[job] = s
      log(string.format("PIA %x f%d %s (w0 %08x)", job, game.frame(), s, r & 0xffffffff))
    end
  end
  return true
end)
for _, a in ipairs({0x70025419d4, 0x7002557da8}) do  -- a leave starting: who (the caller chain)
  hook(a, function(c)
    local t = {}
    for _, v in ipairs(c:bt(16)) do t[#t + 1] = string.format("%x", v) end
    log(string.format("PIALEAVE %x f%d job %x lr %x bt %s", a, game.frame(), c:x(0), c.lr, table.concat(t, " ")))
  end)
end
log("za_pia_steps.lua on")
