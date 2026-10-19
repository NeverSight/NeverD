typedef unsigned int U32;
typedef unsigned char U8;
typedef unsigned long long U64;
#define API __declspec(dllimport)
API void *MmAllocateContiguousMemory(U64, U64);
API void MmFreeContiguousMemory(void *);
API U64 MmGetPhysicalAddress(void *);
API void *IoAllocateMdl(void *, U32, U8, U8, void *);
API void MmBuildMdlForNonPagedPool(void *);
API void *MmGetSystemAddressForMdlSafe(void *, U32);
API void IoFreeMdl(void *);
API U8 KfRaiseIrql(U8);
API void KeLowerIrql(U8);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  if (MmAllocateContiguousMemory(4096, 0xffffff))
    return (int)0xc000000d;
  U8 Old = KfRaiseIrql(2);
  U8 *P = MmAllocateContiguousMemory(8193, ~(U64)0);
  if (!P || ((U64)P & 4095))
    return (int)0xc000000d;
  const U64 Physical = MmGetPhysicalAddress(P);
  if (MmGetPhysicalAddress(P + 4096) != Physical + 4096 ||
      MmGetPhysicalAddress(P + 8192) != Physical + 8192)
    return (int)0xc000000d;
  P[0] = 0x11;
  P[4096] = 0x22;
  P[8192] = 0x33;
  void *MDL = IoAllocateMdl(P, 8193, 0, 0, 0);
  if (!MDL)
    return (int)0xc000000d;
  MmBuildMdlForNonPagedPool(MDL);
  U8 *V = MmGetSystemAddressForMdlSafe(MDL, 16);
  if (V != P || V[0] != 0x11 || V[4096] != 0x22 || V[8192] != 0x33)
    return (int)0xc000000d;
  U64 *PFN = (U64 *)((U8 *)MDL + 0x30);
  if (PFN[0] * 4096 != Physical || PFN[1] != PFN[0] + 1 || PFN[2] != PFN[1] + 1)
    return (int)0xc000000d;
  IoFreeMdl(MDL);
  MmFreeContiguousMemory(P);
  KeLowerIrql(Old);
  *(void **)((U8 *)Driver + 0x68) = Unload;
  return 0;
}
