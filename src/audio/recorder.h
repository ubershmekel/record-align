#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/capture.h"

struct RecordTrack;

struct TrackStatus {
  std::string device_id;
  std::string name;
  std::string file_name;
  std::string error;
  int sample_rate = 0;
  int channels = 0;
  bool raw_mode = false;
  float peak = 0.0f;              // Highest level since the last status() call.
  uint64_t frames_written = 0;    // Including silence inserted for gaps.
  uint64_t gap_frames = 0;        // Silence inserted because the device skipped ahead.
  uint64_t dropped_frames = 0;    // Audio lost because the disk fell behind. Replaced by silence.
  uint64_t discontinuities = 0;   // Glitches reported by the OS.
};

// Each armed device captures continuously, driving its level meter, and writes
// to disk only between start() and stop(). Audio is passed from each capture
// thread through a ring buffer to one disk thread, so a slow disk can't stall
// capture. Anything that goes missing is replaced with silence and logged, so
// each file's timeline stays true to its device clock.
class Recorder {
 public:
  Recorder();
  ~Recorder();

  bool arm(const InputDevice& device, std::string* err);
  void disarm(const std::string& device_id);
  bool is_armed(const std::string& device_id) const;

  // Creates a new timestamped folder inside base_dir.
  bool start(const std::filesystem::path& base_dir, std::string* err);
  // Records into exactly this folder. Used by start() and tests.
  bool start_in(const std::filesystem::path& session_dir, std::string* err);
  void stop();

  bool recording() const { return recording_; }
  double elapsed_seconds() const;
  const std::filesystem::path& session_dir() const { return session_dir_; }
  std::vector<TrackStatus> status();

  // Tests use this to record from a fake source instead of a real device.
  CaptureSink* arm_sink_for_test(const InputDevice& device);

 private:
  void disk_loop();
  void write_session_file();

  std::vector<std::unique_ptr<RecordTrack>> tracks_;
  std::thread disk_thread_;
  std::atomic<bool> stop_disk_{false};
  bool recording_ = false;
  std::chrono::steady_clock::time_point started_at_;
  std::filesystem::path session_dir_;
};
