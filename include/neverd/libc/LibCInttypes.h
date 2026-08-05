#ifndef NEVERD_LIBC_LIBCINTTYPES_H
#define NEVERD_LIBC_LIBCINTTYPES_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C inttypes.h — integer-type conversions
inline constexpr std::string_view kInttypesHeader = "inttypes.h";

inline constexpr std::array kInttypesFunctions = {
    "imaxabs",
    "imaxdiv",
    "strtoimax",
    "strtoumax",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCINTTYPES_H
