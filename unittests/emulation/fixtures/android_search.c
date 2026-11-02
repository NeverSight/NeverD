/* Independent character-search callers; no libc implementation is embedded. */
typedef unsigned long u64;
extern char *strchr(const char *, int);
extern char *strrchr(const char *, int);
extern char *__strchr_chk(const char *, int, u64);
extern char *__strrchr_chk(const char *, int, u64);
extern int *__errno(void);
extern int mprotect(void *, u64, int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

extern void *memchr(const void *, int, u64);
__asm__(".type memchr, %function");
extern void *__memchr_chk(const void *, int, u64, u64);
__asm__(".type __memchr_chk, %function");

u64 memory_search(const void *input, u64 character, u64 count, u64 extent,
                  u64 checked, u64 route, u64 *error) {
  void *(*plain)(const void *, int, u64) = memchr;
  void *(*bounded)(const void *, int, u64, u64) = __memchr_chk;
  if (route) {
    void *library = dlopen("libmemory-search.so", 2);
    if (!library)
      return 97;
    void *function = dlsym(library, checked ? "__memchr_chk" : "memchr");
    if (!function)
      return 96;
    if (checked)
      bounded = function;
    else
      plain = function;
    if (route == 2 && dlclose(library))
      return 95;
  }
  *__errno() = 733;
  void *result = checked ? bounded(input, (int)character, count, extent)
                         : plain(input, (int)character, count);
  *error = (u64)*__errno();
  return (u64)result;
}

u64 memory_search_readonly(void *page, const void *input, u64 character,
                           u64 count, u64 extent, u64 checked) {
  if (mprotect(page, 4096, 1))
    return 98;
  return (u64)(checked ? __memchr_chk(input, (int)character, count, extent)
                       : memchr(input, (int)character, count));
}

u64 search_sequence(u64 *out) {
  const char text[] = {'a', (char)0x80, ':', 'b', ':', (char)0xff, 0};
  *__errno() = 733;
  out[0] = strchr(text, ':') - text;
  out[1] = strrchr(text, ':') - text;
  out[2] = __strchr_chk(text, ':', 3) - text;
  out[3] = __strrchr_chk(text, ':', sizeof(text)) - text;
  out[4] = strchr(text, 0x180) - text;
  out[5] = strrchr(text, -1) - text;
  out[6] = __strchr_chk(text, 0, sizeof(text)) - text;
  out[7] = __strrchr_chk(text, 0, sizeof(text)) - text;
  out[8] = (u64)strchr(text, 'z');
  out[9] = (u64)__strchr_chk(text, 'z', sizeof(text));
  out[10] = (u64)strrchr(text, 'z');
  out[11] = (u64)__strrchr_chk(text, 'z', sizeof(text));
  out[12] = __strchr_chk(text, ':', ~(u64)0) - text;
  out[13] = __strrchr_chk(text, ':', 0x100000007UL) - text;
  out[14] = *__errno();
  const volatile unsigned char *bytes = (const unsigned char *)text;
  out[15] = bytes[0] == 'a' && bytes[1] == 0x80 && bytes[2] == ':' &&
            bytes[3] == 'b' && bytes[4] == ':' && bytes[5] == 0xff && !bytes[6];
  return 0;
}

u64 search_supplied(const char *input, int character, u64 bound, u64 mode) {
  switch (mode) {
  case 0:
    return (u64)strchr(input, character);
  case 1:
    return (u64)strrchr(input, character);
  case 2:
    return (u64)__strchr_chk(input, character, bound);
  case 3:
    return (u64)__strrchr_chk(input, character, bound);
  default:
    return 99;
  }
}

u64 search_readonly(void *page, const char *input, int character, u64 bound,
                    u64 mode) {
  if (mprotect(page, 4096, 1))
    return 98;
  return search_supplied(input, character, bound, mode);
}

u64 search_dynamic(u64 *out, u64 close_first, u64 reverse) {
  const char text[] = "left:right:tail";
  void *library = dlopen("libsearch.so", 2);
  if (!library)
    return 97;
  char *(*find)(const char *, int, u64) =
      dlsym(library, reverse ? "__strrchr_chk" : "__strchr_chk");
  if (!find)
    return 96;
  if (close_first && dlclose(library))
    return 95;
  *__errno() = 733;
  out[0] = find(text, ':', sizeof(text)) - text;
  out[1] = find(text, 0, sizeof(text)) - text;
  out[2] = *__errno();
  return (unsigned int)dlclose(library);
}

extern int atoi(const char *);
extern long atol(const char *);
extern long long atoll(const char *);
u64 decimal_supplied(const char *input, u64 mode, unsigned *saved_errno) {
  *__errno() = 733;
  u64 result = mode == 0   ? (u64)(long)atoi(input)
               : mode == 1 ? (u64)atol(input)
                           : (u64)atoll(input);
  *saved_errno = *__errno();
  return result;
}
u64 decimal_dynamic(const char *input, u64 closed) {
  void *library = dlopen("libsearch.so", 2);
  if (!library)
    return 97;
  int (*convert)(const char *) = dlsym(library, "atoi");
  if (!convert)
    return 96;
  if (closed && dlclose(library))
    return 95;
  return (u64)(long)convert(input);
}

extern char *strstr(const char *, const char *);
extern void *memmem(const void *, u64, const void *, u64);
__asm__(".type memmem, %function");
u64 binary_substring(const void *text, u64 count, const void *needle,
                     u64 length, u64 route, u64 *error) {
  void *(*find)(const void *, u64, const void *, u64) = memmem;
  if (route) {
    void *library = dlopen("libsearch.so", 2);
    if (!library)
      return 97;
    find = dlsym(library, "memmem");
    if (!find)
      return 96;
    if (route == 2 && dlclose(library))
      return 95;
  }
  *__errno() = 733;
  void *result = find(text, count, needle, length);
  *error = (u64)*__errno();
  return (u64)result;
}
u64 binary_substring_readonly(void *page, const void *text, u64 count,
                              const void *needle, u64 length) {
  if (mprotect(page, 4096, 1))
    return 98;
  return (u64)memmem(text, count, needle, length);
}

u64 substring_supplied(const char *text, const char *needle, unsigned *error) {
  *__errno() = 733;
  char *result = strstr(text, needle);
  *error = *__errno();
  return (u64)result;
}
u64 substring_dynamic(const char *text, const char *needle, u64 closed) {
  void *library = dlopen("libsearch.so", 2);
  if (!library)
    return 97;
  char *(*find)(const char *, const char *) = dlsym(library, "strstr");
  if (!find)
    return 96;
  if (closed && dlclose(library))
    return 95;
  return (u64)find(text, needle);
}

extern char *strcasestr(const char *, const char *);
__asm__(".type strcasestr, %function");
u64 case_substring(const char *text, const char *needle, u64 route,
                   u64 *error) {
  char *(*find)(const char *, const char *) = strcasestr;
  if (route) {
    void *library = dlopen("libsearch.so", 2);
    if (!library)
      return 97;
    find = dlsym(library, "strcasestr");
    if (!find)
      return 96;
    if (route == 2 && dlclose(library))
      return 95;
  }
  *__errno() = 733;
  char *result = find(text, needle);
  *error = (u64)*__errno();
  return (u64)result;
}
u64 case_substring_readonly(void *page, const char *text, const char *needle) {
  if (mprotect(page, 4096, 1))
    return 98;
  return (u64)strcasestr(text, needle);
}

extern u64 strspn(const char *, const char *);
extern char *strpbrk(const char *, const char *);
__asm__(".type strpbrk, %function");
u64 member_search(const char *text, const char *accept, u64 route, u64 *error) {
  char *(*find)(const char *, const char *) = strpbrk;
  if (route) {
    void *library = dlopen("libsearch.so", 2);
    if (!library)
      return 97;
    find = dlsym(library, "strpbrk");
    if (!find)
      return 96;
    if (route == 2 && dlclose(library))
      return 95;
  }
  *__errno() = 733;
  char *result = find(text, accept);
  *error = (u64)*__errno();
  return (u64)result;
}
u64 member_search_readonly(void *page, const char *text, const char *accept) {
  if (mprotect(page, 4096, 1))
    return 98;
  return (u64)strpbrk(text, accept);
}

u64 span_supplied(const char *text, const char *accept, unsigned *error) {
  *__errno() = 733;
  u64 result = strspn(text, accept);
  *error = *__errno();
  return result;
}
u64 span_readonly(void *page, const char *text, const char *accept) {
  if (mprotect(page, 4096, 1))
    return 98;
  return strspn(text, accept);
}
u64 span_dynamic(const char *text, const char *accept, u64 closed) {
  void *library = dlopen("libsearch.so", 2);
  if (!library)
    return 97;
  u64 (*span)(const char *, const char *) = dlsym(library, "strspn");
  if (!span)
    return 96;
  if (closed && dlclose(library))
    return 95;
  return span(text, accept);
}

extern u64 strcspn(const char *, const char *);
u64 reject_span_supplied(const char *text, const char *reject,
                         unsigned *error) {
  *__errno() = 733;
  u64 result = strcspn(text, reject);
  *error = *__errno();
  return result;
}
u64 reject_span_readonly(void *page, const char *text, const char *reject) {
  if (mprotect(page, 4096, 1))
    return 98;
  return strcspn(text, reject);
}
u64 reject_span_dynamic(const char *text, const char *reject, u64 closed) {
  void *library = dlopen("libsearch.so", 2);
  if (!library)
    return 97;
  u64 (*span)(const char *, const char *) = dlsym(library, "strcspn");
  if (!span)
    return 96;
  if (closed && dlclose(library))
    return 95;
  return span(text, reject);
}
