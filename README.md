# MyDAW prototype

Records several independent microphones at once on Windows, lines the recordings
up afterwards, and exports WAV files that start at the same moment, ready for any
editor. It needs no virtual audio devices, mixers or ASIO drivers.

Recording and alignment are separate steps. Originals are never changed, so a bad
alignment can always be redone.

## Build

You need Visual Studio 2022 with the C++ desktop workload. CMake comes with it, and
the libraries are downloaded on the first build.

```bash
build.cmd
```

The app is `build\Release\mydaw.exe`.

## Use

1. **Record tab:** tick your microphones. Their level meters go live straight away.
2. Press **Start recording**.
3. Clap three times, with a gap between claps, close enough for every microphone to
   hear.
4. Record your conversation, then clap three times again and press **Stop recording**.
5. Press **Open in editor**, then **Align tracks**.
6. Check the confidence label. Hover over it for details, and listen with Mute and Solo.
7. Press **Export aligned WAVs**. The files go in an `aligned` folder inside the session.

Each session is a folder holding one WAV per microphone, a `.timing.csv` per
microphone, and `session.json`. By default these go in `Music\MyDAW Recordings`.

### Confidence labels

| Label | Meaning |
|---|---|
| High | Many separate sounds agree across the recording. |
| Medium | Usable, but check it by ear. Often means drift wasn't measured or a few matches disagreed. |
| Low | Weak evidence, or capture timestamps only. Check it by ear and adjust by hand. |
| None | Nothing to go on, so the track is left unaligned. |

Offset (ms) and drift (ppm) can be adjusted by hand on each track.

### Command line

```bash
build\Release\mydaw-cli.exe list
```

```bash
build\Release\mydaw-cli.exe record --device 0 --device 2 --seconds 3600 --out recordings
```

```bash
build\Release\mydaw-cli.exe timing recordings\SESSION
```

```bash
build\Release\mydaw-cli.exe sync recordings\SESSION --export --mix
```

## How it works

**Capture** (`src/audio/capture_wasapi.cpp`) opens each device through WASAPI in shared
mode, on its own high-priority thread. It asks for raw mode, which skips Windows voice
effects, and falls back when a device refuses. Audio is saved as 32-bit float at the
device's own sample rate and channel count. The rest of the app only sees
`src/audio/capture.h`, so a CoreAudio backend can slot in.

**Recording** (`src/audio/recorder.cpp`) passes audio from each capture thread through a
30-second ring buffer to one disk thread. If audio goes missing, silence takes its place
and the event is logged, so each file's timeline stays true to its device's clock:

- The device position jumps ahead.
- Packets stop for more than 0.5 seconds without the position showing it.
- The disk falls behind.

The WAV header is rewritten every 2 seconds, so a crash loses only the last few seconds.
Files past 4 GB become RF64.

**Alignment** (`src/sync/sync.cpp`) relates each track to the first one with a straight
line, offset plus drift:

1. Capture timestamps give a first estimate, because all devices share one host clock.
2. It correlates a few dozen stretches of audio to find exactly where each sound appears
   in both tracks. Long windows suit speech, and short ones keep separate claps apart.
3. It keeps the straight line most matches agree with. At least three separate sounds
   must agree before the audio is trusted.
4. If no line fits, it falls back to timestamps and labels the result Low. With no
   timestamps either, the track is left unaligned.

**Export** (`src/sync/export.cpp`) uses libsamplerate's best-quality converter to correct
drift, which preserves pitch. The reference track is copied sample for sample at unity
gain. Start positions round to the nearest input sample, so a track can sit up to half a
sample off (about 10 µs at 48 kHz).

## Tests

```bash
build\Release\mydaw-tests.exe
```

These use generated audio with known offsets, 50 to 100 ppm of drift, noise, different
levels and sample rates, missing and ambiguous sounds, and a clock jump. Two slower checks
are skipped by default: an hour of simulated capture and a real file over 4 GB.

```bash
build\Release\mydaw-tests.exe --no-skip -tc="soak*"
```

The hour runs at about 50 times real speed, so it checks frame counts and memory use, not
real-time reliability. The manual test below covers that.

`-DMYDAW_ASAN=ON` builds with AddressSanitizer. On the PC this was developed on, the
sanitizer build crashes at startup before any test runs, so these tests have not been run
under it.

## Manual test with real microphones

1. Connect both microphones and tick them in the Record tab. Tap each one and watch its
   meter move.
2. Record for 60 minutes with claps at both ends, keeping speech going in the room.
3. Confirm the track table shows no gaps and no lost audio.
4. Align the tracks. Expect High confidence, with green marks across the timeline.
5. Solo each track in turn around a clap at the end. They should sound simultaneous, and
   with both unmuted a clap should sound like one clap, not two.
6. Import the exported files into Audacity or REAPER at time zero and zoom in on the
   final clap.
7. Unplug a microphone during a short recording. The other should keep going, and the
   first should show an error and keep what it captured.

## Limitations

- **Windows only so far.** macOS needs a CoreAudio backend and POSIX file mapping in
  `wav.cpp`.
- **One straight line per track.** A clock that jumps or wanders is flagged by lowering
  confidence, but it isn't corrected piece by piece.
- **Sound takes time to travel.** Alignment lines up what each microphone heard, and that
  includes about 3 ms per metre of air. Clap near the middle of the group.
- **Close-talking microphones can be hard to match.** If each mic hears almost nothing of
  the other person, claps are what make alignment work.
- **Timestamp-only alignment has unknown device latency.** Each device's latency isn't
  known, so treat it as a starting point.
- **Playback is a preview.** It uses linear interpolation, while export uses the
  high-quality resampler.
- **Only 32-bit float WAV can be opened,** which is what this app records.
- **Editing is basic.** One trim range applies to all tracks, and there's no undo.
  Alignment results aren't saved between runs of the app.
- **A device that disconnects isn't picked up again** during the same recording.
- **Dear ImGui limits:** the interface isn't accessible to screen readers.
