#ifndef NEVERD_LIBC_SYS_LIBCSEM_H
#define NEVERD_LIBC_SYS_LIBCSEM_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/sem.h — System V semaphores
inline constexpr std::string_view kSysSemHeader = "sys/sem.h";

inline constexpr std::array kSysSemFunctions = {
    "semctl",
    "semget",
    "semop",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSEM_H
