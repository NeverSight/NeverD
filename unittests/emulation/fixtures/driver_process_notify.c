//===- driver_process_notify.c - Real legacy process callbacks ------------===//
// NeverD Decompiler
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
#define API __declspec(dllimport)
API U32 PsSetCreateProcessNotifyRoutine(void (*)(U64, U64, U8), U8);
API U64 PsGetCurrentProcessId(void);
API U64 PsGetCurrentThreadId(void);
API U64 PsGetCurrentThread(void);
API U64 PsGetThreadProcessId(U64);
API U8 ExGetPreviousMode(void);
API U8 KeGetCurrentIrql(void);
API U8 KeAreApcsDisabled(void);
API U8 KeAreAllApcsDisabled(void);
API void KeEnterCriticalRegion(void);
API void KeLeaveCriticalRegion(void);
API U32 PsLookupProcessByProcessId(U64, U64 *);
API void ObfDereferenceObject(U64);
static U32 Count, Bad;
static U64 PairThread;
static void check(U64 Parent, U64 PID, U8 Create, U32 Which) {
  U64 Object = 0;
  const U32 Event = Count / 2;
  const U64 ExpectedPID = Event == 0 || Event == 3 ? 100 : 200;
  const U64 Actor = Event == 0 ? 4 : Event == 1 ? 100 : Event == 2 ? 200 : 100;
  if (Count >= 8 || Count % 2 != Which || Parent != 4 || PID != ExpectedPID ||
      !!Create != (Event < 2) || PsGetCurrentProcessId() != Actor ||
      PsGetThreadProcessId(PsGetCurrentThread()) != Actor ||
      ExGetPreviousMode() || KeGetCurrentIrql() || !KeAreApcsDisabled() ||
      KeAreAllApcsDisabled())
    Bad = 1;
  if (!Which)
    PairThread = PsGetCurrentThreadId();
  else if (PsGetCurrentThreadId() != PairThread)
    Bad = 1;
  KeEnterCriticalRegion();
  KeLeaveCriticalRegion();
  if (!KeAreApcsDisabled())
    Bad = 1;
  if (PsLookupProcessByProcessId(PID, &Object))
    Bad = 1;
  else
    ObfDereferenceObject(Object);
  if (Event == 3 && PsLookupProcessByProcessId(200, &Object) != 0xc000000bU)
    Bad = 1;
  ++Count;
}
static void first(U64 Parent, U64 PID, U8 Create) {
  check(Parent, PID, Create, 0);
}
static void second(U64 Parent, U64 PID, U8 Create) {
  check(Parent, PID, Create, 1);
}
static void unload(U64 Driver) {
  U64 Object = 0;
  (void)Driver;
  if (Count != 8 || Bad || KeAreApcsDisabled() || KeAreAllApcsDisabled() ||
      PsLookupProcessByProcessId(100, &Object) != 0xc000000bU ||
      PsLookupProcessByProcessId(200, &Object) != 0xc000000bU ||
      PsSetCreateProcessNotifyRoutine(first, 1) ||
      PsSetCreateProcessNotifyRoutine(second, 1))
    *(volatile U32 *)0 = 0xbad;
}
U32 DriverEntry(U64 Driver, U64 Registry) {
  (void)Registry;
  *(U64 *)(Driver + 0x68) = (U64)unload;
  if (PsSetCreateProcessNotifyRoutine(first, 0) ||
      PsSetCreateProcessNotifyRoutine(second, 0) || Count)
    return 0xc0000001U;
  return 0;
}
