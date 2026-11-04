#include "platform/files.h"
#include "audio/recorder.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include "audio/ring_buffer.h"
#include "audio/wav.h"

namespace fs = std::filesystem;

namespace {

constexpr double kRingSeconds = 30.0;
constexpr double kTimingIntervalSeconds = 0.25;
constexpr double kStallSeconds = 0.5;
constexpr auto kCheckpointInterval = std::chrono::seconds(2);

struct TimingRow {
  char kind = 'T';  // T timestamp, G gap, D dropped, X OS discontinuity.
  uint64_t frame = 0;
  double host_seconds = 0.0;
  uint64_t count = 0;
};

std::string safe_file_name(const std::string& name) {
  std::string out;
  for (char c : name) {
    bool bad = strchr("<>:\"/\\|?*", c) != nullptr || (unsigned char)c < 32;
    out += bad ? '_' : c;
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  return out.empty() ? "microphone" : out;
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((unsigned char)c < 32) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

struct RecordTrack final : CaptureSink {
  // Capture callbacks must finish before the fields they access are destroyed.
  ~RecordTrack() override { stream.reset(); }

  InputDevice device;
  std::unique_ptr<CaptureStream> stream;
  std::atomic<int> sample_rate{0};
  std::atomic<int> channels{0};
  SpscRing<float> audio;
  SpscRing<TimingRow> rows{4096};
  std::atomic<bool> writing{false};
  std::atomic<bool> in_packet{false};
  std::atomic<float> meter{0.0f};
  std::atomic<uint64_t> frames_written{0};
  std::atomic<uint64_t> gap_frames{0};
  std::atomic<uint64_t> dropped_frames{0};
  std::atomic<uint64_t> discontinuities{0};
  std::atomic<bool> has_error{false};
  std::mutex error_mutex;
  std::string error;

  // Used only by the capture thread.
  bool have_previous = false;
  bool write_started = false;
  uint64_t previous_device_end = 0;
  double previous_host_end = 0.0;
  double next_timing_at = 0.0;
  uint64_t file_frames = 0;
  uint64_t pending_silence = 0;

  // Used only by the disk thread, plus start() and stop() when it isn't running.
  WavWriter wav;
  FILE* timing_file = nullptr;
  fs::path wav_path;
  std::atomic<bool> disk_failed{false};

  void set_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(error_mutex);
    error = message;
    has_error.store(true);
  }

  std::string get_error() {
    std::lock_guard<std::mutex> lock(error_mutex);
    return error;
  }

  void on_format(int rate, int ch) override {
    audio.reset(size_t(rate * ch * kRingSeconds));
    sample_rate.store(rate);
    channels.store(ch);
  }

  void on_error(const std::string& message) override {
    set_error(message);
    writing.store(false);
  }

  void flush_silence(int ch) {
    if (pending_silence == 0) return;
    uint64_t room = audio.write_available() / ch;
    uint64_t n = std::min(pending_silence, room);
    audio.push_zeros(size_t(n) * ch);
    file_frames += n;
    pending_silence -= n;
  }

  void on_packet(const CapturePacket& p) override {
    // stop() pairs with this flag: once it has cleared writing and seen this
    // false, this thread can't touch the ring buffers until it sees writing again.
    in_packet.store(true);
    const int rate = sample_rate.load(std::memory_order_relaxed);
    const int ch = channels.load(std::memory_order_relaxed);

    float peak = 0.0f;
    const size_t count = size_t(p.frames) * ch;
    for (size_t i = 0; i < count; i++) peak = std::max(peak, std::fabs(p.samples[i]));
    if (peak > meter.load(std::memory_order_relaxed)) meter.store(peak, std::memory_order_relaxed);

    // A jump in device position, or a pause that the position doesn't show, means audio is missing.
    uint64_t gap = 0;
    if (have_previous) {
      if (p.device_position > previous_device_end) gap = p.device_position - previous_device_end;
      double host_gap = p.host_seconds - previous_host_end;
      if (gap == 0 && host_gap > kStallSeconds) gap = uint64_t(host_gap * rate);
    }
    have_previous = true;
    previous_device_end = p.device_position + p.frames;
    previous_host_end = p.host_seconds + double(p.frames) / rate;

    if (!writing.load()) {
      write_started = false;
      in_packet.store(false);
      return;
    }

    if (!write_started) {
      write_started = true;
      gap = 0;
      next_timing_at = 0.0;
    } else if (p.discontinuity) {
      discontinuities.fetch_add(1, std::memory_order_relaxed);
      TimingRow row{'X', file_frames, p.host_seconds, 0};
      rows.push(&row, 1);
    }

    if (gap > 0) {
      pending_silence += gap;
      gap_frames.fetch_add(gap, std::memory_order_relaxed);
      TimingRow row{'G', file_frames, p.host_seconds, gap};
      rows.push(&row, 1);
    }

    flush_silence(ch);
    if (pending_silence == 0 && audio.write_available() >= count) {
      if (p.host_seconds >= next_timing_at) {
        TimingRow row{'T', file_frames, p.host_seconds, 0};
        rows.push(&row, 1);
        next_timing_at = p.host_seconds + kTimingIntervalSeconds;
      }
      audio.push(p.samples, count);
      file_frames += p.frames;
    } else {
      pending_silence += p.frames;
      dropped_frames.fetch_add(p.frames, std::memory_order_relaxed);
      TimingRow row{'D', file_frames, p.host_seconds, p.frames};
      rows.push(&row, 1);
    }
    frames_written.store(file_frames, std::memory_order_relaxed);
    in_packet.store(false);
  }

  // Disk thread. Returns true if it moved any data.
  bool drain(std::vector<float>* buffer) {
    bool moved = false;
    const int ch = channels.load();
    size_t whole_frames = buffer->size() / ch * ch;
    for (;;) {
      size_t available = audio.read_available() / ch * ch;
      if (available == 0) break;
      size_t n = audio.pop(buffer->data(), std::min(available, whole_frames));
      if (!disk_failed.load() && !wav.write(buffer->data(), n / ch)) {
        disk_failed.store(true);
        set_error(wav.error());
      }
      moved = true;
    }
    TimingRow row;
    while (rows.pop(&row, 1) == 1) {
      if (timing_file) {
        fprintf(timing_file, "%c,%" PRIu64 ",%.7f,%" PRIu64 "\n", row.kind, row.frame, row.host_seconds, row.count);
      }
      moved = true;
    }
    return moved;
  }

  void checkpoint() {
    if (!disk_failed.load() && !wav.checkpoint()) {
      disk_failed.store(true);
      set_error(wav.error());
    }
    if (timing_file) fflush(timing_file);
  }

  void close_files() {
    if (!wav.close() && !disk_failed.load()) {
      disk_failed.store(true);
      set_error(wav.error());
    }
    if (timing_file) fclose(timing_file);
    timing_file = nullptr;
  }
};

Recorder::Recorder() = default;

Recorder::~Recorder() {
  stop();
  tracks_.clear();
}

bool Recorder::arm(const InputDevice& device, std::string* err) {
  if (recording_) {
    if (err) *err = "Can't change microphones while recording";
    return false;
  }
  if (is_armed(device.id)) return true;
  auto track = std::make_unique<RecordTrack>();
  track->device = device;
  track->stream = open_capture(device.id, track.get(), err);
  if (!track->stream) return false;
  tracks_.push_back(std::move(track));
  return true;
}

CaptureSink* Recorder::arm_sink_for_test(const InputDevice& device) {
  auto track = std::make_unique<RecordTrack>();
  track->device = device;
  CaptureSink* sink = track.get();
  tracks_.push_back(std::move(track));
  return sink;
}

void Recorder::disarm(const std::string& device_id) {
  if (recording_) return;
  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                               [&](const std::unique_ptr<RecordTrack>& t) { return t->device.id == device_id; }),
                tracks_.end());
}

bool Recorder::is_armed(const std::string& device_id) const {
  for (const auto& t : tracks_) {
    if (t->device.id == device_id) return true;
  }
  return false;
}

bool Recorder::start(const fs::path& base_dir, std::string* err) {
  time_t now = time(nullptr);
  tm local;
  local_time(&now, &local);
  char name[64];
  strftime(name, sizeof(name), "%Y-%m-%d_%H-%M-%S", &local);
  return start_in(base_dir / name, err);
}

bool Recorder::start_in(const fs::path& session_dir, std::string* err) {
  if (recording_) return true;
  if (tracks_.empty()) {
    if (err) *err = "Select at least one microphone";
    return false;
  }
  for (const auto& t : tracks_) {
    if (t->has_error.load()) {
      if (err) *err = t->device.name + " is unavailable: " + t->get_error();
      return false;
    }
  }

  std::error_code ec;
  fs::create_directories(session_dir, ec);
  if (ec) {
    if (err) *err = "Could not create " + session_dir.u8string() + ": " + ec.message();
    return false;
  }
  session_dir_ = session_dir;

  int index = 1;
  for (auto& t : tracks_) {
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%02d_", index++);
    std::string stem = prefix + safe_file_name(t->device.name);
    t->wav_path = session_dir / fs::u8path(stem + ".wav");
    if (!t->wav.open(t->wav_path, t->sample_rate.load(), t->channels.load(), err)) {
      for (auto& other : tracks_) other->close_files();
      return false;
    }
    t->timing_file = open_file((session_dir / fs::u8path(stem + ".timing.csv")).c_str(), "w");
    if (t->timing_file) fputs("kind,frame,host_seconds,count\n", t->timing_file);
    t->disk_failed.store(false);
    t->frames_written.store(0);
    t->gap_frames.store(0);
    t->dropped_frames.store(0);
    t->discontinuities.store(0);
    // Safe without a lock: writing is false, so the capture thread leaves these alone.
    while (t->in_packet.load()) std::this_thread::yield();
    t->file_frames = 0;
    t->pending_silence = 0;
  }
  write_session_file();

  stop_disk_.store(false);
  disk_thread_ = std::thread([this] { disk_loop(); });
  started_at_ = std::chrono::steady_clock::now();
  recording_ = true;
  for (auto& t : tracks_) t->writing.store(true);
  return true;
}

void Recorder::stop() {
  if (!recording_) return;
  for (auto& t : tracks_) t->writing.store(false);
  for (auto& t : tracks_) {
    while (t->in_packet.load()) std::this_thread::yield();
  }
  stop_disk_.store(true);
  disk_thread_.join();
  recording_ = false;
  write_session_file();
}

void Recorder::disk_loop() {
  std::vector<float> buffer(1 << 16);
  auto last_checkpoint = std::chrono::steady_clock::now();
  for (;;) {
    bool stopping = stop_disk_.load();
    bool moved = false;
    for (auto& t : tracks_) moved |= t->drain(&buffer);
    auto now = std::chrono::steady_clock::now();
    if (now - last_checkpoint >= kCheckpointInterval) {
      for (auto& t : tracks_) t->checkpoint();
      last_checkpoint = now;
    }
    if (stopping && !moved) break;
    if (!moved) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  for (auto& t : tracks_) t->close_files();
}

void Recorder::write_session_file() {
  FILE* f = open_file((session_dir_ / "session.json").c_str(), "w");
  if (!f) return;
  time_t now = time(nullptr);
  tm utc;
  utc_time(&now, &utc);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
  fprintf(f, "{\n  \"written_at\": \"%s\",\n  \"tracks\": [\n", stamp);
  for (size_t i = 0; i < tracks_.size(); i++) {
    RecordTrack& t = *tracks_[i];
    fprintf(f,
            "    {\"file\": \"%s\", \"device\": \"%s\", \"sample_rate\": %d, \"channels\": %d, "
            "\"raw_mode\": %s, \"frames\": %" PRIu64 ", \"gap_frames\": %" PRIu64 ", \"dropped_frames\": %" PRIu64
            ", \"discontinuities\": %" PRIu64 ", \"error\": \"%s\"}%s\n",
            json_escape(t.wav_path.filename().u8string()).c_str(), json_escape(t.device.name).c_str(),
            t.sample_rate.load(), t.channels.load(), (t.stream && t.stream->raw_mode()) ? "true" : "false",
            t.frames_written.load(), t.gap_frames.load(), t.dropped_frames.load(), t.discontinuities.load(),
            json_escape(t.get_error()).c_str(), i + 1 < tracks_.size() ? "," : "");
  }
  fputs("  ]\n}\n", f);
  fclose(f);
}

double Recorder::elapsed_seconds() const {
  if (!recording_) return 0.0;
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at_).count();
}

std::vector<TrackStatus> Recorder::status() {
  std::vector<TrackStatus> out;
  for (auto& t : tracks_) {
    TrackStatus s;
    s.device_id = t->device.id;
    s.name = t->device.name;
    s.file_name = t->wav_path.filename().u8string();
    s.error = t->get_error();
    s.sample_rate = t->sample_rate.load();
    s.channels = t->channels.load();
    s.raw_mode = t->stream && t->stream->raw_mode();
    s.peak = t->meter.exchange(0.0f);
    s.frames_written = t->frames_written.load();
    s.gap_frames = t->gap_frames.load();
    s.dropped_frames = t->dropped_frames.load();
    s.discontinuities = t->discontinuities.load();
    out.push_back(std::move(s));
  }
  return out;
}
