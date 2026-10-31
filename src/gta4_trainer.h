// Liberty Trainer - a single-player trainer menu built into the port, modeled
// on sjaak327's Simple Native Trainer for GTA IV (the menu everyone used on
// PC): player options, car spawning, teleports (waypoint included), time,
// weather and weapons, driven with the pad.
//
// Open and close it with L3 + R3. While it is open the game sees no pad input
// (the same ownership rule the system dialogs use). Navigation: D-pad up and
// down to move, left and right to change a value, A (Cross) to pick, B
// (Circle) to go back.
//
// Everything the menu does goes through the game's own script natives, the
// way Simple Native Trainer did it: the handlers the game registers at boot
// are captured from the existing registration hook, and each action calls
// them with the game's real native-call context layout, on the game's own
// thread, from the per-frame input poll. Anything that did not resolve shows
// as unavailable instead of acting. The menu refuses to act in a network
// session: it is for single-player.
#pragma once
#include <cstdint>
#include <memory>

struct PPCContext;

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

namespace gta4::trainer {

// Called from the one existing native-registration hook (gta4_motion_hooks),
// beside gta4::quicksave::ObserveNativeRegistration. r3 = name, r4 = handler.
void ObserveNativeRegistration(PPCContext& context, uint8_t* base);

// Called once per frame from the input-poll hook (gta4_input_hooks,
// sub_828CCD60), on the game thread, before the retail poll: reads the pad
// and runs the menu. It calls no game natives — selections are queued.
void Poll(PPCContext& ctx, uint8_t* base);

// Called from the per-frame script-stage hooks (the two sites that also
// drive gta4::quicksave::Poll): the only context where the game's natives
// are safe to call. Drains the queued menu actions, maintains the toggles
// and finishes pending vehicle spawns.
void RunActions(PPCContext& ctx, uint8_t* base);

// True while the menu owns the pad; the XAM input path then gives the game a
// cleared pad, exactly as it does while a system dialog is up.
bool InputOwned();

// The in-game ImGui overlay that draws the menu. Created at game setup next
// to the FPS counter overlay; drawing is display-only (all input handling is
// in Poll, on the game thread).
std::unique_ptr<rex::ui::ImGuiDialog> CreateOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace gta4::trainer

// C linkage for the XAM input path (rexglue core cannot include gta4 headers).
extern "C" bool GTA4_TrainerOwnsGuestPad();
