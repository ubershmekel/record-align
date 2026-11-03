#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>

#include "audio/edits.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  explicit TempDir(const char* name) {
    path = fs::temp_directory_path() / (std::string("mydaw_test_") + name);
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

}  // namespace

TEST_CASE("edits survive a save and load exactly") {
  TempDir dir("edits_roundtrip");
  SessionEdits edits;
  edits.in_set = true;
  edits.in_seconds = 1.25;
  TrackEdit plain;
  plain.name = "01_Microphone (HD Webcam)";
  plain.gain_db = -3.5;
  plain.mute = true;
  edits.tracks.push_back(plain);

  TrackEdit aligned;
  aligned.name = "02_Mic";
  aligned.offset_ms = 78.181234567;
  aligned.drift_ppm = -1.31;
  aligned.has_sync = true;
  aligned.sync.map.offset_seconds = 0.078181234567;
  aligned.sync.map.drift = -1.31e-6;
  aligned.sync.method = SyncMethod::Audio;
  aligned.sync.confidence = 0.7;
  aligned.sync.nonlinear = true;
  aligned.sync.anchors_used = 8;
  aligned.sync.anchors_usable = 9;
  aligned.sync.residual_rms_ms = 0.4;
  aligned.sync.matched_span_seconds = 12.5;
  aligned.sync.has_timestamps = true;
  aligned.sync.timestamps.offset_seconds = 0.00278;
  aligned.sync.message = "Offset 78 ms, with spaces and (punctuation).";
  SyncAnchor anchor;
  anchor.ref_seconds = 1.64;
  anchor.lag_seconds = 0.0784;
  anchor.strength = 47.1;
  anchor.window_seconds = 0.4;
  anchor.inlier = true;
  aligned.sync.anchors.push_back(anchor);
  edits.tracks.push_back(aligned);

  std::string err;
  REQUIRE(save_edits(dir.path, edits, &err));
  CHECK_FALSE(fs::exists(dir.path / "edits.txt.tmp"));

  SessionEdits loaded;
  REQUIRE(load_edits(dir.path, &loaded));
  CHECK(loaded.in_set);
  CHECK(loaded.in_seconds == 1.25);
  CHECK_FALSE(loaded.out_set);
  REQUIRE(loaded.tracks.size() == 2);
  CHECK(loaded.tracks[0].name == plain.name);
  CHECK(loaded.tracks[0].gain_db == -3.5);
  CHECK(loaded.tracks[0].mute);
  CHECK_FALSE(loaded.tracks[0].has_sync);

  const TrackEdit& t = loaded.tracks[1];
  CHECK(t.offset_ms == aligned.offset_ms);
  CHECK(t.drift_ppm == aligned.drift_ppm);
  REQUIRE(t.has_sync);
  CHECK(t.sync.map.offset_seconds == aligned.sync.map.offset_seconds);
  CHECK(t.sync.method == SyncMethod::Audio);
  CHECK(t.sync.confidence == 0.7);
  CHECK(t.sync.nonlinear);
  CHECK_FALSE(t.sync.drift_measured);
  CHECK(t.sync.anchors_used == 8);
  CHECK(t.sync.has_timestamps);
  CHECK(t.sync.timestamps.offset_seconds == 0.00278);
  CHECK(t.sync.message == aligned.sync.message);
  REQUIRE(t.sync.anchors.size() == 1);
  CHECK(t.sync.anchors[0].lag_seconds == 0.0784);
  CHECK(t.sync.anchors[0].inlier);
}

TEST_CASE("a session without edits, or with an unknown file, loads nothing") {
  TempDir dir("edits_missing");
  SessionEdits loaded;
  CHECK_FALSE(load_edits(dir.path, &loaded));

  FILE* f = _wfopen((dir.path / "edits.txt").c_str(), L"w");
  REQUIRE(f);
  fputs("something else\ntrack x\n", f);
  fclose(f);
  CHECK_FALSE(load_edits(dir.path, &loaded));
  CHECK(loaded.tracks.empty());
}
