#ifndef NEVERD_LIBC_LIBCTERMIOS_H
#define NEVERD_LIBC_LIBCTERMIOS_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX termios.h — terminal I/O control
inline constexpr std::string_view kTermiosHeader = "termios.h";

inline constexpr std::array kTermiosFunctions = {
    "cfgetispeed", "cfgetospeed", "cfsetispeed", "cfsetospeed",
    "tcdrain",     "tcflow",      "tcflush",     "tcgetattr",
    "tcgetsid",    "tcsendbreak", "tcsetattr",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCTERMIOS_H
