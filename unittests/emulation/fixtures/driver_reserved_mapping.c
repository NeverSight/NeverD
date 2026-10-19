// Original guest: reserve, map, unmap, reuse and release the same system VA.
typedef unsigned int U32;
typedef unsigned long long U64;
#define API __declspec(dllimport)
API void *MmAllocateMappingAddress(U64, U32);
API void MmFreeMappingAddress(void *, U32);
API void *MmMapLockedPagesWithReservedMapping(void *, U32, void *, U32);
API void MmUnmapReservedMapping(void *, U32, void *);
API void *ExAllocatePoolWithTag(U32, U64, U32);
API void ExFreePoolWithTag(void *, U32);
API void *IoAllocateMdl(void *, U32, unsigned char, unsigned char, void *);
API void IoFreeMdl(void *);
API void MmProbeAndLockPages(void *, unsigned char, U32);
API void MmUnlockPages(void *);
API void *MmAllocatePagesForMdlEx(U64, U64, U64, U64, U32, U32);
API void MmFreePagesFromMdl(void *);
API void ExFreePool(void *);
API void *MmGetSystemAddressForMdlSafe(void *, U32);
API unsigned char KfRaiseIrql(unsigned char);
API void KeLowerIrql(unsigned char);
static void *Reservation;
static const U32 Tag = 0x53524552;
static void unload(void *Driver) {
  (void)Driver;
#ifndef LEAK_RESERVATION
  MmFreeMappingAddress(Reservation, Tag);
#endif
}
U32 DriverEntry(void *Driver, void *Path) {
  (void)Path;
  Reservation = MmAllocateMappingAddress(8192, Tag);
  unsigned char *Pool = ExAllocatePoolWithTag(0, 12288, Tag);
  if (!Reservation || !Pool || ((U64)Reservation & 4095))
    return 0xc0000001;
  void *MDL = IoAllocateMdl(Pool + 7, 8192, 0, 0, 0);
  if (!MDL)
    return 0xc0000001;
  MmProbeAndLockPages(MDL, 0, 1);
  if (MmMapLockedPagesWithReservedMapping(Reservation, Tag, MDL, 1))
    return 0xc0000001;
  MmUnlockPages(MDL);
  IoFreeMdl(MDL);
  MDL = IoAllocateMdl(Pool + 7, 4096, 0, 0, 0);
  if (!MDL)
    return 0xc0000001;
  MmProbeAndLockPages(MDL, 0, 1);
  for (U32 I = 0; I < 2; ++I) {
    unsigned char Previous = KfRaiseIrql(2);
    unsigned char *Alias =
        MmMapLockedPagesWithReservedMapping(Reservation, Tag, MDL, 1);
    if (Alias != (unsigned char *)Reservation + 7 ||
        *(U64 *)((unsigned char *)MDL + 0x18) != (U64)Alias ||
        MmGetSystemAddressForMdlSafe(MDL, 16) != Alias)
      return 0xc0000001;
    Alias[0] = (unsigned char)(0x31 + I);
    Alias[4095] = (unsigned char)(0x41 + I);
    if (Pool[7] != 0x31 + I || Pool[4102] != 0x41 + I)
      return 0xc0000001;
    MmUnmapReservedMapping(Alias, Tag, MDL);
    if ((*(unsigned short *)((unsigned char *)MDL + 0xa) & 1) ||
        *(U64 *)((unsigned char *)MDL + 0x18) != (U64)Alias)
      return 0xc0000001;
    KeLowerIrql(Previous);
  }
  MmUnlockPages(MDL);
  IoFreeMdl(MDL);
  ExFreePoolWithTag(Pool, Tag);
  void *Pages = MmAllocatePagesForMdlEx(0, ~(U64)0, 0, 8192, 0, 4);
  if (!Pages)
    return 0xc0000001;
  unsigned char *Noncached =
      MmMapLockedPagesWithReservedMapping(Reservation, Tag, Pages, 1);
  if (Noncached != Reservation)
    return 0xc0000001;
  Noncached[4096] = 0x67;
  if (Noncached[4096] != 0x67)
    return 0xc0000001;
  MmUnmapReservedMapping(Noncached, Tag, Pages);
  MmFreePagesFromMdl(Pages);
  ExFreePool(Pages);
  *(U64 *)((unsigned char *)Driver + 0x68) = (U64)unload;
  return 0;
}
