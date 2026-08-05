#ifndef NEVERD_LIBC_ARPA_LIBCINET_H
#define NEVERD_LIBC_ARPA_LIBCINET_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX arpa/inet.h — internet address manipulation
inline constexpr std::string_view kArpaInetHeader = "arpa/inet.h";

inline constexpr std::array kArpaInetFunctions = {
    "htonl",     "htons",     "inet_addr", "inet_aton", "inet_ntoa",
    "inet_ntop", "inet_pton", "ntohl",     "ntohs",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_ARPA_LIBCINET_H
