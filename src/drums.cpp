// 8-pad drum sampler. Built-in synthetic kit; samples/padN.wav overrides pad N.
#include "core.hpp"

#include <dr_wav.h>

#include <filesystem>
#include <random>

namespace otto {

  static std::vector<float> render(float seconds, auto&& f)
  {
    std::vector<float> v(size_t(seconds * kSR));
    for (size_t i = 0; i < v.size(); i++) v[i] = f(float(i) / kSR);
    return v;
  }

  Drums::Drums()
  {
    auto& R = registry();
    vol = &R.add("drum.vol", "Volumen Drums", 0, 1, 0.8f);
    tune = &R.add("drum.tune", "Afinacion Drums", -12, 12, 0, 0, true);

    std::mt19937 rng(1);
    std::uniform_real_distribution<float> U(-1, 1);
    auto noise = [&] { return U(rng); };

    // kick: pitch-swept sine
    float ph = 0;
    smp[0] = render(0.5f, [&](float t) {
      float f = 45 + 110 * std::exp(-t * 30);
      ph += f / kSR;
      return std::sin(2 * kPi * ph) * std::exp(-t * 7);
    });
    // snare: tone + noise
    smp[1] = render(0.3f, [&](float t) {
      return 0.5f * std::sin(2 * kPi * 190 * t) * std::exp(-t * 25) + 0.6f * noise() * std::exp(-t * 14);
    });
    // hats: high-passed noise
    OnePole lp;
    lp.lp(6000);
    smp[2] = render(0.08f, [&](float t) { float n = noise(); return (n - lp(n)) * std::exp(-t * 60); });
    smp[3] = render(0.4f, [&](float t) { float n = noise(); return (n - lp(n)) * std::exp(-t * 9); });
    // clap: 3 bursts then tail
    smp[4] = render(0.3f, [&](float t) {
      float env = t < 0.03f ? std::exp(-std::fmod(t, 0.01f) * 300) : std::exp(-(t - 0.03f) * 18);
      return noise() * env * 0.7f;
    });
    // toms
    for (int k : {5, 6}) {
      float f0 = k == 5 ? 110.f : 180.f, p = 0;
      smp[size_t(k)] = render(0.4f, [&](float t) {
        p += (f0 * (0.7f + 0.3f * std::exp(-t * 20))) / kSR;
        return std::sin(2 * kPi * p) * std::exp(-t * 9);
      });
    }
    // cowbell: two squares
    smp[7] = render(0.25f, [&](float t) {
      auto sq = [](float x) { return std::fmod(x, 1.f) < 0.5f ? 1.f : -1.f; };
      return 0.3f * (sq(540 * t) + sq(800 * t)) * std::exp(-t * 15);
    });
    rate_base.fill(1.f);
  }

  void Drums::load_dir(const std::string& dir)
  {
    for (int i = 0; i < N; i++) {
      auto path = std::filesystem::path(dir) / ("pad" + std::to_string(i + 1) + ".wav");
      if (!std::filesystem::exists(path)) continue;
      unsigned ch = 0, sr = 0;
      drwav_uint64 frames = 0;
      float* d = drwav_open_file_and_read_pcm_frames_f32(path.string().c_str(), &ch, &sr, &frames, nullptr);
      if (!d) continue;
      std::vector<float> mono(static_cast<size_t>(frames));
      for (size_t f = 0; f < frames; f++) {
        float s = 0;
        for (unsigned c = 0; c < ch; c++) s += d[f * ch + c];
        mono[f] = s / float(ch);
      }
      drwav_free(d, nullptr);
      smp[size_t(i)] = std::move(mono);
      rate_base[size_t(i)] = float(sr) / kSR;
    }
  }

  void Drums::hit(int pad, float vel)
  {
    if (pad < 0 || pad >= N) return;
    pos[size_t(pad)] = 0;
    playing[size_t(pad)] = true;
    gain[size_t(pad)] = vel;
  }

  float Drums::process()
  {
    float rate = std::pow(2.f, float(tune->geti()) / 12.f);
    float out = 0;
    for (size_t i = 0; i < N; i++) {
      if (!playing[i]) continue;
      auto& s = smp[i];
      size_t p = size_t(pos[i]);
      if (p + 1 >= s.size()) { playing[i] = false; continue; }
      float f = float(pos[i] - double(p));
      out += (s[p] + (s[p + 1] - s[p]) * f) * gain[i];
      pos[i] += rate * rate_base[i];
    }
    return out * vol->get();
  }

} // namespace otto
