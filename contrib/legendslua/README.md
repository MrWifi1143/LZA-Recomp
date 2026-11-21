# Legends Lua

Lua 5.3 scripting for the recompiled games (Pokémon Legends: Z-A and Legends: Arceus). It's a runtime plugin: hook
any game function, read and write game memory, call the game's own functions and run something every frame. Built
for research and for mods that need code.

**Only run scripts you trust.** A script can do anything the game can: change your save, crash the game, or read any
of its memory.

## Turning it on

1. The plugin ships in `plugins/` next to the game (`liblegendslua.so` on Linux, `legendslua.dll` on Windows). To
   build it yourself: `make legendslua`.
2. Switch it on in the start screen: **Mods → Legends Lua**. (Or in `larecomp.ini`: `plugins_on = liblegendslua.so`.
   Use the plugin's *file name*.)
3. Put scripts in `<game folder>/lua/*.lua`. They run in name order as the game starts, before the game's first
   instruction, so boot-time functions can be hooked too.
4. Saving a script reloads it (hot reload): its hooks are dropped and it runs again.

The log (`lasrecomp.log` / `play.log`) shows `[lua] running <name>.lua` and anything a script prints.

## The live console

Write Lua to `<dir>/lua_run.lua`, where `<dir>` is `LARECOMP_LIVE` if set, else the game folder. It runs once in the
running game and is then removed. What it prints goes to the log and to `<dir>/lua_out.txt`, so you can poke a
running game from a shell:

```sh
echo 'log(string.format("%x", mem.u64(0x7006159000)))' > live/lua_run.lua; sleep 2; cat live/lua_out.txt
```

## API

Addresses are the running game's (main at `0x7000000000`). The recompiled functions are named `f_<address>`, so a
function you find in a disassembly can be hooked at its address directly.

| Call | What it does |
|---|---|
| `log(...)` / `print(...)` | to the log, as `[lua] ...` |
| `hook(addr, fn)` | `fn(c)` runs when the game calls `addr`. Return `true` to skip the game's function (set `c`'s x0 first). Several hooks on one address run in order. `hook(addr, nil)` drops them. |
| `on_frame(fn)` | `fn(frame)` every presented frame (render thread: memory yes, game calls no) |
| `c:x(n)` `c:setx(n, v)` | registers x0..x30 (64-bit integers) |
| `c:s(n)` `c:sets(n, v)` `c:d(n)` `c:setd(n, v)` | float / double in v0..v31 |
| `c:vf(n, lane)` `c:setvf(n, lane, v)` | one float lane of a vector register |
| `c.lr` `c.sp` `c.addr` | return address, stack pointer, the hooked function |
| `c:original()` | run the game's function now; its result is then in `c`'s x0 / v0 (return `true` afterwards so it doesn't run twice) |
| `c:call(addr, x0, x1, ...)` | call a game function from inside a hook; returns its x0 |
| `c:bt([n])` | the caller chain (frame pointers): a table of return addresses |
| `mem.u8/u16/u32/u64/i32/f32/f64(addr)` | reads; `nil` when not readable |
| `mem.w8/w16/w32/w64/wf32/wf64(addr, v)` | writes; `false` when not writable |
| `mem.str(addr[, max])` `mem.bytes(addr, n)` `mem.valid(addr, n)` `mem.dump(addr, n)` | strings, raw bytes, checks, hex dumps |
| `mem.find(lo, hi, bytes[, max])` | every address in `[lo, hi)` holding `bytes` (build them with `string.pack`) |
| `game.title` `game.root` `game.frame()` | program id, game folder, frames presented |
| `watch(addr[, len])` `watch_reset()` | hardware watchpoints (Linux, up to 4): each writer is logged as `[hwwatch]`. `LARECOMP_HWWATCH_RW=1`: readers too. `LARECOMP_HWWATCH_ALL=<n>`: every hit (up to n) with its frame, not only each instruction's first. |

Hooks run on the game's own threads, one at a time (one lock): keep them short. There's no `io` / `os` / `package`
library.

## Recipes

**What does a function get called with?**

```lua
local n = 0
hook(0x70011407b0, function(c)
  n = n + 1
  if n <= 20 then log(string.format("x0 %x x2 %d s0 %g s1 %g from %x", c:x(0), c:x(2), c:s(0), c:s(1), c.lr)) end
end)
```

**Change an argument, or a value a function reads, for that call only:**

```lua
hook(0x70011407b0, function(c)
  if c:x(2) == 6 then c:sets(0, c:s(0) * 3) end  -- runs before the game's function, which then sees the new value
end)
```

**Who calls this?** `c:bt(16)` gives the return addresses; find the functions containing them in a disassembly. Code
that runs as a job ends its chain in the job runner. Then look for the list or queue the job reads and watch who
writes it.

**What changes in an object from frame to frame?** Snapshot it in `on_frame` and log the fields that change: a
counter stepping by one per frame stands out at once. That's how the battle openings' frame counter in Arceus was
found.

**Who writes this field?** `watch(addr, 4)`. For memory that gets reused (pooled objects), watch it after the object
is gone: the next object built there shows its constructor.

**Where is this value?** `mem.find(0x7100000000, 0x7400000000, string.pack("<f", 30), 100000)` lists every float 30 in
the game's heap. Patch them in halves to find which one matters. Values loaded with a level are only in memory after
it loads, so patch after that. Check side effects too: changing data you don't understand can make the game loop
(e.g. the same character created over and over).

**Profile one window of play:** with `LARECOMP_FNPROF_FILE=<path>`, creating `<path>.on` starts counting every game
function's calls and `<path>.off` writes them (to `LARECOMP_FNPROF_OUT`). Compare a window where something happens
with one where it doesn't.

## Research scripts

`research/` has the scripts used to find bugs in each game (copy one into `lua/` and set what you want at its top),
e.g. `za_streaming.lua` (what streams in where), `za_render.lua` (render passes and targets), `za_npc_bodies.lua`
(characters as they are created and removed, with your distance to them), and in Arceus `pla_research.lua`.
