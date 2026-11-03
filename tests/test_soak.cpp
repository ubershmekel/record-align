// Slow checks, skipped by default. Run with: mydaw-tests --no-skip -tc="soak*"
#include <doctest/doctest.h>

#include <windows.h>

#include <psapi.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "audio/recorder.h"
#include "audio/wav.h"

namespace fs = std::filesystem;

namespace {

size_t private_bytes() {
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
  return counters.PrivateUsage;
}

}  // namespace

TEST_CASE("soak: one hour of two stereo tracks keeps every frame and doesn't grow memory" * doctest::skip()) {
  fs::path dir = fs::temp_directory_path() / "mydaw_test_soak";
  fs::remove_all(dir);

  const int rate = 48000;
  const int channels = 2;
  const uint32_t packet_frames = 480;
  const uint64_t packets = 3600 * rate / packet_frames;

  Recorder recorder;
  InputDevice a, b;
  a.id = "a";
  a.name = "A";
  b.id = "b";
  b.name = "B";
  CaptureSink* sink_a = recorder.arm_sink_for_test(a);
  CaptureSink* sink_b = recorder.arm_sink_for_test(b);
  sink_a->on_format(rate, channels);
  sink_b->on_format(rate, channels);

  std::string err;
  REQUIRE(recorder.start_in(dir, &err));
  const size_t memory_at_start = private_bytes();
  size_t memory_peak = memory_at_start;

  std::vector<float> samples(size_t(packet_frames) * channels);
  float counter = 0.0f;
  for (uint64_t p = 0; p < packets; p++) {
    for (auto& s : samples) {
      s = counter;
      counter = counter > 1000.0f ? 0.0f : counter + 1.0f;
    }
    CapturePacket packet;
    packet.samples = samples.data();
    packet.frames = packet_frames;
    packet.device_position = p * packet_frames;
    packet.host_seconds = 1000.0 + double(p) * packet_frames / rate;
    sink_a->on_packet(packet);
    sink_b->on_packet(packet);
    if (p % 1000 == 999) {
      // Ten seconds of audio per step keeps this about 50 times faster than real
      // time but within what a disk can absorb.
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      memory_peak = std::max(memory_peak, private_bytes());
    }
  }
  recorder.stop();

  auto status = recorder.status();
  const uint64_t expected = packets * packet_frames;
  for (const auto& s : status) {
    // At 50 times real speed the 30 second buffer covers only a 0.6 second disk
    // stall, so a little loss is possible here. It has to stay small and logged,
    // and the timeline check below has to hold regardless.
    MESSAGE("dropped " << double(s.dropped_frames) / rate << " seconds at 50x speed");
    CHECK(s.dropped_frames < expected / 100);
    CHECK(s.gap_frames == 0);
    CHECK(s.frames_written == expected);
    WavFile f;
    REQUIRE(f.open(dir / s.file_name, &err));
    CHECK(f.frames() == expected);
    CHECK_FALSE(f.recovered());
  }
  MESSAGE("memory growth while recording: " << (memory_peak - memory_at_start) / 1000000 << " MB");
  CHECK(memory_peak - memory_at_start < 100000000);
  fs::remove_all(dir);
}

TEST_CASE("soak: a real file past 4 GB becomes RF64 and reads back" * doctest::skip()) {
  fs::path dir = fs::temp_directory_path() / "mydaw_test_rf64_real";
  fs::remove_all(dir);
  fs::create_directories(dir);
  fs::path path = dir / "big.wav";

  std::string err;
  std::vector<float> block(1 << 20);
  const uint64_t blocks = (uint64_t(4300) * 1000 * 1000) / (block.size() * 4);
  {
    WavWriter w;
    REQUIRE(w.open(path, 48000, 1, &err));
    for (uint64_t b = 0; b < blocks; b++) {
      block[0] = float(b);
      REQUIRE(w.write(block.data(), block.size()));
      if (b % 256 == 0) REQUIRE(w.checkpoint());
    }
    REQUIRE(w.close());
  }
  CHECK(fs::file_size(path) > 0xFFFFFFFFull);

  WavFile f;
  REQUIRE(f.open(path, &err));
  CHECK_FALSE(f.recovered());
  CHECK(f.frames() == blocks * block.size());
  CHECK(f.samples()[(blocks - 1) * block.size()] == float(blocks - 1));
  f.close();
  fs::remove_all(dir);
}
