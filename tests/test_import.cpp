#include <doctest/doctest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <vector>

#include "audio/edits.h"
#include "audio/import.h"
#include "audio/wav.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  explicit TempDir(const char* name) {
    path = fs::temp_directory_path() / (std::string("mydaw_test_") + name);
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void write_test_wav(const fs::path& path, int rate, int channels, size_t frames) {
  std::vector<float> samples(frames * channels);
  for (size_t i = 0; i < samples.size(); i++) samples[i] = float(int(i % 200) - 100) / 200.0f;
  WavWriter wav;
  std::string err;
  REQUIRE(wav.open(path, rate, channels, &err));
  REQUIRE(wav.write(samples.data(), frames));
  REQUIRE(wav.close());
}

}  // namespace

TEST_CASE("video files are recognized by extension") {
  CHECK(looks_like_video("clip.MP4"));
  CHECK(looks_like_video("clip.mov"));
  CHECK_FALSE(looks_like_video("take.wav"));
  CHECK_FALSE(looks_like_video("take.m4a"));
}

TEST_CASE("importing keeps samples, rate and channels, and leaves the original alone") {
  if (!ffmpeg_available()) {
    MESSAGE("Skipped because ffmpeg isn't installed");
    return;
  }
  TempDir dir("import");
  const fs::path input = dir.path / "my take.wav";
  write_test_wav(input, 44100, 2, 44100);
  const auto size_before = fs::file_size(input);

  fs::path session;
  std::string err;
  std::atomic<float> progress{0.0f};
  REQUIRE(import_files({input}, dir.path, &session, &progress, &err));
  CHECK(progress.load() == 1.0f);
  CHECK(fs::file_size(input) == size_before);

  WavFile before, after;
  REQUIRE(before.open(input, &err));
  REQUIRE(after.open(session / "01_my take.wav", &err));
  CHECK(after.sample_rate() == 44100);
  CHECK(after.channels() == 2);
  REQUIRE(after.frames() == before.frames());
  CHECK(std::equal(before.samples(), before.samples() + before.frames() * 2, after.samples()));
  CHECK(fs::exists(session / "sources.txt"));
}

TEST_CASE("a video file becomes the master even when it isn't first") {
  if (!ffmpeg_available()) {
    MESSAGE("Skipped because ffmpeg isn't installed");
    return;
  }
  TempDir dir("import_master");
  const fs::path audio = dir.path / "mic.wav";
  write_test_wav(audio, 48000, 1, 4800);
  // ffmpeg can read a WAV through any name only if told the format, so make a real video.
  const fs::path video = dir.path / "phone.mp4";
  const std::wstring make = L"ffmpeg -v error -nostdin -y -f lavfi -i color=c=black:s=64x64:r=10:d=1 -i \"" +
                            audio.wstring() + L"\" -c:v libx264 -c:a aac -shortest \"" + video.wstring() + L"\"";
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  std::wstring command = make;
  REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                         &process));
  WaitForSingleObject(process.hProcess, INFINITE);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  REQUIRE(fs::exists(video));

  fs::path session;
  std::string err;
  const bool imported = import_files({audio, video}, dir.path, &session, nullptr, &err);
  INFO(err);
  REQUIRE(imported);
  SessionEdits edits;
  REQUIRE(load_edits(session, &edits));
  CHECK(edits.master_name == "02_phone");
  CHECK(edits.tracks.size() == 2);
}

TEST_CASE("a file with no audio fails with a clear message and leaves nothing behind") {
  if (!ffmpeg_available()) {
    MESSAGE("Skipped because ffmpeg isn't installed");
    return;
  }
  TempDir dir("import_bad");
  const fs::path bad = dir.path / "notes.txt";
  FILE* f = _wfopen(bad.c_str(), L"w");
  REQUIRE(f);
  fputs("not audio\n", f);
  fclose(f);

  const fs::path base = dir.path / "out";
  fs::path session;
  std::string err;
  CHECK_FALSE(import_files({bad}, base, &session, nullptr, &err));
  CHECK(err.find("notes.txt") != std::string::npos);
  CHECK(fs::is_empty(base));
}
