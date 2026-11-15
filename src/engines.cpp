// Synth engines ported from OTTO (develop branch, src/app/engines/synths/{ottofm,nuke}).
// Original (c) 2021 Bitfield Audio, CC BY-NC-SA 4.0.
#include "core.hpp"

#include <Gamma/Envelope.h>
#include <Gamma/Oscillator.h>

namespace otto {

  Registry& registry()
  {
    static Registry r;
    return r;
  }

  static float envelope_stage_duration(float d) { return 5 * d * d + 0.001f; }

  /// Simple polyphonic voice allocator (OTTO uses VoiceManager<Voice, 6>).
  template<class V, int N>
  struct Poly {
    std::array<V, N> v;
    std::array<int, N> note;
    std::array<uint32_t, N> age{};
    uint32_t clock = 0;
    Poly() { note.fill(-1); }

    V* on(int n)
    {
      int best = -1;
      for (int i = 0; i < N; i++) // free & silent
        if (note[i] < 0 && !v[i].active()) { best = i; break; }
      if (best < 0)
        for (int i = 0; i < N; i++) // releasing, oldest
          if (note[i] < 0 && (best < 0 || age[i] < age[best])) best = i;
      if (best < 0)
        for (int i = 0; i < N; i++) // steal oldest
          if (best < 0 || age[i] < age[best]) best = i;
      note[best] = n;
      age[best] = ++clock;
      v[best].base = mtof(float(n));
      return &v[best];
    }
    template<class F>
    void off(int n, F&& f)
    {
      for (int i = 0; i < N; i++)
        if (note[i] == n) { f(v[i]); note[i] = -1; }
    }
  };

  // =====================================================================
  // OTTO.FM — 4 operator phase-modulation synth, 11 algorithms
  // =====================================================================
  namespace fm {
    struct Fraction { int n, d; };
    constexpr std::array<Fraction, 31> fractions = {
      {{0, 1},  {1, 64}, {1, 32}, {1, 16},  {3, 32}, {1, 8},  {5, 32}, {1, 4},  {5, 16}, {7, 16}, {1, 2},
       {5, 8},  {3, 4},  {1, 1},  {17, 12}, {3, 2},  {13, 8}, {7, 4},  {2, 1},  {5, 2},  {19, 7}, {3, 1},
       {22, 7}, {7, 2},  {4, 1},  {5, 1},   {6, 1},  {7, 1},  {8, 1},  {16, 1}, {32, 1}}};

    struct OpParams {
      Param *ratio, *detune, *level, *shape, *a, *d, *s, *r;
    };

    struct Operator {
      gam::AccumPhase<> sine{440, 0, 1};
      gam::ADSR<> env;
      float ratio = 1, detune = 0, feedback = 0, level = 1, prev = 0;
      Operator() { env.finish(); }

      float operator()(float pm = 0) noexcept
      {
        float s = gam::scl::sinP9(gam::scl::wrap(sine.nextPhase() + pm + feedback * prev, 1.f, -1.f));
        prev = s * env() * level;
        return prev;
      }
      void freq(float f) noexcept { sine.freq(f * ratio + detune); }
      void apply(const OpParams& p) noexcept
      {
        env.attack(envelope_stage_duration(p.a->get()));
        env.decay(envelope_stage_duration(p.d->get()));
        env.release(envelope_stage_duration(p.r->get()));
        env.sustain(p.s->get());
        auto fr = fractions[size_t(p.ratio->geti())];
        ratio = float(fr.n) / float(fr.d);
        detune = 20 * p.detune->get();
        feedback = (p.shape->get() - 0.5f) * 2.f;
        level = p.level->get();
      }
    };

    struct Voice {
      std::array<Operator, 4> op;
      float base = 440;
      bool active() const
      {
        for (auto& o : op)
          if (!o.env.done()) return true;
        return false;
      }
      float operator()(int alg, float fmul) noexcept
      {
        for (auto& o : op) o.freq(base * fmul);
        auto& [op0, op1, op2, op3] = op;
        switch (alg) {
          case 0: return op0(op1(op2(op3(0))));
          case 1: return op0(op1(op2(0) + op3(0)));
          case 2: return op0(op1(op2(0)) + op3(0));
          case 3: {
            float aux = op3(0);
            return op0(op1(aux) + op2(aux));
          }
          case 4: {
            float aux = op2(op3(0));
            return (op0(aux) + op1(aux)) * 0.5f;
          }
          case 5: return (op0(0) + op1(op2(op3(0)))) * 0.5f;
          case 6: return op0(op1(0) + op2(0) + op3(0));
          case 7: return (op0(op1(0)) + op2(op3(0))) * 0.5f;
          case 8: {
            float aux = op3(0);
            return (op0(aux) + op1(aux) + op2(aux)) * 0.33f;
          }
          case 9: return (op0(0) + op1(0) + op2(op3(0))) * 0.33f;
          case 10: return (op0(0) + op1(0) + op2(0) + op3(0)) * 0.25f;
          default: return 0.f;
        }
      }
    };

    struct Engine final : otto::Engine {
      Param* alg;
      std::array<OpParams, 4> ops;
      Poly<Voice, 6> poly;
      int alg_i = 0;

      Engine()
      {
        title = "OTTO.FM";
        auto& R = registry();
        pages = {"ALGORITMO"};
        alg = &R.add("fm.alg", "Algoritmo", 0, 10, 0, 0, true);
        volume = &R.add("fm.vol", "Volumen", 0, 1, 0.7f, 0);
        params = {alg, volume};
        for (int k = 0; k < 4; k++) {
          auto id = "fm.op" + std::to_string(k + 1) + ".";
          auto nm = "Op" + std::to_string(k + 1) + " ";
          int po = 1 + 2 * k, pe = 2 + 2 * k;
          pages.push_back("OP" + std::to_string(k + 1) + " OSC");
          pages.push_back("OP" + std::to_string(k + 1) + " ENV");
          auto& o = ops[size_t(k)];
          o.ratio = &R.add(id + "ratio", nm + "Ratio", 0, 30, 13, po, true);
          o.detune = &R.add(id + "detune", nm + "Detune", -1, 1, 0, po);
          o.level = &R.add(id + "level", nm + "Nivel", 0, 1, 1, po);
          o.shape = &R.add(id + "shape", nm + "Feedback", 0, 1, 0.5f, po);
          o.a = &R.add(id + "a", nm + "Attack", 0, 1, 0.2f, pe);
          o.d = &R.add(id + "d", nm + "Decay", 0, 1, 0.2f, pe);
          o.s = &R.add(id + "s", nm + "Sustain", 0, 1, 0.7f, pe);
          o.r = &R.add(id + "r", nm + "Release", 0, 1, 0.2f, pe);
          for (auto* p : {o.ratio, o.detune, o.level, o.shape, o.a, o.d, o.s, o.r}) params.push_back(p);
        }
      }

      void update() override
      {
        alg_i = alg->geti();
        for (auto& v : poly.v)
          for (int k = 0; k < 4; k++) v.op[size_t(k)].apply(ops[size_t(k)]);
      }
      void note_on(int n, float) override
      {
        for (auto& o : poly.on(n)->op) o.env.resetSoft();
      }
      void note_off(int n) override
      {
        poly.off(n, [](Voice& v) {
          for (auto& o : v.op) o.env.release();
        });
      }
      void all_off() override
      {
        for (int i = 0; i < 6; i++)
          if (poly.note[size_t(i)] >= 0) note_off(poly.note[size_t(i)]);
      }
      float process() override
      {
        float s = 0;
        for (auto& v : poly.v) s += v(alg_i, freq_mul);
        return s * 0.35f;
      }
    };
  } // namespace fm

  // =====================================================================
  // NUKE — 2 osc virtual analogue: moog saw, ladder filter, filter env, LFO
  // =====================================================================
  namespace nuke {
    enum class Mode { None, Ringmod, Sync, AM };

    // Moog ladder, based on an implementation by Aaron Krajeski (Public Domain), via OTTO
    struct MoogLadder {
      double stage[4] = {0, 0, 0, 0}, delay[4] = {0, 0, 0, 0};
      double p = 0, k = 0, t1 = 0, t2 = 1, cutoff = 0;
      float res = 0.2f, resonance = 0;
      float operator()(float in)
      {
        double x = in - resonance * stage[3];
        stage[0] = x * p + delay[0] * p - k * stage[0];
        stage[1] = stage[0] * p + delay[1] * p - k * stage[1];
        stage[2] = stage[1] * p + delay[2] * p - k * stage[2];
        stage[3] = stage[2] * p + delay[3] * p - k * stage[3];
        stage[3] -= (stage[3] * stage[3] * stage[3]) / 6.0;
        delay[0] = x;
        delay[1] = stage[0];
        delay[2] = stage[1];
        delay[3] = stage[2];
        return float(stage[3]);
      }
      void setResonance(float r)
      {
        res = r;
        resonance = float(r * (t2 + 6.0 * t1) / (t2 - 6.0 * t1));
      }
      void setCutoff(float c)
      {
        cutoff = 2.0 * c / kSR;
        p = cutoff * (1.8 - 0.8 * cutoff);
        k = 2.0 * std::sin(cutoff * kPi * 0.5) - 1.0;
        t1 = (1.0 - p) * 1.386249;
        t2 = 12.0 + t1 * t1;
        setResonance(res);
      }
    };

    // Saw + EQ approximating the Moog oscillator shape (OTTO dsp::MoogSaw)
    struct MoogSaw {
      gam::Saw<> saw{440};
      float g = 0.54f, a = 0, b = 0, in_prev = 0, out_prev = 0;
      void freq(float f)
      {
        saw.freq(f);
        float f2 = f * f;
        a = 0.6398f - 2.417e-4f * f + 1.335e-8f * f2;
        b = 0.3894f - 3.102e-4f * f + 2.217e-8f * f2;
      }
      float operator()()
      {
        float in = saw();
        // ponytail: OTTO assigns in_prev before use (so the b-term cancels); kept as-is for the same tone
        in_prev = in;
        return out_prev = g * (in - b * in_prev) + a * out_prev;
      }
    };

    /// OTTO dsp::ADS — bipolar Minimoog-style filter envelope
    struct ADS : gam::Env<2> {
      ADS()
      {
        sustainPoint(2);
        levels(-0.5f, 0.5f, 0);
        lengths()[0] = 0.01f;
        lengths()[1] = 0.5f;
        curves(4, -4);
      }
      void attack(float l) { lengths()[0] = l; }
      void decay(float l) { lengths()[1] = l; }
      void sustain(float v)
      {
        levels()[0] = -v;
        levels()[1] = 1 - v;
        levels()[2] = 0;
      }
    };

    float neighborhoods(float in, int min, int max)
    {
      float num_steps = float(max - min);
      float scaled = min + 0.9f * in * (num_steps + 1) - 0.49f;
      float integral = std::round(scaled);
      float fractional = (scaled - integral) * 2;
      return integral + fractional * fractional * fractional / 2.f;
    }

    struct Shared {
      Mode mode = Mode::None;
      float osc2_ratio = 1, osc2_fixed = 2, mix1 = .5f, mix2 = .5f, cutoff = .5f, env_f_amt = 0;
      float lfo_pitch = 0, lfo_vol = 0, lfo_filt = 0, lfo_osc2 = 0;
      float lfo_val = 0;
    };

    struct Voice {
      float base = 440;
      MoogSaw osc1, osc2;
      float sync_ph = 0, master_ph = 0; // hard sync
      gam::Sine<> am{2};
      gam::ADSR<> env;
      ADS env_filter;
      gam::AD<> lfo_env{0.1f, 1.f, 1.f, 0.f};
      MoogLadder filter;
      Voice()
      {
        env.finish();
        env_filter.finish();
        lfo_env.finish();
      }
      bool active() const { return !env.done(); }

      float operator()(const Shared& s, float fmul)
      {
        float lfo = s.lfo_val * lfo_env();
        float freq = base * fmul * std::pow(2.f, lfo * s.lfo_pitch);
        // filter
        float fc = base * s.cutoff * s.cutoff * 10 * std::pow(2.f, 0.3f * lfo * s.lfo_filt);
        fc += (std::exp(env_filter() * s.env_f_amt) - 1.f) * 20000.f;
        filter.setCutoff(std::clamp(fc, 1.f, 20000.f));

        float pre = 0;
        float o2pitch = 1 + lfo * s.lfo_osc2;
        switch (s.mode) {
          case Mode::None:
            osc1.freq(freq);
            osc2.freq(freq * s.osc2_ratio * o2pitch);
            pre = 0.5f * (s.mix1 * osc1() + s.mix2 * osc2());
            break;
          case Mode::Ringmod: {
            osc1.freq(freq);
            osc2.freq(freq * s.osc2_ratio * o2pitch);
            float a = osc1(), b = osc2();
            pre = s.mix1 * a + s.mix2 * a * b;
          } break;
          case Mode::Sync: {
            // ponytail: naive hard-sync saw (OTTO uses PolyBLEP); aliasing at high notes
            osc1.freq(freq);
            master_ph += freq / kSR;
            sync_ph += freq * s.osc2_ratio * o2pitch / kSR;
            if (master_ph >= 1) { master_ph -= 1; sync_ph = 0; }
            if (sync_ph >= 1) sync_ph -= 1;
            pre = s.mix1 * osc1() + s.mix2 * (2 * sync_ph - 1);
          } break;
          case Mode::AM: // unfinished in OTTO; here: osc1 amplitude-modulated by fixed-freq sine
            osc1.freq(freq);
            am.freq(s.osc2_fixed);
            pre = osc1() * (s.mix1 + s.mix2 * am());
            break;
        }
        return filter(pre) * env() * (1 + lfo * s.lfo_vol) * 0.25f;
      }
    };

    struct Engine final : otto::Engine {
      Param *osc2_pitch, *osc_mix, *mode, *cutoff, *res, *f_amt, *lfo_filt, *a, *d, *s, *r, *fa, *fd, *fs,
        *lfo_osc2, *lfo_speed, *lfo_type, *lfo_a, *lfo_d, *lfo_pitch, *lfo_vol;
      Poly<Voice, 6> poly;
      Shared sh;
      gam::LFO<> lfo;
      int lfo_shape = 0;

      Engine()
      {
        title = "NUKE";
        pages = {"OSC", "FILTRO", "ENV", "FILTRO ENV", "LFO", "LFO DEST"};
        auto& R = registry();
        osc2_pitch = &R.add("nuke.osc2", "Osc2 Pitch", 0, 1, 0.5f, 0);
        osc_mix = &R.add("nuke.mix", "Mezcla Osc", 0, 1, 0.5f, 0);
        mode = &R.add("nuke.mode", "Modo (none/ring/sync/am)", 0, 3, 0, 0, true);
        volume = &R.add("nuke.vol", "Volumen", 0, 1, 0.7f, 0);
        cutoff = &R.add("nuke.cutoff", "Cutoff", 0, 1, 0.7f, 1);
        res = &R.add("nuke.res", "Resonancia", 0, 1, 0.2f, 1);
        f_amt = &R.add("nuke.famt", "Env Filtro", 0, 1, 0, 1);
        lfo_filt = &R.add("nuke.lfo_filt", "LFO>Filtro", 0, 1, 0, 1);
        a = &R.add("nuke.a", "Attack", 0, 1, 0, 2);
        d = &R.add("nuke.d", "Decay", 0, 1, 0, 2);
        s = &R.add("nuke.s", "Sustain", 0, 1, 1, 2);
        r = &R.add("nuke.r", "Release", 0, 1, 0.1f, 2);
        fa = &R.add("nuke.fa", "F Attack", 0, 1, 0.1f, 3);
        fd = &R.add("nuke.fd", "F Decay", 0, 1, 0.1f, 3);
        fs = &R.add("nuke.fs", "F Sustain", 0, 1, 0.5f, 3);
        lfo_osc2 = &R.add("nuke.lfo_osc2", "LFO>Osc2", 0, 1, 0, 3);
        lfo_speed = &R.add("nuke.lfo_speed", "LFO Vel", 0, 1, 0.2f, 4);
        lfo_type = &R.add("nuke.lfo_type", "LFO Forma", 0, 7, 0, 4, true);
        lfo_a = &R.add("nuke.lfo_a", "LFO Attack", 0, 1, 0, 4);
        lfo_d = &R.add("nuke.lfo_d", "LFO Decay", 0, 1, 1, 4);
        lfo_pitch = &R.add("nuke.lfo_pitch", "LFO>Pitch", -1, 1, 0, 5);
        lfo_vol = &R.add("nuke.lfo_vol", "LFO>Volumen", 0, 1, 0, 5);
        params = {osc2_pitch, osc_mix, mode, volume, cutoff, res, f_amt, lfo_filt, a, d, s, r, fa, fd, fs, lfo_osc2,
                  lfo_speed, lfo_type, lfo_a, lfo_d, lfo_pitch, lfo_vol};
      }

      void update() override
      {
        sh.mode = Mode(mode->geti());
        float op = osc2_pitch->get();
        switch (sh.mode) {
          case Mode::None:
          case Mode::Ringmod: sh.osc2_ratio = std::pow(2.f, neighborhoods(op, -3, 3)); break;
          case Mode::Sync: sh.osc2_ratio = 1.f + op * 2.f; break;
          case Mode::AM: sh.osc2_fixed = std::pow(2.f, 10.f * op); break;
        }
        sh.mix1 = 1.f - osc_mix->get();
        sh.mix2 = osc_mix->get();
        sh.cutoff = cutoff->get();
        sh.env_f_amt = f_amt->get();
        sh.lfo_pitch = lfo_pitch->get();
        sh.lfo_vol = lfo_vol->get();
        sh.lfo_filt = lfo_filt->get();
        sh.lfo_osc2 = lfo_osc2->get();
        float ls = lfo_speed->get();
        lfo.freq(ls * ls * 15 + 0.1f);
        lfo_shape = lfo_type->geti();
        for (auto& v : poly.v) {
          v.filter.setResonance(0.1f + 0.6f * res->get());
          v.env.attack(envelope_stage_duration(a->get()));
          v.env.decay(envelope_stage_duration(d->get()));
          v.env.sustain(s->get());
          v.env.release(envelope_stage_duration(r->get()));
          v.env_filter.attack(envelope_stage_duration(fa->get()));
          v.env_filter.decay(4 * envelope_stage_duration(fd->get()));
          v.env_filter.sustain(fs->get());
          v.lfo_env.attack(envelope_stage_duration(lfo_a->get()));
          v.lfo_env.decay(4 * envelope_stage_duration(lfo_d->get()));
          if (lfo_d->get() > 0.99f) v.lfo_env.sustainPoint(1);
          else v.lfo_env.sustainDisable();
        }
      }
      float lfo_take()
      {
        switch (lfo_shape) {
          case 0: return 1.f;
          case 1: return lfo.up();
          case 2: return lfo.down();
          case 3: return lfo.tri();
          case 4: return lfo.sqr();
          case 5: return lfo.sineP9();
          case 6: return lfo.C2();
          case 7: return lfo.S5();
          default: return 0.f;
        }
      }
      void note_on(int n, float) override
      {
        auto* v = poly.on(n);
        v->env.resetSoft();
        v->env_filter.reset();
        v->lfo_env.reset();
      }
      void note_off(int n) override
      {
        poly.off(n, [](Voice& v) {
          v.env.release();
          v.lfo_env.release();
        });
      }
      void all_off() override
      {
        for (int i = 0; i < 6; i++)
          if (poly.note[size_t(i)] >= 0) note_off(poly.note[size_t(i)]);
      }
      float process() override
      {
        sh.lfo_val = lfo_take();
        float out = 0;
        for (auto& v : poly.v)
          if (v.active()) out += v(sh, freq_mul);
        return out;
      }
    };
  } // namespace nuke

  // =====================================================================
  // STRING — Karplus-Strong plucked string (new, not from OTTO)
  // =====================================================================
  namespace str {
    struct Voice {
      DelayLine dl{2400};
      float base = 440, prev = 0, energy = 0;
      bool held = false;
      uint32_t seed = 12345;
      bool active() const { return energy > 1e-5f; }
      float noise()
      {
        seed = seed * 1664525u + 1013904223u;
        return float(int32_t(seed)) / 2147483648.f;
      }
      void pluck(float bright, float vel)
      {
        OnePole lp;
        lp.lp(300 + bright * bright * 12000);
        int n = int(kSR / base) + 2;
        for (int i = 0; i < n; i++) dl.write(lp(noise()) * vel);
        energy = 1;
        held = true;
      }
      float operator()(float fb, float damp, float fmul)
      {
        float d = std::clamp(kSR / (base * fmul) - 1.f, 2.f, 2390.f);
        float out = dl.read(d);
        float f = damp * out + (1 - damp) * prev; // averaging lowpass = string damping
        prev = out;
        dl.write(f * (held ? fb : fb * 0.97f));
        energy = energy * 0.9995f + std::abs(out) * 0.0005f;
        return out;
      }
    };

    struct Engine final : otto::Engine {
      Param *bright, *sustain, *tone;
      Poly<Voice, 6> poly;
      float fb = 0.99f, damp = 0.5f, br = 0.7f;
      Engine()
      {
        title = "STRING";
        pages = {"CUERDA"};
        auto& R = registry();
        bright = &R.add("str.bright", "Brillo", 0, 1, 0.7f, 0);
        sustain = &R.add("str.sustain", "Sustain", 0, 1, 0.8f, 0);
        tone = &R.add("str.tone", "Tono", 0, 1, 0.5f, 0);
        volume = &R.add("str.vol", "Volumen", 0, 1, 0.7f, 0);
        params = {bright, sustain, tone, volume};
      }
      void update() override
      {
        fb = 0.98f + 0.0199f * sustain->get();
        damp = 0.5f + 0.49f * tone->get();
        br = bright->get();
      }
      void note_on(int n, float vel) override { poly.on(n)->pluck(br, 0.4f + 0.6f * vel); }
      void note_off(int n) override
      {
        poly.off(n, [](Voice& v) { v.held = false; });
      }
      void all_off() override
      {
        for (int i = 0; i < 6; i++)
          if (poly.note[size_t(i)] >= 0) note_off(poly.note[size_t(i)]);
      }
      float process() override
      {
        float s = 0;
        for (auto& v : poly.v)
          if (v.active()) s += v(fb, damp, freq_mul);
        return s * 0.6f;
      }
    };
  } // namespace str

  // =====================================================================
  // CLUSTER — 6 detuned oscillators per voice into ladder filter (new)
  // =====================================================================
  namespace cluster {
    constexpr std::array<float, 6> kOff = {-1.f, -0.6f, -0.25f, 0.25f, 0.6f, 1.f};
    struct Voice {
      std::array<float, 6> ph{0, .17f, .33f, .5f, .66f, .83f};
      float base = 440, sub_ph = 0;
      gam::ADSR<> env;
      nuke::MoogLadder filt;
      Voice() { env.finish(); }
      bool active() const { return !env.done(); }
      float operator()(float spread, float shape, float sub, float fmul)
      {
        float s = 0;
        for (size_t k = 0; k < 6; k++) {
          ph[k] += base * fmul * (1 + spread * 0.03f * kOff[k]) / kSR;
          if (ph[k] >= 1) ph[k] -= 1;
          float saw = 2 * ph[k] - 1, pulse = ph[k] < 0.5f ? 1.f : -1.f;
          s += saw + (pulse - saw) * shape;
        }
        sub_ph += base * fmul * 0.5f / kSR;
        if (sub_ph >= 1) sub_ph -= 1;
        s = s / 6 + sub * std::sin(2 * kPi * sub_ph);
        return filt(s) * env();
      }
    };

    struct Engine final : otto::Engine {
      Param *spread, *shape, *cutoff, *res, *sub, *a, *d, *s, *r;
      Poly<Voice, 6> poly;
      float sp = 0, sh = 0, sb = 0;
      Engine()
      {
        title = "CLUSTER";
        pages = {"CLUSTER", "ENV", "FILTRO"};
        auto& R = registry();
        spread = &R.add("cl.spread", "Spread", 0, 1, 0.4f, 0);
        shape = &R.add("cl.shape", "Forma", 0, 1, 0, 0);
        cutoff = &R.add("cl.cutoff", "Cutoff", 0, 1, 0.6f, 0);
        volume = &R.add("cl.vol", "Volumen", 0, 1, 0.6f, 0);
        a = &R.add("cl.a", "Attack", 0, 1, 0.1f, 1);
        d = &R.add("cl.d", "Decay", 0, 1, 0.3f, 1);
        s = &R.add("cl.s", "Sustain", 0, 1, 0.7f, 1);
        r = &R.add("cl.r", "Release", 0, 1, 0.3f, 1);
        res = &R.add("cl.res", "Resonancia", 0, 1, 0.2f, 2);
        sub = &R.add("cl.sub", "Sub Oct", 0, 1, 0, 2);
        params = {spread, shape, cutoff, volume, a, d, s, r, res, sub};
      }
      void update() override
      {
        sp = spread->get();
        sh = shape->get();
        sb = sub->get();
        float fc = 80.f * std::pow(200.f, cutoff->get());
        for (auto& v : poly.v) {
          v.filt.res = 0.1f + 0.6f * res->get();
          v.filt.setCutoff(std::min(fc, 18000.f));
          v.env.attack(envelope_stage_duration(a->get()));
          v.env.decay(envelope_stage_duration(d->get()));
          v.env.sustain(s->get());
          v.env.release(envelope_stage_duration(r->get()));
        }
      }
      void note_on(int n, float) override { poly.on(n)->env.resetSoft(); }
      void note_off(int n) override
      {
        poly.off(n, [](Voice& v) { v.env.release(); });
      }
      void all_off() override
      {
        for (int i = 0; i < 6; i++)
          if (poly.note[size_t(i)] >= 0) note_off(poly.note[size_t(i)]);
      }
      float process() override
      {
        float out = 0;
        for (auto& v : poly.v)
          if (v.active()) out += v(sp, sh, sb, freq_mul);
        return out * 0.3f;
      }
    };
  } // namespace cluster

  // =====================================================================
  // SAMPLER — chromatic sample player, root C4 (new)
  // =====================================================================
  namespace smp {
    struct Voice {
      double pos = 0;
      float base = 440;
      gam::ADSR<> env;
      Voice() { env.finish(); }
      bool active() const { return !env.done(); }
    };

    struct Engine final : otto::Engine {
      Param *start, *end, *loop, *a, *d, *s, *r, *tune, *fine;
      Poly<Voice, 6> poly;
      // double buffer: UI fills the inactive slot, then flips `cur`
      std::array<std::vector<float>, 2> buf;
      std::array<float, 2> sr_ratio{1, 1};
      std::atomic<int> cur{0};
      std::string name = "(interno)";

      Engine()
      {
        title = "SAMPLER";
        pages = {"SAMPLE", "ENV", "AFINAR"};
        auto& R = registry();
        start = &R.add("smp.start", "Inicio", 0, 1, 0, 0);
        end = &R.add("smp.end", "Fin", 0, 1, 1, 0);
        loop = &R.add("smp.loop", "Loop", 0, 1, 1, 0, true);
        volume = &R.add("smp.vol", "Volumen", 0, 1, 0.8f, 0);
        a = &R.add("smp.a", "Attack", 0, 1, 0.05f, 1);
        d = &R.add("smp.d", "Decay", 0, 1, 0.3f, 1);
        s = &R.add("smp.s", "Sustain", 0, 1, 1, 1);
        r = &R.add("smp.r", "Release", 0, 1, 0.25f, 1);
        tune = &R.add("smp.tune", "Semitonos", -24, 24, 0, 2, true);
        fine = &R.add("smp.fine", "Fino", -1, 1, 0, 2);
        params = {start, end, loop, volume, a, d, s, r, tune, fine};
        // built-in sample: soft organ "aah" at C4 with vibrato, loopable
        auto& b = buf[0];
        b.resize(size_t(kSR * 1.5f));
        double ph = 0;
        for (size_t i = 0; i < b.size(); i++) {
          float t = float(i) / kSR;
          ph += 261.63 * (1 + 0.004 * std::sin(2 * kPi * 5 * t)) / kSR;
          float x = 0;
          for (int h = 1; h <= 8; h++) x += std::sin(float(2 * kPi * ph * h)) / float(h * h) * (h == 3 ? 2.f : 1.f);
          b[i] = 0.4f * x * std::min(1.f, t * 20);
        }
      }
      const std::vector<float>& data() const { return buf[size_t(cur.load())]; }

      void update() override
      {
        for (auto& v : poly.v) {
          v.env.attack(envelope_stage_duration(a->get()));
          v.env.decay(envelope_stage_duration(d->get()));
          v.env.sustain(s->get());
          v.env.release(envelope_stage_duration(r->get()));
        }
      }
      void note_on(int n, float) override
      {
        auto* v = poly.on(n);
        v->pos = start->get() * double(data().size());
        v->env.resetSoft();
      }
      void note_off(int n) override
      {
        poly.off(n, [](Voice& v) { v.env.release(); });
      }
      void all_off() override
      {
        for (int i = 0; i < 6; i++)
          if (poly.note[size_t(i)] >= 0) note_off(poly.note[size_t(i)]);
      }
      float process() override
      {
        const auto& b = data();
        if (b.size() < 4) return 0;
        double n = double(b.size());
        double s0 = start->get() * n, s1 = std::max(s0 + 64, end->get() * n);
        s1 = std::min(s1, n - 2);
        bool lp = loop->geti() != 0;
        float tr = std::pow(2.f, (float(tune->geti()) + fine->get()) / 12.f) * sr_ratio[size_t(cur.load())];
        float out = 0;
        for (auto& v : poly.v) {
          if (!v.active()) continue;
          if (v.pos >= s1) {
            if (lp) v.pos = s0 + std::fmod(v.pos - s0, std::max(1.0, s1 - s0));
            else { v.env.finish(); continue; }
          }
          size_t p = size_t(v.pos);
          if (p + 1 >= b.size()) { v.env.finish(); continue; } // sample swapped under us
          float f = float(v.pos - double(p));
          out += (b[p] + (b[p + 1] - b[p]) * f) * v.env();
          v.pos += double(v.base / 261.63f * tr * freq_mul);
        }
        return out * 0.5f;
      }
    };
  } // namespace smp

  bool sampler_load(Engine& e, const std::string& path)
  {
    auto* s = dynamic_cast<smp::Engine*>(&e);
    if (!s) return false;
    std::vector<float> mono;
    unsigned sr = 0;
    if (!read_audio_mono(path, mono, sr)) return false;
    int slot = 1 - s->cur.load();
    s->buf[size_t(slot)] = std::move(mono);
    s->sr_ratio[size_t(slot)] = float(sr) / kSR;
    s->name = path;
    s->cur.store(slot); // ponytail: voices mid-note jump into the new buffer; bounds-checked, no crash
    return true;
  }

  const std::vector<float>* sampler_data(Engine& e)
  {
    auto* s = dynamic_cast<smp::Engine*>(&e);
    return s ? &s->data() : nullptr;
  }

  std::unique_ptr<Engine> make_ottofm() { return std::make_unique<fm::Engine>(); }
  std::unique_ptr<Engine> make_nuke() { return std::make_unique<nuke::Engine>(); }
  std::unique_ptr<Engine> make_string() { return std::make_unique<str::Engine>(); }
  std::unique_ptr<Engine> make_cluster() { return std::make_unique<cluster::Engine>(); }
  std::unique_ptr<Engine> make_sampler() { return std::make_unique<smp::Engine>(); }

} // namespace otto
