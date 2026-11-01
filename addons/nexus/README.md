# Dinka Nexus (model by IGNO, ported to IV by Milcin) on the PS5 port

An add-on car on PC, and here too: its own vehicle (`nexus`), with the mod's
handling and colour combinations. Spawn **Car Spawning → Dinka Nexus**. It
shows the Infernus's name and has its engine sound, as the mod's line chooses.

Built from the mod's `nexus.wft` and its *No liveries* `nexus.wtd` (the
liveries need ZolikaPatch), on the stock **Banshee**:

```
run_vehicle.py <export> <work> nexus.wft nexus.wtd out nexus banshee
```

| | |
|---|---|
| Body, interior, wheels, lamps | the mod's (108 geometries, one LOD: the game uses it at every distance), 22/22 checks |
| Paint | the Banshee's paint1 (the mod's paint3). The mod's colour list puts its body colours first, as paint1 takes them, so it is used as it comes |
| Doors | the mod's gullwing doors turn about the car's length, the Banshee's about the vertical: they stay shut, fixed to the body (the game's own door swings unseen as you get in) |
| Handling | the mod's, but 6 gears instead of its 7 (stock IV cars have at most 6) |
| Physics wheels | the mod's own size (0.347 m) |
| Bonnet, boot, glass | fixed to the body |
| Moving steering wheel, liveries | ZolikaPatch features: not here |
| Collision | the Banshee's |
| Data lines | `vehicles_ide.txt` (frequency 0, the ZolikaPatch flags left out), `handling.txt`, `carcols.txt`; placed beside the Infernus |
