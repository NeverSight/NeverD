//===- linux_time.c - Original x64/ARM64 Linux clock workloads -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
extern U64 linux_service(U64, U64, U64, U64);
#if defined(__aarch64__)
enum {
  Sleep = 101,
  GetTV = 169,
  GetClock = 113,
  Protect = 226,
  Write = 64,
  Exit = 93
};
#else
enum {
  Sleep = 35,
  GetTV = 96,
  GetClock = 228,
  Protect = 10,
  Write = 1,
  Exit = 60
};
#endif
static unsigned char Pages[8192] __attribute__((aligned(4096)));
static void finish(U64 Status) {
  linux_service(Exit, Status, 0, 0);
  __builtin_trap();
}
static void check(U64 A, U64 B) {
  if (A != B)
    finish(91);
}
static void output(void *P, U64 N) {
  check(linux_service(Write, 1, (U64)P, N), N);
}
void process_main(U64 *Stack) {
  if (Stack[0] != 2)
    finish(91);
  char Mode = ((const char **)(Stack + 1))[1][0];
  if (Mode == 's') {
    long Request[2] = {-1, 0};
    check(linux_service(Sleep, (U64)Request, 1, 0), (U64)-22);
    check(linux_service(Sleep, 1, 0, 0), (U64)-14);
    Request[0] = 0;
    check(linux_service(Sleep, (U64)Request, (U64)Request, 0), 0);
    Request[0] = 1;
    Request[1] = 7;
    check(linux_service(Sleep, (U64)Request, (U64)Request, 0), 0);
    check(Request[0], 1);
    check(Request[1], 7);
    check(linux_service(GetClock, 0, (U64)Pages, 0), 0);
    check(linux_service(GetClock, 1, (U64)Pages + 16, 0), 0);
    output(Pages, 32);
  } else if (Mode == 'r') {
    for (U64 Phase = 0; Phase != 2; ++Phase) {
      U64 Destination = (U64)Pages + Phase * 48;
      check(linux_service(GetClock, 0, Destination, 0), 0);
      check(linux_service(GetClock, 1, Destination + 16, 0), 0);
      check(linux_service(GetClock, 0x1234567800000004UL, Destination + 32, 0),
            0);
      if (!Phase) {
        long Request[2] = {1, 7};
        check(linux_service(Sleep, (U64)Request, (U64)Request, 0), 0);
        check(Request[0], 1);
        check(Request[1], 7);
      }
    }
    output(Pages, 112);
  } else if (Mode == 'n') {
    check(linux_service(GetTV, (U64)Pages, (U64)Pages + 16, 0), 0);
    check(linux_service(GetClock, 0, (U64)Pages + 24, 0), 0);
    check(linux_service(GetClock, 0x1122334400000001UL, (U64)Pages + 40, 0), 0);
    check(linux_service(GetTV, 0, 0, 0), 0);
    output(Pages, 56);
#if defined(__x86_64__)
    U64 First = linux_service(201, 0, 0, 0);
    check(linux_service(201, (U64)Pages + 64, 0, 0), First);
    check(*(U64 *)(Pages + 64), First);
    output(Pages + 64, 8);
#endif
  } else if (Mode == 'f') {
    check(linux_service(GetClock, 10, 1, 0), (U64)-22);
    check(linux_service(GetClock, 0, 0, 0), (U64)-14);
    check(linux_service(GetTV, 1, 0, 0), (U64)-14);
    check(linux_service(GetTV, (U64)Pages, 1, 0), (U64)-14);
    output(Pages, 16);
#if defined(__x86_64__)
    check(linux_service(201, 1, 0, 0), (U64)-14);
#endif
    check(linux_service(Protect, (U64)Pages + 4096, 4096, 1), 0);
    check(linux_service(GetTV, (U64)Pages + 4088, 0, 0), (U64)-14);
    output(Pages + 4088, 8);
  } else if (Mode == 'm') {
    linux_service(GetClock, 0, (U64)Pages, 0);
    finish(91);
  } else if (Mode == 'd') {
    linux_service(GetClock, (U64)-3, (U64)Pages, 0);
    finish(91);
  } else {
    check(linux_service(Protect, (U64)Pages + 4096, 4096, 1), 0);
    linux_service(GetClock, 0, (U64)Pages + 4088, 0);
    finish(91);
  }
  finish(0);
}
