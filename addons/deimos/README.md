# Grotti Deimos GTS (harithd, ported to IV by Milcin) on the PS5 port

An add-on car on PC, and here too: its own vehicle (`deimos`), with the mod's
own handling and colour combinations. Spawn **Car Spawning → Grotti Deimos
GTS**. In-game it shows the Turismo's name (and has its engine sound), as the
mod's line chooses.

## Pop-up headlights

In the car, **d-pad left** raises or lowers the lamp pods, and the headlights
follow (pods up: lights on, pods down: lights off). The pods swing up on
their hinge in about a third of a second. The d-pad doesn't change the radio
in the Deimos. *Vehicle Options → Deimos Pop-ups* switches to **Automatic**:
up whenever IV turns car lights on (night, rain, fog).

On PC, ZolikaPatch (a patch the port cannot run) turns the pods 30°. Here:

- The mod's pods are a box per side behind the front bumper: housing
  (`misc_y`), rear wall and floor (`misc_m`), front lamps (`misc_w`,
  `misc_p`) and lenses (`misc_headlight_l/r`), all hung from `misc_a`, the
  hinge at their back edge, flush with the bonnet.
- The car is built on the stock **Banshee**, whose `extra_1` is a bone no
  physics part drives. The converter binds every pod part to it and moves it
  to the hinge, in every copy of the skeleton's pose.
- The trainer turns that bone the way the game itself poses a bone: it
  writes the bone's local matrix in the car's live skeleton (the one
  `sub_822832E8` hands the game when it hides an extra), turning it about the
  car's width, front edge rising, to 30°. The extra is kept switched on (the
  game picks extras at random, and a hidden one is sent to the world's
  origin).

Converted with:
`run_vehicle.py ... deimos banshee "extra_1=misc_a,misc_y,misc_w,misc_p,misc_m,misc_headlight_l,misc_headlight_r"`

## Limits

| | |
|---|---|
| Doors | open and close: the Banshee's door joints moved to the Deimos's hinges (0.39 m forward), its door and window on them (since v1.7.8) |
| Bonnet, boot, glass | fixed to the body: they don't open or break (the Deimos hinges its engine cover and front boot at the other end from the Banshee's lids) |
| Moving steering wheel | a ZolikaPatch feature: the wheel is fixed |
| Collision | the Banshee's (the template) |
| Physics wheels | the mod's own size (0.33 m); on the Banshee's 0.38 m wheels it floated 5 cm |
| Tuning parts (`misc_*` bones) | the non-tuning version, as the mod's Addon folder ships it |
