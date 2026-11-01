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
#include "gta4_time_machine.h"

#include <rex/platform.h>
#if REX_PLATFORM_PS5
#include "ps5/game/sound.h"
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
uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
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
std::optional<InvokeResult> Invoke(PPCContext& parent, uint8_t* base, std::string_view name,
                                   std::initializer_list<uint32_t> values = {},
                                   int output_argument = -1, uint32_t result_seed = 0,
                                   int output_count = 1, int text_argument = -1,
                                   std::string_view text = {}, int output_word = 0) {
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

// Calls a game function (not a native) with up to two arguments on the same
// kind of scratch frame; returns its r3.
std::optional<uint32_t> CallGuest(PPCContext& parent, uint8_t* base, uint32_t address, uint32_t r3,
                                  uint32_t r4 = 0) {
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

enum RideHookId : size_t { kHookCharInModel, kHookCarModel, kHookPoliceVehicle, kRideHookCount };
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
std::array<RideHook, kRideHookCount> g_ride_hooks = {{
    {"IS_CHAR_IN_MODEL", RideCharInModel},
    {"IS_CAR_MODEL", RideCarModel},
    {"IS_CHAR_IN_ANY_POLICE_VEHICLE", RidePoliceVehicle},
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
};
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
struct TeleportChoice {
  std::string_view label;
  float x, y;
};
// Approximate map spots; the teleport finds the ground itself.
constexpr std::array kTeleports = {
    TeleportChoice{"Francis Intl Airport", 2137.0f, 741.0f},
    TeleportChoice{"Happiness Island", -2241.0f, 366.0f},
    TeleportChoice{"Star Junction", -261.0f, 787.0f},
    TeleportChoice{"Middle Park", -241.0f, 1035.0f},
    TeleportChoice{"Broker Bridge (east)", 553.0f, 185.0f},
};

enum class Page { kMain, kPlayer, kVehicle, kSpawn, kTeleport, kWorld, kWeapons, kChaos, kStatus, kTimeMachine };
enum class Action {
  kNone, kOpenPlayer, kOpenVehicle, kOpenSpawn, kOpenTeleport, kOpenWorld,
  kOpenWeapons, kOpenChaos, kOpenStatus,
  kToggleRain, kToggleGravity,
  kToggleGod, kToggleNeverWanted, kWantedUp, kWantedDown, kWantedApply,
  kMoney, kHeal,
  kToggleVehicleGod, kRepair, kToggleWarpIn, kToggleGadgets, kToggleJet,
  kTogglePopupAuto, kToggleAnyRide, kToggleCarLights,
  kSpawnVehicle, kTeleportWaypoint, kTeleportPreset, kSavePosition, kLoadPosition,
  kHourUp, kHourDown, kMinuteUp, kMinuteDown, kTimeApply,
  kWeatherNext, kWeatherPrevious, kWeatherApply, kWeatherRelease,
  kGiveLoadout, kRemoveWeapons,
  kOpenTimeMachine, kTmPreset, kTmField, kTmCircuits, kTmRefuel, kTmHover,
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
  bool raining = false, gravity_off = false;
  bool any_ride = false;     // Bring Your Own Ride: missions take the player's vehicle
  bool popup_auto = false;   // Deimos pop-ups: d-pad left (false) or up with the lights (true)
  bool car_lights = true;    // any car's headlights on d-pad left (the one option on by default)
  int wanted_target = 0;
  int hour = 12, minute = 0, weather = 1;
  // The DeLorean's destination (set from the menu: a preset, or each field),
  // and the menu's requests the time machine acts on in the car.
  TcdDate tm_dest;
  int tm_preset = 1;
  bool tm_toggle_circuits = false, tm_refuel = false, tm_toggle_hover = false;
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
struct DrawModel {
  bool open = false;
  std::string title, footer, toast;
  std::vector<DrawLine> lines;
};
DrawModel g_draw;
std::atomic<bool> g_input_owned{false};

// Menu selections cross from the input poll to the script stage here. Both
// run on the game's main thread, so plain containers suffice; the atomics
// carry the pad sample for the gadgets and the online verdict back.
struct PendingItem {
  Action action = Action::kNone;
  int index = -1;
};
std::vector<PendingItem> g_pending;
std::atomic<uint16_t> g_gadget_buttons{0}, g_gadget_previous{0};
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
bool g_have_nexus = false, g_have_arbiter = false, g_have_dominator = false;
bool g_have_lazar = false, g_have_gauntlet = false;
bool g_have_dmc12 = false, g_have_bttf1 = false, g_have_bttf2 = false, g_have_bttf3 = false, g_have_bttf2h = false;
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
         s.raining || s.pending_model >= 0 || !g_addons_probed || g_have_f16 || g_have_deimos ||
         g_have_batmobile || g_have_nexus || g_have_arbiter || g_have_dominator || g_have_lazar ||
         g_have_gauntlet || g_have_dmc12 || HaveTimeMachines() || s.car_lights;
}

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
               {"Trainer Status", Action::kOpenStatus}};
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
               {"Repair Current Vehicle", Action::kRepair},
               {s.warp_in ? "Warp Into Spawned Car: ON" : "Warp Into Spawned Car: OFF",
                Action::kToggleWarpIn},
               {s.gadgets ? "Batmobile Gadgets: ON (L1 missile, d-pad guns/boost, horn bomb)"
                          : "Batmobile Gadgets: OFF",
                Action::kToggleGadgets},
               {s.jet ? "F-16 Flight and Weapons: ON" : "F-16 Flight and Weapons: OFF",
                Action::kToggleJet},
               {s.popup_auto ? "Deimos/Nexus Pop-ups: Automatic (with the lights)"
                             : "Deimos/Nexus Pop-ups: D-pad Left",
                Action::kTogglePopupAuto},
               {s.car_lights ? "Headlights on D-pad Left: ON" : "Headlights on D-pad Left: OFF",
                Action::kToggleCarLights},
               {s.any_ride ? "Bring Your Own Ride: ON" : "Bring Your Own Ride: OFF",
                Action::kToggleAnyRide}};
      break;
    case Page::kSpawn:
      for (int i = 0; i < int(kVehicles.size()); ++i)
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
               {"Part II: Fly / Land (d-pad right in the car)", Action::kTmHover}};
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
struct PendingTeleport {
  bool active = false;
  float x = 0, y = 0, from = 0;  // where, and the height the ground is looked for below
  uint32_t car = 0;              // the vehicle moved along, or 0
  uint64_t until = 0;
};
PendingTeleport g_teleport;

std::optional<float> GroundBelow(PPCContext& ctx, uint8_t* base, float x, float y, float from) {
  const auto ground = Invoke(ctx, base, "GET_GROUND_Z_FOR_3D_COORD", {Bits(x), Bits(y), Bits(from), 0}, 3);
  if (!ground)
    return std::nullopt;
  const float gz = ground->f(0);
  if (!std::isfinite(gz) || gz == 0.0f || gz > from)
    return std::nullopt;
  return gz;
}

void PlaceOnGround(PPCContext& ctx, uint8_t* base, const GameContext& game, uint32_t car, float x, float y,
                   float gz) {
  if (car) {
    Invoke(ctx, base, "SET_CAR_COORDINATES", {car, Bits(x), Bits(y), Bits(gz + 1.0f)});
    Invoke(ctx, base, "SET_CAR_ON_GROUND_PROPERLY", {car});
  } else {
    // (A metre over the ground, as the trainer's teleports have always put
    // the player: a short drop at most.)
    Invoke(ctx, base, "SET_CHAR_COORDINATES", {game.ped, Bits(x), Bits(y), Bits(gz + 1.0f)});
  }
}

void Teleport(PPCContext& ctx, uint8_t* base, const GameContext& game, float x, float y,
              std::optional<float> z_hint, State& s) {
  uint32_t car = 0;
  if (const auto v = CurrentVehicle(ctx, base, game))
    car = *v;
  // A saved position keeps its level (inside, under a bridge); a waypoint or
  // landmark takes the highest ground there.
  const float from = z_hint ? *z_hint + 2.0f : 1000.0f;
  const float focus = z_hint.value_or(30.0f);
  if (NativeKnown("LOAD_SCENE"))
    Invoke(ctx, base, "LOAD_SCENE", {Bits(x), Bits(y), Bits(focus)});
  Invoke(ctx, base, "REQUEST_COLLISION_AT_POSN", {Bits(x), Bits(y), Bits(focus)});
  if (const auto gz = GroundBelow(ctx, base, x, y, from)) {
    PlaceOnGround(ctx, base, game, car, x, y, *gz);
    REXLOG_INFO("gta4-trainer: teleported to {:.1f}, {:.1f}, ground {:.1f}{}", x, y, *gz, car ? " (with the vehicle)" : "");
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
  g_teleport = PendingTeleport{true, x, y, from, car, NowMs() + 8000};
  REXLOG_INFO("gta4-trainer: teleport to {:.1f}, {:.1f}: waiting for the ground there to load", x, y);
  Toast(s, "Teleporting: loading the area...", 8000);
}

// Every frame while a teleport waits for its ground.
void TeleportTick(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s) {
  auto& t = g_teleport;
  if (!t.active)
    return;
  Invoke(ctx, base, "REQUEST_COLLISION_AT_POSN", {Bits(t.x), Bits(t.y), Bits(std::min(t.from, 60.0f))});
  const auto gz = GroundBelow(ctx, base, t.x, t.y, t.from);
  const bool timed_out = NowMs() > t.until;
  if (!gz && !timed_out)
    return;
  if (t.car)
    Invoke(ctx, base, "FREEZE_CAR_POSITION", {t.car, 0});
  else
    Invoke(ctx, base, "FREEZE_CHAR_POSITION", {game.ped, 0});
  t.active = false;
  if (gz) {
    PlaceOnGround(ctx, base, game, t.car, t.x, t.y, *gz);
    REXLOG_INFO("gta4-trainer: teleport landed, ground {:.1f}", *gz);
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
enum PoseSlot : int {
  kPoseDeimos = 0, kPoseF16 = 1, kPoseBatmobile = 2, kPoseNexus = 3, kPoseLazar = 4, kPoseDelorean = 5,
  kPoseSlots = 6
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
  std::array<BonePose, 4> bones{};
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
                       (vehicle.model == kF16 && s.jet) || (vehicle.model == kBatmobile && s.gadgets);
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
    if (s.pending_hash == ModelHash("F16"))  // ...and the jet's landing gear
      PrepareJet(ctx, base, handle);
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
// The jet's lock-on, GTA V's way: the nearest working car within 12 degrees
// of the nose out to 350 m, held there for 0.6 s, locks (a tone, a red mark
// on the radar, a red light over it). Missiles fired while locked home on
// it; the cannon aims at it while it is within 15 degrees of the nose.
// Turning away past 35 degrees, losing it past 450 m or its wreck drops it.
struct JetLock {
  uint32_t candidate = 0, target = 0, blip = 0;
  uint64_t candidate_since = 0, next_scan = 0;
  float pos[3]{};
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
  // the afterburner's flame (a looped effect) and its size step
  uint32_t flame = 0;
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
constexpr float kGun[3] = {0.74f, 3.29f, 0.55f};
constexpr float kGuns[2][3] = {{0.74f, 3.29f, 0.55f}, {-0.74f, 3.29f, 0.55f}};  // both sides, as asked
constexpr float kPylons[2][3] = {{-2.5f, -3.0f, -0.3f}, {2.5f, -3.0f, -0.3f}};
constexpr float kNozzle[3] = {0.0f, -5.45f, 0.40f};
// Landing gear: the converter hangs the nose gear on extra_1 and the main
// gears on extra_5 and extra_6, each at its hinge. They fold up (the nose
// gear aft about the width, the mains inwards about the length) and are
// then hidden, as the PC mod's are (bHideGears): the fuselage has no wells.
struct GearLeg {
  uint32_t extra;
  int axis;  // 0: the car's width (X), 1: its length (Y)
  float degrees;
};
constexpr std::array<GearLeg, 3> kGearLegs = {{{1, 0, -95.0f}, {5, 1, -95.0f}, {6, 1, 95.0f}}};
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
void GunSound() {
  g_jet_synth.Shot();
  EnsureVoice();
}
void LaunchSound() {
  g_jet_synth.Launch();
  EnsureVoice();
}
// Once a pass: a voice left running with nothing to play is stopped.
void SoundTick() {
#if REX_PLATFORM_PS5
  if (g_jet_voice && g_jet_voice->Active() && g_jet_synth.Quiet())
    g_jet_voice->Stop();
#endif
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
  const auto ground = Invoke(ctx, base, "GET_GROUND_Z_FOR_3D_COORD", {Bits(x), Bits(y), Bits(z), 0}, 3);
  if (!ground)
    return std::nullopt;
  const float gz = ground->f(0);
  if (!std::isfinite(gz) || gz == 0.0f)
    return std::nullopt;
  return gz;
}

void Explode(PPCContext& ctx, uint8_t* base, const float p[3], float radius) {
  // type 2 (rocket), audible, visible, normal camera shake
  Invoke(ctx, base, "ADD_EXPLOSION",
         {Bits(p[0]), Bits(p[1]), Bits(p[2]), 2, Bits(radius), 1, 0, Bits(1.0f)});
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
}

// Every frame in the jet with its weapons on.
void UpdateLock(PPCContext& ctx, uint8_t* base, JetState& j, State& s, const float from[3], const float fwd[3],
                uint64_t now) {
  auto& l = j.lock;
  if (l.target) {
    bool keep = false;
    if (const auto tp = CarPosition(ctx, base, l.target)) {
      float v[3], len = 0;
      for (int a = 0; a < 3; ++a) {
        v[a] = (*tp)[a] - from[a];
        len += v[a] * v[a];
      }
      len = std::sqrt(len);
      const float c = len > 1.0f ? (v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2]) / len : 1.0f;
      const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {l.target});
      keep = len < 450.0f && c > 0.819f && !(dead && dead->r0());  // (cos 35 degrees)
      for (int a = 0; a < 3; ++a)
        l.pos[a] = (*tp)[a];
    }
    if (!keep) {
      DropLock(ctx, base, l);
      Toast(s, "Lock lost.", 800);
    } else {
      Invoke(ctx, base, "DRAW_LIGHT_WITH_RANGE",
             {Bits(l.pos[0]), Bits(l.pos[1]), Bits(l.pos[2] + 1.4f), 255, 0, 0, Bits(5.0f), Bits(7.0f)});
    }
  }
  if (now < l.next_scan)
    return;
  l.next_scan = now + 200;
  const uint32_t found = CarAhead(ctx, base, j.car, from, fwd, 12.0f, 350.0f);
  if (!found || found == l.target) {
    l.candidate = 0;
  } else if (found != l.candidate) {
    l.candidate = found;
    l.candidate_since = now;
  } else if (now - l.candidate_since >= 600) {
    DropLock(ctx, base, l);
    l.target = found;
    if (const auto tp = CarPosition(ctx, base, found))
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
  const auto start = CarPoint(ctx, base, j.car, kPylons[j.pylon]);
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
  m.speed = std::max(j.speed, 0.0f) + 90.0f;
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
  auto scatter = [](float spread) {
    seed = seed * 1664525u + 1013904223u;
    return (float((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * spread;
  };
  bool flashed = false, aimed = false;
  float line[3] = {fwd[0], fwd[1], fwd[2]};
  float origin[3]{};
  for (const auto& gun_at : kGuns) {
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
  static const uint32_t kF16 = ModelHash("F16");
  const float eased = t * t * (3.0f - 2.0f * t);
  const float scale = t >= 0.999f ? 0.0f : 1.0f;
  int entry = 0;
  for (const auto& leg : kGearLegs)
    PoseExtraBone(ctx, base, car, kF16, kPoseF16, entry++, leg.extra, leg.axis, leg.degrees * eased, scale);
}

// An F-16 just made or first met: its gear's extras shown (the game picks
// extras at random, and a hidden one is sent to the world's origin), down.
void PrepareJet(PPCContext& ctx, uint8_t* base, uint32_t car, bool getting_in) {
  for (const auto& leg : kGearLegs)
    ShowExtra(ctx, base, car, leg.extra, getting_in);
  g_jet.gear = 0.0f;
  g_jet.gear_up = false;
  g_jet.next_gear_write = 0;
  PoseGear(ctx, base, car, 0.0f);
}

void StopFlame(PPCContext& ctx, uint8_t* base, JetState& j) {
  if (j.flame)
    Invoke(ctx, base, "STOP_PTFX", {j.flame});
  j.flame = 0;
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
  const auto r = Invoke(ctx, base, "START_PTFX_ON_VEH",
                        {0, car, Bits(kNozzle[0]), Bits(kNozzle[1]), Bits(kNozzle[2]), 0, 0, 0,
                         Bits(step == 2 ? 1.7f : 0.7f)},
                        -1, 0, 1, 0, "weap_rocket");
  j.flame = r ? r->r0() : 0;
  if (!j.flame_logged) {
    j.flame_logged = true;
    REXLOG_INFO("gta4-trainer: F-16 exhaust flame {:08X}", j.flame);
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
//  - horn (L3): plant a bomb behind the car, marked by a blinking red light;
//    the horn again sets it off;
//  - d-pad right (hold): the booster, the rear turbine flaming and roaring.
// Those buttons are hidden from the game then (no drive-by on L1, no radio,
// no horn); d-pad up stays the phone. Missiles in flight and a planted bomb
// carry on after the player leaves the car. The canopy needs no toggle (see
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
  bool bomb = false;
  uint32_t bomb_object = 0;
  float bomb_pos[3]{};
  uint64_t bomb_at = 0;
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
constexpr float kBatBomb[3] = {0.0f, -3.4f, -0.2f};
constexpr std::string_view kBombModel = "w_grenade";
constexpr uint64_t kMissileReloadMs = 3000;  // one launch, then the launcher resets

// One volley from both barrels of a pair, along the car's length (+1 ahead,
// -1 behind), each barrel with IV's machine-gun muzzle flash. (v1.7.7 fired
// the barrels in turn and flashed only every other round: always the left.)
void BatVolley(PPCContext& ctx, uint8_t* base, uint32_t car, const float (&muzzles)[2][3], float along) {
  static uint32_t seed = 0x5EED1234u;
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

bool LaunchRocket(PPCContext& ctx, uint8_t* base, uint32_t car, State& s) {
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
  const float ahead_local[3] = {kBatLauncher[0], kBatLauncher[1] + 100.0f, kBatLauncher[2]};
  const auto start = CarPoint(ctx, base, car, kBatLauncher), ahead = CarPoint(ctx, base, car, ahead_local);
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
  const float mouth[3] = {kBatLauncher[0], kBatLauncher[1] + 0.3f, kBatLauncher[2]};
  CarEffect(ctx, base, car, "muz_machine", mouth, 2.5f);
  LaunchSound();
  Toast(s, r.target ? "Missile locked on the car ahead." : "Missile away (nothing to lock on).", 1100);
  if (g_gadget.rocket_log < 12) {
    ++g_gadget.rocket_log;
    REXLOG_INFO("gta4-trainer: Batmobile missile away at {:.0f} m/s (object {:08X}, trail {:08X}, target {:08X})",
                r.speed, r.object, r.trail, r.target);
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

void PlantBomb(PPCContext& ctx, uint8_t* base, uint32_t car, State& s) {
  auto p = CarPoint(ctx, base, car, kBatBomb);
  if (!p)
    return;
  const auto gz = GroundZ(ctx, base, (*p)[0], (*p)[1], (*p)[2] + 1.5f);
  if (gz)
    (*p)[2] = *gz + 0.06f;
  for (int a = 0; a < 3; ++a)
    g_gadget.bomb_pos[a] = (*p)[a];
  g_gadget.bomb_object = 0;
  const uint32_t hash = ModelHash(kBombModel);
  const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {hash});
  if (loaded && loaded->r0()) {
    const auto obj = Invoke(ctx, base, "CREATE_OBJECT", {hash, Bits((*p)[0]), Bits((*p)[1]), Bits((*p)[2]), 0, 1}, 4);
    if (obj && obj->words[0]) {
      g_gadget.bomb_object = obj->words[0];
      Invoke(ctx, base, "SET_OBJECT_COLLISION", {g_gadget.bomb_object, 0});
      Invoke(ctx, base, "FREEZE_OBJECT_POSITION", {g_gadget.bomb_object, 1});
    }
  }
  g_gadget.bomb = true;
  g_gadget.bomb_at = NowMs();
  Toast(s, "Bomb planted: the horn (L3) again sets it off.", 2500);
  if (g_gadget.bomb_log < 12) {
    ++g_gadget.bomb_log;
    REXLOG_INFO("gta4-trainer: Batmobile bomb planted at {:.1f}, {:.1f}, {:.1f} (ground {}, object {:08X})",
                (*p)[0], (*p)[1], (*p)[2], gz ? "found" : "not found", g_gadget.bomb_object);
  }
}

void SetOffBomb(PPCContext& ctx, uint8_t* base, bool quietly) {
  if (!g_gadget.bomb)
    return;
  const auto& p = g_gadget.bomb_pos;
  if (!quietly) {
    // A cluster: the charge, and two beside it.
    const float left[3] = {p[0] - 1.2f, p[1] + 0.5f, p[2]}, right[3] = {p[0] + 1.2f, p[1] - 0.5f, p[2]};
    Explode(ctx, base, p, 1.5f);
    Explode(ctx, base, left, 1.0f);
    Explode(ctx, base, right, 1.0f);
    if (g_gadget.bomb_log < 12) {
      ++g_gadget.bomb_log;
      REXLOG_INFO("gta4-trainer: Batmobile bomb set off at {:.1f}, {:.1f}, {:.1f}", p[0], p[1], p[2]);
    }
  }
  DeleteObject(ctx, base, g_gadget.bomb_object);
  g_gadget.bomb = false;
}

// A planted bomb's blinking light (drawn every frame, like Rockstar's scripts
// draw theirs: red, a few metres' reach).
void BombLight(PPCContext& ctx, uint8_t* base) {
  if (!g_gadget.bomb || ((NowMs() - g_gadget.bomb_at) / 350) % 2)
    return;
  const auto& p = g_gadget.bomb_pos;
  Invoke(ctx, base, "DRAW_LIGHT_WITH_RANGE",
         {Bits(p[0]), Bits(p[1]), Bits(p[2] + 0.25f), 255, 0, 0, Bits(3.0f), Bits(8.0f)});
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
  return g_gadget.bomb || std::any_of(g_gadget.rockets.begin(), g_gadget.rockets.end(),
                                      [](const BatRocket& r) { return r.live; });
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
      SetOffBomb(ctx, base, true);  // the toggle off: a planted bomb goes quietly
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
  // The bomb on the horn (L3): plant, then set off. Acted on release, so
  // L3 + R3 (the menu) never plants one.
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
  if (horn_tap) {
    if (g_gadget.bomb)
      SetOffBomb(ctx, base, false);
    else
      PlantBomb(ctx, base, car, s);
  }
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
};
constexpr CanopySpec kBatCanopy{"BATMOBILE", "Batmobile", "interiorlight", 60.0f, 0.45f, 0.7f,
                                {-2.1f, 0.2f, 0.0f}, kPoseBatmobile, 0, true, true};
constexpr CanopySpec kJetCanopy{"F16", "F-16", "interiorlight", 38.0f, 0.9f, 1.2f,
                                {-2.3f, 4.3f, 0.0f}, kPoseF16, 3, true, false};
CanopyState g_canopy, g_jet_canopy;
// The Lazar 3's side ramps (the PC model's doors, hinged at their bottom
// edge, folding out and down: 105 degrees there): the player in and out on
// the left, a passenger on the right, both ramps moving together on their
// hydraulics, slower than a canopy.
void LazarRampSound(bool opening);  // (with the Lazar's turret, below)
constexpr CanopySpec kLazarRamps{"LAZAR3", "Lazar 3", "extra_6", -100.0f, 1.5f, 1.7f, {-3.0f, 0.75f, 0.0f},
                                 kPoseLazar, 2, true, true, 1, "interiorlight", 100.0f, LazarRampSound};
CanopyState g_lazar_ramps;

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
  // On foot: is the game starting to put the player into one?
  const auto getting = Invoke(ctx, base, "IS_CHAR_GETTING_IN_TO_A_CAR", {game.ped});
  if (!getting || !getting->r0())
    return 0;
  const auto target = Invoke(ctx, base, "GET_CAR_CHAR_IS_USING", {game.ped, 0}, 1);
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
  float origin[3]{}, start[3]{}, dir[3]{}, pos[3]{};
  float speed = 0.0f;
  uint64_t fired = 0;
  int frames = 0;
};
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
    if (z.in_car)
      LazarServoSound(0.0f);
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
  if ((buttons & X_INPUT_GAMEPAD_LEFT_SHOULDER) && now >= z.next_shot && !s.open) {
    z.next_shot = now + kLazarShotMs;
    FireLazar(ctx, base, z, s);
  }
  return X_INPUT_GAMEPAD_LEFT_SHOULDER;
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
  // Part II's flight
  bool hover = false, landing = false, swap_pending = false, models_requested = false;
  float altitude = 0.0f, heading = 0.0f, fwd = 0.0f, climb = 0.0f, bank = 0.0f, pitch = 0.0f;
  int hover_log = 0;
};
TimeMachineState g_tm;
std::atomic<bool> g_tm_in_car{false};

// What the screen shows (the TCD, the flash), published for the overlay.
struct TcdModel {
  bool visible = false, empty = false, hover = false;
  TcdDate dest, present, last;
  int mph = 0;
  float flash = 0.0f;  // 0..1, a white flash over the screen
};
std::mutex g_tcd_mutex;
TcdModel g_tcd;
std::atomic<bool> g_tcd_active{false};
uint64_t g_flash_until = 0, g_flash_start = 0;

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
  m.next_boom = NowMs();
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

// Part II in the air: the left stick drives and turns, R2 climbs, L2 sinks;
// it hovers where it is with nothing pressed. Its attitude (the heading, a
// lean into the turn and the acceleration) and its speed along the nose
// are set every frame as the F-16's are; the height is held by moving it
// up or down a little each frame.
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
  m.pitch += (std::clamp(-accel / std::max(dt, 1e-3f) * 0.012f + m.climb * 0.02f, -0.25f, 0.25f) - m.pitch) *
             std::min(1.0f, 3.0f * dt);
  float z = (*c)[2] + m.climb * dt;
  const auto gz = GroundZ(ctx, base, (*c)[0], (*c)[1], (*c)[2] + 1.0f);
  if (gz && z < *gz + 0.85f) {
    z = *gz + 0.85f;
    m.climb = std::max(m.climb, 0.0f);
  }
  m.altitude = gz ? z - *gz : 99.0f;
  const Quat q = QNormalize(QMul(QMul(QAxis(0, 0, 1, m.heading), QAxis(1, 0, 0, m.pitch)), QAxis(0, 1, 0, m.bank)));
  Invoke(ctx, base, "SET_VEHICLE_QUATERNION", {m.car, Bits(q.x), Bits(q.y), Bits(q.z), Bits(q.w)});
  PlaceCar(ctx, base, m.car, (*c)[0], (*c)[1], z);
  Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {m.car, Bits(m.fwd)});
  g_tm_synth.SetHover(0.35f + 0.45f * std::min(1.0f, std::fabs(m.fwd) / 40.0f) + 0.2f * std::fabs(up));
  if (m.hover_log < 4 && dt > 0.0f) {
    ++m.hover_log;
    REXLOG_INFO("gta4-trainer: time machine flying: {:.1f} m up, {:.1f} m/s ahead, climbing {:.1f} m/s", m.altitude,
                m.fwd, m.climb);
  }
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
  // A trip under way carries on whatever happens (the car is ours till it is back).
  if (m.phase == 1) {
    if (now - m.phase_at >= kGoneMs || !VehicleAddress(base, m.car))
      Arrive(ctx, base, game, m);
    return film ? uint32_t(X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_DPAD_RIGHT | X_INPUT_GAMEPAD_LEFT_SHOULDER) : 0;
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
      m.hover = false;
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
    m.hover_form = vehicle.model == ModelHash("DELHOV");
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
  // Part II's models: both kept loaded for the conversion.
  if (film == 2 && !m.models_requested) {
    Invoke(ctx, base, "REQUEST_MODEL", {ModelHash("DELHOV")});
    Invoke(ctx, base, "REQUEST_MODEL", {ModelHash("DELTWO")});
    m.models_requested = true;
  }
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
      if (!m.hover) {
        if (m.hover_form || SwapForm(ctx, base, game, m, true, s)) {
          m.hover = true;
          m.landing = false;
          m.fwd = speed;
          m.climb = 3.0f;  // up off the road
          m.pitch = m.bank = 0.0f;
          if (const auto h = Invoke(ctx, base, "GET_CAR_HEADING", {m.car, 0}, 1))
            m.heading = h->f(0) * 3.14159265f / 180.0f;
          Toast(s, "Flight mode: left stick to fly, R2 up, L2 down. D-pad right lands.", 3500);
        }
      } else {
        m.landing = true;
        Toast(s, "Landing...", 1500);
      }
    }
  }
  if (m.hover) {
    Hover(ctx, base, m, pad, dt);
    if (m.landing && m.altitude < 1.05f) {
      if (SwapForm(ctx, base, game, m, false, s)) {
        m.hover = m.landing = false;
        g_tm_synth.SetHover(0.0f);
      }
    }
  } else {
    g_tm_synth.SetHover(0.0f);
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
  const bool active = t.visible || t.flash > 0.0f;
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
  synth.SetTarget(rpm, throttle, boost, e.volume);
}

// Every frame in the F-16 (a look a few times a second otherwise, and while
// its missiles fly). The jet's own behaviour (engine sound, landing gear,
// exhaust) always; flight and weapons with the menu's toggle. Returns the
// buttons the jet uses, hidden from the game: R3 (the gear), and with the
// weapons on the d-pad (not the radio).
uint32_t JetControls(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, const DrivePad& pad) {
  auto& j = g_jet;
  const uint64_t now = NowMs();
  const float dt = j.last_ms ? std::clamp(float(now - j.last_ms) / 1000.0f, 0.0f, 0.05f) : 0.0f;
  j.last_ms = now;
  UpdateMissiles(ctx, base, j, dt);
  // Who is driving what: checked every frame in the jet, a few times a
  // second otherwise.
  if (!j.in_jet && now < j.next_probe)
    return 0;
  j.next_probe = now + 400;
  static const uint32_t kF16 = ModelHash("F16");
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  const auto car = vehicle.car;
  const bool in_jet = car && vehicle.model == kF16;
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
    }
    j.in_jet = j.flying = false;
    j.announced = false;
    return 0;
  }
  if (!j.in_jet || j.car != *car) {
    j.flying = false;
    if (j.car != *car)
      PrepareJet(ctx, base, *car, true);
    else  // back in the same jet: make sure the gear's extras still show
      for (const auto& leg : kGearLegs)
        ShowExtra(ctx, base, *car, leg.extra, true);
    j.car = *car;
    j.prev_buttons = pad.buttons;  // no edge from a button held while getting in
    j.r3_held = false;
    j.entered_at = now;
    j.spool_logged = j.ready_told = false;
    j.pull_logs = j.flight_logs = 0;
    j.next_pull_log = j.next_flight_log = 0;
    REXLOG_INFO("gta4-trainer: F-16: player in the jet; flight and weapons {}", s.jet ? "on" : "off (the menu)");
  }
  j.in_jet = true;
  if (s.jet && !j.rocket_requested) {
    Invoke(ctx, base, "REQUEST_MODEL", {ModelHash(kRocketModel)});
    j.rocket_requested = true;
  }
  if (!j.announced) {
    j.announced = true;
    Toast(s, s.jet ? "F-16: hold RT until the engines are at full power, then pull back to take off. LT air "
                     "brake, LB/RB rudder. D-pad right: cannon, left: missile (both lock on a car ahead). R3: gear."
                   : "F-16: R3 landing gear. Flight and weapons: Vehicle Options > F-16 Flight and Weapons.",
          7000);
  }
  float ground_speed = 0.0f;
  if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {*car, 0}, 1))
    ground_speed = v->f(0);
  const float lx = Axis(pad.lx), ly = Axis(pad.ly);
  const float throttle = pad.right_trigger / 255.0f, airbrake = pad.left_trigger / 255.0f;
  const float rudder = ((pad.buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER) ? 1.0f : 0.0f) -
                       ((pad.buttons & X_INPUT_GAMEPAD_LEFT_SHOULDER) ? 1.0f : 0.0f);
  const bool burner = throttle > 0.9f && (j.flying || ground_speed > 20.0f);
  // How high the jet is: its origin over the ground under it.
  float clearance = 100.0f;
  if (s.jet)
    if (const auto at = CarPosition(ctx, base, *car))
      if (const auto below = GroundZ(ctx, base, (*at)[0], (*at)[1], (*at)[2] + 0.5f))
        clearance = (*at)[2] - *below;

  if (!s.jet) {
    j.flying = false;
  } else {
    const float rpm = g_jet_synth.Rpm();
    const bool spooled = throttle > 0.85f && rpm > kFullPower;  // full power, heard
    if (spooled && !j.spool_logged) {
      j.spool_logged = true;
      REXLOG_INFO("gta4-trainer: F-16 engines at full power {} ms after getting in (spool {:.2f}, rolling at "
                  "{:.1f} m/s)",
                  now - j.entered_at, rpm, ground_speed);
    }
    if (!j.flying && spooled && !j.ready_told && ground_speed < kTakeoffSpeed) {
      j.ready_told = true;
      Toast(s, "F-16: full power. Pull back to take off.", 1500);
    }
    if (throttle < 0.5f)
      j.ready_told = false;
    const bool pulled = ly < -0.4f;
    if (!j.flying && pulled && !(ground_speed > kTakeoffSpeed || spooled) && j.pull_logs < 8 &&
        now >= j.next_pull_log) {
      // Pulled back too early: what the takeoff saw (the log's answer to
      // "it won't take off").
      ++j.pull_logs;
      j.next_pull_log = now + 2000;
      REXLOG_INFO("gta4-trainer: F-16 stick back, not yet: rolling at {:.1f} m/s (takeoff {:.0f}), throttle "
                  "{:.2f}, spool {:.2f} (full power {:.2f})",
                  ground_speed, kTakeoffSpeed, throttle, rpm, kFullPower);
    }
    if (!j.flying && pulled && (ground_speed > kTakeoffSpeed || spooled)) {
      const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {*car, 0, 0, 0, 0}, 1, 0, 4);
      if (!q || !NativeKnown("SET_VEHICLE_QUATERNION")) {
        if (j.pull_logs < 8 && now >= j.next_pull_log) {
          ++j.pull_logs;
          j.next_pull_log = now + 2000;
          REXLOG_WARN("gta4-trainer: F-16 can't take off: the jet's attitude can't be {}",
                      q ? "set (SET_VEHICLE_QUATERNION unknown)" : "read (GET_VEHICLE_QUATERNION failed)");
        }
      } else {
        // Launched: at flying speed at least, on its wheels as it stands;
        // the nose comes up from there (kRotateRate at least, at once).
        j.q = QNormalize({q->f(0), q->f(1), q->f(2), q->f(3)});
        j.speed = std::max(ground_speed, kLaunchSpeed);
        j.flying = true;
        const float authority = std::clamp((j.speed - 12.0f) / (kAuthoritySpeed - 12.0f), 0.2f, 1.0f);
        j.rate[0] = std::max(-ly * 0.95f * authority, kRotateRate);
        j.rate[1] = j.rate[2] = 0.0f;
        j.rest_clearance = std::clamp(clearance, 0.3f, 3.0f);
        j.climbed = false;
        j.airborne_at = now;
        j.blocked_since = 0;
        j.flight_logs = 0;
        j.next_flight_log = now + 1000;
        REXLOG_INFO("gta4-trainer: F-16 airborne: rolling at {:.1f} m/s, launched at {:.1f} ({:.2f} m over the "
                    "ground on its wheels, engines {}, spool {:.2f})",
                    ground_speed, j.speed, clearance, spooled ? "at full power" : "spooling", rpm);
      }
    }
    if (j.flying) {
      const float nose_axis[3] = {0, 1, 0}, right_axis[3] = {1, 0, 0}, up_axis[3] = {0, 0, 1};
      float f[3], r[3], u[3];
      QRotate(j.q, nose_axis, f);
      QRotate(j.q, right_axis, r);
      QRotate(j.q, up_axis, u);
      const float v = j.speed;
      // Speed: the engine's thrust against drag (the air brake adds to it)
      // and gravity along the climb.
      const float thrust = kIdleThrust + (kDryThrust - kIdleThrust) * throttle + (burner ? kBurnerThrust : 0.0f);
      const float drag = kDrag * v * v * (1.0f + 2.5f * airbrake) + 2.0f * airbrake;
      j.speed = std::clamp(v + (thrust - drag - 9.81f * f[2]) * dt, 0.0f, kMaxJetSpeed);
      // Turning: rates easing towards the stick and rudder, with more
      // authority the faster it flies.
      const float authority = std::clamp((v - 12.0f) / (kAuthoritySpeed - 12.0f), 0.2f, 1.0f);
      const float want[3] = {-ly * 0.95f * authority, lx * 2.6f * authority, rudder * 0.45f * authority};
      const float ease = std::min(1.0f, dt * 5.0f);
      for (int a = 0; a < 3; ++a)
        j.rate[a] += (want[a] - j.rate[a]) * ease;
      const bool launching = now - j.airborne_at < kLaunchGrace;
      if (now - j.airborne_at < kRotateMs)
        j.rate[0] = std::max(j.rate[0], kRotateRate);  // rotating off the runway
      j.q = QMul(j.q, QAxis(1, 0, 0, j.rate[0] * dt));   // stick back: nose up
      j.q = QMul(j.q, QAxis(0, 1, 0, j.rate[1] * dt));   // stick right: roll right
      j.q = QMul(j.q, QAxis(0, 0, 1, -j.rate[2] * dt));  // RB: nose right
      // A banked jet turns (right wing down, right.z < 0, turns right)...
      j.q = QMul(QAxis(0, 0, 1, r[2] * 0.3f * authority * dt), j.q);
      // ...and its path bends with gravity against the wings' lift. The lift
      // holds whatever climb or dive it is in, wings level and fast enough
      // (hands off, it keeps its attitude, as GTA V's jets do); slow, banked
      // or upside down the nose drops, and below stalling speed it falls
      // through towards the ground until the dive brings the speed back.
      // (Banked, the wings pull up to twice as hard, half way to holding
      // the height: hands off, a banked jet turns and sinks gently.)
      const float level = std::sqrt(std::max(0.0f, 1.0f - f[2] * f[2]));
      const float bank_cos = std::clamp(u[2] / std::max(level, 0.2f), -1.0f, 1.0f);
      const float hold = std::min(1.0f / std::max(std::fabs(bank_cos), 0.5f), 2.0f);
      const float lift =
          9.81f * level * std::min(1.0f, (v * v) / (kLiftSpeed * kLiftSpeed)) * (0.5f + 0.5f * hold);
      const float along = -9.81f * f[2];
      float bend[3];
      for (int a = 0; a < 3; ++a)
        bend[a] = u[a] * lift + (a == 2 ? -9.81f : 0.0f) - f[a] * along;
      const float axis[3] = {f[1] * bend[2] - f[2] * bend[1], f[2] * bend[0] - f[0] * bend[2],
                             f[0] * bend[1] - f[1] * bend[0]};
      const float mag = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
      if (mag > 1e-3f)
        j.q = QMul(QAxis(axis[0] / mag, axis[1] / mag, axis[2] / mag, mag / std::max(v, 10.0f) * dt), j.q);
      if (v < kStallSpeed && f[2] > -0.95f) {
        // Stalled: the nose falls over towards the ground (out of a climb
        // straight up as well), about the wings' axis.
        const float drop[3] = {f[1] * -1.0f - 0.0f, -(f[0] * -1.0f), 0.0f};  // f x (0, 0, -1)
        const float n = std::sqrt(drop[0] * drop[0] + drop[1] * drop[1]);
        const float fall = (1.0f - v / kStallSpeed) * 1.4f * dt;
        if (n > 1e-3f)
          j.q = QMul(QAxis(drop[0] / n, drop[1] / n, 0.0f, fall), j.q);
        else  // straight up: over the wings
          j.q = QMul(j.q, QAxis(1, 0, 0, -fall));
      }
      j.q = QNormalize(j.q);
      // Touchdown: back near the ground and not diving into it, the wheels
      // (or the belly, gear up) take over, the jet levelled.
      if (clearance > j.rest_clearance + 4.0f)
        j.climbed = true;
      const bool near_ground = clearance < j.rest_clearance + 0.5f && f[2] > -0.35f;
      // Something solid in the way (a wall, a tower): the physics has it.
      // (Not while it leaves the ground: from a standstill the physics needs
      // a moment to reach the speed set.)
      if (!launching && j.speed > 20.0f && ground_speed < 0.25f * j.speed) {
        if (!j.blocked_since)
          j.blocked_since = now;
      } else {
        j.blocked_since = 0;
      }
      if (j.flight_logs < 6 && now >= j.next_flight_log) {
        ++j.flight_logs;
        j.next_flight_log = now + 1500;
        REXLOG_INFO("gta4-trainer: F-16 flying {} ms: {:.1f} m/s (moving at {:.1f}), {:.1f} m up ({}), nose {:.0f} "
                    "degrees, throttle {:.2f}",
                    now - j.airborne_at, j.speed, ground_speed, clearance - j.rest_clearance,
                    j.climbed ? "climbed" : "not yet 4 m", std::asin(std::clamp(f[2], -1.0f, 1.0f)) * 57.2958f,
                    throttle);
      }
      if ((near_ground && (j.climbed || now - j.airborne_at > 5000)) ||
          (j.blocked_since && now - j.blocked_since > 800)) {
        const bool landed = near_ground;
        j.flying = false;
        const Quat level = QAxis(0, 0, 1, std::atan2(-f[0], f[1]));
        if (landed)
          Invoke(ctx, base, "SET_VEHICLE_QUATERNION", {*car, Bits(level.x), Bits(level.y), Bits(level.z), Bits(level.w)});
        Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {*car, Bits(landed ? j.speed : ground_speed)});
        Toast(s, landed ? (j.gear > 0.5f ? "F-16: belly landing!" : "F-16: touchdown.") : "F-16: crashed.", 1500);
        REXLOG_INFO("gta4-trainer: F-16 {} at {:.1f} m/s (moving at {:.1f}, {:.2f} m over the ground)",
                    landed ? "touched down" : "hit something", j.speed, ground_speed, clearance);
      } else {
        Invoke(ctx, base, "SET_VEHICLE_QUATERNION", {*car, Bits(j.q.x), Bits(j.q.y), Bits(j.q.z), Bits(j.q.w)});
        Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {*car, Bits(j.speed)});
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
    if (!j.r3_chord && !s.open) {
      if (!j.gear_up && !j.flying) {
        Toast(s, "F-16: the landing gear stays down on the ground.", 1500);
      } else {
        j.gear_up = !j.gear_up;
        Toast(s, j.gear_up ? "Gear up." : "Gear down.", 1200);
      }
    }
  }
  if (j.gear_up && !j.flying && ground_speed < kStallSpeed) {
    j.gear_up = false;
    Toast(s, "F-16: gear down.", 1200);
  }
  const float gear_before = j.gear;
  const float gear_step = dt / kGearSeconds;
  j.gear = j.gear_up ? std::min(1.0f, j.gear + gear_step) : std::max(0.0f, j.gear - gear_step);
  if (j.gear != gear_before || now >= j.next_gear_write) {
    PoseGear(ctx, base, *car, j.gear);
    j.next_gear_write = now + 250;
  }

  // The engine: idle in the jet, spooling up with the throttle; the
  // afterburner lights at full throttle once it is rolling.
  JetEngineSound(gta4::jet::EngineSynth::kIdle + (1.0f - gta4::jet::EngineSynth::kIdle) * throttle,
                 burner ? 1.0f : 0.0f, kJetVolume);
  UpdateFlame(ctx, base, j, *car, throttle, burner);

  uint32_t used = X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (j.flying)
    used |= X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER;  // the rudder
  if (s.jet) {
    used |= X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT;
    // Where the guns point: the nose. Airborne the flight state knows it; on
    // the ground the jet's own attitude.
    float fwd[3] = {game.forward[0], game.forward[1], game.forward[2]};
    Quat aim = j.q;
    if (j.flying) {
      const float nose[3] = {0, 1, 0};
      QRotate(j.q, nose, fwd);
    } else if (const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {*car, 0, 0, 0, 0}, 1, 0, 4)) {
      aim = QNormalize({q->f(0), q->f(1), q->f(2), q->f(3)});
      const float nose[3] = {0, 1, 0};
      QRotate(aim, nose, fwd);
    }
    if (const auto nose = CarPoint(ctx, base, *car, kGun))
      UpdateLock(ctx, base, j, s, nose->data(), fwd, now);
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
      Toast(s, "Destination set.", 1000);
      break;
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
        Toast(s, "Vehicle repaired.");
      } else {
        Toast(s, "Not in a vehicle.");
      }
      break;
    case Action::kToggleWarpIn:
      s.warp_in = !s.warp_in;
      break;
    case Action::kToggleGadgets:
      s.gadgets = !s.gadgets;
      Toast(s, s.gadgets ? "Batmobile gadgets armed: L1 lock-on missile; d-pad left guns ahead, down guns "
                           "behind, right boost; horn (L3) plants a bomb, again to set it off."
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
      Toast(s, s.jet ? "F-16 flight and weapons on." : "F-16 flight and weapons off.");
      break;
    case Action::kSpawnVehicle:
      StartSpawn(s, item.index, ctx, base);
      break;
    case Action::kTeleportWaypoint:
      TeleportToWaypoint(ctx, base, game, s);
      break;
    case Action::kTeleportPreset: {
      const auto& t = kTeleports[size_t(item.index)];
      Teleport(ctx, base, game, t.x, t.y, std::nullopt, s);
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
      case Page::kMain: model.title = "LIBERTY TRAINER"; break;
      case Page::kPlayer: model.title = "PLAYER OPTIONS"; break;
      case Page::kVehicle: model.title = "VEHICLE OPTIONS"; break;
      case Page::kSpawn: model.title = "CAR SPAWNING"; break;
      case Page::kTeleport: model.title = "TELEPORTING"; break;
      case Page::kWorld: model.title = "TIME AND WEATHER"; break;
      case Page::kWeapons: model.title = "WEAPONS"; break;
      case Page::kChaos: model.title = "CHAOS"; break;
      case Page::kStatus: model.title = "TRAINER STATUS"; break;
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
      constexpr int kWindow = 14;
      int top = 0;
      if (int(items.size()) > kWindow)
        top = std::clamp(s.selected - kWindow / 2, 0, int(items.size()) - kWindow);
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
        }
        model.lines.push_back({std::move(text), i == s.selected, false});
      }
      model.footer = s.page == Page::kMain ? "X select    O close    L3+R3 open/close"
                                           : "X select    O back    < > change";
    }
  }
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
        // Everything else runs where natives are safe.
        default: g_pending.push_back({item.action, item.index}); break;
      }
    }
    if (g_pad.Pressed(buttons, X_INPUT_GAMEPAD_B, now)) {
      if (s.page == Page::kMain)
        s.open = false;
      else {
        s.page = Page::kMain;
        s.selected = 0;
      }
    }
    // No gadget input while the menu owns the pad.
    g_gadget_buttons.store(0, std::memory_order_relaxed);
    g_gadget_previous.store(0, std::memory_order_relaxed);
    std::lock_guard lock(g_drive_mutex);
    g_drive_previous = g_drive_pad = DrivePad{};
  } else {
    g_gadget_previous.store(g_gadget_buttons.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    g_gadget_buttons.store(buttons, std::memory_order_relaxed);
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
  const bool jet_due = g_have_f16 && (g_jet.in_jet || missiles_live || now >= g_jet.next_probe);
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
  const bool lazar_ramps_due = g_have_lazar && (CanopyBusy(g_lazar_ramps) || now >= g_lazar_ramps.next_probe);
  const bool lazar_due = g_have_lazar && (g_lazar.in_car || LazarShellsLive() || now >= g_lazar.next_probe);
  const bool tm_due = HaveTimeMachines() && (g_tm.in_car || g_tm.phase != 0 || g_tm.fires_out || g_tm.vents_out ||
                                             g_tm.frost_until || now >= g_tm.next_probe || g_flash_until > now);
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
  const bool dmc_engine_due = engine_due(g_have_dmc12, g_dmc_engine);
  const bool bttf1_engine_due = engine_due(g_have_bttf1, g_bttf1_engine);
  const bool bttf2_engine_due = engine_due(g_have_bttf2, g_bttf2_engine);
  const bool bttf3_engine_due = engine_due(g_have_bttf3, g_bttf3_engine);
  const bool any_engine_due = bat_engine_due || deimos_engine_due || nexus_engine_due || arbiter_engine_due ||
                              dominator_engine_due || lazar_engine_due || gauntlet_engine_due || dmc_engine_due ||
                              bttf1_engine_due || bttf2_engine_due || bttf3_engine_due;
  const bool rain_due = s.raining && now >= g_next_rain;
  const bool teleport_due = g_teleport.active;
  if (g_pending.empty() && !god_due && !wanted_due && !car_god_due && !gadgets_due && !canopy_due &&
      !jet_canopy_due && !teleport_due && !any_engine_due && s.pending_model < 0 && !jet_due && !rain_due &&
      !popup_due && !nexus_popup_due && !doors_due && !lights_due && !drive_due && !lazar_ramps_due && !lazar_due &&
      !tm_due && g_addons_probed)
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
  if (!g_addons_probed) {
    g_addons_probed = true;
    if (gta4::addons::ShadowsSkippedThisLaunch())
      Toast(s, "Add-on cars are off this launch: the last launch with them did not start. "
               "Details in the log.", 8000);
    auto installed = [&](std::string_view model) {
      const auto known = Invoke(ctx, base, "IS_MODEL_IN_CDIMAGE", {ModelHash(model)});
      return known && known->r0() != 0;
    };
    g_have_deimos = installed("DEIMOS");
    g_have_f16 = installed("F16");
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
    REXLOG_INFO("gta4-trainer: add-ons present: Batmobile {}, F-16 {}, Deimos {}, Dominator 645 {}, Nexus {}, "
                "Arbiter X {}, Lazar 3 {}, Gauntlet Classic {}, DMC-12 {}, time machines {}/{}/{} (flying form {})",
                g_have_batmobile, g_have_f16, g_have_deimos, g_have_dominator, g_have_nexus, g_have_arbiter,
                g_have_lazar, g_have_gauntlet, g_have_dmc12, g_have_bttf1, g_have_bttf2, g_have_bttf3, g_have_bttf2h);
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

void DrawTcd(ImGuiIO& io) {
  TcdModel t;
  {
    std::lock_guard lock(g_tcd_mutex);
    t = g_tcd;
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
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

class TrainerOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit TrainerOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  // (the time circuits' display blinks and the flash fades: drawn every frame then)
  bool WantsContinuousRepaint() const override { return g_tcd_active.load(std::memory_order_relaxed); }
  void OnDraw(ImGuiIO& io) override {
    if (g_tcd_active.load(std::memory_order_relaxed))
      DrawTcd(io);
    DrawModel model;
    {
      std::lock_guard lock(g_draw_mutex);
      model = g_draw;
    }
    if (!model.open && model.toast.empty())
      return;
    // The look leans on GTA IV's own menus: a dark sheet, an amber header
    // band, a filled highlight bar on the selected row.
    const float scale = std::max(1.0f, io.DisplaySize.y / 1080.0f);
    const ImU32 kSheet = IM_COL32(12, 12, 14, 230);
    const ImU32 kBand = IM_COL32(255, 159, 14, 255);
    const ImU32 kBandText = IM_COL32(16, 12, 4, 255);
    const ImU32 kText = IM_COL32(235, 235, 235, 255);
    const ImU32 kDim = IM_COL32(150, 150, 150, 255);
    const ImU32 kSelBar = IM_COL32(255, 159, 14, 235);
    const ImU32 kSelText = IM_COL32(20, 14, 2, 255);
    const ImU32 kToastText = IM_COL32(150, 240, 150, 255);
    const ImU32 kEdge = IM_COL32(255, 159, 14, 90);

    ImGui::SetNextWindowPos(ImVec2(56.0f * scale, 108.0f * scale), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    if (ImGui::Begin("##liberty_trainer", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBackground)) {
      ImGui::SetWindowFontScale(1.3f * scale);
      const float pad = 16.0f * scale;
      const float width = 440.0f * scale;
      const float line_h = ImGui::GetTextLineHeight() + 7.0f * scale;
      const float band_h = line_h + 10.0f * scale;
      auto* draw = ImGui::GetWindowDrawList();
      const ImVec2 origin = ImGui::GetCursorScreenPos();

      float body_h = 0;
      if (model.open) {
        body_h = band_h + 8.0f * scale + float(model.lines.size()) * line_h +
                 (model.footer.empty() ? 0.0f : line_h + 6.0f * scale) + pad;
      }
      const bool has_toast = !model.toast.empty();
      const float toast_h = has_toast ? line_h + 8.0f * scale : 0.0f;
      const float total_h = body_h + (model.open && has_toast ? 6.0f * scale : 0.0f) + toast_h;

      if (model.open) {
        // Sheet and edge.
        draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + body_h), kSheet,
                            8.0f * scale);
        draw->AddRect(origin, ImVec2(origin.x + width, origin.y + body_h), kEdge, 8.0f * scale,
                      0, 1.5f);
        // Header band.
        draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + band_h), kBand,
                            8.0f * scale, ImDrawFlags_RoundCornersTop);
        draw->AddText(ImVec2(origin.x + pad, origin.y + 5.0f * scale), kBandText,
                      model.title.c_str());
        // Rows.
        float y = origin.y + band_h + 8.0f * scale;
        for (const auto& line : model.lines) {
          if (line.selected) {
            draw->AddRectFilled(ImVec2(origin.x + 6.0f * scale, y),
                                ImVec2(origin.x + width - 6.0f * scale, y + line_h - 2.0f * scale),
                                kSelBar, 4.0f * scale);
            draw->AddText(ImVec2(origin.x + pad, y + 2.0f * scale), kSelText, line.text.c_str());
          } else {
            draw->AddText(ImVec2(origin.x + pad, y + 2.0f * scale),
                          line.dim ? kDim : kText, line.text.c_str());
          }
          y += line_h;
        }
        if (!model.footer.empty()) {
          draw->AddLine(ImVec2(origin.x + pad, y + 2.0f * scale),
                        ImVec2(origin.x + width - pad, y + 2.0f * scale), kEdge, 1.0f);
          draw->AddText(ImVec2(origin.x + pad, y + 7.0f * scale), kDim, model.footer.c_str());
        }
      }
      if (has_toast) {
        const float ty = origin.y + body_h + (model.open ? 6.0f * scale : 0.0f);
        const ImVec2 size = ImGui::CalcTextSize(model.toast.c_str());
        draw->AddRectFilled(ImVec2(origin.x, ty),
                            ImVec2(origin.x + size.x + pad * 2, ty + toast_h), kSheet,
                            6.0f * scale);
        draw->AddText(ImVec2(origin.x + pad, ty + 4.0f * scale), kToastText, model.toast.c_str());
      }
      // Reserve the drawn area so auto-resize fits it.
      ImGui::Dummy(ImVec2(width, total_h));
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
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
