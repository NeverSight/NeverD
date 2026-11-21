// Irregular va_list-consuming functions: those with no printf/scanf suffix,
// and those whose name puts another word before the v (glibc's
// obstack_vprintf, Darwin's __svfscanf).  Included by LibCCallTraits.cpp
// with LIBC_VA_LIST_CONSUMER defined.

LIBC_VA_LIST_CONSUMER("vasprintf")
LIBC_VA_LIST_CONSUMER("vsyslog")
LIBC_VA_LIST_CONSUMER("verr")
LIBC_VA_LIST_CONSUMER("verrx")
LIBC_VA_LIST_CONSUMER("vwarn")
LIBC_VA_LIST_CONSUMER("vwarnx")
LIBC_VA_LIST_CONSUMER("obstack_vprintf")
LIBC_VA_LIST_CONSUMER("svfscanf")
LIBC_VA_LIST_CONSUMER("sqlite3_vmprintf")
LIBC_VA_LIST_CONSUMER("sqlite3_vsnprintf")
