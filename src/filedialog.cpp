// Native Windows "open file" dialog for audio files. Kept separate so <windows.h> stays out of main.cpp.
#include <windows.h>
#include <commdlg.h>

#include <string>

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
