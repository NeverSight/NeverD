//===- unpack_runtime_state.h - Independent native object lifetime oracle
//--===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
__declspec(dllimport) void *HeapCreate(U32, U64, U64);
__declspec(dllimport) int HeapDestroy(void *);
__declspec(dllimport) void *HeapReAlloc(void *, U32, void *, U64);
__declspec(dllimport) U64 HeapSize(void *, U32, const void *);
__declspec(dllimport) void InitializeCriticalSection(void *);
__declspec(dllimport) void EnterCriticalSection(void *);
__declspec(dllimport) void LeaveCriticalSection(void *);
__declspec(dllimport) void DeleteCriticalSection(void *);
__declspec(dllimport) U32 GetLastError(void);
__declspec(dllimport) void SetLastError(U32);
__declspec(dllimport) void *VirtualAlloc(void *, U64, U32, U32);
__declspec(dllimport) int VirtualFree(void *, U64, U32);
__declspec(dllimport) int VirtualProtect(void *, U64, U32, U32 *);
static U8 *RuntimeVirtual;
static void prepareVirtualState(void) {
  // This positive fixed-address oracle requests the same vacant range from
  // both environments. Automatic placement has no cross-process guarantee.
  RuntimeVirtual = VirtualAlloc((void *)0x2400000000ULL, 0x10000, 0x2000, 4);
  if (!RuntimeVirtual || VirtualAlloc(RuntimeVirtual + 0x1000, 0x1000, 0x1000,
                                      4) != RuntimeVirtual + 0x1000)
    ExitProcess(FailureStatus);
  RuntimeVirtual[0x1123] = 71;
  U32 Old;
  if (!VirtualProtect(RuntimeVirtual + 0x1000, 0x1000, 2, &Old) || Old != 4)
    ExitProcess(FailureStatus);
}
static int checkVirtualState(void) {
  if (RuntimeVirtual[0x1123] != 71)
    return 0;
  U32 Old;
  if (!VirtualProtect(RuntimeVirtual + 0x1000, 0x1000, 4, &Old) || Old != 2)
    return 0;
  RuntimeVirtual[0x1123] = 72;
  if (!VirtualFree(RuntimeVirtual + 0x1000, 0x1000, 0x4000) ||
      VirtualAlloc(RuntimeVirtual + 0x2000, 0x1000, 0x1000, 4) !=
          RuntimeVirtual + 0x2000 ||
      RuntimeVirtual[0x2123])
    return 0;
  return VirtualFree(RuntimeVirtual, 0, 0x8000) != 0;
}
static void *RuntimeHeap;
static U8 *RuntimeBlock;
static void *RuntimeEncoded;
static U64 RuntimeLock[5];
static U32 RuntimeSlot, RuntimeCallbackCount, RuntimeCallbackFailure;
static void runtimeCleanup(void *Value) {
  ++RuntimeCallbackCount;
  RuntimeCallbackFailure |= FlsGetValue(RuntimeSlot) != 0;
  RuntimeCallbackFailure |= (U64)Value != 16 + RuntimeCallbackCount;
  if (RuntimeCallbackCount == 1)
    RuntimeCallbackFailure |= !FlsSetValue(RuntimeSlot, (void *)18);
}
static void prepareRuntimeState(void) {
  RuntimeHeap = HeapCreate(0, 0, 0);
  if (!RuntimeHeap)
    ExitProcess(FailureStatus);
  RuntimeBlock = HeapAlloc(RuntimeHeap, 0, 7);
  if (!RuntimeBlock)
    ExitProcess(FailureStatus);
  for (U32 I = 0; I < 7; ++I)
    RuntimeBlock[I] = (U8)(I * 11 + 3);
  RuntimeEncoded = EncodePointer(RuntimeBlock + 3);
  RuntimeSlot = FlsAlloc((void *)runtimeCleanup);
  if (RuntimeSlot == 0xffffffffU || !FlsSetValue(RuntimeSlot, (void *)17))
    ExitProcess(FailureStatus);
  InitializeCriticalSection(RuntimeLock);
  EnterCriticalSection(RuntimeLock);
  EnterCriticalSection(RuntimeLock);
  SetLastError(12345);
}
static int checkRuntimeState(void) {
  int Good = GetLastError() == 12345;
  Good &= DecodePointer(RuntimeEncoded) == RuntimeBlock + 3;
  Good &= (U64)FlsGetValue(RuntimeSlot) == 17;
  Good &= HeapSize(RuntimeHeap, 0, RuntimeBlock) >= 7;
  U8 *Small = HeapReAlloc(RuntimeHeap, 8, RuntimeBlock, 19);
  if (!Small)
    return 0;
  for (U32 I = 0; I < 7; ++I)
    Good &= Small[I] == (U8)(I * 11 + 3);
  for (U32 I = 7; I < 19; ++I)
    Good &= Small[I] == 0;
  U8 *Large = HeapReAlloc(RuntimeHeap, 8, Small, 8193);
  if (!Large)
    return 0;
  for (U32 I = 0; I < 7; ++I)
    Good &= Large[I] == (U8)(I * 11 + 3);
  for (U32 I = 7; I < 8193; ++I)
    Good &= Large[I] == 0;
  Good &= HeapFree(RuntimeHeap, 0, Large) != 0;
  Good &= HeapDestroy(RuntimeHeap) != 0;
  Good &= FlsFree(RuntimeSlot) != 0;
  Good &= RuntimeCallbackCount == 2 && !RuntimeCallbackFailure;
  LeaveCriticalSection(RuntimeLock);
  LeaveCriticalSection(RuntimeLock);
  DeleteCriticalSection(RuntimeLock);
  return Good;
}
