// Liberty Trainer (see gta4_trainer.h). Modeled on Simple Native Trainer's
// menu: the same categories, driven with the pad, every action through the
// game's own script natives on the game's thread. The native-call plumbing
// follows gta4_quicksave_hooks.cpp, the one proven way this port calls
// natives from host code.
#include "gta4_trainer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
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
using rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
using rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
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
    // Chaos: raining cars, gravity.
    NativeEntry{"SET_CAR_HEADING"}, NativeEntry{"SET_GRAVITY_OFF"},
    // Grotti Deimos pop-up headlights (vehicle extras).
    NativeEntry{"TURN_OFF_VEHICLE_EXTRA"}, NativeEntry{"GET_TIME_OF_DAY"},
    NativeEntry{"GET_CURRENT_WEATHER"},
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
                                   int output_count = 1) {
  const uint32_t address = NativeAddress(name);
  auto* runtime = rex::Runtime::instance();
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  auto* function = dispatcher && address ? dispatcher->GetFunction(address) : nullptr;
  if (!function || values.size() > 8 || parent.r1.u32 < 0x200)
    return std::nullopt;
  const uint32_t frame = (parent.r1.u32 - 0x200u) & ~15u;
  if (!Span(base, frame, 0x200, true))
    return std::nullopt;
  const uint32_t context = frame + 0x80, result = frame + 0xC0, arguments = frame + 0xD0;
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
    const int k = int(i) - output_argument;
    const bool out = output_argument >= 0 && k >= 0 && k < std::min(output_count, 4);
    Write(base, arguments + uint32_t(i) * 4, out ? result + uint32_t(k) * 4 : value);
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

enum class Page { kMain, kPlayer, kVehicle, kSpawn, kTeleport, kWorld, kWeapons, kChaos, kStatus };
enum class Action {
  kNone, kOpenPlayer, kOpenVehicle, kOpenSpawn, kOpenTeleport, kOpenWorld,
  kOpenWeapons, kOpenChaos, kOpenStatus,
  kToggleRain, kToggleGravity,
  kToggleGod, kToggleNeverWanted, kWantedUp, kWantedDown, kWantedApply,
  kMoney, kHeal,
  kToggleVehicleGod, kRepair, kToggleWarpIn, kToggleGadgets, kToggleJet,
  kCyclePopups, kTogglePopupTilt, kToggleAnyRide,
  kSpawnVehicle, kTeleportWaypoint, kTeleportPreset, kSavePosition, kLoadPosition,
  kHourUp, kHourDown, kMinuteUp, kMinuteDown, kTimeApply,
  kWeatherNext, kWeatherPrevious, kWeatherApply, kWeatherRelease,
  kGiveLoadout, kRemoveWeapons,
};
struct Item {
  std::string_view label;
  Action action = Action::kNone;
  int index = -1;  // list index for spawn/teleport pages
};

// Shared trainer state: written on the game thread, drawn on the UI thread.
struct State {
  bool open = false;
  Page page = Page::kMain;
  int selected = 0;
  int spawn_top = 0;
  // Toggles.
  bool god = false, never_wanted = false, vehicle_god = false, warp_in = true;
  bool gadgets = false;
  bool jet = true;  // F-16 flight and weapons, active only in the jet
  bool raining = false, gravity_off = false;
  bool any_ride = false;     // Bring Your Own Ride: missions take the player's vehicle
  int popup_mode = 0;        // Deimos pop-ups: 0 with the lights, 1 always up, 2 always down
  bool popup_flip = false;   // which way they tilt up (the model's two up poses)
  int wanted_target = 0;
  int hour = 12, minute = 0, weather = 1;
  // Async vehicle spawn.
  int pending_model = -1;
  uint32_t pending_hash = 0;
  int pending_ticks = 0;
  uint64_t pending_since = 0;
  uint64_t pending_loaded_at = 0, pending_ready_since = 0;  // the guard's settle wait
  int pending_stable = 0;
  bool pending_logged = false;
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
bool g_addons_probed = false, g_have_deimos = false, g_have_f16 = false;
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
         s.raining || s.pending_model >= 0 || !g_addons_probed || (s.jet && g_have_f16) ||
         g_have_deimos;
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
               {s.gadgets ? "Batmobile Gadgets: ON (dpad left: guns, right: boost)"
                          : "Batmobile Gadgets: OFF",
                Action::kToggleGadgets},
               {s.jet ? "F-16 Flight and Weapons: ON" : "F-16 Flight and Weapons: OFF",
                Action::kToggleJet},
               {s.popup_mode == 0   ? "Deimos Pop-up Lights: With Headlights"
                : s.popup_mode == 1 ? "Deimos Pop-up Lights: Always Up"
                                    : "Deimos Pop-up Lights: Always Down",
                Action::kCyclePopups},
               {s.popup_flip ? "Deimos Pop-up Tilt: B" : "Deimos Pop-up Tilt: A",
                Action::kTogglePopupTilt},
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

void Teleport(PPCContext& ctx, uint8_t* base, const GameContext& game, float x, float y,
              std::optional<float> z_hint, State& s) {
  float z = z_hint.value_or(60.0f);
  const auto ground =
      Invoke(ctx, base, "GET_GROUND_Z_FOR_3D_COORD", {Bits(x), Bits(y), Bits(400.0f), 0}, 3);
  if (ground) {
    const float gz = ground->f(0);
    if (std::isfinite(gz) && gz > 0.5f)
      z = gz + 1.0f;
  }
  Invoke(ctx, base, "REQUEST_COLLISION_AT_POSN", {Bits(x), Bits(y), Bits(z)});
  Invoke(ctx, base, "SET_CHAR_COORDINATES", {game.ped, Bits(x), Bits(y), Bits(z)});
  Toast(s, "Teleported.");
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
  Toast(s, std::string("Loading ") + std::string(choice.label) + "...", 20000);
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

// The Grotti Deimos's pop-up headlights: the model carries its lamp pods in
// three poses on the template's extras (1: down, 2 and 5: up, tilted one way
// or the other); the game shows or hides extras, the trainer picks the pose.
// Up when the game would have headlights on (night, rain, fog), or as set.
struct PopupState {
  uint32_t car = 0;
  int pose = 0;
  bool in_car = false;
  uint64_t next_probe = 0, next_check = 0;
};
PopupState g_popup;
constexpr std::array<int, 3> kPopupExtras = {1, 2, 5};

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

int WantedPopupPose(PPCContext& ctx, uint8_t* base, const State& s) {
  const bool up = s.popup_mode == 1 || (s.popup_mode == 0 && HeadlightsWanted(ctx, base));
  return up ? (s.popup_flip ? 5 : 2) : 1;
}

void SetPopupPose(PPCContext& ctx, uint8_t* base, uint32_t car, int pose) {
  for (const int extra : kPopupExtras)
    Invoke(ctx, base, "TURN_OFF_VEHICLE_EXTRA", {car, uint32_t(extra), extra == pose ? 0u : 1u});
}

void PopupLights(PPCContext& ctx, uint8_t* base, const GameContext& game, const State& s) {
  if (!g_have_deimos)
    return;
  const uint64_t now = NowMs();
  if (!g_popup.in_car && now < g_popup.next_probe)
    return;
  g_popup.next_probe = now + 400;
  if (g_popup.in_car && now < g_popup.next_check)
    return;
  g_popup.next_check = now + 250;
  static const uint32_t kDeimos = ModelHash("DEIMOS");
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  if (!vehicle.car || vehicle.model != kDeimos) {
    g_popup.in_car = false;
    return;
  }
  const uint32_t car = *vehicle.car;
  const int pose = WantedPopupPose(ctx, base, s);
  if (!g_popup.in_car || g_popup.car != car || g_popup.pose != pose) {
    SetPopupPose(ctx, base, car, pose);
    g_popup.car = car;
    g_popup.pose = pose;
  }
  g_popup.in_car = true;
}

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
    // Big models (the Batmobile is ~10 MB) can take a while to stream in.
    if (now - s.pending_since > 20000) {
      REXLOG_WARN("gta4-trainer: {} (hash {:08X}) did not load within 20 s", model, s.pending_hash);
      rex::FlushLogging();
      give_up("The model did not load.");
    }
    return;
  }
  if (!s.pending_loaded_at)
    s.pending_loaded_at = now;
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
    if (s.pending_hash == ModelHash("DEIMOS"))  // extras come up random: set the pods
      SetPopupPose(ctx, base, handle, WantedPopupPose(ctx, base, s));
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

// JulioNIB's Batmobile gadgets, redone through natives (the PC script's .NET
// plugins cannot run here). Active only with the toggle on and the player
// driving the Batmobile. Returns whether the player is in it (its d-pad
// buttons are then the gadgets', not the radio's).
bool Gadgets(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s,
             uint16_t buttons, uint16_t previous) {
  if (!s.gadgets)
    return false;
  static const uint32_t kBatmobile = ModelHash("BATMOBILE");
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  if (!vehicle.car || vehicle.model != kBatmobile)
    return false;
  const auto car = vehicle.car;
  const float* f = game.forward;
  const float* p = game.position;
  // Front guns: a short explosive volley ahead of the car.
  if ((buttons & X_INPUT_GAMEPAD_DPAD_LEFT) && !(previous & X_INPUT_GAMEPAD_DPAD_LEFT)) {
    for (float ahead : {14.0f, 22.0f}) {
      const float x = p[0] + f[0] * ahead, y = p[1] + f[1] * ahead, z = p[2] + 0.4f;
      // type 2, radius 1.0, audible, visible, normal camera shake
      Invoke(ctx, base, "ADD_EXPLOSION",
             {Bits(x), Bits(y), Bits(z), 2, Bits(1.0f), 1, 0, Bits(1.0f)});
    }
  }
  // Afterburner: hold dpad right.
  if (buttons & X_INPUT_GAMEPAD_DPAD_RIGHT) {
    float speed = 0.0f;
    if (const auto current = Invoke(ctx, base, "GET_CAR_SPEED", {*car, 0}, 1))
      speed = current->f(0);
    const float target = std::min(speed + 3.0f, 55.0f);
    Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {*car, Bits(target)});
  }
  return true;
}

// ---------------------------------------------------------------------------
// The F-16: flight, cannon and missiles. On PC the jet flies through
// JulioNIB's Air Combat IV script (.NET), which cannot run here; this
// re-creates its core through natives. Arcade flight: once airborne, the
// jet's attitude (a quaternion steered by the left stick, banking turns it)
// and its speed along the nose are set every frame, so physics still
// handles collisions. The nose cannon fires hitscan bullets; missiles are
// rocket objects flown ahead that explode where they meet the ground or a
// roof, or at the end of their range.
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
  uint32_t object = 0;
  float pos[3]{}, dir[3]{};
  Quat q;
  float speed = 0, travelled = 0;
};
struct JetState {
  bool flying = false;
  uint32_t car = 0;
  Quat q;
  float speed = 0;
  uint64_t last_ms = 0, next_shot = 0, next_probe = 0;
  bool in_jet = false, announced = false, rocket_requested = false;
  std::array<Missile, 6> missiles{};
};
JetState g_jet;
constexpr float kTakeoffSpeed = 26.0f;  // m/s (~95 km/h): pull back to rotate
constexpr float kStallSpeed = 16.0f;    // below this the jet stops flying
constexpr float kMaxJetSpeed = 105.0f;  // m/s (~380 km/h)
constexpr std::string_view kRocketModel = "cj_rpg_rocket";

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
  if (object)
    Invoke(ctx, base, "DELETE_OBJECT", {0}, 0, object);
  object = 0;
}

// Missiles keep flying after the player leaves the jet.
void UpdateMissiles(PPCContext& ctx, uint8_t* base, JetState& j, float dt) {
  for (auto& m : j.missiles) {
    if (!m.live)
      continue;
    const float step = m.speed * dt;
    for (int a = 0; a < 3; ++a)
      m.pos[a] += m.dir[a] * step;
    m.travelled += step;
    const auto gz = GroundZ(ctx, base, m.pos[0], m.pos[1], m.pos[2] + 2.0f);
    const bool hit = gz && m.pos[2] <= *gz + 0.6f;
    if (hit || m.travelled > 800.0f || !std::isfinite(m.pos[2])) {
      if (hit)
        m.pos[2] = *gz + 0.6f;
      Explode(ctx, base, m.pos, 6.0f);
      DeleteObject(ctx, base, m.object);
      m.live = false;
      continue;
    }
    if (m.object)
      Invoke(ctx, base, "SET_OBJECT_COORDINATES",
             {m.object, Bits(m.pos[0]), Bits(m.pos[1]), Bits(m.pos[2])});
  }
}

void LaunchMissile(PPCContext& ctx, uint8_t* base, JetState& j, const float pos[3],
                   const float fwd[3], const Quat& q, State& s) {
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
  Missile& m = *slot;
  m = {};
  m.live = true;
  for (int a = 0; a < 3; ++a) {
    m.dir[a] = fwd[a];
    m.pos[a] = pos[a] + fwd[a] * 11.0f;  // clear of the nose (8.4 m)
  }
  m.pos[2] -= 0.8f;
  m.q = q;
  m.speed = std::max(j.speed, 0.0f) + 70.0f;
  // The rocket you see: the RPG's projectile model, moved by hand.
  const uint32_t hash = ModelHash(kRocketModel);
  const auto loaded = Invoke(ctx, base, "HAS_MODEL_LOADED", {hash});
  if (loaded && loaded->r0()) {
    const auto obj = Invoke(ctx, base, "CREATE_OBJECT",
                            {hash, Bits(m.pos[0]), Bits(m.pos[1]), Bits(m.pos[2]), 0, 1}, 4);
    if (obj && obj->words[0]) {
      m.object = obj->words[0];
      Invoke(ctx, base, "SET_OBJECT_COLLISION", {m.object, 0});
      Invoke(ctx, base, "FREEZE_OBJECT_POSITION", {m.object, 1});
      Invoke(ctx, base, "SET_OBJECT_QUATERNION",
             {m.object, Bits(q.x), Bits(q.y), Bits(q.z), Bits(q.w)});
    }
  }
}

void FireCannon(PPCContext& ctx, uint8_t* base, const float pos[3], const float fwd[3]) {
  float from[3], to[3];
  for (int a = 0; a < 3; ++a) {
    from[a] = pos[a] + fwd[a] * 10.0f;
    to[a] = pos[a] + fwd[a] * 400.0f;
  }
  // A slight scatter, as a rotary cannon has.
  static uint32_t seed = 0x1234567u;
  for (int a = 0; a < 3; ++a) {
    seed = seed * 1664525u + 1013904223u;
    to[a] += (float((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 3.0f;
  }
  if (NativeKnown("FIRE_SINGLE_BULLET")) {
    Invoke(ctx, base, "FIRE_SINGLE_BULLET",
           {Bits(from[0]), Bits(from[1]), Bits(from[2]), Bits(to[0]), Bits(to[1]), Bits(to[2]), 50});
    return;
  }
  // Without the bullet native: small blasts where the line of fire meets the ground.
  for (float d = 20.0f; d <= 400.0f; d += 20.0f) {
    float p[3] = {pos[0] + fwd[0] * d, pos[1] + fwd[1] * d, pos[2] + fwd[2] * d};
    const auto gz = GroundZ(ctx, base, p[0], p[1], p[2] + 1.0f);
    if (gz && p[2] <= *gz + 0.5f) {
      p[2] = *gz + 0.3f;
      Explode(ctx, base, p, 0.5f);
      return;
    }
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

// Returns whether the player is in the jet (its d-pad is then the weapons').
bool JetControls(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s,
                 const DrivePad& pad, const DrivePad& previous) {
  auto& j = g_jet;
  const uint64_t now = NowMs();
  const float dt = j.last_ms ? std::clamp(float(now - j.last_ms) / 1000.0f, 0.0f, 0.05f) : 0.0f;
  j.last_ms = now;
  UpdateMissiles(ctx, base, j, dt);
  // Who is driving what: checked every frame in the jet, a few times a
  // second otherwise.
  if (!j.in_jet && now < j.next_probe)
    return false;
  j.next_probe = now + 400;
  static const uint32_t kF16 = ModelHash("F16");
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  const auto car = vehicle.car;
  const bool in_jet = car && vehicle.model == kF16;
  if (!in_jet) {
    if (j.in_jet && j.rocket_requested) {
      Invoke(ctx, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {ModelHash(kRocketModel)});
      j.rocket_requested = false;
    }
    j.in_jet = j.flying = false;
    j.announced = false;
    j.car = 0;
    return false;
  }
  if (!j.in_jet || j.car != *car) {
    j.flying = false;
    j.car = *car;
  }
  j.in_jet = true;
  if (!j.rocket_requested) {
    Invoke(ctx, base, "REQUEST_MODEL", {ModelHash(kRocketModel)});
    j.rocket_requested = true;
  }
  if (!j.announced) {
    j.announced = true;
    Toast(s, "F-16: RT throttle, pull back at speed to take off. Dpad right: cannon, left: missile.",
          6000);
  }
  float ground_speed = 0.0f;
  if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {*car, 0}, 1))
    ground_speed = v->f(0);
  const float lx = Axis(pad.lx), ly = Axis(pad.ly);
  const float throttle = pad.right_trigger / 255.0f, airbrake = pad.left_trigger / 255.0f;

  if (!j.flying && ground_speed > kTakeoffSpeed && ly < -0.4f) {
    const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {*car, 0, 0, 0, 0}, 1, 0, 4);
    if (q && NativeKnown("SET_VEHICLE_QUATERNION")) {
      j.q = QNormalize({q->f(0), q->f(1), q->f(2), q->f(3)});
      j.speed = ground_speed;
      j.flying = true;
      REXLOG_INFO("gta4-trainer: F-16 airborne at {:.1f} m/s", ground_speed);
    }
  }
  if (j.flying) {
    j.speed = std::clamp(j.speed + (throttle * 20.0f - airbrake * 28.0f) * dt, 0.0f, kMaxJetSpeed);
    if (j.speed < kStallSpeed) {
      j.flying = false;  // physics takes over: glide down and land
      Toast(s, "F-16: too slow to fly.", 1500);
    } else {
      const float right_axis[3] = {1, 0, 0};
      float right[3];
      QRotate(j.q, right_axis, right);
      // A banked jet turns: right wing down (right.z < 0) turns right.
      j.q = QMul(QAxis(0, 0, 1, right[2] * 1.1f * dt), j.q);
      j.q = QMul(j.q, QAxis(1, 0, 0, -ly * 1.3f * dt));  // stick back: nose up
      j.q = QMul(j.q, QAxis(0, 1, 0, lx * 2.2f * dt));   // stick right: roll right
      j.q = QNormalize(j.q);
      Invoke(ctx, base, "SET_VEHICLE_QUATERNION",
             {*car, Bits(j.q.x), Bits(j.q.y), Bits(j.q.z), Bits(j.q.w)});
      Invoke(ctx, base, "SET_CAR_FORWARD_SPEED", {*car, Bits(j.speed)});
    }
  }

  // Where the guns point: the nose. Airborne the flight state knows it; on
  // the ground the player's own facing (seated, it follows the car).
  float pos[3] = {game.position[0], game.position[1], game.position[2]};
  if (const auto c = Invoke(ctx, base, "GET_CAR_COORDINATES", {*car, 0, 0, 0}, 1, 0, 3)) {
    if (std::isfinite(c->f(0)) && std::isfinite(c->f(1)) && std::isfinite(c->f(2)))
      pos[0] = c->f(0), pos[1] = c->f(1), pos[2] = c->f(2);
  }
  float fwd[3] = {game.forward[0], game.forward[1], game.forward[2]};
  Quat aim = j.q;
  if (j.flying) {
    const float nose[3] = {0, 1, 0};
    QRotate(j.q, nose, fwd);
  } else if (const auto q = Invoke(ctx, base, "GET_VEHICLE_QUATERNION", {*car, 0, 0, 0, 0}, 1, 0, 4)) {
    aim = QNormalize({q->f(0), q->f(1), q->f(2), q->f(3)});
  }
  if ((pad.buttons & X_INPUT_GAMEPAD_DPAD_RIGHT) && now >= j.next_shot) {
    j.next_shot = now + 70;
    FireCannon(ctx, base, pos, fwd);
  }
  if ((pad.buttons & X_INPUT_GAMEPAD_DPAD_LEFT) && !(previous.buttons & X_INPUT_GAMEPAD_DPAD_LEFT))
    LaunchMissile(ctx, base, j, pos, fwd, aim, s);
  return true;
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
      Toast(s, s.gadgets ? "Gadgets armed (in the Batmobile: dpad left guns, right boost)."
                         : "Gadgets off.");
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
    case Action::kCyclePopups:
      s.popup_mode = (s.popup_mode + 1) % 3;
      g_popup.in_car = false;  // re-apply on the next look
      Toast(s, s.popup_mode == 0   ? "Deimos pop-ups follow the headlights."
               : s.popup_mode == 1 ? "Deimos pop-ups always up."
                                   : "Deimos pop-ups always down.");
      break;
    case Action::kTogglePopupTilt:
      s.popup_flip = !s.popup_flip;
      g_popup.in_car = false;
      Toast(s, "Deimos pop-ups tilt the other way.");
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
  const bool jet_due =
      s.jet && g_have_f16 && (g_jet.in_jet || missiles_live || now >= g_jet.next_probe);
  const bool popup_due = g_have_deimos && (g_popup.in_car || now >= g_popup.next_probe);
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
  const bool gadgets_due = s.gadgets && (s_in_batmobile || now >= s_next_gadget_look);
  const bool rain_due = s.raining && now >= g_next_rain;
  if (g_pending.empty() && !god_due && !wanted_due && !car_god_due && !gadgets_due &&
      s.pending_model < 0 && !jet_due && !rain_due && !popup_due && !drive_due &&
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
  if (!g_addons_probed) {
    g_addons_probed = true;
    auto installed = [&](std::string_view model) {
      const auto known = Invoke(ctx, base, "IS_MODEL_IN_CDIMAGE", {ModelHash(model)});
      return known && known->r0() != 0;
    };
    g_have_deimos = installed("DEIMOS");
    g_have_f16 = installed("F16");
    REXLOG_INFO("gta4-trainer: add-ons present: Batmobile {}, F-16 {}, Deimos {}",
                installed("BATMOBILE"), g_have_f16, g_have_deimos);
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
    s_in_batmobile = Gadgets(ctx, base, game, s, buttons, previous);
    if (s_in_batmobile)
      vehicle_mask |= X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT;
    // Consume the edge so the second script-stage site cannot double-fire.
    g_gadget_previous.store(buttons, std::memory_order_relaxed);
  } else if (!s.gadgets) {
    s_in_batmobile = false;
  }
  if (jet_due) {
    DrivePad pad, previous;
    {
      std::lock_guard lock(g_drive_mutex);
      pad = g_drive_pad;
      previous = g_drive_previous;
      g_drive_previous = g_drive_pad;  // consume edges (two script-stage sites)
    }
    if (JetControls(ctx, base, game, s, pad, previous))
      vehicle_mask |= X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT;
  }
  g_vehicle_mask.store(vehicle_mask, std::memory_order_relaxed);
  if (rain_due)
    RainCars(ctx, base, game);
  if (popup_due)
    PopupLights(ctx, base, game, s);
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
class TrainerOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit TrainerOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  bool WantsContinuousRepaint() const override { return false; }
  void OnDraw(ImGuiIO& io) override {
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
