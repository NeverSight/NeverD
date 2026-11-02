/* Independently authored AAPCS64 callers. No Android headers or host libc. */
typedef unsigned long size_t;
typedef unsigned long u64;
typedef __builtin_va_list va_list;
extern int snprintf(char *, size_t, const char *, ...);
extern int vsnprintf(char *, size_t, const char *, va_list);
extern int sprintf(char *, const char *, ...);
extern int vsprintf(char *, const char *, va_list);
extern int __snprintf_chk(char *, size_t, int, size_t, const char *, ...);
extern int __vsnprintf_chk(char *, size_t, int, size_t, const char *, va_list);
extern int __sprintf_chk(char *, int, size_t, const char *, ...);
extern int __vsprintf_chk(char *, int, size_t, const char *, va_list);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int mprotect(void *, size_t, int);

__attribute__((noinline)) static int bounded(char *out, size_t n,
                                             const char *format, ...) {
  va_list original, copy;
  __builtin_va_start(original, format);
  __builtin_va_copy(copy, original);
  int first = vsnprintf(out, n, format, original);
  int second = vsnprintf(out + 256, n, format, copy);
  __builtin_va_end(copy);
  __builtin_va_end(original);
  return first == second ? first : -99;
}
__attribute__((noinline)) static int unbounded(char *out, const char *format,
                                               ...) {
  va_list original, copy;
  __builtin_va_start(original, format);
  __builtin_va_copy(copy, original);
  int first = vsprintf(out, format, original);
  int second = vsprintf(out + 256, format, copy);
  __builtin_va_end(copy);
  __builtin_va_end(original);
  return first == second ? first : -99;
}

__attribute__((noinline)) static int checked_copy(char *out, u64 mode,
                                                  const char *format, ...) {
  va_list original, copy;
  __builtin_va_start(original, format);
  __builtin_va_copy(copy, original);
  int first = mode == 5 ? __vsnprintf_chk(out, 240, 7, 240, format, original)
                        : __vsprintf_chk(out, 7, 240, format, original);
  int second = mode == 5
                   ? __vsnprintf_chk(out + 256, 240, -1, 240, format, copy)
                   : __vsprintf_chk(out + 256, -1, 240, format, copy);
  __builtin_va_end(copy);
  __builtin_va_end(original);
  return first == second ? first : -99;
}

u64 format_integers(char *out, u64 mode) {
  const char *format = "%+08d|%#018lx|%-7.3s|%hhd/%hhu/%hd/%hu|%lld/%llu|"
                       "%zu/%td/%jd|%c/%%|%p";
#define VALUES                                                                 \
  -123, 0xfedcba9876543210UL, "abcdef", -128, 255, -32768, 65535,              \
      (-9223372036854775807LL - 1), 18446744073709551615ULL, 4294967297UL,     \
      -4294967298L, -5L, 0x180, (void *)0x123456789abcdef0UL
  *__errno() = 77;
  int result;
  if (mode == 0)
    result = snprintf(out, 240, format, VALUES);
  else if (mode == 1)
    result = bounded(out, 240, format, VALUES);
  else if (mode == 2)
    result = sprintf(out, format, VALUES);
  else if (mode == 3)
    result = unbounded(out, format, VALUES);
  else if (mode == 4)
    result = __snprintf_chk(out, 240, 7, 240, format, VALUES);
  else if (mode == 6)
    result = __sprintf_chk(out, -1, 240, format, VALUES);
  else
    result = checked_copy(out, mode, format, VALUES);
#undef VALUES
  return *__errno() == 77 ? (unsigned)result : 999;
}

__attribute__((noinline)) static int stacked(char *out, size_t n,
                                             const char *format, u64 a, u64 b,
                                             u64 c, u64 d, u64 last, ...) {
  va_list args;
  __builtin_va_start(args, last);
  int result = vsnprintf(out, n, format, args);
  __builtin_va_end(args);
  return a + b + c + d + last == 15 ? result : -99;
}
u64 format_stacked(char *out) {
  return (unsigned)stacked(out, 128, "%d:%lu:%s:%*.*x", 1, 2, 3, 4, 5, -17,
                           0x100000002UL, "stack", -8, 4, 42);
}

u64 format_flags(char *out) {
  return (unsigned)snprintf(
      out, 240,
      "[%*.*d][%0*.*x][%#o][%#.0o][%#.0x][%+.0d][%.0p][%p][%.3s][%.*s]", -8, 5,
      23, 9, 4, 42, 9, 0, 0, 0, (void *)0, (void *)0, (char *)0, -1, "whole");
}
u64 format_supplied(char *out, u64 n, const char *format, u64 first, u64 second,
                    u64 third) {
  return (unsigned)snprintf(out, n, format, first, second, third);
}
u64 format_one_string(char *out, u64 n, const char *format, const char *value) {
  return (unsigned)snprintf(out, n, format, value);
}
u64 format_unsafe(char *out, u64 mode) {
  if (mode == 0)
    return (unsigned)snprintf(out, 64, "before:%f", 1.25);
  if (mode == 1)
    return (unsigned)snprintf(out, 64, "before:%ls", L"wide");
  if (mode == 2)
    return (unsigned)snprintf(out, 64, "before:%n", (int *)out);
  if (mode == 3)
    return (unsigned)snprintf(out, 64, "before:%2$d", 1, 2);
  if (mode == 4)
    return (unsigned)snprintf(out, 64, "before:%*2$d", 1, 2);
  if (mode == 5)
    return (unsigned)snprintf(out, 64, "before:%2147483648d", 1);
  if (mode == 6)
    return (unsigned)snprintf(out, 64, "%*d", (-2147483647 - 1), 1);
  if (mode == 7)
    return (unsigned)snprintf(out, 64, "%2147483647d!", 1);
  return (unsigned)snprintf(out, 0x100000002UL, "small");
}
u64 format_binary(char *out, u64 n) {
  return (unsigned)snprintf(out, n, "A%cB%sZ", 0, "\x80\xff");
}
u64 format_precision_page(char *out, char *text) {
  return (unsigned)snprintf(out, 16, "%.4s", text);
}
u64 format_readonly_tail(char *out) {
  if (mprotect(out + 2, 4096, 1))
    return 99;
  return (unsigned)snprintf(out, 4096, "%s", "x");
}
u64 format_dynamic(char *out, u64 closed) {
  void *handle = dlopen("libformat-model.so", 2);
  if (!handle)
    return 99;
  int (*call)(char *, size_t, const char *, ...) = dlsym(handle, "snprintf");
  if (!call)
    return 98;
  if (closed && dlclose(handle))
    return 97;
  int result = call(out, 64, "%s=%#lx", "symbol", 0x10000000aUL);
  if (!closed && dlclose(handle))
    return 96;
  return (unsigned)result;
}

/* These entries deliberately supply malformed or boundary va_list metadata.
 * The signature still uses the compiler's real by-value AAPCS64 struct ABI. */
struct saved_args {
  void *stack, *gr_top, *vr_top;
  int gr_offs, vr_offs;
};
_Static_assert(sizeof(va_list) == sizeof(struct saved_args), "AAPCS64 layout");
u64 format_explicit_va(char *out, const struct saved_args *saved,
                       const char *format) {
  va_list args;
  __builtin_memcpy(&args, saved, sizeof(args));
  return (unsigned)vsnprintf(out, 64, format, args);
}

__attribute__((noinline)) static int checked_va(char *out, u64 mode, size_t n,
                                                size_t object_size,
                                                const char *format, ...) {
  va_list args;
  __builtin_va_start(args, format);
  int result = mode == 1
                   ? __vsnprintf_chk(out, n, -1, object_size, format, args)
                   : __vsprintf_chk(out, 7, object_size, format, args);
  __builtin_va_end(args);
  return result;
}

u64 format_fortify_supplied(char *out, u64 mode, size_t n, size_t object_size,
                            const char *format, u64 first, u64 second,
                            u64 third) {
  *__errno() = 77;
  int result;
  if (mode == 0)
    result =
        __snprintf_chk(out, n, 7, object_size, format, first, second, third);
  else if (mode == 2)
    result = __sprintf_chk(out, -1, object_size, format, first, second, third);
  else
    result =
        checked_va(out, mode, n, object_size, format, first, second, third);
  return *__errno() == 77 ? (unsigned)result : 999;
}

u64 format_fortify_explicit_va(char *out, u64 mode, size_t n,
                               size_t object_size, const char *format,
                               const struct saved_args *saved) {
  va_list args;
  __builtin_memcpy(&args, saved, sizeof(args));
  return (unsigned)(mode == 1
                        ? __vsnprintf_chk(out, n, 7, object_size, format, args)
                        : __vsprintf_chk(out, -1, object_size, format, args));
}

__attribute__((noinline)) static int
checked_dynamic_va(void *call, char *out, u64 mode, size_t object_size,
                   const char *format, ...) {
  va_list args;
  __builtin_va_start(args, format);
  int result;
  if (mode == 1) {
    int (*bounded_call)(char *, size_t, int, size_t, const char *, va_list) =
        call;
    result = bounded_call(out, 64, 7, object_size, format, args);
  } else {
    int (*unbounded_call)(char *, int, size_t, const char *, va_list) = call;
    result = unbounded_call(out, -1, object_size, format, args);
  }
  __builtin_va_end(args);
  return result;
}

u64 format_fortify_dynamic(char *out, u64 mode, u64 closed,
                           size_t object_size) {
  const char *names[] = {"__snprintf_chk", "__vsnprintf_chk", "__sprintf_chk",
                         "__vsprintf_chk"};
  if (mode >= 4)
    return 100;
  void *handle = dlopen("libformat-model.so", 2);
  if (!handle)
    return 99;
  void *call = dlsym(handle, names[mode]);
  if (!call)
    return 98;
  if (closed && dlclose(handle))
    return 97;
  int result;
  if (mode == 0) {
    int (*bounded_call)(char *, size_t, int, size_t, const char *, ...) = call;
    result = bounded_call(out, 64, 7, object_size, "%s=%#lx", "symbol",
                          0x10000000aUL);
  } else if (mode == 2) {
    int (*unbounded_call)(char *, int, size_t, const char *, ...) = call;
    result = unbounded_call(out, -1, object_size, "%s=%#lx", "symbol",
                            0x10000000aUL);
  } else {
    result = checked_dynamic_va(call, out, mode, object_size, "%s=%#lx",
                                "symbol", 0x10000000aUL);
  }
  if (!closed && dlclose(handle))
    return 96;
  return (unsigned)result;
}
