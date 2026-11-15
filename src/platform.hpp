#pragma once
#include <filesystem>
#include <string>

struct AppPaths {
  std::filesystem::path resources; // read-only defaults: mapping.default.json, profiles/
  std::filesystem::path data;      // user data: mapping.json, audio.json, presets/, proyectos/, samples/
};

/// Native "open audio file" picker; returns UTF-8 path or "" if cancelled.
std::string open_audio_dialog();
AppPaths app_paths(const std::filesystem::path& exe_dir);
/// TE-ish system font for the UI (may not exist; caller falls back to ImGui default)
const char* ui_font_path();
