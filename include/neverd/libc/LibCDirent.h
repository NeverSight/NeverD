#ifndef NEVERD_LIBC_LIBCDIRENT_H
#define NEVERD_LIBC_LIBCDIRENT_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX dirent.h — directory operations
inline constexpr std::string_view kDirentHeader = "dirent.h";

inline constexpr std::array kDirentFunctions = {
    "alphasort", "closedir",  "dirfd",   "fdopendir", "opendir",
    "readdir",   "readdir_r", "scandir", "seekdir",   "telldir",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCDIRENT_H
