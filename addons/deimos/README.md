# Grotti Deimos GTS (harithd, ported to IV by Milcin) on the PS5 port

An add-on car on PC, and here too: its own vehicle (`deimos`), with the mod's
own handling and colour combinations. Spawn **Car Spawning → Grotti Deimos
GTS**. In-game it shows the Turismo's name, as the mod's line chooses.

## Limits

| | |
|---|---|
| Pop-up headlights | **don't move yet.** On PC they're animated by ZolikaPatch (the `popuplights` flag plus `[PopupLightAngles]`), a patch to the PC game the port cannot run. The covers stay closed. A later build may switch them with the lights by using a stock car with "extras" (switchable parts) as the template. This release exports some such cars from your console to `logs/export/` for that |
| Moving steering wheel | also a ZolikaPatch feature: the wheel is fixed |
| Collision | the FBI Buffalo's (the converter's template): a little taller than the car |
| Tuning parts (`misc_*` bones) | the non-tuning version, as the mod's Addon folder ships it |
