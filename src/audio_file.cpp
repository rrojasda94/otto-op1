// Audio file reading (WAV/MP3/FLAC) with Unicode-safe paths: the file is read through
// std::filesystem (wide paths on Windows) and decoded from memory by dr_libs.
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "core.hpp"

#include <dr_flac.h>
#include <dr_mp3.h>
#include <dr_wav.h>

#include <fstream>
#include <iterator>

namespace otto {

  std::filesystem::path u8path(const std::string& s)
  {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
  }

  bool read_audio(const std::string& utf8_path, std::vector<float>& interleaved, unsigned& channels, unsigned& sr)
  {
    std::ifstream f(u8path(utf8_path), std::ios::binary);
    if (!f) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;

    float* d = nullptr;
    unsigned long long frames = 0;
    {
      drwav_uint64 n = 0;
      d = drwav_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &channels, &sr, &n, nullptr);
      frames = n;
      if (d) {
        interleaved.assign(d, d + frames * channels);
        drwav_free(d, nullptr);
        return true;
      }
    }
    {
      drflac_uint64 n = 0;
      d = drflac_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &channels, &sr, &n, nullptr);
      if (d) {
        interleaved.assign(d, d + n * channels);
        drflac_free(d, nullptr);
        return true;
      }
    }
    {
      drmp3_config cfg{};
      drmp3_uint64 n = 0;
      d = drmp3_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &cfg, &n, nullptr);
      if (d) {
        channels = cfg.channels;
        sr = cfg.sampleRate;
        interleaved.assign(d, d + n * channels);
        drmp3_free(d, nullptr);
        return true;
      }
    }
    return false;
  }

  bool read_audio_mono(const std::string& utf8_path, std::vector<float>& mono, unsigned& sr)
  {
    std::vector<float> in;
    unsigned ch = 0;
    if (!read_audio(utf8_path, in, ch, sr) || ch == 0) return false;
    mono.assign(in.size() / ch, 0.f);
    for (size_t f = 0; f < mono.size(); f++) {
      float s = 0;
      for (unsigned c = 0; c < ch; c++) s += in[f * ch + c];
      mono[f] = s / float(ch);
    }
    return !mono.empty();
  }

} // namespace otto
