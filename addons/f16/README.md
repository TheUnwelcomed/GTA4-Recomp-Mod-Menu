# F-16C Fighting Falcon (SkylineGTRFreak) on the PS5 port

On PC this jet replaces the airport tug and flies through JulioNIB's **Air
Combat IV** script, a .NET plugin that cannot run on the port. Here it's its own
vehicle (`f16`, the tug is untouched), and the trainer flies it.

## Flying it

Spawn **Car Spawning → F-16 Fighter Jet** (it appears 16 m ahead; it's big).
*Vehicle Options → F-16 Flight and Weapons* is on by default.

| | |
|---|---|
| **RT** | throttle: on the ground it drives, in the air it speeds up |
| **LT** | air brake |
| **left stick back** at speed (~95 km/h) | take off |
| left stick | pitch and roll. A banked jet turns |
| **d-pad right** (hold) | nose cannon |
| **d-pad left** | missile (up to 6 in the air at once) |
| slow below ~60 km/h | it stops flying and glides down; land it |

While you're in the jet, d-pad left and right don't change the radio.

How it works: once airborne, the trainer sets the jet's attitude and its
speed along the nose every frame through the game's own natives
(`SET_VEHICLE_QUATERNION`, `SET_CAR_FORWARD_SPEED`). Physics still handles
crashes. The cannon fires real bullets (`FIRE_SINGLE_BULLET`). A missile is
the RPG's rocket model, flown ahead and exploding where it meets the ground,
a roof, or after 800 m. *Trainer Status* lists any native the game did not
provide. A feature that needs it then does nothing instead of crashing.

## Conversion notes / limits

| | |
|---|---|
| Body, cockpit, 3 LODs, textures | the jet's (the two uncompressed textures were compressed to DXT5, at 38 and 55 dB PSNR) |
| Collision and physics | the FBI Buffalo's (the converter's template): wings and tail pass through things |
| Wheels | physics wheels sit at the jet's gear (the nose wheel fills both front slots); the gear is part of the body mesh |
| Liveries (`car_sign_1`-`4`) | not carried: the template has no three-colour paint shader. Colours come from the airport tug's colour rows, as on PC |
| Flaps, gear doors (`misc_*` bones) | fixed in place |
