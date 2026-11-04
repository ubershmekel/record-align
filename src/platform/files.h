#pragma once

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

inline FILE* open_file(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
  std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
  return _wfopen(path.c_str(), wide_mode.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

inline void local_time(const time_t* value, tm* result) {
#ifdef _WIN32
  localtime_s(result, value);
#else
  localtime_r(value, result);
#endif
}

inline void utc_time(const time_t* value, tm* result) {
#ifdef _WIN32
  gmtime_s(result, value);
#else
  gmtime_r(value, result);
#endif
}
