//===- unpack_dynamic_tls_state.h - Independent dynamic slot state -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
__declspec(dllimport) U32 TlsAlloc(void);
__declspec(dllimport) int TlsFree(U32);
__declspec(dllimport) void *TlsGetValue(U32);
__declspec(dllimport) int TlsSetValue(U32, void *);
__declspec(dllimport) U32 FlsAlloc(void *);
__declspec(dllimport) int FlsFree(U32);
__declspec(dllimport) void *FlsGetValue(U32);
__declspec(dllimport) int FlsSetValue(U32, void *);
static U32 DynamicSlot;
static volatile U64 FLSCallbackValue;
static U32 FLSCallbackMode, FLSCallbackFailure, FLSCallbackCount;
static volatile U64 NestedFLSCallbackValue;

static void cleanupNestedFLS(void *Value) {
  NestedFLSCallbackValue = (U64)Value;
  void *Heap = GetProcessHeap();
  U64 *Memory = HeapAlloc(Heap, 0, sizeof(U64));
  if (!Memory) {
    FLSCallbackFailure = 1;
    return;
  }
  *Memory = (U64)Value;
  FLSCallbackFailure |= !HeapFree(Heap, 0, Memory);
}
static void cleanupFLS(void *Value) {
  FLSCallbackValue = (U64)Value;
  ++FLSCallbackCount;
  FLSCallbackFailure |= FlsGetValue(DynamicSlot) != 0;
  if (FLSCallbackMode == RearmedFLSCallbackMode ||
      FLSCallbackMode == EndlessFLSCallbackMode) {
    FLSCallbackFailure |= (U64)Value != InitializeResult + FLSCallbackCount - 1;
    const int Again = FLSCallbackCount < FLSRearmCount ||
                      FLSCallbackMode == EndlessFLSCallbackMode;
    FLSCallbackFailure |=
        !FlsSetValue(DynamicSlot, Again ? (void *)((U64)Value + 1) : 0);
    return;
  }
  if (FLSCallbackMode == RecursiveFLSCallbackMode) {
    FlsFree(DynamicSlot);
    return;
  }
  if (FLSCallbackMode != NestedFLSCallbackMode)
    return;
  FLSCallbackFailure |= !FlsSetValue(DynamicSlot, 0);
  U32 Nested = FlsAlloc((void *)cleanupNestedFLS);
  if (Nested == 0xffffffffU || Nested == DynamicSlot ||
      !FlsSetValue(Nested, (void *)((U64)Value + 1)) || !FlsFree(Nested)) {
    FLSCallbackFailure = 1;
    return;
  }
  FLSCallbackValue += NestedFLSCallbackValue;
}
static int callbackFLSMode(U32 Mode) {
  return Mode >= FreedFLSCallbackMode && Mode <= EndlessFLSCallbackMode;
}

static int dynamicMode(U32 Mode) {
  return Mode >= DynamicTLSMode && Mode <= ClearedUnallocatedTLSValueMode;
}
static int fiberMode(U32 Mode) {
  return Mode == DynamicFLSMode || Mode == ReleasedDynamicFLSMode ||
         Mode == LateDynamicFLSMode || Mode == ZeroDynamicFLSMode;
}
static int lateDynamicMode(U32 Mode) {
  return Mode == LateDynamicTLSMode || Mode == LateDynamicFLSMode;
}
static int releasedDynamicMode(U32 Mode) {
  return Mode == ReleasedDynamicTLSMode || Mode == ReleasedDynamicFLSMode;
}
static U64 dynamicValue(U32 Mode) {
  return Mode == ZeroDynamicTLSMode || Mode == ZeroDynamicFLSMode
             ? 0
             : InitializeResult;
}
static void createDynamicState(U32 Mode) {
  const int Fiber = fiberMode(Mode);
  DynamicSlot = Fiber ? FlsAlloc(0) : TlsAlloc();
  if (DynamicSlot == 0xffffffffU ||
      !(Fiber ? FlsSetValue(DynamicSlot, (void *)dynamicValue(Mode))
              : TlsSetValue(DynamicSlot, (void *)dynamicValue(Mode))))
    ExitProcess(FailureStatus);
}
static void prepareDynamicState(U32 Mode) {
  if (callbackFLSMode(Mode)) {
    FLSCallbackMode = Mode;
    DynamicSlot = FlsAlloc((void *)cleanupFLS);
    if (DynamicSlot == 0xffffffffU ||
        !FlsSetValue(DynamicSlot, Mode == EmptyFLSCallbackMode ||
                                          Mode == ReusedFLSCallbackMode
                                      ? 0
                                      : (void *)(U64)InitializeResult) ||
        !FlsFree(DynamicSlot))
      ExitProcess(FailureStatus);
    if (Mode == ReusedFLSCallbackMode) {
      DynamicSlot = FlsAlloc(0);
      if (DynamicSlot == 0xffffffffU ||
          !FlsSetValue(DynamicSlot, (void *)(U64)InitializeResult) ||
          !FlsFree(DynamicSlot))
        ExitProcess(FailureStatus);
    }
    return;
  }
  if (Mode == UnallocatedTLSValueMode ||
      Mode == ClearedUnallocatedTLSValueMode) {
    DynamicSlot = 63;
    if (!TlsSetValue(DynamicSlot, (void *)(U64)InitializeResult) ||
        (Mode == ClearedUnallocatedTLSValueMode &&
         !TlsSetValue(DynamicSlot, 0)))
      ExitProcess(FailureStatus);
    return;
  }
  if (!dynamicMode(Mode) || lateDynamicMode(Mode))
    return;
  createDynamicState(Mode);
  if (releasedDynamicMode(Mode)) {
    if (!(fiberMode(Mode) ? FlsFree(DynamicSlot) : TlsFree(DynamicSlot)))
      ExitProcess(FailureStatus);
    DynamicSlot = 0;
  }
}
static int validDynamicState(U32 Mode) {
  if (Mode == RearmedFLSCallbackMode)
    return !FLSCallbackFailure && FLSCallbackCount == FLSRearmCount &&
           FLSCallbackValue == InitializeResult + FLSRearmCount - 1;
  if (callbackFLSMode(Mode))
    return !FLSCallbackFailure &&
           FLSCallbackValue ==
               (Mode == NestedFLSCallbackMode  ? 2 * InitializeResult + 1
                : Mode == FreedFLSCallbackMode ? InitializeResult
                                               : 0);
  if (!dynamicMode(Mode) || Mode == ClearedUnallocatedTLSValueMode)
    return 1;
  if (lateDynamicMode(Mode) || releasedDynamicMode(Mode))
    createDynamicState(Mode);
  const U64 Value = (U64)(fiberMode(Mode) ? FlsGetValue(DynamicSlot)
                                          : TlsGetValue(DynamicSlot));
  return Value == dynamicValue(Mode);
}
