#ifndef NEVERD_LIBC_LIBCNLTYPES_H
#define NEVERD_LIBC_LIBCNLTYPES_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX nl_types.h — message catalogue
inline constexpr std::string_view kNlTypesHeader = "nl_types.h";

inline constexpr std::array kNlTypesFunctions = {
    "catclose",
    "catgets",
    "catopen",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCNLTYPES_H
