#ifndef NEVERD_LIBC_LIBCLINK_H
#define NEVERD_LIBC_LIBCLINK_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// GNU/POSIX link.h — ELF program header iteration
inline constexpr std::string_view kLinkHeader = "link.h";

inline constexpr std::array kLinkFunctions = {
    "dl_iterate_phdr",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCLINK_H
