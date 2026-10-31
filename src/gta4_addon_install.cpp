// Bundled add-on auto-install (see gta4_addon_install.h).
//
// The RAGE IMG3 container layout follows SparkIV's RageLib (GPLv3, by
// Arushan), the reference tool for these archives:
//   header (0x14): u32 magic 0xA94E2A52, i32 version=3, i32 entryCount,
//                  i32 tocSize, i16 tocEntrySize=16, i16 unknown
//   toc:   entryCount * 16 bytes, then the NUL-separated name table
//          (tocSize covers both); each entry: u32 sizeOrRscFlags (top two
//          bits set = resource), i32 resourceType, i32 offsetBlock (0x800
//          blocks from file start), i16 usedBlocks, i16 flags
//          (0x2000 = new-style resource, low 11 bits = padding bytes)
//   data:  block-aligned, addressed by each entry's offsetBlock.
// The Xbox 360 build stores the same layout big-endian (its RSC files use
// the 0x52534305 byte order RageLib names MagicBigEndian), so the byte
// order is detected from the archive's own magic and everything is read
// and written through that detection. An encrypted archive (the magic
// matches neither order) is left alone.
#include "gta4_addon_install.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace gta4::addons {
namespace {
namespace fs = std::filesystem;

constexpr uint32_t kImgMagic = 0xA94E2A52u;
constexpr uint32_t kRscMagicLE = 0x05435352u;  // 'RSC\x05' as PC stores it
constexpr uint32_t kBlock = 0x800;

uint32_t Swap32(uint32_t v) { return __builtin_bswap32(v); }
uint16_t Swap16(uint16_t v) { return __builtin_bswap16(v); }

struct Endian {
  bool big = false;
  uint32_t R32(const uint8_t* p) const {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return big ? Swap32(v) : v;
  }
  uint16_t R16(const uint8_t* p) const {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return big ? Swap16(v) : v;
  }
  void W32(uint8_t* p, uint32_t v) const {
    if (big) v = Swap32(v);
    std::memcpy(p, &v, 4);
  }
  void W16(uint8_t* p, uint16_t v) const {
    if (big) v = Swap16(v);
    std::memcpy(p, &v, 2);
  }
};

bool ReadAll(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  if (size < 0) return false;
  out.resize(size_t(size));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
  return bool(in);
}

bool BackupOnce(const fs::path& file) {
  std::error_code ec;
  const fs::path bak = file.string() + ".stock-bak";
  if (fs::exists(bak, ec)) return true;
  fs::copy_file(file, bak, ec);
  if (ec) REXLOG_WARN("gta4-addons: could not back up {}: {}", file.string(), ec.message());
  return !ec;
}

// Appends `line` to a text file unless `token` already appears in it.
bool AppendLineIfMissing(const fs::path& file, std::string_view token, std::string_view line,
                         const char* what) {
  std::vector<uint8_t> raw;
  if (!ReadAll(file, raw)) {
    REXLOG_WARN("gta4-addons: cannot read {}", file.string());
    return false;
  }
  const std::string_view text(reinterpret_cast<const char*>(raw.data()), raw.size());
  if (text.find(token) != std::string_view::npos) return true;  // already installed
  if (!BackupOnce(file)) return false;
  const fs::path tmp = file.string() + ".new";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(raw.data()), std::streamsize(raw.size()));
    if (!text.empty() && text.back() != '\n') out.put('\n');
    out.write(line.data(), std::streamsize(line.size()));
    out.put('\n');
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(tmp, file, ec);
  if (ec) {
    REXLOG_WARN("gta4-addons: could not update {}: {}", file.string(), ec.message());
    return false;
  }
  REXLOG_INFO("gta4-addons: {} registered in {}", what, file.filename().string());
  return true;
}

struct NewFile {
  std::string name;              // name inside the archive
  std::vector<uint8_t> data;     // RSC file contents
  uint32_t rsc_flags = 0, rsc_type = 0;
};

// Loads an RSC file and extracts its type and flags from the 12-byte header,
// accepting either byte order; when the target archive is big-endian the
// header fields are rewritten big-endian so the recompiled reader sees a
// well-formed resource.
bool LoadRsc(const fs::path& path, bool target_big, NewFile& out) {
  if (!ReadAll(path, out.data) || out.data.size() < 12) return false;
  uint32_t magic;
  std::memcpy(&magic, out.data.data(), 4);
  Endian source{};
  if (magic == kRscMagicLE) {
    source.big = false;
  } else if (Swap32(magic) == kRscMagicLE) {
    source.big = true;
  } else {
    REXLOG_WARN("gta4-addons: {} is not an RSC file", path.string());
    return false;
  }
  out.rsc_type = source.R32(out.data.data() + 4);
  out.rsc_flags = source.R32(out.data.data() + 8);
  if (source.big != target_big) {
    const Endian target{target_big};
    target.W32(out.data.data(), kRscMagicLE);
    target.W32(out.data.data() + 4, out.rsc_type);
    target.W32(out.data.data() + 8, out.rsc_flags);
  }
  out.name = path.filename().string();
  return true;
}

struct TocEntry {
  uint32_t word0 = 0;  // size, or RSC flags with the top bits set
  uint32_t type = 0;
  uint32_t offset_block = 0;
  uint16_t used_blocks = 0;
  uint16_t flags = 0;
  std::string name;
};

bool InjectIntoImg(const fs::path& img, std::vector<NewFile> files) {
  std::ifstream in(img, std::ios::binary);
  if (!in) {
    REXLOG_WARN("gta4-addons: cannot open {}", img.string());
    return false;
  }
  std::array<uint8_t, 0x14> header{};
  if (!in.read(reinterpret_cast<char*>(header.data()), header.size())) return false;

  uint32_t magic_raw;
  std::memcpy(&magic_raw, header.data(), 4);
  Endian e{};
  if (magic_raw == kImgMagic) {
    e.big = false;
  } else if (Swap32(magic_raw) == kImgMagic) {
    e.big = true;
  } else {
    REXLOG_WARN(
        "gta4-addons: {} magic {:02X}{:02X}{:02X}{:02X} is neither IMG3 byte order "
        "(encrypted archive?); leaving it alone",
        img.filename().string(), header[0], header[1], header[2], header[3]);
    return false;
  }
  const uint32_t version = e.R32(header.data() + 4);
  const uint32_t count = e.R32(header.data() + 8);
  const uint32_t toc_size = e.R32(header.data() + 12);
  const uint16_t entry_size = e.R16(header.data() + 16);
  if (version != 3 || entry_size != 16 || count == 0 || count > 200000 ||
      toc_size <= count * 16 || toc_size > (64u << 20)) {
    REXLOG_WARN("gta4-addons: {} header is implausible (v{} n{} toc{} es{}); skipping",
                   img.filename().string(), version, count, toc_size, entry_size);
    return false;
  }
  std::vector<uint8_t> toc(toc_size);
  if (!in.read(reinterpret_cast<char*>(toc.data()), std::streamsize(toc.size()))) return false;

  std::vector<TocEntry> entries(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* p = toc.data() + i * 16;
    entries[i] = {e.R32(p), e.R32(p + 4), e.R32(p + 8), e.R16(p + 12), e.R16(p + 14), {}};
  }
  const char* names = reinterpret_cast<const char*>(toc.data()) + count * 16;
  size_t names_len = toc_size - count * 16, cursor = 0;
  for (uint32_t i = 0; i < count && cursor < names_len; ++i) {
    const size_t len = strnlen(names + cursor, names_len - cursor);
    entries[i].name.assign(names + cursor, len);
    cursor += len + 1;
  }
  for (const auto& add : files)
    for (const auto& entry : entries)
      if (entry.name == add.name) {
        REXLOG_INFO("gta4-addons: {} already holds {}", img.filename().string(), add.name);
        return true;  // installed on an earlier boot
      }

  // The new table: every existing entry keeps its order and sizes, only the
  // offsets move; the added files go at the end.
  std::vector<TocEntry> all = entries;
  for (const auto& add : files) {
    TocEntry entry;
    entry.word0 = add.rsc_flags;  // IV resource flags carry the top bits
    entry.type = add.rsc_type;
    const uint32_t padded = uint32_t((add.data.size() + kBlock - 1) / kBlock) * kBlock;
    entry.used_blocks = uint16_t(padded / kBlock);
    entry.flags = uint16_t(0x2000 | ((padded - add.data.size()) & 0x7FF));
    entry.name = add.name;
    all.push_back(std::move(entry));
  }
  size_t new_names = 0;
  for (const auto& entry : all) new_names += entry.name.size() + 1;
  const uint32_t new_toc_size = uint32_t(all.size() * 16 + new_names);
  uint32_t block = uint32_t((0x14 + new_toc_size + kBlock - 1) / kBlock);

  const fs::path out_path = img.string() + ".new";
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  // Header and a placeholder TOC; offsets are filled in as data is copied.
  Endian w = e;
  std::array<uint8_t, 0x14> new_header = header;
  w.W32(new_header.data() + 8, uint32_t(all.size()));
  w.W32(new_header.data() + 12, new_toc_size);
  out.write(reinterpret_cast<const char*>(new_header.data()), new_header.size());
  std::vector<uint8_t> new_toc(new_toc_size, 0);
  out.write(reinterpret_cast<const char*>(new_toc.data()), std::streamsize(new_toc.size()));
  // Pad to the first data block.
  const std::vector<char> zero(kBlock, 0);
  const uint64_t data_start = uint64_t(block) * kBlock;
  for (uint64_t at = 0x14 + new_toc_size; at < data_start; ) {
    const size_t chunk = size_t(std::min<uint64_t>(zero.size(), data_start - at));
    out.write(zero.data(), std::streamsize(chunk));
    at += chunk;
  }
  // Existing payloads, in order, re-offset.
  std::vector<uint8_t> buffer(8u << 20);
  for (uint32_t i = 0; i < count; ++i) {
    auto& entry = all[i];
    uint64_t src = uint64_t(entries[i].offset_block) * kBlock;
    uint64_t bytes = uint64_t(entries[i].used_blocks) * kBlock;
    // A non-resource entry's blocks derive from its byte size.
    if (!(entry.word0 & 0xC0000000u) && entries[i].used_blocks == 0)
      bytes = (uint64_t(entry.word0) + kBlock - 1) / kBlock * kBlock;
    entry.offset_block = block;
    in.clear();
    in.seekg(std::streamoff(src));
    uint64_t left = bytes;
    while (left) {
      const size_t chunk = size_t(std::min<uint64_t>(buffer.size(), left));
      if (!in.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(chunk))) {
        REXLOG_WARN("gta4-addons: short read in {} at entry {} ({}); aborting",
                       img.filename().string(), i, entry.name);
        out.close();
        std::error_code ec;
        fs::remove(out_path, ec);
        return false;
      }
      out.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(chunk));
      left -= chunk;
    }
    block += uint32_t(bytes / kBlock);
  }
  // The added files.
  for (size_t i = 0; i < files.size(); ++i) {
    auto& entry = all[count + i];
    entry.offset_block = block;
    const auto& data = files[i].data;
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    const size_t pad = size_t(entry.used_blocks) * kBlock - data.size();
    for (size_t at = 0; at < pad; at += zero.size())
      out.write(zero.data(), std::streamsize(std::min(zero.size(), pad - at)));
    block += entry.used_blocks;
  }
  // The real TOC.
  {
    size_t name_at = all.size() * 16;
    for (size_t i = 0; i < all.size(); ++i) {
      uint8_t* p = new_toc.data() + i * 16;
      w.W32(p, all[i].word0);
      w.W32(p + 4, all[i].type);
      w.W32(p + 8, all[i].offset_block);
      w.W16(p + 12, all[i].used_blocks);
      w.W16(p + 14, all[i].flags);
      std::memcpy(new_toc.data() + name_at, all[i].name.c_str(), all[i].name.size() + 1);
      name_at += all[i].name.size() + 1;
    }
    out.seekp(0x14);
    out.write(reinterpret_cast<const char*>(new_toc.data()), std::streamsize(new_toc.size()));
  }
  out.flush();
  if (!out) {
    std::error_code ec;
    fs::remove(out_path, ec);
    return false;
  }
  out.close();
  in.close();
  if (!BackupOnce(img)) {
    std::error_code ec;
    fs::remove(out_path, ec);
    return false;
  }
  std::error_code ec;
  fs::rename(out_path, img, ec);
  if (ec) {
    REXLOG_WARN("gta4-addons: could not swap {} in: {}", img.filename().string(), ec.message());
    fs::remove(out_path, ec);
    return false;
  }
  REXLOG_INFO("gta4-addons: {} rebuilt ({}-endian) with {} added file(s)",
              img.filename().string(), e.big ? "big" : "little", files.size());
  return true;
}

fs::path FindGameRoot(const fs::path& user_data_root) {
  std::error_code ec;
  for (const fs::path candidate : {user_data_root, user_data_root.parent_path()}) {
    if (fs::exists(candidate / "game" / "common" / "data" / "handling.dat", ec))
      return candidate / "game";
  }
  return {};
}

fs::path FindVehiclesImg(const fs::path& game_root) {
  std::error_code ec;
  for (const char* platform : {"xbox360", "pc"}) {
    const fs::path path = game_root / platform / "models" / "cdimages" / "vehicles.img";
    if (fs::exists(path, ec)) return path;
  }
  // The layout moved? Take a bounded look.
  for (auto it = fs::recursive_directory_iterator(
           game_root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
    if (!it->is_regular_file(ec)) continue;
    if (it->path().filename() == "vehicles.img") return it->path();
  }
  return {};
}

std::string ReadLineFile(const fs::path& path) {
  std::vector<uint8_t> raw;
  if (!ReadAll(path, raw)) return {};
  std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

}  // namespace

void InstallBundled(const fs::path& app_addons, const fs::path& user_data_root) {
  std::error_code ec;
  const fs::path batmobile = app_addons / "batmobile";
  if (!fs::exists(batmobile, ec)) return;  // nothing bundled
  const fs::path game = FindGameRoot(user_data_root);
  if (game.empty()) {
    REXLOG_INFO("gta4-addons: no installed game yet; add-ons wait for the next boot");
    return;
  }

  const std::string handling = ReadLineFile(batmobile / "handling.txt");
  const std::string ide = ReadLineFile(batmobile / "vehicles_ide.txt");
  bool ok = true;
  if (!handling.empty())
    ok &= AppendLineIfMissing(game / "common" / "data" / "handling.dat", "BATMOBILE", handling,
                              "Batmobile handling");
  if (!ide.empty())
    ok &= AppendLineIfMissing(game / "common" / "data" / "vehicles.ide", "batmobile,", ide,
                              "Batmobile vehicle");

  const fs::path img = FindVehiclesImg(game);
  if (img.empty()) {
    REXLOG_WARN("gta4-addons: vehicles.img was not found under {}", game.string());
    return;
  }
  // Byte order of the archive decides the byte order of the injected
  // resource headers; probe it first so LoadRsc can match it.
  bool big = false;
  {
    std::ifstream probe(img, std::ios::binary);
    uint32_t magic = 0;
    if (!probe.read(reinterpret_cast<char*>(&magic), 4)) return;
    if (magic == kImgMagic) big = false;
    else if (Swap32(magic) == kImgMagic) big = true;
    else {
      REXLOG_WARN("gta4-addons: {} is not a plain IMG3 archive; model step skipped",
                     img.filename().string());
      return;
    }
  }
  std::vector<NewFile> files(2);
  if (!LoadRsc(batmobile / "batmobile.wft", big, files[0]) ||
      !LoadRsc(batmobile / "batmobile.wtd", big, files[1])) {
    REXLOG_WARN("gta4-addons: bundled Batmobile files are missing or malformed");
    return;
  }
  if (InjectIntoImg(img, std::move(files)) && ok)
    REXLOG_INFO("gta4-addons: Batmobile add-on is installed");
}

}  // namespace gta4::addons
