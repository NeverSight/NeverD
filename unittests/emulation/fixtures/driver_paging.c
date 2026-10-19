// Original client: discover the recursive PML4 entry, inspect a PTE, and
// compare a locked MDL alias against both physical-address APIs and its public
// PFN list.
typedef unsigned int U32;
typedef unsigned long long U64;
__declspec(dllimport) void *MmGetVirtualForPhysical(U64);
__declspec(dllimport) U64 MmGetPhysicalAddress(void *);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) void *IoAllocateMdl(void *, U32, unsigned char,
                                          unsigned char, void *);
__declspec(dllimport) void IoFreeMdl(void *);
__declspec(dllimport) void MmProbeAndLockPages(void *, U32, U32);
__declspec(dllimport) void MmBuildMdlForNonPagedPool(void *);
__declspec(dllimport) void MmUnlockPages(void *);
__declspec(dllimport) void *MmMapLockedPagesSpecifyCache(void *, U32, U32,
                                                         void *, U32, U32);
__declspec(dllimport) void MmUnmapLockedPages(void *, void *);
static volatile U64 Marker = 0x123456789abcdef0;
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  U64 Root, Before, After;
  __asm__ volatile("xorl %%r9d, %%r9d\n"
                   "stc\n"
                   "pushfq\n"
                   "popq %1\n"
                   "movq %%cr3, %0\n"
                   "pushfq\n"
                   "popq %2\n"
                   : "=r"(Root), "=&r"(Before), "=&r"(After)
                   :
                   : "r9", "cc", "memory");
  if (!Root || (Root & 4095) || Before != After || (Before & 0x45) != 0x45)
    return (int)0xc000000d;
  const U64 Mask = 0x000ffffffffff000ULL;
  volatile U64 *PML4 = MmGetVirtualForPhysical(Root);
  if (!PML4)
    return (int)0xc000000d;
  U64 Self = 512;
  for (U64 I = 0; I < 512; ++I)
    if ((PML4[I] & Mask) == Root)
      Self = I;
  if (Self == 512)
    return (int)0xc000000d;
  U64 PTEBase = 0xffff000000000000ULL | (Self << 39);
  volatile U64 *PTE =
      (void *)(PTEBase + (((U64)&Marker >> 9) & 0x7ffffffff8ULL));
  if (!(*PTE & 1) || ((*PTE & Mask) + ((U64)&Marker & 4095)) !=
                         MmGetPhysicalAddress((void *)&Marker))
    return (int)0xc000000d;
#ifdef WRITE_PTE
  *PTE = 0; // Must fault before changing guest page-table state.
#endif
  unsigned char *Pool = ExAllocatePoolWithTag(
#ifdef NONPAGED_REMAP
      0,
#else
      1,
#endif
      64, 0x50414745);
  if (!Pool)
    return (int)0xc000009a;
  void *MDL = IoAllocateMdl(Pool, 64, 0, 0, (void *)0);
  if (!MDL)
    return (int)0xc000009a;
#ifdef NONPAGED_REMAP
  MmBuildMdlForNonPagedPool(MDL);
#else
  MmProbeAndLockPages(MDL, 0, 2);
#endif
  unsigned char *Alias =
      MmMapLockedPagesSpecifyCache(MDL, 0, 1, (void *)0, 0, 16);
  if (!Alias)
    return (int)0xc000009a;
  U64 PA = MmGetPhysicalAddress(Pool);
  if (PA != MmGetPhysicalAddress(Alias) ||
      (PA >> 12) != *(U64 *)((unsigned char *)MDL + 0x30))
    return (int)0xc000000d;
  Alias[17] = 0x5a;
  if (Pool[17] != 0x5a)
    return (int)0xc000000d;
  MmUnmapLockedPages(Alias, MDL);
#ifdef NONPAGED_REMAP
  if (*(void **)((unsigned char *)MDL + 0x18) != Pool ||
      *(unsigned short *)((unsigned char *)MDL + 0xa) != 4)
    return (int)0xc000000d;
#else
  MmUnlockPages(MDL);
#endif
  IoFreeMdl(MDL);
  ExFreePoolWithTag(Pool, 0x50414745);
  *(void (**)(void *))((unsigned char *)Driver + 0x68) = Unload;
  return 0;
}
