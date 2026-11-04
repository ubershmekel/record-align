#include "platform/process.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {
#ifdef _WIN32
std::wstring quote(const std::string& argument) {
  int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument.data(), int(argument.size()), nullptr, 0);
  std::wstring wide(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument.data(), int(argument.size()), wide.data(), length);
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t c : wide) {
    if (c == L'\\') { ++slashes; continue; }
    result.append(slashes * (c == L'"' ? 2 : 1) + (c == L'"' ? 1 : 0), L'\\');
    slashes = 0;
    result += c;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}
#else
std::string executable(const std::string& name) {
  // Finder does not inherit the user's shell PATH. Also check standard Homebrew paths.
  if (name == "ffmpeg" || name == "ffprobe") {
    for (const char* prefix : {"/opt/homebrew/bin/", "/usr/local/bin/"}) {
      std::string path = prefix + name;
      if (access(path.c_str(), X_OK) == 0) return path;
    }
  }
  return name;
}
#endif
}  // namespace

bool run_process(const std::vector<std::string>& arguments,
                 const std::function<bool(const char*, size_t)>& on_data) {
  if (arguments.empty()) return false;
#ifdef _WIN32
  std::wstring command;
  for (const auto& argument : arguments) {
    if (!command.empty()) command += L' ';
    command += quote(argument);
  }
  SECURITY_ATTRIBUTES inherit = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &inherit, 0)) return false;
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &inherit, OPEN_EXISTING, 0, nullptr);
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nul;
  startup.hStdOutput = write_end;
  startup.hStdError = nul;
  PROCESS_INFORMATION process = {};
  BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                nullptr, nullptr, &startup, &process);
  CloseHandle(write_end);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!started) { CloseHandle(read_end); return false; }
  std::vector<char> buffer(1 << 20);
  bool ok = true;
  DWORD count = 0;
  while (ReadFile(read_end, buffer.data(), DWORD(buffer.size()), &count, nullptr) && count) {
    if (ok) ok = on_data(buffer.data(), count);
  }
  CloseHandle(read_end);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return ok && code == 0;
#else
  int ends[2];
  if (pipe(ends) != 0) return false;
  fcntl(ends[0], F_SETFD, FD_CLOEXEC);
  fcntl(ends[1], F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addclose(&actions, ends[0]);
  posix_spawn_file_actions_addclose(&actions, ends[1]);
  std::vector<std::string> args = arguments;
  args[0] = executable(args[0]);
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  pid_t child;
  int started = posix_spawnp(&child, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(ends[1]);
  if (started != 0) { close(ends[0]); return false; }
  std::vector<char> buffer(1 << 20);
  bool ok = true;
  for (;;) {
    ssize_t count = read(ends[0], buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { if (count < 0) ok = false; break; }
    if (ok) ok = on_data(buffer.data(), size_t(count));
  }
  close(ends[0]);
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  return ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}
