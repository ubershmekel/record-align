#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

// Turns existing video and audio files into a session the editor can open, using
// ffmpeg to pull out the audio. The originals are only read.

// True if ffmpeg and ffprobe can be run.
bool ffmpeg_available();

// True for file types that normally carry a picture, so one of them is the best
// choice of master when laying audio under a video.
bool looks_like_video(const std::filesystem::path& path);

// Writes the first audio stream of input to a 32-bit float WAV at its own sample
// rate. Silence is added at the front if the stream starts after the container's
// time zero, so the WAV's timeline matches the video's.
bool extract_audio(const std::filesystem::path& input, const std::filesystem::path& output_wav, std::string* err);

// Creates a new session folder in the system temp directory, with one WAV per input
// and a note of where each came from. Sets out_dir and picks the master: the first
// video, or else the first file.
bool import_files(const std::vector<std::filesystem::path>& inputs,
                  std::filesystem::path* out_dir, std::atomic<float>* progress, std::string* err);
