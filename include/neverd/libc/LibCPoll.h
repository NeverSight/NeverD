#ifndef NEVERD_LIBC_LIBCPOLL_H
#define NEVERD_LIBC_LIBCPOLL_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX poll.h — I/O multiplexing
inline constexpr std::string_view kPollHeader = "poll.h";

inline constexpr std::array kPollFunctions = {
    "poll",
    "ppoll",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCPOLL_H
