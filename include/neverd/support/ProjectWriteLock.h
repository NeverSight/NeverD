//===- ProjectWriteLock.h - Cross-process sidecar ownership ------*- C++
//-*-===//
#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace neverd {
// Writers use the same canonical input path, including short-lived CLI writes.
// The lock file stays in place: removing it could split ownership across
// inodes.
class ProjectWriteLock {
public:
  explicit ProjectWriteLock(const std::filesystem::path &Binary) {
    std::error_code EC;
    auto Path = std::filesystem::canonical(Binary, EC);
    if (EC) {
      Error = EC.message();
      return;
    }
    Path += ".neverd-gui.lock";
#ifdef _WIN32
    Handle = CreateFileW(Path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    OVERLAPPED Overlap{};
    if (Handle == INVALID_HANDLE_VALUE ||
        !LockFileEx(Handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &Overlap)) {
      Error = std::error_code(GetLastError(), std::system_category()).message();
      if (Handle != INVALID_HANDLE_VALUE)
        CloseHandle(Handle);
      Handle = INVALID_HANDLE_VALUE;
    }
#else
    Descriptor = ::open(Path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (Descriptor < 0 || ::flock(Descriptor, LOCK_EX | LOCK_NB) != 0) {
      Error = std::error_code(errno, std::generic_category()).message();
      if (Descriptor >= 0)
        ::close(Descriptor);
      Descriptor = -1;
    }
#endif
  }
  ~ProjectWriteLock() {
#ifdef _WIN32
    if (Handle != INVALID_HANDLE_VALUE)
      CloseHandle(Handle);
#else
    if (Descriptor >= 0)
      ::close(Descriptor);
#endif
  }
  ProjectWriteLock(const ProjectWriteLock &) = delete;
  ProjectWriteLock &operator=(const ProjectWriteLock &) = delete;
  explicit operator bool() const { return Error.empty(); }
  const std::string &error() const { return Error; }

private:
  std::string Error;
#ifdef _WIN32
  HANDLE Handle = INVALID_HANDLE_VALUE;
#else
  int Descriptor = -1;
#endif
};
} // namespace neverd
