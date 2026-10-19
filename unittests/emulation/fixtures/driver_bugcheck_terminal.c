// The imported crash request must never return to the following volatile store.
typedef unsigned int U32;
typedef unsigned long long U64;
#define API __declspec(dllimport)
API void KeBugCheck(U32);
API void KeBugCheckEx(U32, U64, U64, U64, U64);
API unsigned char KfRaiseIrql(unsigned char);
API U32 PsCreateSystemThread(U64 *, U32, void *, U64, void *, void (*)(void *),
                             void *);
API U32 ZwClose(U64);
API unsigned char
KeRegisterBugCheckReasonCallback(void *, void (*)(U32, void *, void *, U32),
                                 U32, char *);
static U64 Record[6];
static char Component[] = "NeverDCrash";
static void reason(U32 Reason, void *R, void *Data, U32 Size) {
  (void)Reason;
  (void)R;
  (void)Data;
  (void)Size;
  // Crash/dump callback execution is explicitly outside the terminal-stop
  // contract, and must never be fabricated as part of reporting the request.
  *(volatile U32 *)0xdead1000ULL = 1;
}
static void crash(void) {
#ifdef SIMPLE_BUGCHECK
  KeBugCheck(0xdead0042);
#else
  KeBugCheckEx(0xdead0042, 0x1122334455667788ULL, ~(U64)0, 0,
               0x8000000000000001ULL);
#endif
  *(volatile U32 *)0xdead0000ULL = 1;
}
static void unload(void *Driver) {
  (void)Driver;
#ifdef BUGCHECK_ON_UNLOAD
  crash();
#endif
}
static void worker(void *Context) {
  (void)Context;
  crash();
}
U32 DriverEntry(void *Driver, void *Path) {
  (void)Path;
  if (!KeRegisterBugCheckReasonCallback(Record, reason, 2, Component))
    return 0xc0000001;
  *(U64 *)((unsigned char *)Driver + 0x68) = (U64)unload;
#ifdef BUGCHECK_IN_THREAD
  U64 Handle = 0;
  U32 Status = PsCreateSystemThread(&Handle, 0x1fffff, 0, 0, 0, worker, 0);
  if (Status)
    return Status;
  return ZwClose(Handle);
#elif defined(BUGCHECK_ON_UNLOAD)
  return 0;
#else
  KfRaiseIrql(15);
#ifdef BUGCHECK_IN_SEH
  __try {
    crash();
  } __except (1) {
    return 0xc0000001;
  }
#else
  crash();
#endif
  return 0xc0000001;
#endif
}
