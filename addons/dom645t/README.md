# Vapid Dominator 645 (johndoe968 and Rodger Dodger, ported to IV by Milcin)

An add-on car on PC, and here too: its own vehicle (`dom645t`), with the
mod's handling and colour combinations. Spawn **Car Spawning → Vapid
Dominator 645**. It shows the Banshee's name and has its engine sound, as the
mod's line chooses.

Built from the mod's *No liveries* version (the livery switching needs
ZolikaPatch), on the stock **Banshee**, a low two-door like it:

```
run_vehicle.py <export> <work> dom645t.wft dom645t.wtd out dom645t banshee
```

| | |
|---|---|
| Body, interior, wheels, lamps | the mod's (91 geometries, one LOD: the game uses it at every distance) |
| Paint | the Banshee's paint1 (the mod's paint3 without its livery layer: this is the *No liveries* version). v1.7.5-v1.7.6 borrowed the airtug's paint3 and the body came out white; paint1 takes the car's own colour, like the Deimos's |
| Colours | `carcols.txt`: the mod's combinations with their first and third colours swapped. The mod lists its greys and silvers first (1-20) and its coloured set third (32-105); paint1 takes the first colour, so up to v1.7.10 the body was always grey or silver |
| Physics wheels | the mod's own size (0.32 m); on the Banshee's 0.38 m wheels it floated 6 cm |
| Doors | open and close: the Banshee's door joints moved to the Dominator's hinges (0.28 m), each door with its windows (since v1.7.8) |
| Bonnet, boot, glass | fixed to the body |
| Moving steering wheel | a ZolikaPatch feature: the wheel is fixed |
| Collision | the Banshee's |
