#include "audio/import.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <vector>

#include "audio/edits.h"
#include "audio/wav.h"

namespace fs = std::filesystem;

namespace {

std::wstring quote(const std::wstring& argument) {
  std::wstring out = L"\"";
  for (wchar_t c : argument) {
    if (c == L'"') out += L'\\';
    out += c;
  }
  // A trailing backslash would otherwise escape the closing quote.
  if (!argument.empty() && argument.back() == L'\\') out += L'\\';
  out += L'"';
  return out;
}

// Runs a command with no console window, passing its output to on_data as it
// arrives. Returns false if it can't start or exits with an error.
bool run_command(std::wstring command, const std::function<bool(const char*, size_t)>& on_data) {
  SECURITY_ATTRIBUTES inherit = {};
  inherit.nLength = sizeof(inherit);
  inherit.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &inherit, 1 << 20)) return false;
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE | GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ, &inherit,
                           OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nul;
  startup.hStdOutput = write_end;
  startup.hStdError = nul;
  PROCESS_INFORMATION process = {};
  const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                      nullptr, &startup, &process);
  CloseHandle(write_end);
  CloseHandle(nul);
  if (!started) {
    CloseHandle(read_end);
    return false;
  }

  std::vector<char> buffer(1 << 20);
  bool consumer_ok = true;
  for (;;) {
    DWORD count = 0;
    if (!ReadFile(read_end, buffer.data(), DWORD(buffer.size()), &count, nullptr) || count == 0) break;
    if (consumer_ok) consumer_ok = on_data(buffer.data(), count);
  }
  CloseHandle(read_end);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return consumer_ok && exit_code == 0;
}

bool probe_audio(const fs::path& input, int* sample_rate, int* channels) {
  std::string text;
  const std::wstring command =
      L"ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate,channels -of csv=p=0 " +
      quote(input.wstring());
  const bool ok = run_command(command, [&](const char* data, size_t size) {
    text.append(data, size);
    return true;
  });
  return ok && sscanf(text.c_str(), "%d,%d", sample_rate, channels) == 2 && *sample_rate > 0 && *channels > 0;
}

std::string safe_stem(const fs::path& path) {
  std::string out;
  for (char c : path.stem().u8string()) {
    bool bad = strchr("<>:\"/\\|?*", c) != nullptr || (unsigned char)c < 32;
    out += bad ? '_' : c;
  }
  return out.empty() ? "track" : out;
}

}  // namespace

bool ffmpeg_available() {
  auto ignore = [](const char*, size_t) { return true; };
  return run_command(L"ffmpeg -version", ignore) && run_command(L"ffprobe -version", ignore);
}

bool looks_like_video(const fs::path& path) {
  std::string extension = path.extension().u8string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  for (const char* video : {".mp4", ".mov", ".m4v", ".mkv", ".webm", ".avi", ".mts", ".m2ts", ".wmv"}) {
    if (extension == video) return true;
  }
  return false;
}

bool extract_audio(const fs::path& input, const fs::path& output_wav, std::string* err) {
  const std::string name = input.filename().u8string();
  int sample_rate = 0;
  int channels = 0;
  if (!probe_audio(input, &sample_rate, &channels)) {
    if (err) *err = "Could not find any audio in " + name;
    return false;
  }

  WavWriter wav;
  if (!wav.open(output_wav, sample_rate, channels, err)) return false;

  const std::wstring command = L"ffmpeg -v error -nostdin -i " + quote(input.wstring()) +
                               L" -map 0:a:0 -af aresample=async=1:first_pts=0 -f f32le -acodec pcm_f32le -";
  // Reads can split a frame, so hold on to any partial one for the next read.
  std::vector<char> pending;
  const size_t frame_bytes = size_t(channels) * sizeof(float);
  std::vector<float> aligned;
  const bool ok = run_command(command, [&](const char* data, size_t size) {
    pending.insert(pending.end(), data, data + size);
    const size_t frames = pending.size() / frame_bytes;
    if (frames == 0) return true;
    aligned.resize(frames * channels);
    memcpy(aligned.data(), pending.data(), frames * frame_bytes);
    pending.erase(pending.begin(), pending.begin() + frames * frame_bytes);
    return wav.write(aligned.data(), frames);
  });
  if (!wav.close() || !ok) {
    if (err) *err = "ffmpeg could not read the audio in " + name;
    std::error_code ec;
    fs::remove(output_wav, ec);
    return false;
  }
  if (wav.frames() == 0) {
    if (err) *err = "The audio in " + name + " is empty";
    std::error_code ec;
    fs::remove(output_wav, ec);
    return false;
  }
  return true;
}

bool import_files(const std::vector<fs::path>& inputs, const fs::path& base_dir, fs::path* out_dir,
                  std::atomic<float>* progress, std::string* err) {
  if (inputs.empty()) {
    if (err) *err = "No files to import";
    return false;
  }
  if (!ffmpeg_available()) {
    if (err) *err = "Importing needs ffmpeg, which wasn't found. Install it from ffmpeg.org and make sure it's on your PATH.";
    return false;
  }

  time_t now = time(nullptr);
  tm local;
  localtime_s(&local, &now);
  char stamp[64];
  strftime(stamp, sizeof(stamp), "import_%Y-%m-%d_%H-%M-%S", &local);
  const fs::path dir = base_dir / stamp;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    if (err) *err = "Could not create " + dir.u8string() + ": " + ec.message();
    return false;
  }

  SessionEdits edits;
  FILE* sources = _wfopen((dir / "sources.txt").c_str(), L"w");
  for (size_t i = 0; i < inputs.size(); i++) {
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%02zu_", i + 1);
    const std::string stem = prefix + safe_stem(inputs[i]);
    if (!extract_audio(inputs[i], dir / fs::u8path(stem + ".wav"), err)) {
      if (sources) fclose(sources);
      fs::remove_all(dir, ec);
      return false;
    }
    if (sources) fprintf(sources, "%s <- %s\n", stem.c_str(), inputs[i].u8string().c_str());
    if (edits.master_name.empty() && looks_like_video(inputs[i])) edits.master_name = stem;
    TrackEdit track;
    track.name = stem;
    edits.tracks.push_back(std::move(track));
    if (progress) progress->store(float(i + 1) / inputs.size());
  }
  if (sources) fclose(sources);
  if (edits.master_name.empty()) edits.master_name = edits.tracks[0].name;
  save_edits(dir, edits, nullptr);
  *out_dir = dir;
  return true;
}
