//===- android_copy.c - Independent bounded-copy callers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Freestanding API 28 callers for destination bytes and pointer boundaries.

typedef unsigned long u64;
extern char *strcpy(char *, const char *);
__asm__(".type strcpy, %function");
extern char *__strcpy_chk(char *, const char *, u64);
__asm__(".type __strcpy_chk, %function");
extern char *__strcat_chk(char *, const char *, u64);
__asm__(".type __strcat_chk, %function");
extern char *__strncat_chk(char *, const char *, u64, u64);
__asm__(".type __strncat_chk, %function");
extern char *strncpy(char *, const char *, u64);
extern char *__strncpy_chk(char *, const char *, u64, u64);
extern char *__strncpy_chk2(char *, const char *, u64, u64, u64);
extern u64 strlcpy(char *, const char *, u64);
__asm__(".type strlcpy, %function");
extern u64 __strlcpy_chk(char *, const char *, u64, u64);
__asm__(".type __strlcpy_chk, %function");
extern int *__errno(void);
extern int mprotect(void *, u64, int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

u64 append_checked(char *dst, const char *src, u64 count, u64 dst_size,
                   u64 bounded, u64 route, u64 *error) {
  char *(*full)(char *, const char *, u64) = __strcat_chk;
  char *(*limited)(char *, const char *, u64, u64) = __strncat_chk;
  if (route) {
    void *library = dlopen("libappend-checked.so", 2);
    if (!library)
      return 97;
    void *function = dlsym(library, bounded ? "__strncat_chk" : "__strcat_chk");
    if (!function)
      return 96;
    if (bounded)
      limited = function;
    else
      full = function;
    if (route == 2 && dlclose(library))
      return 95;
  }
  *__errno() = 733;
  char *result =
      bounded ? limited(dst, src, count, dst_size) : full(dst, src, dst_size);
  *error = (u64)*__errno();
  return (u64)result;
}

u64 append_checked_readonly(void *page, char *dst, const char *src, u64 count,
                            u64 dst_size, u64 bounded) {
  if (mprotect(page, 4096, 1))
    return 98;
  return (u64)(bounded ? __strncat_chk(dst, src, count, dst_size)
                       : __strcat_chk(dst, src, dst_size));
}

u64 copy_terminated(char *dst, const char *src, u64 dst_size, u64 checked,
                    u64 route, u64 *error) {
  char *(*plain)(char *, const char *) = strcpy;
  char *(*fortified)(char *, const char *, u64) = __strcpy_chk;
  if (route) {
    void *library = dlopen("libcopy-terminated.so", 2);
    if (!library)
      return 97;
    void *function = dlsym(library, checked ? "__strcpy_chk" : "strcpy");
    if (!function)
      return 96;
    if (checked)
      fortified = function;
    else
      plain = function;
    if (route == 2 && dlclose(library))
      return 95;
  }
  *__errno() = 733;
  char *result = checked ? fortified(dst, src, dst_size) : plain(dst, src);
  *error = (u64)*__errno();
  return (u64)result;
}

u64 copy_terminated_readonly(void *page, char *dst, const char *src,
                             u64 dst_size, u64 checked) {
  if (mprotect(page, 4096, 1))
    return 98;
  return (u64)(checked ? __strcpy_chk(dst, src, dst_size) : strcpy(dst, src));
}

u64 copy_sized(char *dst, const char *src, u64 count, u64 route, u64 *error) {
  u64 (*copy)(char *, const char *, u64) = strlcpy;
  if (route) {
    void *library = dlopen("libcopy.so", 2);
    if (!library)
      return 97;
    copy = dlsym(library, "strlcpy");
    if (!copy)
      return 96;
    if (route == 2 && dlclose(library))
      return 95;
  }
  *__errno() = 733;
  u64 result = copy(dst, src, count);
  *error = (u64)*__errno();
  return result;
}
u64 copy_sized_readonly(void *page, char *dst, const char *src, u64 count) {
  if (mprotect(page, 4096, 1))
    return 98;
  return strlcpy(dst, src, count);
}

u64 copy_sized_checked(char *dst, const char *src, u64 count, u64 dst_size,
                       u64 route, u64 *error) {
  u64 (*copy)(char *, const char *, u64, u64) = __strlcpy_chk;
  if (route) {
    void *library = dlopen("libcopy-sized-checked.so", 2);
    if (!library)
      return 97;
    copy = dlsym(library, "__strlcpy_chk");
    if (!copy)
      return 96;
    if (route == 2 && dlclose(library))
      return 95;
  }
  *__errno() = 733;
  u64 result = copy(dst, src, count, dst_size);
  *error = (u64)*__errno();
  return result;
}

u64 copy_sized_checked_readonly(void *page, char *dst, const char *src,
                                u64 count, u64 dst_size) {
  if (mprotect(page, 4096, 1))
    return 98;
  return __strlcpy_chk(dst, src, count, dst_size);
}

u64 copy_raw(char *dst, const char *src, u64 count) {
  return (u64)strncpy(dst, src, count);
}
u64 copy_supplied(char *dst, const char *src, u64 count, unsigned *error) {
  *__errno() = 733;
  char *result = strncpy(dst, src, count);
  *error = *__errno();
  return (u64)result;
}
u64 copy_readonly(void *page, char *dst, const char *src, u64 count) {
  if (mprotect(page, 4096, 1))
    return 98;
  return (u64)strncpy(dst, src, count);
}
u64 copy_dynamic(char *dst, u64 closed) {
  void *library = dlopen("libcopy.so", 2);
  if (!library)
    return 97;
  char *(*copy)(char *, const char *, u64) = dlsym(library, "strncpy");
  if (!copy)
    return 96;
  if (closed && dlclose(library))
    return 95;
  char *result = copy(dst, "ab", 6);
  if (dlclose(library))
    return 94;
  return (u64)result;
}

u64 copy_checked(char *dst, const char *src, u64 count, u64 dst_size,
                 u64 src_size, u64 mode) {
  if (mode)
    return (u64)__strncpy_chk2(dst, src, count, dst_size, src_size);
  return (u64)__strncpy_chk(dst, src, count, dst_size);
}

u64 copy_checked_supplied(char *dst, const char *src, u64 count, u64 dst_size,
                          u64 src_size, u64 mode, unsigned *error) {
  *__errno() = 733;
  u64 result = copy_checked(dst, src, count, dst_size, src_size, mode);
  *error = *__errno();
  return result;
}

u64 copy_checked_readonly(void *page, char *dst, const char *src, u64 count,
                          u64 dst_size, u64 src_size, u64 mode) {
  if (mprotect(page, 4096, 1))
    return 98;
  return copy_checked(dst, src, count, dst_size, src_size, mode);
}

u64 copy_checked_dynamic(char *dst, const char *src, u64 count, u64 dst_size,
                         u64 src_size, u64 mode, u64 closed) {
  void *library = dlopen("libcopy-checked.so", 2);
  if (!library)
    return 97;
  void *function = dlsym(library, mode ? "__strncpy_chk2" : "__strncpy_chk");
  if (!function)
    return 96;
  if (closed && dlclose(library))
    return 95;
  char *result;
  if (mode) {
    char *(*copy)(char *, const char *, u64, u64, u64) = function;
    result = copy(dst, src, count, dst_size, src_size);
  } else {
    char *(*copy)(char *, const char *, u64, u64) = function;
    result = copy(dst, src, count, dst_size);
  }
  if (dlclose(library))
    return 94;
  return (u64)result;
}
