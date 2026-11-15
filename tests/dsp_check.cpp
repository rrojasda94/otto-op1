// Smallest check that fails if the DSP breaks: finite output, sound on note-on, silence after release.
#include "core.hpp"

#include <Gamma/Domain.h>

#undef NDEBUG // keep asserts in Release builds
#include <cassert>
#include <cstdio>
#include <filesystem>

using namespace otto;

static float render(Engine& e, int samples, bool check_finite = true)
{
  float peak = 0;
  for (int i = 0; i < samples; i++) {
    if (i % 64 == 0) e.update();
    float s = e.process();
    if (check_finite && !std::isfinite(s)) { std::printf("NaN/inf in %s\n", e.title.c_str()); std::abort(); }
    peak = std::max(peak, std::abs(s));
  }
  return peak;
}

int main()
{
  gam::sampleRate(kSR);
  auto st = std::make_unique<Studio>();

  for (auto& e : st->synths) {
    e->update();
    e->note_on(60, 0.8f);
    float on = render(*e, int(kSR));
    e->note_off(60);
    render(*e, int(kSR)); // release tail
    float tail = render(*e, int(kSR * 0.1f));
    std::printf("%-8s peak %.3f tail %.6f\n", e->title.c_str(), on, tail);
    assert(on > 0.01f && "engine makes sound");
    assert(tail < 1e-3f && "engine goes silent after release");
  }

  // full studio: drums + euclid + reverb + tape record/playback
  registry().find("fx.rev.mix")->set(0.6f);
  registry().find("fx.delay.mix")->set(0.5f);
  registry().find("tape.bars")->set(1);
  std::vector<float> buf(512);
  // default: no sequence at all -> pressing play alone must not trigger drums
  st->ui_q.push({Ev::Play});
  for (int b = 0; b < 20; b++) st->process(buf.data(), 256);
  for (auto& h : st->pad_hits) assert(h == 0 && "nothing plays by default");
  st->ui_q.push({Ev::Stop});
  st->steps[0][0] = 0x1111;          // kick on beats of module 1
  st->ui_q.push({Ev::ToggleRec});    // starts transport, records track 1
  float peak = 0;
  for (int b = 0; b < int(kSR * 2 / 256); b++) { // 1 bar @120 = 2 s
    st->process(buf.data(), 256);
    for (float s : buf) { assert(std::isfinite(s)); peak = std::max(peak, std::abs(s)); }
  }
  st->ui_q.push({Ev::ToggleRec}); // stop recording, keep playing
  st->process(buf.data(), 256);
  float taped = 0;
  for (float s : st->tape.trk[0]) taped = std::max(taped, std::abs(s));
  std::printf("studio peak %.3f, tape track1 peak %.3f\n", peak, taped);
  assert(peak > 0.01f && taped > 0.01f);

  assert(Euclid::hit(0, 16, 4, 0) && !Euclid::hit(1, 16, 4, 0) && Euclid::hit(4, 16, 4, 0));

  // step pattern triggers pads; mix export writes a file
  st->steps[0][5] = 0xFFFF; // module 1 (selected): tom on every 16th
  st->steps[1][6] = 0xFFFF; // module 2 not selected: must stay silent
  uint32_t before = st->pad_hits[5];
  for (int b = 0; b < 40; b++) st->process(buf.data(), 256); // ~0.2 s at 120 bpm = 1.6 steps
  assert(st->pad_hits[5] > before && "step sequencer hits pad");
  assert(st->pad_hits[6] == 0 && "only the selected module's pattern plays");
  auto wav = (std::filesystem::temp_directory_path() / "otto_mix_check.wav").string();
  assert(st->export_mix(wav) && std::filesystem::file_size(wav) > 1000);
  std::filesystem::remove(wav);

  // unicode path (ñ/á) round-trip: export mix, then load it into a pad and into the sampler
  auto dir = std::filesystem::temp_directory_path() / std::filesystem::path(u8"otto_ñandú_check");
  auto uwav = dir / "mezcla.wav";
  auto u8 = uwav.u8string();
  std::string upath(u8.begin(), u8.end());
  assert(st->export_mix(upath) && "export to unicode path");
  assert(st->drums.load_pad(7, upath) && "pad loads from unicode path");
  assert(sampler_load(*st->synths.back(), upath) && "sampler loads from unicode path");
  std::filesystem::remove_all(dir);

  // melodic sequencer: only notes, no drums -> synth sounds on its own
  st->ui_q.push({Ev::Stop});
  for (auto& pat : st->steps)
    for (auto& s : pat) s = 0;
  for (auto& t : st->tape.mute) t = true;
  st->synth_sel->set(0);
  st->melody[0][0] = 60;
  st->melody[0][4] = 67;
  st->ui_q.push({Ev::Play});
  float mel_peak = 0;
  for (int b = 0; b < 200; b++) {
    st->process(buf.data(), 256);
    for (float s : buf) mel_peak = std::max(mel_peak, std::abs(s));
  }
  std::printf("melody peak %.3f\n", mel_peak);
  assert(mel_peak > 0.01f && "melodic sequence plays");

  // reverse: head moves backwards
  st->tape_rev->set(1);
  size_t p0 = st->tape_pos;
  st->process(buf.data(), 256);
  size_t p1 = st->tape_pos;
  assert((p1 < p0 || p1 > p0 + 1000) && "reverse moves the head backwards (or wraps)");
  st->tape_rev->set(0);

  // tape stop halts transport after the slow-down
  st->ui_q.push({Ev::TapeStop});
  for (int b = 0; b < int(kSR * 0.8f / 256); b++) st->process(buf.data(), 256);
  assert(!st->playing && "tape stop ends playback");
  std::puts("OK");
  return 0;
}
