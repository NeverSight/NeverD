//===- driver_large_pool.c - Multi-megabyte pool and MDL client -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) void *memset(void *, int, U64);
__declspec(dllimport) void *IoAllocateMdl(void *, U32, U8, U8, void *);
__declspec(dllimport) void MmBuildMdlForNonPagedPool(void *);
__declspec(dllimport) void IoFreeMdl(void *);
__declspec(dllimport) U64 MmGetPhysicalAddress(void *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);

static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, void *RegistryPath) {
  (void)RegistryPath;
  enum { Size = 0x22d350, Tag = 0x4c415247 };
  U8 *Buffer = ExAllocatePoolWithTag(0, Size, Tag);
  if (!Buffer)
    return (int)0xc000009aU;
  if ((U64)Buffer & 4095)
    return (int)0xc000000dU;
  memset(Buffer, 0x5a, Size);
  for (U32 I = 0; I < Size; I += 4096)
    if (Buffer[I] != 0x5a)
      return (int)0xc000000dU;
  Buffer[Size - 1] = 0xa5;
  void *MDL = IoAllocateMdl(Buffer, Size, 0, 0, 0);
  if (!MDL)
    return (int)0xc000009aU;
  MmBuildMdlForNonPagedPool(MDL);
  U64 First = MmGetPhysicalAddress(Buffer);
  U64 Last = MmGetPhysicalAddress(Buffer + Size - 1);
  if (!First || !Last || First == Last || Buffer[Size - 1] != 0xa5)
    return (int)0xc000000dU;
  IoFreeMdl(MDL);
  ExFreePoolWithTag(Buffer, Tag);
  *(void **)((U8 *)Driver + 0x68) = Unload;
  DbgPrint("large pool and MDL completed");
  return 0;
}
