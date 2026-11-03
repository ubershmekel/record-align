#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The boundary between the platform audio API and everything else. A macOS
// backend implements these same functions with CoreAudio.

struct InputDevice {
  std::string id;
  std::string name;
  int sample_rate = 0;
  int channels = 0;
};

struct CapturePacket {
  const float* samples = nullptr;  // Interleaved, valid only during the callback.
  uint32_t frames = 0;
  uint64_t device_position = 0;    // Frames on this device's own clock.
  double host_seconds = 0.0;       // When the first frame was captured, on a clock shared by all devices.
  bool discontinuity = false;      // The OS reported a glitch before this packet.
};

// Called on the stream's capture thread.
class CaptureSink {
 public:
  virtual ~CaptureSink() = default;
  virtual void on_format(int sample_rate, int channels) = 0;  // Before the first packet.
  virtual void on_packet(const CapturePacket& packet) = 0;
  virtual void on_error(const std::string& message) = 0;      // No packets follow this.
};

class CaptureStream {
 public:
  virtual ~CaptureStream() = default;  // Stops capture and joins the thread.
  virtual int sample_rate() const = 0;
  virtual int channels() const = 0;
  virtual bool raw_mode() const = 0;   // True if OS audio effects are bypassed.
};

std::vector<InputDevice> list_input_devices(std::string* err);
std::unique_ptr<CaptureStream> open_capture(const std::string& device_id, CaptureSink* sink, std::string* err);
double host_seconds_now();
