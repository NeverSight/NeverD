//===- android_proc.c - Independent live Android process callers ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
typedef long I64;
extern int open(const char *, int, ...);
extern int close(int);
extern int dup(int);
extern I64 read(int, void *, U64);
extern I64 write(int, const void *, U64);
extern I64 lseek(int, I64, int);
extern I64 syscall(I64, ...);
extern int fstat(int, void *);
extern void *mmap(void *, U64, int, int, int, I64);
extern int mprotect(void *, U64, int);
extern int munmap(void *, U64);
extern void *opendir(const char *);
extern void *readdir(void *);
extern void rewinddir(void *);
extern int closedir(void *);
extern void *fopen(const char *, const char *);
extern char *fgets(char *, int, void *);
extern int fclose(void *);
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
extern int gettid(void);
extern int prctl(int, ...);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

static int equal(const char *A, const char *B) {
  while (*A && *A == *B) {
    ++A;
    ++B;
  }
  return *A == *B;
}
static int contains(const char *A, const char *B) {
  for (; *A; ++A) {
    const char *X = A, *Y = B;
    while (*Y && *X == *Y) {
      ++X;
      ++Y;
    }
    if (!*Y)
      return 1;
  }
  return 0;
}
static int task_count(void *D) {
  int Count = 0;
  for (;;) {
    const char *E = readdir(D);
    if (!E)
      return Count;
    if (E[19] >= '0' && E[19] <= '9')
      ++Count;
  }
}
static U64 Mutex;
static U64 ChildID;
static void *child(void *Cookie) {
  ChildID = gettid();
  void *S = fopen("/proc/thread-self/status", "r");
  if (!S)
    return (void *)91;
  char Line[160];
  int Found = 0;
  while (fgets(Line, sizeof(Line), S))
    if (equal(Line, "Pid:\t1001\n"))
      Found = 1;
  if (!Found || fclose(S))
    return (void *)92;
  pthread_mutex_lock(&Mutex);
  pthread_mutex_unlock(&Mutex);
  return Cookie;
}

U64 proc_thread_lifecycle(void) {
  U64 T;
  if (pthread_mutex_lock(&Mutex) || pthread_create(&T, 0, child, (void *)73))
    return 1;
  void *D = opendir("/proc/self/task");
  if (!D || task_count(D) != 2)
    return 2;
  void *S = fopen("/proc/self/status", "r");
  if (!S)
    return 3;
  int Found = 0;
  char Line[160];
  while (fgets(Line, sizeof(Line), S))
    if (equal(Line, "Threads:\t2\n"))
      Found = 1;
  if (!Found || fclose(S))
    return 4;
  if (pthread_mutex_unlock(&Mutex))
    return 5;
  void *Value;
  if (pthread_join(T, &Value) || Value != (void *)73 || ChildID != 1001)
    return 6;
  rewinddir(D);
  if (task_count(D) != 1 || closedir(D))
    return 7;
  if (open("/proc/1000/task/1001/status", 0) != -1)
    return 8;
  return 0;
}

U64 proc_live_maps(void) {
  char Buffer[4096];
  void *Region = mmap((void *)0x30000000, 8192, 3, 34, -1, 0);
  if (Region != (void *)0x30000000)
    return 1;
  int FD = open("/proc/self/maps", 0);
  if (FD < 0)
    return 2;
  I64 N = read(FD, Buffer, sizeof(Buffer) - 1);
  if (N < 0)
    return 3;
  Buffer[N] = 0;
  if (!contains(Buffer, "30000000-30002000 rw-p"))
    return 4;
  if (mprotect((char *)Region + 4096, 4096, 1) || lseek(FD, 0, 0))
    return 5;
  N = syscall(63, FD, Buffer, sizeof(Buffer) - 1, 0, 0, 0);
  if (N < 0)
    return 6;
  Buffer[N] = 0;
  if (!contains(Buffer, "30001000-30002000 r--p"))
    return 7;
  if (munmap(Region, 8192) || lseek(FD, 0, 0))
    return 8;
  N = read(FD, Buffer, sizeof(Buffer) - 1);
  if (N < 0)
    return 9;
  Buffer[N] = 0;
  if (contains(Buffer, "30000000-") || contains(Buffer, "30001000-"))
    return 10;
  return close(FD) ? 11 : 0;
}

static I64 raw_prctl(U64 Option, U64 Suboption, U64 Address, U64 Size,
                     U64 Name) {
  register U64 X0 __asm__("x0") = Option;
  register U64 X1 __asm__("x1") = Suboption;
  register U64 X2 __asm__("x2") = Address;
  register U64 X3 __asm__("x3") = Size;
  register U64 X4 __asm__("x4") = Name;
  register U64 X8 __asm__("x8") = 167;
  __asm__ volatile("svc #0"
                   : "+r"(X0)
                   : "r"(X1), "r"(X2), "r"(X3), "r"(X4), "r"(X8)
                   : "memory", "cc");
  return (I64)X0;
}

U64 proc_prctl_option(U64 *Out, U64 Option, U64 Route) {
  int (*Call)(int, ...) = prctl;
  if (Route >= 3) {
    void *Handle = dlopen("libprctl.so", 2);
    Call = (int (*)(int, ...))dlsym(Handle, "prctl");
    if (Route == 4)
      dlclose(Handle);
  }
  *__errno() = 733;
  I64 Result;
  if (Route == 1)
    Result = raw_prctl(Option, ~0UL, ~0UL, ~0UL, ~0UL);
  else if (Route == 2)
    Result = syscall(167, Option, ~0UL, ~0UL, ~0UL, ~0UL);
  else
    Result = Call((int)Option, ~0UL, ~0UL, ~0UL, ~0UL);
  Out[0] = (U64)Result;
  Out[1] = (U64)*__errno();
  return 0;
}

static int maps_have(const char *Text) {
  char Buffer[4096];
  int FD = open("/proc/self/maps", 0);
  if (FD < 0)
    return -1;
  I64 N = read(FD, Buffer, sizeof(Buffer) - 1);
  if (N < 0 || close(FD))
    return -1;
  Buffer[N] = 0;
  return contains(Buffer, Text);
}

U64 proc_prctl_names(void) {
  char Name[] = "first";
  char *Region = mmap((void *)0x30000000, 12288, 3, 34, -1, 0);
  if (Region != (void *)0x30000000)
    return 1;
  *__errno() = 73;
  if (prctl(0x53564d41, 0UL, Region, 12288UL, Name) || *__errno() != 73 ||
      maps_have("[anon:first]") != 1)
    return 2;
  Name[0] = 'F';
  if (maps_have("[anon:First]") != 1)
    return 3;
  if (raw_prctl(0x7654321053564d41UL, 0, (U64)Region, 1, (U64) "raw") ||
      maps_have("[anon:raw]") != 1)
    return 4;
  if (syscall(167, 0x53564d41UL, 0UL, Region, 4096UL, 0UL) ||
      maps_have("[anon:raw]") != 0)
    return 5;
  if (raw_prctl(0x53564d41, 1UL << 32, (U64)Region, 4096, 0) != -22 ||
      raw_prctl(0x53564d41, ~0UL, (U64)Region, 0, ~0UL))
    return 6;
  if (prctl(0x53564d41, 0UL, Region + 1, 0UL, Name) != -1 || *__errno() != 22)
    return 7;
  if (munmap(Region + 4096, 4096))
    return 8;
  if (prctl(0x53564d41, 0UL, Region, 12288UL, "hole") != -1 ||
      *__errno() != 12 || maps_have("[anon:hole]") != 1)
    return 9;
  if (raw_prctl(0x53564d41, 0, (U64)Region, 12288, 1) != -12 ||
      maps_have("[anon:<fault>]") != 1)
    return 10;
  if (munmap(Region, 12288))
    return 11;
  Region = mmap((void *)0x30000000, 12288, 3, 34, -1, 0);
  if (Region != (void *)0x30000000 || maps_have("[anon:") != 0)
    return 12;
  return munmap(Region, 12288) ? 13 : 0;
}

static U64 NamedThreadPage;
static void *named_child(void *Unused) {
  char Local;
  (void)Unused;
  NamedThreadPage = (U64)&Local & ~4095UL;
  return (void *)(I64)prctl(0x53564d41, 0UL, NamedThreadPage, 4096UL,
                            "retired-child");
}

U64 proc_prctl_thread_retirement(void) {
  U64 Thread;
  void *Value;
  if (pthread_create(&Thread, 0, named_child, 0) ||
      pthread_join(Thread, &Value) || Value)
    return 1;
  void *Region = mmap((void *)NamedThreadPage, 4096, 3, 34, -1, 0);
  if (Region != (void *)NamedThreadPage)
    return 2;
  if (maps_have("[anon:retired-child]") != 0)
    return 3;
  return munmap(Region, 4096) ? 4 : 0;
}

U64 proc_prctl_missing(void) { return prctl(0x53564d41, 0UL, 0UL, 0UL, 0UL); }

U64 proc_prctl_unknown(void) { return prctl(15, 0UL, 0UL, 0UL, 0UL); }

U64 proc_write_stat(void) {
  int FD = open("/fixture/value", 2 | 524288);
  if (FD != 3)
    return 1;
  int Alias = dup(FD);
  if (Alias != 4 || lseek(Alias, 8192, 0) != 8192)
    return 2;
  if (write(FD, "XY", 2) != 2)
    return 3;
  U64 Stat[16];
  // Keep this OS fixture's independent checks scalar. Some AArch64 compilers
  // combine adjacent comparisons into SIMD even with vectorization disabled.
  const volatile U64 *Observed = Stat;
  if (fstat(Alias, Stat) || Observed[6] != 8194 || Observed[8] != 16 ||
      Observed[11] != 100 || Observed[12] != 7 || Observed[13] != 100 ||
      Observed[14] != 7)
    return 4;
  char Buffer[160];
  int Info = open("/proc/self/fdinfo/4", 0);
  I64 N = read(Info, Buffer, sizeof(Buffer) - 1);
  if (N < 0)
    return 5;
  Buffer[N] = 0;
  if (!equal(Buffer, "pos:\t8194\nflags:\t0400002\nmnt_id:\t1\nino:\t42\n"))
    return 6;
  if (close(Info) || close(FD) || close(Alias))
    return 7;
  return 0;
}
