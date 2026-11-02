//===- android_time.c - Original API 28 clock ABI workloads --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
typedef long I64;
struct Timespec {
  I64 Seconds, Nanoseconds;
};
struct Timeval {
  I64 Seconds, Microseconds;
};
struct Timezone {
  int West, DST;
};
extern I64 time(I64 *);
extern int clock_gettime(int, struct Timespec *);
extern int nanosleep(const struct Timespec *, struct Timespec *);
extern int gettimeofday(struct Timeval *, struct Timezone *);
extern int clock_getres(int, struct Timespec *);
extern int *__errno(void);
extern int mprotect(void *, U64, int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

static U64 raw(U64 Number, U64 First, U64 Second) {
  register U64 X0 __asm__("x0") = First;
  register U64 X1 __asm__("x1") = Second;
  register U64 X8 __asm__("x8") = Number;
  __asm__ volatile("svc #0" : "+r"(X0) : "r"(X1), "r"(X8) : "memory");
  return X0;
}
U64 time_call(U64 Operation, U64 First, U64 Second, int *Error, U64 Readonly) {
  if (Readonly && mprotect((void *)Readonly, 4096, 1))
    return 99;
  *__errno() = 73;
  U64 Result;
  switch (Operation) {
  case 0:
    Result = (U64)time((I64 *)First);
    break;
  case 1:
    Result = (U64)(I64)gettimeofday((void *)First, (void *)Second);
    break;
  case 2:
    Result = (U64)(I64)clock_gettime((int)First, (void *)Second);
    break;
  case 3:
    Result = raw(169, First, Second);
    break;
  case 4:
    Result = raw(113, First, Second);
    break;
  default:
    Result = (U64)(I64)clock_getres((int)First, (void *)Second);
    break;
  }
  *Error = *__errno();
  return Result;
}
// Volatile observations keep this fixture inside the checked scalar contract.
U64 time_sequence(volatile U64 *Out) {
  struct Timespec Now, Mono;
  struct Timeval TV;
  struct Timezone Zone;
  *__errno() = 73;
  Out[0] = (U64)time(0);
  Out[1] = (U64)time((I64 *)(Out + 2));
  if (clock_gettime(0, &Now) || clock_gettime(1, &Mono) ||
      gettimeofday(&TV, &Zone))
    return 1;
  Out[3] = Now.Seconds;
  Out[4] = Now.Nanoseconds;
  Out[5] = Mono.Seconds;
  Out[6] = Mono.Nanoseconds;
  Out[7] = TV.Seconds;
  Out[8] = TV.Microseconds;
  Out[9] = (U64)(I64)Zone.West;
  Out[10] = (U64)(I64)Zone.DST;
  Out[11] = (U64)time(0);
  Out[12] = *__errno();
  return 0;
}
U64 time_dynamic(U64 *Out, U64 Closed) {
  void *Handle = dlopen("libclock-model.so", 2);
  I64 (*Time)(I64 *) = dlsym(Handle, "time");
  int (*Clock)(int, struct Timespec *) = dlsym(Handle, "clock_gettime");
  int (*TV)(struct Timeval *, struct Timezone *) =
      dlsym(Handle, "gettimeofday");
  if (!Handle || !Time || !Clock || !TV)
    return 1;
  if (Closed)
    dlclose(Handle);
  Out[0] = (U64)Time(0);
  if (Clock(1, (void *)(Out + 1)) || TV((void *)(Out + 3), 0))
    return 2;
  if (!Closed)
    dlclose(Handle);
  return 0;
}

U64 raw_clock_sequence(volatile U64 *Out) {
  void *Handle = dlopen("libclock-model.so", 2);
  int (*Clock)(int, struct Timespec *) = dlsym(Handle, "clock_gettime");
  if (!Handle || !Clock)
    return 1;
  *__errno() = 73;
  for (U64 Phase = 0; Phase != 2; ++Phase) {
    volatile U64 *Now = Out + Phase * 8;
    if (clock_gettime(4, (void *)Now) ||
        raw(113, 0x1234567800000004UL, (U64)(Now + 2)) ||
        Clock(4, (void *)(Now + 4)) || clock_gettime(1, (void *)(Now + 6)))
      return 2;
    if (!Phase) {
      struct Timespec Request = {1, 7};
      if (nanosleep(&Request, &Request) || Request.Seconds != 1 ||
          Request.Nanoseconds != 7)
        return 3;
    }
  }
  Out[16] = *__errno();
  return (U64)(I64)dlclose(Handle);
}
