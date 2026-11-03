#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "sync/sync.h"

// Everything the user has decided about a session, stored as text next to the
// recordings. The WAV files themselves are never changed.

struct TrackEdit {
  std::string name;  // WAV file name without extension.
  double offset_ms = 0.0;
  double drift_ppm = 0.0;
  double gain_db = 0.0;
  bool mute = false;
  bool has_sync = false;
  SyncResult sync;
};

struct SessionEdits {
  std::vector<TrackEdit> tracks;
  bool in_set = false;
  bool out_set = false;
  double in_seconds = 0.0;
  double out_seconds = 0.0;
};

constexpr const char* kEditsFileName = "edits.txt";

// Replaces the file in one step, so a crash can't leave half of it behind.
bool save_edits(const std::filesystem::path& session_dir, const SessionEdits& edits, std::string* err);

// Returns false if the session has no saved edits or the file isn't readable.
bool load_edits(const std::filesystem::path& session_dir, SessionEdits* out);
