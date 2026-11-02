/* Independent guest callers; no host headers, error catalog or libc. */
typedef unsigned long U;
extern char *strerror(int);
extern int strerror_r(int, char *, U);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern char *dlerror(void);
extern int mprotect(void *, U, int);
extern int munmap(void *, U);
extern int pthread_create(U *, const void *, void *(*)(void *), void *);
extern int pthread_join(U, void **);
#ifndef ERROR_DYNAMIC_ONLY
/* A machine ABI adapter deliberately leaves the upper argument bits intact. */
extern char *error_wide(U);
__asm__(".text\n.globl error_wide\n.type error_wide, @function\n"
        "error_wide:\n b strerror\n.size error_wide, .-error_wide\n");
#endif

static U copy_text(unsigned char *out, const char *p) {
  for (U i = 0; i != 96; ++i) {
    unsigned char c = (unsigned char)p[i];
    out[i] = c;
    if (!c)
      return i;
  }
  return (U)-1;
}

U error_query(unsigned char *out, U code, U mode) {
  char *(*query)(U);
#ifndef ERROR_DYNAMIC_ONLY
  query = error_wide;
#else
  if (!mode)
    return 1;
#endif
  if (mode) {
    void *library = dlopen("liberrors.so", 2);
    query = (char *(*)(U))dlsym(library, "strerror");
    if (!query)
      return 2;
    if (mode == 2)
      dlclose(library);
  }
  *__errno() = 733;
  char *text = query(code);
  ((U *)out)[0] = (U)text;
  ((U *)out)[1] = copy_text(out + 32, text);
  ((U *)out)[2] = (unsigned)*__errno();
  return 0;
}

#ifndef ERROR_DYNAMIC_ONLY
U error_sweep(unsigned char *out) {
  for (U i = 0; i != 136; ++i)
    if (error_query(out + i * 128, i, 0))
      return 1;
  static const U extra[] = {0xffffffffUL,  0x80000000UL,
                            0x7fffffffUL,  0xffffffff00000002UL,
                            0x100000000UL, 0x11223344ffffffffUL};
  for (U i = 0; i != sizeof(extra) / sizeof(extra[0]); ++i)
    if (error_query(out + (136 + i) * 128, extra[i], 0))
      return 2;
  return 0;
}

U error_lifetime(unsigned char *out) {
  dlopen("absent-errors.so", 2);
  char *linker = dlerror();
  if (!linker)
    return 1;
  copy_text(out + 512, linker);
  dlopen("absent-errors.so", 2);
  *__errno() = 733;
  char *unknown = strerror(-12345);
  char *known = strerror(2);
  ((U *)out)[0] = (U)unknown;
  ((U *)out)[1] = (U)known;
  copy_text(out + 64, unknown);
  ((U *)out)[2] = (U)strerror(2);
  ((U *)out)[3] = (U)strerror(-1);
  ((U *)out)[4] = (unsigned)*__errno();
  copy_text(out + 192, unknown);
  copy_text(out + 320, known);
  char *after = dlerror();
  ((U *)out)[5] = (U)after;
  copy_text(out + 640, after);
  ((U *)out)[6] = (U)dlerror();
  return 0;
}

static volatile U ready[2], go;
static unsigned char *worker_outputs[2];

static void *worker(void *argument) {
  U index = (U)argument;
  unsigned char *out = worker_outputs[index];
  *__errno() = (int)(81 + index);
  char *unknown = strerror(index ? 58 : 41);
  ((U *)out)[0] = (U)unknown;
  ((U *)out)[1] = (U)strerror(2);
  ready[index] = 1;
  while (!go) {
  }
  copy_text(out + 64, unknown);
  ((U *)out)[2] = (unsigned)*__errno();
  return 0;
}

U error_threads(unsigned char *out) {
  U threads[2];
  *__errno() = 77;
  char *unknown = strerror(-1);
  ((U *)out)[0] = (U)unknown;
  ((U *)out)[2] = (U)strerror(2);
  for (U i = 0; i != 2; ++i) {
    worker_outputs[i] = out + 256 * (i + 1);
    if (pthread_create(threads + i, 0, worker, (void *)i))
      return 1;
  }
  while (!ready[0] || !ready[1]) {
  }
  go = 1;
  for (U i = 0; i != 2; ++i)
    if (pthread_join(threads[i], 0))
      return 2;
  copy_text(out + 64, unknown);
  ((U *)out)[1] = (unsigned)*__errno();
  return 0;
}

U error_write_known(void) {
  volatile char *text = strerror(2);
  *text = 'x';
  return 0;
}

U error_known_unmapped(void) {
  *__errno() = 733;
  char *p = strerror(2);
  if (munmap((void *)((U)p & ~(U)4095), 4096))
    return 1;
  char *q = error_wide(0xffffffff00000002UL);
  return p == q && *__errno() == 733 ? 0 : 2;
}

U error_readonly_tls(void) {
  U tls = (U)__errno() & ~(U)4095;
  strerror(-1);
  if (mprotect((void *)tls, 4096, 1))
    return 1;
  return (U)strerror(41);
}

U error_reentrant_not_admitted(unsigned char *out) {
  return (U)strerror_r(2, (char *)out, 96);
}
#endif
