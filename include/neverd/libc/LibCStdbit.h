#ifndef NEVERD_LIBC_LIBCSTDBIT_H
#define NEVERD_LIBC_LIBCSTDBIT_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// C23 stdbit.h — bit manipulation utilities
inline constexpr std::string_view kStdbitHeader = "stdbit.h";

inline constexpr std::array kStdbitFunctions = {
    // bit_ceil
    "stdc_bit_ceil_uc",
    "stdc_bit_ceil_ui",
    "stdc_bit_ceil_ul",
    "stdc_bit_ceil_ull",
    "stdc_bit_ceil_us",
    // bit_floor
    "stdc_bit_floor_uc",
    "stdc_bit_floor_ui",
    "stdc_bit_floor_ul",
    "stdc_bit_floor_ull",
    "stdc_bit_floor_us",
    // bit_width
    "stdc_bit_width_uc",
    "stdc_bit_width_ui",
    "stdc_bit_width_ul",
    "stdc_bit_width_ull",
    "stdc_bit_width_us",
    // count_ones (popcount)
    "stdc_count_ones_uc",
    "stdc_count_ones_ui",
    "stdc_count_ones_ul",
    "stdc_count_ones_ull",
    "stdc_count_ones_us",
    // count_zeros
    "stdc_count_zeros_uc",
    "stdc_count_zeros_ui",
    "stdc_count_zeros_ul",
    "stdc_count_zeros_ull",
    "stdc_count_zeros_us",
    // first_leading_one
    "stdc_first_leading_one_uc",
    "stdc_first_leading_one_ui",
    "stdc_first_leading_one_ul",
    "stdc_first_leading_one_ull",
    "stdc_first_leading_one_us",
    // first_leading_zero
    "stdc_first_leading_zero_uc",
    "stdc_first_leading_zero_ui",
    "stdc_first_leading_zero_ul",
    "stdc_first_leading_zero_ull",
    "stdc_first_leading_zero_us",
    // first_trailing_one
    "stdc_first_trailing_one_uc",
    "stdc_first_trailing_one_ui",
    "stdc_first_trailing_one_ul",
    "stdc_first_trailing_one_ull",
    "stdc_first_trailing_one_us",
    // first_trailing_zero
    "stdc_first_trailing_zero_uc",
    "stdc_first_trailing_zero_ui",
    "stdc_first_trailing_zero_ul",
    "stdc_first_trailing_zero_ull",
    "stdc_first_trailing_zero_us",
    // has_single_bit
    "stdc_has_single_bit_uc",
    "stdc_has_single_bit_ui",
    "stdc_has_single_bit_ul",
    "stdc_has_single_bit_ull",
    "stdc_has_single_bit_us",
    // leading_ones
    "stdc_leading_ones_uc",
    "stdc_leading_ones_ui",
    "stdc_leading_ones_ul",
    "stdc_leading_ones_ull",
    "stdc_leading_ones_us",
    // leading_zeros
    "stdc_leading_zeros_uc",
    "stdc_leading_zeros_ui",
    "stdc_leading_zeros_ul",
    "stdc_leading_zeros_ull",
    "stdc_leading_zeros_us",
    // trailing_ones
    "stdc_trailing_ones_uc",
    "stdc_trailing_ones_ui",
    "stdc_trailing_ones_ul",
    "stdc_trailing_ones_ull",
    "stdc_trailing_ones_us",
    // trailing_zeros
    "stdc_trailing_zeros_uc",
    "stdc_trailing_zeros_ui",
    "stdc_trailing_zeros_ul",
    "stdc_trailing_zeros_ull",
    "stdc_trailing_zeros_us",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTDBIT_H
