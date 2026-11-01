# Grotti Deimos GTS (harithd, ported to IV by Milcin) on the PS5 port

An add-on car on PC, and here too: its own vehicle (`deimos`), with the mod's
own handling and colour combinations. Spawn **Car Spawning → Grotti Deimos
GTS**. Its game name is its own (`DEIMOS`), so IV gives it no engine sound
of its own: the trainer plays a synthesized one (below). Up to v1.7.12 it
showed the Turismo's name and had its engine, as the mod's line chooses.

## Engine sound

Synthesized in the trainer (`src/gta4_car_engine.h`, `kDeimosEngine`), on
the physically informed model the add-ons share since v1.7.18: the engine of
the late-70s mid-engined Ferrari the Deimos echoes, a 3-litre flat-plane V8
(firing order 1-5-3-7-4-8-2-6, the banks taking turns, each firing evenly
every 180 degrees: a hard, even rasp, no American burble), each cylinder's
exhaust pulse varying a little from one cycle to the next, long tubular
headers merging in one muffler box, four twin-choke carburettors on short
runners (their howl and honk at full throttle), idle at 950 rpm, revs to
7,700, pops on lift-off. The revs come from the road speed through five
simulated gears; getting in the starter cranks it, getting out it winds
down. Its doors latch and slam (its game name has no IV sounds, its doors'
included), and the pods' electric motor whirs as they swing.
`tools/car/render_preview.cpp deimos` renders a scripted drive to a WAV.
(v1.7.13-v1.7.17: an abstract voice that sounded synthetic.)

## Pop-up headlights

Getting in, the pods flip up and the lamps come on; getting out, they flip
down and go off. In the car, **d-pad left** raises or lowers the lamp pods,
and the headlights follow (pods up: lights on, pods down: lights off). The pods swing up on
their hinge in about a third of a second. The d-pad doesn't change the radio
in the Deimos. *Vehicle Options → Deimos/Nexus Pop-ups* switches to **Automatic**:
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
