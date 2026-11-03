#include "sync/export.h"

#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "audio/wav.h"

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kChunkFrames = 1 << 16;

double track_start(const ExportTrack& t) { return -t.map.offset_seconds / (1.0 + t.map.drift); }

double track_end(const ExportTrack& t) {
  return (t.audio.seconds() - t.map.offset_seconds) / (1.0 + t.map.drift);
}

bool write_silence(WavWriter* wav, int channels, uint64_t frames, std::vector<float>* scratch) {
  std::fill(scratch->begin(), scratch->end(), 0.0f);
  const uint64_t per_chunk = scratch->size() / channels;
  while (frames > 0) {
    uint64_t n = std::min(frames, per_chunk);
    if (!wav->write(scratch->data(), n)) return false;
    frames -= n;
  }
  return true;
}

bool write_scaled(WavWriter* wav, const float* samples, uint64_t frames, int channels, float gain,
                  std::vector<float>* scratch) {
  if (gain == 1.0f) return wav->write(samples, frames);
  const size_t count = size_t(frames) * channels;
  if (scratch->size() < count) scratch->resize(count);
  for (size_t i = 0; i < count; i++) (*scratch)[i] = samples[i] * gain;
  return wav->write(scratch->data(), frames);
}

// Writes the track for one output range. progress_update receives 0 to 1.
template <typename ProgressFn>
bool render_track(const ExportTrack& t, const fs::path& path, int rate, double start, uint64_t total_frames,
                  ProgressFn progress_update, std::string* err) {
  WavWriter wav;
  const int ch = t.audio.channels;
  if (!wav.open(path, rate, ch, err)) return false;
  auto fail = [&]() {
    if (err) *err = wav.error();
    return false;
  };
  std::vector<float> scratch(size_t(kChunkFrames) * ch);
  std::vector<float> gain_scratch;

  // Output frames before this track's first sample are silence.
  const double lead = (track_start(t) - start) * rate;
  const uint64_t lead_frames = lead > 0.0 ? std::min(total_frames, uint64_t(std::ceil(lead))) : 0;
  if (!write_silence(&wav, ch, lead_frames, &scratch)) return fail();
  uint64_t written = lead_frames;

  const double first_seconds =
      t.map.offset_seconds + (1.0 + t.map.drift) * (start + double(lead_frames) / rate);
  const int64_t first_frame = std::max<int64_t>(0, std::llround(first_seconds * t.audio.sample_rate));
  uint64_t read_pos = std::min<uint64_t>(uint64_t(first_frame), t.audio.frames);

  if (t.map.drift == 0.0 && t.audio.sample_rate == rate) {
    while (written < total_frames && read_pos < t.audio.frames) {
      uint64_t n = std::min({kChunkFrames, total_frames - written, t.audio.frames - read_pos});
      if (!write_scaled(&wav, t.audio.samples + read_pos * ch, n, ch, t.gain, &gain_scratch)) return fail();
      read_pos += n;
      written += n;
      progress_update(double(written) / total_frames);
    }
  } else {
    int src_error = 0;
    std::unique_ptr<SRC_STATE, SRC_STATE* (*)(SRC_STATE*)> src(src_new(SRC_SINC_BEST_QUALITY, ch, &src_error),
                                                              src_delete);
    if (!src) {
      if (err) *err = src_strerror(src_error);
      return false;
    }
    const double ratio = double(rate) / (t.audio.sample_rate * (1.0 + t.map.drift));
    bool input_done = false;
    while (written < total_frames) {
      uint64_t input_left = t.audio.frames - read_pos;
      SRC_DATA d = {};
      d.data_in = t.audio.samples + read_pos * ch;
      d.input_frames = long(std::min(input_left, kChunkFrames));
      d.data_out = scratch.data();
      d.output_frames = long(std::min(kChunkFrames, total_frames - written));
      d.end_of_input = input_left <= kChunkFrames ? 1 : 0;
      d.src_ratio = ratio;
      src_error = src_process(src.get(), &d);
      if (src_error) {
        if (err) *err = src_strerror(src_error);
        return false;
      }
      read_pos += uint64_t(d.input_frames_used);
      if (d.output_frames_gen > 0) {
        if (!write_scaled(&wav, scratch.data(), uint64_t(d.output_frames_gen), ch, t.gain, &gain_scratch)) {
          return fail();
        }
        written += uint64_t(d.output_frames_gen);
        progress_update(double(written) / total_frames);
      } else if (d.end_of_input) {
        input_done = true;
        break;
      }
    }
    (void)input_done;
  }

  if (written < total_frames && !write_silence(&wav, ch, total_frames - written, &scratch)) return fail();
  if (!wav.close()) return fail();
  return true;
}

bool write_mix(const std::vector<fs::path>& paths, const std::vector<ExportTrack>& tracks, const fs::path& out_path,
               int rate, std::string* err) {
  std::vector<std::unique_ptr<WavFile>> files;
  std::vector<size_t> sources;
  for (size_t i = 0; i < paths.size(); i++) {
    if (!tracks[i].in_mix) continue;
    auto file = std::make_unique<WavFile>();
    if (!file->open(paths[i], err)) return false;
    files.push_back(std::move(file));
  }
  if (files.empty()) {
    if (err) *err = "Every track is muted, so there is nothing to mix";
    return false;
  }

  const uint64_t frames = files[0]->frames();
  std::vector<float> block(size_t(kChunkFrames) * 2);

  auto mix_block = [&](uint64_t start, uint64_t n) {
    std::fill(block.begin(), block.begin() + size_t(n) * 2, 0.0f);
    for (const auto& file : files) {
      const int ch = file->channels();
      const float* s = file->samples() + start * ch;
      for (uint64_t i = 0; i < n; i++) {
        block[size_t(i) * 2] += s[i * ch];
        block[size_t(i) * 2 + 1] += s[i * ch + (ch > 1 ? 1 : 0)];
      }
    }
  };

  // The first pass finds the peak, so the mix can be scaled down rather than clip.
  float peak = 0.0f;
  for (uint64_t pos = 0; pos < frames; pos += kChunkFrames) {
    uint64_t n = std::min(kChunkFrames, frames - pos);
    mix_block(pos, n);
    for (size_t i = 0; i < size_t(n) * 2; i++) peak = std::max(peak, std::fabs(block[i]));
  }
  const float scale = peak > 0.99f ? 0.99f / peak : 1.0f;

  WavWriter wav;
  if (!wav.open(out_path, rate, 2, err)) return false;
  for (uint64_t pos = 0; pos < frames; pos += kChunkFrames) {
    uint64_t n = std::min(kChunkFrames, frames - pos);
    mix_block(pos, n);
    for (size_t i = 0; i < size_t(n) * 2; i++) block[i] *= scale;
    if (!wav.write(block.data(), n)) {
      if (err) *err = wav.error();
      return false;
    }
  }
  if (!wav.close()) {
    if (err) *err = wav.error();
    return false;
  }
  return true;
}

}  // namespace

void timeline_extent(const std::vector<ExportTrack>& tracks, double* start, double* end) {
  *start = 0.0;
  *end = 0.0;
  bool first = true;
  for (const auto& t : tracks) {
    double a = track_start(t);
    double b = track_end(t);
    *start = first ? a : std::min(*start, a);
    *end = first ? b : std::max(*end, b);
    first = false;
  }
}

bool export_tracks(const std::vector<ExportTrack>& tracks, const ExportOptions& options, std::atomic<float>* progress,
                   std::string* err) {
  if (tracks.empty()) {
    if (err) *err = "No tracks to export";
    return false;
  }
  double start = 0.0, end = 0.0;
  timeline_extent(tracks, &start, &end);
  if (options.use_range) {
    start = options.range_start;
    end = options.range_end;
  }
  if (end <= start) {
    if (err) *err = "The export range is empty";
    return false;
  }

  std::error_code ec;
  fs::create_directories(options.out_dir, ec);
  if (ec) {
    if (err) *err = "Could not create " + options.out_dir.u8string() + ": " + ec.message();
    return false;
  }

  const uint64_t total_frames = uint64_t(std::llround((end - start) * options.sample_rate));
  std::vector<fs::path> paths;
  for (size_t i = 0; i < tracks.size(); i++) {
    fs::path path = options.out_dir / fs::u8path(tracks[i].name + ".wav");
    auto update = [&](double fraction) {
      if (progress) progress->store(float((i + fraction) / tracks.size()));
    };
    if (!render_track(tracks[i], path, options.sample_rate, start, total_frames, update, err)) return false;
    paths.push_back(path);
  }
  if (options.write_mix && !write_mix(paths, tracks, options.out_dir / "mix.wav", options.sample_rate, err)) {
    return false;
  }
  if (progress) progress->store(1.0f);
  return true;
}
