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

void Playback::seek(double seconds) {
  // Just store the target. render() handles the full crossfade (fade-out old
  // audio → jump → fade-in new audio) without touching the stream buffer.
  // SDL_ClearAudioStream was removed: it caused a burp because the hardware
  // device buffer still held old audio at a non-zero amplitude, and our
  // fade-in would start from 0 immediately after — an abrupt amplitude step.
  pending_seek_.store(seconds);
}

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

void Playback::mix(float* stereo, int offset, int frames, double start) const {
  bool any_solo = false;
  for (const PlaybackTrack* t : tracks_) any_solo = any_solo || t->solo.load();

  for (const PlaybackTrack* t : tracks_) {
    if (t->mute.load() || (any_solo && !t->solo.load()) || !t->wav) continue;
    const float gain = t->gain.load();
    const double track_offset = t->offset_seconds.load();
    const double drift = t->drift.load();
    const WavFile& wav = *t->wav;
    const int ch = wav.channels();
    const double rate = wav.sample_rate();
    const int64_t last = int64_t(wav.frames()) - 1;
    const float* samples = wav.samples();
    for (int i = 0; i < frames; i++) {
      const double when = start + double(i) / kOutputRate;
      const double pos = (track_offset + (1.0 + drift) * when) * rate;
      if (pos < 0.0) continue;
      const int64_t index = int64_t(pos);
      if (index >= last) break;
      const float fraction = float(pos - double(index));
      const float* a = samples + index * ch;
      const float* b = a + ch;
      const int right = ch > 1 ? 1 : 0;
      const int cv = t->channel_view.load();
      float* out = stereo + (offset + i) * 2;
      if (cv >= 0 && ch >= 2) {
        const int c = cv < ch ? cv : 0;
        const float v = gain * (a[c] + (b[c] - a[c]) * fraction);
        if (cv == 0) out[0] += v; else out[1] += v;
      } else {
        out[0] += gain * (a[0] + (b[0] - a[0]) * fraction);
        out[1] += gain * (a[right] + (b[right] - a[right]) * fraction);
      }
    }
  }
}

void Playback::render(float* stereo, int frames) {
  std::fill(stereo, stereo + size_t(frames) * 2, 0.0f);

  // While paused: snap to seek position immediately — no audio to crossfade.
  if (!playing_.load()) {
    double sought = pending_seek_.load();
    if (sought != kNoSeek) {
      if (pending_seek_.compare_exchange_strong(sought, kNoSeek)) {
        position_.store(sought);
        fade_out_frames_.store(0);
        fade_in_frames_.store(0);
      }
    }
    return;
  }

  // If a new seek arrived and we're not already fading out, kick off a fade-out.
  // Cancelling any in-progress fade-in prevents the two envelopes from fighting.
  if (pending_seek_.load() != kNoSeek && fade_out_frames_.load() == 0) {
    fade_out_frames_.store(kDeClickFrames);
    fade_in_frames_.store(0);
  }

  int frame_idx = 0;

  // Phase 1: fade-out — render old position with a decreasing amplitude envelope.
  // When the envelope reaches zero the pending seek is applied and we transition
  // into the fade-in phase (which may start within the same batch).
  int fo = fade_out_frames_.load();
  if (fo > 0) {
    int n = std::min(fo, frames);
    double pos = position_.load();
    mix(stereo, 0, n, pos);
    for (int i = 0; i < n; i++) {
      float env = float(fo - 1 - i) / float(kDeClickFrames);
      stereo[i * 2]     *= env;
      stereo[i * 2 + 1] *= env;
    }
    position_.store(pos + double(n) / kOutputRate);
    frame_idx = n;
    int new_fo = fo - n;
    fade_out_frames_.store(new_fo);
    if (new_fo == 0) {
      double sought = pending_seek_.load();
      if (sought != kNoSeek) {
        if (pending_seek_.compare_exchange_strong(sought, kNoSeek)) {
          position_.store(sought);
          fade_in_frames_.store(kDeClickFrames);
        }
      }
    }
  }

  // Phase 2: normal playback with optional fade-in for the remaining frames.
  int remaining = frames - frame_idx;
  if (remaining > 0) {
    double pos = position_.load();
    mix(stereo, frame_idx, remaining, pos);

    int fi = fade_in_frames_.load();
    if (fi > 0) {
      int fade_n = std::min(fi, remaining);
      int fade_start = kDeClickFrames - fi;
      for (int i = 0; i < fade_n; i++) {
        float env = float(fade_start + i) / float(kDeClickFrames);
        stereo[(frame_idx + i) * 2]     *= env;
        stereo[(frame_idx + i) * 2 + 1] *= env;
      }
      fade_in_frames_.store(fi - fade_n);
    }

    position_.store(pos + double(remaining) / kOutputRate);
  }

  for (int i = 0; i < frames * 2; i++) stereo[i] = std::max(-1.0f, std::min(1.0f, stereo[i]));

  if (position_.load() >= stop_at_.load()) playing_.store(false);
}
