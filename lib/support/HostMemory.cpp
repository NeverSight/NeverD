//===- HostMemory.cpp - The host's physical memory ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/support/HostMemory.h"

#ifdef _WIN32
#include "llvm/Support/Windows/WindowsSupport.h"
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#endif

namespace neverd {

std::optional<uint64_t> hostPhysicalMemoryBytes() {
#ifdef _WIN32
  MEMORYSTATUSEX Status{};
  Status.dwLength = sizeof(Status);
  if (!GlobalMemoryStatusEx(&Status) || !Status.ullTotalPhys)
    return std::nullopt;
  return static_cast<uint64_t>(Status.ullTotalPhys);
#elif defined(__APPLE__)
  uint64_t Bytes = 0;
  size_t Size = sizeof(Bytes);
  if (sysctlbyname("hw.memsize", &Bytes, &Size, nullptr, 0) != 0 || !Bytes)
    return std::nullopt;
  return Bytes;
#else
  const long Pages = sysconf(_SC_PHYS_PAGES);
  const long PageSize = sysconf(_SC_PAGESIZE);
  if (Pages <= 0 || PageSize <= 0)
    return std::nullopt;
  return static_cast<uint64_t>(Pages) * static_cast<uint64_t>(PageSize);
#endif
}

} // namespace neverd
