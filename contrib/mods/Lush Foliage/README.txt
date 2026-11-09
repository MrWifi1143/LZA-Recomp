Lush Foliage (Pokemon Legends: Arceus recomp)

More grass and more trees. Turn it on in the start screen's Mods page.

NEEDS A DECENT PC. Every extra plant is drawn by your graphics card and every extra tree costs the game CPU time each
frame. Tested on an RTX 4090 at 4K: about 0.2 ms more a frame for the plants, 60 fps held in Jubilife and the forests.
On a weaker PC, or if you see drops, lower plant_density in mod.ini (2), or turn the mod off.

- Grass, flowers and small plants: 3x as many (plant_density, 1-4), placed by the recomp around the game's own.
- Trees: 19,294 more across the seven areas - the forests about half again as full, and forest on the hills and steep
  banks round the playing areas that nobody can reach (so the land stops looking bare). They are solid like the game's
  own and stay off paths, buildings and Pokemon spawns.

The trees are made from YOUR game files the first time the game starts with the mod on (about a second; about 140 MB
in this folder's romfs\). This folder holds no game data: trees.lft is only where each new tree stands. If something
goes wrong, build-trees.log here says what.

mod.ini:
  plant_density = 3       each plant drawn this many times (1 = the game's, up to 4)
  plant_spread = 0.6      how far (metres) the extra plants stand from the one they copy
  tree_patch = trees.lft  delete this line for the plants alone, no extra trees
