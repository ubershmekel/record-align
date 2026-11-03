#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

#include "audio/recorder.h"
#include "audio/session.h"
#include "audio/wav.h"
#include "sync/export.h"
#include "sync/sync.h"

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979;

// A short sound defined by a formula, so it can be sampled exactly at any rate or clock.
struct Burst {
  double time = 0.0;
  double decay = 0.03;
  double amplitude = 1.0;
  std::vector<double> frequencies;
  std::vector<double> phases;
};

struct Scene {
  std::vector<Burst> bursts;
};

Burst make_burst(std::mt19937& rng, double time, double amplitude, double decay) {
  std::uniform_real_distribution<double> freq(300.0, 6000.0);
  std::uniform_real_distribution<double> phase(0.0, 2.0 * kPi);
  Burst b;
  b.time = time;
  b.amplitude = amplitude / 5.0;
  b.decay = decay;
  for (int i = 0; i < 25; i++) {
    b.frequencies.push_back(freq(rng));
    b.phases.push_back(phase(rng));
  }
  return b;
}

// Adds three claps at each time given. Each clap sounds different, as real ones do.
void add_claps(Scene* s, std::mt19937& rng, double at) {
  for (int i = 0; i < 3; i++) s->bursts.push_back(make_burst(rng, at + i * 0.6, 1.0, 0.02));
}

void add_speech_like(Scene* s, std::mt19937& rng, double start, double end, double per_second) {
  std::uniform_real_distribution<double> amplitude(0.05, 0.3);
  std::uniform_real_distribution<double> decay(0.03, 0.12);
  std::uniform_real_distribution<double> gap(0.0, 2.0 / per_second);
  for (double t = start; t < end; t += gap(rng)) {
    s->bursts.push_back(make_burst(rng, t, amplitude(rng), decay(rng)));
  }
}

struct TrackParams {
  int rate = 48000;
  int channels = 1;
  double seconds = 120.0;
  double offset = 0.0;     // other_seconds = offset + (1 + drift) * scene_seconds
  double drift = 0.0;
  double level = 1.0;
  double noise = 0.0005;
  unsigned seed = 1;
};

std::vector<float> render(const Scene& scene, const TrackParams& p) {
  const uint64_t frames = uint64_t(p.seconds * p.rate);
  std::vector<float> out(size_t(frames) * p.channels, 0.0f);
  for (const Burst& b : scene.bursts) {
    const double length = b.decay * 8.0;
    const double start_time = p.offset + (1.0 + p.drift) * b.time;
    int64_t first = std::max<int64_t>(0, int64_t(std::ceil(start_time * p.rate)));
    int64_t last = std::min<int64_t>(int64_t(frames), int64_t((start_time + length) * p.rate));
    for (int64_t f = first; f < last; f++) {
      const double t = ((double(f) / p.rate) - p.offset) / (1.0 + p.drift) - b.time;
      if (t < 0.0) continue;
      double sum = 0.0;
      for (size_t k = 0; k < b.frequencies.size(); k++) {
        sum += std::sin(2.0 * kPi * b.frequencies[k] * t + b.phases[k]);
      }
      const float v = float(p.level * b.amplitude * std::exp(-t / b.decay) * sum);
      for (int c = 0; c < p.channels; c++) out[size_t(f) * p.channels + c] += v;
    }
  }
  std::mt19937 rng(p.seed);
  std::normal_distribution<float> gauss(0.0f, float(p.noise));
  for (auto& s : out) s += gauss(rng);
  return out;
}

AudioView view(const std::vector<float>& samples, const TrackParams& p) {
  AudioView v;
  v.samples = samples.data();
  v.channels = p.channels;
  v.sample_rate = p.rate;
  v.frames = samples.size() / p.channels;
  return v;
}

Scene clap_scene(double seconds, unsigned seed = 7) {
  std::mt19937 rng(seed);
  Scene s;
  add_claps(&s, rng, 3.0);
  add_claps(&s, rng, seconds - 6.0);
  return s;
}

}  // namespace

TEST_CASE("claps at both ends recover offset and drift") {
  const double seconds = 120.0;
  Scene scene = clap_scene(seconds);
  TrackParams a;
  a.seconds = seconds;
  TrackParams b = a;
  b.offset = 0.137;
  b.drift = 100e-6;
  b.seed = 2;
  auto ta = render(scene, a);
  auto tb = render(scene, b);

  SyncResult r = align_tracks(view(ta, a), view(tb, b), nullptr);
  CHECK(r.method == SyncMethod::Audio);
  CHECK(r.drift_measured);
  CHECK(r.map.offset_seconds == doctest::Approx(0.137).epsilon(0.0002 / 0.137));
  CHECK(std::fabs(r.map.drift - 100e-6) < 5e-6);
  CHECK(r.confidence >= 0.4);
  CHECK_FALSE(r.nonlinear);
}

TEST_CASE("negative offsets and negative drift work") {
  const double seconds = 120.0;
  Scene scene = clap_scene(seconds);
  TrackParams a;
  a.seconds = seconds;
  a.offset = 0.4;  // Reference started first, so the other track begins later in the scene.
  TrackParams b = a;
  b.offset = 0.15;
  b.drift = -50e-6;
  b.seed = 3;
  auto ta = render(scene, a);
  auto tb = render(scene, b);

  SyncResult r = align_tracks(view(ta, a), view(tb, b), nullptr);
  CHECK(r.method == SyncMethod::Audio);
  CHECK(std::fabs(r.map.drift + 50e-6) < 5e-6);
  // Check where a sound actually lands instead of the raw offset value.
  const double ref_time = 0.4 + 60.0;
  const double other_time = 0.15 + (1.0 - 50e-6) * 60.0;
  const double predicted = r.map.offset_seconds + (1.0 + r.map.drift) * ref_time;
  CHECK(std::fabs(predicted - other_time) < 0.0005);
}

TEST_CASE("very different microphone levels don't matter") {
  const double seconds = 120.0;
  Scene scene = clap_scene(seconds);
  TrackParams a;
  a.seconds = seconds;
  a.level = 1.0;
  TrackParams b = a;
  b.level = 0.02;
  b.offset = 0.25;
  b.drift = 50e-6;
  b.noise = 0.0002;
  b.seed = 4;
  auto ta = render(scene, a);
  auto tb = render(scene, b);

  SyncResult r = align_tracks(view(ta, a), view(tb, b), nullptr);
  CHECK(r.method == SyncMethod::Audio);
  CHECK(std::fabs(r.map.offset_seconds - 0.25) < 0.0005);
  CHECK(std::fabs(r.map.drift - 50e-6) < 5e-6);
}

TEST_CASE("different sample rates align") {
  const double seconds = 120.0;
  Scene scene = clap_scene(seconds);
  TrackParams a;
  a.seconds = seconds;
  TrackParams b = a;
  b.rate = 44100;
  b.offset = 0.09;
  b.drift = 80e-6;
  b.seed = 5;
  auto ta = render(scene, a);
  auto tb = render(scene, b);

  SyncResult r = align_tracks(view(ta, a), view(tb, b), nullptr);
  CHECK(r.method == SyncMethod::Audio);
  CHECK(std::fabs(r.map.offset_seconds - 0.09) < 0.0005);
  CHECK(std::fabs(r.map.drift - 80e-6) < 5e-6);
}

TEST_CASE("stereo tracks align") {
  const double seconds = 60.0;
  Scene scene = clap_scene(seconds);
  TrackParams a;
  a.seconds = seconds;
  a.channels = 2;
  TrackParams b = a;
  b.channels = 1;
  b.offset = 0.03;
  b.seed = 6;
  auto ta = render(scene, a);
  auto tb = render(scene, b);

  SyncResult r = align_tracks(view(ta, a), view(tb, b), nullptr);
  CHECK(r.method == SyncMethod::Audio);
  CHECK(std::fabs(r.map.offset_seconds - 0.03) < 0.0005);
}

TEST_CASE("speech-like shared sound aligns without any claps") {
  const double seconds = 180.0;
  std::mt19937 rng(11);
  Scene scene;
  add_speech_like(&scene, rng, 1.0, seconds - 1.0, 4.0);
  TrackParams a;
  a.seconds = seconds;
  a.noise = 0.003;
  TrackParams b = a;
  b.offset = 0.21;
  b.drift = 60e-6;
  b.level = 0.5;
  b.seed = 12;
  auto ta = render(scene, a);
  auto tb = render(scene, b);

  SyncResult r = align_tracks(view(ta, a), view(tb, b), nullptr);
  CHECK(r.method == SyncMethod::Audio);
  CHECK(r.drift_measured);
  CHECK(std::fabs(r.map.offset_seconds - 0.21) < 0.0005);
  CHECK(std::fabs(r.map.drift - 60e-6) < 5e-6);
  CHECK(r.confidence >= 0.7);
}

TEST_CASE("recordings with nothing in common report no alignment") {
  const double seconds = 90.0;
  std::mt19937 rng_a(21), rng_b(22);
  Scene scene_a, scene_b;
  add_speech_like(&scene_a, rng_a, 1.0, seconds - 1.0, 3.0);
  add_speech_like(&scene_b, rng_b, 1.0, seconds - 1.0, 3.0);
  TrackParams a;
  a.seconds = seconds;
  a.noise = 0.003;
  TrackParams b = a;
  b.seed = 23;
  auto ta = render(scene_a, a);
  auto tb = render(scene_b, b);

  SyncResult r = align_tracks(view(ta, a), view(tb, b), nullptr);
  CHECK(r.method == SyncMethod::None);
  CHECK(r.confidence < 0.2);
  CHECK(r.map.offset_seconds == 0.0);
  CHECK_FALSE(r.message.empty());
}

TEST_CASE("timestamps are the fallback when there is nothing in common") {
  const double seconds = 60.0;
  std::mt19937 rng_a(31), rng_b(32);
  Scene scene_a, scene_b;
  add_speech_like(&scene_a, rng_a, 1.0, seconds - 1.0, 3.0);
  add_speech_like(&scene_b, rng_b, 1.0, seconds - 1.0, 3.0);
  TrackParams a;
  a.seconds = seconds;
  TrackParams b = a;
  b.seed = 33;
  auto ta = render(scene_a, a);
  auto tb = render(scene_b, b);

  TimeMap prior;
  prior.offset_seconds = 0.012;
  prior.drift = 20e-6;
  SyncResult r = align_tracks(view(ta, a), view(tb, b), &prior);
  CHECK(r.method == SyncMethod::Timestamps);
  CHECK(r.map.offset_seconds == prior.offset_seconds);
  CHECK(r.map.drift == prior.drift);
  CHECK(r.confidence < 0.4);
}

TEST_CASE("a silent track fails cleanly") {
  TrackParams a;
  a.seconds = 30.0;
  a.noise = 0.0;
  std::vector<float> silent(size_t(a.seconds * a.rate), 0.0f);
  Scene scene = clap_scene(30.0);
  TrackParams b = a;
  b.noise = 0.001;
  auto tb = render(scene, b);
  SyncResult r = align_tracks(view(silent, a), view(tb, b), nullptr);
  CHECK(r.method == SyncMethod::None);
}

TEST_CASE("only one clap sequence gives the offset but says drift wasn't measured") {
  const double seconds = 120.0;
  std::mt19937 rng(41);
  Scene scene;
  add_claps(&scene, rng, 3.0);
  TrackParams a;
  a.seconds = seconds;
  TrackParams b = a;
  b.offset = 0.05;
  b.seed = 42;
  auto ta = render(scene, a);
  auto tb = render(scene, b);

  SyncResult r = align_tracks(view(ta, a), view(tb, b), nullptr);
  CHECK(r.method == SyncMethod::Audio);
  CHECK_FALSE(r.drift_measured);
  CHECK(std::fabs(r.map.offset_seconds - 0.05) < 0.001);
  CHECK(r.confidence < 0.7);
  CHECK(r.message.find("drift") != std::string::npos);
}

TEST_CASE("a clock jump partway through is flagged instead of silently averaged") {
  const double seconds = 180.0;
  std::mt19937 rng(51);
  Scene early, late;
  add_speech_like(&early, rng, 1.0, seconds * 0.5, 4.0);
  add_speech_like(&late, rng, seconds * 0.5, seconds - 1.0, 4.0);
  TrackParams a;
  a.seconds = seconds;
  a.noise = 0.003;
  Scene both = early;
  both.bursts.insert(both.bursts.end(), late.bursts.begin(), late.bursts.end());
  auto ta = render(both, a);

  TrackParams b1 = a;
  b1.offset = 0.1;
  b1.seed = 52;
  TrackParams b2 = b1;
  b2.offset = 0.15;  // 50 ms jump halfway.
  b2.seed = 53;
  auto part1 = render(early, b1);
  auto part2 = render(late, b2);
  for (size_t i = 0; i < part1.size(); i++) part1[i] += part2[i];

  SyncResult r = align_tracks(view(ta, a), view(part1, b1), nullptr);
  CHECK(r.nonlinear);
  CHECK(r.confidence < 0.5);
}

TEST_CASE("capture timestamps yield clock rate and start time") {
  std::vector<TimingSample> a, b;
  std::mt19937 rng(61);
  std::normal_distribution<double> jitter(0.0, 0.001);
  const int rate = 48000;
  for (int i = 0; i < 400; i++) {
    uint64_t frame = uint64_t(i) * 12000;
    double t = double(frame) / rate;
    a.push_back({frame, 5000.0 + t * (1.0 + 10e-6) + jitter(rng)});
    b.push_back({frame, 5000.25 + t * (1.0 - 40e-6) + jitter(rng)});
  }
  // A spike, like a delayed packet.
  a[100].host_seconds += 0.2;

  ClockFit fa = fit_clock(a, rate);
  ClockFit fb = fit_clock(b, rate);
  REQUIRE(fa.valid);
  REQUIRE(fb.valid);
  CHECK(fa.samples_used < 400);

  TimeMap map;
  REQUIRE(time_map_from_clocks(fa, fb, &map));
  CHECK(std::fabs(map.offset_seconds + 0.25) < 0.001);
  CHECK(std::fabs(map.drift - 50e-6) < 5e-6);
}

TEST_CASE("too few timestamps give no clock estimate") {
  std::vector<TimingSample> few = {{0, 1.0}, {100, 1.1}};
  CHECK_FALSE(fit_clock(few, 1000).valid);
}

TEST_CASE("export produces same-length tracks that really line up") {
  const double seconds = 60.0;
  Scene scene = clap_scene(seconds);
  TrackParams a;
  a.seconds = seconds;
  a.noise = 0.0;
  TrackParams b = a;
  b.offset = 0.137;
  b.drift = 100e-6;
  auto ta = render(scene, a);
  auto tb = render(scene, b);

  ExportTrack ea;
  ea.audio = view(ta, a);
  ea.name = "a";
  ExportTrack eb;
  eb.audio = view(tb, b);
  eb.map.offset_seconds = 0.137;
  eb.map.drift = 100e-6;
  eb.name = "b";

  fs::path dir = fs::temp_directory_path() / "mydaw_test_export";
  fs::remove_all(dir);
  ExportOptions options;
  options.out_dir = dir;
  options.write_mix = true;
  std::string err;
  std::atomic<float> progress{0.0f};
  REQUIRE(export_tracks({ea, eb}, options, &progress, &err));
  CHECK(progress.load() == 1.0f);

  WavFile fa, fb, mix;
  REQUIRE(fa.open(dir / "a.wav", &err));
  REQUIRE(fb.open(dir / "b.wav", &err));
  REQUIRE(mix.open(dir / "mix.wav", &err));
  CHECK(fa.frames() == fb.frames());
  CHECK(mix.frames() == fa.frames());
  CHECK(mix.channels() == 2);

  // The other track started earlier, so it sets the origin and the reference
  // track gets this much silence at the front.
  const uint64_t lead = uint64_t(std::ceil(0.137 / (1.0 + 100e-6) * 48000));

  // Both the first and last claps must land within a sample of each other. Start
  // position is rounded to a whole input sample, so up to half a sample of error
  // is expected, which costs a little correlation at high frequencies.
  for (double clap_time : {3.0, seconds - 6.0 + 1.2}) {
    const int64_t center = int64_t(clap_time * 48000) + int64_t(lead);
    double energy_a = 0.0, energy_b = 0.0;
    for (int64_t i = center; i < center + 4800; i++) {
      energy_a += fa.samples()[i] * fa.samples()[i];
      energy_b += fb.samples()[i] * fb.samples()[i];
    }
    REQUIRE(energy_a > 0.0);
    int best_lag = 0;
    double best = -2.0;
    for (int lag = -48; lag <= 48; lag++) {
      double sum = 0.0;
      for (int64_t i = center; i < center + 4800; i++) sum += double(fa.samples()[i]) * fb.samples()[i + lag];
      const double correlation = sum / std::sqrt(energy_a * energy_b);
      if (correlation > best) {
        best = correlation;
        best_lag = lag;
      }
    }
    CHECK(std::abs(best_lag) <= 1);
    CHECK(best > 0.97);
  }

  fa.close();
  fb.close();
  mix.close();
  fs::remove_all(dir);
}

TEST_CASE("full path: recorder files and timestamps through session loading to sync") {
  const double seconds = 60.0;
  Scene scene = clap_scene(seconds);
  TrackParams pa;
  pa.seconds = seconds;
  TrackParams pb = pa;
  pb.offset = 0.31;
  pb.drift = 70e-6;
  pb.seed = 9;
  auto ta = render(scene, pa);
  auto tb = render(scene, pb);

  fs::path dir = fs::temp_directory_path() / "mydaw_test_pipeline";
  fs::remove_all(dir);
  std::string err;
  {
    Recorder recorder;
    InputDevice a, b;
    a.id = "a";
    a.name = "A";
    b.id = "b";
    b.name = "B";
    CaptureSink* sinks[2] = {recorder.arm_sink_for_test(a), recorder.arm_sink_for_test(b)};
    sinks[0]->on_format(48000, 1);
    sinks[1]->on_format(48000, 1);
    REQUIRE(recorder.start_in(dir, &err));

    // Timestamps on track B carry 40 ms of unknown device latency, so they are
    // close to the truth but not right. The audio has to correct them.
    const double latency = 0.040;
    const uint32_t packet = 480;
    for (uint64_t f = 0; f + packet <= ta.size(); f += packet) {
      CapturePacket p;
      p.frames = packet;
      p.device_position = f;
      p.samples = ta.data() + f;
      p.host_seconds = 500.0 + double(f) / 48000;
      sinks[0]->on_packet(p);
      p.samples = tb.data() + f;
      p.host_seconds = 500.0 + (double(f) / 48000 - pb.offset) / (1.0 + pb.drift) + latency;
      sinks[1]->on_packet(p);
      // Far faster than real time, but slow enough not to overflow the ring buffers.
      if (f % (packet * 100) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    recorder.stop();
  }

  Session session;
  REQUIRE(load_session(dir, &session, &err));
  REQUIRE(session.tracks.size() == 2);
  const ClockFit clock_a = fit_clock(session.tracks[0].timing, 48000);
  const ClockFit clock_b = fit_clock(session.tracks[1].timing, 48000);
  TimeMap prior;
  REQUIRE(time_map_from_clocks(clock_a, clock_b, &prior));
  CHECK(std::fabs(prior.drift - pb.drift) < 2e-6);
  CHECK(std::fabs(prior.offset_seconds - (pb.offset - 0.040)) < 0.001);

  AudioView va, vb;
  va.samples = session.tracks[0].wav->samples();
  va.frames = session.tracks[0].wav->frames();
  vb.samples = session.tracks[1].wav->samples();
  vb.frames = session.tracks[1].wav->frames();
  SyncResult r = align_tracks(va, vb, &prior);
  CHECK(r.method == SyncMethod::Audio);
  CHECK(std::fabs(r.map.offset_seconds - pb.offset) < 0.0005);
  CHECK(std::fabs(r.map.drift - pb.drift) < 5e-6);
  CHECK(r.confidence >= 0.7);

  session = Session();
  fs::remove_all(dir);
}

TEST_CASE("the reference track is copied exactly, without resampling") {
  TrackParams a;
  a.seconds = 5.0;
  a.noise = 0.01;
  Scene scene;
  auto ta = render(scene, a);

  ExportTrack ea;
  ea.audio = view(ta, a);
  ea.name = "a";
  fs::path dir = fs::temp_directory_path() / "mydaw_test_copy";
  fs::remove_all(dir);
  ExportOptions options;
  options.out_dir = dir;
  std::string err;
  REQUIRE(export_tracks({ea}, options, nullptr, &err));
  WavFile f;
  REQUIRE(f.open(dir / "a.wav", &err));
  REQUIRE(f.frames() == ta.size());
  CHECK(std::equal(ta.begin(), ta.end(), f.samples()));
  f.close();
  fs::remove_all(dir);
}

TEST_CASE("export range trims every track to the same span") {
  TrackParams a;
  a.seconds = 10.0;
  a.noise = 0.01;
  Scene scene;
  auto ta = render(scene, a);
  ExportTrack ea;
  ea.audio = view(ta, a);
  ea.name = "a";
  fs::path dir = fs::temp_directory_path() / "mydaw_test_range";
  fs::remove_all(dir);
  ExportOptions options;
  options.out_dir = dir;
  options.use_range = true;
  options.range_start = 2.0;
  options.range_end = 5.0;
  std::string err;
  REQUIRE(export_tracks({ea}, options, nullptr, &err));
  WavFile f;
  REQUIRE(f.open(dir / "a.wav", &err));
  CHECK(f.frames() == 3 * 48000);
  CHECK(f.samples()[0] == ta[2 * 48000]);
  f.close();
  fs::remove_all(dir);
}
