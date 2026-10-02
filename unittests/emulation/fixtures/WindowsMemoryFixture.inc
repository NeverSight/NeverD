//===- WindowsMemoryFixture.inc - Original Windows virtual memory checks -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef struct {
  void *Base, *AllocationBase;
  DWORD AllocationProtection;
  unsigned short Partition;
  ULONG_PTR Size;
  DWORD State, Protection, Type;
} MemoryInfo;
#define NEVERD_WINDOWS_MEMORY_ABI(Size, Text)                                  \
  _Static_assert(sizeof(MemoryInfo) == Size, Text);
#include "WindowsMemoryCases.def"
#undef NEVERD_WINDOWS_MEMORY_ABI
static unsigned char *teb(void);
__declspec(dllimport) void *VirtualAlloc(void *, ULONG_PTR, DWORD, DWORD);
__declspec(dllimport) int VirtualFree(void *, ULONG_PTR, DWORD);
__declspec(dllimport) int VirtualProtect(void *, ULONG_PTR, DWORD, DWORD *);
__declspec(dllimport) ULONG_PTR VirtualQuery(const void *, MemoryInfo *,
                                             ULONG_PTR);
__declspec(dllimport) int FlushInstructionCache(HANDLE, const void *,
                                                ULONG_PTR);

static void memoryCheck(int OK, unsigned Line) {
  if (OK)
    return;
  const DWORD Observed[] = {Line, GetLastError()};
  DWORD Written;
  WriteFile(GetStdHandle(StderrSelector), Observed, sizeof(Observed), &Written,
            0);
  require(0);
}
#define VM_CHECK(Condition) memoryCheck(!!(Condition), __LINE__)
static MemoryInfo memoryInfo(const void *Address) {
  MemoryInfo Info;
  VM_CHECK(VirtualQuery(Address, &Info, sizeof(Info)) == sizeof(Info));
  return Info;
}
static void memoryLifecycle(void) {
  unsigned char *Base = VirtualAlloc(0, 3 * VMPage, VMReserve, VMNoAccess);
  VM_CHECK(Base && !((ULONG_PTR)Base % VMGranule));
  MemoryInfo Info = memoryInfo(Base);
  VM_CHECK(Info.Base == Base && Info.AllocationBase == Base &&
           Info.Size == 3 * VMPage && Info.State == VMReserve &&
           Info.Type == VMPrivate && Info.AllocationProtection == VMNoAccess);
  VM_CHECK(VirtualAlloc(Base + VMPage + 7, 1, VMCommit, VMReadWrite) ==
           Base + VMPage);
  VM_CHECK(Base[VMPage] == 0 && Base[2 * VMPage - 1] == 0);
  Base[VMPage] = VMCodeFirst;
  VM_CHECK(VirtualAlloc(Base + VMPage, VMPage, VMCommit, VMReadOnly) ==
           Base + VMPage);
  Info = memoryInfo(Base + VMPage);
  VM_CHECK(Info.Protection == VMReadOnly && Base[VMPage] == VMCodeFirst);
  DWORD Old = LastErrorSeed;
  SetLastError(LastErrorSeed);
  VM_CHECK(!VirtualProtect(Base, 3 * VMPage, VMReadWrite, &Old));
  VM_CHECK(GetLastError() == VMInvalidAddress);
  VM_CHECK(Old == VMNoAccess);
  Old = LastErrorSeed;
  VM_CHECK(!VirtualProtect(Base + VMPage, 2 * VMPage, VMReadWrite, &Old));
  VM_CHECK(GetLastError() == VMInvalidAddress);
  if (Old != VMNoAccess) {
    DWORD Written;
    WriteFile(GetStdHandle(StderrSelector), &Old, sizeof(Old), &Written, 0);
    VM_CHECK(0);
  }
  VM_CHECK(memoryInfo(Base + VMPage).Protection == VMReadOnly);
  VM_CHECK(VirtualFree(Base, 2 * VMPage, VMDecommit));
  VM_CHECK(VirtualFree(Base, 2 * VMPage, VMDecommit));
  VM_CHECK(memoryInfo(Base).State == VMReserve);
  VM_CHECK(VirtualAlloc(Base, 3 * VMPage, VMCommit, VMReadWrite) == Base);
  VM_CHECK(Base[VMPage] == 0);
  Info = memoryInfo(Base + VMPage + 1);
  VM_CHECK(Info.Base == Base + VMPage && Info.Size == 2 * VMPage &&
           Info.AllocationBase == Base &&
           Info.AllocationProtection == VMNoAccess);
  VM_CHECK(!VirtualFree(Base, VMPage, VMRelease));
  VM_CHECK(GetLastError() == InvalidParameter);
  VM_CHECK(!VirtualFree(Base + VMPage, 0, VMRelease));
  VM_CHECK(GetLastError() == VMInvalidAddress);
  VM_CHECK(VirtualFree(Base, 0, VMDecommit));
  VM_CHECK(memoryInfo(Base).State == VMReserve);
  VM_CHECK(VirtualFree(Base, 0, VMRelease));
  VM_CHECK(memoryInfo(Base).State == VMFree);
}
static void memoryAlignment(void) {
  unsigned char *Base = VirtualAlloc(0, 2 * VMGranule, VMReserve, VMReadWrite);
  VM_CHECK(Base && VirtualFree(Base, 0, VMRelease));
  VM_CHECK(VirtualAlloc(Base + VMPage + 3, VMPage, VMReserve, VMReadWrite) ==
           Base);
  VM_CHECK(memoryInfo(Base).Size == 3 * VMPage);
  VM_CHECK(VirtualAlloc(Base + VMPage - 1, 2, VMCommit, VMReadWrite) == Base);
  MemoryInfo Info = memoryInfo(Base);
  VM_CHECK(Info.Size == 2 * VMPage && Info.State == VMCommit);
  DWORD Old = 0;
  VM_CHECK(VirtualProtect(Base + VMPage - 1, 2, VMReadOnly, &Old));
  VM_CHECK(Old == VMReadWrite && memoryInfo(Base).Size == 2 * VMPage);
  VM_CHECK(!VirtualAlloc(Base + 2 * VMPage, 2 * VMPage, VMCommit, VMReadWrite));
  VM_CHECK(GetLastError() == VMInvalidAddress);
  VM_CHECK(memoryInfo(Base + 2 * VMPage).State == VMReserve);
  VM_CHECK(VirtualFree(Base + VMPage - 1, 2, VMDecommit));
  VM_CHECK(memoryInfo(Base).Size == 3 * VMPage);
  VM_CHECK(VirtualFree(Base, 0, VMRelease));
  VM_CHECK(VirtualAlloc(Base, VMGranule, VMReserve | VMCommit, VMReadWrite) ==
           Base);
  VM_CHECK(VirtualAlloc(Base + VMGranule, VMGranule, VMReserve | VMCommit,
                        VMReadWrite) == Base + VMGranule);
  VM_CHECK(!VirtualProtect(Base, VMGranule + VMPage, VMReadOnly, &Old));
  VM_CHECK(GetLastError() == VMInvalidAddress);
  VM_CHECK(memoryInfo(Base).Size == VMGranule &&
           memoryInfo(Base).Protection == VMReadWrite);
  VM_CHECK(VirtualFree(Base + VMGranule, 0, VMRelease));
  VM_CHECK(VirtualFree(Base, 0, VMRelease));
}
static void memoryQuery(void) {
  unsigned char *Base = VirtualAlloc(0, 2 * VMPage, VMCommit, VMReadWrite);
  VM_CHECK(Base);
  MemoryInfo Info = memoryInfo(GetModuleHandleW(0));
  VM_CHECK(Info.AllocationBase == GetModuleHandleW(0) && Info.Type == VMImage &&
           Info.State == VMCommit);
  Info = memoryInfo(&Info);
  VM_CHECK(Info.Type == VMPrivate && Info.State == VMCommit &&
           Info.AllocationBase == *(void **)(teb() + VMDeallocationStack));
  void *Heap = HeapAlloc(GetProcessHeap(), 0, HeapBytes);
  VM_CHECK(Heap && memoryInfo(Heap).Type == VMPrivate);
  VM_CHECK(HeapFree(GetProcessHeap(), 0, Heap));
  VM_CHECK(!VirtualQuery(Base, &Info, sizeof(Info) - 1));
  VM_CHECK(GetLastError() == VMBadLength);
  VM_CHECK(!VirtualQuery(Base, 0, sizeof(Info)));
  VM_CHECK(GetLastError() == VMNoAccessError);
  VM_CHECK(!VirtualQuery((void *)(ULONG_PTR)-1, &Info, sizeof(Info)));
  VM_CHECK(GetLastError() == InvalidParameter);
  VM_CHECK(!VirtualProtect(Base, VMPage, VMReadOnly, 0));
  VM_CHECK(GetLastError() == VMNoAccessError &&
           memoryInfo(Base).Protection == VMReadWrite);
  VM_CHECK(VirtualQuery(Base, (MemoryInfo *)Base, sizeof(Info)) ==
           sizeof(Info));
  VM_CHECK(((MemoryInfo *)Base)->AllocationBase == Base &&
           ((MemoryInfo *)Base)->Size == 2 * VMPage);
  DWORD Old;
  VM_CHECK(VirtualProtect(Base + VMPage, VMPage, VMNoAccess, &Old));
  VM_CHECK(
      !VirtualQuery(Base, (MemoryInfo *)(Base + VMPage - 1), sizeof(Info)));
  VM_CHECK(GetLastError() == VMNoAccessError);
  VM_CHECK(VirtualFree(Base, 0, VMRelease));
}
static void memoryCode(int ExecuteFault) {
  unsigned char *Base =
      VirtualAlloc(0, VMPage, VMReserve | VMCommit, VMReadWrite);
  VM_CHECK(Base);
#ifdef _M_X64
  const unsigned char *Code = VMCodeX64;
  const unsigned Size = sizeof(VMCodeX64);
#else
  const unsigned char *Code = VMCodeARM64;
  const unsigned Size = sizeof(VMCodeARM64);
#endif
  for (unsigned I = 0; I < Size; ++I)
    Base[I] = Code[I];
  DWORD Old;
  if (ExecuteFault)
    ((DWORD (*)(void))Base)();
  VM_CHECK(VirtualProtect(Base, VMPage, VMExecuteRead, &Old));
  VM_CHECK(Old == VMReadWrite);
  VM_CHECK(FlushInstructionCache(GetCurrentProcess(), Base, Size));
  VM_CHECK(((DWORD (*)(void))Base)() == VMCodeFirst);
  VM_CHECK(VirtualProtect(Base, VMPage, VMReadWrite, &Old));
  VM_CHECK(Old == VMExecuteRead);
#ifdef _M_X64
  Base[VMCodePatchX64] = VMCodeSecond;
#else
  *(DWORD *)(Base + VMCodePatchARM64) = VMCodeWordARM64;
#endif
  VM_CHECK(VirtualProtect(Base, VMPage, VMExecuteReadWrite, &Old));
  VM_CHECK(FlushInstructionCache(GetCurrentProcess(), 0, 0));
  VM_CHECK(((DWORD (*)(void))Base)() == VMCodeSecond);
  VM_CHECK(VirtualFree(Base, 0, VMRelease));
}
static void memoryAlias(void) {
  DWORD *Base = VirtualAlloc(0, VMPage, VMCommit, VMReadWrite);
  VM_CHECK(Base);
  *Base = LastErrorSeed;
  VM_CHECK(VirtualProtect(Base, VMPage, VMReadWrite, Base));
  VM_CHECK(*Base == VMReadWrite);
  *Base = LastErrorSeed;
  SetLastError(LastErrorSeed);
  const int OK = VirtualProtect(Base, VMPage, VMReadOnly, Base);
  const DWORD Error = GetLastError();
  const MemoryInfo Info = memoryInfo(Base);
  if (!OK || Error != LastErrorSeed || *Base != LastErrorSeed ||
      Info.Protection != VMReadOnly) {
    const DWORD Observed[] = {(DWORD)OK, Error, *Base, Info.Protection};
    DWORD Written;
    WriteFile(GetStdHandle(StderrSelector), Observed, sizeof(Observed),
              &Written, 0);
    VM_CHECK(0);
  }
  VM_CHECK(VirtualFree(Base, 0, VMRelease));
}
static void memoryPlacement(void) {
  unsigned char *First =
      VirtualAlloc(0, VMPage, VMReserve | VMTopDown, VMReadWrite);
  unsigned char *Second =
      VirtualAlloc(0, VMPage, VMReserve | VMTopDown, VMReadWrite);
  VM_CHECK(First && Second && Second < First &&
           !((ULONG_PTR)First % VMGranule) && !((ULONG_PTR)Second % VMGranule));
  VM_CHECK(!VirtualAlloc(First, VMPage, VMReserve, VMReadWrite));
  VM_CHECK(GetLastError() == VMInvalidAddress);
  VM_CHECK(VirtualAlloc(First, VMPage, VMCommit, VMReadWrite) == First);
  First[VMPage - 1] = VMCodeFirst;
  VM_CHECK(First[0] == 0 && First[VMPage - 1] == VMCodeFirst);
  VM_CHECK(VirtualFree(First, 0, VMRelease));
  VM_CHECK(VirtualFree(Second, 0, VMRelease));
}
static void memoryScenario(char Mode) {
  switch (Mode) {
  case 'A':
    memoryLifecycle();
    return;
  case 'B':
    memoryAlignment();
    return;
  case 'C':
    memoryQuery();
    return;
  case 'D':
    memoryCode(0);
    return;
  case 'E':
    memoryAlias();
    return;
  case 'F':
    memoryPlacement();
    return;
  case 'G': {
    unsigned char *Base = VirtualAlloc(0, VMReclaimSize, VMCommit, VMReadWrite);
    VM_CHECK(Base);
    for (unsigned I = 0; I < 3; ++I) {
      Base[VMPage] = VMCodeFirst;
      VM_CHECK(VirtualFree(Base + VMPage, VMReclaimSize - VMPage, VMDecommit));
      VM_CHECK(VirtualAlloc(Base + VMPage, VMReclaimSize - VMPage, VMCommit,
                            VMReadWrite) == Base + VMPage);
      VM_CHECK(Base[VMPage] == 0);
    }
    VM_CHECK(VirtualFree(Base, 0, VMRelease));
    return;
  }
  case 'M':
    memoryCode(1);
    return;
  default:
    break;
  }
  if (Mode < 'H' || Mode > 'N')
    return;
  volatile unsigned char *Base =
      VirtualAlloc(0, VMPage, VMReserve, VMReadWrite);
  VM_CHECK(Base);
  if (Mode == 'H')
    *Base = VMCodeFirst;
  VM_CHECK(VirtualAlloc((void *)Base, VMPage, VMCommit, VMReadWrite) == Base);
  DWORD Old;
  if (Mode == 'I')
    VM_CHECK(VirtualProtect((void *)Base, VMPage, VMNoAccess, &Old));
  if (Mode == 'J')
    VM_CHECK(VirtualProtect((void *)Base, VMPage, VMReadOnly, &Old));
  if (Mode == 'K')
    VM_CHECK(VirtualFree((void *)Base, 0, VMDecommit));
  if (Mode == 'L')
    VM_CHECK(VirtualFree((void *)Base, 0, VMRelease));
  if (Mode == 'N')
    VM_CHECK(VirtualProtect((void *)Base, VMPage, VMReadWrite | VMGuard, &Old));
  *Base = VMCodeFirst;
  VM_CHECK(0);
}
#undef VM_CHECK
