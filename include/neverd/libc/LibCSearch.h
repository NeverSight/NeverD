#ifndef NEVERD_LIBC_LIBCSEARCH_H
#define NEVERD_LIBC_LIBCSEARCH_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX search.h — hash/tree/linear search
inline constexpr std::string_view kSearchHeader = "search.h";

inline constexpr std::array kSearchFunctions = {
    "hcreate", "hcreate_r", "hdestroy", "hdestroy_r", "hsearch",  "hsearch_r",
    "insque",  "lfind",     "lsearch",  "remque",     "tdestroy", "tdelete",
    "tfind",   "tsearch",   "twalk",    "twalk_r",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSEARCH_H
