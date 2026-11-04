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
