#include "platform/files.h"
#include "audio/import.h"

#include "platform/process.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <vector>

#include "audio/edits.h"
#include "audio/wav.h"

namespace fs = std::filesystem;

namespace {

bool probe_audio(const fs::path& input, int* sample_rate, int* channels) {
  std::string text;
  const std::vector<std::string> command = {
      "ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
      "stream=sample_rate,channels", "-of", "csv=p=0", input.u8string()};
  const bool ok = run_process(command, [&](const char* data, size_t size) {
    text.append(data, size);
    return true;
  });
  return ok && sscanf(text.c_str(), "%d,%d", sample_rate, channels) == 2 && *sample_rate > 0 && *channels > 0;
}

std::string safe_stem(const fs::path& path) {
  std::string out;
  for (char c : path.stem().u8string()) {
    bool bad = strchr("<>:\"/\\|?*", c) != nullptr || (unsigned char)c < 32;
    out += bad ? '_' : c;
  }
  return out.empty() ? "track" : out;
}

}  // namespace

bool ffmpeg_available() {
  auto ignore = [](const char*, size_t) { return true; };
  return run_process({"ffmpeg", "-version"}, ignore) && run_process({"ffprobe", "-version"}, ignore);
}

bool looks_like_video(const fs::path& path) {
  std::string extension = path.extension().u8string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  for (const char* video : {".mp4", ".mov", ".m4v", ".mkv", ".webm", ".avi", ".mts", ".m2ts", ".wmv"}) {
    if (extension == video) return true;
  }
  return false;
}

bool extract_audio(const fs::path& input, const fs::path& output_wav, std::string* err) {
  const std::string name = input.filename().u8string();
  int sample_rate = 0;
  int channels = 0;
  if (!probe_audio(input, &sample_rate, &channels)) {
    if (err) *err = "Could not find any audio in " + name;
    return false;
  }

  WavWriter wav;
  if (!wav.open(output_wav, sample_rate, channels, err)) return false;

  const std::vector<std::string> command = {
      "ffmpeg", "-v", "error", "-nostdin", "-i", input.u8string(), "-map", "0:a:0",
      "-af", "aresample=async=1:first_pts=0", "-f", "f32le", "-acodec", "pcm_f32le", "-"};
  // Reads can split a frame, so hold on to any partial one for the next read.
  std::vector<char> pending;
  const size_t frame_bytes = size_t(channels) * sizeof(float);
  std::vector<float> aligned;
  const bool ok = run_process(command, [&](const char* data, size_t size) {
    pending.insert(pending.end(), data, data + size);
    const size_t frames = pending.size() / frame_bytes;
    if (frames == 0) return true;
    aligned.resize(frames * channels);
    memcpy(aligned.data(), pending.data(), frames * frame_bytes);
    pending.erase(pending.begin(), pending.begin() + frames * frame_bytes);
    return wav.write(aligned.data(), frames);
  });
  if (!wav.close() || !ok) {
    if (err) *err = "ffmpeg could not read the audio in " + name;
    std::error_code ec;
    fs::remove(output_wav, ec);
    return false;
  }
  if (wav.frames() == 0) {
    if (err) *err = "The audio in " + name + " is empty";
    std::error_code ec;
    fs::remove(output_wav, ec);
    return false;
  }
  return true;
}

bool import_files(const std::vector<fs::path>& inputs, fs::path* out_dir,
                  std::atomic<float>* progress, std::string* err) {
  if (inputs.empty()) {
    if (err) *err = "No files to import";
    return false;
  }
  if (!ffmpeg_available()) {
    if (err) *err = "Importing needs ffmpeg, which wasn't found. Install it from ffmpeg.org and make sure it's on your PATH.";
    return false;
  }

  time_t now = time(nullptr);
  tm local;
  local_time(&now, &local);
  char stamp[64];
  strftime(stamp, sizeof(stamp), "import_%Y-%m-%d_%H-%M-%S", &local);
  const fs::path dir = fs::temp_directory_path() / "record-align" / stamp;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    if (err) *err = "Could not create " + dir.u8string() + ": " + ec.message();
    return false;
  }

  SessionEdits edits;
  FILE* sources = open_file((dir / "sources.txt").c_str(), "w");
  for (size_t i = 0; i < inputs.size(); i++) {
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%02zu_", i + 1);
    const std::string stem = prefix + safe_stem(inputs[i]);
    if (!extract_audio(inputs[i], dir / fs::u8path(stem + ".wav"), err)) {
      if (sources) fclose(sources);
      fs::remove_all(dir, ec);
      return false;
    }
    if (sources) fprintf(sources, "%s <- %s\n", stem.c_str(), inputs[i].u8string().c_str());
    if (edits.master_name.empty() && looks_like_video(inputs[i])) edits.master_name = stem;
    TrackEdit track;
    track.name = stem;
    edits.tracks.push_back(std::move(track));
    if (progress) progress->store(float(i + 1) / inputs.size());
  }
  if (sources) fclose(sources);
  if (edits.master_name.empty()) edits.master_name = edits.tracks[0].name;
  save_edits(dir, edits, nullptr);
  *out_dir = dir;
  return true;
}
