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
extern int __system_property_get(const char *, char *);
extern int android_get_device_api_level(void);
extern long write(int, const void *, u64);
extern u64 unknown_native_function(u64);
static volatile u64 seed = 5;
static volatile u64 *volatile seed_pointer = &seed;
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
u64 raw_error(void) {
  *__errno() = 77;
  register u64 x0 __asm__("x0") = 99;
  register u64 x1 __asm__("x1") = 0;
  register u64 x2 __asm__("x2") = 5;
  register u64 x8 __asm__("x8") = 64;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0 == (u64)-9 && *__errno() == 77 ? 0 : 1;
}
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
extern char *dlerror(void);
typedef u64 (*length_fn)(const char *);

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
u64 dynamic_closed(void) {
  void *h = dlopen("libfixture.so", 2);
  length_fn function = (length_fn)dlsym(h, "strlen");
  dlclose(h);
  return function("input");
}
u64 dynamic_scope(u64 scope) { return (u64)dlsym((void *)scope, "strlen"); }
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
