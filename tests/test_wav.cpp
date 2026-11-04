#include "platform/files.h"
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

#include "audio/ring_buffer.h"
#include "audio/wav.h"

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

std::vector<float> ramp(size_t count) {
  std::vector<float> v(count);
  for (size_t i = 0; i < count; i++) v[i] = float(i) * 1e-4f;
  return v;
}

}  // namespace

TEST_CASE("WAV round trip keeps every sample") {
  TempDir dir("roundtrip");
  fs::path path = dir.path / "a.wav";
  auto data = ramp(2000);
  std::string err;
  {
    WavWriter w;
    REQUIRE(w.open(path, 44100, 2, &err));
    REQUIRE(w.write(data.data(), 1000));
    REQUIRE(w.close());
  }
  WavFile f;
  REQUIRE(f.open(path, &err));
  CHECK(f.sample_rate() == 44100);
  CHECK(f.channels() == 2);
  CHECK(f.frames() == 1000);
  CHECK_FALSE(f.recovered());
  for (size_t i = 0; i < data.size(); i++) REQUIRE(f.samples()[i] == data[i]);
}

TEST_CASE("audio written after the last checkpoint is recovered after a crash") {
  TempDir dir("crash");
  fs::path path = dir.path / "a.wav";
  auto data = ramp(1000);
  std::string err;
  {
    WavWriter w;
    REQUIRE(w.open(path, 48000, 1, &err));
    REQUIRE(w.write(data.data(), 400));
    REQUIRE(w.checkpoint());
    REQUIRE(w.write(data.data() + 400, 600));
    REQUIRE(w.checkpoint());
    REQUIRE(w.close());
  }
  // Simulate a crash: put back a header that claims only the first 400 frames.
  FILE* f = open_file(path.c_str(), "r+b");
  REQUIRE(f);
  uint32_t stale = 1600;
  fseek(f, 88, SEEK_SET);
  fwrite(&stale, 4, 1, f);
  fclose(f);

  WavFile recovered;
  REQUIRE(recovered.open(path, &err));
  CHECK(recovered.recovered());
  CHECK(recovered.frames() == 1000);
  CHECK(recovered.samples()[999] == data[999]);
}

TEST_CASE("a file with a zero-length header is recovered") {
  TempDir dir("zero");
  fs::path path = dir.path / "a.wav";
  auto data = ramp(100);
  std::string err;
  {
    WavWriter w;
    REQUIRE(w.open(path, 48000, 1, &err));
    REQUIRE(w.write(data.data(), 100));
    REQUIRE(w.close());
  }
  FILE* f = open_file(path.c_str(), "r+b");
  REQUIRE(f);
  uint32_t zero = 0;
  fseek(f, 88, SEEK_SET);
  fwrite(&zero, 4, 1, f);
  fclose(f);

  WavFile recovered;
  REQUIRE(recovered.open(path, &err));
  CHECK(recovered.frames() == 100);
}

TEST_CASE("files past the RIFF size limit become RF64 and still read back") {
  TempDir dir("rf64");
  fs::path path = dir.path / "a.wav";
  auto data = ramp(4000);
  std::string err;
  {
    WavWriter w;
    w.rf64_threshold = 1000;
    REQUIRE(w.open(path, 48000, 1, &err));
    REQUIRE(w.write(data.data(), 4000));
    REQUIRE(w.close());
  }
  FILE* raw = open_file(path.c_str(), "rb");
  REQUIRE(raw);
  char id[5] = {};
  fread(id, 1, 4, raw);
  fclose(raw);
  CHECK(std::string(id) == "RF64");

  WavFile f;
  REQUIRE(f.open(path, &err));
  CHECK(f.frames() == 4000);
  CHECK_FALSE(f.recovered());
  CHECK(f.samples()[3999] == data[3999]);
}

TEST_CASE("ring buffer passes data intact between two threads") {
  SpscRing<uint32_t> ring(1024);
  const uint32_t total = 5000000;
  std::thread producer([&] {
    uint32_t next = 0;
    uint32_t block[97];
    while (next < total) {
      uint32_t n = std::min<uint32_t>(97, total - next);
      for (uint32_t i = 0; i < n; i++) block[i] = next + i;
      next += uint32_t(ring.push(block, n));
    }
  });
  uint32_t expected = 0;
  bool intact = true;
  uint32_t block[131];
  while (expected < total) {
    size_t n = ring.pop(block, 131);
    for (size_t i = 0; i < n; i++) intact = intact && block[i] == expected + i;
    expected += uint32_t(n);
  }
  producer.join();
  CHECK(intact);
}

TEST_CASE("ring buffer refuses to overfill") {
  SpscRing<float> ring(8);
  std::vector<float> data(20, 1.0f);
  CHECK(ring.push(data.data(), 20) == 8);
  CHECK(ring.write_available() == 0);
  CHECK(ring.push_zeros(1) == 0);
  float out[3];
  CHECK(ring.pop(out, 3) == 3);
  CHECK(ring.push_zeros(5) == 3);
  CHECK(ring.read_available() == 8);
}
