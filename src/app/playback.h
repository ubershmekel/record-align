#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "audio/wav.h"
#include "sync/sync.h"

struct SDL_AudioStream;

// Live settings for one track. The UI writes them and the audio thread reads them.
struct PlaybackTrack {
  const WavFile* wav = nullptr;
  std::atomic<float> gain{1.0f};
  std::atomic<bool> mute{false};
  std::atomic<bool> solo{false};
  std::atomic<double> offset_seconds{0.0};
  std::atomic<double> drift{0.0};
};

// Mixes tracks to stereo for listening. This is a preview: it uses simple
// interpolation to stay cheap, while export uses the high-quality resampler.
class Playback {
 public:
  ~Playback();

  bool open(std::string* err);
  void close();

  // Pauses first. The tracks must outlive the playback or the next set_tracks call.
  void set_tracks(std::vector<PlaybackTrack*> tracks);
  void play();
  void pause();
  bool playing() const { return playing_.load(); }
  // Seek to a new position. Safe to call from any thread.
  void seek(double seconds);
  double position() const { return position_.load(); }
  void set_stop_at(double seconds) { stop_at_.store(seconds); }

 private:
  static void callback(void* user, SDL_AudioStream* stream, int additional, int total);
  void render(float* stereo, int frames);

  SDL_AudioStream* stream_ = nullptr;
  std::vector<PlaybackTrack*> tracks_;
  std::vector<float> buffer_;
  std::atomic<double> position_{0.0};
  std::atomic<double> stop_at_{0.0};
  std::atomic<bool> playing_{false};
  // Set by seek(); render() applies it atomically so it never gets overwritten.
  static constexpr double kNoSeek = -1e300;
  std::atomic<double> pending_seek_{kNoSeek};
};
