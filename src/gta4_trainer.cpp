// Liberty Trainer (see gta4_trainer.h). Modeled on Simple Native Trainer's
// menu: the same categories, driven with the pad, every action through the
// game's own script natives on the game's thread. The native-call plumbing
// follows gta4_quicksave_hooks.cpp, the one proven way this port calls
// natives from host code.
#include "gta4_trainer.h"
#include "gta4_addon_install.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include "gta4_init.h"
#include "gta4_jet_engine.h"
#include "gta4_car_engine.h"
#include "gta4_beyond_engine.h"
#include "gta4_time_machine.h"
#include "gta4_island_sound.h"
#include <fstream>
#include "gta4_mission_pass.h"
#include "gta4_jet_flight.h"

#include <rex/platform.h>
#if REX_PLATFORM_PS5
#include "ps5/game/sound.h"
#if defined(__PROSPERO__)
#include "ps5/hub/ps5_hub.h"  // (its fonts, for the menu)
#endif
#endif

REXCVAR_DEFINE_BOOL(gta4_trainer, true, "GTA IV/Gameplay",
                    "The built-in single-player trainer menu (open with L3 + R3)");

namespace gta4::trainer {
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_GAMEPAD_BACK;
using rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
using rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
using rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
using rex::input::X_INPUT_GAMEPAD_DPAD_UP;
using rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
using rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
using rex::input::X_INPUT_GAMEPAD_Y;
void PerformanceMode(bool on);  // the 60 FPS Mode (defined further down, outside the unnamed namespace)

namespace {

// ---------------------------------------------------------------------------
// Guest memory helpers (the quicksave file's, kept identical in behaviour).
// ---------------------------------------------------------------------------
bool Span(uint8_t* base, uint32_t address, uint32_t size, bool write = false) {
  if (!base || !address || !size)
    return false;
  const uint64_t last64 = uint64_t(address) + size - 1;
  if (last64 > UINT32_MAX)
    return false;
  const uint32_t last = uint32_t(last64);
  auto* kernel = REX_KERNEL_STATE();
  auto* memory = kernel ? kernel->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  if (!heap || heap != memory->LookupHeap(last))
    return false;
  const auto access = heap->QueryRangeAccess(address, last);
  using rex::memory::PageAccess;
  return access == PageAccess::kReadWrite || access == PageAccess::kExecuteReadWrite ||
         (!write && (access == PageAccess::kReadOnly || access == PageAccess::kExecuteReadOnly));
}
uint32_t Read(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, address), sizeof(v));
  return __builtin_bswap32(v);
}
void Write(uint8_t* base, uint32_t address, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(rex::memory::GuestPtr(base, address), &v, sizeof(v));
}
float ReadFloat(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(Read(base, address));
}
uint32_t Bits(float v) { return std::bit_cast<uint32_t>(v); }
std::string String(uint8_t* base, uint32_t address, size_t cap = 96) {
  std::string out;
  for (size_t i = 0; i < cap; ++i) {
    if (uint64_t(address) + i > UINT32_MAX || !Span(base, address + uint32_t(i), 1))
      return {};
    const char c = char(*rex::memory::GuestPtr(base, address + uint32_t(i)));
    if (!c)
      return out;
    out.push_back(c);
  }
  return {};
}
uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// ---------------------------------------------------------------------------
// The crash notes (v1.7.43). The PS5 loses the log's tail when the game
// crashes (logs 108 and 113: the trainer's lines before the fault, written
// seconds earlier, missing from the file), so the last things that happened
// - each model the game placed, the trainer's map lines, a shader effect
// the game could not find - are kept here in host memory, a ring of short
// lines, and the port's fault handler appends them to its crash line
// (ps5_diag.cpp, GTA4_TrainerCrashNotes: no allocation, no lock). The
// writers race only over which line a slot holds.
// ---------------------------------------------------------------------------
constexpr size_t kNoteRing = 96, kNoteLine = 112;
char g_notes[kNoteRing][kNoteLine];
std::atomic<uint32_t> g_note_head{0};
void Note(std::string_view text) {
  const uint32_t n = g_note_head.fetch_add(1, std::memory_order_relaxed);
  char* line = g_notes[n % kNoteRing];
  const size_t len = std::min(text.size(), kNoteLine - 1);
  std::memcpy(line, text.data(), len);
  line[len] = 0;
}

// RAGE's atStringHash (Jenkins one-at-a-time, lowercased): model names.
uint32_t ModelHash(std::string_view name) {
  uint32_t h = 0;
  for (char c : name) {
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');
    h += uint8_t(c);
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;
  h += h << 15;
  return h;
}

// ---------------------------------------------------------------------------
// Natives: captured at registration, called with the game's context layout.
// ---------------------------------------------------------------------------
struct NativeEntry {
  std::string_view name;
  uint32_t handler = 0;
};
std::array g_natives = {
    // Context and guards.
    NativeEntry{"GET_PLAYER_ID"}, NativeEntry{"GET_PLAYER_CHAR"},
    NativeEntry{"IS_PLAYER_PLAYING"}, NativeEntry{"IS_NETWORK_GAME_RUNNING"},
    NativeEntry{"IS_WANTED_LEVEL_GREATER"}, NativeEntry{"PRINT_HELP"},
    // Player.
    NativeEntry{"SET_CHAR_INVINCIBLE"}, NativeEntry{"SET_CHAR_PROOFS"},
    NativeEntry{"SET_CHAR_HEALTH"}, NativeEntry{"ADD_ARMOUR_TO_CHAR"},
    NativeEntry{"ADD_SCORE"}, NativeEntry{"CLEAR_WANTED_LEVEL"},
    NativeEntry{"ALTER_WANTED_LEVEL"}, NativeEntry{"APPLY_WANTED_LEVEL_CHANGE_NOW"},
    // Weapons.
    NativeEntry{"GIVE_WEAPON_TO_CHAR"}, NativeEntry{"REMOVE_ALL_CHAR_WEAPONS"},
    // Vehicles.
    NativeEntry{"REQUEST_MODEL"}, NativeEntry{"HAS_MODEL_LOADED"},
    NativeEntry{"IS_MODEL_IN_CDIMAGE"}, NativeEntry{"MARK_MODEL_AS_NO_LONGER_NEEDED"},
    NativeEntry{"CREATE_CAR"}, NativeEntry{"SET_CAR_ON_GROUND_PROPERLY"},
    NativeEntry{"MARK_CAR_AS_NO_LONGER_NEEDED"}, NativeEntry{"WARP_CHAR_INTO_CAR"},
    NativeEntry{"FIX_CAR"}, NativeEntry{"SET_CAR_PROOFS"},
    NativeEntry{"IS_CHAR_IN_ANY_CAR"}, NativeEntry{"GET_CAR_CHAR_IS_USING"},
    // Teleporting.
    NativeEntry{"SET_CHAR_COORDINATES"}, NativeEntry{"GET_FIRST_BLIP_INFO_ID"},
    NativeEntry{"DOES_BLIP_EXIST"}, NativeEntry{"GET_BLIP_COORDS"},
    NativeEntry{"GET_GROUND_Z_FOR_3D_COORD"}, NativeEntry{"REQUEST_COLLISION_AT_POSN"},
    // World.
    NativeEntry{"SET_TIME_OF_DAY"}, NativeEntry{"FORCE_WEATHER_NOW"},
    NativeEntry{"RELEASE_WEATHER"},
    // Batmobile gadgets (JulioNIB's script re-done natively; see the repo).
    NativeEntry{"ADD_EXPLOSION"}, NativeEntry{"GET_CAR_MODEL"},
    NativeEntry{"SET_CAR_FORWARD_SPEED"}, NativeEntry{"GET_CAR_SPEED"},
    // F-16 flight, cannon and missiles (Air Combat IV re-done natively).
    NativeEntry{"GET_CAR_COORDINATES"}, NativeEntry{"GET_VEHICLE_QUATERNION"},
    NativeEntry{"SET_VEHICLE_QUATERNION"}, NativeEntry{"FIRE_SINGLE_BULLET"},
    NativeEntry{"CREATE_OBJECT"}, NativeEntry{"DELETE_OBJECT"},
    NativeEntry{"SET_OBJECT_COORDINATES"}, NativeEntry{"SET_OBJECT_QUATERNION"},
    NativeEntry{"SET_OBJECT_COLLISION"}, NativeEntry{"FREEZE_OBJECT_POSITION"},
    NativeEntry{"GET_OFFSET_FROM_CAR_IN_WORLD_COORDS"}, NativeEntry{"GET_CLOSEST_CAR"},
    NativeEntry{"TRIGGER_PTFX"}, NativeEntry{"TRIGGER_PTFX_ON_VEH"},
    NativeEntry{"START_PTFX_ON_OBJ"}, NativeEntry{"START_PTFX_ON_VEH"}, NativeEntry{"STOP_PTFX"},
    NativeEntry{"GET_CAR_COLOURS"},
    // Batmobile missile (a physics rocket, as TBOGT's tony8 flies its own),
    // bomb light, canopy entry.
    NativeEntry{"SET_OBJECT_INITIAL_VELOCITY"}, NativeEntry{"SET_OBJECT_RECORDS_COLLISIONS"},
    NativeEntry{"HAS_OBJECT_COLLIDED_WITH_ANYTHING"}, NativeEntry{"GET_OBJECT_COORDINATES"},
    NativeEntry{"SET_OBJECT_INVINCIBLE"}, NativeEntry{"DRAW_LIGHT_WITH_RANGE"},
    NativeEntry{"IS_CHAR_GETTING_IN_TO_A_CAR"}, NativeEntry{"CLEAR_CHAR_TASKS_IMMEDIATELY"},
    NativeEntry{"WARP_CHAR_FROM_CAR_TO_COORD"}, NativeEntry{"IS_CAR_DEAD"},
    // Setting the player down (canopies, teleports): collision, holding still
    // while the ground streams in, loading a destination.
    NativeEntry{"SET_CHAR_COLLISION"}, NativeEntry{"FREEZE_CHAR_POSITION"}, NativeEntry{"LOAD_SCENE"},
    NativeEntry{"TASK_LEAVE_CAR_IMMEDIATELY"}, NativeEntry{"SET_CHAR_VISIBLE"}, NativeEntry{"CLEAR_CHAR_TASKS"},
    NativeEntry{"SET_CAR_COORDINATES"}, NativeEntry{"FREEZE_CAR_POSITION"},
    // Passengers through the Batmobile's canopy: who is getting in (the
    // player's group, whoever is at the passenger side), the seat, the warp.
    NativeEntry{"GET_PLAYER_GROUP"}, NativeEntry{"GET_GROUP_SIZE"}, NativeEntry{"GET_GROUP_MEMBER"},
    NativeEntry{"GET_CLOSEST_CHAR"}, NativeEntry{"IS_CAR_PASSENGER_SEAT_FREE"},
    NativeEntry{"GET_CHAR_IN_CAR_PASSENGER_SEAT"}, NativeEntry{"WARP_CHAR_INTO_CAR_AS_PASSENGER"},
    NativeEntry{"IS_CHAR_SITTING_IN_CAR"}, NativeEntry{"IS_CHAR_DEAD"},
    // Batmobile missile lock: the target's mark on the radar.
    NativeEntry{"ADD_BLIP_FOR_CAR"}, NativeEntry{"CHANGE_BLIP_COLOUR"}, NativeEntry{"REMOVE_BLIP"},
    // Chaos: raining cars, gravity.
    NativeEntry{"SET_CAR_HEADING"}, NativeEntry{"SET_GRAVITY_OFF"},
    // Deimos and Nexus pop-up headlights (the pods' extra, the lamps); the
    // Nexus's gullwings follow the game's doors.
    NativeEntry{"TURN_OFF_VEHICLE_EXTRA"}, NativeEntry{"IS_VEHICLE_EXTRA_TURNED_ON"},
    NativeEntry{"GET_DOOR_ANGLE_RATIO"},
    // The Lazar 3's turret follows the camera: where it looks, and the car's heading.
    NativeEntry{"GET_GAME_CAM"}, NativeEntry{"GET_CAM_ROT"}, NativeEntry{"GET_CAR_HEADING"},
    NativeEntry{"GET_CAM_POS"},
    // The DeLorean time machines: the fire trails, the effects, the car
    // vanishing and coming back, its frost, the hover conversion's swap.
    NativeEntry{"START_SCRIPT_FIRE"}, NativeEntry{"REMOVE_SCRIPT_FIRE"}, NativeEntry{"START_PTFX"},
    NativeEntry{"SET_CAR_VISIBLE"}, NativeEntry{"SET_CAR_COLLISION"}, NativeEntry{"DELETE_CAR"},
    NativeEntry{"SET_CAR_COLOURS"}, NativeEntry{"SET_CAR_COORDINATES_NO_OFFSET"},
    NativeEntry{"GET_TIME_OF_DAY"},
    NativeEntry{"GET_CURRENT_WEATHER"}, NativeEntry{"FORCE_CAR_LIGHTS"},
    // Bring Your Own Ride: the checks it answers and the vehicle kinds.
    NativeEntry{"IS_CHAR_IN_MODEL"}, NativeEntry{"IS_CAR_MODEL"},
    NativeEntry{"IS_THIS_MODEL_A_CAR"}, NativeEntry{"IS_THIS_MODEL_A_BIKE"},
    NativeEntry{"IS_THIS_MODEL_A_HELI"}, NativeEntry{"IS_THIS_MODEL_A_BOAT"},
    // The jet's targeting (the camera, to put the targets' marks on the
    // screen) and the police helicopters that come after it.
    NativeEntry{"HAS_CAR_BEEN_DAMAGED_BY_CAR"}, NativeEntry{"HAS_CAR_BEEN_DAMAGED_BY_CHAR"},
    NativeEntry{"SET_CAN_BURST_CAR_TYRES"}, NativeEntry{"BURST_CAR_TYRE"},
    NativeEntry{"CLEAR_CAR_LAST_DAMAGE_ENTITY"},
    NativeEntry{"GET_CAM_FOV"}, NativeEntry{"CREATE_CHAR_INSIDE_CAR"}, NativeEntry{"CREATE_CHAR_AS_PASSENGER"}, NativeEntry{"TASK_HELI_MISSION"}, NativeEntry{"TASK_COMBAT"}, NativeEntry{"SET_HELI_BLADES_FULL_SPEED"}, NativeEntry{"SET_CHAR_WILL_DO_DRIVEBYS"}, NativeEntry{"SET_CHAR_KEEP_TASK"}, NativeEntry{"DELETE_CHAR"}, NativeEntry{"MARK_CHAR_AS_NO_LONGER_NEEDED"}, NativeEntry{"SET_CHAR_ACCURACY"}, NativeEntry{"IS_CHAR_INJURED"}, NativeEntry{"SET_CHAR_HEADING"},
    // v1.7.30: map destinations (the pieces placed exactly), no dragging the
    // player out of the armoured cars and aircraft.
    NativeEntry{"CREATE_OBJECT_NO_OFFSET"}, NativeEntry{"SET_CHAR_CANT_BE_DRAGGED_OUT"},
    // v1.7.32: the map's pieces checked after the arrival.
    NativeEntry{"DOES_OBJECT_EXIST"}, NativeEntry{"IS_OBJECT_ON_SCREEN"},
    // v1.7.37: a place's life - its parked cars and boats, the people about.
    NativeEntry{"CREATE_CHAR"}, NativeEntry{"SET_CHAR_RANDOM_COMPONENT_VARIATION"},
    NativeEntry{"TASK_GO_STRAIGHT_TO_COORD"}, NativeEntry{"TASK_STAND_STILL"}, NativeEntry{"DOES_CHAR_EXIST"},
    NativeEntry{"GET_CHAR_COORDINATES"}, NativeEntry{"DOES_VEHICLE_EXIST"}, NativeEntry{"FORCE_WEATHER"},
    NativeEntry{"SET_BLOCKING_OF_NON_TEMPORARY_EVENTS"}, NativeEntry{"TASK_WANDER_STANDARD"},
    // v1.7.39: the creatures (the sharks' bite).
    NativeEntry{"IS_CHAR_IN_WATER"}, NativeEntry{"GET_CHAR_HEALTH"},
    // v1.7.40: the safehouse on a place (its blip, the game's save menu at its bed).
    NativeEntry{"ADD_BLIP_FOR_CONTACT"}, NativeEntry{"CHANGE_BLIP_SPRITE"}, NativeEntry{"CHANGE_BLIP_DISPLAY"},
    NativeEntry{"CHANGE_BLIP_PRIORITY"}, NativeEntry{"CHANGE_BLIP_NAME_FROM_ASCII"}, NativeEntry{"ACTIVATE_SAVE_MENU"},
};
std::mutex g_native_mutex;

uint32_t NativeAddress(std::string_view name) {
  std::lock_guard lock(g_native_mutex);
  for (const auto& n : g_natives)
    if (n.name == name)
      return n.handler;
  return 0;
}
bool NativeKnown(std::string_view name) { return NativeAddress(name) != 0; }

// Calls a native with the game's real native context layout on a scratch
// frame below the current guest stack (the quicksave Invoke, extended with a
// seedable result slot for pointer-argument natives and a 16-byte result
// readback for vector outputs). Returns the result slot or nothing when the
// native is not available.
struct InvokeResult {
  std::array<uint32_t, 4> words{};
  uint32_t r0() const { return words[0]; }
  float f(size_t i) const { return std::bit_cast<float>(words[i]); }
};
// Handles the trainer keeps from frame to frame go stale: a wreck the game
// cleared away, a ped it removed. Several natives look a handle up and use
// what they get without checking it (GET_CHAR_IN_CAR_PASSENGER_SEAT reads the
// seat at +0xF44 of a null vehicle: the crash of a blown-up Lazar 3, v1.7.23),
// so a handle its pool no longer holds is never passed on. The pools, as the
// game's own lookups read them: vehicles at 0x82FB0700, peds at 0x831D5388
// (+0 storage, +4 a generation byte per slot, top bit set when free, +8 the
// slot count, +12 the slot size); a handle is slot << 8 | generation.
constexpr uint32_t kVehiclePoolPtr = 0x82FB0700, kPedPoolPtr = 0x831D5388;

bool HandleGone(uint8_t* base, uint32_t pool_pointer, uint32_t handle) {
  if (!Span(base, pool_pointer, 4))
    return false;  // (the pool not readable: nothing known, the call goes ahead)
  const uint32_t pool = Read(base, pool_pointer);
  if (!Span(base, pool, 16))
    return false;
  const uint32_t slot = handle >> 8, flags = Read(base, pool + 4), count = Read(base, pool + 8);
  if (count && slot >= count)
    return true;
  if (!Span(base, flags + slot, 1))
    return false;
  return *rex::memory::GuestPtr(base, flags + slot) != (handle & 0xFF);
}

// Which argument of a native the trainer uses is a vehicle or a ped handle
// (by value; `seeded`: passed by pointer, the handle in the result seed).
struct HandleUse {
  std::string_view name;
  int8_t vehicle = -1, ped = -1;
  bool seeded = false;
};
constexpr HandleUse kHandleUses[] = {
    {"ADD_BLIP_FOR_CAR", 0}, {"FIX_CAR", 0}, {"FORCE_CAR_LIGHTS", 0}, {"FREEZE_CAR_POSITION", 0},
    {"GET_CAR_COLOURS", 0}, {"GET_CAR_COORDINATES", 0}, {"GET_CAR_HEADING", 0}, {"GET_CAR_MODEL", 0},
    {"GET_CAR_SPEED", 0}, {"GET_CHAR_IN_CAR_PASSENGER_SEAT", 0}, {"GET_DOOR_ANGLE_RATIO", 0},
    {"GET_OFFSET_FROM_CAR_IN_WORLD_COORDS", 0}, {"GET_VEHICLE_QUATERNION", 0},
    {"IS_CAR_PASSENGER_SEAT_FREE", 0}, {"IS_VEHICLE_EXTRA_TURNED_ON", 0}, {"SET_CAR_COLLISION", 0},
    {"SET_CAR_COLOURS", 0}, {"SET_CAR_COORDINATES", 0}, {"SET_CAR_FORWARD_SPEED", 0},
    {"SET_CAR_HEADING", 0}, {"SET_CAR_ON_GROUND_PROPERLY", 0}, {"SET_CAR_PROOFS", 0},
    {"SET_CAR_VISIBLE", 0}, {"SET_HELI_BLADES_FULL_SPEED", 0}, {"SET_VEHICLE_QUATERNION", 0},
    {"TURN_OFF_VEHICLE_EXTRA", 0}, {"CREATE_CHAR_INSIDE_CAR", 0}, {"CREATE_CHAR_AS_PASSENGER", 0},
    {"SET_CAN_BURST_CAR_TYRES", 0}, {"HAS_CAR_BEEN_DAMAGED_BY_CAR", 0}, {"CLEAR_CAR_LAST_DAMAGE_ENTITY", 0},
    {"BURST_CAR_TYRE", 0}, {"SET_CAR_COORDINATES_NO_OFFSET", 0}, {"START_PTFX_ON_VEH", 1},
    {"TRIGGER_PTFX_ON_VEH", 1}, {"HAS_CAR_BEEN_DAMAGED_BY_CHAR", 0, 1},
    {"WARP_CHAR_FROM_CAR_TO_COORD", -1, 0},
    {"IS_CAR_DEAD", 0}, {"DELETE_CAR", 0, -1, true}, {"MARK_CAR_AS_NO_LONGER_NEEDED", 0, -1, true},
    {"ADD_ARMOUR_TO_CHAR", -1, 0}, {"CLEAR_CHAR_TASKS", -1, 0}, {"CLEAR_CHAR_TASKS_IMMEDIATELY", -1, 0},
    {"FREEZE_CHAR_POSITION", -1, 0}, {"GET_CAR_CHAR_IS_USING", -1, 0}, {"GIVE_WEAPON_TO_CHAR", -1, 0},
    {"IS_CHAR_DEAD", -1, 0}, {"IS_CHAR_INJURED", -1, 0}, {"IS_CHAR_GETTING_IN_TO_A_CAR", -1, 0},
    {"IS_CHAR_IN_ANY_CAR", -1, 0}, {"IS_CHAR_SITTING_IN_CAR", 1, 0}, {"REMOVE_ALL_CHAR_WEAPONS", -1, 0},
    {"SET_CHAR_ACCURACY", -1, 0}, {"SET_CHAR_COLLISION", -1, 0}, {"SET_CHAR_COORDINATES", -1, 0},
    {"SET_CHAR_HEALTH", -1, 0}, {"SET_CHAR_INVINCIBLE", -1, 0}, {"SET_CHAR_KEEP_TASK", -1, 0},
    {"SET_CHAR_PROOFS", -1, 0}, {"SET_CHAR_VISIBLE", -1, 0}, {"SET_CHAR_WILL_DO_DRIVEBYS", -1, 0},
    {"SET_CHAR_HEADING", -1, 0}, {"TASK_COMBAT", -1, 0}, {"TASK_HELI_MISSION", 1, 0},
    {"TASK_LEAVE_CAR_IMMEDIATELY", 1, 0}, {"WARP_CHAR_INTO_CAR", 1, 0},
    {"WARP_CHAR_INTO_CAR_AS_PASSENGER", 1, 0}, {"DELETE_CHAR", -1, 0, true},
    {"MARK_CHAR_AS_NO_LONGER_NEEDED", -1, 0, true}, {"SET_CHAR_CANT_BE_DRAGGED_OUT", -1, 0},
    {"SET_CHAR_RANDOM_COMPONENT_VARIATION", -1, 0}, {"TASK_GO_STRAIGHT_TO_COORD", -1, 0},
    {"TASK_STAND_STILL", -1, 0}, {"DOES_CHAR_EXIST", -1, 0}, {"GET_CHAR_COORDINATES", -1, 0},
    {"DOES_VEHICLE_EXIST", 0}, {"SET_BLOCKING_OF_NON_TEMPORARY_EVENTS", -1, 0}, {"TASK_WANDER_STANDARD", -1, 0},
    {"IS_CHAR_IN_WATER", -1, 0}, {"GET_CHAR_HEALTH", -1, 0},
};

// Whether the call must not go ahead: a handle in it the game no longer has.
bool StaleHandleCall(uint8_t* base, std::string_view name, std::initializer_list<uint32_t> values,
                     uint32_t result_seed) {
  for (const HandleUse& use : kHandleUses) {
    if (use.name != name)
      continue;
    auto arg = [&](int i) -> std::optional<uint32_t> {
      if (i < 0)
        return std::nullopt;
      if (use.seeded && i == 0)
        return result_seed;
      if (size_t(i) >= values.size())
        return std::nullopt;
      return *(values.begin() + i);
    };
    bool vehicle_gone = false, ped_gone = false;
    if (const auto v = arg(use.vehicle))
      vehicle_gone = HandleGone(base, kVehiclePoolPtr, *v);
    if (const auto p = arg(use.ped))
      ped_gone = HandleGone(base, kPedPoolPtr, *p);
    if (vehicle_gone || ped_gone) {
      // (GET_CHAR_IN_CAR_PASSENGER_SEAT turns an empty seat into a made-up
      // handle, which IS_CHAR_SITTING_IN_CAR is then asked about: expected)
      static std::atomic<uint32_t> logged{0};
      if (logged.fetch_add(1, std::memory_order_relaxed) < 6)
        REXLOG_INFO("gta4-trainer: {} not called: its {} handle is not one the game has", name,
                    vehicle_gone ? "vehicle" : "ped");
    }
    return vehicle_gone || ped_gone;
  }
  return false;
}

std::optional<InvokeResult> Invoke(PPCContext& parent, uint8_t* base, std::string_view name,
                                   std::initializer_list<uint32_t> values = {},
                                   int output_argument = -1, uint32_t result_seed = 0,
                                   int output_count = 1, int text_argument = -1,
                                   std::string_view text = {}, int output_word = 0) {
  if (StaleHandleCall(base, name, values, result_seed)) {
    // A vehicle or ped that is gone is dead as far as anyone asking goes.
    if (name == "IS_CAR_DEAD" || name == "IS_CHAR_DEAD" || name == "IS_CHAR_INJURED") {
      InvokeResult dead;
      dead.words[0] = 1;
      return dead;
    }
    return std::nullopt;
  }
  const uint32_t address = NativeAddress(name);
  auto* runtime = rex::Runtime::instance();
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  auto* function = dispatcher && address ? dispatcher->GetFunction(address) : nullptr;
  if (!function || values.size() > 12 || text.size() > 0xEF || parent.r1.u32 < 0x200)
    return std::nullopt;
  const uint32_t frame = (parent.r1.u32 - 0x200u) & ~15u;
  if (!Span(base, frame, 0x200, true))
    return std::nullopt;
  // context, result slot, up to 12 arguments, then a string argument's text
  const uint32_t context = frame + 0x80, result = frame + 0xC0, arguments = frame + 0xD0,
                 text_at = frame + 0x100;
  if (text_argument >= 0) {
    auto* to = rex::memory::GuestPtr(base, text_at);
    std::memcpy(to, text.data(), text.size());
    to[text.size()] = 0;
  }
  Write(base, frame, parent.r1.u32);
  Write(base, context, result);
  Write(base, context + 4, uint32_t(values.size()));
  Write(base, context + 8, arguments);
  Write(base, context + 12, 0);
  Write(base, result, result_seed);
  Write(base, result + 4, 0);
  Write(base, result + 8, 0);
  Write(base, result + 12, 0);
  size_t i = 0;
  // Pointer arguments: `output_count` consecutive ones from
  // `output_argument`, each aimed at its own word of the result slot (a
  // native returning x, y, z through three pointers fills words 0-2).
  for (const auto value : values) {
    // (output_word: the first output's word, past a returned value's)
    const int k = int(i) - output_argument;
    const bool out = output_argument >= 0 && k >= 0 && k < std::min(output_count, 4 - output_word);
    Write(base, arguments + uint32_t(i) * 4,
          int(i) == text_argument ? text_at : (out ? result + uint32_t(k + output_word) * 4 : value));
    ++i;
  }
  PPCContext call = parent;
  call.r1.u32 = frame;
  call.r3.u32 = context;
  call.lr = 0x82844BA0;
  function(call, base);
  InvokeResult out;
  for (size_t w = 0; w < out.words.size(); ++w)
    out.words[w] = Read(base, result + uint32_t(w) * 4);
  return out;
}

// Calls a game function (not a native) with up to three arguments on the
// same kind of scratch frame; returns its r3.
std::optional<uint32_t> CallGuest(PPCContext& parent, uint8_t* base, uint32_t address, uint32_t r3,
                                  uint32_t r4 = 0, uint32_t r5 = 0) {
  auto* runtime = rex::Runtime::instance();
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  auto* function = dispatcher ? dispatcher->GetFunction(address) : nullptr;
  if (!function || parent.r1.u32 < 0x200)
    return std::nullopt;
  const uint32_t frame = (parent.r1.u32 - 0x200u) & ~15u;
  if (!Span(base, frame, 0x200, true))
    return std::nullopt;
  Write(base, frame, parent.r1.u32);
  PPCContext call = parent;
  call.r1.u32 = frame;
  call.r3.u32 = r3;
  call.r4.u32 = r4;
  call.r5.u32 = r5;
  call.lr = 0x82844BA0;
  function(call, base);
  return call.r3.u32;
}

// A script vehicle handle's CVehicle, as the game's own handle lookups do it
// (IS_CAR_MODEL's worker, sub_82594230): the vehicle pool at 0x82FB0700
// (+0 storage, +4 a generation byte per slot, +12 the slot size), the slot
// in the handle's upper bits, its generation in the low byte.
uint32_t VehicleAddress(uint8_t* base, uint32_t handle) {
  constexpr uint32_t kVehiclePool = 0x82FB0700;
  if (!handle || !Span(base, kVehiclePool, 4))
    return 0;
  const uint32_t pool = Read(base, kVehiclePool);
  if (!Span(base, pool, 16))
    return 0;
  const uint32_t slot = handle >> 8, storage = Read(base, pool), flags = Read(base, pool + 4),
                 size = Read(base, pool + 12);
  if (!Span(base, flags + slot, 1) || *rex::memory::GuestPtr(base, flags + slot) != (handle & 0xFF))
    return 0;
  const uint32_t vehicle = storage + slot * size;
  return Span(base, vehicle, 64) ? vehicle : 0;
}

// ---------------------------------------------------------------------------
// The per-frame game context.
// ---------------------------------------------------------------------------
// From the existing ports' hooks (gta4_sony_feedback.cpp): the ped's matrix
// pointer and position/forward rows, for spawn placement and saved positions.
constexpr uint32_t kPedMatrix = 32, kMatrixForward = 16, kMatrixPosition = 48, kPedInline = 16;

struct GameContext {
  bool valid = false;
  bool online = false;
  uint32_t player = 0;
  uint32_t ped = 0;  // script handle (GET_PLAYER_CHAR), used for natives
  uint32_t ped_address = 0;  // guest CPed pointer, used only to read the matrix
  float position[3]{};
  float forward[3]{};
};

// The player-info pointer chain the feedback hooks use, for the CPed address
// (natives use handles; the matrix needs the object).
constexpr uint32_t kPrimaryPlayer = 0x82A98778, kPlayerInfoTable = 0x82C01C70;
constexpr uint32_t kPedOffset = 1400;

// Raw world gate, no natives: true once the local player exists and is
// playing. The input poll uses this alone; everything heavier runs in the
// script stage.
bool WorldAlive(uint8_t* base) {
  if (!Span(base, kPrimaryPlayer, 4))
    return false;
  const uint32_t index = Read(base, kPrimaryPlayer);
  if (index >= 16 || !Span(base, kPlayerInfoTable + index * 4, 4))
    return false;
  const uint32_t info = Read(base, kPlayerInfoTable + index * 4);
  if (!info || !Span(base, info, kPedOffset + 4) || Read(base, info + 1232) != 2)
    return false;
  const uint32_t ped = Read(base, info + kPedOffset);
  return ped && Span(base, ped, 2692);
}

GameContext ReadContext(PPCContext& ctx, uint8_t* base) {
  GameContext out;
  // The world gate comes FIRST, as raw span-checked reads, before any native
  // is called: the player natives dereference the player info without a null
  // check, so calling them during boot or loading (the info table is still
  // empty) is the crash the first console run hit (a page fault at
  // info+~0x4CE, IS_PLAYER_PLAYING reading a null info's state field). The
  // same chain the port's DualSense feedback walks, with the same checks.
  if (!Span(base, kPrimaryPlayer, 4))
    return out;
  const uint32_t index = Read(base, kPrimaryPlayer);
  if (index >= 16 || !Span(base, kPlayerInfoTable + index * 4, 4))
    return out;
  const uint32_t info = Read(base, kPlayerInfoTable + index * 4);
  if (!info || !Span(base, info, kPedOffset + 4))
    return out;
  // info+1232 is the player state; 2 means playing (gta4_sony_feedback.cpp).
  if (Read(base, info + 1232) != 2)
    return out;
  const uint32_t ped_address = Read(base, info + kPedOffset);
  if (!ped_address || !Span(base, ped_address, 2692))
    return out;
  out.ped_address = ped_address;
  const uint32_t matrix = Read(base, ped_address + kPedMatrix);
  uint32_t position = 0, forward = 0;
  if (matrix && Span(base, matrix, 64)) {
    position = matrix + kMatrixPosition;
    forward = matrix + kMatrixForward;
  } else if (!matrix) {
    position = ped_address + kPedInline;
  }
  for (int axis = 0; axis < 3; ++axis) {
    if (position)
      out.position[axis] = ReadFloat(base, position + uint32_t(axis) * 4);
    if (forward)
      out.forward[axis] = ReadFloat(base, forward + uint32_t(axis) * 4);
  }
  // Only now, with a live player behind them, the natives.
  const auto player = Invoke(ctx, base, "GET_PLAYER_ID");
  if (!player)
    return out;
  const auto playing = Invoke(ctx, base, "IS_PLAYER_PLAYING", {player->r0()});
  if (!playing || !playing->r0())
    return out;
  out.player = player->r0();
  const auto ped = Invoke(ctx, base, "GET_PLAYER_CHAR", {out.player, 0}, 1);
  if (!ped || !ped->words[0])
    return out;
  out.ped = ped->words[0];
  const auto online = Invoke(ctx, base, "IS_NETWORK_GAME_RUNNING");
  out.online = online && online->r0() != 0;
  out.valid = true;
  return out;
}

// ---------------------------------------------------------------------------
// Bring Your Own Ride. A mission that wants a particular vehicle checks for it
// with IS_CHAR_IN_MODEL ("is the player in a <model>?") or IS_CAR_MODEL ("is
// this car a <model>?"), and the police computer with
// IS_CHAR_IN_ANY_POLICE_VEHICLE. With the option on, those answer yes for the
// vehicle the player is in whenever it is the same kind as the one asked
// about (anything on wheels for anything on wheels, a helicopter for a
// helicopter, a boat for a boat), so no script ever treats a car as a
// helicopter. The replacements are registered in the game's place at boot,
// through the thunks the motion hooks use, and pass straight through while
// the option is off. A mission's own story vehicle (the van it hands you) is
// a different check and still counts.
// ---------------------------------------------------------------------------
std::atomic<bool> g_any_ride{false};
std::atomic<uint32_t> g_ride_calls{0};  // checks scripts asked (for the cost line)

// The script thread whose native is running (rage::scrThread*, set by the
// interpreter only for the length of a native call: gta4_input_hooks.cpp),
// and its program key (+8: joaat of the script's name).
constexpr uint32_t kExecutingScriptThread = 0x8319277C;
constexpr uint32_t kThreadProgramKey = 8;
uint32_t ExecutingThread(uint8_t* base) {
  if (!Span(base, kExecutingScriptThread, 4))
    return 0;
  const uint32_t thread = Read(base, kExecutingScriptThread);
  return thread && Span(base, thread, 0x100) ? thread : 0;
}
uint32_t ExecutingScriptKey(uint8_t* base) {
  const uint32_t thread = ExecutingThread(base);
  return thread ? Read(base, thread + kThreadProgramKey) : 0;
}
// The forklift: the car wash, the toll booths and the bridge and dock
// patrols ask "is this car a forklift?" only to turn it away. v1.7.21 and
// earlier answered yes for any car with Bring Your Own Ride on, so the car
// wash never started (Vlad's "take Jimmy's car to the car wash").
constexpr uint32_t kForklift = 0x58E49664;

enum class Kind : uint8_t { kOther, kRoad, kHeli, kBoat };

struct Ride {
  uint64_t at = 0;  // when it was looked up (NowMs)
  uint32_t ped = 0, car = 0, model = 0;
  Kind kind = Kind::kOther;
};
Ride g_ride;
std::unordered_map<uint32_t, Kind> g_model_kinds;

Kind ModelKind(PPCContext& ctx, uint8_t* base, uint32_t model) {
  if (const auto it = g_model_kinds.find(model); it != g_model_kinds.end())
    return it->second;
  auto is = [&](std::string_view native) {
    const auto r = Invoke(ctx, base, native, {model});
    return r && r->r0() != 0;
  };
  Kind kind = Kind::kOther;
  if (is("IS_THIS_MODEL_A_CAR") || is("IS_THIS_MODEL_A_BIKE"))
    kind = Kind::kRoad;
  else if (is("IS_THIS_MODEL_A_HELI"))
    kind = Kind::kHeli;
  else if (is("IS_THIS_MODEL_A_BOAT"))
    kind = Kind::kBoat;
  if (g_model_kinds.size() < 2048)
    g_model_kinds.emplace(model, kind);
  return kind;
}

// The player's ped and vehicle, looked up at most every 100 ms (scripts ask
// many times a frame), never in a network session.
const Ride& PlayerRide(PPCContext& ctx, uint8_t* base) {
  const uint64_t now = NowMs();
  if (now - g_ride.at < 100)
    return g_ride;
  g_ride = Ride{};
  g_ride.at = now;
  if (!WorldAlive(base))
    return g_ride;
  const auto online = Invoke(ctx, base, "IS_NETWORK_GAME_RUNNING");
  if (!online || online->r0())
    return g_ride;
  const auto player = Invoke(ctx, base, "GET_PLAYER_ID");
  if (!player)
    return g_ride;
  const auto ped = Invoke(ctx, base, "GET_PLAYER_CHAR", {player->r0(), 0}, 1);
  if (!ped || !ped->words[0])
    return g_ride;
  g_ride.ped = ped->words[0];
  const auto in_car = Invoke(ctx, base, "IS_CHAR_IN_ANY_CAR", {g_ride.ped});
  if (!in_car || !in_car->r0())
    return g_ride;
  const auto car = Invoke(ctx, base, "GET_CAR_CHAR_IS_USING", {g_ride.ped, 0}, 1);
  if (!car || !car->words[0])
    return g_ride;
  const auto model = Invoke(ctx, base, "GET_CAR_MODEL", {car->words[0], 0}, 1);
  if (!model || !model->words[0])
    return g_ride;
  g_ride.car = car->words[0];
  g_ride.model = model->words[0];
  g_ride.kind = ModelKind(ctx, base, g_ride.model);
  return g_ride;
}

enum RideHookId : size_t {
  kHookCharInModel, kHookCarModel, kHookPoliceVehicle,
  // Complete Current Mission: a look at each script as it waits for the next
  // frame (IS_PLAYER_PLAYING too, should WAIT not be a native here), and
  // Rockstar's debug keys the retail missions still ask about.
  kHookWait, kHookPlayerPlaying, kHookKeyJustPressed, kHookKeyPressed,
  kRideHookCount
};
struct RideHook {
  std::string_view name;
  PPCFunc* replacement;
  uint32_t original = 0;          // the game's handler
  PPCFunc* original_fn = nullptr;
  uint32_t thunk = 0;             // ours, registered in its place
};
void RideCharInModel(PPCContext& ctx, uint8_t* base);
void RideCarModel(PPCContext& ctx, uint8_t* base);
void RidePoliceVehicle(PPCContext& ctx, uint8_t* base);
void HookWait(PPCContext& ctx, uint8_t* base);
void HookPlayerPlaying(PPCContext& ctx, uint8_t* base);
void HookKeyJustPressed(PPCContext& ctx, uint8_t* base);
void HookKeyPressed(PPCContext& ctx, uint8_t* base);
std::array<RideHook, kRideHookCount> g_ride_hooks = {{
    {"IS_CHAR_IN_MODEL", RideCharInModel},
    {"IS_CAR_MODEL", RideCarModel},
    {"IS_CHAR_IN_ANY_POLICE_VEHICLE", RidePoliceVehicle},
    {"WAIT", HookWait},
    {"IS_PLAYER_PLAYING", HookPlayerPlaying},
    {"IS_KEYBOARD_KEY_JUST_PRESSED", HookKeyJustPressed},
    {"IS_KEYBOARD_KEY_PRESSED", HookKeyPressed},
}};

bool RideHooksInstalled() {
  return g_ride_hooks[kHookCharInModel].thunk || g_ride_hooks[kHookCarModel].thunk;
}

// The native call context: +0 the address of the result slot, +8 the
// arguments, four bytes each.
uint32_t NativeArg(uint8_t* base, uint32_t native, uint32_t i) {
  return Read(base, Read(base, native + 8) + i * 4);
}
void NativeSayYes(uint8_t* base, uint32_t native) {
  if (const uint32_t slot = Read(base, native))
    Write(base, slot, 1);
}
// Runs the game's own handler; true when the option is on and it said no.
bool GameSaidNo(RideHookId id, PPCContext& ctx, uint8_t* base) {
  const uint32_t native = ctx.r3.u32;
  g_ride_calls.fetch_add(1, std::memory_order_relaxed);
  if (auto* original = g_ride_hooks[id].original_fn)
    original(ctx, base);
  if (!g_any_ride.load(std::memory_order_relaxed))
    return false;
  const uint32_t slot = Read(base, native);
  return slot && Read(base, slot) == 0;
}

void RideCharInModel(PPCContext& ctx, uint8_t* base) {
  const uint32_t native = ctx.r3.u32;
  if (!GameSaidNo(kHookCharInModel, ctx, base))
    return;
  const uint32_t ped = NativeArg(base, native, 0), model = NativeArg(base, native, 1);
  // Only a mission's own question (not the car wash's, a date's, a friend's
  // or the taxi job's), and never about the forklift.
  if (model == kForklift || !gta4::missions::IsMissionScript(ExecutingScriptKey(base)))
    return;
  const Ride& ride = PlayerRide(ctx, base);
  if (ped && ped == ride.ped && ride.car && ride.kind != Kind::kOther &&
      ModelKind(ctx, base, model) == ride.kind)
    NativeSayYes(base, native);
}

void RideCarModel(PPCContext& ctx, uint8_t* base) {
  const uint32_t native = ctx.r3.u32;
  if (!GameSaidNo(kHookCarModel, ctx, base))
    return;
  const uint32_t car = NativeArg(base, native, 0), model = NativeArg(base, native, 1);
  if (model == kForklift || !gta4::missions::IsMissionScript(ExecutingScriptKey(base)))
    return;
  const Ride& ride = PlayerRide(ctx, base);
  if (car && car == ride.car && ride.kind != Kind::kOther &&
      ModelKind(ctx, base, model) == ride.kind)
    NativeSayYes(base, native);
}

void RidePoliceVehicle(PPCContext& ctx, uint8_t* base) {
  const uint32_t native = ctx.r3.u32;
  if (!GameSaidNo(kHookPoliceVehicle, ctx, base))
    return;
  const uint32_t ped = NativeArg(base, native, 0);
  const Ride& ride = PlayerRide(ctx, base);
  if (ped && ped == ride.ped && ride.car && ride.kind == Kind::kRoad)
    NativeSayYes(base, native);
}

// ---------------------------------------------------------------------------
// Complete Current Mission. The mission ends through its own code, as if
// played to the end (its rewards, the story moving on, the save prompt):
//  - most retail missions still hold Rockstar's debug pass, a check of the
//    S key (IS_KEYBOARD_KEY_[JUST_]PRESSED(31)) that runs their "mission
//    passed" routine: for that mission's script that question is answered
//    yes for a few seconds;
//  - the others are moved to the stage that runs it: their stage variables
//    (statics, at the base of the script's stack) set to the values whose
//    case calls it, the statics found by values the script set once at its
//    start (a signature), so nothing is written unless they all match.
// Which way, and the values, are worked out from the decompiled scripts
// (tools/missions, gta4_mission_pass.h). The look happens inside the
// mission's own WAIT, on its thread, between two of its frames.
// ---------------------------------------------------------------------------
enum class CompleteResult : int { kLooking, kStaged, kKeyed, kNoStatics, kNotSupported, kOddStage };
std::atomic<uint64_t> g_complete_until{0};
std::atomic<bool> g_complete_active{false};
std::atomic<int> g_complete_result{int(CompleteResult::kLooking)};
std::atomic<uint32_t> g_complete_key{0}, g_complete_thread{0};
std::atomic<uint64_t> g_complete_seen{0};  // when the mission's thread was last seen waiting
std::atomic<uint32_t> g_statics_offset{0xFFFFFFFFu};  // where the statics' pointer sits in a thread (logged)

bool StaticMatches(uint8_t* base, uint32_t statics, const gta4::missions::StaticValue& v) {
  const uint32_t at = statics + 4u * v.index;
  if (!Span(base, at, 4))
    return false;
  if (!v.is_float)
    return int32_t(Read(base, at)) == v.i;
  const float f = ReadFloat(base, at);
  return std::fabs(f - v.f) <= 1e-4f * std::max(1.0f, std::fabs(v.f));
}

// The script's statics: a pointer in its thread (the stack's base) at which
// every value of the signature is found.
uint32_t FindStatics(uint8_t* base, uint32_t thread, const gta4::missions::Pass& pass) {
  if (pass.signature_count < 4)
    return 0;
  for (uint32_t off = 0; off < 0x100; off += 4) {
    const uint32_t p = Read(base, thread + off);
    if (!p || (p & 3) || !Span(base, p, 4))
      continue;
    for (uint32_t delta = 0; delta <= 0x40; delta += 4) {
      bool all = true;
      for (int k = 0; k < pass.signature_count && all; ++k)
        all = StaticMatches(base, p + delta, pass.signature[size_t(k)]);
      if (all) {
        if (g_statics_offset.exchange(off << 8 | delta) != (off << 8 | delta))
          REXLOG_INFO("gta4-trainer: a script's statics: thread+0x{:X} points {} bytes before them", off, delta);
        return p + delta;
      }
    }
  }
  return 0;
}

void MissionLook(uint8_t* base) {
  if (!g_complete_active.load(std::memory_order_relaxed))
    return;
  const uint32_t thread = ExecutingThread(base);
  if (!thread)
    return;
  const uint32_t key = Read(base, thread + kThreadProgramKey);
  if (!gta4::missions::IsMissionScript(key))
    return;
  const uint64_t now = NowMs();
  if (key == g_complete_key.load(std::memory_order_relaxed))
    g_complete_seen.store(now, std::memory_order_relaxed);
  if (g_complete_result.load(std::memory_order_relaxed) != int(CompleteResult::kLooking))
    return;
  g_complete_key.store(key, std::memory_order_relaxed);
  g_complete_seen.store(now, std::memory_order_relaxed);
  const auto* pass = gta4::missions::FindPass(key);
  if (!pass || (!pass->by_key && !pass->write_count)) {
    REXLOG_INFO("gta4-trainer: mission script {:08X} ({}) running; no way to complete it from the menu", key,
                pass ? std::string(pass->name) : std::string("not in the table"));
    g_complete_result.store(int(CompleteResult::kNotSupported), std::memory_order_relaxed);
    return;
  }
  if (pass->by_key) {
    g_complete_thread.store(thread, std::memory_order_relaxed);
    g_complete_result.store(int(CompleteResult::kKeyed), std::memory_order_relaxed);
    REXLOG_INFO("gta4-trainer: completing {}: its debug pass key answered yes (thread {:08X})",
                std::string(pass->name), thread);
    return;
  }
  const uint32_t statics = FindStatics(base, thread, *pass);
  if (!statics) {
    REXLOG_WARN("gta4-trainer: completing {}: its statics were not found (thread {:08X}); nothing written",
                std::string(pass->name), thread);
    g_complete_result.store(int(CompleteResult::kNoStatics), std::memory_order_relaxed);
    return;
  }
  // Sane stage values only (small numbers): anything else, nothing written.
  for (int k = 0; k < pass->write_count; ++k) {
    const int32_t now_value = int32_t(Read(base, statics + 4u * pass->writes[size_t(k)].index));
    if (now_value < -1 || now_value > 255) {
      REXLOG_WARN("gta4-trainer: completing {}: static {} holds {}, not a stage; nothing written",
                  std::string(pass->name), pass->writes[size_t(k)].index, now_value);
      g_complete_result.store(int(CompleteResult::kOddStage), std::memory_order_relaxed);
      return;
    }
  }
  for (int k = 0; k < pass->write_count; ++k) {
    const auto& w = pass->writes[size_t(k)];
    const uint32_t at = statics + 4u * w.index;
    REXLOG_INFO("gta4-trainer: completing {}: static {} {} -> {}", std::string(pass->name), w.index,
                int32_t(Read(base, at)), w.i);
    Write(base, at, uint32_t(w.i));
  }
  g_complete_result.store(int(CompleteResult::kStaged), std::memory_order_relaxed);
}

void CallOriginal(RideHookId id, PPCContext& ctx, uint8_t* base) {
  if (auto* original = g_ride_hooks[id].original_fn)
    original(ctx, base);
}
void HookWait(PPCContext& ctx, uint8_t* base) {
  MissionLook(base);  // (before the wait: the thread's state as the script left it)
  CallOriginal(kHookWait, ctx, base);
}
void HookPlayerPlaying(PPCContext& ctx, uint8_t* base) {
  CallOriginal(kHookPlayerPlaying, ctx, base);
  MissionLook(base);
}
void DebugKey(RideHookId id, PPCContext& ctx, uint8_t* base) {
  const uint32_t native = ctx.r3.u32;
  CallOriginal(id, ctx, base);
  if (!g_complete_active.load(std::memory_order_relaxed) ||
      g_complete_result.load(std::memory_order_relaxed) != int(CompleteResult::kKeyed))
    return;
  if (NativeArg(base, native, 0) != 31 || ExecutingThread(base) != g_complete_thread.load(std::memory_order_relaxed))
    return;
  static uint32_t logged = 0;
  if (logged++ < 4)
    REXLOG_INFO("gta4-trainer: the mission asked for its debug pass key: yes");
  NativeSayYes(base, native);
}
void HookKeyJustPressed(PPCContext& ctx, uint8_t* base) { DebugKey(kHookKeyJustPressed, ctx, base); }
void HookKeyPressed(PPCContext& ctx, uint8_t* base) { DebugKey(kHookKeyPressed, ctx, base); }

// ---------------------------------------------------------------------------
// The menu: Simple Native Trainer's categories, sized for a pad.
// ---------------------------------------------------------------------------
struct VehicleChoice {
  std::string_view label, model;
  float distance = 5.0f;  // spawn this far ahead of the player
};
constexpr std::array kVehicles = {
    VehicleChoice{"Infernus", "INFERNUS"},     VehicleChoice{"Turismo", "TURISMO"},
    VehicleChoice{"Comet", "COMET"},           VehicleChoice{"Banshee", "BANSHEE"},
    VehicleChoice{"Super GT", "SUPERGT"},      VehicleChoice{"Coquette", "COQUETTE"},
    VehicleChoice{"Sultan RS", "SULTANRS"},    VehicleChoice{"Sultan", "SULTAN"},
    VehicleChoice{"Sentinel", "SENTINEL"},     VehicleChoice{"Feltzer", "FELTZER"},
    VehicleChoice{"Cognoscenti", "COGNOSCENTI"}, VehicleChoice{"Huntley", "HUNTLEY"},
    VehicleChoice{"Patriot", "PATRIOT"},       VehicleChoice{"Cavalcade", "CAVALCADE"},
    VehicleChoice{"Sabre GT", "SABREGT"},      VehicleChoice{"Dukes", "DUKES"},
    VehicleChoice{"NRG 900", "NRG900"},        VehicleChoice{"PCJ 600", "PCJ"},
    VehicleChoice{"Sanchez", "SANCHEZ"},       VehicleChoice{"Police", "POLICE"},
    VehicleChoice{"Taxi", "TAXI"},
    VehicleChoice{"Ambulance", "AMBULANCE"},   VehicleChoice{"Firetruck", "FIRETRUK"},
    VehicleChoice{"Phantom", "PHANTOM"},       VehicleChoice{"Annihilator", "ANNIHILATOR"},
    VehicleChoice{"Maverick", "MAVERICK"},     VehicleChoice{"Buzzard (TBoGT)", "BUZZARD"},
    VehicleChoice{"APC (TBoGT)", "APC"},       VehicleChoice{"Hexer (TLAD)", "HEXER"},
    // Bundled add-ons: their own vehicles beside the stock ones (converted to
    // the 360 format and installed by gta4_addon_install.cpp at launch).
    VehicleChoice{"Batmobile", "BATMOBILE"},
    VehicleChoice{"F-16 Fighter Jet", "F16", 16.0f},
    VehicleChoice{"Grotti Deimos GTS", "DEIMOS"},
    VehicleChoice{"Vapid Dominator 645", "DOM645T"},
    VehicleChoice{"Dinka Nexus", "NEXUS"},
    VehicleChoice{"Imponte Arbiter X", "ARBITERX"},
    VehicleChoice{"Lazar 3 (armed)", "LAZAR3", 7.5f},
    VehicleChoice{"Bravado Gauntlet Classic", "GAUNTLETC"},
    VehicleChoice{"DeLorean DMC-12", "DELNOR"},
    VehicleChoice{"DeLorean Time Machine (Part I)", "DELONE"},
    VehicleChoice{"DeLorean Time Machine (Part II)", "DELTWO"},
    VehicleChoice{"DeLorean Time Machine (Part III)", "DELTHR"},
    VehicleChoice{"Batwing", "BATWING", 16.0f},
    VehicleChoice{"Batman Beyond Batmobile", "BEYOND", 7.0f},
    VehicleChoice{"Dodge Challenger SRT Hellcat", "HELLCAT"},
};
// The game's own vehicles come first in the list; the add-ons from here.
constexpr int kFirstAddon = [] {
  for (size_t i = 0; i < kVehicles.size(); ++i)
    if (kVehicles[i].model == "BATMOBILE")
      return int(i);
  return int(kVehicles.size());
}();
struct WeaponChoice {
  std::string_view label;
  uint32_t type, ammo;
};
constexpr std::array kLoadout = {
    WeaponChoice{"Knife", 3, 1},          WeaponChoice{"Baseball bat", 1, 1},
    WeaponChoice{"Pistol", 7, 9999},      WeaponChoice{"Desert Eagle", 9, 9999},
    WeaponChoice{"Shotgun", 10, 9999},    WeaponChoice{"Baretta", 11, 9999},
    WeaponChoice{"Micro Uzi", 12, 9999},  WeaponChoice{"MP5", 13, 9999},
    WeaponChoice{"AK-47", 14, 9999},      WeaponChoice{"M4", 15, 9999},
    WeaponChoice{"Sniper rifle", 16, 9999}, WeaponChoice{"M40A1", 17, 9999},
    WeaponChoice{"RPG", 18, 9999},        WeaponChoice{"Grenades", 4, 25},
    WeaponChoice{"Molotovs", 5, 25},
};
constexpr std::array<std::string_view, 8> kWeathers = {
    "Extra sunny", "Sunny", "Sunny and windy", "Cloudy",
    "Rain",        "Drizzle", "Foggy",         "Lightning"};
// The places' spots on the Teleport page (v1.7.40): each installed map
// set's arrival, then its own spots (map.txt "spot"); the labels live here
// for the menu's string views.
struct PlaceSpot {
  const gta4::addons::MapInfo* info = nullptr;
  const gta4::addons::MapSpot* spot = nullptr;  // null: the arrival
  std::string label;
};
const std::vector<PlaceSpot>& PlaceSpots() {
  static std::vector<PlaceSpot> spots;
  static std::once_flag once;
  std::call_once(once, [] {
    for (const auto& info : gta4::addons::InstalledMaps()) {
      if (!info.installed)
        continue;
      // ("Oct 26, 1984 (Gator Keys)": the name in the brackets)
      std::string place = info.label;
      const size_t open = place.rfind('('), close = place.rfind(')');
      if (open != std::string::npos && close != std::string::npos && close > open + 1)
        place = place.substr(open + 1, close - open - 1);
      spots.push_back({&info, nullptr, place + ": the arrival"});
      for (const auto& sp : info.spots)
        spots.push_back({&info, &sp, place + ": " + sp.label});
    }
  });
  return spots;
}

struct TeleportChoice {
  std::string_view label;
  float x, y;
  float z = 0.0f;  // the ground's height there, when known (0: found below)
};
// Approximate map spots; the teleport finds the ground itself. The airport:
// in front of the terminal, where Rockstar's own scripts set the player down
// (TBOGT's tony11, TLAD's stubbs1). v1.7.21 aimed at a spot by the fence and
// took a ground of 0 before the airport's had loaded: the player fell
// through the world onto the road outside.
constexpr std::array kTeleports = {
    TeleportChoice{"Francis Intl Airport", 2188.4f, 638.8f, 5.1f},
    TeleportChoice{"Happiness Island", -2241.0f, 366.0f},
    TeleportChoice{"Star Junction", -261.0f, 787.0f},
    TeleportChoice{"Middle Park", -241.0f, 1035.0f},
    TeleportChoice{"Broker Bridge (east)", 553.0f, 185.0f},
};

enum class Page {
  kMain, kPlayer, kVehicle, kSpawn, kSpawnStandard, kTeleport, kWorld, kWeapons, kChaos, kStatus, kTimeMachine
};
enum class Action {
  kNone, kOpenPlayer, kOpenVehicle, kOpenSpawn, kOpenTeleport, kOpenWorld,
  kOpenWeapons, kOpenChaos, kOpenStatus,
  kToggleRain, kToggleGravity,
  kToggleGod, kToggleNeverWanted, kWantedUp, kWantedDown, kWantedApply,
  kMoney, kHeal,
  kToggleVehicleGod, kRepair, kToggleWarpIn, kToggleGadgets, kToggleJet, kToggleJetExpert,
  kTogglePopupAuto, kToggleAnyRide, kToggleCarLights,
  kSpawnVehicle, kTeleportWaypoint, kTeleportPreset, kTeleportPlace, kSavePosition, kLoadPosition,
  kHourUp, kHourDown, kMinuteUp, kMinuteDown, kTimeApply,
  kWeatherNext, kWeatherPrevious, kWeatherApply, kWeatherRelease,
  kGiveLoadout, kRemoveWeapons,
  kOpenTimeMachine, kTmPreset, kTmField, kTmCircuits, kTmRefuel, kTmHover,
  kOpenSpawnStandard, kCompleteMission, kTogglePerformance, kJetRoute,
  kTmUnloadMap, kTmPieces, kJetLandMode,
  kBeyondSummon, kBeyondCloak,
};
struct Item {
  std::string_view label;
  Action action = Action::kNone;
  int index = -1;  // list index for spawn/teleport pages
};

// A date on the DeLorean's time circuits (24-hour clock).
struct TcdDate {
  int year = 1955, month = 11, day = 5, hour = 6, minute = 0;
};
// Destinations from the films (the first is worked out: today).
struct TcdPreset {
  std::string_view name;
  TcdDate date;
  std::string_view map;  // a place it takes the car to (an installed map set), or none
};
constexpr std::array kTcdPresets = {
    TcdPreset{"Today", {}},
    TcdPreset{"Nov 5, 1955 (Hill Valley)", {1955, 11, 5, 6, 0}},
    TcdPreset{"Nov 12, 1955 (the storm)", {1955, 11, 12, 22, 4}},
    TcdPreset{"Oct 26, 1985 (Twin Pines Mall)", {1985, 10, 26, 1, 21}},
    TcdPreset{"Oct 21, 2015 (the future)", {2015, 10, 21, 16, 29}},
    TcdPreset{"Sep 2, 1885 (the Old West)", {1885, 9, 2, 8, 0}},
    TcdPreset{"Jul 4, 1776 (Independence)", {1776, 7, 4, 12, 0}},
    TcdPreset{"Dec 25, 0000", {0, 12, 25, 0, 0}},
    // (a place as well as a time: the map sets' own islands, out to sea)
    TcdPreset{"Oct 26, 1984 (Gator Keys)", {1984, 10, 26, 10, 0}, "gatorkeys"},
    TcdPreset{"Dec 8, 1941 (Wake Island)", {1941, 12, 8, 7, 0}, "wake"},
    TcdPreset{"May 5, 1200 (Hyrule Castle)", {1200, 5, 5, 10, 0}, "hyrule"},
    TcdPreset{"Jun 20, 1962 (Route 66)", {1962, 6, 20, 17, 30}, "route66"},
};

// Shared trainer state: written on the game thread, drawn on the UI thread.
struct State {
  bool open = false;
  Page page = Page::kMain;
  int selected = 0;
  int spawn_top = 0;
  // Toggles.
  // Every option starts off (the menu changes nothing until asked to), but
  // the headlight switch on d-pad left and the F-16's flight, which does
  // nothing outside the jet. (Off, as it was until v1.7.16, every launch
  // left the jet a car that could not take off until the menu was visited.)
  bool god = false, never_wanted = false, vehicle_god = false, warp_in = false;
  bool gadgets = false;
  bool jet = true;  // F-16 flight and weapons, active only in the jet
  bool jet_expert = false;  // the jets' controls: Standard (Ace Combat's, assisted) or Expert (GTA V's)
  bool raining = false, gravity_off = false;
  bool any_ride = false;     // Bring Your Own Ride: missions take the player's vehicle
  bool jet_route = false;    // the jet's autopilot asked (menu) to fly to the airport and land
  int jet_land_mode = 0;     // holding d-pad down lands: 0 on open ground nearby, 1 straight ahead, 2 at the airport
  bool performance = false;  // 60 FPS Mode
  bool popup_auto = false;   // Deimos pop-ups: d-pad left (false) or up with the lights (true)
  bool car_lights = true;    // any car's headlights on d-pad left (the one option on by default)
  int wanted_target = 0;
  int hour = 12, minute = 0, weather = 1;
  // The DeLorean's destination (set from the menu: a preset, or each field),
  // and the menu's requests the time machine acts on in the car.
  TcdDate tm_dest;
  int tm_preset = 1;
  std::string tm_map;  // the destination's place (a map set's name), or none: the city
  bool map_movable = false;  // a place's pieces as movable objects (v1.7.41 and before) rather than buildings (v1.7.42)
  bool tm_toggle_circuits = false, tm_refuel = false, tm_toggle_hover = false;
  // The Beyond Batmobile's requests from the menu (v1.7.44): summoned to the
  // player, its cloak.
  bool beyond_summon = false, beyond_toggle_cloak = false;
  // Async vehicle spawn.
  int pending_model = -1;
  uint32_t pending_hash = 0;
  int pending_ticks = 0;
  uint64_t pending_since = 0;
  uint64_t pending_loaded_at = 0, pending_ready_since = 0;  // the guard's settle wait
  int pending_stable = 0;
  bool pending_logged = false;
  uint64_t pending_renew_at = 0;  // the request renewed while it waits
  int pending_renewals = 0;
  uint64_t pending_toast_step = 0;
  // Saved position.
  bool have_saved = false;
  float saved[3]{};
  // Status.
  std::string toast;
  uint64_t toast_until = 0;
  uint64_t tick = 0;
};
State g_state;
std::mutex g_draw_mutex;

struct DrawLine {
  std::string text;
  bool selected = false, dim = false;
};
// Shown on the menu's header band, every page.
constexpr const char* kMenuCredit = "by @TheUnwelcomed";
struct DrawModel {
  bool open = false;
  std::string title, footer, toast;
  std::vector<DrawLine> lines;
  // v1.7.24's glass panel switcher: the sections as tabs (L1/R1), which one
  // is shown, which way the last switch went (the slide), the list's place
  int tab = 0, page_serial = 0, slide = 0;
  std::vector<std::string> tabs;
  int first = 0, total = 0;  // the window's first row and the page's rows
};
DrawModel g_draw;
std::atomic<bool> g_input_owned{false};
std::atomic<bool> g_menu_visible{false};  // the menu or a toast on screen (animated: drawn every frame)

// Menu selections cross from the input poll to the script stage here. Both
// run on the game's main thread, so plain containers suffice; the atomics
// carry the pad sample for the gadgets and the online verdict back.
struct PendingItem {
  Action action = Action::kNone;
  int index = -1;
};
std::vector<PendingItem> g_pending;
std::atomic<uint16_t> g_gadget_buttons{0}, g_gadget_previous{0};
// When Y last went down (the jets' quick way in looks between its probes).
std::atomic<uint64_t> g_y_down_at{0};
// The full pad for the F-16's controls (sticks and triggers), sampled by
// the input poll while the menu is shut; zero while it is open.
struct DrivePad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0;
};
std::mutex g_drive_mutex;
DrivePad g_drive_pad, g_drive_previous;
// Buttons the in-vehicle controls currently use (the Batmobile's and the
// jet's d-pad), hidden from the game so they do not also change the radio.
std::atomic<uint32_t> g_vehicle_mask{0};
std::atomic<bool> g_online{false};
uint64_t g_last_gun_ms = 0;
// Cheap gate for the script-thread hook: only when the trainer actually has
// something to do does RunActions run (set by Poll, recomputed after a run).
std::atomic<bool> g_has_work{false};
// Frames, counted by the input poll (once per frame), so the script-thread
// hook - which fires once per script thread, dozens of times a frame - runs
// one pass per frame without reading a clock on every thread.
std::atomic<uint32_t> g_frame{0};
uint32_t g_last_run_frame = ~0u;
// What the trainer itself costs, measured and logged every 10 seconds next to
// the port's own timing lines: frames, passes, time spent in them and in the
// menu poll, and how often scripts asked the mission-vehicle checks.
struct TrainerCost {
  uint64_t window_start_ms = 0;
  uint32_t frames = 0, passes = 0;
  uint64_t pass_ns = 0, poll_ns = 0, worst_pass_ns = 0;
};
TrainerCost g_cost;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
// Which bundled add-ons this install has, looked up once (IS_MODEL_IN_CDIMAGE)
// so their watchers cost nothing on installs without them.
bool g_addons_probed = false, g_have_deimos = false, g_have_f16 = false, g_have_batmobile = false;
bool g_have_batwing = false;
bool g_have_nexus = false, g_have_arbiter = false, g_have_dominator = false;
bool g_have_lazar = false, g_have_gauntlet = false;
bool g_have_dmc12 = false, g_have_bttf1 = false, g_have_bttf2 = false, g_have_bttf3 = false, g_have_bttf2h = false;
bool g_have_beyond = false;  // the Batman Beyond Batmobile (v1.7.44)
bool g_have_hellcat = false;  // the Challenger SRT Hellcat (v1.7.44)
std::atomic<bool> g_beyond_cloaked{false};  // (for the menu's line)
bool HaveTimeMachines() { return g_have_bttf1 || g_have_bttf2 || g_have_bttf3; }
// Pad adjustment for the XAM path: low 16 bits masked from the game, high
// 16 bits pressed into it (the replayed tap).
std::atomic<uint32_t> g_pad_adjust{0};
// The script thread currently (or just) run by sub_82844200, and the
// executing-thread global the VM keeps while scripts call natives
// (gta4_quicksave_hooks.cpp). CREATE_CAR and friends read it to attach the
// new entity to the calling script, so acting with it null was the spawn
// crash: the shim below sets it the way the VM itself does.
constexpr uint32_t kExecutingThread = 0x8319277C;

void Toast(State& s, std::string text, uint64_t ms = 2500) {
  s.toast = std::move(text);
  s.toast_until = NowMs() + ms;
}

// Whether the script-stage pass has anything to do. Nothing switched on and
// no add-on watchers needed means the trainer costs nothing per frame.
bool WantsWork(const State& s) {
  return s.open || !g_pending.empty() || s.god || s.never_wanted || s.vehicle_god || s.gadgets ||
         s.raining || s.pending_model >= 0 || !g_addons_probed || g_have_f16 || g_have_batwing || g_have_deimos ||
         g_have_batmobile || g_have_nexus || g_have_arbiter || g_have_dominator || g_have_lazar ||
         g_have_gauntlet || g_have_dmc12 || HaveTimeMachines() || g_have_beyond || g_have_hellcat || s.car_lights;
}

// The sections the menu's tab strip shows, L1/R1 going through them.
constexpr std::array<std::pair<Page, const char*>, 9> kTabs = {{{Page::kMain, "HOME"},
                                                                {Page::kPlayer, "PLAYER"},
                                                                {Page::kVehicle, "VEHICLES"},
                                                                {Page::kSpawn, "SPAWN"},
                                                                {Page::kTeleport, "TELEPORT"},
                                                                {Page::kWorld, "WORLD"},
                                                                {Page::kWeapons, "WEAPONS"},
                                                                {Page::kChaos, "CHAOS"},
                                                                {Page::kTimeMachine, "DELOREAN"}}};
int TabOf(Page page) {
  if (page == Page::kSpawnStandard)
    return 3;
  for (size_t i = 0; i < kTabs.size(); ++i)
    if (kTabs[i].first == page)
      return int(i);
  return 0;  // (Menu Status: under Home)
}
std::atomic<int> g_page_serial{0}, g_page_slide{0};

std::vector<Item> PageItems(const State& s) {
  std::vector<Item> items;
  switch (s.page) {
    case Page::kMain:
      items = {{"Player Options", Action::kOpenPlayer},
               {"Vehicle Options", Action::kOpenVehicle},
               {"Car Spawning", Action::kOpenSpawn},
               {"Teleporting", Action::kOpenTeleport},
               {"Time and Weather", Action::kOpenWorld},
               {"Weapons", Action::kOpenWeapons},
               {"Chaos", Action::kOpenChaos},
               {"Time Machine (DeLorean)", Action::kOpenTimeMachine},
               {"Complete Current Mission", Action::kCompleteMission},
               {s.performance ? "60 FPS Mode: ON" : "60 FPS Mode: OFF", Action::kTogglePerformance},
               {"Menu Status", Action::kOpenStatus}};
      break;
    case Page::kPlayer:
      items = {{s.god ? "God Mode: ON" : "God Mode: OFF", Action::kToggleGod},
               {s.never_wanted ? "Never Wanted: ON" : "Never Wanted: OFF",
                Action::kToggleNeverWanted},
               {"Wanted Level (left/right, A to set)", Action::kWantedApply},
               {"Give $100,000", Action::kMoney},
               {"Heal and Armor", Action::kHeal}};
      break;
    case Page::kVehicle:
      items = {{s.vehicle_god ? "Vehicle God Mode: ON" : "Vehicle God Mode: OFF",
                Action::kToggleVehicleGod},
               {"Repair Current Vehicle (and back on its wheels)", Action::kRepair},
               {s.warp_in ? "Warp Into Spawned Car: ON" : "Warp Into Spawned Car: OFF",
                Action::kToggleWarpIn},
               {s.gadgets ? "Batmobile Gadgets: ON (L1 missile, d-pad guns/boost, horn spikes)"
                          : "Batmobile Gadgets: OFF",
                Action::kToggleGadgets},
               {s.jet ? "Jet Flight and Weapons (F-16, Batwing): ON" : "Jet Flight and Weapons (F-16, Batwing): OFF",
                Action::kToggleJet},
               {"Beyond Batmobile: Summon It (it flies to you)", Action::kBeyondSummon},
               {g_beyond_cloaked.load(std::memory_order_relaxed) ? "Beyond Batmobile: Cloak ON (d-pad down in it)"
                                                                 : "Beyond Batmobile: Cloak OFF (d-pad down in it)",
                Action::kBeyondCloak},
               {s.jet_expert ? "Jet Controls: Expert (roll and pull, like GTA V)"
                             : "Jet Controls: Standard (bank to turn, like Ace Combat)",
                Action::kToggleJetExpert},
               {s.popup_auto ? "Deimos/Nexus Pop-ups: Automatic (with the lights)"
                             : "Deimos/Nexus Pop-ups: D-pad Left",
                Action::kTogglePopupAuto},
               {s.car_lights ? "Headlights on D-pad Left: ON" : "Headlights on D-pad Left: OFF",
                Action::kToggleCarLights},
               {s.any_ride ? "Bring Your Own Ride: ON" : "Bring Your Own Ride: OFF",
                Action::kToggleAnyRide},
               {"Jet Autopilot: Fly to Francis Intl and Land", Action::kJetRoute},
               {s.jet_land_mode == 0   ? "Autopilot Landing (hold d-pad down): Open Ground Nearby"
                : s.jet_land_mode == 1 ? "Autopilot Landing (hold d-pad down): Straight Ahead"
                                       : "Autopilot Landing (hold d-pad down): Francis Intl",
                Action::kJetLandMode}};
      break;
    case Page::kSpawn:
      // The add-ons, and the game's own vehicles a page further.
      for (int i = kFirstAddon; i < int(kVehicles.size()); ++i)
        items.push_back({kVehicles[size_t(i)].label, Action::kSpawnVehicle, i});
      items.push_back({"Standard Vehicles  >", Action::kOpenSpawnStandard});
      break;
    case Page::kSpawnStandard:
      for (int i = 0; i < kFirstAddon; ++i)
        items.push_back({kVehicles[size_t(i)].label, Action::kSpawnVehicle, i});
      break;
    case Page::kTeleport:
      items.push_back({"Teleport To Waypoint", Action::kTeleportWaypoint});
      items.push_back({"Save Current Position", Action::kSavePosition});
      items.push_back({s.have_saved ? "Teleport To Saved Position"
                                    : "Teleport To Saved Position (none saved)",
                       Action::kLoadPosition});
      for (int i = 0; i < int(kTeleports.size()); ++i)
        items.push_back({kTeleports[size_t(i)].label, Action::kTeleportPreset, i});
      {
        const auto& spots = PlaceSpots();
        for (int i = 0; i < int(spots.size()); ++i)
          items.push_back({spots[size_t(i)].label, Action::kTeleportPlace, i});
      }
      break;
    case Page::kWorld:
      items = {{"Hour (left/right)", Action::kTimeApply},
               {"Minute (left/right)", Action::kTimeApply},
               {"Set Time Now", Action::kTimeApply},
               {"Weather (left/right)", Action::kWeatherApply},
               {"Force Weather Now", Action::kWeatherApply},
               {"Release Weather", Action::kWeatherRelease}};
      break;
    case Page::kTimeMachine:
      items = {{"Destination", Action::kTmPreset},  // (labels filled in by PublishDraw)
               {"Month", Action::kTmField},
               {"Day", Action::kTmField},
               {"Year", Action::kTmField},
               {"Hour", Action::kTmField},
               {"Minute", Action::kTmField},
               {"Time Circuits On/Off (d-pad down in the car)", Action::kTmCircuits},
               {"Refuel: Plutonium / Mr. Fusion (L1 in the car)", Action::kTmRefuel},
               {"Part II: Fly / Land (d-pad right in the car)", Action::kTmHover},
               {"Unload Map Destination", Action::kTmUnloadMap},  // (label filled in by PublishDraw)
               {"Place Pieces As", Action::kTmPieces}};           // (label filled in by PublishDraw)
      break;
    case Page::kWeapons:
      items = {{"Give Weapon Loadout", Action::kGiveLoadout},
               {"Remove All Weapons", Action::kRemoveWeapons}};
      break;
    case Page::kChaos:
      items = {{s.raining ? "Raining Cars: ON" : "Raining Cars: OFF", Action::kToggleRain},
               {s.gravity_off ? "Gravity: OFF" : "Gravity: Normal", Action::kToggleGravity}};
      break;
    case Page::kStatus:
      break;
  }
  return items;
}

// ---------------------------------------------------------------------------
// Actions (game thread; natives only).
// ---------------------------------------------------------------------------
void ApplyGod(PPCContext& ctx, uint8_t* base, const GameContext& game, bool on) {
  Invoke(ctx, base, "SET_CHAR_INVINCIBLE", {game.ped, on ? 1u : 0u});
  const uint32_t p = on ? 1u : 0u;
  Invoke(ctx, base, "SET_CHAR_PROOFS", {game.ped, p, p, p, p, p});
}

void Heal(PPCContext& ctx, uint8_t* base, const GameContext& game) {
  Invoke(ctx, base, "SET_CHAR_HEALTH", {game.ped, 200});
  Invoke(ctx, base, "ADD_ARMOUR_TO_CHAR", {game.ped, 100});
}

std::optional<uint32_t> CurrentVehicle(PPCContext& ctx, uint8_t* base, const GameContext& game) {
  const auto in_car = Invoke(ctx, base, "IS_CHAR_IN_ANY_CAR", {game.ped});
  if (!in_car || !in_car->r0())
    return std::nullopt;
  const auto car = Invoke(ctx, base, "GET_CAR_CHAR_IS_USING", {game.ped, 0}, 1);
  if (!car || !car->words[0])
    return std::nullopt;
  return car->words[0];
}

// The player's vehicle and its model, looked up at most once per pass and
// shared by the watchers (jet, gadgets, pop-ups).
// Whether the player was in a vehicle at the last look: with the menu open,
// the game still gets the throttle (R2) then, so the car keeps going.
std::atomic<bool> g_in_vehicle{false};

struct PassVehicle {
  uint64_t pass = ~0ull;
  std::optional<uint32_t> car;
  uint32_t model = 0;
};
PassVehicle g_pass_vehicle;
uint64_t g_pass = 0;

const PassVehicle& PlayerVehicle(PPCContext& ctx, uint8_t* base, const GameContext& game) {
  if (g_pass_vehicle.pass != g_pass) {
    g_pass_vehicle = PassVehicle{};
    g_pass_vehicle.pass = g_pass;
    g_pass_vehicle.car = CurrentVehicle(ctx, base, game);
    g_in_vehicle.store(g_pass_vehicle.car.has_value(), std::memory_order_relaxed);
    if (g_pass_vehicle.car)
      if (const auto m = Invoke(ctx, base, "GET_CAR_MODEL", {*g_pass_vehicle.car, 0}, 1))
        g_pass_vehicle.model = m->words[0];
  }
  return g_pass_vehicle;
}

// Teleports: the destination is loaded first (LOAD_SCENE, as Rockstar's
// scripts do after moving the player across the map), then the player (or
// the vehicle he is driving, with him in it) is put on the ground there.
// When the ground is still not known, the player waits there held still
// while its collision streams in (up to 8 seconds) and is put down once it
// is. v1.7.10 and before guessed 60 m up when the waypoint's ground was not
// loaded yet: the player fell from the sky, sometimes through the city.
bool MapGuarding();  // (a place being stood under the player, v1.7.40)
// The standing place's own ground at (x, y), from its height map (v1.7.43:
// the game's ground probe never sees a place's pieces - log 114, Wake Island:
// every cell in the physics world and the probe returning the water's 0.00
// under the arrival; defined with the life, further down).
std::optional<float> PlaceGroundUnder(float x, float y);
struct PendingTeleport {
  bool active = false;
  float x = 0, y = 0, from = 0;  // where, and the height the ground is looked for below
  float lowest = -1000.0f;       // ground lower than this is not loaded yet (a known height's)
  uint32_t car = 0;              // the vehicle moved along, or 0
  uint64_t until = 0;
  std::optional<float> heading;  // faced once put down (a place's spot, v1.7.40)
};
PendingTeleport g_teleport;

// (v1.7.43: the native answers through its pointer and returns whether it
// found ground - and the trainer's call put both in the same word, the flag
// written last: every "ground" it ever read was 0 or a denormal 1.4e-45,
// logged "0.0", and teleports put the player a metre over nothing (the
// places' arrivals under their terrain: Wake Island's sinking, logs 111 and
// 114; the city's docks at their luck). The answer has its own word now.)
std::optional<float> GroundBelow(PPCContext& ctx, uint8_t* base, float x, float y, float from) {
  const auto ground =
      Invoke(ctx, base, "GET_GROUND_Z_FOR_3D_COORD", {Bits(x), Bits(y), Bits(from), 0}, 3, 0, 1, -1, {}, 1);
  if (!ground || !ground->words[0])
    return std::nullopt;
  const float gz = ground->f(1);
  if (!std::isfinite(gz) || gz > from)
    return std::nullopt;
  return gz;
}

void PlaceOnGround(PPCContext& ctx, uint8_t* base, const GameContext& game, uint32_t car, float x, float y,
                   float gz, std::optional<float> heading = std::nullopt, bool on_place = false) {
  if (car) {
    Invoke(ctx, base, "SET_CAR_COORDINATES", {car, Bits(x), Bits(y), Bits(gz + 1.0f)});
    if (heading)
      Invoke(ctx, base, "SET_CAR_HEADING", {car, Bits(*heading)});
    if (on_place)  // (the game's probe sees none of a place's ground: the car is let fall the metre instead)
      Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {car, Bits(0.05f)});
    else
      Invoke(ctx, base, "SET_CAR_ON_GROUND_PROPERLY", {car});
  } else {
    // (A metre over the ground, as the trainer's teleports have always put
    // the player: a short drop at most.)
    Invoke(ctx, base, "SET_CHAR_COORDINATES", {game.ped, Bits(x), Bits(y), Bits(gz + 1.0f)});
    if (heading)
      Invoke(ctx, base, "SET_CHAR_HEADING", {game.ped, Bits(*heading)});
  }
}

void Teleport(PPCContext& ctx, uint8_t* base, const GameContext& game, float x, float y,
              std::optional<float> z_hint, State& s, std::optional<float> heading = std::nullopt) {
  uint32_t car = 0;
  if (const auto v = CurrentVehicle(ctx, base, game))
    car = *v;
  // A saved position keeps its level (inside, under a bridge); a waypoint or
  // landmark takes the highest ground there.
  const float from = z_hint ? *z_hint + 2.0f : 1000.0f;
  const float focus = z_hint.value_or(30.0f);
  Note(fmt::format("teleport to ({:.0f}, {:.0f}): LOAD_SCENE", x, y));
  if (NativeKnown("LOAD_SCENE"))
    Invoke(ctx, base, "LOAD_SCENE", {Bits(x), Bits(y), Bits(focus)});
  Note("LOAD_SCENE done");
  Invoke(ctx, base, "REQUEST_COLLISION_AT_POSN", {Bits(x), Bits(y), Bits(focus)});
  // (with a known height, ground more than 3 m under it is the sea or the
  // low-detail world: the real ground there has not loaded yet)
  const float lowest = z_hint ? *z_hint - 3.0f : -1000.0f;
  if (const auto gz = GroundBelow(ctx, base, x, y, from); gz && *gz >= lowest && !MapGuarding()) {
    PlaceOnGround(ctx, base, game, car, x, y, *gz, heading);
    REXLOG_INFO("gta4-trainer: teleported to {:.1f}, {:.1f}, ground {:.1f}{}", x, y, *gz, car ? " (with the vehicle)" : "");
    Toast(s, "Teleported.");
    return;
  }
  // A place standing there whose collision the probe did not find yet: its
  // own height map (v1.7.43).
  if (!MapGuarding())
    if (const auto pz = PlaceGroundUnder(x, y)) {
      PlaceOnGround(ctx, base, game, car, x, y, *pz, heading, true);
      REXLOG_INFO("gta4-trainer: teleported to {:.1f}, {:.1f}, the place's own ground {:.1f}{}", x, y, *pz,
                  car ? " (with the vehicle)" : "");
      Toast(s, "Teleported.");
      return;
    }
  // Not loaded yet: wait there, held still, until it is.
  const float hold = z_hint.value_or(60.0f);
  if (car) {
    Invoke(ctx, base, "SET_CAR_COORDINATES", {car, Bits(x), Bits(y), Bits(hold)});
    Invoke(ctx, base, "FREEZE_CAR_POSITION", {car, 1});
  } else {
    Invoke(ctx, base, "SET_CHAR_COORDINATES", {game.ped, Bits(x), Bits(y), Bits(hold)});
    Invoke(ctx, base, "FREEZE_CHAR_POSITION", {game.ped, 1});
  }
  g_teleport = PendingTeleport{true, x, y, from, lowest, car, NowMs() + 10000, heading};
  REXLOG_INFO("gta4-trainer: teleport to {:.1f}, {:.1f}: waiting for the ground there to load", x, y);
  if (!MapGuarding())
    Toast(s, "Teleporting: loading the area...", 8000);
}

// Every frame while a teleport waits for its ground.
void TeleportTick(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s) {
  auto& t = g_teleport;
  if (!t.active)
    return;
  if (MapGuarding()) {  // (a place being stood under the spot: the ground is its, and comes with it)
    t.until = NowMs() + 10000;
    return;
  }
  Invoke(ctx, base, "REQUEST_COLLISION_AT_POSN", {Bits(t.x), Bits(t.y), Bits(std::min(t.from, 60.0f))});
  auto gz = GroundBelow(ctx, base, t.x, t.y, t.from);
  if (gz && *gz < t.lowest)
    gz.reset();
  // A place standing under the spot whose collision the probe did not find:
  // its own height map, now (v1.7.43).
  bool own_ground = false;
  if (!gz)
    if (const auto pz = PlaceGroundUnder(t.x, t.y)) {
      gz = pz;
      own_ground = true;
    }
  const bool timed_out = NowMs() > t.until;
  if (!gz && !timed_out)
    return;
  // (out of time: looked for once more from well above, in case the probe
  // started inside something over the spot, a canopy or a bridge)
  if (!gz && t.from < 900.0f)
    if (const auto high = GroundBelow(ctx, base, t.x, t.y, t.from + 60.0f); high && *high >= t.lowest)
      gz = high;
  if (t.car)
    Invoke(ctx, base, "FREEZE_CAR_POSITION", {t.car, 0});
  else
    Invoke(ctx, base, "FREEZE_CHAR_POSITION", {game.ped, 0});
  t.active = false;
  if (gz) {
    PlaceOnGround(ctx, base, game, t.car, t.x, t.y, *gz, t.heading, own_ground);
    REXLOG_INFO("gta4-trainer: teleport landed, ground {:.1f}{}", *gz, own_ground ? " (the place's own height map)" : "");
    Toast(s, "Teleported.");
  } else {
    REXLOG_WARN("gta4-trainer: teleport: the ground at {:.1f}, {:.1f} never loaded", t.x, t.y);
    Toast(s, "The ground there didn't load. Let go where you are.", 3000);
  }
}

void TeleportToWaypoint(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s) {
  // Blip type 8 is the player's waypoint marker.
  const auto blip = Invoke(ctx, base, "GET_FIRST_BLIP_INFO_ID", {8});
  const uint32_t id = blip ? blip->r0() : 0;
  std::optional<InvokeResult> exists;
  if (id)
    exists = Invoke(ctx, base, "DOES_BLIP_EXIST", {id});
  if (!exists || !exists->r0()) {
    Toast(s, "Set a waypoint on the map first.");
    return;
  }
  const auto coords = Invoke(ctx, base, "GET_BLIP_COORDS", {id, 0}, 1);
  if (!coords) {
    Toast(s, "The waypoint could not be read.");
    return;
  }
  Teleport(ctx, base, game, coords->f(0), coords->f(1), std::nullopt, s);
}

void StartSpawn(State& s, int model_index, PPCContext& ctx, uint8_t* base) {
  const auto& choice = kVehicles[size_t(model_index)];
  const uint32_t hash = ModelHash(choice.model);
  const auto known = Invoke(ctx, base, "IS_MODEL_IN_CDIMAGE", {hash});
  if (!known || !known->r0()) {
    REXLOG_INFO("gta4-trainer: spawn refused, model {} hash {:08X} unknown to the game",
                std::string(choice.model), hash);
    Toast(s, std::string(choice.label) + ": not installed or not in this episode.");
    return;
  }
  // A spawn still waiting gives way to the new one (and lets its model go).
  if (s.pending_model >= 0 && s.pending_hash != hash)
    Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {s.pending_hash});
  REXLOG_INFO("gta4-trainer: spawning {} (hash {:08X})", std::string(choice.model), hash);
  rex::FlushLogging();
  Invoke(ctx, base, "REQUEST_MODEL", {hash});
  s.pending_model = model_index;
  s.pending_hash = hash;
  s.pending_ticks = 0;
  s.pending_since = NowMs();
  s.pending_loaded_at = s.pending_ready_since = 0;
  s.pending_stable = 0;
  s.pending_logged = false;
  s.pending_renew_at = s.pending_since + 4000;
  s.pending_renewals = 0;
  s.pending_toast_step = 0;
  Toast(s, std::string("Loading ") + std::string(choice.label) + "...", 6000);
}

// ---------------------------------------------------------------------------
// Spawn guard. CREATE_CAR trusts a model completely: it reads the model's
// vehicle structure (model info +196, an entry of the 50-entry VehicleStruct
// pool) and, for each of the model's extras, the parent of that extra's bone,
// with no null checks. Console logs 030/032 crashed on a missing structure
// (reading 0x164) and log 036 on an extra whose bone had no parent (reading
// 0x14), so the trainer checks all of it first, waits while the game is still
// setting the model up, and refuses (with a full report in the log) instead.
// ---------------------------------------------------------------------------
constexpr uint32_t kModelInfoTable = 0x82B3A1A0;
constexpr uint32_t kModelInfoCount = 31000;  // the table's size (the model info init)
constexpr uint32_t kModelInfoFragType = 8, kModelInfoVehicleStructure = 196;
constexpr uint32_t kVehicleStructPool = 0x8303206C;
// The CVehicleModelInfo store: +0 capacity, +4 count, +8 the array of 976-byte
// entries. sub_821C83C0 builds it at boot; the vehicles.ide loader adds each
// car with sub_821CA5F0, which has no capacity check.
constexpr uint32_t kVehicleModelStore = 0x82A9305C, kVehicleModelInfoSize = 976;
constexpr uint32_t kExtraVehicleModels = 64;
// vehicles.ide's cars-line formats (sub_8256DC70), reported for reference.
constexpr uint32_t kCarsLineFormat = 0x820263F8, kCarsLineFormatAlt = 0x820263CC;
// The vehicle structure holds one bone index per vehicle component;
// components 89-98 are extra_1 to extra_10.
constexpr uint32_t kFirstExtraComponent = 89, kExtraComponents = 10;
constexpr uint32_t kFragDrawable = 0xB4, kDrawableSkeleton = 12, kSkeletonBoneCount = 20;
constexpr uint32_t kBoneSize = 0xE0, kBoneParent = 0x10;
int g_hash_field = -2;  // -2: not looked for yet, -1: not found

uint32_t Read16(uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, address), sizeof(v));
  return __builtin_bswap16(v);
}

// Readability for a scan, remembering the 4 KB pages already checked: each
// check takes the heap's lock, and a table scan would otherwise take it tens
// of thousands of times.
struct ReadableCache {
  std::array<uint32_t, 16> pages{};  // page number + 1 (0 = empty)
  size_t next = 0;
  bool Check(uint8_t* base, uint32_t address, uint32_t size) {
    const uint32_t first = address >> 12;
    const uint32_t last = uint32_t((uint64_t(address) + size - 1) >> 12);
    if (first == last)
      for (const uint32_t p : pages)
        if (p == first + 1)
          return true;
    if (!Span(base, address, size))
      return false;
    if (first == last) {
      pages[next] = first + 1;
      next = (next + 1) % pages.size();
    }
    return true;
  }
};

template <typename Visit>
void ForEachModelInfo(uint8_t* base, Visit&& visit) {
  constexpr uint32_t kChunk = 1000;
  for (uint32_t first = 0; first < kModelInfoCount; first += kChunk) {
    const uint32_t n = std::min(kChunk, kModelInfoCount - first);
    if (!Span(base, kModelInfoTable + first * 4, n * 4))
      continue;
    for (uint32_t i = first; i < first + n; ++i) {
      const uint32_t info = Read(base, kModelInfoTable + i * 4);
      if (info && visit(i, info))
        return;
    }
  }
}

// The name-hash field: +60 on this build, confirmed from three stock models;
// otherwise searched for in the first 64 words.
void DiscoverHashField(uint8_t* base) {
  g_hash_field = -1;
  const uint32_t probes[3] = {ModelHash("POLICE"), ModelHash("TAXI"), ModelHash("ADMIRAL")};
  ReadableCache readable;
  for (const int known : {60, -1}) {
    std::array<uint8_t, 64> hits{};
    ForEachModelInfo(base, [&](uint32_t, uint32_t info) {
      const uint32_t from = known >= 0 ? uint32_t(known) / 4 : 0, to = known >= 0 ? from + 1 : 64;
      if (!readable.Check(base, info + from * 4, (to - from) * 4))
        return false;
      for (uint32_t k = from; k < to; ++k) {
        const uint32_t v = Read(base, info + k * 4);
        for (int p = 0; p < 3; ++p)
          if (v == probes[p])
            hits[k] |= uint8_t(1u << p);
      }
      return known >= 0 && hits[from] == 7;
    });
    for (int k = 0; k < 64; ++k)
      if (hits[size_t(k)] == 7) {
        g_hash_field = k * 4;
        break;
      }
    if (g_hash_field >= 0)
      break;
  }
  REXLOG_INFO("gta4-trainer: model info name-hash field at +{}", g_hash_field);
}

// Name hash -> model info, built in one pass and kept; a stale or missing
// entry rebuilds it (at most every two seconds).
std::unordered_map<uint32_t, uint32_t> g_model_index;
uint64_t g_model_index_built = 0;

void BuildModelIndex(uint8_t* base) {
  g_model_index.clear();
  g_model_index_built = NowMs();
  ReadableCache readable;
  const uint32_t field = uint32_t(g_hash_field);
  ForEachModelInfo(base, [&](uint32_t, uint32_t info) {
    if (readable.Check(base, info + field, 4))
      g_model_index.emplace(Read(base, info + field), info);  // the first one wins
    return false;
  });
}

uint32_t FindModelInfo(uint8_t* base, uint32_t hash) {
  if (g_hash_field == -2)
    DiscoverHashField(base);
  if (g_hash_field < 0)
    return 0;
  const uint32_t field = uint32_t(g_hash_field);
  auto lookup = [&]() -> uint32_t {
    const auto it = g_model_index.find(hash);
    if (it == g_model_index.end())
      return 0;
    return Span(base, it->second + field, 4) && Read(base, it->second + field) == hash ? it->second
                                                                                       : 0;
  };
  if (g_model_index.empty())
    BuildModelIndex(base);
  if (const uint32_t info = lookup())
    return info;
  if (NowMs() - g_model_index_built < 2000)
    return 0;
  BuildModelIndex(base);
  return lookup();
}

// Everything CREATE_CAR will read, checked.
struct ModelCheck {
  uint32_t info = 0, vtable = 0, fragment = 0, structure = 0;
  std::string problem;  // empty when the model is ready
};
uint32_t g_vehicle_vtable = 0;  // a stock car's model info vtable (ADMIRAL's)

ModelCheck CheckModel(uint8_t* base, uint32_t hash) {
  ModelCheck c;
  c.info = FindModelInfo(base, hash);
  if (!c.info) {
    c.problem = "no model info";
    return c;
  }
  if (!g_vehicle_vtable)
    if (const uint32_t admiral = FindModelInfo(base, ModelHash("ADMIRAL")))
      if (Span(base, admiral, 4))
        g_vehicle_vtable = Read(base, admiral);
  if (!Span(base, c.info, kModelInfoVehicleStructure + 4)) {
    c.problem = "model info unreadable";
    return c;
  }
  c.vtable = Read(base, c.info);
  c.fragment = Read(base, c.info + kModelInfoFragType);
  c.structure = Read(base, c.info + kModelInfoVehicleStructure);
  if (g_vehicle_vtable && c.vtable != g_vehicle_vtable) {
    c.problem = "not a vehicle model info";
    return c;
  }
  if (!c.fragment) {
    c.problem = "fragment not attached";
    return c;
  }
  if (!c.structure) {
    c.problem = "no vehicle structure";
    return c;
  }
  if (Span(base, kVehicleStructPool, 4)) {
    const uint32_t pool = Read(base, kVehicleStructPool);
    if (pool && Span(base, pool, 16)) {
      const uint32_t storage = Read(base, pool), size = Read(base, pool + 8),
                     stride = Read(base, pool + 12);
      if (stride && (c.structure < storage || c.structure >= storage + size * stride ||
                     (c.structure - storage) % stride)) {
        c.problem = "vehicle structure outside its pool";
        return c;
      }
    }
  }
  if (!Span(base, c.structure + kFirstExtraComponent * 4, kExtraComponents * 4) ||
      !Span(base, c.fragment + kFragDrawable, 4)) {
    c.problem = "vehicle data unreadable";
    return c;
  }
  const uint32_t drawable = Read(base, c.fragment + kFragDrawable);
  const uint32_t skeleton =
      drawable && Span(base, drawable + kDrawableSkeleton, 4) ? Read(base, drawable + kDrawableSkeleton) : 0;
  if (!skeleton || !Span(base, skeleton, kSkeletonBoneCount + 2)) {
    // Nothing to check the extras against: let it through (the stock cars
    // must never be held back by this check), noted once.
    static uint32_t s_noted = 0;
    if (s_noted != hash) {
      s_noted = hash;
      REXLOG_INFO("gta4-trainer: model {:08X}: skeleton not found (drawable {:08X}), extras "
                  "not checked", hash, drawable);
    }
    return c;
  }
  const uint32_t bones = Read(base, skeleton), bone_count = Read16(base, skeleton + kSkeletonBoneCount);
  for (uint32_t k = 0; k < kExtraComponents; ++k) {
    const uint32_t bone = Read(base, c.structure + (kFirstExtraComponent + k) * 4);
    if (bone == 0xFFFFFFFFu)
      continue;
    const uint32_t parent_at = bones + bone * kBoneSize + kBoneParent;
    if (bone >= bone_count || !Span(base, parent_at, 4) || !Read(base, parent_at)) {
      c.problem = fmt::format("extra_{} is bone {} with no parent ({} bones)", k + 1, bone,
                              bone_count);
      return c;
    }
  }
  return c;
}

// The full report for a model the guard turned away: where the game keeps it,
// how it compares with a stock car, and the store it should live in.
void ReportModel(uint8_t* base, std::string_view name, uint32_t hash, const ModelCheck& c) {
  if (Span(base, kVehicleModelStore, 12)) {
    const uint32_t capacity = Read(base, kVehicleModelStore);
    const uint32_t count = Read(base, kVehicleModelStore + 4);
    const uint32_t array = Read(base, kVehicleModelStore + 8);
    std::string where = "outside it";
    if (array && c.info >= array && c.info < array + capacity * kVehicleModelInfoSize)
      where = fmt::format("entry {}{}", (c.info - array) / kVehicleModelInfoSize,
                          (c.info - array) % kVehicleModelInfoSize ? " (misaligned)" : "");
    REXLOG_WARN("gta4-trainer: report: vehicle model store capacity {} count {} array {:08X}; "
                "{} model info {:08X} is {}",
                capacity, count, array, std::string(name), c.info, where);
  }
  int index = -1, admiral_index = -1, same_name = 0;
  uint32_t admiral = 0;
  const uint32_t admiral_hash = ModelHash("ADMIRAL");
  if (g_hash_field >= 0) {
    const uint32_t field = uint32_t(g_hash_field);
    ReadableCache readable;
    ForEachModelInfo(base, [&](uint32_t i, uint32_t info) {
      if (info == c.info)
        index = int(i);
      if (readable.Check(base, info + field, 4)) {
        const uint32_t v = Read(base, info + field);
        same_name += v == hash;
        if (v == admiral_hash && !admiral) {
          admiral = info;
          admiral_index = int(i);
        }
      }
      return false;
    });
  }
  REXLOG_WARN("gta4-trainer: report: {} is model {} ({} model infos carry its name), vtable "
              "{:08X}; ADMIRAL {:08X} is model {}, vtable {:08X}",
              std::string(name), index, same_name, c.vtable, admiral, admiral_index,
              g_vehicle_vtable);
  if (Span(base, kVehicleStructPool, 4)) {
    const uint32_t pool = Read(base, kVehicleStructPool);
    if (pool && Span(base, pool, 24))
      REXLOG_WARN("gta4-trainer: report: VehicleStruct pool {:08X}: {:08X} {:08X} {} {} {} {}", pool,
                  Read(base, pool), Read(base, pool + 4), Read(base, pool + 8), Read(base, pool + 12),
                  Read(base, pool + 16), Read(base, pool + 20));
  }
  auto words = [&](std::string label, uint32_t at, uint32_t bytes) {
    if (!at || !Span(base, at, bytes))
      return;
    for (uint32_t row = 0; row < bytes; row += 64) {
      std::string line;
      for (uint32_t k = row; k < row + 64 && k < bytes; k += 4)
        line += fmt::format(" {:08X}", Read(base, at + k));
      REXLOG_WARN("gta4-trainer: report: {} +{:03X}:{}", label, row, line);
    }
  };
  words(std::string(name), c.info, kVehicleModelInfoSize);
  words("ADMIRAL", admiral, kVehicleModelInfoSize);
  words(std::string(name) + " vtable", c.vtable, 256);
  words("ADMIRAL vtable", g_vehicle_vtable, 256);
  REXLOG_WARN("gta4-trainer: report: vehicles.ide cars-line formats \"{}\" / \"{}\"",
              String(base, kCarsLineFormat, 200), String(base, kCarsLineFormatAlt, 200));
  rex::FlushLogging();
}

// Bones the trainer poses, a slot per car: the Deimos's pop-up pods, the
// F-16's landing gear (three bones), the Batmobile's canopy, the Nexus's pods
// (entry 0) and gullwing doors (entries 1 and 2). The pose last set for each
// is re-applied by the per-frame hook below (sub_82381FA0), right after the
// game's own per-frame bone edits for that car.
// The Lazar 3's: its turret (entry 0), its gun (1), its ramps (2, 3). The
// DeLoreans': their gullwings (entries 1 and 2, as the Nexus's).
// The Batwing's: its gear (entries 0-2, as the F-16's) and canopy (3).
enum PoseSlot : int {
  kPoseDeimos = 0, kPoseF16 = 1, kPoseBatmobile = 2, kPoseNexus = 3, kPoseLazar = 4, kPoseDelorean = 5,
  kPoseBatwing = 6, kPoseBeyond = 7, kPoseSlots = 8
};

// Pop-up headlights: the Grotti Deimos's and the Dinka Nexus's. The converter
// hangs both lamp pods (housings, lamps and lenses) on the template's
// extra_1, placed on their hinge at the back of the pods, flush with the
// bonnet at rest. The trainer swings that bone the way the game itself poses
// a bone: the bone's local matrix in the car's live skeleton (what
// sub_822832E8 hands the game when it hides an extra), a rotation about the
// car's width, front edge rising. Getting in they rise and light, getting out
// they fold and go out; d-pad left in the car raises or lowers them, the
// headlights following; or they rise whenever IV turns car lights on (night,
// rain, fog).
struct PopupSpec {
  std::string_view model, label;
  int slot;          // its pose slot (the pods: entry 0)
  uint32_t extra;    // the extra whose bone holds the pods
  float up_degrees;  // how far they swing up
};
// The Deimos: ZolikaPatch's angle for the mod on PC. The Nexus: its lamps sit
// in the pods tipped 30 degrees down, so 30 up faces them forward with the
// pods' tops level (the mod sets no pop-ups: on PC they stay down).
constexpr PopupSpec kDeimosPopups{"DEIMOS", "Deimos", kPoseDeimos, 1, 30.0f};
constexpr PopupSpec kNexusPopups{"NEXUS", "Nexus", kPoseNexus, 1, 30.0f};
struct PopupState {
  const PopupSpec* spec;
  uint32_t car = 0;
  bool in_car = false;
  bool swinging = false;  // the pods are on the move (the motor has sounded)
  bool up = false;          // where they are heading
  float angle = 0.0f;       // where they are, degrees
  uint16_t prev_buttons = 0;
  uint64_t next_probe = 0, next_check = 0, last_ms = 0, next_write = 0;
};
PopupState g_popup{&kDeimosPopups}, g_nexus_popup{&kNexusPopups};
void PopupMotorSound(const PopupSpec& spec, float seconds);  // (with the cars' engines, below)
constexpr float kPopupDegreesPerSecond = 85.0f;  // up in about a third of a second
constexpr uint32_t kEntitySkeleton = 0x8217DB60;  // entity -> its live crSkeleton

struct BonePose {
  int32_t bone = -1;
  std::array<uint32_t, 16> words{};
};
struct PosedVehicle {
  uint32_t vehicle = 0, info = 0;
  std::array<BonePose, 8> bones{};
};
std::mutex g_pose_mutex;
std::array<PosedVehicle, kPoseSlots> g_posed;
std::array<std::atomic<uint32_t>, kPoseSlots> g_posed_vehicle{};  // the hook's quick check

// Whether a CVehicle is (still) an instance of the model whose info is given.
bool VehicleIsModel(uint8_t* base, uint32_t vehicle, uint32_t info) {
  if (!vehicle || !info || !Span(base, vehicle + 0x2E, 2))
    return false;
  const int32_t index = int16_t(Read16(base, vehicle + 0x2E));
  return index >= 0 && index < int32_t(kModelInfoCount) &&
         Span(base, kModelInfoTable + 4 * uint32_t(index), 4) &&
         Read(base, kModelInfoTable + 4 * uint32_t(index)) == info;
}

// The bone's local matrix slot in the vehicle's live skeleton, or 0.
uint32_t PopupLocalSlot(PPCContext& ctx, uint8_t* base, uint32_t vehicle, int32_t bone) {
  const auto skeleton = CallGuest(ctx, base, kEntitySkeleton, vehicle);
  if (bone < 0 || !skeleton || !*skeleton || !Span(base, *skeleton, 0x18))
    return 0;
  const uint32_t locals = Read(base, *skeleton + 0x0C), count = Read(base, *skeleton + 0x10);
  if (uint32_t(bone) >= count || !Span(base, locals + 80 * uint32_t(bone), 64, true))
    return 0;
  return locals + 80 * uint32_t(bone);
}

// Turns a car's extra_<n> bone about the car's X (width) or Y (length) axis
// through the bone's rest position (its hinge), scaled by `scale` (0 hides
// the parts on it, collapsed onto the hinge), and remembers the pose for the
// hook. False when the car, its skeleton or the bone cannot be read safely;
// nothing is written then.
bool PoseBone(PPCContext& ctx, uint8_t* base, uint32_t vehicle, uint32_t info, int slot, int entry,
              int32_t bone, int axis, float degrees, float scale);

bool PoseExtraBone(PPCContext& ctx, uint8_t* base, uint32_t handle, uint32_t model_hash, int slot,
                   int entry, uint32_t extra, int axis, float degrees, float scale) {
  const uint32_t vehicle = VehicleAddress(base, handle);
  const uint32_t info = vehicle ? FindModelInfo(base, model_hash) : 0;
  if (!info || !VehicleIsModel(base, vehicle, info) || !Span(base, info + 196, 4))
    return false;
  const uint32_t structure = Read(base, info + 196);
  const uint32_t component = 88 + extra;  // extra_1..10 are the structure's components 89..98
  if (!Span(base, structure + 4 * component, 4))
    return false;
  const int32_t bone = int32_t(Read(base, structure + 4 * component));
  return PoseBone(ctx, base, vehicle, info, slot, entry, bone, axis, degrees, scale);
}

// A bone of a car's skeleton by name (the skeleton data's bone records:
// 0xE0 bytes each, the name first), or -1. For pivots the converter hangs on
// a bone that is not an extra (the Batmobile's canopy on interiorlight).
int32_t FindBone(PPCContext& ctx, uint8_t* base, uint32_t vehicle, std::string_view name) {
  const auto skeleton = CallGuest(ctx, base, kEntitySkeleton, vehicle);
  if (!skeleton || !*skeleton || !Span(base, *skeleton, 8))
    return -1;
  const uint32_t data = Read(base, *skeleton + 4);
  if (!Span(base, data, 24))
    return -1;
  const uint32_t bones = Read(base, data), count = Read16(base, data + 20);
  if (count > 512 || !Span(base, bones, 0xE0 * count))
    return -1;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = Read(base, bones + 0xE0 * i);
    if (at && String(base, at, 32) == name)
      return int32_t(i);
  }
  return -1;
}

// The bone's rest local matrix in the skeleton data (+16: rotation rows,
// then the translation row: its hinge, relative to its parent), or 0.
uint32_t RestMatrix(PPCContext& ctx, uint8_t* base, uint32_t vehicle, int32_t bone) {
  const auto skeleton = bone >= 0 ? CallGuest(ctx, base, kEntitySkeleton, vehicle) : std::nullopt;
  if (!skeleton || !*skeleton || !Span(base, *skeleton, 8))
    return 0;
  const uint32_t data = Read(base, *skeleton + 4);
  if (!Span(base, data, 24))
    return 0;
  const uint32_t rest = Read(base, data + 16) + 64 * uint32_t(bone);
  return Span(base, rest, 64) ? rest : 0;
}

// A rotation by `degrees` about the car's X, Y or Z axis, for row vectors
// (v' = v M): about X, y turns towards z (a front edge rising, a wheel
// swinging aft and up for a negative angle); about Y, z turns towards x;
// about Z, y turns towards -x (to the left, as headings turn).
void TurnMatrix(int axis, float degrees, float (&m)[3][3]) {
  const float rad = degrees * 3.14159265f / 180.0f, c = std::cos(rad), s = std::sin(rad);
  const float x[3][3] = {{1, 0, 0}, {0, c, s}, {0, -s, c}};
  const float y[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
  const float z[3][3] = {{c, s, 0}, {-s, c, 0}, {0, 0, 1}};
  const auto& t = axis == 2 ? z : axis == 1 ? y : x;
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k)
      m[r][k] = t[r][k];
}

// The general form: the bone's rest rotation turned by `turn` (v' = v turn
// rest), at its rest translation (its hinge) or, given `hinge`, there
// instead (a part carried round by another's turn: the Lazar's gun on its
// turret), scaled by `scale`.
bool PoseBoneMatrix(PPCContext& ctx, uint8_t* base, uint32_t vehicle, uint32_t info, int slot, int entry,
                    int32_t bone, const float (&turn)[3][3], const float* hinge, float scale);

bool PoseBone(PPCContext& ctx, uint8_t* base, uint32_t vehicle, uint32_t info, int slot, int entry,
              int32_t bone, int axis, float degrees, float scale) {
  float turn[3][3];
  TurnMatrix(axis, degrees, turn);
  return PoseBoneMatrix(ctx, base, vehicle, info, slot, entry, bone, turn, nullptr, scale);
}

bool PoseBoneMatrix(PPCContext& ctx, uint8_t* base, uint32_t vehicle, uint32_t info, int slot, int entry,
                    int32_t bone, const float (&turn)[3][3], const float* hinge, float scale) {
  const uint32_t local = PopupLocalSlot(ctx, base, vehicle, bone);
  const uint32_t rest = local ? RestMatrix(ctx, base, vehicle, bone) : 0;
  if (!local || !rest)
    return false;
  std::array<uint32_t, 16> words{};
  for (uint32_t row = 0; row < 3; ++row) {
    for (uint32_t col = 0; col < 3; ++col) {
      float v = 0;
      for (uint32_t k = 0; k < 3; ++k)
        v += turn[row][k] * ReadFloat(base, rest + 16 * k + 4 * col);
      words[4 * row + col] = Bits(v * scale);
    }
    words[4 * row + 3] = Read(base, rest + 16 * row + 12);
  }
  for (uint32_t w = 0; w < 4; ++w)  // the hinge stays put (unless carried)
    words[12 + w] = hinge && w < 3 ? Bits(hinge[w]) : Read(base, rest + 48 + 4 * w);
  for (uint32_t w = 0; w < 16; ++w)
    Write(base, local + 4 * w, words[w]);
  {
    std::lock_guard lock(g_pose_mutex);
    auto& posed = g_posed[size_t(slot)];
    if (posed.vehicle != vehicle) {
      posed = PosedVehicle{};
      posed.vehicle = vehicle;
    }
    posed.info = info;
    posed.bones[size_t(entry)] = BonePose{bone, words};
  }
  g_posed_vehicle[size_t(slot)].store(vehicle, std::memory_order_relaxed);
  return true;
}

// From the hook: the trainer's poses for this car again, after the game's
// own per-frame bone edits (whatever reset its skeleton earlier in the frame).
void ReapplyPoses(PPCContext& ctx, uint8_t* base, uint32_t vehicle) {
  if (!vehicle)
    return;
  for (size_t slot = 0; slot < g_posed.size(); ++slot) {
    if (g_posed_vehicle[slot].load(std::memory_order_relaxed) != vehicle)
      continue;
    PosedVehicle posed;
    {
      std::lock_guard lock(g_pose_mutex);
      posed = g_posed[slot];
    }
    if (posed.vehicle != vehicle || !VehicleIsModel(base, vehicle, posed.info)) {
      g_posed_vehicle[slot].store(0, std::memory_order_relaxed);  // gone, or not ours any more
      continue;
    }
    for (const auto& pose : posed.bones) {
      if (pose.bone < 0)
        continue;
      if (const uint32_t local = PopupLocalSlot(ctx, base, vehicle, pose.bone))
        for (uint32_t w = 0; w < 16; ++w)
          Write(base, local + 4 * w, pose.words[w]);
    }
  }
}

bool PosePopupBone(PPCContext& ctx, uint8_t* base, const PopupSpec& spec, uint32_t handle, float degrees) {
  return PoseExtraBone(ctx, base, handle, ModelHash(spec.model), spec.slot, 0, spec.extra, 0, degrees, 1.0f);
}

void SetPopupLights(PPCContext& ctx, uint8_t* base, uint32_t car, bool up, bool automatic) {
  // FORCE_CAR_LIGHTS: 0 the game's choice, 1 off, 2 on.
  Invoke(ctx, base, "FORCE_CAR_LIGHTS", {car, automatic ? 0u : (up ? 2u : 1u)});
}

bool HeadlightsWanted(PPCContext& ctx, uint8_t* base) {
  if (const auto t = Invoke(ctx, base, "GET_TIME_OF_DAY", {0, 0}, 0, 0, 2)) {
    const uint32_t hour = t->words[0];
    if (hour < 24 && (hour >= 20 || hour < 6))
      return true;
  }
  if (const auto w = Invoke(ctx, base, "GET_CURRENT_WEATHER", {0}, 0)) {
    const uint32_t weather = w->words[0];
    if (weather >= 4 && weather <= 7)  // rain, drizzle, fog, lightning
      return true;
  }
  return false;
}

// Shows a car's extra when the game has it off (it picks extras at random,
// and a hidden one's parts are sent to the world's origin). Only then:
// turning an extra on while the player sat in the Deimos snapped its open
// door shut as he reached for it (v1.7.9), so on getting in nothing is
// turned on unless the game says it is off.
void ShowExtra(PPCContext& ctx, uint8_t* base, uint32_t car, uint32_t extra, bool getting_in) {
  const auto on = Invoke(ctx, base, "IS_VEHICLE_EXTRA_TURNED_ON", {car, extra});
  if (on ? on->r0() != 0 : getting_in)
    return;
  Invoke(ctx, base, "TURN_OFF_VEHICLE_EXTRA", {car, extra, 0u});
}

// A Deimos or Nexus just made (or first met): its pods' extra shown, pods down.
void PreparePopups(PPCContext& ctx, uint8_t* base, PopupState& p, uint32_t car, bool getting_in = false) {
  ShowExtra(ctx, base, car, p.spec->extra, getting_in);
  p.car = car;
  p.up = false;
  p.angle = 0.0f;
  p.next_write = 0;
  PosePopupBone(ctx, base, *p.spec, car, 0.0f);
}

// Whether the pods are due a look this frame (in the car, or still folding).
bool PopupsDue(const PopupState& p, uint64_t now) {
  return p.in_car || (p.car && p.angle > 0.0f) || now >= p.next_probe;
}

// Every frame in the car (a look a few times a second otherwise). Returns
// whether the player is in it with the d-pad switch on (d-pad left is then
// the pods', not the radio's).
// Getting in (the car switched on) the pods flip up and the lamps come on;
// getting out (switched off) they flip down and go out. With the automatic
// mode they follow the game's headlights instead.
bool PopupLights(PPCContext& ctx, uint8_t* base, const GameContext& game, const State& s, uint16_t buttons,
                 PopupState& p, bool have) {
  if (!have)
    return false;
  const uint64_t now = NowMs();
  const auto& spec = *p.spec;
  const bool folding = !p.in_car && p.car && p.angle > 0.0f;  // still going down
  if (!p.in_car && !folding && now < p.next_probe)
    return false;
  bool in_car = p.in_car;
  if (!p.in_car || now >= p.next_check) {
    p.next_probe = now + 400;
    p.next_check = now + 250;
    const uint32_t model = ModelHash(spec.model);
    const auto& vehicle = PlayerVehicle(ctx, base, game);
    in_car = vehicle.car && vehicle.model == model;
    if (!in_car) {
      if (p.in_car && p.car) {
        // Switched off: down and out.
        p.up = false;
        if (VehicleAddress(base, p.car))
          Invoke(ctx, base, "FORCE_CAR_LIGHTS", {p.car, 1u});
        p.last_ms = now;
      }
      p.in_car = false;
      if (!folding && !(p.car && p.angle > 0.0f))
        return false;
    } else {
      const uint32_t car = *vehicle.car;
      if (!p.in_car || p.car != car) {
        if (p.car != car)
          PreparePopups(ctx, base, p, car, true);
        else  // back in the same car: make sure the pods' extra is still shown
          ShowExtra(ctx, base, car, spec.extra, true);
        p.prev_buttons = buttons;  // no edge from a button held while getting in
        p.last_ms = now;
        // Switched on: up and lit (or the game's choice, automatic).
        p.up = s.popup_auto ? HeadlightsWanted(ctx, base) : true;
        SetPopupLights(ctx, base, car, p.up, s.popup_auto);
      }
    }
    if (in_car) {
      if (s.popup_auto) {
        const bool want = HeadlightsWanted(ctx, base);
        if (want != p.up)
          p.up = want;
      }
      p.in_car = true;
    }
  }
  // The switch.
  const bool pressed = in_car && (buttons & X_INPUT_GAMEPAD_DPAD_LEFT) &&
                       !(p.prev_buttons & X_INPUT_GAMEPAD_DPAD_LEFT);
  p.prev_buttons = buttons;
  if (pressed && !s.popup_auto) {
    p.up = !p.up;
    SetPopupLights(ctx, base, p.car, p.up, false);
  }
  // The swing, eased at both ends; the pose is re-applied now and then while
  // settled, in case the game reset the skeleton (a car streamed back in).
  const float dt = std::min<float>(float(now - p.last_ms), 100.0f) / 1000.0f;
  p.last_ms = now;
  const float target = p.up ? spec.up_degrees : 0.0f;
  const float before = p.angle;
  // The motor whirs as the pods start to swing.
  if (target != before && !p.swinging)
    PopupMotorSound(spec, std::fabs(target - before) / kPopupDegreesPerSecond);
  p.swinging = target != before;
  const float step = kPopupDegreesPerSecond * dt;
  p.angle = std::fabs(target - before) <= step ? target : before + (target > before ? step : -step);
  if (p.angle != before || now >= p.next_write) {
    const float t = p.angle / spec.up_degrees;
    const float eased = spec.up_degrees * t * t * (3.0f - 2.0f * t);
    PosePopupBone(ctx, base, spec, p.car, eased);
    p.next_write = now + 250;
  }
  return in_car && !s.popup_auto;
}

// The add-on cars' doors. Their game names have no sounds in IV (so that
// only the trainer's engines play), doors included, so the trainer follows
// the doors of the one the player is in, getting into or just got out of,
// through the game's own door (GET_DOOR_ANGLE_RATIO: it opens as Niko pulls
// the handle and shuts as he swings it closed, the game's animations as on
// any car): a latch letting go as one opens, a slam as it shuts.
// The Dinka Nexus's doors are gullwings. The converter hangs each door panel
// (and its window) on a pivot of its own (the template's overheat points,
// which the game only reads to place engine smoke), hinged along the roof
// where the mod hangs them, turning about the car's length: 71 degrees up,
// the mod's own travel. The game's ordinary doors are still there, unseen,
// and each gullwing follows its door, raised as far as the game has it open;
// its gas struts hiss going up and coming down.
enum DoorCar : int {
  kDoorDeimos = 0, kDoorDominator, kDoorArbiter, kDoorNexus, kDoorGauntlet,
  kDoorDmc12, kDoorBttf1, kDoorBttf2, kDoorBttf3, kDoorBttf2Hover, kDoorCars
};
constexpr std::string_view kDoorModels[kDoorCars] = {"DEIMOS", "DOM645T", "ARBITERX", "NEXUS", "GAUNTLETC",
                                                     "DELNOR", "DELONE", "DELTWO", "DELTHR", "DELHOV"};
// The Nexus's and the DeLoreans' doors are gullwings: the converter hangs them
// on the template's overheat points, hinged along the roof, and they lift as
// the game's own (unseen) doors swing.
bool DoorIsGullwing(int kind) { return kind == kDoorNexus || kind >= kDoorDmc12; }
int DoorPoseSlot(int kind) { return kind >= kDoorDmc12 ? kPoseDelorean : kPoseNexus; }
enum class DoorEvent { kOpening, kLowering, kShut };
void DoorSound(int car, DoorEvent event);  // (with the cars' engines, below)
struct DoorWatch {
  uint32_t car = 0;            // the car followed
  int kind = -1;               // which of kDoorModels
  uint32_t info = 0;           // its model (the Nexus's pivots looked up for it)
  int32_t bone[2] = {-1, -1};  // the Nexus's pivots: driver's (left), passenger's
  float ratio[2]{}, peak[2]{};
  bool open[2]{}, lowering[2]{};
  uint64_t next_probe = 0, last_active = 0, next_write = 0;
  int log = 0;
};
DoorWatch g_doors;
constexpr std::string_view kGullwingPivots[2] = {"overheat", "overheat_2"};
constexpr float kGullwingDegrees = 71.0f;

bool HaveDoorCars() {
  return g_have_deimos || g_have_dominator || g_have_arbiter || g_have_nexus || g_have_gauntlet || g_have_dmc12 ||
         HaveTimeMachines();
}

bool DoorsBusy(const DoorWatch& d, uint64_t now) {
  return d.car && (now - d.last_active < 1500 || d.ratio[0] > 0.005f || d.ratio[1] > 0.005f);
}

int DoorCarKind(uint32_t model) {
  static const std::array<uint32_t, kDoorCars> hashes = [] {
    std::array<uint32_t, kDoorCars> h{};
    for (int k = 0; k < kDoorCars; ++k)
      h[size_t(k)] = ModelHash(kDoorModels[size_t(k)]);
    return h;
  }();
  for (int k = 0; k < kDoorCars; ++k)
    if (hashes[size_t(k)] == model)
      return k;
  return -1;
}

// Every frame while the player is in one of these cars, getting into one,
// or one he left still has a door open; a look a few times a second
// otherwise.
void FollowDoors(PPCContext& ctx, uint8_t* base, const GameContext& game) {
  auto& d = g_doors;
  const uint64_t now = NowMs();
  const bool busy = DoorsBusy(d, now);
  if (!busy && now < d.next_probe)
    return;
  d.next_probe = now + 300;
  // In one, getting into one, or the one he left with a door still open.
  uint32_t car = 0;
  int kind = -1;
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  if (vehicle.car) {
    kind = DoorCarKind(vehicle.model);
    if (kind >= 0)
      car = *vehicle.car;
  } else if (const auto getting = Invoke(ctx, base, "IS_CHAR_GETTING_IN_TO_A_CAR", {game.ped});
             getting && getting->r0()) {
    if (const auto target = Invoke(ctx, base, "GET_CAR_CHAR_IS_USING", {game.ped, 0}, 1);
        target && target->words[0])
      if (const auto m = Invoke(ctx, base, "GET_CAR_MODEL", {target->words[0], 0}, 1)) {
        kind = DoorCarKind(m->words[0]);
        if (kind >= 0)
          car = target->words[0];
      }
  }
  const bool active = car != 0;
  if (!car && busy) {
    car = d.car;
    kind = d.kind;
  }
  if (!car || kind < 0) {
    d.car = 0;
    return;
  }
  if (car != d.car) {
    const int log = d.log;
    d = DoorWatch{};
    d.log = log;
    d.car = car;
    d.kind = kind;
    d.next_probe = now + 300;
    // (a door already open when first met: no latch sound for it)
    for (int k = 0; k < 2; ++k)
      if (const auto r = Invoke(ctx, base, "GET_DOOR_ANGLE_RATIO", {car, uint32_t(k), 0}, 2))
        if (std::isfinite(r->f(0)) && std::fabs(r->f(0)) > 0.04f) {
          d.open[k] = true;
          d.peak[k] = std::fabs(r->f(0));
        }
  }
  if (active)
    d.last_active = now;
  const uint32_t model = ModelHash(kDoorModels[size_t(kind)]);
  const uint32_t vehicle_at = VehicleAddress(base, car);
  const uint32_t info = vehicle_at ? FindModelInfo(base, model) : 0;
  if (!info || !VehicleIsModel(base, vehicle_at, info)) {
    d.car = 0;  // gone (or the slot reused)
    return;
  }
  const bool gullwing = DoorIsGullwing(kind);
  if (gullwing && d.info != info) {
    d.info = info;
    for (int k = 0; k < 2; ++k)
      d.bone[k] = FindBone(ctx, base, vehicle_at, kGullwingPivots[k]);
    if (d.log < 3) {
      ++d.log;
      REXLOG_INFO("gta4-trainer: {} gullwings on bones {} and {}{}", std::string(kDoorModels[size_t(kind)]),
                  d.bone[0], d.bone[1],
                  NativeKnown("GET_DOOR_ANGLE_RATIO") ? "" : " (no GET_DOOR_ANGLE_RATIO: they stay shut)");
    }
  }
  const bool rewrite = now >= d.next_write;
  if (rewrite)
    d.next_write = now + 250;
  for (int k = 0; k < 2; ++k) {
    float ratio = 0.0f;
    if (const auto r = Invoke(ctx, base, "GET_DOOR_ANGLE_RATIO", {car, uint32_t(k), 0}, 2))
      ratio = std::isfinite(r->f(0)) ? std::clamp(std::fabs(r->f(0)), 0.0f, 1.0f) : 0.0f;
    // The latch as it leaves the frame; a gullwing's struts on its way down;
    // the slam as it reaches the frame again.
    if (!d.open[k] && ratio > 0.04f) {
      d.open[k] = true;
      d.lowering[k] = false;
      d.peak[k] = ratio;
      DoorSound(kind, DoorEvent::kOpening);
    } else if (d.open[k]) {
      d.peak[k] = std::max(d.peak[k], ratio);
      if (gullwing && !d.lowering[k] && d.peak[k] > 0.3f && ratio < d.peak[k] - 0.06f) {
        d.lowering[k] = true;
        DoorSound(kind, DoorEvent::kLowering);
      }
      if (ratio < 0.015f) {
        d.open[k] = false;
        if (d.peak[k] > 0.1f)
          DoorSound(kind, DoorEvent::kShut);
        d.peak[k] = 0.0f;
      }
    }
    if (gullwing && d.bone[k] >= 0 && (ratio != d.ratio[k] || rewrite))
      PoseBone(ctx, base, vehicle_at, info, DoorPoseSlot(kind), 1 + k, d.bone[k], 1,
               (k == 0 ? 1.0f : -1.0f) * kGullwingDegrees * ratio, 1.0f);
    if (d.log < 8 && ratio > 0.5f && d.ratio[k] <= 0.5f) {
      ++d.log;
      REXLOG_INFO("gta4-trainer: {} {} door open (ratio {:.2f})", std::string(kDoorModels[size_t(kind)]),
                  k == 0 ? "driver's" : "passenger's", ratio);
    }
    d.ratio[k] = ratio;
  }
}

// Headlights on d-pad left, in any car (an option, on by default; getting
// out switches the lamps off). The cars
// whose d-pad left already does something keep it: the Deimos (its pop-ups),
// the F-16 with its weapons on, the Batmobile with its gadgets on. Returns
// whether the player is in a car it switches (d-pad left is then not the
// radio's).
struct LightsState {
  bool in_car = false, on = false;
  uint32_t car = 0;
  uint16_t prev_buttons = 0;
  uint64_t next_look = 0;
};
LightsState g_lights;

bool CarLights(PPCContext& ctx, uint8_t* base, const GameContext& game, const State& s, uint16_t buttons) {
  const uint64_t now = NowMs();
  if (!g_lights.in_car || now >= g_lights.next_look) {
    g_lights.next_look = now + (g_lights.in_car ? 250 : 400);
    static const uint32_t kDeimos = ModelHash("DEIMOS"), kF16 = ModelHash("F16"),
                          kBatmobile = ModelHash("BATMOBILE"), kNexus = ModelHash("NEXUS");
    const auto& vehicle = PlayerVehicle(ctx, base, game);
    const bool taken = vehicle.model == kDeimos || vehicle.model == kNexus ||
                       (vehicle.model == kF16 && s.jet) || (vehicle.model == ModelHash("BATWING") && s.jet) ||
                       (vehicle.model == kBatmobile && s.gadgets);
    if (!vehicle.car || taken) {
      // Out of the car (switched off): its lamps go out.
      if (g_lights.in_car && g_lights.car && VehicleAddress(base, g_lights.car))
        Invoke(ctx, base, "FORCE_CAR_LIGHTS", {g_lights.car, 1u});
      g_lights.in_car = false;
      return false;
    }
    if (!g_lights.in_car || g_lights.car != *vehicle.car) {
      g_lights.car = *vehicle.car;
      Invoke(ctx, base, "FORCE_CAR_LIGHTS", {g_lights.car, 0u});  // the game's choice again
      g_lights.on = HeadlightsWanted(ctx, base);  // what the game is showing now
      g_lights.prev_buttons = buttons;           // no edge from a button held while getting in
    }
    g_lights.in_car = true;
  }
  const bool pressed = (buttons & X_INPUT_GAMEPAD_DPAD_LEFT) &&
                       !(g_lights.prev_buttons & X_INPUT_GAMEPAD_DPAD_LEFT);
  g_lights.prev_buttons = buttons;
  if (pressed) {
    g_lights.on = !g_lights.on;
    // FORCE_CAR_LIGHTS: 0 the game's choice, 1 off, 2 on.
    Invoke(ctx, base, "FORCE_CAR_LIGHTS", {g_lights.car, g_lights.on ? 2u : 1u});
  }
  return true;
}

void PrepareJet(PPCContext& ctx, uint8_t* base, uint32_t car, bool getting_in = false);
bool PrepareAircraft(PPCContext& ctx, uint8_t* base, uint32_t car, uint32_t model);

void PrepareLazar(PPCContext& ctx, uint8_t* base, uint32_t car, bool getting_in);  // (with its turret, below)

void FinishSpawn(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s) {
  if (s.pending_model < 0)
    return;
  const auto& choice = kVehicles[size_t(s.pending_model)];
  const std::string model(choice.model);
  const uint64_t now = NowMs();
  auto give_up = [&](std::string toast) {
    Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {s.pending_hash});
    s.pending_model = -1;
    Toast(s, std::move(toast), 6000);
  };
  const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {s.pending_hash});
  if (!loaded) {
    s.pending_model = -1;
    return;
  }
  if (!loaded->r0()) {
    ++s.pending_ticks;
    // Big models (the Batmobile is ~10 MB) can take a while to stream in,
    // longer once the world has been streaming a while (log 053: 17 s for
    // the Batmobile, a second Dominator not in 20). The request is renewed
    // every few seconds, in case the streamer let it go, and the wait shown.
    const uint64_t waited = now - s.pending_since;
    if (now >= s.pending_renew_at) {
      s.pending_renew_at = now + 4000;
      Invoke(ctx, base, "REQUEST_MODEL", {s.pending_hash});
      ++s.pending_renewals;
    }
    if (waited / 5000 != s.pending_toast_step) {
      s.pending_toast_step = waited / 5000;
      Toast(s, "Loading " + std::string(choice.label) + "... " + std::to_string(waited / 1000) + " s", 6000);
    }
    if (waited > 60000) {
      REXLOG_WARN("gta4-trainer: {} (hash {:08X}) did not load within 60 s ({} renewed requests)", model,
                  s.pending_hash, s.pending_renewals);
      rex::FlushLogging();
      give_up("The model did not load: the game's streaming memory may be full. Try again in a quieter spot.");
    }
    return;
  }
  if (!s.pending_loaded_at) {
    s.pending_loaded_at = now;
    if (now - s.pending_since > 3000)
      REXLOG_INFO("gta4-trainer: {} loaded after {:.1f} s ({} renewed requests)", model,
                  (now - s.pending_since) / 1000.0, s.pending_renewals);
  }
  // The guard. The game can finish setting a model up a moment after it
  // reports it loaded, so everything must be in place, and stay so, first.
  const ModelCheck check = CheckModel(base, s.pending_hash);
  if (!check.problem.empty()) {
    s.pending_ready_since = 0;
    s.pending_stable = 0;
    if (!s.pending_logged) {
      s.pending_logged = true;
      REXLOG_INFO("gta4-trainer: {} loaded, waiting: {} (model info {:08X}, fragment {:08X}, "
                  "structure {:08X})",
                  model, check.problem, check.info, check.fragment, check.structure);
    }
    if (now - s.pending_loaded_at < 8000)
      return;
    REXLOG_WARN("gta4-trainer: {} not spawned: {} after 8 s (model info {:08X}, vtable {:08X}, "
                "fragment {:08X}, structure {:08X})",
                model, check.problem, check.info, check.vtable, check.fragment, check.structure);
    ReportModel(base, model, s.pending_hash, check);
    if (gta4::addons::ShadowsSkippedThisLaunch())
      give_up("Add-on cars are off for this launch (the last two launches closed before the city "
              "loaded). Restart the game to turn them back on.");
    else
      give_up(std::string(choice.label) +
              " did not load properly, so it was not spawned (details in the log).");
    return;
  }
  if (!s.pending_ready_since) {
    s.pending_ready_since = now;
    if (s.pending_logged)
      REXLOG_INFO("gta4-trainer: {} ready {} ms after it loaded", model, now - s.pending_loaded_at);
  }
  if (++s.pending_stable < 3 || now - s.pending_ready_since < 250)
    return;
  // In front of the player (further for the long ones), slightly raised.
  const float ahead = choice.distance;
  const float x = game.position[0] + game.forward[0] * ahead;
  const float y = game.position[1] + game.forward[1] * ahead;
  const float z = game.position[2] + 1.0f;
  REXLOG_INFO("gta4-trainer: creating {} (model info {:08X}, structure {:08X})", model, check.info,
              check.structure);
  rex::FlushLogging();
  const auto car =
      Invoke(ctx, base, "CREATE_CAR", {s.pending_hash, Bits(x), Bits(y), Bits(z), 0, 1}, 4);
  if (car && car->words[0]) {
    const uint32_t handle = car->words[0];
    Invoke(ctx, base, "SET_CAR_ON_GROUND_PROPERLY", {handle});
    if (s.pending_hash == ModelHash("DEIMOS"))  // extras come up random: show the pods
      PreparePopups(ctx, base, g_popup, handle);
    if (s.pending_hash == ModelHash("NEXUS"))
      PreparePopups(ctx, base, g_nexus_popup, handle);
    PrepareAircraft(ctx, base, handle, s.pending_hash);  // ...and a jet's landing gear
    if (s.pending_hash == ModelHash("LAZAR3"))  // ...and the Lazar's turret, gun and ramp
      PrepareLazar(ctx, base, handle, false);
    if (const auto colours = Invoke(ctx, base, "GET_CAR_COLOURS", {handle, 0, 0}, 1, 0, 2))
      REXLOG_INFO("gta4-trainer: {} colours {} and {}", model, colours->words[0], colours->words[1]);
    if (s.warp_in)
      Invoke(ctx, base, "WARP_CHAR_INTO_CAR", {game.ped, handle});
    // Hand the car to the world so it cleans up like traffic.
    Invoke(ctx, base, "MARK_CAR_AS_NO_LONGER_NEEDED", {0}, 0, handle);
    REXLOG_INFO("gta4-trainer: {} spawned", model);
    Toast(s, std::string(choice.label) + " spawned.");
  } else {
    Toast(s, "The car could not be created here.");
  }
  Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {s.pending_hash});
  s.pending_model = -1;
}

// ---------------------------------------------------------------------------
// The F-16: flight, cannon and missiles, landing gear, engine sound and
// afterburner. On PC the jet flies through JulioNIB's Air Combat IV script
// (.NET), which cannot run here; this re-creates its core through natives.
// Arcade flight: once airborne, the jet's attitude (a quaternion steered by
// the left stick, banking turns it) and its speed along the nose are set
// every frame, so physics still handles collisions. The cannon fires hitscan
// bullets with IV's machine-gun muzzle flash; missiles are RPG rockets with
// their smoke trail, flown ahead until they meet the ground, a roof or a
// vehicle. The engine is synthesized (gta4_jet_engine.h): IV has no jets.
// ---------------------------------------------------------------------------
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};
Quat QMul(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat QAxis(float ax, float ay, float az, float angle) {
  const float h = angle * 0.5f, sn = std::sin(h);
  return {ax * sn, ay * sn, az * sn, std::cos(h)};
}
Quat QNormalize(Quat q) {
  const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!(n > 1e-6f) || !std::isfinite(n))
    return {};
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}
// v' = q v q*, for a unit quaternion.
void QRotate(const Quat& q, const float v[3], float out[3]) {
  const float tx = 2 * (q.y * v[2] - q.z * v[1]);
  const float ty = 2 * (q.z * v[0] - q.x * v[2]);
  const float tz = 2 * (q.x * v[1] - q.y * v[0]);
  out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
  out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
  out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

struct Missile {
  bool live = false;
  uint32_t object = 0, trail = 0;
  uint32_t target = 0;  // the locked car it homes on, or 0
  float pos[3]{}, dir[3]{};
  float speed = 0, travelled = 0;
};
// The jet's lock-on, GTA V's way: every car, bike, boat and helicopter
// within 600 m and 40 degrees of the nose is a target (a dot over it on the
// HUD); the one nearest the nose, or the one d-pad up picked, held 0.6 s
// locks (brackets closing on it, then red; a tone, a red mark on the radar,
// a red light over it). Missiles fired while locked home on it; the cannon
// aims at it while it is within 15 degrees of the nose. Turning away past 50
// degrees, losing it past 750 m or its wreck drops it.
struct JetLock {
  uint32_t candidate = 0, target = 0, blip = 0;
  uint32_t pick = 0;  // the target d-pad up chose (0: the one nearest the nose)
  uint64_t candidate_since = 0, next_scan = 0;
  uint64_t hidden_since = 0;  // the locked one out of sight since (v1.7.28)
  float pos[3]{};
  // how far targets are taken and kept (m): the jets' sky, the Lazar's street
  float range = 500.0f, keep_range = 600.0f;
  int hidden_logs = 0;
};
struct JetState {
  bool flying = false;
  uint32_t car = 0;
  Quat q;
  float speed = 0;
  uint64_t last_ms = 0, next_shot = 0, next_probe = 0, next_gear_write = 0;
  bool in_jet = false, announced = false, rocket_requested = false;
  std::array<Missile, 6> missiles{};
  // landing gear: 0 down .. 1 up (folded, hidden), and where it is heading
  float gear = 0.0f;
  bool gear_up = false, r3_held = false, r3_chord = false;
  // the afterburner's flames (looped effects, one per nozzle) and their size step
  uint32_t flame = 0, flame2 = 0;
  int flame_step = 0;
  uint16_t prev_buttons = 0;
  int pylon = 0;
  uint32_t shots = 0;
  int missile_log = 0;
  bool weapons_logged = false, flame_logged = false;
  // flight: turn rates (pitch, roll, yaw, rad/s) easing towards the stick;
  // the jet's height over the ground on its wheels; touchdown bookkeeping
  float rate[3]{};
  float rest_clearance = 1.2f;
  bool climbed = false, ready_told = false;
  uint64_t airborne_at = 0, blocked_since = 0;
  // what the log is told about this time in the jet (the takeoff's inputs)
  uint64_t entered_at = 0, next_pull_log = 0, next_flight_log = 0;
  bool spool_logged = false;
  int pull_logs = 0, flight_logs = 0;
  JetLock lock;
  // v1.7.22's flight model (gta4_jet_flight.h) and autopilot
  gta4::flight::State fs;
  gta4::flight::Pilot pilot;
  float last_pos[3]{};
  bool have_last_pos = false, vr_told = false, runway_checked = false;
  uint64_t ap_pressed_at = 0, next_stall_warn = 0;
  // v1.7.24's takeoff roll: its own speed, whether its height on its wheels
  // is known, what the log is told about the roll
  float roll_v = 0.0f;
  bool rest_known = false;
  int roll_logs = 0;
  uint64_t next_roll_log = 0;
  // v1.7.26: since when it has lain on its body with its wheels under the
  // road, or on its side or back (then it is stood back on its wheels)
  uint64_t stuck_since = 0;
  int stuck_logs = 0;
  // v1.7.30: the autopilot's search for open ground to land on
  struct Site {
    float x = 0, y = 0, yaw = 0, score = 0;
  };
  struct Search {
    bool active = false;
    uint64_t started = 0, next_scan = 0;
    float sea = 0.0f;
    std::vector<Site> sites;  // to try, nearest first
    size_t next = 0;
    bool found = false;
    Site best;
    float best_z = 0.0f;
    int probes = 0, tried = 0, scans = 0;
  } search;
};
JetState g_jet;
// Flight, after GTA V's jets: the engine's thrust (the throttle, the
// afterburner past 90%) against drag sets the speed, so idle settles near
// 25 m/s, full dry power at 70 and the afterburner at 90 (GTA V's Lazer
// tops out near 87); climbing costs speed and diving gains it. The stick
// turns the jet at rates that build up and die away and grow with airspeed;
// LB/RB work the rudder. The wings hold it up from about 32 m/s: slower,
// steeply banked or upside down the nose drops, and a stall dives until the
// speed comes back. v1.7.10 kept whatever speed it had at idle, up to 105.
// Taking off: pull back once the engines are at full power (the spool you
// hear, the throttle held), or rolling at 12 m/s; the jet leaves at flying
// speed and rotates nose up from its wheels (at least kRotateRate for the
// first moments, however gently the stick is pulled). For the first
// kLaunchGrace nothing ends the flight: the wheels are still on the ground
// and the physics has not caught up with the speed yet. (v1.7.14 and before
// needed 26 m/s on the ground; v1.7.15-16 tipped the nose up 7 degrees in
// one frame, into the runway.)
constexpr float kTakeoffSpeed = 12.0f;  // m/s (~43 km/h)
constexpr float kLaunchSpeed = 38.0f;   // m/s: what it leaves the ground at, at least
constexpr float kRotateRate = 0.35f;    // rad/s nose up, at least, while it rotates
constexpr uint64_t kRotateMs = 700, kLaunchGrace = 1500;  // ms
constexpr float kFullPower = 0.85f;     // the spool (0 off .. 1 full) that counts as full power
constexpr float kStallSpeed = 16.0f;    // on the ground below this the gear comes down
constexpr float kMaxJetSpeed = 95.0f;   // m/s (~340 km/h)
constexpr float kIdleThrust = 1.3f, kDryThrust = 10.3f, kBurnerThrust = 6.7f;  // m/s^2
constexpr float kDrag = 0.0021f;          // per (m/s)^2: full dry power settles at 70 m/s
constexpr float kLiftSpeed = 32.0f;       // full lift from here up
constexpr float kAuthoritySpeed = 45.0f;  // full control authority from here up
constexpr float kJetVolume = 0.4f;        // the engine's level (v1.7.10 played it at full)
constexpr std::string_view kRocketModel = "cj_rpg_rocket";
// Where the mod's Air Combat IV config (ac-iv.ini) puts them, in the jet's
// own space: the cannon by the cockpit, missiles under the wings, the nozzle.
// The aircraft the flight code flies: the F-16, and the Batwing (metalwars'
// model from the Batman game), whose PC version flew as the airtug with a
// plane handling line. Each: its two guns, its two missile rails, its
// nozzles (the afterburner's flames), whether it has landing gear to raise.
// Landing gear: the converter hangs the nose gear on extra_1 and the main
// gears on extra_5 and extra_6, each at its hinge. They fold up and are then
// hidden (neither aircraft has wells for them). The F-16's nose gear folds
// aft about the width, its mains inwards about the length; the Batwing's
// three wheels all fold aft.
struct GearLeg {
  uint32_t extra;
  int axis;  // 0: the car's width (X), 1: its length (Y)
  float degrees;
};
struct JetSpec {
  std::string_view model, label;
  float guns[2][3], pylons[2][3], nozzles[2][3];
  int nozzle_count;
  bool gear;
  float flame;  // the afterburner flame's size
  const gta4::flight::Params* flight;
  std::array<GearLeg, 3> legs;
  int slot;      // where the pose of its gear (and canopy) is kept
  int voice;     // its engine's voicing (gta4_jet_engine.h)
  float stand;   // its origin's height over the ground standing on its wheels (m)
  // where its wheels touch the ground (its own space: the nose gear, then the
  // left and right mains), kept out of the ground on its wheels (v1.7.30)
  const float (*contacts)[3] = nullptr;
};
constexpr float kBatwingContacts[3][3] = {{0.0f, 1.48f, -1.74f}, {-1.37f, -1.49f, -1.74f}, {1.37f, -1.49f, -1.74f}};
constexpr JetSpec kF16Jet{"F16", "F-16",
                          {{0.74f, 3.29f, 0.55f}, {-0.74f, 3.29f, 0.55f}},  // both sides, as asked
                          {{-2.5f, -3.0f, -0.3f}, {2.5f, -3.0f, -0.3f}},
                          {{0.0f, -5.45f, 0.40f}, {0.0f, 0.0f, 0.0f}}, 1, true, 1.0f, &gta4::flight::kF16,
                          {{{1, 0, -95.0f}, {5, 1, -95.0f}, {6, 1, 95.0f}}}, kPoseF16, 0, 1.05f};
// The Batwing's: guns at the tips of its nose prongs, missiles under the
// wing roots, its two engines' nozzles at the tail (the mod's exhaust
// points, a little aft).
constexpr JetSpec kBatwingJet{"BATWING", "Batwing",
                              {{0.62f, 4.3f, -0.86f}, {-0.62f, 4.3f, -0.86f}},
                              {{-2.4f, -1.6f, -0.95f}, {2.4f, -1.6f, -0.95f}},
                              {{1.2f, -4.25f, -0.26f}, {-1.2f, -4.25f, -0.26f}}, 2, true, 0.85f,
                              &gta4::flight::kBatwing,
                              {{{1, 0, -95.0f}, {5, 0, -95.0f}, {6, 0, -95.0f}}}, kPoseBatwing, 1, 1.65f,
                              kBatwingContacts};
const JetSpec* g_jet_spec = &kF16Jet;
const JetSpec* JetSpecOf(uint32_t model) {
  static const uint32_t f16 = ModelHash("F16"), batwing = ModelHash("BATWING");
  return model == f16 ? &kF16Jet : model == batwing ? &kBatwingJet : nullptr;
}
constexpr float kGearSeconds = 2.6f;

// The engine's voice in the port's own mixer (beside the game's sound).
gta4::jet::EngineSynth g_jet_synth;
#if REX_PLATFORM_PS5
class JetSource final : public gta4ps5::sound::Source {
 public:
  size_t Read(float* lr, size_t frames) override {
    g_jet_synth.Render(lr, frames);
    return frames;
  }
  bool Rewind() override { return true; }
};
gta4ps5::sound::Voice* g_jet_voice = nullptr;
#endif
// Starts the voice when it is not running (gunfire, a launch outside the jet).
void EnsureVoice() {
#if REX_PLATFORM_PS5
  if (!g_jet_voice) {
    gta4ps5::sound::StartOutput();
    g_jet_voice = gta4ps5::sound::NewVoice();
    g_jet_voice->game_sound = true;
  }
  if (!g_jet_voice->Active()) {
    g_jet_synth.SetTarget(0.0f, 0.0f, 1.0f);
    g_jet_voice->SetVolume(1.0f);
    g_jet_voice->Play(std::make_unique<JetSource>(), true);
  }
#endif
}
// The landing gear's hydraulics, and the Batwing's canopy (its own actuator
// and latch), through the jet's voice.
void GearSound(bool down) {
  g_jet_synth.Gear(kGearSeconds, down);
  EnsureVoice();
}
void BatwingCanopySound(bool opening) {
  g_jet_synth.SetVoice(1);
  g_jet_synth.Canopy(opening ? 1.1f : 1.3f, opening);
  EnsureVoice();
}
void GunSound() {
  g_jet_synth.Shot();
  EnsureVoice();
}
void LaunchSound() {
  g_jet_synth.Launch();
  EnsureVoice();
}
// Once a pass: a voice left running with nothing to play is stopped.
void LifeSoundTick();
void SoundTick() {
#if REX_PLATFORM_PS5
  if (g_jet_voice && g_jet_voice->Active() && g_jet_synth.Quiet())
    g_jet_voice->Stop();
#endif
  LifeSoundTick();
}

// Spool target (0 off, idle .. 1), afterburner, volume. The voice runs while
// the engine turns and stops once it has wound down.
void JetEngineSound(float spool, float afterburner, float volume) {
  g_jet_synth.SetTarget(spool, afterburner, volume);
#if REX_PLATFORM_PS5
  if (!g_jet_voice) {
    if (spool <= 0.0f)
      return;
    gta4ps5::sound::StartOutput();
    g_jet_voice = gta4ps5::sound::NewVoice();
    g_jet_voice->game_sound = true;  // quiet while the game mutes its own sound
  }
  if (spool > 0.0f && !g_jet_voice->Active()) {
    g_jet_voice->SetVolume(1.0f);
    g_jet_voice->Play(std::make_unique<JetSource>(), true);
  } else if (spool <= 0.0f && g_jet_voice->Active() && g_jet_synth.Quiet()) {
    g_jet_voice->Stop();
  }
#endif
}

float Axis(int16_t v) {
  const float f = float(v) / 32767.0f;
  constexpr float kDead = 0.18f;
  if (std::fabs(f) < kDead)
    return 0.0f;
  return std::clamp((f - std::copysign(kDead, f)) / (1.0f - kDead), -1.0f, 1.0f);
}

std::optional<float> GroundZ(PPCContext& ctx, uint8_t* base, float x, float y, float z) {
  // (the found flag in word 0, the height in word 1: see GroundBelow)
  const auto ground = Invoke(ctx, base, "GET_GROUND_Z_FOR_3D_COORD", {Bits(x), Bits(y), Bits(z), 0}, 3, 0, 1, -1, {}, 1);
  if (!ground || !ground->words[0])
    return std::nullopt;
  const float gz = ground->f(1);
  if (!std::isfinite(gz))
    return std::nullopt;
  return gz;
}

// The menu's weapons' blasts have no culprit in the game (ADD_EXPLOSION
// takes none), so the police never minded the Lazar's cannon or the jets'
// missiles. Each blast the player sets off adds heat (and each car the
// Lazar rams, a little; one it wrecks, a blast's worth), and the heat raises
// the wanted level the way the game's own crimes do: two stars at once,
// three after a few more blasts, four for a rampage; it cools off by one
// every 15 seconds. Never Wanted keeps it off.
float g_heat_pending = 0.0f, g_heat = 0.0f;
uint64_t g_heat_ms = 0;
void AddHeat(float weight) { g_heat_pending += weight; }

void Explode(PPCContext& ctx, uint8_t* base, const float p[3], float radius) {
  // type 2 (rocket), audible, visible, normal camera shake
  Invoke(ctx, base, "ADD_EXPLOSION",
         {Bits(p[0]), Bits(p[1]), Bits(p[2]), 2, Bits(radius), 1, 0, Bits(1.0f)});
  AddHeat(1.0f);
}

void DeleteObject(PPCContext& ctx, uint8_t* base, uint32_t& object) {
  if (object && object != 0xFFFFFFFFu)  // (all ones: a charge with no object)
    Invoke(ctx, base, "DELETE_OBJECT", {0}, 0, object);
  object = 0;
}

// A point in a car's own space, in the world.
std::optional<std::array<float, 3>> CarPoint(PPCContext& ctx, uint8_t* base, uint32_t car,
                                              const float local[3]) {
  const auto r = Invoke(ctx, base, "GET_OFFSET_FROM_CAR_IN_WORLD_COORDS",
                        {car, Bits(local[0]), Bits(local[1]), Bits(local[2]), 0, 0, 0}, 4, 0, 3);
  if (!r)
    return std::nullopt;
  const std::array<float, 3> p{r->f(0), r->f(1), r->f(2)};
  if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
    return std::nullopt;
  return p;
}

// One-shot particle effects (IV's own: its scripts fire "muz_machine" and
// "imp_bullet_default" this way). True when the game started it.
bool CarEffect(PPCContext& ctx, uint8_t* base, uint32_t car, std::string_view name, const float at[3],
               float scale) {
  const auto r = Invoke(ctx, base, "TRIGGER_PTFX_ON_VEH",
                        {0, car, Bits(at[0]), Bits(at[1]), Bits(at[2]), 0, 0, 0, Bits(scale)}, -1, 0, 1, 0,
                        name);
  return r && r->r0();
}
bool WorldEffect(PPCContext& ctx, uint8_t* base, std::string_view name, const float p[3], float scale) {
  const auto r = Invoke(ctx, base, "TRIGGER_PTFX",
                        {0, Bits(p[0]), Bits(p[1]), Bits(p[2]), 0, 0, 0, Bits(scale)}, -1, 0, 1, 0, name);
  return r && r->r0();
}

void EndMissile(PPCContext& ctx, uint8_t* base, Missile& m) {
  if (m.trail)
    Invoke(ctx, base, "STOP_PTFX", {m.trail});
  m.trail = 0;
  DeleteObject(ctx, base, m.object);
  m.live = false;
}

// An object's nose (its +Y) along a direction, level (no roll).
void PointObjectAlong(PPCContext& ctx, uint8_t* base, uint32_t object, const float dir[3]) {
  if (!object)
    return;
  const float heading = std::atan2(-dir[0], dir[1]), pitch = std::asin(std::clamp(dir[2], -1.0f, 1.0f));
  const Quat q = QNormalize(QMul(QAxis(0, 0, 1, heading), QAxis(1, 0, 0, pitch)));
  Invoke(ctx, base, "SET_OBJECT_QUATERNION", {object, Bits(q.x), Bits(q.y), Bits(q.z), Bits(q.w)});
}

// Where a car is, or nothing (gone).
std::optional<std::array<float, 3>> CarPosition(PPCContext& ctx, uint8_t* base, uint32_t car) {
  if (!VehicleAddress(base, car))
    return std::nullopt;
  const auto r = Invoke(ctx, base, "GET_CAR_COORDINATES", {car, 0, 0, 0}, 1, 0, 3);
  if (!r)
    return std::nullopt;
  const std::array<float, 3> p{r->f(0), r->f(1), r->f(2)};
  if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || (p[0] == 0 && p[1] == 0))
    return std::nullopt;
  return p;
}

// Turns `dir` towards `to` (from `at`) by at most `rate` rad/s; true when it
// pointed somewhere new.
bool TurnTowards(float dir[3], const float at[3], const float to[3], float rate, float dt) {
  float want[3], len = 0;
  for (int a = 0; a < 3; ++a) {
    want[a] = to[a] - at[a];
    len += want[a] * want[a];
  }
  len = std::sqrt(len);
  if (!(len > 0.5f))
    return false;
  float dot = 0;
  for (int a = 0; a < 3; ++a) {
    want[a] /= len;
    dot += want[a] * dir[a];
  }
  const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
  const float step = std::min(1.0f, rate * dt / std::max(angle, 1e-4f));
  float n = 0;
  for (int a = 0; a < 3; ++a) {
    dir[a] += (want[a] - dir[a]) * step;
    n += dir[a] * dir[a];
  }
  n = std::sqrt(n);
  if (!(n > 1e-4f))
    return false;
  for (int a = 0; a < 3; ++a)
    dir[a] /= n;
  return true;
}

// The nearest working car ahead of `from` along `fwd`, within `degrees` of
// it, out to `range` m: GET_CLOSEST_CAR looked for at points along the line,
// its search widening with distance.
uint32_t CarAhead(PPCContext& ctx, uint8_t* base, uint32_t self, const float from[3], const float fwd[3],
                  float degrees, float range) {
  const float min_cos = std::cos(degrees * 3.14159265f / 180.0f);
  for (const float d : {30.0f, 55.0f, 85.0f, 120.0f, 160.0f, 205.0f, 255.0f, 310.0f}) {
    if (d > range)
      break;
    const float p[3] = {from[0] + fwd[0] * d, from[1] + fwd[1] * d, from[2] + fwd[2] * d};
    const auto near = Invoke(ctx, base, "GET_CLOSEST_CAR",
                             {Bits(p[0]), Bits(p[1]), Bits(p[2]), Bits(4.0f + d * 0.21f), 0, 70});
    if (!near || !near->r0() || near->r0() == self)
      continue;
    const uint32_t car = near->r0();
    const auto cp = CarPosition(ctx, base, car);
    if (!cp)
      continue;
    float v[3], len = 0;
    for (int a = 0; a < 3; ++a) {
      v[a] = (*cp)[a] - from[a];
      len += v[a] * v[a];
    }
    len = std::sqrt(len);
    if (!(len > 1.0f) || len > range || (v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2]) / len < min_cos)
      continue;
    const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {car});
    if (dead && dead->r0())
      continue;
    return car;
  }
  return 0;
}

void DropLock(PPCContext& ctx, uint8_t* base, JetLock& l) {
  if (l.blip) {
    const auto exists = Invoke(ctx, base, "DOES_BLIP_EXIST", {l.blip});
    if (exists && exists->r0())
      Invoke(ctx, base, "REMOVE_BLIP", {l.blip});
  }
  l.blip = 0;
  l.target = 0;
  l.candidate = 0;
  l.hidden_since = 0;
}

// The HUD's targeting marks (a dot on each car or helicopter the jet can
// lock, brackets closing on the one it is locking, red once locked).
struct JetMark {
  float x = 0, y = 0;   // on the screen, 0..1
  bool heli = false;
  int state = 0;        // 0 a candidate, 1 locking (progress), 2 locked
  float progress = 0, distance = 0;
};
std::vector<JetMark> g_jet_marks;  // the targeting's, this frame (game thread)

// ---- targets: the vehicle pool, read directly ------------------------------
// Every vehicle in the game's pool (the layout VehicleAddress reads: +8 the
// slot count; a slot whose generation byte has its top bit set is free).
template <typename F>
void ForEachVehicle(uint8_t* base, F&& f) {
  constexpr uint32_t kVehiclePool = 0x82FB0700;
  if (!Span(base, kVehiclePool, 4))
    return;
  const uint32_t pool = Read(base, kVehiclePool);
  if (!Span(base, pool, 16))
    return;
  const uint32_t storage = Read(base, pool), flags = Read(base, pool + 4), count = Read(base, pool + 8),
                 size = Read(base, pool + 12);
  if (!count || count > 2048 || size < 64 || size > 0x4000 || !Span(base, flags, count))
    return;
  for (uint32_t slot = 0; slot < count; ++slot) {
    const uint8_t gen = *rex::memory::GuestPtr(base, flags + slot);
    if (gen & 0x80)
      continue;
    const uint32_t vehicle = storage + slot * size;
    if (Span(base, vehicle, 64))
      f((slot << 8) | gen, vehicle);
  }
}

// An entity's position from its matrix (entity +32, position row at +48; no
// matrix: inline at +16), as the player's is read. Trusted for vehicles once
// the jet's agrees with GET_CAR_COORDINATES; until then (or if it never
// does) the native is asked.
int g_entity_pos_ok = 0;  // 0 not checked yet, 1 memory, -1 the native
bool EntityPosition(uint8_t* base, uint32_t entity, float out[3]) {
  if (!Span(base, entity + kPedMatrix, 4))
    return false;
  const uint32_t matrix = Read(base, entity + kPedMatrix);
  const uint32_t at = matrix ? (Span(base, matrix, 64) ? matrix + kMatrixPosition : 0u) : entity + kPedInline;
  if (!at || !Span(base, at, 12))
    return false;
  for (uint32_t a = 0; a < 3; ++a)
    out[a] = ReadFloat(base, at + 4 * a);
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]) &&
         std::fabs(out[0]) < 20000.0f && std::fabs(out[1]) < 20000.0f;
}
void CheckEntityPositions(PPCContext& ctx, uint8_t* base, uint32_t car) {
  if (g_entity_pos_ok != 0)
    return;
  const auto native = CarPosition(ctx, base, car);
  const uint32_t vehicle = VehicleAddress(base, car);
  float p[3];
  if (!native || !vehicle || !EntityPosition(base, vehicle, p))
    return;
  const float d = std::hypot(p[0] - (*native)[0], p[1] - (*native)[1], p[2] - (*native)[2]);
  g_entity_pos_ok = d < 0.5f ? 1 : -1;
  REXLOG_INFO("gta4-trainer: jet targeting: vehicle positions {} (memory {:.1f} {:.1f} {:.1f}, native {:.1f} "
              "{:.1f} {:.1f})", g_entity_pos_ok > 0 ? "read from memory" : "asked of the game",
              p[0], p[1], p[2], (*native)[0], (*native)[1], (*native)[2]);
}
std::optional<std::array<float, 3>> TargetPosition(PPCContext& ctx, uint8_t* base, uint32_t car) {
  if (g_entity_pos_ok > 0) {
    float p[3];
    const uint32_t vehicle = VehicleAddress(base, car);
    if (vehicle && EntityPosition(base, vehicle, p))
      return std::array<float, 3>{p[0], p[1], p[2]};
    return std::nullopt;
  }
  return CarPosition(ctx, base, car);
}

struct JetTarget {
  uint32_t car = 0;
  float pos[3]{};
  float cosang = 0, dist = 0;
  bool heli = false;
};
std::vector<JetTarget> g_jet_targets;  // the last scan, nearest the nose first (game thread)
constexpr float kTargetCos = 0.766f, kTargetKeepCos = 0.643f;  // 40 and 50 degrees
// Line of sight to the targets (v1.7.28: the marks and locks went to cars
// behind buildings, half a kilometre off). GET_GROUND_Z_FOR_3D_COORD from
// high over points along the line finds the top of whatever stands there:
// a roof over the line hides what is behind it (a bridge over it, too).
struct Sight {
  uint32_t car = 0;
  bool clear = false;
  uint64_t at = 0;
};
std::array<Sight, 32> g_sight{};
constexpr uint64_t kSightMs = 500;
constexpr int kSightProbes = 40;  // a scan's probes at most

bool LineClear(PPCContext& ctx, uint8_t* base, const float a[3], const float b[3], int& budget) {
  const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (len < 10.0f)
    return true;
  const float step = std::clamp(len / 16.0f, 6.0f, 25.0f);
  for (float at = 5.0f; at < len - 4.0f; at += step) {
    --budget;
    const float k = at / len;
    const float p[3] = {a[0] + d[0] * k, a[1] + d[1] * k, a[2] + d[2] * k};
    if (const auto top = GroundZ(ctx, base, p[0], p[1], p[2] + 250.0f); top && *top > p[2] + 1.0f)
      return false;
  }
  return true;
}

// Whether a car can be seen from `from`: tested again after kSightMs while
// the scan's probes last; not known yet (and none left): nothing.
std::optional<bool> SightTo(PPCContext& ctx, uint8_t* base, const float from[3], uint32_t car, const float pos[3],
                            uint64_t now, int& budget) {
  Sight* slot = nullptr;
  Sight* oldest = &g_sight[0];
  for (auto& e : g_sight) {
    if (e.car == car) {
      slot = &e;
      break;
    }
    if (e.at < oldest->at)
      oldest = &e;
  }
  if (slot && now - slot->at < kSightMs)
    return slot->clear;
  if (budget <= 0)
    return slot ? std::optional<bool>(slot->clear) : std::nullopt;
  const float to[3] = {pos[0], pos[1], pos[2] + 1.0f};
  const bool clear = LineClear(ctx, base, from, to, budget);
  if (!slot)
    slot = oldest;
  *slot = Sight{car, clear, now};
  return clear;
}

constexpr uint64_t kLockMs = 600;
constexpr size_t kMaxTargets = 12;

// The targets ahead (five or six times a second): the pool's vehicles in the
// cone, nearest the nose first; working ones only, and only what can be
// driven or flown (cars, bikes, boats, helicopters; not trains).
void ScanTargets(PPCContext& ctx, uint8_t* base, const JetState& j, const float from[3], const float fwd[3],
                 uint64_t now) {
  g_jet_targets.clear();
  if (g_entity_pos_ok <= 0) {
    // (positions not readable directly: the nose's search, one at a time)
    if (const uint32_t car = CarAhead(ctx, base, j.car, from, fwd, 12.0f, 350.0f))
      if (const auto p = CarPosition(ctx, base, car)) {
        JetTarget t;
        t.car = car;
        std::copy(p->begin(), p->end(), t.pos);
        t.cosang = 1.0f;
        if (const auto m = Invoke(ctx, base, "GET_CAR_MODEL", {car, 0}, 1))
          t.heli = ModelKind(ctx, base, m->words[0]) == Kind::kHeli;
        g_jet_targets.push_back(t);
      }
    return;
  }
  std::vector<JetTarget> near;
  ForEachVehicle(base, [&](uint32_t handle, uint32_t vehicle) {
    if (handle == j.car)
      return;
    JetTarget t;
    if (!EntityPosition(base, vehicle, t.pos))
      return;
    float v[3], d = 0;
    for (int a = 0; a < 3; ++a) {
      v[a] = t.pos[a] - from[a];
      d += v[a] * v[a];
    }
    d = std::sqrt(d);
    if (d < 8.0f || d > j.lock.range)
      return;
    t.cosang = (v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2]) / d;
    if (t.cosang < kTargetCos)
      return;
    t.car = handle;
    t.dist = d;
    near.push_back(t);
  });
  std::sort(near.begin(), near.end(), [](const JetTarget& a, const JetTarget& b) { return a.cosang > b.cosang; });
  int budget = kSightProbes;
  for (auto& t : near) {
    if (g_jet_targets.size() >= kMaxTargets)
      break;
    const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {t.car});
    if (dead && dead->r0())
      continue;
    const auto model = Invoke(ctx, base, "GET_CAR_MODEL", {t.car, 0}, 1);
    if (!model)
      continue;
    const Kind kind = ModelKind(ctx, base, model->words[0]);
    if (kind == Kind::kOther)
      continue;
    t.heli = kind == Kind::kHeli;
    // only what can be seen: not behind a building
    const auto seen = SightTo(ctx, base, from, t.car, t.pos, now, budget);
    if (!seen || !*seen)
      continue;
    g_jet_targets.push_back(t);
  }
}

// Every frame in the jet with its weapons on. `cycle`: d-pad up was pressed
// (the next target round).
void UpdateLock(PPCContext& ctx, uint8_t* base, JetState& j, State& s, const float from[3], const float fwd[3],
                uint64_t now, bool cycle) {
  auto& l = j.lock;
  CheckEntityPositions(ctx, base, j.car);
  if (now >= l.next_scan) {
    l.next_scan = now + 170;
    ScanTargets(ctx, base, j, from, fwd, now);
  }
  if (l.target) {
    bool keep = false;
    if (const auto tp = TargetPosition(ctx, base, l.target)) {
      float v[3], len = 0;
      for (int a = 0; a < 3; ++a) {
        v[a] = (*tp)[a] - from[a];
        len += v[a] * v[a];
      }
      len = std::sqrt(len);
      const float c = len > 1.0f ? (v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2]) / len : 1.0f;
      const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {l.target});
      // out of sight (behind a building) a second and a half: let go
      int budget = 12;
      const auto seen = SightTo(ctx, base, from, l.target, tp->data(), now, budget);
      if (seen && !*seen) {
        if (!l.hidden_since)
          l.hidden_since = now;
      } else {
        l.hidden_since = 0;
      }
      const bool hidden = l.hidden_since && now - l.hidden_since > 1500;
      keep = len < l.keep_range && c > kTargetKeepCos && !(dead && dead->r0()) && !hidden;
      if (hidden && l.hidden_logs < 6) {
        ++l.hidden_logs;
        REXLOG_INFO("gta4-trainer: lock on {:08X} let go: out of sight {:.0f} m off", l.target, len);
      }
      for (int a = 0; a < 3; ++a)
        l.pos[a] = (*tp)[a];
    }
    if (!keep) {
      DropLock(ctx, base, l);
      l.pick = 0;
      Toast(s, "Lock lost.", 800);
    } else {
      Invoke(ctx, base, "DRAW_LIGHT_WITH_RANGE",
             {Bits(l.pos[0]), Bits(l.pos[1]), Bits(l.pos[2] + 1.4f), 255, 0, 0, Bits(5.0f), Bits(7.0f)});
    }
  }
  // d-pad up: the next one round (from the locked one, or the one locking).
  if (cycle && !g_jet_targets.empty()) {
    const uint32_t current = l.target ? l.target : l.candidate;
    size_t next = 0;
    for (size_t i = 0; i < g_jet_targets.size(); ++i)
      if (g_jet_targets[i].car == current) {
        next = (i + 1) % g_jet_targets.size();
        break;
      }
    const uint32_t pick = g_jet_targets[next].car;
    if (pick != l.target || g_jet_targets.size() == 1) {
      DropLock(ctx, base, l);
      l.pick = l.candidate = pick;
      l.candidate_since = now;
    }
  }
  if (l.target)
    return;
  // Locking: the one picked while it is still a target, else the one nearest the nose.
  uint32_t want = 0;
  if (l.pick && std::any_of(g_jet_targets.begin(), g_jet_targets.end(),
                            [&](const JetTarget& t) { return t.car == l.pick; }))
    want = l.pick;
  else {
    l.pick = 0;
    if (!g_jet_targets.empty())
      want = g_jet_targets.front().car;
  }
  if (want != l.candidate) {
    l.candidate = want;
    l.candidate_since = now;
  }
  if (l.candidate && now - l.candidate_since >= kLockMs) {
    const uint32_t found = l.candidate;
    DropLock(ctx, base, l);
    l.pick = found == l.pick ? found : l.pick;
    l.target = found;
    if (const auto tp = TargetPosition(ctx, base, found))
      for (int a = 0; a < 3; ++a)
        l.pos[a] = (*tp)[a];
    if (const auto b = Invoke(ctx, base, "ADD_BLIP_FOR_CAR", {found, 0}, 1); b && b->r0()) {
      l.blip = b->r0();
      Invoke(ctx, base, "CHANGE_BLIP_COLOUR", {l.blip, 1});
    }
    g_jet_synth.Beep();
    EnsureVoice();
    Toast(s, "Target locked.", 900);
  }
}

// ---- the screen: where a point in the world shows --------------------------
// The game's current viewport (rage::grcViewport, the global's pointer, or
// the global itself): its camera's position at +0x70 and its view-projection
// matrix at +0x100 (rows for row vectors: clip = [x y z 1] M). Used only
// while that camera is where GET_CAM_POS puts the game's (not a shadow's or
// a reflection's viewport) and the point ahead of the jet lands on the
// screen. Otherwise the camera's own position, rotation and field of view
// (natives) make the projection, its heading's convention settled by that
// same point.
constexpr uint32_t kCurrentViewportGlobal = 0x831C2200;
constexpr uint32_t kViewportCamera = 0x70, kViewportViewProj = 0x100;
std::atomic<float> g_display_aspect{16.0f / 9.0f};  // (from the overlay)
struct Projector {
  bool ok = false, matrix = false;
  float m[4][4]{};
  float cam[3]{}, f[3]{}, r[3]{}, u[3]{};
  float tan_half = 0.6f, aspect = 16.0f / 9.0f;
  bool Project(const float p[3], float& sx, float& sy) const {
    if (matrix) {
      const float cx = p[0] * m[0][0] + p[1] * m[1][0] + p[2] * m[2][0] + m[3][0];
      const float cy = p[0] * m[0][1] + p[1] * m[1][1] + p[2] * m[2][1] + m[3][1];
      const float cw = p[0] * m[0][3] + p[1] * m[1][3] + p[2] * m[2][3] + m[3][3];
      if (!(cw > 0.5f))
        return false;
      sx = 0.5f + 0.5f * cx / cw;
      sy = 0.5f - 0.5f * cy / cw;
    } else {
      const float d[3] = {p[0] - cam[0], p[1] - cam[1], p[2] - cam[2]};
      const float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
      if (!(z > 0.5f))
        return false;
      sx = 0.5f + 0.5f * (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (z * tan_half * aspect);
      sy = 0.5f - 0.5f * (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / (z * tan_half);
    }
    return std::isfinite(sx) && std::isfinite(sy);
  }
};
int g_cam_convention = -1;  // the heading convention that fits (-1 not settled)
int g_projector_log = 0;
bool OnScreen(const Projector& pr, const float p[3]) {
  float sx, sy;
  return pr.Project(p, sx, sy) && sx > 0.02f && sx < 0.98f && sy > 0.02f && sy < 0.98f;
}
Projector MakeProjector(PPCContext& ctx, uint8_t* base, const float probe[3]) {
  Projector pr;
  const auto cam = Invoke(ctx, base, "GET_GAME_CAM", {0}, 0);
  if (!cam || !cam->words[0])
    return pr;
  const auto pos = Invoke(ctx, base, "GET_CAM_POS", {cam->words[0], 0, 0, 0}, 1, 0, 3);
  if (!pos || !std::isfinite(pos->f(0)))
    return pr;
  const float cp[3] = {pos->f(0), pos->f(1), pos->f(2)};
  // The viewport's matrix.
  const uint32_t candidates[2] = {Span(base, kCurrentViewportGlobal, 4) ? Read(base, kCurrentViewportGlobal) : 0u,
                                  kCurrentViewportGlobal};
  for (const uint32_t vp : candidates) {
    if (!vp || !Span(base, vp + kViewportCamera, 12) || !Span(base, vp + kViewportViewProj, 64))
      continue;
    float d2 = 0;
    for (uint32_t a = 0; a < 3; ++a) {
      const float d = ReadFloat(base, vp + kViewportCamera + 4 * a) - cp[a];
      d2 += d * d;
    }
    if (!(d2 < 4.0f))
      continue;
    Projector mp;
    mp.matrix = true;
    for (uint32_t row = 0; row < 4; ++row)
      for (uint32_t col = 0; col < 4; ++col)
        mp.m[row][col] = ReadFloat(base, vp + kViewportViewProj + 16 * row + 4 * col);
    if (OnScreen(mp, probe)) {
      mp.ok = true;
      if (g_projector_log < 1) {
        ++g_projector_log;
        REXLOG_INFO("gta4-trainer: jet targeting: the viewport's matrix ({:08X}) projects the targets", vp);
      }
      return mp;
    }
  }
  // The camera's own: position, rotation (pitch, roll, heading in degrees),
  // field of view.
  const auto rot = Invoke(ctx, base, "GET_CAM_ROT", {cam->words[0], 0, 0, 0}, 1, 0, 3);
  const auto fov = Invoke(ctx, base, "GET_CAM_FOV", {cam->words[0], 0}, 1);
  if (!rot || !std::isfinite(rot->f(0)) || !std::isfinite(rot->f(2)))
    return pr;
  float fov_deg = fov && std::isfinite(fov->f(0)) && fov->f(0) > 5.0f && fov->f(0) < 140.0f ? fov->f(0) : 45.0f;
  pr.tan_half = std::tan(fov_deg * 0.5f * 3.14159265f / 180.0f);
  pr.aspect = g_display_aspect.load(std::memory_order_relaxed);
  std::copy(cp, cp + 3, pr.cam);
  const float pitch = rot->f(0) * 3.14159265f / 180.0f;
  auto basis = [&](int convention) {
    // 0: heading as entities' (0 north, turning left); 1-3: a quarter turn
    // round each; 4-7: the same, turning right.
    const float h = (convention % 4) * 1.5707963f + (convention >= 4 ? -1.0f : 1.0f) * rot->f(2) * 3.14159265f / 180.0f;
    pr.f[0] = -std::sin(h) * std::cos(pitch);
    pr.f[1] = std::cos(h) * std::cos(pitch);
    pr.f[2] = std::sin(pitch);
    pr.r[0] = std::cos(h);
    pr.r[1] = std::sin(h);
    pr.r[2] = 0.0f;
    pr.u[0] = pr.r[1] * pr.f[2] - pr.r[2] * pr.f[1];
    pr.u[1] = pr.r[2] * pr.f[0] - pr.r[0] * pr.f[2];
    pr.u[2] = pr.r[0] * pr.f[1] - pr.r[1] * pr.f[0];
  };
  if (g_cam_convention >= 0) {
    basis(g_cam_convention);
    pr.ok = OnScreen(pr, probe);
    if (pr.ok)
      return pr;
  }
  // (settled by the point ahead of the jet: the convention that puts it
  // nearest the middle of the screen)
  int best = -1;
  float best_d = 1e9f;
  for (int c = 0; c < 8; ++c) {
    basis(c);
    float sx, sy;
    if (pr.Project(probe, sx, sy)) {
      const float d = std::fabs(sx - 0.5f) + std::fabs(sy - 0.5f);
      if (d < best_d) {
        best_d = d;
        best = c;
      }
    }
  }
  if (best < 0 || best_d > 0.9f)
    return pr;
  basis(best);
  pr.ok = true;
  if (g_cam_convention != best && g_projector_log < 4) {
    ++g_projector_log;
    REXLOG_INFO("gta4-trainer: jet targeting: projecting with the camera's rotation (convention {}, fov {:.0f})",
                best, fov_deg);
  }
  g_cam_convention = best;
  return pr;
}

// The HUD's marks for this frame: every target's dot, the one locking (its
// brackets' progress) and the locked one, where they show on the screen.
void BuildMarks(PPCContext& ctx, uint8_t* base, const JetState& j, const float probe[3], uint64_t now) {
  g_jet_marks.clear();
  if (g_jet_targets.empty() && !j.lock.target)
    return;
  const Projector pr = MakeProjector(ctx, base, probe);
  if (!pr.ok)
    return;
  auto add = [&](uint32_t car, const float p[3], bool heli, float dist) {
    const float at[3] = {p[0], p[1], p[2] + (heli ? 0.6f : 1.0f)};
    float sx, sy;
    if (!pr.Project(at, sx, sy) || sx < 0.0f || sx > 1.0f || sy < 0.0f || sy > 1.0f)
      return;
    JetMark m;
    m.x = sx;
    m.y = sy;
    m.heli = heli;
    m.distance = dist;
    if (car == j.lock.target) {
      m.state = 2;
      m.progress = 1.0f;
    } else if (car == j.lock.candidate) {
      m.state = 1;
      m.progress = std::clamp(float(now - j.lock.candidate_since) / float(kLockMs), 0.0f, 1.0f);
    }
    g_jet_marks.push_back(m);
  };
  bool locked_shown = false;
  for (const auto& t : g_jet_targets) {
    float p[3] = {t.pos[0], t.pos[1], t.pos[2]};
    if (const auto now_at = TargetPosition(ctx, base, t.car))  // (where it is this frame)
      std::copy(now_at->begin(), now_at->end(), p);
    add(t.car, p, t.heli, t.dist);
    locked_shown |= t.car == j.lock.target;
  }
  if (j.lock.target && !locked_shown)
    add(j.lock.target, j.lock.pos, false, 0.0f);
}

// Missiles keep flying after the player leaves the jet; a locked one homes
// on its car (3 rad/s at most, its nose turning with it).
void UpdateMissiles(PPCContext& ctx, uint8_t* base, JetState& j, float dt) {
  for (auto& m : j.missiles) {
    if (!m.live)
      continue;
    std::optional<std::array<float, 3>> aim;
    if (m.target) {
      aim = CarPosition(ctx, base, m.target);
      if (!aim)
        m.target = 0;  // gone: on straight
      else
        (*aim)[2] += 0.3f;
    }
    if (aim && m.travelled > 12.0f && TurnTowards(m.dir, m.pos, aim->data(), 3.0f, dt))
      PointObjectAlong(ctx, base, m.object, m.dir);
    const float step = m.speed * dt;
    for (int a = 0; a < 3; ++a)
      m.pos[a] += m.dir[a] * step;
    m.travelled += step;
    const char* hit = nullptr;
    const auto gz = GroundZ(ctx, base, m.pos[0], m.pos[1], m.pos[2] + 2.0f);
    if (gz && m.pos[2] <= *gz + 0.6f) {
      m.pos[2] = *gz + 0.6f;
      hit = "the ground";
    }
    if (!hit && aim) {
      float t2 = 0;
      for (int a = 0; a < 3; ++a)
        t2 += (m.pos[a] - (*aim)[a]) * (m.pos[a] - (*aim)[a]);
      if (t2 < 4.0f * 4.0f) {
        hit = "its target";
        for (int a = 0; a < 3; ++a)
          m.pos[a] = (*aim)[a];
      }
    }
    if (!hit && m.travelled > 25.0f) {  // (clear of the jet first)
      const auto near = Invoke(ctx, base, "GET_CLOSEST_CAR",
                               {Bits(m.pos[0]), Bits(m.pos[1]), Bits(m.pos[2]), Bits(5.0f), 0, 70});
      if (near && near->r0() && near->r0() != j.car)
        hit = "a vehicle";
    }
    if (!hit && (m.travelled > 1500.0f || !std::isfinite(m.pos[2])))
      hit = "the end of its range";
    if (hit) {
      Explode(ctx, base, m.pos, 8.0f);
      if (j.missile_log < 16) {
        ++j.missile_log;
        REXLOG_INFO("gta4-trainer: F-16 missile hit {} after {:.0f} m", hit, m.travelled);
      }
      EndMissile(ctx, base, m);
      continue;
    }
    if (m.object)
      Invoke(ctx, base, "SET_OBJECT_COORDINATES",
             {m.object, Bits(m.pos[0]), Bits(m.pos[1]), Bits(m.pos[2])});
  }
}

void LaunchMissile(PPCContext& ctx, uint8_t* base, JetState& j, const float fwd[3], const Quat& q,
                   State& s) {
  Missile* slot = nullptr;
  for (auto& m : j.missiles)
    if (!m.live) {
      slot = &m;
      break;
    }
  if (!slot) {
    Toast(s, "Missiles reloading.", 900);
    return;
  }
  const auto start = CarPoint(ctx, base, j.car, g_jet_spec->pylons[j.pylon]);
  j.pylon ^= 1;  // left, right, left...
  if (!start)
    return;
  Missile& m = *slot;
  m = {};
  m.live = true;
  for (int a = 0; a < 3; ++a) {
    m.dir[a] = fwd[a];
    m.pos[a] = (*start)[a];
  }
  m.speed = std::max(j.flying ? j.fs.speed : 0.0f, 0.0f) + 90.0f;
  m.target = j.lock.target;
  // The rocket you see: the RPG's projectile with its smoke trail, moved by hand.
  const uint32_t hash = ModelHash(kRocketModel);
  const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {hash});
  if (loaded && loaded->r0()) {
    const auto obj = Invoke(ctx, base, "CREATE_OBJECT",
                            {hash, Bits(m.pos[0]), Bits(m.pos[1]), Bits(m.pos[2]), 0, 1}, 4);
    if (obj && obj->words[0]) {
      m.object = obj->words[0];
      Invoke(ctx, base, "SET_OBJECT_COLLISION", {m.object, 0});
      Invoke(ctx, base, "FREEZE_OBJECT_POSITION", {m.object, 1});
      Invoke(ctx, base, "SET_OBJECT_QUATERNION", {m.object, Bits(q.x), Bits(q.y), Bits(q.z), Bits(q.w)});
      if (const auto trail = Invoke(ctx, base, "START_PTFX_ON_OBJ",
                                    {0, m.object, 0, 0, 0, 0, 0, 0, Bits(1.0f)}, -1, 0, 1, 0, "weap_rocket"))
        m.trail = trail->r0();
    }
  }
  g_jet_synth.Launch();
  if (j.missile_log < 16) {
    ++j.missile_log;
    REXLOG_INFO("gta4-trainer: F-16 missile launched at {:.0f} m/s (rocket model {}, object {:08X}, trail {:08X}, "
                "target {:08X})",
                m.speed, loaded && loaded->r0() ? "loaded" : "not loaded", m.object, m.trail, m.target);
  }
}

// The cannon: a round from each side every shot, each with IV's machine-gun
// muzzle flash (the PC mod fires from one side, as the real F-16's single
// gun does; both sides here, as asked). Locked on a car within 15 degrees
// of the nose, both aim at it.
void FireCannon(PPCContext& ctx, uint8_t* base, JetState& j, const float fwd[3]) {
  static uint32_t seed = 0x1234567u;
  AddHeat(0.07f);  // (a second's burst: two stars, as the Batmobile's guns)
  auto scatter = [](float spread) {
    seed = seed * 1664525u + 1013904223u;
    return (float((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * spread;
  };
  bool flashed = false, aimed = false;
  float line[3] = {fwd[0], fwd[1], fwd[2]};
  float origin[3]{};
  for (const auto& gun_at : g_jet_spec->guns) {
    const auto gun = CarPoint(ctx, base, j.car, gun_at);
    if (!gun)
      continue;
    float to[3], dir[3] = {fwd[0], fwd[1], fwd[2]};
    if (j.lock.target) {
      float v[3], len = 0;
      for (int a = 0; a < 3; ++a) {
        v[a] = j.lock.pos[a] + (a == 2 ? 0.4f : 0.0f) - (*gun)[a];
        len += v[a] * v[a];
      }
      len = std::sqrt(len);
      if (len > 2.0f && (v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2]) / len > 0.966f) {  // (cos 15 degrees)
        for (int a = 0; a < 3; ++a)
          dir[a] = v[a] / len;
        aimed = true;
      }
    }
    float from[3];
    for (int a = 0; a < 3; ++a) {
      from[a] = (*gun)[a] + dir[a] * 3.0f;
      to[a] = (*gun)[a] + dir[a] * (aimed ? 450.0f : 900.0f) + scatter(aimed ? 2.0f : 5.0f);
      line[a] = dir[a];
      origin[a] = from[a];
    }
    Invoke(ctx, base, "FIRE_SINGLE_BULLET",
           {Bits(from[0]), Bits(from[1]), Bits(from[2]), Bits(to[0]), Bits(to[1]), Bits(to[2]), 60});
    if ((j.shots & 1) == 0)
      flashed |= CarEffect(ctx, base, j.car, "muz_machine", gun_at, 1.6f);
  }
  // Where the burst meets the ground, a puff of IV's bullet impact (every
  // third round: the line of fire is walked in 45 m steps).
  bool puff = false;
  if (j.shots % 3 == 0) {
    for (float d = 45.0f; d <= 900.0f; d += 45.0f) {
      float p[3] = {origin[0] + line[0] * d, origin[1] + line[1] * d, origin[2] + line[2] * d};
      const auto gz = GroundZ(ctx, base, p[0], p[1], p[2] + 1.0f);
      if (gz && p[2] <= *gz + 0.5f) {
        p[2] = *gz + 0.2f;
        puff = WorldEffect(ctx, base, "imp_bullet_default", p, 3.0f);
        break;
      }
    }
  }
  GunSound();
  if (!j.weapons_logged) {
    j.weapons_logged = true;
    REXLOG_INFO("gta4-trainer: F-16 cannon firing from both sides (FIRE_SINGLE_BULLET {}, muzzle flash {}, impact "
                "effect {}, {})",
                NativeKnown("FIRE_SINGLE_BULLET") ? "found" : "missing", flashed ? "started" : "not started",
                puff ? "started" : "none yet", aimed ? "aimed at the lock" : "along the nose");
  }
  ++j.shots;
}

// The landing gear at `t` (0 down, 1 up), eased; folded away it is hidden.
void PoseGear(PPCContext& ctx, uint8_t* base, uint32_t car, float t) {
  const JetSpec& spec = *g_jet_spec;
  const float eased = t * t * (3.0f - 2.0f * t);
  const float scale = t >= 0.999f ? 0.0f : 1.0f;
  int entry = 0;
  for (const auto& leg : spec.legs)
    PoseExtraBone(ctx, base, car, ModelHash(spec.model), spec.slot, entry++, leg.extra, leg.axis,
                  leg.degrees * eased, scale);
}

// An F-16 just made or first met: its gear's extras shown (the game picks
// extras at random, and a hidden one is sent to the world's origin), down.
void PrepareJet(PPCContext& ctx, uint8_t* base, uint32_t car, bool getting_in) {
  for (const auto& leg : g_jet_spec->legs)
    ShowExtra(ctx, base, car, leg.extra, getting_in);
  g_jet.gear = 0.0f;
  g_jet.gear_up = false;
  g_jet.next_gear_write = 0;
  PoseGear(ctx, base, car, 0.0f);
}

// A jet just made: its gear's extras shown and down (the F-16, the Batwing).
bool PrepareAircraft(PPCContext& ctx, uint8_t* base, uint32_t car, uint32_t model) {
  const JetSpec* spec = JetSpecOf(model);
  if (!spec || !spec->gear)
    return false;
  g_jet_spec = spec;
  PrepareJet(ctx, base, car);
  return true;
}

void StopFlame(PPCContext& ctx, uint8_t* base, JetState& j) {
  for (uint32_t* f : {&j.flame, &j.flame2}) {
    if (*f)
      Invoke(ctx, base, "STOP_PTFX", {*f});
    *f = 0;
  }
  j.flame_step = 0;
}

// The exhaust: IV's rocket flame at the nozzle, small at high throttle,
// large with the afterburner.
void UpdateFlame(PPCContext& ctx, uint8_t* base, JetState& j, uint32_t car, float throttle, bool burner) {
  const int step = burner ? 2 : (throttle > 0.55f ? 1 : 0);
  if (step == j.flame_step)
    return;
  StopFlame(ctx, base, j);
  j.flame_step = step;
  if (!step)
    return;
  const JetSpec& spec = *g_jet_spec;
  for (int k = 0; k < spec.nozzle_count; ++k) {
    const auto& n = spec.nozzles[k];
    const auto r = Invoke(ctx, base, "START_PTFX_ON_VEH",
                          {0, car, Bits(n[0]), Bits(n[1]), Bits(n[2]), 0, 0, 0,
                           Bits((step == 2 ? 1.7f : 0.7f) * spec.flame)},
                          -1, 0, 1, 0, "weap_rocket");
    (k ? j.flame2 : j.flame) = r ? r->r0() : 0;
  }
  if (!j.flame_logged) {
    j.flame_logged = true;
    REXLOG_INFO("gta4-trainer: {} exhaust flame {:08X}", std::string(spec.label), j.flame);
  }
}

// Raining cars, the old trainers' favourite: ordinary cars dropped from the
// sky around the player, handed to the world at once so it clears them up.
constexpr std::array<std::string_view, 8> kRainModels = {
    "TAXI", "ADMIRAL", "BLISTA", "ESPERANTO", "BANSHEE", "STALION", "CAVALCADE", "INGOT"};
uint64_t g_next_rain = 0;
size_t g_rain_turn = 0;

void SetRainModels(PPCContext& ctx, uint8_t* base, bool want) {
  for (const auto model : kRainModels)
    Invoke(ctx, base, want ? "REQUEST_MODEL" : "MARK_MODEL_AS_NO_LONGER_NEEDED", {ModelHash(model)});
}

void RainCars(PPCContext& ctx, uint8_t* base, const GameContext& game) {
  const uint64_t now = NowMs();
  if (now < g_next_rain)
    return;
  g_next_rain = now + 450;
  static uint32_t seed = 0x2468ACEu;
  auto next = [&] {
    seed = seed * 1664525u + 1013904223u;
    return float((seed >> 8) & 0xFFFF) / 65535.0f;
  };
  for (size_t tries = 0; tries < kRainModels.size(); ++tries) {
    const uint32_t hash = ModelHash(kRainModels[g_rain_turn++ % kRainModels.size()]);
    const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {hash});
    if (!loaded || !loaded->r0() || !CheckModel(base, hash).problem.empty())
      continue;
    const float x = game.position[0] + (next() - 0.5f) * 44.0f;
    const float y = game.position[1] + (next() - 0.5f) * 44.0f;
    const float z = game.position[2] + 25.0f + next() * 12.0f;
    const auto car = Invoke(ctx, base, "CREATE_CAR", {hash, Bits(x), Bits(y), Bits(z), 0, 1}, 4);
    if (car && car->words[0]) {
      Invoke(ctx, base, "SET_CAR_HEADING", {car->words[0], Bits(next() * 360.0f)});
      Invoke(ctx, base, "MARK_CAR_AS_NO_LONGER_NEEDED", {0}, 0, car->words[0]);
    }
    return;
  }
}

// JulioNIB's Batmobile gadgets, redone through natives (the PC script's .NET
// plugin cannot run here). With the toggle on, driving the Batmobile:
//  - L1: a missile out of the round intake in the nose: the RPG's rocket
//    with its smoke trail, flown by the physics engine (its velocity set
//    every frame, so it meets walls, cars and the ground for real, as
//    Rockstar's own scripted rockets do in TBOGT's tony8) and set off where
//    it hits. It locks on the nearest car ahead and homes in on it (marked
//    red on the radar); one every 3 seconds;
//  - d-pad left / down (hold): the twin miniguns, ahead / behind, both
//    barrels together;
//  - horn (L3): drop a spike strip across the road behind the car; the
//    tyres of any car that crosses it burst (never a Batmobile's, nor the
//    armoured Lazar's); up to two down, each gone after 30 s or 250 m;
//  - d-pad right (hold): the booster, the rear turbine flaming and roaring.
// Those buttons are hidden from the game then (no drive-by on L1, no radio,
// no horn); d-pad up stays the phone. Missiles in flight and spike strips
// carry on after the player leaves the car. The missiles, the guns and the
// strips all draw a wanted level (AddHeat). The canopy needs no toggle (see
// BatCanopy).
struct BatRocket {
  bool live = false, physics = true, armed = false;
  uint32_t object = 0, trail = 0;
  uint32_t target = 0, blip = 0;  // the car it homes on (0: straight ahead), its radar mark
  float origin[3]{};  // where it left the car
  float start[3]{}, dir[3]{}, pos[3]{};  // its line (re-drawn from where it is while it homes)
  float speed = 0, travelled = 0;
  uint64_t launched = 0;
  int frames = 0;
};
struct GadgetState {
  uint64_t next_front = 0, next_rear = 0, rocket_ready = 0, last_ms = 0;
  bool rocket_reloading = false;
  uint32_t car = 0;
  bool models_requested = false, l3_held = false, l3_chord = false, boosting = false, in_car = false;
  std::array<BatRocket, 4> rockets{};
  // v1.7.25: the horn drops a spike strip (the bomb, v1.7.7-1.7.24, went)
  struct Strip {
    bool live = false;
    float centre[3]{}, across[2]{};  // where, and its line across the road
    std::array<uint32_t, 3> pieces{};
    std::array<uint32_t, 24> burst{};  // the cars it has had (or let by)
    uint8_t burst_n = 0;
    bool laid = false;   // its pieces put down (once the model is in)
    uint32_t owner = 0;  // the Batmobile that dropped it: never its tyres
    uint64_t dropped = 0;
  };
  std::array<Strip, 2> strips{};
  uint32_t flame = 0;
  int rocket_log = 0, bomb_log = 0;
  bool guns_logged = false, boost_logged = false;
};
GadgetState g_gadget;
// From the mod's own body (car space): its nose at y 3.7, its tail at -2.7,
// the round turbine intake in the middle of the nose (its mouth at y 3.0-3.4,
// 0.5 m across, centred at z 0), the turbine's nozzle under the rear fairing.
// The missile leaves from the intake (the FBI template's collision ends at
// y 2.5, so it starts clear of the car).
constexpr float kBatGunsFront[2][3] = {{-0.55f, 3.75f, -0.15f}, {0.55f, 3.75f, -0.15f}};
constexpr float kBatGunsRear[2][3] = {{-0.55f, -2.85f, -0.10f}, {0.55f, -2.85f, -0.10f}};
constexpr float kBatLauncher[3] = {0.0f, 3.30f, 0.0f};
constexpr float kBatNozzle[3] = {0.0f, -2.45f, -0.12f};
constexpr float kBatBomb[3] = {0.0f, -4.6f, -0.2f};  // (the spike strip's middle, behind the tail)
// The strip: three pieces of IV's scrap metal laid end to end across the
// road (each 2.1 m), out 30 seconds, gone sooner when the Batmobile is far.
constexpr std::string_view kBombModel = "bm_metalscraps";
constexpr uint64_t kStripMs = 30000;
constexpr float kStripHalf = 3.4f, kStripDepth = 1.4f;
constexpr uint64_t kMissileReloadMs = 3000;  // one launch, then the launcher resets

// One volley from both barrels of a pair, along the car's length (+1 ahead,
// -1 behind), each barrel with IV's machine-gun muzzle flash. (v1.7.7 fired
// the barrels in turn and flashed only every other round: always the left.)
void BatVolley(PPCContext& ctx, uint8_t* base, uint32_t car, const float (&muzzles)[2][3], float along) {
  static uint32_t seed = 0x5EED1234u;
  AddHeat(0.08f);  // (a second's firing: two stars)
  bool fired = false, flashed = false;
  for (const auto& muzzle : muzzles) {
    const float far_point[3] = {muzzle[0], muzzle[1] + along * 300.0f, muzzle[2]};
    const auto from = CarPoint(ctx, base, car, muzzle), to = CarPoint(ctx, base, car, far_point);
    if (!from || !to)
      continue;
    float end[3];
    for (int a = 0; a < 3; ++a) {
      seed = seed * 1664525u + 1013904223u;
      end[a] = (*to)[a] + (float((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 3.0f;
    }
    Invoke(ctx, base, "FIRE_SINGLE_BULLET",
           {Bits((*from)[0]), Bits((*from)[1]), Bits((*from)[2]), Bits(end[0]), Bits(end[1]), Bits(end[2]), 35});
    flashed |= CarEffect(ctx, base, car, "muz_machine", muzzle, 1.2f);
    fired = true;
  }
  if (fired)
    GunSound();
  if (fired && !g_gadget.guns_logged) {
    g_gadget.guns_logged = true;
    REXLOG_INFO("gta4-trainer: Batmobile miniguns firing, both barrels (muzzle flash {})",
                flashed ? "started" : "not started");
  }
}

void DropTarget(PPCContext& ctx, uint8_t* base, BatRocket& r) {
  if (r.blip) {
    const auto exists = Invoke(ctx, base, "DOES_BLIP_EXIST", {r.blip});
    if (exists && exists->r0())
      Invoke(ctx, base, "REMOVE_BLIP", {r.blip});
  }
  r.blip = 0;
  r.target = 0;
}

void EndRocket(PPCContext& ctx, uint8_t* base, BatRocket& r) {
  DropTarget(ctx, base, r);
  if (r.trail)
    Invoke(ctx, base, "STOP_PTFX", {r.trail});
  r.trail = 0;
  DeleteObject(ctx, base, r.object);
  r.live = false;
}

// The physics flies it: along its line at its speed, pulled back onto the
// line (gravity's sag) a little every frame.
void SteerRocket(PPCContext& ctx, uint8_t* base, const BatRocket& r) {
  float along = 0;
  for (int a = 0; a < 3; ++a)
    along += (r.pos[a] - r.start[a]) * r.dir[a];
  float v[3];
  for (int a = 0; a < 3; ++a)
    v[a] = r.dir[a] * r.speed + (r.start[a] + r.dir[a] * along - r.pos[a]) * 6.0f;
  Invoke(ctx, base, "SET_OBJECT_INITIAL_VELOCITY", {r.object, Bits(v[0]), Bits(v[1]), Bits(v[2])});
}

// The car a missile locks on: the nearest working one in front, looked for
// along the nose's line out to 160 m (the search widening with distance).
uint32_t FindLockTarget(PPCContext& ctx, uint8_t* base, uint32_t car) {
  for (const float ahead : {12.0f, 25.0f, 40.0f, 60.0f, 85.0f, 120.0f, 160.0f}) {
    const float at_local[3] = {0.0f, kBatLauncher[1] + ahead, 0.0f};
    const auto at = CarPoint(ctx, base, car, at_local);
    if (!at)
      return 0;
    const auto near = Invoke(ctx, base, "GET_CLOSEST_CAR",
                             {Bits((*at)[0]), Bits((*at)[1]), Bits((*at)[2]), Bits(4.0f + ahead * 0.12f), 0, 70});
    if (!near || !near->r0() || near->r0() == car || !VehicleAddress(base, near->r0()))
      continue;
    const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {near->r0()});
    if (dead && dead->r0())
      continue;  // a wreck: look further on
    return near->r0();
  }
  return 0;
}

// The rocket's nose (its +Y) along a direction, level (no roll).
void PointRocket(PPCContext& ctx, uint8_t* base, const BatRocket& r) { PointObjectAlong(ctx, base, r.object, r.dir); }

// (v1.7.44: the launcher's place on the car and its sound are the caller's -
// the Beyond Batmobile fires the same missiles from its nose.)
bool LaunchRocket(PPCContext& ctx, uint8_t* base, uint32_t car, State& s, const float (&launcher)[3] = kBatLauncher,
                  void (*sound)() = LaunchSound, std::string_view who = "Batmobile") {
  BatRocket* slot = nullptr;
  for (auto& r : g_gadget.rockets)
    if (!r.live) {
      slot = &r;
      break;
    }
  if (!slot) {
    Toast(s, "Missiles reloading.", 800);
    return false;
  }
  const uint32_t hash = ModelHash(kRocketModel);
  const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {hash});
  if (!loaded || !loaded->r0()) {
    Toast(s, "Missiles loading...", 800);
    return false;
  }
  const float ahead_local[3] = {launcher[0], launcher[1] + 100.0f, launcher[2]};
  const auto start = CarPoint(ctx, base, car, launcher), ahead = CarPoint(ctx, base, car, ahead_local);
  if (!start || !ahead)
    return false;
  BatRocket& r = *slot;
  r = {};
  float len = 0;
  for (int a = 0; a < 3; ++a) {
    r.dir[a] = (*ahead)[a] - (*start)[a];
    len += r.dir[a] * r.dir[a];
    r.origin[a] = r.start[a] = r.pos[a] = (*start)[a];
  }
  len = std::sqrt(len);
  if (!(len > 1.0f))
    return false;
  for (auto& c : r.dir)
    c /= len;
  float car_speed = 0.0f;
  if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {car, 0}, 1))
    car_speed = std::max(v->f(0), 0.0f);
  r.speed = car_speed + 30.0f;  // out of the launcher, then it speeds up
  r.target = FindLockTarget(ctx, base, car);
  const auto obj = Invoke(ctx, base, "CREATE_OBJECT", {hash, Bits(r.pos[0]), Bits(r.pos[1]), Bits(r.pos[2]), 0, 1}, 4);
  if (!obj || !obj->words[0])
    return false;
  r.object = obj->words[0];
  r.live = true;
  r.launched = NowMs();
  // Rockstar's way: the vehicle's own attitude (the rocket's nose is its +Y).
  if (const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {car, 0, 0, 0, 0}, 1, 0, 4))
    Invoke(ctx, base, "SET_OBJECT_QUATERNION", {r.object, q->words[0], q->words[1], q->words[2], q->words[3]});
  Invoke(ctx, base, "SET_OBJECT_INVINCIBLE", {r.object, 1});
  Invoke(ctx, base, "SET_OBJECT_COLLISION", {r.object, 1});
  SteerRocket(ctx, base, r);
  // Locked: the target shows red on the radar until the missile is done.
  if (r.target)
    if (const auto b = Invoke(ctx, base, "ADD_BLIP_FOR_CAR", {r.target, 0}, 1); b && b->r0()) {
      r.blip = b->r0();
      Invoke(ctx, base, "CHANGE_BLIP_COLOUR", {r.blip, 1});
    }
  if (const auto trail = Invoke(ctx, base, "START_PTFX_ON_OBJ", {0, r.object, 0, 0, 0, 0, 0, 0, Bits(1.0f)}, -1, 0,
                                1, 0, "weap_rocket"))
    r.trail = trail->r0();
  // The launch flash out of the intake.
  const float mouth[3] = {launcher[0], launcher[1] + 0.3f, launcher[2]};
  CarEffect(ctx, base, car, "muz_machine", mouth, 2.5f);
  sound();
  Toast(s, r.target ? "Missile locked on the car ahead." : "Missile away (nothing to lock on).", 1100);
  if (g_gadget.rocket_log < 12) {
    ++g_gadget.rocket_log;
    REXLOG_INFO("gta4-trainer: {} missile away at {:.0f} m/s (object {:08X}, trail {:08X}, target {:08X})",
                std::string(who), r.speed, r.object, r.trail, r.target);
  }
  return true;
}

// Every frame while one flies (also after the player has left the car).
void UpdateRockets(PPCContext& ctx, uint8_t* base, float dt) {
  const uint64_t now = NowMs();
  for (auto& r : g_gadget.rockets) {
    if (!r.live)
      continue;
    ++r.frames;
    r.speed = std::min(r.speed + 140.0f * dt, r.target ? 75.0f : 95.0f);
    // Homing, once it is out of the launcher: the line turns towards the locked
    // car (3 rad/s at most) from where the missile is, its nose with it; a red
    // light hangs over the target.
    std::optional<std::array<float, 3>> aim;
    if (r.target && r.frames > 3) {
      if (VehicleAddress(base, r.target))
        aim = CarPosition(ctx, base, r.target);
      if (!aim) {
        DropTarget(ctx, base, r);  // gone: on straight
      } else {
        (*aim)[2] += 0.3f;
        float want[3], len = 0;
        for (int a = 0; a < 3; ++a) {
          want[a] = (*aim)[a] - r.pos[a];
          len += want[a] * want[a];
        }
        len = std::sqrt(len);
        if (len > 0.5f) {
          float dot = 0;
          for (int a = 0; a < 3; ++a) {
            want[a] /= len;
            dot += want[a] * r.dir[a];
          }
          const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
          const float step = std::min(1.0f, 3.0f * dt / std::max(angle, 1e-4f));
          float n = 0;
          for (int a = 0; a < 3; ++a) {
            r.dir[a] += (want[a] - r.dir[a]) * step;
            n += r.dir[a] * r.dir[a];
          }
          n = std::sqrt(n);
          if (n > 1e-4f)
            for (int a = 0; a < 3; ++a) {
              r.dir[a] /= n;
              r.start[a] = r.pos[a];  // the line now runs from here
            }
          PointRocket(ctx, base, r);
        }
        Invoke(ctx, base, "DRAW_LIGHT_WITH_RANGE",
               {Bits((*aim)[0]), Bits((*aim)[1]), Bits((*aim)[2] + 1.3f), 255, 0, 0, Bits(4.0f), Bits(6.0f)});
      }
    }
    if (r.physics) {
      if (const auto at = Invoke(ctx, base, "GET_OBJECT_COORDINATES", {r.object, 0, 0, 0}, 1, 0, 3))
        for (int a = 0; a < 3; ++a)
          r.pos[a] = at->f(a);
      float moved = 0;
      for (int a = 0; a < 3; ++a)
        moved += (r.pos[a] - r.origin[a]) * (r.pos[a] - r.origin[a]);
      // The physics did not take it (frozen, asleep): flown by hand from here,
      // still meeting the ground and cars, not walls.
      if (r.frames == 6 && moved < 4.0f) {
        r.physics = false;
        Invoke(ctx, base, "FREEZE_OBJECT_POSITION", {r.object, 1});
        Invoke(ctx, base, "SET_OBJECT_COLLISION", {r.object, 0});
        REXLOG_INFO("gta4-trainer: Batmobile missile did not move under physics; flown by hand");
      }
    }
    if (r.physics) {
      SteerRocket(ctx, base, r);
    } else {
      for (int a = 0; a < 3; ++a)
        r.pos[a] += r.dir[a] * r.speed * dt;
      Invoke(ctx, base, "SET_OBJECT_COORDINATES", {r.object, Bits(r.pos[0]), Bits(r.pos[1]), Bits(r.pos[2])});
    }
    float d2 = 0;
    for (int a = 0; a < 3; ++a)
      d2 += (r.pos[a] - r.origin[a]) * (r.pos[a] - r.origin[a]);
    r.travelled = std::sqrt(d2);
    const char* hit = nullptr;
    if (aim) {
      float t2 = 0;
      for (int a = 0; a < 3; ++a)
        t2 += (r.pos[a] - (*aim)[a]) * (r.pos[a] - (*aim)[a]);
      if (t2 < 2.6f * 2.6f)
        hit = "its target";
    }
    if (!hit && r.physics && !r.armed && r.travelled > 3.0f) {
      // Collisions are recorded once it is clear of the car.
      Invoke(ctx, base, "SET_OBJECT_RECORDS_COLLISIONS", {r.object, 1});
      r.armed = true;
    } else if (!hit && r.physics && r.armed) {
      const auto c = Invoke(ctx, base, "HAS_OBJECT_COLLIDED_WITH_ANYTHING", {r.object});
      if (c && c->r0())
        hit = "something";
    }
    if (!hit) {
      const auto gz = GroundZ(ctx, base, r.pos[0], r.pos[1], r.pos[2] + 2.0f);
      if (gz && r.pos[2] <= *gz + 0.15f)
        hit = "the ground";
    }
    if (!hit && r.travelled > 6.0f) {
      const auto near = Invoke(ctx, base, "GET_CLOSEST_CAR",
                               {Bits(r.pos[0]), Bits(r.pos[1]), Bits(r.pos[2]), Bits(3.5f), 0, 70});
      if (near && near->r0() && near->r0() != g_gadget.car)
        hit = "a vehicle";
    }
    if (!hit && (r.travelled > 320.0f || now - r.launched > 7000 || !std::isfinite(r.pos[2])))
      hit = "the end of its range";
    if (!hit)
      continue;
    // A target hit goes off on the car itself.
    const bool on_target = aim && hit == std::string_view("its target");
    if (std::isfinite(r.pos[2]))
      Explode(ctx, base, on_target ? aim->data() : r.pos, on_target ? 2.0f : 1.5f);
    if (g_gadget.rocket_log < 12) {
      ++g_gadget.rocket_log;
      REXLOG_INFO("gta4-trainer: Batmobile missile hit {} after {:.0f} m ({}{})", hit, r.travelled,
                  r.physics ? "physics" : "by hand", r.target ? ", homing" : "");
    }
    EndRocket(ctx, base, r);
  }
}

void ClearStrip(PPCContext& ctx, uint8_t* base, GadgetState::Strip& strip) {
  for (auto& piece : strip.pieces)
    DeleteObject(ctx, base, piece);
  strip = {};
}

// The strip's three pieces, end to end along its line, each on the ground
// under it; none until the model is in (then the next frame's look).
int LayStrip(PPCContext& ctx, uint8_t* base, GadgetState::Strip& strip) {
  const uint32_t hash = ModelHash(kBombModel);
  const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {hash});
  if (!loaded || !loaded->r0()) {
    Invoke(ctx, base, "REQUEST_MODEL", {hash});
    return 0;
  }
  strip.laid = true;
  int made = 0;
  // Each piece's length (its Y) along the strip: turned about z so.
  const float yaw = std::atan2(-strip.across[0], strip.across[1]);
  const Quat q = QAxis(0, 0, 1, yaw);
  for (int k = 0; k < 3; ++k) {
    const float off = (float(k) - 1.0f) * 2.1f;
    float at[3] = {strip.centre[0] + strip.across[0] * off, strip.centre[1] + strip.across[1] * off, strip.centre[2]};
    if (const auto gz = GroundZ(ctx, base, at[0], at[1], at[2] + 1.5f); gz && std::fabs(*gz - at[2]) < 1.5f)
      at[2] = *gz;
    const auto obj = Invoke(ctx, base, "CREATE_OBJECT", {hash, Bits(at[0]), Bits(at[1]), Bits(at[2] + 0.02f), 0, 1}, 4);
    if (!obj || !obj->words[0])
      continue;
    const uint32_t o = obj->words[0];
    strip.pieces[size_t(k)] = o;
    Invoke(ctx, base, "SET_OBJECT_COLLISION", {o, 0});
    Invoke(ctx, base, "FREEZE_OBJECT_POSITION", {o, 1});
    Invoke(ctx, base, "SET_OBJECT_QUATERNION", {o, Bits(q.x), Bits(q.y), Bits(q.z), Bits(q.w)});
    ++made;
  }
  return made;
}

// The horn: a spike strip dropped behind the car, across the road.
void PlantBomb(PPCContext& ctx, uint8_t* base, uint32_t car, State& s) {
  const auto p = CarPoint(ctx, base, car, kBatBomb);
  const float side_local[3] = {1.0f, kBatBomb[1], kBatBomb[2]};
  const auto side = CarPoint(ctx, base, car, side_local);
  if (!p || !side)
    return;
  float across[2] = {(*side)[0] - (*p)[0], (*side)[1] - (*p)[1]};
  const float len = std::sqrt(across[0] * across[0] + across[1] * across[1]);
  if (!(len > 0.1f))
    return;
  across[0] /= len;
  across[1] /= len;
  // The oldest strip makes way.
  auto& strip = *std::min_element(g_gadget.strips.begin(), g_gadget.strips.end(),
                                  [](const auto& a, const auto& b) { return a.dropped < b.dropped; });
  ClearStrip(ctx, base, strip);
  strip.live = true;
  strip.owner = car;
  strip.dropped = NowMs();
  for (int a = 0; a < 3; ++a)
    strip.centre[a] = (*p)[a];
  strip.across[0] = across[0];
  strip.across[1] = across[1];
  if (const auto gz = GroundZ(ctx, base, (*p)[0], (*p)[1], (*p)[2] + 1.5f))
    strip.centre[2] = *gz;
  const int made = LayStrip(ctx, base, strip);
  Toast(s, "Spike strip down: tyres that cross it burst. Gone in 30 s.", 2200);
  if (g_gadget.bomb_log < 12) {
    ++g_gadget.bomb_log;
    REXLOG_INFO("gta4-trainer: Batmobile spike strip at {:.1f}, {:.1f}, {:.1f} ({} pieces)", strip.centre[0],
                strip.centre[1], strip.centre[2], made);
  }
}

// Every frame while a strip is down: a car over it (not the Batmobile)
// has its tyres burst, once; the strip goes after 30 s, or when the
// Batmobile is 250 m off, or with the gadgets.
void SetOffBomb(PPCContext& ctx, uint8_t* base, bool quietly) {
  (void)quietly;  // (the gadgets off: every strip taken up)
  for (auto& strip : g_gadget.strips)
    if (strip.live)
      ClearStrip(ctx, base, strip);
}
void BombLight(PPCContext& ctx, uint8_t* base) {
  const uint64_t now = NowMs();
  for (auto& strip : g_gadget.strips) {
    if (!strip.live)
      continue;
    bool far = false;
    if (g_gadget.car && VehicleAddress(base, g_gadget.car))
      if (const auto at = CarPosition(ctx, base, g_gadget.car)) {
        const float dx = (*at)[0] - strip.centre[0], dy = (*at)[1] - strip.centre[1];
        far = dx * dx + dy * dy > 250.0f * 250.0f;
      }
    if (now - strip.dropped > kStripMs || far) {
      ClearStrip(ctx, base, strip);
      continue;
    }
    if (!strip.laid)
      LayStrip(ctx, base, strip);
    ForEachVehicle(base, [&](uint32_t handle, uint32_t vehicle) {
      // Never the Batmobile's own tyres (reversing over it, or in another).
      if (handle == g_gadget.car || handle == strip.owner)
        return;
      float p[3];
      if (!EntityPosition(base, vehicle, p))
        return;
      const float dx = p[0] - strip.centre[0], dy = p[1] - strip.centre[1], dz = p[2] - strip.centre[2];
      if (std::fabs(dz) > 2.5f || dx * dx + dy * dy > 30.0f)
        return;
      const float along = dx * strip.across[0] + dy * strip.across[1];
      const float through = -dx * strip.across[1] + dy * strip.across[0];
      if (std::fabs(along) > kStripHalf || std::fabs(through) > kStripDepth)
        return;
      if (std::find(strip.burst.begin(), strip.burst.end(), handle) != strip.burst.end())
        return;
      strip.burst[strip.burst_n++ % strip.burst.size()] = handle;  // (once each)
      // Any Batmobile goes over it unharmed, and so does the armoured Lazar;
      // only road vehicles have tyres to burst (a bike its two: 0 and 4).
      static const uint32_t kBatmobile = ModelHash("BATMOBILE"), kLazar = ModelHash("LAZAR3");
      const auto m = Invoke(ctx, base, "GET_CAR_MODEL", {handle, 0}, 1);
      if (!m || m->words[0] == kBatmobile || m->words[0] == kLazar ||
          ModelKind(ctx, base, m->words[0]) != Kind::kRoad)
        return;
      const auto bike = Invoke(ctx, base, "IS_THIS_MODEL_A_BIKE", {m->words[0]});
      if (bike && bike->r0()) {
        for (const uint32_t tyre : {0u, 4u})
          Invoke(ctx, base, "BURST_CAR_TYRE", {handle, tyre});
      } else {
        for (const uint32_t tyre : {0u, 1u, 4u, 5u})
          Invoke(ctx, base, "BURST_CAR_TYRE", {handle, tyre});
      }
      AddHeat(0.5f);  // (two cars' tyres: two stars)
      if (g_gadget.bomb_log < 12) {
        ++g_gadget.bomb_log;
        REXLOG_INFO("gta4-trainer: Batmobile spike strip burst the tyres of car {:08X}", handle);
      }
    });
  }
}

// The booster's flame: IV's rocket flame out of the turbine.
void BatFlame(PPCContext& ctx, uint8_t* base, uint32_t car, bool on) {
  if (!on) {
    if (g_gadget.flame)
      Invoke(ctx, base, "STOP_PTFX", {g_gadget.flame});
    g_gadget.flame = 0;
    return;
  }
  if (g_gadget.flame)
    return;
  const auto r = Invoke(ctx, base, "START_PTFX_ON_VEH",
                        {0, car, Bits(kBatNozzle[0]), Bits(kBatNozzle[1]), Bits(kBatNozzle[2]), 0, 0, 0, Bits(1.6f)},
                        -1, 0, 1, 0, "weap_rocket");
  g_gadget.flame = r ? r->r0() : 0;
  if (!g_gadget.boost_logged) {
    g_gadget.boost_logged = true;
    REXLOG_INFO("gta4-trainer: Batmobile booster flame {:08X}", g_gadget.flame);
  }
}

// Whether the gadgets want every frame (a missile flying, a bomb's light).
bool GadgetsBusy() {
  return std::any_of(g_gadget.strips.begin(), g_gadget.strips.end(), [](const auto& t) { return t.live; }) ||
         std::any_of(g_gadget.rockets.begin(), g_gadget.rockets.end(), [](const BatRocket& r) { return r.live; });
}

uint32_t Gadgets(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint16_t buttons,
                 uint16_t previous) {
  const uint64_t now = NowMs();
  const float dt = g_gadget.last_ms ? std::clamp(float(now - g_gadget.last_ms) / 1000.0f, 0.0f, 0.05f) : 0.0f;
  g_gadget.last_ms = now;
  UpdateRockets(ctx, base, dt);
  BombLight(ctx, base);
  static const uint32_t kBatmobile = ModelHash("BATMOBILE");
  uint32_t car = 0;
  if (s.gadgets) {
    const auto& vehicle = PlayerVehicle(ctx, base, game);
    if (vehicle.car && vehicle.model == kBatmobile)
      car = *vehicle.car;
  }
  if (!car) {
    if (g_gadget.in_car) {
      BatFlame(ctx, base, g_gadget.car, false);
      if (g_gadget.boosting)
        JetEngineSound(0.0f, 0.0f, 0.9f);
      g_gadget.boosting = false;
    }
    g_gadget.in_car = false;
    if (!s.gadgets)
      SetOffBomb(ctx, base, true);  // the toggle off: the strips taken up
    if (g_gadget.models_requested && !GadgetsBusy()) {
      Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {ModelHash(kBombModel)});
      Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {ModelHash(kRocketModel)});
      g_gadget.models_requested = false;
    }
    return 0;
  }
  if (!g_gadget.in_car || g_gadget.car != car) {
    g_gadget.l3_held = false;  // no edge from a button held while getting in
    previous = buttons;
  }
  g_gadget.in_car = true;
  g_gadget.car = car;
  if (!g_gadget.models_requested) {
    Invoke(ctx, base, "REQUEST_MODEL", {ModelHash(kBombModel)});
    Invoke(ctx, base, "REQUEST_MODEL", {ModelHash(kRocketModel)});
    g_gadget.models_requested = true;
  }
  // Missile: L1, one per press; the launcher then resets for 3 s.
  if ((buttons & X_INPUT_GAMEPAD_LEFT_SHOULDER) && !(previous & X_INPUT_GAMEPAD_LEFT_SHOULDER)) {
    if (now < g_gadget.rocket_ready) {
      Toast(s, "Missile reloading: " + std::to_string((g_gadget.rocket_ready - now + 999) / 1000) + " s", 900);
    } else if (LaunchRocket(ctx, base, car, s)) {
      g_gadget.rocket_ready = now + kMissileReloadMs;
      g_gadget.rocket_reloading = true;
    }
  }
  if (g_gadget.rocket_reloading && now >= g_gadget.rocket_ready) {
    g_gadget.rocket_reloading = false;
    Toast(s, "Missile ready.", 900);
  }
  // Miniguns: hold d-pad left (ahead) or down (behind).
  if ((buttons & X_INPUT_GAMEPAD_DPAD_LEFT) && now >= g_gadget.next_front) {
    g_gadget.next_front = now + 70;
    BatVolley(ctx, base, car, kBatGunsFront, 1.0f);
  }
  if ((buttons & X_INPUT_GAMEPAD_DPAD_DOWN) && now >= g_gadget.next_rear) {
    g_gadget.next_rear = now + 70;
    BatVolley(ctx, base, car, kBatGunsRear, -1.0f);
  }
  // The spike strip on the horn (L3). Acted on release, so L3 + R3 (the
  // menu) never drops one.
  const bool l3 = buttons & X_INPUT_GAMEPAD_LEFT_THUMB, r3 = buttons & X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (l3 && !(previous & X_INPUT_GAMEPAD_LEFT_THUMB)) {
    g_gadget.l3_held = true;
    g_gadget.l3_chord = false;
  }
  if (l3 && r3)
    g_gadget.l3_chord = true;
  const bool horn_tap = !l3 && g_gadget.l3_held && !g_gadget.l3_chord && !s.open;
  if (!l3)
    g_gadget.l3_held = false;
  if (horn_tap)
    PlantBomb(ctx, base, car, s);  // (a spike strip; a third takes up the oldest)
  // Booster: hold d-pad right; the turbine flames and roars.
  const bool boost = buttons & X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (boost) {
    float speed = 0.0f;
    if (const auto current = Invoke(ctx, base, "GET_CAR_SPEED", {car, 0}, 1))
      speed = current->f(0);
    const float target = std::min(speed + 3.0f, 55.0f);
    Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {car, Bits(target)});
  }
  // (The roar is the Batmobile's own engine's: CarEngine reads `boosting`.)
  BatFlame(ctx, base, car, boost);
  g_gadget.boosting = boost;
  return X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT |
         X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_LEFT_THUMB;
}

// Canopies, the doors of the cars that have none: the converter hangs the
// canopy on a template bone (interiorlight), hinged at its rear edge. Getting
// in, the canopy pops open, the player is put in the seat and it closes;
// getting out (Y, kept from the game inside) the same the other way, the
// player set down beside the cockpit. The Batmobile's (frame and glass, on
// the FBI template) pops up; the F-16's bubble rises more slowly, as a jet's
// does. Both open only at a standstill on the ground (rolling or in the air
// Y is the game's: the player jumps out). No toggle. The cockpits under them
// (seats, dash, the jet's ejection seat and panel) are the mods' own.
struct CanopyState {
  // kOpeningRider: up for someone else getting in, the player in his seat
  // (or with him: kOpeningIn); kRiderOut: up while a passenger climbs out.
  enum Phase : int { kShut, kOpeningIn, kClosingIn, kOpeningOut, kLeaving, kClosingOut, kOpeningRider, kRiderOut };
  Phase phase = kShut;
  uint32_t car = 0;   // the vehicle being got into or out of
  float open = 0.0f;  // 0 shut .. 1 open
  float hold = 0.0f;  // a beat with it up
  uint64_t next_probe = 0, last_ms = 0, started = 0;
  bool in_car = false, sounded = false;
  bool hidden = false;          // the player hidden while he climbs out
  uint64_t leaving_since = 0;
  uint16_t prev_buttons = 0;
  uint32_t info = 0;  // the model whose pivot bone is known
  int32_t bone = -1, bone2 = -1;
  int log = 0;
  // Where the player was set down, watched for a few seconds: a fall
  // through the world puts them back there.
  float exit[3]{};
  uint64_t exit_at = 0, unfreeze_at = 0;
  int catches = 0, watch_log = 0, exits = 0;
  // Passengers (the Batmobile's): one stopped on his way in, put in the
  // passenger seat once the canopy is up; the one last seen sitting there;
  // one climbing out, hidden until he is out and set down beside.
  uint32_t rider = 0, seated = 0, leaver = 0;
  uint64_t leaver_since = 0, next_rider_look = 0;
  uint64_t y_seen = 0;  // the last Y press the quick way in has acted on
  int rider_log = 0;
};
struct CanopySpec {
  std::string_view model, label, bone;
  float degrees;  // open: the front edge up, about the rear edge
  float open_seconds, close_seconds;
  float exit[3];  // where the player is set down, in the vehicle's own space
  int slot, entry;  // where the trainer keeps the pose (PoseBone)
  bool ground_only;  // opens only standing still on the ground
  bool riders;       // a passenger seat: others get in and out through it too
  // The way in turns about the car's X axis (0) or its length (1: the
  // Lazar's side ramps, folding down); a second one opening with it (the
  // other ramp, at entry + 1); its sound (the Batmobile's hiss by default).
  int axis = 0;
  std::string_view bone2 = {};
  float degrees2 = 0.0f;
  void (*sound)(bool opening) = nullptr;
  // Y within a few metres opens it at once and puts the player in (the
  // jets: the game's walk to a door high up the Batwing's side took ages).
  bool quick_entry = false;
};
constexpr CanopySpec kBatCanopy{"BATMOBILE", "Batmobile", "interiorlight", 60.0f, 0.45f, 0.7f,
                                {-2.1f, 0.2f, 0.0f}, kPoseBatmobile, 0, true, true};
constexpr CanopySpec kJetCanopy{"F16", "F-16", "interiorlight", 38.0f, 0.75f, 1.0f,
                                {-2.3f, 4.3f, 0.0f}, kPoseF16, 3, true, false, 0, {}, 0.0f, nullptr, true};
// The Batwing's canopy: cut out of its one-piece body by the converter
// (from the rear of its side windows forward, above the sills) and hung on
// interiorlight at its rear edge; it opens 40 degrees on its own actuator.
void BatwingCanopySound(bool opening);  // (with the jet's sounds)
constexpr CanopySpec kBatwingCanopy{"BATWING", "Batwing", "interiorlight", 40.0f, 0.8f, 1.1f, {-3.0f, -0.4f, 0.0f},
                                    kPoseBatwing, 3, true, true, 0, {}, 0.0f, BatwingCanopySound, true};
CanopyState g_canopy, g_jet_canopy, g_batwing_canopy;
// The Lazar 3's side ramps (the PC model's doors, hinged at their bottom
// edge, folding out and down: 105 degrees there): the player in and out on
// the left, a passenger on the right, both ramps moving together on their
// hydraulics, slower than a canopy.
void LazarRampSound(bool opening);  // (with the Lazar's turret, below)
// v1.7.24: they open 40 degrees, not 100, and quicker: the model has no
// inside, and the open ramps showed the hollow shell; Y beside it puts the
// player in at once, as the jets.
constexpr CanopySpec kLazarRamps{"LAZAR3", "Lazar 3", "extra_6", -40.0f, 0.8f, 0.9f, {-3.0f, 0.75f, 0.0f},
                                 kPoseLazar, 2, true, true, 1, "interiorlight", 40.0f, LazarRampSound, true};
CanopyState g_lazar_ramps;
// The Beyond Batmobile's canopy (v1.7.44): the bubble over its spine, bound
// by its builder (tools/car/beyond_build.py) to the template's interiorlight
// bone at the bubble's rear edge (0, -0.75, 0.74 in the car's space); it
// rises 34 degrees. The car hovers, so it is never "on the ground": the
// flight code (BeyondTick) brings it down to its hover height and to a stop
// before Y reaches the canopy, and takes Y meanwhile. The player is set down
// to its left, clear of the wing. Y beside it puts the player in at once.
void BeyondCanopySound(bool opening);  // (the Beyond Batmobile's sounds, below)
constexpr CanopySpec kBeyondCanopy{"BEYOND", "Beyond Batmobile", "interiorlight", 34.0f, 0.6f, 0.8f,
                                   {-2.3f, 0.3f, 0.0f}, kPoseBeyond, 0, false, false, 0, {}, 0.0f, BeyondCanopySound,
                                   true};
CanopyState g_beyond_canopy;

void CanopySound(bool opening);  // (the Batmobile's sound section, below)

bool PoseCanopy(PPCContext& ctx, uint8_t* base, const CanopySpec& spec, CanopyState& c, uint32_t car, float open) {
  const uint32_t model = ModelHash(spec.model);
  const uint32_t vehicle = VehicleAddress(base, car);
  const uint32_t info = vehicle ? FindModelInfo(base, model) : 0;
  if (!info || !VehicleIsModel(base, vehicle, info))
    return false;
  if (c.info != info) {
    c.info = info;
    c.bone = FindBone(ctx, base, vehicle, spec.bone);
    c.bone2 = spec.bone2.empty() ? -1 : FindBone(ctx, base, vehicle, spec.bone2);
    REXLOG_INFO("gta4-trainer: {} canopy on bone {} ({}){}", std::string(spec.label), c.bone, std::string(spec.bone),
                spec.bone2.empty() ? std::string() : fmt::format(", and bone {} ({})", c.bone2, std::string(spec.bone2)));
  }
  if (c.bone < 0)
    return false;
  const float t = std::clamp(open, 0.0f, 1.0f), eased = t * t * (3.0f - 2.0f * t);
  if (c.bone2 >= 0)
    PoseBone(ctx, base, vehicle, info, spec.slot, spec.entry + 1, c.bone2, spec.axis, spec.degrees2 * eased, 1.0f);
  return PoseBone(ctx, base, vehicle, info, spec.slot, spec.entry, c.bone, spec.axis, spec.degrees * eased, 1.0f);
}

void PlayCanopySound(const CanopySpec& spec, bool opening) { (spec.sound ? spec.sound : CanopySound)(opening); }

bool CanopyBusy(const CanopyState& c) {
  return c.phase != CanopyState::kShut || c.in_car || c.exit_at != 0 || c.leaver != 0;
}

// The player, out of the seat and on his feet (the game's own exit took him
// out), put on the ground beside the cockpit. SET_CHAR_COORDINATES with the
// ground's height a metre up (what the trainer's teleports always gave it).
// The ground is looked for from above the vehicle's roof (under awnings
// too); with no plausible answer (another level, a roof) he stays where the
// game set him down.
void PlaceBeside(PPCContext& ctx, uint8_t* base, const GameContext& game, const CanopySpec& spec, CanopyState& c,
                 uint64_t now) {
  const std::string label(spec.label);
  const auto p = CarPoint(ctx, base, c.car, spec.exit);
  const auto at = CarPosition(ctx, base, c.car);
  std::optional<float> gz;
  if (p && at) {
    gz = GroundZ(ctx, base, (*p)[0], (*p)[1], (*at)[2] + 1.5f);
    if (gz && (*gz > (*at)[2] + 0.8f || *gz < (*at)[2] - 3.0f))
      gz.reset();
  }
  if (p && gz) {
    Invoke(ctx, base, "SET_CHAR_COORDINATES", {game.ped, Bits((*p)[0]), Bits((*p)[1]), Bits(*gz + 1.0f)});
    c.exit[0] = (*p)[0];
    c.exit[1] = (*p)[1];
    c.exit[2] = *gz;
  } else {
    for (int a = 0; a < 3; ++a)
      c.exit[a] = game.position[a];
    c.exit[2] -= 1.0f;  // (a standing ped's origin rides about a metre up)
  }
  c.exit_at = now;
  c.catches = 0;
  c.watch_log = 0;
  ++c.exits;
  REXLOG_INFO("gta4-trainer: {}: player out of the seat after {} ms; {} ({:.1f}, {:.1f}, ground {:.2f})", label,
              now - c.leaving_since, p && gz ? "put beside the cockpit" : "left where the game set him down",
              c.exit[0], c.exit[1], c.exit[2]);
}

// Passengers through a canopy (the Batmobile's). The game would walk them
// to a door that isn't there and play its animation through the body; here
// one on his way in is stopped there, the canopy rises and he is put in the
// passenger seat, as the player is put in his; one getting out is hidden
// while he climbs out, the canopy up, and set down beside the passenger side
// once he is out.
bool GettingInto(PPCContext& ctx, uint8_t* base, uint32_t ped, uint32_t car) {
  if (!ped)
    return false;
  const auto getting = Invoke(ctx, base, "IS_CHAR_GETTING_IN_TO_A_CAR", {ped});
  if (!getting || !getting->r0())
    return false;
  const auto target = Invoke(ctx, base, "GET_CAR_CHAR_IS_USING", {ped, 0}, 1);
  return target && target->words[0] == car;
}

// Someone other than the player on his way into the car: the player's group
// first (a friend, a mission's buddy following Niko), then whoever is
// nearest the passenger side (a mission's passenger sent to get in). 0: no one.
uint32_t FindRider(PPCContext& ctx, uint8_t* base, const GameContext& game, uint32_t car) {
  if (const auto g = Invoke(ctx, base, "GET_PLAYER_GROUP", {game.player, 0}, 1); g && g->words[0]) {
    const uint32_t group = g->words[0];
    if (const auto size = Invoke(ctx, base, "GET_GROUP_SIZE", {group, 0, 0}, 1, 0, 2)) {
      const uint32_t count = std::min<uint32_t>(size->words[1], 8);
      for (uint32_t i = 0; i < count; ++i) {
        const auto m = Invoke(ctx, base, "GET_GROUP_MEMBER", {group, i, 0}, 2);
        if (m && m->words[0] && m->words[0] != game.ped && GettingInto(ctx, base, m->words[0], car))
          return m->words[0];
      }
    }
  }
  // (Not while wanted: a policeman reaching for the driver is not a passenger.)
  if (const auto wanted = Invoke(ctx, base, "IS_WANTED_LEVEL_GREATER", {game.player, 0}); !wanted || wanted->r0())
    return 0;
  const float side[3] = {2.0f, 0.0f, 0.0f};
  if (const auto at = CarPoint(ctx, base, car, side)) {
    // (it returns whether it found one: the ped goes in the next word)
    const auto near = Invoke(ctx, base, "GET_CLOSEST_CHAR",
                             {Bits((*at)[0]), Bits((*at)[1]), Bits((*at)[2]), Bits(4.0f), 1, 1, 0}, 6, 0, 1, -1, {},
                             1);
    if (near && near->words[0] && near->words[1] && near->words[1] != game.ped &&
        GettingInto(ctx, base, near->words[1], car))
      return near->words[1];
  }
  return 0;
}

bool SeatFree(PPCContext& ctx, uint8_t* base, uint32_t car, uint32_t seat) {
  const auto free = Invoke(ctx, base, "IS_CAR_PASSENGER_SEAT_FREE", {car, seat});
  return free && free->r0();
}

// The rider into the passenger seat, now that the canopy is up.
void SeatRider(PPCContext& ctx, uint8_t* base, const CanopySpec& spec, CanopyState& c) {
  const uint32_t rider = c.rider;
  c.rider = 0;
  if (!rider)
    return;
  const auto dead = Invoke(ctx, base, "IS_CHAR_DEAD", {rider});
  if ((dead && dead->r0()) || !SeatFree(ctx, base, c.car, 0) || !NativeKnown("WARP_CHAR_INTO_CAR_AS_PASSENGER"))
    return;
  Invoke(ctx, base, "WARP_CHAR_INTO_CAR_AS_PASSENGER", {rider, c.car, 0});
  if (c.rider_log < 12) {
    ++c.rider_log;
    REXLOG_INFO("gta4-trainer: {}: passenger {:08X} in through the canopy", std::string(spec.label), rider);
  }
}

// One climbing out: once out, set down beside the passenger side (the
// player's spot mirrored), shown again.
void SetDownLeaver(PPCContext& ctx, uint8_t* base, const CanopySpec& spec, CanopyState& c, bool out) {
  const uint32_t ped = c.leaver;
  c.leaver = 0;
  if (out) {
    const float side[3] = {-spec.exit[0], spec.exit[1], 0.0f};
    const auto p = CarPoint(ctx, base, c.car, side);
    const auto at = CarPosition(ctx, base, c.car);
    if (p && at)
      if (const auto gz = GroundZ(ctx, base, (*p)[0], (*p)[1], (*at)[2] + 1.5f);
          gz && *gz <= (*at)[2] + 0.8f && *gz >= (*at)[2] - 3.0f)
        Invoke(ctx, base, "SET_CHAR_COORDINATES", {ped, Bits((*p)[0]), Bits((*p)[1]), Bits(*gz + 1.0f)});
  }
  Invoke(ctx, base, "SET_CHAR_VISIBLE", {ped, 1});
  if (c.rider_log < 12) {
    ++c.rider_log;
    REXLOG_INFO("gta4-trainer: {}: passenger {:08X} {}", std::string(spec.label), ped,
                out ? "out through the canopy, set down beside" : "did not get out in 4 s: shown again");
  }
}

// Every frame the canopy is watched: the passenger seat (his getting out,
// seen at once), the one climbing out.
void Passengers(PPCContext& ctx, uint8_t* base, const CanopySpec& spec, CanopyState& c, uint64_t now) {
  if (c.leaver) {
    const auto in_car = Invoke(ctx, base, "IS_CHAR_IN_ANY_CAR", {c.leaver});
    const bool out = in_car && !in_car->r0();
    if (out || now - c.leaver_since > 4000)
      SetDownLeaver(ctx, base, spec, c, out);
  }
  if (!c.car || c.leaver || !(c.in_car || c.phase != CanopyState::kShut))
    return;
  const auto p = Invoke(ctx, base, "GET_CHAR_IN_CAR_PASSENGER_SEAT", {c.car, 0, 0}, 2);
  const uint32_t ped = p ? p->words[0] : 0;
  if (!ped) {
    c.seated = 0;
    return;
  }
  const auto sitting = Invoke(ctx, base, "IS_CHAR_SITTING_IN_CAR", {ped, c.car});
  if (sitting && sitting->r0()) {
    c.seated = ped;
    return;
  }
  if (ped != c.seated)
    return;  // (on his way in)
  // He sat there and is on his way out (cars only let passengers out at a
  // standstill: anything else is not a getting out).
  float speed = 0.0f;
  if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {c.car, 0}, 1))
    speed = std::fabs(v->f(0));
  if (speed > 2.5f)
    return;
  Invoke(ctx, base, "SET_CHAR_VISIBLE", {ped, 0});
  c.leaver = ped;
  c.leaver_since = now;
  c.seated = 0;
  if (c.phase == CanopyState::kShut) {
    c.phase = CanopyState::kRiderOut;
    c.open = c.hold = 0.0f;
    c.started = now;
    c.sounded = false;
  }
}

// Returns the buttons it keeps from the game: Y while the player is in the
// vehicle and the canopy is the way out.
uint32_t Canopy(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint16_t buttons,
                const CanopySpec& spec, CanopyState& c) {
  const uint64_t now = NowMs();
  const float dt = c.last_ms ? std::clamp(float(now - c.last_ms) / 1000.0f, 0.0f, 0.05f) : 0.0f;
  c.last_ms = now;
  const uint16_t prev = c.prev_buttons;
  c.prev_buttons = buttons;
  const uint32_t model = ModelHash(spec.model);
  const std::string label(spec.label);
  uint32_t keep = c.in_car ? uint32_t(X_INPUT_GAMEPAD_Y) : 0u;
  // Just set down: watched for 4 seconds. Dropping through the world, the
  // player is held still on the spot (collision on) and let go a moment
  // later; after four tries, back in the seat rather than lost under the map.
  if (c.unfreeze_at && now >= c.unfreeze_at) {
    c.unfreeze_at = 0;
    Invoke(ctx, base, "FREEZE_CHAR_POSITION", {game.ped, 0});
  }
  if (c.hidden && c.phase != CanopyState::kLeaving) {
    Invoke(ctx, base, "SET_CHAR_VISIBLE", {game.ped, 1});
    c.hidden = false;
  }
  if (c.exit_at) {
    if (c.exits <= 3 && c.watch_log < 12 && now - c.exit_at >= 250 * uint64_t(c.watch_log)) {
      ++c.watch_log;
      REXLOG_INFO("gta4-trainer: {}: {} ms after setting down the player is at z {:.2f} (ground {:.2f})", label,
                  now - c.exit_at, game.position[2], c.exit[2]);
    }
    if (now - c.exit_at > 4000 || c.in_car) {
      c.exit_at = 0;
    } else if (game.position[2] < c.exit[2] - 1.2f && !c.unfreeze_at) {
      if (c.catches < 4) {
        ++c.catches;
        Invoke(ctx, base, "FREEZE_CHAR_POSITION", {game.ped, 1});
        Invoke(ctx, base, "SET_CHAR_COORDINATES",
               {game.ped, Bits(c.exit[0]), Bits(c.exit[1]), Bits(c.exit[2] + 1.0f)});
        Invoke(ctx, base, "SET_CHAR_COLLISION", {game.ped, 1});
        c.unfreeze_at = now + 350;
        REXLOG_WARN("gta4-trainer: {}: player dropping through the world after getting out (z {:.1f}, ground "
                    "{:.1f}); held on the spot, try {}",
                    label, game.position[2], c.exit[2], c.catches);
      } else {
        Invoke(ctx, base, "FREEZE_CHAR_POSITION", {game.ped, 0});
        const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {c.car});
        if (dead && !dead->r0())
          Invoke(ctx, base, "WARP_CHAR_INTO_CAR", {game.ped, c.car});
        Toast(s, std::string(label) + ": couldn't set you down here. Back in the seat; try another spot.", 3500);
        REXLOG_WARN("gta4-trainer: {}: player kept dropping through the world; put back in the seat", label);
        c.exit_at = 0;
      }
    }
  }
  if (spec.riders)
    Passengers(ctx, base, spec, c, now);
  // Someone following the player in (his group joins him), or in after him
  // while the canopy is still up or coming down: stopped where he is and put
  // in the passenger seat with the canopy up.
  if (spec.riders && !c.rider && c.car && now >= c.next_rider_look &&
      (c.phase == CanopyState::kOpeningIn || c.phase == CanopyState::kClosingIn)) {
    c.next_rider_look = now + 250;
    if (const uint32_t rider = FindRider(ctx, base, game, c.car); rider && SeatFree(ctx, base, c.car, 0)) {
      Invoke(ctx, base, "CLEAR_CHAR_TASKS_IMMEDIATELY", {rider});
      c.rider = rider;
      if (c.phase == CanopyState::kClosingIn) {
        c.phase = CanopyState::kOpeningRider;
        c.hold = 0.0f;
        c.started = now;
        c.sounded = c.open > 0.5f;  // (back up from part way: no second hiss)
      }
    }
  }
  // (and held there while the canopy rises: his group would walk him on)
  if (c.rider && GettingInto(ctx, base, c.rider, c.car))
    Invoke(ctx, base, "CLEAR_CHAR_TASKS_IMMEDIATELY", {c.rider});
  switch (c.phase) {
    case CanopyState::kRiderOut: {
      // Up while a passenger climbs out (hidden), then down again.
      const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {c.car});
      if (!dead || dead->r0() || now - c.started > 6000) {
        c.phase = CanopyState::kShut;
        c.open = 0.0f;
        return keep;
      }
      if (!c.sounded) {
        c.sounded = true;
        PlayCanopySound(spec, true);
      }
      c.open = std::min(1.0f, c.open + dt / spec.open_seconds);
      PoseCanopy(ctx, base, spec, c, c.car, c.open);
      if (c.open < 1.0f || c.leaver)
        return keep;
      c.phase = c.in_car ? CanopyState::kClosingIn : CanopyState::kClosingOut;
      c.hold = 0.0f;
      c.sounded = false;
      return keep;
    }
    case CanopyState::kOpeningIn:
    case CanopyState::kOpeningOut:
    case CanopyState::kOpeningRider: {
      // (A vehicle that is gone or wrecked meanwhile ends it.)
      const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {c.car});
      if (!dead || dead->r0() || now - c.started > 6000) {
        c.phase = CanopyState::kShut;
        c.open = 0.0f;
        c.rider = 0;  // (let go: the game's own way in again)
        return 0;
      }
      if (!c.sounded) {
        c.sounded = true;
        PlayCanopySound(spec, true);
      }
      c.open = std::min(1.0f, c.open + dt / spec.open_seconds);
      PoseCanopy(ctx, base, spec, c, c.car, c.open);
      if (c.open < 1.0f || (c.hold += dt) < 0.15f)
        return keep;
      if (c.phase == CanopyState::kOpeningRider) {
        SeatRider(ctx, base, spec, c);
        c.phase = CanopyState::kClosingIn;
        c.hold = 0.0f;
        c.sounded = false;
        return keep;
      }
      if (c.phase == CanopyState::kOpeningIn) {
        Invoke(ctx, base, "WARP_CHAR_INTO_CAR", {game.ped, c.car});
        c.in_car = true;
        SeatRider(ctx, base, spec, c);  // (and anyone who followed him)
      } else {
        // Out the game's own way, as Rockstar's brucie_heli takes the player
        // out of a car (CLEAR_CHAR_TASKS, TASK_LEAVE_CAR_IMMEDIATELY), hidden
        // while he climbs out; kLeaving puts him beside the cockpit once he
        // is on his feet. v1.7.8-v1.7.11 warped him out of the seat
        // (WARP_CHAR_FROM_CAR_TO_COORD) and he fell through the world.
        if (c.log < 8) {
          ++c.log;
          REXLOG_INFO("gta4-trainer: {} canopy open, player getting out", label);
        }
        Invoke(ctx, base, "CLEAR_CHAR_TASKS", {game.ped});
        if (NativeKnown("SET_CHAR_VISIBLE")) {
          Invoke(ctx, base, "SET_CHAR_VISIBLE", {game.ped, 0});
          c.hidden = true;
        }
        Invoke(ctx, base, "TASK_LEAVE_CAR_IMMEDIATELY", {game.ped, c.car});
        c.leaving_since = now;
        c.phase = CanopyState::kLeaving;
        return X_INPUT_GAMEPAD_Y;
      }
      if (c.log < 8) {
        ++c.log;
        REXLOG_INFO("gta4-trainer: {} canopy open, player in", label);
      }
      c.phase = c.phase == CanopyState::kOpeningIn ? CanopyState::kClosingIn : CanopyState::kClosingOut;
      c.hold = 0.0f;
      c.sounded = false;
      return c.in_car ? uint32_t(X_INPUT_GAMEPAD_Y) : 0u;
    }
    case CanopyState::kLeaving: {
      // On his feet beside it: put down, shown, the canopy closes.
      const auto in_car = Invoke(ctx, base, "IS_CHAR_IN_ANY_CAR", {game.ped});
      const bool out = in_car && !in_car->r0();
      if (out && now - c.leaving_since >= 150) {
        PlaceBeside(ctx, base, game, spec, c, now);
        if (c.hidden) {
          Invoke(ctx, base, "SET_CHAR_VISIBLE", {game.ped, 1});
          c.hidden = false;
        }
        c.in_car = false;
        c.phase = CanopyState::kClosingOut;
        c.hold = 0.0f;
        c.sounded = false;
        return 0;
      }
      if (now - c.leaving_since > 3500) {
        // The game did not take him out: shown again, the canopy closes over him.
        if (c.hidden) {
          Invoke(ctx, base, "SET_CHAR_VISIBLE", {game.ped, 1});
          c.hidden = false;
        }
        REXLOG_WARN("gta4-trainer: {}: the game did not take the player out in 3.5 s; canopy closes", label);
        c.phase = CanopyState::kClosingOut;
        c.hold = 0.0f;
        c.sounded = false;
      }
      return X_INPUT_GAMEPAD_Y;
    }
    case CanopyState::kClosingIn:
    case CanopyState::kClosingOut:
      if ((c.hold += dt) >= 0.35f) {  // a beat with it up, then down
        if (!c.sounded) {
          c.sounded = true;
          PlayCanopySound(spec, false);
        }
        c.open = std::max(0.0f, c.open - dt / spec.close_seconds);
        PoseCanopy(ctx, base, spec, c, c.car, c.open);
        if (c.open <= 0.0f) {
          c.phase = CanopyState::kShut;
          c.sounded = false;
        }
      }
      return keep;
    case CanopyState::kShut:
      break;
  }
  if (!c.in_car && now < c.next_probe)
    return 0;
  c.next_probe = now + 250;
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  if (vehicle.car && vehicle.model == model) {
    if (!c.in_car || c.car != *vehicle.car)
      PoseCanopy(ctx, base, spec, c, *vehicle.car, 0.0f);  // shut (and its pivot looked up)
    c.in_car = true;
    c.car = *vehicle.car;
    if (c.bone < 0 || !NativeKnown("TASK_LEAVE_CAR_IMMEDIATELY"))
      return 0;  // no canopy pivot (or no way to take the player out): the game's own way out
    if (spec.ground_only) {
      // In the air or rolling, Y is the game's (the player jumps out).
      float speed = 0.0f;
      if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {c.car, 0}, 1))
        speed = std::fabs(v->f(0));
      if (g_jet.flying || speed > 2.5f)
        return 0;
    }
    if ((buttons & X_INPUT_GAMEPAD_Y) && !(prev & X_INPUT_GAMEPAD_Y) && !s.open) {
      c.phase = CanopyState::kOpeningOut;
      c.open = c.hold = 0.0f;
      c.started = now;
      c.sounded = false;
      return X_INPUT_GAMEPAD_Y;
    }
    // Someone else getting in while the player sits there: the canopy rises
    // for him.
    if (spec.riders && now >= c.next_rider_look) {
      c.next_rider_look = now + 250;
      if (const uint32_t rider = FindRider(ctx, base, game, c.car); rider && SeatFree(ctx, base, c.car, 0)) {
        Invoke(ctx, base, "CLEAR_CHAR_TASKS_IMMEDIATELY", {rider});
        c.rider = rider;
        c.phase = CanopyState::kOpeningRider;
        c.open = c.hold = 0.0f;
        c.started = now;
        c.sounded = false;
      }
    }
    return X_INPUT_GAMEPAD_Y;
  }
  c.in_car = false;
  if (vehicle.car)
    return 0;
  // On foot: Y by a jet puts the player in at once; otherwise, is the game
  // starting to put the player into one?
  std::optional<InvokeResult> target;
  const uint64_t y_at = g_y_down_at.load(std::memory_order_relaxed);
  if (spec.quick_entry && y_at > c.y_seen && now - y_at < 400 && !s.open) {
    c.y_seen = y_at;
    const auto near = Invoke(ctx, base, "GET_CLOSEST_CAR",
                             {Bits(game.position[0]), Bits(game.position[1]), Bits(game.position[2]), Bits(7.0f), 0,
                              70});
    if (near && near->r0()) {
      const auto m = Invoke(ctx, base, "GET_CAR_MODEL", {near->r0(), 0}, 1);
      if (m && m->words[0] == model) {
        InvokeResult t;
        t.words[0] = near->r0();
        target = t;
      }
    }
  }
  if (!target) {
    const auto getting = Invoke(ctx, base, "IS_CHAR_GETTING_IN_TO_A_CAR", {game.ped});
    if (!getting || !getting->r0())
      return 0;
    target = Invoke(ctx, base, "GET_CAR_CHAR_IS_USING", {game.ped, 0}, 1);
  }
  if (!target || !target->words[0])
    return 0;
  const auto got = Invoke(ctx, base, "GET_CAR_MODEL", {target->words[0], 0}, 1);
  if (!got || got->words[0] != model || !NativeKnown("CLEAR_CHAR_TASKS_IMMEDIATELY") ||
      !NativeKnown("WARP_CHAR_INTO_CAR"))
    return 0;
  const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {target->words[0]});
  if (!dead || dead->r0() || !PoseCanopy(ctx, base, spec, c, target->words[0], 0.0f) || c.bone < 0)
    return 0;  // no pivot: the game's own way in
  Invoke(ctx, base, "CLEAR_CHAR_TASKS_IMMEDIATELY", {game.ped});
  c.car = target->words[0];
  c.phase = CanopyState::kOpeningIn;
  c.open = c.hold = 0.0f;
  c.started = now;
  c.sounded = false;
  return 0;
}

// The add-on cars' own engines: original synthesized voices
// (gta4_car_engine.h) played through the port's mixer like the jet's. The
// Batmobile's, in the spirit of the 1989/1992 film car: a turbine whine over
// a deep, lumpy V8, a roar on the booster (gadgets on, d-pad right), meaner
// flat out. The Deimos's, the Nexus's, the Arbiter X's and the Dominator
// 645's on a physically informed model of the real engines their cars are
// drawn from (each cylinder firing in its real order into its bank's
// exhaust, the pipes' own resonances, the intake, the valve gear): a late-70s
// Ferrari's flat-plane V8, the NSX's VTEC V6, the GTO's Ram Air 400 and the
// '65 Mustang's 289. Revs come from the road speed through five simulated
// gears (the throttle revs it standing still); getting in cranks the
// starter, getting out it winds down. The cars' game names (BATMOBILE,
// DEIMOS, NEXUS, ARBITERX, DOM645T) have no sounds in IV, so these are the
// only ones, their doors' too (FollowDoors) (up to v1.7.10 the Batmobile
// drove with the Sabre GT's engine, up to v1.7.12 the Deimos with the
// Turismo's, up to v1.7.16 the Nexus with the Infernus's and the other two
// with the Banshee's). Canopy hiss and clunk (the Batmobile's and the
// F-16's) come from the Batmobile's voice; each car's pop-ups, doors and
// struts from its own.
gta4::car::EngineSynth g_bat_synth{gta4::car::kBatmobile}, g_deimos_synth{gta4::car::kDeimos};
gta4::car::EngineSynth g_nexus_synth{gta4::car::kNexus}, g_arbiter_synth{gta4::car::kArbiter},
    g_dominator_synth{gta4::car::kDominator};
gta4::car::EngineSynth g_lazar_synth{gta4::car::kLazar}, g_gauntlet_synth{gta4::car::kGauntlet};
// The Hellcat (v1.7.44): its supercharged Hemi.
gta4::car::EngineSynth g_hellcat_synth{gta4::car::kHellcat};
// The DeLoreans: each its own voice of the same PRV V6.
gta4::car::EngineSynth g_dmc_synth{gta4::car::kDelorean}, g_bttf1_synth{gta4::car::kDelorean},
    g_bttf2_synth{gta4::car::kDelorean}, g_bttf3_synth{gta4::car::kDelorean};
#if REX_PLATFORM_PS5
class SynthSource final : public gta4ps5::sound::Source {
 public:
  explicit SynthSource(gta4::car::EngineSynth& synth) : synth_(synth) {}
  size_t Read(float* lr, size_t frames) override {
    synth_.Render(lr, frames);
    return frames;
  }
  bool Rewind() override { return true; }

 private:
  gta4::car::EngineSynth& synth_;
};
#endif
struct CarEngineState {
  gta4::car::EngineSynth* synth;
  std::string_view label, model;
  std::array<float, 5> gear_top;  // m/s at the rev limit in each gear
  float volume;                   // its level in the mix
  bool air_brakes = false;        // a truck's: their hiss at a stop
  std::string_view model2 = {};   // a second model it also is (Part II's DeLorean in hover form)
  bool in_car = false, running = false;  // running: the voice plays (winding down too)
  bool rolling = false;
  int gear = 0;
  uint64_t next_probe = 0;
  int log = 0;
#if REX_PLATFORM_PS5
  gta4ps5::sound::Voice* voice = nullptr;
#endif
};
// (Levels: v1.7.11-v1.7.13 played the Batmobile at full level, much louder
// than the game's own cars.)
CarEngineState g_bat_engine{&g_bat_synth, "Batmobile", "BATMOBILE", {12.0f, 21.0f, 31.0f, 42.0f, 56.0f}, 0.35f};
// (The physically informed four are levelled to the same full-throttle
// loudness in gta4_car_engine.h, about the Batmobile's in the game.)
CarEngineState g_deimos_engine{&g_deimos_synth, "Deimos", "DEIMOS", {14.0f, 24.0f, 35.0f, 47.0f, 62.0f}, 0.38f};
CarEngineState g_nexus_engine{&g_nexus_synth, "Nexus", "NEXUS", {15.0f, 25.0f, 36.0f, 48.0f, 63.0f}, 0.38f};
CarEngineState g_arbiter_engine{&g_arbiter_synth, "Arbiter X", "ARBITERX", {13.0f, 23.0f, 34.0f, 46.0f, 60.0f},
                                0.36f};
CarEngineState g_dominator_engine{&g_dominator_synth, "Dominator 645", "DOM645T",
                                  {13.0f, 22.0f, 33.0f, 45.0f, 58.0f}, 0.37f};
// The Lazar: governed at 2,300 through a truck's gears, about 100 km/h flat out.
CarEngineState g_lazar_engine{&g_lazar_synth, "Lazar 3", "LAZAR3", {5.5f, 10.0f, 15.0f, 21.0f, 28.0f}, 0.42f, true};
CarEngineState g_gauntlet_engine{&g_gauntlet_synth, "Gauntlet Classic", "GAUNTLETC",
                                 {13.0f, 23.0f, 34.0f, 46.0f, 61.0f}, 0.38f};
// The Hellcat: 707 hp through an eight-speed, here five, geared long.
CarEngineState g_hellcat_engine{&g_hellcat_synth, "Hellcat", "HELLCAT", {16.0f, 27.0f, 39.0f, 52.0f, 68.0f}, 0.40f};
// The DMC-12's five-speed: about 110 mph flat out (88 needs fourth).
constexpr std::array<float, 5> kDmcGears = {14.0f, 24.0f, 34.0f, 43.0f, 50.0f};
CarEngineState g_dmc_engine{&g_dmc_synth, "DeLorean DMC-12", "DELNOR", kDmcGears, 0.38f};
CarEngineState g_bttf1_engine{&g_bttf1_synth, "Time Machine (Part I)", "DELONE", kDmcGears, 0.38f};
CarEngineState g_bttf2_engine{&g_bttf2_synth, "Time Machine (Part II)", "DELTWO", kDmcGears, 0.38f, false, "DELHOV"};
CarEngineState g_bttf3_engine{&g_bttf3_synth, "Time Machine (Part III)", "DELTHR", kDmcGears, 0.38f};

// Starts a car's voice (want) or stops it once its synth has gone quiet.
void CarVoice(CarEngineState& e, bool want) {
#if REX_PLATFORM_PS5
  if (!e.voice) {
    if (!want)
      return;
    gta4ps5::sound::StartOutput();
    e.voice = gta4ps5::sound::NewVoice();
    e.voice->game_sound = true;  // quiet while the game mutes its own sound
  }
  if (want && !e.voice->Active()) {
    e.voice->SetVolume(1.0f);
    e.voice->Play(std::make_unique<SynthSource>(*e.synth), true);
  } else if (!want && e.voice->Active() && e.synth->Quiet()) {
    e.voice->Stop();
  }
#else
  (void)e;
  (void)want;
#endif
}

void CanopySound(bool opening) {
  if (opening)
    g_bat_synth.CanopyOpen();
  else
    g_bat_synth.CanopyShut();
  CarVoice(g_bat_engine, true);
  g_bat_engine.running = true;  // (stopped by CarEngine once quiet)
}

// The pop-up lamps' motor (the Deimos's or the Nexus's), for as long as they
// swing.
void PopupMotorSound(const PopupSpec& spec, float seconds) {
  auto& e = spec.slot == kPoseNexus ? g_nexus_engine : g_deimos_engine;
  e.synth->Motor(seconds);
  CarVoice(e, true);
  e.running = true;
}

// The add-on cars' doors (FollowDoors): the latch letting go, a gullwing's
// struts (going up with the latch, and on the way down), the slam.
void DoorSound(int car, DoorEvent event) {
  CarEngineState* const engines[kDoorCars] = {&g_deimos_engine, &g_dominator_engine, &g_arbiter_engine,
                                              &g_nexus_engine,  &g_gauntlet_engine,  &g_dmc_engine,
                                              &g_bttf1_engine,  &g_bttf2_engine,     &g_bttf3_engine,
                                              &g_bttf2_engine};
  if (car < 0 || car >= kDoorCars)
    return;
  auto& e = *engines[car];
  const bool gullwing = DoorIsGullwing(car);
  switch (event) {
    case DoorEvent::kOpening:
      e.synth->DoorOpen();
      if (gullwing)
        e.synth->CanopyOpen(false);  // the struts lifting it
      break;
    case DoorEvent::kLowering:
      e.synth->CanopyShut(false);
      break;
    case DoorEvent::kShut:
      e.synth->DoorShut();
      break;
  }
  CarVoice(e, true);
  e.running = true;  // (stopped by CarEngine once quiet)
}

// ---------------------------------------------------------------------------
// The Lazar 3 (Grom and SkylineGTRFreak's model, Milcin's port, on whose PC
// version the gun neither turns nor fires). The converter hangs the turret
// on extra_1 (its ring, at the roof's centre) and the gun on extra_5 (its
// trunnions). Here, with the player in it, the turret follows the camera
// round (the right stick aims it) and the gun its pitch, each at its motors'
// speed, their whine rising as they slew; L1 (hidden from the game then: no
// drive-by) fires the cannon, three rounds a second: a round down the
// barrel at once (hitscan, as the jet's cannon) and a shell flying out of
// the muzzle that bursts where it meets anything (as the Batmobile's
// missiles do), with IV's muzzle flash and the gun's report. The side ramps
// are its doors (kLazarRamps).
// ---------------------------------------------------------------------------
struct Shell {
  bool live = false, armed = false, physics = true;
  uint32_t object = 0;
  uint32_t target = 0;  // the locked vehicle it homes on, or 0
  float origin[3]{}, start[3]{}, dir[3]{}, pos[3]{};
  float speed = 0.0f;
  uint64_t fired = 0;
  int frames = 0;
};
// The Lazar's lock-on: the jets' (JetLock, the HUD's marks), along its gun.
JetState g_lazar_aim;
// The cars it has rammed lately (the heat: a little a ram, a blast's worth
// when one it rammed is wrecked within 10 s).
struct Rammed {
  uint32_t car = 0;
  uint64_t at = 0;
  bool wrecked = false;
};
std::array<Rammed, 12> g_rammed{};
uint64_t g_next_ram_look = 0;
void PublishLazarHud(bool on);
struct LazarState {
  uint32_t car = 0, info = 0;
  bool in_car = false, model_requested = false;
  int32_t turret_bone = -1, gun_bone = -1;
  float turret_hinge[3]{}, gun_hinge[3]{};
  float yaw = 0.0f, pitch = 0.0f;  // degrees: left of dead ahead; above level
  float posed_yaw = 999.0f, posed_pitch = 999.0f;
  uint64_t last_ms = 0, next_probe = 0, next_shot = 0, next_pose = 0;
  int shots = 0, aim_log = 0, shot_log = 0, burst_log = 0;
  std::array<Shell, 8> shells{};
};
LazarState g_lazar;
constexpr float kLazarYawRate = 70.0f, kLazarPitchRate = 40.0f;  // degrees a second
constexpr float kLazarPitchMin = -8.0f, kLazarPitchMax = 45.0f;
constexpr float kLazarAimLift = 8.0f;  // the chase camera looks down on the car: the gun a little above its line
constexpr float kLazarBarrel = 2.31f;  // the trunnions to the muzzle (the mod's weapon_1a), m
constexpr uint64_t kLazarShotMs = 320;
constexpr float kLazarShellSpeed = 150.0f;

void LazarRampSound(bool opening) {
  g_lazar_synth.Hydraulic(opening ? kLazarRamps.open_seconds : kLazarRamps.close_seconds, opening);
  CarVoice(g_lazar_engine, true);
  g_lazar_engine.running = true;  // (stopped by CarEngine once quiet)
}
void LazarCannonSound() {
  g_lazar_synth.Cannon();
  CarVoice(g_lazar_engine, true);
  g_lazar_engine.running = true;
}
void LazarServoSound(float level) {
  g_lazar_synth.Servo(level);
  if (level > 0.0f) {
    CarVoice(g_lazar_engine, true);
    g_lazar_engine.running = true;
  }
}

bool LazarShellsLive() {
  return std::any_of(g_lazar.shells.begin(), g_lazar.shells.end(), [](const Shell& sh) { return sh.live; });
}

// A Lazar just made, or got into: its turret, gun and left ramp's extras
// shown (the game picks extras at random; a hidden one's parts are sent to
// the world's origin).
void PrepareLazar(PPCContext& ctx, uint8_t* base, uint32_t car, bool getting_in) {
  for (const uint32_t extra : {1u, 5u, 6u})
    ShowExtra(ctx, base, car, extra, getting_in);
  // Armoured: its run-flat tyres don't burst (they did, to a pistol). Set
  // when it is made and again each time the player gets in.
  if (NativeKnown("SET_CAN_BURST_CAR_TYRES"))
    Invoke(ctx, base, "SET_CAN_BURST_CAR_TYRES", {car, 0});
}

// The turret turned by `yaw` about its ring, the gun pitched on its
// trunnions and carried round with it.
void PoseTurret(PPCContext& ctx, uint8_t* base, LazarState& z, uint32_t car) {
  static const uint32_t kModel = ModelHash("LAZAR3");
  const uint32_t vehicle = VehicleAddress(base, car);
  const uint32_t info = vehicle ? FindModelInfo(base, kModel) : 0;
  if (!info || !VehicleIsModel(base, vehicle, info))
    return;
  if (z.info != info) {
    z.info = info;
    z.turret_bone = FindBone(ctx, base, vehicle, "extra_1");
    z.gun_bone = FindBone(ctx, base, vehicle, "extra_5");
    for (const auto& [bone, hinge] : {std::pair{z.turret_bone, z.turret_hinge}, std::pair{z.gun_bone, z.gun_hinge}})
      if (const uint32_t rest = RestMatrix(ctx, base, vehicle, bone))
        for (int a = 0; a < 3; ++a)
          hinge[a] = ReadFloat(base, rest + 48 + 4 * uint32_t(a));
    REXLOG_INFO("gta4-trainer: Lazar 3 turret on bone {} at ({:.2f}, {:.2f}, {:.2f}), gun on bone {} at ({:.2f}, "
                "{:.2f}, {:.2f})",
                z.turret_bone, z.turret_hinge[0], z.turret_hinge[1], z.turret_hinge[2], z.gun_bone, z.gun_hinge[0],
                z.gun_hinge[1], z.gun_hinge[2]);
  }
  if (z.turret_bone < 0 || z.gun_bone < 0)
    return;
  float yaw[3][3], pitch[3][3], both[3][3];
  TurnMatrix(2, z.yaw, yaw);
  TurnMatrix(0, z.pitch, pitch);
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k)
      both[r][k] = pitch[r][0] * yaw[0][k] + pitch[r][1] * yaw[1][k] + pitch[r][2] * yaw[2][k];
  float at[3];
  for (int k = 0; k < 3; ++k) {
    float v = 0;
    for (int m = 0; m < 3; ++m)
      v += (z.gun_hinge[m] - z.turret_hinge[m]) * yaw[m][k];
    at[k] = z.turret_hinge[k] + v;
  }
  PoseBoneMatrix(ctx, base, vehicle, info, kPoseLazar, 0, z.turret_bone, yaw, nullptr, 1.0f);
  PoseBoneMatrix(ctx, base, vehicle, info, kPoseLazar, 1, z.gun_bone, both, at, 1.0f);
}

// The muzzle and the gun's line in the car's own space.
void LazarGunLine(const LazarState& z, float (&muzzle)[3], float (&dir)[3]) {
  const float yr = z.yaw * 3.14159265f / 180.0f, pr = z.pitch * 3.14159265f / 180.0f;
  const float cy = std::cos(yr), sy = std::sin(yr), cp = std::cos(pr), sp = std::sin(pr);
  // (relative to the ring: the trunnions, then the barrel pitched; turned with the turret)
  const float q[3] = {z.gun_hinge[0] - z.turret_hinge[0],
                      z.gun_hinge[1] - z.turret_hinge[1] + kLazarBarrel * cp,
                      z.gun_hinge[2] - z.turret_hinge[2] + kLazarBarrel * sp};
  muzzle[0] = z.turret_hinge[0] + q[0] * cy - q[1] * sy;
  muzzle[1] = z.turret_hinge[1] + q[0] * sy + q[1] * cy;
  muzzle[2] = z.turret_hinge[2] + q[2];
  dir[0] = -cp * sy;
  dir[1] = cp * cy;
  dir[2] = sp;
}

// Where the camera looks, relative to the car: degrees left of its nose,
// and up from level. The chase camera always looks at the car, so the line
// from the camera to the car is where it looks, read in the car's own axes
// (no angle conventions involved: v1.7.20 took GET_CAM_ROT's yaw for a
// heading, and it is a quarter turn off one, log 076). Pitch is the camera's
// own. Close in (a camera in the car) its yaw is used, turned a quarter.
std::optional<std::pair<float, float>> CameraAim(PPCContext& ctx, uint8_t* base, uint32_t car, bool log) {
  const auto cam = Invoke(ctx, base, "GET_GAME_CAM", {0}, 0);
  if (!cam || !cam->words[0])
    return std::nullopt;
  const auto rot = Invoke(ctx, base, "GET_CAM_ROT", {cam->words[0], 0, 0, 0}, 1, 0, 3);
  const auto pos = Invoke(ctx, base, "GET_CAM_POS", {cam->words[0], 0, 0, 0}, 1, 0, 3);
  const auto at = CarPosition(ctx, base, car);
  static constexpr float kAhead[3] = {0.0f, 1.0f, 0.0f}, kRight[3] = {1.0f, 0.0f, 0.0f};
  const auto ahead = CarPoint(ctx, base, car, kAhead), right = CarPoint(ctx, base, car, kRight);
  if (!rot || !at || !ahead || !right || !std::isfinite(rot->f(0)))
    return std::nullopt;
  const float pitch = rot->f(0);
  float yaw = 0.0f;
  bool from_line = false;
  if (pos && std::isfinite(pos->f(0))) {
    const float look[2] = {(*at)[0] - pos->f(0), (*at)[1] - pos->f(1)};
    const float fwd[2] = {(*ahead)[0] - (*at)[0], (*ahead)[1] - (*at)[1]};
    const float side[2] = {(*right)[0] - (*at)[0], (*right)[1] - (*at)[1]};
    if (look[0] * look[0] + look[1] * look[1] > 2.0f * 2.0f) {
      yaw = std::atan2(-(look[0] * side[0] + look[1] * side[1]), look[0] * fwd[0] + look[1] * fwd[1]) *
            180.0f / 3.14159265f;
      from_line = true;
    }
  }
  if (!from_line) {
    const auto heading = Invoke(ctx, base, "GET_CAR_HEADING", {car, 0}, 1);
    if (!heading || !std::isfinite(rot->f(2)))
      return std::nullopt;
    yaw = rot->f(2) + 90.0f - heading->f(0);
  }
  if (log)
    REXLOG_INFO("gta4-trainer: Lazar 3 aim: camera {:.1f} degrees from the nose ({}), {:.1f} up (its yaw {:.1f})",
                std::remainder(yaw, 360.0f), from_line ? "from its line to the car" : "from its yaw", pitch,
                rot->f(2));
  return std::pair{std::remainder(yaw, 360.0f), pitch};
}

void ShellBurst(PPCContext& ctx, uint8_t* base, const float p[3]) {
  // type 0 (grenade): a shell's burst, smaller than the missiles'
  Invoke(ctx, base, "ADD_EXPLOSION", {Bits(p[0]), Bits(p[1]), Bits(p[2]), 0, Bits(0.9f), 1, 0, Bits(0.35f)});
  AddHeat(1.0f);
}

void ApplyHeat(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint64_t now) {
  if (g_heat_ms)
    g_heat = std::max(0.0f, g_heat - float(now - g_heat_ms) / 15000.0f);
  g_heat_ms = now;
  if (g_heat_pending <= 0.0f)
    return;
  g_heat += g_heat_pending;
  g_heat_pending = 0.0f;
  // (v1.7.24 also bailed out on a player index of 0, which is the player's
  // in single player: no blast ever raised the wanted level)
  if (s.never_wanted)
    return;
  const int stars = g_heat >= 8.0f ? 4 : g_heat >= 3.5f ? 3 : g_heat >= 1.0f ? 2 : 0;
  if (!stars)
    return;
  const auto above = Invoke(ctx, base, "IS_WANTED_LEVEL_GREATER", {game.player, uint32_t(stars - 1)});
  if (!above || above->r0())
    return;  // (already as wanted, or more)
  Invoke(ctx, base, "ALTER_WANTED_LEVEL", {game.player, uint32_t(stars)});
  Invoke(ctx, base, "APPLY_WANTED_LEVEL_CHANGE_NOW", {game.player, 0});
  REXLOG_INFO("gta4-trainer: weapons' blasts: wanted level {} (heat {:.1f})", stars, g_heat);
}

void FireLazar(PPCContext& ctx, uint8_t* base, LazarState& z, State& s) {
  float muzzle[3], line[3];
  LazarGunLine(z, muzzle, line);
  float ahead[3];
  for (int a = 0; a < 3; ++a)
    ahead[a] = muzzle[a] + line[a] * 100.0f;
  const auto from = CarPoint(ctx, base, z.car, muzzle), far_point = CarPoint(ctx, base, z.car, ahead);
  if (!from || !far_point)
    return;
  float dir[3], len = 0;
  for (int a = 0; a < 3; ++a) {
    dir[a] = (*far_point)[a] - (*from)[a];
    len += dir[a] * dir[a];
  }
  len = std::sqrt(len);
  if (!(len > 1.0f))
    return;
  for (auto& c : dir)
    c /= len;
  // the round down the barrel (people and cars hit at once)
  const float out[3] = {(*from)[0] + dir[0] * 0.8f, (*from)[1] + dir[1] * 0.8f, (*from)[2] + dir[2] * 0.8f};
  const float to[3] = {(*from)[0] + dir[0] * 400.0f, (*from)[1] + dir[1] * 400.0f, (*from)[2] + dir[2] * 400.0f};
  Invoke(ctx, base, "FIRE_SINGLE_BULLET",
         {Bits(out[0]), Bits(out[1]), Bits(out[2]), Bits(to[0]), Bits(to[1]), Bits(to[2]), 150});
  // the flash, along the barrel
  const float flash_at[3] = {muzzle[0] + line[0] * 0.3f, muzzle[1] + line[1] * 0.3f, muzzle[2] + line[2] * 0.3f};
  const auto flashed = Invoke(ctx, base, "TRIGGER_PTFX_ON_VEH",
                              {0, z.car, Bits(flash_at[0]), Bits(flash_at[1]), Bits(flash_at[2]), Bits(z.pitch), 0,
                               Bits(z.yaw), Bits(2.6f)},
                              -1, 0, 1, 0, "muz_machine");
  LazarCannonSound();
  // the shell: the physics flies it out of the muzzle
  Shell* slot = nullptr;
  for (auto& sh : z.shells)
    if (!sh.live) {
      slot = &sh;
      break;
    }
  const uint32_t hash = ModelHash(kRocketModel);
  const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {hash});
  uint32_t object = 0;
  if (slot && loaded && loaded->r0()) {
    Shell& sh = *slot;
    sh = {};
    for (int a = 0; a < 3; ++a) {
      sh.dir[a] = dir[a];
      sh.origin[a] = sh.start[a] = sh.pos[a] = (*from)[a] + dir[a] * 1.5f;
    }
    sh.speed = kLazarShellSpeed;
    sh.target = g_lazar_aim.lock.target;
    const auto obj =
        Invoke(ctx, base, "CREATE_OBJECT", {hash, Bits(sh.pos[0]), Bits(sh.pos[1]), Bits(sh.pos[2]), 0, 1}, 4);
    if (obj && obj->words[0]) {
      object = sh.object = obj->words[0];
      sh.live = true;
      sh.fired = NowMs();
      PointObjectAlong(ctx, base, sh.object, sh.dir);
      Invoke(ctx, base, "SET_OBJECT_INVINCIBLE", {sh.object, 1});
      Invoke(ctx, base, "SET_OBJECT_COLLISION", {sh.object, 1});
      Invoke(ctx, base, "SET_OBJECT_INITIAL_VELOCITY",
             {sh.object, Bits(dir[0] * sh.speed), Bits(dir[1] * sh.speed), Bits(dir[2] * sh.speed)});
    }
  }
  ++z.shots;
  if (z.shot_log < 6) {
    ++z.shot_log;
    REXLOG_INFO("gta4-trainer: Lazar 3 cannon fired (turret {:.0f}, gun {:.0f} degrees; flash {}, shell {:08X})",
                z.yaw, z.pitch, flashed && flashed->r0() ? "started" : "not started", object);
  }
  (void)s;
}

// Every frame while shells fly (also after the player has left the Lazar).
void UpdateShells(PPCContext& ctx, uint8_t* base, LazarState& z, float dt) {
  const uint64_t now = NowMs();
  for (auto& sh : z.shells) {
    if (!sh.live)
      continue;
    ++sh.frames;
    if (sh.physics) {
      if (const auto at = Invoke(ctx, base, "GET_OBJECT_COORDINATES", {sh.object, 0, 0, 0}, 1, 0, 3))
        for (int a = 0; a < 3; ++a)
          sh.pos[a] = at->f(a);
      float moved = 0;
      for (int a = 0; a < 3; ++a)
        moved += (sh.pos[a] - sh.origin[a]) * (sh.pos[a] - sh.origin[a]);
      if (sh.frames == 5 && moved < 4.0f) {  // the physics did not take it: flown by hand
        sh.physics = false;
        Invoke(ctx, base, "FREEZE_OBJECT_POSITION", {sh.object, 1});
        Invoke(ctx, base, "SET_OBJECT_COLLISION", {sh.object, 0});
      }
    }
    // Locked: it turns towards its target (up to 1.6 rad/s), its line
    // starting again where it is.
    if (sh.target && sh.frames > 2) {
      if (const auto tp = TargetPosition(ctx, base, sh.target)) {
        float want[3], len = 0;
        for (int a = 0; a < 3; ++a) {
          want[a] = (*tp)[a] + (a == 2 ? 0.6f : 0.0f) - sh.pos[a];
          len += want[a] * want[a];
        }
        len = std::sqrt(len);
        if (len > 1.0f) {
          float dot = 0;
          for (int a = 0; a < 3; ++a) {
            want[a] /= len;
            dot += want[a] * sh.dir[a];
          }
          const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
          const float k = angle > 1e-4f ? std::min(1.0f, 1.6f * dt / angle) : 1.0f;
          float n = 0;
          for (int a = 0; a < 3; ++a) {
            sh.dir[a] += (want[a] - sh.dir[a]) * k;
            n += sh.dir[a] * sh.dir[a];
          }
          n = std::sqrt(std::max(n, 1e-6f));
          for (int a = 0; a < 3; ++a) {
            sh.dir[a] /= n;
            sh.start[a] = sh.pos[a];
          }
          if (sh.object)
            PointObjectAlong(ctx, base, sh.object, sh.dir);
        }
      } else {
        sh.target = 0;  // (gone)
      }
    }
    if (sh.physics) {
      float along = 0;
      for (int a = 0; a < 3; ++a)
        along += (sh.pos[a] - sh.start[a]) * sh.dir[a];
      float v[3];
      for (int a = 0; a < 3; ++a)
        v[a] = sh.dir[a] * sh.speed + (sh.start[a] + sh.dir[a] * along - sh.pos[a]) * 6.0f;
      Invoke(ctx, base, "SET_OBJECT_INITIAL_VELOCITY", {sh.object, Bits(v[0]), Bits(v[1]), Bits(v[2])});
    } else {
      for (int a = 0; a < 3; ++a)
        sh.pos[a] += sh.dir[a] * sh.speed * dt;
      Invoke(ctx, base, "SET_OBJECT_COORDINATES", {sh.object, Bits(sh.pos[0]), Bits(sh.pos[1]), Bits(sh.pos[2])});
    }
    float d2 = 0;
    for (int a = 0; a < 3; ++a)
      d2 += (sh.pos[a] - sh.origin[a]) * (sh.pos[a] - sh.origin[a]);
    const float travelled = std::sqrt(d2);
    const char* hit = nullptr;
    if (sh.physics && !sh.armed && travelled > 4.0f) {
      Invoke(ctx, base, "SET_OBJECT_RECORDS_COLLISIONS", {sh.object, 1});
      sh.armed = true;
    } else if (sh.physics && sh.armed) {
      const auto c = Invoke(ctx, base, "HAS_OBJECT_COLLIDED_WITH_ANYTHING", {sh.object});
      if (c && c->r0())
        hit = "something";
    }
    if (!hit) {
      const auto gz = GroundZ(ctx, base, sh.pos[0], sh.pos[1], sh.pos[2] + 2.0f);
      if (gz && sh.pos[2] <= *gz + 0.15f)
        hit = "the ground";
    }
    if (!hit && travelled > 5.0f) {
      const auto near = Invoke(ctx, base, "GET_CLOSEST_CAR",
                               {Bits(sh.pos[0]), Bits(sh.pos[1]), Bits(sh.pos[2]), Bits(2.6f), 0, 70});
      if (near && near->r0() && near->r0() != z.car)
        hit = "a vehicle";
    }
    const bool spent = travelled > 450.0f || now - sh.fired > 4000 || !std::isfinite(sh.pos[2]);
    if (!hit && !spent)
      continue;
    if (hit && std::isfinite(sh.pos[2]))
      ShellBurst(ctx, base, sh.pos);
    if (z.burst_log < 6) {
      ++z.burst_log;
      REXLOG_INFO("gta4-trainer: Lazar 3 shell {} after {:.0f} m ({})", hit ? "hit" : "spent", travelled,
                  hit ? hit : "out of range");
    }
    DeleteObject(ctx, base, sh.object);
    sh.live = false;
  }
}

// Every frame in a Lazar (a look a few times a second otherwise, and while
// its shells fly). Returns the buttons it keeps from the game: L1.
uint32_t LazarTurret(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint16_t buttons) {
  auto& z = g_lazar;
  const uint64_t now = NowMs();
  const float dt = z.last_ms ? std::clamp(float(now - z.last_ms) / 1000.0f, 0.0f, 0.05f) : 0.0f;
  z.last_ms = now;
  UpdateShells(ctx, base, z, dt);
  if (!z.in_car && now < z.next_probe)
    return 0;
  z.next_probe = now + 300;
  static const uint32_t kModel = ModelHash("LAZAR3");
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  if (!vehicle.car || vehicle.model != kModel) {
    if (z.in_car) {
      LazarServoSound(0.0f);
      DropLock(ctx, base, g_lazar_aim.lock);
      g_jet_marks.clear();
      PublishLazarHud(false);
    }
    if (z.model_requested && !LazarShellsLive()) {
      Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {ModelHash(kRocketModel)});
      z.model_requested = false;
    }
    z.in_car = false;
    return 0;
  }
  if (!z.in_car || z.car != *vehicle.car) {
    if (z.car != *vehicle.car) {
      z.yaw = z.pitch = 0.0f;  // a Lazar not met before: its turret dead ahead
      z.posed_yaw = z.posed_pitch = 999.0f;
      z.car = *vehicle.car;
    }
    PrepareLazar(ctx, base, z.car, true);
    if (!z.model_requested) {
      Invoke(ctx, base, "REQUEST_MODEL", {ModelHash(kRocketModel)});
      z.model_requested = true;
    }
    z.next_shot = now + 400;  // (no round from a press that got the player in)
  }
  z.in_car = true;
  // The camera's line, the gun a little above it (the chase camera looks
  // down on the car), within the gun's travel.
  float want_yaw = z.yaw, want_pitch = z.pitch;
  const bool log_aim = z.aim_log < 6 && now >= z.next_pose;
  if (log_aim)
    ++z.aim_log;
  if (const auto aim = CameraAim(ctx, base, z.car, log_aim)) {
    want_yaw = aim->first;
    want_pitch = std::clamp(aim->second + kLazarAimLift, kLazarPitchMin, kLazarPitchMax);
  }
  const float yaw_step = std::clamp(std::remainder(want_yaw - z.yaw, 360.0f), -kLazarYawRate * dt, kLazarYawRate * dt);
  const float pitch_step = std::clamp(want_pitch - z.pitch, -kLazarPitchRate * dt, kLazarPitchRate * dt);
  z.yaw = std::remainder(z.yaw + yaw_step, 360.0f);
  z.pitch += pitch_step;
  const float slew = dt > 0.0f ? std::max(std::fabs(yaw_step) / (kLazarYawRate * dt),
                                          std::fabs(pitch_step) / (kLazarPitchRate * dt))
                               : 0.0f;
  LazarServoSound(slew > 0.05f ? std::min(1.0f, slew) : 0.0f);
  if (std::fabs(std::remainder(z.yaw - z.posed_yaw, 360.0f)) > 0.05f || std::fabs(z.pitch - z.posed_pitch) > 0.05f ||
      now >= z.next_pose) {
    PoseTurret(ctx, base, z, z.car);
    z.posed_yaw = z.yaw;
    z.posed_pitch = z.pitch;
    z.next_pose = now + 500;
  }
  // Targets along the gun (the jets' lock-on: dots, the nearest to the
  // line locks in 0.6 s, d-pad up picks the next); its shells home on the lock.
  uint32_t kept = X_INPUT_GAMEPAD_LEFT_SHOULDER;
  {
    float muzzle[3], line[3], ahead[3];
    LazarGunLine(z, muzzle, line);
    for (int a = 0; a < 3; ++a)
      ahead[a] = muzzle[a] + line[a] * 100.0f;
    const auto from = CarPoint(ctx, base, z.car, muzzle), far_point = CarPoint(ctx, base, z.car, ahead);
    if (from && far_point) {
      float fwd[3], len = 0;
      for (int a = 0; a < 3; ++a) {
        fwd[a] = (*far_point)[a] - (*from)[a];
        len += fwd[a] * fwd[a];
      }
      len = std::sqrt(len);
      if (len > 1.0f) {
        for (auto& c : fwd)
          c /= len;
        g_lazar_aim.car = z.car;
        g_lazar_aim.lock.range = 200.0f, g_lazar_aim.lock.keep_range = 240.0f;  // (the street's)
        const bool cycle = (buttons & X_INPUT_GAMEPAD_DPAD_UP) && !(g_lazar_aim.prev_buttons & X_INPUT_GAMEPAD_DPAD_UP) &&
                           !s.open;
        UpdateLock(ctx, base, g_lazar_aim, s, from->data(), fwd, now, cycle);
        const float probe[3] = {(*from)[0] + fwd[0] * 60.0f, (*from)[1] + fwd[1] * 60.0f, (*from)[2] + fwd[2] * 60.0f};
        BuildMarks(ctx, base, g_lazar_aim, probe, now);
        PublishLazarHud(true);
        kept |= X_INPUT_GAMEPAD_DPAD_UP;
      }
    }
  }
  g_lazar_aim.prev_buttons = buttons;
  // Cars it runs into: within 9 m, damaged by it (the game keeps who last
  // damaged a car), counted once each, a wreck again.
  if (now >= g_next_ram_look && NativeKnown("HAS_CAR_BEEN_DAMAGED_BY_CAR")) {
    g_next_ram_look = now + 200;
    if (const auto at = CarPosition(ctx, base, z.car)) {
      ForEachVehicle(base, [&](uint32_t handle, uint32_t vehicle) {
        if (handle == z.car)
          return;
        float p[3];
        if (!EntityPosition(base, vehicle, p))
          return;
        const float dx = p[0] - (*at)[0], dy = p[1] - (*at)[1], dz = p[2] - (*at)[2];
        if (dx * dx + dy * dy + dz * dz > 81.0f)
          return;
        // (the game may put the damage down to the Lazar or to its driver)
        const auto hit = Invoke(ctx, base, "HAS_CAR_BEEN_DAMAGED_BY_CAR", {handle, z.car});
        bool rammed = hit && hit->r0();
        if (!rammed && NativeKnown("HAS_CAR_BEEN_DAMAGED_BY_CHAR")) {
          const auto by = Invoke(ctx, base, "HAS_CAR_BEEN_DAMAGED_BY_CHAR", {handle, game.ped});
          rammed = by && by->r0();
        }
        if (!rammed)
          return;
        Invoke(ctx, base, "CLEAR_CAR_LAST_DAMAGE_ENTITY", {handle});
        auto slot = std::find_if(g_rammed.begin(), g_rammed.end(), [&](const Rammed& r) { return r.car == handle; });
        if (slot == g_rammed.end())
          slot = std::min_element(g_rammed.begin(), g_rammed.end(),
                                  [](const Rammed& a, const Rammed& b) { return a.at < b.at; });
        if (slot->car != handle) {
          *slot = Rammed{handle, now, false};
          AddHeat(0.4f);
        }
        slot->at = now;
      });
      for (auto& r : g_rammed) {
        if (!r.car || r.wrecked || now - r.at > 10000 || !VehicleAddress(base, r.car))
          continue;  // (one the game has cleared away is not a wreck of ours)
        const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {r.car});
        if (dead && dead->r0()) {
          r.wrecked = true;
          AddHeat(1.0f);
        }
      }
    }
  }
  if ((buttons & X_INPUT_GAMEPAD_LEFT_SHOULDER) && now >= z.next_shot && !s.open) {
    z.next_shot = now + kLazarShotMs;
    FireLazar(ctx, base, z, s);
  }
  return kept;
}

// ---------------------------------------------------------------------------
// The DeLorean time machines (the cars of Time Travel IV 2.0a: Sersorroza's
// models, MrFusion92 and Lorenzo3024it's mod, whose .NET script can't run
// here; its time travel is re-created through natives). In Part I's, Part
// II's or Part III's car:
//  - d-pad down: the time circuits on or off (the TCD on screen: destination,
//    present and last time departed, the speedometer, the EMPTY lamp);
//  - the destination is set in the menu's Time Machine page (presets, or
//    month, day, year, hour and minute; the keypad beeps);
//  - at 88 mph, circuits on and the reactor charged: the build-up (the flux
//    capacitor quickening, a blue glow, sparks), then the car is gone in a
//    flash and a boom, leaving two trails of fire on the road (none in the
//    air); the game's clock goes to the destination's time; seconds later it
//    is back with a triple boom, frosted over, its vents steaming;
//  - after a trip the reactor is spent: L1 standing still reloads it
//    (plutonium in Part I, Mr. Fusion in Parts II and III);
//  - Part II: d-pad right folds the wheels and it flies (the left stick
//    drives and turns, R2 climbs, L2 sinks; d-pad right again lands it).
//    The flying car is the mod's hover model, swapped in on the spot.
// Those buttons are hidden from the game in the car (no radio on d-pad
// down / right, no drive-by on L1). The sounds are the trainer's own
// (gta4_time_machine.h): made from scratch, nothing from the films.
// ---------------------------------------------------------------------------
gta4::car::TimeMachineSynth g_tm_synth;
#if REX_PLATFORM_PS5
class TimeMachineSource final : public gta4ps5::sound::Source {
 public:
  size_t Read(float* lr, size_t frames) override {
    g_tm_synth.Render(lr, frames);
    return frames;
  }
  bool Rewind() override { return true; }
};
gta4ps5::sound::Voice* g_tm_voice = nullptr;
#endif
// Starts the time machine's voice (want), or stops it once quiet.
void TimeMachineVoice(bool want) {
#if REX_PLATFORM_PS5
  if (!g_tm_voice) {
    if (!want)
      return;
    gta4ps5::sound::StartOutput();
    g_tm_voice = gta4ps5::sound::NewVoice();
    g_tm_voice->game_sound = true;
  }
  if (want && !g_tm_voice->Active()) {
    g_tm_voice->SetVolume(1.0f);
    g_tm_voice->Play(std::make_unique<TimeMachineSource>(), true);
  } else if (!want && g_tm_voice->Active() && g_tm_synth.Quiet()) {
    g_tm_voice->Stop();
  }
#else
  (void)want;
#endif
}

constexpr float kMph88 = 88.0f * 0.44704f;  // m/s
constexpr float kMph60 = 60.0f * 0.44704f;
constexpr uint64_t kGoneMs = 2400;          // the car away between the departure and the arrival
constexpr int kTrailPoints = 9;             // fire points along each wheel's track
// Car space (the mod's body): its rear wheels' contact patches, the vents
// behind the cabin, the nose.
constexpr float kRearWheels[2][3] = {{-0.77f, -1.25f, -0.55f}, {0.77f, -1.25f, -0.55f}};
constexpr float kVents[2][3] = {{-0.45f, -1.55f, 0.55f}, {0.45f, -1.55f, 0.55f}};
constexpr float kNose[3] = {0.0f, 2.2f, 0.0f};

const char* const kMonths[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                 "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
int DaysIn(int year, int month) {
  static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 2 && leap ? 29 : kDays[std::clamp(month, 1, 12) - 1];
}
TcdDate TodaysDate() {
  const std::time_t t = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  return TcdDate{local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min};
}

struct TimeMachineState {
  uint32_t car = 0, model = 0;
  int film = 0;  // 1-3; 0: not in one
  bool in_car = false, hover_form = false;
  bool sink_stalled = false;  // landing, not coming down any further (no ground found)
  float last_z = 0, stalled_for = 0;
  int landing_log = 0;
  bool circuits = false, charged = true;
  TcdDate present, last;
  bool dated = false;
  int last_minutes = -1;
  // The trip: 0 ready; 1 gone; 2 just back (the frost and the vents).
  int phase = 0;
  uint64_t phase_at = 0;
  float charge = 0.0f, speed = 0.0f;
  bool flying = false;
  float at[3]{};          // where it left
  Quat q;                 // its attitude then
  float gone_speed = 0.0f;
  float tracks[2][kTrailPoints][3]{};  // the rear wheels' last points (a ring)
  int track_head = 0, track_count = 0;
  float last_track[3]{};
  std::array<uint32_t, 2 * kTrailPoints> fires{};
  uint64_t fires_out = 0;
  uint64_t next_spark = 0;
  uint32_t vents[2]{}, drips = 0;
  uint64_t vents_out = 0;
  int colours[2] = {-1, -1};
  uint64_t frost_until = 0;
  int booms_left = 0;
  uint64_t next_boom = 0;
  uint16_t prev_buttons = 0;
  uint64_t next_probe = 0, last_ms = 0;
  int log = 0;
  // Part II's flight: the wheels fold flat under the car (0 down .. 1 flat),
  // the same car flying (v1.7.21 swapped in a wheel-less model, DELHOV, and
  // swapping back failed: stuck in the flying form)
  float fold = 0.0f;
  bool fold_logged = false;
  std::array<int32_t, 4> wheel_bones{-1, -1, -1, -1};
  uint32_t wheel_bones_car = 0;
  bool hover = false, landing = false, swap_pending = false, models_requested = false;
  float altitude = 0.0f, heading = 0.0f, fwd = 0.0f, climb = 0.0f, bank = 0.0f, pitch = 0.0f;
  int hover_log = 0;
  // v1.7.24: a floor it has met (where it took off from, or where the
  // physics pushed it back up coming down), good within a few metres: the
  // ground probe misses some surfaces (a car park's deck read 14.7 m too
  // low), and the car, placed lower each frame, went through them.
  float floor_z = -1e9f, floor_x = 0.0f, floor_y = 0.0f;
  float placed_z = 0.0f, placed_vz = 0.0f, landing_for = 0.0f;
  float hold_z = 0.0f;  // the height it holds in the air (v1.7.30)
  bool placed = false;
  int floor_log = 0;
  // v1.7.30: a trip to a place (a map set) or back to the city from one;
  // home is where the car left the city.
  const gta4::addons::MapInfo* trip_map = nullptr;
  bool going_home = false, have_home = false, scene_loaded = false;
  float home[3]{};
  Quat home_q;
  uint64_t next_wait_toast = 0, scene_at = 0;
  // v1.7.31: on a trip to a place or home from one the car waits (hidden,
  // still) over where it is going, so the world there streams in and the
  // city's streams out (in v1.7.30 it waited in the city: the place's models
  // never got in); where it comes back to, once known, with the scene there
  // asked for first.
  bool moved = false, dest_set = false, unload_after = false;
  float dest[3]{};
  Quat dest_q;
  std::string arrive_toast;
  // v1.7.32: the departure plays out first (the car gone in its flash, the
  // trails burning where it left, kGoneMs), then the screen goes dark and
  // the car goes on ahead (v1.7.31 went dark at once: the departure cut off).
  uint64_t veil_at = 0;
};
TimeMachineState g_tm;
constexpr float kTmContact = 0.55f;  // its origin over the road on its wheels (the rear patches' depth)
std::atomic<bool> g_tm_in_car{false};

// What the screen shows (the TCD, the flash), published for the overlay.
struct TcdModel {
  bool visible = false, empty = false, hover = false;
  TcdDate dest, present, last;
  int mph = 0;
  float flash = 0.0f;  // 0..1, a white flash over the screen
  float veil = 0.0f;   // 0..1, black over the screen (a trip's wait for a place)
};
std::mutex g_tcd_mutex;
TcdModel g_tcd;
std::atomic<bool> g_tcd_active{false};
uint64_t g_flash_until = 0, g_flash_start = 0;
// The veil: from one level to another over a time (v1.7.31).
float g_veil_from = 0.0f, g_veil_to = 0.0f;
uint64_t g_veil_start = 0, g_veil_ms = 1;
float VeilLevel(uint64_t now) {
  const float t = std::clamp(float(now - g_veil_start) / float(std::max<uint64_t>(g_veil_ms, 1)), 0.0f, 1.0f);
  return g_veil_from + (g_veil_to - g_veil_from) * t;
}
void Veil(float to, uint64_t ms) {
  const uint64_t now = NowMs();
  g_veil_from = VeilLevel(now);
  g_veil_to = to;
  g_veil_start = now;
  g_veil_ms = std::max<uint64_t>(ms, 1);
}

// Puts a car exactly there (its own origin), or as SET_CAR_COORDINATES puts it.
void PlaceCar(PPCContext& ctx, uint8_t* base, uint32_t car, float x, float y, float z) {
  Invoke(ctx, base, NativeKnown("SET_CAR_COORDINATES_NO_OFFSET") ? "SET_CAR_COORDINATES_NO_OFFSET" : "SET_CAR_COORDINATES",
         {car, Bits(x), Bits(y), Bits(z)});
}

int TimeMachineFilm(uint32_t model) {
  static const uint32_t one = ModelHash("DELONE"), two = ModelHash("DELTWO"), three = ModelHash("DELTHR"),
                        hover = ModelHash("DELHOV");
  if (model == one) return 1;
  if (model == two || model == hover) return 2;
  if (model == three) return 3;
  return 0;
}

void Flash(uint64_t ms) {
  const uint64_t now = NowMs();
  g_flash_start = now;
  g_flash_until = now + ms;
}

// The game's clock read for the TCD's present: the date moves on a day each
// time the clock passes midnight.
void FollowClock(PPCContext& ctx, uint8_t* base, TimeMachineState& m) {
  const auto t = Invoke(ctx, base, "GET_TIME_OF_DAY", {0, 0}, 0, 0, 2);
  if (!t)
    return;
  const int hour = int(t->words[0]) % 24, minute = int(t->words[1]) % 60, minutes = hour * 60 + minute;
  if (!m.dated) {
    m.dated = true;
    m.present = TodaysDate();
    m.last = m.present;
  } else if (m.last_minutes >= 0 && minutes + 6 * 60 < m.last_minutes) {
    if (++m.present.day > DaysIn(m.present.year, m.present.month)) {
      m.present.day = 1;
      if (++m.present.month > 12)
        m.present.month = 1, ++m.present.year;
    }
  }
  m.last_minutes = minutes;
  m.present.hour = hour;
  m.present.minute = minute;
}

// Every frame of the trip and the build-up: where the rear wheels have been
// (for the fire trails), a point every couple of metres.
void TrackWheels(PPCContext& ctx, uint8_t* base, TimeMachineState& m) {
  const auto c = CarPosition(ctx, base, m.car);
  if (!c)
    return;
  float d2 = 0;
  for (int a = 0; a < 3; ++a)
    d2 += ((*c)[a] - m.last_track[a]) * ((*c)[a] - m.last_track[a]);
  if (m.track_count && d2 < 2.5f * 2.5f)
    return;
  for (int a = 0; a < 3; ++a)
    m.last_track[a] = (*c)[a];
  for (int w = 0; w < 2; ++w)
    if (const auto p = CarPoint(ctx, base, m.car, kRearWheels[w]))
      for (int a = 0; a < 3; ++a)
        m.tracks[w][m.track_head][a] = (*p)[a];
  m.track_head = (m.track_head + 1) % kTrailPoints;
  m.track_count = std::min(m.track_count + 1, kTrailPoints);
}

void ClearFires(PPCContext& ctx, uint8_t* base, TimeMachineState& m) {
  for (auto& f : m.fires) {
    if (f)
      Invoke(ctx, base, "REMOVE_SCRIPT_FIRE", {f});
    f = 0;
  }
  m.fires_out = 0;
}

void StopVents(PPCContext& ctx, uint8_t* base, TimeMachineState& m) {
  for (auto& v : m.vents) {
    if (v)
      Invoke(ctx, base, "STOP_PTFX", {v});
    v = 0;
  }
  if (m.drips)
    Invoke(ctx, base, "STOP_PTFX", {m.drips});
  m.drips = 0;
  m.vents_out = 0;
}

// Where a trip goes (v1.7.30): to a place (the destination's map set,
// loaded now if it is not yet), back to the city from one (the destination
// not that place), or only through time (the car comes back where it left).
const gta4::addons::MapInfo* FindMap(std::string_view name);
bool MapRequest(PPCContext& ctx, uint8_t* base, const gta4::addons::MapInfo& info);
bool OnMap(const gta4::addons::MapInfo& m, const std::array<float, 3>& p, float margin);
std::array<float, 3> MapPoint(const gta4::addons::MapInfo& m, float x, float y, float z);
struct MapRun;
const gta4::addons::MapInfo* MapStandingNow();
void PlanTrip(PPCContext& ctx, uint8_t* base, TimeMachineState& m, State& s) {
  m.trip_map = nullptr;
  m.going_home = false;
  m.scene_loaded = false;
  const auto* dest = s.tm_map.empty() ? nullptr : FindMap(s.tm_map);
  const auto* here = MapStandingNow();
  const std::array<float, 3> at{m.at[0], m.at[1], m.at[2]};
  const bool on_here = here && OnMap(*here, at, 120.0f);
  if (!s.tm_map.empty() && (!dest || !dest->installed)) {
    Toast(s, "That place isn't installed in this launch: a trip through time only.", 3000);
    REXLOG_WARN("gta4-trainer: time machine: the place {} is not installed ({}); time only", s.tm_map,
                dest ? "its files or lines were not added this launch" : "no such map set");
    dest = nullptr;
  }
  if (dest && dest != (on_here ? here : nullptr)) {
    if (!on_here) {  // (from the city: home is here)
      for (int a = 0; a < 3; ++a)
        m.home[a] = m.at[a];
      m.home_q = m.q;
      m.have_home = true;
    }
    m.trip_map = dest;
    MapRequest(ctx, base, *dest);
  } else if (!dest && on_here) {
    m.going_home = m.have_home;
  }
  REXLOG_INFO("gta4-trainer: time machine trip: {}", m.trip_map ? "to " + m.trip_map->name
                                                     : m.going_home ? std::string("home from ") + here->name
                                                     : std::string("through time only"));
}

// 88 mph: the car is gone, its trails burning behind it.
void Depart(PPCContext& ctx, uint8_t* base, const GameContext& game, TimeMachineState& m, State& s) {
  const auto c = CarPosition(ctx, base, m.car);
  const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {m.car, 0, 0, 0, 0}, 1, 0, 4);
  if (!c || !q)
    return;
  for (int a = 0; a < 3; ++a)
    m.at[a] = (*c)[a];
  m.q = {q->f(0), q->f(1), q->f(2), q->f(3)};
  m.gone_speed = std::max(m.speed, kMph88);
  m.flying = m.hover;
  PlanTrip(ctx, base, m, s);
  g_tm_synth.SetCharge(0.0f);
  g_tm_synth.Depart(m.flying);
  if (const auto nose = CarPoint(ctx, base, m.car, kNose))
    WorldEffect(ctx, base, "qub_lg_explode_blue", nose->data(), 1.6f);
  Flash(380);
  // The fire trails: where the rear wheels ran, both sides (none in the air).
  ClearFires(ctx, base, m);
  int lit = 0;
  if (!m.flying)
    for (int w = 0; w < 2; ++w)
      for (int k = 0; k < m.track_count; ++k) {
        float p[3];
        for (int a = 0; a < 3; ++a)
          p[a] = m.tracks[w][k][a];
        if (const auto gz = GroundZ(ctx, base, p[0], p[1], p[2] + 1.5f); gz && std::fabs(*gz - p[2]) < 2.0f)
          p[2] = *gz;
        const auto f = Invoke(ctx, base, "START_SCRIPT_FIRE", {Bits(p[0]), Bits(p[1]), Bits(p[2]), 0, 0});
        if (f && f->r0())
          m.fires[size_t(w * kTrailPoints + k)] = f->r0(), ++lit;
      }
  m.fires_out = NowMs() + 7000;
  // Gone: hidden, still, passing through nothing, the driver with it.
  Invoke(ctx, base, "SET_CAR_VISIBLE", {m.car, 0});
  Invoke(ctx, base, "SET_CAR_COLLISION", {m.car, 0});
  Invoke(ctx, base, "FREEZE_CAR_POSITION", {m.car, 1});
  Invoke(ctx, base, "SET_CHAR_VISIBLE", {game.ped, 0});
  // To a place, or home from one: once the departure has played out (see
  // TimeMachine), the car waits high over where it is going (v1.7.31), so
  // the world there streams in and the city's out, the screen dark.
  m.moved = m.dest_set = m.unload_after = false;
  m.veil_at = 0;
  m.arrive_toast.clear();
  // The time circuits: the clock goes to the destination's time; the
  // present becomes the destination, the last time departed the old present.
  m.last = m.present;
  m.present = s.tm_dest;
  m.last_minutes = s.tm_dest.hour * 60 + s.tm_dest.minute;
  Invoke(ctx, base, "SET_TIME_OF_DAY", {uint32_t(s.tm_dest.hour), uint32_t(s.tm_dest.minute)});
  m.charged = false;
  m.phase = 1;
  m.phase_at = NowMs();
  m.track_count = 0;
  REXLOG_INFO("gta4-trainer: time machine (Part {}) departed at {:.0f} mph{} for {} {} {:04d} {:02d}:{:02d}; {} fire "
              "points",
              m.film, m.speed / 0.44704f, m.flying ? " in the air" : "", kMonths[std::clamp(s.tm_dest.month, 1, 12) - 1],
              s.tm_dest.day, s.tm_dest.year, s.tm_dest.hour, s.tm_dest.minute, lit);
}

// Back: where it left (a little further on, when the road is there), moving
// as it was, with a triple boom; frosted over, its vents steaming.
void Arrive(PPCContext& ctx, uint8_t* base, const GameContext& game, TimeMachineState& m) {
  const uint64_t away = NowMs() - m.phase_at;
  const bool veiled = m.moved || VeilLevel(NowMs()) > 0.0f;
  if (veiled)
    Veil(0.0f, 450);
  m.moved = false;
  if (!VehicleAddress(base, m.car)) {  // (v1.7.30's crash: the car was gone, its handle stale)
    Invoke(ctx, base, "SET_CHAR_VISIBLE", {game.ped, 1});
    m.phase = 0;
    REXLOG_WARN("gta4-trainer: time machine: the car was gone after {} ms away; no arrival", away);
    return;
  }
  float p[3] = {m.at[0], m.at[1], m.at[2]};
  // a little further along its line
  const float fx = 2.0f * (m.q.x * m.q.y - m.q.w * m.q.z), fy = 1.0f - 2.0f * (m.q.x * m.q.x + m.q.z * m.q.z);
  const float ahead[3] = {p[0] + fx * 12.0f, p[1] + fy * 12.0f, p[2]};
  if (!m.flying) {
    if (const auto gz = GroundZ(ctx, base, ahead[0], ahead[1], ahead[2] + 2.0f); gz && std::fabs(*gz + 0.6f - p[2]) < 1.5f)
      p[0] = ahead[0], p[1] = ahead[1];
  } else {
    p[0] = ahead[0], p[1] = ahead[1];
  }
  PlaceCar(ctx, base, m.car, p[0], p[1], p[2]);
  Invoke(ctx, base, "SET_VEHICLE_QUATERNION", {m.car, Bits(m.q.x), Bits(m.q.y), Bits(m.q.z), Bits(m.q.w)});
  Invoke(ctx, base, "FREEZE_CAR_POSITION", {m.car, 0});
  Invoke(ctx, base, "SET_CAR_COLLISION", {m.car, 1});
  Invoke(ctx, base, "SET_CAR_VISIBLE", {m.car, 1});
  Invoke(ctx, base, "SET_CHAR_VISIBLE", {game.ped, 1});
  Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {m.car, Bits(m.hover ? m.fwd : m.gone_speed * 0.92f)});
  g_tm_synth.Arrive();
  m.booms_left = 3;
  m.next_boom = NowMs() + (veiled ? 300 : 0);  // (out of the dark, the three flashes seen)
  // Frost: its stainless paint goes icy (put back in a while).
  if (const auto cols = Invoke(ctx, base, "GET_CAR_COLOURS", {m.car, 0, 0}, 1, 0, 2)) {
    m.colours[0] = int(cols->words[0]);
    m.colours[1] = int(cols->words[1]);
    Invoke(ctx, base, "SET_CAR_COLOURS", {m.car, 24, 24});
    m.frost_until = NowMs() + 14000;
  }
  m.phase = 2;
  m.phase_at = NowMs();
  REXLOG_INFO("gta4-trainer: time machine back after {} ms ({:.1f}, {:.1f}, {:.1f})", away, p[0], p[1], p[2]);
}

// The car gone while away (v1.7.30: its driver got out during a long wait,
// and the world, which a spawned car is handed to, took the hidden car at
// once): no arrival to make. The driver is seen again; if the car had gone on
// ahead to where it was going, he is put back where it left.
void TripLost(PPCContext& ctx, uint8_t* base, const GameContext& game, TimeMachineState& m, State& s) {
  REXLOG_WARN("gta4-trainer: time machine: the car was gone after {} ms away; the trip ended there",
              NowMs() - m.phase_at);
  Invoke(ctx, base, "SET_CHAR_VISIBLE", {game.ped, 1});
  if (m.moved)
    Teleport(ctx, base, game, m.at[0], m.at[1], m.at[2], s);
  if (VeilLevel(NowMs()) > 0.0f)
    Veil(0.0f, 500);
  m.moved = m.dest_set = m.unload_after = m.going_home = false;
  m.trip_map = nullptr;
  m.arrive_toast.clear();
  m.phase = 0;
  Toast(s, "The time machine was lost on the way.", 3000);
}

// The cold vents and the frost after a trip; the trails going out.
void AfterTrip(PPCContext& ctx, uint8_t* base, TimeMachineState& m, uint64_t now) {
  if (m.booms_left > 0 && now >= m.next_boom) {
    // the three flashes with the three booms
    if (const auto c = CarPosition(ctx, base, m.car))
      WorldEffect(ctx, base, "qub_lg_explode_blue", c->data(), 1.3f);
    Flash(220);
    --m.booms_left;
    m.next_boom = now + 170;
    if (m.booms_left == 0) {
      g_tm_synth.Vents();
      for (int v = 0; v < 2; ++v) {
        const auto r = Invoke(ctx, base, "START_PTFX_ON_VEH",
                              {0, m.car, Bits(kVents[v][0]), Bits(kVents[v][1]), Bits(kVents[v][2]), Bits(-30.0f), 0,
                               Bits(v ? 25.0f : -25.0f), Bits(1.2f)},
                              -1, 0, 1, 0, "shot_directed_steam");
        m.vents[v] = r ? r->r0() : 0;
      }
      if (const auto r = Invoke(ctx, base, "START_PTFX_ON_VEH",
                                {0, m.car, 0, 0, Bits(0.4f), 0, 0, 0, Bits(1.0f)}, -1, 0, 1, 0, "water_carwash_drips"))
        m.drips = r->r0();
      m.vents_out = now + 7000;
    }
  }
  if (m.vents_out && now >= m.vents_out)
    StopVents(ctx, base, m);
  if (m.fires_out && now >= m.fires_out)
    ClearFires(ctx, base, m);
  if (m.frost_until && now >= m.frost_until) {
    m.frost_until = 0;
    if (m.colours[0] >= 0 && VehicleAddress(base, m.car))
      Invoke(ctx, base, "SET_CAR_COLOURS", {m.car, uint32_t(m.colours[0]), uint32_t(m.colours[1])});
  }
  if (m.phase == 2 && now - m.phase_at > 8000)
    m.phase = 0;
}

// The build-up towards 88: a blue glow, sparks around the body.
void BuildUp(PPCContext& ctx, uint8_t* base, TimeMachineState& m, uint64_t now) {
  if (m.charge < 0.25f)
    return;
  if (const auto c = CarPosition(ctx, base, m.car)) {
    const float k = (m.charge - 0.25f) / 0.75f;
    Invoke(ctx, base, "DRAW_LIGHT_WITH_RANGE",
           {Bits((*c)[0]), Bits((*c)[1]), Bits((*c)[2] + 0.4f), 120, 170, 255, Bits(3.0f + 6.0f * k),
            Bits(1.0f + 5.0f * k)});
  }
  if (now >= m.next_spark) {
    m.next_spark = now + uint64_t(260 - 190 * m.charge);
    static uint32_t seed = 0x88u;
    seed = seed * 1664525u + 1013904223u;
    const float x = (float((seed >> 8) & 0xFF) / 255.0f - 0.5f) * 1.8f;
    const float y = (float((seed >> 16) & 0xFF) / 255.0f - 0.5f) * 4.0f;
    const float at[3] = {x, y, 0.2f + 0.5f * float((seed >> 24) & 0x7) / 7.0f};
    const bool sparked = CarEffect(ctx, base, m.car, "ambient_electrical_sparks", at, 0.8f + 0.8f * m.charge);
    static int s_spark_log = 0;
    if (s_spark_log < 2) {
      ++s_spark_log;
      REXLOG_INFO("gta4-trainer: time machine build-up at {:.0f} mph (sparks {})", m.speed / 0.44704f,
                  sparked ? "started" : "not started");
    }
  }
}

// Part II: the wheels folded (the hover model swapped in) or down again.
// The new car takes the old one's place, attitude and speed, the player in
// its seat; the old one goes.
bool SwapForm(PPCContext& ctx, uint8_t* base, const GameContext& game, TimeMachineState& m, bool to_flight, State& s) {
  const uint32_t want = ModelHash(to_flight ? "DELHOV" : "DELTWO");
  const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {want});
  if (!loaded || !loaded->r0()) {
    Invoke(ctx, base, "REQUEST_MODEL", {want});
    Toast(s, to_flight ? "Flight conversion loading..." : "Wheels loading...", 900);
    return false;
  }
  const auto c = CarPosition(ctx, base, m.car);
  const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {m.car, 0, 0, 0, 0}, 1, 0, 4);
  if (!c || !q)
    return false;
  float speed = 0.0f;
  if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {m.car, 0}, 1))
    speed = v->f(0);
  const float lift = to_flight ? 0.35f : 0.0f;
  const auto made = Invoke(ctx, base, "CREATE_CAR", {want, Bits((*c)[0]), Bits((*c)[1]), Bits((*c)[2] + lift), 0, 1}, 4);
  if (!made || !made->words[0])
    return false;
  const uint32_t old = m.car, car = made->words[0];
  Invoke(ctx, base, "SET_VEHICLE_QUATERNION", {car, q->words[0], q->words[1], q->words[2], q->words[3]});
  PlaceCar(ctx, base, car, (*c)[0], (*c)[1], (*c)[2] + lift);
  // its colours as the old one's
  if (const auto cols = Invoke(ctx, base, "GET_CAR_COLOURS", {old, 0, 0}, 1, 0, 2))
    Invoke(ctx, base, "SET_CAR_COLOURS", {car, cols->words[0], cols->words[1]});
  Invoke(ctx, base, "WARP_CHAR_INTO_CAR", {game.ped, car});
  // Only once the player is in the new one does the old one go.
  const auto now_in = Invoke(ctx, base, "GET_CAR_CHAR_IS_USING", {game.ped, 0}, 1);
  if (!now_in || now_in->words[0] != car) {
    Invoke(ctx, base, "DELETE_CAR", {0}, 0, car);
    REXLOG_WARN("gta4-trainer: time machine (Part II): the player could not be put in the {} form",
                to_flight ? "flying" : "driving");
    return false;
  }
  if (!to_flight)  // (on its wheels, standing on the ground, not in it)
    Invoke(ctx, base, "SET_CAR_ON_GROUND_PROPERLY", {car});
  Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {car, Bits(speed)});
  Invoke(ctx, base, "DELETE_CAR", {0}, 0, old);
  Invoke(ctx, base, "MARK_CAR_AS_NO_LONGER_NEEDED", {0}, 0, car);
  WorldEffect(ctx, base, "veh_respray_smoke", c->data(), 0.6f);
  m.car = car;
  m.hover_form = to_flight;
  g_pass_vehicle.pass = ~0ull;  // (the pass's vehicle changed: look again)
  g_tm_synth.Convert(to_flight);
  REXLOG_INFO("gta4-trainer: time machine (Part II) {} (car {:08X} -> {:08X})",
              to_flight ? "wheels folded: flying" : "wheels down: driving", old, car);
  return true;
}

// Part II's wheels folding flat (as in the film: each turns a quarter about
// the car's length, tread down) or coming down again: the wheel bones posed
// in the car's skeleton, re-applied after the game's own per-frame bone
// edits. At 0 the bones are handed back to the game (rest pose written once).
const std::array<std::string_view, 4> kWheelBoneNames{"wheel_lf", "wheel_rf", "wheel_lr", "wheel_rr"};
void FoldWheels(PPCContext& ctx, uint8_t* base, TimeMachineState& m, float fold) {
  const uint32_t vehicle = VehicleAddress(base, m.car);
  const uint32_t info = vehicle ? FindModelInfo(base, m.model) : 0;
  if (!info || !VehicleIsModel(base, vehicle, info))
    return;
  if (m.wheel_bones_car != m.car) {
    m.wheel_bones_car = m.car;
    for (size_t k = 0; k < 4; ++k)
      m.wheel_bones[k] = FindBone(ctx, base, vehicle, kWheelBoneNames[k]);
  }
  int posed = 0;
  for (size_t k = 0; k < 4; ++k) {
    if (m.wheel_bones[k] < 0)
      continue;
    const float side = (k % 2 == 0) ? -1.0f : 1.0f;  // left wheels one way, right the other
    if (PoseBone(ctx, base, vehicle, info, kPoseDelorean, 4 + int(k), m.wheel_bones[k], 1, side * 90.0f * fold,
                 1.0f))
      ++posed;
  }
  if (fold <= 0.0f) {  // down: the game's again (it spins and steers them)
    std::lock_guard lock(g_pose_mutex);
    auto& posed_car = g_posed[size_t(kPoseDelorean)];
    if (posed_car.vehicle == vehicle)
      for (size_t k = 4; k < 8; ++k)
        posed_car.bones[k] = BonePose{};
  }
  if (!m.fold_logged) {
    m.fold_logged = true;
    REXLOG_INFO("gta4-trainer: time machine (Part II): wheel bones {} {} {} {}, {} posed",
                m.wheel_bones[0], m.wheel_bones[1], m.wheel_bones[2], m.wheel_bones[3], posed);
  }
}

// Part II in the air: the left stick drives and turns, R2 climbs, L2 sinks;
// it hovers where it is with nothing pressed. Its attitude (the heading, a
// lean into the turn and the acceleration) and its speed along the nose
// are set every frame as the F-16's are; its height is a held target, moved
// only by R2 / L2 (and the ground under it). v1.7.29 took each frame's height
// from where the physics had left it: leaning forward to speed up pointed
// its speed down, and it sank to the ground and stuck there unless R2 was
// held as well.
void Hover(PPCContext& ctx, uint8_t* base, TimeMachineState& m, const DrivePad& pad, float dt) {
  const auto c = CarPosition(ctx, base, m.car);
  if (!c)
    return;
  const float lx = Axis(pad.lx), ly = Axis(pad.ly);
  const float up = pad.right_trigger / 255.0f - pad.left_trigger / 255.0f;
  const float want_fwd = ly >= 0 ? ly * 48.0f : ly * 12.0f;
  const float accel = std::clamp(want_fwd - m.fwd, -18.0f * dt, 12.0f * dt);
  m.fwd += accel;
  m.heading -= lx * 1.25f * dt * (0.4f + 0.6f * std::min(1.0f, std::fabs(m.fwd) / 15.0f));
  const float want_climb = m.landing ? -4.0f : up * 8.0f;
  m.climb += std::clamp(want_climb - m.climb, -14.0f * dt, 14.0f * dt);
  m.bank += (lx * 0.35f - m.bank) * std::min(1.0f, 4.0f * dt);
  m.pitch += (std::clamp(-accel / std::max(dt, 1e-3f) * 0.004f + m.climb * 0.02f, -0.25f, 0.25f) - m.pitch) *
             std::min(1.0f, 3.0f * dt);
  if (!m.placed)
    m.hold_z = (*c)[2];
  // (pushed up by something it met: it stays up)
  if ((*c)[2] > m.hold_z + 0.6f)
    m.hold_z = (*c)[2];
  m.hold_z += m.climb * dt;
  float z = m.hold_z;
  // The ground under it, and where it will be in half a second (a kerb, a
  // slope, a wall's foot): kept 1.2 m clear of the higher, looked for from
  // well above, so a car that has sunk in still finds the surface over it.
  // (v1.7.21 looked only under it, from 1 m up, and kept 0.85 m: it caught
  // on rising ground and could end up stuck in it.)
  std::optional<float> gz = GroundZ(ctx, base, (*c)[0], (*c)[1], (*c)[2] + 6.0f);
  {
    const float ahead = std::clamp(m.fwd, -10.0f, 48.0f) * 0.5f;
    const float hx = -std::sin(m.heading) * ahead, hy = std::cos(m.heading) * ahead;
    if (const auto g2 = GroundZ(ctx, base, (*c)[0] + hx, (*c)[1] + hy, (*c)[2] + 6.0f))
      if (!gz || *g2 > *gz)
        gz = g2;
  }
  // Coming down, pushed back up by the physics: something solid is there
  // (the probe missed it): a floor, from here.
  // (what its speed along a pitched nose moved it, allowed for)
  if (m.placed && m.climb < -0.5f && (*c)[2] > m.placed_z + m.placed_vz * dt + 0.04f) {
    m.floor_z = (*c)[2] - kTmContact;
    m.floor_x = (*c)[0];
    m.floor_y = (*c)[1];
    if (m.floor_log < 4) {
      ++m.floor_log;
      REXLOG_INFO("gta4-trainer: time machine: something solid under it at {:.1f} (the probe's ground {:.1f})",
                  m.floor_z, gz ? *gz : -999.0f);
    }
  }
  // The floor it met counts within 6 m of where it met it.
  {
    const float dx = (*c)[0] - m.floor_x, dy = (*c)[1] - m.floor_y;
    if (m.floor_z > -1e8f && dx * dx + dy * dy < 36.0f && (!gz || m.floor_z > *gz))
      gz = m.floor_z;
  }
  constexpr float kHoverClear = 1.2f;
  if (gz && z < *gz + kHoverClear) {
    z = *gz + kHoverClear;
    m.hold_z = z;
    m.climb = std::max(m.climb, 0.0f);
  }
  m.altitude = gz ? z - *gz : 99.0f;
  // (landing with no ground found: it has stopped coming down)
  m.sink_stalled = m.landing && dt > 0.0f && (*c)[2] > m.last_z - 0.5f * 4.0f * dt;
  m.last_z = (*c)[2];
  const Quat q = QNormalize(QMul(QMul(QAxis(0, 0, 1, m.heading), QAxis(1, 0, 0, m.pitch)), QAxis(0, 1, 0, m.bank)));
  Invoke(ctx, base, "SET_VEHICLE_QUATERNION", {m.car, Bits(q.x), Bits(q.y), Bits(q.z), Bits(q.w)});
  PlaceCar(ctx, base, m.car, (*c)[0], (*c)[1], z);
  m.placed = true;
  m.placed_z = z;
  {
    const float nose[3] = {0.0f, 1.0f, 0.0f};
    float f[3];
    QRotate(q, nose, f);
    m.placed_vz = m.fwd * f[2];
  }
  Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {m.car, Bits(m.fwd)});
  g_tm_synth.SetHover(0.35f + 0.45f * std::min(1.0f, std::fabs(m.fwd) / 40.0f) + 0.2f * std::fabs(up));
  if (m.hover_log < 4 && dt > 0.0f) {
    ++m.hover_log;
    REXLOG_INFO("gta4-trainer: time machine flying: {:.1f} m up, {:.1f} m/s ahead, climbing {:.1f} m/s", m.altitude,
                m.fwd, m.climb);
  }
}

// ---------------------------------------------------------------------------
// Map destinations (v1.7.30): places the DeLorean travels to, the installer's
// map sets (gta4_addon_install.h). Chosen and confirmed in the Time Machine
// page, a place loads in the background (its models requested). At 88 mph the
// car is gone until the place stands (its pieces as frozen objects, as the mod
// placed them, moved out to sea past the city, its sea level matched to the
// game's) and arrives on it. A trip from there to anywhere else brings the car
// back to where it left the city and deletes the place; Unload deletes it any
// time (taking the player home first if he is on it). One place at a time.
// ---------------------------------------------------------------------------
// The object pool (v1.7.37): the game makes it at boot (sub_82326E10) with
// room for 1300 objects of 800 bytes - [0x82DE5A50]: +0 the objects, +4 a
// flag byte each, +8 the count, +12 an object's size, +20 in use - and it
// holds every object the city streams in near the player, every one a script
// makes, and a place's pieces (sub_822B9B18 stops turning the city's dummies
// into objects under 50 free). The Gator Keys took 156; Wake Island takes
// about 1,400 (its own models, their tiles, the game's props it places, its
// collision carriers): the pool is made with room for 3400 (2.7 MiB in the
// game's heap for 1 MiB). Nothing else in the game holds its size: the one
// other 1300 in the code is a text buffer's.
extern "C" void sub_82326E10(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kObjectPoolPointer = 0x82DE5A50, kObjectPoolName = 0x82011C70, kObjectBytes = 800,
                     kStockObjects = 1300, kObjects = 3400;
  ctx.r3.u32 = 28;
  __imp__sub_821B3510(ctx, base);  // (the game's allocator: the pool object itself)
  const uint32_t pool = ctx.r3.u32;
  if (pool) {
    ctx.r3.u32 = pool;
    ctx.r4.u32 = kObjects;
    ctx.r5.u32 = kObjectPoolName;
    ctx.r6.u32 = kObjectBytes;
    __imp__sub_82507368(ctx, base);  // (the pool's constructor: count, name, an object's size)
  }
  Write(base, kObjectPoolPointer, pool);  // (as the game's own code does, whatever came back)
  ctx.r3.u32 = pool;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("gta4-trainer: object pool made with room for {} objects (the game's {}): {:08X}", kObjects,
                kStockObjects, pool);
  }
}

// The game's entity pools, for the log (v1.7.38: Wake Island's 1,600 pieces
// at once left the car in the sea and the world not streaming - a pool full
// would do that, as would its streaming memory). Each pool (sub_82507368):
// +0 the slots, +4 a flag byte per slot (bit 7 free), +8 the count; its
// pointer in a global. Counts (the 360's): objects 1300 (3400 here),
// buildings 32000, dummies 14000, pointer nodes 65536 single / 16000
// double, entry-info nodes 16000.
struct PoolGlobal {
  const char* name;
  uint32_t global;
};
constexpr PoolGlobal kPools[] = {
    {"objects", 0x82DE5A50},    {"buildings", 0x830248C4},     {"dummies", 0x830248C8},
    {"ptr-single", 0x830328D8}, {"ptr-double", 0x830328DC}, {"entry-info", 0x8305C5A4},
};
std::string DescribePools(uint8_t* base) {
  std::string out;
  for (const auto& pl : kPools) {
    if (!Span(base, pl.global, 4))
      continue;
    const uint32_t pool = Read(base, pl.global);
    if (!pool || !Span(base, pool, 28))
      continue;
    const uint32_t flags = Read(base, pool + 4), count = Read(base, pool + 8);
    if (!flags || count > 200000 || !Span(base, flags, count))
      continue;
    uint32_t used = 0;
    const uint8_t* f = rex::memory::GuestPtr(base, flags);
    for (uint32_t i = 0; i < count; ++i)
      used += !(f[i] & 0x80);
    out += fmt::format("{}{} {}/{}", out.empty() ? "" : ", ", pl.name, used, count);
  }
  return out.empty() ? std::string("pools not readable") : out;
}

struct MapStanding {
  uint32_t hash = 0, object = 0;
  int model = -1;  // in MapRun::models
  bool collision = false;
  bool stock = false;  // one of the game's own models (its collision the game's, not the set's)
  bool wanted = true;  // within the set's stream radius of the focus (v1.7.38; every piece of a set with none)
  float pos[3]{}, q[4]{};
};
struct MapModel {
  std::string name;
  uint32_t hash = 0;
  bool known = true, loaded = false;
  bool requested = false;  // asked of the game (REQUEST_MODEL) and not let go since
  int wanted = 0;          // its pieces within the stream radius
  int standing = 0;        // its pieces made (objects)
  int index = -1;  // in the model table (its streaming entry: the model module's first entry + this)
};
// One of the place's collision dictionaries (v1.7.34; several from v1.7.37, a
// big set's in dictionaries of ~3 MiB): its slot in the bounds store, asked
// for with the models and kept in (flag 2) till the place goes.
struct MapDictionary {
  std::string name;
  int slot = -1;
  bool in = false;
};
struct MapRun {
  const gta4::addons::MapInfo* info = nullptr;
  int phase = 0;  // 0 nothing; 1 loading (requested); 2 standing
  uint64_t requested_at = 0, loaded_at = 0, next_check = 0, next_renew = 0, unload_at = 0, next_report = 0;
  std::vector<MapModel> models;
  std::vector<MapStanding> pieces;
  bool loaded = false;
  int loaded_count = 0, known_count = 0, renewals = 0, logs = 0, reports = 0;
  uint64_t check_at = 0;  // (v1.7.32: when to look at the pieces after an arrival)
  std::vector<MapDictionary> dicts;
  uint64_t bounds_in_at = 0;  // when the last of them came in
  // Streaming by distance (v1.7.38; MapInfo::stream > 0): the point the
  // pieces are wanted about (the arrival till the player stands there, then
  // the player), and when to look again.
  float focus[3]{};
  uint64_t next_stream = 0;
  int wanted_pieces = 0, made_pieces = 0, stream_rounds = 0;
  // The guard (v1.7.40): the place stood under a player held over it (a
  // loaded game there, the menu's teleport to one of its spots).
  bool guarding = false;
  uint64_t guard_since = 0, guard_toast = 0;
  int probe = 0, probe_misses = 0;  // standing pieces looked for in turn (a piece gone: the world rebuilt)
  // Its safehouse (v1.7.40): the blip at its door; the bed's save point armed
  // till stood on, armed again once left.
  uint32_t house_blip = 0;
  bool bed_armed = true, bed_near = false;
  // Whether any dictionary is in the store; whether every one that is has loaded.
  bool AnyDictionary() const {
    return std::any_of(dicts.begin(), dicts.end(), [](const MapDictionary& d) { return d.slot >= 0; });
  }
  bool DictionariesIn() const {
    return std::all_of(dicts.begin(), dicts.end(), [](const MapDictionary& d) { return d.slot < 0 || d.in; });
  }
  bool IsDictionarySlot(uint16_t slot) const {
    return std::any_of(dicts.begin(), dicts.end(), [&](const MapDictionary& d) { return d.slot >= 0 && slot == uint16_t(d.slot); });
  }
  std::string DictionaryNames() const {
    std::string out;
    for (const auto& d : dicts)
      out += (out.empty() ? "" : ", ") + d.name + ".xbd";
    return out.empty() ? std::string("no dictionary") : out;
  }
};
MapRun g_map;
// (v1.7.43: ms from a moment to a tick's `now`, which can be a few ms older
// than a moment set by the menu's action in the same frame - never wrapped)
inline uint64_t Since(uint64_t now, uint64_t moment) { return now > moment ? now - moment : 0; }
constexpr uint64_t kMapLoadLimitMs = 45000;  // a trip waits this long for the place, then turns back
constexpr uint64_t kMapBoundsWaitMs = 20000;  // and this long for its collision dictionaries, then places it anyway

// ---- the game's streaming, read for the map's log (v1.7.31) ----
// Entries: 24 bytes each from [0x83032744]: +0 the resource's flags, +4 its
// image (top byte) and place, +8 its state (the top two bits: 0 out, 1 in,
// 2 asked for, 3 loading) and size, +23 its module. Modules: 100 bytes each
// from 0x82D515B8 (their count at +2500): +84 the file extension, +88 the
// first entry. sub_8250D328 (the table, an entry, an array) lists the entries
// an object needs in first, as the game's own requests do. Models are module
// [0x82B58604], their entries in the model table's order. A file whose name
// is over 24 characters with its extension is not registered at all
// (sub_825127C0 copies 24 and looks for the extension's dot in them).
constexpr uint32_t kStreamEntries = 0x83032744, kStreamModules = 0x82D515B8, kModelModuleIndex = 0x82B58604,
                   kStreamDependencies = 0x8250D328, kStreamManager = 0x82A9AA7C;
struct StreamEntry {
  uint32_t index = 0, flags = 0, where = 0, packed = 0;
  int module = -1;
  int state() const { return int(packed >> 30); }
  uint32_t size() const { return packed & 0x3FFFFFFF; }
};
int StreamModuleCount(uint8_t* base) {
  const uint32_t n = Span(base, kStreamModules + 2500, 4) ? Read(base, kStreamModules + 2500) : 0;
  return n <= 25 ? int(n) : 0;  // (room for 25: the count sits where a 26th would start)
}
std::optional<StreamEntry> ReadStreamEntry(uint8_t* base, uint32_t index) {
  if (index >= 0xFFFF || !Span(base, kStreamEntries, 4))
    return std::nullopt;
  const uint32_t table = Read(base, kStreamEntries), e = table + index * 24;
  if (!table || !Span(base, e, 24))
    return std::nullopt;
  StreamEntry out;
  out.index = index;
  out.flags = Read(base, e);
  out.where = Read(base, e + 4);
  out.packed = Read(base, e + 8);
  const int module = *rex::memory::GuestPtr(base, e + 23);
  out.module = module < StreamModuleCount(base) ? module : -1;
  return out;
}
std::string StreamModuleName(uint8_t* base, int module) {
  if (module < 0 || module >= StreamModuleCount(base))
    return "?";
  const uint32_t at = kStreamModules + uint32_t(module) * 100 + 84;
  std::string name;
  for (uint32_t i = 0; i < 4 && Span(base, at + i, 1); ++i) {
    const char c = char(*rex::memory::GuestPtr(base, at + i));
    if (c < 0x20 || c > 0x7E)
      break;
    name += c;
  }
  return name.empty() ? "?" : name;
}
uint32_t StreamModuleFirst(uint8_t* base, int module) {
  const uint32_t at = kStreamModules + uint32_t(module) * 100 + 88;
  return module >= 0 && Span(base, at, 4) ? Read(base, at) : 0;
}
std::string DescribeEntry(uint8_t* base, const StreamEntry& e) {
  static constexpr const char* kStates[4] = {"out", "in", "asked", "loading"};
  return fmt::format("{}#{} {} {} B (flags {:08X}, image {})", StreamModuleName(base, e.module),
                     e.index - StreamModuleFirst(base, e.module), kStates[e.state()], e.size(), e.flags,
                     e.where >> 24);
}
// The entries one needs in first (the game's own list).
std::vector<uint32_t> StreamDependencies(PPCContext& parent, uint8_t* base, uint32_t index) {
  auto* runtime = rex::Runtime::instance();
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  auto* function = dispatcher ? dispatcher->GetFunction(kStreamDependencies) : nullptr;
  if (!function || parent.r1.u32 < 0x200)
    return {};
  const uint32_t frame = (parent.r1.u32 - 0x200u) & ~15u, list = frame + 0x100;
  if (!Span(base, frame, 0x200, true))
    return {};
  Write(base, frame, parent.r1.u32);
  PPCContext call = parent;
  call.r1.u32 = frame;
  call.r3.u32 = kStreamModules;
  call.r4.u32 = index;
  call.r5.u32 = list;
  call.lr = 0x82844BA0;
  function(call, base);
  const uint32_t n = std::min<uint32_t>(call.r3.u32, 64);
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < n; ++i)
    out.push_back(Read(base, list + i * 4));
  return out;
}

// ---- collision dictionaries: how a model gets its bound (v1.7.34) ----
// The game links a model to its collision only when the bound dictionary
// (.xbd) holding it loads: the bounds store's set callback (sub_82288E90)
// walks the dictionary, and for each key that is a model's name hash
// (sub_821CA090) makes an archetype for the bound and calls the model's
// SetPhysics (sub_82360060: +0x04 the archetype, +0x52 the dictionary's
// slot). An object gets collision only from a model whose +0x52 is set
// (sub_82500400, sub_82478040 ask for that slot's dictionary; none, no
// physics). The links outlive the dictionary (when it goes, +0x04 is cleared,
// +0x52 kept), so at boot the game loads every dictionary once - unless its
// cache is good (sub_82289958): then only those in images outside the cache,
// the rest coming from the cache's records. vehicles.img is in it, so the
// island's dictionary was never loaded: log 097's pieces all had +0x52 0xFFFF
// (stock bm_plank01: 0x0097, rubbish.xbd's slot) and no collision. The
// trainer asks for it with the models now.
//   Bounds store pool [0x82C6C3F4]: +0 entries, +4 a flag byte each (0x80 a
//   free slot), +8 the count (330), +12 an entry's size. Entry: +0 the
//   dictionary while in, +4 references, +8 the name's hash, +16/+32 a box.
//   Module index [0x82A98C1C]. sub_821D00F0 (slot, module, flags) asks for an
//   object (flags 0x10 priority, 0x2 kept: not let go while set);
//   sub_821CFF60 (slot, module) clears 0x2 again.
constexpr uint32_t kBoundsPool = 0x82C6C3F4, kBoundsModuleIndex = 0x82A98C1C, kStreamRequest = 0x821D00F0,
                   kStreamUnkeep = 0x821CFF60, kModelInfoArchetype = 0x04, kModelInfoBoundsSlot = 0x52;
constexpr uint32_t kBoundsRequestFlags = 0x12;  // (as the game asks for a dictionary it needs now, sub_82289588)
struct BoundsPool {
  uint32_t entries = 0, flags = 0, count = 0, size = 0;
};
std::optional<BoundsPool> ReadBoundsPool(uint8_t* base) {
  if (!Span(base, kBoundsPool, 4))
    return std::nullopt;
  const uint32_t pool = Read(base, kBoundsPool);
  if (!pool || !Span(base, pool, 16))
    return std::nullopt;
  BoundsPool p{Read(base, pool), Read(base, pool + 4), Read(base, pool + 8), Read(base, pool + 12)};
  if (p.count == 0 || p.count > 4096 || p.size < 16 || p.size > 256 || !Span(base, p.flags, p.count) ||
      !Span(base, p.entries, p.count * p.size))
    return std::nullopt;
  return p;
}
// The slot of the dictionary named so (its name hash, as the store keeps it), or -1.
int BoundsSlot(uint8_t* base, std::string_view name) {
  const auto p = ReadBoundsPool(base);
  if (!p)
    return -1;
  const uint32_t hash = ModelHash(name);
  for (uint32_t i = 0; i < p->count; ++i)
    if (!(*rex::memory::GuestPtr(base, p->flags + i) & 0x80) && Read(base, p->entries + i * p->size + 8) == hash)
      return int(i);
  return -1;
}
// A slot's entry address, or 0.
uint32_t BoundsEntry(uint8_t* base, int slot) {
  const auto p = ReadBoundsPool(base);
  if (!p || slot < 0 || uint32_t(slot) >= p->count || (*rex::memory::GuestPtr(base, p->flags + uint32_t(slot)) & 0x80))
    return 0;
  return p->entries + uint32_t(slot) * p->size;
}
uint32_t BoundsModule(uint8_t* base) {
  return Span(base, kBoundsModuleIndex, 4) ? Read(base, kBoundsModuleIndex) : 0xFFFFFFFFu;
}
// The slot's streaming entry, described for the log.
std::string DescribeBoundsSlot(uint8_t* base, int slot) {
  const uint32_t module = BoundsModule(base), entry = BoundsEntry(base, slot);
  if (slot < 0 || !entry || module >= uint32_t(StreamModuleCount(base)))
    return "unreadable";
  const auto e = ReadStreamEntry(base, StreamModuleFirst(base, int(module)) + uint32_t(slot));
  return fmt::format("slot {}: {}, dictionary {:08X}, {} references", slot,
                     e ? DescribeEntry(base, *e) : std::string("its streaming entry unreadable"), Read(base, entry),
                     Read(base, entry + 4));
}

const gta4::addons::MapInfo* FindMap(std::string_view name) {
  for (const auto& m : gta4::addons::InstalledMaps())
    if (m.name == name)
      return &m;
  return nullptr;
}
// The mod's coordinates to the game's.
float MapDz(const gta4::addons::MapInfo& m) { return m.sea_dst - m.sea_src; }
std::array<float, 3> MapPoint(const gta4::addons::MapInfo& m, float x, float y, float z) {
  return {x + m.shift[0], y + m.shift[1], z + MapDz(m)};
}
// Whether a point (the game's coordinates) is over the place.
bool OnMap(const gta4::addons::MapInfo& m, const std::array<float, 3>& p, float margin = 120.0f) {
  return p[0] >= m.area[0] + m.shift[0] - margin && p[0] <= m.area[2] + m.shift[0] + margin &&
         p[1] >= m.area[1] + m.shift[1] - margin && p[1] <= m.area[3] + m.shift[1] + margin;
}

// What the place's loading is waiting for: each model not in yet, its own
// streaming entry and those it needs first (v1.7.31; v1.7.30's log showed
// none of the 56 in after two minutes and nothing more).
void MapReport(PPCContext& ctx, uint8_t* base, const char* why) {
  auto& g = g_map;
  if (!g.info)
    return;
  ++g.reports;
  const uint32_t mm = Span(base, kModelModuleIndex, 4) ? Read(base, kModelModuleIndex) : 0xFF;
  const uint32_t first = StreamModuleFirst(base, int(mm));
  std::string budget;
  if (Span(base, kStreamManager, 64))
    for (uint32_t w = 8; w < 16; ++w)
      budget += fmt::format(" {:X}", Read(base, kStreamManager + w * 4));
  REXLOG_INFO("gta4-trainer: map {} report ({}): {} of {} models in after {} ms; model module {} ({}), streaming "
              "manager words 8-15:{}",
              g.info->name, why, g.loaded_count, g.known_count, NowMs() - g.requested_at, mm,
              StreamModuleName(base, int(mm)), budget);
  int shown = 0;
  for (const auto& mdl : g.models) {
    if (!mdl.known || mdl.loaded || mdl.index < 0 || mdl.wanted == 0)
      continue;
    if (++shown > 4)
      break;
    const uint32_t index = first + uint32_t(mdl.index);
    const auto e = ReadStreamEntry(base, index);
    if (!e) {
      REXLOG_INFO("gta4-trainer:   {} (model {}): its streaming entry not readable", mdl.name, mdl.index);
      continue;
    }
    std::string needs;
    for (const uint32_t d : StreamDependencies(ctx, base, index))
      if (const auto de = ReadStreamEntry(base, d))
        needs += "; " + DescribeEntry(base, *de);
    REXLOG_INFO("gta4-trainer:   {} (model {}): {}{}", mdl.name, mdl.index, DescribeEntry(base, *e),
                needs.empty() ? std::string("; needs nothing listed") : needs);
  }
}

void LifeStop(PPCContext& ctx, uint8_t* base, const char* why);
void MapDropPiece(PPCContext& ctx, uint8_t* base, MapStanding& p);
void MapUnload(PPCContext& ctx, uint8_t* base, const char* why) {
  auto& g = g_map;
  if (!g.info)
    return;
  LifeStop(ctx, base, why);
  int deleted = 0;
  for (auto& p : g.pieces)
    if (p.object) {
      MapDropPiece(ctx, base, p);
      ++deleted;
    }
  int let_go = 0;
  for (auto& mdl : g.models)
    if (mdl.known && mdl.requested) {
      Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {mdl.hash});
      mdl.requested = false;
      ++let_go;
    }
  if (g.house_blip) {
    if (const auto e = Invoke(ctx, base, "DOES_BLIP_EXIST", {g.house_blip}); e && e->r0())
      Invoke(ctx, base, "REMOVE_BLIP", {g.house_blip});
    g.house_blip = 0;
  }
  // (its collision dictionaries no longer kept: the store lets them go once
  // the pieces' references to them are gone, the models' links staying)
  const uint32_t module = BoundsModule(base);
  std::string kept;
  for (const auto& d : g.dicts)
    if (d.slot >= 0 && module < uint32_t(StreamModuleCount(base))) {
      CallGuest(ctx, base, kStreamUnkeep, uint32_t(d.slot), module);
      kept += (kept.empty() ? ", " : " ") + d.name + ".xbd";
    }
  REXLOG_INFO("gta4-trainer: map {} unloaded ({}): {} pieces deleted, {} models let go{}", g.info->name, why, deleted,
              let_go, kept.empty() ? std::string() : kept + " no longer kept");
  g = MapRun{};
}

// The place's collision dictionaries asked for (kept in till the place goes).
void MapRequestBounds(PPCContext& ctx, uint8_t* base) {
  auto& g = g_map;
  if (!g.info)
    return;
  if (g.dicts.empty())
    for (const auto& name : g.info->bounds)
      g.dicts.push_back(MapDictionary{name});
  const uint32_t module = BoundsModule(base);
  for (auto& d : g.dicts) {
    if (d.slot < 0)
      d.slot = BoundsSlot(base, d.name);
    if (d.slot < 0 || module >= uint32_t(StreamModuleCount(base))) {
      if (g.renewals == 0)
        REXLOG_WARN("gta4-trainer: map {}: its collision dictionary {}.xbd is not in the game's bounds store (module "
                    "{}); the pieces in it will have no collision",
                    g.info->name, d.name, int(module));
      continue;
    }
    CallGuest(ctx, base, kStreamRequest, uint32_t(d.slot), module, kBoundsRequestFlags);
  }
}
// The dictionaries' streaming state, for the log.
std::string DescribeDictionaries(uint8_t* base) {
  std::string out;
  for (const auto& d : g_map.dicts)
    out += fmt::format("{}{}.xbd {}", out.empty() ? "" : "; ", d.name,
                       d.slot < 0 ? std::string("not in the store") : DescribeBoundsSlot(base, d.slot));
  return out.empty() ? std::string("none") : out;
}

// How many of the place's models with collision the game has linked to their
// bounds (their +0x52 one of the set's dictionaries' slots; a game model's any
// slot), of how many; and whether those have their archetypes now (the
// dictionary in).
struct MapLinks {
  int models = 0, linked = 0, archetypes = 0;
  uint32_t first_unlinked = 0;
  uint16_t first_unlinked_slot = 0xFFFF;
};
MapLinks MapCountLinks(uint8_t* base) {
  auto& g = g_map;
  MapLinks out;
  std::vector<bool> seen(g.models.size(), false);
  for (const auto& p : g.pieces) {
    if (!p.collision || p.model < 0 || size_t(p.model) >= seen.size() || seen[size_t(p.model)])
      continue;
    seen[size_t(p.model)] = true;
    ++out.models;
    const uint32_t mi = FindModelInfo(base, p.hash);
    if (!mi || !Span(base, mi, 96))
      continue;
    const uint16_t slot = Read16(base, mi + kModelInfoBoundsSlot);
    if (p.stock ? slot != 0xFFFF : g.IsDictionarySlot(slot)) {
      ++out.linked;
      out.archetypes += Read(base, mi + kModelInfoArchetype) != 0;
    } else if (!out.first_unlinked) {
      out.first_unlinked = p.hash;
      out.first_unlinked_slot = slot;
    }
  }
  return out;
}

// Which pieces are wanted now: every one of a set placed at once; of a set
// streamed by distance (MapInfo::stream), those within that of the focus (a
// standing piece kept to a sixth farther, so the edge does not flicker).
// Their models are asked of the game, and the models of none let go, so the
// set takes only as much of the game's streaming memory as the player sees
// (v1.7.38: Wake Island's 1,600 pieces at once filled it - 260 of 268 MB -
// and the game stopped streaming the world). Returns how many models newly
// wanted were asked for.
int MapWant(PPCContext& ctx, uint8_t* base) {
  auto& g = g_map;
  if (!g.info)
    return 0;
  const float radius = g.info->stream;
  const float in2 = radius * radius, out2 = radius * radius * (7.0f / 6.0f) * (7.0f / 6.0f);
  for (auto& mdl : g.models)
    mdl.wanted = 0;
  g.wanted_pieces = 0;
  for (auto& p : g.pieces) {
    if (radius > 0) {
      const float dx = p.pos[0] - g.focus[0], dy = p.pos[1] - g.focus[1];
      const float d2 = dx * dx + dy * dy;
      p.wanted = d2 < (p.object ? out2 : in2);
    } else {
      p.wanted = true;
    }
    if (p.wanted) {
      ++g.wanted_pieces;
      if (p.model >= 0)
        ++g.models[size_t(p.model)].wanted;
    }
  }
  int asked = 0, let_go = 0;
  g.known_count = 0;
  for (auto& mdl : g.models) {
    if (!mdl.known)
      continue;
    if (mdl.wanted > 0) {
      ++g.known_count;
      if (!mdl.requested) {
        Invoke(ctx, base, "REQUEST_MODEL", {mdl.hash});
        mdl.requested = true;
        ++asked;
      }
    } else if (mdl.requested && mdl.standing == 0) {
      Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {mdl.hash});
      mdl.requested = false;
      mdl.loaded = false;
      ++let_go;
    }
  }
  if (radius > 0 && (asked || let_go) && g.stream_rounds < 40) {
    ++g.stream_rounds;
    REXLOG_INFO("gta4-trainer: map {}: {} pieces wanted within {:.0f} m of ({:.0f}, {:.0f}); {} models asked for, {} "
                "let go ({} models wanted)",
                g.info->name, g.wanted_pieces, radius, g.focus[0], g.focus[1], asked, let_go, g.known_count);
  }
  return asked;
}

// A wanted piece made (its model in): the object, frozen where the mod put it.
bool MapMakePiece(PPCContext& ctx, uint8_t* base, MapStanding& p, bool exact) {
  auto& g = g_map;
  const auto obj = Invoke(ctx, base, exact ? "CREATE_OBJECT_NO_OFFSET" : "CREATE_OBJECT",
                          {p.hash, Bits(p.pos[0]), Bits(p.pos[1]), Bits(p.pos[2]), 0, 1}, 4);
  if (!obj || !obj->words[0])
    return false;
  p.object = obj->words[0];
  Invoke(ctx, base, "SET_OBJECT_QUATERNION", {p.object, Bits(p.q[0]), Bits(p.q[1]), Bits(p.q[2]), Bits(p.q[3])});
  if (!exact)
    Invoke(ctx, base, "SET_OBJECT_COORDINATES", {p.object, Bits(p.pos[0]), Bits(p.pos[1]), Bits(p.pos[2])});
  Invoke(ctx, base, "FREEZE_OBJECT_POSITION", {p.object, 1});
  Invoke(ctx, base, "SET_OBJECT_INVINCIBLE", {p.object, 1});
  Invoke(ctx, base, "SET_OBJECT_COLLISION", {p.object, p.collision ? 1u : 0u});
  if (p.model >= 0)
    ++g.models[size_t(p.model)].standing;
  ++g.made_pieces;
  return true;
}
void MapDropPiece(PPCContext& ctx, uint8_t* base, MapStanding& p) {
  auto& g = g_map;
  if (!p.object)
    return;
  DeleteObject(ctx, base, p.object);
  p.object = 0;
  if (p.model >= 0)
    --g.models[size_t(p.model)].standing;
  --g.made_pieces;
}

// The place's models requested (another place deleted first). Models the
// game does not have (not in its images) are left out, their pieces with them.
// The shader effects the game has loaded (v1.7.42): a list at [0x831C3DE8],
// each effect its name at +32, the name's hash at +36, the next at +84
// (grcEffect's constructor sub_828CA888 links it; sub_828CAA60 loads one by
// name from the shader directory; sub_828CD290 loads those a list file
// names at boot, into a table of 128). A drawable placed names its shaders'
// effects by that hash (sub_828C9C68); one not in the list is a null read
// there (logs 108-109: the Keys' fences, drawn with gta_cutout_fence) - so
// the list is logged once, for mapdraw.py's UNLOADED_EFFECTS.
void LogShaderEffects(uint8_t* base) {
  static bool logged = false;
  if (logged)
    return;
  logged = true;
  constexpr uint32_t kEffectList = 0x831C3DE8;
  if (!Span(base, kEffectList, 4))
    return;
  std::string names;
  int n = 0;
  for (uint32_t e = Read(base, kEffectList); e && Span(base, e, 88) && n < 256; e = Read(base, e + 84)) {
    ++n;
    const uint32_t name = Read(base, e + 32);
    names += fmt::format("{}{} ({:08X})", names.empty() ? "" : ", ", Span(base, name, 1) ? String(base, name) : "?",
                         Read(base, e + 36));
  }
  REXLOG_INFO("gta4-trainer: shader effects loaded: {}: {}", n, names);
  Note(fmt::format("shader effects loaded: {}", n));
}

// What a piece's collision is to the game (v1.7.42). A model's flag word
// (+64 of its info, from its .ide line) has bit 0x4 for a movable object -
// the game makes the city's such entities dummies turned into objects near
// the player (sub_822B9B18), and SetPhysics (sub_82360060) gives their
// bound's archetype a mass and the type flags of an object (+16 0x80, +20
// 0x7BE8); without the bit the archetype gets a building's (+16 0x4).
// v1.7.42 took the probes' "no ground" under the pieces for a type filter
// (GET_GROUND_Z_FOR_3D_COORD asks the physics level for the types 6) and
// made them buildings; v1.7.43 found the trainer had read every probe
// wrong (GroundBelow: the found flag in the height's word), so the probes
// see objects too. The kind stays buildings (terrain nothing pushes about):
// the sets' lines no longer carry the flag; the trainer sets the kind on
// each of the place's models before its collision dictionaries load (the
// archetype is made as they do), movable objects when the menu asks.
std::atomic<bool> g_map_movable{false};
void MapSetPiecesKind(uint8_t* base, bool movable) {
  auto& g = g_map;
  int set = 0, cleared = 0;
  std::vector<bool> own(g.models.size(), false);  // (the set's own models: the game's props keep their kind)
  for (const auto& p : g.pieces)
    if (!p.stock && p.collision && p.model >= 0 && size_t(p.model) < own.size())
      own[size_t(p.model)] = true;
  for (size_t k = 0; k < g.models.size(); ++k) {
    const auto& mdl = g.models[k];
    const uint32_t mi = own[k] && mdl.known ? FindModelInfo(base, mdl.hash) : 0;
    if (!mi || !Span(base, mi, 96, true))
      continue;
    const uint32_t flags = Read(base, mi + 64);
    if (movable && !(flags & 4)) {
      Write(base, mi + 64, flags | 4);
      ++set;
    } else if (!movable && (flags & 4)) {
      Write(base, mi + 64, flags & ~4u);
      ++cleared;
    }
  }
  if (g.info)
    REXLOG_INFO("gta4-trainer: map {}: its pieces {} ({} models' movable bit set, {} cleared)", g.info->name,
                movable ? "movable objects" : "buildings", set, cleared);
}

bool MapRequest(PPCContext& ctx, uint8_t* base, const gta4::addons::MapInfo& info) {
  auto& g = g_map;
  LogShaderEffects(base);
  if (g.info == &info && g.phase != 0)
    return true;
  if (g.info)
    MapUnload(ctx, base, "another place chosen");
  g.info = &info;
  g.phase = 1;
  g.requested_at = NowMs();
  g.next_renew = g.requested_at + 6000;
  g.next_report = g.requested_at + 5000;
  for (const auto& p : info.pieces) {
    MapStanding s;
    s.hash = ModelHash(p.model);
    s.collision = p.collision;
    s.stock = p.stock;
    const auto at = MapPoint(info, p.pos[0], p.pos[1], p.pos[2]);
    for (int a = 0; a < 3; ++a)
      s.pos[a] = at[size_t(a)];
    for (int a = 0; a < 4; ++a)
      s.q[a] = p.q[a];
    auto it = std::find_if(g.models.begin(), g.models.end(), [&](const MapModel& m) { return m.hash == s.hash; });
    if (it == g.models.end()) {
      MapModel mdl;
      mdl.name = p.model;
      mdl.hash = s.hash;
      g.models.push_back(mdl);
      it = g.models.end() - 1;
    }
    s.model = int(it - g.models.begin());
    g.pieces.push_back(s);
  }
  // (the pieces with collision first: the ground before what stands on it,
  // should a pool run short)
  std::stable_partition(g.pieces.begin(), g.pieces.end(), [](const MapStanding& p) { return p.collision; });
  std::string unknown;
  std::unordered_map<uint32_t, size_t> by_info;
  int known = 0;
  for (size_t k = 0; k < g.models.size(); ++k) {
    auto& mdl = g.models[k];
    const auto is_known = Invoke(ctx, base, "IS_MODEL_IN_CDIMAGE", {mdl.hash});
    mdl.known = !is_known || is_known->r0();
    if (!mdl.known) {
      unknown += (unknown.empty() ? "" : ", ") + mdl.name;
      continue;
    }
    ++known;
    if (const uint32_t mi = FindModelInfo(base, mdl.hash))
      by_info[mi] = k;
  }
  // (the pieces wanted about the arrival first; the player once there)
  const auto focus = MapPoint(info, info.arrive[0], info.arrive[1], info.arrive[2]);
  for (int a = 0; a < 3; ++a)
    g.focus[a] = focus[size_t(a)];
  MapWant(ctx, base);
  int used = 0;
  ForEachModelInfo(base, [&](uint32_t i, uint32_t mi) {
    ++used;
    if (const auto it = by_info.find(mi); it != by_info.end())
      g.models[it->second].index = int(i);
    return false;
  });
  REXLOG_INFO("gta4-trainer: map {}: the model table holds {} of {}", info.name, used, kModelInfoCount);
  const size_t stock = size_t(std::count_if(g.pieces.begin(), g.pieces.end(), [](const MapStanding& s) { return s.stock; }));
  REXLOG_INFO("gta4-trainer: map {} requested: {} pieces ({} of the game's own models), {} models ({} not in the "
              "game's images{}{}), the sea there at {:.2f} (the mod's {:.2f}); {}",
              info.name, g.pieces.size(), stock, g.models.size(), g.models.size() - size_t(known),
              unknown.empty() ? "" : ": ", unknown, info.sea_dst, info.sea_src,
              info.stream > 0 ? fmt::format("streamed by distance: {} pieces of {} models wanted within {:.0f} m of the "
                                            "arrival first",
                                            g.wanted_pieces, g.known_count, info.stream)
                              : std::string("placed at once"));
  REXLOG_INFO("gta4-trainer: map {}: the game's pools before it: {}", info.name, DescribePools(base));
  Note(fmt::format("map {} requested: {} pieces, {} models", info.name, g.pieces.size(), g.models.size()));
  MapSetPiecesKind(base, g_map_movable.load(std::memory_order_relaxed));
  MapRequestBounds(ctx, base);
  if (g.AnyDictionary()) {
    const auto links = MapCountLinks(base);
    REXLOG_INFO("gta4-trainer: map {}: collision asked for ({}); {} of its {} models with collision linked already",
                info.name, DescribeDictionaries(base), links.linked, links.models);
  } else if (info.bounds.empty()) {
    REXLOG_INFO("gta4-trainer: map {} has no collision dictionary", info.name);
  }
  return true;
}

// Loading: how many models are in, the requests renewed while some are not,
// a report of what it waits for every few seconds.
void MapCheck(PPCContext& ctx, uint8_t* base, const GameContext& game);
void MapStreamTick(PPCContext& ctx, uint8_t* base, const GameContext& game, uint64_t now);
void MapProbe(PPCContext& ctx, uint8_t* base);
void SafehouseTick(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint64_t now);
void MapTick(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint64_t now) {
  auto& g = g_map;
  if (!g.info || now < g.next_check)
    return;
  g.next_check = now + 250;
  if (g.unload_at && now >= g.unload_at) {
    MapUnload(ctx, base, "the trip home made");
    return;
  }
  if (g.check_at && now >= g.check_at)
    MapCheck(ctx, base, game);
  if (g.phase == 2) {
    MapProbe(ctx, base);
    if (!g.info)
      return;  // (forgotten: the world was rebuilt)
    MapStreamTick(ctx, base, game, now);
    SafehouseTick(ctx, base, game, s, now);
    return;
  }
  if (g.phase != 1 || g.loaded)
    return;
  int in = 0;
  for (auto& mdl : g.models) {
    if (!mdl.known || mdl.wanted == 0)
      continue;
    const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {mdl.hash});
    mdl.loaded = loaded && loaded->r0();
    if (mdl.loaded)
      ++in;
    else if (now >= g.next_renew)
      Invoke(ctx, base, "REQUEST_MODEL", {mdl.hash});
  }
  // Its collision dictionaries: each in once the store holds it (its set
  // callback has linked the models then); asked for again while one is not.
  if (!g.DictionariesIn()) {
    bool missing = false;
    for (auto& d : g.dicts) {
      if (d.slot < 0 || d.in)
        continue;
      const uint32_t entry = BoundsEntry(base, d.slot);
      if (entry && Read(base, entry) != 0) {
        d.in = true;
        g.bounds_in_at = now;
        const auto links = MapCountLinks(base);
        REXLOG_INFO("gta4-trainer: map {}: collision dictionary {}.xbd in after {} ms ({}): {} of {} models with "
                    "collision linked, {} with their bounds{}",
                    g.info->name, d.name, Since(now, g.requested_at), DescribeBoundsSlot(base, d.slot), links.linked,
                    links.models, links.archetypes,
                    links.first_unlinked ? fmt::format(" (not yet: {:08X}, its +0x52 {:04X})", links.first_unlinked,
                                                       links.first_unlinked_slot)
                                         : std::string());
      } else {
        missing = true;
      }
    }
    if (missing && now >= g.next_renew)
      MapRequestBounds(ctx, base);
  }
  if (now >= g.next_renew) {
    g.next_renew = now + 6000;
    ++g.renewals;
  }
  if (in != g.loaded_count && g.logs < 12 && (in == g.known_count || Since(now, g.requested_at) > 3000)) {
    ++g.logs;
    REXLOG_INFO("gta4-trainer: map {}: {} of {} models loaded after {} ms", g.info->name, in, g.known_count,
                Since(now, g.requested_at));
  }
  g.loaded_count = in;
  // (the pieces wait for the dictionaries: one made before its is in gets no
  // collision of its own till the game looks again; 20 s at most, then on
  // without it, logged)
  const bool bounds_ready = g.DictionariesIn() || Since(now, g.requested_at) > kMapBoundsWaitMs;
  if (g.known_count > 0 && in == g.known_count && !bounds_ready && now >= g.next_report && g.reports < 8) {
    g.next_report = now + 5000;
    ++g.reports;
    REXLOG_INFO("gta4-trainer: map {}: models in, waiting for its collision ({})", g.info->name,
                DescribeDictionaries(base));
  }
  if (g.known_count > 0 && in == g.known_count && bounds_ready) {
    if (!g.DictionariesIn())
      REXLOG_WARN("gta4-trainer: map {}: its collision not all in after {} ms ({}); placing the pieces without "
                  "waiting longer",
                  g.info->name, Since(now, g.requested_at), DescribeDictionaries(base));
    g.loaded = true;
    g.loaded_at = now;
    if ((!g_tm.in_car || g_tm.phase == 0) && !g.guarding)
      Toast(s, g.info->label + std::string(" is ready: 88 mph to go."), 2500);
    return;
  }
  if (now >= g.next_report && g.reports < 8) {
    g.next_report = now + (g.reports < 3 ? 5000 : 15000);
    MapReport(ctx, base, "still loading");
  }
}

// The pieces placed (those of the models the game has; all their models in).
bool MapPlace(PPCContext& ctx, uint8_t* base) {
  auto& g = g_map;
  if (!g.info || !g.loaded)
    return false;
  if (g.phase == 2)
    return true;
  const bool exact = NativeKnown("CREATE_OBJECT_NO_OFFSET");
  int made = 0, failed = 0, left_out = 0, waiting = 0;
  for (auto& p : g.pieces) {
    if (p.object || !p.wanted)
      continue;
    if (p.model < 0 || !g.models[size_t(p.model)].known) {
      ++left_out;
      continue;
    }
    if (!MapMakePiece(ctx, base, p, exact)) {
      ++failed;
      continue;
    }
    if (made < 3) {  // (where the game put the first few, for the log)
      if (const auto c = Invoke(ctx, base, "GET_OBJECT_COORDINATES", {p.object, 0, 0, 0}, 1, 0, 3))
        REXLOG_INFO("gta4-trainer: map piece {:08X} at ({:.2f}, {:.2f}, {:.2f}), asked ({:.2f}, {:.2f}, {:.2f})",
                    p.hash, c->f(0), c->f(1), c->f(2), p.pos[0], p.pos[1], p.pos[2]);
    }
    ++made;
  }
  for (const auto& p : g.pieces)
    waiting += !p.wanted;
  const auto links = MapCountLinks(base);
  REXLOG_INFO("gta4-trainer: map {} placed: {} pieces made, {} not, {} left out (models the game hasn't), {} farther "
              "off for later ({}); {} of {} models with collision linked to their bounds ({}; {} with their bounds in)",
              g.info->name, made, failed, left_out, waiting, exact ? "exactly" : "CREATE_OBJECT, then moved",
              links.linked, links.models, g.DictionaryNames(), links.archetypes);
  REXLOG_INFO("gta4-trainer: map {}: the game's pools with it placed: {}", g.info->name, DescribePools(base));
  Note(fmt::format("map {} placed: {} pieces made, {} not", g.info->name, made, failed));
  if (failed)
    return false;
  g.phase = 2;
  return true;
}

// Standing, streamed by distance: twice a second the pieces are wanted about
// the player; those newly wanted are made as their models come in, those
// gone far are deleted and their models let go (v1.7.38).
void MapStreamTick(PPCContext& ctx, uint8_t* base, const GameContext& game, uint64_t now) {
  auto& g = g_map;
  if (!g.info || g.phase != 2 || g.info->stream <= 0 || now < g.next_stream)
    return;
  g.next_stream = now + 500;
  for (int a = 0; a < 3; ++a)
    g.focus[a] = game.position[a];
  MapWant(ctx, base);
  const bool exact = NativeKnown("CREATE_OBJECT_NO_OFFSET");
  int made = 0, dropped = 0, waiting = 0;
  for (auto& p : g.pieces) {
    if (!p.wanted) {
      if (p.object) {
        MapDropPiece(ctx, base, p);
        ++dropped;
      }
      continue;
    }
    if (p.object || p.model < 0)
      continue;
    auto& mdl = g.models[size_t(p.model)];
    if (!mdl.known)
      continue;
    if (!mdl.loaded) {
      const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {mdl.hash});
      mdl.loaded = loaded && loaded->r0();
    }
    if (!mdl.loaded) {
      ++waiting;
      if (now >= g.next_renew)
        Invoke(ctx, base, "REQUEST_MODEL", {mdl.hash});
      continue;
    }
    if (MapMakePiece(ctx, base, p, exact))
      ++made;
    else
      mdl.loaded = false;  // (the game may have let the model go: asked for again next time)
  }
  if (now >= g.next_renew)
    g.next_renew = now + 6000;
  if ((made || dropped) && g.stream_rounds < 40) {
    ++g.stream_rounds;
    REXLOG_INFO("gta4-trainer: map {}: streaming: {} pieces made, {} deleted, {} waiting for their models; {} of {} "
                "standing; pools: {}",
                g.info->name, made, dropped, waiting, g.made_pieces, g.pieces.size(), DescribePools(base));
  }
}

// The place standing now, or none.
const gta4::addons::MapInfo* MapStandingNow() { return g_map.phase == 2 ? g_map.info : nullptr; }

// ---- the player on a place without it; a place's spots; its safehouse (v1.7.40) ----
//
// A game saved on a place (the mansion's bed) loads the player there with
// nothing under him. The guard stands the place under a player held over
// it: once a second, with no place requested and no trip under way, a
// player well inside a place's area (and not flying over it) has it
// requested and is moved to his own spot by Teleport, which holds him there
// and waits for the ground, which comes with the place (TeleportTick waits
// on MapGuarding). The menu's teleport to a place's spot takes the same
// path. A loaded game is told from the player ped's handle changing, and a
// standing piece gone, since the world is rebuilt both ways: what stood is
// forgotten without deleting.
bool MapGuarding() { return g_map.info && g_map.guarding && g_map.phase != 2; }

void LifeForget();
void MapForget(PPCContext& ctx, uint8_t* base, const char* why) {
  auto& g = g_map;
  if (g.info) {
    // (its dictionaries no longer kept: the flag outlives the world)
    const uint32_t module = BoundsModule(base);
    for (const auto& d : g.dicts)
      if (d.slot >= 0 && module < uint32_t(StreamModuleCount(base)))
        CallGuest(ctx, base, kStreamUnkeep, uint32_t(d.slot), module);
    REXLOG_INFO("gta4-trainer: map {} forgotten ({}): its {} pieces went with the world", g.info->name, why,
                g.made_pieces);
  }
  g = MapRun{};
  LifeForget();
  g_teleport.active = false;
}

// One standing piece looked for each tick, in turn: one gone (and the next)
// means the world was rebuilt under the place - a game loaded, or restarted.
void MapProbe(PPCContext& ctx, uint8_t* base) {
  auto& g = g_map;
  if (g.pieces.empty())
    return;
  for (int tries = 0; tries < 8; ++tries) {
    g.probe = (g.probe + 1) % int(g.pieces.size());
    const auto& p = g.pieces[size_t(g.probe)];
    if (!p.object)
      continue;
    const auto exists = Invoke(ctx, base, "DOES_OBJECT_EXIST", {p.object});
    if (!exists)
      return;
    if (exists->r0()) {
      g.probe_misses = 0;
      return;
    }
    if (++g.probe_misses >= 2) {
      MapForget(ctx, base, "a standing piece is gone, and the next: the world was rebuilt");
      return;
    }
    return;
  }
}

// A spot's point in the world (the arrival when none).
std::array<float, 3> SpotPoint(const gta4::addons::MapInfo& info, const gta4::addons::MapSpot* spot) {
  return spot ? MapPoint(info, spot->pos[0], spot->pos[1], spot->pos[2])
              : MapPoint(info, info.arrive[0], info.arrive[1], info.arrive[2]);
}

// The place stood under a point and the player held there till it is: the
// place requested (another standing let go), the time circuits' present its
// date and the clock its hour, the player teleported to the point (held
// still while the ground comes). Home for the time machine, when none is
// known, is where the player was when he was in the city, or the airport.
void StandPlaceUnder(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s,
                     const gta4::addons::MapInfo& info, float x, float y, float z, std::optional<float> heading,
                     const char* why) {
  auto& g = g_map;
  const std::array<float, 3> here{game.position[0], game.position[1], game.position[2]};
  const auto* standing = MapStandingNow();
  bool in_city = true;
  for (const auto& m : gta4::addons::InstalledMaps())
    if (m.installed && OnMap(m, here))
      in_city = false;
  if (!g_tm.have_home) {
    if (in_city) {
      for (int a = 0; a < 3; ++a)
        g_tm.home[a] = here[size_t(a)];
    } else {
      g_tm.home[0] = 2188.4f;  // (in front of Francis International's terminal, the teleport's spot)
      g_tm.home[1] = 638.8f;
      g_tm.home[2] = 5.1f + kTmContact;
    }
    g_tm.home_q = QAxis(0, 0, 1, std::atan2(-game.forward[0], game.forward[1]));
    g_tm.have_home = true;
  }
  if (standing != &info) {
    MapRequest(ctx, base, info);  // (another place standing is let go first)
    g.guarding = true;
    g.guard_since = NowMs();
    g.guard_toast = 0;
    g_tm.present = TcdDate{info.date[0], info.date[1], info.date[2], info.date[3], info.date[4]};
    Invoke(ctx, base, "SET_TIME_OF_DAY", {uint32_t(std::clamp(info.date[3], 0, 23)), uint32_t(std::clamp(info.date[4], 0, 59))});
    REXLOG_INFO("gta4-trainer: {} stood under ({:.1f}, {:.1f}, {:.1f}): {}", info.name, x, y, z, why);
    Note(fmt::format("{} stood under ({:.0f}, {:.0f}, {:.0f}): {}", info.name, x, y, z, why));
    Toast(s, fmt::format("Standing {}...", info.label), 2500);
  }
  Teleport(ctx, base, game, x, y, z, s, heading);
}

// The menu: the player taken to a place's spot (the arrival road when none).
void TeleportToPlace(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s,
                     const gta4::addons::MapInfo& info, const gta4::addons::MapSpot* spot) {
  if (!info.installed) {
    Toast(s, "That place isn't installed in this launch.", 2500);
    return;
  }
  if (g_tm.phase != 0) {
    Toast(s, "Not in the middle of a trip.", 2000);
    return;
  }
  const auto at = SpotPoint(info, spot);
  StandPlaceUnder(ctx, base, game, s, info, at[0], at[1], at[2], spot ? spot->heading : info.arrive[3],
                  spot ? "the menu's teleport to its spot" : "the menu's teleport to its arrival");
}

// Once a second: the guard while a place is stood (placed when loaded; a
// word now and then; given up after the trip's limit), and a player on a
// place with none requested (a loaded game there) given it.
void MapGuard(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint64_t now) {
  auto& g = g_map;
  if (g.info) {
    if (!g.guarding)
      return;
    if (g.phase == 2) {
      g.guarding = false;  // (it stands: the teleport puts the player down on it)
      return;
    }
    // (the guard's clock: the tick's `now` was read before the menu's action
    // set guard_since, so in the action's own frame it is a few ms behind -
    // v1.7.42's first Wake trip was "not ready after 18446744073709551609 ms"
    // and sent home at once, log 114)
    const uint64_t waited = now > g.guard_since ? now - g.guard_since : 0;
    if (g.loaded && MapPlace(ctx, base)) {
      g.guarding = false;
      REXLOG_INFO("gta4-trainer: map {} stood after {} ms (the guard)", g.info->name, waited);
      g.check_at = now + 5000;  // (v1.7.42: the check after a menu teleport too - its physics survey)
      return;
    }
    if (waited > kMapLoadLimitMs) {
      REXLOG_WARN("gta4-trainer: map {} not ready after {} ms ({} of {} models); the player taken home instead",
                  g.info->name, waited, g.loaded_count, g.known_count);
      g.guarding = false;
      g_teleport.active = false;
      const std::string label = g.info->label;
      MapUnload(ctx, base, "not ready in time under the player");
      Toast(s, label + " didn't load in time: back to the city.", 3000);
      if (g_tm.have_home)
        Teleport(ctx, base, game, g_tm.home[0], g_tm.home[1], std::nullopt, s);
      else
        Teleport(ctx, base, game, 2188.4f, 638.8f, 5.1f, s);
      return;
    }
    if (now >= g.guard_toast) {
      g.guard_toast = now + 1500;
      const bool models_in = g.known_count > 0 && g.loaded_count == g.known_count;
      Toast(s, fmt::format("Standing {}... ({})", g.info->label,
                           models_in && !g.DictionariesIn() ? std::string("its ground loading")
                                                            : fmt::format("{} of {} loaded", g.loaded_count, g.known_count)),
            1600);
    }
    return;
  }
  // nothing requested: a player on a place is given it (two looks a second apart)
  static const gta4::addons::MapInfo* s_candidate = nullptr;
  static int s_hits = 0;
  if (g_tm.phase != 0 || g_teleport.active || game.position[2] > 80.0f) {
    s_hits = 0;
    return;
  }
  const std::array<float, 3> here{game.position[0], game.position[1], game.position[2]};
  const gta4::addons::MapInfo* on = nullptr;
  for (const auto& info : gta4::addons::InstalledMaps())
    if (info.installed && OnMap(info, here, -50.0f))
      on = &info;
  if (!on) {
    s_hits = 0;
    return;
  }
  if (on != s_candidate) {
    s_candidate = on;
    s_hits = 0;
  }
  if (++s_hits < 2)
    return;
  s_hits = 0;
  REXLOG_INFO("gta4-trainer: the player is on {} at ({:.1f}, {:.1f}, {:.1f}) with nothing of it standing (a loaded "
              "game there?)",
              on->name, here[0], here[1], here[2]);
  StandPlaceUnder(ctx, base, game, s, *on, here[0], here[1], here[2], std::nullopt, "the player was found on it");
}

// The place's safehouse (its first spot with one): a house blip at its door
// while the place stands (the game's own safehouses' kind: ADD_BLIP_FOR_CONTACT,
// sprite 29, as main.sco adds them), and its bed a save point: standing by
// it on foot opens the game's save menu, once, till the player leaves it.
void SafehouseTick(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint64_t now) {
  auto& g = g_map;
  if (!g.info || g.phase != 2)
    return;
  const gta4::addons::MapSpot* house = nullptr;
  for (const auto& sp : g.info->spots)
    if (sp.safehouse) {
      house = &sp;
      break;
    }
  if (!house)
    return;
  if (!g.house_blip && NativeKnown("ADD_BLIP_FOR_CONTACT")) {
    const auto door = MapPoint(*g.info, house->door[0], house->door[1], house->door[2]);
    if (const auto b = Invoke(ctx, base, "ADD_BLIP_FOR_CONTACT", {Bits(door[0]), Bits(door[1]), Bits(door[2]), 0}, 3);
        b && b->r0()) {
      g.house_blip = b->r0();
      Invoke(ctx, base, "CHANGE_BLIP_SPRITE", {g.house_blip, 29});
      Invoke(ctx, base, "CHANGE_BLIP_DISPLAY", {g.house_blip, 2});
      Invoke(ctx, base, "CHANGE_BLIP_PRIORITY", {g.house_blip, 0});
      if (NativeKnown("CHANGE_BLIP_NAME_FROM_ASCII"))
        Invoke(ctx, base, "CHANGE_BLIP_NAME_FROM_ASCII", {g.house_blip, 0}, -1, 0, 1, 1, house->label);
      REXLOG_INFO("gta4-trainer: {}: the safehouse blip {:08X} at ({:.1f}, {:.1f}, {:.1f}), its bed at ({:.1f}, {:.1f}, {:.1f})",
                  g.info->name, g.house_blip, door[0], door[1], door[2], house->bed[0] + g.info->shift[0],
                  house->bed[1] + g.info->shift[1], house->bed[2] + MapDz(*g.info));
    }
  }
  const auto bed = MapPoint(*g.info, house->bed[0], house->bed[1], house->bed[2]);
  const float dx = game.position[0] - bed[0], dy = game.position[1] - bed[1], dz = game.position[2] - bed[2];
  const float d = std::sqrt(dx * dx + dy * dy);
  if (d < 6.0f && std::fabs(dz) < 3.0f) {
    if (!g.bed_near) {
      g.bed_near = true;
      Toast(s, "Your bed: stand by it to save the game.", 3000);
    }
  } else if (d > 8.0f) {
    g.bed_near = false;
  }
  if (d > 3.0f) {
    g.bed_armed = true;
    return;
  }
  if (!g.bed_armed || d > 1.4f || std::fabs(dz) > 1.6f || !NativeKnown("ACTIVATE_SAVE_MENU"))
    return;
  if (const auto in_car = Invoke(ctx, base, "IS_CHAR_IN_ANY_CAR", {game.ped}); in_car && in_car->r0())
    return;
  g.bed_armed = false;
  REXLOG_INFO("gta4-trainer: {}: the player at the bed ({:.1f} m): the save menu", g.info->name, d);
  Invoke(ctx, base, "ACTIVATE_SAVE_MENU", {});
  (void)now;
}

// A look at the pieces a few seconds after an arrival (v1.7.32; v1.7.31's
// island showed its buildings but not its terrain): how many still exist and
// are drawn, those near the player not drawn, and the ground the game finds
// under the arrival against the place's own.
// An object's entity from its script handle: the object pool ([0x82DE5A50]:
// +0 the objects, +4 a flag byte each, +12 an object's size; a handle is its
// slot << 8 | the flag), as VehicleAddress reads the vehicles'.
uint32_t ObjectAddress(uint8_t* base, uint32_t handle) {
  constexpr uint32_t kObjectPoolPointer = 0x82DE5A50;
  if (!handle || !Span(base, kObjectPoolPointer, 4))
    return 0;
  const uint32_t pool = Read(base, kObjectPoolPointer);
  if (!Span(base, pool, 16))
    return 0;
  const uint32_t slot = handle >> 8, storage = Read(base, pool), flags = Read(base, pool + 4),
                 size = Read(base, pool + 12);
  if (!Span(base, flags + slot, 1) || *rex::memory::GuestPtr(base, flags + slot) != (handle & 0xFF))
    return 0;
  const uint32_t object = storage + slot * size;
  return Span(base, object, 96) ? object : 0;
}

// The physics of the place's own collision pieces as the game holds them
// (v1.7.42: Wake Island's runway and most of its cells had no collision in
// v1.7.41 while the apron had, log 111; the game's ground probe found no
// ground under the runway cell). An object made by CREATE_OBJECT gets its
// physics in sub_82500400: the model's bound dictionary asked for and loaded
// (its +0x52 slot), then the entity's vtable +164 makes its physics instance
// (entity +0x38: a phInst - +4 its archetype, +8 its index in the physics
// level, 0xFFFF outside it) from the model's archetype (+0x04; +0xC its
// bound), and vtable +168 puts the instance in the level when its index is
// 0xFFFF. For each piece near the player: whether it stands, has an
// instance, the instance's level index, its archetype's bound (type 10 a
// mesh, 12 a composite) and whether a probe from 80 m over its origin finds
// ground - the pieces the game collides with against those it does not.
void MapPhysicsSurvey(PPCContext& ctx, uint8_t* base, const GameContext& game) {
  auto& g = g_map;
  if (!g.info)
    return;
  struct Row {
    float d = 0;
    const MapStanding* p = nullptr;
  };
  std::vector<Row> rows;
  for (const auto& p : g.pieces) {
    if (!p.collision || p.stock || !p.object)
      continue;
    const float dx = p.pos[0] - game.position[0], dy = p.pos[1] - game.position[1];
    rows.push_back({std::sqrt(dx * dx + dy * dy), &p});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
  int standing = 0, with_inst = 0, in_level = 0, with_ground = 0, listed = 0;
  for (const auto& r : rows) {
    const auto& p = *r.p;
    const uint32_t entity = ObjectAddress(base, p.object);
    const uint32_t mi = FindModelInfo(base, p.hash);
    const uint32_t archetype = mi && Span(base, mi, 96) ? Read(base, mi + kModelInfoArchetype) : 0;
    const uint32_t slot = mi && Span(base, mi, 96) ? Read16(base, mi + kModelInfoBoundsSlot) : 0xFFFF;
    const uint32_t inst = entity ? Read(base, entity + 0x38) : 0;
    const bool inst_ok = inst && Span(base, inst, 16);
    const uint32_t level = inst_ok ? Read16(base, inst + 8) : 0xFFFF;
    const uint32_t inst_arch = inst_ok ? Read(base, inst + 4) : 0;
    const uint32_t bound = inst_arch && Span(base, inst_arch, 16) ? Read(base, inst_arch + 0xC) : 0;
    const uint32_t btype = bound && Span(base, bound, 0x80) ? (Read(base, bound + 4) >> 24) : 0;
    std::string box;
    if (bound && Span(base, bound, 0x80))
      box = fmt::format(" z {:.1f}..{:.1f}", ReadFloat(base, bound + 0x28), ReadFloat(base, bound + 0x18));
    float ex = 0, ey = 0, ez = 0;
    if (entity) {
      const uint32_t m = Read(base, entity + 0x20);
      if (m && Span(base, m, 64))
        ex = ReadFloat(base, m + 48), ey = ReadFloat(base, m + 52), ez = ReadFloat(base, m + 56);
    }
    const auto ground = GroundZ(ctx, base, p.pos[0], p.pos[1], p.pos[2] + 80.0f);
    standing += entity != 0;
    with_inst += inst_ok;
    in_level += inst_ok && level != 0xFFFF;
    with_ground += ground.has_value();
    if (listed < 40 && r.d < 600.0f) {
      ++listed;
      REXLOG_INFO("gta4-trainer:   {} {:.0f} m: entity {:08X} at ({:.1f}, {:.1f}, {:.1f}); model slot {:04X} archetype "
                  "{:08X}; inst {:08X} level {:04X} archetype {:08X} bound {:08X} type {}{}; ground {}",
                  p.model >= 0 ? g.models[size_t(p.model)].name : std::string("?"), r.d, entity, ex, ey, ez, slot,
                  archetype, inst, level, inst_arch, bound, btype, box,
                  ground ? fmt::format("{:.2f}", *ground) : std::string("none"));
    }
  }
  REXLOG_INFO("gta4-trainer:   physics: {} of the place's {} collision pieces stand, {} with an instance, {} in the "
              "level, {} with ground under a probe at their origin; the player at ({:.1f}, {:.1f}, {:.1f})",
              standing, rows.size(), with_inst, in_level, with_ground, game.position[0], game.position[1],
              game.position[2]);
  // (v1.7.43: the probe under the player's own feet, from just over him and
  // from well above, beside the place's own height map there)
  {
    const float px = game.position[0], py = game.position[1], pz = game.position[2];
    const auto near = GroundZ(ctx, base, px, py, pz + 2.0f);
    const auto high = GroundZ(ctx, base, px, py, pz + 50.0f);
    const auto own = PlaceGroundUnder(px, py);
    REXLOG_INFO("gta4-trainer:   the ground under the player: the game's probe from 2 m up {}, from 50 m up {}; the "
                "place's height map {}",
                near ? fmt::format("{:.2f}", *near) : std::string("none"),
                high ? fmt::format("{:.2f}", *high) : std::string("none"),
                own ? fmt::format("{:.2f}", *own) : std::string("none"));
  }
  // The physics level itself ([0x82FF5360], made at boot - sub_82478B00:
  // extents set (sub_8294BE28: +80 their centre, +96 half the longest side,
  // the octree's cube), room for 6000 objects (+38) and 1500 active (+136)):
  // its cube and its first words, for the counts.
  // The pools an object's physics instance comes from (CObject's physics
  // init, sub_82328520: a frag's from [0x8309D588] (864 of 176 bytes), the
  // rest from [0x8309D584] (4690 of 96, sub_826225E0) - none free, no
  // physics): their counts and how many are in use (+20).
  for (const auto& [name, ptr] : {std::pair{"instances", 0x8309D584u}, std::pair{"frag instances", 0x8309D588u}}) {
    if (!Span(base, ptr, 4))
      continue;
    const uint32_t pool = Read(base, ptr);
    if (pool && Span(base, pool, 24))
      REXLOG_INFO("gta4-trainer:   the {} pool {:08X}: {} of {} in use ({} bytes each)", name, pool,
                  Read(base, pool + 20), Read(base, pool + 8), Read(base, pool + 12));
  }
  constexpr uint32_t kPhysicsLevelPointer = 0x82FF5360;
  if (Span(base, kPhysicsLevelPointer, 4)) {
    const uint32_t level = Read(base, kPhysicsLevelPointer);
    if (level && Span(base, level, 0x100)) {
      std::string words;
      for (uint32_t w = 0; w < 48; ++w)
        words += fmt::format("{}{:08X}", w % 8 ? " " : " | ", Read(base, level + w * 4));
      REXLOG_INFO("gta4-trainer:   the physics level {:08X}: centre ({:.0f}, {:.0f}, {:.0f}), half-side {:.0f}, room "
                  "for {} objects, {} active; words{}",
                  level, ReadFloat(base, level + 80), ReadFloat(base, level + 84), ReadFloat(base, level + 88),
                  ReadFloat(base, level + 96), Read16(base, level + 38), Read16(base, level + 136), words);
    }
  }
}

void MapCheck(PPCContext& ctx, uint8_t* base, const GameContext& game) {
  auto& g = g_map;
  g.check_at = 0;
  if (!g.info || g.phase != 2)
    return;
  int exist = 0, shown = 0, listed = 0;
  std::string near_hidden;
  for (const auto& p : g.pieces) {
    if (!p.object)
      continue;
    if (NativeKnown("DOES_OBJECT_EXIST")) {
      const auto e = Invoke(ctx, base, "DOES_OBJECT_EXIST", {p.object});
      if (e && !e->r0())
        continue;
    }
    ++exist;
    bool on = false;
    if (NativeKnown("IS_OBJECT_ON_SCREEN")) {
      const auto o = Invoke(ctx, base, "IS_OBJECT_ON_SCREEN", {p.object});
      on = o && o->r0();
    }
    shown += on;
    const float dx = p.pos[0] - game.position[0], dy = p.pos[1] - game.position[1];
    const float d = std::sqrt(dx * dx + dy * dy);
    if (!on && d < 150.0f && listed < 8) {
      ++listed;
      const std::string name = p.model >= 0 ? g.models[size_t(p.model)].name : "?";
      near_hidden += fmt::format("{}{} {:.0f} m", near_hidden.empty() ? "" : ", ", name, d);
    }
  }
  const auto& a = g.info->arrive;
  const auto at = MapPoint(*g.info, a[0], a[1], a[2]);
  const auto ground = GroundZ(ctx, base, at[0], at[1], at[2] + 3.0f);
  // (where the car and the player are now: on the road at about its height,
  // or in the sea under it - v1.7.32's arrival, its collision missing)
  std::string car = "gone";
  if (VehicleAddress(base, g_tm.car))
    if (const auto c = CarPosition(ctx, base, g_tm.car))
      car = fmt::format("({:.1f}, {:.1f}, {:.2f})", (*c)[0], (*c)[1], (*c)[2]);
  REXLOG_INFO("gta4-trainer: map {} check: {} of {} pieces exist, {} drawn (on screen); near the player and not "
              "drawn: {}; the ground under the arrival {} (the place's {:.2f}); the car at {}, the player at "
              "({:.1f}, {:.1f}, {:.2f})",
              g.info->name, exist, g.pieces.size(), shown, near_hidden.empty() ? std::string("none") : near_hidden,
              ground ? fmt::format("{:.2f}", *ground) : std::string("not found"), at[2], car, game.position[0],
              game.position[1], game.position[2]);
  if (g.AnyDictionary()) {
    const auto links = MapCountLinks(base);
    REXLOG_INFO("gta4-trainer:   collision: {}; {} of {} models linked, {} with their bounds", DescribeDictionaries(base),
                links.linked, links.models, links.archetypes);
  }
  REXLOG_INFO("gta4-trainer:   the game's pools: {}", DescribePools(base));
  // Each standing piece of the place's own collision asked for ground at
  // its origin (and, failing that, 40 m to each side), from 80 m up: which
  // of them the game collides with (v1.7.40: Wake Island's runway cell had
  // none under the arrival, its neighbour had, log 107).
  {
    int standing = 0, with_ground = 0, stock_standing = 0, stock_hit = 0;
    std::string none, stock_none;
    for (const auto& p : g.pieces) {
      if (!p.collision || !p.object)
        continue;
      if (p.stock) {  // (the game's own prop: whatever its bound puts under a probe at its origin)
        ++stock_standing;
        if (GroundZ(ctx, base, p.pos[0], p.pos[1], p.pos[2] + 30.0f).has_value())
          ++stock_hit;
        else if (stock_none.size() < 300)
          stock_none += fmt::format("{}{} ({:.0f}, {:.0f})", stock_none.empty() ? "" : ", ",
                                    p.model >= 0 ? g.models[size_t(p.model)].name : std::string("?"), p.pos[0], p.pos[1]);
        continue;
      }
      ++standing;
      bool found = GroundZ(ctx, base, p.pos[0], p.pos[1], p.pos[2] + 80.0f).has_value();
      int sides = 0;
      if (!found) {
        static constexpr float kSides[4][2] = {{40.0f, 0.0f}, {-40.0f, 0.0f}, {0.0f, 40.0f}, {0.0f, -40.0f}};
        for (const auto& d : kSides)
          sides += GroundZ(ctx, base, p.pos[0] + d[0], p.pos[1] + d[1], p.pos[2] + 80.0f).has_value();
      }
      if (found || sides)
        ++with_ground;
      else if (none.size() < 600)
        none += fmt::format("{}{} ({:.0f}, {:.0f})", none.empty() ? "" : ", ",
                            p.model >= 0 ? g.models[size_t(p.model)].name : std::string("?"), p.pos[0], p.pos[1]);
    }
    if (standing)
      REXLOG_INFO("gta4-trainer:   the place's own collision: {} pieces standing, {} with ground under them (at the "
                  "origin or 40 m to a side); none under: {}",
                  standing, with_ground, none.empty() ? std::string("-") : none);
    if (stock_standing)
      REXLOG_INFO("gta4-trainer:   the game's own props: {} standing, {} with something under a probe at their origin; "
                  "nothing under: {}",
                  stock_standing, stock_hit, stock_none.empty() ? std::string("-") : stock_none);
  }
  // The model under the arrival (one with collision) and one without, as the
  // game holds them: its model info's first 24 words and the streaming
  // entries it needs (its collision dictionary among them or not), beside a
  // stock prop whose collision comes from its .ide's dictionary (BM_plank01,
  // rubbish.xbd) - for when the island still has no collision.
  const MapStanding* under = nullptr;
  const MapStanding* bare = nullptr;
  float best = 1e30f;
  for (const auto& p : g.pieces) {
    const float dx = p.pos[0] - at[0], dy = p.pos[1] - at[1], d = dx * dx + dy * dy;
    if (p.collision && d < best)
      best = d, under = &p;
    if (!p.collision && !bare)
      bare = &p;
  }
  auto dump = [&](const char* what, uint32_t hash, int index) {
    const uint32_t mi = FindModelInfo(base, hash);
    std::string words;
    if (mi && Span(base, mi, 96))
      for (uint32_t w = 0; w < 24; ++w)
        words += fmt::format(" {:08X}", Read(base, mi + w * 4));
    std::string needs;
    const uint32_t mm = Span(base, kModelModuleIndex, 4) ? Read(base, kModelModuleIndex) : 0xFF;
    if (index >= 0)
      for (const uint32_t d : StreamDependencies(ctx, base, StreamModuleFirst(base, int(mm)) + uint32_t(index)))
        if (const auto de = ReadStreamEntry(base, d))
          needs += "; " + DescribeEntry(base, *de);
    REXLOG_INFO("gta4-trainer:   {} {:08X}: info {:08X}:{}{}", what, hash, mi, words.empty() ? " (none)" : words,
                needs.empty() ? std::string("; needs nothing listed") : needs);
  };
  auto index_of = [&](const MapStanding* p) {
    return p && p->model >= 0 ? g.models[size_t(p->model)].index : -1;
  };
  if (under)
    dump(("under the arrival " + g.models[size_t(under->model)].name).c_str(), under->hash, index_of(under));
  if (bare)
    dump(("no collision " + g.models[size_t(bare->model)].name).c_str(), bare->hash, index_of(bare));
  int stock = -1;
  const uint32_t plank = ModelHash("bm_plank01"), plank_info = FindModelInfo(base, plank);
  ForEachModelInfo(base, [&](uint32_t i, uint32_t mi) {
    if (mi != plank_info)
      return false;
    stock = int(i);
    return true;
  });
  dump("stock prop bm_plank01", plank, plank_info ? stock : -1);
  MapPhysicsSurvey(ctx, base, game);
}

// ---------------------------------------------------------------------------
// A place's life (v1.7.37): what makes an island more than its pieces. While
// a place stands and the player is on it:
//  - the vehicles the mod parks (its .wpl car generators: the Gator Keys'
//    sports cars at the hotel, its sailboat and helicopter; Wake Island's
//    dinghies, jeeps and the carrier's helicopters) stand there, made as the
//    player comes within reach and let go of again far away, as the game's
//    own generators do; one driven off is the player's;
//  - people who belong there (the set's ped models: tourists, joggers, hotel
//    staff; airmen, sailors) are kept about the player, walking from spot to
//    spot - the island has no paths for the game's own population, so the
//    trainer walks them on straight lines over its own ground grid
//    (ground.bin, as the game's ground probe does not see the pieces);
//  - the weather the place asks for holds (the Keys' endless sunshine, the
//    atoll's wind), and its own sound plays (gta4_island_sound.h).
// ---------------------------------------------------------------------------
struct LifeVehicle {
  uint32_t hash = 0, car = 0;
  float pos[3]{}, heading = 0.0f;  // (the game's coordinates)
  bool asked = false, taken = false;
  std::string model;
};
struct LifePed {
  uint32_t ped = 0;
  uint64_t next_task = 0;
  float spot[3]{};
};
struct LifeGround {
  float x0 = 0, y0 = 0;
  int cell = 0, w = 0, h = 0;
  std::vector<int16_t> cm;
  bool loaded = false, tried = false;
  std::optional<float> At(float x, float y) const {
    if (!loaded || cell <= 0)
      return std::nullopt;
    const int i = int(std::floor((x - x0) / float(cell))), j = int(std::floor((y - y0) / float(cell)));
    if (i < 0 || j < 0 || i >= w || j >= h)
      return std::nullopt;
    const int16_t v = cm[size_t(j) * size_t(w) + size_t(i)];
    if (v == -32768)
      return std::nullopt;
    return float(v) / 100.0f;
  }
};
// A creature about (v1.7.39): its parts as frozen objects the trainer moves
// every other frame - the body where the creature is, each part turned
// about its pivot by its swing - and its mind: a swimmer cruising the water
// and hunting a swimmer; a walker wandering the land; an amphibian lying
// on a shore, then moving to another.
struct LifeCreature {
  int kind = -1;  // in MapInfo::creatures
  std::vector<uint32_t> objects;  // one per part, the body first
  float pos[3]{}, heading = 0, speed = 0, roll = 0, clock = 0;
  float target[2]{};
  bool has_target = false, hunting = false, in_water = true;
  uint64_t born = 0, next_think = 0, rest_until = 0, bite_at = 0, next_bite = 0;
  int bites = 0;
};
struct MapLife {
  const gta4::addons::MapInfo* info = nullptr;
  std::vector<LifeVehicle> vehicles;
  std::vector<LifeCreature> creatures;
  std::vector<uint32_t> creature_models;  // every part model, asked for while the place stands
  bool creature_models_asked = false;
  uint64_t next_creature = 0, creature_frame = 0;
  int made_creatures = 0, creature_logs = 0;
  bool creature_frame_odd = false;
  std::vector<uint32_t> ped_models;
  std::vector<std::string> ped_names;
  std::vector<LifePed> peds;
  LifeGround ground;
  uint64_t next_tick = 0, next_ped = 0;
  bool weather_forced = false, ambience_on = false, models_asked = false;
  int made_vehicles = 0, made_peds = 0, logs = 0, cursor = 0;
  uint32_t seed = 0x1234567u;
  float Uniform() {
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) * (1.0f / 16777216.0f);
  }
};
MapLife g_life;
gta4::island::Ambience g_island_synth;
#if REX_PLATFORM_PS5
class IslandSource final : public gta4ps5::sound::Source {
 public:
  size_t Read(float* lr, size_t frames) override {
    g_island_synth.Render(lr, frames);
    return frames;
  }
  bool Rewind() override { return true; }
};
gta4ps5::sound::Voice* g_island_voice = nullptr;
#endif
constexpr float kLifeVehicleNear = 300.0f, kLifeVehicleFar = 420.0f;  // made within, let go beyond (m)
constexpr float kLifePedNear = 28.0f, kLifePedSpread = 45.0f, kLifePedFar = 130.0f;
constexpr uint32_t kPedTypeCivMale = 4, kPedTypeCivFemale = 5;

void LifeAmbience(bool on) {
#if REX_PLATFORM_PS5
  auto& L = g_life;
  if (on) {
    if (!g_island_voice) {
      gta4ps5::sound::StartOutput();
      g_island_voice = gta4ps5::sound::NewVoice();
      g_island_voice->game_sound = true;
    }
    const int character = L.info && L.info->ambience == "atoll" ? gta4::island::Ambience::kAtoll
                                                                  : gta4::island::Ambience::kKeys;
    g_island_synth.SetTarget(0.9f, character);
    if (!g_island_voice->Active()) {
      g_island_voice->SetVolume(1.0f);
      g_island_voice->Play(std::make_unique<IslandSource>(), true);
    }
  } else {
    g_island_synth.SetTarget(0.0f, gta4::island::Ambience::kKeys);
  }
#endif
}
// The world rebuilt under the life (a game loaded): forgotten, nothing
// deleted - its vehicles, people and creatures went with the world.
void LifeForget() {
  auto& L = g_life;
  if (!L.info)
    return;
  REXLOG_INFO("gta4-trainer: life of {} forgotten: its {} vehicles, {} people and {} creatures went with the world",
              L.info->name, L.made_vehicles, L.peds.size(), L.creatures.size());
  LifeAmbience(false);
  L = MapLife{};
}
// (once a pass: the voice stopped once its fade is done)
void LifeSoundTick() {
#if REX_PLATFORM_PS5
  if (g_island_voice && g_island_voice->Active() && !g_life.ambience_on && g_island_synth.Quiet())
    g_island_voice->Stop();
#endif
}

// A place's ground (ground.bin: the collision's walkable surface, the highest
// point of each 4 m cell, centimetres), read once per place (v1.7.43: for the
// teleports and the parked cars as well as the life).
const LifeGround& PlaceGround(const gta4::addons::MapInfo& info) {
  static std::unordered_map<const gta4::addons::MapInfo*, LifeGround> grounds;
  auto& g = grounds[&info];
  if (g.tried)
    return g;
  g.tried = true;
  const auto path = info.dir / "ground.bin";
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return g;
  char magic[4] = {};
  f.read(magic, 4);
  uint32_t cell = 0, w = 0, h = 0;
  f.read(reinterpret_cast<char*>(&g.x0), 4);
  f.read(reinterpret_cast<char*>(&g.y0), 4);
  f.read(reinterpret_cast<char*>(&cell), 4);
  f.read(reinterpret_cast<char*>(&w), 4);
  f.read(reinterpret_cast<char*>(&h), 4);
  if (std::memcmp(magic, "GRND", 4) != 0 || !f || cell == 0 || cell > 64 || w == 0 || h == 0 || w * h > 4000000u)
    return g;
  g.cell = int(cell);
  g.w = int(w);
  g.h = int(h);
  g.cm.resize(size_t(w) * h);
  f.read(reinterpret_cast<char*>(g.cm.data()), std::streamsize(g.cm.size() * 2));
  g.loaded = bool(f);
  // (the grid is in the mod's coordinates: moved to the game's here)
  g.x0 += info.shift[0];
  g.y0 += info.shift[1];
  REXLOG_INFO("gta4-trainer: the ground of {} read: {}x{} cells of {} m from ({:.0f}, {:.0f}){}", info.name, w, h,
              cell, g.x0, g.y0, g.loaded ? "" : " (short: not used)");
  return g;
}
void LifeLoadGround(const gta4::addons::MapInfo& info) { g_life.ground = PlaceGround(info); }
// The ground's height at (x, y), the game's coordinates: the set's own grid.
std::optional<float> LifeGroundAt(float x, float y) {
  const auto z = g_life.ground.At(x, y);
  if (!z || !g_life.info)
    return std::nullopt;
  return *z + MapDz(*g_life.info);
}
std::optional<float> PlaceGroundUnder(float x, float y) {
  const auto& g = g_map;
  if (!g.info || g.phase != 2)
    return std::nullopt;
  const std::array<float, 3> p{x, y, g.info->sea_dst};
  if (!OnMap(*g.info, p, 0.0f))
    return std::nullopt;
  const auto z = PlaceGround(*g.info).At(x, y);
  if (!z)
    return std::nullopt;
  return *z + MapDz(*g.info);
}

void LifeStart(PPCContext& ctx, uint8_t* base, const gta4::addons::MapInfo& info) {
  auto& L = g_life;
  L = MapLife{};
  L.info = &info;
  for (const auto& v : info.vehicles) {
    LifeVehicle lv;
    lv.model = v.model;
    lv.hash = ModelHash(v.model);
    const auto at = MapPoint(info, v.pos[0], v.pos[1], v.pos[2]);
    for (int a = 0; a < 3; ++a)
      lv.pos[a] = at[size_t(a)];
    lv.heading = v.heading;
    L.vehicles.push_back(lv);
  }
  for (const auto& n : info.peds) {
    const uint32_t hash = ModelHash(n);
    const auto known = Invoke(ctx, base, "IS_MODEL_IN_CDIMAGE", {hash});
    if (known && !known->r0())
      continue;
    L.ped_models.push_back(hash);
    L.ped_names.push_back(n);
  }
  for (const auto& k : info.creatures)
    for (const auto& part : k.parts)
      L.creature_models.push_back(ModelHash(part.model));
  LifeLoadGround(info);
  REXLOG_INFO("gta4-trainer: life of {}: {} vehicles parked, {} ped models ({} about), weather {}, ambience {}, {} "
              "kinds of creature ({} part models)",
              info.name, L.vehicles.size(), L.ped_models.size(), info.wander, info.weather,
              info.ambience.empty() ? "none" : info.ambience, info.creatures.size(), L.creature_models.size());
}

void LifeDeleteCreature(PPCContext& ctx, uint8_t* base, LifeCreature& c) {
  for (auto& o : c.objects)
    DeleteObject(ctx, base, o);
  c.objects.clear();
}

void LifeStop(PPCContext& ctx, uint8_t* base, const char* why) {
  auto& L = g_life;
  if (!L.info)
    return;
  int cars = 0, peds = 0;
  for (auto& v : L.vehicles) {
    if (!v.car)
      continue;
    const auto exists = Invoke(ctx, base, "DOES_VEHICLE_EXIST", {v.car});
    if (exists && exists->r0()) {
      Invoke(ctx, base, "MARK_CAR_AS_NO_LONGER_NEEDED", {0}, 0, v.car);  // (the game lets it go)
      if (!v.taken) {
        Invoke(ctx, base, "DELETE_CAR", {0}, 0, v.car);
        ++cars;
      }
    }
    v.car = 0;
  }
  for (auto& p : L.peds) {
    const auto exists = Invoke(ctx, base, "DOES_CHAR_EXIST", {p.ped});
    if (exists && exists->r0()) {
      Invoke(ctx, base, "DELETE_CHAR", {0}, 0, p.ped);
      ++peds;
    }
  }
  L.peds.clear();
  for (const uint32_t h : L.ped_models)
    Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {h});
  int creatures = 0;
  for (auto& c : L.creatures) {
    LifeDeleteCreature(ctx, base, c);
    ++creatures;
  }
  L.creatures.clear();
  for (const uint32_t h : L.creature_models)
    Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {h});
  if (L.weather_forced)
    Invoke(ctx, base, "RELEASE_WEATHER", {});
  LifeAmbience(false);
  REXLOG_INFO("gta4-trainer: life of {} ended ({}): {} vehicles, {} people and {} creatures removed", L.info->name,
              why, cars, peds, creatures);
  const auto* info = L.info;
  L = MapLife{};
  (void)info;
}

// A ped's next walk: a straight line to a point near its spot, or a pause.
void LifeTask(PPCContext& ctx, uint8_t* base, LifePed& p, uint64_t now) {
  auto& L = g_life;
  if (L.Uniform() < 0.3f) {
    Invoke(ctx, base, "TASK_STAND_STILL", {p.ped, uint32_t(4000 + L.Uniform() * 6000)});
    p.next_task = now + 5000 + uint64_t(L.Uniform() * 6000);
    return;
  }
  for (int tries = 0; tries < 6; ++tries) {
    const float a = L.Uniform() * 6.2831853f, d = 8.0f + L.Uniform() * 22.0f;
    const float x = p.spot[0] + std::cos(a) * d, y = p.spot[1] + std::sin(a) * d;
    const auto z = LifeGroundAt(x, y);
    if (!z || *z < L.info->sea_dst + 0.4f || std::fabs(*z - p.spot[2]) > 6.0f)
      continue;
    Invoke(ctx, base, "TASK_GO_STRAIGHT_TO_COORD", {p.ped, Bits(x), Bits(y), Bits(*z), 2, uint32_t(-1)});
    p.next_task = now + 7000 + uint64_t(L.Uniform() * 9000);
    return;
  }
  p.next_task = now + 4000;
}


// ---- the creatures (v1.7.39) ----
constexpr float kCreatureNear = 60.0f, kCreatureSpread = 120.0f, kCreatureFar = 360.0f;
constexpr float kSharkHunt = 90.0f, kSharkBite = 4.5f;

// A point the creature could live at: water (no ground of the place) for a
// swimmer, land with ground for a walker, a shore (shallow water or a low
// beach) for an amphibian. Returns the height the creature's origin sits at.
std::optional<float> CreatureSpot(const gta4::addons::CreatureKind& k, float x, float y, bool& water) {
  auto& L = g_life;
  const auto& info = *L.info;
  const std::array<float, 3> p{x, y, info.sea_dst};
  if (!OnMap(info, p, 40.0f))
    return std::nullopt;
  const auto ground = LifeGroundAt(x, y);
  const bool is_water = !ground || *ground < info.sea_dst - 0.3f;
  if (k.kind == "swimmer") {
    // deep enough for it (the place's collision reaches 3 m under the sea; deeper water has none)
    if (!is_water || (ground && *ground > info.sea_dst - 1.2f - k.height * 0.45f))
      return std::nullopt;
    water = true;
    return info.sea_dst - 1.15f;  // (the fin above the water)
  }
  if (k.kind == "walker") {
    if (!ground || *ground < info.sea_dst + 0.3f)
      return std::nullopt;
    water = false;
    return *ground - k.bottom;
  }
  // an amphibian: the shallows, or a beach within a metre and a half of the sea
  if (is_water) {
    water = true;
    return std::max(info.sea_dst - 0.25f, ground ? *ground - k.bottom : -1000.0f);
  }
  if (*ground > info.sea_dst + 1.5f)
    return std::nullopt;
  water = false;
  return *ground - k.bottom;
}

// A creature's sound to the player: its level by distance, its pan by where
// it is to the player's right.
void CreatureSound(const GameContext& game, const LifeCreature& c, bool bite) {
  const float dx = c.pos[0] - game.position[0], dy = c.pos[1] - game.position[1];
  const float d = std::sqrt(dx * dx + dy * dy);
  const float level = 1.0f / (1.0f + d / 18.0f);
  const float rx = game.forward[1], ry = -game.forward[0];  // (to the player's right)
  const float pan = d > 0.1f ? 0.5f + 0.5f * (dx * rx + dy * ry) / d : 0.5f;
  if (bite)
    g_island_synth.Bite(level, pan);
  else
    g_island_synth.Splash(level, pan);
}

// A new creature made: its parts as frozen objects (no collision: the bite
// is the trainer's own) at its spot.
bool CreatureMake(PPCContext& ctx, uint8_t* base, int kind, float x, float y, float z, float heading, bool water) {
  auto& L = g_life;
  const auto& k = L.info->creatures[size_t(kind)];
  LifeCreature c;
  c.kind = kind;
  c.pos[0] = x;
  c.pos[1] = y;
  c.pos[2] = z;
  c.heading = heading;
  c.in_water = water;
  c.born = NowMs();
  c.clock = L.Uniform() * 6.0f;
  for (const auto& part : k.parts) {
    const uint32_t hash = ModelHash(part.model);
    const auto obj = Invoke(ctx, base, "CREATE_OBJECT_NO_OFFSET", {hash, Bits(x), Bits(y), Bits(z), 0, 1}, 4);
    if (!obj || !obj->words[0]) {
      LifeDeleteCreature(ctx, base, c);
      return false;
    }
    c.objects.push_back(obj->words[0]);
    Invoke(ctx, base, "FREEZE_OBJECT_POSITION", {obj->words[0], 1});
    Invoke(ctx, base, "SET_OBJECT_INVINCIBLE", {obj->words[0], 1});
    Invoke(ctx, base, "SET_OBJECT_COLLISION", {obj->words[0], 0});
  }
  L.creatures.push_back(c);
  ++L.made_creatures;
  return true;
}

// Where the parts stand for the creature's pose: the body turned by its
// heading (and a little roll), each part turned about its pivot by its
// swing (the jaw by the bite).
void CreaturePose(PPCContext& ctx, uint8_t* base, LifeCreature& c, uint64_t now) {
  const auto& k = g_life.info->creatures[size_t(c.kind)];
  const Quat body = QNormalize(QMul(QAxis(0, 0, 1, c.heading), QAxis(0, 1, 0, c.roll)));
  const float bx = body.x, by = body.y, bz = body.z, bw = body.w;
  auto rotate = [&](const Quat& q, const float v[3], float out[3]) {
    // v' = q v q*
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    const float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    const float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
    out[0] = r00 * v[0] + r01 * v[1] + r02 * v[2];
    out[1] = r10 * v[0] + r11 * v[1] + r12 * v[2];
    out[2] = r20 * v[0] + r21 * v[1] + r22 * v[2];
  };
  (void)bx; (void)by; (void)bz; (void)bw;
  for (size_t i = 0; i < k.parts.size() && i < c.objects.size(); ++i) {
    const auto& part = k.parts[i];
    if (!c.objects[i])
      continue;
    float angle = 0.0f;
    if (part.name == "jaw") {
      // open for a third of a second at the bite
      const uint64_t since = c.bite_at ? now - c.bite_at : 1000;
      angle = since < 350 ? 0.45f * std::sin(float(since) / 350.0f * 3.14159265f) : 0.0f;
    } else if (part.amp != 0.0f) {
      const float rate = part.hz * (c.speed > 0.1f ? std::clamp(c.speed / 2.5f, 0.6f, 2.2f) : 0.35f);
      angle = part.amp * 3.14159265f / 180.0f * std::sin(6.2831853f * rate * c.clock + part.phase * 3.14159265f / 180.0f);
    }
    Quat q = body;
    float at[3] = {c.pos[0], c.pos[1], c.pos[2]};
    if (i > 0 && angle != 0.0f) {
      const Quat turn = QAxis(part.axis[0], part.axis[1], part.axis[2], angle);
      q = QNormalize(QMul(body, turn));
      // the pivot stays put: the object moves by p - R_part p, in the body's frame
      float rp[3], shift[3], world[3];
      rotate(turn, part.pivot, rp);
      shift[0] = part.pivot[0] - rp[0];
      shift[1] = part.pivot[1] - rp[1];
      shift[2] = part.pivot[2] - rp[2];
      rotate(body, shift, world);
      at[0] += world[0];
      at[1] += world[1];
      at[2] += world[2];
    }
    Invoke(ctx, base, "SET_OBJECT_COORDINATES", {c.objects[i], Bits(at[0]), Bits(at[1]), Bits(at[2])});
    Invoke(ctx, base, "SET_OBJECT_QUATERNION", {c.objects[i], Bits(q.x), Bits(q.y), Bits(q.z), Bits(q.w)});
  }
}

// A creature's mind, a few times a second: where to go, how fast.
void CreatureThink(PPCContext& ctx, uint8_t* base, const GameContext& game, LifeCreature& c, uint64_t now) {
  auto& L = g_life;
  const auto& info = *L.info;
  const auto& k = info.creatures[size_t(c.kind)];
  c.next_think = now + 400;
  const float dxp = game.position[0] - c.pos[0], dyp = game.position[1] - c.pos[1];
  const float player_d = std::sqrt(dxp * dxp + dyp * dyp);
  // a shark hunts a swimmer
  if (k.kind == "swimmer") {
    bool swimmer = false;
    if (player_d < kSharkHunt && game.ped && NativeKnown("IS_CHAR_IN_WATER")) {
      const auto w = Invoke(ctx, base, "IS_CHAR_IN_WATER", {game.ped});
      swimmer = w && w->r0();
    }
    if (swimmer && c.bites < 4) {
      c.hunting = true;
      c.target[0] = game.position[0];
      c.target[1] = game.position[1];
      c.has_target = true;
      if (player_d < kSharkBite && now >= c.next_bite) {
        c.bite_at = now;
        c.next_bite = now + 2800;
        ++c.bites;
        CreatureSound(game, c, true);
        CreatureSound(game, c, false);
        if (NativeKnown("GET_CHAR_HEALTH")) {
          if (const auto h = Invoke(ctx, base, "GET_CHAR_HEALTH", {game.ped, 0}, 1)) {
            const int health = int(h->r0());
            const int left = std::max(health - 35, 1);  // (never the last point: the game would count it a death by nothing)
            Invoke(ctx, base, "SET_CHAR_HEALTH", {game.ped, uint32_t(left)});
            if (L.creature_logs < 12) {
              ++L.creature_logs;
              REXLOG_INFO("gta4-trainer: life: the {} bit the player ({} -> {})", k.name, health, left);
            }
          }
        }
      }
      return;
    }
    if (c.hunting) {  // (lost it, or had its fill: off it goes)
      c.hunting = false;
      c.has_target = false;
      if (c.bites >= 4)
        c.rest_until = now + 20000;
    }
    if (c.rest_until && now > c.rest_until) {
      c.rest_until = 0;
      c.bites = 0;
    }
  }
  // an amphibian lies still between moves
  if (k.kind == "amphibian" && now < c.rest_until) {
    c.speed = 0.0f;
    return;
  }
  // a new target when none, or reached
  const float dx = c.target[0] - c.pos[0], dy = c.target[1] - c.pos[1];
  if (!c.has_target || dx * dx + dy * dy < 36.0f) {
    if (k.kind == "amphibian" && c.has_target && L.Uniform() < 0.7f) {
      c.rest_until = now + 12000 + uint64_t(L.Uniform() * 30000);
      c.has_target = false;
      c.speed = 0.0f;
      return;
    }
    for (int tries = 0; tries < 8; ++tries) {
      const float a = c.heading + (L.Uniform() - 0.5f) * 2.6f;  // (mostly onward)
      const float d = 30.0f + L.Uniform() * 70.0f;
      const float x = c.pos[0] - std::sin(a) * d, y = c.pos[1] + std::cos(a) * d;
      bool water = false;
      if (!CreatureSpot(k, x, y, water))
        continue;
      c.target[0] = x;
      c.target[1] = y;
      c.has_target = true;
      break;
    }
    if (!c.has_target) {
      c.speed = 0.0f;
      return;
    }
  }
}

// Every other frame: the creatures moved and posed; new ones made and far
// ones let go a few times a second (LifeTick).
void LifeCreatureFrame(PPCContext& ctx, uint8_t* base, const GameContext& game, uint64_t now) {
  auto& L = g_life;
  if (!L.info || L.creatures.empty())
    return;
  L.creature_frame_odd = !L.creature_frame_odd;
  if (L.creature_frame_odd)
    return;
  const float dt = L.creature_frame ? std::clamp(float(now - L.creature_frame) / 1000.0f, 0.0f, 0.1f) : 0.0f;
  L.creature_frame = now;
  const auto& info = *L.info;
  for (auto& c : L.creatures) {
    const auto& k = info.creatures[size_t(c.kind)];
    if (now >= c.next_think)
      CreatureThink(ctx, base, game, c, now);
    c.clock += dt;
    // the speed it wants, and the turn towards its target
    float want = 0.0f;
    if (c.has_target) {
      const float dx = c.target[0] - c.pos[0], dy = c.target[1] - c.pos[1];
      const float to = std::atan2(-dx, dy);  // (heading: 0 north, counter-clockwise)
      float diff = to - c.heading;
      while (diff > 3.14159265f)
        diff -= 6.2831853f;
      while (diff < -3.14159265f)
        diff += 6.2831853f;
      const float turn_rate = k.kind == "swimmer" ? (c.hunting ? 1.4f : 0.7f) : k.kind == "walker" ? 0.5f : 0.8f;
      const float step = std::clamp(diff, -turn_rate * dt, turn_rate * dt);
      c.heading += step;
      want = k.kind == "swimmer" ? (c.hunting ? 7.0f : 3.2f) : k.kind == "walker" ? 1.6f : (c.in_water ? 1.2f : 0.7f);
      if (std::fabs(diff) > 1.2f)
        want *= 0.4f;
      // a swimmer banks into its turns
      const float roll_want = k.kind == "swimmer" ? std::clamp(-diff * 0.5f, -0.35f, 0.35f) : 0.0f;
      c.roll += (roll_want - c.roll) * std::min(1.0f, dt * 2.0f);
    } else {
      c.roll += (0.0f - c.roll) * std::min(1.0f, dt * 2.0f);
    }
    c.speed += (want - c.speed) * std::min(1.0f, dt * 1.5f);
    if (c.speed > 0.01f) {
      const float nx = c.pos[0] - std::sin(c.heading) * c.speed * dt, ny = c.pos[1] + std::cos(c.heading) * c.speed * dt;
      bool water = c.in_water;
      if (const auto z = CreatureSpot(k, nx, ny, water)) {
        c.pos[0] = nx;
        c.pos[1] = ny;
        if (water && !c.in_water && k.kind == "amphibian")
          CreatureSound(game, c, false);
        c.in_water = water;
        // the height follows the ground (or the water) smoothly; a swimmer bobs
        float zt = *z;
        if (k.kind == "swimmer")
          zt += 0.18f * std::sin(c.clock * 0.9f);
        c.pos[2] += (zt - c.pos[2]) * std::min(1.0f, dt * 3.0f);
      } else {
        c.has_target = false;  // (the way ahead is not its element: another target)
        c.speed *= 0.5f;
      }
    } else if (k.kind == "swimmer") {
      c.pos[2] += ((info.sea_dst - 1.15f + 0.18f * std::sin(c.clock * 0.9f)) - c.pos[2]) * std::min(1.0f, dt * 3.0f);
    }
    CreaturePose(ctx, base, c, now);
  }
}

// A few times a second: creatures gone far are let go; new ones made while
// fewer than wanted are about, in their element near the player.
void LifeCreatureTick(PPCContext& ctx, uint8_t* base, const GameContext& game, uint64_t now, bool on) {
  auto& L = g_life;
  const auto& info = *L.info;
  if (info.creatures.empty())
    return;
  if (!L.creature_models_asked) {
    for (const uint32_t h : L.creature_models)
      Invoke(ctx, base, "REQUEST_MODEL", {h});
    L.creature_models_asked = true;
  }
  for (auto it = L.creatures.begin(); it != L.creatures.end();) {
    const float dx = it->pos[0] - game.position[0], dy = it->pos[1] - game.position[1];
    bool keep = dx * dx + dy * dy < kCreatureFar * kCreatureFar;
    if (keep && !it->objects.empty()) {
      const auto exists = Invoke(ctx, base, "DOES_OBJECT_EXIST", {it->objects[0]});
      keep = exists && exists->r0();
    }
    if (!keep) {
      LifeDeleteCreature(ctx, base, *it);
      it = L.creatures.erase(it);
      continue;
    }
    ++it;
  }
  if (!on || now < L.next_creature || !L.ground.loaded)
    return;
  L.next_creature = now + 1200;
  for (size_t kind = 0; kind < info.creatures.size(); ++kind) {
    const auto& k = info.creatures[kind];
    const int about = int(std::count_if(L.creatures.begin(), L.creatures.end(),
                                        [&](const LifeCreature& c) { return c.kind == int(kind); }));
    if (about >= k.count || k.parts.empty())
      continue;
    bool loaded = true;
    for (const auto& part : k.parts) {
      const uint32_t hash = ModelHash(part.model);
      const auto in = Invoke(ctx, base, "HAS_MODEL_LOADED", {hash});
      if (!in || !in->r0()) {
        Invoke(ctx, base, "REQUEST_MODEL", {hash});
        loaded = false;
      }
    }
    if (!loaded)
      continue;
    for (int tries = 0; tries < 10; ++tries) {
      const float a = L.Uniform() * 6.2831853f, d = kCreatureNear + L.Uniform() * kCreatureSpread;
      const float x = game.position[0] + std::cos(a) * d, y = game.position[1] + std::sin(a) * d;
      bool water = false;
      const auto z = CreatureSpot(k, x, y, water);
      if (!z)
        continue;
      if (CreatureMake(ctx, base, int(kind), x, y, *z, L.Uniform() * 6.2831853f, water)) {
        if (L.creature_logs < 12) {
          ++L.creature_logs;
          REXLOG_INFO("gta4-trainer: life: a {} ({}) at ({:.1f}, {:.1f}, {:.2f}), {:.0f} m from the player ({} about)",
                      k.name, water ? "in the water" : "on land", x, y, *z, d, about + 1);
        }
      }
      break;
    }
  }
}

void LifeTick(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint64_t now) {
  auto& L = g_life;
  const auto* standing = MapStandingNow();
  if (!standing) {
    if (L.info)
      LifeStop(ctx, base, "the place went");
    return;
  }
  if (L.info != standing)
    LifeStart(ctx, base, *standing);
  if (now < L.next_tick)
    return;
  L.next_tick = now + 500;
  const auto& info = *L.info;
  const std::array<float, 3> here{game.position[0], game.position[1], game.position[2]};
  const bool on = OnMap(info, here, 250.0f);
  // the weather and the sound, while the player is on the place
  if (on && info.weather >= 0 && !L.weather_forced) {
    Invoke(ctx, base, "FORCE_WEATHER_NOW", {uint32_t(info.weather)});
    if (NativeKnown("FORCE_WEATHER"))
      Invoke(ctx, base, "FORCE_WEATHER", {uint32_t(info.weather)});
    L.weather_forced = true;
  } else if (!on && L.weather_forced) {
    Invoke(ctx, base, "RELEASE_WEATHER", {});
    L.weather_forced = false;
  }
  if (!info.ambience.empty() && on != L.ambience_on) {
    L.ambience_on = on;
    LifeAmbience(on);
  }
  // the vehicles: made within reach, let go of far away; one driven off is the player's
  const auto in_car = CurrentVehicle(ctx, base, game);
  int made = 0;
  for (auto& v : L.vehicles) {
    const float dx = v.pos[0] - here[0], dy = v.pos[1] - here[1];
    const float d = std::sqrt(dx * dx + dy * dy);
    if (v.car) {
      const auto exists = Invoke(ctx, base, "DOES_VEHICLE_EXIST", {v.car});
      if (!exists || !exists->r0()) {
        v.car = 0;
        v.taken = true;  // (gone: wrecked, or the game took it; not made again)
        continue;
      }
      if (in_car && *in_car == v.car) {
        v.taken = true;
        continue;
      }
      if (!v.taken) {
        if (const auto at = CarPosition(ctx, base, v.car)) {
          const float mx = (*at)[0] - v.pos[0], my = (*at)[1] - v.pos[1];
          if (mx * mx + my * my > 12.0f * 12.0f) {
            v.taken = true;  // (driven away: the player's now, the game's to keep or let go)
            Invoke(ctx, base, "MARK_CAR_AS_NO_LONGER_NEEDED", {0}, 0, v.car);
            v.car = 0;
            continue;
          }
        }
        if (d > kLifeVehicleFar) {
          Invoke(ctx, base, "DELETE_CAR", {0}, 0, v.car);
          v.car = 0;
        }
      }
      continue;
    }
    if (v.taken || d > kLifeVehicleNear || made >= 2)
      continue;
    if (!v.asked) {
      const auto known = Invoke(ctx, base, "IS_MODEL_IN_CDIMAGE", {v.hash});
      if (known && !known->r0()) {
        v.taken = true;  // (a model the game has not)
        continue;
      }
      Invoke(ctx, base, "REQUEST_MODEL", {v.hash});
      v.asked = true;
      continue;
    }
    const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {v.hash});
    if (!loaded || !loaded->r0()) {
      Invoke(ctx, base, "REQUEST_MODEL", {v.hash});
      continue;
    }
    const auto car = Invoke(ctx, base, "CREATE_CAR", {v.hash, Bits(v.pos[0]), Bits(v.pos[1]), Bits(v.pos[2] + 0.4f), 0, 1}, 4);
    if (!car || !car->words[0]) {
      v.taken = true;
      continue;
    }
    v.car = car->words[0];
    Invoke(ctx, base, "SET_CAR_HEADING", {v.car, Bits(v.heading)});
    // (on land: the mod parked it on its road; v1.7.43: the game's probe sees
    // none of the place's ground, so SET_CAR_ON_GROUND_PROPERLY left it where
    // it was made, asleep in the air - the car is woken to settle instead;
    // a boat floats where it is)
    if (v.pos[2] > info.sea_dst + 1.0f) {
      Invoke(ctx, base, "SET_CAR_ON_GROUND_PROPERLY", {v.car});
      Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {v.car, Bits(0.05f)});
    }
    Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {v.hash});
    ++made;
    ++L.made_vehicles;
    if (L.logs < 8) {
      ++L.logs;
      REXLOG_INFO("gta4-trainer: life: {} {:08X} parked at ({:.1f}, {:.1f}, {:.1f}) heading {:.0f}, {:.0f} m off",
                  v.model, v.car, v.pos[0], v.pos[1], v.pos[2], v.heading, d);
    }
  }
  // the people: those gone or far are forgotten; new ones come while fewer
  // than wanted are about; each walks on from time to time
  if (!L.models_asked && !L.ped_models.empty()) {
    for (const uint32_t h : L.ped_models)
      Invoke(ctx, base, "REQUEST_MODEL", {h});
    L.models_asked = true;
  }
  for (auto it = L.peds.begin(); it != L.peds.end();) {
    const auto exists = Invoke(ctx, base, "DOES_CHAR_EXIST", {it->ped});
    bool keep = exists && exists->r0();
    if (keep) {
      const auto dead = Invoke(ctx, base, "IS_CHAR_DEAD", {it->ped});
      if (dead && dead->r0())
        keep = false;
    }
    if (keep) {
      if (const auto c = Invoke(ctx, base, "GET_CHAR_COORDINATES", {it->ped, 0, 0, 0}, 1, 0, 3)) {
        const float dx = c->f(0) - here[0], dy = c->f(1) - here[1];
        if (dx * dx + dy * dy > kLifePedFar * kLifePedFar) {
          Invoke(ctx, base, "DELETE_CHAR", {0}, 0, it->ped);
          keep = false;
        }
      }
    }
    if (!keep) {
      it = L.peds.erase(it);
      continue;
    }
    if (now >= it->next_task)
      LifeTask(ctx, base, *it, now);
    ++it;
  }
  if (on && !L.ped_models.empty() && int(L.peds.size()) < info.wander && now >= L.next_ped && L.ground.loaded) {
    L.next_ped = now + 1500;
    for (int tries = 0; tries < 8; ++tries) {
      const uint32_t hash = L.ped_models[size_t(L.cursor) % L.ped_models.size()];
      const std::string& name = L.ped_names[size_t(L.cursor) % L.ped_names.size()];
      ++L.cursor;
      const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {hash});
      if (!loaded || !loaded->r0()) {
        Invoke(ctx, base, "REQUEST_MODEL", {hash});
        continue;
      }
      const float a = L.Uniform() * 6.2831853f, d = kLifePedNear + L.Uniform() * kLifePedSpread;
      const float x = here[0] + std::cos(a) * d, y = here[1] + std::sin(a) * d;
      const auto z = LifeGroundAt(x, y);
      if (!z || *z < info.sea_dst + 0.4f || *z > here[2] + 15.0f || *z < here[2] - 15.0f)
        continue;
      const uint32_t type = (!name.empty() && (name[0] == 'f' || name[0] == 'F')) ? kPedTypeCivFemale : kPedTypeCivMale;
      const auto made_ped = Invoke(ctx, base, "CREATE_CHAR", {type, hash, Bits(x), Bits(y), Bits(*z + 1.0f), 0, 1}, 5);
      if (!made_ped || !made_ped->words[0])
        continue;
      LifePed p;
      p.ped = made_ped->words[0];
      p.spot[0] = x;
      p.spot[1] = y;
      p.spot[2] = *z;
      if (NativeKnown("SET_CHAR_RANDOM_COMPONENT_VARIATION"))
        Invoke(ctx, base, "SET_CHAR_RANDOM_COMPONENT_VARIATION", {p.ped});
      Invoke(ctx, base, "SET_CHAR_HEADING", {p.ped, Bits(L.Uniform() * 360.0f)});
      LifeTask(ctx, base, p, now);
      L.peds.push_back(p);
      ++L.made_peds;
      if (L.made_peds <= 4)
        REXLOG_INFO("gta4-trainer: life: {} {:08X} at ({:.1f}, {:.1f}, {:.2f}), {:.0f} m from the player ({} about)",
                    name, p.ped, x, y, *z, d, L.peds.size());
      break;
    }
  }
  LifeCreatureTick(ctx, base, game, now, on);
  (void)s;
}

// Every frame in a time machine (a look a few times a second otherwise, and
// while a trip's effects play out). Returns the buttons it keeps from the
// game.
uint32_t TimeMachine(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint16_t buttons,
                     const DrivePad& pad) {
  auto& m = g_tm;
  const uint64_t now = NowMs();
  const float dt = m.last_ms ? std::clamp(float(now - m.last_ms) / 1000.0f, 0.0f, 0.05f) : 0.0f;
  m.last_ms = now;
  const bool effects = m.phase != 0 || m.fires_out || m.vents_out || m.frost_until;
  if (!m.in_car && !effects && now < m.next_probe)
    return 0;
  m.next_probe = now + 300;
  const uint16_t prev = m.prev_buttons;
  m.prev_buttons = buttons;
  auto pressed = [&](uint16_t b) { return (buttons & b) && !(prev & b) && !s.open; };
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  const int film = vehicle.car ? TimeMachineFilm(vehicle.model) : 0;
  // A trip under way carries on whatever happens (the car is ours till it is
  // back). Y is kept from the game too (v1.7.31): getting out of the car while
  // it was away left it to the world, which took it at once, unseen, and the
  // arrival then moved a car that was gone (v1.7.30's crash); Y turns a wait
  // for a place back.
  if (m.phase == 1) {
    const uint32_t held = film ? uint32_t(X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_DPAD_RIGHT |
                                          X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_Y)
                               : 0;
    const uint64_t gone = now - m.phase_at;
    if (!VehicleAddress(base, m.car)) {
      TripLost(ctx, base, game, m, s);
      return held;
    }
    // Where it comes back to, once known: the scene there asked for first.
    auto come_back = [&](const float* p, const Quat& q) {
      for (int a = 0; a < 3; ++a)
        m.dest[a] = p[a];
      m.dest_q = q;
      m.dest_set = true;
      Invoke(ctx, base, "REQUEST_COLLISION_AT_POSN", {Bits(p[0]), Bits(p[1]), Bits(p[2])});
      if (NativeKnown("LOAD_SCENE"))
        Invoke(ctx, base, "LOAD_SCENE", {Bits(p[0]), Bits(p[1]), Bits(p[2])});
      m.scene_at = now;
    };
    if ((m.trip_map || m.going_home) && !m.moved && !m.dest_set) {
      // The departure plays out where it left; then dark, and on ahead.
      if (gone < kGoneMs)
        return held;
      if (!m.veil_at) {
        m.veil_at = now;
        Veil(1.0f, 350);
        return held;
      }
      if (now - m.veil_at < 380)
        return held;
      std::array<float, 3> over{m.home[0], m.home[1], m.home[2]};
      if (m.trip_map)
        over = MapPoint(*m.trip_map, m.trip_map->arrive[0], m.trip_map->arrive[1], m.trip_map->arrive[2]);
      PlaceCar(ctx, base, m.car, over[0], over[1], over[2] + 60.0f);
      m.moved = true;
    }
    if (m.trip_map && !m.dest_set) {
      // To a place: away until it stands, then there.
      const auto* info = m.trip_map;
      const bool ready = g_map.info == info && g_map.loaded && MapPlace(ctx, base);
      if (!ready) {
        MapTick(ctx, base, game, s, now);
        const bool turn = pressed(X_INPUT_GAMEPAD_Y) || gone > kMapLoadLimitMs || g_map.info != info;
        if (!turn) {
          if (gone > 2500 && now >= m.next_wait_toast) {
            m.next_wait_toast = now + 1500;
            const bool models_in = g_map.known_count > 0 && g_map.loaded_count == g_map.known_count;
            const std::string what = models_in && !g_map.DictionariesIn()
                                         ? std::string("its ground loading")
                                         : fmt::format("{} of {} loaded", g_map.loaded_count, g_map.known_count);
            Toast(s, fmt::format("Arriving at {}... ({})  Y: turn back", info->label, what), 1600);
          }
          return held;
        }
        REXLOG_WARN("gta4-trainer: time machine: {} not ready after {} ms ({} of {} models){}; back where it left",
                    info->name, gone, g_map.loaded_count, g_map.known_count,
                    gone > kMapLoadLimitMs ? "" : " (turned back)");
        MapReport(ctx, base, "the trip turned back");
        m.arrive_toast = info->label + std::string(gone > kMapLoadLimitMs ? " didn't load in time" : " not reached") +
                         ": back where you left.";
        m.trip_map = nullptr;
        come_back(m.at, m.q);  // (the city there streamed in again first)
      } else {
        // On its road, facing along it; in the air (Part II flying) well over it.
        const auto at = MapPoint(*info, info->arrive[0], info->arrive[1], info->arrive[2]);
        const float lift = m.flying ? std::max(30.0f, m.altitude) : kTmContact;
        const float p[3] = {at[0], at[1], at[2] + lift};
        const float run = std::max(info->arrive[4] - 25.0f, 10.0f);
        m.gone_speed = std::min(m.gone_speed, std::sqrt(2.0f * 6.0f * run));
        REXLOG_INFO("gta4-trainer: time machine arriving at {} ({:.1f}, {:.1f}, {:.1f}), heading {:.0f}, after {} ms",
                    info->name, p[0], p[1], p[2], info->arrive[3], gone);
        m.arrive_toast = info->label;
        m.trip_map = nullptr;
        come_back(p, QAxis(0, 0, 1, info->arrive[3] * 3.14159265f / 180.0f));
      }
    } else if (m.going_home && !m.dest_set) {
      // Back to where it left the city; the place deleted behind it.
      m.going_home = false;
      m.unload_after = true;
      come_back(m.home, m.home_q);
    }
    if (m.dest_set) {
      if (gone < kGoneMs || now - m.scene_at < 600)  // (a moment for the scene after the load)
        return held;
      for (int a = 0; a < 3; ++a)
        m.at[a] = m.dest[a];
      m.q = m.dest_q;
      m.dest_set = false;
      Arrive(ctx, base, game, m);
      if (m.unload_after) {
        m.unload_after = false;
        g_map.unload_at = now + 2000;
      } else if (MapStandingNow()) {
        g_map.check_at = now + 3000;  // (what stands, looked at once the arrival has settled)
      }
      if (!m.arrive_toast.empty())
        Toast(s, m.arrive_toast, 3000);
      m.arrive_toast.clear();
      return held;
    }
    if (gone >= kGoneMs)  // (through time only: back where it left)
      Arrive(ctx, base, game, m);
    return held;
  }
  if (effects && VehicleAddress(base, m.car)) {
    AfterTrip(ctx, base, m, now);
  } else if (effects) {
    // (the car gone or wrecked meanwhile: the trails put out, the rest let go)
    ClearFires(ctx, base, m);
    StopVents(ctx, base, m);
    m.frost_until = 0;
    m.booms_left = 0;
    m.phase = 0;
  }
  if (!film) {
    if (m.in_car) {
      g_tm_synth.SetFlux(0.0f);
      g_tm_synth.SetCharge(0.0f);
      g_tm_synth.SetHover(0.0f);
      m.hover = m.landing = m.hover_form = false;
      if (m.fold > 0.0f) {  // (left while flying: its wheels come down as it drops)
        m.fold = 0.0f;
        FoldWheels(ctx, base, m, 0.0f);
      }
    }
    m.in_car = false;
    g_tm_in_car.store(false, std::memory_order_relaxed);
    TimeMachineVoice(false);
    return 0;
  }
  if (!m.in_car || m.car != *vehicle.car) {
    if (!(m.in_car && m.film == 2 && film == 2))  // (the hover swap keeps everything)
      m.hover = false;
    m.car = *vehicle.car;
    m.model = vehicle.model;
    m.hover_form = vehicle.model == ModelHash("DELHOV");  // (a v1.7.21 flying form)
    m.fold = 0.0f;
    m.wheel_bones_car = 0;
    m.film = film;
    m.prev_buttons = buttons;  // (no edge from a button held getting in)
    if (m.log < 6) {
      ++m.log;
      REXLOG_INFO("gta4-trainer: in the time machine (Part {}{}); circuits {}, reactor {}", film,
                  m.hover_form ? ", flying form" : "", m.circuits ? "on" : "off", m.charged ? "charged" : "spent");
    }
  }
  m.in_car = true;
  g_tm_in_car.store(true, std::memory_order_relaxed);
  TimeMachineVoice(true);
  FollowClock(ctx, base, m);
  float speed = 0.0f;
  if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {m.car, 0}, 1))
    speed = std::fabs(v->f(0));
  m.speed = speed;
  // Part II's driving model, kept loaded (a v1.7.21 flying form lands as it).
  if (film == 2 && !m.models_requested) {
    Invoke(ctx, base, "REQUEST_MODEL", {ModelHash("DELTWO")});
    m.models_requested = true;
  }
  const bool legacy_form = m.model == ModelHash("DELHOV");
  // d-pad down: the time circuits.
  if (pressed(X_INPUT_GAMEPAD_DPAD_DOWN) || s.tm_toggle_circuits) {
    s.tm_toggle_circuits = false;
    m.circuits = !m.circuits;
    if (m.circuits)
      g_tm_synth.CircuitsOn();
    else
      g_tm_synth.CircuitsOff();
    Toast(s, m.circuits ? "Time circuits on." : "Time circuits off.", 1200);
  }
  // L1 at a standstill (or the menu): the reactor reloaded.
  if ((pressed(X_INPUT_GAMEPAD_LEFT_SHOULDER) || s.tm_refuel) && speed < 1.5f) {
    s.tm_refuel = false;
    if (m.charged) {
      Toast(s, film == 1 ? "The reactor is already loaded." : "Mr. Fusion is already charged.", 1500);
    } else {
      m.charged = true;
      if (film == 1)
        g_tm_synth.Plutonium();
      else
        g_tm_synth.Fusion();
      Toast(s, film == 1 ? "Plutonium loaded: 1.21 gigawatts." : "Mr. Fusion charged.", 2000);
    }
  } else if (s.tm_refuel && speed >= 1.5f) {
    s.tm_refuel = false;
    Toast(s, "Stop the car first.", 1200);
  }
  // Part II, d-pad right: fly (the wheels fold) or land.
  uint32_t keep = X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (film == 2) {
    keep |= X_INPUT_GAMEPAD_DPAD_RIGHT;
    if (pressed(X_INPUT_GAMEPAD_DPAD_RIGHT) || s.tm_toggle_hover) {
      s.tm_toggle_hover = false;
      if (legacy_form && !m.hover) {
        // A v1.7.21 flying form standing about: its wheels come down (the
        // driving model swapped in, as that version did).
        if (SwapForm(ctx, base, game, m, false, s))
          Toast(s, "Wheels down.", 1200);
      } else if (!m.hover) {
        m.hover = m.hover_form = true;
        m.landing = false;
        m.fwd = speed;
        m.climb = 3.0f;  // up off the road
        m.placed = false;
        m.landing_for = 0.0f;
        if (const auto at = CarPosition(ctx, base, m.car)) {  // the road it is on: a floor
          m.floor_z = (*at)[2] - kTmContact;
          m.floor_x = (*at)[0];
          m.floor_y = (*at)[1];
        }
        m.pitch = m.bank = 0.0f;
        m.stalled_for = 0.0f;
        if (const auto h = Invoke(ctx, base, "GET_CAR_HEADING", {m.car, 0}, 1))
          m.heading = h->f(0) * 3.14159265f / 180.0f;
        g_tm_synth.Convert(true);
        Toast(s, "Flight mode: left stick to fly, R2 up, L2 down. D-pad right lands.", 3500);
      } else if (!m.landing) {
        m.landing = true;
        m.landing_for = 0.0f;
        if (m.hover_form && !legacy_form) {  // the wheels come down as it descends
          m.hover_form = false;
          g_tm_synth.Convert(false);
        }
        Toast(s, "Landing...", 1500);
      }
    }
  }
  if (m.hover) {
    Hover(ctx, base, m, pad, dt);
    if (m.sink_stalled)
      m.stalled_for += dt;
    else
      m.stalled_for = 0.0f;
    // Coming down: the wheels unfold over the last few metres.
    if (m.landing && m.hover_form && m.altitude < 4.0f && !legacy_form) {
      m.hover_form = false;
      g_tm_synth.Convert(false);
    }
    if (m.landing)
      m.landing_for += dt;
    // (10 s coming down and still no ground: the physics takes it the rest of
    // the way, which no surface lets through)
    if (m.landing && (m.altitude < 1.5f || m.stalled_for > 0.6f || m.landing_for > 10.0f)) {
      if (legacy_form && !SwapForm(ctx, base, game, m, false, s)) {
        if (m.landing_log < 3) {
          ++m.landing_log;
          REXLOG_WARN("gta4-trainer: time machine: landing at {:.1f} m but the wheels could not come down yet",
                      m.altitude);
        }
      } else {
        // Touchdown: back on its wheels, the game driving it again.
        m.hover = m.landing = m.hover_form = false;
        m.stalled_for = 0.0f;
        g_tm_synth.SetHover(0.0f);
        Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {m.car, Bits(m.fwd)});
        Toast(s, "Wheels down.", 1200);
      }
    }
  } else {
    g_tm_synth.SetHover(0.0f);
  }
  // The wheels: folding while flying, down otherwise (0.8 s either way).
  if (film == 2 && !legacy_form) {
    const float want = m.hover && m.hover_form ? 1.0f : 0.0f;
    const float was = m.fold;
    m.fold = want > m.fold ? std::min(want, m.fold + dt / 0.8f) : std::max(want, m.fold - dt / 0.8f);
    if (m.fold != was || m.fold > 0.0f)
      FoldWheels(ctx, base, m, m.fold);
  }
  // The build-up and the trip.
  const bool ready = m.circuits && m.charged && m.phase == 0;
  m.charge = ready ? std::clamp((speed - kMph60) / (kMph88 - kMph60), 0.0f, 1.0f) : 0.0f;
  g_tm_synth.SetFlux(m.circuits && m.charged ? 0.3f + 0.7f * m.charge : 0.0f);
  g_tm_synth.SetCharge(m.charge * m.charge);
  if (ready && speed > kMph60 - 5.0f)
    TrackWheels(ctx, base, m);
  if (ready)
    BuildUp(ctx, base, m, now);
  if (ready && speed >= kMph88)
    Depart(ctx, base, game, m, s);
  else if (m.circuits && !m.charged && speed >= kMph88 && now >= m.next_spark) {
    m.next_spark = now + 4000;
    g_tm_synth.Empty();
    Toast(s, film == 1 ? "No plutonium: the reactor is spent (L1 standing still)." :
                         "Mr. Fusion is empty: refuel with L1 standing still.", 2500);
  }
  return keep;
}

// ---------------------------------------------------------------------------
// The Batman Beyond Batmobile (v1.7.44): the show's car, built from scratch
// (tools/car/beyond_body.py, beyond_build.py: no model taken from anywhere), a
// flying wing with no wheels of its own (the Infernus template's, hidden,
// hold it up when it rests). In it the trainer flies it as it flies the Part
// II DeLorean: the left stick drives and turns (it banks into the turn), R2
// climbs, L2 sinks, it hovers where it is with nothing pressed, kept clear of
// the ground; up to 60 m/s, 95 with the boost (d-pad right held, the twin
// thrusters flaming). L1 fires the Batmobile's lock-on missiles from the
// nose. D-pad down cloaks it - the car and its pilot unseen, the police
// losing him - and uncloaks it. Y brings it down to its hover height and to
// a stop, then the canopy rises (kBeyondCanopy) and the player is set down
// beside it; the car stays where it is, hovering (held in the air), until he
// gets back in or summons it. Summoned (the Vehicles page) it flies to the
// player from wherever it was left, climbing over what is in the way, and
// settles into a hover beside him; gone (the game took it), a new one comes
// in from the sky behind him. Heard all the while (gta4_beyond_engine.h: the
// turbines' whine with Doppler from outside, the hover thrum, the air rush,
// the boost, the canopy, the cloak, a launch). Those buttons are hidden from
// the game in the car.
// ---------------------------------------------------------------------------
gta4::beyond::EngineSynth g_beyond_synth;
#if REX_PLATFORM_PS5
class BeyondSource final : public gta4ps5::sound::Source {
 public:
  size_t Read(float* lr, size_t frames) override {
    g_beyond_synth.Render(lr, frames);
    return frames;
  }
  bool Rewind() override { return true; }
};
gta4ps5::sound::Voice* g_beyond_voice = nullptr;
#endif
// Starts the car's voice (want), or stops it once quiet.
void BeyondVoice(bool want) {
#if REX_PLATFORM_PS5
  if (!g_beyond_voice) {
    if (!want)
      return;
    gta4ps5::sound::StartOutput();
    g_beyond_voice = gta4ps5::sound::NewVoice();
    g_beyond_voice->game_sound = true;
  }
  if (want && !g_beyond_voice->Active()) {
    g_beyond_voice->SetVolume(1.0f);
    g_beyond_voice->Play(std::make_unique<BeyondSource>(), true);
  } else if (!want && g_beyond_voice->Active() && g_beyond_synth.Quiet()) {
    g_beyond_voice->Stop();
  }
#else
  (void)want;
#endif
}

struct BeyondState {
  uint32_t car = 0;  // the car it knows: the one flown, left hovering or summoned
  bool in_car = false;
  bool parked = false;     // left hovering, held in the air where the player got out
  bool cloaked = false;
  bool summoning = false;  // on its way to the player
  bool settling = false;   // Y in flight: coming down to its hover height to let the player out
  bool sounding = false;   // its voice wanted (in it; parked or flying within earshot)
  bool y_taken = false, exit_pulse = false;  // Y kept from the canopy while settling; Y given to it once settled
  bool spawn_pending = false;                // a summoned car waits for its model
  uint64_t spawn_since = 0;
  // The flight (as the time machine's Hover): speed along the nose, heading,
  // climb rate, bank and pitch, the held height.
  float fwd = 0.0f, heading = 0.0f, climb = 0.0f, bank = 0.0f, pitch = 0.0f, hold_z = 0.0f, altitude = 99.0f;
  float boost = 0.0f, spool = 0.0f;
  bool placed = false;
  float placed_z = 0.0f, placed_vz = 0.0f;
  float floor_z = -1e9f, floor_x = 0.0f, floor_y = 0.0f;
  std::array<uint32_t, 2> flames{};
  bool flaming = false;
  uint64_t rocket_ready = 0;
  bool rocket_reloading = false, models_requested = false;
  uint64_t next_probe = 0, last_ms = 0, next_wanted_clear = 0, settle_since = 0, summon_since = 0;
  uint16_t prev_buttons = 0;
  float listen_dist = -1.0f;  // the last distance heard from outside (for the closing speed)
  int log = 0, fly_log = 0, floor_log = 0, summon_log = 0;
};
BeyondState g_beyond;
constexpr float kBeyondClear = 0.9f;                            // its origin over the ground, hovering
constexpr float kBeyondCruise = 60.0f, kBeyondBoostSpeed = 95.0f;  // m/s
constexpr float kBeyondClimb = 12.0f;                           // m/s on R2 / L2
constexpr float kBeyondLauncher[3] = {0.0f, 3.0f, 0.12f};       // the nose's tip (car space)
constexpr float kBeyondJets[2][3] = {{-0.40f, -1.85f, 0.17f}, {0.40f, -1.85f, 0.17f}};  // the thrusters' mouths
constexpr uint64_t kBeyondReloadMs = 2500;

void BeyondCanopySound(bool opening) {
  g_beyond_synth.Canopy(opening);
  g_beyond_synth.Beam(!opening);  // (the beam lifts him in as it shuts; drops him out as it opens)
  BeyondVoice(true);
  g_beyond.sounding = true;
}
void BeyondLaunchSound() { g_beyond_synth.Launch(); }

void BeyondFlames(PPCContext& ctx, uint8_t* base, uint32_t car, bool on) {
  auto& m = g_beyond;
  if (!on) {
    for (auto& f : m.flames) {
      if (f)
        Invoke(ctx, base, "STOP_PTFX", {f});
      f = 0;
    }
    m.flaming = false;
    return;
  }
  if (m.flaming)
    return;
  m.flaming = true;
  for (size_t k = 0; k < 2; ++k) {
    const auto r = Invoke(ctx, base, "START_PTFX_ON_VEH",
                          {0, car, Bits(kBeyondJets[k][0]), Bits(kBeyondJets[k][1]), Bits(kBeyondJets[k][2]), 0, 0, 0,
                           Bits(1.1f)},
                          -1, 0, 1, 0, "weap_rocket");
    m.flames[k] = r ? r->r0() : 0;
  }
}

void BeyondHold(PPCContext& ctx, uint8_t* base, BeyondState& m, bool hold) {
  if (!m.car)
    return;
  Invoke(ctx, base, "FREEZE_CAR_POSITION", {m.car, hold ? 1u : 0u});
  m.parked = hold;
}

// The cloak: the car (and its pilot in it) unseen; the wanted level cleared
// while it holds (the police lose him).
void BeyondCloak(PPCContext& ctx, uint8_t* base, const GameContext& game, BeyondState& m, bool on, bool quiet = false) {
  if (m.car)
    Invoke(ctx, base, "SET_CAR_VISIBLE", {m.car, on ? 0u : 1u});
  if (NativeKnown("SET_CHAR_VISIBLE"))
    Invoke(ctx, base, "SET_CHAR_VISIBLE", {game.ped, on && m.in_car ? 0u : 1u});
  if (m.cloaked != on && !quiet)
    g_beyond_synth.Cloak(on);
  m.cloaked = on;
  g_beyond_cloaked.store(on, std::memory_order_relaxed);
  if (on)
    Invoke(ctx, base, "CLEAR_WANTED_LEVEL", {game.player});
}

// One step of flight. want_fwd: the speed asked along the nose (m/s), turn:
// -1..1 (right positive), up: -1..1 (R2 - L2, or the autopilot's). The
// attitude and the speed are set every frame as the DeLorean's are; the
// height is a held target, moved by `up` and by the ground (the car kept
// kBeyondClear over the higher of the ground under it and half a second
// ahead, and over any floor the physics pushed it up onto).
void BeyondFly(PPCContext& ctx, uint8_t* base, BeyondState& m, float want_fwd, float turn, float up, float dt) {
  const auto c = CarPosition(ctx, base, m.car);
  if (!c)
    return;
  const float accel_limit = want_fwd > m.fwd ? (m.boost > 0.5f ? 30.0f : 20.0f) : 26.0f;
  const float accel = std::clamp(want_fwd - m.fwd, -accel_limit * dt, accel_limit * dt);
  m.fwd += accel;
  m.heading -= turn * 1.5f * dt * (0.45f + 0.55f * std::min(1.0f, std::fabs(m.fwd) / 20.0f));
  const float want_climb = up * kBeyondClimb;
  m.climb += std::clamp(want_climb - m.climb, -22.0f * dt, 22.0f * dt);
  m.bank += (turn * 0.5f - m.bank) * std::min(1.0f, 4.0f * dt);
  m.pitch += (std::clamp(-accel / std::max(dt, 1e-3f) * 0.005f + m.climb * 0.02f, -0.3f, 0.3f) - m.pitch) *
             std::min(1.0f, 3.0f * dt);
  if (!m.placed)
    m.hold_z = (*c)[2];
  if ((*c)[2] > m.hold_z + 0.6f)  // (pushed up by something it met: it stays up)
    m.hold_z = (*c)[2];
  m.hold_z += m.climb * dt;
  float z = m.hold_z;
  std::optional<float> gz = GroundZ(ctx, base, (*c)[0], (*c)[1], (*c)[2] + 6.0f);
  {
    const float ahead = std::clamp(m.fwd, -10.0f, 60.0f) * 0.5f;
    const float hx = -std::sin(m.heading) * ahead, hy = std::cos(m.heading) * ahead;
    if (const auto g2 = GroundZ(ctx, base, (*c)[0] + hx, (*c)[1] + hy, (*c)[2] + 6.0f))
      if (!gz || *g2 > *gz)
        gz = g2;
  }
  // Coming down and pushed back up by the physics: a floor the probe missed.
  if (m.placed && m.climb < -0.5f && (*c)[2] > m.placed_z + m.placed_vz * dt + 0.04f) {
    m.floor_z = (*c)[2] - kBeyondClear;
    m.floor_x = (*c)[0];
    m.floor_y = (*c)[1];
    if (m.floor_log < 4) {
      ++m.floor_log;
      REXLOG_INFO("gta4-trainer: Beyond Batmobile: something solid under it at {:.1f} (the probe's ground {:.1f})",
                  m.floor_z, gz ? *gz : -999.0f);
    }
  }
  {
    const float dx = (*c)[0] - m.floor_x, dy = (*c)[1] - m.floor_y;
    if (m.floor_z > -1e8f && dx * dx + dy * dy < 36.0f && (!gz || m.floor_z > *gz))
      gz = m.floor_z;
  }
  if (gz && z < *gz + kBeyondClear) {
    z = *gz + kBeyondClear;
    m.hold_z = z;
    m.climb = std::max(m.climb, 0.0f);
  }
  m.altitude = gz ? z - *gz : 99.0f;
  const Quat q = QNormalize(QMul(QMul(QAxis(0, 0, 1, m.heading), QAxis(1, 0, 0, m.pitch)), QAxis(0, 1, 0, m.bank)));
  Invoke(ctx, base, "SET_VEHICLE_QUATERNION", {m.car, Bits(q.x), Bits(q.y), Bits(q.z), Bits(q.w)});
  PlaceCar(ctx, base, m.car, (*c)[0], (*c)[1], z);
  m.placed = true;
  m.placed_z = z;
  {
    const float nose[3] = {0.0f, 1.0f, 0.0f};
    float f[3];
    QRotate(q, nose, f);
    m.placed_vz = m.fwd * f[2];
  }
  Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {m.car, Bits(m.fwd)});
  // The turbines: idling in a hover, spooling up with speed, climb and boost.
  const float want_spool = std::clamp(
      gta4::beyond::EngineSynth::kIdle +
          (1.0f - gta4::beyond::EngineSynth::kIdle) *
              std::max({std::fabs(m.fwd) / kBeyondCruise, std::fabs(m.climb) / kBeyondClimb * 0.6f, m.boost}),
      0.0f, 1.0f);
  m.spool += (want_spool - m.spool) * std::min(1.0f, 3.0f * dt);
  if (m.fly_log < 4 && dt > 0.0f) {
    ++m.fly_log;
    REXLOG_INFO("gta4-trainer: Beyond Batmobile flying: {:.1f} m up, {:.1f} m/s ahead, climbing {:.1f} m/s",
                m.altitude, m.fwd, m.climb);
  }
}

// Takes the flight state from where the car is (getting in, or summoned).
void BeyondTakeOff(PPCContext& ctx, uint8_t* base, BeyondState& m, float climb) {
  m.placed = false;
  m.climb = climb;
  m.pitch = m.bank = 0.0f;
  m.floor_z = -1e9f;
  float speed = 0.0f;
  if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {m.car, 0}, 1))
    speed = std::fabs(v->f(0));
  m.fwd = std::min(speed, 10.0f);
  if (const auto h = Invoke(ctx, base, "GET_CAR_HEADING", {m.car, 0}, 1))
    m.heading = h->f(0) * 3.14159265f / 180.0f;
  if (const auto at = CarPosition(ctx, base, m.car)) {
    m.floor_z = (*at)[2] - kBeyondClear;  // (where it rests: a floor)
    m.floor_x = (*at)[0];
    m.floor_y = (*at)[1];
  }
}

bool BeyondCarAlive(PPCContext& ctx, uint8_t* base, BeyondState& m) {
  if (!m.car)
    return false;
  const auto exists = Invoke(ctx, base, "DOES_VEHICLE_EXIST", {m.car});
  if (!exists || !exists->r0()) {
    m.car = 0;
    m.parked = m.summoning = false;
    return false;
  }
  const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {m.car});
  return dead && !dead->r0();
}

// Summoned: the car flies to a spot beside the player (his right, 4 m off),
// up to 50 m/s, climbing to 14 m over the ground on the way and coming down
// over the last stretch; it settles there, hovering. Returns true when it
// has arrived.
bool BeyondAutopilot(PPCContext& ctx, uint8_t* base, const GameContext& game, BeyondState& m, float dt) {
  const auto c = CarPosition(ctx, base, m.car);
  if (!c)
    return false;
  const float rx = game.forward[1], ry = -game.forward[0];  // the player's right
  const float tx = game.position[0] + rx * 4.0f, ty = game.position[1] + ry * 4.0f;
  const float dx = tx - (*c)[0], dy = ty - (*c)[1];
  const float dist = std::sqrt(dx * dx + dy * dy);
  // The heading to face it: the nose is (-sin h, cos h).
  float err = 0.0f;
  if (dist > 1.0f) {
    const float want = std::atan2(-dx, dy);
    err = want - m.heading;
    while (err > 3.14159265f) err -= 6.2831853f;
    while (err < -3.14159265f) err += 6.2831853f;
  }
  const float turn = -std::clamp(err / 0.6f, -1.0f, 1.0f);
  // Slow to turn on the spot when it points away; slower as it closes in.
  float want_fwd = std::min(50.0f, dist / 2.0f);
  if (std::fabs(err) > 1.0f)
    want_fwd = std::min(want_fwd, 3.0f);
  else if (std::fabs(err) > 0.4f)
    want_fwd = std::min(want_fwd, 12.0f);
  if (dist < 1.2f)
    want_fwd = 0.0f;
  // Height: cruise 14 m over the ground far off, the hover height near.
  float up;
  if (dist > 45.0f) {
    const float want_z = (m.altitude < 90.0f ? (*c)[2] - m.altitude : (*c)[2]) + 14.0f;
    up = std::clamp((want_z - (*c)[2]) / 5.0f, -1.0f, 1.0f);
  } else {
    const float want_z = game.position[2] - 1.0f + kBeyondClear + 0.3f;  // (the ped's origin a metre up)
    up = std::clamp((want_z - (*c)[2]) / 4.0f, -0.6f, 0.6f);
  }
  BeyondFly(ctx, base, m, want_fwd, turn, up, dt);
  if (m.summon_log < 3 && dt > 0.0f) {
    ++m.summon_log;
    REXLOG_INFO("gta4-trainer: Beyond Batmobile summoned: {:.0f} m off, {:.1f} m up, {:.0f} m/s", dist, m.altitude,
                m.fwd);
  }
  return dist < 2.0f && std::fabs(m.fwd) < 1.5f && m.altitude < kBeyondClear + 1.2f;
}

// Every frame in the car (and while it hovers or flies nearby); a look a few
// times a second otherwise. Returns the buttons it takes from the game.
uint32_t BeyondTick(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint16_t buttons,
                    const DrivePad& pad) {
  auto& m = g_beyond;
  const uint64_t now = NowMs();
  const float dt = m.last_ms ? std::clamp(float(now - m.last_ms) / 1000.0f, 0.0f, 0.05f) : 0.0f;
  m.last_ms = now;
  m.y_taken = m.exit_pulse = false;
  // (a car left hovering out of earshot is looked at a few times a second)
  const bool busy = m.in_car || m.summoning || m.sounding || m.spawn_pending || s.beyond_summon ||
                    s.beyond_toggle_cloak;
  if (!busy && now < m.next_probe)
    return 0;
  m.next_probe = now + 300;
  const uint16_t prev = m.prev_buttons;
  m.prev_buttons = buttons;
  auto pressed = [&](uint16_t b) { return (buttons & b) && !(prev & b) && !s.open; };
  static const uint32_t kBeyond = ModelHash("BEYOND");
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  const bool in = vehicle.car && vehicle.model == kBeyond;

  // ---- summoned (the menu)
  if (s.beyond_summon) {
    s.beyond_summon = false;
    if (in) {
      Toast(s, "You're in it.", 1500);
    } else if (BeyondCarAlive(ctx, base, m)) {
      BeyondHold(ctx, base, m, false);
      m.summoning = true;
      m.summon_since = now;
      m.summon_log = 0;
      m.settling = false;
      BeyondTakeOff(ctx, base, m, 4.0f);
      if (m.cloaked)
        BeyondCloak(ctx, base, game, m, false);
      Toast(s, "The Batmobile is on its way.", 2500);
    } else {
      // Gone: a new one from the sky behind the player.
      Invoke(ctx, base, "REQUEST_MODEL", {kBeyond});
      m.spawn_pending = true;
      m.spawn_since = now;
      Toast(s, "The Batmobile is on its way.", 2500);
    }
  }
  if (m.spawn_pending) {
    const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {kBeyond});
    if (loaded && loaded->r0()) {
      const float back = 160.0f;
      const float x = game.position[0] - game.forward[0] * back, y = game.position[1] - game.forward[1] * back;
      float z = game.position[2] + 30.0f;
      if (const auto gz = GroundZ(ctx, base, x, y, z + 50.0f))
        z = std::max(z, *gz + 20.0f);
      const auto made = Invoke(ctx, base, "CREATE_CAR", {kBeyond, Bits(x), Bits(y), Bits(z), 0, 1}, 4);
      m.spawn_pending = false;
      Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {kBeyond});
      if (made && made->words[0]) {
        m.car = made->words[0];
        m.cloaked = false;
        m.summoning = true;
        m.summon_since = now;
        m.summon_log = 0;
        m.settling = false;
        BeyondTakeOff(ctx, base, m, 0.0f);
        m.hold_z = z;
        if (const auto h = Invoke(ctx, base, "GET_CAR_HEADING", {m.car, 0}, 1))
          m.heading = h->f(0) * 3.14159265f / 180.0f;
        Invoke(ctx, base, "MARK_CAR_AS_NO_LONGER_NEEDED", {0}, 0, m.car);
        REXLOG_INFO("gta4-trainer: Beyond Batmobile {:08X} summoned from ({:.0f}, {:.0f}, {:.0f})", m.car, x, y, z);
      } else {
        Toast(s, "The Batmobile could not come.", 2000);
      }
    } else if (now - m.spawn_since > 15000) {
      m.spawn_pending = false;
      Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {kBeyond});
      Toast(s, "The Batmobile's model did not load.", 2500);
    }
  }

  // ---- on foot (or in something else)
  if (!in) {
    if (m.in_car) {
      // Just out of it: it stays where it is, hovering; the cloak drops with
      // him (an unseen car is a lost one), the flames go out.
      m.in_car = false;
      m.settling = false;
      BeyondFlames(ctx, base, m.car, false);
      if (m.cloaked)
        BeyondCloak(ctx, base, game, m, false);
      if (BeyondCarAlive(ctx, base, m)) {
        BeyondHold(ctx, base, m, true);
        if (m.log < 8) {
          ++m.log;
          REXLOG_INFO("gta4-trainer: Beyond Batmobile left hovering {:.1f} m up (car {:08X})", m.altitude, m.car);
        }
      }
      m.boost = 0.0f;
    }
    if (s.beyond_toggle_cloak) {
      s.beyond_toggle_cloak = false;
      Toast(s, "The cloak works from inside the Batmobile.", 2000);
    }
    const bool alive = (m.parked || m.summoning) && BeyondCarAlive(ctx, base, m);
    if (m.summoning && alive) {
      if (BeyondAutopilot(ctx, base, game, m, dt)) {
        m.summoning = false;
        BeyondHold(ctx, base, m, true);
        Toast(s, "The Batmobile is here.", 2000);
        REXLOG_INFO("gta4-trainer: Beyond Batmobile arrived (car {:08X})", m.car);
      } else if (now - m.summon_since > 90000) {
        m.summoning = false;  // (lost its way: left where it is, hovering)
        BeyondHold(ctx, base, m, true);
        Toast(s, "The Batmobile could not reach you; it waits where it is.", 3000);
      }
    } else if (m.summoning) {
      // (the game took it on the way: a new one comes instead)
      m.summoning = false;
      if (!m.spawn_pending) {
        Invoke(ctx, base, "REQUEST_MODEL", {kBeyond});
        m.spawn_pending = true;
        m.spawn_since = now;
        REXLOG_INFO("gta4-trainer: the summoned Beyond Batmobile is gone; another comes");
      }
    }
    // Heard from outside while it is near: the hover thrum, or the flight.
    float dist = 1e9f;
    if (alive) {
      if (const auto c = CarPosition(ctx, base, m.car)) {
        const float dx = (*c)[0] - game.position[0], dy = (*c)[1] - game.position[1], dz = (*c)[2] - game.position[2];
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      }
    }
    if (alive && dist < 140.0f) {
      const float closing = m.listen_dist >= 0.0f && dt > 0.0f ? (m.listen_dist - dist) / dt : 0.0f;
      m.listen_dist = dist;
      const float spool = m.summoning ? m.spool : gta4::beyond::EngineSynth::kIdle;
      g_beyond_synth.SetListener(true, dist, std::clamp(closing, -120.0f, 120.0f));
      g_beyond_synth.SetTarget(spool, m.summoning ? std::fabs(m.fwd) : 0.0f, 0.0f, 0.5f);
      BeyondVoice(true);
      m.sounding = true;
    } else {
      m.listen_dist = -1.0f;
      if (m.sounding) {
        g_beyond_synth.SetTarget(0.0f, 0.0f, 0.0f, 0.5f);  // winds down
        BeyondVoice(false);
#if REX_PLATFORM_PS5
        m.sounding = g_beyond_voice && g_beyond_voice->Active();
#else
        m.sounding = false;
#endif
      }
    }
    if (m.models_requested && !m.in_car && !GadgetsBusy()) {
      Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {ModelHash(kRocketModel)});
      m.models_requested = false;
    }
    return 0;
  }

  // ---- in the car
  if (!m.in_car || m.car != *vehicle.car) {
    m.car = *vehicle.car;
    m.in_car = true;
    m.summoning = m.settling = false;
    m.prev_buttons = buttons;  // (no edge from a button held getting in)
    BeyondHold(ctx, base, m, false);
    BeyondTakeOff(ctx, base, m, 2.0f);
    m.boost = 0.0f;
    m.fly_log = 0;
    if (m.cloaked)
      BeyondCloak(ctx, base, game, m, true, true);  // (the pilot unseen with it)
    g_beyond_synth.SetListener(false, 1.0f, 0.0f);
    Toast(s, "Beyond Batmobile: left stick flies, R2 up, L2 down; d-pad right boost, L1 missile, d-pad down cloak, "
             "Y out.",
          5000);
    if (m.log < 8) {
      ++m.log;
      REXLOG_INFO("gta4-trainer: in the Beyond Batmobile (car {:08X}), heading {:.0f} deg", m.car,
                  m.heading * 180.0f / 3.14159265f);
    }
  }
  BeyondVoice(true);
  m.sounding = true;
  if (!m.models_requested) {
    Invoke(ctx, base, "REQUEST_MODEL", {ModelHash(kRocketModel)});
    m.models_requested = true;
  }
  uint32_t keep = X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_DPAD_RIGHT;
  // D-pad down (or the menu): the cloak.
  if (pressed(X_INPUT_GAMEPAD_DPAD_DOWN) || s.beyond_toggle_cloak) {
    s.beyond_toggle_cloak = false;
    BeyondCloak(ctx, base, game, m, !m.cloaked);
    Toast(s, m.cloaked ? "Cloaked: nobody sees the Batmobile, or you." : "Cloak off.", 2000);
  }
  if (m.cloaked && now >= m.next_wanted_clear) {
    m.next_wanted_clear = now + 400;
    Invoke(ctx, base, "CLEAR_WANTED_LEVEL", {game.player});
  }
  // L1: a missile from the nose (the Batmobile's lock-on rockets).
  if (pressed(X_INPUT_GAMEPAD_LEFT_SHOULDER)) {
    if (now < m.rocket_ready) {
      Toast(s, "Missile reloading: " + std::to_string((m.rocket_ready - now + 999) / 1000) + " s", 900);
    } else if (LaunchRocket(ctx, base, m.car, s, kBeyondLauncher, BeyondLaunchSound, "Beyond Batmobile")) {
      m.rocket_ready = now + kBeyondReloadMs;
      m.rocket_reloading = true;
    }
  }
  if (m.rocket_reloading && now >= m.rocket_ready) {
    m.rocket_reloading = false;
    Toast(s, "Missile ready.", 900);
  }
  // Y: down to the hover height and stopped, then the canopy (the canopy
  // code gets Y once it is settled; a second Y meanwhile flies on).
  const bool low_and_still = m.altitude < kBeyondClear + 0.6f && std::fabs(m.fwd) < 1.5f && std::fabs(m.climb) < 0.8f;
  if (pressed(X_INPUT_GAMEPAD_Y)) {
    if (m.settling) {
      m.settling = false;
      m.y_taken = true;
      Toast(s, "Flying on.", 1200);
    } else if (!low_and_still) {
      m.settling = true;
      m.settle_since = now;
      m.y_taken = true;
      Toast(s, "Coming down...", 1500);
    }
  } else if (m.settling) {
    m.y_taken = true;  // (held Y never reaches the canopy while it comes down)
  }
  if (m.settling && (low_and_still || now - m.settle_since > 12000)) {
    m.settling = false;
    m.exit_pulse = true;  // the canopy's turn
  }
  // The boost: d-pad right held.
  const bool boosting = (buttons & X_INPUT_GAMEPAD_DPAD_RIGHT) && !s.open && !m.settling;
  m.boost += ((boosting ? 1.0f : 0.0f) - m.boost) * std::min(1.0f, (boosting ? 2.5f : 1.5f) * dt);
  // The controls (none while it settles).
  float want_fwd, turn, up;
  if (m.settling) {
    want_fwd = 0.0f;
    turn = 0.0f;
    up = m.altitude > kBeyondClear + 0.3f ? -0.5f : 0.0f;
  } else {
    const float lx = Axis(pad.lx), ly = Axis(pad.ly);
    const float top = kBeyondCruise + (kBeyondBoostSpeed - kBeyondCruise) * m.boost;
    want_fwd = ly >= 0 ? ly * top : ly * 15.0f;
    if (boosting)
      want_fwd = std::max(want_fwd, top * 0.6f);  // (the boost drives it on by itself)
    turn = lx;
    up = pad.right_trigger / 255.0f - pad.left_trigger / 255.0f;
  }
  BeyondFly(ctx, base, m, want_fwd, turn, up, dt);
  BeyondFlames(ctx, base, m.car, !m.cloaked && (m.boost > 0.3f || std::fabs(m.fwd) > 35.0f));
  g_beyond_synth.SetListener(false, 1.0f, 0.0f);
  g_beyond_synth.SetTarget(m.spool, std::fabs(m.fwd), m.boost, 0.5f);
  if (m.y_taken)
    keep |= X_INPUT_GAMEPAD_Y;
  return keep;
}

// The TCD and the speedometer as the overlay shows them.
void PublishTcd(const State& s) {
  const auto& m = g_tm;
  TcdModel t;
  t.visible = m.in_car && m.circuits;
  t.empty = !m.charged;
  t.hover = m.hover;
  t.dest = s.tm_dest;
  t.present = m.present;
  t.last = m.last;
  t.mph = int(m.speed / 0.44704f + 0.5f);
  const uint64_t now = NowMs();
  if (now < g_flash_until && g_flash_until > g_flash_start)
    t.flash = float(g_flash_until - now) / float(g_flash_until - g_flash_start);
  t.veil = VeilLevel(now);
  const bool active = t.visible || t.flash > 0.0f || t.veil > 0.0f;
  {
    std::lock_guard lock(g_tcd_mutex);
    g_tcd = t;
  }
  g_tcd_active.store(active, std::memory_order_relaxed);
}

// Every frame in the car, and while its voice still sounds after getting
// out; a look a few times a second otherwise.
void CarEngine(PPCContext& ctx, uint8_t* base, const GameContext& game, const DrivePad& pad, CarEngineState& e,
               float boost) {
  const uint64_t now = NowMs();
  if (!e.in_car && !e.running && now < e.next_probe)
    return;
  e.next_probe = now + 300;
  const uint32_t model = ModelHash(e.model), model2 = e.model2.empty() ? 0u : ModelHash(e.model2);
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  auto& synth = *e.synth;
  if (!vehicle.car || (vehicle.model != model && (!model2 || vehicle.model != model2))) {
    if (e.in_car)
      synth.SetTarget(0.0f, 0.0f, 0.0f, e.volume);  // switched off: it winds down
    e.in_car = false;
    CarVoice(e, false);
#if REX_PLATFORM_PS5
    e.running = e.voice && e.voice->Active();
#else
    e.running = false;
#endif
    return;
  }
  const float idle = gta4::car::EngineSynth::kIdle;
  if (!e.in_car) {
    e.in_car = e.running = true;
    e.gear = 0;
    synth.SetTarget(idle, 0.0f, 0.0f, e.volume);  // the starter, then idle
    CarVoice(e, true);
    if (e.log < 4) {
      ++e.log;
      REXLOG_INFO("gta4-trainer: {} engine started (synthesized)", std::string(e.label));
    }
  }
  float speed = 0.0f;
  if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {*vehicle.car, 0}, 1))
    speed = std::fabs(v->f(0));
  const float throttle = pad.right_trigger / 255.0f;
  // A truck's air brakes let their pressure go once it has stopped.
  if (e.air_brakes) {
    if (speed > 3.0f) {
      e.rolling = true;
    } else if (e.rolling && speed < 0.4f) {
      e.rolling = false;
      synth.AirBrake();
    }
  }
  // Five gears: up near the limit, down when the revs fall low.
  if (speed > e.gear_top[size_t(e.gear)] * 0.94f && e.gear < 4)
    ++e.gear;
  else if (e.gear > 0 && speed < e.gear_top[size_t(e.gear - 1)] * 0.55f)
    --e.gear;
  float rpm = idle + (1.0f - idle) * std::clamp(speed / e.gear_top[size_t(e.gear)], 0.0f, 1.0f);
  // Standing (or crawling) the throttle revs it.
  rpm = std::max(rpm, idle + (1.0f - idle) * 0.6f * throttle * (speed < 3.0f ? 1.0f : 0.35f));
  // Flying (the Part II time machine with its wheels folded, or its v1.7.21
  // flying form): the engine only idles under the hover's hum (v1.7.21
  // revved it with the climb on R2, and it drowned everything).
  if ((model2 && vehicle.model == model2) || (g_tm.hover && g_tm.car == *vehicle.car)) {
    synth.SetTarget(idle, 0.0f, 0.0f, e.volume * 0.35f);
    return;
  }
  synth.SetTarget(rpm, throttle, boost, e.volume);
}

// Every frame in the F-16 (a look a few times a second otherwise, and while
// its missiles fly). The jet's own behaviour (engine sound, landing gear,
// exhaust) always; flight and weapons with the menu's toggle. Returns the
// buttons the jet uses, hidden from the game: R3 (the gear), and with the
// weapons on the d-pad (not the radio).
// The jet's head-up display (drawn by the overlay): speed, height, throttle,
// gear, the air brake, the autopilot, a stall; and the targeting marks (a
// dot on each car or helicopter it can lock, brackets closing on the one it
// is locking, red once locked), as GTA V's jets show them.
struct JetHudModel {
  bool visible = false;
  bool marks_only = false;  // the Lazar 3: its targets' marks, no flight panel
  std::string label, autopilot;
  float kmh = 0, height = 0, throttle = 0, g = 1;
  bool burner = false, airbrake = false, on_ground = true, stalled = false, has_gear = true, weapons = true;
  int gear = 0;  // 0 down, 1 moving, 2 up
  std::vector<JetMark> marks;
};
std::mutex g_hud_mutex;
JetHudModel g_hud;
std::atomic<bool> g_hud_active{false};

std::string AutopilotText(const gta4::flight::Pilot& p) {
  using gta4::flight::Mode;
  using gta4::flight::Phase;
  switch (p.mode) {
    case Mode::kOff: return {};
    case Mode::kHold: return "AUTOPILOT  HOLD";
    case Mode::kLand:
    case Mode::kRoute:
      switch (p.phase) {
        case Phase::kToFix: return std::string("AUTOPILOT  TO ") + p.where;
        case Phase::kIntercept: return "AUTOPILOT  JOINING THE APPROACH";
        case Phase::kFinal: return "AUTOPILOT  ON FINAL";
        case Phase::kFlare: return "AUTOPILOT  FLARE";
        case Phase::kRollout:
        case Phase::kStopped: return "AUTOPILOT  BRAKING";
        default: return "AUTOPILOT";
      }
  }
  return {};
}

void PublishJetHud(const JetState& j, const State& s, float ground_speed, float clearance, float throttle,
                   bool burner, float airbrake) {
  JetHudModel m;
  m.visible = s.jet;
  m.label = std::string(g_jet_spec->label);
  m.kmh = (j.flying ? j.fs.speed : ground_speed) * 3.6f;
  m.height = std::max(0.0f, clearance - j.rest_clearance);
  m.throttle = throttle;
  m.burner = burner;
  m.airbrake = airbrake > 0.1f;
  m.on_ground = !j.flying;
  m.stalled = j.flying && j.fs.stalled;
  m.g = j.flying ? j.fs.n : 1.0f;
  m.has_gear = g_jet_spec->gear;
  m.gear = j.gear < 0.01f ? 0 : j.gear > 0.99f ? 2 : 1;
  m.autopilot = AutopilotText(j.pilot);
  m.weapons = s.jet;
  m.marks = g_jet_marks;
  {
    std::lock_guard lock(g_hud_mutex);
    g_hud = std::move(m);
  }
  g_hud_active.store(s.jet, std::memory_order_relaxed);
}

// The Lazar's: its targets' marks alone.
void PublishLazarHud(bool on) {
  JetHudModel m;
  m.visible = on;
  m.marks_only = true;
  if (on)
    m.marks = g_jet_marks;
  {
    std::lock_guard lock(g_hud_mutex);
    g_hud = std::move(m);
  }
  g_hud_active.store(on, std::memory_order_relaxed);
}

// Police helicopters after the jet. Wanted, in the air in it, two police
// Mavericks come for it, from behind and above: a pilot told to chase and
// attack the player (TASK_HELI_MISSION's attack, as TBOGT's yusuf2 sends its
// gunships after Luis, its height kept to the jet's), a NOOSE gunner at the
// side door with a carbine. One shot down, another comes 20 s later; the
// wanted level gone or the player out of the air, they are let go (the game
// clears them away). They are the jet's targets like any other helicopter.
struct PoliceHeli {
  uint32_t heli = 0, pilot = 0, gunner = 0, blip = 0;
  uint64_t next_task = 0;
};
struct PoliceAir {
  std::array<PoliceHeli, 2> helis{};
  bool requested = false, told = false;
  uint64_t next_check = 0, next_spawn = 0;
  int log = 0;
};
PoliceAir g_police_air;
constexpr std::string_view kPoliceHeliModel = "POLMAV", kPolicePilotModel = "M_Y_COP",
                           kPoliceGunnerModel = "M_Y_SWAT";
constexpr uint32_t kPedTypeCop = 6, kWeaponM4 = 15;

void LetGoOfPed(PPCContext& ctx, uint8_t* base, uint32_t& ped) {
  if (ped)
    Invoke(ctx, base, "MARK_CHAR_AS_NO_LONGER_NEEDED", {0}, 0, ped);
  ped = 0;
}
void LetGoOfHeli(PPCContext& ctx, uint8_t* base, PoliceHeli& h) {
  if (h.blip) {
    const auto exists = Invoke(ctx, base, "DOES_BLIP_EXIST", {h.blip});
    if (exists && exists->r0())
      Invoke(ctx, base, "REMOVE_BLIP", {h.blip});
  }
  LetGoOfPed(ctx, base, h.pilot);
  LetGoOfPed(ctx, base, h.gunner);
  if (h.heli)
    Invoke(ctx, base, "MARK_CAR_AS_NO_LONGER_NEEDED", {0}, 0, h.heli);
  h = PoliceHeli{};
}

void PoliceAirTick(PPCContext& ctx, uint8_t* base, const GameContext& game, const JetState& j, State& s,
                   uint64_t now) {
  auto& pa = g_police_air;
  if (now < pa.next_check)
    return;
  pa.next_check = now + 500;
  bool wanted = false;
  if (j.in_jet && j.flying && s.jet)
    if (const auto w = Invoke(ctx, base, "IS_WANTED_LEVEL_GREATER", {game.player, 0}))
      wanted = w->r0() != 0;
  int live = 0;
  for (auto& h : pa.helis) {
    if (!h.heli)
      continue;
    const auto dead = VehicleAddress(base, h.heli) ? Invoke(ctx, base, "IS_CAR_DEAD", {h.heli}) : std::nullopt;
    const bool gone = !VehicleAddress(base, h.heli) || (dead && dead->r0());
    if (gone || !wanted) {
      if (gone)
        pa.next_spawn = std::max(pa.next_spawn, now + 20000);
      LetGoOfHeli(ctx, base, h);
      continue;
    }
    ++live;
  }
  if (!wanted) {
    if (pa.requested) {
      for (const auto name : {kPoliceHeliModel, kPolicePilotModel, kPoliceGunnerModel})
        Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {ModelHash(name)});
      pa.requested = false;
    }
    pa.told = false;
    return;
  }
  const auto at = CarPosition(ctx, base, j.car);
  if (!at)
    return;
  // Keep them after the jet: their chase re-aimed at its height every 3 s.
  for (auto& h : pa.helis)
    if (h.heli && h.pilot && now >= h.next_task) {
      h.next_task = now + 3000;
      const auto hurt = Invoke(ctx, base, "IS_CHAR_INJURED", {h.pilot});
      if (!(hurt && hurt->r0()))
        Invoke(ctx, base, "TASK_HELI_MISSION",
               {h.pilot, h.heli, 0, game.ped, Bits(-1.0f), Bits(-1.0f), Bits(-1.0f), 6, Bits(70.0f), 30,
                uint32_t(-1), uint32_t(std::max(40, int((*at)[2]) + 15)), 40});
    }
  if (live >= int(pa.helis.size()) || now < pa.next_spawn)
    return;
  if (!pa.requested) {
    for (const auto name : {kPoliceHeliModel, kPolicePilotModel, kPoliceGunnerModel})
      Invoke(ctx, base, "REQUEST_MODEL", {ModelHash(name)});
    pa.requested = true;
    return;
  }
  for (const auto name : {kPoliceHeliModel, kPolicePilotModel, kPoliceGunnerModel}) {
    const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {ModelHash(name)});
    if (!loaded || !loaded->r0())
      return;
  }
  auto slot = std::find_if(pa.helis.begin(), pa.helis.end(), [](const PoliceHeli& h) { return h.heli == 0; });
  if (slot == pa.helis.end())
    return;
  // 600 m behind the jet, off to one side, 60 m above it, facing it.
  float heading = 0.0f;
  if (const auto hd = Invoke(ctx, base, "GET_CAR_HEADING", {j.car, 0}, 1))
    heading = hd->f(0) * 3.14159265f / 180.0f;
  const float side = (slot == pa.helis.begin()) ? -1.0f : 1.0f;
  const float bx = std::sin(heading) * 600.0f, by = -std::cos(heading) * 600.0f;  // behind (+Y is the nose)
  const float x = (*at)[0] + bx + std::cos(heading) * 180.0f * side;
  const float y = (*at)[1] + by + std::sin(heading) * 180.0f * side;
  const float z = std::max((*at)[2] + 60.0f, 80.0f);
  const auto made = Invoke(ctx, base, "CREATE_CAR", {ModelHash(kPoliceHeliModel), Bits(x), Bits(y), Bits(z), 0, 1}, 4);
  if (!made || !made->words[0])
    return;
  PoliceHeli h;
  h.heli = made->words[0];
  const float face = std::atan2(-((*at)[0] - x), (*at)[1] - y) * 180.0f / 3.14159265f;
  Invoke(ctx, base, "SET_CAR_HEADING", {h.heli, Bits(face)});
  Invoke(ctx, base, "SET_HELI_BLADES_FULL_SPEED", {h.heli});
  if (const auto p = Invoke(ctx, base, "CREATE_CHAR_INSIDE_CAR",
                            {h.heli, kPedTypeCop, ModelHash(kPolicePilotModel), 0}, 3); p && p->words[0])
    h.pilot = p->words[0];
  if (const auto g = Invoke(ctx, base, "CREATE_CHAR_AS_PASSENGER",
                            {h.heli, kPedTypeCop, ModelHash(kPoliceGunnerModel), 1, 0}, 4); g && g->words[0])
    h.gunner = g->words[0];
  if (!h.pilot) {
    LetGoOfHeli(ctx, base, h);
    return;
  }
  Invoke(ctx, base, "SET_CHAR_KEEP_TASK", {h.pilot, 1});
  Invoke(ctx, base, "TASK_HELI_MISSION",
         {h.pilot, h.heli, 0, game.ped, Bits(-1.0f), Bits(-1.0f), Bits(-1.0f), 6, Bits(70.0f), 30, uint32_t(-1),
          uint32_t(std::max(40, int((*at)[2]) + 15)), 40});
  h.next_task = now + 3000;
  if (h.gunner) {
    Invoke(ctx, base, "GIVE_WEAPON_TO_CHAR", {h.gunner, kWeaponM4, 9999, 0});
    Invoke(ctx, base, "SET_CHAR_WILL_DO_DRIVEBYS", {h.gunner, 1});
    Invoke(ctx, base, "SET_CHAR_ACCURACY", {h.gunner, 55});
    Invoke(ctx, base, "SET_CHAR_KEEP_TASK", {h.gunner, 1});
    Invoke(ctx, base, "TASK_COMBAT", {h.gunner, game.ped});
  }
  if (const auto b = Invoke(ctx, base, "ADD_BLIP_FOR_CAR", {h.heli, 0}, 1); b && b->r0()) {
    h.blip = b->r0();
    Invoke(ctx, base, "CHANGE_BLIP_COLOUR", {h.blip, 1});
  }
  *slot = h;
  pa.next_spawn = now + 6000;  // (the second one a little after)
  if (!pa.told) {
    pa.told = true;
    Toast(s, "Police helicopters inbound!", 2000);
  }
  if (pa.log < 6) {
    ++pa.log;
    REXLOG_INFO("gta4-trainer: police helicopter {:08X} after the jet (pilot {:08X}, gunner {:08X}) from {:.0f}, "
                "{:.0f}, {:.0f}", h.heli, h.pilot, h.gunner, x, y, z);
  }
}

// Puts a vehicle down level, its origin `stand` m over the ground under it
// (its height standing on its wheels). For the jets: SET_CAR_ON_GROUND_PROPERLY
// measures from the wheels, and with them under the road (the Batwing lying
// on its collision, a metre above its wheels) it left it sunk (v1.7.26).
bool StandOnWheels(PPCContext& ctx, uint8_t* base, uint32_t car, float stand) {
  namespace fl = gta4::flight;
  const auto at = CarPosition(ctx, base, car);
  if (!at)
    return false;
  float yaw = 0.0f;
  if (const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {car, 0, 0, 0, 0}, 1, 0, 4)) {
    const fl::Q qq = fl::Norm({q->f(0), q->f(1), q->f(2), q->f(3)});
    const fl::V3 f = fl::Rot(qq, {0, 1, 0}), r = fl::Rot(qq, {1, 0, 0});
    yaw = std::hypot(f.x, f.y) > 0.3f ? std::atan2(-f.x, f.y) : std::atan2(r.y, r.x);
  }
  const auto gz = GroundZ(ctx, base, (*at)[0], (*at)[1], (*at)[2] + 1.0f);
  if (!gz) {
    Invoke(ctx, base, "SET_CAR_ON_GROUND_PROPERLY", {car});
    return false;
  }
  PlaceCar(ctx, base, car, (*at)[0], (*at)[1], *gz + stand + 0.08f);
  const Quat level = QAxis(0, 0, 1, yaw);
  Invoke(ctx, base, "SET_VEHICLE_QUATERNION", {car, Bits(level.x), Bits(level.y), Bits(level.z), Bits(level.w)});
  return true;
}

// ---------------------------------------------------------------------------
// The autopilot's landing on open ground (v1.7.30; d-pad down held in the
// air, the menu's Autopilot Landing: Open Ground Nearby). Strips around the
// jet are tried with the ground probe, a few dozen probes a frame: each 500 m
// along its heading (from 75 m before its touchdown point), flat (no step
// over 0.6 m between probes 25 m apart, within 2.5 m of the touchdown point's
// height), 18 m wide (its sides probed every 80 m), over the sea, its roofs
// counting (probes from 450 m up), and its approach (3 km back, under a 3.5
// degree glide) clear by 12 m. The best (nearest, least turning) is flown to
// as Francis International's runway is; none, and the jet holds its course
// while the ground ahead is tried again every few seconds, for half a minute,
// then it flies to the airport.
// ---------------------------------------------------------------------------
std::optional<float> TrySite(PPCContext& ctx, uint8_t* base, const JetState::Search& ls, const JetState::Site& site,
                             int& probes) {
  const float dx = -std::sin(site.yaw), dy = std::cos(site.yaw), px = std::cos(site.yaw), py = std::sin(site.yaw);
  constexpr float kTop = 450.0f;
  auto g = [&](float x, float y) {
    ++probes;
    return GroundZ(ctx, base, x, y, kTop);
  };
  const auto z0 = g(site.x, site.y);
  if (!z0 || *z0 < ls.sea + 1.0f || *z0 > kTop - 50.0f)
    return std::nullopt;
  float prev = *z0;
  for (float d = 25.0f; d <= 425.0f; d += 25.0f) {  // (the rollout)
    const auto z = g(site.x + dx * d, site.y + dy * d);
    if (!z || std::fabs(*z - prev) > 0.6f || std::fabs(*z - *z0) > 2.5f || *z < ls.sea + 1.0f)
      return std::nullopt;
    prev = *z;
  }
  prev = *z0;
  for (float d = -25.0f; d >= -75.0f; d -= 25.0f) {  // (a little before the touchdown point)
    const auto z = g(site.x + dx * d, site.y + dy * d);
    if (!z || std::fabs(*z - prev) > 0.6f || std::fabs(*z - *z0) > 2.5f)
      return std::nullopt;
    prev = *z;
  }
  for (float d = 0.0f; d <= 400.0f; d += 80.0f)
    for (const float side : {-9.0f, 9.0f}) {
      const auto z = g(site.x + dx * d + px * side, site.y + dy * d + py * side);
      if (!z || std::fabs(*z - *z0) > 2.5f)
        return std::nullopt;
    }
  for (const float d : {150.0f, 300.0f, 500.0f, 800.0f, 1200.0f, 1700.0f, 2300.0f, 3000.0f}) {
    const auto z = g(site.x - dx * d, site.y - dy * d);
    if (z && *z > *z0 + d * gta4::flight::kGlide - 12.0f)
      return std::nullopt;
  }
  return *z0;
}

// The strips to try around (x, y), the jet heading `heading`: touchdown
// points out to 2.5 km in eleven directions, each strip along twelve
// headings; ordered by how good a landing it would make (near, little turning).
void PlanSites(JetState::Search& ls, float x, float y, float heading) {
  ls.sites.clear();
  ls.next = 0;
  for (const float dist : {700.0f, 1100.0f, 1600.0f, 2200.0f})
    for (const float bearing_deg : {0.0f, 30.0f, -30.0f, 60.0f, -60.0f, 95.0f, -95.0f, 135.0f, -135.0f, 180.0f}) {
      const float b = heading + bearing_deg * 3.14159265f / 180.0f;
      const float ax = x - std::sin(b) * dist, ay = y + std::cos(b) * dist;
      for (int k = 0; k < 12; ++k) {
        const float yaw = gta4::flight::Wrap(heading + float(k) * 3.14159265f / 6.0f);
        JetState::Site site;
        site.x = ax;
        site.y = ay;
        site.yaw = yaw;
        site.score = dist / 1000.0f + std::fabs(bearing_deg) / 90.0f +
                     0.6f * std::fabs(gta4::flight::Wrap(yaw - heading)) / 3.14159265f;
        ls.sites.push_back(site);
      }
    }
  std::sort(ls.sites.begin(), ls.sites.end(),
            [](const JetState::Site& a, const JetState::Site& b) { return a.score < b.score; });
}

// On its wheels, not in the ground (v1.7.30): the Batwing's collision is the
// stock SUV's it was built on, a metre above its wheels. Braking after a
// landing dipped the nose; the trainer's push along the nose then drove it
// down, the nose gear's probe started under the runway, and the nose sank
// in until the body's collision held it: stuck, wheels in the ground (the
// v1.7.27 check looks only under its middle). Each frame on the ground the
// three gear contacts are probed: one more than 0.22 m into the surface puts
// it back on its wheels along the ground's slope; the nose pointing more than
// 2 degrees below the slope while it rolls is lifted to it (its push then
// along the ground, not into it). True when it was set.
bool GearGuard(PPCContext& ctx, uint8_t* base, uint32_t car, const JetSpec& spec, JetState& j, float speed) {
  namespace fl = gta4::flight;
  const auto c = CarPosition(ctx, base, car);
  const auto qr = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {car, 0, 0, 0, 0}, 1, 0, 4);
  if (!c || !qr)
    return false;
  const fl::Q q = fl::Norm({qr->f(0), qr->f(1), qr->f(2), qr->f(3)});
  float ground[3], depth[3];
  for (int k = 0; k < 3; ++k) {
    const fl::V3 p = fl::Rot(q, {spec.contacts[k][0], spec.contacts[k][1], spec.contacts[k][2]});
    const float x = (*c)[0] + p.x, y = (*c)[1] + p.y, z = (*c)[2] + p.z;
    const auto gz = GroundZ(ctx, base, x, y, z + 2.5f);
    if (!gz)
      return false;
    ground[k] = *gz;
    depth[k] = *gz - z;
  }
  const float length = spec.contacts[0][1] - spec.contacts[1][1];
  const float width = spec.contacts[2][0] - spec.contacts[1][0];
  const float mains = 0.5f * (ground[1] + ground[2]);
  const float slope = std::atan2(ground[0] - mains, length);
  const float roll = std::atan2(ground[1] - ground[2], width);  // (right side lower: right wing down)
  const fl::Attitude a = fl::Look(q);
  const float deepest = std::max({depth[0], depth[1], depth[2]});
  const bool dug_in = deepest > 0.22f;
  const bool nose_low = speed > 3.0f && a.gamma < slope - 0.035f;
  if (!dug_in && !nose_low)
    return false;
  const fl::Q level = fl::Norm(fl::Mul(fl::Facing(a.yaw, slope), fl::Axis({0, 1, 0}, dug_in ? roll : a.bank)));
  if (dug_in) {
    // its origin over the ground's plane at its own point, at its standing height
    const float t = (0.0f - spec.contacts[1][1]) / length;
    PlaceCar(ctx, base, car, (*c)[0], (*c)[1], mains + (ground[0] - mains) * t + spec.stand + 0.05f);
  }
  Invoke(ctx, base, "SET_VEHICLE_QUATERNION", {car, Bits(level.x), Bits(level.y), Bits(level.z), Bits(level.w)});
  if (j.stuck_logs < 12) {
    ++j.stuck_logs;
    REXLOG_INFO("gta4-trainer: {} {}: wheels {:.2f} {:.2f} {:.2f} m into the ground, nose {:.1f} degrees (the "
                "ground's {:.1f}), {:.1f} m/s",
                std::string(spec.label), dug_in ? "back on its wheels" : "nose lifted to the ground's slope",
                depth[0], depth[1], depth[2], a.gamma * 57.2958f, slope * 57.2958f, speed);
  }
  return true;
}

uint32_t JetControls(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, const DrivePad& pad) {
  auto& j = g_jet;
  const uint64_t now = NowMs();
  const float dt = j.last_ms ? std::clamp(float(now - j.last_ms) / 1000.0f, 0.0f, 0.05f) : 0.0f;
  j.last_ms = now;
  UpdateMissiles(ctx, base, j, dt);
  PoliceAirTick(ctx, base, game, j, s, now);
  // Who is driving what: checked every frame in the jet, a few times a
  // second otherwise.
  if (!j.in_jet && now < j.next_probe)
    return 0;
  j.next_probe = now + 400;
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  const auto car = vehicle.car;
  const JetSpec* spec = car ? JetSpecOf(vehicle.model) : nullptr;
  const bool in_jet = spec != nullptr;
  if (!in_jet) {
    if (j.in_jet) {
      StopFlame(ctx, base, j);
      if (j.rocket_requested) {
        Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {ModelHash(kRocketModel)});
        j.rocket_requested = false;
      }
    }
    if (j.in_jet) {
      JetEngineSound(0.0f, 0.0f, kJetVolume);  // just left the jet: the engine winds down
      DropLock(ctx, base, j.lock);
      j.pilot.mode = gta4::flight::Mode::kOff;
      g_jet_marks.clear();
      g_hud_active.store(false, std::memory_order_relaxed);
    }
    j.in_jet = j.flying = false;
    j.announced = false;
    return 0;
  }
  if (!j.in_jet || j.car != *car) {
    j.flying = false;
    if (spec != g_jet_spec)
      StopFlame(ctx, base, j);  // (its flames were the other aircraft's)
    g_jet_spec = spec;
    if (!spec->gear)
      j.gear = 0.0f, j.gear_up = false;
    else if (j.car != *car)
      PrepareJet(ctx, base, *car, true);
    else  // back in the same jet: make sure the gear's extras still show
      for (const auto& leg : spec->legs)
        ShowExtra(ctx, base, *car, leg.extra, true);
    j.car = *car;
    j.prev_buttons = pad.buttons;  // no edge from a button held while getting in
    j.r3_held = false;
    j.entered_at = now;
    j.spool_logged = j.ready_told = false;
    j.pull_logs = j.flight_logs = j.roll_logs = 0;
    j.next_pull_log = j.next_flight_log = j.next_roll_log = 0;
    j.rest_known = false;
    j.roll_v = 0.0f;
    j.stuck_since = 0;
    REXLOG_INFO("gta4-trainer: {}: player in the aircraft; flight and weapons {}", std::string(spec->label),
                s.jet ? "on" : "off (the menu)");
  }
  j.in_jet = true;
  if (s.jet && !j.rocket_requested) {
    Invoke(ctx, base, "REQUEST_MODEL", {ModelHash(kRocketModel)});
    j.rocket_requested = true;
  }
  if (!j.announced) {
    j.announced = true;
    const std::string label(g_jet_spec->label);
    const int vr_kmh = int(g_jet_spec->flight->rotate * 3.6f + 0.5f);
    Toast(s, s.jet ? label + ": RT throttle (full: afterburner), LT brakes. Past " + std::to_string(vr_kmh) +
                         " km/h pull back to take off. " +
                         (s.jet_expert ? std::string("Expert: the stick rolls and pulls")
                                       : std::string("Left stick: up/down pitch, left/right bank and turn")) +
                         "; LB/RB rudder" + std::string(g_jet_spec->gear ? ", R3 gear" : "") +
                         ". D-pad: right cannon, left missile, up next target, down autopilot (hold: land)."
                   : label + ": flight and weapons are off: Vehicle Options > Jet Flight and Weapons.",
          9000);
  }
  namespace fl = gta4::flight;
  const fl::Params& P = *g_jet_spec->flight;
  const std::string label(g_jet_spec->label);
  float ground_speed = 0.0f;
  if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {*car, 0}, 1))
    ground_speed = v->f(0);
  const float lx = Axis(pad.lx), ly = Axis(pad.ly);
  const float throttle = pad.right_trigger / 255.0f, airbrake = pad.left_trigger / 255.0f;
  const float rudder = ((pad.buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER) ? 1.0f : 0.0f) -
                       ((pad.buttons & X_INPUT_GAMEPAD_LEFT_SHOULDER) ? 1.0f : 0.0f);
  const bool gear_down = !g_jet_spec->gear || j.gear < 0.5f;
  // Where it is and how high: its origin over the ground under it.
  const auto at = CarPosition(ctx, base, *car);
  float clearance = 100.0f;
  if (s.jet && at)
    if (const auto below = GroundZ(ctx, base, (*at)[0], (*at)[1], (*at)[2] + 0.5f))
      clearance = (*at)[2] - *below;
  // Its speed along its nose on the ground, from how far it moved (reversing: negative).
  float forward_speed = ground_speed, up_z = 1.0f;
  if (at && j.have_last_pos && dt > 0.0f) {
    if (const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {*car, 0, 0, 0, 0}, 1, 0, 4)) {
      up_z = fl::Rot(fl::Norm({q->f(0), q->f(1), q->f(2), q->f(3)}), {0, 0, 1}).z;
      const fl::V3 f = fl::Rot(fl::Norm({q->f(0), q->f(1), q->f(2), q->f(3)}), {0, 1, 0});
      const float moved = ((*at)[0] - j.last_pos[0]) * f.x + ((*at)[1] - j.last_pos[1]) * f.y +
                          ((*at)[2] - j.last_pos[2]) * f.z;
      if (moved < 0.0f)
        forward_speed = -ground_speed;
    }
  }
  if (at) {
    for (int a = 0; a < 3; ++a)
      j.last_pos[a] = (*at)[a];
    j.have_last_pos = true;
  }

  // The autopilot's button: d-pad down; tapped, hold the course; held a
  // second, land straight ahead. (The menu sends it to the airport.)
  const bool dpad_down = pad.buttons & X_INPUT_GAMEPAD_DPAD_DOWN;
  bool ap_tap = false, ap_land = false;
  if (dpad_down && !(j.prev_buttons & X_INPUT_GAMEPAD_DPAD_DOWN))
    j.ap_pressed_at = now;
  if (dpad_down && j.ap_pressed_at && now - j.ap_pressed_at > 900) {
    ap_land = true;
    j.ap_pressed_at = 0;
  }
  if (!dpad_down && (j.prev_buttons & X_INPUT_GAMEPAD_DPAD_DOWN) && j.ap_pressed_at) {
    ap_tap = true;
    j.ap_pressed_at = 0;
  }
  const bool ap_route = std::exchange(s.jet_route, false);
  float eff_throttle = throttle;
  bool eff_burner = throttle > 0.9f && (j.flying || ground_speed > 20.0f);

  if (!s.jet) {
    j.flying = false;
    j.pilot.mode = fl::Mode::kOff;
  } else {
    if ((ap_tap || ap_land || ap_route) && !j.flying) {
      Toast(s, label + (ap_route ? ": take off first; the autopilot flies you there."
                                  : ": the autopilot works in the air."), 2000);
      ap_tap = ap_land = false;
    }
    const float rpm = g_jet_synth.Rpm();
    if (!j.flying && g_jet_spec->contacts && at)
      GearGuard(ctx, base, *car, *g_jet_spec, j, std::fabs(forward_speed));
    if (!j.flying) {
      // ---- Down on its body with its wheels under the road, or on its side
      // or back: stood back on its wheels. (Its collision is the stock car's
      // it was built on: a metre above the Batwing's wheels. A flip or a
      // hard landing could leave it lying on that, its wheels' probes
      // starting under the road, and it never came up again: v1.7.25.)
      const bool sunk = clearance < g_jet_spec->stand - 0.45f;
      const bool tipped = up_z < 0.35f;
      if ((sunk && ground_speed < 60.0f) || (tipped && ground_speed < 3.0f)) {
        if (!j.stuck_since)
          j.stuck_since = now;
      } else {
        j.stuck_since = 0;
      }
      if (j.stuck_since && now - j.stuck_since > (tipped ? 1500u : 500u)) {
        j.stuck_since = 0;
        const bool placed = StandOnWheels(ctx, base, *car, g_jet_spec->stand);
        j.rest_known = false;
        j.roll_v = 0.0f;
        if (!tipped && forward_speed > 2.0f) {  // (rolling: it carries on rolling)
          Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {*car, Bits(forward_speed)});
          j.roll_v = forward_speed;
        }
        Toast(s, label + (tipped ? ": set back on its wheels." : ": back up on its wheels."), 1500);
        if (j.stuck_logs < 8) {
          ++j.stuck_logs;
          REXLOG_INFO("gta4-trainer: {} {}: {:.2f} m over the ground (on its wheels {:.2f}), up {:.2f}, "
                      "{:.1f} m/s: set on its wheels ({})",
                      label, tipped ? "on its side or back" : "down on its body", clearance, g_jet_spec->stand,
                      up_z, ground_speed, placed ? "put down level over the ground" : "no ground found");
        }
      }
      // ---- On its wheels: its own thrust and brakes move it (the game's
      // handling only at walking pace and in reverse), and it leaves the
      // ground pulled back at its rotate speed.
      const float vr = fl::RotateSpeed(P, gear_down);
      const bool pulled = ly < -0.35f;
      // Its height standing on its wheels (the touchdown's and these
      // checks'): taken standing still, and never above what rolling shows.
      const float lowest = std::max(0.3f, g_jet_spec->stand - 0.45f);  // (lower: down on its body)
      if (!j.rest_known || (ground_speed < 0.5f && clearance < 4.0f)) {
        if (clearance < 4.0f && clearance >= lowest) {
          j.rest_clearance = std::min(clearance, 3.0f);
          j.rest_known = true;
        }
      } else if (clearance < j.rest_clearance && clearance >= lowest) {
        j.rest_clearance = clearance;
      }
      const bool grounded = clearance < j.rest_clearance + 0.35f;
      // The roll's own speed. Read back from the game each frame, the game's
      // drag and wheels took a share of every push, and v1.7.22's jets never
      // reached their rotate speed; the game's only wins when something
      // stops the jet (a wall, a car) or nothing is pushing it.
      if (std::fabs(forward_speed - j.roll_v) > 4.0f && forward_speed < j.roll_v)
        j.roll_v = std::max(forward_speed, 0.0f);
      const float roll_v = j.roll_v;
      if (!j.vr_told && roll_v >= vr && throttle > 0.3f) {
        j.vr_told = true;
        Toast(s, label + ": rotate speed. Pull back to take off.", 1500);
      }
      if (roll_v < vr * 0.8f)
        j.vr_told = false;
      if (!j.spool_logged && throttle > 0.85f && rpm > kFullPower) {
        j.spool_logged = true;
        REXLOG_INFO("gta4-trainer: {} engines at full power {} ms after getting in", label, now - j.entered_at);
      }
      if (pulled && roll_v < vr && j.pull_logs < 6 && now >= j.next_pull_log && throttle > 0.2f) {
        ++j.pull_logs;
        j.next_pull_log = now + 2500;
        Toast(s, fmt::format("{}: {:.0f} km/h; it lifts off at {:.0f}.", label, roll_v * 3.6f, vr * 3.6f), 1500);
      }
      if (throttle > 0.3f && j.roll_logs < 12 && now >= j.next_roll_log) {
        ++j.roll_logs;
        j.next_roll_log = now + 1500;
        REXLOG_INFO("gta4-trainer: {} rolling: {:.1f} m/s (the game's {:.1f}), {:.2f} m over the ground (on its "
                    "wheels {:.2f}), throttle {:.2f}",
                    label, roll_v, forward_speed, clearance, j.rest_clearance, throttle);
      }
      // Off the ground on its own and fast (a bump, a ramp, a nose-high
      // stance): it flies, rather than being pushed along its nose in the
      // air (the Batwing floated off that way in v1.7.23).
      const bool lifted = clearance > j.rest_clearance + 0.8f && std::max(ground_speed, roll_v) > 0.75f * vr;
      if ((pulled && roll_v >= vr) || lifted) {
        const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {*car, 0, 0, 0, 0}, 1, 0, 4);
        if (q && NativeKnown("SET_VEHICLE_QUATERNION")) {
          j.fs = fl::State{};
          j.fs.q = fl::Norm({q->f(0), q->f(1), q->f(2), q->f(3)});
          j.fs.speed = std::max(roll_v, ground_speed);
          j.flying = true;
          j.pilot.mode = fl::Mode::kOff;
          j.climbed = false;
          j.airborne_at = now;
          j.blocked_since = 0;
          j.flight_logs = 0;
          j.next_flight_log = now + 1000;
          REXLOG_INFO("gta4-trainer: {} airborne at {:.1f} m/s ({}; rotate speed {:.1f}, gear {}, {:.2f} m over the "
                      "ground, {:.2f} on its wheels)",
                      label, j.fs.speed, lifted ? "off the ground on its own" : "pulled back", vr,
                      gear_down ? "down" : "up", clearance, j.rest_clearance);
        }
      } else {
        const float brake = std::max(airbrake, j.pilot.brakes ? 1.0f : 0.0f);
        const bool drive = grounded && (forward_speed > 2.0f || (throttle > 0.08f && forward_speed > -0.5f));
        if (drive) {
          j.roll_v = fl::Roll(P, roll_v, throttle, eff_burner, brake, gear_down, dt);
          Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {*car, Bits(j.roll_v)});
        } else {
          j.roll_v = std::max(forward_speed, 0.0f);  // (the game's own: coasting, reversing, in the air)
        }
        if (j.pilot.mode != fl::Mode::kOff && ground_speed < 1.0f) {
          j.pilot.mode = fl::Mode::kOff;
          j.pilot.brakes = false;
          Toast(s, label + ": landed. Autopilot off.", 2500);
        }
      }
    }
    if (j.flying && at) {
      // ---- In the air.
      fl::Situation sit;
      sit.pos = {(*at)[0], (*at)[1], (*at)[2]};
      sit.radar = clearance - j.rest_clearance;
      const fl::Attitude att = fl::Look(j.fs.q);
      if (ap_tap) {
        if (j.pilot.mode == fl::Mode::kOff) {
          fl::Engage(j.pilot, fl::Mode::kHold, att, sit);
          Toast(s, fmt::format("Autopilot: holding course, {:.0f} m up, {:.0f} km/h. D-pad down again or the "
                               "stick to take over.",
                               j.pilot.hold_z - sit.pos.z + std::max(sit.radar, 0.0f), fl::kCruiseSpeed * 3.6f),
                3500);
        } else {
          j.pilot.mode = fl::Mode::kOff;
          Toast(s, "Autopilot off: you have control.", 1500);
        }
      }
      // Francis International's runway that the planes in the episodes' end
      // credits take off from: its line through (2392, -62) and (2607, 219),
      // heading north-east; down at its south-west end.
      auto fly_to_airport = [&](const char* toast) {
        fl::Runway r;
        r.aim = {2392.4f, -62.0f, 5.8f};
        r.yaw = std::atan2(-(2607.4f - 2392.4f), 218.6f - -62.0f);
        j.runway_checked = false;
        fl::Engage(j.pilot, fl::Mode::kRoute, fl::Look(j.fs.q), sit, &r);
        j.pilot.where = "FRANCIS INTL";
        Toast(s, toast, 4000);
        REXLOG_INFO("gta4-trainer: {} autopilot to the airport: aim ({:.0f}, {:.0f}, {:.1f}), heading {:.0f}", label,
                    r.aim.x, r.aim.y, r.aim.z, r.yaw * 57.2958f);
      };
      const int land_mode = s.jet_land_mode;
      if (ap_land && land_mode == 0 && !ap_route) {
        // Open ground nearby: looked for over the next frames, the course
        // held meanwhile (the stick still takes over).
        auto& ls = j.search;
        ls = JetState::Search{};
        ls.active = true;
        ls.started = now;
        ls.next_scan = now + 3000;
        ls.sea = gta4::addons::SeaLevel();
        PlanSites(ls, sit.pos.x, sit.pos.y, att.yaw);
        if (j.pilot.mode == fl::Mode::kOff)
          fl::Engage(j.pilot, fl::Mode::kHold, att, sit);
        Toast(s, "Autopilot: looking for open ground to land on...", 3000);
        REXLOG_INFO("gta4-trainer: {} autopilot: looking for open ground from ({:.0f}, {:.0f}), {:.0f} m up, the sea "
                    "at {:.1f}",
                    label, sit.pos.x, sit.pos.y, sit.radar, ls.sea);
      } else if (ap_route || (ap_land && land_mode == 2)) {
        j.search.active = false;
        fly_to_airport("Autopilot: flying to Francis International, then landing on its runway. The stick takes "
                       "over at any time.");
      } else if (ap_land) {
        j.search.active = false;
        fl::Runway r;
        const float hx = att.f.x, hy = att.f.y, hl = std::sqrt(hx * hx + hy * hy);
        const float out = std::max(700.0f, std::max(sit.radar, 0.0f) / fl::kGlide);
        r.aim = {sit.pos.x + (hl > 0.1f ? hx / hl : 0.0f) * out, sit.pos.y + (hl > 0.1f ? hy / hl : 1.0f) * out,
                 sit.pos.z - clearance};
        if (const auto gz = GroundZ(ctx, base, r.aim.x, r.aim.y, sit.pos.z + 50.0f))
          r.aim.z = *gz;
        r.yaw = att.yaw;
        j.runway_checked = true;
        fl::Engage(j.pilot, fl::Mode::kLand, att, sit, &r);
        Toast(s, "Autopilot: landing straight ahead. The stick takes over at any time.", 4000);
        REXLOG_INFO("gta4-trainer: {} autopilot landing ahead: aim ({:.0f}, {:.0f}, {:.1f}), heading {:.0f}", label,
                    r.aim.x, r.aim.y, r.aim.z, r.yaw * 57.2958f);
      }
      // The search for open ground, a few dozen probes a frame.
      if (j.search.active) {
        auto& ls = j.search;
        int probes = 0;
        while (!ls.found && ls.next < ls.sites.size() && probes < 90) {
          const auto& site = ls.sites[ls.next++];
          ++ls.tried;
          if (const auto z = TrySite(ctx, base, ls, site, probes)) {
            ls.found = true;
            ls.best = site;
            ls.best_z = *z;
          }
        }
        ls.probes += probes;
        if (ls.found) {
          ls.active = false;
          fl::Runway r;
          r.aim = {ls.best.x, ls.best.y, ls.best_z};
          r.yaw = ls.best.yaw;
          j.runway_checked = false;
          fl::Engage(j.pilot, fl::Mode::kRoute, fl::Look(j.fs.q), sit, &r);
          j.pilot.where = "LANDING SITE";
          const float away = std::hypot(ls.best.x - sit.pos.x, ls.best.y - sit.pos.y);
          Toast(s, fmt::format("Autopilot: open ground {:.1f} km away. Landing there; the stick takes over at any "
                               "time.",
                               away / 1000.0f),
                4000);
          REXLOG_INFO("gta4-trainer: {} autopilot: landing site ({:.0f}, {:.0f}, {:.1f}) heading {:.0f}, {:.0f} m away: "
                      "{} strips tried, {} probes, {} scans, {} ms",
                      label, ls.best.x, ls.best.y, ls.best_z, ls.best.yaw * 57.2958f, away, ls.tried, ls.probes,
                      ls.scans + 1, now - ls.started);
        } else if (ls.next >= ls.sites.size()) {
          if (now - ls.started > 30000) {
            ls.active = false;
            REXLOG_INFO("gta4-trainer: {} autopilot: no open ground found in {} strips ({} probes); to the airport",
                        label, ls.tried, ls.probes);
            fly_to_airport("Autopilot: no open ground big enough nearby. Flying to Francis International instead.");
          } else if (now >= ls.next_scan) {
            ++ls.scans;
            ls.next_scan = now + 3000;
            PlanSites(ls, sit.pos.x, sit.pos.y, fl::Look(j.fs.q).yaw);
          }
        }
        if (j.pilot.mode == fl::Mode::kOff && ls.active) {
          // (the stick took over: the search stops too)
          ls.active = false;
        }
      }
      // The stick takes over from the autopilot.
      if (j.pilot.mode != fl::Mode::kOff && (std::fabs(lx) > 0.6f || std::fabs(ly) > 0.6f)) {
        j.pilot.mode = fl::Mode::kOff;
        Toast(s, "Autopilot off: you have control.", 1500);
      }
      // The route's runway, checked once it is near: flat ground where it should be.
      if (j.pilot.mode == fl::Mode::kRoute && !j.runway_checked && j.pilot.phase == fl::Phase::kFinal &&
          fl::AlongTrack(j.pilot.runway, sit.pos) < 1100.0f) {
        const auto gz = GroundZ(ctx, base, j.pilot.runway.aim.x, j.pilot.runway.aim.y, j.pilot.runway.aim.z + 40.0f);
        if (gz) {
          j.runway_checked = true;
          REXLOG_INFO("gta4-trainer: {} autopilot: the runway's ground at {:.1f} (expected {:.1f})", label, *gz,
                      j.pilot.runway.aim.z);
          if (std::fabs(*gz - j.pilot.runway.aim.z) > 3.0f) {
            j.pilot.phase = fl::Phase::kAbort;
            j.pilot.why = "the runway isn't where it should be";
          } else {
            j.pilot.runway.aim.z = *gz;
          }
        }
      }
      fl::Input in;
      if (j.pilot.mode != fl::Mode::kOff) {
        const fl::Mode was = j.pilot.mode;
        in = fl::Steer(P, j.pilot, j.fs, sit, dt);
        if (was != fl::Mode::kHold && j.pilot.mode == fl::Mode::kHold && *j.pilot.why)
          Toast(s, std::string("Autopilot: can't land (") + j.pilot.why + "). Holding course; the stick takes over.",
                4000);
        if (g_jet_spec->gear && j.gear_up == in.gear_down) {
          j.gear_up = !in.gear_down;
          GearSound(!j.gear_up);
        }
      } else {
        in.pitch = -ly;
        in.roll = lx;
        in.yaw = rudder;
        in.assist = !s.jet_expert;
        in.throttle = throttle;
        in.airbrake = airbrake;
        in.burner = eff_burner;
      }
      in.gear_down = gear_down;
      eff_throttle = in.throttle;
      eff_burner = in.burner;
      fl::Fly(P, j.fs, in, dt);
      const fl::Attitude a = fl::Look(j.fs.q);
      if (j.fs.stalled && now >= j.next_stall_warn) {
        j.next_stall_warn = now + 2500;
        Toast(s, "STALL: nose down and power up.", 1500);
      }
      // Touchdown: back near the ground and not diving into it, the wheels
      // (or the belly, gear up) take over, the jet levelled, still rolling.
      const bool launching = now - j.airborne_at < kLaunchGrace;
      if (clearance > j.rest_clearance + 4.0f)
        j.climbed = true;
      const bool near_ground = clearance < j.rest_clearance + 0.5f;
      if (!launching && j.fs.speed > 20.0f && ground_speed < 0.25f * j.fs.speed) {
        if (!j.blocked_since)
          j.blocked_since = now;
      } else {
        j.blocked_since = 0;
      }
      if (j.flight_logs < 6 && now >= j.next_flight_log) {
        ++j.flight_logs;
        j.next_flight_log = now + 2000;
        REXLOG_INFO("gta4-trainer: {} flying {} ms: {:.1f} m/s (moving at {:.1f}), {:.1f} m up, climbing {:.0f} "
                    "degrees, {:.1f} g, throttle {:.2f}, autopilot {}",
                    label, now - j.airborne_at, j.fs.speed, ground_speed, clearance - j.rest_clearance,
                    a.gamma * 57.2958f, j.fs.n, in.throttle, int(j.pilot.mode));
      }
      if ((near_ground && !launching && (j.climbed || now - j.airborne_at > 5000)) ||
          (j.blocked_since && now - j.blocked_since > 800)) {
        const float sink = -j.fs.speed * a.f.z;
        const bool landed = near_ground && a.gamma > -0.26f;
        j.flying = false;
        if (landed) {
          const Quat level = QAxis(0, 0, 1, a.yaw);
          Invoke(ctx, base, "SET_VEHICLE_QUATERNION", {*car, Bits(level.x), Bits(level.y), Bits(level.z), Bits(level.w)});
          Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {*car, Bits(j.fs.speed)});
          j.roll_v = j.fs.speed;
          if (j.pilot.mode == fl::Mode::kLand || j.pilot.mode == fl::Mode::kRoute) {
            j.pilot.phase = fl::Phase::kRollout;
            j.pilot.brakes = true;
          } else {
            j.pilot.mode = fl::Mode::kOff;
          }
          Toast(s, label + (sink > 5.0f       ? ": hard landing!"
                            : !gear_down      ? ": belly landing!"
                            : j.pilot.brakes  ? ": touchdown. Braking."
                                              : ": touchdown. LT brakes."),
                1800);
        } else {
          j.pilot.mode = fl::Mode::kOff;
          Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {*car, Bits(ground_speed)});
          j.roll_v = ground_speed;
          Toast(s, label + ": crashed.", 1500);
        }
        REXLOG_INFO("gta4-trainer: {} {} at {:.1f} m/s, sinking {:.1f} m/s, {:.0f} degrees ({:.2f} m over the ground)",
                    label, landed ? "touched down" : "hit something", j.fs.speed, sink, a.gamma * 57.2958f, clearance);
      } else {
        Invoke(ctx, base, "SET_VEHICLE_QUATERNION",
               {*car, Bits(j.fs.q.x), Bits(j.fs.q.y), Bits(j.fs.q.z), Bits(j.fs.q.w)});
        Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {*car, Bits(j.fs.speed)});
      }
    }
  }

  // Landing gear on R3, toggled on release (L3 + R3 together opens the
  // menu). Up only in the air; slowing below flying speed lowers it again.
  const bool r3 = pad.buttons & X_INPUT_GAMEPAD_RIGHT_THUMB;
  const bool l3 = pad.buttons & X_INPUT_GAMEPAD_LEFT_THUMB;
  if (r3 && !(j.prev_buttons & X_INPUT_GAMEPAD_RIGHT_THUMB)) {
    j.r3_held = true;
    j.r3_chord = false;
  }
  if (r3 && l3)
    j.r3_chord = true;
  if (!r3 && j.r3_held) {
    j.r3_held = false;
    if (!j.r3_chord && !s.open && g_jet_spec->gear) {
      if (!j.gear_up && !j.flying) {
        Toast(s, label + ": the landing gear stays down on the ground.", 1500);
      } else {
        j.gear_up = !j.gear_up;
        GearSound(!j.gear_up);
        Toast(s, j.gear_up ? "Gear up: less drag, a faster stall." : "Gear down: more drag, a slower stall.", 1500);
      }
    }
  }
  if (j.gear_up && !j.flying && ground_speed < 16.0f) {
    j.gear_up = false;
    Toast(s, label + ": gear down.", 1200);
  }
  const float gear_before = j.gear;
  const float gear_step = dt / kGearSeconds;
  j.gear = j.gear_up ? std::min(1.0f, j.gear + gear_step) : std::max(0.0f, j.gear - gear_step);
  if (g_jet_spec->gear && (j.gear != gear_before || now >= j.next_gear_write)) {
    PoseGear(ctx, base, *car, j.gear);
    j.next_gear_write = now + 250;
  }

  // The engine: idle in the jet, spooling up with the throttle (the
  // autopilot's, flying itself); the afterburner past 90% once rolling.
  g_jet_synth.SetVoice(g_jet_spec->voice);
  JetEngineSound(gta4::jet::EngineSynth::kIdle + (1.0f - gta4::jet::EngineSynth::kIdle) * eff_throttle,
                 eff_burner ? 1.0f : 0.0f, kJetVolume);
  UpdateFlame(ctx, base, j, *car, eff_throttle, eff_burner);
  PublishJetHud(j, s, ground_speed, clearance, eff_throttle, eff_burner, airbrake);

  uint32_t used = X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (j.flying)
    used |= X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER;  // the rudder
  if (s.jet) {
    // the guns (left, right), the next target (up), the autopilot (down)
    used |= X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT | X_INPUT_GAMEPAD_DPAD_UP |
            X_INPUT_GAMEPAD_DPAD_DOWN;
    // Where the guns point: the nose. Airborne the flight state knows it; on
    // the ground the jet's own attitude.
    float fwd[3] = {game.forward[0], game.forward[1], game.forward[2]};
    Quat aim{j.fs.q.x, j.fs.q.y, j.fs.q.z, j.fs.q.w};
    if (j.flying) {
      const float nose[3] = {0, 1, 0};
      QRotate(aim, nose, fwd);
    } else if (const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {*car, 0, 0, 0, 0}, 1, 0, 4)) {
      aim = QNormalize({q->f(0), q->f(1), q->f(2), q->f(3)});
      const float nose[3] = {0, 1, 0};
      QRotate(aim, nose, fwd);
    }
    const bool cycle = (pad.buttons & X_INPUT_GAMEPAD_DPAD_UP) && !(j.prev_buttons & X_INPUT_GAMEPAD_DPAD_UP) &&
                       !s.open;
    if (const auto nose = CarPoint(ctx, base, *car, g_jet_spec->guns[0])) {
      UpdateLock(ctx, base, j, s, nose->data(), fwd, now, cycle);
      const float probe[3] = {(*nose)[0] + fwd[0] * 60.0f, (*nose)[1] + fwd[1] * 60.0f,
                              (*nose)[2] + fwd[2] * 60.0f};
      BuildMarks(ctx, base, j, probe, now);
    }
    if ((pad.buttons & X_INPUT_GAMEPAD_DPAD_RIGHT) && now >= j.next_shot) {
      j.next_shot = now + 60;
      FireCannon(ctx, base, j, fwd);
    }
    if ((pad.buttons & X_INPUT_GAMEPAD_DPAD_LEFT) && !(j.prev_buttons & X_INPUT_GAMEPAD_DPAD_LEFT))
      LaunchMissile(ctx, base, j, fwd, aim, s);
  }
  j.prev_buttons = pad.buttons;
  return used;
}

void Perform(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, const Item& item) {
  switch (item.action) {
    case Action::kOpenPlayer: s.page = Page::kPlayer; s.selected = 0; break;
    case Action::kOpenVehicle: s.page = Page::kVehicle; s.selected = 0; break;
    case Action::kOpenSpawn: s.page = Page::kSpawn; s.selected = 0; break;
    case Action::kOpenTeleport: s.page = Page::kTeleport; s.selected = 0; break;
    case Action::kOpenWorld: s.page = Page::kWorld; s.selected = 0; break;
    case Action::kOpenWeapons: s.page = Page::kWeapons; s.selected = 0; break;
    case Action::kOpenChaos: s.page = Page::kChaos; s.selected = 0; break;
    case Action::kOpenStatus: s.page = Page::kStatus; s.selected = 0; break;
    case Action::kOpenTimeMachine: s.page = Page::kTimeMachine; s.selected = 0; break;
    case Action::kTmPreset:
    case Action::kTmField:
      g_tm_synth.Enter();
      if (!s.tm_map.empty()) {
        // A place: its pieces start loading now, in the background.
        const auto* info = FindMap(s.tm_map);
        if (info && info->installed) {
          MapRequest(ctx, base, *info);
          Toast(s, g_map.loaded ? "Destination set: " + info->label + ". Ready: 88 mph to go."
                                : "Destination set: " + info->label + ". Loading it in the background...",
                2500);
        } else {
          Toast(s, "Destination set. (That place isn't installed in this launch: time travel only.)", 3000);
        }
      } else {
        Toast(s, "Destination set.", 1000);
      }
      break;
    case Action::kTmPieces:
      s.map_movable = !s.map_movable;
      g_map_movable.store(s.map_movable, std::memory_order_relaxed);
      Toast(s, s.map_movable ? "Pieces as movable objects: from the next place loaded."
                             : "Pieces as buildings: from the next place loaded.",
            2500);
      break;
    case Action::kTmUnloadMap: {
      if (!g_map.info) {
        Toast(s, "No map destination is loaded.", 1500);
        break;
      }
      // The player on the place: taken home first (where the car left the city).
      const std::array<float, 3> at{game.position[0], game.position[1], game.position[2]};
      if (g_map.phase == 2 && OnMap(*g_map.info, at, 150.0f)) {
        if (g_tm.have_home)
          Teleport(ctx, base, game, g_tm.home[0], g_tm.home[1], g_tm.home[2], s);
        else
          Teleport(ctx, base, game, 2392.4f, -62.0f, std::nullopt, s);  // (Francis International)
      }
      const std::string label = g_map.info->label;
      MapUnload(ctx, base, "the menu");
      Toast(s, label + " unloaded.", 2000);
      break;
    }
    case Action::kTmCircuits:
      if (g_tm.in_car)
        s.tm_toggle_circuits = true;
      else
        Toast(s, "Get in a DeLorean time machine first.", 1800);
      break;
    case Action::kTmRefuel:
      if (g_tm.in_car)
        s.tm_refuel = true;
      else
        Toast(s, "Get in a DeLorean time machine first.", 1800);
      break;
    case Action::kTmHover:
      if (g_tm.in_car && g_tm.film == 2)
        s.tm_toggle_hover = true;
      else
        Toast(s, "Only the Part II time machine flies.", 1800);
      break;
    case Action::kToggleGod:
      s.god = !s.god;
      ApplyGod(ctx, base, game, s.god);
      Toast(s, s.god ? "God mode on." : "God mode off.");
      break;
    case Action::kToggleNeverWanted:
      s.never_wanted = !s.never_wanted;
      if (s.never_wanted)
        Invoke(ctx, base, "CLEAR_WANTED_LEVEL", {game.player});
      Toast(s, s.never_wanted ? "Never wanted on." : "Never wanted off.");
      break;
    case Action::kWantedApply:
      if (s.wanted_target == 0) {
        Invoke(ctx, base, "CLEAR_WANTED_LEVEL", {game.player});
      } else {
        Invoke(ctx, base, "ALTER_WANTED_LEVEL", {game.player, uint32_t(s.wanted_target)});
        Invoke(ctx, base, "APPLY_WANTED_LEVEL_CHANGE_NOW", {game.player, 0});
      }
      Toast(s, "Wanted level set to " + std::to_string(s.wanted_target) + ".");
      break;
    case Action::kMoney:
      if (Invoke(ctx, base, "ADD_SCORE", {game.player, 100000}))
        Toast(s, "$100,000 added.");
      else
        Toast(s, "Money native unavailable.");
      break;
    case Action::kHeal:
      Heal(ctx, base, game);
      Toast(s, "Healed.");
      break;
    case Action::kToggleVehicleGod:
      s.vehicle_god = !s.vehicle_god;
      if (!s.vehicle_god) {
        if (const auto car = CurrentVehicle(ctx, base, game))
          Invoke(ctx, base, "SET_CAR_PROOFS", {*car, 0, 0, 0, 0, 0});
      }
      Toast(s, s.vehicle_god ? "Vehicle god mode on." : "Vehicle god mode off.");
      break;
    case Action::kRepair:
      if (const auto car = CurrentVehicle(ctx, base, game)) {
        Invoke(ctx, base, "FIX_CAR", {*car});
        // and back on its wheels (a jet: level over the ground; a car: the game's way)
        const auto model = Invoke(ctx, base, "GET_CAR_MODEL", {*car, 0}, 1);
        if (const JetSpec* jet = model ? JetSpecOf(model->words[0]) : nullptr)
          StandOnWheels(ctx, base, *car, jet->stand);
        else
          Invoke(ctx, base, "SET_CAR_ON_GROUND_PROPERLY", {*car});
        Toast(s, "Vehicle repaired and back on its wheels.");
      } else {
        Toast(s, "Not in a vehicle.");
      }
      break;
    case Action::kToggleWarpIn:
      s.warp_in = !s.warp_in;
      break;
    case Action::kBeyondSummon:
      if (g_have_beyond)
        s.beyond_summon = true;
      else
        Toast(s, "The Beyond Batmobile is not installed (addons/beyond).", 2500);
      break;
    case Action::kBeyondCloak:
      if (g_have_beyond)
        s.beyond_toggle_cloak = true;
      else
        Toast(s, "The Beyond Batmobile is not installed (addons/beyond).", 2500);
      break;
    case Action::kToggleGadgets:
      s.gadgets = !s.gadgets;
      Toast(s, s.gadgets ? "Batmobile gadgets armed: L1 lock-on missile; d-pad left guns ahead, down guns "
                           "behind, right boost; horn (L3) drops a spike strip."
                         : "Gadgets off.",
            s.gadgets ? 6000 : 2500);
      break;
    case Action::kToggleRain:
      s.raining = !s.raining;
      SetRainModels(ctx, base, s.raining);
      Toast(s, s.raining ? "It's raining cars. Stand clear." : "The sky is clear again.");
      break;
    case Action::kToggleGravity:
      if (!NativeKnown("SET_GRAVITY_OFF")) {
        Toast(s, "This game version has no gravity control.");
        break;
      }
      s.gravity_off = !s.gravity_off;
      Invoke(ctx, base, "SET_GRAVITY_OFF", {s.gravity_off ? 1u : 0u});
      Toast(s, s.gravity_off ? "Gravity off: everything floats." : "Gravity back to normal.");
      break;
    case Action::kTogglePopupAuto:
      s.popup_auto = !s.popup_auto;
      g_popup.in_car = g_nexus_popup.in_car = false;  // re-applied on the next look
      Toast(s, s.popup_auto ? "Deimos and Nexus pop-ups rise with the headlights (night, rain, fog)."
                            : "Deimos and Nexus pop-ups: d-pad left in the car raises and lowers them.");
      break;
    case Action::kCompleteMission:
      // The mission found in its own WAIT (MissionLook), its pass run from there.
      Invoke(ctx, base, "CLEAR_WANTED_LEVEL", {game.player});  // (some passes want no wanted level)
      g_complete_result.store(int(CompleteResult::kLooking), std::memory_order_relaxed);
      g_complete_key.store(0, std::memory_order_relaxed);
      g_complete_thread.store(0, std::memory_order_relaxed);
      g_complete_seen.store(0, std::memory_order_relaxed);
      g_complete_until.store(NowMs() + 4000, std::memory_order_relaxed);
      g_complete_active.store(true, std::memory_order_relaxed);
      REXLOG_INFO("gta4-trainer: complete the current mission: looking for it");
      Toast(s, "Completing the current mission...", 2000);
      break;
    case Action::kJetRoute:
      if (!g_jet.in_jet) {
        Toast(s, "Get in the F-16 or the Batwing first.", 2000);
      } else if (!s.jet) {
        Toast(s, "Turn Jet Flight and Weapons on first.", 2000);
      } else if (!g_jet.flying) {
        Toast(s, "Take off first: the autopilot flies you there and lands.", 2500);
      } else {
        s.jet_route = true;
        s.open = false;  // (hands off: the autopilot flies)
      }
      break;
    case Action::kJetLandMode:
      s.jet_land_mode = (s.jet_land_mode + 1) % 3;
      Toast(s, s.jet_land_mode == 0   ? "Holding d-pad down in the air: the autopilot finds open ground nearby and lands."
               : s.jet_land_mode == 1 ? "Holding d-pad down in the air: the autopilot lands straight ahead."
                                      : "Holding d-pad down in the air: the autopilot flies to Francis Intl and lands.",
            3000);
      break;
    case Action::kTogglePerformance:
      s.performance = !s.performance;
      PerformanceMode(s.performance);
      Toast(s, s.performance ? "60 FPS Mode on: fewer cars and people and a shorter draw distance, only while "
                               "frames run long. Your own settings come back when it's off."
                             : "60 FPS Mode off: your own settings again.",
            s.performance ? 5000 : 2500);
      break;
    case Action::kToggleAnyRide:
      if (!RideHooksInstalled()) {
        Toast(s, "Bring Your Own Ride is not available in this game version.");
        break;
      }
      s.any_ride = !s.any_ride;
      g_any_ride.store(s.any_ride, std::memory_order_relaxed);
      Toast(s, s.any_ride ? "Bring Your Own Ride: missions take whatever you're driving."
                          : "Missions want their own vehicles again.",
            4000);
      break;
    case Action::kToggleCarLights:
      s.car_lights = !s.car_lights;
      Toast(s, s.car_lights ? "Headlights: d-pad left in any car switches them on and off."
                            : "Headlights back to the game (d-pad left: the radio).");
      break;
    case Action::kToggleJet:
      s.jet = !s.jet;
      Toast(s, s.jet ? "Jet flight and weapons on (F-16, Batwing)." : "Jet flight and weapons off.");
      break;
    case Action::kToggleJetExpert:
      s.jet_expert = !s.jet_expert;
      Toast(s, s.jet_expert ? "Expert controls: the stick rolls and pulls; banked, pull back to turn."
                            : "Standard controls: left and right bank and turn; let go and the wings level.",
            3000);
      break;
    case Action::kSpawnVehicle:
      StartSpawn(s, item.index, ctx, base);
      break;
    case Action::kTeleportWaypoint:
      TeleportToWaypoint(ctx, base, game, s);
      break;
    case Action::kTeleportPreset: {
      const auto& t = kTeleports[size_t(item.index)];
      Teleport(ctx, base, game, t.x, t.y, t.z != 0.0f ? std::optional<float>(t.z) : std::nullopt, s);
      break;
    }
    case Action::kTeleportPlace: {
      const auto& spots = PlaceSpots();
      if (item.index >= 0 && item.index < int(spots.size()))
        TeleportToPlace(ctx, base, game, s, *spots[size_t(item.index)].info, spots[size_t(item.index)].spot);
      break;
    }
    case Action::kSavePosition:
      std::copy(std::begin(game.position), std::end(game.position), std::begin(s.saved));
      s.have_saved = true;
      Toast(s, "Position saved.");
      break;
    case Action::kLoadPosition:
      if (s.have_saved)
        Teleport(ctx, base, game, s.saved[0], s.saved[1], s.saved[2], s);
      else
        Toast(s, "No position saved yet.");
      break;
    case Action::kTimeApply:
      Invoke(ctx, base, "SET_TIME_OF_DAY", {uint32_t(s.hour), uint32_t(s.minute)});
      Toast(s, "Time set.");
      break;
    case Action::kWeatherApply:
      Invoke(ctx, base, "FORCE_WEATHER_NOW", {uint32_t(s.weather)});
      Toast(s, std::string("Weather: ") + std::string(kWeathers[size_t(s.weather)]));
      break;
    case Action::kWeatherRelease:
      Invoke(ctx, base, "RELEASE_WEATHER", {});
      Toast(s, "Weather released.");
      break;
    case Action::kGiveLoadout:
      for (const auto& w : kLoadout)
        Invoke(ctx, base, "GIVE_WEAPON_TO_CHAR", {game.ped, w.type, w.ammo, 0});
      Toast(s, "Loadout given.");
      break;
    case Action::kRemoveWeapons:
      Invoke(ctx, base, "REMOVE_ALL_CHAR_WEAPONS", {game.ped});
      Toast(s, "Weapons removed.");
      break;
    default:
      break;
  }
}

// Left/right adjustments for value rows.
void Adjust(State& s, int direction) {
  const auto items = PageItems(s);
  if (s.selected >= int(items.size()))
    return;
  switch (s.page) {
    case Page::kPlayer:
      if (items[size_t(s.selected)].action == Action::kWantedApply)
        s.wanted_target = std::clamp(s.wanted_target + direction, 0, 6);
      break;
    case Page::kWorld:
      if (s.selected == 0)
        s.hour = (s.hour + direction + 24) % 24;
      else if (s.selected == 1)
        s.minute = (s.minute + direction + 60) % 60;
      else if (s.selected == 3 || s.selected == 4)
        s.weather = (s.weather + direction + int(kWeathers.size())) % int(kWeathers.size());
      break;
    case Page::kTimeMachine: {
      // The keypad beeps as the date is keyed in (in the car).
      auto& d = s.tm_dest;
      int digit = -1;
      switch (s.selected) {
        case 0:
          s.tm_preset = (s.tm_preset + direction + int(kTcdPresets.size())) % int(kTcdPresets.size());
          d = s.tm_preset == 0 ? TodaysDate() : kTcdPresets[size_t(s.tm_preset)].date;
          s.tm_map = std::string(kTcdPresets[size_t(s.tm_preset)].map);  // (loaded once confirmed, or at 88)
          digit = 10;
          break;
        case 1: d.month = (d.month - 1 + direction + 12) % 12 + 1; digit = d.month % 10; break;
        case 2: d.day = (d.day - 1 + direction + DaysIn(d.year, d.month)) % DaysIn(d.year, d.month) + 1; digit = d.day % 10; break;
        case 3: d.year = std::clamp(d.year + direction, 0, 9999); digit = d.year % 10; break;
        case 4: d.hour = (d.hour + direction + 24) % 24; digit = d.hour % 10; break;
        case 5: d.minute = (d.minute + direction + 60) % 60; digit = d.minute % 10; break;
        default: break;
      }
      d.day = std::min(d.day, DaysIn(d.year, d.month));
      if (digit >= 0 && g_tm_in_car.load(std::memory_order_relaxed))
        g_tm_synth.Key(digit);
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Pad handling: edges with hold-to-repeat, SNT style.
// ---------------------------------------------------------------------------
struct PadTracker {
  uint16_t last = 0;
  std::array<uint64_t, 16> next_repeat{};
  bool Pressed(uint16_t buttons, uint16_t mask, uint64_t now, bool repeat = false) {
    const int bit = std::countr_zero(uint32_t(mask));
    const bool held = buttons & mask;
    const bool was = last & mask;
    if (!held) {
      next_repeat[size_t(bit)] = 0;
      return false;
    }
    if (!was) {
      next_repeat[size_t(bit)] = now + 350;
      return true;
    }
    if (repeat && now >= next_repeat[size_t(bit)]) {
      next_repeat[size_t(bit)] = now + 140;
      return true;
    }
    return false;
  }
};
PadTracker g_pad;

void PublishDraw(const State& s, size_t native_total, size_t native_missing,
                 const std::vector<std::string>& missing_names) {
  DrawModel model;
  model.open = s.open;
  if (s.toast_until > NowMs())
    model.toast = s.toast;
  if (s.open) {
    switch (s.page) {
      case Page::kMain: model.title = "HOME"; break;
      case Page::kPlayer: model.title = "PLAYER OPTIONS"; break;
      case Page::kVehicle: model.title = "VEHICLE OPTIONS"; break;
      case Page::kSpawn: model.title = "CAR SPAWNING"; break;
      case Page::kSpawnStandard: model.title = "STANDARD VEHICLES"; break;
      case Page::kTeleport: model.title = "TELEPORTING"; break;
      case Page::kWorld: model.title = "TIME AND WEATHER"; break;
      case Page::kWeapons: model.title = "WEAPONS"; break;
      case Page::kChaos: model.title = "CHAOS"; break;
      case Page::kStatus: model.title = "MENU STATUS"; break;
      case Page::kTimeMachine: model.title = "TIME MACHINE"; break;
    }
    if (s.page == Page::kStatus) {
      model.lines.push_back({"Natives resolved: " + std::to_string(native_total - native_missing) +
                                 " of " + std::to_string(native_total),
                             false, false});
      for (const auto& name : missing_names)
        model.lines.push_back({"missing: " + name, false, true});
      if (missing_names.empty())
        model.lines.push_back({"Everything is available.", false, false});
      model.footer = "B: back";
    } else {
      const auto items = PageItems(s);
      // A windowed view keeps long lists (car spawning) on screen.
      constexpr int kWindow = 12;
      int top = 0;
      if (int(items.size()) > kWindow)
        top = std::clamp(s.selected - kWindow / 2, 0, int(items.size()) - kWindow);
      model.first = top;
      model.total = int(items.size());
      for (int i = top; i < int(items.size()) && i < top + kWindow; ++i) {
        std::string text{items[size_t(i)].label};
        if (s.page == Page::kPlayer && items[size_t(i)].action == Action::kWantedApply)
          text = "Wanted Level: " + std::to_string(s.wanted_target);
        if (s.page == Page::kWorld) {
          if (i == 0) text = "Hour: " + std::to_string(s.hour);
          if (i == 1) text = "Minute: " + std::to_string(s.minute);
          if (i == 3) text = "Weather: " + std::string(kWeathers[size_t(s.weather)]);
        }
        if (s.page == Page::kTimeMachine) {
          const auto& d = s.tm_dest;
          if (i == 0) text = "Destination: " + std::string(kTcdPresets[size_t(s.tm_preset)].name);
          if (i == 1) text = std::string("Month: ") + kMonths[std::clamp(d.month, 1, 12) - 1];
          if (i == 2) text = fmt::format("Day: {:02d}", d.day);
          if (i == 3) text = fmt::format("Year: {:04d}", d.year);
          if (i == 4) text = fmt::format("Hour: {:02d} {}", (d.hour + 11) % 12 + 1, d.hour < 12 ? "AM" : "PM");
          if (i == 5) text = fmt::format("Minute: {:02d}", d.minute);
          if (i == 9)
            text = !g_map.info          ? std::string("Unload Map Destination (none loaded)")
                   : g_map.phase == 2   ? "Unload " + g_map.info->label + " (despawn)"
                   : g_map.loaded       ? "Unload " + g_map.info->label + " (loaded, not placed)"
                   : g_map.known_count > 0 && g_map.loaded_count == g_map.known_count
                       ? "Unload " + g_map.info->label + " (loading its ground)"
                       : fmt::format("Unload {} (loading {} of {})", g_map.info->label, g_map.loaded_count,
                                     g_map.known_count);
          if (i == 10)
            text = s.map_movable ? "Place Pieces As: Movable Objects (the ground probe misses them)"
                                 : "Place Pieces As: Buildings (default)";
        }
        model.lines.push_back({std::move(text), i == s.selected, false});
      }
      model.footer = s.page == Page::kMain ? "X select    O close    L3+R3 open/close"
                                           : "X select    O back    < > change";
    }
    model.tab = TabOf(s.page);
    for (const auto& t : kTabs)
      model.tabs.emplace_back(t.second);
    model.page_serial = g_page_serial.load(std::memory_order_relaxed);
    model.slide = g_page_slide.load(std::memory_order_relaxed);
  }
  g_menu_visible.store(model.open || !model.toast.empty(), std::memory_order_relaxed);
  std::lock_guard lock(g_draw_mutex);
  g_draw = std::move(model);
}

std::vector<std::string> MissingNatives() {
  std::vector<std::string> missing;
  std::lock_guard lock(g_native_mutex);
  for (const auto& n : g_natives)
    if (!n.handler)
      missing.emplace_back(n.name);
  return missing;
}

}  // namespace

void ObserveNativeRegistration(PPCContext& context, uint8_t* base) {
  const auto name = String(base, context.r3.u32);
  if (name.empty())
    return;
  {
    std::lock_guard lock(g_native_mutex);
    for (auto& entry : g_natives)
      if (entry.name == name) {
        entry.handler = context.r4.u32;  // the game's own handler
        // (v1.7.40: the world's collision probes, to read offline - where
        // the physics level and its extents are, for Wake Island's runway)
        if (name == "GET_GROUND_Z_FOR_3D_COORD" || name == "REQUEST_COLLISION_AT_POSN" || name == "CREATE_OBJECT_NO_OFFSET")
          REXLOG_INFO("gta4-trainer: native {} is the game's {:08X}", name, context.r4.u32);
        break;
      }
  }
  // Bring Your Own Ride: our replacement is registered in the game's place.
  for (auto& hook : g_ride_hooks) {
    if (hook.name != name)
      continue;
    if (!hook.thunk) {
      auto* runtime = rex::Runtime::instance();
      auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
      hook.original = context.r4.u32;
      hook.original_fn = dispatcher ? dispatcher->GetFunction(hook.original) : nullptr;
      if (hook.original_fn)
        hook.thunk = dispatcher->AllocateThunk(hook.replacement, static_cast<uint32_t>(context.lr));
      if (hook.thunk)
        REXLOG_INFO("gta4-trainer: {} answered through thunk {:08X} (game handler {:08X})", name,
                    hook.thunk, hook.original);
      else
        REXLOG_WARN("gta4-trainer: {} could not be hooked (game handler {:08X})", name,
                    hook.original);
    }
    if (hook.thunk && context.r4.u32 == hook.original)
      context.r4.u32 = hook.thunk;
    return;
  }
}

bool InputOwned() { return g_input_owned.load(std::memory_order_relaxed); }

void PollMenu(PPCContext& ctx, uint8_t* base);

void Poll(PPCContext& ctx, uint8_t* base) {
  g_frame.fetch_add(1, std::memory_order_relaxed);
  const uint64_t start = NowNs();
  PollMenu(ctx, base);
  auto& c = g_cost;
  c.poll_ns += NowNs() - start;
  ++c.frames;
  const uint64_t now = NowMs();
  if (!c.window_start_ms) {
    c = TrainerCost{};
    c.window_start_ms = now;
  } else if (now - c.window_start_ms >= 10000) {
    const uint32_t frames = std::max<uint32_t>(c.frames, 1);
    REXLOG_INFO("gta4-trainer: cost over {:.1f} s: {} frames, {} passes, {:.1f} us a frame "
                "(passes {:.1f}, menu {:.1f}; worst pass {:.0f} us), {} mission-vehicle checks",
                (now - c.window_start_ms) / 1000.0, c.frames, c.passes,
                (c.pass_ns + c.poll_ns) / 1000.0 / frames, c.pass_ns / 1000.0 / frames,
                c.poll_ns / 1000.0 / frames, c.worst_pass_ns / 1000.0,
                g_ride_calls.exchange(0, std::memory_order_relaxed));
    c = TrainerCost{};
    c.window_start_ms = now;
  }
}

void PollMenu(PPCContext& ctx, uint8_t* base) {
  // Input and menu only. This hook runs in the frame's input stage, where
  // the game's natives are NOT safe (the first spawn attempt from here page
  // faulted inside CREATE_CAR): everything that touches the game is queued
  // for RunActions, which the script-stage hooks drive.
  (void)ctx;
  if (!REXCVAR_GET(gta4_trainer)) {
    g_input_owned.store(false, std::memory_order_relaxed);
    g_pad_adjust.store(0, std::memory_order_relaxed);
    return;
  }
  auto& s = g_state;
  const uint64_t now = NowMs();

  // Single-player only: in a network session the menu closes and stays shut.
  if (!WorldAlive(base) || g_online.load(std::memory_order_relaxed)) {
    const bool was_open = s.open;
    s.open = false;
    g_input_owned.store(false, std::memory_order_relaxed);
    g_pad_adjust.store(0, std::memory_order_relaxed);
    g_gadget_buttons.store(0, std::memory_order_relaxed);
    g_gadget_previous.store(0, std::memory_order_relaxed);
    {
      std::lock_guard lock(g_drive_mutex);
      g_drive_previous = g_drive_pad = DrivePad{};
    }
    if (was_open || s.toast_until > now)
      PublishDraw(s, g_natives.size(), 0, {});
    return;
  }

  // The pad, as the game last polled it.
  X_INPUT_STATE pad{};
  auto* kernel = REX_KERNEL_STATE();
  auto* emulator = kernel ? kernel->emulator() : nullptr;
  auto* input = emulator ? static_cast<rex::input::InputSystem*>(emulator->input_system()) : nullptr;
  uint16_t buttons = 0;
  if (input && input->TryGetLastState(0, &pad))
    buttons = pad.gamepad.buttons;

  // L3 + R3 together open and close the menu, as in the first releases.
  // The chord itself is hidden from the game, so opening the menu does not
  // also crouch or look behind.
  constexpr uint16_t kChord = X_INPUT_GAMEPAD_LEFT_THUMB | X_INPUT_GAMEPAD_RIGHT_THUMB;
  const bool chord = (buttons & kChord) == kChord;
  const bool was_chord = (g_pad.last & kChord) == kChord;
  if (chord && !was_chord) {
    s.open = !s.open;
    if (s.open) {
      s.page = Page::kMain;
      s.selected = 0;
    }
  }
  // After a chord both sticks stay hidden until both are released: letting
  // go of one first must not hand the game a lone L3 (crouch) or R3.
  static bool s_chord_latched = false;
  if (chord)
    s_chord_latched = true;
  else if (!(buttons & kChord))
    s_chord_latched = false;
  uint32_t adjust = 0;
  if (!s.open) {
    if (s_chord_latched)
      adjust |= kChord;
    // The d-pad the Batmobile's gadgets or the jet's weapons are using.
    adjust |= g_vehicle_mask.load(std::memory_order_relaxed) & 0xFFFF;
  }
  g_pad_adjust.store(adjust, std::memory_order_relaxed);

  if (s.open) {
    const Page page_before = s.page;
    const auto items = PageItems(s);
    const int count = int(items.size());
    if (g_pad.Pressed(buttons, X_INPUT_GAMEPAD_DPAD_UP, now, true) && count)
      s.selected = (s.selected + count - 1) % count;
    if (g_pad.Pressed(buttons, X_INPUT_GAMEPAD_DPAD_DOWN, now, true) && count)
      s.selected = (s.selected + 1) % count;
    if (g_pad.Pressed(buttons, X_INPUT_GAMEPAD_DPAD_LEFT, now, true))
      Adjust(s, -1);
    if (g_pad.Pressed(buttons, X_INPUT_GAMEPAD_DPAD_RIGHT, now, true))
      Adjust(s, 1);
    // L1/R1: the next section along the tab strip (the panel slides over).
    const bool l1 = g_pad.Pressed(buttons, X_INPUT_GAMEPAD_LEFT_SHOULDER, now, true);
    const bool r1 = g_pad.Pressed(buttons, X_INPUT_GAMEPAD_RIGHT_SHOULDER, now, true);
    if (l1 != r1) {
      const int n = int(kTabs.size());
      const int tab = (TabOf(s.page) + (r1 ? 1 : n - 1)) % n;
      s.page = kTabs[size_t(tab)].first;
      s.selected = 0;
      g_page_serial.fetch_add(1, std::memory_order_relaxed);
      g_page_slide.store(r1 ? 1 : -1, std::memory_order_relaxed);
    }
    if (g_pad.Pressed(buttons, X_INPUT_GAMEPAD_A, now) && s.selected < count) {
      const Item& item = items[size_t(s.selected)];
      switch (item.action) {
        // Page navigation is pure UI and stays here.
        case Action::kOpenPlayer: s.page = Page::kPlayer; s.selected = 0; break;
        case Action::kOpenVehicle: s.page = Page::kVehicle; s.selected = 0; break;
        case Action::kOpenSpawn: s.page = Page::kSpawn; s.selected = 0; break;
        case Action::kOpenTeleport: s.page = Page::kTeleport; s.selected = 0; break;
        case Action::kOpenWorld: s.page = Page::kWorld; s.selected = 0; break;
        case Action::kOpenWeapons: s.page = Page::kWeapons; s.selected = 0; break;
        case Action::kOpenChaos: s.page = Page::kChaos; s.selected = 0; break;
        case Action::kOpenStatus: s.page = Page::kStatus; s.selected = 0; break;
        case Action::kOpenTimeMachine: s.page = Page::kTimeMachine; s.selected = 0; break;
        case Action::kOpenSpawnStandard: s.page = Page::kSpawnStandard; s.selected = 0; break;
        // Everything else runs where natives are safe.
        default: g_pending.push_back({item.action, item.index}); break;
      }
    }
    if (g_pad.Pressed(buttons, X_INPUT_GAMEPAD_B, now)) {
      if (s.page == Page::kMain) {
        s.open = false;
      } else if (s.page == Page::kSpawnStandard) {  // back to the spawn page, on its Standard row
        s.page = Page::kSpawn;
        s.selected = int(kVehicles.size()) - kFirstAddon;
      } else {
        s.page = Page::kMain;
        s.selected = 0;
      }
    }
    // (into a page or back out: the panel's contents rise into place)
    if (s.page != page_before && l1 == r1) {
      g_page_serial.fetch_add(1, std::memory_order_relaxed);
      g_page_slide.store(0, std::memory_order_relaxed);
    }
    // No gadget input while the menu owns the pad.
    g_gadget_buttons.store(0, std::memory_order_relaxed);
    g_gadget_previous.store(0, std::memory_order_relaxed);
    std::lock_guard lock(g_drive_mutex);
    g_drive_previous = g_drive_pad = DrivePad{};
  } else {
    const uint16_t was = g_gadget_buttons.load(std::memory_order_relaxed);
    g_gadget_previous.store(was, std::memory_order_relaxed);
    g_gadget_buttons.store(buttons, std::memory_order_relaxed);
    if ((buttons & X_INPUT_GAMEPAD_Y) && !(was & X_INPUT_GAMEPAD_Y))
      g_y_down_at.store(now, std::memory_order_relaxed);
    std::lock_guard lock(g_drive_mutex);
    g_drive_previous = g_drive_pad;
    g_drive_pad = DrivePad{buttons, pad.gamepad.left_trigger, pad.gamepad.right_trigger,
                           pad.gamepad.thumb_lx, pad.gamepad.thumb_ly};
  }
  g_pad.last = buttons;
  g_input_owned.store(s.open, std::memory_order_relaxed);
  if (WantsWork(s))
    g_has_work.store(true, std::memory_order_relaxed);

  // Publishing builds strings; while the menu is shut and nothing is on
  // screen there is nothing to draw, so skip it (one trailing publish clears
  // the overlay after a close or an expired toast).
  static bool s_published = false;
  if (s.open || s.toast_until > now || s_published) {
    const auto missing = MissingNatives();
    PublishDraw(s, g_natives.size(), missing.size(), missing);
    s_published = s.open || s.toast_until > now;
  }
}

// Complete Current Mission, the menu's side: what became of the request.
std::string MissionTitle(std::string_view script) {
  // vlad3 -> Vlad 3, gerry3b -> Gerry 3b, brian_2 -> Brian 2
  std::string out;
  bool digits = false;
  for (size_t k = 0; k < script.size(); ++k) {
    char c = script[k];
    if (c == '_') {
      c = ' ';
    } else if (std::isdigit(static_cast<unsigned char>(c)) && !digits) {
      digits = true;
      if (!out.empty() && out.back() != ' ')
        out += ' ';
    }
    out += k == 0 ? char(std::toupper(static_cast<unsigned char>(c))) : c;
  }
  return out;
}
void CompleteTick(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s) {
  (void)ctx;
  (void)base;
  (void)game;
  const uint64_t now = NowMs();
  static int told = -1;
  const auto result = CompleteResult(g_complete_result.load(std::memory_order_relaxed));
  const auto* pass = gta4::missions::FindPass(g_complete_key.load(std::memory_order_relaxed));
  const std::string name = pass ? MissionTitle(pass->name) : std::string("This mission");
  bool done = false;
  if (int(result) != told) {
    told = int(result);
    switch (result) {
      case CompleteResult::kStaged:
      case CompleteResult::kKeyed:
        Toast(s, "Completing " + name + "...", 3000);
        break;
      case CompleteResult::kNotSupported:
        Toast(s, name + " can't be completed from the menu yet.", 3500);
        done = true;
        break;
      case CompleteResult::kNoStatics:
      case CompleteResult::kOddStage:
        Toast(s, "Couldn't complete " + name + " safely, so nothing was changed.", 3500);
        done = true;
        break;
      default:
        break;
    }
  }
  if (!done && now > g_complete_until.load(std::memory_order_relaxed)) {
    if (result == CompleteResult::kLooking)
      Toast(s, "No mission is running.", 2500);
    else if (now - g_complete_seen.load(std::memory_order_relaxed) < 700)
      Toast(s, name + " is still running: try again in a moment (after a cutscene or a phone call).", 4000);
    done = true;
  }
  if (done) {
    g_complete_active.store(false, std::memory_order_relaxed);
    told = -1;
  }
}

// ---------------------------------------------------------------------------
// 60 FPS Mode. The game's own work each frame (its world, its people and
// traffic, the scene it draws) runs on one core; in busy streets it takes
// 17-25 ms (the log's "game's frames: ... to 25"), over the 16.7 ms a 60 Hz
// TV gives a frame, and such a frame waits for the next refresh and shows
// for two: the hitching. This mode trims that work only while frames run
// long, a step at a time, and gives it back with headroom: fewer cars and
// people (the port's population scales), the cars' reflection every other
// frame, then the draw distance (through the dynamic factor, the user's own
// setting untouched), 72% at most. Off, everything is as it was.
// ---------------------------------------------------------------------------
struct PerfState {
  int level = 0;
  uint64_t last_us = 0, window_us = 0, last_change = 0;
  int frames = 0, long_frames = 0, quiet = 0;
  bool saved = false;
  std::string population[4], env_interval, dynamic, factor, frame_limit;
};
PerfState g_perf;
constexpr const char* kPopulationCvars[4] = {"gta4_traffic_density_scale", "gta4_parked_car_density_scale",
                                             "gta4_ped_density_scale", "gta4_scenario_ped_density_scale"};
struct PerfLevel {
  float population, distance;
  const char* env_interval;
};
// (v1.7.24: two more steps, for the places the game's own work stays over
// a frame at the fourth: the airport with a jet, log 083's 19 ms main thread)
constexpr std::array<PerfLevel, 7> kPerfLevels = {{{1.0f, 1.0f, nullptr},
                                                   {0.85f, 1.0f, "2"},
                                                   {0.75f, 0.9f, "2"},
                                                   {0.65f, 0.8f, "2"},
                                                   {0.6f, 0.72f, "2"},
                                                   {0.5f, 0.66f, "3"},
                                                   {0.42f, 0.6f, "3"}}};

double ParseOr(const std::string& text, double fallback) {
  try {
    return text.empty() ? fallback : std::stod(text);
  } catch (...) {
    return fallback;
  }
}
void ApplyPerfLevel(int level) {
  const auto& L = kPerfLevels[size_t(std::clamp(level, 0, int(kPerfLevels.size()) - 1))];
  for (int k = 0; k < 4; ++k)
    rex::cvar::SetFlagByName(kPopulationCvars[k],
                             fmt::format("{:.3f}", ParseOr(g_perf.population[k], 1.0) * L.population));
  rex::cvar::SetFlagByName("gta4_native_environment_reflection_interval",
                           L.env_interval ? L.env_interval : g_perf.env_interval);
  const bool distance = L.distance < 0.999f;
  rex::cvar::SetFlagByName("gta4_dynamic_draw_distance", distance ? "true" : g_perf.dynamic);
  rex::cvar::SetFlagByName("gta4_draw_distance_dynamic_factor",
                           distance ? fmt::format("{:.3f}", L.distance) : g_perf.factor);
}
void PerformanceMode(bool on) {
  if (on) {
    for (int k = 0; k < 4; ++k)
      g_perf.population[k] = rex::cvar::GetFlagByName(kPopulationCvars[k]);
    g_perf.env_interval = rex::cvar::GetFlagByName("gta4_native_environment_reflection_interval");
    g_perf.dynamic = rex::cvar::GetFlagByName("gta4_dynamic_draw_distance");
    g_perf.factor = rex::cvar::GetFlagByName("gta4_draw_distance_dynamic_factor");
    g_perf.frame_limit = rex::cvar::GetFlagByName("gta4_frame_limit");
    if (g_perf.env_interval.empty())
      g_perf.env_interval = "1";
    if (g_perf.dynamic.empty())
      g_perf.dynamic = "false";
    if (g_perf.factor.empty())
      g_perf.factor = "1.0";
    g_perf.saved = true;
    if (g_perf.frame_limit != "60")
      rex::cvar::SetFlagByName("gta4_frame_limit", "60");
    g_perf.level = 0;
    g_perf.frames = g_perf.long_frames = g_perf.quiet = 0;
    g_perf.last_us = g_perf.window_us = 0;
    REXLOG_INFO("gta4-trainer: 60 FPS Mode on (population {}, reflection every {} frames, dynamic distance {}, "
                "frame limit {})",
                g_perf.population[0], g_perf.env_interval, g_perf.dynamic, g_perf.frame_limit);
  } else if (g_perf.saved) {
    for (int k = 0; k < 4; ++k)
      rex::cvar::SetFlagByName(kPopulationCvars[k], g_perf.population[k].empty() ? "1.0" : g_perf.population[k]);
    rex::cvar::SetFlagByName("gta4_native_environment_reflection_interval", g_perf.env_interval);
    rex::cvar::SetFlagByName("gta4_dynamic_draw_distance", g_perf.dynamic);
    rex::cvar::SetFlagByName("gta4_draw_distance_dynamic_factor", g_perf.factor);
    if (!g_perf.frame_limit.empty())
      rex::cvar::SetFlagByName("gta4_frame_limit", g_perf.frame_limit);
    g_perf.saved = false;
    REXLOG_INFO("gta4-trainer: 60 FPS Mode off: settings restored");
  }
}
// Once a frame while on: the frames over 17.6 ms in each second; over 12% of
// them steps the trimming up (every 2 s at most; every second over 30%), ten
// seconds under 2% steps it down.
void PerformanceTick(State& s) {
  const uint64_t now_us = NowUs();
  const uint64_t last = g_perf.last_us;
  g_perf.last_us = now_us;
  if (!last)
    return;
  const uint64_t frame = now_us - last;
  if (frame > 200000)  // (a loading screen, a pause)
    return;
  ++g_perf.frames;
  if (frame > 17600)
    ++g_perf.long_frames;
  g_perf.window_us += frame;
  if (g_perf.window_us < 1000000)
    return;
  const float share = float(g_perf.long_frames) / float(std::max(g_perf.frames, 1));
  const float fps = float(g_perf.frames) * 1e6f / float(g_perf.window_us);
  const uint64_t now = NowMs();
  int level = g_perf.level;
  // (a lot of long frames: a step a second; a few: one every two)
  const uint64_t step_ms = share > 0.3f ? 1000 : 2000;
  if (share > 0.12f && level < int(kPerfLevels.size()) - 1 && now - g_perf.last_change > step_ms) {
    ++level;
    g_perf.quiet = 0;
  } else if (share < 0.02f) {
    if (++g_perf.quiet >= 10 && level > 0) {
      --level;
      g_perf.quiet = 0;
    }
  } else {
    g_perf.quiet = 0;
  }
  if (level != g_perf.level) {
    g_perf.level = level;
    g_perf.last_change = now;
    ApplyPerfLevel(level);
    REXLOG_INFO("gta4-trainer: 60 FPS Mode: {:.1f} fps, {:.0f}% of frames long; trimming level {}", fps,
                share * 100.0f, level);
  } else if (level >= 2) {
    ApplyPerfLevel(level);  // (the renderer's own controller may have moved the factor)
  }
  g_perf.frames = g_perf.long_frames = 0;
  g_perf.window_us = 0;
  (void)s;
}

// v1.7.30: nobody drags the player out of the armoured cars or the aircraft
// (a cop pulled him out of the Lazar 3 with the game's own jacking move):
// the game's flag for it, kept set while he sits in one, cleared after.
bool g_cant_drag = false;
uint64_t g_cant_drag_renew = 0;
void ArmouredSeat(PPCContext& ctx, uint8_t* base, const GameContext& game, uint64_t now) {
  static const std::array<uint32_t, 4> kModels = {ModelHash("LAZAR3"), ModelHash("BATMOBILE"), ModelHash("F16"),
                                                  ModelHash("BATWING")};
  const auto& v = PlayerVehicle(ctx, base, game);
  const bool in = v.car && std::find(kModels.begin(), kModels.end(), v.model) != kModels.end();
  if (in == g_cant_drag && !(in && now >= g_cant_drag_renew))
    return;
  g_cant_drag_renew = now + 2000;  // (set again now and then: a respawn clears it)
  if (Invoke(ctx, base, "SET_CHAR_CANT_BE_DRAGGED_OUT", {game.ped, in ? 1u : 0u})) {
    if (in != g_cant_drag)
      REXLOG_INFO("gta4-trainer: the player {} be dragged out of his vehicle", in ? "can no longer" : "can again");
    g_cant_drag = in;
  }
}

void RunActions(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(gta4_trainer))
    return;
  auto& s = g_state;
  ++s.tick;
  const uint64_t now = NowMs();
  // (The hook calling this runs it at most once per frame.) The add-on
  // watchers keep the trainer awake only on installs that have the add-ons,
  // and until the player is in one of them they cost a look a few times a
  // second, not a full pass every frame.
  const bool missiles_live = std::any_of(g_jet.missiles.begin(), g_jet.missiles.end(),
                                         [](const Missile& m) { return m.live; });
  const bool jet_due = (g_have_f16 || g_have_batwing) && (g_jet.in_jet || missiles_live || now >= g_jet.next_probe);
  const bool popup_due = g_have_deimos && PopupsDue(g_popup, now);
  const bool nexus_popup_due = g_have_nexus && PopupsDue(g_nexus_popup, now);
  const bool doors_due = HaveDoorCars() && (DoorsBusy(g_doors, now) || now >= g_doors.next_probe);
  const bool lights_due = s.car_lights && (g_lights.in_car || now >= g_lights.next_look);
  // With the menu open: whether the player is driving (R2 then still reaches
  // the game), looked at four times a second.
  static uint64_t s_next_drive_look = 0;
  const bool drive_due = s.open && now >= s_next_drive_look;
  // The toggles on their own clocks: a full pass (with its natives) runs only
  // when one of them is due, not every frame because one is switched on.
  static uint64_t s_next_god = 0, s_next_wanted = 0, s_next_car = 0, s_next_gadget_look = 0;
  static bool s_in_batmobile = false;
  const bool god_due = s.god && now >= s_next_god;
  const bool wanted_due = s.never_wanted && now >= s_next_wanted;
  const bool car_god_due = s.vehicle_god && now >= s_next_car;
  const bool gadgets_due = (s.gadgets && (s_in_batmobile || now >= s_next_gadget_look)) || GadgetsBusy();
  const bool canopy_due = g_have_batmobile && (CanopyBusy(g_canopy) || now >= g_canopy.next_probe);
  const bool jet_canopy_due = g_have_f16 && (CanopyBusy(g_jet_canopy) || now >= g_jet_canopy.next_probe);
  const bool batwing_canopy_due =
      g_have_batwing && (CanopyBusy(g_batwing_canopy) || now >= g_batwing_canopy.next_probe);
  const bool complete_due = g_complete_active.load(std::memory_order_relaxed);
  const bool perf_due = s.performance;
  const bool lazar_ramps_due = g_have_lazar && (CanopyBusy(g_lazar_ramps) || now >= g_lazar_ramps.next_probe);
  const bool lazar_due = g_have_lazar && (g_lazar.in_car || LazarShellsLive() || now >= g_lazar.next_probe);
  const bool tm_due = HaveTimeMachines() && (g_tm.in_car || g_tm.phase != 0 || g_tm.fires_out || g_tm.vents_out ||
                                             g_tm.frost_until || now >= g_tm.next_probe || g_flash_until > now ||
                                             VeilLevel(now) > 0.0f);
  const bool beyond_due = g_have_beyond && (g_beyond.in_car || g_beyond.summoning || g_beyond.sounding ||
                                            g_beyond.spawn_pending || s.beyond_summon || s.beyond_toggle_cloak ||
                                            now >= g_beyond.next_probe);
  const bool beyond_canopy_due =
      g_have_beyond && (CanopyBusy(g_beyond_canopy) || now >= g_beyond_canopy.next_probe);
  const bool bat_engine_due = g_have_batmobile && (g_bat_engine.in_car || g_bat_engine.running ||
                                                   now >= g_bat_engine.next_probe);
  const bool deimos_engine_due = g_have_deimos && (g_deimos_engine.in_car || g_deimos_engine.running ||
                                                   now >= g_deimos_engine.next_probe);
  auto engine_due = [now](bool have, const CarEngineState& e) {
    return have && (e.in_car || e.running || now >= e.next_probe);
  };
  const bool nexus_engine_due = engine_due(g_have_nexus, g_nexus_engine);
  const bool arbiter_engine_due = engine_due(g_have_arbiter, g_arbiter_engine);
  const bool dominator_engine_due = engine_due(g_have_dominator, g_dominator_engine);
  const bool lazar_engine_due = engine_due(g_have_lazar, g_lazar_engine);
  const bool gauntlet_engine_due = engine_due(g_have_gauntlet, g_gauntlet_engine);
  const bool hellcat_engine_due = engine_due(g_have_hellcat, g_hellcat_engine);
  const bool dmc_engine_due = engine_due(g_have_dmc12, g_dmc_engine);
  const bool bttf1_engine_due = engine_due(g_have_bttf1, g_bttf1_engine);
  const bool bttf2_engine_due = engine_due(g_have_bttf2, g_bttf2_engine);
  const bool bttf3_engine_due = engine_due(g_have_bttf3, g_bttf3_engine);
  const bool any_engine_due = bat_engine_due || deimos_engine_due || nexus_engine_due || arbiter_engine_due ||
                              dominator_engine_due || lazar_engine_due || gauntlet_engine_due || dmc_engine_due ||
                              bttf1_engine_due || bttf2_engine_due || bttf3_engine_due || hellcat_engine_due;
  const bool rain_due = s.raining && now >= g_next_rain;
  const bool teleport_due = g_teleport.active;
  const bool heat_due = g_heat_pending > 0.0f;
  static uint64_t s_next_seat_look = 0;
  const bool seat_due = (g_have_lazar || g_have_batmobile || g_have_f16 || g_have_batwing) && now >= s_next_seat_look;
  const bool map_due = g_map.info != nullptr && now >= g_map.next_check;
  const bool creatures_due = !g_life.creatures.empty();
  static uint64_t s_next_guard = 0;
  const bool guard_due = now >= s_next_guard;  // (v1.7.40: a place stood under the player)
  if (g_pending.empty() && !god_due && !wanted_due && !car_god_due && !gadgets_due && !canopy_due && !heat_due &&
      !jet_canopy_due && !batwing_canopy_due && !complete_due && !perf_due && !teleport_due && !any_engine_due &&
      s.pending_model < 0 && !jet_due && !rain_due &&
      !popup_due && !nexus_popup_due && !doors_due && !lights_due && !drive_due && !lazar_ramps_due && !lazar_due &&
      !tm_due && !seat_due && !map_due && !creatures_due && !guard_due && !beyond_due && !beyond_canopy_due &&
      g_addons_probed)
    return;
  // No world yet (menus, loading): look again in half a second, not every frame.
  static uint64_t s_world_retry = 0;
  if (now < s_world_retry && g_pending.empty())
    return;

  const GameContext game = ReadContext(ctx, base);
  g_online.store(game.online, std::memory_order_relaxed);
  if (!game.valid || game.online) {
    if (game.online) {
      // Nothing of the trainer survives into a network session.
      s.god = s.never_wanted = s.vehicle_god = s.gadgets = s.jet = s.raining = false;
      s.any_ride = false;
      g_any_ride.store(false, std::memory_order_relaxed);
    }
    g_pending.clear();
    s.pending_model = -1;
    g_vehicle_mask.store(0, std::memory_order_relaxed);
    s_world_retry = now + 500;
    return;
  }
  ++g_pass;
  gta4::addons::MarkLaunchHealthy();
  // The player ped is another one: the world was rebuilt (a game loaded or
  // started over), and what stood on it went with it (v1.7.40).
  static uint32_t s_last_ped = 0;
  if (game.ped != s_last_ped) {
    if (s_last_ped && (g_map.info || g_life.info))
      MapForget(ctx, base, "the player ped changed: a game loaded or started over");
    s_last_ped = game.ped;
  }
  if (!g_addons_probed) {
    g_addons_probed = true;
    if (gta4::addons::ShadowsSkippedThisLaunch())
      Toast(s, "Add-on cars are off for this launch: the last two launches closed before the "
               "city loaded. Restart the game to turn them back on.", 10000);
    auto installed = [&](std::string_view model) {
      const auto known = Invoke(ctx, base, "IS_MODEL_IN_CDIMAGE", {ModelHash(model)});
      return known && known->r0() != 0;
    };
    g_have_deimos = installed("DEIMOS");
    g_have_f16 = installed("F16");
    g_have_batwing = installed("BATWING");
    g_have_batmobile = installed("BATMOBILE");
    g_have_nexus = installed("NEXUS");
    g_have_arbiter = installed("ARBITERX");
    g_have_dominator = installed("DOM645T");
    g_have_lazar = installed("LAZAR3");
    g_have_gauntlet = installed("GAUNTLETC");
    g_have_dmc12 = installed("DELNOR");
    g_have_bttf1 = installed("DELONE");
    g_have_bttf2 = installed("DELTWO");
    g_have_bttf3 = installed("DELTHR");
    g_have_bttf2h = installed("DELHOV");
    g_have_beyond = installed("BEYOND");
    g_have_hellcat = installed("HELLCAT");
    REXLOG_INFO("gta4-trainer: add-ons present: Batmobile {}, F-16 {}, Batwing {}, Deimos {}, Dominator 645 {}, "
                "Nexus {}, Arbiter X {}, Lazar 3 {}, Gauntlet Classic {}, DMC-12 {}, time machines {}/{}/{} "
                "(v1.7.21 flying form {}), Beyond Batmobile {}, Hellcat {}",
                g_have_batmobile, g_have_f16, g_have_batwing, g_have_deimos, g_have_dominator, g_have_nexus,
                g_have_arbiter, g_have_lazar, g_have_gauntlet, g_have_dmc12, g_have_bttf1, g_have_bttf2, g_have_bttf3,
                g_have_bttf2h, g_have_beyond, g_have_hellcat);
  }

  bool acted = false;
  for (const auto& pending : g_pending) {
    Perform(ctx, base, game, s, Item{{}, pending.action, pending.index});
    acted = true;
  }
  g_pending.clear();

  // Continuous effects, each on its own clock.
  if (god_due) {
    s_next_god = now + 500;
    ApplyGod(ctx, base, game, true);
  }
  if (wanted_due) {
    s_next_wanted = now + 250;
    const auto wanted = Invoke(ctx, base, "IS_WANTED_LEVEL_GREATER", {game.player, 0});
    if (wanted && wanted->r0())
      Invoke(ctx, base, "CLEAR_WANTED_LEVEL", {game.player});
  }
  if (car_god_due) {
    s_next_car = now + 500;
    if (const auto car = CurrentVehicle(ctx, base, game))
      Invoke(ctx, base, "SET_CAR_PROOFS", {*car, 1, 1, 1, 1, 1});
  }
  FinishSpawn(ctx, base, game, s);
  if (teleport_due)
    TeleportTick(ctx, base, game, s);
  if (seat_due) {
    s_next_seat_look = now + 400;
    ArmouredSeat(ctx, base, game, now);
  }
  if (map_due) {
    MapTick(ctx, base, game, s, now);
    LifeTick(ctx, base, game, s, now);
  }
  if (guard_due) {
    s_next_guard = now + (g_map.guarding ? 250 : 1000);
    MapGuard(ctx, base, game, s, now);
  }
  if (creatures_due)
    LifeCreatureFrame(ctx, base, game, now);
  if (drive_due) {
    s_next_drive_look = now + 250;
    PlayerVehicle(ctx, base, game);
  }
  uint32_t vehicle_mask = 0;
  if (gadgets_due) {
    // Every frame in the Batmobile (its buttons), a look a few times a
    // second otherwise.
    s_next_gadget_look = now + 400;
    const uint16_t buttons = g_gadget_buttons.load(std::memory_order_relaxed);
    const uint16_t previous = g_gadget_previous.load(std::memory_order_relaxed);
    const uint32_t used = Gadgets(ctx, base, game, s, buttons, previous);
    s_in_batmobile = used != 0;
    vehicle_mask |= used;
    // Consume the edge so the second script-stage site cannot double-fire.
    g_gadget_previous.store(buttons, std::memory_order_relaxed);
  } else if (!s.gadgets) {
    s_in_batmobile = false;
  }
  if (canopy_due)
    vehicle_mask |= Canopy(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed), kBatCanopy,
                           g_canopy);
  if (jet_canopy_due)
    vehicle_mask |= Canopy(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed), kJetCanopy,
                           g_jet_canopy);
  if (batwing_canopy_due)
    vehicle_mask |= Canopy(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed), kBatwingCanopy,
                           g_batwing_canopy);
  if (complete_due)
    CompleteTick(ctx, base, game, s);
  if (perf_due)
    PerformanceTick(s);
  if (lazar_ramps_due)
    vehicle_mask |= Canopy(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed), kLazarRamps,
                           g_lazar_ramps);
  if (lazar_due)
    vehicle_mask |= LazarTurret(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed));
  if (tm_due) {
    DrivePad pad;
    {
      std::lock_guard lock(g_drive_mutex);
      pad = g_drive_pad;
    }
    vehicle_mask |= TimeMachine(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed), pad);
    PublishTcd(s);
  }
  if (beyond_due || beyond_canopy_due) {
    DrivePad pad;
    {
      std::lock_guard lock(g_drive_mutex);
      pad = g_drive_pad;
    }
    uint16_t buttons = g_gadget_buttons.load(std::memory_order_relaxed);
    if (beyond_due)
      vehicle_mask |= BeyondTick(ctx, base, game, s, buttons, pad);
    // (the canopy sees Y only once the car has settled: BeyondTick keeps it
    // while the car comes down, and gives it once, settled)
    if (g_beyond.y_taken)
      buttons &= uint16_t(~X_INPUT_GAMEPAD_Y);
    if (g_beyond.exit_pulse)
      buttons |= X_INPUT_GAMEPAD_Y;
    vehicle_mask |= Canopy(ctx, base, game, s, buttons, kBeyondCanopy, g_beyond_canopy);
  }
  if (any_engine_due) {
    DrivePad pad;
    {
      std::lock_guard lock(g_drive_mutex);
      pad = g_drive_pad;
    }
    if (bat_engine_due)
      CarEngine(ctx, base, game, pad, g_bat_engine, g_gadget.boosting ? 1.0f : 0.0f);
    if (deimos_engine_due)
      CarEngine(ctx, base, game, pad, g_deimos_engine, 0.0f);
    if (nexus_engine_due)
      CarEngine(ctx, base, game, pad, g_nexus_engine, 0.0f);
    if (arbiter_engine_due)
      CarEngine(ctx, base, game, pad, g_arbiter_engine, 0.0f);
    if (dominator_engine_due)
      CarEngine(ctx, base, game, pad, g_dominator_engine, 0.0f);
    if (lazar_engine_due)
      CarEngine(ctx, base, game, pad, g_lazar_engine, 0.0f);
    if (gauntlet_engine_due)
      CarEngine(ctx, base, game, pad, g_gauntlet_engine, 0.0f);
    if (hellcat_engine_due)
      CarEngine(ctx, base, game, pad, g_hellcat_engine, 0.0f);
    if (dmc_engine_due)
      CarEngine(ctx, base, game, pad, g_dmc_engine, 0.0f);
    if (bttf1_engine_due)
      CarEngine(ctx, base, game, pad, g_bttf1_engine, 0.0f);
    if (bttf2_engine_due)
      CarEngine(ctx, base, game, pad, g_bttf2_engine, 0.0f);
    if (bttf3_engine_due)
      CarEngine(ctx, base, game, pad, g_bttf3_engine, 0.0f);
  }
  if (jet_due) {
    DrivePad pad;
    {
      std::lock_guard lock(g_drive_mutex);
      pad = g_drive_pad;  // (the jet keeps its own button edges)
    }
    vehicle_mask |= JetControls(ctx, base, game, s, pad);
  }
  if (popup_due && PopupLights(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed), g_popup,
                               g_have_deimos))
    vehicle_mask |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (nexus_popup_due && PopupLights(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed),
                                     g_nexus_popup, g_have_nexus))
    vehicle_mask |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (doors_due)
    FollowDoors(ctx, base, game);
  if (lights_due && CarLights(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed)))
    vehicle_mask |= X_INPUT_GAMEPAD_DPAD_LEFT;
  ApplyHeat(ctx, base, game, s, now);
  g_vehicle_mask.store(vehicle_mask, std::memory_order_relaxed);
  SoundTick();
  if (rain_due)
    RainCars(ctx, base, game);
  if (acted || s.toast_until > now) {
    const auto missing = MissingNatives();
    PublishDraw(s, g_natives.size(), missing.size(), missing);
  }
  g_has_work.store(WantsWork(s), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The overlay (UI thread): draws the published model, never touches the game.
// ---------------------------------------------------------------------------
namespace {
// The DeLorean's time circuits as the films' car shows them: three rows of
// LED read-outs on a dark panel (destination time in red, present time in
// green, last time departed in amber), each with its field names above and
// its name plate below, the colons blinking; above them the digital
// speedometer and the EMPTY lamp. Bottom right, with the circuits on.
void SegDigit(ImDrawList* dl, ImVec2 p, float h, int digit, ImU32 on, ImU32 off, ImU32 glow) {
  static constexpr uint8_t kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const uint8_t bits = digit >= 0 && digit <= 9 ? kSeg[digit] : 0;
  const float w = h * 0.55f, t = h * 0.13f, g = t * 0.6f;
  const float x0 = p.x, x1 = p.x + w, y0 = p.y, ym = p.y + h * 0.5f, y1 = p.y + h;
  const ImVec4 seg[7] = {
      {x0 + t, y0, x1 - t, y0 + t},                      // a
      {x1 - t, y0 + t, x1, ym - t * 0.5f},               // b
      {x1 - t, ym + t * 0.5f, x1, y1 - t},               // c
      {x0 + t, y1 - t, x1 - t, y1},                      // d
      {x0, ym + t * 0.5f, x0 + t, y1 - t},               // e
      {x0, y0 + t, x0 + t, ym - t * 0.5f},               // f
      {x0 + t, ym - t * 0.5f, x1 - t, ym + t * 0.5f}};   // g
  for (int k = 0; k < 7; ++k) {
    const bool lit = (bits >> k) & 1;
    if (lit)
      dl->AddRectFilled(ImVec2(seg[k].x - g, seg[k].y - g), ImVec2(seg[k].z + g, seg[k].w + g), glow, t);
    dl->AddRectFilled(ImVec2(seg[k].x, seg[k].y), ImVec2(seg[k].z, seg[k].w), lit ? on : off, t * 0.35f);
  }
}

void SegNumber(ImDrawList* dl, ImVec2 p, float h, int value, int digits, ImU32 on, ImU32 off, ImU32 glow) {
  for (int k = digits - 1; k >= 0; --k) {
    SegDigit(dl, ImVec2(p.x + float(k) * h * 0.72f, p.y), h, value % 10, on, off, glow);
    value /= 10;
  }
}

// The jet's HUD: its targeting marks over the targets, and a small panel
// (bottom right, clear of the radar and the subtitles): speed, height, g,
// throttle, the gear, the air brake, the autopilot, a stall warning.
void DrawJetHud(ImGuiIO& io) {
  JetHudModel m;
  {
    std::lock_guard lock(g_hud_mutex);
    m = g_hud;
  }
  if (!m.visible)
    return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  const float scale = std::max(0.75f, H / 1080.0f);
  const double t = ImGui::GetTime();
  const ImU32 kWhite = IM_COL32(240, 240, 240, 235), kShadow = IM_COL32(0, 0, 0, 170);
  const ImU32 kAmber = IM_COL32(255, 196, 40, 255), kRed = IM_COL32(255, 45, 35, 255);
  const ImU32 kGreen = IM_COL32(110, 255, 140, 255), kDim = IM_COL32(170, 170, 170, 230);
  // Marks.
  for (const auto& k : m.marks) {
    const ImVec2 c(k.x * W, k.y * H);
    const float dot = (k.state == 2 ? 5.5f : 4.0f) * scale;
    const ImU32 col = k.state == 2 ? kRed : k.state == 1 ? kAmber : kWhite;
    if (k.heli) {  // helicopters: a diamond
      const ImVec2 pts[4] = {ImVec2(c.x, c.y - dot * 1.3f), ImVec2(c.x + dot * 1.3f, c.y), ImVec2(c.x, c.y + dot * 1.3f),
                             ImVec2(c.x - dot * 1.3f, c.y)};
      dl->AddConvexPolyFilled(pts, 4, col);
      dl->AddPolyline(pts, 4, kShadow, ImDrawFlags_Closed, 1.2f * scale);
    } else {
      dl->AddCircleFilled(c, dot, col, 16);
      dl->AddCircle(c, dot, kShadow, 16, 1.2f * scale);
    }
    if (k.state >= 1) {  // brackets closing on it
      const float r = (k.state == 2 ? 15.0f : 15.0f + 26.0f * (1.0f - k.progress)) * scale;
      const float arm = 7.0f * scale, th = 2.2f * scale;
      for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
          const ImVec2 corner(c.x + sx * r, c.y + sy * r);
          dl->AddLine(corner, ImVec2(corner.x - sx * arm, corner.y), col, th);
          dl->AddLine(corner, ImVec2(corner.x, corner.y - sy * arm), col, th);
        }
      if (k.state == 2) {
        const char* text = "LOCK";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f + 1, c.y + r + 3 * scale + 1), kShadow, text);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + r + 3 * scale), kRed, text);
      }
    }
  }
  if (m.marks_only)
    return;
  // The panel.
  ImFont* font = ImGui::GetFont();
  const float fs = 22.0f * scale, small = 17.0f * scale, pad = 12.0f * scale;
  const float pw = 380.0f * scale, rows = m.autopilot.empty() && !m.stalled ? 3.0f : 4.0f;
  const float ph = pad * 2 + fs * 1.25f * rows;
  const ImVec2 o(W - pw - 48.0f * scale, H - ph - 150.0f * scale);
  dl->AddRectFilled(o, ImVec2(o.x + pw, o.y + ph), IM_COL32(10, 12, 14, 170), 8.0f * scale);
  dl->AddRect(o, ImVec2(o.x + pw, o.y + ph), IM_COL32(255, 159, 14, 110), 8.0f * scale, 0, 1.5f);
  float y = o.y + pad;
  auto text = [&](float x, float size, ImU32 col, const std::string& str) {
    dl->AddText(font, size, ImVec2(x + 1, y + 1), kShadow, str.c_str());
    dl->AddText(font, size, ImVec2(x, y), col, str.c_str());
  };
  auto right = [&](float size, ImU32 col, const std::string& str) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, str.c_str());
    text(o.x + pw - pad - ts.x, size, col, str);
  };
  text(o.x + pad, fs, kAmber, m.label);
  if (m.has_gear)
    right(small, m.gear == 1 ? kAmber : kDim, m.gear == 0 ? "GEAR DOWN" : m.gear == 2 ? "GEAR UP" : "GEAR MOVING");
  y += fs * 1.25f;
  char line[96];
  std::snprintf(line, sizeof line, "%d MPH", int(std::lround(m.kmh / 1.609344f)));
  text(o.x + pad, fs, kWhite, line);
  std::snprintf(line, sizeof line, "ALT %d FT", int(std::lround(m.height * 3.28084f)));
  text(o.x + pad + 120.0f * scale, fs, kWhite, line);
  if (!m.on_ground) {
    std::snprintf(line, sizeof line, "%.1f G", m.g);
    right(fs, m.g > 6.0f ? kAmber : kWhite, line);
  }
  y += fs * 1.25f;
  {  // throttle bar
    const float bx = o.x + pad, bw = 150.0f * scale, bh = fs * 0.62f, by = y + fs * 0.22f;
    dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), kDim, 2.0f * scale);
    dl->AddRectFilled(ImVec2(bx + 2, by + 2), ImVec2(bx + 2 + (bw - 4) * std::clamp(m.throttle, 0.0f, 1.0f), by + bh - 2),
                      m.burner ? kAmber : kGreen, 1.5f * scale);
    std::snprintf(line, sizeof line, "%d%%", int(std::lround(m.throttle * 100.0f)));
    text(bx + bw + 8.0f * scale, small, kWhite, line);
    std::string flags;
    if (m.burner)
      flags += "AB ";
    if (m.airbrake)
      flags += "BRAKE";
    if (!flags.empty())
      right(small, kAmber, flags);
  }
  y += fs * 1.25f;
  if (m.stalled) {
    if (std::fmod(t, 0.6) < 0.38)
      text(o.x + pad, fs, kRed, "STALL");
  } else if (!m.autopilot.empty()) {
    text(o.x + pad, small, kGreen, m.autopilot);
  }
}

void DrawTcd(ImGuiIO& io) {
  TcdModel t;
  {
    std::lock_guard lock(g_tcd_mutex);
    t = g_tcd;
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  if (t.veil > 0.0f)
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, int(255 * std::min(1.0f, t.veil))));
  if (t.flash > 0.0f)
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(235, 245, 255, int(230 * std::min(1.0f, t.flash))));
  if (!t.visible)
    return;
  ImFont* font = ImGui::GetFont();
  const float scale = std::max(0.75f, io.DisplaySize.y / 1080.0f);
  const float h = 26.0f * scale;             // a digit's height
  const float dw = h * 0.72f;                // a digit's pitch
  const float small = 11.0f * scale, gap = h * 0.45f;
  const float month_w = h * 2.0f, day_w = 2 * dw, year_w = 4 * dw, ampm_w = h * 0.9f, hour_w = 2 * dw,
              colon_w = h * 0.35f, min_w = 2 * dw;
  const float row_w = month_w + day_w + year_w + ampm_w + hour_w + colon_w + min_w + 6 * gap;
  const float row_h = small * 1.4f + h + small * 1.9f + h * 0.35f;
  const float pad = h * 0.45f;
  const float panel_w = row_w + 2 * pad, panel_h = 3 * row_h + 2 * pad;
  const ImVec2 origin(io.DisplaySize.x - panel_w - 36.0f * scale, io.DisplaySize.y - panel_h - 40.0f * scale);
  // the speedometer and the reactor's lamp above the panel
  {
    const float sh = h * 1.25f, sw = 2 * sh * 0.72f;
    const ImVec2 box(origin.x + panel_w - sw - pad * 2 - h * 1.6f, origin.y - sh - pad * 2.2f);
    dl->AddRectFilled(box, ImVec2(origin.x + panel_w, origin.y - pad * 0.4f), IM_COL32(16, 16, 18, 235), 4.0f * scale);
    dl->AddRect(box, ImVec2(origin.x + panel_w, origin.y - pad * 0.4f), IM_COL32(90, 90, 96, 255), 4.0f * scale, 0,
                1.5f * scale);
    SegNumber(dl, ImVec2(box.x + pad, box.y + pad * 0.9f), sh, std::clamp(t.mph, 0, 99), 2,
              IM_COL32(255, 40, 30, 255), IM_COL32(255, 40, 30, 28), IM_COL32(255, 40, 30, 60));
    dl->AddText(font, small * 1.3f, ImVec2(box.x + pad + sw + 6.0f * scale, box.y + pad * 0.9f + sh - small * 1.4f),
                IM_COL32(255, 60, 40, 255), "MPH");
    // EMPTY (the reactor spent), a lamp left of the speedometer
    const ImVec2 lamp(box.x - h * 2.9f, box.y + pad * 0.6f), lamp2(box.x - h * 0.3f, box.y + pad * 0.6f + h * 0.9f);
    dl->AddRectFilled(lamp, lamp2, t.empty ? IM_COL32(255, 30, 20, 255) : IM_COL32(70, 18, 14, 240), 3.0f * scale);
    dl->AddText(font, small * 1.25f, ImVec2(lamp.x + h * 0.38f, lamp.y + h * 0.22f),
                t.empty ? IM_COL32(255, 235, 225, 255) : IM_COL32(140, 70, 60, 255), "EMPTY");
    if (t.hover)
      dl->AddText(font, small * 1.2f, ImVec2(lamp.x, lamp2.y + 4.0f * scale), IM_COL32(120, 255, 150, 255), "FLIGHT");
  }
  dl->AddRectFilled(origin, ImVec2(origin.x + panel_w, origin.y + panel_h), IM_COL32(28, 28, 31, 238), 6.0f * scale);
  dl->AddRect(origin, ImVec2(origin.x + panel_w, origin.y + panel_h), IM_COL32(110, 110, 118, 255), 6.0f * scale, 0,
              2.0f * scale);
  const bool colon = (NowMs() / 500) % 2 == 0;
  struct Row {
    const TcdDate* d;
    const char* name;
    ImU32 on, plate;
  };
  const Row rows[3] = {{&t.dest, "DESTINATION TIME", IM_COL32(255, 45, 32, 255), IM_COL32(150, 25, 20, 255)},
                       {&t.present, "PRESENT TIME", IM_COL32(60, 255, 95, 255), IM_COL32(25, 110, 40, 255)},
                       {&t.last, "LAST TIME DEPARTED", IM_COL32(255, 190, 40, 255), IM_COL32(140, 95, 15, 255)}};
  const ImU32 label = IM_COL32(170, 170, 176, 255);
  for (int r = 0; r < 3; ++r) {
    const auto& row = rows[r];
    const TcdDate& d = *row.d;
    const ImU32 off = (row.on & 0x00FFFFFF) | (26u << 24), glow = (row.on & 0x00FFFFFF) | (55u << 24);
    float x = origin.x + pad;
    const float y_label = origin.y + pad + float(r) * row_h, y = y_label + small * 1.4f;
    auto field = [&](float w, const char* name) {
      dl->AddRectFilled(ImVec2(x - 3 * scale, y - 3 * scale), ImVec2(x + w + 3 * scale, y + h + 3 * scale),
                        IM_COL32(6, 6, 7, 255), 2.0f * scale);
      const ImVec2 ts = font->CalcTextSizeA(small, FLT_MAX, 0.0f, name);
      dl->AddText(font, small, ImVec2(x + (w - ts.x) * 0.5f, y_label), label, name);
    };
    field(month_w, "MONTH");
    const char* mon = kMonths[std::clamp(d.month, 1, 12) - 1];
    dl->AddText(font, h * 1.05f, ImVec2(x + h * 0.05f, y - h * 0.04f), glow, mon);
    dl->AddText(font, h, ImVec2(x + h * 0.08f, y), row.on, mon);
    x += month_w + gap;
    field(day_w, "DAY");
    SegNumber(dl, ImVec2(x + h * 0.08f, y), h, d.day, 2, row.on, off, glow);
    x += day_w + gap;
    field(year_w, "YEAR");
    SegNumber(dl, ImVec2(x + h * 0.08f, y), h, d.year, 4, row.on, off, glow);
    x += year_w + gap;
    // AM / PM lamps
    {
      const bool pm = d.hour >= 12;
      const ImVec2 ts = font->CalcTextSizeA(small, FLT_MAX, 0.0f, "AM");
      dl->AddText(font, small, ImVec2(x + (ampm_w - ts.x) * 0.5f, y_label), label, "AM");
      dl->AddCircleFilled(ImVec2(x + ampm_w * 0.5f, y + h * 0.22f), h * 0.13f, pm ? off : row.on);
      dl->AddText(font, small, ImVec2(x + (ampm_w - ts.x) * 0.5f, y + h * 0.42f), label, "PM");
      dl->AddCircleFilled(ImVec2(x + ampm_w * 0.5f, y + h * 0.88f), h * 0.13f, pm ? row.on : off);
    }
    x += ampm_w + gap;
    field(hour_w, "HOUR");
    SegNumber(dl, ImVec2(x + h * 0.08f, y), h, (d.hour + 11) % 12 + 1, 2, row.on, off, glow);
    x += hour_w + gap * 0.5f;
    const ImU32 dot = colon ? row.on : off;
    dl->AddCircleFilled(ImVec2(x + colon_w * 0.5f, y + h * 0.3f), h * 0.08f, dot);
    dl->AddCircleFilled(ImVec2(x + colon_w * 0.5f, y + h * 0.72f), h * 0.08f, dot);
    x += colon_w + gap * 0.5f;
    field(min_w, "MIN");
    SegNumber(dl, ImVec2(x + h * 0.08f, y), h, d.minute, 2, row.on, off, glow);
    // the row's name plate
    const ImVec2 ts = font->CalcTextSizeA(small * 1.15f, FLT_MAX, 0.0f, row.name);
    const float py = y + h + small * 0.45f, px = origin.x + (panel_w - ts.x) * 0.5f;
    dl->AddRectFilled(ImVec2(px - 10 * scale, py - 2 * scale), ImVec2(px + ts.x + 10 * scale, py + ts.y + 2 * scale),
                      row.plate, 3.0f * scale);
    dl->AddText(font, small * 1.15f, ImVec2(px, py), IM_COL32(240, 240, 240, 255), row.name);
  }
}

// ---------------------------------------------------------------------------
// v1.7.24: the menu as a glass panel switcher, after the user's own Ruffle5
// (a bar of sections switched with L1/R1, glass cards, the focused row
// lifted inside a ring with a light running round it, a row of button
// hints), in Liberty's colours rather than Ruffle's blue and orange: the
// Statue of Happiness's weathered copper green, and old gold for the light.
// Drawn on the foreground list (ImGui has no blur: the glass is layered
// tints, a sheen and a hairline edge).
// ---------------------------------------------------------------------------
namespace glass {
struct Rgb {
  int r, g, b;
};
constexpr Rgb kPatina{86, 214, 188}, kPatinaDeep{38, 128, 112}, kGold{238, 212, 140}, kInk{8, 22, 20},
    kText{232, 242, 238}, kDim{140, 162, 158}, kGlassTop{22, 38, 37}, kGlassBottom{8, 14, 15};
inline ImU32 C(Rgb c, float a) { return IM_COL32(c.r, c.g, c.b, int(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)); }
inline float Ease(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
inline float Breathe(double t, float period) { return 0.5f - 0.5f * std::cos(float(t) * 6.2831853f / period); }

// A soft shadow under a rounded box: widening, fading layers.
void Shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float s, float alpha) {
  for (int i = 7; i >= 1; --i) {
    const float g = float(i) * 3.5f * s;
    dl->AddRectFilled(ImVec2(a.x - g * 0.6f, a.y - g * 0.3f + 12.0f * s), ImVec2(b.x + g * 0.6f, b.y + g + 12.0f * s),
                      IM_COL32(0, 0, 0, int(alpha * 26.0f)), r + g);
  }
}
// A glow around a rounded box (the focused row's halo).
void Glow(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float s, Rgb c, float alpha) {
  for (int i = 6; i >= 1; --i) {
    const float g = float(i) * 2.6f * s;
    dl->AddRectFilled(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g), C(c, alpha * 0.06f * float(7 - i) / 6.0f),
                      r + g);
  }
}
// The glass itself: a tint darkening downwards, a sheen over its top third,
// a hairline edge and a lit top edge.
void Pane(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float s, float alpha) {
  const float h = b.y - a.y;
  const ImU32 top = C(kGlassTop, 0.80f * alpha), bottom = C(kGlassBottom, 0.88f * alpha);
  dl->AddRectFilled(a, ImVec2(b.x, a.y + r), top, r, ImDrawFlags_RoundCornersTop);
  dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + r), ImVec2(b.x, b.y - r), top, top, bottom, bottom);
  dl->AddRectFilled(ImVec2(a.x, b.y - r), b, bottom, r, ImDrawFlags_RoundCornersBottom);
  dl->AddRectFilledMultiColor(ImVec2(a.x + r * 0.7f, a.y + 2.0f * s), ImVec2(b.x - r * 0.7f, a.y + h * 0.34f),
                              IM_COL32(255, 255, 255, int(24 * alpha)), IM_COL32(255, 255, 255, int(24 * alpha)),
                              IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
  dl->AddRect(a, b, C(kPatina, 0.32f * alpha), r, 0, 1.5f * s);
  dl->AddLine(ImVec2(a.x + r, a.y + 1.5f * s), ImVec2(b.x - r, a.y + 1.5f * s), IM_COL32(255, 255, 255, int(56 * alpha)),
              1.0f * s);
}
// The point `d` along a rounded box's outline, clockwise from its top-left.
ImVec2 Perimeter(ImVec2 a, ImVec2 b, float r, float d) {
  const float w = b.x - a.x, h = b.y - a.y;
  r = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
  const float sw = w - 2 * r, sh = h - 2 * r, arc = 1.5707963f * r;
  const float total = 2 * (sw + sh) + 4 * arc;
  d = std::fmod(d, total);
  if (d < 0)
    d += total;
  const ImVec2 corners[4] = {ImVec2(b.x - r, a.y + r), ImVec2(b.x - r, b.y - r), ImVec2(a.x + r, b.y - r),
                             ImVec2(a.x + r, a.y + r)};
  const float start[4] = {-1.5707963f, 0.0f, 1.5707963f, 3.1415927f};
  const ImVec2 from[4] = {ImVec2(a.x + r, a.y), ImVec2(b.x, a.y + r), ImVec2(b.x - r, b.y), ImVec2(a.x, b.y - r)};
  const ImVec2 dir[4] = {ImVec2(1, 0), ImVec2(0, 1), ImVec2(-1, 0), ImVec2(0, -1)};
  const float len[4] = {sw, sh, sw, sh};
  for (int i = 0; i < 4; ++i) {
    if (d <= len[i])
      return ImVec2(from[i].x + dir[i].x * d, from[i].y + dir[i].y * d);
    d -= len[i];
    if (d <= arc) {
      const float t = start[i] + (r > 0 ? d / r : 0.0f);
      return ImVec2(corners[i].x + r * std::cos(t), corners[i].y + r * std::sin(t));
    }
    d -= arc;
  }
  return a;
}
// A light running round a ring: a bright head, a fading tail (a lap in 3.2 s).
void Comet(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float s, double t, float alpha) {
  const float w = b.x - a.x, h = b.y - a.y;
  const float total = 2 * (w + h) - (8 - 2 * 3.1415927f) * std::min(r, std::min(w, h) * 0.5f);
  const float head = float(std::fmod(t / 3.2, 1.0)) * total;
  const float tail = std::min(total * 0.22f, 220.0f * s);
  constexpr int kDots = 26;
  for (int i = kDots - 1; i >= 0; --i) {
    const float k = float(i) / kDots;
    const ImVec2 p = Perimeter(a, b, r, head - k * tail);
    const float fade = (1 - k) * (1 - k);
    dl->AddCircleFilled(p, (1.2f + 1.6f * (1 - k)) * s, C(kGold, alpha * fade), 10);
  }
  const ImVec2 p = Perimeter(a, b, r, head);
  dl->AddCircleFilled(p, 6.0f * s, C(kGold, 0.18f * alpha), 16);
  dl->AddCircleFilled(p, 3.2f * s, IM_COL32(255, 250, 230, int(235 * alpha)), 12);
}
// A pad button's glyph: the PlayStation symbols on a dark disc, or a shoulder
// button's name on a small key. Returns its width.
float Glyph(ImDrawList* dl, ImFont* font, float size, ImVec2 at, const char* which, float alpha) {
  const float r = size * 0.55f;
  const ImVec2 c(at.x + r, at.y + size * 0.5f);
  const std::string_view w(which);
  if (w == "cross" || w == "circle" || w == "square" || w == "triangle") {
    dl->AddCircleFilled(c, r, IM_COL32(12, 16, 18, int(220 * alpha)), 20);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, int(40 * alpha)), 20, 1.0f);
    const float k = r * 0.45f, th = std::max(1.5f, size * 0.09f);
    if (w == "cross") {
      const ImU32 col = IM_COL32(127, 168, 255, int(255 * alpha));
      dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, th);
      dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, th);
    } else if (w == "circle") {
      dl->AddCircle(c, k * 1.1f, IM_COL32(255, 111, 138, int(255 * alpha)), 18, th);
    } else if (w == "square") {
      dl->AddRect(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), IM_COL32(244, 154, 209, int(255 * alpha)), 0.0f,
                  0, th);
    } else {
      const ImVec2 p[3] = {ImVec2(c.x, c.y - k * 1.1f), ImVec2(c.x + k * 1.05f, c.y + k * 0.75f),
                           ImVec2(c.x - k * 1.05f, c.y + k * 0.75f)};
      dl->AddPolyline(p, 3, IM_COL32(79, 216, 177, int(255 * alpha)), ImDrawFlags_Closed, th);
    }
    return 2 * r;
  }
  const float ts = size * 0.62f;
  const ImVec2 ext = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, which);
  const float kw = ext.x + size * 0.5f;
  dl->AddRectFilled(ImVec2(at.x, at.y + size * 0.08f), ImVec2(at.x + kw, at.y + size * 0.92f),
                    IM_COL32(12, 16, 18, int(220 * alpha)), size * 0.2f);
  dl->AddRect(ImVec2(at.x, at.y + size * 0.08f), ImVec2(at.x + kw, at.y + size * 0.92f),
              IM_COL32(255, 255, 255, int(48 * alpha)), size * 0.2f, 0, 1.0f);
  dl->AddText(font, ts, ImVec2(at.x + (kw - ext.x) * 0.5f, at.y + (size - ext.y) * 0.5f),
              C(kText, 0.92f * alpha), which);
  return kw;
}
// A switch: on, the track lit and the knob right.
void Switch(ImDrawList* dl, ImVec2 at, float s, bool on, float alpha, bool focused) {
  const float w = 46.0f * s, h = 24.0f * s;
  const ImVec2 a(at.x - w, at.y - h * 0.5f), b(at.x, at.y + h * 0.5f);
  dl->AddRectFilled(a, b, on ? C(focused ? kInk : kPatina, focused ? 0.55f * alpha : 0.85f * alpha)
                             : IM_COL32(255, 255, 255, int((focused ? 40 : 28) * alpha)),
                    h * 0.5f);
  dl->AddRect(a, b, IM_COL32(255, 255, 255, int(50 * alpha)), h * 0.5f, 0, 1.0f);
  const float kx = on ? b.x - h * 0.5f : a.x + h * 0.5f;
  dl->AddCircleFilled(ImVec2(kx, at.y), h * 0.38f, IM_COL32(250, 252, 250, int(245 * alpha)), 18);
}
}  // namespace glass

struct GlassAnim {
  int serial = -1;
  double switched = -10.0;
  int slide = 0;
};
GlassAnim g_glass;

void DrawGlassMenu(ImGuiIO& io, const DrawModel& model) {
  using namespace glass;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  // The hub's type when it is there (Oswald for the name and the tabs,
  // Inter for the rows), else ImGui's own.
  ImFont *f_head = font, *f_tab = font, *f_body = font, *f_bold = font;
#if REX_PLATFORM_PS5 && defined(__PROSPERO__)
  {
    const auto& hf = gta4ps5::hub::Fonts();
    if (hf.display) f_head = hf.display;
    if (hf.nav) f_tab = hf.nav;
    if (hf.body) f_body = hf.body;
    if (hf.body_bold) f_bold = hf.body_bold;
  }
#endif
  const float s = std::max(0.75f, io.DisplaySize.y / 1080.0f);
  const double t = ImGui::GetTime();
  const float body = 23.0f * s, small = 16.0f * s, tiny = 13.5f * s;
  const float pad = 22.0f * s, radius = 22.0f * s, width = 580.0f * s;
  const ImVec2 origin(56.0f * s, 92.0f * s);
  // The panel's layout, top to bottom.
  const float header_h = 44.0f * s, tabs_h = 40.0f * s, title_h = 34.0f * s, row_h = 46.0f * s,
              hints_h = 40.0f * s;
  if (model.page_serial != g_glass.serial) {
    if (g_glass.serial >= 0)
      g_glass.switched = t;
    g_glass.serial = model.page_serial;
    g_glass.slide = model.slide;
  }
  const float enter = Ease(float((t - g_glass.switched) / 0.24));
  float panel_h = 0.0f;
  if (model.open) {
    panel_h = pad + header_h + tabs_h + 10.0f * s + title_h + float(model.lines.size()) * row_h + 8.0f * s + hints_h +
              pad * 0.6f;
    const ImVec2 a = origin, b(origin.x + width, origin.y + panel_h);
    Shadow(dl, a, b, radius, s, 1.0f);
    Pane(dl, a, b, radius, s, 1.0f);
    float y = a.y + pad;
    // ---- The header: the name, the credit.
    {
      const char* name = "LIBERTYRECOMP";
      const float hs = body * 1.3f;
      const ImVec2 n = f_head->CalcTextSizeA(hs, FLT_MAX, 0.0f, name);
      dl->AddText(f_head, hs, ImVec2(a.x + pad, y - 4.0f * s), C(kPatina, 1.0f), name);
      dl->AddText(f_tab, hs, ImVec2(a.x + pad + n.x + 9.0f * s, y - 4.0f * s), C(kText, 0.92f), "MENU");
      const ImVec2 cr = f_body->CalcTextSizeA(tiny, FLT_MAX, 0.0f, kMenuCredit);
      dl->AddText(f_body, tiny, ImVec2(b.x - pad - cr.x, y - 4.0f * s + (n.y - cr.y) * 0.5f), C(kDim, 0.95f),
                  kMenuCredit);
      y += header_h;
    }
    // ---- The sections: L1, the tabs (the shown one lit, the strip
    // scrolled to keep it in view), R1.
    {
      const float gw = Glyph(dl, f_tab, small * 1.25f, ImVec2(a.x + pad, y + (tabs_h - small * 1.25f) * 0.5f - 2 * s),
                             "L1", 1.0f);
      const float rw_est = gw;
      const float left = a.x + pad + gw + 10.0f * s, right = b.x - pad - rw_est - 10.0f * s;
      Glyph(dl, f_tab, small * 1.25f, ImVec2(b.x - pad - rw_est, y + (tabs_h - small * 1.25f) * 0.5f - 2 * s), "R1",
            1.0f);
      std::vector<float> xs, ws;
      float x = 0.0f;
      for (const auto& tab : model.tabs) {
        const float w = f_tab->CalcTextSizeA(small * 1.08f, FLT_MAX, 0.0f, tab.c_str()).x + 22.0f * s;
        xs.push_back(x);
        ws.push_back(w);
        x += w + 4.0f * s;
      }
      const int cur = std::clamp(model.tab, 0, std::max(0, int(model.tabs.size()) - 1));
      float scroll = 0.0f;
      if (!xs.empty()) {
        const float span = right - left;
        const float want = xs[size_t(cur)] + ws[size_t(cur)] * 0.5f - span * 0.5f;
        scroll = std::clamp(want, 0.0f, std::max(0.0f, x - 4.0f * s - span));
      }
      dl->PushClipRect(ImVec2(left, y), ImVec2(right, y + tabs_h), true);
      const float pill_h = tabs_h - 10.0f * s, py = y + 3.0f * s;
      for (size_t i = 0; i < model.tabs.size(); ++i) {
        const float px = left + xs[i] - scroll;
        const bool on = int(i) == cur;
        if (on) {
          Glow(dl, ImVec2(px, py), ImVec2(px + ws[i], py + pill_h), pill_h * 0.5f, s, kPatina,
               0.6f + 0.4f * Breathe(t, 2.4f));
          dl->AddRectFilled(ImVec2(px, py), ImVec2(px + ws[i], py + pill_h), C(kPatina, 0.95f), pill_h * 0.5f);
          dl->AddRectFilledMultiColor(ImVec2(px + pill_h * 0.4f, py + 1.0f * s),
                                      ImVec2(px + ws[i] - pill_h * 0.4f, py + pill_h * 0.5f),
                                      IM_COL32(255, 255, 255, 60), IM_COL32(255, 255, 255, 60),
                                      IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
        }
        const ImVec2 ts = f_tab->CalcTextSizeA(small * 1.08f, FLT_MAX, 0.0f, model.tabs[i].c_str());
        dl->AddText(f_tab, small * 1.08f, ImVec2(px + (ws[i] - ts.x) * 0.5f, py + (pill_h - ts.y) * 0.5f),
                    on ? C(kInk, 1.0f) : C(kDim, 0.95f), model.tabs[i].c_str());
      }
      dl->PopClipRect();
      // (the strip fades out at its ends, where tabs go on past them)
      const float fade = 26.0f * s;
      const ImU32 glass_on = C(kGlassTop, 0.95f), glass_off = C(kGlassTop, 0.0f);
      if (scroll > 0.5f)
        dl->AddRectFilledMultiColor(ImVec2(left, y), ImVec2(left + fade, y + tabs_h), glass_on, glass_off, glass_off,
                                    glass_on);
      if (x - 4.0f * s - scroll > right - left + 0.5f)
        dl->AddRectFilledMultiColor(ImVec2(right - fade, y), ImVec2(right, y + tabs_h), glass_off, glass_on, glass_on,
                                    glass_off);
      y += tabs_h;
      dl->AddLine(ImVec2(a.x + pad, y + 2.0f * s), ImVec2(b.x - pad, y + 2.0f * s), C(kPatina, 0.22f), 1.0f);
      y += 10.0f * s;
    }
    // ---- The page: it slides in from the side L1/R1 went to (or rises).
    const float alpha = 0.25f + 0.75f * enter;
    const float dx = float(g_glass.slide) * (1.0f - enter) * 70.0f * s;
    const float dy = g_glass.slide == 0 ? (1.0f - enter) * 12.0f * s : 0.0f;
    const float content_top = y;
    dl->PushClipRect(ImVec2(a.x + 4.0f * s, content_top), ImVec2(b.x - 4.0f * s, content_top + title_h +
                                                                                     float(model.lines.size()) * row_h + 6.0f * s),
                     true);
    {
      const ImVec2 ts = f_tab->CalcTextSizeA(small * 1.1f, FLT_MAX, 0.0f, model.title.c_str());
      dl->AddText(f_tab, small * 1.1f, ImVec2(a.x + pad + dx, y + dy + (title_h - ts.y) * 0.5f), C(kPatina, 0.85f * alpha),
                  model.title.c_str());
      if (model.total > int(model.lines.size())) {
        int sel = model.first;
        for (size_t i = 0; i < model.lines.size(); ++i)
          if (model.lines[i].selected)
            sel = model.first + int(i);
        const std::string count = std::to_string(sel + 1) + " / " + std::to_string(model.total);
        const ImVec2 cs = f_body->CalcTextSizeA(small, FLT_MAX, 0.0f, count.c_str());
        dl->AddText(f_body, small, ImVec2(b.x - pad - cs.x + dx, y + dy + (title_h - cs.y) * 0.5f),
                    C(kDim, 0.9f * alpha), count.c_str());
      }
      y += title_h;
    }
    for (const auto& line : model.lines) {
      const ImVec2 ra(a.x + 14.0f * s + dx, y + dy + 3.0f * s), rb(b.x - 14.0f * s + dx, y + dy + row_h - 3.0f * s);
      const float rr = 12.0f * s;
      // Its label and what it is set to: "Label: ON" a switch, "Label: value" the value at the right.
      std::string label = line.text, value;
      int toggle = -1;
      const size_t colon = line.text.rfind(": ");
      if (colon != std::string::npos && colon > 0) {
        std::string v = line.text.substr(colon + 2);
        if (v.rfind("ON", 0) == 0 || v.rfind("OFF", 0) == 0) {
          toggle = v.rfind("ON", 0) == 0 ? 1 : 0;
          label = line.text.substr(0, colon);
          if (v.size() > (toggle ? 2u : 3u))  // "ON (L1 missile...)": the rest stays with the label
            label += " " + v.substr(toggle ? 2 : 3);
        } else if (v.size() <= 24) {
          label = line.text.substr(0, colon);
          value = v;
        }
      }
      const float text_y = y + dy + (row_h - body) * 0.5f;
      const float value_right = rb.x - 14.0f * s;
      if (line.selected) {
        Glow(dl, ra, rb, rr, s, kPatina, (0.7f + 0.3f * Breathe(t, 2.4f)) * alpha);
        dl->AddRectFilled(ra, rb, C(kPatina, 0.93f * alpha), rr);
        dl->AddRectFilledMultiColor(ImVec2(ra.x + rr * 0.6f, ra.y + 1.0f * s), ImVec2(rb.x - rr * 0.6f, ra.y + (rb.y - ra.y) * 0.5f),
                                    IM_COL32(255, 255, 255, int(70 * alpha)), IM_COL32(255, 255, 255, int(70 * alpha)),
                                    IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
        const ImVec2 ring_a(ra.x - 4.0f * s, ra.y - 4.0f * s), ring_b(rb.x + 4.0f * s, rb.y + 4.0f * s);
        dl->AddRect(ring_a, ring_b, C(kPatina, (0.55f + 0.3f * Breathe(t, 2.4f)) * alpha), rr + 4.0f * s, 0, 2.0f * s);
        Comet(dl, ring_a, ring_b, rr + 4.0f * s, s, t, alpha);
      }
      const ImU32 ink = line.selected ? C(kInk, alpha) : line.dim ? C(kDim, alpha) : C(kText, alpha);
      float label_right = value_right;
      if (toggle >= 0) {
        Switch(dl, ImVec2(value_right, y + dy + row_h * 0.5f), s, toggle == 1, alpha, line.selected);
        label_right = value_right - 58.0f * s;
      } else if (!value.empty()) {
        const std::string shown = line.selected ? "\xE2\x80\xB9 " + value + " \xE2\x80\xBA" : value;
        const char* v = line.selected ? value.c_str() : shown.c_str();
        const ImVec2 vs = f_body->CalcTextSizeA(body, FLT_MAX, 0.0f, v);
        if (line.selected) {  // the arrows drawn as small chevrons (the font may lack the marks)
          const float cx0 = value_right - vs.x - 18.0f * s, cx1 = value_right - 2.0f * s, cy = y + dy + row_h * 0.5f;
          const float k = 5.0f * s;
          dl->AddLine(ImVec2(cx0 + k, cy - k), ImVec2(cx0, cy), C(kInk, 0.8f * alpha), 2.0f * s);
          dl->AddLine(ImVec2(cx0, cy), ImVec2(cx0 + k, cy + k), C(kInk, 0.8f * alpha), 2.0f * s);
          dl->AddLine(ImVec2(cx1 - k, cy - k), ImVec2(cx1, cy), C(kInk, 0.8f * alpha), 2.0f * s);
          dl->AddLine(ImVec2(cx1, cy), ImVec2(cx1 - k, cy + k), C(kInk, 0.8f * alpha), 2.0f * s);
          dl->AddText(f_bold, body, ImVec2(value_right - vs.x - 9.0f * s, text_y), ink, v);
          label_right = value_right - vs.x - 30.0f * s;
        } else {
          dl->AddText(f_body, body, ImVec2(value_right - vs.x, text_y), C(kPatina, 0.9f * alpha), v);
          label_right = value_right - vs.x - 12.0f * s;
        }
      }
      dl->PushClipRect(ImVec2(ra.x, ra.y), ImVec2(std::max(ra.x + 10.0f, label_right), rb.y), true);
      dl->AddText(line.selected ? f_bold : f_body, body, ImVec2(ra.x + 16.0f * s, text_y), ink, label.c_str());
      dl->PopClipRect();
      y += row_h;
    }
    dl->PopClipRect();
    // A long page's place in it: a thin bar at the right.
    if (model.total > int(model.lines.size()) && model.total > 0) {
      const float top = content_top + title_h, bottom = top + float(model.lines.size()) * row_h;
      const float h = bottom - top;
      const float th = std::max(18.0f * s, h * float(model.lines.size()) / float(model.total));
      const float ty = top + (h - th) * float(model.first) / float(std::max(1, model.total - int(model.lines.size())));
      dl->AddRectFilled(ImVec2(b.x - 7.0f * s, top), ImVec2(b.x - 4.0f * s, bottom), IM_COL32(255, 255, 255, 18), 2.0f * s);
      dl->AddRectFilled(ImVec2(b.x - 7.0f * s, ty), ImVec2(b.x - 4.0f * s, ty + th), C(kPatina, 0.8f), 2.0f * s);
    }
    y += 8.0f * s;
    // ---- The hints.
    {
      dl->AddLine(ImVec2(a.x + pad, y), ImVec2(b.x - pad, y), C(kPatina, 0.18f), 1.0f);
      float x = a.x + pad;
      const float gy = y + (hints_h - small * 1.3f) * 0.5f;
      auto hint = [&](const char* glyph, const char* words) {
        x += Glyph(dl, f_tab, small * 1.3f, ImVec2(x, gy), glyph, 1.0f) + 7.0f * s;
        const ImVec2 ws = f_body->CalcTextSizeA(small, FLT_MAX, 0.0f, words);
        dl->AddText(f_body, small, ImVec2(x, gy + (small * 1.3f - ws.y) * 0.5f), C(kDim, 1.0f), words);
        x += ws.x + 18.0f * s;
      };
      const bool home = model.tab == 0 && model.title == "HOME";
      hint("cross", "Select");
      hint("circle", home ? "Close" : "Back");
      hint("L1", "");
      x -= 22.0f * s;
      hint("R1", "Sections");
      if (!home)
        hint("L3+R3", "Close");
    }
  }
  // ---- A toast: a glass pill under the panel (or where it would be),
  // wrapped to its width.
  if (!model.toast.empty()) {
    const float wrap = width - 2 * pad;
    const ImVec2 ts = f_body->CalcTextSizeA(small * 1.15f, FLT_MAX, wrap, model.toast.c_str());
    const ImVec2 a(origin.x, origin.y + (model.open ? panel_h + 12.0f * s : 0.0f));
    const ImVec2 b(a.x + std::min(width, ts.x + 2 * pad), a.y + ts.y + 22.0f * s);
    Shadow(dl, a, b, 16.0f * s, s, 0.6f);
    Pane(dl, a, b, 16.0f * s, s, 1.0f);
    dl->AddRectFilled(ImVec2(a.x + 9.0f * s, a.y + 10.0f * s), ImVec2(a.x + 12.0f * s, b.y - 10.0f * s), C(kGold, 0.9f),
                      1.5f * s);
    dl->AddText(f_body, small * 1.15f, ImVec2(a.x + pad, a.y + 11.0f * s), C(kText, 1.0f), model.toast.c_str(), nullptr,
                wrap);
  }
}

class TrainerOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit TrainerOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  // (the HUD's marks follow their targets; the time circuits' display blinks
  // and the flash fades: drawn every frame then)
  bool WantsContinuousRepaint() const override {
    return g_tcd_active.load(std::memory_order_relaxed) || g_hud_active.load(std::memory_order_relaxed) ||
           g_menu_visible.load(std::memory_order_relaxed);
  }
  void OnDraw(ImGuiIO& io) override {
    if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f)
      g_display_aspect.store(io.DisplaySize.x / io.DisplaySize.y, std::memory_order_relaxed);
    if (g_hud_active.load(std::memory_order_relaxed))
      DrawJetHud(io);
    if (g_tcd_active.load(std::memory_order_relaxed))
      DrawTcd(io);
    DrawModel model;
    {
      std::lock_guard lock(g_draw_mutex);
      model = g_draw;
    }
    if (!model.open && model.toast.empty())
      return;
    DrawGlassMenu(io, model);
  }
};
}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateOverlay(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<TrainerOverlay>(drawer);
}

}  // namespace gta4::trainer

extern "C" bool GTA4_TrainerOwnsGuestPad() { return gta4::trainer::InputOwned(); }

// With the menu open the game sees a cleared pad, except the throttle (R2)
// while the player is in a vehicle, so a car keeps going (xam_input.cpp).
extern "C" bool GTA4_TrainerPassesThrottle() {
  return gta4::trainer::InputOwned() &&
         gta4::trainer::g_in_vehicle.load(std::memory_order_relaxed);
}

extern "C" uint32_t GTA4_TrainerPadButtonAdjust() {
  return gta4::trainer::g_pad_adjust.load(std::memory_order_relaxed);
}

// The script-thread run function: its r3 is the thread about to run. The
// trainer acts right after the thread ran, with the VM's executing-thread
// global set the way the VM sets it while scripts call natives, so entity
// creation attaches to a real script thread (the earlier spawn crash was
// CREATE_CAR reading this global as null from a non-script hook).
extern "C" void sub_82844200(PPCContext& ctx, uint8_t* base) {
  const uint32_t thread = ctx.r3.u32;
  __imp__sub_82844200(ctx, base);
  using namespace gta4::trainer;
  if (!g_has_work.load(std::memory_order_relaxed))
    return;
  // At most one pass per frame, decided before any clock or memory query:
  // this hook fires once per script thread, dozens of times a frame.
  const uint32_t frame = g_frame.load(std::memory_order_relaxed);
  if (frame == g_last_run_frame)
    return;
  static const bool executing_global_ok = Span(base, kExecutingThread, 4, true);
  if (!executing_global_ok || !thread || !Span(base, thread, 116))
    return;
  g_last_run_frame = frame;
  const uint64_t start = NowNs();
  const uint32_t previous = Read(base, kExecutingThread);
  if (!previous)
    Write(base, kExecutingThread, thread);
  RunActions(ctx, base);
  if (!previous)
    Write(base, kExecutingThread, previous);
  const uint64_t spent = NowNs() - start;
  g_cost.pass_ns += spent;
  g_cost.worst_pass_ns = std::max(g_cost.worst_pass_ns, spent);
  ++g_cost.passes;
}

// The vehicle model store is sized once at boot (sub_821C83C0) and the
// vehicles.ide loader adds every car to it with no capacity check
// (sub_821CA5F0), so each add-on car needs a free entry: like the PC game's
// limit adjusters, make room for more (64 more entries, about 61 KB).
// Each frame, per vehicle drawn, the game writes its hidden extras into the
// skeleton (sub_82381FA0). The Deimos's pop-up pose is written right after,
// so it holds whatever the frame did to the skeleton before.
extern "C" void sub_82381FA0(PPCContext& ctx, uint8_t* base) {
  const uint32_t vehicle = ctx.r3.u32;
  __imp__sub_82381FA0(ctx, base);
  if (REXCVAR_GET(gta4_trainer))
    gta4::trainer::ReapplyPoses(ctx, base, vehicle);
}

// The base model store (sub_821C8110: CBaseModelInfo, 96 bytes each, every
// .ide's "objs" lines) is sized the same way and filled by an adder that does
// not check either: the map sets' models (v1.7.30, in gta4_addon_install's
// gtxd.ide) get room past the stock ones (1024 more entries, 96 KB).
extern "C" void sub_821C8110(PPCContext& ctx, uint8_t* base) {
  using namespace gta4::trainer;
  constexpr uint32_t kBaseModelStore = 0x82A9302C, kExtraBaseModels = 1024;
  static bool s_raised = false;
  if (ctx.r3.u32 == kBaseModelStore && !s_raised && Span(base, kBaseModelStore, 12, true)) {
    const uint32_t capacity = Read(base, kBaseModelStore);
    if (capacity >= 1000 && capacity <= 200000) {
      s_raised = true;
      Write(base, kBaseModelStore, capacity + kExtraBaseModels);
      REXLOG_INFO("gta4-trainer: base model store raised from {} to {} entries", capacity,
                  capacity + kExtraBaseModels);
    } else {
      REXLOG_WARN("gta4-trainer: base model store capacity {} looks wrong; left alone", capacity);
    }
  }
  __imp__sub_821C8110(ctx, base);
}

extern "C" void sub_821C83C0(PPCContext& ctx, uint8_t* base) {
  using namespace gta4::trainer;
  static bool s_raised = false;
  if (ctx.r3.u32 == kVehicleModelStore && !s_raised && Span(base, kVehicleModelStore, 12, true)) {
    const uint32_t capacity = Read(base, kVehicleModelStore);
    if (capacity >= 16 && capacity <= 4096) {
      s_raised = true;
      Write(base, kVehicleModelStore, capacity + kExtraVehicleModels);
      REXLOG_INFO("gta4-trainer: vehicle model store raised from {} to {} entries", capacity,
                  capacity + kExtraVehicleModels);
    } else {
      REXLOG_WARN("gta4-trainer: vehicle model store capacity {} looks wrong; left alone",
                  capacity);
    }
  }
  __imp__sub_821C83C0(ctx, base);
}

// As the game opens certain data files (vehicles.ide, handling.dat and the
// like) it checks their contents against built-in checksums (sub_821B5538,
// called from the data-file open, sub_821B5600) and, on a mismatch, stops for
// good - the console log 043 crash was that check failing on vehicles.ide.
// The add-ons' lines are exactly such a change, so while their shadow
// common.rpf is in use the check is skipped; without it the game checks as
// it always did.
extern "C" void sub_821B5538(PPCContext& ctx, uint8_t* base) {
  if (gta4::addons::ShadowsActive())
    return;
  __imp__sub_821B5538(ctx, base);
}

// ---------------------------------------------------------------------------
// The crash notes (v1.7.43): the ring's lines, oldest first, for the port's
// fault handler (ps5_diag.cpp calls this weakly, with no allocation).
// ---------------------------------------------------------------------------
extern "C" size_t GTA4_TrainerCrashNotes(char* out, size_t cap) {
  using namespace gta4::trainer;
  if (!out || cap < 2)
    return 0;
  const uint32_t head = g_note_head.load(std::memory_order_relaxed);
  const uint32_t count = head < kNoteRing ? head : uint32_t(kNoteRing);
  size_t n = 0;
  for (uint32_t k = 0; k < count; ++k) {
    const char* line = g_notes[(head - count + k) % kNoteRing];
    size_t len = 0;
    while (len < kNoteLine && line[len])
      ++len;
    if (n + len + 2 > cap)
      break;
    std::memcpy(out + n, line, len);
    n += len;
    out[n++] = '\n';
  }
  out[n] = 0;
  return n;
}

// The model module's load (sub_821C7A08: r3 the model's index, r5 the
// resource's map): the model about to be placed goes into the notes, so a
// crash while the game places one (logs 108, 109, 113: the Keys) names it.
extern "C" void sub_821C7A08(PPCContext& ctx, uint8_t* base) {
  using namespace gta4::trainer;
  const uint32_t index = ctx.r3.u32;
  if (index < kModelInfoCount && Span(base, kModelInfoTable + index * 4, 4)) {
    const uint32_t mi = Read(base, kModelInfoTable + index * 4);
    const uint32_t hash = mi && Span(base, mi, 64) ? Read(base, mi + 60) : 0;
    std::string name;
    for (const auto& m : g_map.models)
      if (m.hash == hash) {
        name = m.name;
        break;
      }
    Note(fmt::format("placing model {} ({:08X}, slot {})", name.empty() ? "?" : name, hash, index));
  }
  __imp__sub_821C7A08(ctx, base);
}

// A shader's placement (sub_828C9C68: r3 the shader's instance data,
// shader + 0x14; r4 the datResource being placed, +0 its chunk map). The
// game looks the shader's effect up by the hash at +20 in its effect list;
// one not there is a null read right after (logs 108, 109, 113). Before the
// game looks, the hash is checked here, and a miss is logged and noted with
// the model being placed, the shader's words and the effect's name the
// shader carries (+0x44: an unfixed offset, translated through the map),
// so the crash report says which file and which shader.
extern "C" void sub_828C9C68(PPCContext& ctx, uint8_t* base) {
  using namespace gta4::trainer;
  constexpr uint32_t kEffectList = 0x831C3DE8;
  const uint32_t inst = ctx.r3.u32, rsc = ctx.r4.u32;
  if (Span(base, inst, 40) && Span(base, kEffectList, 4)) {
    const uint32_t hash = Read(base, inst + 20);
    bool found = false;
    int n = 0;
    for (uint32_t e = Read(base, kEffectList); e && Span(base, e, 88) && n < 256; e = Read(base, e + 84), ++n)
      if (Read(base, e + 36) == hash) {
        found = true;
        break;
      }
    if (!found) {
      const uint32_t shader = inst - 0x14;
      std::string words;
      if (Span(base, shader, 0x60))
        for (uint32_t w = 0; w < 24; ++w)
          words += fmt::format("{}{:08X}", w ? " " : "", Read(base, shader + w * 4));
      // the effect's name: its offset (0x5xxxxxxx) through the map's chunks
      std::string name = "?";
      const uint32_t map = Span(base, rsc, 16) ? Read(base, rsc) : 0;
      const uint32_t raw = Span(base, shader + 0x44, 4) ? Read(base, shader + 0x44) : 0;
      if (map && Span(base, map, 4)) {
        const uint32_t chunks = Read16(base, map) + Read16(base, map + 2);
        for (uint32_t k = 0; k < chunks && k < 128 && Span(base, map + 4 + k * 12, 12); ++k) {
          const uint32_t src = Read(base, map + 4 + k * 12), dest = Read(base, map + 8 + k * 12),
                         size = Read(base, map + 12 + k * 12);
          if (raw >= src && raw < src + size) {
            name = String(base, raw - src + dest, 64);
            break;
          }
        }
      }
      const std::string last = g_note_head.load(std::memory_order_relaxed)
                                   ? g_notes[(g_note_head.load(std::memory_order_relaxed) - 1) % kNoteRing]
                                   : "";
      REXLOG_WARN("gta4-trainer: shader effect miss: hash {:08X} not in the game's {} effects; the shader at {:08X} "
                  "names '{}' (raw {:08X}); {}; the shader's words: {}",
                  hash, n, shader, name, raw, last, words);
      Note(fmt::format("EFFECT MISS {:08X} '{}' shader {:08X} rsc {:08X}", hash, name, shader, rsc));
    }
  }
  __imp__sub_828C9C68(ctx, base);
}
