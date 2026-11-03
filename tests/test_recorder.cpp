#include <doctest/doctest.h>

#include <filesystem>
#include <vector>

#include "audio/recorder.h"
#include "audio/session.h"
#include "audio/wav.h"

namespace fs = std::filesystem;

namespace {

struct FakeDevice {
  CaptureSink* sink;
  int rate;
  uint64_t position = 0;
  double host = 100.0;
  uint32_t next_value = 0;

  void packet(uint32_t frames, uint64_t skip_frames = 0, double extra_host = 0.0) {
    position += skip_frames;
    host += double(skip_frames) / rate + extra_host;
    std::vector<float> samples(frames);
    for (auto& s : samples) s = float(++next_value);
    CapturePacket p;
    p.samples = samples.data();
    p.frames = frames;
    p.device_position = position;
    p.host_seconds = host;
    sink->on_packet(p);
    position += frames;
    host += double(frames) / rate;
  }
};

struct TempDir {
  fs::path path;
  explicit TempDir(const char* name) {
    path = fs::temp_directory_path() / (std::string("mydaw_test_") + name);
    fs::remove_all(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

}  // namespace

TEST_CASE("recorder writes only between start and stop, with timestamps") {
  TempDir dir("rec_basic");
  Recorder recorder;
  InputDevice device;
  device.id = "fake";
  device.name = "Fake: Mic?";
  CaptureSink* sink = recorder.arm_sink_for_test(device);
  sink->on_format(1000, 1);
  FakeDevice fake{sink, 1000};

  fake.packet(100);  // Armed but not recording: meters only.
  std::string err;
  REQUIRE(recorder.start_in(dir.path, &err));
  const uint32_t first_recorded = fake.next_value + 1;
  for (int i = 0; i < 10; i++) fake.packet(100);
  recorder.stop();
  fake.packet(100);

  auto status = recorder.status();
  REQUIRE(status.size() == 1);
  CHECK(status[0].frames_written == 1000);
  CHECK(status[0].gap_frames == 0);
  CHECK(status[0].dropped_frames == 0);
  CHECK(status[0].file_name == "01_Fake_ Mic_.wav");
  CHECK(fs::exists(dir.path / "session.json"));

  WavFile f;
  REQUIRE(f.open(dir.path / status[0].file_name, &err));
  CHECK(f.frames() == 1000);
  CHECK(f.samples()[0] == float(first_recorded));
  CHECK(f.samples()[999] == float(first_recorded + 999));
}

TEST_CASE("a device position jump becomes silence so the timeline stays true") {
  TempDir dir("rec_gap");
  Recorder recorder;
  InputDevice device;
  device.id = "fake";
  device.name = "Mic";
  CaptureSink* sink = recorder.arm_sink_for_test(device);
  sink->on_format(1000, 1);
  FakeDevice fake{sink, 1000};

  std::string err;
  REQUIRE(recorder.start_in(dir.path, &err));
  fake.packet(100);
  fake.packet(100, 50);
  fake.packet(100);
  recorder.stop();

  auto status = recorder.status();
  CHECK(status[0].gap_frames == 50);
  CHECK(status[0].frames_written == 350);

  WavFile f;
  REQUIRE(f.open(dir.path / status[0].file_name, &err));
  REQUIRE(f.frames() == 350);
  CHECK(f.samples()[99] != 0.0f);
  CHECK(f.samples()[100] == 0.0f);
  CHECK(f.samples()[149] == 0.0f);
  CHECK(f.samples()[150] != 0.0f);

  Session session;
  REQUIRE(load_session(dir.path, &session, &err));
  CHECK(session.tracks[0].gap_frames == 50);
}

TEST_CASE("a stall the device position doesn't show is caught by the host clock") {
  TempDir dir("rec_stall");
  Recorder recorder;
  InputDevice device;
  device.id = "fake";
  device.name = "Mic";
  CaptureSink* sink = recorder.arm_sink_for_test(device);
  sink->on_format(1000, 1);
  FakeDevice fake{sink, 1000};

  std::string err;
  REQUIRE(recorder.start_in(dir.path, &err));
  fake.packet(100);
  fake.packet(100, 0, 2.0);
  recorder.stop();

  auto status = recorder.status();
  CHECK(status[0].gap_frames >= 1999);
  CHECK(status[0].gap_frames <= 2001);
}

TEST_CASE("a full ring buffer drops whole packets and keeps the timeline") {
  TempDir dir("rec_drop");
  Recorder recorder;
  InputDevice device;
  device.id = "fake";
  device.name = "Mic";
  CaptureSink* sink = recorder.arm_sink_for_test(device);
  sink->on_format(10, 1);  // 300 sample ring (rounded up to 512), far too small for this burst.
  FakeDevice fake{sink, 10};

  std::string err;
  REQUIRE(recorder.start_in(dir.path, &err));
  uint64_t sent = 0;
  for (int i = 0; i < 2000; i++) {
    fake.packet(500);
    sent += 500;
  }
  recorder.stop();

  auto status = recorder.status();
  CHECK(status[0].dropped_frames > 0);
  // Silence still owed when recording stops can't be written, but only the last
  // stretch can be short: everything before it must be accounted for.
  CHECK(status[0].frames_written <= sent);
  WavFile f;
  REQUIRE(f.open(dir.path / status[0].file_name, &err));
  CHECK(f.frames() == status[0].frames_written);
}

TEST_CASE("a device error is reported and other tracks keep recording") {
  TempDir dir("rec_error");
  Recorder recorder;
  InputDevice a, b;
  a.id = "a";
  a.name = "A";
  b.id = "b";
  b.name = "B";
  CaptureSink* sink_a = recorder.arm_sink_for_test(a);
  CaptureSink* sink_b = recorder.arm_sink_for_test(b);
  sink_a->on_format(1000, 1);
  sink_b->on_format(1000, 1);
  FakeDevice fake_a{sink_a, 1000};
  FakeDevice fake_b{sink_b, 1000};

  std::string err;
  REQUIRE(recorder.start_in(dir.path, &err));
  fake_a.packet(100);
  fake_b.packet(100);
  sink_a->on_error("the device was disconnected or disabled");
  fake_b.packet(100);
  recorder.stop();

  auto status = recorder.status();
  CHECK(status[0].error == "the device was disconnected or disabled");
  CHECK(status[0].frames_written == 100);
  CHECK(status[1].error.empty());
  CHECK(status[1].frames_written == 200);

  WavFile f;
  REQUIRE(f.open(dir.path / status[0].file_name, &err));
  CHECK(f.frames() == 100);
}
