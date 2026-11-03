#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

// Streams 32-bit float WAV to disk. checkpoint() keeps the header valid during
// recording so a crash loses only the last few seconds. Files that outgrow the
// 4 GB RIFF limit become RF64, using a JUNK chunk reserved in the header.
class WavWriter {
 public:
  WavWriter() = default;
  ~WavWriter();
  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  bool open(const std::filesystem::path& path, int sample_rate, int channels, std::string* err);
  bool write(const float* interleaved, uint64_t frames);
  bool checkpoint();
  bool close();

  bool is_open() const { return file_ != nullptr; }
  uint64_t frames() const { return frames_; }
  const std::string& error() const { return error_; }

  uint64_t rf64_threshold = 0xFFFFFFFFull - 4096;  // Lowered by tests.

 private:
  bool patch_header();
  bool fail(const char* what);

  FILE* file_ = nullptr;
  std::filesystem::path path_;
  std::string error_;
  uint64_t frames_ = 0;
  int channels_ = 0;
  bool rf64_ = false;
};

// Read-only memory-mapped view of a float WAV. Nothing is loaded into memory up
// front, so hour-long files are fine. Repairs the size of files left by a crash.
class WavFile {
 public:
  WavFile() = default;
  ~WavFile();
  WavFile(const WavFile&) = delete;
  WavFile& operator=(const WavFile&) = delete;

  bool open(const std::filesystem::path& path, std::string* err);
  void close();

  const float* samples() const { return samples_; }
  uint64_t frames() const { return frames_; }
  int sample_rate() const { return sample_rate_; }
  int channels() const { return channels_; }
  double seconds() const { return sample_rate_ ? double(frames_) / sample_rate_ : 0.0; }
  bool recovered() const { return recovered_; }

 private:
  void* file_handle_ = nullptr;
  void* mapping_ = nullptr;
  const uint8_t* view_ = nullptr;
  const float* samples_ = nullptr;
  uint64_t frames_ = 0;
  int sample_rate_ = 0;
  int channels_ = 0;
  bool recovered_ = false;
};
