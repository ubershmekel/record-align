#include "platform/files.h"
// Desktop UI: pick microphones, record, then align, listen and export.
#ifdef _WIN32
#include <windows.h>
#endif
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

#include "version.h"
#include "app/playback.h"
#include "audio/capture.h"
#include "audio/edits.h"
#include "audio/import.h"
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

// Explains the item just drawn, including when it's disabled.
void tip(const char* text) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

std::string format_number(float value, const char* unit) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.2f%s", value, unit);
  return buffer;
}

// Min/max summaries for drawing long files quickly.
struct TrackPeaks {
  std::vector<float> minimum;
  std::vector<float> maximum;
  std::vector<float> coarse_minimum;
  std::vector<float> coarse_maximum;
  // Per-channel peaks for stereo display (one entry per channel, up to 2).
  std::vector<std::vector<float>> ch_minimum;
  std::vector<std::vector<float>> ch_maximum;
  std::vector<std::vector<float>> ch_coarse_minimum;
  std::vector<std::vector<float>> ch_coarse_maximum;
  int channels = 1;
};

void build_peaks(const WavFile& wav, TrackPeaks* out, std::atomic<bool>* cancel) {
  const int ch = wav.channels();
  out->channels = ch;
  const uint64_t buckets = wav.frames() / kPeakBucketFrames + 1;
  out->minimum.assign(size_t(buckets), 0.0f);
  out->maximum.assign(size_t(buckets), 0.0f);
  const int per_ch = std::min(ch, 2);
  out->ch_minimum.assign(per_ch, std::vector<float>(size_t(buckets), 0.0f));
  out->ch_maximum.assign(per_ch, std::vector<float>(size_t(buckets), 0.0f));
  for (uint64_t b = 0; b < buckets; b++) {
    if (cancel->load()) return;
    const uint64_t first = b * kPeakBucketFrames;
    const uint64_t last = std::min<uint64_t>(wav.frames(), first + kPeakBucketFrames);
    float lo = 0.0f, hi = 0.0f;
    float ch_lo[2] = {0.0f, 0.0f};
    float ch_hi[2] = {0.0f, 0.0f};
    const float* s = wav.samples() + first * ch;
    for (uint64_t f = 0; f < (last - first); f++) {
      for (int c = 0; c < ch; c++) {
        const float v = s[f * ch + c];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        if (c < 2) {
          ch_lo[c] = std::min(ch_lo[c], v);
          ch_hi[c] = std::max(ch_hi[c], v);
        }
      }
    }
    out->minimum[size_t(b)] = lo;
    out->maximum[size_t(b)] = hi;
    for (int c = 0; c < per_ch; c++) {
      out->ch_minimum[c][size_t(b)] = ch_lo[c];
      out->ch_maximum[c][size_t(b)] = ch_hi[c];
    }
  }
  const size_t coarse = out->minimum.size() / kCoarsePeakFactor + 1;
  out->coarse_minimum.assign(coarse, 0.0f);
  out->coarse_maximum.assign(coarse, 0.0f);
  out->ch_coarse_minimum.assign(per_ch, std::vector<float>(coarse, 0.0f));
  out->ch_coarse_maximum.assign(per_ch, std::vector<float>(coarse, 0.0f));
  for (size_t i = 0; i < out->minimum.size(); i++) {
    const size_t c = i / kCoarsePeakFactor;
    out->coarse_minimum[c] = std::min(out->coarse_minimum[c], out->minimum[i]);
    out->coarse_maximum[c] = std::max(out->coarse_maximum[c], out->maximum[i]);
    for (int ci = 0; ci < per_ch; ci++) {
      out->ch_coarse_minimum[ci][c] = std::min(out->ch_coarse_minimum[ci][c], out->ch_minimum[ci][i]);
      out->ch_coarse_maximum[ci][c] = std::max(out->ch_coarse_maximum[ci][c], out->ch_maximum[ci][i]);
    }
  }
}

struct EditTrack {
  PlaybackTrack playback;
  TrackPeaks peaks;
  std::atomic<bool> peaks_ready{false};
};

// What the user can undo. Solo and the playhead are left out: they are for
// listening, and undo shouldn't get stuck stepping back through them.
struct TrackEditState {
  float gain_db = 0.0f;
  float offset_ms = 0.0f;
  float drift_ppm = 0.0f;
  bool mute = false;
  std::shared_ptr<const SyncResult> sync;  // Shared to keep the undo history small.

  bool operator==(const TrackEditState& o) const {
    return gain_db == o.gain_db && offset_ms == o.offset_ms && drift_ppm == o.drift_ppm && mute == o.mute &&
           sync == o.sync;
  }
};

struct EditState {
  std::vector<TrackEditState> tracks;
  size_t master = 0;  // The track the others align to. Its own timing stays at zero.
  bool in_set = false;
  bool out_set = false;
  double in_point = 0.0;
  double out_point = 0.0;

  bool operator==(const EditState& o) const {
    return tracks == o.tracks && master == o.master && in_set == o.in_set && out_set == o.out_set &&
           (!in_set || in_point == o.in_point) && (!out_set || out_point == o.out_point);
  }
  bool operator!=(const EditState& o) const { return !(*this == o); }
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
  EditState edit;       // Current values. Widgets change this directly.
  EditState committed;  // The last settled values, on top of the undo stack.
  std::vector<EditState> undo_stack;
  std::vector<EditState> redo_stack;

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
  bool match_master = false;  // Export exactly the master's span, to drop onto a video.

  // Folder dialogs may answer from another thread.
  std::mutex dialog_mutex;
  std::string dialog_record_dir;
  std::string dialog_session_dir;
  std::vector<std::string> dialog_import_files;
  std::string dialog_save_dir;  // For Save project...

  std::vector<std::string> dropped_files;
  fs::path job_imported_session;  // Set by the import job, guarded by job_mutex.
  fs::path job_saved_session;     // Set by the save-project job, guarded by job_mutex.

  int tab_request = 0;  // 1 selects Record, 2 selects Edit, 3 selects Settings.
  bool edit_tab_was_open = false;
  std::string last_open_dir;        // Persisted across sessions.
  bool session_is_permanent = true; // False for fresh imports not yet saved to a folder.
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
  app.playback.set_stop_at(app.timeline_end);
}

// Pushes the current edits to playback, with zero timing while comparing against unaligned.
void sync_playback(App& app) {
  for (size_t i = 0; i < app.tracks.size(); i++) {
    PlaybackTrack& p = app.tracks[i]->playback;
    const TrackEditState& e = app.edit.tracks[i];
    p.offset_seconds.store(app.aligned ? double(e.offset_ms) / 1000.0 : 0.0);
    p.drift.store(app.aligned ? double(e.drift_ppm) * 1e-6 : 0.0);
    p.gain.store(std::pow(10.0f, e.gain_db / 20.0f));
    p.mute.store(e.mute);
  }
  update_timeline(app);
}

double effective_in(const App& app) { return app.edit.in_set ? app.edit.in_point : app.timeline_start; }
double effective_out(const App& app) { return app.edit.out_set ? app.edit.out_point : app.timeline_end; }

void toggle_playback(App& app) {
  if (app.recorder.recording()) return;
  if (app.playback.playing()) {
    app.playback.pause();
    return;
  }
  if (app.playback.position() >= app.timeline_end) app.playback.seek(effective_in(app));
  app.playback.play();
}

// A plain click listens to just this track, and clicking the only soloed track
// turns solo off. Holding Shift or Ctrl adds or removes tracks from the set.
void click_solo(App& app, size_t index) {
  const ImGuiIO& io = ImGui::GetIO();
  std::atomic<bool>& clicked = app.tracks[index]->playback.solo;
  if (io.KeyShift || io.KeyCtrl || io.KeySuper) {
    clicked.store(!clicked.load());
    return;
  }
  bool others = false;
  for (size_t i = 0; i < app.tracks.size(); i++) {
    if (i != index && app.tracks[i]->playback.solo.load()) others = true;
  }
  const bool turn_on = others || !clicked.load();
  for (auto& t : app.tracks) t->playback.solo.store(false);
  clicked.store(turn_on);
}

void close_session(App& app) {
  app.playback.set_tracks({});
  app.peak_cancel.store(true);
  if (app.peak_thread.joinable()) app.peak_thread.join();
  app.tracks.clear();
  app.session = Session();
  app.edit = EditState();
  app.committed = EditState();
  app.undo_stack.clear();
  app.redo_stack.clear();
}

void save_session_edits(App& app) {
  SessionEdits saved;
  saved.master_name = app.session.tracks[app.edit.master].name;
  saved.in_set = app.edit.in_set;
  saved.out_set = app.edit.out_set;
  saved.in_seconds = app.edit.in_point;
  saved.out_seconds = app.edit.out_point;
  for (size_t i = 0; i < app.edit.tracks.size(); i++) {
    const TrackEditState& e = app.edit.tracks[i];
    TrackEdit t;
    t.name = app.session.tracks[i].name;
    t.offset_ms = e.offset_ms;
    t.drift_ppm = e.drift_ppm;
    t.gain_db = e.gain_db;
    t.mute = e.mute;
    if (e.sync) {
      t.has_sync = true;
      t.sync = *e.sync;
    }
    saved.tracks.push_back(std::move(t));
  }
  std::string err;
  if (!save_edits(app.session.dir, saved, &err)) app.edit_error = err;
}

void load_session_edits(App& app) {
  SessionEdits saved;
  if (!load_edits(app.session.dir, &saved)) return;
  app.edit.in_set = saved.in_set;
  app.edit.out_set = saved.out_set;
  app.edit.in_point = saved.in_seconds;
  app.edit.out_point = saved.out_seconds;
  for (size_t i = 0; i < app.session.tracks.size(); i++) {
    if (app.session.tracks[i].name == saved.master_name) app.edit.master = i;
  }
  for (const TrackEdit& t : saved.tracks) {
    for (size_t i = 0; i < app.session.tracks.size(); i++) {
      if (app.session.tracks[i].name != t.name) continue;
      TrackEditState& e = app.edit.tracks[i];
      e.offset_ms = float(t.offset_ms);
      e.drift_ppm = float(t.drift_ppm);
      e.gain_db = float(t.gain_db);
      e.mute = t.mute;
      if (t.has_sync) e.sync = std::make_shared<const SyncResult>(t.sync);
    }
  }
}

// Records a finished change. Waiting until no widget is active turns a whole
// slider drag into one undo step, and catches every kind of edit in one place.
void commit_edits(App& app) {
  if (app.edit == app.committed || ImGui::IsAnyItemActive()) return;
  app.undo_stack.push_back(app.committed);
  constexpr size_t kMaxUndoSteps = 200;
  if (app.undo_stack.size() > kMaxUndoSteps) app.undo_stack.erase(app.undo_stack.begin());
  app.redo_stack.clear();
  app.committed = app.edit;
  save_session_edits(app);
}

void undo_edit(App& app) {
  commit_edits(app);
  if (app.undo_stack.empty() || ImGui::IsAnyItemActive()) return;
  app.redo_stack.push_back(app.committed);
  app.committed = std::move(app.undo_stack.back());
  app.undo_stack.pop_back();
  app.edit = app.committed;
  save_session_edits(app);
}

void redo_edit(App& app) {
  commit_edits(app);
  if (app.redo_stack.empty() || ImGui::IsAnyItemActive()) return;
  app.undo_stack.push_back(app.committed);
  app.committed = std::move(app.redo_stack.back());
  app.redo_stack.pop_back();
  app.edit = app.committed;
  save_session_edits(app);
}

// Names the difference between two states, for the undo and redo tooltips.
std::string describe_change(const EditState& a, const EditState& b) {
  if (a.master != b.master) return "master track change";
  if (a.in_set != b.in_set || a.out_set != b.out_set || (a.in_set && a.in_point != b.in_point) ||
      (a.out_set && a.out_point != b.out_point)) {
    return "trim";
  }
  int gains = 0;
  bool timing = false, sync = false, mute = false;
  for (size_t i = 0; i < a.tracks.size() && i < b.tracks.size(); i++) {
    const TrackEditState& x = a.tracks[i];
    const TrackEditState& y = b.tracks[i];
    if (x.sync != y.sync) sync = true;
    if (x.offset_ms != y.offset_ms || x.drift_ppm != y.drift_ppm) timing = true;
    if (x.mute != y.mute) mute = true;
    if (x.gain_db != y.gain_db) gains++;
  }
  if (sync) return "align tracks";
  if (timing) return "timing change";
  if (mute) return "mute";
  if (gains > 1) return "gain on several tracks";
  if (gains == 1) return "gain change";
  return "edit";
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
  app.edit.tracks.resize(app.tracks.size());
  load_session_edits(app);
  app.committed = app.edit;
  app.aligned = true;
  sync_playback(app);
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
  const size_t master = app.edit.master;
  app.job_thread = std::thread([&app, master] {
    const SessionTrack& ref = app.session.tracks[master];
    const ClockFit ref_clock = fit_clock(ref.timing, ref.wav->sample_rate());
    std::vector<std::pair<size_t, SyncResult>> results;
    for (size_t i = 0; i < app.session.tracks.size(); i++) {
      if (i == master) continue;
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

// Timing is relative to the master, so changing it means measuring everything again.
void set_master(App& app, size_t index) {
  if (app.job_running.load() || index == app.edit.master) return;
  app.edit.master = index;
  for (TrackEditState& e : app.edit.tracks) {
    e.offset_ms = 0.0f;
    e.drift_ppm = 0.0f;
    e.sync = nullptr;
  }
  start_sync_job(app);
}

void collect_sync_results(App& app) {
  std::lock_guard<std::mutex> lock(app.job_mutex);
  if (!app.job_sync_done) return;
  app.job_sync_done = false;
  for (auto& [index, result] : app.job_sync_results) {
    TrackEditState& e = app.edit.tracks[index];
    e.offset_ms = float(result.map.offset_seconds * 1000.0);
    e.drift_ppm = float(result.map.drift * 1e6);
    e.sync = std::make_shared<const SyncResult>(std::move(result));
  }
  app.job_sync_results.clear();
  app.aligned = true;
  sync_playback(app);
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
    const TrackEditState& state = app.edit.tracks[i];
    ExportTrack e;
    e.audio = view_of(*t.playback.wav);
    e.map.offset_seconds = double(state.offset_ms) / 1000.0;
    e.map.drift = double(state.drift_ppm) * 1e-6;
    e.name = app.session.tracks[i].name;
    e.gain = std::pow(10.0f, state.gain_db / 20.0f);
    e.in_mix = !state.mute && (!any_solo || t.playback.solo.load());
    jobs.push_back(std::move(e));
  }
  ExportOptions options;
  const WavFile& master_wav = *app.session.tracks[app.edit.master].wav;
  options.out_dir = app.session.dir / "aligned";
  options.sample_rate = master_wav.sample_rate();
  options.use_range = app.edit.in_set || app.edit.out_set;
  options.range_start = effective_in(app);
  options.range_end = effective_out(app);
  if (app.match_master && !options.use_range) {
    options.use_range = true;
    options.range_start = 0.0;
    options.range_end = master_wav.seconds();
  }
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

// Pulls the audio out of existing video and audio files into a new session.
void start_import_job(App& app, const std::vector<std::string>& files) {
  if (app.job_running.load() || app.recorder.recording() || files.empty()) return;
  finish_job(app);
  std::vector<fs::path> inputs;
  for (const std::string& file : files) inputs.push_back(fs::u8path(file));
  app.playback.pause();
  app.job_running.store(true);
  app.job_progress.store(0.0f);
  app.job_label = "Importing";
  app.job_message.clear();
  app.job_failed = false;
  app.tab_request = 2;
  app.job_thread = std::thread([&app, inputs] {
    fs::path session;
    std::string err;
    const bool ok = import_files(inputs, &session, &app.job_progress, &err);
    std::lock_guard<std::mutex> lock(app.job_mutex);
    app.job_failed = !ok;
    app.job_message = ok ? std::string() : err;
    app.job_imported_session = ok ? session : fs::path();
    app.job_running.store(false);
  });
}

// Once an import finishes, opens it and aligns straight away, since that's why it was imported.
void collect_import_result(App& app) {
  fs::path session;
  {
    std::lock_guard<std::mutex> lock(app.job_mutex);
    if (app.job_running.load() || app.job_imported_session.empty()) return;
    session = app.job_imported_session;
    app.job_imported_session.clear();
  }
  load_session_into_editor(app, session);
  app.session_is_permanent = false;
  app.match_master = true;
  start_sync_job(app);
}

// Once a save-project job finishes, reloads from the new permanent location.
void collect_save_result(App& app) {
  fs::path saved;
  {
    std::lock_guard<std::mutex> lock(app.job_mutex);
    if (app.job_running.load() || app.job_saved_session.empty()) return;
    saved = app.job_saved_session;
    app.job_saved_session.clear();
  }
  load_session_into_editor(app, saved);
  app.session_is_permanent = true;
  app.last_open_dir = saved.parent_path().u8string();
}

// Sets each track's gain so its loudest moment inside the trim range reaches the
// target. Only the gain changes, so this is undoable and the WAVs stay as they are.
void normalize_all(App& app) {
  constexpr float kTargetDb = -1.0f;
  constexpr float kMaxGainDb = 40.0f;
  constexpr float kSilence = 1e-5f;
  const double start = effective_in(app);
  const double end = effective_out(app);

  std::string report = "Normalized to a -1 dB peak:";
  for (size_t i = 0; i < app.tracks.size(); i++) {
    const EditTrack& t = *app.tracks[i];
    const double rate = t.playback.wav->sample_rate();
    const double offset = t.playback.offset_seconds.load();
    const double drift = t.playback.drift.load();
    const double first = std::max(0.0, (offset + (1.0 + drift) * start) * rate);
    const double last = std::max(0.0, (offset + (1.0 + drift) * end) * rate);
    const size_t b0 = size_t(first / kPeakBucketFrames);
    const size_t b1 = std::min(t.peaks.minimum.size(), size_t(last / kPeakBucketFrames) + 1);
    float peak = 0.0f;
    for (size_t b = b0; b < b1; b++) peak = std::max({peak, -t.peaks.minimum[b], t.peaks.maximum[b]});

    char part[64];
    if (peak < kSilence) {
      snprintf(part, sizeof(part), "%s silent, left alone", i ? "," : "");
    } else {
      const float wanted = kTargetDb - 20.0f * std::log10(peak);
      const float gain = std::max(-kMaxGainDb, std::min(kMaxGainDb, wanted));
      app.edit.tracks[i].gain_db = gain;
      snprintf(part, sizeof(part), "%s %+.1f dB%s", i ? "," : "", gain, gain != wanted ? " (gain limit)" : "");
    }
    report += part;
  }
  std::lock_guard<std::mutex> lock(app.job_mutex);
  app.job_message = report;
  app.job_failed = false;
}

std::string pref_dir() {
  const char* p = SDL_GetPrefPath("record-align", "record-align");
  if (!p) return {};
  std::string r(p);
  SDL_free((void*)p);
  return r;
}

void save_prefs(const App& app) {
  const std::string dir = pref_dir();
  if (dir.empty()) return;
  FILE* f = open_file((fs::u8path(dir) / "prefs.txt").c_str(), "w");
  if (!f) return;
  fprintf(f, "record_dir %s\n", app.base_dir.c_str());
  if (!app.last_open_dir.empty()) fprintf(f, "open_dir %s\n", app.last_open_dir.c_str());
  fclose(f);
}

void load_prefs(App& app) {
  const std::string dir = pref_dir();
  if (dir.empty()) return;
  FILE* f = open_file((fs::u8path(dir) / "prefs.txt").c_str(), "r");
  if (!f) return;
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.size() > 11 && s.substr(0, 11) == "record_dir ") app.base_dir = s.substr(11);
    if (s.size() > 9 && s.substr(0, 9) == "open_dir ") app.last_open_dir = s.substr(9);
  }
  fclose(f);
}

void start_save_session_job(App& app, const fs::path& dest) {
  if (app.job_running.load() || app.session.dir.empty()) return;
  finish_job(app);
  save_session_edits(app);
  const fs::path src = app.session.dir;
  app.job_running.store(true);
  app.job_progress.store(0.0f);
  app.job_label = "Saving project";
  app.job_message.clear();
  app.job_failed = false;
  app.job_thread = std::thread([&app, src, dest] {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
      std::lock_guard<std::mutex> g(app.job_mutex);
      app.job_failed = true;
      app.job_message = "Could not create " + dest.u8string() + ": " + ec.message();
      app.job_running.store(false);
      return;
    }
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(src, ec)) {
      if (e.is_regular_file()) entries.push_back(e.path());
    }
    for (size_t i = 0; i < entries.size(); i++) {
      fs::copy(entries[i], dest / entries[i].filename(), fs::copy_options::overwrite_existing, ec);
      if (ec) {
        std::lock_guard<std::mutex> g(app.job_mutex);
        app.job_failed = true;
        app.job_message = "Could not copy " + entries[i].filename().u8string() + ": " + ec.message();
        app.job_running.store(false);
        return;
      }
      app.job_progress.store(float(i + 1) / entries.size());
    }
    std::lock_guard<std::mutex> g(app.job_mutex);
    app.job_saved_session = dest;
    app.job_running.store(false);
  });
}

void SDLCALL on_record_folder(void* user, const char* const* files, int) {
  App& app = *static_cast<App*>(user);
  if (!files || !files[0]) return;
  std::lock_guard<std::mutex> lock(app.dialog_mutex);
  app.dialog_record_dir = files[0];
}

void SDLCALL on_import_files(void* user, const char* const* files, int) {
  App& app = *static_cast<App*>(user);
  if (!files) return;
  std::lock_guard<std::mutex> lock(app.dialog_mutex);
  app.dialog_import_files.clear();
  for (int i = 0; files[i]; i++) app.dialog_import_files.push_back(files[i]);
}

void show_import_dialog(App& app) {
  static const SDL_DialogFileFilter kFilters[] = {
      {"Video and audio", "mp4;mov;m4v;mkv;webm;avi;mts;m2ts;wmv;wav;mp3;m4a;aac;flac;ogg;opus;aif;aiff"},
      {"All files", "*"},
  };
  SDL_ShowOpenFileDialog(on_import_files, &app, app.window, kFilters, 2, nullptr, true);
}

void SDLCALL on_session_folder(void* user, const char* const* files, int) {
  App& app = *static_cast<App*>(user);
  if (!files || !files[0]) return;
  std::lock_guard<std::mutex> lock(app.dialog_mutex);
  app.dialog_session_dir = files[0];
}

void SDLCALL on_save_folder(void* user, const char* const* files, int) {
  App& app = *static_cast<App*>(user);
  if (!files || !files[0]) return;
  std::lock_guard<std::mutex> lock(app.dialog_mutex);
  app.dialog_save_dir = files[0];
}

void draw_settings_tab(App& app) {
  ImGui::Spacing();
  ImGui::SeparatorText("Recordings");
  const bool recording = app.recorder.recording();
  ImGui::BeginDisabled(recording);
  if (ImGui::Button("Choose folder...")) {
    SDL_ShowOpenFolderDialog(on_record_folder, &app, app.window, app.base_dir.c_str(), false);
  }
  ImGui::EndDisabled();
  tip("Where new recordings are saved. Each recording gets its own subfolder inside this one.");
  ImGui::SameLine();
  ImGui::TextUnformatted(app.base_dir.c_str());
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(kMuted, "Preferences are saved automatically when you change them.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::SeparatorText("About");
  ImGui::Text("Version: %s  built %s %s", RA_GIT_HASH, __DATE__, __TIME__);
  ImGui::Spacing();
  ImGui::TextUnformatted("record-align on GitHub:");
  ImGui::SameLine();
  const char* kGithubUrl = "https://github.com/ubershmekel/record-align";
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.72f, 1.0f, 1.0f));
  ImGui::TextUnformatted(kGithubUrl);
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Click to open in your browser");
  if (ImGui::IsItemClicked()) SDL_OpenURL(kGithubUrl);
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

// Explains the claps, and turns into a prompt for what to do during a recording.
void draw_clap_guide(App& app, bool recording) {
  constexpr double kStartPromptSeconds = 30.0;
  ImGui::BeginChild("clap_guide", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
  if (recording) {
    if (app.recorder.elapsed_seconds() < kStartPromptSeconds) {
      ImGui::TextColored(kAmber, "Clap 3 times now, with a short gap between claps, close enough for every microphone to hear.");
    } else {
      ImGui::TextColored(kMuted, "When you're done, clap 3 more times, then press Stop recording.");
    }
  } else {
    ImGui::TextUnformatted("Clap 3 times at the start of each recording, and 3 times at the end.");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kMuted,
                       "Every microphone runs on its own clock, so they start at slightly different moments and can "
                       "slowly slide apart. Claps give them all the same sharp sounds to match. The first claps line "
                       "up the start, and the last claps measure any sliding, so the tracks stay together all the way "
                       "through.");
    ImGui::PopTextWrapPos();
  }
  ImGui::EndChild();
}

void draw_record_tab(App& app) {
  const bool recording = app.recorder.recording();

  // Quick launcher: open or import before recording.
  ImGui::Spacing();
  const bool busy = app.job_running.load();
  ImGui::BeginDisabled(recording || busy);
  if (ImGui::Button("Open saved project...")) {
    const char* start = app.last_open_dir.empty() ? app.base_dir.c_str() : app.last_open_dir.c_str();
    SDL_ShowOpenFolderDialog(on_session_folder, &app, app.window, start, false);
  }
  tip("Open a folder of recordings made earlier. Any edits you made to it come back too.");
  ImGui::SameLine();
  if (ImGui::Button("Import video or audio files...")) show_import_dialog(app);
  tip("Line up video and audio files you already have, such as a phone video and a separate microphone. "
      "This needs ffmpeg installed. You can also drag files onto this window.");
  ImGui::EndDisabled();
  ImGui::Spacing();

  draw_clap_guide(app, recording);

  ImGui::SeparatorText("Microphones");
  ImGui::BeginDisabled(recording);
  if (ImGui::Button("Refresh list")) refresh_devices(app);
  tip("Look for microphones again, for example after plugging one in.");
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
      tip("Tick to record from this microphone. Its meter goes live, but nothing is saved until you press Start recording.");
      ImGui::EndDisabled();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(kMuted, "%.1f kHz, %s", device.sample_rate / 1000.0,
                         device.channels == 1 ? "mono" : (device.channels == 2 ? "stereo" : "multi"));
      tip("The format the operating system provides for this microphone. It is recorded as is, without converting.");
      ImGui::TableNextColumn();
      const TrackStatus* status = nullptr;
      for (const auto& s : app.status) {
        if (s.device_id == device.id) status = &s;
      }
      if (status) {
        float& hold = app.meter_hold[i];
        hold = std::max(status->peak, hold * 0.97f);
        draw_meter(status->peak, hold, ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()));
        tip("Live level, from -60 dB on the left to 0 dB on the right. The white line holds the recent peak. Red means "
            "the sound is too loud and is distorting.");
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
  tip("Where recordings go. Each recording gets its own new folder inside this one.");
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
    tip("Records every ticked microphone to its own WAV file.");
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

  if (!app.status.empty() && (recording || !app.last_session.empty())) {
    ImGui::Spacing();
    if (ImGui::BeginTable("tracks", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthStretch, 3.0f);
      ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Gaps", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Lost", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 3.0f);
      static const char* const kHeaders[] = {"Track", "Length", "Gaps", "Lost", "State"};
      static const char* const kTips[] = {
          "The microphone this file comes from.",
          "How much has been saved so far.",
          "Time the device skipped. It's filled with silence so the track's timing stays correct.",
          "Audio lost because the disk couldn't keep up. It's filled with silence so the track's timing stays correct.",
          "Whether this track is recording, saved, or has a problem.",
      };
      ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
      for (int c = 0; c < 5; c++) {
        ImGui::TableSetColumnIndex(c);
        ImGui::TableHeader(kHeaders[c]);
        tip(kTips[c]);
      }
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

// Renders one waveform channel (ch_idx >= 0) or all channels mixed (ch_idx == -1).
void draw_channel_waveform(const WavFile& wav,
                            const std::vector<float>& peak_min, const std::vector<float>& peak_max,
                            const std::vector<float>& coarse_min, const std::vector<float>& coarse_max,
                            int ch_idx, double offset, double drift,
                            const ImVec2& p, const ImVec2& size,
                            double view_start, double view_seconds,
                            ImDrawList* draw, ImU32 color, float gain_scale = 1.0f) {
  const double rate = wav.sample_rate();
  const int ch = wav.channels();
  const float mid = p.y + size.y * 0.5f;
  const float amp = size.y * 0.48f * gain_scale;
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
      const float* s = wav.samples() + first * ch;
      if (ch_idx < 0) {
        for (uint64_t i = 0; i < span * ch; i++) {
          lo = std::min(lo, s[i]);
          hi = std::max(hi, s[i]);
        }
      } else {
        for (uint64_t f = 0; f < span; f++) {
          const float v = s[f * ch + ch_idx];
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
      }
    } else if (span < uint64_t(kPeakBucketFrames) * kCoarsePeakFactor * 2) {
      size_t b0 = size_t(first / kPeakBucketFrames);
      size_t b1 = std::min(peak_min.size(), size_t(last / kPeakBucketFrames) + 1);
      for (size_t b = b0; b < b1; b++) {
        lo = std::min(lo, peak_min[b]);
        hi = std::max(hi, peak_max[b]);
      }
    } else {
      const uint64_t coarse_bucket = uint64_t(kPeakBucketFrames) * kCoarsePeakFactor;
      size_t b0 = size_t(first / coarse_bucket);
      size_t b1 = std::min(coarse_min.size(), size_t(last / coarse_bucket) + 1);
      for (size_t b = b0; b < b1; b++) {
        lo = std::min(lo, coarse_min[b]);
        hi = std::max(hi, coarse_max[b]);
      }
    }
    lo = std::max(lo, -1.0f);
    hi = std::min(hi, 1.0f);
    const float top = mid - hi * amp;
    const float bottom = std::max(mid - lo * amp, top + 1.0f);
    draw->AddLine(ImVec2(p.x + x + 0.5f, top), ImVec2(p.x + x + 0.5f, bottom), color);
  }
}

// Draws the waveform for a track. Stereo tracks are split: L on top half, R on bottom half.
void draw_waveform(const EditTrack& t, float gain_db, const ImVec2& p, const ImVec2& size, double view_start, double view_seconds,
                   ImDrawList* draw, bool muted) {
  const WavFile& wav = *t.playback.wav;
  if (!t.peaks_ready.load()) {
    draw->AddText(ImVec2(p.x + 8.0f, p.y + 6.0f), IM_COL32(160, 165, 175, 255), "Reading waveform...");
    return;
  }
  const double offset = t.playback.offset_seconds.load();
  const double drift = t.playback.drift.load();
  // Scale visible amplitude by gain: each 20 dB shifts height by 1×,
  // so +20 dB fills the lane and -20 dB collapses to a sliver. The lane's
  // clip rect handles overflow, so we don't cap the upper end tightly.
  const float gain_scale = std::max(0.1f, 1.0f + gain_db / 20.0f);
  const ImU32 color_l = muted ? IM_COL32(95, 100, 110, 255) : IM_COL32(95, 170, 225, 255);
  const int ch = wav.channels();
  if (ch >= 2 && int(t.peaks.ch_minimum.size()) >= 2) {
    const float half_h = std::floor(size.y * 0.5f);
    const float divider_y = p.y + half_h;
    draw->AddLine(ImVec2(p.x, divider_y), ImVec2(p.x + size.x, divider_y), IM_COL32(50, 55, 65, 180));
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 4.0f, p.y + 1.0f),
                  IM_COL32(120, 140, 160, 180), "L");
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 4.0f, divider_y + 1.0f),
                  IM_COL32(120, 140, 160, 180), "R");
    const ImU32 color_r = muted ? IM_COL32(95, 100, 110, 255) : IM_COL32(85, 155, 215, 255);
    draw_channel_waveform(wav, t.peaks.ch_minimum[0], t.peaks.ch_maximum[0],
                          t.peaks.ch_coarse_minimum[0], t.peaks.ch_coarse_maximum[0],
                          0, offset, drift, p, ImVec2(size.x, half_h),
                          view_start, view_seconds, draw, color_l, gain_scale);
    draw_channel_waveform(wav, t.peaks.ch_minimum[1], t.peaks.ch_maximum[1],
                          t.peaks.ch_coarse_minimum[1], t.peaks.ch_coarse_maximum[1],
                          1, offset, drift, ImVec2(p.x, divider_y), ImVec2(size.x, size.y - half_h),
                          view_start, view_seconds, draw, color_r, gain_scale);
  } else {
    draw_channel_waveform(wav, t.peaks.minimum, t.peaks.maximum,
                          t.peaks.coarse_minimum, t.peaks.coarse_maximum,
                          -1, offset, drift, p, size,
                          view_start, view_seconds, draw, color_l, gain_scale);
  }
}

// Widest gap between what strong matches measured and the fitted line, in ms.
double match_spread_ms(const SyncResult& sync) {
  constexpr double kStrong = 15.0;
  double low = 0.0, high = 0.0;
  bool any = false;
  for (const SyncAnchor& a : sync.anchors) {
    if (a.strength < kStrong) continue;
    const double r = a.lag_seconds - (sync.map.offset_seconds + sync.map.drift * a.ref_seconds);
    low = any ? std::min(low, r) : r;
    high = any ? std::max(high, r) : r;
    any = true;
  }
  return (high - low) * 1000.0;
}

ImVec4 confidence_color(double confidence) { return confidence >= 0.7 ? kGreen : (confidence >= 0.4 ? kAmber : kRed); }

void draw_alignment_report(App& app) {
  ImGui::Spacing();
  ImGui::SeparatorText("Alignment");

  bool any = false;
  for (const auto& e : app.edit.tracks) any = any || e.sync != nullptr;
  if (!any) {
    ImGui::TextColored(kMuted, "Press Align tracks to measure how the microphones line up.");
    return;
  }
  ImGui::TextWrapped(
      "Your original WAV files are not changed. Aligning only saves the numbers below to %s in the session folder. "
      "New WAV files are written only when you export, into the \"aligned\" folder.",
      kEditsFileName);
  ImGui::Spacing();

  static const char* const kHeaders[] = {"Track", "Confidence", "Offset", "Drift", "Drift over recording", "Matches", "Spread", "Timestamps said"};
  static const char* const kHeaderTips[] = {
      "Each track is compared with the master track.",
      "How much to trust this result. Hover over a track's label for the explanation.",
      "How far into this track the master track's first moment falls. Positive means this microphone started first.",
      "How much faster this microphone's clock runs than the master, in parts per million.",
      "How far the two tracks would slide apart by the end of this recording without correction.",
      "Sharp sounds found in both tracks and used for the fit, out of all that were strong enough to consider.",
      "How far apart the strongest matches landed. Zero to 1 ms is tight. Several ms suggests echoes, effects on the "
      "microphone, or sounds made at different distances.",
      "The estimate from device capture times alone, before listening to the audio. It doesn't know each device's "
      "own latency.",
  };
  if (ImGui::BeginTable("alignment", 8, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn(kHeaders[0], ImGuiTableColumnFlags_WidthStretch, 3.0f);
    for (int c = 1; c < 8; c++) ImGui::TableSetupColumn(kHeaders[c], ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < 8; c++) {
      ImGui::TableSetColumnIndex(c);
      ImGui::TableHeader(kHeaders[c]);
      tip(kHeaderTips[c]);
    }
    for (size_t i = 0; i < app.edit.tracks.size(); i++) {
      const std::shared_ptr<const SyncResult>& s = app.edit.tracks[i].sync;
      if (!s) continue;
      const double seconds = app.session.tracks[i].wav->seconds();
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(app.session.tracks[i].name.c_str());
      ImGui::TableNextColumn();
      ImGui::TextColored(confidence_color(s->confidence), "%s", confidence_label(s->confidence));
      ImGui::TableNextColumn();
      ImGui::Text("%.2f ms", s->map.offset_seconds * 1000.0);
      ImGui::TableNextColumn();
      ImGui::Text("%.2f ppm%s", s->map.drift * 1e6, s->drift_measured ? "" : " (estimate)");
      ImGui::TableNextColumn();
      ImGui::Text("%.2f ms", s->map.drift * seconds * 1000.0);
      ImGui::TableNextColumn();
      ImGui::Text("%d of %d", s->anchors_used, s->anchors_usable);
      ImGui::TableNextColumn();
      const double spread = match_spread_ms(*s);
      ImGui::TextColored(spread > 2.0 ? kAmber : kMuted, "%.2f ms", spread);
      ImGui::TableNextColumn();
      if (s->has_timestamps) {
        ImGui::Text("%.2f ms", s->timestamps.offset_seconds * 1000.0);
      } else {
        ImGui::TextColored(kMuted, "none");
      }
    }
    ImGui::EndTable();
  }

  for (size_t i = 0; i < app.edit.tracks.size(); i++) {
    const std::shared_ptr<const SyncResult>& s = app.edit.tracks[i].sync;
    if (!s) continue;
    ImGui::PushID(int(i));
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, confidence_color(s->confidence));
    ImGui::TextWrapped("%s: %s", app.session.tracks[i].name.c_str(), s->message.c_str());
    ImGui::PopStyleColor();
    if (ImGui::TreeNode("Every sound that was compared")) {
      std::vector<SyncAnchor> anchors = s->anchors;
      std::sort(anchors.begin(), anchors.end(),
                [](const SyncAnchor& a, const SyncAnchor& b) { return a.ref_seconds < b.ref_seconds; });
      if (ImGui::BeginTable("matches", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Measured offset");
        ImGui::TableSetupColumn("Off the line by");
        ImGui::TableSetupColumn("Strength");
        ImGui::TableSetupColumn("Used");
        ImGui::TableHeadersRow();
        for (const SyncAnchor& a : anchors) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(format_time_precise(a.ref_seconds).c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%.3f ms", a.lag_seconds * 1000.0);
          ImGui::TableNextColumn();
          ImGui::Text("%+.3f ms", (a.lag_seconds - (s->map.offset_seconds + s->map.drift * a.ref_seconds)) * 1000.0);
          ImGui::TableNextColumn();
          ImGui::Text("%.0f", a.strength);
          ImGui::TableNextColumn();
          if (a.inlier) {
            ImGui::TextColored(kGreen, "yes");
          } else {
            ImGui::TextColored(kMuted, "no");
          }
        }
        ImGui::EndTable();
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
}

void draw_edit_tab(App& app) {
  collect_sync_results(app);
  sync_playback(app);

  const bool busy = app.job_running.load();
  if (!app.session_is_permanent && !app.session.dir.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.46f, 0.76f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.56f, 0.88f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.36f, 0.60f, 1.0f));
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Save project...")) {
      const char* start = app.last_open_dir.empty() ? app.base_dir.c_str() : app.last_open_dir.c_str();
      SDL_ShowOpenFolderDialog(on_save_folder, &app, app.window, start, false);
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    tip("Choose a folder to save this project to. The audio files are copied there. "
        "Your original files are not changed.");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kAmber, "Not saved — changes may be lost.");
    ImGui::SameLine();
  }
  if (ImGui::Button("Open session folder...")) {
    const char* start = app.last_open_dir.empty() ? app.base_dir.c_str() : app.last_open_dir.c_str();
    SDL_ShowOpenFolderDialog(on_session_folder, &app, app.window, start, false);
  }
  tip("Open a folder of recordings made earlier. Any edits you made to it come back too.");
  ImGui::SameLine();
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Import files...")) show_import_dialog(app);
  ImGui::EndDisabled();
  tip("Line up video and audio files you already have, for example a phone video and a separate microphone. The audio "
      "is pulled out into a temporary session, and the first video becomes the master. Your files are not changed. This "
      "needs ffmpeg installed. You can also drag files onto this window.");
  if (!app.session.dir.empty()) {
    ImGui::SameLine();
    ImGui::BeginDisabled(app.undo_stack.empty());
    if (ImGui::Button("Undo")) undo_edit(app);
    if (!app.undo_stack.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Undo %s (Ctrl+Z)", describe_change(app.undo_stack.back(), app.committed).c_str());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(app.redo_stack.empty());
    if (ImGui::Button("Redo")) redo_edit(app);
    if (!app.redo_stack.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Redo %s (Ctrl+Y)", describe_change(app.redo_stack.back(), app.committed).c_str());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(app.session.dir.u8string().c_str());
  }
  if (!app.edit_error.empty()) ImGui::TextColored(kRed, "%s", app.edit_error.c_str());
  if (app.tracks.empty()) {
    ImGui::Spacing();
    if (app.job_running.load()) {
      ImGui::ProgressBar(app.job_progress.load(), ImVec2(240.0f * ImGui::GetStyle().FontScaleDpi, 0.0f), app.job_label.c_str());
    } else if (!app.job_message.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, app.job_failed ? kRed : kGreen);
      ImGui::TextWrapped("%s", app.job_message.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Record something, open a folder of recordings, or drag video and audio files here.");
    return;
  }

  const float scale = ImGui::GetStyle().FontScaleDpi;

  // Transport
  ImGui::Spacing();
  const bool recording = app.recorder.recording();
  ImGui::BeginDisabled(recording);
  if (ImGui::Button(app.playback.playing() ? "Pause" : "Play", ImVec2(70.0f * scale, 0.0f))) toggle_playback(app);
  tip("Play or pause all tracks together (Space).");
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Back to start")) app.playback.seek(effective_in(app));
  tip("Move the playhead to the start, or to the in point if one is set.");
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::Text("%s", format_time_precise(app.playback.position()).c_str());
  tip("Playhead position, in minutes and seconds. It can be slightly negative when a track started before the master track.");
  ImGui::SameLine(0.0f, 24.0f * scale);
  if (ImGui::Button("Set in")) {
    app.edit.in_set = true;
    app.edit.in_point = std::min(app.playback.position(), effective_out(app));
  }
  tip("Export starts from the playhead, trimming off everything before it. Audio stays in your original files. "
      "Right-click to clear.");
  if (ImGui::BeginPopupContextItem("in_menu")) {
    if (ImGui::MenuItem("Clear in point", nullptr, false, app.edit.in_set)) app.edit.in_set = false;
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Set out")) {
    app.edit.out_set = true;
    app.edit.out_point = std::max(app.playback.position(), effective_in(app));
  }
  tip("Export ends at the playhead, trimming off everything after it. Audio stays in your original files. "
      "Right-click to clear.");
  if (ImGui::BeginPopupContextItem("out_menu")) {
    if (ImGui::MenuItem("Clear out point", nullptr, false, app.edit.out_set)) app.edit.out_set = false;
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (app.edit.in_set || app.edit.out_set) {
    ImGui::TextColored(kAmber, "Trim %s to %s", format_time_precise(effective_in(app)).c_str(),
                       format_time_precise(effective_out(app)).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Clear trim")) {
      app.edit.in_set = false;
      app.edit.out_set = false;
    }
    tip("Remove both the in point and the out point, so export covers the whole recording.");
  } else {
    ImGui::TextColored(kMuted, "No trim");
  }
  ImGui::SameLine(0.0f, 24.0f * scale);
  if (ImGui::Button("Zoom in")) {
    double center = app.view_start + app.view_seconds * 0.5;
    app.view_seconds = std::max(0.05, app.view_seconds * 0.5);
    app.view_start = center - app.view_seconds * 0.5;
  }
  tip("Show less time, in more detail. You can also scroll over a waveform.");
  ImGui::SameLine();
  if (ImGui::Button("Zoom out")) {
    double center = app.view_start + app.view_seconds * 0.5;
    app.view_seconds = std::min(app.timeline_end - app.timeline_start, app.view_seconds * 2.0);
    app.view_start = center - app.view_seconds * 0.5;
  }
  tip("Show more time, in less detail.");
  ImGui::SameLine();
  if (ImGui::Button("Fit")) {
    app.view_start = app.timeline_start;
    app.view_seconds = app.timeline_end - app.timeline_start;
  }
  tip("Show the whole recording.");
  app.view_start = std::max(app.timeline_start, std::min(app.view_start, app.timeline_end - app.view_seconds));

  // Sync and export
  ImGui::Spacing();
  ImGui::BeginDisabled(busy || app.tracks.size() < 2);
  if (ImGui::Button("Align tracks")) start_sync_job(app);
  tip("Finds sounds that appear on several tracks, like your claps, and works out how each track lines up with the "
      "master track. This only saves timing numbers. Your WAV files are not changed.");
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::Checkbox("Hear aligned", &app.aligned);
  tip("Turn this off to see and hear the tracks as they were recorded, for comparison.");
  ImGui::SameLine(0.0f, 24.0f * scale);
  bool peaks_ready = true;
  for (const auto& t : app.tracks) peaks_ready = peaks_ready && t->peaks_ready.load();
  ImGui::BeginDisabled(busy || !peaks_ready);
  if (ImGui::Button("Normalize all")) normalize_all(app);
  tip("Sets each track's gain so its loudest moment reaches -1 dB. Claps are often the loudest thing, so to bring up "
      "speech, set the in and out points past the claps first. Your WAV files are not changed.");
  ImGui::EndDisabled();
  ImGui::SameLine(0.0f, 24.0f * scale);
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Export aligned WAVs")) start_export_job(app);
  tip("Writes one new WAV per track into an \"aligned\" folder in the session, all the same length and starting at the "
      "same moment. Drop them in any editor at time zero and they line up. Includes your gain and trim.");
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::Checkbox("Stereo mix", &app.write_mix);
  tip("Also writes mix.wav, combining the tracks you can currently hear. It's scaled down if it would otherwise clip.");
  ImGui::SameLine();
  ImGui::Checkbox("Match master's length", &app.match_master);
  tip("Makes every exported file start and end exactly with the master track, cutting anything outside it. Use this "
      "to lay audio under a video: put the video and the files at time zero. In and out points take priority.");
  if (busy) {
    ImGui::SameLine();
    ImGui::ProgressBar(app.job_progress.load(), ImVec2(160.0f * scale, 0.0f), app.job_label.c_str());
  }
  // This line is always here, so a message appearing doesn't shift the timeline.
  if (!busy && !app.job_message.empty()) {
    ImGui::TextColored(app.job_failed ? kRed : kGreen, "%s", app.job_message.c_str());
  } else {
    ImGui::TextUnformatted(" ");
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
    TrackEditState& e = app.edit.tracks[i];
    const SessionTrack& st = app.session.tracks[i];
    ImGui::PushID(int(i));
    const ImVec2 lane_pos = ImGui::GetCursorScreenPos();

    ImGui::BeginChild("controls", ImVec2(panel_width, lane_height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted(st.name.c_str());
    tip((st.wav_path.u8string() + "\n\nRight-click to reset this track.").c_str());
    if (ImGui::BeginPopupContextItem("track_menu")) {
      if (ImGui::MenuItem("Reset this track")) {
        e.gain_db = 0.0f;
        e.mute = false;
        e.offset_ms = e.sync ? float(e.sync->map.offset_seconds * 1000.0) : 0.0f;
        e.drift_ppm = e.sync ? float(e.sync->map.drift * 1e6) : 0.0f;
        t.playback.solo.store(false);
      }
      ImGui::EndPopup();
    }
    bool solo = t.playback.solo.load();
    ImGui::PushStyleColor(ImGuiCol_Button, e.mute ? ImVec4(0.72f, 0.42f, 0.14f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::SmallButton("Mute")) e.mute = !e.mute;
    ImGui::PopStyleColor();
    tip("Silence this track. It's also left out of the stereo mix, but still gets its own exported file.");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, solo ? ImVec4(0.20f, 0.55f, 0.30f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::SmallButton("Solo")) click_solo(app, i);
    ImGui::PopStyleColor();
    tip("Hear only this track. Click again to hear everything. Shift-click or Ctrl-click to add another track.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##gain", &e.gain_db, -40.0f, 40.0f, "%.1f dB");
    tip("Volume in decibels. 0 dB leaves it unchanged, and +6 dB is about twice the signal level. Applies to playback "
        "and export. Double-click to type a value, right-click to reset.");
    if (ImGui::BeginPopupContextItem("gain_menu")) {
      if (ImGui::MenuItem("Reset gain to 0 dB")) e.gain_db = 0.0f;
      ImGui::EndPopup();
    }
    if (i == app.edit.master) {
      ImGui::TextColored(kGreen, "Master track");
      tip("The track everything else lines up with. Its timing is never changed. Choose a video's own audio here if "
          "you're laying these tracks under that video.");
    } else {
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
      ImGui::DragFloat("ms##offset", &e.offset_ms, 0.1f, 0.0f, 0.0f, "%.2f");
      tip("Offset, in milliseconds: how far into this track the master track's first moment falls. Positive means "
          "this microphone started first. Drag to nudge, double-click to type, right-click to reset.");
      if (ImGui::BeginPopupContextItem("offset_menu")) {
        if (e.sync) {
          const float aligned_ms = float(e.sync->map.offset_seconds * 1000.0);
          if (ImGui::MenuItem(("Reset to alignment result (" + format_number(aligned_ms, " ms") + ")").c_str())) {
            e.offset_ms = aligned_ms;
          }
        }
        if (ImGui::MenuItem("Reset to 0 ms")) e.offset_ms = 0.0f;
        ImGui::EndPopup();
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-40.0f * scale);
      ImGui::DragFloat("ppm##drift", &e.drift_ppm, 0.1f, -500.0f, 500.0f, "%.2f");
      tip("Drift, in parts per million: how much faster this microphone's clock runs than the master. 1 ppm adds "
          "up to 3.6 ms per hour, and 100 ppm to 360 ms per hour. Drag to nudge, double-click to type, right-click to "
          "reset.");
      if (ImGui::BeginPopupContextItem("drift_menu")) {
        if (e.sync) {
          const float aligned_ppm = float(e.sync->map.drift * 1e6);
          if (ImGui::MenuItem(("Reset to alignment result (" + format_number(aligned_ppm, " ppm") + ")").c_str())) {
            e.drift_ppm = aligned_ppm;
          }
        }
        if (ImGui::MenuItem("Reset to 0 ppm")) e.drift_ppm = 0.0f;
        ImGui::EndPopup();
      }
      if (e.sync) {
        ImVec4 color = e.sync->confidence >= 0.7 ? kGreen : (e.sync->confidence >= 0.4 ? kAmber : kRed);
        ImGui::TextColored(color, "%s confidence", confidence_label(e.sync->confidence));
        if (ImGui::IsItemHovered()) {
          ImGui::SetNextWindowSize(ImVec2(420.0f * scale, 0.0f));
          ImGui::BeginTooltip();
          ImGui::TextWrapped("%s", e.sync->message.c_str());
          ImGui::TextColored(kMuted, "Method: %s. Matches used: %d of %d.", sync_method_name(e.sync->method),
                             e.sync->anchors_used, e.sync->anchors_usable);
          ImGui::EndTooltip();
        }
        ImGui::SameLine();
      }
      ImGui::BeginDisabled(busy);
      if (ImGui::SmallButton("Make master")) set_master(app, i);
      tip("Line every other track up with this one instead. This aligns the tracks again.");
      ImGui::EndDisabled();
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
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // Scrolling here zooms instead of scrolling the page.
    if (ImGui::IsItemActive() && !recording) {
      double when = app.view_start + (ImGui::GetIO().MousePos.x - canvas_pos.x) / canvas_width * app.view_seconds;
      // Only seek on first click or actual horizontal movement — IsItemActive() is
      // true every frame while held, which would restart the crossfade each frame
      // and cause the playhead position to replay in a loop.
      if (ImGui::IsItemClicked() || ImGui::GetIO().MouseDelta.x != 0.0f) {
        app.playback.seek(std::max(app.timeline_start, std::min(when, app.timeline_end)));
      }
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
    const bool silent = e.mute || (any_solo && !t.playback.solo.load());
    draw_waveform(t, e.gain_db, canvas_pos, canvas_size, app.view_start, app.view_seconds, draw, silent);
    const SyncAnchor* hovered_anchor = nullptr;
    if (e.sync && app.aligned) {
      const ImVec2 mouse = ImGui::GetMousePos();
      const float pip_half = 5.0f * scale;
      const float pip_h = 11.0f * scale;
      for (const auto& a : e.sync->anchors) {
        float x = canvas_pos.x + float((a.ref_seconds - app.view_start) / app.view_seconds * canvas_width);
        if (x < canvas_pos.x - pip_half || x > canvas_pos.x + canvas_width + pip_half) continue;
        if (a.inlier) {
          draw->AddTriangleFilled(ImVec2(x - pip_half, canvas_pos.y), ImVec2(x + pip_half, canvas_pos.y),
                                  ImVec2(x, canvas_pos.y + pip_h), IM_COL32(120, 210, 130, 230));
          if (!hovered_anchor && mouse.x >= x - pip_half && mouse.x <= x + pip_half &&
              mouse.y >= canvas_pos.y && mouse.y <= canvas_pos.y + pip_h) {
            hovered_anchor = &a;
          }
        } else {
          draw->AddTriangleFilled(ImVec2(x - pip_half * 0.5f, canvas_pos.y),
                                  ImVec2(x + pip_half * 0.5f, canvas_pos.y),
                                  ImVec2(x, canvas_pos.y + pip_h * 0.5f), IM_COL32(150, 155, 160, 100));
        }
      }
    }
    draw->PopClipRect();
    if (hovered_anchor) {
      const double residual = hovered_anchor->lag_seconds -
                              (e.sync->map.offset_seconds + e.sync->map.drift * hovered_anchor->ref_seconds);
      ImGui::SetTooltip("Sync point at %s\nMeasured offset: %.2f ms\nOff fit by: %+.2f ms\nStrength: %.0f",
                        format_time_precise(hovered_anchor->ref_seconds).c_str(),
                        hovered_anchor->lag_seconds * 1000.0, residual * 1000.0, hovered_anchor->strength);
    }
    ImGui::SetCursorScreenPos(ImVec2(lane_pos.x, lane_pos.y + lane_height + 4.0f * scale));
    ImGui::PopID();
  }
  float lanes_bottom = ImGui::GetCursorScreenPos().y;

  // Overlays that span every lane.
  draw->PushClipRect(ImVec2(canvas_x, lanes_top), ImVec2(canvas_x + canvas_width, lanes_bottom), true);
  auto to_x = [&](double when) { return canvas_x + float((when - app.view_start) / app.view_seconds * canvas_width); };
  if (app.edit.in_set) {
    draw->AddRectFilled(ImVec2(canvas_x, lanes_top), ImVec2(to_x(app.edit.in_point), lanes_bottom), IM_COL32(0, 0, 0, 130));
  }
  if (app.edit.out_set) {
    draw->AddRectFilled(ImVec2(to_x(app.edit.out_point), lanes_top), ImVec2(canvas_x + canvas_width, lanes_bottom),
                        IM_COL32(0, 0, 0, 130));
  }
  const float head = to_x(app.playback.position());
  draw->AddLine(ImVec2(head, lanes_top), ImVec2(head, lanes_bottom), IM_COL32(240, 240, 240, 230), 1.5f);
  draw->PopClipRect();

  ImGui::TextColored(kMuted, "Click a waveform to move the playhead. Scroll to zoom. Green pips mark sounds used to align (hover for details). Gray pips were found but not used.");

  draw_alignment_report(app);
}

void apply_dialog_results(App& app) {
  std::string record_dir, session_dir, save_dir;
  std::vector<std::string> import_files;
  {
    std::lock_guard<std::mutex> lock(app.dialog_mutex);
    record_dir.swap(app.dialog_record_dir);
    session_dir.swap(app.dialog_session_dir);
    save_dir.swap(app.dialog_save_dir);
    import_files.swap(app.dialog_import_files);
  }
  if (!record_dir.empty()) {
    app.base_dir = record_dir;
    app.disk_checked_at = -100.0;
    save_prefs(app);
  }
  if (!session_dir.empty()) {
    app.last_open_dir = fs::u8path(session_dir).parent_path().u8string();
    load_session_into_editor(app, fs::u8path(session_dir));
    app.session_is_permanent = true;
    app.tab_request = 2;
    save_prefs(app);
  }
  if (!save_dir.empty()) {
    start_save_session_job(app, fs::u8path(save_dir));
  }
  if (!import_files.empty()) {
    start_import_job(app, import_files);
  }
}

// A dropped folder or project opens as a session. Other files are imported together.
void apply_dropped_files(App& app) {
  std::vector<std::string> files;
  files.swap(app.dropped_files);
  if (files.empty() || app.recorder.recording()) return;
  std::error_code ec;
  fs::path first = fs::u8path(files[0]);
  if (files.size() == 1 && (fs::is_directory(first, ec) || first.extension() == ".reca")) {
    if (first.extension() == ".reca") first = first.parent_path();
    load_session_into_editor(app, first);
    app.tab_request = 2;
  } else {
    start_import_job(app, files);
  }
}

// Test hook: plays clicks and keys from a text file into the UI, one line every
// few frames, and logs the resulting state. Lines look like "click 51 296",
// "rclick 300 299", "ctrl z", "ctrlshift z", "key space", "shift on", "shift off",
// "wait 30" or "log label".
struct UiScript {
  FILE* input = nullptr;
  FILE* output = nullptr;
  int wait_frames = 0;
};

ImGuiKey script_key(const char* name) {
  if (!strcmp(name, "space")) return ImGuiKey_Space;
  if (!strcmp(name, "z")) return ImGuiKey_Z;
  if (!strcmp(name, "y")) return ImGuiKey_Y;
  return ImGuiKey_None;
}

void log_ui_state(App& app, FILE* out, const char* label) {
  fprintf(out, "%-22s", label);
  for (size_t i = 0; i < app.edit.tracks.size(); i++) {
    const TrackEditState& e = app.edit.tracks[i];
    fprintf(out, " [gain %.1f mute %d solo %d]", e.gain_db, e.mute ? 1 : 0, app.tracks[i]->playback.solo.load() ? 1 : 0);
  }
  if (app.edit.in_set) fprintf(out, " in %.2f", app.edit.in_point);
  if (app.edit.out_set) fprintf(out, " out %.2f", app.edit.out_point);
  fprintf(out, " playing %d undo %zu redo %zu\n", app.playback.playing() ? 1 : 0, app.undo_stack.size(),
          app.redo_stack.size());
  fflush(out);
}

// Returns false once the script has finished.
bool run_ui_script(App& app, UiScript& script) {
  if (script.wait_frames-- > 0) return true;
  script.wait_frames = 12;

  char line[256];
  if (!fgets(line, sizeof(line), script.input)) return false;
  char word[32] = {};
  char text[128] = {};
  float x = 0.0f, y = 0.0f;
  sscanf(line, "%31s %127[^\n]", word, text);
  sscanf(text, "%f %f", &x, &y);
  ImGuiIO& io = ImGui::GetIO();
  auto click = [&](ImGuiMouseButton button) {
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(button, true);
    io.AddMouseButtonEvent(button, false);
  };
  auto press = [&](const char* name) {
    io.AddKeyEvent(script_key(name), true);
    io.AddKeyEvent(script_key(name), false);
  };
  if (!strcmp(word, "click")) {
    click(ImGuiMouseButton_Left);
  } else if (!strcmp(word, "rclick")) {
    click(ImGuiMouseButton_Right);
  } else if (!strcmp(word, "key")) {
    press(text);
  } else if (!strcmp(word, "ctrl") || !strcmp(word, "ctrlshift")) {
    const bool shift = !strcmp(word, "ctrlshift");
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
    press(text);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
  } else if (!strcmp(word, "shift")) {
    io.AddKeyEvent(ImGuiMod_Shift, !strcmp(text, "on"));
  } else if (!strcmp(word, "wait")) {
    script.wait_frames = int(x);
  } else if (!strcmp(word, "log")) {
    log_ui_state(app, script.output, text);
  }
  return true;
}

bool save_screenshot(SDL_Renderer* renderer, const char* path) {
  SDL_Window* window = SDL_GetRenderWindow(renderer);
  int width = 0, height = 0, pixel_width = 0, pixel_height = 0;
  SDL_GetWindowSize(window, &width, &height);
  SDL_GetRenderOutputSize(renderer, &pixel_width, &pixel_height);
  SDL_Log("Screenshot: window %dx%d, framebuffer %dx%d, font density %.2f", width, height,
          pixel_width, pixel_height, ImGui::GetIO().DisplayFramebufferScale.x);
  SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
  if (!surface) return false;
  bool ok = SDL_SaveBMP(surface, path);
  SDL_DestroySurface(surface);
  return ok;
}

}  // namespace

// Optional flags for automated checks: --open DIR, --align, --arm-all, --tab edit,
// --import FILE (repeatable), --out DIR,
// --script IN.txt OUT.txt, --screenshot FILE.bmp (saves after --frames N frames or
// when the script ends, then exits).
int main(int argc, char** argv) {
  const char* open_dir = nullptr;
  const char* screenshot_path = nullptr;
  const char* base_dir_override = nullptr;
  std::vector<std::string> import_paths;
  UiScript script;
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
    } else if (!strcmp(argv[i], "--script") && i + 2 < argc) {
      script.input = fopen(argv[++i], "r");
      script.output = fopen(argv[++i], "w");
    } else if (!strcmp(argv[i], "--import") && i + 1 < argc) {
      import_paths.push_back(argv[++i]);
    } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
      base_dir_override = argv[++i];
    } else if (!strcmp(argv[i], "--align")) {
      auto_align = true;
    } else if (!strcmp(argv[i], "--arm-all")) {
      arm_all = true;
    } else if (!strcmp(argv[i], "--tab") && i + 1 < argc) {
      start_on_edit = !strcmp(argv[++i], "edit");
    } else if (argv[i][0] != '-') {
      // Plain path argument: treat a .reca file as its parent directory.
      fs::path p = fs::u8path(argv[i]);
      if (p.extension() == ".reca") p = p.parent_path();
      static std::string reca_dir = p.u8string();
      open_dir = reca_dir.c_str();
    }
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "record-align", SDL_GetError(), nullptr);
    return 1;
  }
  const float dpi = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
  SDL_Window* window = SDL_CreateWindow("record-align", int(1180 * dpi), int(760 * dpi), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
#ifdef _WIN32
  if (window) {
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (hwnd) {
      HICON hSmall = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
      HICON hBig   = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
      if (hSmall) SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
      if (hBig)   SendMessage(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hBig);
    }
  }
#endif
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "record-align", SDL_GetError(), nullptr);
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
  // Blue marks what's selected, so plain buttons are neutral. Tabs have no box
  // until selected, when they join the page below with a bright top edge.
  ImVec4* colors = style.Colors;
  const ImVec4 accent(0.36f, 0.66f, 0.96f, 1.0f);
  const ImVec4 selected_tab(0.20f, 0.22f, 0.26f, 1.0f);
  colors[ImGuiCol_Button] = ImVec4(0.26f, 0.28f, 0.32f, 1.0f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.34f, 0.37f, 0.42f, 1.0f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.42f, 0.46f, 0.52f, 1.0f);
  colors[ImGuiCol_Tab] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  colors[ImGuiCol_TabDimmed] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.17f, 0.18f, 0.21f, 1.0f);
  colors[ImGuiCol_TabSelected] = selected_tab;
  colors[ImGuiCol_TabDimmedSelected] = selected_tab;
  colors[ImGuiCol_TabSelectedOverline] = accent;
  colors[ImGuiCol_TabDimmedSelectedOverline] = accent;
  style.TabRounding = 0.0f;
  style.TabBarBorderSize = 1.0f;
  style.TabBarOverlineSize = 2.0f;
  style.ScaleAllSizes(dpi);
  style.FontScaleDpi = dpi;
#ifdef __APPLE__
  const char* font_path = "/System/Library/Fonts/Supplemental/Arial.ttf";
#else
  const char* font_path = "C:/Windows/Fonts/segoeui.ttf";
#endif
  if (fs::exists(font_path)) io.Fonts->AddFontFromFileTTF(font_path, 16.0f);
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  auto app_owner = std::make_unique<App>();
  App& app = *app_owner;
  app.window = window;
  const char* music = SDL_GetUserFolder(SDL_FOLDER_MUSIC);
  app.base_dir = (fs::u8path(music ? music : ".") / "record-align Recordings").u8string();
  load_prefs(app);
  if (base_dir_override) app.base_dir = base_dir_override;
  refresh_devices(app);
  if (arm_all) {
    for (const auto& device : app.devices) {
      std::string err;
      if (!app.recorder.arm(device, &err)) app.record_error = err;
    }
  }
  if (open_dir) load_session_into_editor(app, fs::u8path(open_dir));
  start_import_job(app, import_paths);
  if (auto_align) start_sync_job(app);
  if (start_on_edit || open_dir) app.tab_request = 2;

  bool running = true;
  int frame = 0;
  while (running) {
    SDL_Event event;
    // Stay light when nothing is moving. Meters and playback need steady redraws.
    const bool animating = !app.status.empty() || app.playback.playing() || app.job_running.load() || script.input;
    if (SDL_WaitEventTimeout(&event, animating ? 16 : 100)) {
      do {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT) running = false;
        if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) app.dropped_files.push_back(event.drop.data);
        if (event.type == SDL_EVENT_DROP_COMPLETE) apply_dropped_files(app);
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
          running = false;
        }
      } while (SDL_PollEvent(&event));
    }

    apply_dialog_results(app);
    collect_import_result(app);
    collect_save_result(app);
    app.status = app.recorder.status();  // Reads and resets the meters, so call it once per frame.

    bool script_done = false;
    if (script.input && frame > 30 && !app.job_running.load()) {
      bool peaks_done = true;
      for (const auto& t : app.tracks) peaks_done = peaks_done && t->peaks_ready.load();
      if (peaks_done) script_done = !run_ui_script(app, script);
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const ImVec2 tab_padding(18.0f * dpi, 9.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, tab_padding);
    const bool tab_bar_open = ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_DrawSelectedOverline);
    ImGui::PopStyleVar();
    if (tab_bar_open) {
      ImGuiTabItemFlags record_flags = app.tab_request == 1 ? ImGuiTabItemFlags_SetSelected : 0;
      ImGuiTabItemFlags edit_flags = app.tab_request == 2 ? ImGuiTabItemFlags_SetSelected : 0;
      ImGuiTabItemFlags settings_flags = app.tab_request == 3 ? ImGuiTabItemFlags_SetSelected : 0;
      app.tab_request = 0;
      auto begin_tab = [&](const char* label, bool selected_before, ImGuiTabItemFlags flags) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, tab_padding);
        ImGui::PushStyleColor(ImGuiCol_Text, selected_before ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kMuted);
        const bool open = ImGui::BeginTabItem(label, nullptr, flags);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return open;
      };
      bool edit_tab_open = false;
      if (begin_tab("Record", !app.edit_tab_was_open, record_flags)) {
        draw_record_tab(app);
        ImGui::EndTabItem();
      }
      if (begin_tab("Edit", app.edit_tab_was_open, edit_flags)) {
        edit_tab_open = true;
        draw_edit_tab(app);
        ImGui::EndTabItem();
      }
      if (begin_tab("Settings", false, settings_flags)) {
        draw_settings_tab(app);
        ImGui::EndTabItem();
      }
      app.edit_tab_was_open = edit_tab_open;
      ImGui::EndTabBar();
      // Only on the Edit tab, so a stray key press can't touch a recording.
      if (edit_tab_open && !app.tracks.empty() && !io.WantTextInput) {
        if (!io.KeyCtrl && !io.KeySuper && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Space, false)) toggle_playback(app);
      }
    }
    if (!app.tracks.empty()) {
      commit_edits(app);
      if (!io.WantTextInput && ((io.KeyCtrl || io.KeySuper) && !io.KeyAlt)) {
        const bool z = ImGui::IsKeyPressed(ImGuiKey_Z);
        if (z && !io.KeyShift) undo_edit(app);
        if ((z && io.KeyShift) || ImGui::IsKeyPressed(ImGuiKey_Y)) redo_edit(app);
      }
    }
    ImGui::End();

    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 20, 21, 25, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

    frame++;
    const bool settled = !app.job_running.load() && (app.tracks.empty() || app.tracks.back()->peaks_ready.load());
    if (screenshot_path && !script.input && frame >= screenshot_frames && settled) {
      save_screenshot(renderer, screenshot_path);
      running = false;
    }
    if (script_done) {
      if (screenshot_path) save_screenshot(renderer, screenshot_path);
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
