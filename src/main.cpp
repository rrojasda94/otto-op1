// otto-op1: OP-1 style groovebox for Windows, controlled by an Arturia MiniLab 3.
#include "core.hpp"
#include "midi.hpp"

#include <Gamma/Domain.h>
#include <GLFW/glfw3.h>
#include <RtAudio.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <filesystem>

using namespace otto;

// OTTO encoder colours
static const ImU32 kEnc[4] = {IM_COL32(58, 134, 255, 255), IM_COL32(56, 190, 80, 255), IM_COL32(245, 245, 245, 255),
                              IM_COL32(255, 70, 60, 255)};

// ---------------------------------------------------------------- audio
struct Audio {
  Studio* st = nullptr;
  std::unique_ptr<RtAudio> dac;
  std::vector<RtAudio::Api> apis;
  int api_i = 0;
  std::vector<unsigned> ids;
  std::vector<std::string> names;
  int dev_i = 0;
  unsigned buffer = 256;
  std::string status;

  static int cb(void* out, void*, unsigned n, double, RtAudioStreamStatus, void* user)
  {
    static_cast<Studio*>(user)->process(static_cast<float*>(out), n);
    return 0;
  }

  void init()
  {
    RtAudio::getCompiledApi(apis);
    for (size_t i = 0; i < apis.size(); i++)
      if (apis[i] == RtAudio::WINDOWS_WASAPI) api_i = int(i);
    scan();
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
    char b[128];
    std::snprintf(b, sizeof b, "%s | buffer %u (%.1f ms)", names[size_t(dev_i)].c_str(), bf, bf * 1000.f / kSR);
    status = b;
    return true;
  }
  void close()
  {
    if (dac && dac->isStreamOpen()) dac->closeStream();
  }
};

// ---------------------------------------------------------------- widgets
static Midi* g_midi = nullptr;

static void learn_button(Param& p)
{
  ImGui::PushID(&p);
  bool active = g_midi->learn == p.id;
  if (active) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 120, 0, 255));
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
    if (ImGui::SliderInt(p.name.c_str(), &v, int(p.min), int(p.max), (p.name + ": %d").c_str())) p.set(float(v));
  } else {
    float v = p.get();
    if (ImGui::SliderFloat(p.name.c_str(), &v, p.min, p.max, (p.name + ": %.2f").c_str())) p.set(v);
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) p.set(p.def);
  ImGui::PopID();
}

/// OP-1 style knob: drag vertically, right click resets
static void knob(Param* p, ImU32 col, float r = 34)
{
  ImGui::BeginGroup();
  ImVec2 c0 = ImGui::GetCursorScreenPos();
  ImGui::PushID(p ? (void*)p : (void*)&col);
  ImGui::InvisibleButton("k", ImVec2(r * 2 + 16, r * 2 + 8));
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 c(c0.x + r + 8, c0.y + r + 4);
  dl->AddCircleFilled(c, r, IM_COL32(40, 40, 44, 255), 40);
  if (p) {
    if (ImGui::IsItemActive()) p->set_norm(p->norm() - ImGui::GetIO().MouseDelta.y * 0.005f);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) p->set(p->def);
    float a0 = 0.75f * kPi, a1 = a0 + 1.5f * kPi * p->norm();
    dl->PathArcTo(c, r - 3, a0, a0 + 1.5f * kPi, 40);
    dl->PathStroke(IM_COL32(70, 70, 76, 255), 0, 5);
    dl->PathArcTo(c, r - 3, a0, a1, 40);
    dl->PathStroke(col, 0, 5);
    dl->AddCircleFilled(ImVec2(c.x + std::cos(a1) * (r - 12), c.y + std::sin(a1) * (r - 12)), 5, col);
  } else {
    dl->AddCircle(c, r - 3, IM_COL32(70, 70, 76, 255), 40, 2);
  }
  ImGui::PopID();
  if (p) {
    char b[64];
    std::snprintf(b, sizeof b, p->integer ? "%.0f" : "%.2f", p->get());
    ImGui::TextUnformatted(p->name.c_str());
    ImGui::TextColored(ImColor(col), "%s", b);
  } else {
    ImGui::TextDisabled("-");
    ImGui::NewLine();
  }
  ImGui::EndGroup();
}

// ---------------------------------------------------------------- OTTO-style screens
/// FM algorithm graph: edges (modulator -> target) and carriers, matching engines.cpp switch
struct Alg {
  std::vector<std::pair<int, int>> edges;
  std::vector<int> carriers;
};
static const Alg kAlgs[11] = {
  {{{3, 2}, {2, 1}, {1, 0}}, {0}},         {{{3, 1}, {2, 1}, {1, 0}}, {0}},
  {{{3, 0}, {2, 1}, {1, 0}}, {0}},         {{{3, 1}, {3, 2}, {1, 0}, {2, 0}}, {0}},
  {{{3, 2}, {2, 1}, {2, 0}}, {0, 1}},      {{{3, 2}, {2, 1}}, {0, 1}},
  {{{1, 0}, {2, 0}, {3, 0}}, {0}},         {{{1, 0}, {3, 2}}, {0, 2}},
  {{{3, 0}, {3, 1}, {3, 2}}, {0, 1, 2}},   {{{3, 2}}, {0, 1, 2}},
  {{}, {0, 1, 2, 3}},
};

static void draw_fm_algorithm(int alg, float hgt)
{
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float W = ImGui::GetContentRegionAvail().x;
  ImGui::Dummy({W, hgt});
  dl->AddRectFilled(o, {o.x + W, o.y + hgt}, IM_COL32(25, 25, 28, 255), 8);
  const auto& a = kAlgs[std::clamp(alg, 0, 10)];
  // row = distance from output (carriers at the bottom); ops in the same row spread horizontally
  int level[4] = {0, 0, 0, 0};
  for (int pass = 0; pass < 4; pass++)
    for (auto [m, t] : a.edges) level[m] = std::max(level[m], level[t] + 1);
  float cx = o.x + W * 0.5f, step = (hgt - 50) / 3.f;
  auto pos = [&](int op) {
    int n = 0, idx = 0;
    for (int k = 0; k < 4; k++)
      if (level[k] == level[op]) { if (k < op) idx++; n++; }
    return ImVec2(cx + (float(idx) - (n - 1) * 0.5f) * 60, o.y + hgt - 30 - level[op] * step);
  };
  for (auto [m, t] : a.edges) dl->AddLine(pos(m), pos(t), IM_COL32(150, 150, 160, 255), 2);
  for (int op = 0; op < 4; op++) {
    bool car = std::find(a.carriers.begin(), a.carriers.end(), op) != a.carriers.end();
    ImVec2 p = pos(op);
    dl->AddCircleFilled(p, 14, car ? kEnc[0] : IM_COL32(70, 70, 76, 255));
    char b[4] = {char('1' + op), 0};
    dl->AddText({p.x - 4, p.y - 7}, IM_COL32(255, 255, 255, 255), b);
  }
  char t[32];
  std::snprintf(t, sizeof t, "ALG %d", alg + 1);
  dl->AddText({o.x + 8, o.y + 6}, kEnc[0], t);
  dl->AddText({o.x + 8, o.y + hgt - 18}, IM_COL32(120, 120, 130, 255), "azul = portadora");
}

/// ADSR curve; values 0..1, durations mapped like OTTO (5*d^2)
static void draw_envelope(float a, float d, float s, float r, ImU32 col, float hgt)
{
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float W = ImGui::GetContentRegionAvail().x;
  ImGui::Dummy({W, hgt});
  dl->AddRectFilled(o, {o.x + W, o.y + hgt}, IM_COL32(25, 25, 28, 255), 8);
  auto dur = [](float x) { return 5 * x * x + 0.001f; };
  float ta = dur(a), td = dur(d), tr = dur(r), hold = 0.5f;
  float total = ta + td + hold + tr, sx = (W - 20) / total, y0 = o.y + hgt - 10, yh = hgt - 20;
  ImVec2 p0{o.x + 10, y0}, p1{p0.x + ta * sx, y0 - yh}, p2{p1.x + td * sx, y0 - yh * s}, p3{p2.x + hold * sx, p2.y},
    p4{p3.x + tr * sx, y0};
  ImVec2 pts[5] = {p0, p1, p2, p3, p4};
  dl->AddPolyline(pts, 5, col, 0, 3);
  for (auto& p : pts) dl->AddCircleFilled(p, 4, col);
}

/// Mini waveform of a tape track (peak per column, strided sampling)
static void draw_tape(const std::vector<float>& trk, size_t frames, float play_frac, ImU32 col, ImVec2 size)
{
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  dl->AddRectFilled(o, {o.x + size.x, o.y + size.y}, IM_COL32(25, 25, 28, 255), 4);
  int cols = int(size.x);
  float mid = o.y + size.y * 0.5f;
  for (int c = 0; c < cols && frames > 0; c++) {
    size_t f0 = frames * size_t(c) / size_t(cols), f1 = frames * size_t(c + 1) / size_t(cols);
    float pk = 0;
    for (size_t f = f0; f < f1; f += 97) pk = std::max(pk, std::abs(trk[f * 2])); // ponytail: strided peak, cheap enough per frame
    pk = std::min(pk, 1.f) * size.y * 0.5f;
    if (pk > 0.5f) dl->AddLine({o.x + c, mid - pk}, {o.x + c, mid + pk}, col);
  }
  float x = o.x + play_frac * size.x;
  dl->AddLine({x, o.y}, {x, o.y + size.y}, IM_COL32(255, 255, 255, 160));
}

static bool colored_button(const char* label, bool on, ImU32 col, ImVec2 size = {0, 0})
{
  if (on) ImGui::PushStyleColor(ImGuiCol_Button, col);
  bool r = ImGui::Button(label, size);
  if (on) ImGui::PopStyleColor();
  return r;
}

// ---------------------------------------------------------------- main
int main(int, char** argv)
{
  namespace fs = std::filesystem;
  fs::path base = fs::absolute(argv[0]).parent_path();

  gam::sampleRate(kSR);
  auto st = std::make_unique<Studio>();
  st->drums.load_dir((base / "samples").string());

  Midi midi(*st);
  g_midi = &midi;
  midi.load((base / "mapping.json").string());
  midi.open_all();

  Audio audio;
  audio.st = st.get();
  audio.init();
  audio.open();

  if (!glfwInit()) return 1;
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  GLFWwindow* win = glfwCreateWindow(1280, 820, "otto-op1", nullptr, nullptr);
  if (!win) return 1;
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::StyleColorsDark();
  auto& style = ImGui::GetStyle();
  style.FrameRounding = 6;
  style.WindowRounding = 0;
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.10f, 1);
  ImGui_ImplGlfw_InitForOpenGL(win, true);
  ImGui_ImplOpenGL3_Init("#version 130");

  char proj[260];
  std::snprintf(proj, sizeof proj, "%s", (base / "proyectos" / "proyecto1").string().c_str());
  std::string proj_status;
  int octave = 60;
  const ImGuiKey piano[] = {ImGuiKey_A, ImGuiKey_W, ImGuiKey_S, ImGuiKey_E, ImGuiKey_D, ImGuiKey_F, ImGuiKey_T, ImGuiKey_G,
                            ImGuiKey_Y, ImGuiKey_H, ImGuiKey_U, ImGuiKey_J, ImGuiKey_K, ImGuiKey_O, ImGuiKey_L};
  auto& R = registry();

  while (!glfwWindowShouldClose(win)) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // PC keyboard as a piano (A..L), Z/X octave, space play, R rec
    if (!ImGui::GetIO().WantTextInput) {
      for (int i = 0; i < 15; i++) {
        if (ImGui::IsKeyPressed(piano[i], false)) st->ui_q.push({Ev::NoteOn, octave + i, 0.8f});
        if (ImGui::IsKeyReleased(piano[i])) st->ui_q.push({Ev::NoteOff, octave + i});
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { st->ui_q.push({Ev::AllOff}); octave = std::max(24, octave - 12); }
      if (ImGui::IsKeyPressed(ImGuiKey_X, false)) { st->ui_q.push({Ev::AllOff}); octave = std::min(96, octave + 12); }
      if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) st->ui_q.push({Ev::TogglePlay});
      if (ImGui::IsKeyPressed(ImGuiKey_R, false)) st->ui_q.push({Ev::ToggleRec});
      if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) midi.action("page_next");
      if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) midi.action("page_prev");
    }

    int w, h;
    glfwGetFramebufferSize(win, &w, &h);
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({float(w), float(h)});
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ---- top bar: engine + transport
    Engine& eng = st->synth();
    for (int i = 0; i < int(st->synths.size()); i++) {
      if (i) ImGui::SameLine();
      if (colored_button(st->synths[size_t(i)]->title.c_str(), st->synth_sel->geti() == i, kEnc[0], {110, 32})) {
        st->synth_sel->set(float(i));
        st->page = 0;
      }
    }
    ImGui::SameLine(0, 30);
    bool playing = st->playing, rec = st->recording;
    if (colored_button(playing ? "PAUSA" : "PLAY", playing, kEnc[1], {80, 32})) st->ui_q.push({Ev::TogglePlay});
    ImGui::SameLine();
    if (ImGui::Button("STOP", {70, 32})) st->ui_q.push({Ev::Stop});
    ImGui::SameLine();
    if (colored_button("REC", rec, kEnc[3], {70, 32})) st->ui_q.push({Ev::ToggleRec});
    ImGui::SameLine(0, 20);
    ImGui::SetNextItemWidth(110);
    {
      int b = st->bpm->geti();
      if (ImGui::DragInt("BPM", &b, 0.5f, 40, 240)) st->bpm->set(float(b));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    {
      int b = st->bars->geti();
      if (ImGui::SliderInt("Compases", &b, 1, 16)) st->bars->set(float(b));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    {
      float v = st->master->get();
      if (ImGui::SliderFloat("Master", &v, 0, 1)) st->master->set(v);
    }
    ImGui::SameLine();
    float pk = st->peak;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, pk > 0.95f ? kEnc[3] : kEnc[1]);
    ImGui::ProgressBar(pk, {80, 14}, "");
    ImGui::PopStyleColor();

    // tape position bar
    size_t lf = std::max<size_t>(1, st->loop_frames);
    float frac = float(st->tape_pos) / float(lf);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, rec ? kEnc[3] : kEnc[0]);
    char posb[64];
    std::snprintf(posb, sizeof posb, "cinta %.1f / %.1f s   paso %d", st->tape_pos / kSR, lf / kSR, std::max(0, int(st->step)) % 16 + 1);
    ImGui::ProgressBar(frac, {-1, 10}, "");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("%s   |   teclado PC: A-L notas, Z/X octava (%d), espacio play, R rec, RePag/AvPag pagina", posb, octave);
    ImGui::Separator();

    float colw = (float(w) - 40) / 3.f;

    // ---- column 1: synth
    ImGui::BeginChild("synth", {colw, 0}, ImGuiChildFlags_Border);
    ImGui::TextColored(ImColor(kEnc[0]), "%s", eng.title.c_str());
    int page = std::clamp(int(st->page), 0, int(eng.pages.size()) - 1);
    for (int i = 0; i < int(eng.pages.size()); i++) {
      if (i % 4) ImGui::SameLine();
      if (colored_button(eng.pages[size_t(i)].c_str(), i == page, IM_COL32(90, 90, 100, 255), {colw / 4 - 12, 0})) st->page = i;
    }
    ImGui::Spacing();
    for (int k = 0; k < 4; k++) {
      if (k) ImGui::SameLine();
      knob(eng.page_param(page, k), kEnc[k]);
    }
    ImGui::TextDisabled("Encoders 1-4 del MiniLab = estas 4 perillas");
    if (auto* p0 = eng.page_param(page, 0); p0 && p0->name.find("Attack") != std::string::npos) {
      auto* p3 = eng.page_param(page, 3);
      bool has_r = p3 && p3->name.find("Release") != std::string::npos;
      draw_envelope(p0->get(), eng.page_param(page, 1)->get(), eng.page_param(page, 2)->get(), has_r ? p3->get() : 0.f,
                    kEnc[1], 90);
    }
    if (eng.title == "OTTO.FM") draw_fm_algorithm(R.find("fm.alg")->geti(), 170);
    if (ImGui::CollapsingHeader("Todos los parametros del motor")) {
      for (auto* p : eng.params) slider(*p);
      slider(*st->mod);
      slider(*st->bend_range);
    }
    if (ImGui::CollapsingHeader("Arpegiador", ImGuiTreeNodeFlags_DefaultOpen)) {
      for (auto* id : {"arp.on", "arp.mode", "arp.oct", "arp.rate", "arp.gate"}) slider(*R.find(id));
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- column 2: drums + euclid
    ImGui::BeginChild("drums", {colw, 0}, ImGuiChildFlags_Border);
    ImGui::TextColored(ImColor(kEnc[1]), "DRUMS");
    static const char* pad_names[8] = {"KICK", "SNARE", "HAT C", "HAT O", "CLAP", "TOM L", "TOM H", "COW"};
    for (int i = 0; i < 8; i++) {
      if (i % 4) ImGui::SameLine();
      ImGui::Button(pad_names[i], {colw / 4 - 12, 48});
      if (ImGui::IsItemActivated()) st->ui_q.push({Ev::Drum, i, 0.9f});
    }
    slider(*st->drums.vol);
    slider(*st->drums.tune);
    ImGui::Spacing();
    ImGui::TextColored(ImColor(kEnc[1]), "EUCLID (suena con PLAY)");
    int cur = std::max(0, int(st->step));
    for (int c = 0; c < Euclid::C; c++) {
      auto& ch = st->euclid.ch[size_t(c)];
      ImGui::PushID(c);
      auto* dl = ImGui::GetWindowDrawList();
      ImVec2 o = ImGui::GetCursorScreenPos();
      int len = ch.len->geti();
      for (int s = 0; s < 16; s++) {
        bool in = s < len;
        bool hit = in && Euclid::hit(s, len, ch.hits->geti(), ch.rot->geti());
        bool now = st->playing && in && (cur % len) == s;
        ImVec2 cc(o.x + 10 + s * (colw - 30) / 16, o.y + 9);
        ImU32 col = !in ? IM_COL32(35, 35, 38, 255) : hit ? kEnc[size_t(c)] : IM_COL32(70, 70, 76, 255);
        dl->AddCircleFilled(cc, now ? 8.f : 6.f, col);
      }
      ImGui::Dummy({colw - 20, 18});
      slider(*ch.len);
      slider(*ch.hits);
      slider(*ch.rot);
      slider(*ch.pad);
      ImGui::PopID();
      ImGui::Separator();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- column 3: fx, tape, midi
    ImGui::BeginChild("right", {0, 0}, ImGuiChildFlags_Border);
    ImGui::TextColored(ImColor(kEnc[3]), "FX  (encoders 5-8)");
    knob(st->reverb.mix, kEnc[0], 26);
    ImGui::SameLine();
    knob(st->reverb.length, kEnc[1], 26);
    ImGui::SameLine();
    knob(st->reverb.shape, kEnc[2], 26);
    ImGui::SameLine();
    knob(st->delay.mix, kEnc[3], 26);
    if (ImGui::CollapsingHeader("Mas FX")) {
      for (auto* p : {st->reverb.shimmer, st->delay.time, st->delay.fb, st->chorus.mix, st->chorus.rate, st->chorus.depth})
        slider(*p);
    }

    ImGui::Separator();
    ImGui::TextColored(ImColor(kEnc[3]), "CINTA 4 PISTAS  (faders 1-4)");
    for (int t = 0; t < Tape::T; t++) {
      ImGui::PushID(100 + t);
      ImGui::BeginGroup();
      float v = st->tape.vol[size_t(t)]->get();
      if (ImGui::VSliderFloat("##v", {44, 110}, &v, 0, 1, "")) st->tape.vol[size_t(t)]->set(v);
      if (ImGui::RadioButton("rec", st->rec_track->geti() == t)) st->rec_track->set(float(t));
      bool m = st->tape.mute[size_t(t)];
      if (ImGui::Checkbox("mute", &m)) st->tape.mute[size_t(t)] = m;
      if (ImGui::SmallButton("borrar")) st->ui_q.push({Ev::ClearTrack, t});
      ImGui::Text("Pista %d", t + 1);
      ImGui::EndGroup();
      ImGui::PopID();
      ImGui::SameLine(0, 14);
    }
    ImGui::NewLine();
    for (int t = 0; t < Tape::T; t++) {
      bool armed = st->rec_track->geti() == t;
      draw_tape(st->tape.trk[size_t(t)], lf, frac, armed && rec ? kEnc[3] : kEnc[size_t(t)],
                {ImGui::GetContentRegionAvail().x, 26});
    }
    ImGui::SetNextItemWidth(-150);
    ImGui::InputText("##proj", proj, sizeof proj);
    ImGui::SameLine();
    if (ImGui::Button("Guardar")) {
      st->ui_q.push({Ev::Stop});
      proj_status = st->save(proj) ? "Guardado" : "Error al guardar";
    }
    ImGui::SameLine();
    if (ImGui::Button("Cargar")) proj_status = st->load(proj) ? "Cargado" : "No se pudo cargar";
    if (!proj_status.empty()) ImGui::TextDisabled("%s", proj_status.c_str());

    ImGui::Separator();
    ImGui::TextColored(ImColor(kEnc[2]), "AUDIO");
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("API", RtAudio::getApiDisplayName(audio.apis[size_t(audio.api_i)]).c_str())) {
      for (int i = 0; i < int(audio.apis.size()); i++)
        if (ImGui::Selectable(RtAudio::getApiDisplayName(audio.apis[size_t(i)]).c_str(), i == audio.api_i)) {
          audio.api_i = i;
          audio.scan();
          audio.open();
        }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##dev", audio.names.empty() ? "-" : audio.names[size_t(audio.dev_i)].c_str())) {
      for (int i = 0; i < int(audio.names.size()); i++)
        if (ImGui::Selectable(audio.names[size_t(i)].c_str(), i == audio.dev_i)) {
          audio.dev_i = i;
          audio.open();
        }
      ImGui::EndCombo();
    }
    {
      static const unsigned sizes[] = {64, 128, 256, 512, 1024};
      int idx = 0;
      for (int i = 0; i < 5; i++)
        if (sizes[i] == audio.buffer) idx = i;
      ImGui::SetNextItemWidth(100);
      if (ImGui::Combo("Buffer", &idx, "64\0128\0256\0512\01024\0")) {
        audio.buffer = sizes[idx];
        audio.open();
      }
    }
    ImGui::TextWrapped("%s", audio.status.c_str());

    ImGui::Separator();
    ImGui::TextColored(ImColor(kEnc[2]), "MIDI");
    {
      std::lock_guard lk(midi.mtx);
      for (auto& p : midi.ports) ImGui::BulletText("%s", p.c_str());
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
                                    "track1", "track2", "track3", "track4"};
    static int act = 0;
    ImGui::SetNextItemWidth(110);
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
    ImGui::EndChild();

    ImGui::End();
    ImGui::Render();
    glViewport(0, 0, w, h);
    glClearColor(0.09f, 0.09f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(win);
  }

  audio.close();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
