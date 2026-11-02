// Independent API 28 LP64 guest-thread workloads. No Android headers or host
// threading implementation is linked into these fixtures.
typedef unsigned long u64;
typedef unsigned int u32;
typedef struct {
  u32 flags, padding;
  u64 stack, size, guard;
  int policy, priority;
  unsigned char reserved[16];
} Attr;
extern int pthread_create(u64 *, const Attr *, void *(*)(void *), void *);
extern int pthread_join(u64, void **);
extern int pthread_detach(u64);
extern u64 pthread_self(void);
extern int pthread_equal(u64, u64);
extern int pthread_getattr_np(u64, Attr *);
extern int pthread_gettid_np(u64);
extern void pthread_exit(void *) __attribute__((noreturn));
extern int pthread_attr_init(Attr *);
extern int pthread_attr_setdetachstate(Attr *, int);
extern int pthread_attr_setstacksize(Attr *, u64);
extern int pthread_attr_setstack(Attr *, void *, u64);
extern int pthread_attr_setguardsize(Attr *, u64);
extern int pthread_once(int *, void (*)(void));
extern int pthread_mutex_init(void *, const void *);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
extern int *__errno(void);
extern int gettid(void);
extern int getpid(void);
extern long syscall(long, ...);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern char *dlerror(void);
extern int __cxa_atexit(void (*)(void *), void *, void *);
extern void __cxa_finalize(void *);
extern int munmap(void *, u64);

static u64 *Shared;

static u64 *WaitOutput;
static int WaitControl, InnerControl;
static u64 WaitMode;
static int (*OnceCall)(int *, void (*)(void));
static int direct_once(int *control, void (*initializer)(void)) {
  return pthread_once(control, initializer);
}
static u64 load_flag(u64 index) {
  return __atomic_load_n(WaitOutput + index, __ATOMIC_ACQUIRE);
}
static void store_flag(u64 index, u64 value) {
  __atomic_store_n(WaitOutput + index, value, __ATOMIC_RELEASE);
}
static void wrong_initializer(void) { store_flag(7, 1); }
static void waited_inner(void) {
  ++WaitOutput[3];
  store_flag(0, 1);
  while (!load_flag(2))
    gettid();
  WaitOutput[4] = 0x123456789abcdef0UL;
}
static void waited_outer(void) {
  ++WaitOutput[1];
  OnceCall(&InnerControl, waited_inner);
  WaitOutput[5] = 0xfedcba9876543210UL;
}
static void *once_waiter(void *arg) {
  u64 index = (u64)arg, *row = WaitOutput + index * 32;
  const u64 cookie = 0xfeedface0000UL + index;
  volatile u64 local = cookie;
  register u64 held __asm__("x19") = cookie;
  *__errno() = 70 + index;
  row[5] = gettid();
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(row[6]));
  __asm__ volatile("fmov d8, %1" : "+r"(held) : "r"(cookie) : "d8");
  store_flag(index * 32, 1);
  const int inner = WaitMode == 1 && index == 2;
  row[1] = OnceCall(inner ? &InnerControl : &WaitControl,
                    index == 1 ? waited_outer : wrong_initializer);
  __asm__ volatile("fmov %1, d8" : "+r"(held), "=r"(row[9]));
  row[8] = held;
  row[2] = WaitOutput[inner ? 4 : 5];
  row[3] = local;
  row[4] = *__errno();
  row[7] = 1;
  return (void *)cookie;
}
u64 threads_once_wait(u64 *out, u64 mode) {
  WaitOutput = out;
  WaitMode = mode;
  OnceCall = direct_once;
  if (mode == 2) {
    void *library = dlopen("libc.so", 0);
    OnceCall = (int (*)(int *, void (*)(void)))dlsym(library, "pthread_once");
    if (!OnceCall)
      return 1;
  }
  u64 threads[3];
  if (pthread_create(threads, 0, once_waiter, (void *)1))
    return 2;
  while (!load_flag(0))
    gettid();
  for (u64 i = 2; i <= 3; ++i)
    if (pthread_create(threads + i - 1, 0, once_waiter, (void *)i))
      return 3;
  while (!load_flag(64) || !load_flag(96))
    gettid();
  store_flag(2, 1);
  out[6] = OnceCall(&WaitControl, wrong_initializer);
  for (u64 i = 1; i <= 3; ++i) {
    void *result;
    if (pthread_join(threads[i - 1], &result) ||
        result != (void *)(0xfeedface0000UL + i))
      return 4;
  }
  out[8] = WaitControl;
  out[9] = InnerControl;
  // Completed controls ignore the initializer argument, including an
  // otherwise invalid address. No additional callback may be invoked.
  return OnceCall(&WaitControl, (void (*)(void))3);
}

static int *FaultControl;
static u64 EntryHandle;
static void fault_initializer(void) {
  store_flag(0, 1);
  if (WaitMode == 1 || WaitMode == 8) {
    if (WaitMode == 8)
      *FaultControl = 0;
    pthread_once(FaultControl, wrong_initializer);
  } else if (WaitMode == 2) {
    pthread_join(EntryHandle, 0);
  } else {
    while (!load_flag(2))
      gettid();
    if (WaitMode == 4)
      munmap(FaultControl, 4096);
  }
}
static void *fault_owner(void *arg) {
  (void)arg;
  pthread_once(FaultControl, fault_initializer);
  return 0;
}
static void *control_mutator(void *arg) {
  (void)arg;
  while (__atomic_load_n(FaultControl, __ATOMIC_ACQUIRE) != 2)
    gettid();
  if (WaitMode == 5)
    __atomic_store_n(FaultControl, 3, __ATOMIC_RELEASE);
  else
    munmap(FaultControl, 4096);
  return 0;
}
static void cycle_inner(void) {
  store_flag(3, 1);
  while (!load_flag(0))
    gettid();
  pthread_once(&WaitControl, wrong_initializer);
}
static void cycle_outer(void) {
  store_flag(0, 1);
  while (!load_flag(3))
    gettid();
  pthread_once(&InnerControl, wrong_initializer);
}
static void *cycle_worker(void *arg) {
  if (arg)
    pthread_once(&WaitControl, cycle_outer);
  else
    pthread_once(&InnerControl, cycle_inner);
  return 0;
}
u64 threads_once_failure(u64 *out, u64 mode) {
  WaitOutput = out;
  WaitMode = mode;
  FaultControl = (int *)(out + 512);
  if (!mode) {
    *FaultControl = 1;
    return pthread_once(FaultControl, wrong_initializer);
  }
  if (mode == 1 || mode == 8)
    return pthread_once(FaultControl, fault_initializer);
  u64 first, second;
  if (mode == 3) {
    pthread_create(&first, 0, cycle_worker, (void *)1);
    pthread_create(&second, 0, cycle_worker, 0);
    return pthread_join(first, 0);
  }
  EntryHandle = pthread_self();
  pthread_create(&first, 0, fault_owner, 0);
  if (mode == 5 || mode == 6)
    pthread_create(&second, 0, control_mutator, 0);
  while (!load_flag(0))
    gettid();
  if (mode != 7)
    store_flag(2, 1);
  return pthread_once(FaultControl, wrong_initializer);
}
static u64 tls(void) {
  u64 value;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
  return value;
}
static u64 raw_call(u64 number, u64 arg) {
  register u64 x8 __asm__("x8") = number;
  register u64 x0 __asm__("x0") = arg;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
  return x0;
}
static void inner_once(void) { ++Shared[(gettid() - 1000) * 32 + 14]; }
static void outer_once(void) {
  int inner = 0;
  pthread_once(&inner, inner_once);
  ++Shared[(gettid() - 1000) * 32 + 14];
}
static void finalize(void *arg) {
  u64 *row = arg;
  row[15] = (u64)gettid();
}
static void register_window(u64 *row, u64 identity) {
  u64 seed = 0x0123456789abcdefUL + identity;
  u64 rounding = identity << 22, status = identity;
  // Branches do not change NZCV. Each quantum must preserve the full vector,
  // scalar loop counter, condition flags and FP controls independently.
  __asm__ volatile("dup v16.2d, %1\n"
                   "msr fpcr, %2\nmsr fpsr, %3\ncmp %3, #1\n"
                   "mov x9, #113\n1: sub x9, x9, #1\ncbnz x9, 1b\n"
                   "str q16, [%0]\nmrs x10, fpcr\nstr x10, [%0, #16]\n"
                   "mrs x10, fpsr\nstr x10, [%0, #24]\n"
                   "cset x10, eq\nstr x10, [%0, #32]\n"
                   "msr fpcr, xzr\nmsr fpsr, xzr"
                   :
                   : "r"(row + 24), "r"(seed), "r"(rounding), "r"(status)
                   : "x9", "x10", "v16", "memory", "cc");
}
static void *worker(void *arg) {
  u64 *row = arg;
  const u64 identity = row[31];
  volatile u64 local = 0xfeed0000 + identity;
  row[0] = pthread_self();
  row[1] = gettid();
  row[2] = raw_call(178, 0);
  row[3] = syscall(178);
  row[4] = getpid();
  row[5] = tls();
  row[6] = (u64)__errno();
  row[7] = *__errno();
  row[9] = (u64)&local;
  row[10] = ((u64 *)tls())[1];
  row[12] = (u64)dlerror();
  *__errno() = 100 + identity;
  dlopen("missing-child.so", 0);
  while (!__atomic_load_n(Shared, __ATOMIC_ACQUIRE)) {
  }
  register_window(row, identity);
  row[11] = (u64)dlerror();
  row[8] = *__errno();
  row[16] = (u64)dlerror();
  u64 mutex[5] = {0x4000, 0, 0, 0, 0};
  pthread_mutex_lock(mutex);
  row[13] = ((u32 *)mutex)[1];
  pthread_mutex_unlock(mutex);
  int once = 0;
  pthread_once(&once, outer_once);
  pthread_once(&once, outer_once);
  __cxa_atexit(finalize, row, row);
  __cxa_finalize(row);
  Attr attr;
  pthread_getattr_np(pthread_self(), &attr);
  row[20] = attr.stack;
  row[21] = attr.size;
  row[22] = attr.guard;
  row[23] = local;
  return (void *)(0xfedc000000000000UL + identity);
}
u64 threads_identity(u64 *out) {
  Shared = out;
  out[1] = (u64)__errno();
  out[2] = tls();
  out[5] = gettid();
  out[11] = pthread_self();
  *__errno() = 77;
  dlopen("missing-parent.so", 0);
  out[32 + 31] = 1;
  out[64 + 31] = 2;
  if (pthread_create(out + 6, 0, worker, out + 32))
    return 1;
  if (pthread_create(out + 7, 0, worker, out + 64))
    return 2;
  out[12] = pthread_gettid_np(out[6]);
  __atomic_store_n(out, 1, __ATOMIC_RELEASE);
  out[10] = pthread_join(out[6], (void **)(out + 8));
  out[10] |= pthread_join(out[7], (void **)(out + 9));
  out[3] = *__errno();
  out[4] = (u64)dlerror();
  out[13] = pthread_equal(pthread_self(), out[11]);
  return 0;
}
static void *short_worker(void *arg) {
  u64 *out = arg;
  out[2] = gettid();
  return (void *)0x1234567800000042UL;
}
u64 threads_detached(u64 *out, u64 mode) {
  Attr attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, 1);
  out[0] = pthread_create(out + 1, mode ? 0 : &attr, short_worker, out);
  if (out[0])
    return out[0];
  if (mode)
    out[3] = pthread_detach(out[1]);
  out[4] = pthread_join(out[1], 0);
  out[5] = pthread_detach(out[1]);
  return 73;
}
static void *exit_worker(void *arg) {
  u64 mode = (u64)arg;
  if (mode == 0)
    pthread_exit((void *)0xfedcba9876543210UL);
  if (mode == 1)
    syscall(93, 7);
  if (mode == 2)
    raw_call(93, 9);
  if (mode == 3)
    raw_call(94, 37);
  if (mode == 4)
    syscall(94, 38);
  return (void *)1;
}
u64 threads_exit(u64 *out, u64 mode) {
  out[0] = pthread_create(out + 1, 0, exit_worker, (void *)mode);
  if (out[0])
    return out[0];
  out[2] = pthread_join(out[1], (void **)(out + 3));
  out[4] = 1;
  return 74;
}
// A thread's terminal return has no guest caller frame to resume. Deliberately
// retain a frame to distinguish that boundary from an ordinary callback.
__attribute__((naked)) void *threads_return_with_stack_residue(void *value) {
  __asm__ volatile("sub sp, sp, #16\nret");
}
__attribute__((naked)) static void *exit_with_stack_residue(void *value) {
  __asm__ volatile("sub sp, sp, #16\nb pthread_exit");
}
u64 threads_terminal_return(u64 *out, u64 mode) {
  Attr attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, (mode & 2) != 0);
  out[0] = pthread_create(out + 1, &attr,
                          mode & 1 ? exit_with_stack_residue
                                   : threads_return_with_stack_residue,
                          (void *)0xfedcba9876543210UL);
  if (!out[0] && !(mode & 2))
    out[2] = pthread_join(out[1], (void **)(out + 3));
  return 80;
}
__attribute__((naked)) static void callback_with_stack_residue(void) {
  __asm__ volatile("sub sp, sp, #16\nret");
}
u64 threads_callback_stack(void) {
  int control = 0;
  pthread_once(&control, callback_with_stack_residue);
  return 81;
}
u64 threads_limits(u64 *out, u64 mode) {
  Attr attr;
  pthread_attr_init(&attr);
  if (mode == 1)
    pthread_attr_setstacksize(&attr, ~0UL);
  if (mode == 2)
    pthread_attr_setstack(&attr, (void *)0x20000000, 16384);
  if (mode == 3)
    attr.policy = 2;
  if (mode == 4)
    pthread_attr_setguardsize(&attr, ~0UL);
  if (mode == 5)
    attr.flags |= 0x100;
  out[1] = 0x12345678;
  *__errno() = 91;
  out[0] = pthread_create(out + 1, &attr, short_worker, out);
  if (mode == 0 && !out[0]) {
    out[3] = pthread_join(out[1], 0);
    out[4] = pthread_create(out + 5, 0, short_worker, out);
  }
  out[6] = *__errno();
  return 0;
}
u64 threads_invalid(u64 *out, u64 mode) {
  if (mode == 0)
    return pthread_join(pthread_self(), 0);
  if (mode == 1)
    return pthread_join(0, 0);
  if (mode == 2)
    return pthread_getattr_np(1, (Attr *)out);
  if (mode == 3)
    return pthread_create((u64 *)1, 0, short_worker, out);
  if (mode == 4)
    return pthread_create(out, 0, (void *(*)(void *))1, out);
  if (mode == 5)
    return pthread_create(out, 0, (void *(*)(void *))out, out);
  return pthread_create(out, (Attr *)1, short_worker, out);
}
u64 threads_dynamic(u64 *out, u64 closed) {
  void *lib = dlopen("libthread-model.so", 0);
  int (*create)(u64 *, const Attr *, void *(*)(void *), void *) =
      dlsym(lib, "pthread_create");
  int (*join)(u64, void **) = dlsym(lib, "pthread_join");
  if (closed)
    dlclose(lib);
  out[0] = create(out + 1, 0, short_worker, out);
  if (!out[0])
    out[3] = join(out[1], (void **)(out + 4));
  return 75;
}
u64 threads_null_tid(u64 *out, u64 dynamic) {
  int (*query)(u64) = pthread_gettid_np;
  if (dynamic) {
    void *lib = dlopen("libthread-model.so", 0);
    query = dlsym(lib, "pthread_gettid_np");
    if (!lib || !query)
      return 99;
  }
  *__errno() = 91;
  int tid = query(0);
  out[0] = (u32)tid;
  out[1] = (u32)*__errno();
  return tid == -1 ? 0 : 1;
}
static void *join_peer(void *arg) {
  u64 *out = arg;
  while (!__atomic_load_n(out + 1, __ATOMIC_ACQUIRE)) {
  }
  out[2] = pthread_join(out[0], 0);
  return 0;
}
u64 threads_join_cycle(u64 *out) {
  out[0] = pthread_self();
  u64 child;
  pthread_create(&child, 0, join_peer, out);
  __atomic_store_n(out + 1, 1, __ATOMIC_RELEASE);
  return pthread_join(child, 0);
}
u64 threads_join_finished(u64 *out, u64 detach) {
  pthread_create(out + 1, 0, short_worker, out);
  // Enough guest work lets the child return before the parent's query.
  for (volatile unsigned i = 0; i < 400; ++i) {
  }
  out[3] = pthread_gettid_np(out[1]);
  return detach ? pthread_detach(out[1])
                : pthread_join(out[1], (void **)(out + 4));
}
static void *guard_worker(void *arg) {
  Attr attr;
  pthread_getattr_np(pthread_self(), &attr);
  *(volatile unsigned char *)attr.stack = 1;
  return arg;
}
static void exiting_once(void) { pthread_exit((void *)9); }
static void *callback_exit_worker(void *arg) {
  int control = 0;
  pthread_once(&control, exiting_once);
  return arg;
}
static void *bad_tls_worker(void *arg) {
  ((volatile u64 *)tls())[1] = 1;
  return (void *)pthread_self();
}
u64 threads_boundary(u64 *out, u64 mode) {
  void *(*start)(void *) = mode == 0   ? guard_worker
                           : mode == 1 ? callback_exit_worker
                                       : bad_tls_worker;
  pthread_create(out + 1, 0, start, out);
  return pthread_join(out[1], 0);
}
u64 threads_last_exit(u64 *out, u64 mode) {
  (void)out;
  if (mode)
    syscall(93, 263);
  else
    raw_call(93, 264);
  return 1;
}

static long raw_priority(u64 number, u64 who, long nice) {
  register u64 x8 __asm__("x8") = number;
  register u64 x0 __asm__("x0") = 0;
  register u64 x1 __asm__("x1") = who;
  register long x2 __asm__("x2") = nice;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x8)
                   : "memory", "cc");
  return (long)x0;
}
static void *priority_grandchild(void *arg) {
  u64 *row = arg;
  row[0] = gettid();
  row[1] = raw_priority(141, 0, 0);
  row[2] = raw_priority(140, 0, 12);
  row[3] = raw_priority(141, 0, 0);
  return 0;
}
static void *priority_child(void *arg) {
  u64 *out = arg, *row = out + 32;
  while (!__atomic_load_n(out + 15, __ATOMIC_ACQUIRE))
    gettid();
  row[0] = gettid();
  row[1] = raw_priority(141, 0, 0);
  row[2] = raw_priority(140, 0, 9);
  row[3] = raw_priority(141, 0, 0);
  *__errno() = 0;
  row[4] = syscall(140, 0, 0, 0);
  row[5] = *__errno();
  row[6] = raw_priority(141, 0, 0);
  u64 grandchild;
  row[7] = pthread_create(&grandchild, 0, priority_grandchild, out + 64);
  if (row[7])
    return (void *)1;
  row[8] = pthread_join(grandchild, 0);
  row[9] = raw_priority(141, 0, 0);
  row[10] = raw_priority(141, out[64], 0);
  return 0;
}
u64 threads_priority(u64 *out, u64 mode) {
  if (!mode) {
    out[0] = raw_priority(140, 0, 3);
    out[1] = raw_priority(141, 0, 0);
  }
  out[4] = gettid();
  out[5] = pthread_create(out + 16, 0, priority_child, out);
  if (out[5])
    return 1;
  if (!mode) {
    out[2] = raw_priority(140, 0, 5);
    out[3] = raw_priority(141, 0, 0);
  }
  __atomic_store_n(out + 15, 1, __ATOMIC_RELEASE);
  out[6] = pthread_join(out[16], 0);
  out[7] = raw_priority(141, out[32], 0);
  out[8] = raw_priority(140, out[32], 19);
  out[9] = raw_priority(141, 0, 0);
  *__errno() = 0;
  out[10] = syscall(141, 0, out[32]);
  out[11] = *__errno();
  return 0;
}

static u64 *TaskOutput;
static u64 raw_pidfd(u64 pid, u64 flags) {
  register u64 x0 __asm__("x0") = pid;
  register u64 x1 __asm__("x1") = flags;
  register u64 x8 __asm__("x8") = 434;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
  return x0;
}
static void *task_catalogue_child(void *unused) {
  (void)unused;
  u64 *out = TaskOutput;
  out[16] = gettid();
  out[17] = raw_pidfd(out[16], 0);
  out[18] = syscall(434, out[16], 0UL);
  out[19] = *__errno();
  out[20] = raw_pidfd(2000, 0);
  out[21] = raw_call(57, out[20]);
  out[22] = syscall(113, (long)((~out[16] << 3) | 2), out + 24);
  out[26] = syscall(113, -16006L, out + 27);
  __atomic_store_n(out, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(out + 6, __ATOMIC_ACQUIRE))
    gettid();
  return 0;
}
u64 threads_task_catalogue(u64 *out, u64 mode) {
  TaskOutput = out;
  u64 child;
  if (pthread_create(&child, 0, task_catalogue_child, 0))
    return 1;
  while (!__atomic_load_n(out, __ATOMIC_ACQUIRE))
    gettid();
  out[1] = raw_pidfd(2000, 0);
  out[2] = raw_call(57, out[1]);
  out[3] = raw_pidfd(out[16], 0);
  out[4] = raw_pidfd(3000, 0);
  out[5] = raw_pidfd(1002, 0);
  out[30] = raw_pidfd(out[16], ~(u64)0);
  if (mode)
    out[31] = raw_pidfd(mode == 1 ? out[16] : 1000, 0x80);
  __atomic_store_n(out + 6, 1, __ATOMIC_RELEASE);
  out[7] = pthread_join(child, 0);
  out[8] = raw_pidfd(out[16], 0);
  out[9] = raw_pidfd(2000, 0);
  out[10] = raw_call(57, out[9]);
  out[11] = raw_pidfd(1000, 0);
  out[12] = raw_call(57, out[11]);
  out[13] = gettid();
  out[14] = syscall(113, (long)((~out[16] << 3) | 2), out + 32);
  out[15] = *__errno();
  return 0;
}
