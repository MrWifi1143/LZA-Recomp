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
| `n:say(text [, seconds])` | A speech bubble over its head with the text (see Speech bubbles, below): `ui.bubble(n, text, seconds)`. |
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
| `game.run(code [, fn])` | Runs `code` as a coroutine, resumed once a frame until it ends: for what waits (the message windows, `Wait_Time`, fades). `fn("done")` once it has ended (a message window: after A closed it), `fn(nil, err)` if it failed. |
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
| `ui.say(npc_or_name, text)` | `ui.message("Name:\n" .. text)`. (`n:say(text)` is a speech bubble instead: it doesn't wait for A.) |
| `ui.bubble(target, text [, seconds [, opts]])` | A speech bubble over the target's head (below). Returns its id. |
| `ui.bubble_hide(id or target)` | That bubble fades out. |
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

## Speech bubbles

```lua
n:say("Hello!")                                  -- over a character (npc.spawn)
ui.bubble("player", "On my way!", 6)             -- over the player, for 6 s
ui.bubble(pikachu, "Pika!", nil, {height = 0.6}) -- over a Pokémon (pokemon.spawn); height over its feet
ui.bubble("npc_A04_z02_02_0031", "Hi")           -- Z-A: over any game object by its name
local id = ui.bubble(vec(10, 2, 5), "Here")      -- over a spot
ui.bubble_hide(id)
```

A white bubble with a tail, the townspeople's look, over the target's head. It follows the target as it walks and the
camera as it turns, and it goes when its time is up, when the target is removed (or its area unloads), or with
`ui.bubble_hide`. Both games, the same way.

* **Text**: UTF-8. Long lines wrap (about 26 characters a line); `\n` breaks a line. Cut at 100 characters (`...` at the
  end). The overlay's font has Latin-1 (accents) and Cyrillic; other characters show as `?`.
* **How long**: `seconds`, or by the text's length (3 to 12 s).
* **Replacing**: a new bubble over the same target replaces the one it has (its id stays). One bubble per target.
* **Many at once**: up to 32. Bubbles that would cover each other are stacked: the nearer one keeps its place, the
  farther one moves above it, with a thin line down to its character. Far ones (35 to 45 m) fade out; ones behind the
  camera or off the screen aren't drawn.
* **Over a spot** (a position, no character): a sign - no tail and no line, centred on the spot (moved up like the
  rest when it would cover another).
* **Height**: a person's head by default (2 m over the feet in Z-A, the player 1.95 m; in Arceus the player 1.7 m and
  its people 1.9 m; a Pokémon 1 m). `opts.height` sets it.
* A script reload clears the scripts' bubbles with their characters.

The runtime draws them over the game's image (its overlay, as the notices), placed with the game's own scene camera
(Z-A: the camera update 0x70018cff60's camera that looks at the player; Arceus: the render camera 0x7002994b70). They're
not the game's own bubbles: those can't be put over just anything. **Z-A**'s are the townspeople's: a townsperson's AI
(`StandTalkNpcLookAt...`) activates its own bubble component (vfunc +0x98, 0x7000a920c8), which builds a "MainPage"
layout instance and asks the bubble manager to show it (0x70009236e0 -> 0x7000fd7cd0 -> the layout's show 0x7000fdb370);
the text is bound to the townsperson's NPC data (its message file and label, e.g. `t1_A04` / `msg_A04_z02_02_speech_31`)
and read when the bubble draws (0x700068d8b0). A script character, the player or a Pokémon has no such component or data.
**Arceus**' bubbles are the event message window in its speech-bubble look (`FieldMessageWindowManager`'s
`CreateMessageWindow`, style 1, over a speaker object: one window at a time, its text from the event's message files)
and the camp people's lines from their talk data - neither can show several lines of our own at once.

### From the runtime (multiplayer chat)

`runtime/bubble.h` (`runtime/hle/bubble.c`, the same in both games) is the C side, for code like the multiplayer's: a
remote player's chat line over their stand-in. Any thread.

```c
#include "bubble.h"
// over a stand-in the multiplayer code moves itself: asked where it stands each drawn frame
static int standin_feet(void *ctx, float feet[3]) {   // 1 there, 0 not now (hidden), -1 gone (the bubble too)
    Peer *p = ctx;
    if (!p->connected) return -1;
    if (!p->standin_up) return 0;
    memcpy(feet, p->standin_pos, 12);
    return 1;
}
BubbleTarget t = {.kind = BUBBLE_FN, .where = standin_feet, .ctx = peer, .owner = BUBBLE_OWNER_MP};
int id = bubble_show(&t, chat_line_utf8, 0);   // 0: by its length; the peer's previous line is replaced
...
bubble_hide_target(&t);                          // (or bubble_hide(id)) when the peer leaves
bubble_clear(BUBBLE_OWNER_MP);                   // every chat bubble (a session ends)
```

Other targets: `BUBBLE_NPC` with a mod API character handle (`.handle`), `BUBBLE_POKEMON` (Arceus, a handle),
`BUBBLE_OBJECT` (Z-A: a game object's name in `.object` - a stand-in made as `luanpc_<k>` by the mod API's characters is
followed by its name), `BUBBLE_PLAYER`, `BUBBLE_POS` (`.pos`). `.height` overrides the head height. `BUBBLE_OWNER_MP`
bubbles survive a script reload (`bubble_clear(BUBBLE_OWNER_SCRIPT)`). The text is cleaned the same way (UTF-8 checked,
control characters made spaces, cut at 100 characters), so a peer's text can be passed as it came - but it's the chat's
job to limit how often a peer may send.

## Not there yet

* **The game's own speech bubbles** over script characters (Z-A's townspeople's; the runtime draws its own instead -
  Speech bubbles, above). Z-A's townspeople have no A prompt: a talkable one says a line in a bubble when the player
  comes near. A script character has the same InteractNPC component, but its record (+0x48) is named after its object
  (`luanpc_001`); pointing that record at a talkable townsperson's, copying that townsperson (`model`), or giving it the
  talkable talk script (research `LARECOMP_MODNPC_TALK=1`) brought no bubble - the bubble is a component of its own.
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

## The overlay, menus, input, sounds, storage (API version 6)

Scripts can draw their own interface over the game, build menus that work with a controller, mouse or keyboard, read
the keyboard and mouse, play their own sounds and keep values per save. The same in both games. A complete example is
`contrib/legendslua/examples/spawnmenu` (**Spawn Menu**: M on the keyboard, L + R on a controller, or `spawnmenu` in the
console).

### Drawing: `draw`, `hud`

```lua
hud.add(function()                                   -- each frame (the same as on("draw", fn))
  local x, y, w, h = draw.box()                      -- the game's 16:9 interface box
  local s = draw.scale()                             -- its height / 1080: design at 1920x1080, multiply by s
  draw.rect(x + 20 * s, y + 20 * s, 300 * s, 80 * s, 0x14171fc8, 12 * s)
  draw.text("Hello", x + 36 * s, y + 40 * s, {size = 28 * s, color = "yellow", shadow = true})
end)
```

Drawing calls only count inside a draw handler; everything one frame draws is handed to the runtime together and drawn
over the game's image (over the speech bubbles, under the runtime's own menus) from the next frame on. Coordinates are
the window's pixels, y down.

| | |
|---|---|
| `draw.size()` | The overlay's width and height (the window's pixels). |
| `draw.box()` | x, y, w, h of the game's interface: 16:9 in the middle. The whole screen at 16:9; on a wider one (ultrawide, "Fill the window") the game keeps its interface in this box, so put yours there too. |
| `draw.scale()` | The box's height / 1080. |
| `draw.rect(x, y, w, h, color [, rounding])` | A filled rectangle, rounded corners if given. |
| `draw.rect_outline(x, y, w, h, color [, thickness [, rounding]])` | Its outline. |
| `draw.line(x1, y1, x2, y2, color [, thickness])` | |
| `draw.circle(x, y, r, color)` / `draw.circle_outline(x, y, r, color [, thickness])` | |
| `draw.triangle(x1, y1, x2, y2, x3, y3, color)` | |
| `draw.gradient(x, y, w, h, top, bottom)` | Top colour to bottom colour. |
| `draw.text(text, x, y [, opts])` | `opts`: `size` (22), `color`, `align` (`"left"`, `"center"`, `"right"`: what x is), `valign` (`"top"`, `"middle"`, `"bottom"`: what y is), `wrap` (a width: lines wrap inside it, `align` then within it; `\n` breaks a line), `shadow`. Or `draw.text(text, x, y, size, color, align)`. |
| `draw.text_size(text [, size [, wrap]])` | Its width and height. |
| `draw.image(img, x, y [, w, h [, opts]])` | An image from `image.load`. `opts`: `tint` (a colour it's multiplied by), `uv = {u0, v0, u1, v1}` (a part of it, 0..1), `rounding`. |
| `draw.clip(x, y, w, h [, fn])` / `draw.unclip()` | What's drawn (by `fn`, or until `unclip`) is cut to the rectangle. |
| `draw.alpha(a)` | Everything drawn after it is that transparent (0..1) until the next `draw.alpha`; each frame starts at 1. |
| `draw.to_screen(pos)` | Where a point of the world is on the screen: x, y and the metres to it; `nil` behind the camera. For name tags. |
| `image.load("images/x.png")` | A PNG or JPG of the mod's folder: `{id, w, h}` (the same file again: the same image). Read once; uploaded the first time it's drawn. |
| `hud.add(fn)` / `hud.remove(id)` | `on("draw", fn)` / `off(id)`. |

**Colours** are `0xRRGGBBAA` numbers, `{r, g, b, a}` tables (0..255; `a` 255 if left out), or a name: `"white"`,
`"black"`, `"red"`, `"green"`, `"blue"`, `"yellow"`, `"gray"`, `"dark"`, `"clear"`. `color(r, g, b [, a])` makes the
number, `color.hex("#ff8800")` / `"#ff880080"`, `color.alpha(c, a)`, `color.lerp(a, b, t)`.

**Text** is the overlay's font (DejaVu Sans on Linux, Segoe UI on Windows) with Japanese and common Chinese (a Noto Sans
CJK JP subset: kana, 5,392 characters in all) and emoji (Noto Emoji, black and white: drawn in the text's colour)
merged in - the speech bubbles get them too. Korean and rarer kanji show as `?`.

### Menus: `gui`

```lua
local menu = gui.window{title = "My menu", w = 560}        -- (w: pixels at 1080 lines; x / y: default centred)
menu:button("Say hi", function() ui.notify("Hi!") end)
menu:checkbox("Fast", false, function(on) ... end)
menu:slider("Hour", 0, 24, 12, function(v) game.set_time(v) end, {step = 0.5, format = "%.1f h"})
menu:choice("Species", {"Pikachu", "Eevee"}, 1, function(i, name) ... end)
menu:list({"One", "Two", "Three"}, function(i, name) ... end)
menu:input("Name", "Bob", function(text) ... end, {max = 24})
menu:label("Text")  menu:separator()  menu:image(img, 80)
on("key", "M", function(e) if e.down then menu:toggle() end end)
```

| | |
|---|---|
| `gui.window{title, w, h, x, y, on_close}` | A window (hidden until `open`). `h`: the most it grows to; longer content scrolls. |
| `w:open()` / `w:close()` / `w:toggle()` / `w:is_open()` | |
| `w:clear()` | Its widgets gone (rebuild it: menus whose content changes). |
| `w:label(text)` | Text (a function: called each frame - a live value). |
| `w:button(text, fn(w))` | |
| `w:checkbox(text, value, fn(on))` | |
| `w:slider(text, min, max, value, fn(v), {step, format})` | Left / right (or dragging it) change it by `step`. |
| `w:choice(text, options, index, fn(i, option))` | Left / right (or a click) go through the options. |
| `w:list(options, fn(i, option), {selected = i})` | One row each; activating one picks it. |
| `w:input(text, value, fn(text), {max})` | Opens the runtime's text dialog (below). |
| `w:image(img [, height])` / `w:separator()` | |
| `gui.close_all()` | Every window of the mod's. |

While a window is open it has the input: **the game's is held back** (the player doesn't move, the camera doesn't
turn, the mouse is let go) and Esc goes to the window instead of opening the runtime's menu (F1 still does). Controls:

* **Controller** (and the keyboard's game keys: arrows, Enter / Space, Backspace / Q): d-pad up / down move (held:
  repeating), A activates, left / right change a slider or choice, B closes.
* **Mouse**: what's under the pointer has the focus, a click activates (drag a slider), the wheel scrolls, a right
  click or the × closes.
* **Keyboard**: Esc closes.

Several windows can be open; the last opened is on top and has the input.

### Keyboard and mouse: `input`, `on("key")`, `on("mouse")`, `on("text")`

```lua
on("key", "F7", function(e) if e.down and not e.rep then ... end end)   -- (a filter: that key only)
on("mouse", "left", function(e) if e.down then log(e.x, e.y) end end)
on("mouse", "wheel", function(e) zoom = zoom + e.wheel end)
```

| | |
|---|---|
| `key` (filter: the key's name) | `e.key` (SDL's names: `"A"`, `"Return"`, `"Escape"`, `"Left Shift"`, `"F5"`...), `e.down`, `e.rep` (held: repeating), `e.shift`, `e.ctrl`, `e.alt`. The game still sees keys (unless a menu holds the input). |
| `mouse` (filter: `"left"`, `"right"`, `"middle"`, `"wheel"`) | `e.button`, `e.down`, `e.x`, `e.y` (overlay pixels), `e.wheel`. |
| `text` | The text typed (when the window takes text input). |
| `input.mouse_pos()` | x, y of the pointer (overlay pixels). |
| `input.mouse_down([button])` / `input.key_down(name)` | |
| `input.grab(on)` | Hold the game's input back yourself (a menu of your own drawing). |
| `input.text(title, text, max, fn(result))` | The runtime's text dialog: typed with a keyboard, the on-screen keyboard with a controller (as the game's own name entry). `fn(text)` on OK, `fn(nil)` cancelled. The game's input is held while it's up. |

The controller stays with `player.button(name)` and `on("button")`.

### Sounds: `sound`

```lua
local pop = sound.load("sounds/pop.ogg")
pop:play()                                              -- everywhere (2D)
pop:play{pos = npc_a:pos(), range = 25, volume = 0.8}   -- from a spot: quieter farther, panned
local loop = pop:play{loop = true, pitch = 1.2}  loop:stop()
```

| | |
|---|---|
| `sound.load(file)` | A `.wav` (8 / 16 / 24 / 32-bit or float, mono or stereo, any rate) or `.ogg` (Vorbis) of the mod's folder, read once. `nil` if it can't be read. |
| `snd:play{volume, pitch, pos, range, loop}` | `volume` 0..4 (1), `pitch` 0.25..4 (its speed), `pos` a spot of the world (silent beyond `range` metres, 30; panned by where it is from the camera), `loop`. Returns a voice. |
| `voice:stop()`, `voice:playing()`, `voice:set_volume(v)`, `voice:set_pos(p)` | |
| `sound.play(file or snd, opts)` / `sound.stop_all()` | |

They're mixed into the game's own output (after the game's mix: the Volume setting and background muting apply), up to
64 at once.

### Storage and files: `storage`, `files`

```lua
storage.set("best", 42)                 -- kept for this save (the game's save_<n>)
local best = storage.get("best", 0)
storage.global.set("first_run", false)  -- kept for the mod, whatever the save
```

| | |
|---|---|
| `storage.get(key [, default])` / `storage.set(key, value)` | Values: strings, numbers, true / false, tables of those (nested; no loops). **They follow the game's save** (API version 8): kept in the save's own folder (`<save root>/save_<n>/lzr_moddata/<mod>.lua`) and written only when the game saves (its save's commit), by a thread of the plugin's. Quit without saving and they are what they were at the last save - like everything else the game keeps; copy an older save folder back and its values come with it (also Legends Studio's Play, which plays on a copy, and an online account's save sync, which carries the folder). Until the game saves, changes are held by the plugin (a reload of the scripts keeps them). A value from before version 8 (`<game folder>/moddata/<mod>/<save>.lua`) is read once if the save has none. |
| `storage.all()` | The whole table (change it in place, then `storage.save()`). |
| `storage.slot()` | Which save it's kept for (`"save_0"`). |
| `storage.global.*` | The same, for the mod as a whole (`moddata/<mod>/_mod.lua`), written half a second after a change whatever the game does (settings, not progress). |
| `files.read(path)` / `files.exists(path)` | A file of the mod's own folder (`"data/levels.txt"`; no `..`): its text, `nil` when missing. |

### From the runtime

`runtime/hud.h` (the draw list and input events), `runtime/modapi_ext.h` and the mod API's version 6 in
`runtime/modapi.h`: `hud_submit` / `hud_size` / `hud_measure` / `hud_image` / `hud_to_screen` (runtime/ui/hud.mm),
`input_grab` / `input_events` / `input_mouse` (runtime/hle/scriptinput.c), `text_prompt` / `text_prompt_poll`
(ui/menu.mm), `sound_*` (runtime/hle/modsound.c), `save_id`. Tests: `<live>/sdl` makes window events (`key M`,
`click 640 300 left`, `move x y`, `wheel 1`, `type text`), `<live>/kbd` answers a script's text dialog, `<live>/wshot`
saves the window image with the overlay, `LARECOMP_MODSOUND_WAV=<file>` records what scripts' sounds added.

### Cost

Measured with `lua.stats` (CPU time of the Lua side each frame; Legends Z-A, i9-12900K with two other games running):
the test mods with nothing drawn 36 us a frame, + the Spawn Menu's HUD panel (4 things drawn) +16 us, + its menu open
(~90 things a frame) +57 us, a test drawing ~45 things with option tables, text measuring and a name tag +80 us. In a hot
loop the same frame is 4 us (panel) to 35 us (the test): the in-game numbers are cold caches on a busy machine.

The render thread (`LARECOMP_HUD_STATS=<frames>` logs it; Legends: Arceus, the same machine): turning the overlay on at
all - ImGui's frame and its draw data recorded - is about 40 us a frame; on top of that the scripts' list costs 5 us
for the HUD panel (4 commands), 14 us with the menu open (42 commands drawn), 18 us for the heavy test (35 commands,
1,930 vertices with its text). Speech bubbles: under 1 us for a few.

## Game UI: the game's own interface (API version 7)

Both games build their interface with Nintendo's `nn::ui2d`: **layouts** (`.bflyt`, in the game's `.arc` archives) made
of named **panes** (Pane, Picture, TextBox, Window, Parts...). Any pane of any layout can be moved, scaled, faded,
hidden, anchored to the real screen's edges, or given other text - by a script, or by a mod with no code at all. The
game's animations go on as before: the change is put on the pane each frame just before the game turns it into what
it draws, and taken off right after.

```lua
for _, l in ipairs(gameui.layouts()) do print(l.name, l.panes) end   -- what is on screen now
for _, p in ipairs(gameui.panes("hud_pokelist_00")) do               -- one layout's panes, in tree order
  print(string.rep("  ", p.depth) .. p.name, p.type, p.x, p.y, p.w, p.h, p.shown)
end
gameui.set("main_menu_top_00", "T_money_00", {text = "Hello"})        -- a TextBox's text
gameui.set("hud_pokelist_00", "RootPane/all", {scale = 0.8, y = 60})  -- smaller and higher
gameui.set("hud_pokelist_00", {alpha = 0.6})                          -- (no pane: the whole layout)
gameui.set("main_menu_top_00", "T_lv_num_00", {hide = true})          -- every pane of that name
print(gameui.text("main_menu_top_00", "T_money_00"))                  -- a TextBox's text now
gameui.reset("main_menu_top_00")                                      -- this script's edits of it taken back
```

In the game, the console command **`ui`** lists the layouts on screen and **`ui <layout>`** its panes (name, type, place,
size, hidden). Legends Studio's **Game UI** window lists every layout in the game's files, with its panes, without
playing (CLI: `uilayouts [filter]`, `uipanes <layout>`).

* **Names**: a layout's name is its `.bflyt`'s (`hud_minimap_field_00`). A pane is named as in the layout (`T_money_00`);
  a name many panes share (Parts copies) edits all of them. `"a/b"` is a pane `b` whose parent is `a` (and `"x/a/b"`
  up the tree); a name ending in `*` matches its beginning (`"N_header_00/P_header_*"`). The pane `""` (or none) is the
  layout itself.
* **Keys** (`gameui.set`'s table and a mod's `.ini` alike):

  | key | what |
  |---|---|
  | `x`, `y` | moved by that many layout units (a 1920 x 1080 space, 0 in the middle, y up) |
  | `scale`, `sx`, `sy` | its scale times that (around its own origin) |
  | `w`, `h` | its size set |
  | `alpha` | its alpha times that (0..1) |
  | `hide` / `show` | not drawn / drawn even where the game hides it |
  | `text` | a TextBox's text (UTF-8, `\n` a new line; cut at the box's own length) - the game's text comes back when the edit goes |
  | `anchor` | `left`, `right`, `top`, `bottom` (or two, `"left top"`), `auto` (the edge it is nearest): kept that far from the real screen's edge as from the 16:9 layout's. On the layout itself: its whole 16:9 box moved to that edge |
  | `fill` | stretched over the whole screen's width (a backdrop); on the layout itself `fill = false`: none of its panes is |
  | `follow` | a pane the game places over something in the world (a marker over a character): its place spread to the real screen's width |
  | `mode` | (the layout) on a screen wider than 16:9: `box` (default: the 16:9 box in the middle), `wide` (as wide as the screen: panes where they were, the anchored ones at the edges; set by itself when a pane is anchored), `stretch` (stretched across, as the game itself would) |
  | `image` | a `.png` / `.dds` file (from the game folder; in a mod's `.ini`, beside it): the texture the pane draws with first becomes the file's. As a texture pack replaces a texture: everywhere that texture is used. The pane's material decides the colours - many HUD icons are masks tinted by their material, so a picture shows as their shape |

* **Who wins**: a script's edits beat a mod's data, which beat the runtime's own; a reload takes the scripts' back.
* `gameui.clone(layout, src, name, {x =, y =, scale =, hide =})`: a new pane - `src` (and what is under it) drawn once
  more, moved / scaled about its own centre. The copy shows what its source shows (its text, its animation); edit it
  later with `gameui.set(layout, name, {...})` (`x`, `y`, `scale`, `hide`). In an `.ini`: a section named for the copy
  with `clone = <src>`. (Not a new pane in the game's tree: a running layout has no GPU constants for one.)

### A UI mod without code

```
<game folder>/mods/<name>/ui/<layout>.ini

; mods/Small Party Bar/ui/hud_pokelist_00.ini
[layout]            ; the layout itself ([*] too)
alpha = 0.6
[RootPane/all]      ; a pane
scale = 0.8
y = 60
```

The file's name is the layout's. A mod with only `ui/` is a mod (the Mods page shows it). The files are read again
within two seconds when one changes, while the game runs.

### Ultrawide

With **Aspect ratio: Fill the window** on a screen wider than 16:9, the runtime itself edits a few layouts (its own,
lowest-priority edits): Z-A's minimap and party bar go to the left edge, the field's markers and the aiming reticle
(placed by the game over things in the world) are spread to the real screen, the map is as wide as the screen with its
header across it, a battle's plate goes left and the move palette right; Arceus' field HUD parts go to the edge each is
nearest (commands and button hints left, the item and party wheel right), its quest markers are spread out, and in a
battle the letterbox bars cross the whole screen, the player's plate goes left, the commands right and the plates over
the Pokémon follow them. Every other layout stays in the middle 16:9 box; a pane as
big as the whole layout (a fade, a menu's dimming, a blurred backdrop) covers the whole screen - told by the pane's own
shape, so sheets like Z-A's Settings keep their proportions. A pane across its whole width (a band: Z-A's menu tint,
Arceus' tab and guide bars) or ending at its left or right edge (a header tab, a side panel, a guide line, a header bar)
stays where it is with its edges continued to the real screen's edges, so what is drawn in it stays in place; a narrow upright strip there (an
accent bar) goes to the screen's edge instead. `fill = 0` on a layout (`[layout]` in a mod's ini) keeps all of its panes
in the box - Arceus' title has it built in. `LARECOMP_GAMEUI_NOWIDE=1` leaves the runtime's own
edits out; `LARECOMP_GAMEUI=0` all of it (research).

### How it works

`runtime/hle/gameui.c` (the same file in both games) hooks `nn::ui2d::Layout::Calculate` (Z-A 2.0.2 `0x7002634188`,
Arceus 1.1.1 `0x7000134e38`): the edits are put on the panes' fields (translate `+0x30`, scale `+0x48`, size `+0x50`,
flags `+0x58` - bit 0 shown, bit 4 "matrix to be made again" -, alpha `+0x59`, name `+0xb0`; a TextBox's text
`+0xd8` / `+0x110` / `+0x112` / `+0x114`) before the game's function and taken off after. `Layout::Draw` (Z-A
`0x7002634720`, Arceus `0x70001353e0`) is hooked to tell the renderer which layout its draws are (a `CMD_UI` record in
the command list, `nvn/cmdbuf.c`) and how it is fitted; `nvn/vulkan/exec.c` places them. A "wide" layout's projection
(`DrawInfo +0x10`) is made as wide as the screen while it is calculated and drawn. Cost: ~6 us a frame on Z-A's field
with the ultrawide edits, ~15 us on Arceus' (its `auto` anchors); nothing to speak of without edits.

C: `runtime/gameui.h` - `gameui_set(layout, pane, &spec, GUI_OWNER_*)`, `gameui_unset`, `gameui_layouts`,
`gameui_panes`, `gameui_text`. The mod API (version 7): `ui_set(layout, pane, key, value)`, `ui_unset`, `ui_layouts`,
`ui_panes`, `ui_text`, `ui_clone`.

# The world

The second half of the API (`contrib/legendslua/world.lua` on the game's `runtime/worldapi.h`): scripts talking between
players, the player held and walked, the party and boxes, battles, objects and effects, traces, the camera, weather.
Same in both games, with any differences noted. Much of it runs the game's own script commands (`game.script` / `game.run`),
so **those answers come a frame later, to a callback** (`world.trace(a, b, function(hit) ... end)`).

## Networking: `net`, `players`, `chat`

Over the shared world: the online server's relay (Options > Online, `world_players`), the same link the other players'
stand-ins come over. Without a shared world nothing is sent (`net.send` returns false).

```lua
net.receive("score", function(from, t) log(from, "scored", t.points) end)
net.send("score", {points = 10, where = player.pos()})            -- everyone else
net.send("hello", {text = "just you"}, players.list()[1])          -- one player (a player table or its id)
for _, p in ipairs(players.list()) do log(p.id, p.pos, p.standin, p.dist) end
chat.say("Hi!")                                                   -- over your head here, over your stand-in there
on("chat", function(from, text) log(from, "said", text) end)
```

| | |
|---|---|
| `net.send(name, data [, to])` | `data`: nil, booleans, numbers, strings, `vec`s and tables of them (8 deep). `to`: a player (`players.list()` entry) or its id; leave it out for everyone. Returns true when sent. |
| `net.receive(name, fn(from, data))` | `from` is the sender's id (16 hex digits). Several handlers per name; every mod's run. |
| `players.list()` | The other players heard in the last 10 s: `{id, pos, yaw, standin, dist}` (`standin`: the character showing them is in the world here). |
| `players.me()` / `players.online()` | Our id / whether the shared world is on. |
| `chat.say(text)` | A chat line: a speech bubble over our player here, and over our stand-in on everyone else's screen; the `chat` event fires there. One line a second. |
| `chat` event | `(from, text)`: a line heard (its bubble is shown by the runtime, scripts or not). |

Console: `chat <text>`, `players`. **The chat key**: T on the keyboard, or L + ZL held on a controller, while the shared
world is on: the runtime's text dialog (the on-screen keyboard with a controller - `input.text`), and what was typed is
said. `chat.key(false)` switches the key off (a mod using T itself), `chat.prompt()` opens the dialog from a script.
Checked with two Z-A copies: T on one, the line over its stand-in on the other (`zaKey-chat-grid.png`).

* **Caps**: a message is at most 1024 bytes (name and data packed); each copy sends at most 30 messages and 8 KB a
  second (bursts of 60 / 16 KB), and takes no more than that from any one player (more is dropped, logged once a minute).
  Chat: one line a second each way per player, 100 characters.
* **Delivery**: the relay is UDP. A message travels in 80-byte pieces (the relay passes 96-byte packets) and is
  dropped whole if a piece is lost (2 s). Send again what must arrive. `to` is checked by the receiver: everyone in the
  world receives the pieces, so don't send secrets.
* **What a message can do**: it's data, never code. A receiver's handler should check what it gets like any input from
  the internet.
* **The runtime side** (`runtime/hle/luanet.c`, the same in both games): pieces are packet type 10 in Z-A's world
  protocol (`hle/warp.c`, header + 80 bytes) and `'L' '1'` + 80 bytes in Arceus' (`hle/pla_world.c`). The sender's id is
  Z-A's own player id (as its positions), Arceus' the relay's (the server's word: `larecomp_world_self` in the relay
  plugin). Chat bubbles use `bubble.h`'s `BUBBLE_FN` target with the world code's `world_player_feet(id)`.
* Tested (2026-11-19): Z-A two copies over the UDP test transport (`LARECOMP_GHOST=<mine>,<theirs>`) and one copy
  hearing itself (`LARECOMP_GHOST_SELF=1`); Arceus two copies through a local `lzaserver --no-accounts`. Messages with
  tables and vecs both ways, a 950-byte message (15 pieces), one sent to a player and one to an unknown id (not delivered),
  60 of a 100-message burst sent (the cap), chat over the other copy's stand-in
  (`testout/lua-world/zaNet-chat-grid.png`, `plaNet-A-sees-B-chat2.png`).

## The player held and walked

| | |
|---|---|
| `player.freeze(true [, {camera = true}])` / `player.freeze(false)` | Every button but + and - kept from the game, both sticks at rest (`camera = true` leaves the right stick to the player). Scripts still see the buttons (`on("button")`, `player.button`), so a frozen player can drive a script's own controls. |
| `player.hold({"A", "B"}, sticks)` | Just these buttons kept from the game; `sticks` `"left"`, `"right"`, `"both"`. `player.hold()` lets go. |
| `player.walk(x, y)` / `player.walk(false)` | The left stick given to the game (-1..1, `y` forward, as the player's own stick: relative to the camera) until `player.walk(false)`. Works while frozen: a script walks the player. |
| `player.anim(n)` | One of the game's player animations. **Z-A**: `FObj_Set_Animation("Player", n)` (1 standing, 74 receiving, 174 talking). **Arceus**: the player animation controller's `Action(n)` (16 yes, 17 no, 12 passing an item). |
| `player.anim_loop(n [, false])`, `player.pose(n [, false])` | **Arceus**: `LoopAction(n)` (9 looking at the phone) / `PhotoPoseAction(n)` (1-15) and back. **Z-A**: the same animation held / `FObj_Reset_AnimationAll`. |
| `player.face_expression(name)` | **Z-A**: `"smile"`, `"serious"`, `"sad"`... |
| `player.dismount()`, `player.riding(fn)` | **Arceus**: off the ride Pokémon (the ride manager's `StopRide`, as every event does) / `fn(true)` while riding. |
| `player.speed(k)` | How fast the player moves, times k (0-10]. **Arceus**: the player's animation controller's master speed (`SetAnimationMasterSpeed`): the walk and run carry the player by their root motion, so faster steps go further - checked 1x 8.4 m, 2x 19.7 m in the same 2 s of stick; all the player's animations play that much faster. A reload puts 1 back. **Z-A**: false - not found (its commands have no speed; tried 2026-11-19: no animation-rate or move-speed command in the framework). |

Input is held in the runtime (`hle/input.c` asks `world_input_filter`, `hle/worldcommon.c`): a reload of the scripts
lets the player go. Windows: both games' exes and `legendslua.dll` booted under Wine with the test mod (party, box levels, traces, `player.speed`,
`battle_start`, an effect: `wine-pla-world.png`, `wine-pla-battle.png`, `wine-za-world.png`). Checked: Z-A frozen, a test's stick push moved nothing; unfrozen it moved 4.5 m; `player.walk(0, 1)`
walked on. **Mounting** from a script: the only path (the ride manager's `StartOdoshishiRide`, Wyrdeer) hung Arceus when
called from the field script's state - left out.

## The camera: `camera`

```lua
camera.look(player.pos() + vec(0, 1.2, 0), {from = player.ahead(3) + vec(0, 1.6, 0), time = 1})
camera.path({{from = a, at = b, time = 2}, {from = c, at = d, time = 3}})
camera.reset(0.5)   -- the game's own camera back, eased over 0.5 s
```

| | |
|---|---|
| `camera.look(target [, opts])` | The event camera at `opts.from` (default 4 m behind the player, 2.2 m up) looking at `target` (a spot, `"player"`, or a character), moving there over `opts.time` seconds (default 1; 0 at once), `opts.fov` degrees (50). |
| `camera.path(shots)` | Each `{from, at, time, fov}` in turn. |
| `camera.reset([seconds])` | Back to the game's camera. |

**Z-A**: `Camera_Move_Angle(time, eye, {pitch, yaw, roll}, fov, 0, 3)` with pitch = up from level and yaw 180 off the
way it looks (checked: a shot from 3 m in front of the player shows the player, `za10-cam-front-grid.png`); reset:
`S060E4139F53705A4(seconds)` + its wait, then what an event's end does (`S87C02A63ACAD9232(false)`,
`SC818B7D84F7486E2(true)`). **Arceus**: `CameraManager` `EventCameraStart` / `EventCameraMove(CameraInterpolationParameter)` /
`EventCameraEnd` (checked, `pla2-cam-grid.png`). While the event camera is on, the player still moves (freeze them if
the shot must hold).

## Traces: `world.trace`, `world.ground`

```lua
world.trace(player.pos() + vec(0, 1, 0), player.pos() + player.forward() * 40, function(hit)
  if hit then log("wall at", hit.pos, hit.dist, hit.normal, hit.what) end
end)
world.ground(player.ahead(10), function(y) log("ground height", y) end)
```

`hit = {pos, dist, normal, what}` or nil. **Z-A**: the framework's own ground ray (`SED24081F313B33A9(from, to)`,
collision groups 8 and 2 - the ground and buildings): no normal. **Arceus**: the player character's field physics
`CastRayToLayerMask(from, to, FieldPhysics.GetGroundLayersBitField())`: position, normal and the collision's raw id.
Checked both (straight down: the player's feet; forward: the wall 12-23 m away).

## Objects and effects: `world.effect`, `world.prop`

```lua
local fx = world.effect{path = "field/item/kira/ef_item_kira", pos = player.ahead(3), scale = 3}   -- Z-A
local fx = world.effect{path = "bin/effect/battle/particle/ew399/ew399_hit.ptcl", pos = player.ahead(3), scale = 2.5}  -- Arceus
fx:move(p) fx:rotate(pitch, yaw, roll) fx:scale(s) fx:remove()
local rock = world.prop{model = "world/ik_obj_template/effect/field/gimmick/field_item303a", pos = player.ahead(3), yaw = 90}  -- Z-A
rock:physics(false) rock:fall(5) rock:show(false) world.props_clear()
```

* **Effects**: the game's particle effects by path. **Z-A**: a path under `ik_effect/data/` without `.ptcl`
  (`CreateEffect` / `IsSetUpEffect` / position / rotation / scale / `PlayEffect` / `RemoveEffect`; there are about 2,500:
  `field/gimmick/holovator/holovator_warp`, `field/event/sand_app/sand_app_01`...). **Arceus**: a `.ptcl` path
  (`PlayParticleVfx` / `IsLoadedParticle` / `EmitParticle` / `SetPosParticle` / `KillParticle`). Checked both
  (`za-fx-cam-grid.png`, `pla6-grid.png`). Z-A clears its effects when one of its events ends.
* **Props** (Z-A): the game's object templates - a model with its look and collision (`CreateObjectTemplate`; 449
  `.trsot` under `world/ik_obj_template/`) or a `.trsot` by its whole path (`trsot =`, the café objects' make). Made
  hidden and shown once loaded; the script drives their load (`Commands.Update` each frame until they're made). The
  game removes props only all together: `world.props_clear()` (`p:remove()` hides one). `p:physics(on)` switches its
  collision and gravity (`S10A25E1BCEA2D263`), `p:fall(d)` drops it onto the ground (`FObj_Falling`), `p:move` /
  `p:rotate` move its object. A game event's end clears them too. Checked: a thorn-vine gimmick made in front of the
  player and cleared (`za2-fx-prop.png`, `za-fx2-grid.png`).
* **Arceus**: no free model or ground-item make in its scripts; Pokémon (`pokemon.spawn`) and people (`npc.spawn`) are.
  Items lying on the ground: neither game's scripts make them.

## The party and the boxes: `party`, `box`, `pokemon.give`

```lua
for i, m in ipairs(party.list()) do log(i, m.species, m.level, m.nickname, m.shiny, table.concat(m.moves, ",")) end
local m = box.get(1, 1)
m:set{nickname = "Sparky", moves = {84, 39}, ivs = {31, 31, 31, 31, 31, 31}}
local b, s = pokemon.give{species = 25, level = 12, shiny = true, name = "Gifty", moves = {84, 39}}
```

A Pokémon read is a table: `species` (national dex), `form`, `level`, `exp`, `nickname`, `nicknamed`, `shiny`,
`alpha`, `gender` (0 male, 1 female, 2 none), `egg`, `moves` (4 numbers), `pp`, `ivs` (HP Atk Def Spe SpA SpD), `nature`,
`ability`, `item`, `ball`, `hp`, `stats` (where the format has them), `ot`, `tid`, `sid`, `pid`, `ec`; `m:bytes()` is the
whole decrypted data (PKHeX's `.pa9` / `.pa8`). Reading is decoding the game's own data as PKHeX does (`hle/worldcommon.c`:
PokeCrypto; the layouts from PKHeX's PA9 / PA8).

| | |
|---|---|
| `party.get(i)` / `party.list()` / `party.count()` | The party (1-6). See below for what each game knows of it. |
| `party.ready()` / `party.refresh([fn])` | Whether the party is known yet / ask the game again now. |
| `box.get(b, s)` / `box.list(b)` | Box `b` (1-32) slot `s` (1-30), as the game holds it (LiveHeX's pointers). |
| `box.put(b, s, bytes)` | A whole Pokémon (another's `m:bytes()`, a PKHeX file's data) into a slot. |
| `box.empty_slot()` | The first empty slot: `b, s`. |
| `m:set{...}` | Writes into its slot at once (a move the game doesn't have - Z-A's smaller move list - isn't shown) (checksum and encryption as the game's): `nickname`, `moves` (PP 20 for a new one), `item`, `form`, `nature`, `ivs`, `shiny` (the PID made to match the trainer's ids, as PKHeX), `exp` (+ `level` with it), `hp`, `ball`, `species`. The game shows it when it next reads the slot (the box screen). |
| `pokemon.give{species, level, form, shiny, name, nickname, moves, ball, alpha}` | A new Pokémon into the first empty box slot: one of the player's own as the mould (their trainer data), species / form / level / moves / colours / a new EC and PID / random IVs set. Its EXP is the level's on that species' growth curve, found from another of that species the player has (level and EXP), else the medium-fast one. `name` is what the box shows (default "Pokémon"). Returns box, slot. |

* **Z-A**: the boxes keep the party format (level and stats are there). The party's members are the game's own
  objects: where each one's data is is asked of the game's Lua (its Player object's party: member -> `[ud]` -> `+8` ->
  `+0x50` -> `+0x30`, checked before it's used) every 3 s once a script uses the party; that data is the stored part
  (no level: the level comes from the member's own `fD2EF1BBB()`). `m:set` on a party member writes the game's own data
  (checked: renamed in the box screen). Checked (`za13-boxes.png`, `za14-edit-grid.png`): a shiny Pikachu Lv 12 given
  into box 1 (the game worked out its stats: 31/31 HP, its moves Thunder Shock + Tail Whip), an Eevee renamed with new
  moves, the party's Totodile renamed.
* **Arceus**: boxes are the stored format (no level in a box - the EXP is): a box Pokémon's `level` is its EXP on its
  species' growth curve (`m.growth`), the curve read from the game's own personal table in its romfs
  (`bin/pml/personal/personal_data_total.perbin`, read once by the runtime: `hle/worldcommon.c`; 1,276 species / forms) -
  checked against the game's own `PokemonCommand.GetLevel` for every box slot of the test save (all equal; under Wine too).
  `pokemon.give` uses the same curves. The party the game plays with is its own
  objects; the save's party block (`[[main+42BA6B0]+200]+68`, 6 x 0x178) is written when the game saves, so a party
  member's names, IVs and trainer come from there and what changes in play (species, form, level, HP, max HP, shiny,
  alpha, moves) from the game's `PokemonCommand` reads (every 3 s once used; `m.live`). `m:set` on a party member takes
  `hp` and `form` only (`PokemonCommand.SetHp` / `ChangeFormNo`; checked HP 20/53 in the satchel, `pla6-grid.png`).
  `pokemon.give` writes a box slot (read back right; not looked at in a pasture).
* Writing is writing the save's data. Test on a copy of the save first; the game keeps what you write when it saves.

## Battles: `battle`

| | |
|---|---|
| `battle.wild(id [, {pos, yaw, field, mode}])` | **Arceus**: a wild battle against one of the game's event encounters (`bin/pokemon/data/poke_event_encount.bin`: `"aipom01"` Aipom Lv 30, `"yukimenoko01"` Froslass Lv 55, the Mount Coronet `mut_*` ones...), started as its events start theirs (`AppCommand.ExecuteEventCommand("call_wild_battle", ...)`), where the player stands. Checked (`pla3-battle-grid.png`). These are challenge battles: the player can't run from them. **Z-A**: false - its battles start from its C++ triggers. |
| `battle_start` event | `{kind}` - a battle began (kind `"unknown"`: not told apart yet). |
| `battle_end` event | `{result, party_hp}` - it ended. `result`: `"caught"` (a Pokémon more in the party or the boxes than at its start), `"lost"` (the whole party fainted), else `"other"` (won or ran: the game keeps no result a script can read for a battle no event of its own started). |
| `battle.active()` / `battle.last()` | A battle on now / the last one's result. |

**How a battle is seen (Arceus)**: the game's C++, not its Lua (its Lua runs on through a battle: the same calls in and
out). Profiling which of the game's functions run in a window of 60 frames (the runtime's `LARECOMP_FNPROF_LIVE`) in the
field and through a battle (its start, its turns, its menus - the satchel, a Pokémon's summary) gave about 100 that run
exactly once a frame from the battle's first moments to its end and never in the field; the runtime hooks one of them
(0x7000898db0, `hle/worldapi.c`) once a script asks: a battle is on while it ran within the last half second. Checked:
`battle_start` fired as the scripted Aipom battle began, its satchel and summary screens kept it on, under Wine too.
The end of a battle wasn't reached in the tests (a challenge battle can't be run from, and losing it by pad automation
didn't finish - the end is that function stopping; the result's check runs 1.5 s after). The profile windows are kept
(`testout/lua-world/fnprof-pla`). **Z-A**: not found - no battle could be started in the test save (its battles come from
wild zones and trainers' triggers; the test save stands in a plaza).

## Weather: `weather`

| | |
|---|---|
| `weather.get(fn)` | **Z-A**: `fn(id)` - the weather its getter reports (`c4B6212B8.f6362F6BB()`: 0 clear, 2 rain...). **Arceus**: `fn(nil)`. |
| `weather.is(name, fn)` | **Z-A**: its own check (`IsCheckWeather("rain" / "storm")`). |
| `weather.set(id)` | false: no setter found. |

What was traced (2026-11-19):
* **Z-A**: its framework's weather calls (`FixWeather`, `SyncWeather`, `ResetWeather`, `BeginSyncWeather` - a seed and a
  lottery result) are empty in the Lua, and no event sets weather (its story's special scenes fix a light instead:
  `FixLightWithPriority(10, "light/spl/...trlgt", 0)`). The getter's path: native 0x7002761dc0 -> 0x700299af90 -> the
  time manager's handle (0x700090de20) -> environment `+0x40` -> 0x7002d92a18 -> the current weather record (0x7002d93e40)
  `+0x10`. What reads that index for the sky and the rain isn't found (setting it changed only what's reported). A rain
  look a script can make: `world.effect{path = "field/weather/et001/et001_rain_wide_range"}`.
* **Arceus**: its scripts have no weather calls; the strings the weather has in the game are its shaders' parameters
  (`GlobalWeatherScale`, `EnableWeatherLayer`), its wet / snow looks and its weather particles - and loading one of those
  particles (`ef_weather_03_camfix_01.ptcl`) as an effect **crashed the game** (they need the weather system's resources:
  don't). `bin/field/weather/weather_table.bin`, named in the game, isn't in its romfs; each area has a
  `local_weather.bin`. Save flags `FSYS_FORCE_WEATHER_ENABLE`, `FSYS_WEATHER_DISABLE_*` exist (`game.work("flag:...")`),
  read by its C++ - untested.

## The world's cost

Nothing until a script uses it: the world part's work each frame is a few table checks (messages and chat lines waiting,
every 4th frame the battle state); the party is asked of the game only once a script has used it (then every 3 s, a
job on the game's thread); traces, effects, the camera and the like are one job each when called.

Measured 2026-11-19 (Core i9-12900K, other games sharing the CPU), the test mod (`net.receive` / `chat` / battle
handlers, a message and a party read every second, console commands), `lua.stats()`:

| | per frame |
|---|---|
| Z-A, 60 fps, the shared world on (a copy hearing itself) | 8.4 us on average (most 176 us: a frame with a message decoded and the party job's answer) |
| Arceus, 30 fps | 19 us on average (most 542 us: a party read - the save's party block decrypted, 6 slots) |
| Windows builds under Wine (the same test mod; the v6 overlay merged in): Arceus / Z-A | 48 / 39 us (most 436 / 451 us) |

The runtime side: a script message costs its pieces' sends (80 bytes each) and, heard, a copy into the queue; held input
is one test in the controller read. A frame is 16,700 us at 60 fps: the world part is 0.05-0.1% of it.

# Quests

A quest library is built into Legends Lua (`contrib/legendslua/lib/quest.lua`, the same file in both games): every mod
has `quest`, and `include("lib/quest.lua")` returns it (a mod's own `lua/lib/quest.lua`, if it has one, wins). A quest
is a table - a giver, what they say, steps from templates, rewards, what it needs first - and the library does the rest:
the giver in the world with a **!** over them, the offer window (**Accept** / **Not now**), a tracker with the objective
and a marker over where to go, progress kept per save, a **?** to hand it in, the rewards, and a journal. Legends
Studio's **Quest Builder** writes these tables from a form (`lua/quests/<id>.lua`, its source `<id>.json`, and the
loader `lua/quests.lua`); they can be written by hand too.

```lua
local quest = include("lib/quest.lua")

quest.define{
  id = "lost_charm",                       -- unique in the mod (its progress is kept under it)
  title = "The Lost Charm",
  description = "Mireille's lucky charm went missing in the plaza. Help her find it.",
  giver = {name = "Mireille", pos = {-564.4, 0.14, -252.3}, yaw = 168, model = "npc_A04_z02_02_0032"},
  npcs = {{name = "Rene", pos = {-560, 0.14, -244}, yaw = 200}},   -- (the quest's own characters, made while it's on)
  lines = {
    offer = {"Oh no, oh no... my lucky charm is gone!", "Could you help me look for it?"},
    accept = "Thank you!", decline = "Oh... all right.", progress = "Any sign of my charm?",
    done = {"My charm, whole again!", "Please, take these."}, after = "I'll never lose it again!",
    locked = "Come back later.",           -- (said while what it needs isn't done)
  },
  steps = {
    {kind = "talk", npc = "Rene", text = "Ask Rene about the charm", lines = {"A Fletchling took it!"}},
    {kind = "go", pos = {-552, 0, -258}, radius = 4, place = "the street corner"},
    {kind = "collect", spots = {{-557, 0, -262}, {-554, 0, -250}}, one = "a charm piece"},
    {kind = "show", npc = "giver", species = 661, what = "a Fletchling", missing = "Do you have a Fletchling?"},
    {kind = "time", when = "night"},
  },
  rewards = {items = {{id = 4, n = 5, name = "Poke Ball"}}, pokemon = {{species = 25, level = 10, name = "Pikachu"}}, money = 0},
  requires = {story = 30000, quests = {"another_quest"}},
}
```

| Field | |
|---|---|
| `giver` | `{name, pos, yaw, model, outfit, body, route}` - a character the quest makes (as `npc.spawn`), or the name of one another script makes (`"Studio NPC 1"`). Arceus: made once the player is in the quest's `area`. |
| `area` | (Arceus) the area its places are in: `"ha_area01"`. |
| `npcs` | The quest's own characters, made while it is on; a step names one with `npc = "<name>"` (or `"giver"`). |
| `lines` | `offer`, `accept`, `decline`, `progress`, `done`, `after`, `locked`: a line or a list (each a page: A for the next). |
| `steps` | In order; each `{kind, text, lines, ...}`: `text` is the objective shown (left out: the library's own words), `lines` what is said when it is done (by the step's character, or as a notice). |
| `rewards` | `items = {{id, n, name}}` (Z-A: the game's own "You obtained..." window), `pokemon = {{species, level, shiny, form, name}}` (into the first empty box slot, `pokemon.give`), `money` (Arceus). |
| `requires` | `story` (Z-A `SYS_WORK_PHASE`, Arceus `WPHA_STORY` at least that), `quests` (ids done first, this mod's or `"<mod>/<id>"`). |
| `turn_in` | `false`: the rewards come with the last step (no going back to the giver). |
| `auto_accept` | `true`: taken without the window. |
| `mark` | `false`: no **!** over the giver. |
| `dialogue` | (Z-A) `"overlay"`: the overlay's box instead of the game's message window. |
| `native` | (Arceus) the game's request list entry it shows as (`"sub_901"`, made by Studio): its progress is set to 10 when accepted and 255 when done. |

**Step kinds**

| kind | fields | done when |
|---|---|---|
| `talk` | `npc` | the player talks to that character |
| `go` | `pos`, `radius` (5), `place` | the player is within `radius` of `pos` (and within 6 m up or down) |
| `collect` | `spots`, `radius` (1.6), `one`, `what` | the player has walked over every spot (each one sparkles when near; a notice counts them) |
| `catch` | `species`, `count` (1), `what` | there are `count` more of the species in the party and the boxes than when the step began (counted a box at a time: a first round of the 32 boxes takes about 16 s) |
| `show` | `npc`, `species`, `what`, `missing` | the player talks to the character with the species in the party |
| `item` | `npc`, `item`, `count`, `take` (true), `missing` | the player talks to the character with `count` of the item in the bag; it is taken. **Arceus** counts the bag (`Bag():GetItemNum` / `SubItem`); **Z-A** has no reader of the bag's counts yet: the character takes the player's word |
| `battle` | `npc`, `encounter` | (Arceus) talking to the character starts a wild battle (`battle.wild(encounter)`); done when it ends, not lost (the `battle_end` event). Z-A: turned into a `custom` step (no battles from scripts) |
| `time` | `when` (`"night"` / `"day"`) | (Z-A) the game's `IsDayTime()` says so. Arceus: no clock reader yet - the step passes at once |
| `wait` | `seconds` | that many seconds of play (kept across restarts) |
| `custom` | | the mod's own code calls `quest.advance(id)` |

**In the game**

* **Talking**: the characters get the talk event (Arceus: the game's own "Ⓐ *name*" prompt; Z-A: A facing them).
  **Z-A** says the lines in the game's own message window (its runtime labels `lza_say_8`..`15`; one coroutine of the
  game's for all of a talk's pages, `__LQ_SEQ` set at its end tells the library it was closed). **Arceus** has no
  own-text window yet: a dialogue box drawn on the overlay with the speaker's name, A for the next page, the player held
  meanwhile (`player.freeze`, camera free).
* **The offer**: talking to the giver says `offer`, then a window - the description, the first objective, the
  rewards, Accept / Not now (controller, mouse, keyboard; opened once A is let go, so the A that closed the talk doesn't
  accept it).
* **The tracker**: top right of the interface box: the quest's title and objective (with n / m), and a diamond over
  where to go with the metres to it (a character's head; a place's spot), held at the screen's edge when off screen.
  The quest tracked is the last one that moved (or the one picked in the journal).
* **The journal**: J on the keyboard, R + d-pad down on a controller, `quest journal` in the console, `quest.journal()`
  - the quests on now (pick one to track it), the ones done, the tracker on / off.
* **Progress** is the mod's per-save storage (`quest.<id>` in `save_<n>/lzr_moddata/<mod>.lua`): `s` none / active / ready / done, the
  step, and a step's own values (spots found, a catch's starting count, a wait's time left). It is written when the
  game saves, with the rewards it gave: quit without saving and the quest is back where the save has it.
* **Notices**: "New quest: ...", each objective as it comes, "Found ... (2/3)", "Quest complete: ...".

| | |
|---|---|
| `quest.define(t)` | A quest (see above). Errors in the table are raised at once, with the step and the field. |
| `quest.state(id)` / `quest.step(id)` / `quest.objective(id)` | `"none"`, `"active"`, `"ready"` (hand it in), `"done"` / the step / the objective's text. |
| `quest.start(id)` / `quest.advance(id)` / `quest.complete(id)` / `quest.reset(id)` | Start it, the next step, done now (rewards given), forgotten (for tests). |
| `quest.track(id)` / `quest.journal([open])` / `quest.list()` | |
| events | `quest_started (id)`, `quest_step (id, step done)`, `quest_progress (id, n, of)`, `quest_done (id)`. |
| console | `quests` (every quest and where it is), `quest start / next / done / reset / track <id>`, `quest step <id> <n>`, `quest journal`. |

**Cost**: nothing until a mod defines a quest. Then a check a few times a second (the steps, a giver's spawn), a box
read every half second while a catch step is on, the J key's handler (scripts then tick every frame), and the overlay
while there is something to show (the tracker, a **!** near, a talk box). Measured with `lua.stats` (i9-12900K, other
games sharing the CPU): Z-A 87 us a frame on average over a whole playthrough (tracker and marker drawn most of it;
most 960 us, a talk's job and a reload), 38 us with the quest done; Arceus 70 us (most 500 us).
`LEGENDSLUA_QUEST_LIB=<file>` runs that file instead of the built-in copy (working on the library: read again at every
reload).

**Checked in the game (2026-11-20)**, script-driven (teleports and pad presses) on test saves:
* Z-A, The Lost Charm (`examples/lostcharm`): the **!**, the offer in the game's window, Accept, a second character,
  a place, three spots, a Fletchling shown, night (`game.set_time(21)`), the **?**, handed in, 5 Poké Balls (the game's
  window) and a Pikachu in box 1, the journal; progress kept across a restart of the game (mid-quest and done).
* Arceus, The Scout's Lost Supplies (`examples/scoutsupplies`): the game's Talk prompt over the giver, the overlay
  talk box, Not now then Accept, a dressed second character, a place, three spots, a Bidoof (put in a box with
  `pokemon.give` as a stand-in for a catch), 3 Poké Balls taken (8 -> 5), handed in: 5 Poké Balls (5 -> 10), 500 money,
  an Eevee in box 1, the journal; with Studio's request list entry (sub_901) its progress read back from the game: 10
  once accepted, 255 when done (the copied request's own reward set to 0: nothing extra given).
* Not working: a character copied from a placement whose character has no navigator (Arceus `area01_npc_senior`) is
  never made (the runtime asks again forever) - leave `model` empty or use another.
* Following the save (2026-11-20, API version 8), both games: the quest saved part way (Z-A's menu Save Progress,
  Arceus' trainer card Save), finished with its rewards, the game quit without saving: back at the saved step, the
  rewards gone (Arceus: Poké Balls 8, money 4,367, no Eevee; Z-A: no Pikachu); finished and saved, then quit: done, with
  the rewards (Arceus: 10, 4,867, the Eevee; Z-A: the Pikachu in box 1), the journal saying so.
* Z-A: the crash seen once in a quest character's spawn after a night change (guest 0x70005e92ec) is FIXED (2026-11-20,
  `hle/newnpc.c`): the spawn read past the runtime's 0x300-byte copy of the station block into whatever followed it in
  the HLE heap; the block is now 0x2040 bytes, zero past the copy (made reliable with `LARECOMP_NEWNPC_ROOM=0x300
  LARECOMP_NEWNPC_FILL=1`; 121 spawns across 12 day / night changes after the fix).
* Z-A Windows build (under Wine): turning or moving a script character (`n:face_player()`, `walk_to`, the quest's talk)
  crashed it (c0000005 accessing 0) - FIXED 2026-11-20 (`hle/newnpc.c`): a character's node is updated by more than one
  game thread, and the move setter published "ready" before allocating its scratch, so the other thread passed a scratch
  of 0 (on Windows hle_alloc's commit held that window open every time). Checked under Wine: the Lost Charm played through
  and the Town Walk walkers walking; Arceus' Windows build plays the quest too.
Shots and logs: `testout/quests/` (final-za, final-pla, fix-za, fix-pla).
