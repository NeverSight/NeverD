//===- aarch64_exclusive_oracle.c - Native Windows ARM64 observations
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "aarch64_exclusive.inc"

#define NEVERD_EXCLUSIVE_ORACLE_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_EXCLUSIVE_ORACLE_WIDE(Name, Value)                              \
  static const ExclusiveWord Name = Value;
#include "AArch64ExclusiveOracle.def"
#undef NEVERD_EXCLUSIVE_ORACLE_WIDE
#undef NEVERD_EXCLUSIVE_ORACLE_VALUE
__declspec(dllimport) void ExitProcess(unsigned);
__declspec(dllimport) void *GetStdHandle(unsigned);
__declspec(dllimport) int WriteFile(void *, const void *, unsigned, unsigned *,
                                    void *);
__declspec(dllimport) void *VirtualAlloc(void *, ExclusiveWord, unsigned,
                                         unsigned);
__declspec(dllimport) int VirtualFree(void *, ExclusiveWord, unsigned);
__declspec(dllimport) int VirtualProtect(void *, ExclusiveWord, unsigned,
                                         unsigned *);

enum {
#define NEVERD_EXCLUSIVE_ORACLE_FIELD(Name) Field##Name,
#include "AArch64ExclusiveOracle.def"
#undef NEVERD_EXCLUSIVE_ORACLE_FIELD
  FieldCount
};
typedef struct NativeException {
  unsigned Code, Flags;
  struct NativeException *Nested;
  void *Address;
  unsigned Count, Padding;
  ExclusiveWord Parameters[MaxParameters];
} NativeException;
typedef struct {
  NativeException *Record;
  unsigned char *Context;
} NativePointers;
__declspec(dllimport) void *
AddVectoredExceptionHandler(unsigned, unsigned (*)(NativePointers *));
__declspec(dllimport) unsigned RemoveVectoredExceptionHandler(void *);
static ExclusiveWord LoadSite, StoreSite, ResumeSite, Observation[FieldCount];
static unsigned char *Pages;

static void fail(unsigned Site, ExclusiveWord First, ExclusiveWord Second) {
  ExclusiveWord Values[] = {Site, First, Second};
  unsigned Written;
  WriteFile(GetStdHandle(StderrSelector), Values, sizeof(Values), &Written, 0);
  ExitProcess(FailureStatus);
}

static unsigned alignmentHandler(NativePointers *P) {
  ExclusiveWord *PC = (ExclusiveWord *)(P->Context + ContextPCOffset);
  if ((*PC != LoadSite && *PC != StoreSite) ||
      (ExclusiveWord)P->Record->Address != *PC ||
      ++Observation[FieldFaults] != 1 || P->Record->Count > MaxParameters)
    fail(SiteHandler, *PC, P->Record->Code);
  Observation[FieldCode] = P->Record->Code;
  Observation[FieldFlags] = P->Record->Flags;
  Observation[FieldParameterCount] = P->Record->Count;
  Observation[FieldFaultPC] = *PC;
  Observation[FieldFaultStage] = *PC == StoreSite;
  Observation[FieldExceptionPC] = (ExclusiveWord)P->Record->Address;
  Observation[FieldContextPC] = *PC;
  ExclusiveWord *GPR = (ExclusiveWord *)(P->Context + ContextGPROffset);
  Observation[FieldContextLow] = GPR[0];
  Observation[FieldContextHigh] = GPR[3];
  Observation[FieldContextStatus] = GPR[2];
  Observation[FieldContextAddress] = GPR[1];
  for (unsigned I = 0; I != P->Record->Count; ++I)
    Observation[FieldParameter0 + I] = P->Record->Parameters[I];
  *PC = ResumeSite;
  return ContinueExecution;
}

static void emit(const void *Data, unsigned Size) {
  unsigned Written = 0;
  if (!WriteFile(GetStdHandle(StdoutSelector), Data, Size, &Written, 0) ||
      Written != Size)
    ExitProcess(FailureStatus);
}

#define NEVERD_EXCLUSIVE_CASE(Name, Load, Store, Width, Count)                 \
  static void alignment##Name(unsigned Index, unsigned Offset, unsigned Mode,  \
                              unsigned Scenario) {                             \
    const int Split = Scenario >= SplitReadOnlyMemory;                         \
    ExclusiveWord *Memory =                                                    \
        (ExclusiveWord *)(Pages + (Split ? PageBytes - WindowAlignment : 0));  \
    const unsigned BaseOffset = Split ? WindowAlignment - Width * Count : 0;   \
    for (unsigned I = 0; I != MemoryWordCount; ++I)                            \
      Memory[I] = I % 2 ? ExclusiveUpdated : ExclusiveInitial;                 \
    for (unsigned I = 0; I != FieldCount; ++I)                                 \
      Observation[I] = 0;                                                      \
    Observation[FieldCaseIndex] = Index;                                       \
    Observation[FieldOffset] = Offset;                                         \
    Observation[FieldMode] = Mode;                                             \
    Observation[FieldMemoryScenario] = Scenario;                               \
    Observation[FieldMemoryBase] = (ExclusiveWord)Memory;                      \
    void *Protected = Pages + (Split ? PageBytes : 0);                         \
    unsigned OldProtection;                                                    \
    const unsigned Protection =                                                \
        Scenario == NoAccessMemory || Scenario == SplitNoAccessMemory          \
            ? PageNoAccess                                                     \
            : PageReadOnly;                                                    \
    if (Scenario != ReadWriteMemory &&                                         \
        !VirtualProtect(Protected, PageBytes, Protection, &OldProtection))     \
      fail(SiteProtection, Scenario, Mode);                                    \
    ExclusiveWord LoadedLow = ExclusiveInitial, LoadedHigh = ExclusiveUpdated; \
    ExclusiveWord Low, High, Status;                                           \
    __asm__ volatile(                                                          \
        "adr x4, 1f\n\tstr x4, [%[load_site]]\n\t"                             \
        "adr x4, 4f\n\tstr x4, [%[store_site]]\n\t"                            \
        "adr x4, 5f\n\tstr x4, [%[resume]]\n\tclrex\n\t"                       \
        "mov x0, %[initial]\n\tmov x3, %[updated]\n\tmov x2, "                 \
        "%[status_seed]\n\t"                                                   \
        "cbz %[load_enabled], 3f\n\tmov x1, %[load_address]\n\t"               \
        "1:\n\t.inst " #Load "\n\t"                                            \
        "mov %[loaded_low], x0\n\tmov %[loaded_high], x3\n\t"                  \
        "3:\n\tcbz %[store_enabled], 5f\n\t"                                   \
        "mov x0, %[updated]\n\tmov x3, %[initial]\n\tmov x1, "                 \
        "%[store_address]\n\t"                                                 \
        "4:\n\t.inst " #Store "\n\t"                                           \
        "5:\n\tmov %[low], x0\n\tmov %[high], x3\n\tmov %[status], x2\n\t"     \
        : [loaded_low] "+&r"(LoadedLow), [loaded_high] "+&r"(LoadedHigh),      \
          [low] "=&r"(Low), [high] "=&r"(High), [status] "=&r"(Status)         \
        : [load_site] "r"(&LoadSite), [store_site] "r"(&StoreSite),            \
          [resume] "r"(&ResumeSite),                                           \
          [load_address] "r"((unsigned char *)Memory + BaseOffset +            \
                             (Mode == AlignedLoadStore ? 0 : Offset)),         \
          [store_address] "r"((unsigned char *)Memory + BaseOffset + Offset),  \
          [load_enabled] "r"((ExclusiveWord)(Mode != StoreOnly)),              \
          [store_enabled] "r"((ExclusiveWord)(Mode != LoadOnly)),              \
          [initial] "r"(ExclusiveInitial), [updated] "r"(ExclusiveUpdated),    \
          [status_seed] "r"((ExclusiveWord)StatusSeed)                         \
        : "x0", "x1", "x2", "x3", "x4", "memory");                             \
    Observation[FieldLoadPC] = LoadSite;                                       \
    Observation[FieldStorePC] = StoreSite;                                     \
    Observation[FieldLoadedLow] = LoadedLow;                                   \
    Observation[FieldLoadedHigh] = LoadedHigh;                                 \
    Observation[FieldReturnLow] = Low;                                         \
    Observation[FieldReturnHigh] = High;                                       \
    Observation[FieldReturnStatus] = Status;                                   \
    if (Scenario != ReadWriteMemory &&                                         \
        !VirtualProtect(Protected, PageBytes, PageReadWrite, &OldProtection))  \
      fail(SiteProtection, Scenario, Mode);                                    \
    for (unsigned I = 0; I != MemoryWordCount; ++I)                            \
      Observation[FieldAfter0 + I] = Memory[I];                                \
    emit(Observation, sizeof(Observation));                                    \
  }
#include "../aarch64/AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_CASE

void entry(void) {
  unsigned Results[] = {
#define NEVERD_EXCLUSIVE_CASE(Name, Load, Store, Width, Count)                 \
  checkExclusive##Name(),
#include "../aarch64/AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_CASE
  };
  emit(Results, sizeof(Results));
  for (unsigned I = 0; I != sizeof(Results) / sizeof(*Results); ++I)
    if (!Results[I])
      ExitProcess(FailureStatus);
  void *Handler = AddVectoredExceptionHandler(1, alignmentHandler);
  if (!Handler)
    fail(SiteRegistration, 0, 0);
  Pages = VirtualAlloc(0, PageBytes * 2, MemoryCommitReserve, PageReadWrite);
  if (!Pages)
    fail(SiteAllocation, 0, 0);
  unsigned Index = 0;
#define NEVERD_EXCLUSIVE_CASE(Name, Load, Store, Width, Count)                 \
  for (unsigned Offset = 1; Offset < Width * Count; ++Offset)                  \
    for (unsigned Mode = 0; Mode < ModeCount; ++Mode)                          \
      for (unsigned Scenario = 0; Scenario < MemoryScenarioCount; ++Scenario)  \
        alignment##Name(Index, Offset, Mode, Scenario);                        \
  ++Index;
#include "../aarch64/AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_CASE
  if (!RemoveVectoredExceptionHandler(Handler))
    fail(SiteRetirement, 0, 0);
  if (!VirtualFree(Pages, 0, MemoryRelease))
    fail(SiteAllocation, 0, 0);
  ExitProcess(0);
}
