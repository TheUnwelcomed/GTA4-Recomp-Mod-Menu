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

// Launch guard: a launch that mounts the shadows leaves a marker until the
// game reaches the city (the trainer calls MarkLaunchHealthy then). The
// marker counts such launches in a row; after two that never got in, a launch
// starts without the shadows - the stock game - and the one after tries again.
void MarkLaunchHealthy();
bool ShadowsSkippedThisLaunch();
}  // namespace gta4::addons
