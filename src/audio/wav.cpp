#include "platform/files.h"
#include "audio/wav.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define _fseeki64 fseeko
#define _commit fsync
#define _fileno fileno
#endif

#include <cctype>
#include <cstring>

namespace {

// Fixed header layout written by WavWriter. Samples start 4-byte aligned.
//    0 RIFF/RF64  size  WAVE
//   12 JUNK/ds64  28 bytes
//   48 fmt        16 bytes
//   72 fact       4 bytes
//   84 data       size
constexpr long kHeaderSize = 92;
constexpr uint32_t kUnknownSize = 0xFFFFFFFFu;
constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

void put_u16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void put_u32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put_u64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
uint16_t get_u16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t get_u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t get_u64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

bool is_chunk_id(const uint8_t* p) {
  for (int i = 0; i < 4; i++) {
    if (!std::isprint(p[i])) return false;
  }
  return true;
}

}  // namespace

WavWriter::~WavWriter() { close(); }

bool WavWriter::fail(const char* what) {
  error_ = std::string(what) + " failed for " + path_.u8string();
  return false;
}

bool WavWriter::open(const std::filesystem::path& path, int sample_rate, int channels, std::string* err) {
  close();
  path_ = path;
  frames_ = 0;
  rf64_ = false;
  channels_ = channels;
  error_.clear();
  file_ = open_file(path.c_str(), "wb");
  if (!file_) {
    if (err) *err = "Could not create " + path.u8string();
    return false;
  }
  // A large stdio buffer keeps the disk thread from making many small writes.
  std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);

  uint8_t h[kHeaderSize] = {};
  std::memcpy(h + 0, "RIFF", 4);
  std::memcpy(h + 8, "WAVE", 4);
  std::memcpy(h + 12, "JUNK", 4);
  put_u32(h + 16, 28);
  std::memcpy(h + 48, "fmt ", 4);
  put_u32(h + 52, 16);
  put_u16(h + 56, kFormatFloat);
  put_u16(h + 58, uint16_t(channels));
  put_u32(h + 60, uint32_t(sample_rate));
  put_u32(h + 64, uint32_t(sample_rate * channels * 4));
  put_u16(h + 68, uint16_t(channels * 4));
  put_u16(h + 70, 32);
  std::memcpy(h + 72, "fact", 4);
  put_u32(h + 76, 4);
  std::memcpy(h + 84, "data", 4);
  if (std::fwrite(h, 1, kHeaderSize, file_) != kHeaderSize || !patch_header()) {
    if (err) *err = "Could not write header to " + path.u8string();
    close();
    return false;
  }
  return true;
}

bool WavWriter::write(const float* interleaved, uint64_t frames) {
  if (!file_) return false;
  size_t count = size_t(frames) * channels_;
  if (std::fwrite(interleaved, sizeof(float), count, file_) != count) return fail("Writing audio");
  frames_ += frames;
  return true;
}

bool WavWriter::patch_header() {
  uint64_t data_bytes = frames_ * channels_ * 4;
  uint64_t riff_bytes = kHeaderSize - 8 + data_bytes;
  if (riff_bytes >= rf64_threshold) rf64_ = true;

  uint8_t riff[8];
  uint8_t ds64[36] = {};
  uint8_t fact[4];
  uint8_t data_size[4];
  if (rf64_) {
    std::memcpy(riff, "RF64", 4);
    put_u32(riff + 4, kUnknownSize);
    std::memcpy(ds64, "ds64", 4);
    put_u32(ds64 + 4, 28);
    put_u64(ds64 + 8, riff_bytes);
    put_u64(ds64 + 16, data_bytes);
    put_u64(ds64 + 24, frames_);
    put_u32(fact, kUnknownSize);
    put_u32(data_size, kUnknownSize);
  } else {
    std::memcpy(riff, "RIFF", 4);
    put_u32(riff + 4, uint32_t(riff_bytes));
    put_u32(fact, uint32_t(frames_));
    put_u32(data_size, uint32_t(data_bytes));
  }

  // The audio is flushed first, so the header never claims more than is on disk.
  if (std::fflush(file_) != 0) return fail("Flushing audio");
  if (_fseeki64(file_, 0, SEEK_SET) != 0) return fail("Seeking");
  if (std::fwrite(riff, 1, 8, file_) != 8) return fail("Writing header");
  if (rf64_) {
    if (_fseeki64(file_, 12, SEEK_SET) != 0) return fail("Seeking");
    if (std::fwrite(ds64, 1, sizeof(ds64), file_) != sizeof(ds64)) return fail("Writing header");
  }
  if (_fseeki64(file_, 80, SEEK_SET) != 0) return fail("Seeking");
  if (std::fwrite(fact, 1, 4, file_) != 4) return fail("Writing header");
  if (_fseeki64(file_, 88, SEEK_SET) != 0) return fail("Seeking");
  if (std::fwrite(data_size, 1, 4, file_) != 4) return fail("Writing header");
  if (_fseeki64(file_, 0, SEEK_END) != 0) return fail("Seeking");
  return true;
}

bool WavWriter::checkpoint() {
  if (!file_) return false;
  if (!patch_header()) return false;
  if (std::fflush(file_) != 0) return fail("Flushing");
  if (_commit(_fileno(file_)) != 0) return fail("Committing to disk");
  return true;
}

bool WavWriter::close() {
  if (!file_) return true;
  bool ok = checkpoint();
  if (std::fclose(file_) != 0) ok = fail("Closing");
  file_ = nullptr;
  return ok;
}

WavFile::~WavFile() { close(); }

void WavFile::close() {
#ifdef _WIN32
  if (view_) UnmapViewOfFile(view_);
  if (mapping_) CloseHandle(mapping_);
  if (file_handle_) CloseHandle(file_handle_);
  file_handle_ = nullptr;
  mapping_ = nullptr;
#else
  if (view_) munmap(const_cast<uint8_t*>(view_), mapped_size_);
  mapped_size_ = 0;
#endif
  view_ = nullptr;
  samples_ = nullptr;
  frames_ = 0;
  sample_rate_ = 0;
  channels_ = 0;
  recovered_ = false;
}

bool WavFile::open(const std::filesystem::path& path, std::string* err) {
  close();
  auto fail = [&](const std::string& why) {
    if (err) *err = path.filename().u8string() + ": " + why;
    close();
    return false;
  };

#ifdef _WIN32
  // FILE_SHARE_WRITE allows opening a file another process is still recording.
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return fail("could not open file");
  file_handle_ = file;

  LARGE_INTEGER size_li;
  if (!GetFileSizeEx(file, &size_li)) return fail("could not read file size");
  uint64_t file_size = uint64_t(size_li.QuadPart);
  if (file_size < 12) return fail("too small to be a WAV file");

  mapping_ = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping_) return fail("could not map file");
  view_ = static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
  if (!view_) return fail("could not map file view");
#else
  int file = ::open(path.c_str(), O_RDONLY);
  if (file < 0) return fail("could not open file");
  struct stat info = {};
  if (fstat(file, &info) != 0 || info.st_size < 12) {
    ::close(file);
    return fail("too small to be a WAV file");
  }
  uint64_t file_size = uint64_t(info.st_size);
  void* mapped = mmap(nullptr, size_t(file_size), PROT_READ, MAP_PRIVATE, file, 0);
  ::close(file);
  if (mapped == MAP_FAILED) return fail("could not map file");
  mapped_size_ = size_t(file_size);
  view_ = static_cast<const uint8_t*>(mapped);
#endif

  bool is_rf64 = std::memcmp(view_, "RF64", 4) == 0;
  if ((!is_rf64 && std::memcmp(view_, "RIFF", 4) != 0) || std::memcmp(view_ + 8, "WAVE", 4) != 0) {
    return fail("not a WAV file");
  }

  uint64_t ds64_data_size = 0;
  uint16_t format = 0;
  uint16_t bits = 0;
  uint64_t pos = 12;
  uint64_t data_pos = 0;
  uint64_t data_size = 0;
  bool has_fmt = false;
  while (pos + 8 <= file_size) {
    const uint8_t* chunk = view_ + pos;
    uint64_t size = get_u32(chunk + 4);
    if (std::memcmp(chunk, "ds64", 4) == 0 && size >= 24 && pos + 8 + 24 <= file_size) {
      ds64_data_size = get_u64(chunk + 16);
    } else if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16 && pos + 8 + size <= file_size) {
      format = get_u16(chunk + 8);
      channels_ = get_u16(chunk + 10);
      sample_rate_ = int(get_u32(chunk + 12));
      bits = get_u16(chunk + 22);
      if (format == kFormatExtensible && size >= 26) format = get_u16(chunk + 32);
      has_fmt = true;
    } else if (std::memcmp(chunk, "data", 4) == 0) {
      data_pos = pos + 8;
      data_size = (size == kUnknownSize && ds64_data_size) ? ds64_data_size : size;
      break;
    }
    pos += 8 + size + (size & 1);
  }
  if (!has_fmt) return fail("missing format chunk");
  if (!data_pos) return fail("missing audio data");
  if (format == kFormatPcm) return fail("integer PCM is not supported yet, only 32-bit float");
  if (format != kFormatFloat || bits != 32) return fail("only 32-bit float WAV is supported");
  if (channels_ < 1 || sample_rate_ < 1) return fail("invalid format");
  if (data_pos % 4 != 0) return fail("unaligned audio data");

  // The header can be stale after a crash. Use all the bytes in the file unless
  // what follows the claimed end really looks like another chunk.
  uint64_t available = file_size - data_pos;
  if (data_size != available) {
    bool has_next_chunk = false;
    uint64_t next = data_pos + data_size + (data_size & 1);
    if (data_size > 0 && next + 8 <= file_size && is_chunk_id(view_ + next)) {
      has_next_chunk = next + 8 + get_u32(view_ + next + 4) <= file_size;
    }
    if (data_size > available || !has_next_chunk) {
      data_size = available;
      recovered_ = true;
    }
  }

  samples_ = reinterpret_cast<const float*>(view_ + data_pos);
  frames_ = data_size / (uint64_t(channels_) * 4);
  return true;
}
