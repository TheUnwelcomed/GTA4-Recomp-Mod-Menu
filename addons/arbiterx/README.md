# Imponte Arbiter X (model by johndoe968, ported to IV by Milcin) on the PS5 port

An add-on car on PC, and here too: its own vehicle (`arbiterx`), with the
mod's handling and colour combinations. Spawn **Car Spawning → Imponte
Arbiter X**. It shows the Banshee's name and has its engine sound, as the
mod's line chooses.

Built from the mod's `arbiterx.wft` and its *No liveries* `arbiterx.wtd`
(the liveries need ZolikaPatch), on the stock **Banshee**, a two-door like it:

```
run_vehicle.py <export> <work> arbiterx.wft arbiterx.wtd out arbiterx banshee
```

| | |
|---|---|
| Body, interior, wheels, lamps | the mod's (119 geometries, one LOD), 22/22 checks |
| Paint | the Banshee's paint1 (the mod's paint3) |
| Colours | `carcols.txt`: the mod's combinations with their first and third colours swapped, as for the Dominator 645 (same author): the mod lists greys first (0-20) and its coloured set third, and paint1 takes the first colour |
| Doors | open and close: the Banshee's door joints moved to the Arbiter's hinges (0.40 m), each door with its window |
| Physics wheels | the mod's own size (0.33 m) |
| Bonnet, boot, glass | fixed to the body |
| Moving steering wheel, liveries | ZolikaPatch features: not here |
| Collision | the Banshee's (the Arbiter is longer: its ends can poke into walls) |
| GPU data | 1 MB load blocks (its largest geometry needs them), like the Dominator's body |
