//===- aarch64_atomic_oracle.c - Native Windows ARM64 LSE observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long long AtomicWord;
#define NEVERD_ATOMIC_VALUE(Name, Value) static const AtomicWord Name = Value;
#include "../aarch64/AArch64AtomicCases.def"
#undef NEVERD_ATOMIC_VALUE
#define NEVERD_ATOMIC_ORACLE_VALUE(Name, Value) enum { Name = Value };
#include "AArch64AtomicOracle.def"
#undef NEVERD_ATOMIC_ORACLE_VALUE
enum {
#define NEVERD_ATOMIC_FAMILY(Name) Name,
#include "../aarch64/AArch64AtomicCases.def"
#undef NEVERD_ATOMIC_FAMILY
};
enum {
#define NEVERD_ATOMIC_ORACLE_FIELD(Name) Field##Name,
#include "AArch64AtomicOracle.def"
#undef NEVERD_ATOMIC_ORACLE_FIELD
  FieldCount
};
typedef struct NativeException {
  unsigned Code, Flags;
  struct NativeException *Nested;
  void *Address;
  unsigned Count, Padding;
  AtomicWord Parameters[MaxParameters];
} NativeException;
typedef struct {
  NativeException *Record;
  unsigned char *Context;
} NativePointers;
__declspec(dllimport) void ExitProcess(unsigned);
__declspec(dllimport) void *GetStdHandle(unsigned);
__declspec(dllimport) int WriteFile(void *, const void *, unsigned, unsigned *,
                                    void *);
__declspec(dllimport) void *VirtualAlloc(void *, AtomicWord, unsigned,
                                         unsigned);
__declspec(dllimport) int VirtualFree(void *, AtomicWord, unsigned);
__declspec(dllimport) int VirtualProtect(void *, AtomicWord, unsigned,
                                         unsigned *);
__declspec(dllimport) void *
AddVectoredExceptionHandler(unsigned, unsigned (*)(NativePointers *));
__declspec(dllimport) unsigned RemoveVectoredExceptionHandler(void *);
static AtomicWord AtomicSite, ResumeSite, Observation[FieldCount];
static unsigned char *Pages;

static void fail(unsigned Site, AtomicWord First, AtomicWord Second) {
  AtomicWord Values[] = {Site, First, Second};
  unsigned Written;
  WriteFile(GetStdHandle(StderrSelector), Values, sizeof(Values), &Written, 0);
  ExitProcess(FailureStatus);
}
static unsigned handler(NativePointers *P) {
  AtomicWord *PC = (AtomicWord *)(P->Context + ContextPCOffset);
  if (*PC != AtomicSite || (AtomicWord)P->Record->Address != *PC ||
      ++Observation[FieldFaults] != 1 || P->Record->Count > MaxParameters)
    fail(SiteHandler, *PC, P->Record->Code);
  Observation[FieldCode] = P->Record->Code;
  Observation[FieldFlags] = P->Record->Flags;
  Observation[FieldParameterCount] = P->Record->Count;
  Observation[FieldExceptionPC] = (AtomicWord)P->Record->Address;
  Observation[FieldContextPC] = *PC;
  AtomicWord *GPR = (AtomicWord *)(P->Context + ContextGPROffset);
  for (unsigned I = 0; I < FieldContextFlags - FieldContext0; ++I)
    Observation[FieldContext0 + I] = GPR[I];
  Observation[FieldContextFlags] =
      *(unsigned *)(P->Context + ContextFlagsOffset) & NZCVMask;
  for (unsigned I = 0; I < P->Record->Count; ++I)
    Observation[FieldParameter0 + I] = P->Record->Parameters[I];
  *PC = ResumeSite;
  return ContinueExecution;
}
static void emit(void) {
  unsigned Written = 0;
  if (!WriteFile(GetStdHandle(StdoutSelector), Observation, sizeof(Observation),
                 &Written, 0) ||
      Written != sizeof(Observation))
    ExitProcess(FailureStatus);
}
#include "aarch64_atomic.inc"
static const struct {
  unsigned Kind, Width, Count;
  void (*Execute)(AtomicWord *, AtomicWord *);
} Cases[] = {
#define NEVERD_ATOMIC_CASE(Name, Kind, Width, Count, Word, Expected)           \
  {Kind, Width, Count, atomic##Name},
#include "../aarch64/AArch64AtomicCases.def"
#undef NEVERD_ATOMIC_CASE
};
static const struct {
  unsigned Offset, Protection, Split, Mismatch;
} Scenarios[] = {
#define NEVERD_ATOMIC_ORACLE_SCENARIO(Name, Offset, Protection, Split,         \
                                      Mismatch)                                \
  {Offset, Protection, Split, Mismatch},
#include "AArch64AtomicOracle.def"
#undef NEVERD_ATOMIC_ORACLE_SCENARIO
};
static AtomicWord readBytes(const unsigned char *Memory, unsigned Width) {
  AtomicWord Value = 0;
  for (unsigned B = 0; B < Width; ++B)
    Value |= (AtomicWord)Memory[B] << (B * ByteBits);
  return Value;
}
static void observe(unsigned Index, unsigned Scenario) {
  const unsigned Width = Cases[Index].Width, Size = Width * Cases[Index].Count;
  const int CompareValue =
      Cases[Index].Kind == Compare || Cases[Index].Kind == ComparePair;
  if ((Scenarios[Scenario].Offset == 1 && Size == Granule) ||
      (Scenarios[Scenario].Offset == 2 && Size == 1) ||
      (Scenarios[Scenario].Mismatch && !CompareValue))
    return;
  AtomicWord *Memory =
      (AtomicWord *)(Pages +
                     (Scenarios[Scenario].Split ? PageBytes - Granule : 0));
  const unsigned Offset = Scenarios[Scenario].Offset == 2
                              ? Granule - Size + 1
                              : Scenarios[Scenario].Offset;
  for (unsigned I = 0; I < MemoryWordCount; ++I)
    Memory[I] = I % 2 ? Operand : Initial;
  for (unsigned I = 0; I < FieldCount; ++I)
    Observation[I] = 0;
  Observation[FieldCaseIndex] = Index;
  Observation[FieldScenario] = Scenario;
  Observation[FieldMemoryBase] = (AtomicWord)Memory;
  const unsigned char *Address = (unsigned char *)Memory + Offset;
  const AtomicWord Mask = Width == sizeof(AtomicWord)
                              ? ~(AtomicWord)0
                              : ((AtomicWord)1 << (Width * ByteBits)) - 1;
  AtomicWord Input[] = {Operand, InitialStatus,       Operand,
                        Initial, (AtomicWord)Address, InitialFlags};
  if (CompareValue) {
    Input[0] = readBytes(Address, Width) | (InitialStatus & ~Mask);
    if (Scenarios[Scenario].Mismatch)
      Input[0] ^= 1;
    if (Cases[Index].Count == 2)
      Input[1] = readBytes(Address + Width, Width) | (InitialStatus & ~Mask);
  }
  void *Protected = Pages + (Scenarios[Scenario].Split ? PageBytes : 0);
  unsigned OldProtection;
  if (Scenarios[Scenario].Protection != PageReadWrite &&
      !VirtualProtect(Protected, PageBytes, Scenarios[Scenario].Protection,
                      &OldProtection))
    fail(SiteProtection, Index, Scenario);
  Cases[Index].Execute(Input, Observation + FieldReturn0);
  Observation[FieldAtomicPC] = AtomicSite;
  if (Scenarios[Scenario].Protection != PageReadWrite &&
      !VirtualProtect(Protected, PageBytes, PageReadWrite, &OldProtection))
    fail(SiteProtection, Index, Scenario);
  for (unsigned I = 0; I < MemoryWordCount; ++I)
    Observation[FieldAfter0 + I] = Memory[I];
  emit();
}
void entry(void) {
  void *Handler = AddVectoredExceptionHandler(1, handler);
  if (!Handler)
    fail(SiteRegistration, 0, 0);
  Pages = VirtualAlloc(0, PageBytes * 2, MemoryCommitReserve, PageReadWrite);
  if (!Pages)
    fail(SiteAllocation, 0, 0);
  for (unsigned Index = 0; Index < sizeof(Cases) / sizeof(Cases[0]); ++Index)
    for (unsigned Scenario = 0;
         Scenario < sizeof(Scenarios) / sizeof(Scenarios[0]); ++Scenario)
      observe(Index, Scenario);
  if (!RemoveVectoredExceptionHandler(Handler))
    fail(SiteRetirement, 0, 0);
  if (!VirtualFree(Pages, 0, MemoryRelease))
    fail(SiteAllocation, 0, 0);
  ExitProcess(0);
}
