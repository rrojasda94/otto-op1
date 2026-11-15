// Effects: chorus, tempo delay, and Wormhole reverb (ported from OTTO master
// src/engines/fx/wormhole/wormhole.dsp — Faust original by Jonatan Midtgaard,
// adapted from "OWLGAZER" by Xavier Godart). CC BY-NC-SA 4.0.
#include "core.hpp"

namespace otto {

  Chorus::Chorus()
  {
    auto& R = registry();
    mix = &R.add("fx.chorus.mix", "Chorus Mix", 0, 1, 0);
    rate = &R.add("fx.chorus.rate", "Chorus Vel", 0, 1, 0.3f);
    depth = &R.add("fx.chorus.depth", "Chorus Prof", 0, 1, 0.5f);
  }

  void Chorus::process(float in, float& l, float& r)
  {
    dl.write(in);
    ph += (0.1f + 3.f * rate->get()) / kSR;
    if (ph >= 1) ph -= 1;
    float d = depth->get() * 0.004f * kSR;
    float base = 0.012f * kSR;
    float wl = dl.read(base + d * (0.5f + 0.5f * std::sin(2 * kPi * ph)));
    float wr = dl.read(base + d * (0.5f + 0.5f * std::cos(2 * kPi * ph)));
    float m = mix->get();
    l = in * (1 - m * 0.5f) + wl * m;
    r = in * (1 - m * 0.5f) + wr * m;
  }

  PingDelay::PingDelay()
  {
    auto& R = registry();
    time = &R.add("fx.delay.time", "Delay (1/16)", 1, 8, 3, 0, true);
    fb = &R.add("fx.delay.fb", "Delay Feedback", 0, 0.95f, 0.4f);
    mix = &R.add("fx.delay.mix", "Delay Mix", 0, 1, 0);
    tl.lp(5000);
    tr.lp(5000);
  }

  void PingDelay::process(float& L, float& R, float bpm)
  {
    float d = std::min(float(time->geti()) * 60.f / bpm / 4.f * kSR, kSR * 2 - 4);
    float dlv = l.read(d), drv = r.read(d);
    float f = fb->get();
    l.write(L + tl(drv) * f); // ping-pong: cross feedback
    r.write(R + tr(dlv) * f);
    float m = mix->get();
    L += dlv * m;
    R += drv * m;
  }

  // Wormhole: 8-line feedback delay network, Hadamard feedback, 6 allpasses per line,
  // shimmer (octave-up pitch shift) on line 4.
  static constexpr std::array<float, 9> kLate = {1356, 1422, 1557, 1617, 1933, 2401, 3125, 6561, 14641};
  static constexpr std::array<float, 8> kEarly = {243, 343, 441, 625, 727, 1331, 2403, 3119};

  Wormhole::Wormhole()
  {
    auto& R = registry();
    mix = &R.add("fx.rev.mix", "Reverb Mix", 0, 1, 0.25f);
    length = &R.add("fx.rev.length", "Reverb Largo", 0.05f, 1, 0.5f);
    shape = &R.add("fx.rev.shape", "Reverb Tono", 0, 1, 0.6f);
    shimmer = &R.add("fx.rev.shimmer", "Shimmer", 0, 1.2f, 0);
    for (int i = 0; i < N; i++) {
      late[size_t(i)] = DelayLine(8192);
      // ponytail: Faust fdelay clamps to MAXDELAY=8192, so the 14641 line is effectively 8191
      late_len[size_t(i)] = std::min(kLate[size_t(i) + 1], 8190.f);
      for (int j = 0; j < NAP; j++) ap[size_t(i)][size_t(j)].len = kEarly[size_t(j) + 1];
    }
    hp_line2.lp(100);
  }

  void Wormhole::process(float& L, float& R)
  {
    float m = mix->get();
    float decay = std::min(length->get() * 0.5f + 0.5f, 0.985f); // ponytail: capped below 1 for stability
    float s = shape->get();
    float hicut = 100.f * std::pow(140.f, s); // log 100..14000 Hz
    float pm = shimmer->get();

    lfo_ph += 0.25f / kSR;
    if (lfo_ph >= 1) lfo_ph -= 1;
    float tl = 1 + 0.001f * std::sin(2 * kPi * lfo_ph);

    float x = 0.5f * (L + R);
    std::array<float, N> y{};
    for (size_t i = 0; i < N; i++) {
      float v = x + fb[i];
      for (auto& a : ap[i]) { // allpass, g = 0.6
        float dv = a.d.read(a.len * tl - 1);
        float w = v + 0.6f * dv;
        a.d.write(w);
        v = dv - 0.6f * w;
      }
      late[i].write(v);
      float out = late[i].read(late_len[i] * tl - 1);
      if (i == 3 && pm > 0) {
        // pitch shifter: two crossfaded read heads sweeping a 2048-sample window, +12 semitones
        shimmer_dl.write(v);
        shimmer_ph += 1.f / 2048.f;
        if (shimmer_ph >= 1) shimmer_ph -= 1;
        float p2 = std::fmod(shimmer_ph + 0.5f, 1.f);
        float a1 = shimmer_dl.read((1 - shimmer_ph) * 2048.f);
        float a2 = shimmer_dl.read((1 - p2) * 2048.f);
        float sh = a1 * std::sin(kPi * shimmer_ph) + a2 * std::sin(kPi * p2);
        out = out * (1 - std::min(pm, 1.f)) + sh * pm;
      }
      y[i] = out / std::sqrt(float(N));
    }
    y[2] -= hp_line2(y[2]); // highpass 100 Hz on line 3

    // Hadamard 8x8 (unnormalized, as Faust ro.hadamard) -> decay -> lowpass
    std::array<float, N> h = y;
    for (size_t len = 1; len < N; len <<= 1)
      for (size_t i = 0; i < N; i += 2 * len)
        for (size_t j = i; j < i + len; j++) {
          float a = h[j], b = h[j + len];
          h[j] = a + b;
          h[j + len] = a - b;
        }
    OnePole coef;
    coef.lp(hicut);
    for (size_t i = 0; i < N; i++) {
      auto& lp = fb_lp[i];
      lp[0].a = lp[1].a = coef.a;
      fb[i] = lp[1](lp[0](h[i] * decay)); // y was already /sqrt(N): H*y keeps energy
    }

    float wl = 0, wr = 0;
    for (size_t i = 0; i < N; i += 2) {
      wl += y[i];
      wr += y[i + 1];
    }
    for (auto* f : {&out_lp_l[0], &out_lp_l[1], &out_lp_r[0], &out_lp_r[1]}) f->a = coef.a;
    wl = out_lp_l[1](out_lp_l[0](wl));
    wr = out_lp_r[1](out_lp_r[0](wr));
    L = L * (1 - m) + wl * m;
    R = R * (1 - m) + wr * m;
  }

} // namespace otto
