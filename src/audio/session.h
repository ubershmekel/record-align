#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "audio/wav.h"
#include "sync/sync.h"

struct SessionTrack {
  std::filesystem::path wav_path;
  std::string name;
  std::unique_ptr<WavFile> wav;
  std::vector<TimingSample> timing;
  uint64_t gap_frames = 0;
  uint64_t dropped_frames = 0;
  uint64_t discontinuities = 0;
};

struct Session {
  std::filesystem::path dir;
  std::vector<SessionTrack> tracks;
};

// Opens every WAV in the folder, with its .timing.csv if it has one.
bool load_session(const std::filesystem::path& dir, Session* out, std::string* err);
