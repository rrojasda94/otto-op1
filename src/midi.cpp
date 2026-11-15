#include "midi.hpp"

#include <RtMidi.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace otto {

  Midi::Midi(Studio& s) : st(s) {}
  Midi::~Midi() = default;

  void Midi::push_log(std::string s)
  {
    if (!path_.empty()) { // mirror to midi_log.txt next to mapping.json (for remote debugging)
      static std::ofstream f(std::filesystem::path(path_).parent_path() / "midi_log.txt");
      f << s << '\n' << std::flush;
    }
    log.push_back(std::move(s));
    while (log.size() > 200) log.pop_front();
  }

  void Midi::open_all()
  {
    std::lock_guard lk(mtx);
    ins.clear();
    ports.clear();
    try {
      RtMidiIn probe;
      unsigned n = probe.getPortCount();
      for (unsigned i = 0; i < n; i++) {
        auto in = std::make_unique<RtMidiIn>();
        auto name = in->getPortName(i);
        in->openPort(i);
        in->ignoreTypes(true, true, true); // sysex, clock, active sensing
        in->setCallback(
          [](double, std::vector<unsigned char>* msg, void* self) {
            if (msg && !msg->empty()) static_cast<Midi*>(self)->on_msg(msg->data(), msg->size());
          },
          this);
        ports.push_back(name);
        ins.push_back(std::move(in));
      }
      push_log("Puertos MIDI abiertos: " + std::to_string(n));
    } catch (RtMidiError& e) {
      push_log(std::string("Error MIDI: ") + e.what());
    }
  }

  Param* Midi::resolve(const std::string& t)
  {
    if (t.size() == 4 && t.starts_with("enc") && t[3] >= '1' && t[3] <= '4')
      return st.synth().page_param(st.page, t[3] - '1');
    return registry().find(t);
  }

  void Midi::action(const std::string& a)
  {
    auto& e = st.synth();
    int pages = int(e.pages.size());
    if (a == "play") st.midi_q.push({Ev::TogglePlay});
    else if (a == "stop") st.midi_q.push({Ev::Stop});
    else if (a == "rec") st.midi_q.push({Ev::ToggleRec});
    else if (a == "clear") st.midi_q.push({Ev::ClearTrack, st.rec_track->geti()});
    else if (a.starts_with("track") && a.size() == 6) st.rec_track->set(float(a[5] - '1'));
    else if (a == "page_next") st.page = (st.page + 1) % pages;
    else if (a == "page_prev") st.page = (st.page + pages - 1) % pages;
    else if (a == "synth_next") {
      st.synth_sel->set(float((st.synth_sel->geti() + 1) % int(st.synths.size())));
      st.page = 0;
    } else if (a == "arp") st.arp.on->set(st.arp.on->geti() ? 0.f : 1.f);
    else if (a == "tapestop") st.midi_q.push({Ev::TapeStop});
    else if (a == "reverse") st.tape_rev->set(st.tape_rev->geti() ? 0.f : 1.f);
    else if (a == "note_rec") st.note_rec = !st.note_rec;
    else if (a == "preset_next") st.preset_nav += 1;
    else if (a == "preset_prev") st.preset_nav -= 1;
    else if (a == "sample_zoom") st.sample_zoom_press();
    else if (a == "sample_next") st.sample_nav += 1;
    else if (a == "sample_prev") st.sample_nav -= 1;
  }

  void Midi::on_msg(const unsigned char* m, size_t n)
  {
    std::lock_guard lk(mtx);
    uint8_t type = m[0] & 0xF0;
    int ch = (m[0] & 0x0F) + 1;
    int d1 = n > 1 ? m[1] : 0, d2 = n > 2 ? m[2] : 0;
    char buf[96];
    std::snprintf(buf, sizeof buf, "ch%-2d %s %3d %3d", ch,
                  type == 0x90 ? "NOTE ON " : type == 0x80 ? "NOTE OFF" : type == 0xB0 ? "CC      " : type == 0xE0 ? "BEND    " : "OTRO    ",
                  d1, d2);
    push_log(buf);

    auto match = [&](const Binding& b, int num) { return b.num == num && (b.ch == 0 || b.ch == ch); };

    if (type == 0x90 && d2 > 0) {
      if (!learn.empty() && learn.starts_with("action:")) {
        std::erase_if(notes, [&](auto& b) { return match(b, d1); });
        notes.push_back({ch, d1, learn});
        push_log("Aprendido: nota " + std::to_string(d1) + " -> " + learn);
        learn.clear();
        return;
      }
      for (auto& b : notes)
        if (match(b, d1)) { action(b.target.substr(7)); return; }
      if (ch == drum_ch && d1 >= drum_base && d1 < drum_base + Drums::N)
        st.midi_q.push({Ev::Drum, d1 - drum_base, float(d2) / 127.f});
      else st.midi_q.push({Ev::NoteOn, d1, float(d2) / 127.f});
    } else if (type == 0x80 || type == 0x90) {
      for (auto& b : notes)
        if (match(b, d1)) return;
      if (!(ch == drum_ch && d1 >= drum_base && d1 < drum_base + Drums::N)) st.midi_q.push({Ev::NoteOff, d1});
    } else if (type == 0xB0) {
      if (!learn.empty()) {
        std::erase_if(cc, [&](auto& b) { return match(b, d1); });
        cc.push_back({ch, d1, learn});
        push_log("Aprendido: CC " + std::to_string(d1) + " -> " + learn);
        learn.clear();
        return;
      }
      bool bound = false;
      for (auto& b : cc) {
        if (!match(b, d1)) continue;
        bound = true;
        if (b.target.starts_with("action:")) {
          if (d2 > 63) action(b.target.substr(7));
          continue;
        }
        // absolute controls only send on change; a repeated value near 64 means relative encoder
        if (!b.relative) {
          b.same = (d2 == b.last && d2 >= 56 && d2 <= 72) ? b.same + 1 : 0;
          b.last = d2;
          if (b.same >= 2) {
            b.relative = true;
            push_log("CC " + std::to_string(d1) + " detectado como encoder relativo");
          }
        }
        auto* p = resolve(b.target);
        if (!p) { push_log("  -> " + b.target + " (no existe en esta pagina)"); continue; }
        if (b.relative) {
          int delta = d2 - 64; // Arturia relative: 64 +/- n
          if (delta == 0) continue;
          if (p->integer) p->set(p->get() + float(delta > 0 ? 1 : -1));
          else p->set_norm(p->norm() + float(delta) / 100.f); // calibration: 100 ticks = full range
        } else p->set_norm(float(d2) / 127.f);
        char t[96];
        std::snprintf(t, sizeof t, "  -> %s = %.2f", p->name.c_str(), p->get());
        push_log(t);
      }
      if (!bound) push_log("  (CC sin asignar: usa L junto al parametro)");
    } else if (type == 0xE0) {
      int v = (d2 << 7) | d1;
      st.midi_q.push({Ev::Bend, 0, float(v - 8192) / 8192.f});
    }
  }

  bool Midi::load(const std::string& path)
  {
    path_ = path;
    std::ifstream in(u8path(path));
    if (!in) return false;
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) return false;
    std::lock_guard lk(mtx);
    drum_ch = j.value("drum_channel", 10);
    drum_base = j.value("drum_base_note", 36);
    cc.clear();
    notes.clear();
    for (auto& b : j["cc"]) cc.push_back({b.value("ch", 0), b.value("cc", 0), b.value("target", ""), b.value("relative", false)});
    for (auto& b : j["notes"]) notes.push_back({b.value("ch", 0), b.value("note", 0), b.value("target", "")});
    return true;
  }

  void Midi::merge_defaults(const std::string& path)
  {
    std::ifstream in(path);
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (!in || j.is_discarded()) return;
    std::lock_guard lk(mtx);
    // add default bindings whose CC/note the user has not bound yet (keeps learned mappings)
    for (auto& b : j["cc"]) {
      int num = b.value("cc", -1);
      if (std::none_of(cc.begin(), cc.end(), [&](auto& x) { return x.num == num; }))
        cc.push_back({b.value("ch", 0), num, b.value("target", ""), b.value("relative", false)});
    }
    for (auto& b : j["notes"]) {
      int num = b.value("note", -1);
      if (std::none_of(notes.begin(), notes.end(), [&](auto& x) { return x.num == num; }))
        notes.push_back({b.value("ch", 0), num, b.value("target", "")});
    }
  }

  bool Midi::save()
  {
    std::lock_guard lk(mtx);
    nlohmann::json j;
    j["drum_channel"] = drum_ch;
    j["drum_base_note"] = drum_base;
    j["cc"] = nlohmann::json::array();
    j["notes"] = nlohmann::json::array();
    for (auto& b : cc) j["cc"].push_back({{"ch", b.ch}, {"cc", b.num}, {"target", b.target}, {"relative", b.relative}});
    for (auto& b : notes) j["notes"].push_back({{"ch", b.ch}, {"note", b.num}, {"target", b.target}});
    std::ofstream out(path_);
    out << j.dump(2);
    return bool(out);
  }

} // namespace otto
