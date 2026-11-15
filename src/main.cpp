// otto-op1: OP-1 style groovebox for Windows, controlled by an Arturia MiniLab 3.
#include "core.hpp"
#include "midi.hpp"

#include <Gamma/Domain.h>
#include <GLFW/glfw3.h>
#include <RtAudio.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace otto;
namespace fs = std::filesystem;

std::string open_audio_dialog(); // filedialog.cpp

static std::string utf8(const fs::path& p)
{
  auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

static std::string note_name(int n)
{
  static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (n < 0) return "-";
  return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

// ---------------------------------------------------------------- palette
// OTTO encoder colours + one hue per engine / pad
static const ImU32 kEnc[4] = {IM_COL32(58, 134, 255, 255), IM_COL32(56, 200, 90, 255), IM_COL32(245, 245, 245, 255),
                              IM_COL32(255, 70, 60, 255)};
static const ImU32 kEngineCol[5] = {IM_COL32(58, 134, 255, 255), IM_COL32(255, 140, 0, 255), IM_COL32(255, 215, 0, 255),
                                    IM_COL32(190, 90, 255, 255), IM_COL32(0, 210, 200, 255)};
static const ImU32 kPadCol[8] = {IM_COL32(255, 70, 60, 255),  IM_COL32(255, 140, 0, 255), IM_COL32(255, 215, 0, 255),
                                 IM_COL32(120, 220, 60, 255), IM_COL32(0, 210, 200, 255), IM_COL32(58, 134, 255, 255),
                                 IM_COL32(150, 90, 255, 255), IM_COL32(255, 90, 200, 255)};
static const char* kPadNames[8] = {"KICK", "SNARE", "HAT", "OPEN", "CLAP", "TOM L", "TOM H", "COW"};
static const ImU32 kScreen = IM_COL32(8, 8, 10, 255), kPanel = IM_COL32(26, 26, 31, 255),
                   kDim = IM_COL32(70, 70, 78, 255), kText = IM_COL32(230, 230, 235, 255);

static ImU32 scale_col(ImU32 c, float k)
{
  ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
  return ImGui::ColorConvertFloat4ToU32({v.x * k, v.y * k, v.z * k, v.w});
}

// ---------------------------------------------------------------- audio
struct Audio {
  Studio* st = nullptr;
  std::unique_ptr<RtAudio> dac;
  std::vector<RtAudio::Api> apis;
  int api_i = 0;
  std::vector<unsigned> ids;
  std::vector<std::string> names;
  int dev_i = 0;
  unsigned buffer = 128;
  std::string status;
  std::string settings_path;

  static int cb(void* out, void*, unsigned n, double, RtAudioStreamStatus, void* user)
  {
    static_cast<Studio*>(user)->process(static_cast<float*>(out), n);
    return 0;
  }
  int api_index(RtAudio::Api a) const
  {
    for (size_t i = 0; i < apis.size(); i++)
      if (apis[i] == a) return int(i);
    return -1;
  }
  bool select_device(const std::string& name)
  {
    for (size_t i = 0; i < names.size(); i++)
      if (names[i] == name) { dev_i = int(i); return true; }
    return false;
  }
  /// Remembered choice, else ASIO (low latency; prefers "FL Studio ASIO") with WASAPI fallback
  void start(const std::string& path)
  {
    settings_path = path;
    RtAudio::getCompiledApi(apis);
    std::ifstream in(path);
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (in && !j.is_discarded()) {
      buffer = j.value("buffer", buffer);
      for (size_t i = 0; i < apis.size(); i++)
        if (RtAudio::getApiName(apis[i]) == j.value("api", "")) api_i = int(i);
      scan();
      select_device(j.value("device", ""));
      if (open()) return;
    }
    if (int a = api_index(RtAudio::WINDOWS_ASIO); a >= 0) {
      api_i = a;
      scan();
      for (const char* pref : {"FL Studio ASIO", "ASIO4ALL v2"})
        if (select_device(pref)) break;
      if (open()) return;
    }
    api_i = std::max(0, api_index(RtAudio::WINDOWS_WASAPI));
    scan();
    open();
  }
  void remember()
  {
    if (settings_path.empty() || names.empty()) return;
    nlohmann::json j{{"api", RtAudio::getApiName(apis[size_t(api_i)])}, {"device", names[size_t(dev_i)]}, {"buffer", buffer}};
    std::ofstream(settings_path) << j.dump(2);
  }
  void scan()
  {
    close();
    dac = std::make_unique<RtAudio>(apis[size_t(api_i)], [this](RtAudioErrorType, const std::string& t) { status = t; });
    ids.clear();
    names.clear();
    dev_i = 0;
    unsigned def = dac->getDefaultOutputDevice();
    for (unsigned id : dac->getDeviceIds()) {
      auto info = dac->getDeviceInfo(id);
      if (info.outputChannels < 2) continue;
      if (id == def) dev_i = int(ids.size());
      ids.push_back(id);
      names.push_back(info.name);
    }
  }
  bool open()
  {
    close();
    if (ids.empty()) { status = "Sin dispositivos de salida"; return false; }
    RtAudio::StreamParameters p;
    p.deviceId = ids[size_t(dev_i)];
    p.nChannels = 2;
    RtAudio::StreamOptions o;
    o.flags = RTAUDIO_MINIMIZE_LATENCY;
    o.streamName = "otto-op1";
    unsigned bf = buffer;
    if (dac->openStream(&p, nullptr, RTAUDIO_FLOAT32, unsigned(kSR), &bf, &cb, st, &o) != RTAUDIO_NO_ERROR) {
      status = dac->getErrorText();
      return false;
    }
    dac->startStream();
    char b[160];
    std::snprintf(b, sizeof b, "%s | buffer %u (%.1f ms)", names[size_t(dev_i)].c_str(), bf, bf * 1000.f / kSR);
    status = b;
    return true;
  }
  void close()
  {
    if (dac && dac->isStreamOpen()) dac->closeStream();
  }
};

// ---------------------------------------------------------------- small widgets
static Midi* g_midi = nullptr;
static ImFont* g_big = nullptr;

static void learn_button(Param& p)
{
  ImGui::PushID(&p);
  bool active = g_midi->learn == p.id;
  if (active) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(220, 130, 0, 255));
  if (ImGui::SmallButton("L")) {
    std::lock_guard lk(g_midi->mtx);
    g_midi->learn = active ? "" : p.id;
  }
  if (active) ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("MIDI learn: pulsa y mueve un control del MiniLab");
  ImGui::PopID();
}

static void slider(Param& p)
{
  ImGui::PushID(&p);
  learn_button(p);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1);
  if (p.integer) {
    int v = p.geti();
    if (ImGui::SliderInt("##s", &v, int(p.min), int(p.max), (p.name + ": %d").c_str())) p.set(float(v));
  } else {
    float v = p.get();
    if (ImGui::SliderFloat("##s", &v, p.min, p.max, (p.name + ": %.2f").c_str())) p.set(v);
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) p.set(p.def);
  ImGui::PopID();
}

/// Big OP-1 style knob: drag vertically, wheel fine-tunes, right click resets
static void knob(Param* p, ImU32 col, float r, const char* caption = nullptr)
{
  ImGui::BeginGroup();
  ImVec2 c0 = ImGui::GetCursorScreenPos();
  float w = r * 2 + 20;
  ImGui::PushID(p ? (void*)p : (void*)&col);
  ImGui::InvisibleButton("k", ImVec2(w, r * 2 + 8));
  bool hov = ImGui::IsItemHovered();
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 c(c0.x + w * 0.5f, c0.y + r + 4);
  dl->AddCircleFilled({c.x, c.y + 3}, r, IM_COL32(0, 0, 0, 120), 48); // shadow
  dl->AddCircleFilled(c, r, hov ? IM_COL32(52, 52, 58, 255) : IM_COL32(42, 42, 48, 255), 48);
  float a0 = 0.75f * kPi;
  for (int t = 0; t <= 10; t++) { // ticks
    float a = a0 + 1.5f * kPi * float(t) / 10.f;
    dl->AddLine({c.x + std::cos(a) * (r + 3), c.y + std::sin(a) * (r + 3)},
                {c.x + std::cos(a) * (r + 7), c.y + std::sin(a) * (r + 7)}, kDim, 2);
  }
  if (p) {
    if (ImGui::IsItemActive()) p->set_norm(p->norm() - ImGui::GetIO().MouseDelta.y * 0.006f);
    if (hov && ImGui::GetIO().MouseWheel != 0)
      p->set(p->integer ? p->get() + ImGui::GetIO().MouseWheel : p->min + (p->norm() + ImGui::GetIO().MouseWheel * 0.02f) * (p->max - p->min));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) p->set(p->def);
    float a1 = a0 + 1.5f * kPi * p->norm();
    dl->PathArcTo(c, r - 5, a0, a0 + 1.5f * kPi, 48);
    dl->PathStroke(IM_COL32(25, 25, 28, 255), 0, 8);
    dl->PathArcTo(c, r - 5, a0, a1, 48);
    dl->PathStroke(col, 0, 8);
    dl->AddCircleFilled(c, r * 0.55f, IM_COL32(30, 30, 34, 255), 32);
    dl->AddLine(c, {c.x + std::cos(a1) * (r * 0.55f), c.y + std::sin(a1) * (r * 0.55f)}, col, 4);
    char b[32];
    std::snprintf(b, sizeof b, p->integer ? "%.0f" : "%.2f", p->get());
    ImVec2 ts = ImGui::CalcTextSize(b);
    dl->AddText({c.x - ts.x * 0.5f, c.y + r * 0.6f}, kText, b);
  }
  ImGui::PopID();
  const char* label = caption ? caption : p ? p->name.c_str() : "-";
  float tw = ImGui::CalcTextSize(label).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (w - tw) * 0.5f));
  ImGui::TextColored(ImColor(p ? col : kDim), "%s", label);
  ImGui::EndGroup();
}

static bool pill(const char* label, bool on, ImU32 col, ImVec2 size = {0, 0})
{
  ImGui::PushStyleColor(ImGuiCol_Button, on ? col : kPanel);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? col : scale_col(col, 0.45f));
  ImGui::PushStyleColor(ImGuiCol_Text, on ? IM_COL32(10, 10, 12, 255) : kText);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20);
  bool r = ImGui::Button(label, size);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  return r;
}

// ---------------------------------------------------------------- screen drawings (rect based)
struct Alg {
  std::vector<std::pair<int, int>> edges;
  std::vector<int> carriers;
};
static const Alg kAlgs[11] = {
  {{{3, 2}, {2, 1}, {1, 0}}, {0}},       {{{3, 1}, {2, 1}, {1, 0}}, {0}},
  {{{3, 0}, {2, 1}, {1, 0}}, {0}},       {{{3, 1}, {3, 2}, {1, 0}, {2, 0}}, {0}},
  {{{3, 2}, {2, 1}, {2, 0}}, {0, 1}},    {{{3, 2}, {2, 1}}, {0, 1}},
  {{{1, 0}, {2, 0}, {3, 0}}, {0}},       {{{1, 0}, {3, 2}}, {0, 2}},
  {{{3, 0}, {3, 1}, {3, 2}}, {0, 1, 2}}, {{{3, 2}}, {0, 1, 2}},
  {{}, {0, 1, 2, 3}},
};

static void draw_fm(ImDrawList* dl, ImVec2 o, ImVec2 sz, int alg, float energy)
{
  const auto& a = kAlgs[std::clamp(alg, 0, 10)];
  int level[4] = {0, 0, 0, 0};
  for (int pass = 0; pass < 4; pass++)
    for (auto [m, t] : a.edges) level[m] = std::max(level[m], level[t] + 1);
  float cx = o.x + sz.x * 0.5f, step = (sz.y - 70) / 3.f;
  auto pos = [&](int op) {
    int n = 0, idx = 0;
    for (int k = 0; k < 4; k++)
      if (level[k] == level[op]) { if (k < op) idx++; n++; }
    return ImVec2(cx + (float(idx) - float(n - 1) * 0.5f) * 64, o.y + sz.y - 34 - float(level[op]) * step);
  };
  for (auto [m, t] : a.edges) dl->AddLine(pos(m), pos(t), IM_COL32(150, 150, 160, 255), 3);
  for (int op = 0; op < 4; op++) {
    bool car = std::find(a.carriers.begin(), a.carriers.end(), op) != a.carriers.end();
    ImVec2 p = pos(op);
    float r = 15 + (car ? energy * 8 : 0);
    if (car) dl->AddCircleFilled(p, r + 6, scale_col(kEnc[0], 0.3f));
    dl->AddCircleFilled(p, r, car ? kEnc[0] : kDim);
    char b[4] = {char('1' + op), 0};
    dl->AddText({p.x - 4, p.y - 8}, kText, b);
  }
  char t[32];
  std::snprintf(t, sizeof t, "ALGORITMO %d", alg + 1);
  dl->AddText({o.x + 10, o.y + 8}, kEnc[0], t);
}

static void draw_envelope(ImDrawList* dl, ImVec2 o, ImVec2 sz, float a, float d, float s, float r, ImU32 col)
{
  auto dur = [](float x) { return 5 * x * x + 0.001f; };
  float ta = dur(a), td = dur(d), tr = dur(r), hold = 0.5f;
  float total = ta + td + hold + tr, sx = (sz.x - 30) / total, y0 = o.y + sz.y - 16, yh = sz.y - 50;
  ImVec2 pts[5] = {{o.x + 15, y0}, {}, {}, {}, {}};
  pts[1] = {pts[0].x + ta * sx, y0 - yh};
  pts[2] = {pts[1].x + td * sx, y0 - yh * s};
  pts[3] = {pts[2].x + hold * sx, pts[2].y};
  pts[4] = {pts[3].x + tr * sx, y0};
  for (int i = 0; i < 4; i++) // filled area
    dl->AddQuadFilled(pts[i], pts[i + 1], {pts[i + 1].x, y0}, {pts[i].x, y0}, scale_col(col, 0.25f));
  dl->AddPolyline(pts, 5, col, 0, 3);
  const char* lab[4] = {"A", "D", "S", "R"};
  for (int i = 1; i < 5; i++) {
    dl->AddCircleFilled(pts[i], 5, kEnc[size_t(i - 1)]);
    dl->AddText({pts[i].x - 4, pts[i].y - 22}, kEnc[size_t(i - 1)], lab[i - 1]);
  }
  dl->AddText({o.x + 10, o.y + 8}, col, "ENVOLVENTE");
}

static void draw_string(ImDrawList* dl, ImVec2 o, ImVec2 sz, float energy, float t)
{
  float y = o.y + sz.y * 0.55f;
  dl->AddCircleFilled({o.x + 20, y}, 6, kDim);
  dl->AddCircleFilled({o.x + sz.x - 20, y}, 6, kDim);
  for (int k = 0; k < 3; k++) { // three harmonics shimmering
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 60; i++) {
      float x = float(i) / 60.f;
      float amp = energy * sz.y * 0.35f / float(k + 1);
      pts.push_back({o.x + 20 + x * (sz.x - 40), y + std::sin(kPi * x * float(k + 1)) * amp * std::sin(t * 30.f * float(k + 1))});
    }
    dl->AddPolyline(pts.data(), int(pts.size()), scale_col(kEngineCol[2], 1.f - 0.3f * float(k)), 0, 3.f - float(k));
  }
  dl->AddText({o.x + 10, o.y + 8}, kEngineCol[2], "CUERDA");
}

static void draw_cluster(ImDrawList* dl, ImVec2 o, ImVec2 sz, float spread, float energy, float t)
{
  float cx = o.x + sz.x * 0.5f;
  const float off[6] = {-1, -0.6f, -0.25f, 0.25f, 0.6f, 1};
  for (int k = 0; k < 6; k++) {
    float x = cx + off[k] * (10 + spread * sz.x * 0.4f);
    float h = (0.25f + energy * 0.75f) * (sz.y - 60) * (0.7f + 0.3f * std::sin(t * 6 + float(k)));
    dl->AddRectFilled({x - 7, o.y + sz.y - 20 - h}, {x + 7, o.y + sz.y - 20}, kEngineCol[3], 4);
  }
  dl->AddText({o.x + 10, o.y + 8}, kEngineCol[3], "CLUSTER");
}

static void draw_nuke(ImDrawList* dl, ImVec2 o, ImVec2 sz, float cutoff, float res)
{
  std::vector<ImVec2> pts;
  float fc = 0.05f + cutoff * 0.9f, q = 0.5f + res * 6;
  for (int i = 0; i <= 80; i++) {
    float x = float(i) / 80.f, w = x / fc;
    float mag = 1.f / std::sqrt((1 - w * w) * (1 - w * w) + (w / q) * (w / q));
    mag = std::min(mag / std::max(q, 1.f), 1.f); // DC sits below the resonant peak, like a lowpass
    pts.push_back({o.x + 10 + x * (sz.x - 20), o.y + sz.y - 16 - mag * (sz.y - 50)});
  }
  dl->AddPolyline(pts.data(), int(pts.size()), kEngineCol[1], 0, 3);
  dl->AddText({o.x + 10, o.y + 8}, kEngineCol[1], "FILTRO LADDER");
}

static void draw_wave(ImDrawList* dl, ImVec2 o, ImVec2 sz, const std::vector<float>& w, float s0, float s1, ImU32 col)
{
  float mid = o.y + sz.y * 0.55f;
  int cols = int(sz.x - 20);
  for (int c = 0; c < cols && !w.empty(); c++) {
    size_t a = w.size() * size_t(c) / size_t(cols), b = w.size() * size_t(c + 1) / size_t(cols);
    float pk = 0;
    for (size_t i = a; i < b; i += 7) pk = std::max(pk, std::abs(w[i]));
    float x = o.x + 10 + float(c);
    float fx = float(c) / float(cols);
    ImU32 cc = (fx >= s0 && fx <= s1) ? col : kDim;
    dl->AddLine({x, mid - pk * sz.y * 0.4f}, {x, mid + pk * sz.y * 0.4f}, cc);
  }
  dl->AddLine({o.x + 10 + s0 * float(cols), o.y + 30}, {o.x + 10 + s0 * float(cols), o.y + sz.y - 8}, kEnc[0], 2);
  dl->AddLine({o.x + 10 + s1 * float(cols), o.y + 30}, {o.x + 10 + s1 * float(cols), o.y + sz.y - 8}, kEnc[1], 2);
  dl->AddText({o.x + 10, o.y + 8}, col, "SAMPLE");
}

static void draw_reel(ImDrawList* dl, ImVec2 c, float r_tape, float angle, ImU32 col)
{
  dl->AddCircleFilled(c, r_tape, IM_COL32(40, 30, 22, 255), 48); // wound tape
  dl->AddCircle(c, 30, kDim, 40, 3);
  for (int k = 0; k < 3; k++) {
    float a = angle + float(k) * 2.f * kPi / 3.f;
    dl->AddLine({c.x + std::cos(a) * 8, c.y + std::sin(a) * 8}, {c.x + std::cos(a) * 28, c.y + std::sin(a) * 28}, col, 5);
  }
  dl->AddCircleFilled(c, 8, col);
}

// ---------------------------------------------------------------- app state
struct UI {
  Studio& st;
  Midi& midi;
  Audio& audio;
  fs::path base;
  int octave = 48;
  int mouse_note = -1;
  std::array<uint32_t, 8> last_hits{};
  std::array<float, 8> flash{};
  float reel = 0, energy = 0, time = 0;
  char proj[260]{}, wav[260]{};
  std::string status;
  std::array<ImVec4, 8> pad_rect{}; // x0,y0,x1,y1 for drag & drop onto pads
  // presets of the selected engine
  std::vector<std::string> presets;
  std::string presets_for;
  int preset_idx = -1;
  char preset_name[64] = "mi sonido";
};

static fs::path preset_dir(UI& u) { return u.base / "presets" / u.st.synth().title; }

static void refresh_presets(UI& u)
{
  u.presets.clear();
  std::error_code ec;
  for (auto& e : fs::directory_iterator(preset_dir(u), ec))
    if (e.path().extension() == ".json") u.presets.push_back(utf8(e.path().stem()));
  std::sort(u.presets.begin(), u.presets.end());
  u.presets_for = u.st.synth().title;
}

static bool save_preset(UI& u, const std::string& name)
{
  std::error_code ec;
  fs::create_directories(preset_dir(u), ec);
  nlohmann::json j;
  for (auto* p : u.st.synth().params) j[p->id] = p->get();
  std::ofstream f(preset_dir(u) / u8path(name + ".json"));
  f << j.dump(2);
  refresh_presets(u);
  return bool(f);
}

static bool load_preset(UI& u, const std::string& name)
{
  std::ifstream f(preset_dir(u) / u8path(name + ".json"));
  auto j = nlohmann::json::parse(f, nullptr, false);
  if (!f || j.is_discarded()) return false;
  for (auto& [id, v] : j.items())
    if (auto* p = registry().find(id); p && v.is_number()) p->set(v.get<float>());
  return true;
}

static void header(UI& u)
{
  auto& st = u.st;
  auto* dl = ImGui::GetWindowDrawList();
  ImGui::PushFont(g_big);
  const char* logo = "otto";
  ImVec2 p = ImGui::GetCursorScreenPos();
  float x = p.x;
  for (int i = 0; i < 4; i++) {
    char ch[2] = {logo[i], 0};
    dl->AddText(g_big, g_big->FontSize, {x, p.y}, kEnc[size_t(i)], ch);
    x += ImGui::CalcTextSize(ch).x + 1;
  }
  dl->AddText(g_big, g_big->FontSize, {x + 6, p.y}, kDim, "op1");
  ImGui::Dummy({x - p.x + 6 + ImGui::CalcTextSize("op1").x, g_big->FontSize});
  ImGui::PopFont();

  for (int i = 0; i < int(st.synths.size()); i++) {
    ImGui::SameLine();
    if (pill(st.synths[size_t(i)]->title.c_str(), st.synth_sel->geti() == i, kEngineCol[i % 5], {100, 34})) {
      st.synth_sel->set(float(i));
      st.page = 0;
    }
  }

  // transport: drawn icons
  ImGui::SameLine(0, 30);
  bool playing = st.playing, rec = st.recording;
  auto icon_btn = [&](const char* id, ImU32 bg, auto&& draw) {
    ImVec2 o = ImGui::GetCursorScreenPos();
    bool r = ImGui::InvisibleButton(id, {44, 34});
    dl->AddRectFilled(o, {o.x + 44, o.y + 34}, ImGui::IsItemHovered() ? scale_col(bg, 1.2f) : bg, 17);
    draw(ImVec2(o.x + 22, o.y + 17));
    return r;
  };
  if (icon_btn("play", playing ? kEnc[1] : kPanel, [&](ImVec2 c) {
        if (playing) {
          dl->AddRectFilled({c.x - 7, c.y - 8}, {c.x - 2, c.y + 8}, kText);
          dl->AddRectFilled({c.x + 2, c.y - 8}, {c.x + 7, c.y + 8}, kText);
        } else dl->AddTriangleFilled({c.x - 6, c.y - 9}, {c.x - 6, c.y + 9}, {c.x + 9, c.y}, kText);
      }))
    st.ui_q.push({Ev::TogglePlay});
  ImGui::SameLine();
  if (icon_btn("stop", kPanel, [&](ImVec2 c) { dl->AddRectFilled({c.x - 7, c.y - 7}, {c.x + 7, c.y + 7}, kText); }))
    st.ui_q.push({Ev::Stop});
  ImGui::SameLine();
  bool blink = rec && std::fmod(u.time, 1.f) < 0.5f;
  if (icon_btn("rec", rec ? scale_col(kEnc[3], blink ? 1.f : 0.6f) : kPanel,
               [&](ImVec2 c) { dl->AddCircleFilled(c, 8, rec ? kText : kEnc[3]); }))
    st.ui_q.push({Ev::ToggleRec});

  ImGui::SameLine(0, 24);
  ImGui::SetNextItemWidth(90);
  int b = st.bpm->geti();
  if (ImGui::DragInt("BPM", &b, 0.5f, 40, 240)) st.bpm->set(float(b));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90);
  int bars = st.bars->geti();
  if (ImGui::SliderInt("compases", &bars, 1, 16)) st.bars->set(float(bars));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90);
  float mv = st.master->get();
  if (ImGui::SliderFloat("master", &mv, 0, 1, "%.2f")) st.master->set(mv);

  // VU
  ImGui::SameLine();
  ImVec2 v = ImGui::GetCursorScreenPos();
  float pk = st.peak;
  ImGui::Dummy({110, 34});
  for (int i = 0; i < 12; i++) {
    float th = float(i) / 12.f;
    ImU32 c = i < 8 ? kEnc[1] : i < 10 ? kEngineCol[2] : kEnc[3];
    dl->AddRectFilled({v.x + float(i) * 9, v.y + 8}, {v.x + float(i) * 9 + 7, v.y + 26}, pk > th ? c : IM_COL32(40, 40, 44, 255), 2);
  }
}

static void screen(UI& u, float H)
{
  auto& st = u.st;
  Engine& eng = st.synth();
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float W = ImGui::GetContentRegionAvail().x;
  ImGui::Dummy({W, H});
  dl->AddRectFilled(o, {o.x + W, o.y + H}, kScreen, 14);
  dl->AddRect(o, {o.x + W, o.y + H}, IM_COL32(50, 50, 56, 255), 14, 0, 2);

  float zw[3] = {W * 0.34f, W * 0.36f, W * 0.30f};
  ImVec2 z0 = {o.x + 8, o.y + 8}, s0 = {zw[0] - 12, H - 16};
  ImVec2 z1 = {o.x + zw[0], o.y + 8}, s1 = {zw[1] - 8, H - 16};
  ImVec2 z2 = {o.x + zw[0] + zw[1], o.y + 8}, s2 = {zw[2] - 8, H - 16};

  // --- zone 1: engine picture (or envelope when on an envelope page)
  int page = std::clamp(int(st.page), 0, int(eng.pages.size()) - 1);
  int sel = st.synth_sel->geti();
  auto* p0 = eng.page_param(page, 0);
  if (p0 && p0->name.find("Attack") != std::string::npos) {
    auto* p3 = eng.page_param(page, 3);
    bool has_r = p3 && p3->name.find("Release") != std::string::npos;
    draw_envelope(dl, z0, s0, p0->get(), eng.page_param(page, 1)->get(), eng.page_param(page, 2)->get(),
                  has_r ? p3->get() : 0.f, kEngineCol[sel % 5]);
  } else if (eng.title == "OTTO.FM") draw_fm(dl, z0, s0, registry().find("fm.alg")->geti(), u.energy);
  else if (eng.title == "NUKE") draw_nuke(dl, z0, s0, registry().find("nuke.cutoff")->get(), registry().find("nuke.res")->get());
  else if (eng.title == "STRING") draw_string(dl, z0, s0, u.energy, u.time);
  else if (eng.title == "CLUSTER") draw_cluster(dl, z0, s0, registry().find("cl.spread")->get(), u.energy, u.time);
  else if (auto* w = sampler_data(eng))
    draw_wave(dl, z0, s0, *w, registry().find("smp.start")->get(), registry().find("smp.end")->get(), kEngineCol[4]);
  dl->AddText({z0.x + 10, z0.y + s0.y - 22}, kDim, eng.pages[size_t(page)].c_str());

  // --- zone 2: cassette
  size_t lf = std::max<size_t>(1, st.loop_frames);
  float frac = float(st.tape_pos) / float(lf);
  float cy = z1.y + 70;
  ImVec2 lc{z1.x + s1.x * 0.28f, cy}, rc{z1.x + s1.x * 0.72f, cy};
  dl->AddRectFilled({z1.x + 10, z1.y + 6}, {z1.x + s1.x - 10, cy + 60}, IM_COL32(30, 30, 34, 255), 10);
  float rmin = 32, rmax = 52;
  draw_reel(dl, lc, rmin + (rmax - rmin) * (1 - frac), u.reel, st.recording ? kEnc[3] : kText);
  draw_reel(dl, rc, rmin + (rmax - rmin) * frac, u.reel * 1.3f, st.recording ? kEnc[3] : kText);
  dl->AddLine({lc.x, cy + 50}, {rc.x, cy + 50}, IM_COL32(90, 70, 50, 255), 3);
  char pos[64];
  std::snprintf(pos, sizeof pos, "%05.1fs / %.1fs", st.tape_pos / kSR, lf / kSR);
  dl->AddText({z1.x + s1.x * 0.5f - 45, cy - 8}, kDim, pos);
  float ty = cy + 70, th = std::max(12.f, (s1.y - (ty - z1.y) - 6) / 4.f - 4);
  for (int t = 0; t < Tape::T; t++) {
    ImVec2 a{z1.x + 10, ty + float(t) * (th + 4)}, b{z1.x + s1.x - 10, a.y + th};
    bool armed = st.rec_track->geti() == t;
    dl->AddRectFilled(a, b, armed ? IM_COL32(36, 24, 24, 255) : IM_COL32(22, 22, 26, 255), 4);
    const auto& trk = st.tape.trk[size_t(t)];
    int cols = int(b.x - a.x);
    float mid = (a.y + b.y) * 0.5f;
    ImU32 col = st.tape.mute[size_t(t)] ? kDim : kEnc[size_t(t)];
    for (int c = 0; c < cols; c++) {
      size_t f0 = lf * size_t(c) / size_t(cols), f1 = lf * size_t(c + 1) / size_t(cols);
      float pk = 0;
      for (size_t f = f0; f < f1; f += 113) pk = std::max(pk, std::abs(trk[f * 2])); // ponytail: strided peak
      pk = std::min(pk, 1.f) * th * 0.5f;
      if (pk > 0.5f) dl->AddLine({a.x + float(c), mid - pk}, {a.x + float(c), mid + pk}, col);
    }
    float li = st.loop_in->get(), lo2 = st.loop_out->get();
    if (li > 0) dl->AddRectFilled(a, {a.x + li * float(cols), b.y}, IM_COL32(0, 0, 0, 150));
    if (lo2 < 1) dl->AddRectFilled({a.x + lo2 * float(cols), a.y}, b, IM_COL32(0, 0, 0, 150));
    float x = a.x + frac * float(cols);
    dl->AddLine({x, a.y}, {x, b.y}, IM_COL32(255, 255, 255, 180), 2);
  }

  // --- zone 3: oscilloscope
  ImVec2 sc{z2.x + 4, z2.y}, ss{s2.x - 4, s2.y};
  float mid = sc.y + ss.y * 0.5f;
  for (int g = 1; g < 4; g++)
    dl->AddLine({sc.x, sc.y + ss.y * float(g) / 4}, {sc.x + ss.x, sc.y + ss.y * float(g) / 4}, IM_COL32(25, 40, 30, 255));
  uint32_t w = st.scope_w;
  // trigger on rising zero crossing for a stable picture
  uint32_t start = w - 600;
  for (uint32_t i = w - 1000; i < w - 520; i++)
    if (st.scope[(i - 1) % Studio::kScope] < 0 && st.scope[i % Studio::kScope] >= 0) { start = i; break; }
  std::vector<ImVec2> pts;
  for (int i = 0; i < 500; i++)
    pts.push_back({sc.x + float(i) / 500.f * ss.x,
                   mid - std::clamp(st.scope[(start + uint32_t(i)) % Studio::kScope].load(), -1.f, 1.f) * ss.y * 0.45f});
  ImU32 sc_col = kEngineCol[sel % 5];
  dl->AddPolyline(pts.data(), int(pts.size()), scale_col(sc_col, 0.35f), 0, 6);
  dl->AddPolyline(pts.data(), int(pts.size()), sc_col, 0, 2);
  dl->AddText({sc.x + 6, sc.y + 6}, sc_col, eng.title.c_str());
  char bpm[32];
  std::snprintf(bpm, sizeof bpm, "%d BPM", st.bpm->geti());
  dl->AddText({sc.x + ss.x - 70, sc.y + 6}, kDim, bpm);
}

static void controls(UI& u)
{
  auto& st = u.st;
  Engine& eng = st.synth();
  int page = std::clamp(int(st.page), 0, int(eng.pages.size()) - 1);
  ImGui::BeginGroup();
  for (int k = 0; k < 4; k++) {
    if (k) ImGui::SameLine(0, 14);
    knob(eng.page_param(page, k), kEnc[k], 40);
  }
  for (int i = 0; i < int(eng.pages.size()); i++) {
    if (i) ImGui::SameLine();
    if (pill(eng.pages[size_t(i)].c_str(), i == page, kEngineCol[st.synth_sel->geti() % 5])) st.page = i;
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, 40);
  ImGui::BeginGroup();
  ImGui::TextDisabled("FX  (encoders 5-8)");
  knob(st.reverb.mix, kEnc[0], 28, "Reverb");
  ImGui::SameLine();
  knob(st.reverb.length, kEnc[1], 28, "Largo");
  ImGui::SameLine();
  knob(st.reverb.shape, kEnc[2], 28, "Tono");
  ImGui::SameLine();
  knob(st.delay.mix, kEnc[3], 28, "Delay");
  ImGui::EndGroup();
}

static void keyboard(UI& u, ImVec2 size)
{
  auto& st = u.st;
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("kbd", size);
  bool active = ImGui::IsItemActive();
  ImVec2 m = ImGui::GetIO().MousePos;
  const int white[7] = {0, 2, 4, 5, 7, 9, 11};
  const int nw = 15; // 2 octaves + top C
  float kw = size.x / float(nw);
  auto held = [&](int n) { return n >= 0 && n < 128 && ((st.held[size_t(n >> 6)].load() >> (n & 63)) & 1); };
  ImU32 col = kEngineCol[st.synth_sel->geti() % 5];

  int hover = -1;
  // black keys first for hit test
  for (int i = 0; i < nw - 1; i++) {
    int semi = white[i % 7];
    if (semi == 4 || semi == 11) continue;
    float x = o.x + (float(i) + 0.68f) * kw;
    if (m.x >= x && m.x < x + kw * 0.64f && m.y >= o.y && m.y < o.y + size.y * 0.6f) hover = u.octave + (i / 7) * 12 + semi + 1;
  }
  if (hover < 0 && m.x >= o.x && m.x < o.x + size.x && m.y >= o.y && m.y < o.y + size.y) {
    int i = int((m.x - o.x) / kw);
    hover = u.octave + (i / 7) * 12 + white[i % 7];
  }
  int want = active ? hover : -1;
  if (want != u.mouse_note) {
    if (u.mouse_note >= 0) st.ui_q.push({Ev::NoteOff, u.mouse_note});
    if (want >= 0) st.ui_q.push({Ev::NoteOn, want, 0.8f});
    u.mouse_note = want;
  }

  for (int i = 0; i < nw; i++) {
    int n = u.octave + (i / 7) * 12 + white[i % 7];
    ImVec2 a{o.x + float(i) * kw + 1, o.y}, b{a.x + kw - 2, o.y + size.y};
    dl->AddRectFilled(a, b, held(n) ? col : IM_COL32(225, 225, 230, 255), 6, ImDrawFlags_RoundCornersBottom);
    if (i % 7 == 0) {
      char c[8];
      std::snprintf(c, sizeof c, "C%d", n / 12 - 1);
      dl->AddText({a.x + 4, b.y - 20}, IM_COL32(90, 90, 100, 255), c);
    }
  }
  for (int i = 0; i < nw - 1; i++) {
    int semi = white[i % 7];
    if (semi == 4 || semi == 11) continue;
    int n = u.octave + (i / 7) * 12 + semi + 1;
    float x = o.x + (float(i) + 0.68f) * kw;
    dl->AddRectFilled({x, o.y}, {x + kw * 0.64f, o.y + size.y * 0.6f}, held(n) ? col : IM_COL32(28, 28, 32, 255), 4,
                      ImDrawFlags_RoundCornersBottom);
  }
}

static void pads(UI& u, ImVec2 size)
{
  auto& st = u.st;
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float pw = (size.x - 3 * 8) / 4, ph = (size.y - 8) / 2;
  for (int i = 0; i < 8; i++) {
    uint32_t h = st.pad_hits[size_t(i)];
    if (h != u.last_hits[size_t(i)]) {
      u.last_hits[size_t(i)] = h;
      u.flash[size_t(i)] = 1;
    }
    ImVec2 a{o.x + float(i % 4) * (pw + 8), o.y + float(i / 4) * (ph + 8)}, b{a.x + pw, a.y + ph};
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(i);
    ImGui::InvisibleButton("pad", {pw, ph});
    if (ImGui::IsItemActivated()) st.ui_q.push({Ev::Drum, i, 0.9f});
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s\nclic derecho o arrastra un audio aqui para cambiarlo",
                        st.drums.names[size_t(i)].empty() ? "kit interno" : st.drums.names[size_t(i)].c_str());
    if (ImGui::BeginPopupContextItem("padmenu")) {
      if (ImGui::MenuItem("Cargar sample...")) {
        auto path = open_audio_dialog();
        if (!path.empty()) u.status = st.drums.load_pad(i, path) ? "Pad cargado" : "No se pudo leer el audio";
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    u.pad_rect[size_t(i)] = {a.x, a.y, b.x, b.y};
    float f = u.flash[size_t(i)];
    float grow = f * 4;
    if (f > 0.05f) dl->AddRectFilled({a.x - grow, a.y - grow}, {b.x + grow, b.y + grow}, scale_col(kPadCol[i], 0.4f * f), 12);
    dl->AddRectFilled(a, b, scale_col(kPadCol[i], 0.35f + 0.65f * f), 10);
    ImU32 tc = f > 0.5f ? IM_COL32(10, 10, 12, 230) : IM_COL32(255, 255, 255, 220);
    dl->AddText({a.x + 8, a.y + 6}, tc, kPadNames[i]);
    if (!st.drums.names[size_t(i)].empty()) dl->AddText({a.x + 8, b.y - 22}, tc, st.drums.names[size_t(i)].substr(0, 14).c_str());
  }
  ImGui::SetCursorScreenPos({o.x, o.y + size.y});
  ImGui::Dummy({0, 0});
}

static void melody_row(UI& u)
{
  auto& st = u.st;
  auto* dl = ImGui::GetWindowDrawList();
  ImU32 col = kEngineCol[st.synth_sel->geti() % 5];
  bool nrec = st.note_rec;
  if (pill(nrec ? "REC NOTAS  ON" : "REC NOTAS", nrec, kEnc[3], {150, 28})) st.note_rec = !nrec;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Con PLAY activo, lo que toques en el teclado se graba en la secuencia de este motor");
  ImGui::SameLine();
  if (ImGui::Button("Borrar notas"))
    for (auto& n : st.cur_melody()) n = -1;
  ImGui::SameLine();
  ImGui::TextDisabled("clic = ultima nota (%s)  der = silencio  rueda = +/-1", note_name(st.last_note).c_str());
  slider(*st.seq_gate);

  int cur = st.playing ? std::max(0, int(st.step)) % 16 : -1;
  ImVec2 o = ImGui::GetCursorScreenPos();
  float W = ImGui::GetContentRegionAvail().x, cw = (W - 70) / 16.f, chh = 34;
  dl->AddText({o.x, o.y + 8}, col, st.synth().title.c_str());
  for (int s = 0; s < 16; s++) {
    auto& cell = st.cur_melody()[size_t(s)];
    int n = cell;
    ImVec2 a{o.x + 64 + float(s) * cw, o.y}, b{a.x + cw - 3, a.y + chh};
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(1000 + s);
    if (ImGui::InvisibleButton("n", {cw - 3, chh}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight)) {
      if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) cell = -1;
      else cell = st.last_note.load();
    }
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0 && n >= 0)
      cell = std::clamp(n + int(ImGui::GetIO().MouseWheel), 0, 127);
    ImGui::PopID();
    ImU32 c = n >= 0 ? col : (s % 4 == 0 ? IM_COL32(44, 44, 50, 255) : IM_COL32(34, 34, 40, 255));
    if (s == cur) c = n >= 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(70, 70, 80, 255);
    dl->AddRectFilled(a, b, c, 6);
    if (n >= 0) dl->AddText({a.x + 5, a.y + 9}, IM_COL32(10, 10, 12, 255), note_name(n).c_str());
  }
  ImGui::SetCursorScreenPos({o.x, o.y + chh + 8});
  ImGui::Separator();
}

static void tab_sequence(UI& u)
{
  auto& st = u.st;
  auto* dl = ImGui::GetWindowDrawList();
  melody_row(u);
  bool srec = st.step_rec;
  if (pill(srec ? "REC PATRON  ON" : "REC PATRON", srec, kEnc[3], {150, 28})) st.step_rec = !srec;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Con PLAY activo, lo que toques en los pads se graba cuantizado a 1/16");
  ImGui::SameLine();
  if (ImGui::Button("Borrar patron"))
    for (auto& s : st.cur_steps()) s = 0;
  ImGui::SameLine();
  ImGui::TextDisabled("clic en la grilla para activar pasos");

  int cur = st.playing ? std::max(0, int(st.step)) % 16 : -1;
  ImVec2 o = ImGui::GetCursorScreenPos();
  float W = ImGui::GetContentRegionAvail().x, cw = (W - 70) / 16.f, chh = 20;
  for (int p = 0; p < 8; p++) {
    dl->AddText({o.x, o.y + float(p) * (chh + 3) + 2}, kPadCol[p], kPadNames[p]);
    for (int s = 0; s < 16; s++) {
      ImVec2 a{o.x + 64 + float(s) * cw, o.y + float(p) * (chh + 3)}, b{a.x + cw - 3, a.y + chh};
      bool on = (st.cur_steps()[size_t(p)].load() >> s) & 1;
      ImGui::SetCursorScreenPos(a);
      ImGui::PushID(p * 16 + s);
      if (ImGui::InvisibleButton("st", {cw - 3, chh})) st.cur_steps()[size_t(p)].fetch_xor(uint16_t(1u << s));
      ImGui::PopID();
      ImU32 c = on ? kPadCol[p] : (s % 4 == 0 ? IM_COL32(44, 44, 50, 255) : IM_COL32(34, 34, 40, 255));
      if (s == cur) c = on ? IM_COL32(255, 255, 255, 255) : IM_COL32(70, 70, 80, 255);
      dl->AddRectFilled(a, b, c, 4);
    }
  }
  ImGui::SetCursorScreenPos({o.x, o.y + 8 * (chh + 3) + 6});

  if (ImGui::CollapsingHeader("Euclid (4 canales)")) {
    if (ImGui::BeginTable("euc", 5, ImGuiTableFlags_SizingStretchSame)) {
      for (int c = 0; c < Euclid::C; c++) {
        auto& ch = st.cur_euclid().ch[size_t(c)];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImVec2 p = ImGui::GetCursorScreenPos();
        int len = ch.len->geti();
        for (int s = 0; s < 16; s++)
          dl->AddCircleFilled({p.x + 6 + float(s) * 9, p.y + 9}, (st.playing && len && cur >= 0 && int(st.step) % len == s) ? 4.5f : 3.f,
                              s >= len ? IM_COL32(30, 30, 34, 255)
                              : Euclid::hit(s, len, ch.hits->geti(), ch.rot->geti()) ? kEnc[size_t(c)] : kDim);
        ImGui::Dummy({150, 18});
        for (auto* prm : {ch.len, ch.hits, ch.rot, ch.pad}) {
          ImGui::TableNextColumn();
          slider(*prm);
        }
      }
      ImGui::EndTable();
    }
  }
  if (ImGui::CollapsingHeader("Arpegiador", ImGuiTreeNodeFlags_DefaultOpen))
    for (auto* id : {"arp.on", "arp.mode", "arp.oct", "arp.rate", "arp.gate"}) slider(*registry().find(id));
}

static void tab_fx(UI& u)
{
  auto& st = u.st;
  Param* fx[] = {st.reverb.mix, st.reverb.length, st.reverb.shape, st.reverb.shimmer, st.delay.time,
                 st.delay.fb, st.delay.mix, st.chorus.mix, st.chorus.rate, st.chorus.depth};
  const ImU32 cols[] = {kEnc[0], kEnc[0], kEnc[0], kEnc[0], kEnc[1], kEnc[1], kEnc[1], kEnc[3], kEnc[3], kEnc[3]};
  for (int i = 0; i < 10; i++) {
    if (i % 5) ImGui::SameLine(0, 12);
    knob(fx[i], cols[i], 30);
  }
  ImGui::TextDisabled("azul = Wormhole reverb (de OTTO)   verde = delay ping-pong   rojo = chorus");
}

static void tab_tape(UI& u)
{
  auto& st = u.st;
  for (int t = 0; t < Tape::T; t++) {
    ImGui::PushID(100 + t);
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, kEnc[size_t(t)]);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kEnc[size_t(t)]);
    float v = st.tape.vol[size_t(t)]->get();
    if (ImGui::VSliderFloat("##v", {50, 120}, &v, 0, 1, "")) st.tape.vol[size_t(t)]->set(v);
    ImGui::PopStyleColor(2);
    if (pill("REC", st.rec_track->geti() == t, kEnc[3], {50, 0})) st.rec_track->set(float(t));
    bool m = st.tape.mute[size_t(t)];
    if (pill("MUTE", m, kDim, {50, 0})) st.tape.mute[size_t(t)] = !m;
    if (ImGui::SmallButton("borrar")) st.ui_q.push({Ev::ClearTrack, t});
    ImGui::TextColored(ImColor(kEnc[size_t(t)]), " Pista %d", t + 1);
    ImGui::EndGroup();
    ImGui::PopID();
    ImGui::SameLine(0, 18);
  }
  ImGui::BeginGroup();
  knob(st.tape_speed, kEngineCol[2], 30, "Velocidad");
  ImGui::SameLine();
  ImGui::BeginGroup();
  if (pill("REVERSA", st.tape_rev->geti() != 0, kEngineCol[3], {110, 30})) st.tape_rev->set(st.tape_rev->geti() ? 0.f : 1.f);
  if (pill("TAPE STOP", st.tape_stopping, kEnc[3], {110, 30})) st.ui_q.push({Ev::TapeStop});
  if (ImGui::SmallButton("velocidad 1x")) st.tape_speed->set(1);
  ImGui::EndGroup();
  ImGui::SetNextItemWidth(260);
  slider(*st.loop_in);
  ImGui::SetNextItemWidth(260);
  slider(*st.loop_out);
  ImGui::EndGroup();
  ImGui::SameLine(0, 18);
  ImGui::BeginGroup();
  ImGui::TextDisabled("Proyecto (carpeta)");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##proj", u.proj, sizeof u.proj);
  if (ImGui::Button("Guardar")) {
    st.ui_q.push({Ev::Stop});
    u.status = st.save(u.proj) ? "Proyecto guardado" : "Error al guardar";
  }
  ImGui::SameLine();
  if (ImGui::Button("Cargar")) u.status = st.load(u.proj) ? "Proyecto cargado" : "No se pudo cargar";
  ImGui::SameLine();
  if (ImGui::Button("Exportar mezcla WAV")) {
    auto path = (fs::path(u.proj) / "mezcla.wav").string();
    u.status = st.export_mix(path) ? "Exportado: " + path : "Error al exportar";
  }
  if (!u.status.empty()) ImGui::TextWrapped("%s", u.status.c_str());
  ImGui::EndGroup();
}

static void tab_engine(UI& u)
{
  auto& st = u.st;
  Engine& eng = st.synth();
  if (eng.title == "SAMPLER") {
    ImGui::TextDisabled("WAV para el sampler (se toca en C4 = nota original)");
    ImGui::SetNextItemWidth(-120);
    ImGui::InputText("##wav", u.wav, sizeof u.wav);
    ImGui::SameLine();
    if (ImGui::Button("Cargar", {-1, 0})) u.status = sampler_load(eng, u.wav) ? "Sample cargado" : "No se pudo leer el audio";
    if (ImGui::Button("Abrir archivo...")) {
      auto path = open_audio_dialog();
      if (!path.empty()) {
        std::snprintf(u.wav, sizeof u.wav, "%s", path.c_str());
        u.status = sampler_load(eng, path) ? "Sample cargado" : "No se pudo leer el audio";
      }
    }
    ImGui::SameLine();
    if (!u.status.empty()) ImGui::TextDisabled("%s", u.status.c_str());
    ImGui::TextDisabled("tip: arrastra un wav/mp3/flac a la ventana");
  }
  // presets
  if (u.presets_for != eng.title) refresh_presets(u);
  ImGui::TextColored(ImColor(kEngineCol[st.synth_sel->geti() % 5]), "PRESETS %s", eng.title.c_str());
  ImGui::SetNextItemWidth(200);
  ImGui::InputText("##pname", u.preset_name, sizeof u.preset_name);
  ImGui::SameLine();
  if (ImGui::Button("Guardar preset") && u.preset_name[0])
    u.status = save_preset(u, u.preset_name) ? "Preset guardado" : "No se pudo guardar el preset";
  for (int i = 0; i < int(u.presets.size()); i++) {
    if (i % 6) ImGui::SameLine();
    if (pill(u.presets[size_t(i)].c_str(), i == u.preset_idx, kEngineCol[st.synth_sel->geti() % 5], {130, 0})) {
      u.preset_idx = i;
      load_preset(u, u.presets[size_t(i)]);
    }
  }
  if (u.presets.empty()) ImGui::TextDisabled("sin presets: ajusta el sonido, ponle nombre y pulsa Guardar preset");
  ImGui::Separator();
  for (auto* p : eng.params) slider(*p);
  slider(*st.mod);
  slider(*st.bend_range);
  slider(*st.drums.vol);
  slider(*st.drums.tune);
}

static void tab_io(UI& u)
{
  auto& audio = u.audio;
  auto& midi = u.midi;
  ImGui::SetNextItemWidth(130);
  if (ImGui::BeginCombo("API", RtAudio::getApiDisplayName(audio.apis[size_t(audio.api_i)]).c_str())) {
    for (int i = 0; i < int(audio.apis.size()); i++)
      if (ImGui::Selectable(RtAudio::getApiDisplayName(audio.apis[size_t(i)]).c_str(), i == audio.api_i)) {
        audio.api_i = i;
        audio.scan();
        if (audio.open()) audio.remember();
      }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(320);
  if (ImGui::BeginCombo("Salida", audio.names.empty() ? "-" : audio.names[size_t(audio.dev_i)].c_str())) {
    for (int i = 0; i < int(audio.names.size()); i++)
      if (ImGui::Selectable(audio.names[size_t(i)].c_str(), i == audio.dev_i)) {
        audio.dev_i = i;
        if (audio.open()) audio.remember();
      }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  static const unsigned sizes[] = {64, 128, 256, 512, 1024};
  int idx = 0;
  for (int i = 0; i < 5; i++)
    if (sizes[i] == audio.buffer) idx = i;
  ImGui::SetNextItemWidth(90);
  if (ImGui::Combo("Buffer", &idx, "64\0128\0256\0512\01024\0")) {
    audio.buffer = sizes[idx];
    if (audio.open()) audio.remember();
  }
  ImGui::TextDisabled("%s", audio.status.c_str());
  ImGui::TextDisabled("Menor latencia: API asio + FL Studio ASIO o ASIO4ALL, buffer 64-128. WASAPI suma ~10-20 ms.");
  ImGui::Separator();
  {
    std::lock_guard lk(midi.mtx);
    for (auto& p : midi.ports) {
      ImGui::BulletText("%s", p.c_str());
      ImGui::SameLine();
    }
    ImGui::NewLine();
    if (!midi.learn.empty()) {
      ImGui::TextColored(ImColor(IM_COL32(255, 170, 0, 255)), "Esperando control para: %s", midi.learn.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("cancelar")) midi.learn.clear();
    }
  }
  if (ImGui::Button("Reescanear MIDI")) midi.open_all();
  ImGui::SameLine();
  if (ImGui::Button("Guardar mapeo")) midi.save();
  ImGui::SameLine();
  static const char* actions[] = {"play", "stop", "rec", "clear", "page_next", "page_prev", "synth_next", "arp",
                                  "track1", "track2", "track3", "track4", "tapestop", "reverse", "note_rec",
                                  "preset_next", "preset_prev"};
  static int act = 0;
  ImGui::SetNextItemWidth(120);
  ImGui::Combo("##act", &act, actions, IM_ARRAYSIZE(actions));
  ImGui::SameLine();
  if (ImGui::Button("Learn accion")) {
    std::lock_guard lk(midi.mtx);
    midi.learn = std::string("action:") + actions[act];
  }
  ImGui::BeginChild("log", {0, 0}, ImGuiChildFlags_Border);
  {
    std::lock_guard lk(midi.mtx);
    for (auto& l : midi.log) ImGui::TextUnformatted(l.c_str());
  }
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}

// ---------------------------------------------------------------- main
static UI* g_ui = nullptr;

/// --shot <file.bmp>: render ~3 s, save the window as 24-bit BMP and quit (UI self-check)
static void save_bmp(const char* path, int w, int h)
{
  std::vector<unsigned char> px(size_t(w) * size_t(h) * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, px.data());
  int row = (w * 3 + 3) & ~3;
  uint32_t size = 54 + uint32_t(row * h);
  unsigned char hd[54] = {'B', 'M'};
  auto put32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) hd[o + i] = (unsigned char)(v >> (8 * i)); };
  put32(2, size);
  put32(10, 54);
  put32(14, 40);
  put32(18, uint32_t(w));
  put32(22, uint32_t(h));
  hd[26] = 1;
  hd[28] = 24;
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  std::fwrite(hd, 1, 54, f);
  std::vector<unsigned char> pad(size_t(row - w * 3), 0);
  for (int y = 0; y < h; y++) {
    std::fwrite(&px[size_t(y) * size_t(w) * 3], 1, size_t(w) * 3, f);
    std::fwrite(pad.data(), 1, pad.size(), f);
  }
  std::fclose(f);
}

int main(int argc, char** argv)
{
  fs::path base = fs::absolute(argv[0]).parent_path();
  const char* shot = (argc > 2 && std::string(argv[1]) == "--shot") ? argv[2] : nullptr;

  gam::sampleRate(kSR);
  auto st = std::make_unique<Studio>();
  st->drums.load_dir((base / "samples").string());
  if (fs::exists(base / "samples" / "sampler.wav")) sampler_load(*st->synths.back(), (base / "samples" / "sampler.wav").string());

  Midi midi(*st);
  g_midi = &midi;
  midi.load((base / "mapping.json").string());
  midi.open_all();

  Audio audio;
  audio.st = st.get();
  audio.start((base / "audio.json").string());
  std::printf("audio: %s [%s]\n", audio.status.c_str(), RtAudio::getApiName(audio.apis[size_t(audio.api_i)]).c_str());
  std::fflush(stdout);

  if (!glfwInit()) return 1;
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  GLFWwindow* win = glfwCreateWindow(1440, 920, "otto-op1", nullptr, nullptr);
  if (!win) return 1;
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);
  ImGui::CreateContext();
  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  // TE-ish typography (Bahnschrift ships with Windows 10/11); falls back to ImGui default
  const char* font = "C:\\Windows\\Fonts\\bahnschrift.ttf";
  if (fs::exists(font)) {
    io.Fonts->AddFontFromFileTTF(font, 17.f);
    g_big = io.Fonts->AddFontFromFileTTF(font, 32.f);
  } else g_big = io.Fonts->AddFontDefault();

  ImGui::StyleColorsDark();
  auto& style = ImGui::GetStyle();
  style.FrameRounding = 8;
  style.GrabRounding = 8;
  style.ChildRounding = 12;
  style.WindowRounding = 0;
  style.TabRounding = 10;
  style.FramePadding = {10, 6};
  style.ItemSpacing = {8, 7};
  auto* C = style.Colors;
  C[ImGuiCol_WindowBg] = ImVec4(0.067f, 0.067f, 0.078f, 1);
  C[ImGuiCol_ChildBg] = ImGui::ColorConvertU32ToFloat4(kPanel);
  C[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.16f, 1);
  C[ImGuiCol_FrameBgHovered] = ImVec4(0.2f, 0.2f, 0.23f, 1);
  C[ImGuiCol_Button] = ImVec4(0.16f, 0.16f, 0.19f, 1);
  C[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.25f, 0.3f, 1);
  C[ImGuiCol_SliderGrab] = ImGui::ColorConvertU32ToFloat4(kEnc[0]);
  C[ImGuiCol_SliderGrabActive] = ImGui::ColorConvertU32ToFloat4(kEnc[1]);
  C[ImGuiCol_Tab] = ImVec4(0.12f, 0.12f, 0.14f, 1);
  C[ImGuiCol_TabSelected] = ImGui::ColorConvertU32ToFloat4(scale_col(kEnc[0], 0.7f));
  C[ImGuiCol_TabHovered] = ImGui::ColorConvertU32ToFloat4(kEnc[0]);
  C[ImGuiCol_Header] = ImVec4(0.16f, 0.16f, 0.19f, 1);
  C[ImGuiCol_CheckMark] = ImGui::ColorConvertU32ToFloat4(kEnc[1]);
  ImGui_ImplGlfw_InitForOpenGL(win, true);
  ImGui_ImplOpenGL3_Init("#version 130");

  UI ui{*st, midi, audio, base};
  g_ui = &ui;
  std::snprintf(ui.proj, sizeof ui.proj, "%s", (base / "proyectos" / "proyecto1").string().c_str());
  std::snprintf(ui.wav, sizeof ui.wav, "%s", (base / "samples" / "sampler.wav").string().c_str());
  // drag & drop a .wav onto the window -> sampler
  glfwSetDropCallback(win, [](GLFWwindow* w, int n, const char** paths) {
    if (n < 1 || !g_ui) return;
    double mx, my;
    glfwGetCursorPos(w, &mx, &my);
    for (int i = 0; i < 8; i++) {
      auto r = g_ui->pad_rect[size_t(i)];
      if (mx >= r.x && mx < r.z && my >= r.y && my < r.w) {
        g_ui->status = g_ui->st.drums.load_pad(i, paths[0]) ? "Pad cargado (arrastrado)" : "No se pudo leer el audio";
        return;
      }
    }
    std::snprintf(g_ui->wav, sizeof g_ui->wav, "%s", paths[0]);
    g_ui->st.synth_sel->set(float(g_ui->st.synths.size() - 1));
    g_ui->status = sampler_load(*g_ui->st.synths.back(), paths[0]) ? "Sample cargado (arrastrado)" : "No se pudo leer el audio";
  });

  const ImGuiKey piano[] = {ImGuiKey_A, ImGuiKey_W, ImGuiKey_S, ImGuiKey_E, ImGuiKey_D, ImGuiKey_F, ImGuiKey_T, ImGuiKey_G,
                            ImGuiKey_Y, ImGuiKey_H, ImGuiKey_U, ImGuiKey_J, ImGuiKey_K, ImGuiKey_O, ImGuiKey_L};

  while (!glfwWindowShouldClose(win)) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    float dt = io.DeltaTime;
    ui.time += dt;
    if (st->playing) ui.reel += dt * 2.5f;
    ui.energy = ui.energy * 0.85f + std::min(1.f, st->peak * 1.5f) * 0.15f;
    for (auto& f : ui.flash) f *= std::pow(0.02f, dt); // ~0.5 s fade

    if (!io.WantTextInput) {
      for (int i = 0; i < 15; i++) {
        if (ImGui::IsKeyPressed(piano[i], false)) st->ui_q.push({Ev::NoteOn, ui.octave + 12 + i, 0.8f});
        if (ImGui::IsKeyReleased(piano[i])) st->ui_q.push({Ev::NoteOff, ui.octave + 12 + i});
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { st->ui_q.push({Ev::AllOff}); ui.octave = std::max(12, ui.octave - 12); }
      if (ImGui::IsKeyPressed(ImGuiKey_X, false)) { st->ui_q.push({Ev::AllOff}); ui.octave = std::min(96, ui.octave + 12); }
      if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) st->ui_q.push({Ev::TogglePlay});
      if (ImGui::IsKeyPressed(ImGuiKey_R, false)) st->ui_q.push({Ev::ToggleRec});
      if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) midi.action("page_next");
      if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) midi.action("page_prev");
      for (int i = 0; i < 5; i++)
        if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_1 + i), false)) { st->synth_sel->set(float(i)); st->page = 0; }
    }

    int w, h;
    glfwGetFramebufferSize(win, &w, &h);
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({float(w), float(h)});
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (int nav = st->preset_nav.exchange(0); nav != 0) {
      if (ui.presets_for != st->synth().title) refresh_presets(ui);
      if (!ui.presets.empty()) {
        int n = int(ui.presets.size());
        ui.preset_idx = ((ui.preset_idx + nav) % n + n) % n;
        load_preset(ui, ui.presets[size_t(ui.preset_idx)]);
      }
    }
    header(ui);
    screen(ui, std::clamp(float(h) * 0.30f, 220.f, 320.f));
    ImGui::Spacing();
    controls(ui);
    ImGui::Spacing();

    float W = ImGui::GetContentRegionAvail().x;
    keyboard(ui, {W * 0.62f, 120});
    ImGui::SameLine(0, 16);
    pads(ui, {W * 0.38f - 16, 120});
    ImGui::TextDisabled("teclado PC: A-L notas  Z/X octava  1-5 motor  espacio play  R rec  RePag/AvPag pagina  | arrastra audio: a un pad o al sampler");

    ImGui::BeginChild("tabs", {0, 0}, ImGuiChildFlags_Border);
    if (ImGui::BeginTabBar("t")) {
      if (ImGui::BeginTabItem("SECUENCIA")) { tab_sequence(ui); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("FX")) { tab_fx(ui); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("CINTA")) { tab_tape(ui); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("MOTOR")) { tab_engine(ui); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("AUDIO / MIDI")) { tab_io(ui); ImGui::EndTabItem(); }
      ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::End();
    ImGui::Render();
    glViewport(0, 0, w, h);
    glClearColor(0.067f, 0.067f, 0.078f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (shot && ui.time > 3.f) {
      save_bmp(shot, w, h);
      glfwSetWindowShouldClose(win, 1);
    }
    glfwSwapBuffers(win);
    if (shot && ui.time > 0.5f && ui.time < 0.6f) { // demo activity for the screenshot
      if (argc > 3) st->synth_sel->set(float(std::atoi(argv[3])));
      if (argc > 4) st->page = std::atoi(argv[4]);
      st->ui_q.push({Ev::Play});
      st->ui_q.push({Ev::NoteOn, 60, 0.9f});
      st->ui_q.push({Ev::Drum, 0, 0.9f});
      st->cur_steps()[2] = 0x5555;
    }
  }

  audio.close();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
