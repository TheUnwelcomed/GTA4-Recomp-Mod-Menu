// Offline preview of the menu's glass panel (src/gta4_trainer.cpp's
// DrawGlassMenu, copied out by extract_glass.py): ImGui's draw lists
// rasterized in software over a game frame, with the hub's fonts, so the
// layout can be looked at without a console. See extract_glass.py.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include "imgui.h"
#define REX_PLATFORM_PS5 1
#define __PROSPERO__ 1
namespace gta4ps5::hub {
struct Assets { ImFont *display = nullptr, *nav = nullptr, *body = nullptr, *body_bold = nullptr; };
Assets g_assets;
const Assets& Fonts() { return g_assets; }
}
struct DrawLine { std::string text; bool selected = false, dim = false; };
constexpr const char* kMenuCredit = "by @TheUnwelcomed";
struct DrawModel {
  bool open = false;
  std::string title, footer, toast;
  std::vector<DrawLine> lines;
  int tab = 0, page_serial = 0, slide = 0;
  std::vector<std::string> tabs;
  int first = 0, total = 0;
};
#include "glass_snippet.inc"

static std::vector<float> fb;  // RGB, 0..1
static int W = 1920, H = 1080;
static const unsigned char* tex = nullptr;
static int tw = 0, th = 0;

static void Raster(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const ImVec4& clip) {
  const float minx = std::max(clip.x, std::floor(std::min({a.pos.x, b.pos.x, c.pos.x})));
  const float maxx = std::min(clip.z, std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x})));
  const float miny = std::max(clip.y, std::floor(std::min({a.pos.y, b.pos.y, c.pos.y})));
  const float maxy = std::min(clip.w, std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y})));
  const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
  if (std::fabs(area) < 1e-9f) return;
  for (int y = int(miny); y < int(maxy); ++y)
    for (int x = int(minx); x < int(maxx); ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      float w0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
      float w1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
      float w2 = 1 - w0 - w1;
      if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
      auto ch = [&](ImU32 col, int sh) { return float((col >> sh) & 0xFF) / 255.0f; };
      float col[4];
      for (int k = 0; k < 4; ++k) col[k] = w0 * ch(a.col, 8 * k) + w1 * ch(b.col, 8 * k) + w2 * ch(c.col, 8 * k);
      const float u = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x, v = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y;
      const int tx = std::clamp(int(u * tw), 0, tw - 1), ty = std::clamp(int(v * th), 0, th - 1);
      const unsigned char* t = tex + 4 * (ty * tw + tx);
      const float alpha = col[3] * t[3] / 255.0f;
      float* d = &fb[3 * (y * W + x)];
      for (int k = 0; k < 3; ++k) d[k] = d[k] * (1 - alpha) + col[k] * (t[k] / 255.0f) * alpha;
    }
}

int main(int argc, char** argv) {
  const char* bg = argv[1];
  const char* out = argv[2];
  const int page = argc > 3 ? std::atoi(argv[3]) : 0;
  const double when = argc > 4 ? std::atof(argv[4]) : 3.0;
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(W), float(H));
  const std::string F = std::getenv("FONTS") ? std::string(std::getenv("FONTS")) + "/" : "fonts/";
  io.Fonts->AddFontDefault();
  gta4ps5::hub::g_assets.display = io.Fonts->AddFontFromFileTTF((F + "oswald-latin-700-normal.ttf").c_str(), 144.0f);
  gta4ps5::hub::g_assets.nav = io.Fonts->AddFontFromFileTTF((F + "oswald-latin-600-normal.ttf").c_str(), 68.0f);
  gta4ps5::hub::g_assets.body = io.Fonts->AddFontFromFileTTF((F + "inter-latin-400-normal.ttf").c_str(), 52.0f);
  gta4ps5::hub::g_assets.body_bold = io.Fonts->AddFontFromFileTTF((F + "inter-latin-600-normal.ttf").c_str(), 52.0f);
  unsigned char* pixels; io.Fonts->GetTexDataAsRGBA32(&pixels, &tw, &th); tex = pixels;
  // background (a PPM 1920x1080)
  fb.assign(size_t(W) * H * 3, 0.2f);
  if (FILE* f = std::fopen(bg, "rb")) {
    int w, h, m; if (std::fscanf(f, "P6 %d %d %d", &w, &h, &m) == 3 && w == W && h == H) { std::fgetc(f);
      std::vector<unsigned char> raw(size_t(w) * h * 3); std::fread(raw.data(), 1, raw.size(), f);
      for (size_t i = 0; i < raw.size(); ++i) fb[i] = raw[i] / 255.0f; }
    std::fclose(f);
  }
  DrawModel m;
  m.open = true;
  m.tabs = {"HOME", "PLAYER", "VEHICLES", "SPAWN", "TELEPORT", "WORLD", "WEAPONS", "CHAOS", "DELOREAN"};
  if (page == 0) {
    m.title = "HOME"; m.tab = 0;
    for (auto t : {"Player Options", "Vehicle Options", "Car Spawning", "Teleporting", "Time and Weather", "Weapons", "Chaos",
                   "Time Machine (DeLorean)", "Complete Current Mission", "60 FPS Mode: ON", "Menu Status"})
      m.lines.push_back({t, false, false});
    m.lines[1].selected = true; m.total = 11;
  } else if (page == 1) {
    m.title = "VEHICLE OPTIONS"; m.tab = 2;
    for (auto t : {"Vehicle God Mode: OFF", "Repair Current Vehicle", "Warp Into Spawned Car: ON",
                   "Batmobile Gadgets: ON (L1 missile, d-pad guns/boost, horn bomb)", "Jet Flight and Weapons (F-16, Batwing): ON",
                   "Jet Controls: Standard (bank to turn, like Ace Combat)", "Deimos/Nexus Pop-ups: Automatic (with the lights)",
                   "Headlights on D-pad Left: ON", "Bring Your Own Ride: OFF"})
      m.lines.push_back({t, false, false});
    m.lines[4].selected = true; m.total = 14; m.first = 0;
    m.toast = "Jet flight and weapons on (F-16, Batwing).";
  } else {
    m.title = "TIME AND WEATHER"; m.tab = 5;
    for (auto t : {"Hour: 14", "Minute: 30", "Set Time", "Weather: Extra Sunny", "Set Weather", "Freeze Time: OFF"})
      m.lines.push_back({t, false, false});
    m.lines[3].selected = true; m.total = 6;
  }
  m.page_serial = 0;
  // two frames: the first fixes the page serial, the second is drawn "when" seconds later
  for (int frame = 0; frame < 2; ++frame) {
    io.DeltaTime = frame ? float(when) : 0.016f;
    ImGui::NewFrame();
    DrawGlassMenu(io, m);
    ImGui::Render();
  }
  ImDrawData* dd = ImGui::GetDrawData();
  for (int l = 0; l < dd->CmdListsCount; ++l) {
    const ImDrawList* list = dd->CmdLists[l];
    for (const auto& cmd : list->CmdBuffer) {
      for (unsigned i = 0; i < cmd.ElemCount; i += 3) {
        const ImDrawIdx* idx = list->IdxBuffer.Data + cmd.IdxOffset + i;
        const ImDrawVert* v = list->VtxBuffer.Data + cmd.VtxOffset;
        Raster(v[idx[0]], v[idx[1]], v[idx[2]], cmd.ClipRect);
      }
    }
  }
  FILE* f = std::fopen(out, "wb");
  std::fprintf(f, "P6 %d %d 255\n", W, H);
  for (float v : fb) std::fputc(int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f), f);
  std::fclose(f);
}
