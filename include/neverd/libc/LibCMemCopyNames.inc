// Memory-copy libc symbols recognised by jump-table init detection.
// Included with LIBC_MEM_COPY_SYMBOL defined; entries are compared after
// stripLeadingUnderscores() (Mach-O `_` prefix and glibc `__` both normalize).

LIBC_MEM_COPY_SYMBOL("memcpy")
LIBC_MEM_COPY_SYMBOL("memmove")
LIBC_MEM_COPY_SYMBOL("memcpy_chk")
LIBC_MEM_COPY_SYMBOL("memmove_chk")
