#include "audio/session.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace fs = std::filesystem;

namespace {

void load_timing(const fs::path& path, SessionTrack* track) {
  FILE* f = _wfopen(path.c_str(), L"r");
  if (!f) return;
  char line[128];
  while (fgets(line, sizeof(line), f)) {
    char kind = 0;
    uint64_t frame = 0;
    double host = 0.0;
    uint64_t count = 0;
    if (sscanf(line, "%c,%" SCNu64 ",%lf,%" SCNu64, &kind, &frame, &host, &count) != 4) continue;
    if (kind == 'T') {
      track->timing.push_back({frame, host});
    } else if (kind == 'G') {
      track->gap_frames += count;
    } else if (kind == 'D') {
      track->dropped_frames += count;
    } else if (kind == 'X') {
      track->discontinuities++;
    }
  }
  fclose(f);
}

}  // namespace

bool load_session(const fs::path& dir, Session* out, std::string* err) {
  out->dir = dir;
  out->tracks.clear();

  std::vector<fs::path> paths;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".wav") paths.push_back(entry.path());
  }
  if (ec) {
    if (err) *err = "Could not read " + dir.u8string() + ": " + ec.message();
    return false;
  }
  if (paths.empty()) {
    if (err) *err = "No WAV files in " + dir.u8string();
    return false;
  }
  std::sort(paths.begin(), paths.end());

  for (const auto& path : paths) {
    SessionTrack track;
    track.wav_path = path;
    track.name = path.stem().u8string();
    track.wav = std::make_unique<WavFile>();
    if (!track.wav->open(path, err)) return false;
    fs::path timing_path = path;
    timing_path.replace_extension(".timing.csv");
    load_timing(timing_path, &track);
    out->tracks.push_back(std::move(track));
  }
  return true;
}
