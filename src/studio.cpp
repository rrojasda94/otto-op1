// Studio: routes events, runs synth + drums + FX + sequencers + 4-track tape.
#include "core.hpp"

#include <dr_wav.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

namespace otto {

  // ---------------- Arp ----------------
  Arp::Arp()
  {
    auto& R = registry();
    on = &R.add("arp.on", "Arp On", 0, 1, 0, 0, true);
    mode = &R.add("arp.mode", "Arp Modo (up/down/updown/random/orden)", 0, 4, 0, 0, true);
    oct = &R.add("arp.oct", "Arp Octavas", 1, 3, 1, 0, true);
    rate = &R.add("arp.rate", "Arp Div (1/4 1/8 1/16 1/32)", 0, 3, 2, 0, true);
    gate = &R.add("arp.gate", "Arp Gate", 0.05f, 1, 0.5f);
    held.reserve(32);
  }

  void Arp::press(int n)
  {
    if (std::find(held.begin(), held.end(), n) == held.end() && held.size() < 32) held.push_back(n);
  }
  void Arp::release(int n) { std::erase(held, n); }

  void Arp::tick(Engine& e, float bpm)
  {
    double step = kSR * 60.0 / bpm / double(1 << rate->geti());
    phase += 1;
    if (sounding >= 0 && phase >= off_at) {
      e.note_off(sounding);
      sounding = -1;
    }
    if (phase < step) return;
    phase = 0;
    if (held.empty()) { idx = 0; return; }

    std::array<int, 192> seq{}; // 32 notes * 3 oct, doubled for updown
    size_t n = 0;
    int m = mode->geti();
    std::array<int, 32> base{};
    size_t hb = held.size();
    std::copy(held.begin(), held.end(), base.begin());
    if (m != 4) std::sort(base.begin(), base.begin() + long(hb));
    for (int o = 0; o < oct->geti(); o++)
      for (size_t i = 0; i < hb; i++) seq[n++] = base[i] + 12 * o;
    if (m == 1) std::reverse(seq.begin(), seq.begin() + long(n));
    if (m == 2 && n > 2) // up then down, without repeating the ends
      for (size_t i = n - 2; i >= 1; i--) seq[n++] = seq[i];
    int note = m == 3 ? seq[size_t(std::rand()) % n] : seq[size_t(idx) % n];
    idx++;
    if (sounding >= 0) e.note_off(sounding);
    e.note_on(note, 0.8f);
    sounding = note;
    off_at = step * gate->get();
  }

  // ---------------- Euclid ----------------
  Euclid::Euclid(const std::string& prefix)
  {
    auto& R = registry();
    const int def_pad[C] = {0, 1, 2, 3};
    for (int i = 0; i < C; i++) {
      auto id = prefix + "euc" + std::to_string(i + 1) + ".";
      auto nm = "Euclid " + std::to_string(i + 1) + " ";
      ch[size_t(i)].len = &R.add(id + "len", nm + "Largo", 1, 16, 16, 0, true);
      ch[size_t(i)].hits = &R.add(id + "hits", nm + "Golpes", 0, 16, 0, 0, true); // silent until the user dials hits
      ch[size_t(i)].rot = &R.add(id + "rot", nm + "Rotacion", 0, 15, 0, 0, true);
      ch[size_t(i)].pad = &R.add(id + "pad", nm + "Pad", 0, 7, float(def_pad[i]), 0, true);
    }
  }

  // ---------------- Tape ----------------
  Tape::Tape()
  {
    for (int t = 0; t < T; t++) {
      trk[size_t(t)].assign(kMaxFrames * 2, 0.f);
      vol[size_t(t)] = &registry().add("tape.vol" + std::to_string(t + 1), "Pista " + std::to_string(t + 1), 0, 1, 0.8f);
    }
  }

  // ---------------- Studio ----------------
  Studio::Studio()
  {
    synths.push_back(make_ottofm());
    synths.push_back(make_nuke());
    synths.push_back(make_string());
    synths.push_back(make_cluster());
    synths.push_back(make_sampler());
    for (int m = 0; m < kMods; m++) euclids.emplace_back("m" + std::to_string(m + 1) + ".");
    auto& R = registry();
    synth_sel = &R.add("studio.synth", "Motor", 0, float(synths.size() - 1), 0, 0, true);
    bpm = &R.add("studio.bpm", "BPM", 40, 240, 120, 0, true);
    bars = &R.add("tape.bars", "Compases cinta", 1, 16, 4, 0, true);
    mod = &R.add("studio.mod", "Mod (vibrato)", 0, 1, 0);
    bend_range = &R.add("studio.bend", "Rango Pitch Bend", 0, 12, 2, 0, true);
    master = &R.add("master.vol", "Master", 0, 1, 0.8f);
    rec_track = &R.add("tape.rec", "Pista armada", 0, 3, 0, 0, true);
    seq_gate = &R.add("seq.gate", "Gate secuencia", 0.05f, 1, 0.5f);
    tape_speed = &R.add("tape.speed", "Velocidad cinta", 0.25f, 2, 1);
    tape_rev = &R.add("tape.rev", "Reversa", 0, 1, 0, 0, true);
    loop_in = &R.add("tape.in", "Loop inicio", 0, 1, 0);
    loop_out = &R.add("tape.out", "Loop fin", 0, 1, 1);
    for (auto& m : melody)
      for (auto& n : m) n = -1;
    smp_start = registry().find("smp.start");
    smp_end = registry().find("smp.end");
  }

  void Studio::hit_pad(int pad, float vel)
  {
    if (pad < 0 || pad >= Drums::N) return;
    drums.hit(pad, vel);
    pad_hits[size_t(pad)].fetch_add(1, std::memory_order_relaxed);
  }

  void Studio::set_held(int n, bool on)
  {
    if (n < 0 || n > 127) return;
    uint64_t bit = uint64_t(1) << (n & 63);
    auto& w = held[size_t(n >> 6)];
    if (on) w.fetch_or(bit, std::memory_order_relaxed);
    else w.fetch_and(~bit, std::memory_order_relaxed);
  }

  void Studio::handle(const Ev& e)
  {
    bool arp_on = arp.on->geti() != 0;
    switch (e.t) {
      case Ev::NoteOn:
        set_held(e.a, true);
        last_note = e.a;
        if (note_rec && playing) { // quantize to nearest 16th of the melodic sequence
          double spb16 = kSR * 60.0 / bpm->get() / 4.0;
          cur_melody()[size_t(int(std::lround(tpos / spb16)) % 16)] = e.a;
        }
        if (arp_on) arp.press(e.a);
        else synth().note_on(e.a, e.b);
        break;
      case Ev::NoteOff:
        set_held(e.a, false);
        if (arp_on) arp.release(e.a);
        else synth().note_off(e.a);
        break;
      case Ev::Drum:
        hit_pad(e.a, e.b);
        if (step_rec && playing && e.a >= 0 && e.a < Drums::N) { // quantize to nearest 16th
          double spb16 = kSR * 60.0 / bpm->get() / 4.0;
          int st = int(std::lround(double(tape_pos.load()) / spb16)) % 16;
          cur_steps()[size_t(e.a)].fetch_or(uint16_t(1u << st));
        }
        break;
      case Ev::Bend: bend = e.b; break;
      case Ev::Play: playing = true; break;
      case Ev::TogglePlay: playing = !playing; break;
      case Ev::Stop:
        playing = false;
        recording = false;
        tape_stopping = false;
        stop_ramp = 1;
        tape_pos = 0;
        tpos = 0;
        step = -1;
        if (seq_sounding >= 0) { synth().note_off(seq_sounding); seq_sounding = -1; }
        break;
      case Ev::TapeStop:
        if (playing) tape_stopping = true;
        break;
      case Ev::ToggleRec:
        recording = !recording;
        if (recording) playing = true;
        break;
      case Ev::ClearTrack:
        if (e.a >= 0 && e.a < Tape::T) {
          tape.clearing[size_t(e.a)] = true;
          tape.clear_pos[size_t(e.a)] = 0;
        }
        break;
      case Ev::AllOff: synth().all_off(); break;
    }
  }

  void Studio::process(float* out, unsigned frames)
  {
    Ev e;
    while (midi_q.pop(e)) handle(e);
    while (ui_q.pop(e)) handle(e);

    int sel = synth_sel->geti();
    if (sel != cur_synth) {
      synths[size_t(cur_synth)]->all_off();
      arp.sounding = -1;
      seq_sounding = -1;
      cur_synth = sel;
    }
    Engine& eng = synth();
    bool arp_on = arp.on->geti() != 0;
    if (arp_was_on && !arp_on) {
      eng.all_off();
      arp.held.clear();
      arp.sounding = -1;
    }
    arp_was_on = arp_on;
    eng.update();

    float b = bpm->get();
    size_t loop = std::min(size_t(float(bars->geti()) * 4.f * 60.f / b * kSR), Tape::kMaxFrames);
    loop_frames = loop;
    double spb16 = kSR * 60.0 / b / 4.0;
    float bendmul = std::pow(2.f, bend * float(bend_range->geti()) / 12.f);
    float modv = mod->get(), evol = eng.volume->get(), mvol = master->get();
    int rt = rec_track->geti();
    bool play = playing, rec = recording;
    bool stopping = tape_stopping;
    // loop region quantized to 16ths, at least one 16th long
    double lo = std::floor(loop_in->get() * double(loop) / spb16) * spb16;
    double hi = std::max(lo + spb16, std::round(loop_out->get() * double(loop) / spb16) * spb16);
    hi = std::min(hi, double(loop));
    if (lo >= hi) lo = 0;
    float dir = tape_rev->geti() ? -1.f : 1.f;
    // ponytail: recording runs at 1x (direction kept); varispeed only on playback
    float speed = rec ? dir : dir * tape_speed->get();
    if (tpos < lo || tpos >= hi) tpos = dir > 0 ? lo : hi - 1;
    int last_step = step;
    auto& mel = cur_melody();
    float gate = seq_gate->get();
    float pk = 0;

    for (unsigned i = 0; i < frames; i++) {
      vib_ph += 5.5f / kSR;
      if (vib_ph >= 1) vib_ph -= 1;
      eng.freq_mul = bendmul * (1 + modv * 0.02f * std::sin(2 * kPi * vib_ph));
      if (arp_on) arp.tick(eng, b);

      if (seq_sounding >= 0 && (seq_off -= 1) <= 0) {
        eng.note_off(seq_sounding);
        seq_sounding = -1;
      }
      if (play) {
        int st = int(tpos / spb16);
        if (st != last_step) {
          last_step = st;
          if (int n = mel[size_t(st % 16)].load(std::memory_order_relaxed); n >= 0) {
            if (seq_sounding >= 0) eng.note_off(seq_sounding);
            eng.note_on(n, 0.85f);
            seq_sounding = n;
            seq_off = spb16 * gate;
          }
          for (auto& c : cur_euclid().ch)
            if (Euclid::hit(st, c.len->geti(), c.hits->geti(), c.rot->geti())) hit_pad(c.pad->geti(), 0.9f);
          for (int p = 0; p < Drums::N; p++)
            if ((cur_steps()[size_t(p)].load(std::memory_order_relaxed) >> (st % 16)) & 1) hit_pad(p, 0.85f);
        }
      }

      float x = eng.process() * evol + drums.process();
      float L, R;
      chorus.process(x, L, R);
      delay.process(L, R, b);
      reverb.process(L, R);

      float oL = L, oR = R;
      if (play) {
        float sp = speed * stop_ramp;
        size_t i0 = size_t(tpos), i1 = i0 + 1 < size_t(hi) ? i0 + 1 : size_t(lo);
        float fr = float(tpos - double(i0));
        for (size_t t = 0; t < Tape::T; t++) {
          float* a = &tape.trk[t][i0 * 2];
          const float* b2 = &tape.trk[t][i1 * 2];
          if (rec && int(t) == rt) { // overdub at 1x
            a[0] += L;
            a[1] += R;
          }
          if (!tape.mute[t]) {
            float v = tape.vol[t]->get();
            oL += (a[0] + (b2[0] - a[0]) * fr) * v;
            oR += (a[1] + (b2[1] - a[1]) * fr) * v;
          }
        }
        tpos += sp;
        if (tpos >= hi) tpos = lo + std::fmod(tpos - hi, hi - lo);
        if (tpos < lo) tpos = hi - std::fmod(lo - tpos, hi - lo);
        if (stopping) { // OP-1 style tape stop: ~0.6 s slow-down, then halt
          stop_ramp -= 1.f / (0.6f * kSR);
          if (stop_ramp <= 0) {
            stop_ramp = 1;
            stopping = false;
            play = false;
            playing = false;
            recording = false;
            tape_stopping = false;
          }
        }
      }
      oL = std::tanh(oL * mvol);
      oR = std::tanh(oR * mvol);
      out[2 * i] = oL;
      out[2 * i + 1] = oR;
      pk = std::max(pk, std::abs(oL));
      uint32_t sw = scope_w.load(std::memory_order_relaxed);
      scope[sw % kScope].store(0.5f * (oL + oR), std::memory_order_relaxed);
      scope_w.store(sw + 1, std::memory_order_relaxed);
    }
    tape_pos = size_t(tpos);
    step = last_step;
    peak = pk;

    for (size_t t = 0; t < Tape::T; t++) { // chunked clear
      if (!tape.clearing[t]) continue;
      auto& v = tape.trk[t];
      size_t n = std::min<size_t>(96000, v.size() - tape.clear_pos[t]);
      std::fill_n(v.begin() + long(tape.clear_pos[t]), n, 0.f);
      tape.clear_pos[t] += n;
      if (tape.clear_pos[t] >= v.size()) tape.clearing[t] = false;
    }
  }

  // ---------------- sampler zoom + session library ----------------
  void Studio::sample_zoom_reset()
  {
    for (auto* p : {smp_start, smp_end}) {
      p->view_lo = 0;
      p->view_hi = 1;
    }
  }

  void Studio::sample_zoom_press()
  {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - last_zoom_ms.exchange(now) < 400) { // double press -> whole sample
      sample_zoom_reset();
      last_zoom_ms = -100000;
      return;
    }
    float s = smp_start->get(), e = smp_end->get();
    if (e < s) std::swap(s, e);
    float m = std::max((e - s) * 0.1f, 0.0005f); // 10% margin so the edges can still move outwards
    float lo = std::max(0.f, s - m), hi = std::min(1.f, e + m);
    for (auto* p : {smp_start, smp_end}) {
      p->view_lo = lo;
      p->view_hi = hi;
    }
  }

  static void remember(std::vector<std::string>& pool, const std::string& path)
  {
    if (std::find(pool.begin(), pool.end(), path) == pool.end()) pool.push_back(path);
  }

  bool Studio::load_sampler(const std::string& path)
  {
    if (!sampler_load(*synths.back(), path)) return false;
    sampler_path = path;
    remember(sample_pool, path);
    sample_zoom_reset();
    return true;
  }

  bool Studio::load_pad(int pad, const std::string& path)
  {
    if (!drums.load_pad(pad, path)) return false;
    pad_paths[size_t(pad)] = path;
    remember(sample_pool, path);
    return true;
  }

  // ---------------- save / load ----------------
  bool Studio::save(const std::string& dir)
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(u8path(dir), ec);
    nlohmann::json j;
    for (auto& p : registry().all) j["params"][p.id] = p.get();
    j["samples"]["sampler"] = sampler_path;
    j["samples"]["pads"] = pad_paths;
    j["samples"]["pool"] = sample_pool;
    for (auto& pat : steps) {
      nlohmann::json row = nlohmann::json::array();
      for (auto& s : pat) row.push_back(s.load());
      j["steps"].push_back(row);
    }
    for (auto& m : melody) {
      nlohmann::json row = nlohmann::json::array();
      for (auto& n : m) row.push_back(n.load());
      j["melody"].push_back(row);
    }
    std::ofstream(u8path(dir) / "project.json") << j.dump(2);
    size_t frames = loop_frames;
    for (size_t t = 0; t < Tape::T; t++) {
      drwav_data_format f{};
      f.container = drwav_container_riff;
      f.format = DR_WAVE_FORMAT_IEEE_FLOAT;
      f.channels = 2;
      f.sampleRate = unsigned(kSR);
      f.bitsPerSample = 32;
      drwav w;
      auto path = u8path(dir) / ("track" + std::to_string(t + 1) + ".wav");
      if (!drwav_init_file_write_w(&w, path.c_str(), &f, nullptr)) return false;
      drwav_write_pcm_frames(&w, frames, tape.trk[t].data());
      drwav_uninit(&w);
    }
    return true;
  }

  bool Studio::export_mix(const std::string& path)
  {
    std::error_code ec;
    std::filesystem::create_directories(u8path(path).parent_path(), ec);
    size_t frames = loop_frames;
    std::vector<float> mix(frames * 2, 0.f);
    for (size_t t = 0; t < Tape::T; t++) {
      if (tape.mute[t]) continue;
      float v = tape.vol[t]->get();
      for (size_t i = 0; i < frames * 2; i++) mix[i] += tape.trk[t][i] * v;
    }
    float m = master->get();
    for (auto& s : mix) s = std::tanh(s * m);
    drwav_data_format f{};
    f.container = drwav_container_riff;
    f.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    f.channels = 2;
    f.sampleRate = unsigned(kSR);
    f.bitsPerSample = 32;
    drwav w;
    if (!drwav_init_file_write_w(&w, u8path(path).c_str(), &f, nullptr)) return false;
    drwav_write_pcm_frames(&w, frames, mix.data());
    drwav_uninit(&w);
    return true;
  }

  bool Studio::load(const std::string& dir)
  {
    namespace fs = std::filesystem;
    std::ifstream in(u8path(dir) / "project.json");
    if (!in) return false;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) return false;
    // ponytail: caller stops transport first; audio skips tape while stopped, so writing here is safe enough
    ui_q.push({Ev::Stop});
    for (auto& [id, v] : j["params"].items())
      if (auto* p = registry().find(id); p && v.is_number()) p->set(v.get<float>());
    for (auto& pat : steps)
      for (auto& s : pat) s = 0;
    if (j.contains("samples")) { // restore the session library and what each slot had loaded
      auto& s = j["samples"];
      for (auto& p : s.value("pool", std::vector<std::string>{})) remember(sample_pool, p);
      if (auto sp = s.value("sampler", std::string{}); !sp.empty()) load_sampler(sp);
      auto pads = s.value("pads", std::vector<std::string>{});
      for (size_t i = 0; i < pads.size() && i < size_t(Drums::N); i++)
        if (!pads[i].empty()) load_pad(int(i), pads[i]);
    }
    for (auto& m : melody)
      for (auto& n : m) n = -1;
    if (j.contains("melody") && j["melody"].is_array())
      for (size_t m = 0; m < melody.size() && m < j["melody"].size(); m++)
        for (size_t i = 0; i < 16 && i < j["melody"][m].size(); i++) melody[m][i] = j["melody"][m][i].get<int>();
    if (j.contains("steps") && j["steps"].is_array())
      for (size_t m = 0; m < steps.size() && m < j["steps"].size(); m++) {
        auto& row = j["steps"][m];
        if (row.is_number()) { steps[0][m] = row.get<uint16_t>(); continue; } // v2 format: one global pattern
        for (size_t i = 0; i < steps[m].size() && i < row.size(); i++) steps[m][i] = row[i].get<uint16_t>();
      }
    for (size_t t = 0; t < Tape::T; t++) {
      auto path = u8path(dir) / ("track" + std::to_string(t + 1) + ".wav");
      unsigned ch = 0, sr = 0;
      drwav_uint64 n = 0;
      float* d = drwav_open_file_and_read_pcm_frames_f32_w(path.c_str(), &ch, &sr, &n, nullptr);
      std::fill(tape.trk[t].begin(), tape.trk[t].end(), 0.f);
      if (!d) continue;
      n = std::min<drwav_uint64>(n, Tape::kMaxFrames);
      for (size_t f = 0; f < n; f++) {
        tape.trk[t][2 * f] = d[f * ch];
        tape.trk[t][2 * f + 1] = d[f * ch + (ch > 1 ? 1 : 0)];
      }
      drwav_free(d, nullptr);
    }
    return true;
  }

} // namespace otto
