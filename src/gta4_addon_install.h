// Bundled add-on auto-install (the Batmobile): at app setup, before the game
// launches, registration lines are appended to handling.dat / vehicles.ide
// and the model files are injected into vehicles.img, so installing the
// add-on is nothing more than copying the app folder.
//
// Safety rules: idempotent (a second boot changes nothing); every touched
// file gets a one-time .stock-bak backup beside it; the image is rebuilt to
// a temporary file and swapped in only after it is complete; any surprise in
// any file (wrong magic, implausible header, short read) means that step is
// skipped with a log line and the game boots stock.
//
// The game reads its data files (vehicles.ide, handling.dat, carcols.dat,
// vehOff.csv) from common.rpf, so the add-ons' lines go into a shadow copy of
// that archive. InstallBundled returns each shadow with the guest path it
// stands in for (game:\common.rpf), and the app redirects that path to it
// before the game boots. Nothing returned: the game keeps its own archives.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace gta4::addons {
struct ShadowArchive {
  std::string guest_path;          // e.g. game:\common.rpf
  std::filesystem::path shadow;    // the edited copy on the host
};
std::vector<ShadowArchive> InstallBundled(const std::filesystem::path& app_addons,
                                          const std::filesystem::path& user_data_root);

// Set by the app once a shadow is mounted. The game checks some data files
// (vehicles.ide, handling.dat...) against built-in checksums as it opens them
// and stops on a mismatch; the trainer skips that check while this is set.
void SetShadowsActive(bool active);
bool ShadowsActive();

// Map sets (addons/maps/<name>, made by the repo's tools/maps/mapset.py):
// places for the DeLorean's time travel. Their models, textures and
// collision go into vehicles.img with the cars; their model lines into
// common.rpf's gtxd.ide (the shadow; gta.dat already loads it); water under
// them into water.dat when the stock sea does not reach there. The trainer
// places the pieces as frozen objects when the destination is chosen (its
// collision dictionaries asked for with the models: the game links a model
// to its bound only when the dictionary holding it loads) and deletes them
// again. A piece may be one of the game's own models (place.txt collision 2,
// v1.7.37: a mod placing the game's props): no file, the game's collision.
struct MapPiece {
  std::string model;
  float q[4]{};    // its rotation (x, y, z, w)
  float pos[3]{};  // where it stands, in the mod's coordinates
  bool collision = false;
  bool stock = false;  // the game's own model (no file of the set's; its collision the game's own)
};
// A vehicle the mod parks (its .wpl car generators, vehicles.txt): the
// trainer puts it there while the player is near (v1.7.37).
struct MapVehicle {
  std::string model;  // the game's own vehicle
  float pos[3]{};     // the mod's coordinates
  float heading = 0;  // degrees
};
struct MapInfo {
  std::string name, label;
  int date[5]{};             // year, month, day, hour, minute
  float shift[2]{};          // the mod's coordinates to the game's (x, y)
  float sea_src = 0.0f;      // the mod's sea level
  float sea_dst = 0.0f;      // the game's, where it is put (from water.dat)
  float arrive[5]{};         // x, y, z (the mod's), heading (degrees), clear run ahead (m)
  float area[4]{};           // min x, min y, max x, max y (the mod's)
  std::vector<std::string> bounds;  // its collision dictionaries' names (<name>.xbd), or none
  std::vector<MapPiece> pieces;
  // Its life (v1.7.37): the vehicles the mod parks; the game's ped models
  // that belong there (map.txt "peds"), of which `wander` are kept about the
  // player; the weather there (map.txt "weather", the game's id, -1 none);
  // the sound of the place (map.txt "ambience": the trainer's own, by name).
  std::vector<MapVehicle> vehicles;
  std::vector<std::string> peds;
  int wander = 0;
  int weather = -1;
  std::string ambience;
  std::filesystem::path dir;  // its folder (ground.bin, the trainer's own ground grid, is read from it)
  bool installed = false;    // in vehicles.img and gtxd.ide this launch
};
// The map sets this launch installed (filled by InstallBundled).
const std::vector<MapInfo>& InstalledMaps();
// The level most of the stock sea is at (water.dat), or 0 when not read.
float SeaLevel();

// Launch guard: a launch that mounts the shadows leaves a marker until the
// game reaches the city (the trainer calls MarkLaunchHealthy then). The
// marker counts such launches in a row; after two that never got in, a launch
// starts without the shadows - the stock game - and the one after tries again.
void MarkLaunchHealthy();
bool ShadowsSkippedThisLaunch();
}  // namespace gta4::addons
