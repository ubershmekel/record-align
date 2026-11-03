#include "app/playback.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kOutputRate = 48000;
}

Playback::~Playback() { close(); }

bool Playback::open(std::string* err) {
  if (stream_) return true;
  SDL_AudioSpec spec = {};
  spec.format = SDL_AUDIO_F32;
  spec.channels = 2;
  spec.freq = kOutputRate;
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Playback::callback, this);
  if (!stream_) {
    if (err) *err = std::string("Could not open audio output: ") + SDL_GetError();
    return false;
  }
  SDL_ResumeAudioStreamDevice(stream_);
  return true;
}

void Playback::close() {
  if (stream_) SDL_DestroyAudioStream(stream_);
  stream_ = nullptr;
  playing_.store(false);
}

void Playback::set_tracks(std::vector<PlaybackTrack*> tracks) {
  playing_.store(false);
  if (stream_) SDL_LockAudioStream(stream_);
  tracks_ = std::move(tracks);
  if (stream_) SDL_UnlockAudioStream(stream_);
}

void Playback::play() { playing_.store(true); }

void Playback::pause() { playing_.store(false); }

void Playback::callback(void* user, SDL_AudioStream* stream, int additional, int) {
  auto* self = static_cast<Playback*>(user);
  while (additional > 0) {
    int frames = std::min(additional / int(2 * sizeof(float)), 4096);
    if (frames <= 0) break;
    if (self->buffer_.size() < size_t(frames) * 2) self->buffer_.resize(size_t(frames) * 2);
    self->render(self->buffer_.data(), frames);
    SDL_PutAudioStreamData(stream, self->buffer_.data(), frames * int(2 * sizeof(float)));
    additional -= frames * int(2 * sizeof(float));
  }
}

void Playback::render(float* stereo, int frames) {
  std::fill(stereo, stereo + size_t(frames) * 2, 0.0f);
  if (!playing_.load()) return;

  bool any_solo = false;
  for (const PlaybackTrack* t : tracks_) any_solo = any_solo || t->solo.load();

  const double start = position_.load();
  for (const PlaybackTrack* t : tracks_) {
    if (t->mute.load() || (any_solo && !t->solo.load()) || !t->wav) continue;
    const float gain = t->gain.load();
    const double offset = t->offset_seconds.load();
    const double drift = t->drift.load();
    const WavFile& wav = *t->wav;
    const int ch = wav.channels();
    const double rate = wav.sample_rate();
    const int64_t last = int64_t(wav.frames()) - 1;
    const float* samples = wav.samples();
    for (int i = 0; i < frames; i++) {
      const double when = start + double(i) / kOutputRate;
      const double pos = (offset + (1.0 + drift) * when) * rate;
      if (pos < 0.0) continue;
      const int64_t index = int64_t(pos);
      if (index >= last) break;
      const float fraction = float(pos - double(index));
      const float* a = samples + index * ch;
      const float* b = a + ch;
      const int right = ch > 1 ? 1 : 0;
      stereo[i * 2] += gain * (a[0] + (b[0] - a[0]) * fraction);
      stereo[i * 2 + 1] += gain * (a[right] + (b[right] - a[right]) * fraction);
    }
  }
  for (int i = 0; i < frames * 2; i++) stereo[i] = std::max(-1.0f, std::min(1.0f, stereo[i]));

  const double next = start + double(frames) / kOutputRate;
  position_.store(next);
  if (next >= stop_at_.load()) playing_.store(false);
}
