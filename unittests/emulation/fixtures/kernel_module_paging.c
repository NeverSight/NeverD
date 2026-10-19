// Native driver fixtures read cached recursive PTEs without a synchronizing
// API.
typedef unsigned long long U64;
#define PTE_BASE 0xffffff0000000000ULL
#define PTE_MASK 0x8000000000000007ULL

#if defined(PRIMARY)
__declspec(dllimport) U64 ModulePagingSelf(U64 PteBase);
static volatile U64 PrimaryValue = 42;
static volatile U64 *volatile PrimaryAnchor = &PrimaryValue;
long DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  return *PrimaryAnchor == 42 && ModulePagingSelf(PTE_BASE) == 42
             ? 0
             : (long)0xc0000001;
}
#else
__declspec(dllexport) volatile U64 PagingValue = 42;
__declspec(dllexport) volatile U64 PagingCalls;
__declspec(dllexport) volatile U64 *volatile PagingAnchor = &PagingValue;
static U64 pte(U64 Address, U64 PteBase) {
  return *(volatile U64 *)(PteBase + ((Address >> 9) & 0x7ffffffff8ULL));
}
__declspec(dllexport) U64 ModulePagingSelf(U64 PteBase) {
  ++PagingCalls;
  return (pte((U64)&ModulePagingSelf, PteBase) & PTE_MASK) == 1 &&
                 (pte((U64)&PagingValue, PteBase) & PTE_MASK) ==
                     0x8000000000000003ULL
             ? *PagingAnchor
             : 0xbad;
}
__declspec(dllexport) U64 ModulePagingAfterRead(U64 Address, U64 PteBase,
                                                U64 ExpectedFlags) {
  ++PagingCalls;
  U64 Value = *(volatile U64 *)Address;
  return (pte(Address, PteBase) & PTE_MASK) == ExpectedFlags ? Value : 0xbad;
}
#endif
