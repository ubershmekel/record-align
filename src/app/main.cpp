// Desktop UI: pick microphones, record, then align, listen and export.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/playback.h"
#include "audio/capture.h"
#include "audio/recorder.h"
#include "audio/session.h"
#include "sync/export.h"
#include "sync/sync.h"

namespace fs = std::filesystem;

namespace {

const ImVec4 kRed(0.90f, 0.30f, 0.27f, 1.0f);
const ImVec4 kAmber(0.92f, 0.68f, 0.20f, 1.0f);
const ImVec4 kGreen(0.35f, 0.75f, 0.40f, 1.0f);
const ImVec4 kMuted(0.62f, 0.64f, 0.68f, 1.0f);

constexpr int kPeakBucketFrames = 256;
constexpr int kCoarsePeakFactor = 16;

std::string format_time(double seconds) {
  if (seconds < 0.0) seconds = 0.0;
  int total = int(seconds);
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
  return buffer;
}

std::string format_time_precise(double seconds) {
  const char* sign = seconds < 0.0 ? "-" : "";
  seconds = std::fabs(seconds);
  int minutes = int(seconds / 60.0);
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%s%d:%06.3f", sign, minutes, seconds - minutes * 60);
  return buffer;
}

// Min/max summaries for drawing long files quickly.
struct TrackPeaks {
  std::vector<float> minimum;
  std::vector<float> maximum;
  std::vector<float> coarse_minimum;
  std::vector<float> coarse_maximum;
};

void build_peaks(const WavFile& wav, TrackPeaks* out, std::atomic<bool>* cancel) {
  const int ch = wav.channels();
  const uint64_t buckets = wav.frames() / kPeakBucketFrames + 1;
  out->minimum.assign(size_t(buckets), 0.0f);
  out->maximum.assign(size_t(buckets), 0.0f);
  for (uint64_t b = 0; b < buckets; b++) {
    if (cancel->load()) return;
    const uint64_t first = b * kPeakBucketFrames;
    const uint64_t last = std::min<uint64_t>(wav.frames(), first + kPeakBucketFrames);
    float lo = 0.0f, hi = 0.0f;
    const float* s = wav.samples() + first * ch;
    for (uint64_t i = 0; i < (last - first) * ch; i++) {
      lo = std::min(lo, s[i]);
      hi = std::max(hi, s[i]);
    }
    out->minimum[size_t(b)] = lo;
    out->maximum[size_t(b)] = hi;
  }
  const size_t coarse = out->minimum.size() / kCoarsePeakFactor + 1;
  out->coarse_minimum.assign(coarse, 0.0f);
  out->coarse_maximum.assign(coarse, 0.0f);
  for (size_t i = 0; i < out->minimum.size(); i++) {
    size_t c = i / kCoarsePeakFactor;
    out->coarse_minimum[c] = std::min(out->coarse_minimum[c], out->minimum[i]);
    out->coarse_maximum[c] = std::max(out->coarse_maximum[c], out->maximum[i]);
  }
}

struct EditTrack {
  PlaybackTrack playback;
  float gain_db = 0.0f;
  float offset_ms = 0.0f;  // Shown in the UI. These two are only the editable form of the time map.
  float drift_ppm = 0.0f;
  SyncResult sync;
  bool has_sync = false;
  TrackPeaks peaks;
  std::atomic<bool> peaks_ready{false};
};

struct App {
  SDL_Window* window = nullptr;

  // Recording
  std::vector<InputDevice> devices;
  Recorder recorder;
  std::string device_error;
  std::string record_error;
  std::string base_dir;
  std::vector<float> meter_hold;
  std::vector<TrackStatus> status;
  fs::path last_session;
  uint64_t disk_free_bytes = 0;
  double disk_checked_at = -100.0;

  // Editing
  Session session;
  std::vector<std::unique_ptr<EditTrack>> tracks;
  std::string edit_error;
  Playback playback;
  std::thread peak_thread;
  std::atomic<bool> peak_cancel{false};
  bool aligned = true;
  double timeline_start = 0.0;
  double timeline_end = 1.0;
  double view_start = 0.0;
  double view_seconds = 1.0;
  double in_point = 0.0;
  double out_point = 1.0;
  bool trimmed = false;

  // Background jobs
  std::thread job_thread;
  std::atomic<bool> job_running{false};
  std::atomic<float> job_progress{0.0f};
  std::string job_label;
  std::string job_message;
  bool job_failed = false;
  std::mutex job_mutex;
  std::vector<std::pair<size_t, SyncResult>> job_sync_results;
  bool job_sync_done = false;
  bool write_mix = false;

  // Folder dialogs may answer from another thread.
  std::mutex dialog_mutex;
  std::string dialog_record_dir;
  std::string dialog_session_dir;

  int tab_request = 0;  // 1 selects Record, 2 selects Edit.
};

AudioView view_of(const WavFile& wav) {
  AudioView v;
  v.samples = wav.samples();
  v.frames = wav.frames();
  v.channels = wav.channels();
  v.sample_rate = wav.sample_rate();
  return v;
}

void finish_job(App& app) {
  if (app.job_thread.joinable()) app.job_thread.join();
}

void refresh_devices(App& app) {
  app.device_error.clear();
  app.devices = list_input_devices(&app.device_error);
  if (app.devices.empty() && app.device_error.empty()) app.device_error = "No microphones found.";
}

void update_timeline(App& app) {
  std::vector<ExportTrack> extents;
  for (auto& t : app.tracks) {
    ExportTrack e;
    e.audio = view_of(*t->playback.wav);
    e.map.offset_seconds = t->playback.offset_seconds.load();
    e.map.drift = t->playback.drift.load();
    extents.push_back(e);
  }
  timeline_extent(extents, &app.timeline_start, &app.timeline_end);
  if (app.timeline_end <= app.timeline_start) app.timeline_end = app.timeline_start + 1.0;
  if (!app.trimmed) {
    app.in_point = app.timeline_start;
    app.out_point = app.timeline_end;
  }
  app.playback.set_stop_at(app.timeline_end);
}

// Pushes the UI's offset and drift values to playback, or zero while comparing against unaligned.
void apply_alignment(App& app) {
  for (auto& t : app.tracks) {
    t->playback.offset_seconds.store(app.aligned ? double(t->offset_ms) / 1000.0 : 0.0);
    t->playback.drift.store(app.aligned ? double(t->drift_ppm) * 1e-6 : 0.0);
  }
  update_timeline(app);
}

void close_session(App& app) {
  app.playback.set_tracks({});
  app.peak_cancel.store(true);
  if (app.peak_thread.joinable()) app.peak_thread.join();
  app.tracks.clear();
  app.session = Session();
}

void load_session_into_editor(App& app, const fs::path& dir) {
  if (app.job_running.load()) return;
  finish_job(app);
  close_session(app);
  app.edit_error.clear();
  app.job_message.clear();
  if (!load_session(dir, &app.session, &app.edit_error)) {
    app.session = Session();
    return;
  }
  for (auto& st : app.session.tracks) {
    auto t = std::make_unique<EditTrack>();
    t->playback.wav = st.wav.get();
    app.tracks.push_back(std::move(t));
  }
  app.trimmed = false;
  apply_alignment(app);
  app.view_start = app.timeline_start;
  app.view_seconds = app.timeline_end - app.timeline_start;

  std::vector<PlaybackTrack*> pointers;
  for (auto& t : app.tracks) pointers.push_back(&t->playback);
  if (app.playback.open(&app.edit_error)) {
    app.playback.set_tracks(pointers);
    app.playback.seek(app.timeline_start);
  }

  app.peak_cancel.store(false);
  app.peak_thread = std::thread([&app] {
    for (auto& t : app.tracks) {
      build_peaks(*t->playback.wav, &t->peaks, &app.peak_cancel);
      if (app.peak_cancel.load()) return;
      t->peaks_ready.store(true);
    }
  });
}

void start_sync_job(App& app) {
  if (app.job_running.load() || app.tracks.size() < 2) return;
  finish_job(app);
  app.job_running.store(true);
  app.job_progress.store(0.0f);
  app.job_label = "Aligning tracks";
  app.job_message.clear();
  app.job_failed = false;
  app.job_thread = std::thread([&app] {
    const SessionTrack& ref = app.session.tracks[0];
    const ClockFit ref_clock = fit_clock(ref.timing, ref.wav->sample_rate());
    std::vector<std::pair<size_t, SyncResult>> results;
    for (size_t i = 1; i < app.session.tracks.size(); i++) {
      const SessionTrack& t = app.session.tracks[i];
      TimeMap prior;
      const bool has_prior = time_map_from_clocks(ref_clock, fit_clock(t.timing, t.wav->sample_rate()), &prior);
      SyncResult r = align_tracks(view_of(*ref.wav), view_of(*t.wav), has_prior ? &prior : nullptr, &app.job_progress);
      results.emplace_back(i, std::move(r));
    }
    std::lock_guard<std::mutex> lock(app.job_mutex);
    app.job_sync_results = std::move(results);
    app.job_sync_done = true;
    app.job_running.store(false);
  });
}

void collect_sync_results(App& app) {
  std::lock_guard<std::mutex> lock(app.job_mutex);
  if (!app.job_sync_done) return;
  app.job_sync_done = false;
  for (auto& [index, result] : app.job_sync_results) {
    EditTrack& t = *app.tracks[index];
    t.offset_ms = float(result.map.offset_seconds * 1000.0);
    t.drift_ppm = float(result.map.drift * 1e6);
    t.sync = std::move(result);
    t.has_sync = true;
  }
  app.job_sync_results.clear();
  app.aligned = true;
  apply_alignment(app);
  app.view_start = app.timeline_start;
  app.view_seconds = app.timeline_end - app.timeline_start;
}

void start_export_job(App& app) {
  if (app.job_running.load() || app.tracks.empty()) return;
  finish_job(app);

  std::vector<ExportTrack> jobs;
  bool any_solo = false;
  for (auto& t : app.tracks) any_solo = any_solo || t->playback.solo.load();
  for (size_t i = 0; i < app.tracks.size(); i++) {
    EditTrack& t = *app.tracks[i];
    ExportTrack e;
    e.audio = view_of(*t.playback.wav);
    e.map.offset_seconds = double(t.offset_ms) / 1000.0;
    e.map.drift = double(t.drift_ppm) * 1e-6;
    e.name = app.session.tracks[i].name;
    e.gain = t.playback.gain.load();
    e.in_mix = !t.playback.mute.load() && (!any_solo || t.playback.solo.load());
    jobs.push_back(std::move(e));
  }
  ExportOptions options;
  options.out_dir = app.session.dir / "aligned";
  options.sample_rate = app.session.tracks[0].wav->sample_rate();
  options.use_range = app.trimmed;
  options.range_start = app.in_point;
  options.range_end = app.out_point;
  options.write_mix = app.write_mix;

  app.job_running.store(true);
  app.job_progress.store(0.0f);
  app.job_label = "Exporting";
  app.job_message.clear();
  app.job_failed = false;
  app.job_thread = std::thread([&app, jobs = std::move(jobs), options] {
    std::string err;
    bool ok = export_tracks(jobs, options, &app.job_progress, &err);
    std::lock_guard<std::mutex> lock(app.job_mutex);
    app.job_failed = !ok;
    app.job_message = ok ? "Exported to " + options.out_dir.u8string() : err;
    app.job_running.store(false);
  });
}

void SDLCALL on_record_folder(void* user, const char* const* files, int) {
  App& app = *static_cast<App*>(user);
  if (!files || !files[0]) return;
  std::lock_guard<std::mutex> lock(app.dialog_mutex);
  app.dialog_record_dir = files[0];
}

void SDLCALL on_session_folder(void* user, const char* const* files, int) {
  App& app = *static_cast<App*>(user);
  if (!files || !files[0]) return;
  std::lock_guard<std::mutex> lock(app.dialog_mutex);
  app.dialog_session_dir = files[0];
}

void draw_meter(float level, float hold, const ImVec2& size) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  draw->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(40, 42, 48, 255), 3.0f);

  auto to_x = [&](float linear) {
    float db = linear > 1e-5f ? 20.0f * std::log10(linear) : -100.0f;
    float fraction = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
    return p.x + fraction * size.x;
  };
  ImU32 color = level >= 0.99f ? IM_COL32(230, 75, 70, 255)
                               : (level > 0.5f ? IM_COL32(235, 175, 50, 255) : IM_COL32(85, 190, 100, 255));
  draw->AddRectFilled(p, ImVec2(to_x(level), p.y + size.y), color, 3.0f);
  float hx = to_x(hold);
  draw->AddLine(ImVec2(hx, p.y), ImVec2(hx, p.y + size.y), IM_COL32(235, 235, 235, 220), 1.5f);
}

void draw_record_tab(App& app) {
  const bool recording = app.recorder.recording();

  ImGui::SeparatorText("Microphones");
  ImGui::BeginDisabled(recording);
  if (ImGui::Button("Refresh list")) refresh_devices(app);
  ImGui::EndDisabled();
  if (!app.device_error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kRed, "%s", app.device_error.c_str());
  }

  if (app.meter_hold.size() != app.devices.size()) app.meter_hold.assign(app.devices.size(), 0.0f);

  if (ImGui::BeginTable("devices", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("format", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("level", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    for (size_t i = 0; i < app.devices.size(); i++) {
      const InputDevice& device = app.devices[i];
      ImGui::PushID(int(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      bool armed = app.recorder.is_armed(device.id);
      ImGui::BeginDisabled(recording);
      if (ImGui::Checkbox(device.name.c_str(), &armed)) {
        app.record_error.clear();
        if (armed) {
          std::string err;
          if (!app.recorder.arm(device, &err)) app.record_error = device.name + ": " + err;
        } else {
          app.recorder.disarm(device.id);
        }
      }
      ImGui::EndDisabled();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(kMuted, "%.1f kHz, %s", device.sample_rate / 1000.0,
                         device.channels == 1 ? "mono" : (device.channels == 2 ? "stereo" : "multi"));
      ImGui::TableNextColumn();
      const TrackStatus* status = nullptr;
      for (const auto& s : app.status) {
        if (s.device_id == device.id) status = &s;
      }
      if (status) {
        float& hold = app.meter_hold[i];
        hold = std::max(status->peak, hold * 0.97f);
        draw_meter(status->peak, hold, ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()));
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (!app.record_error.empty()) ImGui::TextColored(kRed, "%s", app.record_error.c_str());

  ImGui::SeparatorText("Save to");
  ImGui::BeginDisabled(recording);
  if (ImGui::Button("Choose folder...")) {
    SDL_ShowOpenFolderDialog(on_record_folder, &app, app.window, app.base_dir.c_str(), false);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextUnformatted(app.base_dir.c_str());
  const double now = double(SDL_GetTicks()) / 1000.0;
  if (now - app.disk_checked_at > 5.0) {
    std::error_code ec;
    fs::path existing = fs::u8path(app.base_dir);
    while (!fs::exists(existing, ec) && existing.has_parent_path() && existing != existing.parent_path()) {
      existing = existing.parent_path();
    }
    auto space = fs::space(existing, ec);
    app.disk_free_bytes = ec ? 0 : uint64_t(space.available);
    app.disk_checked_at = now;
  }
  uint64_t bytes_per_second = 0;
  for (const auto& s : app.status) bytes_per_second += uint64_t(s.sample_rate) * s.channels * 4;
  if (bytes_per_second > 0 && app.disk_free_bytes > 0) {
    double hours = double(app.disk_free_bytes) / bytes_per_second / 3600.0;
    ImGui::TextColored(hours < 1.0 ? kAmber : kMuted, "%.1f GB free, room for about %.1f hours with these microphones",
                       double(app.disk_free_bytes) / 1e9, hours);
  }

  ImGui::Spacing();
  ImGui::Spacing();
  if (!recording) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.24f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.14f, 0.14f, 1.0f));
    ImGui::BeginDisabled(app.status.empty());
    if (ImGui::Button("Start recording", ImVec2(180.0f * ImGui::GetStyle().FontScaleDpi, 44.0f * ImGui::GetStyle().FontScaleDpi))) {
      app.record_error.clear();
      std::string err;
      if (!app.recorder.start(fs::u8path(app.base_dir), &err)) app.record_error = err;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    if (app.status.empty()) {
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(kMuted, "Tick at least one microphone to start.");
    }
  } else {
    if (ImGui::Button("Stop recording", ImVec2(180.0f * ImGui::GetStyle().FontScaleDpi, 44.0f * ImGui::GetStyle().FontScaleDpi))) {
      app.recorder.stop();
      app.last_session = app.recorder.session_dir();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kRed, "RECORDING");
    ImGui::Text("%s", format_time(app.recorder.elapsed_seconds()).c_str());
    ImGui::EndGroup();
  }

  if (recording) {
    ImGui::TextColored(kMuted, "For the best sync, clap three times near all the microphones now, and again before stopping.");
  }

  if (!app.status.empty() && (recording || !app.last_session.empty())) {
    ImGui::Spacing();
    if (ImGui::BeginTable("tracks", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthStretch, 3.0f);
      ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Gaps", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Lost", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 3.0f);
      ImGui::TableHeadersRow();
      for (const auto& s : app.status) {
        const double rate = std::max(1, s.sample_rate);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(format_time(double(s.frames_written) / rate).c_str());
        ImGui::TableNextColumn();
        if (s.gap_frames > 0) {
          ImGui::TextColored(kAmber, "%.2f s", double(s.gap_frames) / rate);
        } else {
          ImGui::TextColored(kMuted, "none");
        }
        ImGui::TableNextColumn();
        if (s.dropped_frames > 0) {
          ImGui::TextColored(kRed, "%.2f s", double(s.dropped_frames) / rate);
        } else {
          ImGui::TextColored(kMuted, "none");
        }
        ImGui::TableNextColumn();
        if (!s.error.empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, kRed);
          ImGui::TextWrapped("%s", s.error.c_str());
          ImGui::PopStyleColor();
        } else {
          ImGui::TextColored(recording ? kGreen : kMuted, recording ? "Recording" : "Saved");
        }
      }
      ImGui::EndTable();
    }
  }

  if (!recording && !app.last_session.empty()) {
    ImGui::Spacing();
    ImGui::TextUnformatted(("Saved to " + app.last_session.u8string()).c_str());
    if (ImGui::Button("Open in editor")) {
      load_session_into_editor(app, app.last_session);
      app.tab_request = 2;
    }
  }
}

// Converts min/max at the mapped file position into a waveform column.
void draw_waveform(const EditTrack& t, const ImVec2& p, const ImVec2& size, double view_start, double view_seconds,
                   ImDrawList* draw, bool muted) {
  const WavFile& wav = *t.playback.wav;
  const double rate = wav.sample_rate();
  const double offset = t.playback.offset_seconds.load();
  const double drift = t.playback.drift.load();
  const float mid = p.y + size.y * 0.5f;
  const float scale = size.y * 0.48f;
  const ImU32 color = muted ? IM_COL32(95, 100, 110, 255) : IM_COL32(95, 170, 225, 255);
  if (!t.peaks_ready.load()) {
    draw->AddText(ImVec2(p.x + 8.0f, p.y + 6.0f), IM_COL32(160, 165, 175, 255), "Reading waveform...");
    return;
  }
  const int columns = int(size.x);
  for (int x = 0; x < columns; x++) {
    const double t0 = view_start + view_seconds * x / columns;
    const double t1 = view_start + view_seconds * (x + 1) / columns;
    const double f0 = (offset + (1.0 + drift) * t0) * rate;
    const double f1 = (offset + (1.0 + drift) * t1) * rate;
    if (f1 < 0.0 || f0 >= double(wav.frames())) continue;
    const uint64_t first = uint64_t(std::max(0.0, f0));
    const uint64_t last = std::min<uint64_t>(wav.frames(), uint64_t(std::max(0.0, f1)) + 1);
    float lo = 0.0f, hi = 0.0f;
    const uint64_t span = last > first ? last - first : 1;
    if (span < kPeakBucketFrames) {
      const int ch = wav.channels();
      const float* s = wav.samples() + first * ch;
      for (uint64_t i = 0; i < span * ch; i++) {
        lo = std::min(lo, s[i]);
        hi = std::max(hi, s[i]);
      }
    } else if (span < uint64_t(kPeakBucketFrames) * kCoarsePeakFactor * 2) {
      size_t b0 = size_t(first / kPeakBucketFrames);
      size_t b1 = std::min(t.peaks.minimum.size(), size_t(last / kPeakBucketFrames) + 1);
      for (size_t b = b0; b < b1; b++) {
        lo = std::min(lo, t.peaks.minimum[b]);
        hi = std::max(hi, t.peaks.maximum[b]);
      }
    } else {
      const uint64_t coarse = uint64_t(kPeakBucketFrames) * kCoarsePeakFactor;
      size_t b0 = size_t(first / coarse);
      size_t b1 = std::min(t.peaks.coarse_minimum.size(), size_t(last / coarse) + 1);
      for (size_t b = b0; b < b1; b++) {
        lo = std::min(lo, t.peaks.coarse_minimum[b]);
        hi = std::max(hi, t.peaks.coarse_maximum[b]);
      }
    }
    lo = std::max(lo, -1.0f);
    hi = std::min(hi, 1.0f);
    const float top = mid - hi * scale;
    const float bottom = std::max(mid - lo * scale, top + 1.0f);
    draw->AddLine(ImVec2(p.x + x + 0.5f, top), ImVec2(p.x + x + 0.5f, bottom), color);
  }
}

void draw_edit_tab(App& app) {
  collect_sync_results(app);

  if (ImGui::Button("Open session folder...")) {
    SDL_ShowOpenFolderDialog(on_session_folder, &app, app.window, app.base_dir.c_str(), false);
  }
  if (!app.session.dir.empty()) {
    ImGui::SameLine();
    ImGui::TextUnformatted(app.session.dir.u8string().c_str());
  }
  if (!app.edit_error.empty()) ImGui::TextColored(kRed, "%s", app.edit_error.c_str());
  if (app.tracks.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Record something, or open a folder of recordings.");
    return;
  }

  const bool busy = app.job_running.load();
  const float scale = ImGui::GetStyle().FontScaleDpi;

  // Transport
  ImGui::Spacing();
  const bool recording = app.recorder.recording();
  ImGui::BeginDisabled(recording);
  const bool playing = app.playback.playing();
  if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(70.0f * scale, 0.0f))) {
    if (playing) {
      app.playback.pause();
    } else {
      if (app.playback.position() >= app.timeline_end) app.playback.seek(app.in_point);
      app.playback.play();
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Back to start")) app.playback.seek(app.in_point);
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::Text("%s", format_time_precise(app.playback.position()).c_str());
  ImGui::SameLine(0.0f, 24.0f * scale);
  if (ImGui::Button("Set in")) {
    app.in_point = std::min(app.playback.position(), app.out_point);
    app.trimmed = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Set out")) {
    app.out_point = std::max(app.playback.position(), app.in_point);
    app.trimmed = true;
  }
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kMuted, "Trim %s to %s", format_time_precise(app.in_point).c_str(),
                     format_time_precise(app.out_point).c_str());
  ImGui::SameLine(0.0f, 24.0f * scale);
  if (ImGui::Button("Zoom in")) {
    double center = app.view_start + app.view_seconds * 0.5;
    app.view_seconds = std::max(0.05, app.view_seconds * 0.5);
    app.view_start = center - app.view_seconds * 0.5;
  }
  ImGui::SameLine();
  if (ImGui::Button("Zoom out")) {
    double center = app.view_start + app.view_seconds * 0.5;
    app.view_seconds = std::min(app.timeline_end - app.timeline_start, app.view_seconds * 2.0);
    app.view_start = center - app.view_seconds * 0.5;
  }
  ImGui::SameLine();
  if (ImGui::Button("Fit")) {
    app.view_start = app.timeline_start;
    app.view_seconds = app.timeline_end - app.timeline_start;
  }
  app.view_start = std::max(app.timeline_start, std::min(app.view_start, app.timeline_end - app.view_seconds));

  // Sync and export
  ImGui::Spacing();
  ImGui::BeginDisabled(busy || app.tracks.size() < 2);
  if (ImGui::Button("Align tracks")) start_sync_job(app);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Checkbox("Hear aligned", &app.aligned)) apply_alignment(app);
  ImGui::SameLine(0.0f, 24.0f * scale);
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Export aligned WAVs")) start_export_job(app);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::Checkbox("Also export a stereo mix", &app.write_mix);
  if (busy) {
    ImGui::SameLine();
    ImGui::ProgressBar(app.job_progress.load(), ImVec2(160.0f * scale, 0.0f), app.job_label.c_str());
  }
  if (!busy && !app.job_message.empty()) {
    ImGui::TextColored(app.job_failed ? kRed : kGreen, "%s", app.job_message.c_str());
  }

  // Timeline
  ImGui::Spacing();
  const float panel_width = 300.0f * scale;
  const float lane_height = 112.0f * scale;
  const float canvas_width = std::max(100.0f, ImGui::GetContentRegionAvail().x - panel_width - 8.0f * scale);
  const float canvas_x = ImGui::GetCursorScreenPos().x + panel_width + 8.0f * scale;
  ImDrawList* draw = ImGui::GetWindowDrawList();

  // Ruler
  {
    const float ruler_height = 22.0f * scale;
    ImVec2 ruler_pos(canvas_x, ImGui::GetCursorScreenPos().y);
    ImGui::Dummy(ImVec2(panel_width, ruler_height));
    draw->AddRectFilled(ruler_pos, ImVec2(ruler_pos.x + canvas_width, ruler_pos.y + ruler_height),
                        IM_COL32(32, 34, 40, 255));
    static const double kSteps[] = {0.01, 0.05, 0.1, 0.5, 1, 5, 10, 30, 60, 300, 600, 1800, 3600};
    double step = kSteps[12];
    for (double s : kSteps) {
      if (s / app.view_seconds * canvas_width >= 90.0f * scale) {
        step = s;
        break;
      }
    }
    for (double t = std::floor(app.view_start / step) * step; t < app.view_start + app.view_seconds; t += step) {
      float x = canvas_x + float((t - app.view_start) / app.view_seconds * canvas_width);
      if (x < canvas_x) continue;
      draw->AddLine(ImVec2(x, ruler_pos.y + ruler_height * 0.55f), ImVec2(x, ruler_pos.y + ruler_height),
                    IM_COL32(120, 125, 135, 255));
      draw->AddText(ImVec2(x + 3.0f, ruler_pos.y + 1.0f), IM_COL32(170, 175, 185, 255),
                    format_time_precise(t).c_str());
    }
  }

  float lanes_top = ImGui::GetCursorScreenPos().y;
  bool any_solo = false;
  for (auto& t : app.tracks) any_solo = any_solo || t->playback.solo.load();

  for (size_t i = 0; i < app.tracks.size(); i++) {
    EditTrack& t = *app.tracks[i];
    const SessionTrack& st = app.session.tracks[i];
    ImGui::PushID(int(i));
    const ImVec2 lane_pos = ImGui::GetCursorScreenPos();

    ImGui::BeginChild("controls", ImVec2(panel_width, lane_height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted(st.name.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.wav_path.u8string().c_str());
    bool mute = t.playback.mute.load();
    bool solo = t.playback.solo.load();
    ImGui::PushStyleColor(ImGuiCol_Button, mute ? ImVec4(0.72f, 0.42f, 0.14f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::SmallButton("Mute")) t.playback.mute.store(!mute);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, solo ? ImVec4(0.20f, 0.55f, 0.30f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::SmallButton("Solo")) t.playback.solo.store(!solo);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##gain", &t.gain_db, -40.0f, 20.0f, "%.1f dB")) {
      t.playback.gain.store(std::pow(10.0f, t.gain_db / 20.0f));
    }
    if (i == 0) {
      ImGui::TextColored(kMuted, "Reference track");
    } else {
      bool changed = false;
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
      changed |= ImGui::DragFloat("ms##offset", &t.offset_ms, 0.1f, 0.0f, 0.0f, "%.2f");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-40.0f * scale);
      changed |= ImGui::DragFloat("ppm##drift", &t.drift_ppm, 0.1f, -500.0f, 500.0f, "%.2f");
      if (changed && app.aligned) apply_alignment(app);
      if (t.has_sync) {
        ImVec4 color = t.sync.confidence >= 0.7 ? kGreen : (t.sync.confidence >= 0.4 ? kAmber : kRed);
        ImGui::TextColored(color, "%s confidence", confidence_label(t.sync.confidence));
        if (ImGui::IsItemHovered()) {
          ImGui::SetNextWindowSize(ImVec2(420.0f * scale, 0.0f));
          ImGui::BeginTooltip();
          ImGui::TextWrapped("%s", t.sync.message.c_str());
          ImGui::TextColored(kMuted, "Method: %s. Matches used: %d of %d.", sync_method_name(t.sync.method),
                             t.sync.anchors_used, t.sync.anchors_usable);
          ImGui::EndTooltip();
        }
      }
    }
    if (st.gap_frames > 0 || st.dropped_frames > 0) {
      ImGui::TextColored(kAmber, "%.2f s of silence was inserted",
                         double(st.gap_frames + st.dropped_frames) / st.wav->sample_rate());
    }
    ImGui::EndChild();

    const ImVec2 canvas_pos(canvas_x, lane_pos.y);
    const ImVec2 canvas_size(canvas_width, lane_height);
    ImGui::SetCursorScreenPos(canvas_pos);
    ImGui::InvisibleButton("lane", canvas_size);
    if (ImGui::IsItemActive() && !recording) {
      double when = app.view_start + (ImGui::GetIO().MousePos.x - canvas_pos.x) / canvas_width * app.view_seconds;
      app.playback.seek(std::max(app.timeline_start, std::min(when, app.timeline_end)));
    }
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
      const double mouse_time = app.view_start + (ImGui::GetIO().MousePos.x - canvas_pos.x) / canvas_width * app.view_seconds;
      const double factor = ImGui::GetIO().MouseWheel > 0.0f ? 0.8 : 1.25;
      const double next = std::max(0.05, std::min(app.timeline_end - app.timeline_start, app.view_seconds * factor));
      app.view_start = mouse_time - (mouse_time - app.view_start) * next / app.view_seconds;
      app.view_seconds = next;
    }

    draw->PushClipRect(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), true);
    draw->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                        IM_COL32(24, 26, 31, 255));
    const bool silent = t.playback.mute.load() || (any_solo && !t.playback.solo.load());
    draw_waveform(t, canvas_pos, canvas_size, app.view_start, app.view_seconds, draw, silent);
    if (t.has_sync && app.aligned) {
      for (const auto& a : t.sync.anchors) {
        if (!a.inlier) continue;
        float x = canvas_pos.x + float((a.ref_seconds - app.view_start) / app.view_seconds * canvas_width);
        draw->AddTriangleFilled(ImVec2(x - 4.0f, canvas_pos.y), ImVec2(x + 4.0f, canvas_pos.y),
                                ImVec2(x, canvas_pos.y + 7.0f), IM_COL32(120, 210, 130, 230));
      }
    }
    draw->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(lane_pos.x, lane_pos.y + lane_height + 4.0f * scale));
    ImGui::PopID();
  }
  float lanes_bottom = ImGui::GetCursorScreenPos().y;

  // Overlays that span every lane.
  draw->PushClipRect(ImVec2(canvas_x, lanes_top), ImVec2(canvas_x + canvas_width, lanes_bottom), true);
  auto to_x = [&](double when) { return canvas_x + float((when - app.view_start) / app.view_seconds * canvas_width); };
  if (app.in_point > app.timeline_start) {
    draw->AddRectFilled(ImVec2(canvas_x, lanes_top), ImVec2(to_x(app.in_point), lanes_bottom), IM_COL32(0, 0, 0, 130));
  }
  if (app.out_point < app.timeline_end) {
    draw->AddRectFilled(ImVec2(to_x(app.out_point), lanes_top), ImVec2(canvas_x + canvas_width, lanes_bottom),
                        IM_COL32(0, 0, 0, 130));
  }
  const float head = to_x(app.playback.position());
  draw->AddLine(ImVec2(head, lanes_top), ImVec2(head, lanes_bottom), IM_COL32(240, 240, 240, 230), 1.5f);
  draw->PopClipRect();

  ImGui::TextColored(kMuted, "Click a waveform to move the playhead. Scroll over it to zoom. Green marks show the sounds used to align.");

  bool warned = false;
  for (size_t i = 1; i < app.tracks.size(); i++) {
    const EditTrack& t = *app.tracks[i];
    if (t.has_sync && t.sync.confidence < 0.7) {
      if (!warned) ImGui::Spacing();
      warned = true;
      ImGui::PushStyleColor(ImGuiCol_Text, t.sync.confidence < 0.4 ? kRed : kAmber);
      ImGui::TextWrapped("%s: %s", app.session.tracks[i].name.c_str(), t.sync.message.c_str());
      ImGui::PopStyleColor();
    }
  }
}

void apply_dialog_results(App& app) {
  std::lock_guard<std::mutex> lock(app.dialog_mutex);
  if (!app.dialog_record_dir.empty()) {
    app.base_dir = app.dialog_record_dir;
    app.dialog_record_dir.clear();
    app.disk_checked_at = -100.0;
  }
  if (!app.dialog_session_dir.empty()) {
    load_session_into_editor(app, fs::u8path(app.dialog_session_dir));
    app.dialog_session_dir.clear();
  }
}

bool save_screenshot(SDL_Renderer* renderer, const char* path) {
  SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
  if (!surface) return false;
  bool ok = SDL_SaveBMP(surface, path);
  SDL_DestroySurface(surface);
  return ok;
}

}  // namespace

// Optional flags for automated checks: --open DIR, --align, --arm-all,
// --tab edit, --screenshot FILE.bmp (saves after --frames N frames, then exits).
int main(int argc, char** argv) {
  const char* open_dir = nullptr;
  const char* screenshot_path = nullptr;
  bool auto_align = false;
  bool arm_all = false;
  bool start_on_edit = false;
  int screenshot_frames = 60;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--open") && i + 1 < argc) {
      open_dir = argv[++i];
    } else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
      screenshot_path = argv[++i];
    } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
      screenshot_frames = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--align")) {
      auto_align = true;
    } else if (!strcmp(argv[i], "--arm-all")) {
      arm_all = true;
    } else if (!strcmp(argv[i], "--tab") && i + 1 < argc) {
      start_on_edit = !strcmp(argv[++i], "edit");
    }
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MyDAW", SDL_GetError(), nullptr);
    return 1;
  }
  const float dpi = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
  SDL_Window* window = SDL_CreateWindow("MyDAW", int(1180 * dpi), int(760 * dpi), SDL_WINDOW_RESIZABLE);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MyDAW", SDL_GetError(), nullptr);
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0.0f;
  style.FrameRounding = 4.0f;
  style.ChildRounding = 4.0f;
  style.ScaleAllSizes(dpi);
  style.FontScaleDpi = dpi;
  if (fs::exists("C:/Windows/Fonts/segoeui.ttf")) io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 16.0f);
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  auto app_owner = std::make_unique<App>();
  App& app = *app_owner;
  app.window = window;
  const char* music = SDL_GetUserFolder(SDL_FOLDER_MUSIC);
  app.base_dir = (fs::u8path(music ? music : ".") / "MyDAW Recordings").u8string();
  refresh_devices(app);
  if (arm_all) {
    for (const auto& device : app.devices) {
      std::string err;
      if (!app.recorder.arm(device, &err)) app.record_error = err;
    }
  }
  if (open_dir) load_session_into_editor(app, fs::u8path(open_dir));
  if (auto_align) start_sync_job(app);
  if (start_on_edit || open_dir) app.tab_request = 2;

  bool running = true;
  int frame = 0;
  while (running) {
    SDL_Event event;
    // Stay light when nothing is moving. Meters and playback need steady redraws.
    const bool animating = !app.status.empty() || app.playback.playing() || app.job_running.load();
    if (SDL_WaitEventTimeout(&event, animating ? 16 : 100)) {
      do {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT) running = false;
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
          running = false;
        }
      } while (SDL_PollEvent(&event));
    }

    apply_dialog_results(app);
    app.status = app.recorder.status();  // Reads and resets the meters, so call it once per frame.

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (ImGui::BeginTabBar("tabs")) {
      ImGuiTabItemFlags record_flags = app.tab_request == 1 ? ImGuiTabItemFlags_SetSelected : 0;
      ImGuiTabItemFlags edit_flags = app.tab_request == 2 ? ImGuiTabItemFlags_SetSelected : 0;
      app.tab_request = 0;
      if (ImGui::BeginTabItem("Record", nullptr, record_flags)) {
        draw_record_tab(app);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Edit", nullptr, edit_flags)) {
        draw_edit_tab(app);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::End();

    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 20, 21, 25, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

    frame++;
    const bool settled = !app.job_running.load() && (app.tracks.empty() || app.tracks.back()->peaks_ready.load());
    if (screenshot_path && frame >= screenshot_frames && settled) {
      save_screenshot(renderer, screenshot_path);
      running = false;
    }
    SDL_RenderPresent(renderer);
  }

  app.recorder.stop();
  finish_job(app);
  close_session(app);
  app.playback.close();
  app_owner.reset();

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
