#include "audio/edits.h"

#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

namespace {

constexpr const char* kHeader = "record-align 1";
constexpr const char* kLegacyHeader = "mydaw-edits 1";

int method_to_int(SyncMethod method) { return method == SyncMethod::Audio ? 1 : (method == SyncMethod::Timestamps ? 2 : 0); }

SyncMethod int_to_method(int value) {
  return value == 1 ? SyncMethod::Audio : (value == 2 ? SyncMethod::Timestamps : SyncMethod::None);
}

// The rest of the line after "key ", without the newline.
std::string rest_of_line(const char* line, size_t key_length) {
  std::string value(line + key_length);
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
  return value;
}

bool starts_with(const char* line, const char* key) { return strncmp(line, key, strlen(key)) == 0; }

}  // namespace

bool save_edits(const fs::path& session_dir, const SessionEdits& edits, std::string* err) {
  const fs::path final_path = session_dir / kEditsFileName;
  fs::path temp_path = final_path;
  temp_path += ".tmp";

  FILE* f = _wfopen(temp_path.c_str(), L"w");
  if (!f) {
    if (err) *err = "Could not save edits to " + final_path.u8string();
    return false;
  }
  fprintf(f, "%s\n", kHeader);
  if (!edits.master_name.empty()) fprintf(f, "master %s\n", edits.master_name.c_str());
  if (edits.in_set) fprintf(f, "in %.17g\n", edits.in_seconds);
  if (edits.out_set) fprintf(f, "out %.17g\n", edits.out_seconds);
  for (const TrackEdit& t : edits.tracks) {
    fprintf(f, "track %s\n", t.name.c_str());
    fprintf(f, "offset_ms %.17g\ndrift_ppm %.17g\ngain_db %.17g\nmute %d\n", t.offset_ms, t.drift_ppm, t.gain_db,
            t.mute ? 1 : 0);
    if (!t.has_sync) continue;
    const SyncResult& s = t.sync;
    fprintf(f, "sync %.17g %.17g %d %.17g %d %d %d %d %.17g %.17g %d %.17g %.17g\n", s.map.offset_seconds, s.map.drift,
            method_to_int(s.method), s.confidence, s.drift_measured ? 1 : 0, s.nonlinear ? 1 : 0, s.anchors_used,
            s.anchors_usable, s.residual_rms_ms, s.matched_span_seconds, s.has_timestamps ? 1 : 0,
            s.timestamps.offset_seconds, s.timestamps.drift);
    fprintf(f, "message %s\n", s.message.c_str());
    for (const SyncAnchor& a : s.anchors) {
      fprintf(f, "anchor %.17g %.17g %.17g %.17g %d\n", a.ref_seconds, a.lag_seconds, a.strength, a.window_seconds,
              a.inlier ? 1 : 0);
    }
  }
  const bool wrote = ferror(f) == 0;
  const bool closed = fclose(f) == 0;
  std::error_code ec;
  if (wrote && closed) fs::rename(temp_path, final_path, ec);
  if (!wrote || !closed || ec) {
    if (err) *err = "Could not save edits to " + final_path.u8string();
    fs::remove(temp_path, ec);
    return false;
  }
  return true;
}

bool load_edits(const fs::path& session_dir, SessionEdits* out) {
  *out = SessionEdits();
  FILE* f = _wfopen((session_dir / kEditsFileName).c_str(), L"r");
  if (!f) f = _wfopen((session_dir / kLegacyEditsFileName).c_str(), L"r");
  if (!f) return false;

  char line[1024];
  if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }
  const std::string header = rest_of_line(line, 0);
  if (header != kHeader && header != kLegacyHeader) {
    fclose(f);
    return false;
  }
  TrackEdit* track = nullptr;
  while (fgets(line, sizeof(line), f)) {
    double value = 0.0;
    int flag = 0;
    if (starts_with(line, "master ")) {
      out->master_name = rest_of_line(line, strlen("master "));
    } else if (sscanf(line, "in %lf", &value) == 1) {
      out->in_set = true;
      out->in_seconds = value;
    } else if (sscanf(line, "out %lf", &value) == 1) {
      out->out_set = true;
      out->out_seconds = value;
    } else if (starts_with(line, "track ")) {
      out->tracks.emplace_back();
      track = &out->tracks.back();
      track->name = rest_of_line(line, strlen("track "));
    } else if (!track) {
      continue;
    } else if (sscanf(line, "offset_ms %lf", &value) == 1) {
      track->offset_ms = value;
    } else if (sscanf(line, "drift_ppm %lf", &value) == 1) {
      track->drift_ppm = value;
    } else if (sscanf(line, "gain_db %lf", &value) == 1) {
      track->gain_db = value;
    } else if (sscanf(line, "mute %d", &flag) == 1) {
      track->mute = flag != 0;
    } else if (starts_with(line, "sync ")) {
      SyncResult& s = track->sync;
      int method = 0, measured = 0, nonlinear = 0, stamped = 0;
      if (sscanf(line, "sync %lf %lf %d %lf %d %d %d %d %lf %lf %d %lf %lf", &s.map.offset_seconds, &s.map.drift,
                 &method, &s.confidence, &measured, &nonlinear, &s.anchors_used, &s.anchors_usable, &s.residual_rms_ms,
                 &s.matched_span_seconds, &stamped, &s.timestamps.offset_seconds, &s.timestamps.drift) == 13) {
        s.method = int_to_method(method);
        s.drift_measured = measured != 0;
        s.nonlinear = nonlinear != 0;
        s.has_timestamps = stamped != 0;
        track->has_sync = true;
      }
    } else if (starts_with(line, "message ")) {
      track->sync.message = rest_of_line(line, strlen("message "));
    } else if (starts_with(line, "anchor ")) {
      SyncAnchor a;
      int inlier = 0;
      if (sscanf(line, "anchor %lf %lf %lf %lf %d", &a.ref_seconds, &a.lag_seconds, &a.strength, &a.window_seconds,
                 &inlier) == 5) {
        a.inlier = inlier != 0;
        track->sync.anchors.push_back(a);
      }
    }
  }
  fclose(f);
  return true;
}
