-- Z-A research (Legends Lua): the walk-up trade's link test and Pia's reliable window under it - how the "boxed" trades
-- were traced (2026-11-17, za-mp19: both copies in the trade box, one never shows the other's name). Copy into
-- <game folder>/lua/ on both copies. Z-A 2.0.2.
--
--   Once linked, each copy sends a fixed 1211-byte block (game header: channel 1, kind 0) over Pia's ReliableProtocol
--   every frame for a few seconds - as fast as Pia takes them - then one 354-byte result (channel 1, kind 1); the other
--   copy shows our name (GetService ngc:u) once the result came. The game's send 0x7001a77f6c(this, data, size,
--   reliable, &header u16, station) asks Pia first (0x7001a776f0 -> ReliableProtocol vtable +0x98) and returns false
--   when Pia's sliding window (64 messages) is full. The result is sent once (0x700095f8f4 -> 0x7000960aac) and dropped
--   whatever the send said: at a full window it is never sent. On a lagged link the blocks keep the window full for
--   seconds (the first reliable message often needs 5-14 sends: no resend timer until an RTT is known, then
--   1.4 x RTT + base). Fixed in runtime/hle/za_fixes.c (link_send_hook: blocks held back near a full window).
--
--   Logs: LINKSEND (game sends: header, size, sent / NOT SENT, caller chain the first times), LINKMSG (what Pia took /
--   handed over: size, FNV-1a, first bytes), RSW (the ReliableProtocol's window +0x1a0: messages in it, its head seq, each
--   message's sends and resend due in ms).
local function hx(a, n) local t = {} for i = 0, n - 1 do t[#t + 1] = string.format("%02x", mem.u8(a + i) or 0) end return table.concat(t) end
local n, nf = {}, {}
hook(0x7001a77f6c, function(c)
  local hdr, size, bt = mem.u16(c:x(4)) or 0, c:x(2) & 0xffffffff, c:bt(8)
  c:original()
  local ok = c:x(0) & 1
  n[hdr] = (n[hdr] or 0) + 1
  if ok == 0 then nf[hdr] = (nf[hdr] or 0) + 1 end
  if hdr ~= 0x0001 or n[hdr] <= 2 or (ok == 0 and nf[hdr] <= 2) or n[hdr] % 50 == 0 then
    local t = {}
    if n[hdr] <= 2 or ok == 0 then for _, v in ipairs(bt) do t[#t + 1] = string.format("%x", v) end end
    log(string.format("LINKSEND f%d header %04x size %d -> %s (#%d, %d not sent) %s", game.frame(), hdr, size,
      ok == 1 and "sent" or "NOT SENT", n[hdr], nf[hdr] or 0, table.concat(t, " ")))
  end
  return true
end)
hook(0x700256f468, function(c)  -- ReliableProtocol::Send(this, station, data, size)
  local d, sz = c:x(2), c:x(3) & 0xffffffff
  if (mem.u16(d) or 0) ~= 0x0001 then log(string.format("LINKMSG out f%d size %d %s", game.frame(), sz, hx(d, math.min(sz, 16)))) end
end)
hook(0x700256f630, function(c)  -- ReliableProtocol::Receive(this, station out, buffer, size*, max; result in x8)
  local buf, szp, r8 = c:x(2), c:x(3), c:x(8)
  c:original()
  local sz = mem.u32(szp) or 0
  if (mem.u32(r8) or 1) == 0 and sz > 0 and (mem.u16(buf) or 0) ~= 0x0001 then
    log(string.format("LINKMSG in f%d size %d %s", game.frame(), sz, hx(buf, math.min(sz, 16))))
  end
  return true
end)
local last = {}
hook(0x7002571e30, function(c)  -- ReliableSlidingWindow update / send
  local o = c:x(0)
  c:original()
  if mem.u64(o) ~= 0x7003c8ea40 then return true end
  local cnt, head, ring, size = mem.u32(o + 0x28) or 0, mem.u32(o + 0x2c) or 0, mem.u64(o + 0x30) or 0, mem.u32(o + 0x38) or 1
  local s = string.format("in window %d head seq %04x", cnt, mem.u16(o + 0x6c) or 0)
  if cnt > 0 then
    local e = ring + (head % size) * 0x5b8
    local tpm = mem.u64(0x70040fae68) or 0
    local due = mem.u64(e + 8) or 0
    s = s .. string.format(" first: sends %d due %s", mem.u16(e + 0x14) or 0,
      (due == 0 or tpm == 0) and "-" or string.format("%d ms", (due - c:call(0x7003160900)) // tpm))
  end
  if last[o] ~= s:gsub(" due .*", "") then last[o] = s:gsub(" due .*", ""); log(string.format("RSW %x f%d %s", o, game.frame(), s)) end
  return true
end)
log("za_link_test.lua on")
