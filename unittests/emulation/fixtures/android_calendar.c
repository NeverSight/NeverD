/* Authored callers for the API 28 LP64 calendar ABI. No host calendar code. */
typedef unsigned long U;
typedef long S;
typedef unsigned char B;
struct TM {
  int sec, min, hour, mday, mon, year, wday, yday, isdst;
  long gmtoff;
  const char *zone;
};
_Static_assert(sizeof(struct TM) == 56, "LP64 tm size");
extern struct TM *localtime(const S *);
extern struct TM *gmtime(const S *);
extern struct TM *localtime_r(const S *, struct TM *);
extern struct TM *gmtime_r(const S *, struct TM *);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int mprotect(void *, U, int);
extern int pthread_create(U *, const void *, void *(*)(void *), void *);
extern int pthread_join(U, void **);
static void copy(B *d, const B *s, U n) {
  for (U i = 0; i < n; ++i)
    d[i] = s[i];
}
U calendar_call(B *out, U op, S seconds, U mode, U named) {
  struct TM *(*single)(const S *) = op == 0 ? localtime : gmtime;
  struct TM *(*caller)(const S *, struct TM *) =
      op == 2 ? localtime_r : gmtime_r;
  if (named) {
    const char *name = op == 0   ? "localtime"
                       : op == 1 ? "gmtime"
                       : op == 2 ? "localtime_r"
                                 : "gmtime_r";
    void *library = dlopen("libcalendar.so", 2);
    if (op < 2)
      single = (struct TM * (*)(const S *)) dlsym(library, name);
    else
      caller = (struct TM * (*)(const S *, struct TM *)) dlsym(library, name);
    if (named == 2)
      dlclose(library);
  }
  S *input = &seconds;
  struct TM *dest = (struct TM *)(out + 0x100);
  if (mode == 1) {
    input = (S *)dest;
    *input = seconds;
  }
  if (mode == 2)
    input = (S *)1;
  if (mode == 3)
    dest = (struct TM *)1;
  if (mode == 4)
    mprotect(out, 4096, 1);
  *__errno() = 77;
  struct TM *result = op < 2 ? single(input) : caller(input, dest);
  ((U *)out)[0] = (U)result;
  ((U *)out)[1] = (unsigned)*__errno();
  if (!result)
    return 1;
  copy(out + 0x200, (B *)result, 56);
  for (U i = 0; i < 33; ++i) {
    out[0x300 + i] = result->zone[i];
    if (!result->zone[i])
      break;
  }
  return 0;
}
static void *worker(void *argument) {
  B *out = argument;
  S seconds = 0;
  *__errno() = 88;
  struct TM *p = localtime(&seconds);
  copy(out + 0x300, (B *)p, 56);
  ((U *)out)[6] = (unsigned)*__errno();
  return p;
}
U calendar_sequence(B *out, U threaded) {
  U *r = (U *)out;
  S seconds = -1;
  *__errno() = 77;
  struct TM *first = gmtime(&seconds);
  r[0] = (U)first;
  copy(out + 0x200, (B *)first, 56);
  if (threaded) {
    U thread;
    void *second;
    if (pthread_create(&thread, 0, worker, out) ||
        pthread_join(thread, &second))
      return 1;
    r[1] = (U)second;
  } else {
    seconds = 0;
    struct TM *second = localtime(&seconds);
    r[1] = (U)second;
    copy(out + 0x300, (B *)second, 56);
    r[6] = 77;
  }
  seconds = 951782400; /* 2000-02-29 00:00:00 UTC. */
  r[2] = (U)gmtime_r(&seconds, (struct TM *)(out + 0x100));
  copy(out + 0x400, (B *)first, 56);
  r[3] = (unsigned)*__errno();
  r[4] = first == (struct TM *)r[1];
  r[5] = first != (struct TM *)r[2];
  return 0;
}
__asm__(".type localtime,@function\n.type gmtime,@function\n"
        ".type localtime_r,@function\n.type gmtime_r,@function");
