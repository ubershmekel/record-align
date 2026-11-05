# record-align — Help

record-align records multiple microphones simultaneously, aligns their timing, and exports synchronized WAV files ready to drop into any editor.

---

## Record tab

### Clap guide

Before and after recording, clap three times sharply. Every microphone hears the same claps, which give the alignment engine fixed reference points to match the tracks. The first three claps lock the start; the last three measure any clock drift over the recording.

### Microphones list

Each detected input device appears here with its sample rate and channel format.

- **Checkbox** — arm this device for recording. Its level meter goes live immediately.
- **Level meter** — green is normal, amber is loud, red means the signal is clipping. The white tick holds the recent peak.
- **Refresh list** — re-scan for devices (use this after plugging in a new microphone).

### Save to

Where recordings are stored. Each recording gets its own timestamped subfolder. The disk-free estimate updates every five seconds based on how many microphones are armed.

- **Choose folder…** — open a folder picker to change the recordings directory. This is also available in the **Settings** tab.

### Start / Stop recording

**Start recording** (red button) — begins recording from every armed microphone at once. Disabled until at least one microphone is armed.

**Stop recording** — ends the recording, saves all WAV files to disk, and immediately loads the session into the **Edit** tab so you can review and align it.

**RECORDING / elapsed time** — shown while recording is active.

### Track status table

Visible during and after a recording. Shows each track's saved length, any clock gaps (silence inserted to maintain correct timing), and lost audio (dropped by the disk when it couldn't keep up).

**Open in editor** — loads the last recording into the Edit tab.

---

## Edit tab

### Top toolbar

**Save project…** (blue, shown when unsaved) — copies the session's audio and edit data to a folder you choose. Appears when a session was imported or recorded but not yet saved permanently. Until saved, changes may be lost if the app closes.

**Not saved** (amber warning) — reminder that the current session lives in a temporary folder.

**Open session folder…** — open a previously saved session. Any edits you made to it (alignment, gain, trim) are restored from the `project.reca` file inside it.

**Import files…** — extract audio from video or audio files you already have (MP4, MOV, WAV, MP3, etc.) and load them as a session. Requires [ffmpeg](https://ffmpeg.org) to be installed. You can also **drag files** onto the window.

**Undo / Redo** — step through the edit history. The tooltip names the change. Keyboard: **Ctrl+Z** / **Ctrl+Y** (or Ctrl+Shift+Z).

---

### Transport

**Play / Pause** — start or pause all tracks together. Keyboard: **Space** (Edit tab only).

**Back to start** — move the playhead to the beginning, or to the in point if one is set.

**Playhead time** — current position in `minutes:seconds.ms` format. Slightly negative when a track started before the master.

---

### Trim (in / out points)

**Set in** — marks the current playhead position as the start of the export. Everything before it is excluded. Right-click to clear.

**Set out** — marks the current playhead position as the end of the export. Everything after it is excluded. Right-click to clear.

**Clear trim** — removes both the in and out points.

Trim points affect export and normalize but do not change your original WAV files.

---

### Zoom

**Zoom in / Zoom out** — halve or double the visible time range. You can also **scroll** over any waveform to zoom toward the mouse.

**Fit** — show the full recording.

---

### Alignment and export

**Align tracks** — analyzes the audio of every track, finds matching sharp sounds (your claps), and computes how each track lines up with the master. Results are saved to `project.reca`; your original WAV files are never changed. Requires at least two tracks.

**Hear aligned** — toggle between the aligned view (using the computed offsets and drift) and the raw recorded positions, for comparison.

**Normalize all** — sets each track's gain so its loudest moment inside the trim range hits −1 dB. Because claps are often the loudest sounds, set your in/out points past the claps first so the speech is normalized instead. Your WAV files are not changed.

**Export aligned WAVs** — writes one new WAV per track into an `aligned` subfolder, all the same length and starting at the same moment. Drop them into any editor at time zero and they line up. Includes your gain and trim.

**Stereo mix** — when exporting, also writes `mix.wav`: all unmuted tracks combined into a stereo file. The mix is scaled down automatically if any peak would clip.

**Match master's length** — trims every exported file to start and end exactly with the master track. Use this to lay audio under a video: put the video and the exported files at time zero. In/out points take priority over this setting.

---

### Timeline

Each track shows its waveform in a horizontal lane. The white vertical line is the playhead.

- **Click a waveform** — seek the playhead to that position.
- **Scroll over a waveform** — zoom in or out, centered on the mouse.
- **Green triangles (pips)** — sounds used for alignment. Hover to see the measured offset and strength.
- **Gray triangles** — sounds considered but not used (too weak or inconsistent).
- **Shaded edges** — the region outside the in/out trim points.

Stereo tracks show L on the top half and R on the bottom half, with a divider line.

---

### Track controls

Each track has a control panel on the left of the timeline. All changes are undoable.

**Track name** — the WAV filename. Hover for the full path.

**⋮ button** (or right-click the track name) — opens the track options menu:
- **Reset this track** — restores the track's gain, mute, offset, and drift to their last aligned values (or zero if not yet aligned).
- **Make master** — promotes this track to master. Every other track aligns to it. Triggers a re-align automatically.
- **Split to L / R** — decomposes a stereo WAV into two separate tracks, one for the left channel and one for the right. Each can then be controlled independently (gain, mute, solo). Exporting split tracks produces separate mono WAV files. Available only for stereo files whose waveform has finished loading. Clears the undo history.

**Mute** — silence this track in playback and in the stereo mix. It still gets its own exported file.

**Solo** — hear only this track. Click again to hear everything. Hold **Shift** or **Ctrl** to add or remove additional tracks from the solo set.

**Gain slider** — volume in decibels (−40 to +40 dB). 0 dB leaves the signal unchanged; +6 dB is roughly twice the level. Applied to both playback and export. Double-click to type a value. Right-click → **Reset gain to 0 dB**.

**Master track** — the track shown in green. Its timing is never changed; all other tracks align to it. To change it, use **Make master** in the track menu.

**ms (offset)** — time offset in milliseconds: how far into this track the master track's first moment falls. Positive means this microphone started recording first. Drag to nudge, double-click to type. Right-click to reset to the alignment result or to 0.

**ppm (drift)** — clock drift in parts per million: how much faster this microphone's clock runs than the master. 100 ppm adds up to 360 ms of drift per hour. Drag to nudge, double-click to type. Right-click to reset.

**Confidence indicator** — green/amber/red shows how reliable the alignment result is. Hover for a full explanation. Shown after Align tracks has run.

**Make master** (inline button) — same as the menu item, for quick access.

---

### Alignment report

Below the timeline, after **Align tracks** has run, a table shows each track's confidence, computed offset, drift, and how well the matched sounds fit the alignment. Expand **Every sound that was compared** for per-match detail.

---

## Settings tab

**Choose folder…** — set the default recordings directory. Preferences are saved automatically.

**Version / built** — the git hash and build date of this binary.

**GitHub link** — click to open the project page in your browser.

---

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| Space | Play / Pause (Edit tab only, while no text field is focused) |
| Ctrl+Z | Undo |
| Ctrl+Y or Ctrl+Shift+Z | Redo |

---

## Workflow overview

1. **Record** — arm microphones, clap three times, record, clap three times, stop.
2. **Align** — press **Align tracks** in the Edit tab.
3. **Review** — listen with **Hear aligned** on; adjust gain, mute, or solo as needed.
4. **Trim** (optional) — set in/out points to cut claps and silence from the export.
5. **Normalize** (optional) — press **Normalize all** after setting trim points past the claps.
6. **Export** — press **Export aligned WAVs**. Find the results in the `aligned` subfolder.

For video work: enable **Match master's length**, set the video track as master, and drop the exported WAVs onto the video timeline at time zero.
