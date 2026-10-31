// Host tool for GTA IV Xbox 360 resources: XMem LZX decode (the port's
// patched libmspack, the same persistent per-frame decoding the port uses
// at runtime) and an encoder that emits LZX *uncompressed* blocks, one per
// 32 KiB frame, in the same framing. Decoding the encoder's output with the
// decoder is the round-trip proof.
//
//   xlzx selftest
//   xlzx unrsc <in.xft|xtd> <out.bin>          360 resource -> raw body (virt+phys)
//   xlzx rsc <type> <flags> <in.bin> <out>      raw body -> 360 resource
//
// 360 resource: u32be 0x05435352 'RSC\x05'-as-BE, u32be type, u32be flags,
// then u32be 0x0FF512EF, u32be compressed length, then XMem frames:
//   u16be compressed_size                        (frame is 0x8000 bytes out)
//   0xFF, u16be uncompressed_size, u16be compressed_size
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "lzx.h"
#include "mspack.h"
}

namespace {
constexpr size_t kFrame = 0x8000;
constexpr int kWindowBits = 17;  // 0x20000, the retail window
constexpr uint32_t kRscMagic = 0x05435352u;
constexpr uint32_t kXmemMagic = 0x0FF512EFu;

struct MemFile {
  const uint8_t* in = nullptr;
  uint8_t* out = nullptr;
  size_t size = 0, off = 0, read = 0;
  bool eof = false, writable = false;
};
int Read(mspack_file* f, void* d, int n) {
  auto* m = reinterpret_cast<MemFile*>(f);
  if (m->writable || n < 0) return -1;
  size_t c = std::min<size_t>(size_t(n), m->size - m->off);
  if (c) { std::memcpy(d, m->in + m->off, c); m->off += c; m->read += c; }
  else m->eof = true;
  return int(c);
}
int Write(mspack_file* f, void* s, int n) {
  auto* m = reinterpret_cast<MemFile*>(f);
  if (!m->writable || n < 0) return -1;
  size_t c = std::min<size_t>(size_t(n), m->size - m->off);
  if (c) { std::memcpy(m->out + m->off, s, c); m->off += c; }
  return int(c);
}
void* Alloc(mspack_system*, size_t n) { return std::calloc(n, 1); }
void Free(void* p) { std::free(p); }
void Copy(void* s, void* d, size_t n) { std::memcpy(d, s, n); }

struct Decoder {
  mspack_system sys{};
  MemFile in, out;
  lzxd_stream* s = nullptr;
  uint64_t total = 0;
  Decoder() {
    sys.read = Read; sys.write = Write; sys.alloc = Alloc; sys.free = Free; sys.copy = Copy;
    out.writable = true;
    s = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&in), reinterpret_cast<mspack_file*>(&out),
                  kWindowBits, 0, 4096, 0, 0);
    if (s) s->uncompressed_block_padding = 0;  // XMem: no pad after odd raw block
  }
  ~Decoder() { if (s) lzxd_free(s); }
  bool Frame(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len) {
    in = MemFile{}; in.in = src; in.size = src_len;
    out = MemFile{}; out.out = dst; out.size = dst_len; out.writable = true;
    s->input = reinterpret_cast<mspack_file*>(&in);
    s->output = reinterpret_cast<mspack_file*>(&out);
    s->i_ptr = s->inbuf; s->i_end = s->inbuf; s->bit_buffer = 0; s->bits_left = 0;
    s->input_end = 0;
    const uint64_t next = total + dst_len;
    lzxd_set_output_length(s, off_t(next));
    int r;
    if ((next % kFrame) == 0 && dst_len > 1) {
      r = lzxd_decompress(s, off_t(dst_len - 1));
      if (r == MSPACK_ERR_OK) r = lzxd_decompress(s, 1);
    } else {
      r = lzxd_decompress(s, off_t(dst_len));
    }
    if (r != MSPACK_ERR_OK || out.off != dst_len) return false;
    total = next;
    return true;
  }
};

std::vector<uint8_t> Load(const char* p) {
  std::vector<uint8_t> v;
  FILE* f = std::fopen(p, "rb");
  if (!f) return v;
  std::fseek(f, 0, SEEK_END);
  v.resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  if (!v.empty() && std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
  std::fclose(f);
  return v;
}
bool Save(const char* p, const std::vector<uint8_t>& v) {
  FILE* f = std::fopen(p, "wb");
  if (!f) return false;
  bool ok = std::fwrite(v.data(), 1, v.size(), f) == v.size();
  std::fclose(f);
  return ok;
}
uint32_t BE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void PutBE32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}
size_t BodySize(uint32_t flags) {
  const size_t virt = size_t(flags & 0x7FF) << (((flags >> 11) & 0xF) + 8);
  const size_t phys = size_t((flags >> 15) & 0x7FF) << (((flags >> 26) & 0xF) + 8);
  return virt + phys;
}

bool DecodeStream(const uint8_t* src, size_t src_size, size_t expected, std::vector<uint8_t>& out,
                  bool verbose) {
  Decoder d;
  if (!d.s) return false;
  out.assign(expected, 0);
  size_t pos = 0, done = 0, frames = 0;
  while (done < expected) {
    if (pos + 2 > src_size) {
      if (verbose) std::fprintf(stderr, "eof at frame %zu (done %zu of %zu)\n", frames, done, expected);
      return false;
    }
    size_t usize = kFrame, csize;
    if (src[pos] == 0xFF) {
      if (pos + 5 > src_size) return false;
      usize = size_t(src[pos + 1]) << 8 | src[pos + 2];
      csize = size_t(src[pos + 3]) << 8 | src[pos + 4];
      pos += 5;
    } else {
      csize = size_t(src[pos]) << 8 | src[pos + 1];
      pos += 2;
    }
    usize = std::min(usize, expected - done);
    if (csize == 0 || pos + csize > src_size) {
      if (verbose) std::fprintf(stderr, "bad frame %zu: csize=%zu at %zu\n", frames, csize, pos);
      return false;
    }
    std::vector<uint8_t> chunk(src + pos, src + pos + csize);
    chunk.resize(csize + 4, 0);  // the retail wrapper's 4 read-ahead bytes
    if (!d.Frame(chunk.data(), chunk.size(), out.data() + done, usize)) {
      if (verbose) std::fprintf(stderr, "decode failed at frame %zu (usize=%zu csize=%zu)\n",
                                frames, usize, csize);
      return false;
    }
    pos += csize;
    done += usize;
    ++frames;
  }
  if (verbose) std::fprintf(stderr, "decoded %zu frames, %zu bytes, %zu of %zu input bytes used\n",
                            frames, done, pos, src_size);
  return true;
}

// LZX bit writer: 16-bit little-endian words, bits filled MSB first.
struct Bits {
  std::vector<uint8_t> bytes;
  uint32_t acc = 0;
  int n = 0;
  void Put(uint32_t value, int count) {
    while (count > 0) {
      const int take = std::min(count, 16 - n);
      const uint32_t chunk = (value >> (count - take)) & ((1u << take) - 1);
      acc = (acc << take) | chunk;
      n += take;
      count -= take;
      if (n == 16) {
        bytes.push_back(uint8_t(acc & 0xFF));
        bytes.push_back(uint8_t(acc >> 8));
        acc = 0;
        n = 0;
      }
    }
  }
  // The decoder consumes 1-16 bits to align before a raw block's R0..R2.
  void AlignForRaw() { Put(0, n == 0 ? 16 : 16 - n); }
};

std::vector<uint8_t> EncodeStream(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  size_t done = 0;
  bool first = true;
  while (done < data.size()) {
    const size_t usize = std::min(kFrame, data.size() - done);
    Bits b;
    if (first) { b.Put(0, 1); first = false; }  // no Intel E8 translation
    b.Put(3, 3);                                 // LZX_BLOCKTYPE_UNCOMPRESSED
    b.Put(uint32_t(usize) >> 8, 16);
    b.Put(uint32_t(usize) & 0xFF, 8);
    b.AlignForRaw();
    std::vector<uint8_t> frame = std::move(b.bytes);
    const uint8_t r[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};  // R0=R1=R2=1
    frame.insert(frame.end(), r, r + 12);
    frame.insert(frame.end(), data.begin() + long(done), data.begin() + long(done + usize));
    const size_t csize = frame.size();
    if (usize == kFrame) {
      out.push_back(uint8_t(csize >> 8)); out.push_back(uint8_t(csize));
    } else {
      out.push_back(0xFF);
      out.push_back(uint8_t(usize >> 8)); out.push_back(uint8_t(usize));
      out.push_back(uint8_t(csize >> 8)); out.push_back(uint8_t(csize));
    }
    out.insert(out.end(), frame.begin(), frame.end());
    done += usize;
  }
  return out;
}
}  // namespace

int main(int argc, char** argv) {
  const std::string cmd = argc >= 2 ? argv[1] : "";
  if (cmd == "selftest") {
    std::mt19937 rng(7);
    for (size_t size : {size_t(1), size_t(31), size_t(0x8000), size_t(0x8001), size_t(0x18000),
                        size_t(0x18003), size_t(1626112)}) {
      std::vector<uint8_t> data(size);
      for (auto& b : data) b = uint8_t(rng());
      const auto enc = EncodeStream(data);
      std::vector<uint8_t> dec;
      if (!DecodeStream(enc.data(), enc.size(), data.size(), dec, false) || dec != data) {
        std::printf("FAIL size=%zu\n", size);
        return 1;
      }
    }
    std::printf("SELFTEST PASS\n");
    return 0;
  }
  if (cmd == "unrsc" && argc == 4) {
    const auto src = Load(argv[2]);
    if (src.size() < 20 || BE32(src.data()) != kRscMagic || BE32(src.data() + 12) != kXmemMagic) {
      std::fprintf(stderr, "not a 360 RSC/XMem resource\n");
      return 1;
    }
    const uint32_t type = BE32(src.data() + 4), flags = BE32(src.data() + 8);
    const uint32_t clen = BE32(src.data() + 16);
    if (20 + size_t(clen) > src.size()) { std::fprintf(stderr, "truncated\n"); return 1; }
    std::vector<uint8_t> out;
    if (!DecodeStream(src.data() + 20, clen, BodySize(flags), out, true)) return 1;
    std::fprintf(stderr, "type=%u flags=%08X body=%zu\n", type, flags, out.size());
    return Save(argv[3], out) ? 0 : 1;
  }
  if (cmd == "rsc" && argc == 6) {
    const uint32_t type = uint32_t(std::strtoul(argv[2], nullptr, 0));
    const uint32_t flags = uint32_t(std::strtoul(argv[3], nullptr, 0));
    const auto body = Load(argv[4]);
    if (body.size() != BodySize(flags)) {
      std::fprintf(stderr, "body %zu != flags size %zu\n", body.size(), BodySize(flags));
      return 1;
    }
    const auto stream = EncodeStream(body);
    std::vector<uint8_t> out;
    PutBE32(out, kRscMagic); PutBE32(out, type); PutBE32(out, flags);
    PutBE32(out, kXmemMagic); PutBE32(out, uint32_t(stream.size()));
    out.insert(out.end(), stream.begin(), stream.end());
    // Prove it before writing.
    std::vector<uint8_t> check;
    if (!DecodeStream(out.data() + 20, stream.size(), body.size(), check, false) || check != body) {
      std::fprintf(stderr, "round-trip check failed\n");
      return 1;
    }
    return Save(argv[5], out) ? 0 : 1;
  }
  std::fprintf(stderr, "usage: xlzx selftest | unrsc in out | rsc type flags body out\n");
  return 2;
}
