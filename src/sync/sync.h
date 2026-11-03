#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Works out how two independently clocked recordings line up. Runs after
// recording and never changes the originals.
//
// The result is a straight-line map between each file's own time:
//   other_seconds = offset_seconds + (1 + drift) * ref_seconds

struct AudioView {
  const float* samples = nullptr;  // Interleaved.
  uint64_t frames = 0;
  int channels = 1;
  int sample_rate = 48000;
  double seconds() const { return double(frames) / sample_rate; }
};

struct TimingSample {
  uint64_t frame = 0;
  double host_seconds = 0.0;
};

// host_seconds = intercept + slope * (frame / sample_rate), fitted while ignoring outliers.
struct ClockFit {
  bool valid = false;
  double intercept = 0.0;
  double slope = 1.0;
  double rms_ms = 0.0;
  int samples_used = 0;
};
ClockFit fit_clock(const std::vector<TimingSample>& samples, int sample_rate);

struct TimeMap {
  double offset_seconds = 0.0;
  double drift = 0.0;
};
// Uses capture timestamps, which share a host clock, to relate two tracks.
bool time_map_from_clocks(const ClockFit& ref, const ClockFit& other, TimeMap* out);

enum class SyncMethod {
  None,        // Nothing to go on. The map is left at zero.
  Audio,       // Matched common sounds in both tracks.
  Timestamps,  // No usable common sound, so capture timestamps alone.
};

struct SyncAnchor {
  double ref_seconds = 0.0;
  double lag_seconds = 0.0;   // other_seconds - ref_seconds at this point.
  double strength = 0.0;      // How far the match stands above its noise floor.
  double window_seconds = 0.0;
  bool inlier = false;
};

struct SyncResult {
  TimeMap map;
  SyncMethod method = SyncMethod::None;
  double confidence = 0.0;      // 0 to 1.
  bool drift_measured = false;  // False if matches covered too little time to measure drift.
  bool nonlinear = false;       // Strong matches disagree with any single straight line.
  int anchors_used = 0;
  int anchors_usable = 0;
  double residual_rms_ms = 0.0;
  double matched_span_seconds = 0.0;  // Time between the first and last matches used.
  bool has_timestamps = false;        // Capture timestamps gave a first estimate.
  TimeMap timestamps;
  std::vector<SyncAnchor> anchors;
  std::string message;          // Plain-language explanation for the user.
};

// prior can be null. If progress is set it moves from 0 to 1.
SyncResult align_tracks(const AudioView& ref, const AudioView& other, const TimeMap* prior,
                        std::atomic<float>* progress = nullptr);

const char* sync_method_name(SyncMethod method);
const char* confidence_label(double confidence);
