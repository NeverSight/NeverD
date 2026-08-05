#ifndef NEVERD_LIBC_SYS_LIBCMMAN_H
#define NEVERD_LIBC_SYS_LIBCMMAN_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/mman.h — memory management
inline constexpr std::string_view kSysMmanHeader = "sys/mman.h";

inline constexpr std::array kSysMmanFunctions = {
    "madvise", "memfd_create",  "mincore",  "mlock",
    "mlock2",  "mlockall",      "mmap",     "mprotect",
    "mremap",  "msync",         "munlock",  "munlockall",
    "munmap",  "posix_madvise", "shm_open", "shm_unlink",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCMMAN_H
