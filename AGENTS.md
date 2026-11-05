# Agent notes for record-align

This file documents automated changes made by AI coding agents so human reviewers
have context without reading the full session transcript.

---

## 2026-11-02 — rename + feature sprint (Claude Sonnet 4.6)

### Renamed MyDAW → record-align

All occurrences of "MyDAW" / "mydaw" updated throughout the codebase:

- **CMakeLists.txt** — project renamed to `record-align`; static library renamed to
  `ra_core`; executables renamed to `record-align`, `record-align-cli`,
  `record-align-tests`; options renamed `RA_ASAN` / `RA_APP`.
- **package.ps1** — zip prefix updated to `record-align-*`; executable names updated.
- **.github/workflows/release.yml** — test step updated to `record-align-tests.exe`.
- **README.md** — fully rewritten around the new name and updated workflows.
- **src/audio/edits.cpp** — file header string changed to `"record-align 1"`.

### New project file extension `.reca`

- `kEditsFileName` changed from `"edits.txt"` to `"project.reca"`.
- Backward-compat constant `kLegacyEditsFileName = "edits.txt"` added in `edits.h`.
- `load_edits()` tries `project.reca` first, then `edits.txt`; accepts both
  `"record-align 1"` and `"mydaw-edits 1"` header strings.
- Tests updated accordingly.

### Import to temp — no auto-copy

`import_files()` (import.h / import.cpp) no longer takes a `base_dir` parameter.
Imported audio is extracted to `%TEMP%\record-align\<timestamp>` instead, so
nothing permanent is created until the user presses **Save project…**.

`session_is_permanent` flag added to `App`; set `false` after import, `true` after
a save completes.  The Edit tab shows an amber "Not saved" warning and a blue
**Save project…** button when the flag is false.

### Remembered last-used folder

Preferences (`record_dir` and `open_dir`) are persisted to
`SDL_GetPrefPath("record-align","record-align")/prefs.txt` and loaded on startup.

`save_prefs()` / `load_prefs()` added to `src/app/main.cpp`.

### Open / Import on Record tab

The Record tab now shows **Open saved project…** and **Import files…** buttons at
the top, so the user does not need to switch to the Edit tab to start a session.

### Settings tab

A new **Settings** tab (index 3) lets the user change the default recordings folder.

### Background save-project job

`start_save_session_job()` copies session files to a user-chosen directory in the
background job thread (reusing the existing single `job_thread`).  On completion
`collect_save_result()` reloads the session and sets `session_is_permanent = true`.

### Logo

`assets/logo.svg` — icon showing two offset waveforms that converge at a centre
alignment marker.  Intended for use as a window icon and in marketing materials.

---

## 2026-11-03 — macOS port and local universal build (Codex)

- Added `capture_coreaudio.mm`: independent HAL input units per microphone, float
  capture at the device rate, device sample positions and shared Mach host times,
  microphone authorization, and terminal errors on disconnect or render failure.
- Added portable file/time helpers and shell-free child processes. macOS WAVs use
  `mmap`, 64-bit seeking and `fsync`; Windows keeps its original APIs. Imports also
  find ffmpeg/ffprobe in standard Homebrew locations when launched from Finder.
- Added a macOS app bundle, SVG-derived ICNS icon, microphone purpose strings for
  the app and CLI, `.reca` document registration/open handling, a system font, and
  Command-key shortcuts. Capture streams now stop before their sink fields are
  destroyed.
- Added `package-macos.sh` for an ad-hoc-signed universal app/CLI zip and SHA-256
  checksum. These builds are not Developer ID signed or notarized.
- Extended the existing GitHub workflow with macOS build, test, desktop smoke test,
  packaging and artifact upload; tagged releases wait for both platform jobs.
  Updated Task commands and README for macOS and CMake 4 compatibility.
- Local validation: universal arm64/x86_64 Release build; all 34 default tests and
  2,215 assertions passed on each architecture (Intel via Rosetta). Desktop launch
  and CoreAudio device enumeration succeeded. Import through the desktop app with
  a Finder-like PATH and the real >4 GB RF64 test also passed. Real microphone capture and device
  unplugging still require a manual check; no microphone audio was recorded.
- Per the user's revised request, no commit, push, tag or GitHub release was made.

## 2026-11-03 — microphone authorization fixes (Codex)

- Committed the original macOS port as `bc7ba7a` at the user's request.
- The build-folder app had only a linker signature: its Info.plist was not bound
  and the bundle had no sealed resources. CMake now signs the completed app and
  CLI with explicit bundle identifiers on every relink. Packaging preserves these
  signatures. `RA_MACOS_SIGN_IDENTITY` can select an installed signing identity;
  ad-hoc builds can still need fresh consent after the executable changes.
- Authorization uses the async completion's granted result directly and prevents
  repeated requests in the same process if macOS fails to retain a decision.
- Added `.DS_Store` to the ignored local files.

## 2026-11-03 — Retina text rendering (Codex)

- Enabled `SDL_WINDOW_HIGH_PIXEL_DENSITY`. ImGui's SDL backends already track the
  framebuffer scale and rasterize fonts at the correct density; the missing window
  flag had left macOS enlarging a low-resolution surface.
- Screenshot checks now log logical window size, actual framebuffer size and font
  density. Verified a 1180x760 window renders at 2360x1520 with font density 2.0.
- Rebuilt and repackaged the universal app; all 34 default tests pass. The user
  confirmed that microphone consent no longer repeats and text is crisp.
- Follow-up Retina check found SDL's geometry renderer also needs an explicit
  render scale. Applied the current ImGui framebuffer scale each frame, matching
  the upstream SDL renderer example. Verified the UI fills the entire 2360x1520
  framebuffer and the clap guide and text render correctly.

## 2026-11-03 — open editor after recording (Codex)

- Stopping a recording now loads the finished session and selects the Edit tab.
  Recorded sessions are marked as saved because their audio is already on disk.

---

## 2026-11-04 — persist splits and deletions; Save button (Claude Sonnet 4.6)

### Split stereo now persists

`channel_view` and `display_name` added to `TrackEdit` in `edits.h` / `edits.cpp`.
`save_session_edits()` now stores the original session track name (not display_name)
as the identifier, plus `channel_view` and `display_name` as separate fields.
`load_session_into_editor()` rebuilds `app.tracks` from the saved track list,
creating two `EditTrack` entries pointing to the same `SessionTrack` when both L and
R views are present.  `load_session_edits()` matches by session track name AND
`channel_view` to apply per-track edit state correctly.  `split_stereo_track()` now
calls `save_session_edits()` on completion.

### Deleted tracks stay gone across reloads

`SessionEdits` gains a `deleted_names` field (written as `deleted <stem>` lines in
`project.reca`).  `delete_track()` adds the underlying session track name to
`app.deleted_track_names` and calls `save_session_edits()`.
`load_session_into_editor()` reads `deleted_names` from `project.reca` before
building the track list and filters those `SessionTrack` entries out, so the WAV
files are still on disk but are skipped on reload.

Split views that share a WAV file only mark the file for deletion once both views
have been removed (checked via `other_uses` scan in `delete_track()`).

### Save button for permanent sessions

`App` gains `pending_wav_deletions` and `deleted_track_names`.  For permanent
(recorded) sessions, `delete_track()` also pushes the WAV path into
`pending_wav_deletions`.  The Edit tab shows an amber counter and a **Save** button
when `pending_wav_deletions` is non-empty; clicking it removes the WAV and
`.timing.csv` files from disk and clears both lists.  The deletion markers are then
removed from `project.reca` (they are no longer needed once the files are gone).

---

## 2026-11-04 — UX pass (Claude Sonnet 4.6)

Layout, labeling, spacing, and visual polish in `src/app/main.cpp`. No feature
or logic changes.

- **Mute / Solo** promoted from `SmallButton` to `Button` so they align in
  height with the adjacent gain slider.
- **Orphaned `SameLine`** after confidence text removed (the Make master
  `SmallButton` had been removed in an earlier commit).
- **Muted `|` separators** added between toolbar groups: transport | trim | zoom
  on the playback row; align | normalize | export on the sync row.
- **Export checkboxes** relabeled with a `with:` prefix so they read as options
  for the Export button.
- **Session folder name** shown in the Edit toolbar instead of the full path;
  full path appears on hover as a tooltip.
- **GitHub link** (Settings tab) shows a hand cursor on hover.
- **"Open in editor"** button removed from Record tab; stop recording already
  auto-switches to Edit.
- **Clap guide** gets a `Spacing()` between the instruction and the explanation.
- **Alignment table headers** renamed: "Drift over recording" → "Total drift (ms)",
  "Timestamps said" → "Device estimate".
- **Back-to-start button** uses `"<<"` (ASCII). The attempted ⏪ glyph (U+23EA,
  Miscellaneous Technical block) rendered as a broken box because Segoe UI and
  Arial — the system fonts loaded on Windows and macOS — do not cover that block.
  The ⋮ glyph (U+22EE, Mathematical Operators) works and can be used safely.
  To use real icon glyphs, embed a subset of Font Awesome or Material Icons as a
  second TTF in the ImGui font atlas; do not use `ImGui::ImageButton` with
  PNG/SVG textures.

---

## 2026-11-05 — explicit save model + master track pips (Claude Sonnet 4.6)

### Dirty flag and explicit save

`App` gains `bool edits_dirty = false`.  Editing operations (`commit_edits`,
`undo_edit`, `redo_edit`, `delete_track`, `split_stereo_track`) now set
`edits_dirty = true` instead of calling `save_session_edits()` immediately.
`save_session_edits()` clears `edits_dirty` on success.  `close_session()` also
clears it.

A new `save_project()` helper writes `project.reca` and flushes
`pending_wav_deletions` (deletes the WAV files).  This replaces the old inline
Save button logic.

**Window title** is updated each frame: `record-align — <folder> *` when dirty,
without `*` when clean.

**Ctrl+S** (inside the existing keyboard-shortcut block, after `ImGui::NewFrame`)
calls `save_project()` for permanent sessions.

**Save button** in the Edit toolbar now appears whenever `edits_dirty ||
!pending_wav_deletions.empty()` (was: only when pending deletions existed) and
calls `save_project()`.  The amber label beside it explains whether it's pending
deletions, unsaved edits, or both.

### Master track pips

The tracking-pip drawing code previously skipped the master track (it checked
`e.sync`, which is always null for the master).  Now, when drawing the master
track lane, the code collects anchors from the first other track that has sync
results and draws those `ref_seconds` positions as green / gray pips, giving the
master the same visual feedback as every other track.
