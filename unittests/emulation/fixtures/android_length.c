//===- android_length.c - Independent Bionic string-length callers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Freestanding API 28 callers for scan order, object bounds and providers.

typedef unsigned long u64;
extern u64 strlen(const char *);
extern u64 strnlen(const char *, u64);
extern u64 __strlen_chk(const char *, u64);
extern int *__errno(void);
extern int mprotect(void *, u64, int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

u64 length_raw(const char *text, u64 bound, u64 mode) {
  if (mode == 0)
    return strlen(text);
  if (mode == 1)
    return strnlen(text, bound);
  return __strlen_chk(text, bound);
}

u64 length_supplied(const char *text, u64 bound, u64 mode, unsigned *error) {
  *__errno() = 733;
  u64 result = length_raw(text, bound, mode);
  *error = *__errno();
  return result;
}

u64 length_protected(void *page, u64 size, int permissions, const char *text,
                     u64 bound, u64 mode) {
  if (mprotect(page, size, permissions))
    return 98;
  return length_raw(text, bound, mode);
}

u64 length_dynamic(const char *text, u64 bound, u64 closed) {
  void *library = dlopen("liblength-model.so", 2);
  if (!library)
    return 97;
  u64 (*length)(const char *, u64) = dlsym(library, "__strlen_chk");
  if (!length)
    return 96;
  if (closed && dlclose(library))
    return 95;
  u64 result = length(text, bound);
  if (dlclose(library))
    return 94;
  return result;
}
