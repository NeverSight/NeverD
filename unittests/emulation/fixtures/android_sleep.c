//===- android_sleep.c - Independent relative sleep and wait workloads ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
struct Timespec {
  long Seconds, Nanoseconds;
};
extern int nanosleep(const struct Timespec *, struct Timespec *);
extern int usleep(unsigned);
extern unsigned sleep(unsigned);
extern long syscall(long, ...);
extern int clock_gettime(int, struct Timespec *);
extern long time(long *);
extern int *__errno(void);
extern int gettid(void);
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
extern int pthread_once(int *, void (*)(void));
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

static long sleep_route(U64 Mode, const struct Timespec *Request,
                        struct Timespec *Remaining) {
  if (Mode == 0)
    return nanosleep(Request, Remaining);
  if (Mode == 1)
    return syscall(101, Request, Remaining);
  if (Mode == 3)
    return usleep(
        (unsigned)(Request->Seconds * 1000000 + Request->Nanoseconds / 1000));
  if (Mode == 4)
    return sleep((unsigned)Request->Seconds);
  register U64 X0 __asm__("x0") = (U64)Request;
  register U64 X1 __asm__("x1") = (U64)Remaining;
  register U64 X8 __asm__("x8") = 101;
  __asm__ volatile("svc #0" : "+r"(X0) : "r"(X1), "r"(X8) : "memory");
  return (long)X0;
}

U64 sleep_call(U64 Mode, const struct Timespec *Request,
               struct Timespec *Remaining, U64 *Out) {
  *__errno() = 73;
  long Result = sleep_route(Mode, Request, Remaining);
  Out[0] = *__errno();
  if (clock_gettime(0, (void *)(Out + 1)))
    return 99;
  Out[3] = time(0);
  return (U64)Result;
}

static volatile U64 *Shared;
static U64 Lock[5];
static int Once;
static U64 Mode;

static void initializer(void) {
  struct Timespec Request = {0, 5};
  Shared[5] = 1;
  nanosleep(&Request, &Request);
  Shared[6] = 1;
}

static void *worker(void *Argument) {
  U64 ID = (U64)Argument;
  volatile U64 *Row = Shared + ID * 32;
  U64 Cookie = 0xfeedface00000000UL + ID;
  volatile U64 Local = Cookie;
  register U64 Held __asm__("x19") = Cookie;
  struct Timespec Request = {0, ID == 1 ? 3 : ID == 2 ? 1 : 2};
  *__errno() = 70 + ID;
  Row[7] = gettid();
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(Row[8]));
  __asm__ volatile("fmov d8, %1" : "+r"(Held) : "r"(Cookie) : "d8");
  if (Mode && ID == 1)
    pthread_mutex_lock(Lock);
  Row[0] = sleep_route(ID - 1, &Request, &Request);
  __asm__ volatile("fmov %1, d8" : "+r"(Held), "=r"(Row[4]));
  Row[3] = Held;
  Row[2] = Local;
  Row[1] = *__errno();
  struct Timespec Now;
  clock_gettime(1, &Now);
  Row[5] = Now.Seconds;
  Row[6] = Now.Nanoseconds;
  if (Mode) {
    if (ID != 1)
      pthread_mutex_lock(Lock);
    pthread_mutex_unlock(Lock);
    pthread_once(&Once, initializer);
    Row[9] = Shared[6];
  }
  pthread_mutex_lock(Lock);
  Shared[1 + Shared[0]++] = ID;
  pthread_mutex_unlock(Lock);
  return (void *)Cookie;
}

U64 sleep_workers(U64 *Out, U64 Interleave) {
  Shared = Out;
  Mode = Interleave;
  U64 Threads[3];
  for (U64 I = 0; I < 3; ++I)
    if (pthread_create(Threads + I, 0, worker, (void *)(I + 1)))
      return 1;
  for (U64 I = 0; I < 3; ++I) {
    void *Value;
    if (pthread_join(Threads[I], &Value) ||
        Value != (void *)(0xfeedface00000001UL + I))
      return 2;
  }
  Out[7] = time(0);
  return 0;
}

U64 sleep_dynamic(U64 Closed) {
  void *Library = dlopen("libsleep-model.so", 0);
  int (*Sleep)(const struct Timespec *, struct Timespec *) =
      dlsym(Library, "nanosleep");
  if (Closed)
    dlclose(Library);
  struct Timespec Request = {0, 1};
  return Sleep(&Request, (void *)1);
}

// Forward the full ABI register so the model must narrow useconds_t itself.
__attribute__((naked)) static int sleep_micro_register(U64 Microseconds) {
  __asm__("b usleep");
}

U64 sleep_micro(U64 Microseconds, U64 Route, U64 *Out) {
  int (*Sleep)(unsigned) = usleep;
  if (Route == 1 || Route == 2) {
    void *Library = dlopen("libsleep-model.so", 0);
    Sleep = dlsym(Library, "usleep");
    if (Route == 2)
      dlclose(Library);
  }
  *__errno() = 73;
  int Result = Route == 3 ? sleep_micro_register(Microseconds)
                          : Sleep((unsigned)Microseconds);
  Out[0] = *__errno();
  if (clock_gettime(0, (void *)(Out + 1)))
    return 99;
  Out[3] = time(0);
  return (U64)(long)Result;
}

__attribute__((naked)) static unsigned sleep_second_register(U64 Seconds) {
  __asm__("b sleep");
}

U64 sleep_seconds(U64 Seconds, U64 Route, U64 *Out) {
  unsigned (*Sleep)(unsigned) = sleep;
  if (Route == 1 || Route == 2) {
    void *Library = dlopen("libsleep-model.so", 0);
    Sleep = dlsym(Library, "sleep");
    if (Route == 2)
      dlclose(Library);
  }
  *__errno() = 73;
  unsigned Result =
      Route == 3 ? sleep_second_register(Seconds) : Sleep((unsigned)Seconds);
  Out[0] = *__errno();
  if (clock_gettime(0, (void *)(Out + 1)))
    return 99;
  Out[3] = time(0);
  return Result;
}

static void *spinner(void *Argument) {
  (void)Argument;
  for (;;)
    gettid();
}
U64 sleep_busy(U64 *Out, U64 Route) {
  U64 Thread;
  if (pthread_create(&Thread, 0, spinner, 0))
    return 1;
  struct Timespec Request = {1, 0};
  sleep_route(Route, &Request, 0);
  Out[0] = 1;
  return 0;
}
