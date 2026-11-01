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
#include "gta4_jet_engine.h"

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
    // Batmobile missile lock: the target's mark on the radar.
    NativeEntry{"ADD_BLIP_FOR_CAR"}, NativeEntry{"CHANGE_BLIP_COLOUR"}, NativeEntry{"REMOVE_BLIP"},
    // Chaos: raining cars, gravity.
    NativeEntry{"SET_CAR_HEADING"}, NativeEntry{"SET_GRAVITY_OFF"},
    // Grotti Deimos pop-up headlights (the pods' extra, the lamps).
    NativeEntry{"TURN_OFF_VEHICLE_EXTRA"}, NativeEntry{"IS_VEHICLE_EXTRA_TURNED_ON"},
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
                                   std::string_view text = {}) {
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
    const int k = int(i) - output_argument;
    const bool out = output_argument >= 0 && k >= 0 && k < std::min(output_count, 4);
    Write(base, arguments + uint32_t(i) * 4,
          int(i) == text_argument ? text_at : (out ? result + uint32_t(k) * 4 : value));
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
  kTogglePopupAuto, kToggleAnyRide, kToggleCarLights,
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
  // Every option starts off: the menu changes nothing until asked to.
  bool god = false, never_wanted = false, vehicle_god = false, warp_in = false;
  bool gadgets = false;
  bool jet = false;  // F-16 flight and weapons, active only in the jet
  bool raining = false, gravity_off = false;
  bool any_ride = false;     // Bring Your Own Ride: missions take the player's vehicle
  bool popup_auto = false;   // Deimos pop-ups: d-pad left (false) or up with the lights (true)
  bool car_lights = false;   // any car's headlights on d-pad left
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
         g_have_batmobile ||
         s.car_lights;
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
               {s.gadgets ? "Batmobile Gadgets: ON (L1 missile, d-pad guns/boost, horn bomb)"
                          : "Batmobile Gadgets: OFF",
                Action::kToggleGadgets},
               {s.jet ? "F-16 Flight and Weapons: ON" : "F-16 Flight and Weapons: OFF",
                Action::kToggleJet},
               {s.popup_auto ? "Deimos Pop-ups: Automatic (with the lights)"
                             : "Deimos Pop-ups: D-pad Left",
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

// The Grotti Deimos's pop-up headlights. The converter hangs both lamp pods
// (housings, lamps and lenses) on the template's extra_1, placed on their
// hinge at the back of the pods, flush with the bonnet at rest. The trainer
// swings that bone the way the game itself poses a bone: the bone's local
// matrix in the car's live skeleton (what sub_822832E8 hands the game when
// it hides an extra), a rotation about the car's width, front edge rising.
// D-pad left in the car raises or lowers them, the headlights following; or
// they rise whenever IV turns car lights on (night, rain, fog).
struct PopupState {
  uint32_t car = 0;
  bool in_car = false;
  bool up = false;          // where they are heading
  float angle = 0.0f;       // where they are, degrees
  uint16_t prev_buttons = 0;
  uint64_t next_probe = 0, next_check = 0, last_ms = 0, next_write = 0;
};
PopupState g_popup;
constexpr uint32_t kPopupExtra = 1;          // extra_1 holds the pods
constexpr float kPopupUpDegrees = 30.0f;     // ZolikaPatch's angle for this mod on PC
constexpr float kPopupDegreesPerSecond = 85.0f;  // up in about a third of a second
constexpr uint32_t kEntitySkeleton = 0x8217DB60;  // entity -> its live crSkeleton

// Bones the trainer poses: the Deimos's pop-up pods (slot 0), the F-16's
// landing gear (slot 1, three bones) and the Batmobile's canopy (slot 2). The
// pose last set for each is
// re-applied by the per-frame hook below (sub_82381FA0), right after the
// game's own per-frame bone edits for that car.
struct BonePose {
  int32_t bone = -1;
  std::array<uint32_t, 16> words{};
};
struct PosedVehicle {
  uint32_t vehicle = 0, info = 0;
  std::array<BonePose, 4> bones{};
};
enum PoseSlot : int { kPoseDeimos = 0, kPoseF16 = 1, kPoseBatmobile = 2 };
std::mutex g_pose_mutex;
std::array<PosedVehicle, 3> g_posed;
std::array<std::atomic<uint32_t>, 3> g_posed_vehicle{};  // the hook's quick check

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

bool PoseBone(PPCContext& ctx, uint8_t* base, uint32_t vehicle, uint32_t info, int slot, int entry,
              int32_t bone, int axis, float degrees, float scale) {
  const uint32_t local = PopupLocalSlot(ctx, base, vehicle, bone);
  const auto skeleton = local ? CallGuest(ctx, base, kEntitySkeleton, vehicle) : std::nullopt;
  if (!local || !skeleton)
    return false;
  const uint32_t data = Read(base, *skeleton + 4);
  if (!Span(base, data, 24))
    return false;
  // The bone's rest local matrix (skeleton data +16: rotation rows, then the
  // translation row: the hinge), turned about its own X axis.
  const uint32_t rest = Read(base, data + 16) + 64 * uint32_t(bone);
  if (!Span(base, rest, 64))
    return false;
  // Row vectors (v' = v M): about X, y turns towards z (a front edge
  // rising, a wheel swinging aft and up for a negative angle); about Y, z
  // turns towards x.
  const float rad = degrees * 3.14159265f / 180.0f, c = std::cos(rad), s = std::sin(rad);
  const float turn_x[3][3] = {{1, 0, 0}, {0, c, s}, {0, -s, c}};
  const float turn_y[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
  const auto& turn = axis == 1 ? turn_y : turn_x;
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
  for (uint32_t w = 0; w < 4; ++w)  // the hinge stays put
    words[12 + w] = Read(base, rest + 48 + 4 * w);
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

bool PosePopupBone(PPCContext& ctx, uint8_t* base, uint32_t handle, float degrees) {
  static const uint32_t kDeimos = ModelHash("DEIMOS");
  return PoseExtraBone(ctx, base, handle, kDeimos, kPoseDeimos, 0, kPopupExtra, 0, degrees, 1.0f);
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

// A Deimos just made (or first met): its pods' extra shown, pods down.
void PreparePopups(PPCContext& ctx, uint8_t* base, uint32_t car, bool getting_in = false) {
  ShowExtra(ctx, base, car, kPopupExtra, getting_in);
  g_popup.car = car;
  g_popup.up = false;
  g_popup.angle = 0.0f;
  g_popup.next_write = 0;
  PosePopupBone(ctx, base, car, 0.0f);
}

// Every frame in the Deimos (a look a few times a second otherwise). Returns
// whether the player is in it with the d-pad switch on (d-pad left is then
// the pods', not the radio's).
bool PopupLights(PPCContext& ctx, uint8_t* base, const GameContext& game, const State& s,
                 uint16_t buttons) {
  if (!g_have_deimos)
    return false;
  const uint64_t now = NowMs();
  if (!g_popup.in_car && now < g_popup.next_probe)
    return false;
  if (!g_popup.in_car || now >= g_popup.next_check) {
    g_popup.next_probe = now + 400;
    g_popup.next_check = now + 250;
    static const uint32_t kDeimos = ModelHash("DEIMOS");
    const auto& vehicle = PlayerVehicle(ctx, base, game);
    if (!vehicle.car || vehicle.model != kDeimos) {
      g_popup.in_car = false;
      return false;
    }
    const uint32_t car = *vehicle.car;
    if (!g_popup.in_car || g_popup.car != car) {
      if (g_popup.car != car)
        PreparePopups(ctx, base, car, true);
      else  // back in the same car: make sure the pods' extra is still shown
        ShowExtra(ctx, base, car, kPopupExtra, true);
      g_popup.prev_buttons = buttons;  // no edge from a button held while getting in
      g_popup.last_ms = now;
      SetPopupLights(ctx, base, car, g_popup.up, s.popup_auto);
    }
    if (s.popup_auto) {
      const bool want = HeadlightsWanted(ctx, base);
      if (want != g_popup.up)
        g_popup.up = want;
    }
    g_popup.in_car = true;
  }
  // The switch.
  const bool pressed = (buttons & X_INPUT_GAMEPAD_DPAD_LEFT) &&
                       !(g_popup.prev_buttons & X_INPUT_GAMEPAD_DPAD_LEFT);
  g_popup.prev_buttons = buttons;
  if (pressed && !s.popup_auto) {
    g_popup.up = !g_popup.up;
    SetPopupLights(ctx, base, g_popup.car, g_popup.up, false);
  }
  // The swing, eased at both ends; the pose is re-applied now and then while
  // settled, in case the game reset the skeleton (a car streamed back in).
  const float dt = std::min<float>(float(now - g_popup.last_ms), 100.0f) / 1000.0f;
  g_popup.last_ms = now;
  const float target = g_popup.up ? kPopupUpDegrees : 0.0f;
  const float before = g_popup.angle;
  const float step = kPopupDegreesPerSecond * dt;
  g_popup.angle = std::fabs(target - before) <= step ? target : before + (target > before ? step : -step);
  if (g_popup.angle != before || now >= g_popup.next_write) {
    const float t = g_popup.angle / kPopupUpDegrees;
    const float eased = kPopupUpDegrees * t * t * (3.0f - 2.0f * t);
    PosePopupBone(ctx, base, g_popup.car, eased);
    g_popup.next_write = now + 250;
  }
  return !s.popup_auto;
}

// Headlights on d-pad left, in any car (an option, off by default). The cars
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
                          kBatmobile = ModelHash("BATMOBILE");
    const auto& vehicle = PlayerVehicle(ctx, base, game);
    const bool taken = vehicle.model == kDeimos || (vehicle.model == kF16 && s.jet) ||
                       (vehicle.model == kBatmobile && s.gadgets);
    if (!vehicle.car || taken) {
      g_lights.in_car = false;
      return false;
    }
    if (!g_lights.in_car || g_lights.car != *vehicle.car) {
      g_lights.car = *vehicle.car;
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
      PreparePopups(ctx, base, handle);
    if (s.pending_hash == ModelHash("F16"))  // ...and the jet's landing gear
      PrepareJet(ctx, base, handle);
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
  float pos[3]{}, dir[3]{};
  float speed = 0, travelled = 0;
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
};
JetState g_jet;
constexpr float kTakeoffSpeed = 26.0f;  // m/s (~95 km/h): pull back to rotate
constexpr float kStallSpeed = 16.0f;    // below this the jet stops flying
constexpr float kMaxJetSpeed = 105.0f;  // m/s (~380 km/h)
constexpr std::string_view kRocketModel = "cj_rpg_rocket";
// Where the mod's Air Combat IV config (ac-iv.ini) puts them, in the jet's
// own space: the cannon by the cockpit, missiles under the wings, the nozzle.
constexpr float kGun[3] = {0.74f, 3.29f, 0.55f};
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

// Missiles keep flying after the player leaves the jet.
void UpdateMissiles(PPCContext& ctx, uint8_t* base, JetState& j, float dt) {
  for (auto& m : j.missiles) {
    if (!m.live)
      continue;
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
    REXLOG_INFO("gta4-trainer: F-16 missile launched at {:.0f} m/s (rocket model {}, object {:08X}, trail {:08X})",
                m.speed, loaded && loaded->r0() ? "loaded" : "not loaded", m.object, m.trail);
  }
}

void FireCannon(PPCContext& ctx, uint8_t* base, JetState& j, const float fwd[3]) {
  const auto gun = CarPoint(ctx, base, j.car, kGun);
  if (!gun)
    return;
  float from[3], to[3];
  for (int a = 0; a < 3; ++a) {
    from[a] = (*gun)[a] + fwd[a] * 3.0f;
    to[a] = (*gun)[a] + fwd[a] * 900.0f;
  }
  // A slight scatter, as a rotary cannon has.
  static uint32_t seed = 0x1234567u;
  for (int a = 0; a < 3; ++a) {
    seed = seed * 1664525u + 1013904223u;
    to[a] += (float((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 5.0f;
  }
  Invoke(ctx, base, "FIRE_SINGLE_BULLET",
         {Bits(from[0]), Bits(from[1]), Bits(from[2]), Bits(to[0]), Bits(to[1]), Bits(to[2]), 60});
  const bool flash = (j.shots & 1) == 0 ? CarEffect(ctx, base, j.car, "muz_machine", kGun, 1.6f) : true;
  // Where the burst meets the ground, a puff of IV's bullet impact (every
  // third round: the line of fire is walked in 45 m steps).
  bool puff = false;
  if (j.shots % 3 == 0) {
    for (float d = 45.0f; d <= 900.0f; d += 45.0f) {
      float p[3] = {from[0] + fwd[0] * d, from[1] + fwd[1] * d, from[2] + fwd[2] * d};
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
    REXLOG_INFO("gta4-trainer: F-16 cannon firing (FIRE_SINGLE_BULLET {}, muzzle flash {}, impact effect {})",
                NativeKnown("FIRE_SINGLE_BULLET") ? "found" : "missing", flash ? "started" : "not started",
                puff ? "started" : "none yet");
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
void PointRocket(PPCContext& ctx, uint8_t* base, const BatRocket& r) {
  const float heading = std::atan2(-r.dir[0], r.dir[1]), pitch = std::asin(std::clamp(r.dir[2], -1.0f, 1.0f));
  const Quat q = QNormalize(QMul(QAxis(0, 0, 1, heading), QAxis(1, 0, 0, pitch)));
  Invoke(ctx, base, "SET_OBJECT_QUATERNION", {r.object, Bits(q.x), Bits(q.y), Bits(q.z), Bits(q.w)});
}

// Where a car is, or nothing (gone).
std::optional<std::array<float, 3>> CarPosition(PPCContext& ctx, uint8_t* base, uint32_t car) {
  const auto r = Invoke(ctx, base, "GET_CAR_COORDINATES", {car, 0, 0, 0}, 1, 0, 3);
  if (!r)
    return std::nullopt;
  const std::array<float, 3> p{r->f(0), r->f(1), r->f(2)};
  if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || (p[0] == 0 && p[1] == 0))
    return std::nullopt;
  return p;
}

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
    JetEngineSound(0.92f, 1.0f, 0.9f);
  } else if (g_gadget.boosting) {
    JetEngineSound(0.0f, 0.0f, 0.9f);  // winds down
  }
  BatFlame(ctx, base, car, boost);
  g_gadget.boosting = boost;
  return X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT |
         X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_LEFT_THUMB;
}

// The Batmobile's canopy, its door: the converter hangs the canopy (frame
// and glass) on the FBI template's interiorlight bone, hinged at its rear
// edge. Getting in, the canopy pops open, the player is put in the seat and
// it closes; getting out (Y, kept from the game in the Batmobile) the same the
// other way, the player set down beside the driver's side. No toggle: it
// stands in for the doors the Batmobile does not have. The cockpit under it
// (seats, dash, wheel) is the mod's own.
struct CanopyState {
  enum Phase : int { kShut, kOpeningIn, kClosingIn, kOpeningOut, kClosingOut };
  Phase phase = kShut;
  uint32_t car = 0;   // the Batmobile being got into or out of
  float open = 0.0f;  // 0 shut .. 1 open
  float hold = 0.0f;  // a beat with it up
  uint64_t next_probe = 0, last_ms = 0, started = 0;
  bool in_car = false;
  uint16_t prev_buttons = 0;
  uint32_t info = 0;  // the model whose pivot bone is known
  int32_t bone = -1;
  int log = 0;
  // Where the player was set down, watched for a few seconds: a fall
  // through the world puts them back there.
  float exit[3]{};
  uint64_t exit_at = 0;
  int catches = 0;
};
CanopyState g_canopy;
constexpr std::string_view kCanopyBone = "interiorlight";
constexpr float kCanopyDegrees = 60.0f;  // the front edge up, about the rear edge
constexpr float kCanopyOpenSeconds = 0.45f, kCanopyCloseSeconds = 0.7f;
constexpr float kCanopyExit[3] = {-2.1f, 0.2f, 0.0f};  // beside the driver's seat

bool PoseCanopy(PPCContext& ctx, uint8_t* base, uint32_t car, float open) {
  static const uint32_t kBat = ModelHash("BATMOBILE");
  const uint32_t vehicle = VehicleAddress(base, car);
  const uint32_t info = vehicle ? FindModelInfo(base, kBat) : 0;
  if (!info || !VehicleIsModel(base, vehicle, info))
    return false;
  if (g_canopy.info != info) {
    g_canopy.info = info;
    g_canopy.bone = FindBone(ctx, base, vehicle, kCanopyBone);
    REXLOG_INFO("gta4-trainer: Batmobile canopy on bone {} ({})", g_canopy.bone, std::string(kCanopyBone));
  }
  if (g_canopy.bone < 0)
    return false;
  const float t = std::clamp(open, 0.0f, 1.0f);
  return PoseBone(ctx, base, vehicle, info, kPoseBatmobile, 0, g_canopy.bone, 0,
                  kCanopyDegrees * t * t * (3.0f - 2.0f * t), 1.0f);
}

// Whether the canopy wants every frame (moving, the player in a Batmobile,
// or just set down beside one).
bool CanopyBusy() {
  return g_canopy.phase != CanopyState::kShut || g_canopy.in_car || g_canopy.exit_at != 0;
}

// Returns the buttons it keeps from the game: Y while the player is in a
// Batmobile (its way out is the canopy's).
uint32_t BatCanopy(PPCContext& ctx, uint8_t* base, const GameContext& game, State& s, uint16_t buttons) {
  auto& c = g_canopy;
  const uint64_t now = NowMs();
  const float dt = c.last_ms ? std::clamp(float(now - c.last_ms) / 1000.0f, 0.0f, 0.05f) : 0.0f;
  c.last_ms = now;
  const uint16_t prev = c.prev_buttons;
  c.prev_buttons = buttons;
  static const uint32_t kBat = ModelHash("BATMOBILE");
  const uint32_t keep = c.in_car ? uint32_t(X_INPUT_GAMEPAD_Y) : 0u;
  // Just set down: if the player drops through the world, back beside the car.
  if (c.exit_at) {
    if (now - c.exit_at > 3000 || c.in_car) {
      c.exit_at = 0;
    } else if (game.position[2] < c.exit[2] - 2.5f && c.catches < 3) {
      ++c.catches;
      Invoke(ctx, base, "CLEAR_CHAR_TASKS_IMMEDIATELY", {game.ped});
      Invoke(ctx, base, "SET_CHAR_COORDINATES", {game.ped, Bits(c.exit[0]), Bits(c.exit[1]), Bits(c.exit[2] + 1.0f)});
      REXLOG_WARN("gta4-trainer: Batmobile: player fell through the world after getting out (z {:.1f}, ground "
                  "{:.1f}); set back beside the car",
                  game.position[2], c.exit[2]);
      c.exit_at = now;
    }
  }
  switch (c.phase) {
    case CanopyState::kOpeningIn:
    case CanopyState::kOpeningOut: {
      // (A car that is gone or wrecked meanwhile ends it.)
      const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {c.car});
      if (!dead || dead->r0() || now - c.started > 6000) {
        c.phase = CanopyState::kShut;
        c.open = 0.0f;
        return 0;
      }
      c.open = std::min(1.0f, c.open + dt / kCanopyOpenSeconds);
      PoseCanopy(ctx, base, c.car, c.open);
      if (c.open < 1.0f || (c.hold += dt) < 0.15f)
        return keep;
      if (c.phase == CanopyState::kOpeningIn) {
        Invoke(ctx, base, "WARP_CHAR_INTO_CAR", {game.ped, c.car});
        c.in_car = true;
      } else {
        // As Rockstar's own scripts do it (dating_kate): the player's tasks
        // cleared first, then set down at the ground's height. v1.7.8 kept
        // the driving task and gave a height a metre up: the player fell
        // through the world.
        Invoke(ctx, base, "CLEAR_CHAR_TASKS_IMMEDIATELY", {game.ped});
        if (auto p = CarPoint(ctx, base, c.car, kCanopyExit)) {
          const auto gz = GroundZ(ctx, base, (*p)[0], (*p)[1], (*p)[2] + 1.0f);  // (below awnings)
          const float z = gz ? *gz : (*p)[2] - 0.6f;  // (the car's origin rides ~0.6 m up)
          Invoke(ctx, base, "WARP_CHAR_FROM_CAR_TO_COORD", {game.ped, Bits((*p)[0]), Bits((*p)[1]), Bits(z)});
          c.exit[0] = (*p)[0];
          c.exit[1] = (*p)[1];
          c.exit[2] = z;
          c.exit_at = now;
          c.catches = 0;
          REXLOG_INFO("gta4-trainer: Batmobile: player set down at {:.1f}, {:.1f}, {:.1f} (ground {})", (*p)[0],
                      (*p)[1], z, gz ? "found" : "not found");
        }
        c.in_car = false;
      }
      if (c.log < 8) {
        ++c.log;
        REXLOG_INFO("gta4-trainer: Batmobile canopy open, player {}", c.phase == CanopyState::kOpeningIn ? "in" : "out");
      }
      c.phase = c.phase == CanopyState::kOpeningIn ? CanopyState::kClosingIn : CanopyState::kClosingOut;
      c.hold = 0.0f;
      return c.in_car ? uint32_t(X_INPUT_GAMEPAD_Y) : 0u;
    }
    case CanopyState::kClosingIn:
    case CanopyState::kClosingOut:
      if ((c.hold += dt) >= 0.35f) {  // a beat with it up, then down
        c.open = std::max(0.0f, c.open - dt / kCanopyCloseSeconds);
        PoseCanopy(ctx, base, c.car, c.open);
        if (c.open <= 0.0f)
          c.phase = CanopyState::kShut;
      }
      return keep;
    case CanopyState::kShut:
      break;
  }
  if (!c.in_car && now < c.next_probe)
    return 0;
  c.next_probe = now + 250;
  const auto& vehicle = PlayerVehicle(ctx, base, game);
  if (vehicle.car && vehicle.model == kBat) {
    if (!c.in_car || c.car != *vehicle.car)
      PoseCanopy(ctx, base, *vehicle.car, 0.0f);  // shut (and its pivot looked up)
    c.in_car = true;
    c.car = *vehicle.car;
    if (g_canopy.bone < 0 || !NativeKnown("WARP_CHAR_FROM_CAR_TO_COORD"))
      return 0;  // no canopy pivot (or no way to set the player down): the game's own way out
    if ((buttons & X_INPUT_GAMEPAD_Y) && !(prev & X_INPUT_GAMEPAD_Y) && !s.open) {
      c.phase = CanopyState::kOpeningOut;
      c.open = c.hold = 0.0f;
      c.started = now;
    }
    return X_INPUT_GAMEPAD_Y;
  }
  c.in_car = false;
  if (vehicle.car)
    return 0;
  // On foot: is the game starting to put the player into a Batmobile?
  const auto getting = Invoke(ctx, base, "IS_CHAR_GETTING_IN_TO_A_CAR", {game.ped});
  if (!getting || !getting->r0())
    return 0;
  const auto target = Invoke(ctx, base, "GET_CAR_CHAR_IS_USING", {game.ped, 0}, 1);
  if (!target || !target->words[0])
    return 0;
  const auto model = Invoke(ctx, base, "GET_CAR_MODEL", {target->words[0], 0}, 1);
  if (!model || model->words[0] != kBat || !NativeKnown("CLEAR_CHAR_TASKS_IMMEDIATELY") ||
      !NativeKnown("WARP_CHAR_INTO_CAR"))
    return 0;
  const auto dead = Invoke(ctx, base, "IS_CAR_DEAD", {target->words[0]});
  if (!dead || dead->r0() || !PoseCanopy(ctx, base, target->words[0], 0.0f) || g_canopy.bone < 0)
    return 0;  // no pivot: the game's own way in
  Invoke(ctx, base, "CLEAR_CHAR_TASKS_IMMEDIATELY", {game.ped});
  c.car = target->words[0];
  c.phase = CanopyState::kOpeningIn;
  c.open = c.hold = 0.0f;
  c.started = now;
  return 0;
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
    if (j.in_jet)
      JetEngineSound(0.0f, 0.0f, 0.8f);  // just left the jet: the engine winds down
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
  }
  j.in_jet = true;
  if (s.jet && !j.rocket_requested) {
    Invoke(ctx, base, "REQUEST_MODEL", {ModelHash(kRocketModel)});
    j.rocket_requested = true;
  }
  if (!j.announced) {
    j.announced = true;
    Toast(s, s.jet ? "F-16: RT throttle, pull back at speed to take off. D-pad right: cannon, left: missile. "
                     "R3: landing gear."
                   : "F-16: R3 landing gear. Flight and weapons: Vehicle Options > F-16 Flight and Weapons.",
          6000);
  }
  float ground_speed = 0.0f;
  if (const auto v = Invoke(ctx, base, "GET_CAR_SPEED", {*car, 0}, 1))
    ground_speed = v->f(0);
  const float lx = Axis(pad.lx), ly = Axis(pad.ly);
  const float throttle = pad.right_trigger / 255.0f, airbrake = pad.left_trigger / 255.0f;

  if (!s.jet) {
    j.flying = false;
  } else {
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
  const bool burner = throttle > 0.9f && (j.flying || ground_speed > 20.0f);
  JetEngineSound(gta4::jet::EngineSynth::kIdle + (1.0f - gta4::jet::EngineSynth::kIdle) * throttle,
                 burner ? 1.0f : 0.0f, 1.0f);
  UpdateFlame(ctx, base, j, *car, throttle, burner);

  uint32_t used = X_INPUT_GAMEPAD_RIGHT_THUMB;
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
      g_popup.in_car = false;  // re-applied on the next look
      Toast(s, s.popup_auto ? "Deimos pop-ups rise with the headlights (night, rain, fog)."
                            : "Deimos pop-ups: d-pad left in the car raises and lowers them.");
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
  const bool jet_due = g_have_f16 && (g_jet.in_jet || missiles_live || now >= g_jet.next_probe);
  const bool popup_due = g_have_deimos && (g_popup.in_car || now >= g_popup.next_probe);
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
  const bool canopy_due = g_have_batmobile && (CanopyBusy() || now >= g_canopy.next_probe);
  const bool rain_due = s.raining && now >= g_next_rain;
  if (g_pending.empty() && !god_due && !wanted_due && !car_god_due && !gadgets_due && !canopy_due &&
      s.pending_model < 0 && !jet_due && !rain_due && !popup_due && !lights_due && !drive_due &&
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
    REXLOG_INFO("gta4-trainer: add-ons present: Batmobile {}, F-16 {}, Deimos {}, Dominator 645 {}",
                g_have_batmobile, g_have_f16, g_have_deimos, installed("DOM645T"));
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
    const uint32_t used = Gadgets(ctx, base, game, s, buttons, previous);
    s_in_batmobile = used != 0;
    vehicle_mask |= used;
    // Consume the edge so the second script-stage site cannot double-fire.
    g_gadget_previous.store(buttons, std::memory_order_relaxed);
  } else if (!s.gadgets) {
    s_in_batmobile = false;
  }
  if (canopy_due)
    vehicle_mask |= BatCanopy(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed));
  if (jet_due) {
    DrivePad pad;
    {
      std::lock_guard lock(g_drive_mutex);
      pad = g_drive_pad;  // (the jet keeps its own button edges)
    }
    vehicle_mask |= JetControls(ctx, base, game, s, pad);
  }
  if (popup_due &&
      PopupLights(ctx, base, game, s, g_gadget_buttons.load(std::memory_order_relaxed)))
    vehicle_mask |= X_INPUT_GAMEPAD_DPAD_LEFT;
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
