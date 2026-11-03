#include <initguid.h>
#include <windows.h>

#include <objbase.h>
#include <propkeydef.h>

#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <future>
#include <thread>

#include "audio/capture.h"

using Microsoft::WRL::ComPtr;

namespace {

std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

std::string describe_hresult(const char* action, HRESULT hr) {
  const char* why = nullptr;
  switch (hr) {
    case E_ACCESSDENIED:
      why = "access denied. Check Settings > Privacy & security > Microphone";
      break;
    case AUDCLNT_E_DEVICE_INVALIDATED:
      why = "the device was disconnected or disabled";
      break;
    case AUDCLNT_E_DEVICE_IN_USE:
      why = "another app has exclusive control of the device";
      break;
    case AUDCLNT_E_RESOURCES_INVALIDATED:
      why = "the device's format or configuration changed";
      break;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
      why = "the Windows Audio service is not running";
      break;
    default:
      break;
  }
  char buf[256];
  if (why) {
    snprintf(buf, sizeof(buf), "%s: %s", action, why);
  } else {
    snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lX)", action, (unsigned long)hr);
  }
  return buf;
}

struct SampleFormat {
  bool is_float = false;
  int container_bits = 0;
  int channels = 0;
  int sample_rate = 0;
};

bool parse_format(const WAVEFORMATEX* wf, SampleFormat* out) {
  out->channels = wf->nChannels;
  out->sample_rate = int(wf->nSamplesPerSec);
  out->container_bits = wf->wBitsPerSample;
  WORD tag = wf->wFormatTag;
  if (tag == WAVE_FORMAT_EXTENSIBLE) {
    // The first field of the subformat GUID holds the plain format tag.
    tag = WORD(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf)->SubFormat.Data1);
  }
  out->is_float = tag == WAVE_FORMAT_IEEE_FLOAT;
  if (out->is_float) return out->container_bits == 32;
  return tag == WAVE_FORMAT_PCM &&
         (out->container_bits == 16 || out->container_bits == 24 || out->container_bits == 32);
}

void convert_to_float(const BYTE* src, float* dst, size_t count, const SampleFormat& f) {
  if (f.is_float) {
    memcpy(dst, src, count * sizeof(float));
  } else if (f.container_bits == 16) {
    const int16_t* s = reinterpret_cast<const int16_t*>(src);
    for (size_t i = 0; i < count; i++) dst[i] = float(s[i]) / 32768.0f;
  } else if (f.container_bits == 24) {
    for (size_t i = 0; i < count; i++) {
      int32_t v = int32_t(uint32_t(src[i * 3]) << 8 | uint32_t(src[i * 3 + 1]) << 16 | uint32_t(src[i * 3 + 2]) << 24);
      dst[i] = float(double(v) / 2147483648.0);
    }
  } else {
    const int32_t* s = reinterpret_cast<const int32_t*>(src);
    for (size_t i = 0; i < count; i++) dst[i] = float(double(s[i]) / 2147483648.0);
  }
}

struct ComScope {
  HRESULT hr;
  ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(hr)) CoUninitialize();
  }
};

class WasapiStream final : public CaptureStream {
 public:
  WasapiStream(std::string id, CaptureSink* sink) : id_(std::move(id)), sink_(sink) {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~WasapiStream() override {
    SetEvent(stop_event_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(stop_event_);
  }

  bool start(std::string* err) {
    std::promise<std::string> opened;
    auto result = opened.get_future();
    thread_ = std::thread([this, &opened] { run(opened); });
    std::string message = result.get();
    if (!message.empty()) {
      if (err) *err = message;
      thread_.join();
      return false;
    }
    return true;
  }

  int sample_rate() const override { return format_.sample_rate; }
  int channels() const override { return format_.channels; }
  bool raw_mode() const override { return raw_mode_; }

 private:
  HRESULT open_client(IMMDevice* device, bool raw, ComPtr<IAudioClient>* client_out, HANDLE audio_event) {
    ComPtr<IAudioClient> client;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return hr;
    if (raw) {
      ComPtr<IAudioClient2> client2;
      hr = client.As(&client2);
      if (FAILED(hr)) return hr;
      AudioClientProperties props = {};
      props.cbSize = sizeof(props);
      props.eCategory = AudioCategory_Media;
      props.Options = AUDCLNT_STREAMOPTIONS_RAW;
      hr = client2->SetClientProperties(&props);
      if (FAILED(hr)) return hr;
    }
    WAVEFORMATEX* wf = nullptr;
    hr = client->GetMixFormat(&wf);
    if (FAILED(hr)) return hr;
    if (!parse_format(wf, &format_)) {
      CoTaskMemFree(wf);
      return E_NOTIMPL;
    }
    // One second of OS buffering gives the capture thread room if it gets delayed.
    const REFERENCE_TIME kBufferDuration = 10000000;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration, 0, wf, nullptr);
    CoTaskMemFree(wf);
    if (FAILED(hr)) return hr;
    hr = client->SetEventHandle(audio_event);
    if (FAILED(hr)) return hr;
    *client_out = client;
    return S_OK;
  }

  void run(std::promise<std::string>& opened) {
    ComScope com;
    if (FAILED(com.hr)) {
      opened.set_value(describe_hresult("Starting COM", com.hr));
      return;
    }
    HANDLE audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDevice(widen(id_).c_str(), &device);
    if (FAILED(hr)) {
      CloseHandle(audio_event);
      opened.set_value(describe_hresult("Opening the device", hr));
      return;
    }

    // Prefer raw capture, which skips OS voice effects, but not all drivers allow it.
    raw_mode_ = true;
    hr = open_client(device.Get(), true, &client, audio_event);
    if (FAILED(hr)) {
      raw_mode_ = false;
      hr = open_client(device.Get(), false, &client, audio_event);
    }
    if (hr == E_NOTIMPL) {
      CloseHandle(audio_event);
      opened.set_value("The device's audio format is not supported");
      return;
    }
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr)) {
      sink_->on_format(format_.sample_rate, format_.channels);
      hr = client->Start();
    }
    if (FAILED(hr)) {
      CloseHandle(audio_event);
      opened.set_value(describe_hresult("Starting capture", hr));
      return;
    }
    opened.set_value(std::string());

    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    capture_loop(client.Get(), capture.Get(), audio_event);

    client->Stop();
    if (task) AvRevertMmThreadCharacteristics(task);
    CloseHandle(audio_event);
  }

  void capture_loop(IAudioClient* client, IAudioCaptureClient* capture, HANDLE audio_event) {
    std::vector<float> converted;
    const HANDLE handles[2] = {stop_event_, audio_event};
    for (;;) {
      DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 2000);
      if (wait == WAIT_OBJECT_0) return;
      if (wait == WAIT_TIMEOUT) {
        // Some drivers go quiet on removal rather than returning an error at once.
        UINT32 padding = 0;
        HRESULT hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
          sink_->on_error(describe_hresult("Capture", hr));
          return;
        }
        continue;
      }
      for (;;) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 device_position = 0;
        UINT64 qpc_position = 0;
        HRESULT hr = capture->GetBuffer(&data, &frames, &flags, &device_position, &qpc_position);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
        if (FAILED(hr)) {
          sink_->on_error(describe_hresult("Capture", hr));
          return;
        }
        size_t count = size_t(frames) * format_.channels;
        if (converted.size() < count) converted.resize(count);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          std::fill(converted.begin(), converted.begin() + count, 0.0f);
        } else {
          convert_to_float(data, converted.data(), count, format_);
        }
        CapturePacket packet;
        packet.samples = converted.data();
        packet.frames = frames;
        packet.device_position = device_position;
        packet.host_seconds = double(qpc_position) * 1e-7;  // GetBuffer reports QPC in 100 ns units.
        packet.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        sink_->on_packet(packet);
        hr = capture->ReleaseBuffer(frames);
        if (FAILED(hr)) {
          sink_->on_error(describe_hresult("Capture", hr));
          return;
        }
      }
    }
  }

  std::string id_;
  CaptureSink* sink_;
  HANDLE stop_event_ = nullptr;
  std::thread thread_;
  SampleFormat format_;
  bool raw_mode_ = false;
};

void list_devices_on_this_thread(std::vector<InputDevice>* out, std::string* err) {
  ComScope com;
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDeviceCollection> collection;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
  UINT count = 0;
  if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
  if (FAILED(hr)) {
    if (err) *err = describe_hresult("Listing devices", hr);
    return;
  }
  for (UINT i = 0; i < count; i++) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(i, &device))) continue;
    InputDevice info;
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) continue;
    info.id = narrow(id);
    CoTaskMemFree(id);

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
      PROPVARIANT name;
      PropVariantInit(&name);
      if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
        info.name = narrow(name.pwszVal);
      }
      PropVariantClear(&name);
    }
    if (info.name.empty()) info.name = "Unknown microphone";

    ComPtr<IAudioClient> client;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(client.GetAddressOf())))) {
      WAVEFORMATEX* wf = nullptr;
      if (SUCCEEDED(client->GetMixFormat(&wf))) {
        info.sample_rate = int(wf->nSamplesPerSec);
        info.channels = wf->nChannels;
        CoTaskMemFree(wf);
      }
    }
    out->push_back(std::move(info));
  }
}

}  // namespace

std::vector<InputDevice> list_input_devices(std::string* err) {
  // A separate thread because the caller's thread may already be in another COM mode.
  std::vector<InputDevice> devices;
  std::thread worker([&] { list_devices_on_this_thread(&devices, err); });
  worker.join();
  return devices;
}

std::unique_ptr<CaptureStream> open_capture(const std::string& device_id, CaptureSink* sink, std::string* err) {
  auto stream = std::make_unique<WasapiStream>(device_id, sink);
  if (!stream->start(err)) return nullptr;
  return stream;
}

double host_seconds_now() {
  LARGE_INTEGER counter;
  LARGE_INTEGER frequency;
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return double(counter.QuadPart) / double(frequency.QuadPart);
}
