#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "sync/sync.h"

struct ExportTrack {
  AudioView audio;
  TimeMap map;          // Relates the reference timeline to this file's time.
  std::string name;     // File name without extension.
  float gain = 1.0f;
  bool in_mix = true;
  int channel_view = -1;  // -1=all, 0=L, 1=R; produces mono WAV when set
};

struct ExportOptions {
  std::filesystem::path out_dir;
  int sample_rate = 48000;
  bool use_range = false;
  double range_start = 0.0;  // Reference timeline seconds.
  double range_end = 0.0;
  bool write_mix = false;
};

// The whole span covered by any track, on the reference timeline.
void timeline_extent(const std::vector<ExportTrack>& tracks, double* start, double* end);

// Writes one WAV per track, all the same length and starting at the same moment,
// so they line up when dropped into any editor. A track that needs no timing or
// rate change is copied without resampling. Originals are only read.
bool export_tracks(const std::vector<ExportTrack>& tracks, const ExportOptions& options, std::atomic<float>* progress,
                   std::string* err);
