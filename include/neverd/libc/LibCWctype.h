#ifndef NEVERD_LIBC_LIBCWCTYPE_H
#define NEVERD_LIBC_LIBCWCTYPE_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C wctype.h — wide-character classification
inline constexpr std::string_view kWctypeHeader = "wctype.h";

inline constexpr std::array kWctypeFunctions = {
    "iswalnum",  "iswalpha", "iswblank", "iswcntrl", "iswctype", "iswdigit",
    "iswgraph",  "iswlower", "iswprint", "iswpunct", "iswspace", "iswupper",
    "iswxdigit", "towlower", "towupper", "wctype",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCWCTYPE_H
