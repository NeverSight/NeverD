#ifndef NEVERD_LIBC_LIBCSTDFIX_H
#define NEVERD_LIBC_LIBCSTDFIX_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// C23 stdfix.h — fixed-point arithmetic
inline constexpr std::string_view kStdfixHeader = "stdfix.h";

inline constexpr std::array kStdfixFunctions = {
    // absolute value
    "abshk",
    "abshr",
    "absk",
    "abslk",
    "abslr",
    "absr",
    // exponential
    "exphk",
    "expk",
    // to-bits conversion
    "hrbits",
    "uhrbits",
    "rbits",
    "urbits",
    "lrbits",
    "ulrbits",
    "hkbits",
    "uhkbits",
    "kbits",
    "ukbits",
    "lkbits",
    "ulkbits",
    // from-bits conversion
    "bitshr",
    "bitsuhr",
    "bitsr",
    "bitsur",
    "bitslr",
    "bitsulr",
    "bitshk",
    "bitsuhk",
    "bitsk",
    "bitsuk",
    "bitslk",
    "bitsulk",
    // integer division
    "idivr",
    "idivlr",
    "idivk",
    "idivlk",
    "idivur",
    "idivulr",
    "idivuk",
    "idivulk",
    // round
    "roundhk",
    "roundhr",
    "roundk",
    "roundlk",
    "roundlr",
    "roundr",
    "rounduhk",
    "rounduhr",
    "rounduk",
    "roundulk",
    "roundulr",
    "roundur",
    // sqrt
    "sqrtuhk",
    "sqrtuhr",
    "sqrtuk",
    "sqrtulk",
    "sqrtulr",
    "sqrtur",
    "uhksqrtus",
    "uksqrtui",
    // count leading shifts
    "countlshr",
    "countlsr",
    "countlslr",
    "countlshk",
    "countlsk",
    "countlslk",
    "countlsuhr",
    "countlsur",
    "countlsulr",
    "countlsuhk",
    "countlsuk",
    "countlsulk",
    // misc
    "rdivi",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTDFIX_H
