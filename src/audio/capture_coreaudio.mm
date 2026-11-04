#include "audio/capture.h"

#import <AVFoundation/AVFoundation.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <thread>

namespace {

double ticks_to_seconds(uint64_t ticks) {
  static const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t value;
    mach_timebase_info(&value);
    return value;
  }();
  return double(ticks) * double(timebase.numer) / double(timebase.denom) * 1e-9;
}

template<class T>
bool property(AudioObjectID object, AudioObjectPropertySelector selector,
              AudioObjectPropertyScope scope, T* value) {
  AudioObjectPropertyAddress address = {selector, scope, kAudioObjectPropertyElementMain};
  UInt32 size = sizeof(T);
  return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, value) == noErr;
}

std::string string_property(AudioObjectID object, AudioObjectPropertySelector selector) {
  CFStringRef value = nullptr;
  if (!property(object, selector, kAudioObjectPropertyScopeGlobal, &value) || !value) return {};
  std::vector<char> buffer(CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1);
  bool ok = CFStringGetCString(value, buffer.data(), buffer.size(), kCFStringEncodingUTF8);
  CFRelease(value);
  return ok ? buffer.data() : "";
}

std::vector<AudioDeviceID> devices(std::string* err) {
  AudioObjectPropertyAddress address = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) {
    if (err) *err = "Could not list CoreAudio devices";
    return {};
  }
  std::vector<AudioDeviceID> result(size / sizeof(AudioDeviceID));
  if (size && AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, result.data()) != noErr) {
    if (err) *err = "Could not read CoreAudio devices";
    return {};
  }
  result.resize(size / sizeof(AudioDeviceID));
  return result;
}

UInt32 input_channels(AudioDeviceID device) {
  AudioObjectPropertyAddress address = {kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput,
                                        kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || !size) return 0;
  // max_align_t storage keeps the variable-length AudioBufferList aligned.
  std::vector<std::max_align_t> storage((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
  auto* buffers = reinterpret_cast<AudioBufferList*>(storage.data());
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, buffers) != noErr) return 0;
  UInt32 count = 0;
  for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) count += buffers->mBuffers[i].mNumberChannels;
  return count;
}

bool microphone_access(std::string* err) {
  @autoreleasepool {
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (status == AVAuthorizationStatusNotDetermined) {
      dispatch_semaphore_t ready = dispatch_semaphore_create(0);
      [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
        dispatch_semaphore_signal(ready);
      }];
      dispatch_semaphore_wait(ready, DISPATCH_TIME_FOREVER);
      status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    }
    if (status == AVAuthorizationStatusAuthorized) return true;
    if (err) *err = "Microphone access denied. Enable record-align in System Settings > Privacy & Security > Microphone.";
    return false;
  }
}

class CoreAudioStream final : public CaptureStream {
 public:
  explicit CoreAudioStream(CaptureSink* sink) : sink_(sink) {}
  ~CoreAudioStream() override {
    stopping_.store(true);
    if (monitor_.joinable()) monitor_.join();
    if (unit_) {
      AudioOutputUnitStop(unit_);
      AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
    }
  }

  bool start(AudioDeviceID device, std::string* err) {
    device_ = device;
    auto check = [&](OSStatus status, const char* action) {
      if (status == noErr) return true;
      if (err) *err = std::string(action) + " failed (CoreAudio " + std::to_string(status) + ")";
      return false;
    };
    AudioComponentDescription description = {};
    description.componentType = kAudioUnitType_Output;
    description.componentSubType = kAudioUnitSubType_HALOutput;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    if (!component) { if (err) *err = "CoreAudio HAL output unit unavailable"; return false; }
    if (!check(AudioComponentInstanceNew(component, &unit_), "Creating capture unit")) return false;
    UInt32 enabled = 1, disabled = 0;
    if (!check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                                   &enabled, sizeof(enabled)), "Enabling input") ||
        !check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,
                                   &disabled, sizeof(disabled)), "Disabling output") ||
        !check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                                   &device, sizeof(device)), "Selecting microphone")) return false;
    AudioStreamBasicDescription format = {};
    UInt32 size = sizeof(format);
    if (!check(AudioUnitGetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1,
                                   &format, &size), "Reading microphone format")) return false;
    channels_ = int(input_channels(device));
    rate_ = int(std::lround(format.mSampleRate));
    if (channels_ <= 0 || rate_ <= 0) { if (err) *err = "Microphone has no usable input format"; return false; }
    format = {};
    format.mSampleRate = rate_;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBytesPerPacket = format.mBytesPerFrame = UInt32(channels_) * sizeof(float);
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = channels_;
    format.mBitsPerChannel = 32;
    if (!check(AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
                                   &format, sizeof(format)), "Setting float capture format")) return false;
    UInt32 max_frames = 0;
    size = sizeof(max_frames);
    if (!check(AudioUnitGetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                   &max_frames, &size), "Reading capture buffer size")) return false;
    samples_.resize(size_t(max_frames) * channels_);
    AURenderCallbackStruct callback = {input, this};
    if (!check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0,
                                   &callback, sizeof(callback)), "Installing capture callback") ||
        !check(AudioUnitInitialize(unit_), "Initializing capture")) return false;
    sink_->on_format(rate_, channels_);
    if (!check(AudioOutputUnitStart(unit_), "Starting microphone")) return false;
    monitor_ = std::thread([this] {
      while (!stopping_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        UInt32 alive = 0;
        bool connected = property(device_, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, &alive);
        if (failure_.load() || !connected || !alive) {
          AudioOutputUnitStop(unit_);  // Wait for callbacks before reporting the terminal error.
          if (!stopping_.load()) sink_->on_error("Microphone disconnected or capture failed. Reconnect and arm it again.");
          return;
        }
      }
    });
    return true;
  }

  int sample_rate() const override { return rate_; }
  int channels() const override { return channels_; }
  bool raw_mode() const override { return true; }  // HAL input, without VoiceProcessingIO.

 private:
  static OSStatus input(void* context, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* timestamp,
                        UInt32, UInt32 frames, AudioBufferList*) {
    return static_cast<CoreAudioStream*>(context)->receive(flags, timestamp, frames);
  }

  OSStatus receive(AudioUnitRenderActionFlags* flags, const AudioTimeStamp* timestamp, UInt32 frames) {
    if (stopping_.load() || failure_.load()) return noErr;
    if (size_t(frames) * channels_ > samples_.size()) {
      failure_.store(true);
      return kAudioUnitErr_TooManyFramesToProcess;
    }
    AudioBufferList buffers = {};
    buffers.mNumberBuffers = 1;
    buffers.mBuffers[0] = {UInt32(channels_), UInt32(size_t(frames) * channels_ * sizeof(float)), samples_.data()};
    OSStatus status = AudioUnitRender(unit_, flags, timestamp, 1, frames, &buffers);
    if (status != noErr) { failure_.store(true); return status; }
    const bool has_position = timestamp->mFlags & kAudioTimeStampSampleTimeValid;
    const double position = has_position ? timestamp->mSampleTime : double(next_frame_);
    if (!started_) { first_frame_ = position; started_ = true; }
    double relative = has_position ? position - first_frame_ : double(next_frame_);
    if (relative < 0) { failure_.store(true); return noErr; }
    uint64_t frame = uint64_t(std::llround(relative));
    CapturePacket packet;
    packet.samples = samples_.data();
    packet.frames = frames;
    packet.device_position = frame;
    packet.host_seconds = (timestamp->mFlags & kAudioTimeStampHostTimeValid)
                            ? ticks_to_seconds(timestamp->mHostTime) : host_seconds_now() - double(frames) / rate_;
    packet.discontinuity = next_frame_ != 0 && frame != next_frame_;
    next_frame_ = frame + frames;
    sink_->on_packet(packet);
    return noErr;
  }

  CaptureSink* sink_;
  AudioUnit unit_ = nullptr;
  AudioDeviceID device_ = kAudioObjectUnknown;
  int rate_ = 0, channels_ = 0;
  std::vector<float> samples_;
  std::atomic<bool> stopping_{false}, failure_{false};
  std::thread monitor_;
  double first_frame_ = 0;
  uint64_t next_frame_ = 0;
  bool started_ = false;
};
}  // namespace

std::vector<InputDevice> list_input_devices(std::string* err) {
  std::vector<InputDevice> result;
  for (AudioDeviceID device : devices(err)) {
    UInt32 channels = input_channels(device);
    Float64 rate = 0;
    if (!channels || !property(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate)) continue;
    InputDevice info;
    info.id = string_property(device, kAudioDevicePropertyDeviceUID);
    info.name = string_property(device, kAudioObjectPropertyName);
    info.channels = int(channels);
    info.sample_rate = int(std::lround(rate));
    if (!info.id.empty()) result.push_back(std::move(info));
  }
  return result;
}

std::unique_ptr<CaptureStream> open_capture(const std::string& id, CaptureSink* sink, std::string* err) {
  if (!sink || !microphone_access(err)) return nullptr;
  for (AudioDeviceID device : devices(err)) {
    if (string_property(device, kAudioDevicePropertyDeviceUID) != id) continue;
    auto stream = std::make_unique<CoreAudioStream>(sink);
    if (!stream->start(device, err)) return nullptr;
    return stream;
  }
  if (err) *err = "Microphone is no longer available. Refresh the device list.";
  return nullptr;
}

double host_seconds_now() { return ticks_to_seconds(mach_absolute_time()); }
