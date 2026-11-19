# Lua mods: the mod API

Mods can carry Lua scripts. A script can make **new characters** that the game itself creates (they look, move and
animate like the game's own people), dress them, pose them, walk them around, make **Pokémon**, give **items**, set the
**time of day**, read and set the story's progress values, run the game's **own script commands** (Z-A: message
windows, its objects, everything its events use), react to the player, add console commands, show notices on screen
and hook anything in the game. The API is the same in **Legends: Z-A** and **Legends: Arceus**. Where the two
games differ, this page says so.

The scripts run in **Legends Lua** (`contrib/legendslua`), a Lua 5.3 plugin that ships in the game's `plugins/` folder
(`liblegendslua.so` on Linux, `legendslua.dll` on Windows).
You don't have to switch it on. When a mod that carries scripts is on, the game loads Legends Lua for it. A game with no
such mod doesn't load it at all, so it costs nothing.

**Only run mods you trust.** A script can do anything the game can do: change your save, crash the game, or read any of
its memory.

## A mod with scripts

```
<game folder>/mods/<name>/
    lua/            every *.lua here runs, in name order, as the game starts
        lib/        (anything in a folder below lua/ runs only when a script includes it)
    romfs/          (optional: the files a mod replaces, as usual)
```

Switch the mod on in the start screen (**Mods**), or start it from Legends Studio (**Play**). A mod can be scripts alone
(just `lua/`), and it still shows on the Mods page.

* Each mod has **its own globals**. A variable one mod sets never overwrites another mod's. Everything below (`npc`,
  `on`, `timer` and so on) is there in every mod.
* **Saving a script reloads everything.** The running game drops everything the scripts made (characters, handlers,
  timers, commands, hooks) and runs the scripts again, within half a second. You can edit while you play.
* **Errors never stop the game.** An error is written to the game's log (`lasrecomp.log` / `play.log`, lines starting
  `[lua]`) with where it happened. If a handler fails 10 times, it is switched off and a notice says so. A script that
  runs on for more than a second at once (an endless loop) is stopped. Lua's memory is capped at 256 MB.
* `<game folder>/lua/*.lua` also runs (the shared globals, meant for research scripts). That folder needs Legends Lua
  switched on in the Mods page.

## Quick start

`mods/hello/lua/hello.lua`:

```lua
-- one new townsperson, 3 m in front of the player once they're in a level, who walks back and forth
local t
t = timer.every(1, function()
  local p = player.pos()
  if not p then return end          -- (no level yet: tried again in a second)
  timer.cancel(t)
  local a = player.ahead(3)
  local b = a + player.forward() * 6
  local n = npc.spawn{pos = a, name = "Hello"}
  n:walk_route({b, a}, {loop = true, wait = 2})
  n:on("talk", function(n) n:face_player() n:say("Hello!") end)
end)
```

## Characters: `npc`

```lua
local n = npc.spawn{pos = {x, y, z}, yaw = 90, name = "Bob", model = "...", outfit = "player"}
```

| Field | |
|---|---|
| `pos` | Where to make it: `{x, y, z}`, `vec(x, y, z)` or `{x = .., y = .., z = ..}`. If you leave it out, the character goes 2 m in front of the player. |
| `yaw` | Which way it faces, in degrees: 0 faces +z, 90 faces +x. |
| `name` | Your name for it (used in logs, `npc.find`, `n:say`). |
| `outfit` | (Optional) dress it like a player. `"player"`: the player character's clothes. **Arceus**: the outfit the player has on now, and a table puts other items over it, `{[slot] = "item name", ...}` (the 12 slots of the save's dress-up data: 0 hair, 4 hat, 7 clothes were checked; item names from `bin/chara/table/dressup_table_p1.bin` / `_p2.bin`, e.g. `"p1_hdw0001_00_hunting_green"`). **Z-A**: `"player"` only, the parts the player character's model was made with (not the save's look on top, so some pieces differ from what you wear). |
| `body` | (Optional, Arceus) `"p1"` or `"p2"`: which of the two player bodies a dressed character has (default: the player's). |
| `talk` | (Optional, Arceus) `false`: no Talk prompt over it, even while a `talk` handler is for it (see the `talk` event). |
| `model` | (Optional) what it looks like. **Z-A**: a station NPC's id from the level data (`npc_A04_z02_02_0002`; `npc.models()` lists the ones loaded near you). Leave it out to copy the nearest townsperson's look. **Arceus**: a character id (`tr0036_00_senpai_m`), its hash (`"0x2d992cf818c93910"`), or the name of a placement in the area (`area00_npc_g_002`, to copy that NPC's look). Leave it out to copy a villager of the area. |

`npc.spawn` returns a **handle** at once. The game makes the character when the player is within about 50 m (Z-A) or
60 m (Arceus) of its spot and that part of the world is loaded. When the player goes far away, or a loading screen
unloads the area, the character goes with it. The handle stays valid, and the character is made again at its last
spot when the player comes back, carrying on its walk. Removing a handle is the only thing that ends it.

* **Z-A**: a new character is made from a "station" NPC (one the city places to stand somewhere) that the game has
  loaded nearby. With no such station near the spot, the character waits (the log says so).
* **Arceus**: characters belong to the area they were first made in (each area has its own coordinates), and are made
  only while the player is in that area. Up to 32 at once in all. A new character is a copy of one of the area's own
  placements (a villager's, unless `model` says otherwise), so make it at least 2 m from the player.

### Methods

| | |
|---|---|
| `n:walk_to(pos [, run])` / `n:run_to(pos)` | Walk (or run) there. **Z-A**: the game's own navigation walks it (FObj_Move_To_PosXYZ_Navi, as its events walk people): on the ground, around things; done within 1 m of the goal. If the game's walk doesn't get it anywhere for 1.5 s (it happens to some characters after a leg), it walks on in a straight line (the height eased between the points, through walls). **Arceus, dressed** (`outfit`): walked the way the player is, by the player's own movement and physics, so it follows the ground, climbs slopes and walks any number of legs; a goal it can't reach within a minute ends the walk where it got to. **Arceus, not dressed**: the game's own pathfinding on the navigation mesh. A goal off the mesh (inside a building, on a roof) is refused. After a walk has ended, the navigator plays the next legs' walk on the spot (its mover finds no root motion and sets no speed), so those legs are steered: its physics character given the way to the goal, a straight line against the world's collision. Routes loop either way; dressed ones walk the most naturally. |
| `n:walk_route(points, opts)` | Walk a list of points in turn. `opts`: `loop` (start again at the end), `run`, `wait` (seconds at each point), `start` (the first point to head for), `on_point = function(n, i)`. |
| `n:pause()` / `n:resume()` | Stop on a route and stand, then carry on where it was going. |
| `n:stop()` | Stand where it is (and forget the route). |
| `n:set_pos(pos [, yaw])` | Put it there at once. |
| `n:face(yaw or pos)` / `n:face_player()` | Turn it. |
| `n:remove()` | Gone for good. **Z-A**: put out of sight, and the game lets it go a second later. |
| `n:pos()` / `n:yaw()` | Where it is now (or where it will be made). `nil` once removed. |
| `n:state()` | `"waiting"` (not in the world now), `"coming"` (being made), `"up"`, `"walking"`, `"removed"`. |
| `n:is_up()` / `n:valid()` | In the world / not removed. |
| `n:dist([pos])` | Metres to the player (or to `pos`), along the ground. |
| `n:say(text [, seconds])` | A notice on screen: `Name: text`. |
| `n:anim(name, value [, kind])` | Set one of its animation's parameters (the game's own: what its animation graph reads to pick a pose). `value` true/false sets a bool, a whole number an int, any other number a float, `nil` fires a trigger (or pass `kind` `"b"`, `"i"`, `"f"`, `"t"`). Returns whether the character has that parameter. Checked: **Z-A** `n:anim("sit_bool", true) n:anim("sit_int", 1)` sits (int 2 another way; back to `false` / `0` stands up). The city's own people also use `speak_bool` + `wait_int` 2 (chatting) and `loop_action_bool`. **Arceus** `n:anim("set_squat_bool", true)` crouches (dressed characters run the player's animation, the others a villager's). In a pose a Z-A character doesn't walk: stand it up first. |
| `n:trigger(name)` | Fire a trigger parameter (`n:anim(name)`). |
| `n:play_once(n)` | Its once-action `n`: `once_action_int` n, then `once_action_trigger` (Z-A characters also `wait_int` 1, their route goes through their default wait). What plays is the motion its set has in that state - or a new one put there with Studio (below). |
| `n:play_anim(id)` | (Z-A) One of the game's own animations by number, as its events play them (`FObj_Set_Animation`: 1 is standing; events use numbers like 2, 20, 174). |
| `n:object()` | (Z-A) The name the game knows it by (`"luanpc_001"`), for `game.call`'s `FObj_*` commands. `nil` until it is up; it can change when it is made again. |
| `n:on(event, fn)` | This character's events: `"talk"`, `"near"`, `"far"`, `"arrived"`, `"up"`, `"gone"`. |
| `n.name`, `n.data` | Its name, and a table for your own data. |
| `n.near_radius` | Metres for `near` / `far` (default 4). |

`npc.list()` lists this mod's characters, `npc.all()` everyone's, `npc.find(name)` finds one by name, and
`npc.models()` returns what new characters can look like here as text (also the console command `models`).

## Pokémon: `pokemon` (Arceus)

```lua
local p = pokemon.spawn{species = 25, pos = player.ahead(4), yaw = 180, form = 0, shiny = true, name = "Sparky"}
```

Makes a Pokémon in the world, standing where it was put: the game's own Pokémon object, with its model, colours and
idle animation. `species` is the national dex number (one the game has), `form` the form number (default 0), `shiny`
true for the shiny colours (Z-A also `gender`). It returns a handle at once (or `nil` and a reason), and the Pokémon is
made within a frame or two in the level the player is in.

* **Arceus**: made by the runtime like the shared world's ridden Pokémon (up to 16 at once, shared with them). A
  loading screen into another level ends it.
* **Z-A**: made by the game's own event framework, as its events make theirs (`dyn_MANKII_1`...: its make by name, here
  `luapoke_<n>`), so any Pokémon the game has a model for. `p:pos()` is where it was made (ask the game with
  `game.call("Commands.FObj_Get_Pos", p.object, fn)`); `p.object` is its name for the game's `FObj_*` commands. An area
  load (a loading screen, the time of day changing) ends it, and one made while an area is loading may not show.

| | |
|---|---|
| `p:pos()` | Where it is, or `nil` when it isn't in the world. |
| `p:state()` / `p:valid()` | `"coming"`, `"up"`, `"removed"` (also once its level is gone) / not removed. |
| `p:remove()` | Gone (Arceus: the game ends it the next frame; Z-A: the framework's own delete). |
| `p:anim(name, value [, kind])` / `p:trigger(name)` | Its animation's parameters, as a character's `n:anim`. Z-A: set on its node, found by its name at its next update. Arceus: through its animation component, at the next camera update. |
| `p:play_once(n [, seconds])` | **Z-A**: its once-action `n` (`once_action_int` n + `once_action_trigger`): `n` 1-10 route to `#0_def01_once_actionNN#` (Studio's `motionstates <model> once_action` lists them). **Arceus**: its emotion `n` held `seconds` (default 3), then let go - `loop_emotion_int` n + `loop_emotion_bool`, entered with `one_emotion_trigger` (1 sleep, 2 eat for most: `motionstates <model> emotion`). Its one-shot branch (glad) wants the bool true to enter and false at the entry, which a script doesn't reach. |

**A new motion, played from a script** (Z-A, checked with Pikachu): Studio's rig commands put a new animation in one of
a model's once-action states, in a mod, and the script plays that state.

```sh
legends-studio-cli --mod mods/mine/romfs <romfs> motionadd ik_pokemon/data/pm0025/pm0025_00_00/pm0025_00_00.trmdl \
  myspin spin.glb spin like 00550_glad01 play-any "#0_def01_once_action07#"
```

```lua
local p = pokemon.spawn{species = 25, pos = player.ahead(3)}
timer.after(3, function() p:play_once(7) end)   -- once_action 7 now plays "myspin"
```
| `pokemon.all()` | This mod's Pokémon. |

## Items and the time of day

| | |
|---|---|
| `item.give(id, n [, show])` | `n` of item `id` into the bag, by the game's own add. **Arceus**: `Bag::AddItem` (the script binding `Bag.AddItem`); Hisui's own items have their own numbers - its Poké Ball is 1710 (4 is the modern one, which Arceus doesn't use). **Z-A**: `Commands.AddItem`; with `show` the game's own "You obtained 10 Poké Balls!" window too (A closes it). Returns true when asked. |
| `game.set_time(hour)` | The time of day: the game's clock set to that hour (0-24; `18.5` is 18:30). **Z-A**: as the bench's "wait until night" does. The story may hold the clock (the test save held 15:00): the hold is let go. The city changes after the game's own short black transition (lit windows and the night zones at night); the area loads again, so script Pokémon go and characters are made again. **Arceus**: as resting at a camp's tent does (the camp's `time_change`: morning is 7, midday 12, evening 19, nightfall 21), without the rest - the sky, light and stars change at once, nothing loads again, and the clock runs on from there. Returns false for an hour outside 0-24. |

## The game's own scripts: `game.script`, `game.call`, `game.run`

Both games run their events as Lua inside the game, Haxe compiled to Lua. **Z-A**: Lua 5.4, the framework
`script/lua/bin/release/main/main.blua` and every event in `event_ik.blua`, the jobs on its main state. **Arceus**: Lua
5.3 with the C++ side bound through sol2 (`bin/haxe/release/event/*.blua`, `field/*.blua`), the jobs on the player's
field script's state (about 500 globals: `Bag`, `AppCommand`, `CameraManager`, `FieldMessageWindowManager`,
`Global.GetFieldMessageWindowManager()`...; names are plain). A mod can run Lua text there, at the game's next frame:

```lua
game.call("Commands.FObj_Get_Pos", "Player", function(r) print("the player is at", r) end)
game.script("return Commands.IsDayTime()", function(r, err) print("day?", r) end)
game.run("Commands.S6DCE2895AC79F359(Commands.S5DF104B25CC642C7(4), 10)")  -- the "You obtained 10 Poke Balls!" window
```

| | |
|---|---|
| `game.script(code [, fn])` | Runs `code` once in the game's Lua; `fn(result, err)` gets what it returned, as text (`"nil"` for nothing). |
| `game.call(name, args... [, fn])` | Calls one function with these arguments (numbers, strings, booleans, `{x, y, z}` / `vec`, lists). A list result comes back as its items with spaces between. |
| `game.run(code [, fn])` | Runs `code` as a coroutine, resumed once a frame until it ends: for what waits (the message windows, `Wait_Time`, fades). |
| `game.hash(name)` | The game's name for a member: `"S"` + its FNV-1a 64 in hex. |

* `Commands` is the framework's commands class (`C0C062DE5AED06885`). Its members' names are hashed in the game;
  `Commands.Name` in the code is turned into `Commands.S<hash>` for you. Studio (`zalua chunk main`) shows the framework
  with the names it recovered: what each command takes.
* The natives (`c<hex>.f<hex>`) take what they take: one given the wrong kind of object can crash the game (a `pcall`
  doesn't catch that).
* Useful ones: `FObj_Set_Pos(name, x, y, z)`, `FObj_Set_Ang(name, yaw)`, `FObj_Get_Pos(name)`, `FObj_Set_Animation(name, n)`,
  `FObj_Set_Visibility(name, bool)`, `FObj_LookAt_Obj(name, other, "head")`, `FObj_Move_To_PosXYZ_Navi(name, x, y, z)` -
  on any named object: `"Player"`, a character's `n:object()`, a Pokémon's `p.object`, the city's own people.
  `AddItem(id, n)`, `IsDayTime()`. The game's texts by label: `ShowMessageWindow(kind, label, file)` + `HideMessageWindow()`
  in `game.run`, once their message file is loaded as the framework does it (its item window, above, does that itself;
  the item window is the one checked).
* **Only what a mod would do**: this is the game's own machinery, with nothing checked. A wrong call can break an event
  or the save, like any raw-layer hook.
* Each job costs a load of its text: tens of microseconds on the game's thread. Keep them for events, not every frame.
* Arceus: `Commands` and the name hashing are Z-A's only; `game.call("Global.GetPlayerComponent", fn)` and the like.

## The player: `player`

| | |
|---|---|
| `player.pos()` | A `vec`, or `nil` before a level is up. |
| `player.yaw()` | Which way they face, in degrees. |
| `player.forward()` | A unit `vec` facing the same way. |
| `player.ahead(d)` | The spot `d` metres in front of them. |
| `player.teleport(x, y, z)` | Put them there (within the loaded level). |
| `player.button(name)` | Whether that button is held: `A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK`. |

## Events

```lua
local id = on("tick", function(dt) ... end)                 -- every frame
on("button", "X", function() ui.notify("X!") end)           -- a filter: only X
on("talk", function(n) n:say("Hi") end)                     -- any of the mods' characters
some_npc:on("near", function(n, d) ... end)                 -- one character's
off(id)
```

| Event | Arguments | When |
|---|---|---|
| `tick` | `dt` (seconds) | Every frame. |
| `area_loaded` | `{id, name}` | A part of the world finished loading. **Arceus**: an area's people were made (`name` = `area00`...). **Z-A**: a batch of the city's people (`name` = the city block, `A04`...). |
| `button` / `button_up` | button name | A button pressed / let go (filter: the name). |
| `talk` | character | The player talked to one of the mods' characters. **Arceus**: through the game's own Talk prompt - while a `talk` handler is for a character (a global `on("talk")`, or its own `n:on("talk")`), facing it shows "Ⓐ *its name*" as over the game's people (a villager's look says what that villager is called, a dressed one your name), and A on it is the talk. The game does nothing else (no event; the player can walk off). `npc.spawn{..., talk = false}` keeps one without the prompt. **Z-A**: its townspeople have no prompt: A pressed while facing a character within 2.2 m (the game still sees the A press too). |
| `near` / `far` | character, metres | The player came within `n.near_radius` / went beyond it. |
| `npc_up` / `npc_gone` | character | Made in the world / gone with its area. |
| `npc_arrived` | character | A `walk_to` (or a route without `loop`) ended. |
| `loaded` / `unload` | | After every script ran / before a reload. |

`emit(name, ...)` fires your own events, which other mods can listen to.

## More

| | |
|---|---|
| `timer.after(sec, fn)` / `timer.every(sec, fn)` | Returns an id; `timer.cancel(id)` stops it. |
| `ui.notify(text [, sec])` | A notice at the top of the screen (default 4 s). |
| `ui.message(text [, fn])` | **Z-A**: the game's own message window (the one its people talk in) with this text - `\n` for a new line; A closes it, then `fn()`. Elsewhere a notice. |
| `ui.say(npc_or_name, text)` | `ui.message("Name:\n" .. text)`. (`n:say(text)` stays a notice: it doesn't wait for A.) |
| `cmd.add(name, fn, help)` | A console command (below). Its arguments are the words after the name; numbers come as numbers. |
| `vec(x, y, z)` | `+ - *`, `:len()`, `:dist(b)`, `:dist2d(b)`, `:yaw_to(b)`, `:ahead(yaw, d)`, `:lerp(b, t)`, `:unpack()`, `.x .y .z`. |
| `include("lib/x.lua")` | Runs another file of this mod in its globals and returns what it returns. |
| `log(...)` / `print(...)` | Writes to the log (`[lua] <mod>: ...`). |
| `game.name` | `"za"` or `"pla"`. Also `game.dt` (the last frame's seconds), `game.title`, `game.frame()`. |
| `game.work(name [, value])` | One of the game's own progress values ("works", the numbers its story scripts read and set): its value, or `nil` when the game has none by that name (yet). With `value` it writes it, so be careful: the story reads these. **Z-A**: e.g. `"SYS_WORK_PHASE"`. **Arceus**: e.g. `"WPHA_STORY"`, and its flags as `"flag:<name>"` (0 / 1). |
| `lua.stats()` | What the scripts cost (below). |

### The console

While the game runs, a file `lua_cmd` in the game folder (or in `LARECOMP_LIVE` if that is set) runs one command per
line, then is removed. Output goes to `lua_out.txt` next to it and to the log:

```sh
echo "npcs" > lua_cmd; sleep 1; cat lua_out.txt
```

Built in: `help`, `npcs` (every script character, its state and where it is), `models`, `where` (the player), `tp x y z`,
`stats`. For arbitrary Lua, write it to `lua_run.lua` instead. It runs once in the shared globals (`mods.<name>` is a
mod's globals).

## Cost

Without a mod that has scripts, Legends Lua isn't loaded at all, so it costs nothing.

With scripts, the render thread runs the scripts' share once a frame: events, timers and the characters' states. When
no script listens for every frame (`tick`, buttons, `talk`), that runs only every 4th frame. Files are watched for
changes from a thread of the plugin's own, so the game's threads never touch the disk for it. `lua.stats()` and the
console command `stats` show the CPU time per frame (average and most), and `bench` times each API call.
`LARECOMP_LUA_STATS=1` logs the cost every minute, and `LARECOMP_MODNPC_STATS=1` logs the runtime side's.

Measured on 2026-11-19 (Core i9-12900K, both games, with other games sharing the CPU):

| | per frame |
|---|---|
| no mod with scripts | 0 (not loaded) |
| the plugin alone, outside the game (a bench harness): empty mod / Town Walk | 0.5 / 0.8 us |
| in the game, Town Walk (3 characters, near / far / talk handlers) | about 20-25 us CPU (the same work on cold caches) |
| Z-A runtime: characters' bookkeeping | 0.3-0.7 us; 3-7 us with characters made |
| Z-A runtime: the node hook (only once a script moves a character) | about 2500 calls a frame, 1 test each past a filter |
| Arceus runtime: characters' bookkeeping (camera update) | 5-16 us with 3 characters |
| Arceus, the test mod: 3 dressed characters walking, 2 Pokémon, `tick` handlers | 25 us (most 335 us, a reload) |
| Arceus Windows build under Wine, Village Walk with 3 dressed walkers | 26 us |
| Z-A, the test mod: 3 characters, one dressed, poses | 16 us |
| Z-A, Town Walk + the test mod: 3 walkers on the game's navigation, 3 Pokémon, game scripts | 18-20 us (the game's thread: a job's load and run on top) |
| Arceus, the test mod: an undressed villager's steered loop, items | 20 us |

At 60 fps a frame is 16,700 us, so a mod like the samples costs about 0.1-0.2% of the frame.

## Not there yet

* **The townspeople's speech bubbles** on script characters. Z-A's townspeople have no A prompt: a talkable one
  (talk script `StandTalkNpcLookAt...`) says a line in a bubble when the player comes near, and A does nothing more. A
  script character's `talk` is an A press near it, and with `ui.say` that opens the game's own window - so talking to one
  looks like the game. The bubble is the townspeople's C++ AI; a script character has the same InteractNPC component, but
  its record (+0x48) is named after its object (`luanpc_001`). Pointing that record at a talkable townsperson's, copying
  that townsperson (`model`), or giving it the talkable talk script (research `LARECOMP_MODNPC_TALK=1`) brought no bubble.
* **Weather** (both games). Z-A's framework has no setter (its `FixWeather` is empty); the weather the game reports
  comes from an object of the environment clock's (a list of 4 entries, the current one at +0x68): pointing it at
  another entry changes what the game reports (`c4B6212B8.f6362F6BB()`), but nothing on screen. Arceus' scripts have
  no weather calls.
* **Arceus' own-text window**: its line decoder is found (0x7002b2e654, the same as Z-A's) and its
  window's `CreateMessageWindow` (0x70024862cc) takes the message file and label as 64-bit FNV hashes; the hash-to-line
  lookup isn't found yet, and the message files are loaded per event.
* **Z-A outfits other than `"player"`.**

## The raw layer

Underneath, everything from `contrib/legendslua/README.md` is still there: `hook(addr, fn)` on any game function,
`mem.*` reads and writes, `c:call` into the game, `on_frame`, hardware `watch`. Use it for what the mod API doesn't
have yet. It's research-grade: addresses differ between the two games and between game versions.

## Examples

`contrib/legendslua/examples/` has the game's sample mod (**Town Walk** for Z-A, **Village Walk** for Arceus): three
townspeople walk a square; one stops, turns and greets the player who comes near, and walks on when they leave. Talk
to one for a line. Copy the folder to `mods/` and switch it on. (In Arceus the three are dressed: one in your outfit,
two with other items over it. In Z-A they walk on the game's own navigation.)

**React when the player comes near:**

```lua
local guard = npc.spawn{pos = {-721.5, 0.2, -920.5}, yaw = 180, name = "Guard"}
guard.near_radius = 5
guard:on("near", function(n) n:face_player() n:say("Halt! Who goes there?") end)
guard:on("far", function(n) n:face(180) end)
```

**A console command that makes a crowd:**

```lua
cmd.add("crowd", function(count)
  for i = 1, count or 5 do
    npc.spawn{pos = player.ahead(3 + i), name = "Fan " .. i}:face_player()
  end
end, "crowd [n]: n characters lined up in front of you")
```

**Follow the player:**

```lua
local buddy
on("area_loaded", function()
  if not buddy then buddy = npc.spawn{name = "Buddy"} end
end)
timer.every(0.5, function()
  if buddy and buddy:is_up() and buddy:dist() > 3 then buddy:walk_to(player.ahead(-1.5)) end
end)
```

**Legends Studio:** in a Z-A level, **Add > NPC** writes a new townsperson into the mod's `lua/studio_npcs.lua`. The
**Scripts** panel names it, turns it and gives it a walk route, and has an editor and this API at hand. **Play** starts
the game with the mod, so its scripts run.
