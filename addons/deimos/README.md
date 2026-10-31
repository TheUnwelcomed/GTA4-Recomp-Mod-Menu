# Grotti Deimos GTS (harithd, ported to IV by Milcin) on the PS5 port

An add-on car on PC, and here too: its own vehicle (`deimos`), with the mod's
own handling and colour combinations. Spawn **Car Spawning → Grotti Deimos
GTS**. In-game it shows the Turismo's name, as the mod's line chooses.

## Pop-up headlights

On PC the lamp pods are turned 30° by ZolikaPatch, a patch to the PC game the
port cannot run. Here the car is built on the stock **Banshee**, which has
three *extras* (parts the game can show or hide). The converter puts the pods
on them in three poses: extra 1 is down, extras 2 and 5 are 30° up, tilted
one way or the other. The trainer shows the right pose:

- *Vehicle Options → Deimos Pop-up Lights*: **With Headlights** (up at
  night, 20:00-06:00, and in rain or fog, when IV turns car lights on),
  **Always Up** or **Always Down**.
- *Deimos Pop-up Tilt*: **A** or **B**. Pick whichever looks right; the
  direction could not be checked without a console.

The pods switch between poses rather than swinging smoothly. Converted with:
`run_vehicle.py ... deimos banshee "misc_headlight_l,misc_headlight_r=extra_1@0,extra_2@30,extra_5@-30"`

## Limits

| | |
|---|---|
| Moving steering wheel | a ZolikaPatch feature: the wheel is fixed |
| Collision | the Banshee's (the template) |
| Tuning parts (`misc_*` bones) | the non-tuning version, as the mod's Addon folder ships it |
