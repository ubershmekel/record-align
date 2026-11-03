# MyDAW

**Line up audio from different devices, using a few claps.**

Record a conversation on two microphones, or film on a phone while a separate mic
records, and the tracks won't match. They start at different moments, and each device's
clock runs at a slightly different speed. This app measures the difference and exports
WAV files that line up from start to finish, ready to drop into any video or audio
editor.

It works two ways:

- **Record:** capture several microphones at once on one Windows PC, each to its own
  file. It needs no virtual audio devices, mixers or ASIO drivers.
- **Import:** bring in video and audio files you already have, and line them up.

Your original files are never changed. Aligning only saves timing numbers, and new files
are written only when you export.

## Clap, then align

Clap three times where every microphone can hear it, at the start and again at the end.

- The first claps show how far apart the tracks started.
- The last claps show how much they slid apart along the way.

Any sharp sound works, and shared speech often does too, but claps are the most reliable.

## Install

**Windows:** download the zip from the
[Releases](../../releases) page, unzip it and run `mydaw.exe`. Importing files also needs
[ffmpeg](https://ffmpeg.org/download.html) on your PATH. Recording doesn't.

macOS isn't supported yet.

## Record microphones

1. On the **Record** tab, tick your microphones. Their level meters go live straight away.
2. Press **Start recording**, then clap three times.
3. Record, clap three more times, then press **Stop recording**.
4. Press **Open in editor**, then **Align tracks**.
5. Check the result, then press **Export aligned WAVs**.

Recordings go in `Music\MyDAW Recordings` unless you choose another folder.

## Line up existing files

1. On the **Edit** tab, press **Import files...**, or drag files onto the window. Video and
   audio files both work, such as a phone video and a recorder's WAV.
2. The app copies out the audio and aligns it straight away. The first video becomes the
   **master track**, the one everything else lines up with. Press **Make master** on another
   track to change that.
3. Press **Export aligned WAVs**. With **Match master's length** on, which is the default
   for imports, each file starts and ends exactly with the master.
4. In your editor, put the video and the exported files at time zero.

If you have several videos, each one's audio can be aligned, but the videos themselves
aren't moved or re-exported. The simple case works best: one video as master, with any
number of audio files under it.

## The Edit tab

| Control | What it does |
|---|---|
| **Play**, or Space | Plays all tracks together. Click a waveform to move the playhead, and scroll to zoom. |
| **Mute** | Silences a track. |
| **Solo** | Hears only this track. Click again to hear everything. Shift-click or Ctrl-click adds a track. |
| **dB** | Sets a track's volume. |
| **ms** | Offset: how far into this track the master's first moment falls. |
| **ppm** | Drift: how much faster this track's clock runs than the master's. 1 ppm is 3.6 ms per hour. |
| **Set in**, **Set out** | Trims the export. |
| **Normalize all** | Sets each track's gain so its loudest moment reaches -1 dB. Claps count, so set in and out points past them to bring up speech. |
| **Undo**, **Redo** | Ctrl+Z and Ctrl+Y or Ctrl+Shift+Z. Covers gain, mute, timing, alignment and trim, but not solo. |

Hover over anything for an explanation. Right-click gain, ms, ppm, Set in, Set out or a
track's name to reset it. Double-click a number to type a value.

### Reading the alignment results

Green marks on the timeline show the sounds that were used. The **Alignment** section
under the timeline shows the numbers, and each track gets a confidence label:

| Label | Meaning |
|---|---|
| High | Several separate sounds agree. |
| Medium | Usable, but check it by ear. A few matches may disagree. |
| Low | Weak evidence, or capture timestamps only. Check it by ear and adjust by hand. |
| None | Nothing to go on, so the track is left unaligned. |

**Spread** shows how far the strongest matches landed from each other. Up to about 1 ms is
tight. Several ms suggests echoes, effects on the microphone, or sounds made at different
distances.

## What's in a session folder

| File | What it is |
|---|---|
| `01_name.wav`, `02_name.wav`... | One track per microphone or imported file, as 32-bit float. |
| `01_name.timing.csv`... | Capture timestamps and any gaps. Recordings only. |
| `session.json` | Recording details. Recordings only. |
| `sources.txt` | Which original file each track came from. Imports only. |
| `edits.txt` | Your alignment, gain, mute and trim. Saved automatically. |
| `aligned/` | What you exported. |

## Limitations

- **Windows only for now.** macOS needs its own capture code, and the release workflow
  would then need a Mac job.
- **Sound takes time to travel,** about 3 ms per metre. Alignment lines up what each mic
  heard, so clap near the middle of the group.
- **Close-talking mics can be hard to match** without claps, if each mic hears almost
  nothing of the other person.
- **Timestamps alone aren't enough.** They don't include each device's own latency. On one
  real test they were about 75 ms off, so treat a timestamps-only result as a starting point.
- **Each track gets one straight-line correction.** A clock that jumps partway through is
  flagged with lower confidence, but it isn't corrected piece by piece.
- **Playback is a preview** with simple interpolation. Export uses a high-quality resampler.
- **A microphone that disconnects isn't picked up again** during that recording. It keeps
  what it captured, and the others carry on.
- **The interface isn't accessible to screen readers.** That's a limit of Dear ImGui.

## For developers

You need Visual Studio 2022 with the C++ desktop workload. CMake comes with it, and the
libraries download on the first build.

```bash
build.cmd
```

The app is at `build\Release\mydaw.exe`, with a command-line tool beside it:

```bash
build\Release\mydaw-cli.exe list
```

```bash
build\Release\mydaw-cli.exe record --device 0 --device 2 --seconds 3600 --out recordings
```

```bash
build\Release\mydaw-cli.exe sync recordings\SESSION --export --mix
```

### Code map

| Path | Job |
|---|---|
| `src/audio/capture_wasapi.cpp` | Windows capture. The rest of the code only sees `capture.h`. |
| `src/audio/recorder.cpp` | Ring buffers, disk thread, and silence in place of anything missing. |
| `src/audio/wav.cpp` | WAV writing that survives crashes, RF64 past 4 GB, and memory-mapped reading. |
| `src/audio/import.cpp` | Pulls audio out of existing files with ffmpeg. |
| `src/audio/edits.cpp` | Reads and writes `edits.txt`. |
| `src/sync/sync.cpp` | Works out offset and drift. |
| `src/sync/export.cpp` | Resampling and export, using libsamplerate. |
| `src/app/` | The Dear ImGui and SDL3 interface. |

### How alignment works

1. Capture timestamps give a first estimate, when a track has them.
2. A coarse search over each track's sudden sounds finds a starting offset if needed.
3. Correlating a few dozen stretches of audio finds where each sound falls in both tracks.
4. The straight line most matches agree with becomes the result. At least three separate
   sounds must agree before the audio is trusted.

### Tests

```bash
build\Release\mydaw-tests.exe
```

These use generated audio with known offsets, drift, noise, levels and sample rates, plus
missing and ambiguous sounds and a clock jump. Import tests skip themselves without ffmpeg.
Two slower checks are off by default: an hour of simulated capture at about 50× speed, and a
real file over 4 GB.

```bash
build\Release\mydaw-tests.exe --no-skip -tc="soak*"
```

The app also takes test flags, listed above `main` in `src/app/main.cpp`. `--script` plays
clicks and keys into the interface and logs the resulting state.

### Releases

Pushing a tag like `v0.1.0` makes GitHub Actions build, test and publish a Windows zip. See
`.github/workflows/release.yml`.

```bash
git tag v0.1.0
```

```bash
git push origin v0.1.0
```

### Manual test with real microphones

1. Connect two microphones and tick them. Tap each one and watch its meter move.
2. Record for 60 minutes with claps at both ends and speech in the room.
3. Confirm the track table shows no gaps and no lost audio.
4. Align. Expect High confidence, with green marks across the timeline.
5. With both tracks playing, a final clap should sound like one clap, not two.
6. Import the exported files into Audacity or REAPER at time zero and zoom in on it.
7. Unplug a microphone during a short recording. The other should keep going, and the first
   should show an error and keep what it captured.
