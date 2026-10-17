/* Authored callers: no host auxv, headers, libraries or process state. */
typedef unsigned long U;
extern U getauxval(U);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int __cxa_atexit(void (*)(void *), void *, void *);
extern void __cxa_finalize(void *);
extern int pthread_create(U *, const void *, void *(*)(void *), void *);
extern int pthread_join(U, void **);
static U ConstructorValue, ConstructorErrno;
__attribute__((constructor)) static void initialize(void) {
  *__errno() = 91;
  ConstructorValue = getauxval(16);
  ConstructorErrno = (unsigned)*__errno();
}
U auxiliary_query(U *out, U mode, U type) {
  U (*query)(U) = getauxval;
  if (mode) {
    void *library = dlopen("libauxv.so", 2);
    query = (U (*)(U))dlsym(library, "getauxval");
    if (!query)
      return 1;
    if (mode == 2)
      dlclose(library);
  }
  *__errno() = 77;
  out[0] = query(type);
  out[1] = (unsigned)*__errno();
  out[2] = ConstructorValue;
  out[3] = ConstructorErrno;
  return 0;
}
static void finalize(void *argument) {
  U *out = argument;
  out[0]++;
  out[1] = getauxval(26);
  out[2] = (unsigned)*__errno();
}
U auxiliary_finalize(U *out) {
  out[0] = 0;
  *__errno() = 83;
  if (__cxa_atexit(finalize, out, (void *)0x1234))
    return 1;
  __cxa_finalize((void *)0x1234);
  __cxa_finalize((void *)0x1234);
  out[3] = (unsigned)*__errno();
  return 0;
}
static void *worker(void *argument) {
  U *out = argument;
  *__errno() = 88;
  out[0] = getauxval(42);
  out[1] = (unsigned)*__errno();
  out[2] = getauxval(26);
  out[3] = (unsigned)*__errno();
  return 0;
}
U auxiliary_thread(U *out) {
  U thread;
  *__errno() = 77;
  if (pthread_create(&thread, 0, worker, out))
    return 1;
  if (pthread_join(thread, 0))
    return 2;
  out[4] = (unsigned)*__errno();
  out[5] = getauxval(16);
  out[6] = (unsigned)*__errno();
  return 0;
}
/* The indirect address reference must retain callable ELF type at O2. */
__asm__(".type getauxval, @function");
