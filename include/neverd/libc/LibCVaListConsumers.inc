// Irregular va_list-consuming functions with no printf/scanf suffix.
// Included by LibCCallTraits.cpp with LIBC_VA_LIST_CONSUMER defined.

LIBC_VA_LIST_CONSUMER("vasprintf")
LIBC_VA_LIST_CONSUMER("vsyslog")
LIBC_VA_LIST_CONSUMER("verr")
LIBC_VA_LIST_CONSUMER("verrx")
LIBC_VA_LIST_CONSUMER("vwarn")
LIBC_VA_LIST_CONSUMER("vwarnx")
