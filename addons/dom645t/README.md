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
| Paint | the mod's paint3 shader, taken from the stock airtug (the Banshee has only paint1) |
| Doors, bonnet, boot, glass | fixed to the body |
| Moving steering wheel | a ZolikaPatch feature: the wheel is fixed |
| Collision | the Banshee's |
