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
#include <cctype>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rijndael-alg-fst.h>

namespace gta4::addons {
namespace {
namespace fs = std::filesystem;

constexpr uint32_t kImgMagic = 0xA94E2A52u;
constexpr uint32_t kRscMagicLE = 0x05435352u;  // 'RSC\x05' as PC stores it
constexpr uint32_t kBlock = 0x800;

uint32_t Swap32(uint32_t v) { return __builtin_bswap32(v); }
uint16_t Swap16(uint16_t v) { return __builtin_bswap16(v); }

// The console's archives keep their header and TOC under the same cipher
// the RPFs use: AES-256-ECB applied 16 times (gta4_rpf_extractor.cpp). Only
// whole 16-byte blocks are ciphered; a trailing partial block stays plain,
// matching the reference tooling.
void CryptBlocks(uint8_t* data, size_t size, const std::vector<uint8_t>& key, bool encrypt) {
  const size_t whole = size & ~size_t(15);
  if (!whole || key.size() != 32)
    return;
  std::array<uint32_t, 4 * (MAXNR + 1)> round_keys{};
  const int rounds = encrypt ? rijndaelKeySetupEnc(round_keys.data(), key.data(), 256)
                             : rijndaelKeySetupDec(round_keys.data(), key.data(), 256);
  if (rounds <= 0)
    return;
  std::array<uint8_t, 16> output{};
  for (int pass = 0; pass < 16; ++pass) {
    for (size_t offset = 0; offset < whole; offset += 16) {
      if (encrypt)
        rijndaelEncrypt(round_keys.data(), rounds, data + offset, output.data());
      else
        rijndaelDecrypt(round_keys.data(), rounds, data + offset, output.data());
      std::memcpy(data + offset, output.data(), output.size());
    }
  }
}

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

// The game's text data files (vehicles.ide, handling.dat, carcols.dat,
// vehOff.csv) are edited line by line. Every line keeps its own ending, so a
// CRLF file stays CRLF, and a file is only rewritten when an edit changed it.
struct TextLines {
  std::vector<std::string> lines;  // without the '\n'; a '\r' stays on the line
  bool final_newline = true;
};

bool LoadText(const fs::path& file, TextLines& out) {
  std::vector<uint8_t> raw;
  if (!ReadAll(file, raw))
    return false;
  const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
  out.lines.clear();
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) {
      out.lines.push_back(text.substr(pos));
      out.final_newline = false;
      return true;
    }
    out.lines.push_back(text.substr(pos, eol - pos));
    pos = eol + 1;
  }
  out.final_newline = true;
  return true;
}

bool SaveText(const fs::path& file, const TextLines& text, const char* what) {
  if (!BackupOnce(file))
    return false;
  const fs::path tmp = file.string() + ".new";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    for (size_t i = 0; i < text.lines.size(); ++i) {
      out.write(text.lines[i].data(), std::streamsize(text.lines[i].size()));
      if (i + 1 < text.lines.size() || text.final_newline)
        out.put('\n');
    }
    if (!out)
      return false;
  }
  std::error_code ec;
  fs::rename(tmp, file, ec);
  if (ec) {
    REXLOG_WARN("gta4-addons: could not update {}: {}", file.string(), ec.message());
    return false;
  }
  REXLOG_INFO("gta4-addons: {} applied in {}", what, file.string());
  return true;
}

bool IsSeparator(char c) { return c == ' ' || c == '\t' || c == ',' || c == '\r'; }

// Lowercased whitespace/comma-separated tokens: two lines that differ only
// in spacing or case are the same entry.
std::vector<std::string> Tokens(std::string_view line) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && IsSeparator(line[i])) ++i;
    const size_t start = i;
    while (i < line.size() && !IsSeparator(line[i])) ++i;
    if (i > start) {
      std::string token(line.substr(start, i - start));
      for (char& c : token) c = char(std::tolower(static_cast<unsigned char>(c)));
      tokens.push_back(std::move(token));
    }
  }
  return tokens;
}

// The first token of a line (the model or handling name), lowercased; its
// position comes back too so it can be swapped for another name.
std::string FirstToken(std::string_view line, size_t* at = nullptr, size_t* len = nullptr) {
  size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  const size_t start = i;
  while (i < line.size() && !IsSeparator(line[i])) ++i;
  if (at) *at = start;
  if (len) *len = i - start;
  std::string token(line.substr(start, i - start));
  for (char& c : token) c = char(std::tolower(static_cast<unsigned char>(c)));
  return token;
}

bool EditText(const fs::path& file, const char* what,
              const std::function<bool(std::vector<std::string>&)>& edit) {
  TextLines text;
  if (!LoadText(file, text)) {
    REXLOG_WARN("gta4-addons: cannot read {}", file.string());
    return false;
  }
  std::vector<std::string> lines = text.lines;
  if (!edit(lines))
    return false;
  if (lines == text.lines) {
    REXLOG_INFO("gta4-addons: {} already in place in {}", what, file.string());
    return true;
  }
  text.lines = std::move(lines);
  return SaveText(file, text, what);
}

// The donor's line ending, so an inserted line matches its neighbours.
std::string WithEnding(std::string_view line, const std::string& donor) {
  std::string out(line);
  if (!donor.empty() && donor.back() == '\r')
    out.push_back('\r');
  return out;
}

// Puts `line` into the file directly after the donor model's line - inside
// the same section, which a plain append would miss (vehicles.ide closes its
// "cars" section with "end", and lines after it are never read). Lines an
// earlier attempt left for the same name, wherever they are, are dropped
// first, so the file ends up with exactly one.
bool InsertAfterDonor(const fs::path& file, std::string_view donor, std::string_view line,
                      const char* what) {
  const std::string name = FirstToken(line);
  return EditText(file, what, [&](std::vector<std::string>& lines) {
    std::erase_if(lines, [&](const std::string& l) { return FirstToken(l) == name; });
    for (size_t i = 0; i < lines.size(); ++i) {
      if (FirstToken(lines[i]) == donor) {
        lines.insert(lines.begin() + std::ptrdiff_t(i + 1), WithEnding(line, lines[i]));
        return true;
      }
    }
    REXLOG_WARN("gta4-addons: no '{}' line in {} to place {} after", std::string(donor),
                file.string(), what);
    return false;
  });
}

// For files with one or more lines per model (carcols.dat's colour sections,
// vehOff.csv): every donor line gets a twin for the new model right after
// it, the name written in the donor's case.
bool CloneDonorLines(const fs::path& file, std::string_view donor, std::string_view model,
                     const char* what) {
  return EditText(file, what, [&](std::vector<std::string>& lines) {
    std::erase_if(lines, [&](const std::string& l) { return FirstToken(l) == model; });
    for (size_t i = 0; i < lines.size(); ++i) {
      size_t at = 0, len = 0;
      if (FirstToken(lines[i], &at, &len) != donor)
        continue;
      const std::string original = lines[i].substr(at, len);
      const bool upper = std::none_of(original.begin(), original.end(),
                                      [](char c) { return std::islower(static_cast<unsigned char>(c)); });
      std::string name(model);
      for (char& c : name)
        c = char(upper ? std::toupper(static_cast<unsigned char>(c)) : c);
      std::string twin = lines[i];
      twin.replace(at, len, name);
      lines.insert(lines.begin() + std::ptrdiff_t(i + 1), std::move(twin));
      ++i;
    }
    return true;
  });
}

// Earlier releases put the Batmobile INTO the FBI slot. When the donor's line
// is still the one they wrote, it goes back to stock: from the .stock-bak
// copy taken before that first change when it has a genuine one, otherwise
// from the stock line shipped here.
bool RestoreDonorLine(const fs::path& file, std::string_view donor,
                      const std::function<bool(std::string_view)>& is_legacy,
                      std::string_view stock, const char* what) {
  std::string replacement(stock);
  TextLines backup;
  if (LoadText(file.string() + ".stock-bak", backup)) {
    for (const auto& l : backup.lines) {
      if (FirstToken(l) == donor && !is_legacy(l)) {
        replacement = l;
        if (!replacement.empty() && replacement.back() == '\r')
          replacement.pop_back();
        break;
      }
    }
  }
  return EditText(file, what, [&](std::vector<std::string>& lines) {
    for (auto& l : lines) {
      if (FirstToken(l) == donor && is_legacy(l)) {
        REXLOG_INFO("gta4-addons: putting the stock {} line back in {}", std::string(donor),
                    file.string());
        l = WithEnding(replacement, l);
      }
    }
    return true;
  });
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

bool InjectIntoImg(const fs::path& img, const std::vector<uint8_t>& key,
                   const std::vector<fs::path>& rsc_paths, bool append_missing,
                   const std::vector<std::string>& remove_names = {}) {
  std::ifstream in(img, std::ios::binary);
  if (!in) {
    REXLOG_WARN("gta4-addons: cannot open {}", img.string());
    return false;
  }
  std::array<uint8_t, 0x14> header{};
  if (!in.read(reinterpret_cast<char*>(header.data()), header.size())) return false;

  // Plain, either byte order - or encrypted, which the first 16 bytes
  // decrypted with the game's own RPF key reveal.
  auto match = [](const uint8_t* p, Endian& e) {
    uint32_t magic;
    std::memcpy(&magic, p, 4);
    if (magic == kImgMagic) { e.big = false; return true; }
    if (Swap32(magic) == kImgMagic) { e.big = true; return true; }
    return false;
  };
  Endian e{};
  bool encrypted = false;
  if (!match(header.data(), e)) {
    std::array<uint8_t, 0x14> trial = header;
    CryptBlocks(trial.data(), 16, key, false);
    if (!key.empty() && match(trial.data(), e)) {
      encrypted = true;
      header = trial;
    } else {
      REXLOG_WARN(
          "gta4-addons: {} magic {:02X}{:02X}{:02X}{:02X} is neither plain nor "
          "RPF-key-encrypted IMG3; leaving it alone",
          img.filename().string(), header[0], header[1], header[2], header[3]);
      return false;
    }
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
  if (encrypted)
    CryptBlocks(toc.data(), toc.size(), key, false);

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
  // The byte order the GAME expects of a resource header is read off a
  // stock payload of the same kind (.xft/.xtd) - the container's own order
  // says nothing about it: the console's archive is little-endian around
  // big-endian resources - and the bundled files are stamped to match. A
  // same-named entry is preferred; a brand-new name probes any entry with
  // its extension. Ground truth is logged so a console log settles any doubt.
  auto extension = [](const std::string& name) {
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : name.substr(dot);
  };
  std::vector<uint32_t> probe_order;
  for (const auto& path : rsc_paths)
    for (uint32_t i = 0; i < count; ++i)
      if (entries[i].name == path.filename().string())
        probe_order.push_back(i);
  for (const auto& path : rsc_paths)
    for (uint32_t i = 0; i < count; ++i)
      if ((entries[i].word0 & 0xC0000000u) &&
          extension(entries[i].name) == extension(path.filename().string()) &&
          std::find(remove_names.begin(), remove_names.end(), entries[i].name) ==
              remove_names.end()) {
        probe_order.push_back(i);
        break;
      }
  bool rsc_big = e.big;
  bool probed = false;
  for (const uint32_t i : probe_order) {
    {
      std::array<uint8_t, 4> head{};
      in.clear();
      in.seekg(std::streamoff(uint64_t(entries[i].offset_block) * kBlock));
      if (in.read(reinterpret_cast<char*>(head.data()), head.size())) {
        uint32_t magic;
        std::memcpy(&magic, head.data(), 4);
        if (magic == kRscMagicLE) { rsc_big = false; probed = true; }
        else if (Swap32(magic) == kRscMagicLE) { rsc_big = true; probed = true; }
        REXLOG_INFO("gta4-addons: {} stock {} header {:02X}{:02X}{:02X}{:02X} ({}-endian rsc)",
                    img.filename().string(), entries[i].name, head[0], head[1], head[2], head[3],
                    rsc_big ? "big" : "little");
      }
    }
    if (probed)
      break;
  }
  std::vector<NewFile> files(rsc_paths.size());
  for (size_t i = 0; i < rsc_paths.size(); ++i) {
    if (!LoadRsc(rsc_paths[i], rsc_big, files[i])) {
      REXLOG_WARN("gta4-addons: bundled file {} is missing or malformed",
                  rsc_paths[i].filename().string());
      return false;
    }
  }

  // A bundled file whose name the archive already carries replaces that
  // entry's payload (the IV engine only accepts vehicles it already knows,
  // so car mods swap an existing model); an unknown name is appended only
  // when this archive is the append target. An entry whose stored byte size
  // already equals the bundled file counts as current.
  auto fill_meta = [](TocEntry& entry, const NewFile& add) {
    entry.word0 = add.rsc_flags;  // IV resource flags carry the top bits
    entry.type = add.rsc_type;
    const uint32_t padded = uint32_t((add.data.size() + kBlock - 1) / kBlock) * kBlock;
    entry.used_blocks = uint16_t(padded / kBlock);
    entry.flags = uint16_t(0x2000 | ((padded - add.data.size()) & 0x7FF));
  };
  std::vector<int> replace_with(count, -1);
  std::vector<bool> matched(files.size(), false);
  std::vector<size_t> appended;
  bool work = false;
  // Entries an earlier release added that the game never reads (PC-named
  // .wft/.wtd files) are dropped.
  std::vector<bool> drop(count, false);
  for (uint32_t i = 0; i < count; ++i) {
    if (std::find(remove_names.begin(), remove_names.end(), entries[i].name) !=
        remove_names.end()) {
      drop[i] = true;
      work = true;
      REXLOG_INFO("gta4-addons: dropping unused {} from {}", entries[i].name,
                  img.filename().string());
    }
  }
  for (size_t f = 0; f < files.size(); ++f) {
    for (uint32_t i = 0; i < count; ++i) {
      if (entries[i].name != files[f].name)
        continue;
      matched[f] = true;
      const uint32_t stored =
          uint32_t(entries[i].used_blocks) * kBlock - uint32_t(entries[i].flags & 0x7FF);
      if (stored == files[f].data.size()) {
        REXLOG_INFO("gta4-addons: {} already carries this {}", img.filename().string(),
                    files[f].name);
      } else {
        replace_with[i] = int(f);
        work = true;
      }
      break;
    }
    if (!matched[f] && append_missing) {
      appended.push_back(f);
      work = true;
    }
  }
  if (!work) {
    REXLOG_INFO("gta4-addons: {} needs no change", img.string());
    return true;
  }

  // The new table: kept entries in their order (`source` >= 0 is the old
  // index), then appended files (`source` = -1 - file index).
  std::vector<TocEntry> all;
  std::vector<int64_t> source;
  for (uint32_t i = 0; i < count; ++i) {
    if (drop[i])
      continue;
    TocEntry entry = entries[i];
    if (replace_with[i] >= 0)
      fill_meta(entry, files[size_t(replace_with[i])]);
    all.push_back(std::move(entry));
    source.push_back(i);
  }
  for (const size_t f : appended) {
    TocEntry entry;
    fill_meta(entry, files[f]);
    entry.name = files[f].name;
    all.push_back(std::move(entry));
    source.push_back(-1 - int64_t(f));
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
  // Payloads in order: copied from the original, or the replacement's bytes.
  std::vector<uint8_t> buffer(8u << 20);
  auto write_bundled = [&](TocEntry& entry, const NewFile& add) {
    entry.offset_block = block;
    out.write(reinterpret_cast<const char*>(add.data.data()), std::streamsize(add.data.size()));
    const size_t pad = size_t(entry.used_blocks) * kBlock - add.data.size();
    for (size_t at = 0; at < pad; at += zero.size())
      out.write(zero.data(), std::streamsize(std::min(zero.size(), pad - at)));
    block += entry.used_blocks;
  };
  for (size_t k = 0; k < all.size(); ++k) {
    auto& entry = all[k];
    if (source[k] < 0) {
      write_bundled(entry, files[size_t(-1 - source[k])]);
      continue;
    }
    const uint32_t i = uint32_t(source[k]);
    if (replace_with[i] >= 0) {
      write_bundled(entry, files[size_t(replace_with[i])]);
      continue;
    }
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
    if (encrypted) {
      CryptBlocks(new_header.data(), 16, key, true);
      CryptBlocks(new_toc.data(), new_toc.size(), key, true);
    }
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(new_header.data()), new_header.size());
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
  REXLOG_INFO("gta4-addons: {} rebuilt ({}-endian, {}): {} entries, {} file(s) added or updated",
              img.filename().string(), e.big ? "big" : "little",
              encrypted ? "encrypted" : "plain", all.size(), files.size());
  return true;
}

// Reference export for the PC->360 model converter: copies the named
// entries' stored bytes out of a vehicles.img (decrypting the TOC like the
// injector does) into out_dir, with a full TOC listing beside them. Runs
// once; a later boot that finds the files leaves them alone.
void ExportReference(const fs::path& img, const std::vector<uint8_t>& key, const fs::path& out_dir,
                     const std::vector<std::string>& names) {
  std::error_code ec;
  // Done once per list: a marker names what was asked for (cars missing from
  // the archive must not make every boot export again).
  std::string wanted;
  for (const auto& name : names) wanted += name + "\n";
  {
    std::vector<uint8_t> marker;
    if (ReadAll(out_dir / "exported.txt", marker) &&
        std::string(marker.begin(), marker.end()) == wanted &&
        fs::exists(out_dir / "vehicles_img_toc.txt", ec))
      return;
  }
  std::ifstream in(img, std::ios::binary);
  std::array<uint8_t, 0x14> header{};
  if (!in || !in.read(reinterpret_cast<char*>(header.data()), header.size()))
    return;
  Endian e{};
  bool encrypted = false;
  auto match = [&](const uint8_t* p) {
    uint32_t magic;
    std::memcpy(&magic, p, 4);
    if (magic == kImgMagic) { e.big = false; return true; }
    if (Swap32(magic) == kImgMagic) { e.big = true; return true; }
    return false;
  };
  if (!match(header.data())) {
    std::array<uint8_t, 0x14> trial = header;
    CryptBlocks(trial.data(), 16, key, false);
    if (key.empty() || !match(trial.data())) {
      REXLOG_WARN("gta4-addons: export: {} is not a readable IMG3", img.string());
      return;
    }
    encrypted = true;
    header = trial;
  }
  const uint32_t count = e.R32(header.data() + 8);
  const uint32_t toc_size = e.R32(header.data() + 12);
  if (count == 0 || count > 200000 || toc_size <= count * 16 || toc_size > (64u << 20))
    return;
  std::vector<uint8_t> toc(toc_size);
  if (!in.read(reinterpret_cast<char*>(toc.data()), std::streamsize(toc.size())))
    return;
  if (encrypted)
    CryptBlocks(toc.data(), toc.size(), key, false);
  fs::create_directories(out_dir, ec);
  std::ofstream listing(out_dir / "vehicles_img_toc.txt", std::ios::trunc);
  listing << "# " << img.string() << " endian=" << (e.big ? "big" : "little")
          << " encrypted=" << (encrypted ? 1 : 0) << " entries=" << count << "\n";
  listing << "# name word0 type offset_block used_blocks flags\n";
  const char* names_base = reinterpret_cast<const char*>(toc.data()) + count * 16;
  size_t names_len = toc_size - count * 16, cursor = 0;
  int exported = 0;
  for (uint32_t i = 0; i < count && cursor < names_len; ++i) {
    const uint8_t* p = toc.data() + i * 16;
    const uint32_t word0 = e.R32(p), type = e.R32(p + 4), offset = e.R32(p + 8);
    const uint16_t used = e.R16(p + 12), flags = e.R16(p + 14);
    const size_t len = strnlen(names_base + cursor, names_len - cursor);
    const std::string name(names_base + cursor, len);
    cursor += len + 1;
    char line[160];
    std::snprintf(line, sizeof(line), "%s %08X %08X %u %u %04X\n", name.c_str(), word0, type,
                  offset, used, flags);
    listing << line;
    if (std::find(names.begin(), names.end(), name) == names.end())
      continue;
    const uint64_t size = uint64_t(used) * kBlock - (flags & 0x7FF);
    std::vector<uint8_t> data(size);
    in.clear();
    in.seekg(std::streamoff(uint64_t(offset) * kBlock));
    if (!in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())))
      continue;
    std::ofstream out(out_dir / name, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    std::snprintf(line, sizeof(line), "%s.toc", name.c_str());
    std::ofstream meta(out_dir / line, std::ios::trunc);
    std::snprintf(line, sizeof(line), "word0=%08X type=%08X used_blocks=%u flags=%04X size=%llu\n",
                  word0, type, used, flags, (unsigned long long)size);
    meta << line;
    ++exported;
  }
  std::ofstream(out_dir / "exported.txt", std::ios::trunc) << wanted;
  REXLOG_INFO("gta4-addons: exported {} reference file(s) and the TOC listing to {}", exported,
              out_dir.string());
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

// Puts `line` into a named section ("car3" ... "end"), replacing lines an
// earlier run left for the same model. False when the file has no such
// section (the caller falls back to cloning the donor's rows).
bool InsertIntoSection(const fs::path& file, std::string_view section, std::string_view line,
                       const char* what) {
  TextLines probe;
  if (!LoadText(file, probe))
    return false;
  const auto has_section = std::any_of(probe.lines.begin(), probe.lines.end(), [&](const std::string& l) {
    return FirstToken(l) == section;
  });
  if (!has_section)
    return false;
  const std::string name = FirstToken(line);
  return EditText(file, what, [&](std::vector<std::string>& lines) {
    std::erase_if(lines, [&](const std::string& l) { return FirstToken(l) == name; });
    for (size_t i = 0; i < lines.size(); ++i) {
      if (FirstToken(lines[i]) != section)
        continue;
      for (size_t j = i + 1; j < lines.size(); ++j) {
        if (FirstToken(lines[j]) == "end") {
          lines.insert(lines.begin() + std::ptrdiff_t(j), WithEnding(line, lines[j]));
          return true;
        }
      }
    }
    return false;
  });
}

// One bundled vehicle: addons/<folder>/addon.txt names it and its donor (the
// stock car whose lines it is placed beside and whose per-model rows it
// borrows), beside <model>.xft/.xtd, handling.txt and vehicles_ide.txt; an
// optional carcols.txt carries its own colour section.
struct Addon {
  fs::path dir;
  std::string model, donor = "fbi", colours;
  std::string handling, ide;
  std::string carcols_section, carcols_row;
};

std::string Lower(std::string text) {
  for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::vector<Addon> FindAddons(const fs::path& app_addons) {
  std::vector<Addon> out;
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (auto it = fs::directory_iterator(app_addons, ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
    if (it->is_directory(ec))
      dirs.push_back(it->path());
  std::sort(dirs.begin(), dirs.end());
  for (const auto& dir : dirs) {
    TextLines manifest;
    if (!LoadText(dir / "addon.txt", manifest))
      continue;
    Addon a;
    a.dir = dir;
    for (auto line : manifest.lines) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = Lower(line.substr(0, eq));
      std::string value = line.substr(eq + 1);
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
      if (key == "model") a.model = Lower(value);
      else if (key == "donor") a.donor = Lower(value);
      else if (key == "colours" || key == "colors") a.colours = Lower(value);
    }
    if (a.colours.empty()) a.colours = a.donor;
    a.handling = ReadLineFile(dir / "handling.txt");
    a.ide = ReadLineFile(dir / "vehicles_ide.txt");
    TextLines carcols;
    if (LoadText(dir / "carcols.txt", carcols)) {
      for (auto line : carcols.lines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string token = FirstToken(line);
        if (token.empty() || token == "end" || token[0] == '#') continue;
        if (token == a.model) a.carcols_row = line;
        else if (a.carcols_section.empty()) a.carcols_section = token;
      }
    }
    std::error_code fec;
    if (a.model.empty() || a.handling.empty() || a.ide.empty() ||
        FirstToken(a.ide) != a.model || !fs::exists(dir / (a.model + ".xft"), fec) ||
        !fs::exists(dir / (a.model + ".xtd"), fec)) {
      REXLOG_WARN("gta4-addons: {} is incomplete; skipped", dir.string());
      continue;
    }
    out.push_back(std::move(a));
  }
  return out;
}

}  // namespace

void InstallBundled(const fs::path& app_addons, const fs::path& user_data_root) {
  std::error_code ec;
  if (!fs::exists(app_addons, ec)) return;  // nothing bundled
  const std::vector<Addon> addons = FindAddons(app_addons);
  if (addons.empty()) return;
  const fs::path game = FindGameRoot(user_data_root);
  if (game.empty()) {
    REXLOG_INFO("gta4-addons: no installed game yet; add-ons wait for the next boot");
    return;
  }

  // Every bundled car is its own vehicle beside the stock ones: no stock
  // model is replaced (so the FBI cars of 4-star chases stay FBI cars). The
  // model files are converted 360 resources (<model>.xft / .xtd, made by the
  // PC-to-360 converter in the mod's repo and validated against the stock
  // 360 FBI); the 360 build streams .xft/.xtd, so the PC .wft/.wtd files
  // earlier releases installed were never looked up. Each vehicle line goes
  // into vehicles.ide's cars section right after its donor's line, with its
  // own handling line, and colour/camera rows (its own or the donor's
  // copied). The .stock-bak files undo everything.
  //
  // What the earlier FBI-slot releases wrote over the FBI, and the stock
  // lines to put back when no genuine backup is at hand.
  auto legacy_handling = [](std::string_view line) {
    const auto t = Tokens(line);
    return t.size() > 1 && t[0] == "fbi" && t[1] == "9550.0";  // the Batmobile's mass
  };
  auto legacy_ide = [](std::string_view line) {
    const auto t = Tokens(line);
    return !t.empty() && t[0] == "fbi" && t.back().find("truckfirefx") != std::string::npos;
  };
  constexpr std::string_view kStockFbiHandling =
      "FBI          1550.0  7.5   85\t0.0 0.0 -0.24  \t\t0.0  5  0.22  1.0 150.0\t0.25 0.65 0.7  "
      "35.0\t\t1.35 1.10 13.5 0.15 0.48\t\t2.0  1.2  1.2  0.13 -0.13 0.0  0.5  \t\t1.0 1.0 0.8 "
      "1.5 \t0.0  25000 \t\t440010\t\t1\t\t\t\t0";
  constexpr std::string_view kStockFbiIde =
      "fbi,\t\tfbi,\t\tcar,\t\tFBI,\t\tFBI,\t\tVEH@STD,\tVEH@STD_LO4_LO4,\t100,\t999,\t\t"
      "0.2622, 0.2622,\t\t0,\t5,\t1.0\t,0,\t-";
  bool ok = true;
  // The base game and, when installed, each episode: the episodes read
  // their own copies of these files.
  std::vector<fs::path> data_roots = {game / "common" / "data"};
  // The title update mounts OVER the base at runtime, so a data file that
  // exists under game/update is the one the game reads: it gets the lines
  // too, or the base edit is shadowed.
  if (fs::exists(game / "update", ec)) {
    for (const char* sub : {"common/data", "update/common/data"}) {
      const fs::path extra = game / "update" / sub;
      if (fs::exists(extra / "vehicles.ide", ec) || fs::exists(extra / "handling.dat", ec))
        data_roots.push_back(extra);
    }
  }
  for (const char* episode : {"TLAD", "TBOGT"}) {
    const fs::path extra = game.parent_path() / "dlc" / episode / "common" / "data";
    if (fs::exists(extra / "vehicles.ide", ec))
      data_roots.push_back(extra);
  }
  for (const auto& data : data_roots) {
    if (fs::exists(data / "handling.dat", ec))
      ok &= RestoreDonorLine(data / "handling.dat", "fbi", legacy_handling, kStockFbiHandling,
                             "stock FBI handling");
    if (fs::exists(data / "vehicles.ide", ec))
      ok &= RestoreDonorLine(data / "vehicles.ide", "fbi", legacy_ide, kStockFbiIde,
                             "stock FBI vehicle");
    for (const auto& a : addons) {
      const std::string label = a.model;
      if (fs::exists(data / "handling.dat", ec))
        ok &= InsertAfterDonor(data / "handling.dat", a.donor, a.handling,
                               (label + " handling").c_str());
      if (fs::exists(data / "vehicles.ide", ec))
        ok &= InsertAfterDonor(data / "vehicles.ide", a.donor, a.ide, (label + " vehicle").c_str());
      if (fs::exists(data / "carcols.dat", ec)) {
        // Its own colour row when it brought one and the section exists;
        // otherwise a copy of a stock car's rows.
        if (a.carcols_row.empty() ||
            !InsertIntoSection(data / "carcols.dat", a.carcols_section, a.carcols_row,
                               (label + " colours").c_str()))
          ok &= CloneDonorLines(data / "carcols.dat", a.colours, a.model,
                                (label + " colours").c_str());
      }
      // Camera offsets: the donor's (the converted car keeps its physics).
      if (fs::exists(data / "vehOff.csv", ec))
        ok &= CloneDonorLines(data / "vehOff.csv", a.donor, a.model,
                              (label + " camera offsets").c_str());
    }
  }

  const fs::path img = FindVehiclesImg(game);
  if (img.empty()) {
    REXLOG_WARN("gta4-addons: vehicles.img was not found under {}", game.string());
    return;
  }
  // The console's archives are usually encrypted with the game's own RPF
  // key, which the installer leaves in the game root.
  std::vector<uint8_t> key;
  {
    std::vector<uint8_t> raw;
    if (ReadAll(game / "aes_key.bin", raw) && raw.size() == 32)
      key = std::move(raw);
    else
      REXLOG_WARN("gta4-addons: {} has no aes_key.bin; only a plain archive can be edited",
                  game.string());
  }
  // Every vehicles.img in the installed tree: base, title update, episodes.
  // Whichever the game streams from must carry the cars, so all of them get
  // them, and each is named in the log with its size for diagnosis.
  std::vector<fs::path> images;
  for (auto it = fs::recursive_directory_iterator(
           game, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
    if (it->is_regular_file(ec) && it->path().filename() == "vehicles.img")
      images.push_back(it->path());
  }
  for (const char* episode : {"TLAD", "TBOGT"}) {
    const fs::path dlc = game.parent_path() / "dlc" / episode;
    std::error_code dlc_ec;
    for (auto it = fs::recursive_directory_iterator(
             dlc, fs::directory_options::skip_permission_denied, dlc_ec);
         !dlc_ec && it != fs::recursive_directory_iterator(); it.increment(dlc_ec)) {
      if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
      if (it->is_regular_file(dlc_ec) && it->path().filename() == "vehicles.img")
        images.push_back(it->path());
    }
  }
  if (images.empty()) {
    REXLOG_WARN("gta4-addons: no vehicles.img anywhere under {}", game.string());
    return;
  }
  // Reference files for the PC->360 converter (logs/export on the console):
  // stock cars to build add-ons on - the FBI, low sports cars, cars with
  // extras (switchable parts) and the airport tug.
  {
    std::vector<std::string> refs;
    for (const char* car : {"fbi", "turismo", "infernus", "comet", "banshee", "coquette", "taxi",
                            "taxi2", "police", "police2", "noose", "sultan", "feroci",
                            "cavalcade", "airtug"}) {
      refs.push_back(std::string(car) + ".xft");
      refs.push_back(std::string(car) + ".xtd");
    }
    ExportReference(images.front(), key, app_addons.parent_path() / "logs" / "export", refs);
  }
  std::vector<fs::path> files;
  for (const auto& a : addons) {
    files.push_back(a.dir / (a.model + ".xft"));
    files.push_back(a.dir / (a.model + ".xtd"));
  }
  bool any = false;
  for (const auto& image : images) {
    REXLOG_INFO("gta4-addons: vehicles.img at {} ({} bytes)", image.string(),
                uint64_t(fs::file_size(image, ec)));
    any |= InjectIntoImg(image, key, files, /*append_missing=*/true,
                         {"batmobile.wft", "batmobile.wtd", "fbi.wft", "fbi.wtd"});
  }
  if (any && ok) {
    std::string names;
    for (const auto& a : addons) names += (names.empty() ? "" : ", ") + a.model;
    REXLOG_INFO("gta4-addons: installed as their own vehicles: {}", names);
  }
}

}  // namespace gta4::addons
