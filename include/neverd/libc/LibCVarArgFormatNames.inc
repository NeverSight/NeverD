// Standalone printf/scanf family names whose fixed parameter count is not
// derivable from a longer suffix.  Included with LIBC_VARARG_FORMAT_NAME.

LIBC_VARARG_FORMAT_NAME("printf", 1)
LIBC_VARARG_FORMAT_NAME("scanf", 1)
LIBC_VARARG_FORMAT_NAME("wprintf", 1)
LIBC_VARARG_FORMAT_NAME("wscanf", 1)
// Microsoft's count-only formatters have no destination or capacity.
LIBC_VARARG_FORMAT_NAME("scprintf", 1)
LIBC_VARARG_FORMAT_NAME("scwprintf", 1)
// Darwin's libmalloc.
LIBC_VARARG_FORMAT_NAME("malloc_printf", 1)
