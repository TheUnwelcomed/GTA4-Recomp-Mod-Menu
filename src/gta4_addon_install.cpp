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
#include <map>
#include <set>
#include <atomic>
#include <cctype>
#include <cmath>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ostream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <zlib.h>

#include <rex/logging.h>
#include <rijndael-alg-fst.h>

namespace gta4::addons {
namespace {
namespace fs = std::filesystem;

// Shadow state for this launch (see the header).
std::atomic<bool> g_shadows_active{false};
fs::path g_launch_marker;
bool g_shadows_skipped = false;

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

// A plain read-and-write copy. v1.7.1's index copied no files at all on the
// console with the standard library's copy and directory-entry sizes, so the
// copy and the size below are done by hand.
bool CopyBytes(const fs::path& from, const fs::path& to) {
  std::ifstream in(from, std::ios::binary);
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!in || !out)
    return false;
  std::vector<char> chunk(1u << 20);
  while (in) {
    in.read(chunk.data(), std::streamsize(chunk.size()));
    const std::streamsize got = in.gcount();
    if (got > 0 && !out.write(chunk.data(), got))
      return false;
  }
  return in.eof() && bool(out);
}

// The size of a file, by opening it when the file system will not say.
uint64_t SizeOf(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (!ec)
    return size;
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    return UINT64_MAX;
  const std::streamoff end = in.tellg();
  return end < 0 ? UINT64_MAX : uint64_t(end);
}

bool BackupOnce(const fs::path& file) {
  std::error_code ec;
  const fs::path bak = file.string() + ".stock-bak";
  if (fs::exists(bak, ec)) return true;
  if (!CopyBytes(file, bak)) {
    REXLOG_WARN("gta4-addons: could not back up {}", file.string());
    return false;
  }
  return true;
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

// The name a line is filed under: its first field, or the second on the
// console's vehOff.csv lines ("VEHICLE <model> FIELD BONNET_FWD POS ...").
std::string KeyToken(std::string_view line, size_t* at = nullptr, size_t* len = nullptr) {
  size_t a = 0, l = 0;
  std::string token = FirstToken(line, &a, &l);
  if (token == "vehicle") {
    size_t i = a + l;
    while (i < line.size() && IsSeparator(line[i])) ++i;
    a = i;
    while (i < line.size() && !IsSeparator(line[i])) ++i;
    l = i - a;
    token = std::string(line.substr(a, l));
    for (char& c : token) c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  if (at) *at = a;
  if (len) *len = l;
  return token;
}

// A line's fields as written (case kept), split the way the game splits one:
// on spaces, tabs and commas.
std::vector<std::string> RawTokens(std::string_view line) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && IsSeparator(line[i])) ++i;
    const size_t start = i;
    while (i < line.size() && !IsSeparator(line[i])) ++i;
    if (i > start)
      tokens.emplace_back(line.substr(start, i - start));
  }
  return tokens;
}

// The console's vehicles.ide has 15 fields a line where the PC's has 16 (an
// extra number before the flags), and its own animation groups. An add-on's
// PC line is rebuilt on its template car's console line: the template's type
// and animation groups (the converted model has the template's seats and
// doors), with the add-on's name, texture dictionary, handling, game name,
// frequency, count, wheel sizes, dirt, swankness, LOD multiplier and flags.
// Anything else (a PC-format template) leaves the add-on's line as it is.
// The frequency is the share of traffic the model gets once it is loaded:
// an add-on stays out of traffic (0) unless its addon.txt asks for it, so
// nobody else drives the Batmobile.
std::string ConsoleIdeLine(const std::string& addon_line, const std::string& template_line,
                           bool traffic = false) {
  const auto a = RawTokens(addon_line), t = RawTokens(template_line);
  if (t.size() != 15 || a.size() < 15)
    return addon_line;
  std::vector<std::string> r = t;
  for (const size_t i : {0, 1, 3, 4, 7, 8, 9, 10, 11, 12, 13})
    r[i] = a[i];
  r[14] = a.back();
  if (!traffic)
    r[7] = "0";
  std::string line;
  for (size_t i = 0; i < 14; ++i)
    line += r[i] + (i < 13 ? ", " : "");
  return line + "\t" + r[14];
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
    // Already right: exactly one line for the name, identical, and in the
    // donor's section (no "end" between them) - leave it where it is.
    std::vector<size_t> mine;
    size_t donor_at = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
      const std::string token = FirstToken(lines[i]);
      if (token == name)
        mine.push_back(i);
      else if (token == donor && donor_at == lines.size())
        donor_at = i;
    }
    if (mine.size() == 1 && donor_at < lines.size() && Tokens(lines[mine[0]]) == Tokens(line)) {
      const size_t from = std::min(mine[0], donor_at), to = std::max(mine[0], donor_at);
      bool split = false;
      for (size_t i = from + 1; i < to; ++i)
        split |= FirstToken(lines[i]) == "end";
      if (!split)
        return true;
    }
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
  auto twin_of = [&](const std::string& line) {
    size_t at = 0, len = 0;
    KeyToken(line, &at, &len);
    const std::string original = line.substr(at, len);
    const bool upper = std::none_of(original.begin(), original.end(),
                                    [](char c) { return std::islower(static_cast<unsigned char>(c)); });
    std::string name(model);
    for (char& c : name)
      c = char(upper ? std::toupper(static_cast<unsigned char>(c)) : c);
    std::string twin = line;
    twin.replace(at, len, name);
    return twin;
  };
  return EditText(file, what, [&](std::vector<std::string>& lines) {
    // Already right: one twin per donor line, identical, each after its donor
    // line in the same section (other add-ons' twins may sit between).
    std::vector<size_t> donors, mine;
    for (size_t i = 0; i < lines.size(); ++i) {
      const std::string token = KeyToken(lines[i]);
      if (token == donor)
        donors.push_back(i);
      else if (token == model)
        mine.push_back(i);
    }
    bool right = !donors.empty() && donors.size() == mine.size();
    for (size_t k = 0; right && k < donors.size(); ++k) {
      right = mine[k] > donors[k] && lines[mine[k]] == twin_of(lines[donors[k]]);
      for (size_t x = donors[k] + 1; right && x < mine[k]; ++x)
        right = FirstToken(lines[x]) != "end" && KeyToken(lines[x]) != donor;
    }
    if (right)
      return true;
    std::erase_if(lines, [&](const std::string& l) { return KeyToken(l) == model; });
    for (size_t i = 0; i < lines.size(); ++i) {
      if (KeyToken(lines[i]) != donor)
        continue;
      lines.insert(lines.begin() + std::ptrdiff_t(i + 1), twin_of(lines[i]));
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
                   const std::vector<std::string>& remove_names = {},
                   const std::vector<std::string>& owned_prefixes = {}) {
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
  // So are a map set's files an earlier release added that this one no
  // longer has (v1.7.32: the island's big pieces became tiles): names with a
  // set's own prefix that no bundled file has.
  std::vector<bool> drop(count, false);
  int stale = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (std::find(remove_names.begin(), remove_names.end(), entries[i].name) !=
        remove_names.end()) {
      drop[i] = true;
      work = true;
      REXLOG_INFO("gta4-addons: dropping unused {} from {}", entries[i].name,
                  img.filename().string());
      continue;
    }
    const bool owned = std::any_of(owned_prefixes.begin(), owned_prefixes.end(), [&](const std::string& p) {
      return p.size() >= 3 && entries[i].name.rfind(p, 0) == 0;
    });
    if (owned && std::none_of(rsc_paths.begin(), rsc_paths.end(), [&](const fs::path& f) {
          return f.filename().string() == entries[i].name;
        })) {
      drop[i] = true;
      work = true;
      ++stale;
    }
  }
  if (stale)
    REXLOG_INFO("gta4-addons: dropping {} map file(s) from {} that this release no longer has", stale,
                img.filename().string());
  for (size_t f = 0; f < files.size(); ++f) {
    for (uint32_t i = 0; i < count; ++i) {
      if (entries[i].name != files[f].name)
        continue;
      matched[f] = true;
      const uint32_t stored =
          uint32_t(entries[i].used_blocks) * kBlock - uint32_t(entries[i].flags & 0x7FF);
      // Current only when the stored bytes ARE the bundled file: a fixed
      // conversion can keep the exact size of the one it replaces.
      bool same = stored == files[f].data.size() && entries[i].word0 == files[f].rsc_flags;
      if (same) {
        std::vector<uint8_t> current(files[f].data.size());
        in.clear();
        in.seekg(std::streamoff(uint64_t(entries[i].offset_block) * kBlock));
        same = bool(in.read(reinterpret_cast<char*>(current.data()),
                            std::streamsize(current.size()))) &&
               std::memcmp(current.data(), files[f].data.data(), current.size()) == 0;
      }
      if (same) {
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

// Writes one archive's table of contents (name, flags, type, block, size)
// to `listing`; false when it is not a readable IMG3.
bool ListImg(const fs::path& img, const std::vector<uint8_t>& key, std::ostream& listing) {
  std::ifstream in(img, std::ios::binary);
  std::array<uint8_t, 0x14> header{};
  if (!in || !in.read(reinterpret_cast<char*>(header.data()), header.size()))
    return false;
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
    if (key.empty() || !match(trial.data()))
      return false;
    encrypted = true;
    header = trial;
  }
  const uint32_t count = e.R32(header.data() + 8);
  const uint32_t toc_size = e.R32(header.data() + 12);
  if (count == 0 || count > 200000 || toc_size <= count * 16 || toc_size > (64u << 20))
    return false;
  std::vector<uint8_t> toc(toc_size);
  if (!in.read(reinterpret_cast<char*>(toc.data()), std::streamsize(toc.size())))
    return false;
  if (encrypted)
    CryptBlocks(toc.data(), toc.size(), key, false);
  listing << "# " << img.string() << " endian=" << (e.big ? "big" : "little")
          << " encrypted=" << (encrypted ? 1 : 0) << " entries=" << count << "\n";
  const char* names = reinterpret_cast<const char*>(toc.data()) + count * 16;
  size_t names_len = toc_size - count * 16, cursor = 0;
  for (uint32_t i = 0; i < count && cursor < names_len; ++i) {
    const uint8_t* p = toc.data() + i * 16;
    const size_t len = strnlen(names + cursor, names_len - cursor);
    char line[200];
    std::snprintf(line, sizeof(line), "%.*s %08X %08X %u %u %04X\n", int(len), names + cursor,
                  e.R32(p), e.R32(p + 4), e.R32(p + 8), unsigned(e.R16(p + 12)),
                  unsigned(e.R16(p + 14)));
    listing << line;
    cursor += len + 1;
  }
  return true;
}

// RPF2, the archive format the game reads its data from (common.rpf holds
// vehicles.ide, handling.dat and the rest). Read here for the index and for
// the add-ons' shadow archive (below).
bool IsTextData(std::string ext) {
  for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".dat" || ext == ".ide" || ext == ".csv" || ext == ".txt" || ext == ".xml" ||
         ext == ".ipl" || ext == ".ini" || ext == ".zon";
}

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void PutLe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

constexpr uint32_t kRpf2Magic = 0x32465052u;  // 'RPF2'
constexpr uint64_t kRpfTocOffset = 0x800, kRpfBlock = 0x800;

struct RpfFile {
  std::string path;  // from the root, lowercase ("/data/vehicles.ide")
  size_t toc_offset = 0;
  uint32_t size = 0, offset = 0, stored = 0;
  bool compressed = false, resource = false;
};
struct Rpf {
  uint32_t toc_size = 0, count = 0;
  bool encrypted = false;
  uint64_t file_size = 0;
  std::vector<uint8_t> toc;  // decrypted
  std::vector<RpfFile> files;
  const RpfFile* Find(std::string_view path) const {
    for (const auto& f : files)
      if (f.path == path)
        return &f;
    return nullptr;
  }
};

std::optional<Rpf> ReadRpf(const fs::path& path, const std::vector<uint8_t>& key) {
  std::ifstream in(path, std::ios::binary);
  std::array<uint8_t, 20> header{};
  if (!in || !in.read(reinterpret_cast<char*>(header.data()), header.size()) ||
      Le32(header.data()) != kRpf2Magic)
    return std::nullopt;
  Rpf rpf;
  rpf.toc_size = Le32(header.data() + 4);
  rpf.count = Le32(header.data() + 8);
  rpf.encrypted = Le32(header.data() + 16) != 0;
  rpf.file_size = SizeOf(path);
  if (!rpf.count || rpf.count > 1000000 || uint64_t(rpf.count) * 16 > rpf.toc_size ||
      rpf.toc_size > (64u << 20) || (rpf.encrypted && key.size() != 32) ||
      rpf.file_size == UINT64_MAX || kRpfTocOffset + rpf.toc_size > rpf.file_size)
    return std::nullopt;
  rpf.toc.resize(rpf.toc_size);
  in.seekg(std::streamoff(kRpfTocOffset));
  if (!in.read(reinterpret_cast<char*>(rpf.toc.data()), std::streamsize(rpf.toc.size())))
    return std::nullopt;
  if (rpf.encrypted)
    CryptBlocks(rpf.toc.data(), rpf.toc.size(), key, false);
  const uint8_t* toc = rpf.toc.data();
  const size_t names = size_t(rpf.count) * 16;
  auto name_of = [&](uint32_t i) {
    const uint32_t at = Le32(toc + i * 16);
    if (names + at >= rpf.toc.size())
      return std::string("?");
    std::string name(reinterpret_cast<const char*>(toc + names + at),
                     strnlen(reinterpret_cast<const char*>(toc + names + at),
                             rpf.toc.size() - names - at));
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    return name;
  };
  if (!(Le32(toc + 8) & 0x80000000u))
    return std::nullopt;  // the root must be a directory
  std::vector<std::pair<uint32_t, std::string>> pending = {{0, ""}};
  std::vector<bool> seen(rpf.count);
  while (!pending.empty()) {
    auto [dir, base_path] = pending.back();
    pending.pop_back();
    if (seen[dir])
      continue;
    seen[dir] = true;
    const uint32_t first = Le32(toc + dir * 16 + 8) & 0x7FFFFFFF;
    const uint32_t n = Le32(toc + dir * 16 + 12) & 0x0FFFFFFF;
    if (uint64_t(first) + n > rpf.count)
      return std::nullopt;
    for (uint32_t i = first; i < first + n; ++i) {
      const uint8_t* e = toc + i * 16;
      const std::string child = base_path + "/" + name_of(i);
      if (Le32(e + 8) & 0x80000000u) {
        pending.emplace_back(i, child);
        continue;
      }
      RpfFile f;
      f.path = child;
      f.toc_offset = size_t(i) * 16;
      f.size = Le32(e + 4);
      const uint32_t w2 = Le32(e + 8), w3 = Le32(e + 12);
      f.resource = (w3 & 0xC0000000u) == 0xC0000000u;
      f.offset = f.resource ? (w2 & 0x7FFFFF00u) : w2;
      f.stored = f.resource ? f.size : (w3 & 0xBFFFFFFFu);
      f.compressed = !f.resource && (w3 & 0x40000000u) != 0;
      if (uint64_t(f.offset) + f.stored > rpf.file_size)
        return std::nullopt;
      rpf.files.push_back(std::move(f));
    }
  }
  return rpf;
}

// A plain (non-resource) file's contents, inflated when stored compressed.
bool ReadRpfData(const fs::path& path, const RpfFile& f, std::vector<uint8_t>& out) {
  if (f.resource || f.size > (64u << 20) || f.stored > (64u << 20))
    return false;
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> raw(f.stored);
  in.seekg(std::streamoff(f.offset));
  if (!in || !in.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size())))
    return false;
  if (!f.compressed) {
    raw.resize(std::min<size_t>(raw.size(), f.size));
    out = std::move(raw);
    return true;
  }
  out.assign(f.size, 0);
  z_stream z{};
  if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
    return false;
  z.next_in = raw.data();
  z.avail_in = uInt(raw.size());
  z.next_out = out.data();
  z.avail_out = uInt(out.size());
  const int status = inflate(&z, Z_FINISH);
  inflateEnd(&z);
  return status == Z_STREAM_END && z.avail_out == 0;
}

int ListRpf(const fs::path& path, const std::vector<uint8_t>& key, std::ostream& listing,
            const fs::path& data_out, const std::string& prefix) {
  const auto rpf = ReadRpf(path, key);
  if (!rpf)
    return 0;
  listing << "# " << path.string() << " entries=" << rpf->count << "\n";
  int copied = 0;
  for (const auto& f : rpf->files) {
    listing << f.path << " " << f.size << (f.resource ? " rsc" : "") << "\n";
    if (f.resource || f.size > (2u << 20) || !IsTextData(fs::path(f.path).extension().string()))
      continue;
    std::vector<uint8_t> text;
    if (!ReadRpfData(path, f, text))
      continue;
    std::string flat = prefix + f.path;
    std::replace(flat.begin(), flat.end(), '/', '_');
    std::ofstream(data_out / flat, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(text.data()), std::streamsize(text.size()));
    ++copied;
  }
  return copied;
}

// `path` relative to `base` as text ("game/common/data/vehicles.ide"); the
// library's own relative() answers "." on the console.
std::string RelativeTo(const fs::path& path, const fs::path& base) {
  const std::string p = path.generic_string(), b = base.generic_string();
  if (!b.empty() && p.size() > b.size() && p.compare(0, b.size(), b) == 0 && p[b.size()] == '/')
    return p.substr(b.size() + 1);
  return p;
}

// The game's layout for the converter (logs/export/index on the console):
// every file under the install with its size, every archive's table of
// contents, and copies of the small text data files (timecycle, materials,
// weather, handling...). Once per version of this list.
void ExportGameIndex(const fs::path& game, const std::vector<uint8_t>& key, const fs::path& out) {
  std::error_code ec;
  constexpr std::string_view kVersion = "index v3\n";
  {
    std::vector<uint8_t> marker;
    if (ReadAll(out / "done.txt", marker) && std::string(marker.begin(), marker.end()) == kVersion)
      return;
  }
  fs::create_directories(out / "data", ec);
  fs::create_directories(out / "toc", ec);
  std::ofstream files(out / "files.txt", std::ios::trunc);
  int archives = 0, copied = 0;
  std::vector<fs::path> roots = {game};
  if (fs::exists(game.parent_path() / "dlc", ec))
    roots.push_back(game.parent_path() / "dlc");
  for (const auto& root : roots) {
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it.depth() > 8) { it.disable_recursion_pending(); continue; }
      std::error_code fec;
      if (!it->is_regular_file(fec))
        continue;
      const auto& path = it->path();
      const uint64_t size = SizeOf(path);
      const std::string relative = RelativeTo(path, game.parent_path());
      files << relative << " " << size << "\n";
      std::string ext = path.extension().string();
      for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
      if (ext == ".img") {
        std::string flat = relative;
        std::replace(flat.begin(), flat.end(), '/', '_');
        std::ofstream listing(out / "toc" / (flat + ".txt"), std::ios::trunc);
        archives += ListImg(path, key, listing) ? 1 : 0;
      } else if (ext == ".rpf") {
        std::string flat = relative;
        std::replace(flat.begin(), flat.end(), '/', '_');
        std::ofstream listing(out / "toc" / (flat + ".txt"), std::ios::trunc);
        copied += ListRpf(path, key, listing, out / "data", flat);
        ++archives;
      } else if (IsTextData(ext) && size < (2u << 20) &&
                 path.string().find(".stock-bak") == std::string::npos) {
        std::string flat = relative;
        std::replace(flat.begin(), flat.end(), '/', '_');
        copied += CopyBytes(path, out / "data" / flat) ? 1 : 0;
      }
    }
  }
  std::ofstream(out / "done.txt", std::ios::trunc) << kVersion;
  REXLOG_INFO("gta4-addons: game index exported to {} ({} archives listed, {} data files)",
              out.string(), archives, copied);
}

// Map references for the map converter (logs/export/maps on the console):
// from a few of the game's own map archives (the smallest, a middle one and
// the largest of those under a maps folder), a few entries of every kind
// they hold (drawables, drawable dictionaries, texture dictionaries, bounds,
// bound dictionaries, stream placements, navmeshes...), the smallest of each
// kind first, with each archive's full table of contents, and the .ide and
// .wpl files beside them. What the PC map formats are converted against, as
// the cars were against the stock cars. Once per version of this list; at
// most 40 MB.
// One map archive's entries copied to `dir` (its table in toc.txt, each
// entry's raw resource beside a .toc line of its header words), with the
// .ide/.wpl/.ipl beside the archive: up to `per_kind_max` entries of each
// kind, none over 6 MB, while `total` stays under `total_cap`.
int CopyImgEntries(const fs::path& img, const std::vector<uint8_t>& key, const fs::path& dir, int per_kind_max,
                   uint64_t& total, uint64_t total_cap) {
  std::error_code ec;
  int copied = 0;
  fs::create_directories(dir, ec);
  do {
    {
      std::ofstream listing(dir / "toc.txt", std::ios::trunc);
      ListImg(img, key, listing);
    }
    // the table again, read here to pick and copy entries
    std::ifstream in(img, std::ios::binary);
    std::array<uint8_t, 0x14> header{};
    if (!in || !in.read(reinterpret_cast<char*>(header.data()), header.size()))
      break;
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
      if (key.empty() || !match(trial.data()))
        break;
      encrypted = true;
      header = trial;
    }
    const uint32_t count = e.R32(header.data() + 8), toc_size = e.R32(header.data() + 12);
    if (count == 0 || count > 200000 || toc_size <= count * 16 || toc_size > (64u << 20))
      break;
    std::vector<uint8_t> toc(toc_size);
    if (!in.read(reinterpret_cast<char*>(toc.data()), std::streamsize(toc.size())))
      break;
    if (encrypted)
      CryptBlocks(toc.data(), toc.size(), key, false);
    struct Entry {
      std::string name, ext;
      uint64_t offset = 0, size = 0;
      uint32_t word0 = 0, type = 0;
      uint16_t flags = 0;
    };
    std::vector<Entry> entries;
    const char* names = reinterpret_cast<const char*>(toc.data()) + count * 16;
    size_t names_len = toc_size - count * 16, cursor = 0;
    for (uint32_t i = 0; i < count && cursor < names_len; ++i) {
      const uint8_t* p = toc.data() + i * 16;
      const size_t len = strnlen(names + cursor, names_len - cursor);
      Entry entry;
      entry.name.assign(names + cursor, len);
      cursor += len + 1;
      entry.word0 = e.R32(p);
      entry.type = e.R32(p + 4);
      entry.offset = uint64_t(e.R32(p + 8)) * kBlock;
      entry.flags = e.R16(p + 14);
      entry.size = uint64_t(e.R16(p + 12)) * kBlock - (entry.flags & 0x7FF);
      const auto dot = entry.name.rfind('.');
      entry.ext = dot == std::string::npos ? std::string() : entry.name.substr(dot + 1);
      for (char& c : entry.ext) c = char(std::tolower(static_cast<unsigned char>(c)));
      if (entry.name.find('/') == std::string::npos && entry.name.find('\\') == std::string::npos &&
          entry.name.find("..") == std::string::npos && !entry.name.empty())
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.ext != b.ext ? a.ext < b.ext : a.size < b.size; });
    std::map<std::string, int> per_kind;
    for (const auto& entry : entries) {
      if (per_kind[entry.ext] >= per_kind_max || entry.size > (6u << 20) || total + entry.size > total_cap)
        continue;
      std::vector<uint8_t> data(entry.size);
      in.clear();
      in.seekg(std::streamoff(entry.offset));
      if (!in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())))
        continue;
      std::ofstream file(dir / entry.name, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
      char line[160];
      std::snprintf(line, sizeof(line), "word0=%08X type=%08X flags=%04X size=%llu\n", entry.word0, entry.type,
                    unsigned(entry.flags), (unsigned long long)entry.size);
      std::ofstream(dir / (entry.name + ".toc"), std::ios::trunc) << line;
      ++per_kind[entry.ext];
      total += entry.size;
      ++copied;
    }
    // the placements and definitions beside the archive
    for (const char* ext : {".ide", ".wpl", ".ipl"}) {
      fs::path beside = img;
      beside.replace_extension(ext);
      if (fs::exists(beside, ec) && SizeOf(beside) < (4u << 20) && CopyBytes(beside, dir / beside.filename()))
        ++copied;
    }
  } while (false);
  return copied;
}

void ExportMapReference(const fs::path& game, const std::vector<uint8_t>& key, const fs::path& out) {
  std::error_code ec;
  constexpr std::string_view kVersion = "maps v2\n";
  {
    std::vector<uint8_t> marker;
    if (ReadAll(out / "done.txt", marker) && std::string(marker.begin(), marker.end()) == kVersion)
      return;
  }
  std::vector<std::pair<uint64_t, fs::path>> archives;
  std::vector<fs::path> extras;
  for (auto it = fs::recursive_directory_iterator(game, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 8) { it.disable_recursion_pending(); continue; }
    std::error_code fec;
    if (!it->is_regular_file(fec))
      continue;
    std::string path = it->path().generic_string(), ext = it->path().extension().string();
    for (char& c : path) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".img" && path.find("/maps/") != std::string::npos)
      archives.emplace_back(SizeOf(it->path()), it->path());
    else if (ext == ".img" && (path.ends_with("/paths.img") || path.ends_with("/navmeshes.img")))
      extras.push_back(it->path());  // (the traffic's nodes and the pedestrians' navmeshes)
  }
  fs::create_directories(out, ec);
  std::ofstream summary(out / "maps.txt", std::ios::trunc);
  summary << "# map archives under the install: " << archives.size() << "\n";
  std::sort(archives.begin(), archives.end());
  for (const auto& [size, path] : archives)
    summary << RelativeTo(path, game.parent_path()) << " " << size << "\n";
  std::vector<fs::path> picked;
  if (!archives.empty()) {
    for (const size_t i : {size_t(0), archives.size() / 2, archives.size() - 1})
      if (std::find(picked.begin(), picked.end(), archives[i].second) == picked.end())
        picked.push_back(archives[i].second);
  }
  for (const auto& extra : extras) {
    summary << RelativeTo(extra, game.parent_path()) << " " << SizeOf(extra) << "\n";
    if (std::find(picked.begin(), picked.end(), extra) == picked.end())
      picked.push_back(extra);
  }
  uint64_t total = 0;
  int copied = 0;
  for (const auto& img : picked) {
    std::string flat = RelativeTo(img, game.parent_path());
    std::replace(flat.begin(), flat.end(), '/', '_');
    copied += CopyImgEntries(img, key, out / flat, 3, total, 40ull << 20);
  }
  std::ofstream(out / "done.txt", std::ios::trunc) << kVersion;
  REXLOG_INFO("gta4-addons: map references exported to {} ({} map archives found, {} files from {}, {} bytes)",
              out.string(), archives.size(), copied, picked.size(), total);
}


// v1.7.29: the map converter's shader and collision references (gta_trees,
// the cutouts, the fences' and windows' shaders; props' bound dictionaries)
// and the game's own map lists. Whole small prop archives, every entry, and
// common.rpf's gta.dat, images.txt and water.dat. Once per version.
void ExportMapSets(const fs::path& game, const std::vector<uint8_t>& key, const fs::path& out) {
  std::error_code ec;
  constexpr std::string_view kVersion = "mapsets v1\n";
  {
    std::vector<uint8_t> marker;
    if (ReadAll(out / "done.txt", marker) && std::string(marker.begin(), marker.end()) == kVersion)
      return;
  }
  constexpr std::string_view kWanted[] = {
      "/maps/props/vegetation/ext_veg.img", "/maps/props/vegetation/int_veg.img",
      "/maps/props/roadside/fences.img",    "/maps/props/windows/gen_win.img",
      "/maps/props/residential/lights.img", "/maps/props/roadside/lamppost.img",
  };
  fs::create_directories(out, ec);
  std::ofstream summary(out / "sets.txt", std::ios::trunc);
  uint64_t total = 0;
  int copied = 0, archives = 0;
  for (auto it = fs::recursive_directory_iterator(game, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 8) { it.disable_recursion_pending(); continue; }
    std::error_code fec;
    if (!it->is_regular_file(fec))
      continue;
    std::string path = it->path().generic_string();
    for (char& c : path) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (std::none_of(std::begin(kWanted), std::end(kWanted), [&](std::string_view w) { return path.ends_with(w); }))
      continue;
    std::string flat = RelativeTo(it->path(), game.parent_path());
    std::replace(flat.begin(), flat.end(), '/', '_');
    const int n = CopyImgEntries(it->path(), key, out / flat, 100000, total, 48ull << 20);
    summary << RelativeTo(it->path(), game.parent_path()) << " " << SizeOf(it->path()) << " " << n << "\n";
    copied += n;
    ++archives;
  }
  // the map lists the converter's additions go into
  for (const fs::path& rpf_path : {game / "common.rpf"}) {
    const auto rpf = ReadRpf(rpf_path, key);
    if (!rpf)
      continue;
    for (const char* inner : {"/data/gta.dat", "/data/images.txt", "/data/water.dat"}) {
      const RpfFile* f = rpf->Find(inner);
      std::vector<uint8_t> data;
      if (f && ReadRpfData(rpf_path, *f, data)) {
        std::ofstream(out / fs::path(inner).filename(), std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        ++copied;
      }
    }
  }
  std::ofstream(out / "done.txt", std::ios::trunc) << kVersion;
  REXLOG_INFO("gta4-addons: map sets exported to {} ({} archives, {} files, {} bytes)", out.string(), archives, copied,
              total);
}

// Niko's own model for the converter (logs/export/playerped on the console):
// the player's ped archive (playerped.rpf, as the disc has it) and the files
// the installer unpacked from it, to build outfits on (their skeleton and
// the console's ped shaders). Once per version of this list; nothing over
// 96 MB is copied.
void ExportPlayerPed(const fs::path& game, const fs::path& out) {
  std::error_code ec;
  constexpr std::string_view kVersion = "playerped v1\n";
  {
    std::vector<uint8_t> marker;
    if (ReadAll(out / "done.txt", marker) && std::string(marker.begin(), marker.end()) == kVersion)
      return;
  }
  std::vector<fs::path> found;
  for (auto it = fs::recursive_directory_iterator(game, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 8) { it.disable_recursion_pending(); continue; }
    std::string stem = it->path().stem().string();
    for (char& c : stem) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (stem == "playerped")
      found.push_back(it->path());
  }
  fs::create_directories(out, ec);
  std::ofstream listing(out / "found.txt", std::ios::trunc);
  int copied = 0;
  uint64_t total = 0;
  auto copy = [&](const fs::path& from, const std::string& name) {
    const uint64_t size = SizeOf(from);
    listing << RelativeTo(from, game.parent_path()) << " " << size << "\n";
    if (total + size > (96ull << 20))
      return;
    if (CopyBytes(from, out / name)) {
      ++copied;
      total += size;
    }
  };
  for (const auto& path : found) {
    std::error_code fec;
    if (fs::is_regular_file(path, fec)) {
      copy(path, path.filename().string());
    } else if (fs::is_directory(path, fec)) {
      fs::create_directories(out / "files", fec);
      for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, fec);
           !fec && it != fs::recursive_directory_iterator(); it.increment(fec))
        if (it->is_regular_file(fec))
          copy(it->path(), "files/" + it->path().filename().string());
    }
  }
  std::ofstream(out / "done.txt", std::ios::trunc) << kVersion;
  REXLOG_INFO("gta4-addons: player model exported to {} ({} places named playerped, {} files, {} bytes)",
              out.string(), found.size(), copied, total);
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
  std::string template_model;  // the stock car it was converted on (default: the donor)
  bool traffic = false;        // traffic=yes: other drivers may use it too
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
      else if (key == "template") a.template_model = Lower(value);
      else if (key == "traffic") a.traffic = Lower(value) == "yes" || value == "1";
    }
    if (a.colours.empty()) a.colours = a.donor;
    if (a.template_model.empty()) a.template_model = a.donor;
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

// What the earlier FBI-slot releases wrote over the FBI, and the stock lines
// to put back when no genuine backup is at hand.
bool LegacyHandling(std::string_view line) {
  const auto t = Tokens(line);
  return t.size() > 1 && t[0] == "fbi" && t[1] == "9550.0";  // the Batmobile's mass
}
bool LegacyIde(std::string_view line) {
  const auto t = Tokens(line);
  return !t.empty() && t[0] == "fbi" && t.back().find("truckfirefx") != std::string::npos;
}
constexpr std::string_view kStockFbiHandling =
    "FBI          1550.0  7.5   85\t0.0 0.0 -0.24  \t\t0.0  5  0.22  1.0 150.0\t0.25 0.65 0.7  "
    "35.0\t\t1.35 1.10 13.5 0.15 0.48\t\t2.0  1.2  1.2  0.13 -0.13 0.0  0.5  \t\t1.0 1.0 0.8 "
    "1.5 \t0.0  25000 \t\t440010\t\t1\t\t\t\t0";
constexpr std::string_view kStockFbiIde =
    "fbi,\t\tfbi,\t\tcar,\t\tFBI,\t\tFBI,\t\tVEH@STD,\tNULL,\t\t100,\t999,\t\t"
    "0.2622, 0.2622,\t\t0,\t5,\t1.0\t-";

// The template car's line in a vehicles.ide.
std::string TemplateLine(const fs::path& file, const std::string& model) {
  TextLines text;
  if (!LoadText(file, text))
    return {};
  for (auto line : text.lines) {
    if (FirstToken(line) != model)
      continue;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    return line;
  }
  return {};
}

// The donor's handling id, from the vehicles.ide beside handling.dat (its
// line's fourth field: the Cavalcade's is CAVCADE, not its model name), or
// the model name itself when that cannot be read. (v1.7.20 looked for the
// model name and placed no handling for the Lazar 3: it drove without any.)
std::string DonorHandlingId(const fs::path& handling_file, const std::string& donor) {
  TextLines ide;
  if (!LoadText(handling_file.parent_path() / "vehicles.ide", ide))
    return donor;
  for (const auto& line : ide.lines) {
    const auto t = Tokens(line);
    if (t.size() > 3 && t[0] == donor)
      return t[3];
  }
  return donor;
}

// Everything one data file takes for the add-ons, chosen by its name: each
// vehicle line right after its donor's line in vehicles.ide's cars section
// (in the console's format), its handling line, and colour and camera rows
// (its own or the donor's copied).
bool EditDataFile(const fs::path& file, const std::vector<Addon>& addons) {
  const std::string name = Lower(file.filename().string());
  bool fine = true;
  if (name == "handling.dat")
    fine &= RestoreDonorLine(file, "fbi", LegacyHandling, kStockFbiHandling, "stock FBI handling");
  if (name == "vehicles.ide")
    fine &= RestoreDonorLine(file, "fbi", LegacyIde, kStockFbiIde, "stock FBI vehicle");
  for (const auto& a : addons) {
    const std::string label = a.model;
    if (name == "handling.dat")
      fine &= InsertAfterDonor(file, DonorHandlingId(file, a.donor), a.handling, (label + " handling").c_str());
    if (name == "vehicles.ide")
      fine &= InsertAfterDonor(file, a.donor,
                               ConsoleIdeLine(a.ide, TemplateLine(file, a.template_model), a.traffic),
                               (label + " vehicle").c_str());
    if (name == "carcols.dat") {
      // Its own colour row when it brought one and the section exists;
      // otherwise a copy of a stock car's rows.
      if (a.carcols_row.empty() ||
          !InsertIntoSection(file, a.carcols_section, a.carcols_row, (label + " colours").c_str()))
        fine &= CloneDonorLines(file, a.colours, a.model, (label + " colours").c_str());
    }
    // Camera offsets: the donor's (the converted car keeps its body shape).
    if (name == "vehoff.csv")
      fine &= CloneDonorLines(file, a.donor, a.model, (label + " camera offsets").c_str());
  }
  return fine;
}

// ---------------------------------------------------------------------------
// Map sets: places for the DeLorean's time travel (see the header). Each is a
// folder addons/maps/<name> holding its resources (.xdr models, .xtd texture
// dictionaries, the collision dictionary gtxd.xbd), objs.ide (its model
// lines), place.txt (one line per piece: model, rotation x y z w, position,
// whether it has collision) and map.txt (its name, label, date, the shift
// from the mod's coordinates, its sea level, the arrival, its area).
// ---------------------------------------------------------------------------
std::vector<MapInfo> g_maps;
float g_sea_level = 0.0f;

struct MapSet {
  MapInfo info;
  fs::path dir;
  std::vector<std::string> objs;  // its model lines
  std::vector<fs::path> files;    // its resources
  std::string prefix;             // its files' names start with it (map.txt "prefix")
};

std::string Trimmed(std::string line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
    line.pop_back();
  size_t start = 0;
  while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
    ++start;
  return line.substr(start);
}

std::vector<MapSet> FindMaps(const fs::path& maps_dir) {
  std::vector<MapSet> out;
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (auto it = fs::directory_iterator(maps_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
    if (it->is_directory(ec))
      dirs.push_back(it->path());
  std::sort(dirs.begin(), dirs.end());
  for (const auto& dir : dirs) {
    TextLines meta, place, objs;
    if (!LoadText(dir / "map.txt", meta) || !LoadText(dir / "place.txt", place) ||
        !LoadText(dir / "objs.ide", objs)) {
      REXLOG_WARN("gta4-addons: map set {} is incomplete; left out", dir.string());
      continue;
    }
    MapSet m;
    m.dir = dir;
    MapInfo& info = m.info;
    info.dir = dir;
    for (const auto& raw : meta.lines) {
      const std::string line = Trimmed(raw);
      const size_t space = line.find(' ');
      if (line.empty() || line[0] == '#' || space == std::string::npos)
        continue;
      const std::string key = line.substr(0, space), value = Trimmed(line.substr(space + 1));
      if (key == "name")
        info.name = Lower(value);
      else if (key == "label")
        info.label = value;
      else if (key == "date")
        std::sscanf(value.c_str(), "%d %d %d %d %d", &info.date[0], &info.date[1], &info.date[2], &info.date[3],
                    &info.date[4]);
      else if (key == "shift")
        std::sscanf(value.c_str(), "%f %f", &info.shift[0], &info.shift[1]);
      else if (key == "sea")
        std::sscanf(value.c_str(), "%f", &info.sea_src);
      else if (key == "arrive")
        std::sscanf(value.c_str(), "%f %f %f %f %f", &info.arrive[0], &info.arrive[1], &info.arrive[2],
                    &info.arrive[3], &info.arrive[4]);
      else if (key == "area")
        std::sscanf(value.c_str(), "%f %f %f %f", &info.area[0], &info.area[1], &info.area[2], &info.area[3]);
      else if (key == "prefix" && value.size() >= 3 && value.back() == '_')
        m.prefix = Lower(value);
      else if (key == "bounds" || key == "peds") {  // (names: several collision dictionaries; the ped models)
        std::string word;
        auto& into = key == "bounds" ? info.bounds : info.peds;
        for (const char c : Lower(value) + " ") {
          if (c == ' ' || c == '\t') {
            if (!word.empty())
              into.push_back(word);
            word.clear();
          } else {
            word += c;
          }
        }
      } else if (key == "wander")
        std::sscanf(value.c_str(), "%d", &info.wander);
      else if (key == "weather")
        std::sscanf(value.c_str(), "%d", &info.weather);
      else if (key == "ambience")
        info.ambience = Lower(value);
      else if (key == "stream")
        std::sscanf(value.c_str(), "%f", &info.stream);
    }
    // (the place's creatures: optional, creatures.txt)
    TextLines creatures;
    if (LoadText(dir / "creatures.txt", creatures)) {
      for (const auto& raw : creatures.lines) {
        const std::string line = Trimmed(raw);
        if (line.empty() || line[0] == '#')
          continue;
        std::vector<std::string> w;
        std::string word;
        for (const char c : line + " ") {
          if (c == ' ' || c == '\t') {
            if (!word.empty())
              w.push_back(word);
            word.clear();
          } else {
            word += c;
          }
        }
        if (w.size() >= 2 && w[0] == "creature") {
          CreatureKind k;
          k.name = Lower(w[1]);
          for (size_t i = 2; i + 1 < w.size(); i += 2) {
            if (w[i] == "kind")
              k.kind = Lower(w[i + 1]);
            else if (w[i] == "length")
              k.length = std::strtof(w[i + 1].c_str(), nullptr);
            else if (w[i] == "height")
              k.height = std::strtof(w[i + 1].c_str(), nullptr);
            else if (w[i] == "bottom")
              k.bottom = std::strtof(w[i + 1].c_str(), nullptr);
            else if (w[i] == "count")
              k.count = std::atoi(w[i + 1].c_str());
          }
          info.creatures.push_back(k);
        } else if (w.size() >= 3 && w[0] == "part" && !info.creatures.empty()) {
          CreaturePart part;
          part.name = Lower(w[1]);
          part.model = Lower(w[2]);
          for (size_t i = 3; i < w.size(); ++i) {
            if (w[i] == "pivot" && i + 3 < w.size()) {
              for (int a = 0; a < 3; ++a)
                part.pivot[a] = std::strtof(w[i + 1 + size_t(a)].c_str(), nullptr);
              i += 3;
            } else if (w[i] == "axis" && i + 3 < w.size()) {
              for (int a = 0; a < 3; ++a)
                part.axis[a] = std::strtof(w[i + 1 + size_t(a)].c_str(), nullptr);
              i += 3;
            } else if (w[i] == "swing" && i + 3 < w.size()) {
              part.amp = std::strtof(w[i + 1].c_str(), nullptr);
              part.hz = std::strtof(w[i + 2].c_str(), nullptr);
              part.phase = std::strtof(w[i + 3].c_str(), nullptr);
              i += 3;
            }
          }
          info.creatures.back().parts.push_back(part);
        }
      }
    }
    // (the vehicles the mod parks: optional)
    TextLines vehicles;
    if (LoadText(dir / "vehicles.txt", vehicles))
      for (const auto& raw : vehicles.lines) {
        const std::string line = Trimmed(raw);
        if (line.empty() || line[0] == '#')
          continue;
        char model[64] = {};
        MapVehicle v;
        if (std::sscanf(line.c_str(), "%63s %f %f %f %f", model, &v.pos[0], &v.pos[1], &v.pos[2], &v.heading) == 5) {
          v.model = Lower(model);
          info.vehicles.push_back(std::move(v));
        }
      }
    int stock_pieces = 0;
    for (const auto& raw : place.lines) {
      const std::string line = Trimmed(raw);
      if (line.empty() || line[0] == '#')
        continue;
      char model[64] = {};
      MapPiece p;
      int collision = 0;
      if (std::sscanf(line.c_str(), "%63s %f %f %f %f %f %f %f %d", model, &p.q[0], &p.q[1], &p.q[2], &p.q[3],
                      &p.pos[0], &p.pos[1], &p.pos[2], &collision) == 9) {
        p.model = Lower(model);
        p.collision = collision != 0;
        p.stock = collision == 2;  // (the game's own model: the mod placed one of the game's props)
        stock_pieces += p.stock;
        info.pieces.push_back(std::move(p));
      }
    }
    for (const auto& raw : objs.lines) {
      const std::string line = Trimmed(raw);
      if (!line.empty() && line[0] != '#')
        m.objs.push_back(line);
    }
    // Only the files its model lines name (<model>.xdr, <txd>.xtd) and its
    // collision dictionary: copying a release over the last leaves the last
    // one's files in the folder (v1.7.32's log: 192 files for 176; the
    // island's old untiled pieces went into vehicles.img again). The
    // dictionary is the one map.txt names ("bounds"; v1.7.34 on, <prefix>col,
    // so sets don't share one), else (an earlier release's set) the folder's
    // only .xbd.
    std::set<std::string> named;
    for (const auto& b : info.bounds)
      named.insert(b + ".xbd");
    for (const auto& line : m.objs) {
      const size_t c1 = line.find(','), c2 = c1 == std::string::npos ? c1 : line.find(',', c1 + 1);
      if (c2 == std::string::npos)
        continue;
      named.insert(Lower(Trimmed(line.substr(0, c1))) + ".xdr");
      named.insert(Lower(Trimmed(line.substr(c1 + 1, c2 - c1 - 1))) + ".xtd");
    }
    int unnamed = 0;
    std::vector<std::string> dictionaries;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      const std::string ext = Lower(it->path().extension().string());
      if (!it->is_regular_file(ec) || (ext != ".xdr" && ext != ".xtd" && ext != ".xbd"))
        continue;
      const bool any_dictionary = ext == ".xbd" && info.bounds.empty();
      if (!any_dictionary && !named.count(Lower(it->path().filename().string()))) {
        ++unnamed;
        continue;
      }
      if (any_dictionary)
        dictionaries.push_back(Lower(it->path().stem().string()));
      // The game registers an image's files by the first 24 characters of
      // the name (with the extension; sub_825127C0): a longer one is never
      // registered (v1.7.30: gk_gtr_deco_polgrnda018, "1 unknown to the game").
      if (it->path().filename().string().size() > 24) {
        REXLOG_WARN("gta4-addons: map set {}: {} has a name over 24 characters, which the game can't register; "
                    "left out",
                    dir.filename().string(), it->path().filename().string());
        continue;
      }
      m.files.push_back(it->path());
    }
    ec.clear();
    std::sort(m.files.begin(), m.files.end());
    if (info.bounds.empty() && dictionaries.size() == 1)
      info.bounds = dictionaries;
    else if (info.bounds.empty() && dictionaries.size() > 1)
      REXLOG_WARN("gta4-addons: map set {} has {} collision dictionaries and map.txt names none; its pieces "
                  "will have no collision",
                  dir.filename().string(), dictionaries.size());
    for (auto b = info.bounds.begin(); b != info.bounds.end();) {
      const bool have = std::any_of(m.files.begin(), m.files.end(), [&](const fs::path& f) {
        return Lower(f.filename().string()) == *b + ".xbd";
      });
      if (!have) {
        REXLOG_WARN("gta4-addons: map set {}: its collision dictionary {}.xbd is missing; the pieces in it will "
                    "have no collision",
                    dir.filename().string(), *b);
        b = info.bounds.erase(b);
      } else {
        ++b;
      }
    }
    if (unnamed)
      REXLOG_INFO("gta4-addons: map set {}: {} file(s) in its folder no model line names (an earlier release's); "
                  "left out",
                  dir.filename().string(), unnamed);
    if (info.name.empty() || info.pieces.empty() || m.files.empty() || m.objs.empty()) {
      REXLOG_WARN("gta4-addons: map set {} has no name, pieces, models or files; left out", dir.string());
      continue;
    }
    REXLOG_INFO("gta4-addons: map set {} (\"{}\"): {} pieces ({} of the game's own models), {} models, {} files, "
                "{} collision dictionar{}, {}; its life: {} vehicles, {} ped models ({} about), weather {}, ambience {}",
                info.name, info.label, info.pieces.size(), stock_pieces, m.objs.size(), m.files.size(),
                info.bounds.size(), info.bounds.size() == 1 ? "y" : "ies",
                info.stream > 0 ? fmt::format("streamed within {:.0f} m", info.stream) : std::string("all placed at once"),
                info.vehicles.size(), info.peds.size(), info.wander, info.weather,
                info.ambience.empty() ? "none" : info.ambience);
    for (const auto& k : info.creatures)
      REXLOG_INFO("gta4-addons: map set {}: creature {} ({}, {:.1f} m, {} about): {} parts", info.name, k.name, k.kind,
                  k.length, k.count, k.parts.size());
    out.push_back(std::move(m));
  }
  return out;
}

// gtxd.ide (the global texture dictionaries' parents, loaded by gta.dat
// before everything else) with the map sets' models in a section of their own.
std::vector<uint8_t> WithMapModels(const std::vector<uint8_t>& original, const std::vector<MapSet>& maps) {
  std::string text(original.begin(), original.end());
  const std::string nl = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
  if (!text.empty() && text.back() != '\n')
    text += nl;
  text += "# LibertyRecomp: the DeLorean's destinations (map sets)" + nl + "objs" + nl;
  for (const auto& m : maps)
    for (const auto& line : m.objs)
      text += line + nl;
  text += "end" + nl;
  return std::vector<uint8_t>(text.begin(), text.end());
}

// water.dat with water under each map set where the stock sea does not reach
// (a quad over its area, at the level of the stock water nearest it); each
// map set's sea level noted for the trainer. A quad line: four corners of
// seven numbers (x, y, z and four more), then two; the new one copies the
// first such line, corners moved.
std::vector<uint8_t> WithMapWater(const std::vector<uint8_t>& original, std::vector<MapSet>& maps) {
  struct Quad {
    float x0, y0, x1, y1, z;
  };
  std::string text(original.begin(), original.end());
  std::vector<std::string> lines;
  for (size_t pos = 0; pos < text.size();) {
    const size_t eol = text.find('\n', pos);
    lines.push_back(text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
    pos = eol == std::string::npos ? text.size() : eol + 1;
  }
  std::vector<Quad> quads;
  std::vector<float> template_values;
  for (const auto& raw : lines) {
    std::vector<float> v;
    const char* p = raw.c_str();
    char* end = nullptr;
    while (*p) {
      const float f = std::strtof(p, &end);
      if (end == p)
        break;
      v.push_back(f);
      p = end;
    }
    if (v.size() != 30)
      continue;
    Quad q{1e9f, 1e9f, -1e9f, -1e9f, v[2]};
    for (int c = 0; c < 4; ++c) {
      q.x0 = std::min(q.x0, v[size_t(7 * c)]);
      q.x1 = std::max(q.x1, v[size_t(7 * c)]);
      q.y0 = std::min(q.y0, v[size_t(7 * c + 1)]);
      q.y1 = std::max(q.y1, v[size_t(7 * c + 1)]);
    }
    quads.push_back(q);
    if (template_values.empty())
      template_values = v;
  }
  const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  bool added = false;
  // (the level most of the stock sea is at: the trainer's look-out for water)
  if (!quads.empty()) {
    std::map<int, int> votes;
    for (const auto& q : quads)
      ++votes[int(std::lround(q.z * 10.0f))];
    g_sea_level = float(std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) {
                          return a.second < b.second;
                        })->first) / 10.0f;
    REXLOG_INFO("gta4-addons: water.dat: {} quads, most at {:.1f}", quads.size(), g_sea_level);
  }
  for (auto& m : maps) {
    MapInfo& info = m.info;
    // (the check over the place itself; new water a good way past it)
    constexpr float kMargin = 250.0f, kCheck = 30.0f;
    const float x0 = info.area[0] + info.shift[0] - kMargin, y0 = info.area[1] + info.shift[1] - kMargin;
    const float x1 = info.area[2] + info.shift[0] + kMargin, y1 = info.area[3] + info.shift[1] + kMargin;
    const float cx0 = x0 + kMargin - kCheck, cy0 = y0 + kMargin - kCheck;
    const float cx1 = x1 - kMargin + kCheck, cy1 = y1 - kMargin + kCheck;
    int covered = 0, samples = 0;
    std::vector<float> levels;
    for (int i = 0; i <= 14; ++i)
      for (int j = 0; j <= 14; ++j) {
        const float x = cx0 + (cx1 - cx0) * float(i) / 14.0f, y = cy0 + (cy1 - cy0) * float(j) / 14.0f;
        ++samples;
        for (const auto& q : quads)
          if (x >= q.x0 && x <= q.x1 && y >= q.y0 && y <= q.y1) {
            ++covered;
            levels.push_back(q.z);
            break;
          }
      }
    float sea = 0.0f;
    if (!levels.empty()) {
      std::sort(levels.begin(), levels.end());
      sea = levels[levels.size() / 2];
    } else if (!quads.empty()) {
      // (none near: the level most of the stock sea is at)
      std::map<int, int> votes;
      for (const auto& q : quads)
        ++votes[int(std::lround(q.z * 10.0f))];
      sea = float(std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) {
                    return a.second < b.second;
                  })->first) / 10.0f;
    }
    info.sea_dst = sea;
    if (covered >= samples * 98 / 100) {
      REXLOG_INFO("gta4-addons: map set {}: the stock sea covers it (level {:.2f})", info.name, sea);
      continue;
    }
    if (template_values.empty()) {
      REXLOG_WARN("gta4-addons: map set {}: water.dat has no quad line to copy; no water added", info.name);
      continue;
    }
    // The template's corners, each moved to the new area's matching corner.
    float tx0 = 1e9f, tx1 = -1e9f, ty0 = 1e9f, ty1 = -1e9f;
    for (int c = 0; c < 4; ++c) {
      tx0 = std::min(tx0, template_values[size_t(7 * c)]);
      tx1 = std::max(tx1, template_values[size_t(7 * c)]);
      ty0 = std::min(ty0, template_values[size_t(7 * c + 1)]);
      ty1 = std::max(ty1, template_values[size_t(7 * c + 1)]);
    }
    std::vector<float> v = template_values;
    for (int c = 0; c < 4; ++c) {
      v[size_t(7 * c)] = v[size_t(7 * c)] >= (tx0 + tx1) / 2 ? x1 : x0;
      v[size_t(7 * c + 1)] = v[size_t(7 * c + 1)] >= (ty0 + ty1) / 2 ? y1 : y0;
      v[size_t(7 * c + 2)] = sea;
    }
    std::string line;
    for (size_t k = 0; k < v.size(); ++k) {
      char num[32];
      std::snprintf(num, sizeof(num), k + 2 >= v.size() && k + 2 == v.size() ? "%.0f" : "%.3f", v[k]);
      line += (k ? (k % 7 == 0 && k < 28 ? "   " : " ") : "") + std::string(num);
    }
    if (!text.empty() && text.back() != '\n')
      text += nl;
    text += line + nl;
    added = true;
    REXLOG_INFO("gta4-addons: map set {}: water added over ({:.0f}, {:.0f})-({:.0f}, {:.0f}) at {:.2f} ({} of {} "
                "points had stock water)",
                info.name, x0, y0, x1, y1, sea, covered, samples);
  }
  if (!added)
    return original;
  return std::vector<uint8_t>(text.begin(), text.end());
}

// ---------------------------------------------------------------------------
// The game reads vehicles.ide, handling.dat, carcols.dat and vehOff.csv from
// common.rpf (default.dat names them on the common: device, which is the
// archive), not from the copy the port's installer unpacked beside it:
// console log 039's vehicle store held exactly the archive's 127 cars, and
// the add-ons came out as plain props. So their lines go into a shadow
// common.rpf, the way the port gives the game its own button prompts
// (rpf_button_prompts.cpp): a copy of the archive with the edited files
// appended and its table of contents pointed at them, with game:\common.rpf
// redirected to the copy by the app (gta4_ps5_app.cpp). The installed archive
// itself is never changed.
// ---------------------------------------------------------------------------
uint64_t Fnv1a(uint64_t h, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

std::string Hex64(uint64_t v) {
  static const char* kDigits = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i, v >>= 4)
    out[size_t(i)] = kDigits[v & 15];
  return out;
}

using EditedFile = std::pair<std::string, std::vector<uint8_t>>;  // path in the archive, contents

bool WritePadding(std::fstream& out, uint64_t from, uint64_t to) {
  if (to <= from)
    return true;
  std::vector<char> zeros(size_t(to - from), 0);
  out.seekp(std::streamoff(from));
  return bool(out.write(zeros.data(), std::streamsize(zeros.size())));
}

// Raw deflate, the way the archive stores its compressed files.
bool Deflate(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
  z_stream z{};
  if (deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    return false;
  out.resize(deflateBound(&z, uLong(in.size())));
  z.next_in = const_cast<Bytef*>(in.data());
  z.avail_in = uInt(in.size());
  z.next_out = out.data();
  z.avail_out = uInt(out.size());
  const int status = deflate(&z, Z_FINISH);
  out.resize(out.size() - z.avail_out);
  deflateEnd(&z);
  return status == Z_STREAM_END;
}

bool WriteShadowRpf(const fs::path& source, const Rpf& rpf, const std::vector<uint8_t>& key,
                    const std::vector<EditedFile>& files, const fs::path& destination) {
  if (!CopyBytes(source, destination))
    return false;
  std::fstream out(destination, std::ios::binary | std::ios::in | std::ios::out);
  if (!out)
    return false;
  std::vector<uint8_t> toc = rpf.toc;
  uint64_t end = rpf.file_size;
  for (const auto& [inner, data] : files) {
    const RpfFile* f = rpf.Find(inner);
    if (!f || f->resource)
      return false;
    // Stored the way the original was: compressed if it was.
    std::vector<uint8_t> packed;
    if (f->compressed && !Deflate(data, packed))
      return false;
    const std::vector<uint8_t>& stored = f->compressed ? packed : data;
    const uint64_t at = (end + kRpfBlock - 1) / kRpfBlock * kRpfBlock;
    if (at + stored.size() >= 0x80000000ull || !WritePadding(out, end, at))
      return false;
    out.seekp(std::streamoff(at));
    if (!out.write(reinterpret_cast<const char*>(stored.data()), std::streamsize(stored.size())))
      return false;
    end = at + stored.size();
    // Size, offset, size in the archive (with the compressed flag).
    PutLe32(toc.data() + f->toc_offset + 4, uint32_t(data.size()));
    PutLe32(toc.data() + f->toc_offset + 8, uint32_t(at));
    PutLe32(toc.data() + f->toc_offset + 12,
            uint32_t(stored.size()) | (f->compressed ? 0x40000000u : 0u));
  }
  if (!WritePadding(out, end, (end + kRpfBlock - 1) / kRpfBlock * kRpfBlock))
    return false;
  if (rpf.encrypted)
    CryptBlocks(toc.data(), toc.size(), key, true);
  out.seekp(std::streamoff(kRpfTocOffset));
  out.write(reinterpret_cast<const char*>(toc.data()), std::streamsize(toc.size()));
  out.flush();
  return bool(out);
}

bool ShadowHolds(const fs::path& shadow, const std::vector<uint8_t>& key,
                 const std::vector<EditedFile>& files) {
  const auto rpf = ReadRpf(shadow, key);
  if (!rpf)
    return false;
  for (const auto& [inner, data] : files) {
    const RpfFile* f = rpf->Find(inner);
    std::vector<uint8_t> now;
    if (!f || !ReadRpfData(shadow, *f, now) || now != data)
      return false;
  }
  return true;
}

std::optional<fs::path> BuildShadowArchive(const fs::path& source, const Rpf& archive,
                                           const std::vector<uint8_t>& key,
                                           const std::vector<Addon>& addons, std::vector<MapSet>& maps,
                                           const fs::path& cache, const std::string& label) {
  std::error_code ec;
  const Rpf* rpf = &archive;
  const fs::path work = cache / "work" / label;
  fs::create_directories(work, ec);
  // The archive's own copies, edited exactly as the loose ones are.
  std::vector<EditedFile> edited;
  uint64_t fingerprint = Fnv1a(1469598103934665603ull, rpf->toc.data(), rpf->toc.size());
  for (const std::string inner :
       {"/data/vehicles.ide", "/data/handling.dat", "/data/carcols.dat", "/data/vehoff.csv"}) {
    const RpfFile* f = rpf->Find(inner);
    std::vector<uint8_t> original, now;
    if (!f)
      continue;
    if (!ReadRpfData(source, *f, original)) {
      REXLOG_WARN("gta4-addons: {} has an unreadable {}", source.string(), inner);
      continue;
    }
    const fs::path file = work / fs::path(inner).filename();
    fs::remove(file.string() + ".stock-bak", ec);
    std::ofstream(file, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(original.data()), std::streamsize(original.size()));
    EditDataFile(file, addons);
    if (!ReadAll(file, now))
      continue;
    fingerprint = Fnv1a(fingerprint, inner.data(), inner.size());
    fingerprint = Fnv1a(fingerprint, now.data(), now.size());
    if (now != original)
      edited.emplace_back(inner, std::move(now));
  }
  // The map sets' models (gtxd.ide) and the sea under them (water.dat); and
  // gta.dat made to load gtxd.ide should it not (every copy seen does).
  if (!maps.empty()) {
    if (const RpfFile* f = rpf->Find("/data/gta.dat")) {
      std::vector<uint8_t> original;
      if (ReadRpfData(source, *f, original)) {
        const std::string text = Lower(std::string(original.begin(), original.end()));
        if (text.find("gtxd.ide") == std::string::npos) {
          std::string now(original.begin(), original.end());
          const std::string nl = now.find("\r\n") != std::string::npos ? "\r\n" : "\n";
          if (!now.empty() && now.back() != '\n')
            now += nl;
          now += "IDE common:/DATA/gtxd.IDE" + nl;
          REXLOG_WARN("gta4-addons: {}'s gta.dat does not load gtxd.ide; added", source.string());
          std::vector<uint8_t> bytes(now.begin(), now.end());
          fingerprint = Fnv1a(fingerprint, bytes.data(), bytes.size());
          edited.emplace_back("/data/gta.dat", std::move(bytes));
        } else {
          REXLOG_INFO("gta4-addons: gta.dat loads gtxd.ide (the map sets' models go there)");
        }
      }
    }
    for (const std::string inner : {"/data/gtxd.ide", "/data/water.dat"}) {
      const RpfFile* f = rpf->Find(inner);
      std::vector<uint8_t> original;
      if (!f || !ReadRpfData(source, *f, original)) {
        REXLOG_WARN("gta4-addons: {} has no readable {}; the map sets {}", source.string(), inner,
                    inner == "/data/gtxd.ide" ? "stay out" : "get no water of their own");
        continue;
      }
      std::vector<uint8_t> now = inner == "/data/gtxd.ide" ? WithMapModels(original, maps)
                                                             : WithMapWater(original, maps);
      fingerprint = Fnv1a(fingerprint, inner.data(), inner.size());
      fingerprint = Fnv1a(fingerprint, now.data(), now.size());
      if (now != original)
        edited.emplace_back(inner, std::move(now));
    }
  }
  if (edited.empty()) {
    REXLOG_INFO("gta4-addons: {} needs no change", source.string());
    return std::nullopt;
  }
  const fs::path destination = cache / (label + "-" + Hex64(fingerprint) + ".rpf");
  if (fs::exists(destination, ec) && ShadowHolds(destination, key, edited)) {
    REXLOG_INFO("gta4-addons: shadow of {} up to date: {}", source.string(), destination.string());
    return destination;
  }
  // Only the current shadow is kept.
  std::vector<fs::path> stale;
  for (auto it = fs::directory_iterator(cache, ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
    if (it->path().filename().string().rfind(label + "-", 0) == 0)
      stale.push_back(it->path());
  for (const auto& old : stale)
    fs::remove(old, ec);
  const fs::path temporary = destination.string() + ".tmp";
  if (!WriteShadowRpf(source, *rpf, key, edited, temporary) ||
      !ShadowHolds(temporary, key, edited)) {
    fs::remove(temporary, ec);
    REXLOG_WARN("gta4-addons: the shadow of {} could not be built in {}; the add-ons stay out "
                "of the game's vehicle list", source.string(), cache.string());
    return std::nullopt;
  }
  ec.clear();
  fs::rename(temporary, destination, ec);
  if (ec) {
    REXLOG_WARN("gta4-addons: could not publish {}: {}", destination.string(), ec.message());
    fs::remove(temporary, ec);
    return std::nullopt;
  }
  REXLOG_INFO("gta4-addons: built a shadow of {} ({} file(s) with the add-ons' lines): {}",
              source.string(), edited.size(), destination.string());
  return destination;
}

// Every archive the game might read the data files from: common.rpf in the
// game folder (game:\common.rpf), and one in the title update's folder
// (update:\...) should it carry its own copy. Each that holds any of the
// data files gets a shadow.
std::vector<ShadowArchive> BuildShadows(const fs::path& game, const std::vector<uint8_t>& key,
                                        const std::vector<Addon>& addons, std::vector<MapSet>& maps,
                                        const fs::path& cache) {
  std::vector<ShadowArchive> out;
  std::error_code ec;
  fs::create_directories(cache, ec);
  struct Place {
    fs::path folder;
    std::string guest, label;
  };
  for (const Place& place : {Place{game, "game:\\", "game"}, Place{game / "update", "update:\\", "update"}}) {
    std::vector<fs::path> archives;
    for (auto it = fs::directory_iterator(place.folder, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
      std::error_code fec;
      if (it->is_regular_file(fec) && Lower(it->path().extension().string()) == ".rpf")
        archives.push_back(it->path());
    }
    ec.clear();
    std::sort(archives.begin(), archives.end());
    for (const auto& source : archives) {
      const auto rpf = ReadRpf(source, key);
      if (!rpf || (!rpf->Find("/data/vehicles.ide") && !rpf->Find("/data/handling.dat")))
        continue;
      const std::string name = source.filename().string();
      const std::string label = place.label + "-" + Lower(source.stem().string());
      // (the map sets' lines only in the base game's archive, the one gta.dat's gtxd.ide is in)
      std::vector<MapSet> none;
      std::vector<MapSet>& these = place.label == "game" ? maps : none;
      if (const auto shadow = BuildShadowArchive(source, *rpf, key, addons, these, cache, label))
        out.push_back({place.guest + name, *shadow});
    }
  }
  if (out.empty())
    REXLOG_INFO("gta4-addons: no archive with the data files to shadow; the game reads them loose");
  return out;
}

std::vector<ShadowArchive> InstallBundled(const fs::path& app_addons,
                                          const fs::path& user_data_root) {
  std::error_code ec;
  if (!fs::exists(app_addons, ec)) return {};  // nothing bundled
  const std::vector<Addon> addons = FindAddons(app_addons);
  if (addons.empty()) return {};
  std::vector<MapSet> maps = FindMaps(app_addons / "maps");
  const fs::path game = FindGameRoot(user_data_root);
  if (game.empty()) {
    REXLOG_INFO("gta4-addons: no installed game yet; add-ons wait for the next boot");
    return {};
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
  bool ok = true;
  // Every copy of the data files the game might read - base, title update,
  // episodes - found by name in any letter case (the update's files keep the
  // case of the package they came from; this file system minds case).
  std::vector<fs::path> data_files;
  {
    // Where data files live (the models and maps folders, thousands of
    // files, are left out of the walk).
    std::vector<fs::path> roots;
    for (const fs::path& root : {game / "common", game / "update", game.parent_path() / "dlc"})
      if (fs::exists(root, ec))
        roots.push_back(root);
    for (const auto& root : roots) {
      std::error_code wec;
      for (auto it = fs::recursive_directory_iterator(
               root, fs::directory_options::skip_permission_denied, wec);
           !wec && it != fs::recursive_directory_iterator(); it.increment(wec)) {
        if (it.depth() > 8) { it.disable_recursion_pending(); continue; }
        std::error_code fec;
        if (!it->is_regular_file(fec))
          continue;
        const std::string name = Lower(it->path().filename().string());
        if (name == "vehicles.ide" || name == "handling.dat" || name == "carcols.dat" ||
            name == "vehoff.csv")
          data_files.push_back(it->path());
      }
    }
    std::sort(data_files.begin(), data_files.end());
    for (const auto& f : data_files)
      REXLOG_INFO("gta4-addons: data file {}", f.string());
  }
  for (const auto& file : data_files) {
    // Only the base game's own copies must take every edit; another copy (an
    // update's or an episode's, perhaps partial) takes what applies to it.
    std::error_code eq_ec;
    const bool base_copy = fs::equivalent(file.parent_path(), game / "common" / "data", eq_ec);
    const bool fine = EditDataFile(file, addons);
    if (base_copy)
      ok &= fine;
  }

  const fs::path img = FindVehiclesImg(game);
  if (img.empty()) {
    REXLOG_WARN("gta4-addons: vehicles.img was not found under {}", game.string());
    return {};
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
  // The data files the game actually reads: in common.rpf. (Not this launch
  // if the last two that used them never reached the city.)
  const fs::path cache = game.parent_path() / "addon_cache";
  std::vector<ShadowArchive> shadows = BuildShadows(game, key, addons, maps, cache);
  if (!shadows.empty()) {
    // The marker counts the launches in a row that mounted the shadows and
    // never got to the city. One alone proves nothing (the game closed at
    // the loading screen, or from the PS5 menu, says the same), so it takes
    // two before a launch starts without them. (An older marker, with no
    // count, is one.)
    const fs::path marker = cache / "launch-pending.txt";
    g_launch_marker = marker;
    int missed = 0;
    if (fs::exists(marker, ec)) {
      std::vector<uint8_t> raw;
      ReadAll(marker, raw);
      const std::string text(raw.begin(), raw.end());
      const size_t colon = text.rfind(':');
      missed = 1;
      if (colon != std::string::npos) {
        const int n = std::atoi(text.c_str() + colon + 1);
        if (n > 0) missed = n;
      }
    }
    if (missed >= 2) {
      fs::remove(marker, ec);
      g_shadows_skipped = true;
      shadows.clear();
      REXLOG_WARN("gta4-addons: the last {} launches with the add-ons' data never reached the "
                  "city; this launch starts without it (the next one tries again)", missed);
    } else {
      if (missed)
        REXLOG_WARN("gta4-addons: the last launch with the add-ons' data never reached the city "
                    "(closed early, or stopped); trying them again");
      std::ofstream(marker, std::ios::trunc)
          << "launches with the add-ons' shadow archive that have not reached the city: "
          << (missed + 1) << "\n";
    }
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
    return shadows;
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
    ExportGameIndex(game, key, app_addons.parent_path() / "logs" / "export" / "index");
    ExportPlayerPed(game, app_addons.parent_path() / "logs" / "export" / "playerped");
    ExportMapReference(game, key, app_addons.parent_path() / "logs" / "export" / "maps");
    ExportMapSets(game, key, app_addons.parent_path() / "logs" / "export" / "mapsets");
  }
  std::vector<fs::path> files;
  for (const auto& a : addons) {
    files.push_back(a.dir / (a.model + ".xft"));
    files.push_back(a.dir / (a.model + ".xtd"));
  }
  // The map sets' resources too, while their models are in this launch's
  // gtxd.ide (without the shadow they would be files nothing names).
  const bool maps_named = std::any_of(shadows.begin(), shadows.end(), [](const ShadowArchive& sh) {
    return sh.guest_path.rfind("game:\\", 0) == 0;
  });
  std::vector<std::string> owned;
  if (maps_named)
    for (const auto& m : maps) {
      files.insert(files.end(), m.files.begin(), m.files.end());
      if (!m.prefix.empty())
        owned.push_back(m.prefix);
    }
  // (v1.7.33's island collision was gtxd.xbd, named after the .ide; from
  // v1.7.34 each set's is its own, <prefix>col.xbd, and the old one goes)
  std::vector<std::string> unused = {"batmobile.wft", "batmobile.wtd", "fbi.wft", "fbi.wtd"};
  if (std::none_of(files.begin(), files.end(), [](const fs::path& f) { return Lower(f.filename().string()) == "gtxd.xbd"; }))
    unused.push_back("gtxd.xbd");
  bool any = false;
  for (const auto& image : images) {
    REXLOG_INFO("gta4-addons: vehicles.img at {} ({} bytes)", image.string(),
                uint64_t(fs::file_size(image, ec)));
    any |= InjectIntoImg(image, key, files, /*append_missing=*/true, unused, owned);
  }
  if (any && ok) {
    std::string names;
    for (const auto& a : addons) names += (names.empty() ? "" : ", ") + a.model;
    REXLOG_INFO("gta4-addons: installed as their own vehicles: {}", names);
  }
  g_maps.clear();
  for (auto& m : maps) {
    m.info.installed = any && maps_named;
    REXLOG_INFO("gta4-addons: map set {} {} (sea level there {:.2f})", m.info.name,
                m.info.installed ? "installed" : "not installed this launch", m.info.sea_dst);
    g_maps.push_back(m.info);
  }
  return shadows;
}

const std::vector<MapInfo>& InstalledMaps() { return g_maps; }
float SeaLevel() { return g_sea_level; }

void SetShadowsActive(bool active) { g_shadows_active.store(active, std::memory_order_relaxed); }
bool ShadowsActive() { return g_shadows_active.load(std::memory_order_relaxed); }

void MarkLaunchHealthy() {
  static bool s_done = false;
  if (s_done || g_launch_marker.empty())
    return;
  s_done = true;
  std::error_code ec;
  if (fs::remove(g_launch_marker, ec))
    REXLOG_INFO("gta4-addons: reached the city with the add-ons' data; launch marker cleared");
}

bool ShadowsSkippedThisLaunch() { return g_shadows_skipped; }
}  // namespace gta4::addons
