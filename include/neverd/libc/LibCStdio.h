#ifndef NEVERD_LIBC_LIBCSTDIO_H
#define NEVERD_LIBC_LIBCSTDIO_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C stdio.h — formatted I/O, streams, file operations
inline constexpr std::string_view kStdioHeader = "stdio.h";

inline constexpr std::array kStdioFunctions = {
    "asprintf",
    "clearerr",
    "clearerr_unlocked",
    "dprintf",
    "fclose",
    "fdopen",
    "feof",
    "feof_unlocked",
    "ferror",
    "ferror_unlocked",
    "fflush",
    "fgetc",
    "fgetc_unlocked",
    "fgetpos",
    "fgets",
    "fileno",
    "flockfile",
    "fopen",
    "fopencookie",
    "fprintf",
    "fputc",
    "fputs",
    "fread",
    "fread_unlocked",
    "freopen",
    "fscanf",
    "fseek",
    "fseeko",
    "fsetpos",
    "ftell",
    "ftello",
    "ftrylockfile",
    "funlockfile",
    "fwrite",
    "fwrite_unlocked",
    "getc",
    "getc_unlocked",
    "getchar",
    "getchar_unlocked",
    "gets",
    "perror",
    "printf",
    "putc",
    "putchar",
    "puts",
    "remove",
    "rename",
    "rewind",
    "scanf",
    "setbuf",
    "setbuffer",
    "setlinebuf",
    "setvbuf",
    "snprintf",
    "sprintf",
    "sscanf",
    "tmpfile",
    "tmpnam",
    "ungetc",
    "vasprintf",
    "vdprintf",
    "vfprintf",
    "vfscanf",
    "vprintf",
    "vscanf",
    "vsnprintf",
    "vsprintf",
    "vsscanf",
};

/// Fixed arity of the non-variadic stdio.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kStdioArity = std::to_array<LibCArityEntry>({
    {"puts", {1, 0}},    {"fputs", {2, 0}},    {"fputc", {2, 0}},
    {"putc", {2, 0}},    {"putchar", {1, 0}},  {"fgetc", {1, 0}},
    {"getc", {1, 0}},    {"getchar", {0, 0}},  {"fgets", {3, 0}},
    {"fopen", {2, 0}},   {"freopen", {3, 0}},  {"fdopen", {2, 0}},
    {"fclose", {1, 0}},  {"fflush", {1, 0}},   {"fread", {4, 0}},
    {"fwrite", {4, 0}},  {"fseek", {3, 0}},    {"fseeko", {3, 0}},
    {"ftell", {1, 0}},   {"ftello", {1, 0}},   {"rewind", {1, 0}},
    {"fgetpos", {2, 0}}, {"fsetpos", {2, 0}},  {"feof", {1, 0}},
    {"ferror", {1, 0}},  {"clearerr", {1, 0}}, {"fileno", {1, 0}},
    {"remove", {1, 0}},  {"rename", {2, 0}},   {"perror", {1, 0}},
    {"setbuf", {2, 0}},  {"setvbuf", {4, 0}},  {"ungetc", {2, 0}},
    {"tmpfile", {0, 0}}, {"popen", {2, 0}},    {"pclose", {1, 0}},
    {"putw", {2, 0}},    {"getw", {1, 0}},
    {"vdprintf", {2, 0}}, {"vfprintf", {3, 0}}, {"vfscanf", {2, 0}},
    {"vprintf", {2, 0}},  {"vscanf", {1, 0}},   {"vsnprintf", {3, 0}},
    {"vsprintf", {2, 0}}, {"vsscanf", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTDIO_H
