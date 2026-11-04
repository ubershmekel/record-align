// Command-line front end, for testing capture and sync without the UI.
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "audio/capture.h"
#include "audio/recorder.h"
#include "audio/session.h"
#include "sync/export.h"
#include "sync/sync.h"

namespace fs = std::filesystem;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void handle_sigint(int) { g_stop = 1; }

int usage() {
  puts(
      "record-align-cli list\n"
      "record-align-cli record --device N [--device N...] [--seconds S] [--out DIR]\n"
      "record-align-cli timing SESSION_DIR\n"
      "record-align-cli sync SESSION_DIR [--export] [--mix]");
  return 2;
}

int cmd_list() {
  std::string err;
  auto devices = list_input_devices(&err);
  if (!err.empty()) {
    fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  for (size_t i = 0; i < devices.size(); i++) {
    printf("%zu: %s  (%d Hz, %d ch)\n", i, devices[i].name.c_str(), devices[i].sample_rate, devices[i].channels);
  }
  return 0;
}

int cmd_record(int argc, char** argv) {
  std::vector<int> indexes;
  double seconds = 0.0;
  fs::path out = "recordings";
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--device") && i + 1 < argc) {
      indexes.push_back(atoi(argv[++i]));
    } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
      seconds = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
      out = fs::u8path(argv[++i]);
    } else {
      return usage();
    }
  }
  if (indexes.empty()) return usage();

  std::string err;
  auto devices = list_input_devices(&err);
  Recorder recorder;
  for (int index : indexes) {
    if (index < 0 || index >= int(devices.size())) {
      fprintf(stderr, "No device %d\n", index);
      return 1;
    }
    if (!recorder.arm(devices[size_t(index)], &err)) {
      fprintf(stderr, "%s: %s\n", devices[size_t(index)].name.c_str(), err.c_str());
      return 1;
    }
  }
  if (!recorder.start(out, &err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  printf("Recording to %s\n", recorder.session_dir().u8string().c_str());
  std::signal(SIGINT, handle_sigint);

  auto last_print = std::chrono::steady_clock::now();
  while (!g_stop && (seconds <= 0.0 || recorder.elapsed_seconds() < seconds)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto now = std::chrono::steady_clock::now();
    if (now - last_print < std::chrono::seconds(5)) continue;
    last_print = now;
    printf("%7.1fs", recorder.elapsed_seconds());
    for (const auto& s : recorder.status()) {
      printf(" | peak %.3f gaps %" PRIu64 " dropped %" PRIu64 "%s", s.peak, s.gap_frames, s.dropped_frames,
             s.error.empty() ? "" : " ERROR");
    }
    puts("");
    fflush(stdout);
  }
  recorder.stop();
  for (const auto& s : recorder.status()) {
    printf("%s: %d Hz, %d ch, raw=%d, %" PRIu64 " frames (%.2fs), gaps %" PRIu64 ", dropped %" PRIu64
           ", OS glitches %" PRIu64 "%s%s\n",
           s.file_name.c_str(), s.sample_rate, s.channels, s.raw_mode ? 1 : 0, s.frames_written,
           s.sample_rate ? double(s.frames_written) / s.sample_rate : 0.0, s.gap_frames, s.dropped_frames,
           s.discontinuities, s.error.empty() ? "" : ", ERROR: ", s.error.c_str());
  }
  printf("Session: %s\n", recorder.session_dir().u8string().c_str());
  return 0;
}

AudioView view_of(const SessionTrack& t) {
  AudioView v;
  v.samples = t.wav->samples();
  v.frames = t.wav->frames();
  v.channels = t.wav->channels();
  v.sample_rate = t.wav->sample_rate();
  return v;
}

int cmd_timing(const char* dir) {
  Session session;
  std::string err;
  if (!load_session(fs::u8path(dir), &session, &err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::vector<ClockFit> fits;
  for (const auto& t : session.tracks) {
    ClockFit fit = fit_clock(t.timing, t.wav->sample_rate());
    fits.push_back(fit);
    printf("%s: %.2fs, %zu timestamps, used %d, jitter %.3f ms, gaps %" PRIu64 ", dropped %" PRIu64
           ", OS glitches %" PRIu64 "\n",
           t.name.c_str(), t.wav->seconds(), t.timing.size(), fit.samples_used, fit.rms_ms, t.gap_frames,
           t.dropped_frames, t.discontinuities);
  }
  for (size_t i = 1; i < fits.size(); i++) {
    TimeMap map;
    if (time_map_from_clocks(fits[0], fits[i], &map)) {
      printf("%s vs %s: started %.3f ms apart, drift %.2f ppm\n", session.tracks[i].name.c_str(),
             session.tracks[0].name.c_str(), map.offset_seconds * 1000.0, map.drift * 1e6);
    } else {
      printf("%s vs %s: not enough timing data\n", session.tracks[i].name.c_str(), session.tracks[0].name.c_str());
    }
  }
  return 0;
}

int cmd_sync(int argc, char** argv) {
  if (argc < 1) return usage();
  bool do_export = false;
  bool mix = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--export")) {
      do_export = true;
    } else if (!strcmp(argv[i], "--mix")) {
      mix = true;
    } else {
      return usage();
    }
  }

  Session session;
  std::string err;
  if (!load_session(fs::u8path(argv[0]), &session, &err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  const SessionTrack& ref = session.tracks[0];
  const ClockFit ref_clock = fit_clock(ref.timing, ref.wav->sample_rate());

  std::vector<ExportTrack> tracks;
  ExportTrack first;
  first.audio = view_of(ref);
  first.name = ref.name;
  tracks.push_back(first);
  printf("Reference: %s\n", ref.name.c_str());

  for (size_t i = 1; i < session.tracks.size(); i++) {
    const SessionTrack& t = session.tracks[i];
    TimeMap prior;
    bool has_prior = time_map_from_clocks(ref_clock, fit_clock(t.timing, t.wav->sample_rate()), &prior);
    SyncResult r = align_tracks(view_of(ref), view_of(t), has_prior ? &prior : nullptr);
    printf("%s\n  confidence %s (%.2f), method: %s\n  %s\n", t.name.c_str(), confidence_label(r.confidence),
           r.confidence, sync_method_name(r.method), r.message.c_str());
    if (has_prior) {
      printf("  timestamps alone said: offset %.2f ms, drift %.2f ppm\n", prior.offset_seconds * 1000.0,
             prior.drift * 1e6);
    }
    for (const auto& a : r.anchors) {
      printf("    t=%8.2fs lag=%10.3f ms strength=%6.1f %s\n", a.ref_seconds, a.lag_seconds * 1000.0, a.strength,
             a.inlier ? "used" : "");
    }
    ExportTrack track;
    track.audio = view_of(t);
    track.map = r.map;
    track.name = t.name;
    tracks.push_back(track);
  }

  if (do_export) {
    ExportOptions options;
    options.out_dir = session.dir / "aligned";
    options.sample_rate = ref.wav->sample_rate();
    options.write_mix = mix;
    if (!export_tracks(tracks, options, nullptr, &err)) {
      fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    printf("Exported to %s\n", options.out_dir.u8string().c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const char* command = argv[1];
  if (!strcmp(command, "list")) return cmd_list();
  if (!strcmp(command, "record")) return cmd_record(argc - 2, argv + 2);
  if (!strcmp(command, "timing") && argc == 3) return cmd_timing(argv[2]);
  if (!strcmp(command, "sync")) return cmd_sync(argc - 2, argv + 2);
  return usage();
}
