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
#pragma once
#include <filesystem>

namespace gta4::addons {
void InstallBundled(const std::filesystem::path& app_addons,
                    const std::filesystem::path& user_data_root);
}  // namespace gta4::addons
