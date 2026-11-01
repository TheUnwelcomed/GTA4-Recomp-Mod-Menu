# Dinka Nexus (model by IGNO, ported to IV by Milcin) on the PS5 port

An add-on car on PC, and here too: its own vehicle (`nexus`), with the mod's
handling and colour combinations. Spawn **Car Spawning → Dinka Nexus**. Its
game name is its own (`NEXUS`, which IV has no engine sound for), so the
engine you hear is the trainer's synthesized one (`src/gta4_car_engine.h`,
the physically informed model since v1.7.18: the early-90s NSX's 3-litre V6
the car is drawn from, firing evenly to 8,000 rpm, the intake hardening into
a wail as its VTEC cam comes in at 5,800). Its doors latch and slam there
too. Up to v1.7.16 it borrowed the Infernus's name and engine, as the mod's
line chooses.

Built from the mod's `nexus.wft` and its *No liveries* `nexus.wtd` (the
liveries need ZolikaPatch), on the stock **Banshee**:

```
run_vehicle.py <export> <work> nexus.wft nexus.wtd out nexus banshee \
    "extra_1=light_cover;overheat=door_dside_f;overheat_2=door_pside_f"
```

| | |
|---|---|
| Body, interior, wheels, lamps | the mod's (108 geometries, one LOD: the game uses it at every distance), 22/22 checks |
| Paint | the Banshee's paint1 (the mod's paint3). The mod's colour list puts its body colours first, as paint1 takes them, so it is used as it comes |
| Doors | the mod's gullwing doors turn about the car's length (71 degrees up, hinged along the roof), the Banshee's about the vertical, so the game can't swing them. Each door panel (with its window) rides a pivot of its own instead, the Banshee's `overheat` and `overheat_2` points moved to the mod's hinges; the game still opens and shuts the Banshee's doors there, unseen, as Niko gets in and out, and the trainer raises each gullwing as far as the game has its door open (`GET_DOOR_ANGLE_RATIO`), with a strut hiss going up and a clunk coming down. (Up to v1.7.16 they stayed shut.) Engine smoke on a damaged Nexus may rise from those points, on the roof |
| Pop-up headlights | the pods (housings, lamps, lenses: the mod's `light_cover`) on the Banshee's `extra_1`, hinged where the mod's `light_cover` is; the trainer swings them 30 degrees up (the lamps sit in the pods tipped 30 degrees down, so they then face forward) the way it does the Deimos's: up and lit getting in, down and off getting out, d-pad left in between |
| Handling | the mod's, but 6 gears instead of its 7 (stock IV cars have at most 6) |
| Physics wheels | the mod's own size (0.347 m) |
| Bonnet, boot, glass | fixed to the body |
| Moving steering wheel, liveries | ZolikaPatch features: not here |
| Collision | the Banshee's |
| Data lines | `vehicles_ide.txt` (frequency 0, the ZolikaPatch flags left out, the game name `NEXUS`), `handling.txt`, `carcols.txt`; placed beside the Infernus |
