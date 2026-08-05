#ifndef NEVERD_LIBC_LIBCSPAWN_H
#define NEVERD_LIBC_LIBCSPAWN_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX spawn.h — process creation
inline constexpr std::string_view kSpawnHeader = "spawn.h";

inline constexpr std::array kSpawnFunctions = {
    "posix_spawn",
    "posix_spawn_file_actions_addclose",
    "posix_spawn_file_actions_adddup2",
    "posix_spawn_file_actions_addopen",
    "posix_spawn_file_actions_destroy",
    "posix_spawn_file_actions_init",
    "posix_spawnattr_destroy",
    "posix_spawnattr_getflags",
    "posix_spawnattr_getpgroup",
    "posix_spawnattr_getsigdefault",
    "posix_spawnattr_getsigmask",
    "posix_spawnattr_init",
    "posix_spawnattr_setflags",
    "posix_spawnattr_setpgroup",
    "posix_spawnattr_setsigdefault",
    "posix_spawnattr_setsigmask",
    "posix_spawnp",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSPAWN_H
