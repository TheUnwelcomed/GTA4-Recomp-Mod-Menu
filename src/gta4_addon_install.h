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
}  // namespace gta4::addons
