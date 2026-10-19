/* Independently authored native Android fixtures; no SDK or device required. */
typedef unsigned long u64;
extern void *memcpy(void *, const void *, u64);
extern void *memmove(void *, const void *, u64);
extern void *memset(void *, int, u64);
extern int memcmp(const void *, const void *, u64);
extern int strcmp(const char *, const char *);
extern u64 strlen(const char *);
extern void *malloc(u64);
extern void *calloc(u64, u64);
extern void *realloc(void *, u64);
extern void free(void *);
extern int *__errno(void);
extern int getpagesize(void);
extern long sysconf(int);
extern int madvise(void *, u64, int);
extern long syscall(long, ...);
extern int __system_property_get(const char *, char *);
extern int android_get_device_api_level(void);
extern int getpid(void);
extern int gettid(void);
extern unsigned int getuid(void);
extern unsigned int geteuid(void);
extern unsigned int getgid(void);
extern unsigned int getegid(void);
extern int setuid(unsigned int);
extern long write(int, const void *, u64);
extern u64 unknown_native_function(u64);
static volatile u64 seed = 5;
static volatile u64 *volatile seed_pointer = &seed;
/* A supplied executable page lets tests address exact instruction boundaries.
 */
u64 observation_call(u64 (*entry)(u64 *, u64), u64 *out, u64 value) {
  return entry(out, value);
}
u64 observation_allocated(u64 (*entry)(u64 *, u64), u64 *out, u64 value) {
  u64 *buffer = (u64 *)malloc(32);
  if (!buffer)
    return 99;
  buffer[2] = value;
  u64 result = entry(buffer, 0);
  out[0] = buffer[0];
  out[1] = buffer[1];
  free(buffer);
  return result;
}
extern int pthread_create(u64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(u64, void **);
u64 observation_threads(void *(*entry)(void *), u64 *out) {
  u64 first, second;
  if (pthread_create(&first, 0, entry, out) ||
      pthread_create(&second, 0, entry, out + 4))
    return 99;
  if (pthread_join(first, 0) || pthread_join(second, 0))
    return 98;
  return out[0] + out[4];
}
__attribute__((constructor)) static void initialize(void) { seed = 17; }
u64 add_arguments(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h, u64 i,
                  u64 j) {
  return *seed_pointer + a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g +
         8 * h + 9 * i + 10 * j;
}
u64 properties(char *out) {
  char local[92];
  int n = __system_property_get("test.device", local);
  if (n != 6 || strcmp(local, "sample") || strlen(local) != 6)
    return 1;
  memcpy(out, local, 7);
  return android_get_device_api_level();
}
u64 absent_property(char *out) { return __system_property_get("missing", out); }
u64 inspect_memory_input(unsigned char *p, u64 n) {
  u64 hash = 14695981039346656037ul;
  const u64 *words = (const u64 *)p;
  for (u64 i = 0; i < n / 8; ++i)
    hash = (hash ^ words[i]) * 1099511628211ul;
  for (u64 i = n & ~(u64)7; i < n; ++i)
    hash = (hash ^ p[i]) * 1099511628211ul;
  if (n)
    p[0] ^= 255;
  p[n] = 165;
  return hash;
}
u64 allocation(char *out) {
  char *p = calloc(4, 8);
  if (!p || p[31])
    return 1;
  memset(p, 'A', 32);
  p[3] = 'Z';
  p = realloc(p, 64);
  if (!p || p[3] != 'Z' || p[31] != 'A')
    return 2;
  memmove(p + 1, p, 31);
  memcpy(out, p, 32);
  free(p);
  void *huge = malloc(~(u64)0);
  return huge == 0 && *__errno() == 12 ? 0 : 3;
}
u64 libc_error(void) {
  *__errno() = 77;
  long result = write(99, (const void *)0, 5);
  return result == -1 && *__errno() == 9 ? 0 : 1;
}
u64 page_size(unsigned int *out) {
  *__errno() = 77;
  out[0] = getpagesize();
  out[1] = *__errno();
  return out[0];
}
u64 page_queries(u64 *out) {
  *__errno() = 77;
  out[0] = getpagesize();
  out[1] = sysconf(0x27);
  out[2] = sysconf(0x28);
  out[3] = *__errno();
  unsigned char *page = (unsigned char *)malloc(out[2]);
  if (!page)
    return 1;
  page[out[2] - 1] = 0x5a;
  out[4] = (u64)page % out[2];
  out[5] = page[out[2] - 1];
  free(page);
  return 0;
}
u64 page_query(u64 name) { return sysconf((int)name); }
u64 memory_advice(u64 *out, u64 address, u64 length, u64 advice, u64 route) {
  *__errno() = 77;
  u64 result;
  if (!route) {
    result = madvise((void *)address, length, (int)advice);
  } else if (route == 1) {
    register u64 x0 __asm__("x0") = address;
    register u64 x1 __asm__("x1") = length;
    register u64 x2 __asm__("x2") = advice;
    register u64 x8 __asm__("x8") = 233;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x8)
                     : "memory");
    result = x0;
  } else {
    result = syscall(233, address, length, advice);
  }
  out[0] = result;
  out[1] = *__errno();
  return result;
}
u64 raw_error(void) {
  *__errno() = 77;
  register u64 x0 __asm__("x0") = 99;
  register u64 x1 __asm__("x1") = 0;
  register u64 x2 __asm__("x2") = 5;
  register u64 x8 __asm__("x8") = 64;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0 == (u64)-9 && *__errno() == 77 ? 0 : 1;
}
u64 identity_queries(unsigned int *out) {
  *__errno() = 77;
  out[0] = getpid();
  out[1] = gettid();
  out[2] = getuid();
  out[3] = geteuid();
  out[4] = getgid();
  out[5] = getegid();
  const u64 numbers[] = {172, 178, 174, 175, 176, 177};
  for (unsigned int i = 0; i < 6; ++i) {
    register u64 x0 __asm__("x0") = ~(u64)0;
    register u64 x8 __asm__("x8") = numbers[i];
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    if (x0 != out[i])
      return 1;
  }
  return *__errno() == 77 ? 0 : 2;
}
u64 identity_mutation(void) { return setuid(0); }
u64 print_bytes(void) {
  const char text[] = {'A', 0, (char)255, '\n'};
  return write(1, text, sizeof(text));
}
u64 unknown_call(void) { return unknown_native_function(19); }
u64 bad_pointer(void) {
  memcpy((void *)1, (const void *)2, 8);
  return 0;
}
u64 bad_free(void) {
  free((void *)123);
  return 0;
}
u64 busy_loop(void) {
  for (;;)
    __asm__ volatile("");
}
u64 forged_trap(void) {
  __asm__ volatile("svc #0x4e44");
  return 0;
}
u64 tls_slots(void) {
  u64 tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  return (u64)__errno() == tp + 16 && *(u64 *)(tp + 40) != 0;
}
u64 stack_failure(void) {
  u64 tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  *(volatile u64 *)(tp + 40) ^= 1;
  return 0;
}

extern volatile unsigned char __sF[];
u64 stdio_identity(void) { return (u64)__sF != 0; }
u64 stdio_content(void) { return __sF[0]; }

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
u64 device_api_level(unsigned int *out, u64 mode) {
  int (*query)(void) = android_get_device_api_level;
  if (mode) {
    void *handle = dlopen("libplatform.so", 2);
    if (!handle)
      return 1;
    query = (int (*)(void))dlsym(handle, "android_get_device_api_level");
    if (!query)
      return 2;
    if (mode == 2 && dlclose(handle))
      return 3;
  }
  *__errno() = 77;
  int level = query();
  out[0] = (unsigned int)level;
  out[1] = (unsigned int)*__errno();
  out[2] = (unsigned int)query();
  out[3] = (unsigned int)*__errno();
  return (u64)(long)level;
}
extern char *dlerror(void);
typedef u64 (*length_fn)(const char *);
typedef unsigned int (*identity_fn)(void);

u64 dynamic_page_query(u64 *out, u64 after_close, u64 name) {
  *__errno() = 77;
  void *handle = dlopen("libpages.so", 2);
  if (!handle)
    return 100;
  long (*query)(int) = (long (*)(int))dlsym(handle, "sysconf");
  if (!query)
    return 101;
  out[0] = query((int)name);
  out[1] = *__errno();
  if (dlclose(handle))
    return 102;
  if (after_close)
    out[2] = query((int)name);
  return 0;
}

u64 dynamic_page_size(unsigned int *out, u64 after_close) {
  *__errno() = 77;
  void *handle = dlopen("libpages.so", 2);
  if (!handle)
    return 100;
  int (*query)(void) = (int (*)(void))dlsym(handle, "getpagesize");
  if (!query)
    return 101;
  out[0] = query();
  if (dlclose(handle))
    return 102;
  if (after_close)
    out[2] = query();
  out[1] = *__errno();
  return 0;
}

u64 dynamic_memory_advice(u64 address, u64 length, u64 advice,
                          u64 after_close) {
  void *handle = dlopen("libadvice.so", 2);
  if (!handle)
    return 100;
  int (*call)(void *, u64, int) =
      (int (*)(void *, u64, int))dlsym(handle, "madvise");
  if (!call)
    return 101;
  u64 result = call((void *)address, length, (int)advice);
  if (dlclose(handle))
    return 102;
  return after_close ? (u64)call((void *)address, length, (int)advice) : result;
}

u64 dynamic_identities(unsigned int *out, u64 after_close) {
  *__errno() = 77;
  void *handle = dlopen("libidentity.so", 2);
  if (!handle)
    return 100;
  const char *names[] = {"getuid", "geteuid", "getgid", "getegid"};
  identity_fn saved = 0;
  for (unsigned int i = 0; i < 4; ++i) {
    identity_fn query = (identity_fn)dlsym(handle, names[i]);
    if (!query)
      return 101 + i;
    out[i] = query();
    if (!i)
      saved = query;
  }
  if (dlclose(handle))
    return 105;
  if (after_close)
    return saved();
  return *__errno() == 77 ? 0 : 106;
}

u64 dynamic_lookup(void) {
  void *h = dlopen("libfixture.so", 2);
  if (!h)
    return 100;
  length_fn length = (length_fn)dlsym(h, "strlen");
  if (!length)
    return 101;
  u64 result = length("four");
  if (dlclose(h))
    return 102;
  return result;
}
u64 dynamic_lifecycle(void) {
  *__errno() = 77;
  if (dlerror() || dlopen("libfixture.so", 6) || !dlerror() || dlerror())
    return 1;
  void *a = dlopen("libfixture.so", 1);
  void *b = dlopen("libfixture.so", 2);
  void *c = dlopen("libfixture.so", 6);
  if (!a || a != b || a != c)
    return 2;
  if (dlsym(a, "missing_export"))
    return 3;
  length_fn length = (length_fn)dlsym(a, "strlen");
  if (!length || !dlerror() || dlerror() ||
      dlsym(b, "strlen") != (void *)length)
    return 4;
  if (dlclose(a) || dlclose(b) || length("ok") != 2 || dlclose(c))
    return 5;
  if (dlsym(a, "strlen") || !dlerror() || dlerror() || dlclose(a) != -1 ||
      !dlerror())
    return 6;
  void *d = dlopen("libfixture.so", 2);
  if (!d || d == a || dlsym(a, "strlen") || !dlerror() || !dlsym(d, "strlen"))
    return 7;
  if (dlclose(d) || dlopen("missing.so", 2))
    return 8;
  const char *error = dlerror();
  if (!error || strlen(error) == 0 || dlerror() || *__errno() != 77)
    return 9;
  u64 tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  if (dlsym(a, 0) || *(u64 *)(tp + 48) == 0)
    return 10;
  if ((u64)dlerror() == 0 || *(u64 *)(tp + 48) != 0 || dlerror())
    return 11;
  return 0;
}
u64 dynamic_unknown(void) {
  void *h = dlopen("libfixture.so", 2);
  length_fn function = (length_fn)dlsym(h, "unmodeled_fixture_export");
  return function("input");
}
u64 dynamic_dispatch(const char *name, u64 after_close) {
  void *h = dlopen("libfixture.so", 2);
  length_fn function = (length_fn)dlsym(h, name);
  if (after_close)
    dlclose(h);
  return function((const char *)1);
}
u64 dynamic_closed(void) {
  void *h = dlopen("libfixture.so", 2);
  length_fn function = (length_fn)dlsym(h, "strlen");
  dlclose(h);
  return function("input");
}
u64 dynamic_scope(u64 scope) { return (u64)dlsym((void *)scope, "strlen"); }
u64 default_call(void) {
  length_fn length = (length_fn)dlsym(0, "strlen");
  return length ? length("scoped") : 100;
}
u64 default_missing(void) {
  *__errno() = 77;
  if (dlerror() || dlsym(0, "missing_export"))
    return 1;
  const char *error = dlerror();
  return error && *error && !dlerror() && *__errno() == 77 ? 0 : 2;
}
u64 default_unknown(void) {
  length_fn function = (length_fn)dlsym(0, "unmodeled_fixture_export");
  return function ? function("input") : 100;
}
u64 default_resident_lifecycle(void) {
  *__errno() = 77;
  if (dlerror() || dlsym(0, "missing_export"))
    return 1;
  length_fn length = (length_fn)dlsym(0, "strlen");
  /* A successful lookup does not consume the earlier linker error. */
  if (!length || length("before open") != 11 || !dlerror() || dlerror())
    return 2;
  void *a = dlopen("libfixture.so", 6);
  void *b = dlopen("libfixture.so", 2);
  if (!a || a != b || dlsym(a, "strlen") != (void *)length)
    return 3;
  if (dlclose(a) || dlclose(b) || length("resident") != 8 ||
      dlsym(0, "strlen") != (void *)length ||
      dlsym(a, "strlen") != (void *)length)
    return 4;
  void *c = dlopen("libfixture.so", 6);
  if (c != a || dlclose(c) || dlerror() || *__errno() != 77)
    return 5;
  return 0;
}
u64 default_local_open(void) {
  void *a = dlopen("libfixture.so", 2);
  if (!a || !dlsym(a, "strlen") || dlsym(0, "strlen") || !dlerror())
    return 1;
  return dlclose(a) ? 2 : 0;
}
u64 dynamic_flag_combinations(u64 flags) {
  void *a = dlopen("libfixture.so", (int)flags);
  if (flags & ~0x1107ULL)
    return !a && dlerror() && !dlerror() ? 0 : 1;
  if (!a || !dlsym(a, "strlen") || dlerror())
    return 2;
  return dlclose(a) ? 3 : 0;
}
u64 dynamic_open(u64 name, u64 flags) {
  return (u64)dlopen((const char *)name, (int)flags);
}
u64 dynamic_bad_name(void) {
  return (u64)dlsym(dlopen("libfixture.so", 2), (const char *)1);
}
u64 dynamic_providers(void) {
  void *a = dlopen("libfixture.so", 2);
  void *b = dlopen("libother.so", 2);
  length_fn first = (length_fn)dlsym(a, "strlen");
  length_fn second = (length_fn)dlsym(b, "strlen");
  if (!first || !second || first == second)
    return 1;
  if (dlsym(a, "only_in_other") || !dlerror() || !dlsym(b, "only_in_other"))
    return 2;
  return first("ab") + second("cde");
}
