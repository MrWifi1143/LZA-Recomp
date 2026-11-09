Lush Foliage (Pokemon Legends: Arceus recomp)

3x the grass, flowers and small plants. Turn it on in the start screen's Mods page.

NEEDS A DECENT PC. Every extra plant is drawn by your graphics card and every extra tree costs the game CPU time each
frame. Tested on an RTX 4090 at 4K: about 0.2 ms more a frame at 3x, 60 fps held. On a weaker PC, or if you see
drops, lower plant_density in mod.ini (2), or turn the mod off.

mod.ini:
  plant_density = 3     each grass blade, flower and small plant drawn this many times (1 = the game's, up to 4)
  plant_spread = 0.6    how far (metres) the extra plants stand from the one they copy - small keeps them off paths

More trees (in the forests, and on the hills and banks round the playing areas) are made from your own game files with
Legends Studio - they can't be shipped here (they would contain the game's data):
  legends-studio-cli --mod "mods/Lush Foliage/romfs" <your extracted romfs folder> foliage all 1.5 1 0.5
It needs the game's RomFS as a folder (not the start screen's romfs.bin). The trees are solid like the game's own.
