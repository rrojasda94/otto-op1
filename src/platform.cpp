// Platform bits: file dialog, where to read resources / write user data, UI font.
#include "platform.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>

std::string open_audio_dialog()
{
  wchar_t buf[2048] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.lpstrFilter = L"Audio (wav, mp3, flac)\0*.wav;*.mp3;*.flac\0Todos\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = 2048;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n > 0 ? n - 1 : 0), '\0');
  WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
  return out;
}

AppPaths app_paths(const std::filesystem::path& exe_dir) { return {exe_dir, exe_dir}; } // portable: next to the exe

const char* ui_font_path() { return "C:\\Windows\\Fonts\\bahnschrift.ttf"; }

#else // macOS

std::string open_audio_dialog()
{
  // ponytail: AppleScript picker via osascript, avoids Objective-C; blocks the UI while open (fine for a modal picker)
  FILE* p = popen("osascript -e 'POSIX path of (choose file of type {\"public.audio\"} with prompt \"Elegir audio\")' 2>/dev/null", "r");
  if (!p) return {};
  char buf[4096] = "";
  std::string out;
  if (std::fgets(buf, sizeof buf, p)) out = buf;
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

AppPaths app_paths(const std::filesystem::path& exe_dir)
{
  namespace fs = std::filesystem;
  // inside an .app bundle: resources in Contents/Resources, user data in Application Support
  if (exe_dir.parent_path().filename() == "Contents" && fs::exists(exe_dir.parent_path() / "Resources")) {
    const char* home = std::getenv("HOME");
    fs::path data = fs::path(home ? home : "/tmp") / "Library" / "Application Support" / "otto-op1";
    std::error_code ec;
    fs::create_directories(data / "samples", ec);
    return {exe_dir.parent_path() / "Resources", data};
  }
  return {exe_dir, exe_dir};
}

const char* ui_font_path() { return "/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf"; }

#endif
