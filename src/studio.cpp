// Studio: routes events, runs synth + drums + FX + sequencers + 4-track tape.
#include "core.hpp"

#include <dr_wav.h>
#include <nlohmann/json.hpp>

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
  Euclid::Euclid()
  {
    auto& R = registry();
    const int def_hits[C] = {4, 0, 0, 0}, def_pad[C] = {0, 1, 2, 3};
    for (int i = 0; i < C; i++) {
      auto id = "euc" + std::to_string(i + 1) + ".";
      auto nm = "Euclid " + std::to_string(i + 1) + " ";
      ch[size_t(i)].len = &R.add(id + "len", nm + "Largo", 1, 16, 16, 0, true);
      ch[size_t(i)].hits = &R.add(id + "hits", nm + "Golpes", 0, 16, float(def_hits[i]), 0, true);
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
    auto& R = registry();
    synth_sel = &R.add("studio.synth", "Motor", 0, float(synths.size() - 1), 0, 0, true);
    bpm = &R.add("studio.bpm", "BPM", 40, 240, 120, 0, true);
    bars = &R.add("tape.bars", "Compases cinta", 1, 16, 4, 0, true);
    mod = &R.add("studio.mod", "Mod (vibrato)", 0, 1, 0);
    bend_range = &R.add("studio.bend", "Rango Pitch Bend", 0, 12, 2, 0, true);
    master = &R.add("master.vol", "Master", 0, 1, 0.8f);
    rec_track = &R.add("tape.rec", "Pista armada", 0, 3, 0, 0, true);
  }

  void Studio::handle(const Ev& e)
  {
    bool arp_on = arp.on->geti() != 0;
    switch (e.t) {
      case Ev::NoteOn:
        if (arp_on) arp.press(e.a);
        else synth().note_on(e.a, e.b);
        break;
      case Ev::NoteOff:
        if (arp_on) arp.release(e.a);
        else synth().note_off(e.a);
        break;
      case Ev::Drum: drums.hit(e.a, e.b); break;
      case Ev::Bend: bend = e.b; break;
      case Ev::Play: playing = true; break;
      case Ev::TogglePlay: playing = !playing; break;
      case Ev::Stop:
        playing = false;
        recording = false;
        tape_pos = 0;
        step = -1;
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
    size_t pos = tape_pos;
    if (pos >= loop) pos = 0;
    int last_step = step;
    float pk = 0;

    for (unsigned i = 0; i < frames; i++) {
      vib_ph += 5.5f / kSR;
      if (vib_ph >= 1) vib_ph -= 1;
      eng.freq_mul = bendmul * (1 + modv * 0.02f * std::sin(2 * kPi * vib_ph));
      if (arp_on) arp.tick(eng, b);

      if (play) {
        int st = int(double(pos) / spb16);
        if (st != last_step) {
          last_step = st;
          for (auto& c : euclid.ch)
            if (Euclid::hit(st, c.len->geti(), c.hits->geti(), c.rot->geti())) drums.hit(c.pad->geti(), 0.9f);
        }
      }

      float x = eng.process() * evol + drums.process();
      float L, R;
      chorus.process(x, L, R);
      delay.process(L, R, b);
      reverb.process(L, R);

      float oL = L, oR = R;
      if (play) {
        for (size_t t = 0; t < Tape::T; t++) {
          float* s = &tape.trk[t][pos * 2];
          if (rec && int(t) == rt) { // overdub
            s[0] += L;
            s[1] += R;
          }
          if (!tape.mute[t]) {
            float v = tape.vol[t]->get();
            oL += s[0] * v;
            oR += s[1] * v;
          }
        }
        if (++pos >= loop) pos = 0;
      }
      oL = std::tanh(oL * mvol);
      oR = std::tanh(oR * mvol);
      out[2 * i] = oL;
      out[2 * i + 1] = oR;
      pk = std::max(pk, std::abs(oL));
    }
    tape_pos = pos;
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

  // ---------------- save / load ----------------
  bool Studio::save(const std::string& dir)
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    nlohmann::json j;
    for (auto& p : registry().all) j["params"][p.id] = p.get();
    std::ofstream(fs::path(dir) / "project.json") << j.dump(2);
    size_t frames = loop_frames;
    for (size_t t = 0; t < Tape::T; t++) {
      drwav_data_format f{};
      f.container = drwav_container_riff;
      f.format = DR_WAVE_FORMAT_IEEE_FLOAT;
      f.channels = 2;
      f.sampleRate = unsigned(kSR);
      f.bitsPerSample = 32;
      drwav w;
      auto path = (fs::path(dir) / ("track" + std::to_string(t + 1) + ".wav")).string();
      if (!drwav_init_file_write(&w, path.c_str(), &f, nullptr)) return false;
      drwav_write_pcm_frames(&w, frames, tape.trk[t].data());
      drwav_uninit(&w);
    }
    return true;
  }

  bool Studio::load(const std::string& dir)
  {
    namespace fs = std::filesystem;
    std::ifstream in(fs::path(dir) / "project.json");
    if (!in) return false;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) return false;
    // ponytail: caller stops transport first; audio skips tape while stopped, so writing here is safe enough
    ui_q.push({Ev::Stop});
    for (auto& [id, v] : j["params"].items())
      if (auto* p = registry().find(id); p && v.is_number()) p->set(v.get<float>());
    for (size_t t = 0; t < Tape::T; t++) {
      auto path = (fs::path(dir) / ("track" + std::to_string(t + 1) + ".wav")).string();
      unsigned ch = 0, sr = 0;
      drwav_uint64 n = 0;
      float* d = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &sr, &n, nullptr);
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
