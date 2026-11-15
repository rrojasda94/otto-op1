#pragma once
#include "core.hpp"

#include <deque>
#include <mutex>

class RtMidiIn;

namespace otto {

  struct Binding {
    int ch = 0;    // 1-16, 0 = any
    int num = 0;   // CC number or note
    std::string target; // param id, "enc1".."enc4", or "action:<name>"
    bool relative = false;
    int last = -1, same = 0; // relative auto-detect: endless encoders repeat 63/65
  };

  /// MIDI input (all ports) + MiniLab mapping. Callbacks run on RtMidi threads.
  struct Midi {
    explicit Midi(Studio& s);
    ~Midi();
    void open_all();
    /// user mapping file; remembers the path for save() even when the file does not exist yet
    bool load(const std::string& path);
    bool save();
    /// add bindings from a profile for CC/note numbers the user has not bound
    void merge_defaults(const std::string& path);
    /// replace all bindings with a controller profile and save it as the user's default
    bool apply_profile(const std::string& path);
    /// profile file whose "port_match" appears in a connected port name ("" if none)
    std::string detect_profile(const std::filesystem::path& dir);
    /// profile file whose "profile" name matches (for merging defaults)
    static std::string find_profile(const std::filesystem::path& dir, const std::string& name);

    std::string profile;          // name of the controller profile the mapping came from
    std::atomic<bool> dirty{false}; // set on learn / edit; the UI saves

    std::mutex mtx; // guards everything below + Studio::midi_q producer side
    std::vector<std::string> ports;
    std::vector<Binding> cc, notes;
    int drum_ch = 10, drum_base = 36;
    std::string learn;
    std::deque<std::string> log;

    void on_msg(const unsigned char* m, size_t n);
    void action(const std::string& a);
    Param* resolve(const std::string& target);

  private:
    Studio& st;
    std::vector<std::unique_ptr<RtMidiIn>> ins;
    std::string path_;
    void push_log(std::string s);
    bool parse(const std::string& file); // fills bindings (caller holds no lock)
  };

} // namespace otto
