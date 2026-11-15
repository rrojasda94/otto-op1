// otto-op1 — OP-1 style groovebox for Windows.
// DSP ported from OTTO (c) 2021 Bitfield Audio, CC BY-NC-SA 4.0.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace otto {

  inline constexpr float kSR = 48000.f;
  inline constexpr float kPi = std::numbers::pi_v<float>;

  inline float mtof(float n) { return 440.f * std::pow(2.f, (n - 69.f) / 12.f); }

  // ---------- lock-free single-producer / single-consumer queue ----------
  template<class T, size_t N>
  struct Ring {
    std::array<T, N> buf{};
    std::atomic<size_t> w{0}, r{0};
    bool push(const T& v)
    {
      auto wi = w.load(std::memory_order_relaxed);
      auto n = (wi + 1) % N;
      if (n == r.load(std::memory_order_acquire)) return false;
      buf[wi] = v;
      w.store(n, std::memory_order_release);
      return true;
    }
    bool pop(T& v)
    {
      auto ri = r.load(std::memory_order_relaxed);
      if (ri == w.load(std::memory_order_acquire)) return false;
      v = buf[ri];
      r.store((ri + 1) % N, std::memory_order_release);
      return true;
    }
  };

  // ---------- small DSP helpers ----------
  struct OnePole {
    float z = 0, a = 1;
    void lp(float hz) { a = 1.f - std::exp(-2.f * kPi * std::clamp(hz, 1.f, kSR * 0.45f) / kSR); }
    float operator()(float x) { return z += a * (x - z); }
  };

  struct DelayLine {
    std::vector<float> b;
    size_t w = 0;
    explicit DelayLine(size_t n = 2) : b(n, 0.f) {}
    void write(float x)
    {
      b[w] = x;
      w = (w + 1) % b.size();
    }
    /// d = samples ago (0 = newest), fractional, linear interpolation
    float read(float d) const
    {
      float rp = float(w) - 1.f - std::clamp(d, 0.f, float(b.size() - 2));
      if (rp < 0) rp += float(b.size());
      size_t i0 = size_t(rp) % b.size();
      size_t i1 = (i0 + 1) % b.size();
      float f = rp - std::floor(rp);
      return b[i0] + (b[i1] - b[i0]) * f;
    }
  };

  // ---------- parameters: written by UI/MIDI threads, read by audio ----------
  struct Param {
    std::string id, name;
    float min, max, def;
    int page;
    bool integer;
    std::atomic<float> val;
    Param(std::string id_, std::string name_, float mn, float mx, float d, int pg, bool in)
      : id(std::move(id_)), name(std::move(name_)), min(mn), max(mx), def(d), page(pg), integer(in), val(d)
    {}
    float get() const { return val.load(std::memory_order_relaxed); }
    int geti() const { return int(std::lround(get())); }
    void set(float v)
    {
      v = std::clamp(v, min, max);
      if (integer) v = std::round(v);
      val.store(v, std::memory_order_relaxed);
    }
    void set_norm(float n) { set(min + std::clamp(n, 0.f, 1.f) * (max - min)); }
    float norm() const { return max > min ? (get() - min) / (max - min) : 0.f; }
  };

  struct Registry {
    std::deque<Param> all; // deque: stable addresses
    Param& add(std::string id, std::string name, float mn, float mx, float d, int page = 0, bool integer = false)
    {
      return all.emplace_back(std::move(id), std::move(name), mn, mx, d, page, integer);
    }
    Param* find(std::string_view id)
    {
      for (auto& p : all)
        if (p.id == id) return &p;
      return nullptr;
    }
  };
  Registry& registry();

  // ---------- engine interface ----------
  struct Engine {
    std::string title;
    std::vector<std::string> pages;
    std::vector<Param*> params;
    Param* volume = nullptr;
    float freq_mul = 1.f; // pitch bend * vibrato, set by Studio per sample

    virtual ~Engine() = default;
    virtual void note_on(int note, float vel) = 0;
    virtual void note_off(int note) = 0;
    virtual void all_off() = 0;
    virtual void update() = 0; // read params; audio thread, once per block
    virtual float process() = 0;

    /// k-th param (0..3) of a page, or nullptr
    Param* page_param(int page, int k) const
    {
      int i = 0;
      for (auto* p : params)
        if (p->page == page && i++ == k) return p;
      return nullptr;
    }
  };

  std::unique_ptr<Engine> make_ottofm();
  std::unique_ptr<Engine> make_nuke();
  std::unique_ptr<Engine> make_string();
  std::unique_ptr<Engine> make_cluster();
  std::unique_ptr<Engine> make_sampler();
  /// Load a WAV into the SAMPLER engine (UI thread). false if not a sampler or unreadable.
  bool sampler_load(Engine& e, const std::string& path);
  /// Current sample data of a SAMPLER engine, nullptr otherwise (for drawing)
  const std::vector<float>* sampler_data(Engine& e);

  // ---------- drum sampler (8 pads) ----------
  /// UTF-8 string -> filesystem path (keeps accents/ñ working on Windows)
  std::filesystem::path u8path(const std::string& s);
  /// Decode WAV / MP3 / FLAC. false if unreadable.
  bool read_audio(const std::string& utf8_path, std::vector<float>& interleaved, unsigned& channels, unsigned& sr);
  bool read_audio_mono(const std::string& utf8_path, std::vector<float>& mono, unsigned& sr);

  struct Drums {
    static constexpr int N = 8;
    // double buffer per pad: UI loads into the inactive slot, then flips cur (audio never sees a half-written vector)
    std::array<std::array<std::vector<float>, 2>, N> slot;
    std::array<std::array<float, 2>, N> rate_base{};
    std::array<std::atomic<int>, N> cur{};
    std::array<std::string, N> names;
    const std::vector<float>& smp(size_t i) const { return slot[i][size_t(cur[i].load(std::memory_order_relaxed))]; }
    bool load_pad(int pad, const std::string& utf8_path); // UI thread
    std::array<double, N> pos{};
    std::array<bool, N> playing{};
    Param *vol, *tune;
    Drums();
    void load_dir(const std::string& dir); // samples/pad1.wav..pad8.wav override synthetic kit
    void hit(int pad, float vel);
    float process();
    std::array<float, N> gain{};
  };

  // ---------- effects ----------
  struct Chorus {
    DelayLine dl{4096};
    float ph = 0;
    Param *mix, *rate, *depth;
    Chorus();
    void process(float in, float& l, float& r);
  };

  struct PingDelay {
    DelayLine l{int(kSR * 2)}, r{int(kSR * 2)};
    OnePole tl, tr;
    Param *time, *fb, *mix;
    PingDelay();
    void process(float& L, float& R, float bpm);
  };

  /// Port of OTTO "Wormhole" (black hole reverb, Faust original by J. Midtgaard / X. Godart)
  struct Wormhole {
    static constexpr int N = 8, NAP = 6;
    struct AP {
      DelayLine d{2048};
      float len = 100;
    };
    std::array<std::array<AP, NAP>, N> ap;
    std::array<DelayLine, N> late;
    std::array<float, N> late_len{};
    std::array<float, N> fb{};
    std::array<std::array<OnePole, 2>, N> fb_lp;
    std::array<OnePole, 2> out_lp_l, out_lp_r;
    OnePole hp_line2;
    DelayLine shimmer_dl{8192};
    float shimmer_ph = 0;
    float lfo_ph = 0;
    Param *mix, *length, *shape, *shimmer;
    Wormhole();
    void process(float& L, float& R);
  };

  // ---------- sequencing ----------
  struct Arp {
    Param *on, *mode, *oct, *rate, *gate;
    std::vector<int> held;   // in play order
    int idx = 0;
    int sounding = -1;
    double phase = 0;     // samples since last step
    double off_at = -1;
    void press(int n);
    void release(int n);
    /// advances one sample; calls note_on/note_off on engine
    void tick(Engine& e, float bpm);
    Arp();
  };

  struct Euclid {
    static constexpr int C = 4;
    struct Ch {
      Param *len, *hits, *rot, *pad;
    };
    std::array<Ch, C> ch;
    explicit Euclid(const std::string& prefix);
    static bool hit(int step, int len, int hits, int rot)
    {
      if (len <= 0 || hits <= 0) return false;
      int i = ((step % len) + rot) % len;
      return (i * hits) % len < hits;
    }
  };

  // ---------- tape: 4 stereo looping tracks ----------
  struct Tape {
    static constexpr int T = 4;
    static constexpr size_t kMaxFrames = size_t(kSR) * 64; // 64 s per track
    std::array<std::vector<float>, T> trk;                 // interleaved stereo
    std::array<Param*, T> vol{};
    std::array<std::atomic<bool>, T> mute{};
    std::array<size_t, T> clear_pos{};                     // chunked clear, avoids glitches
    std::array<bool, T> clearing{};
    Tape();
  };

  // ---------- events into the audio thread ----------
  struct Ev {
    enum Type : uint8_t { NoteOn, NoteOff, Drum, Bend, Play, Stop, TogglePlay, ToggleRec, ClearTrack, AllOff, TapeStop };
    Type t;
    int a = 0;
    float b = 0;
  };

  struct Studio {
    std::vector<std::unique_ptr<Engine>> synths;
    Drums drums;
    Chorus chorus;
    PingDelay delay;
    Wormhole reverb;
    Arp arp;
    Tape tape;

    Param *synth_sel, *bpm, *bars, *mod, *master, *rec_track, *bend_range;

    Ring<Ev, 1024> midi_q, ui_q; // one producer each
    std::atomic<bool> playing{false}, recording{false};
    std::atomic<size_t> tape_pos{0}, loop_frames{0};
    std::atomic<int> step{-1};
    std::atomic<int> page{0};
    std::atomic<float> peak{0};

    // Each module (synth slot) owns its own drum pattern + Euclid; only the selected module's sequence plays.
    static constexpr int kMods = 5;
    using Pattern = std::array<std::atomic<uint16_t>, Drums::N>; // bit i = step i
    std::array<Pattern, kMods> steps{};
    std::vector<Euclid> euclids;
    int mod_index() const { return std::clamp(synth_sel->geti(), 0, kMods - 1); }
    Pattern& cur_steps() { return steps[size_t(mod_index())]; }
    Euclid& cur_euclid() { return euclids[size_t(mod_index())]; }

    // Melodic 16-step sequence per module: MIDI note per step, -1 = rest. Keys record into it with note_rec.
    using Melody = std::array<std::atomic<int>, 16>;
    std::array<Melody, kMods> melody;
    Melody& cur_melody() { return melody[size_t(mod_index())]; }
    std::atomic<bool> note_rec{false};
    std::atomic<int> last_note{60};
    Param* seq_gate;

    // Tape transport extras (OP-1 style): varispeed, reverse, loop in/out, tape stop
    Param *tape_speed, *tape_rev, *loop_in, *loop_out;
    std::atomic<bool> tape_stopping{false};
    std::atomic<int> preset_nav{0}; // +1/-1 requested by MIDI, consumed by the UI
    std::atomic<bool> step_rec{false};
    // for the UI: hit counters per pad, held keys bitmask, oscilloscope ring
    std::array<std::atomic<uint32_t>, Drums::N> pad_hits{};
    std::array<std::atomic<uint64_t>, 2> held{};
    static constexpr size_t kScope = 1024;
    std::array<std::atomic<float>, kScope> scope{};
    std::atomic<uint32_t> scope_w{0};

    Studio();
    Engine& synth() { return *synths[size_t(synth_sel->geti())]; }
    void process(float* out_interleaved, unsigned frames);

    bool save(const std::string& dir);
    bool load(const std::string& dir);
    /// Render the 4 tape tracks (faders, mutes, master) to one stereo WAV
    bool export_mix(const std::string& path);

  private:
    void handle(const Ev& e);
    void hit_pad(int pad, float vel);
    void set_held(int note, bool on);
    int cur_synth = 0;
    double tpos = 0;          // fractional tape head
    float stop_ramp = 1;      // 1 -> 0 during tape stop
    int seq_sounding = -1;    // note held by the melodic sequencer
    double seq_off = 0;       // samples until its note-off
    float bend = 0, vib_ph = 0;
    bool arp_was_on = false;
  };

} // namespace otto
