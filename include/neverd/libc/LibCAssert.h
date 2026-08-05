#ifndef NEVERD_LIBC_LIBCASSERT_H
#define NEVERD_LIBC_LIBCASSERT_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C assert.h — runtime assertions
inline constexpr std::string_view kAssertHeader = "assert.h";

inline constexpr std::array kAssertFunctions = {
    "__assert_fail",
    "__assert_rtn",
    "__assert",
    "assert",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCASSERT_H
