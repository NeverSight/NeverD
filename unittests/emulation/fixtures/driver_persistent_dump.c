//===- driver_persistent_dump.c - Original capture metadata client -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) void RtlCaptureContext(void *);
__declspec(dllimport) U32 KeCapturePersistentThreadState(const void *, void *,
                                                         U32, U64, U64, U64,
                                                         U64, void *);

int DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  __declspec(align(16)) U8 Context[1232];
  RtlCaptureContext(Context);
  U8 *Dump = (U8 *)ExAllocatePoolWithTag(0, 0x40000, 0x706d7544);
  if (!Dump)
    return (int)0xc000009a;
  const U32 Written = KeCapturePersistentThreadState(
      Context, (void *)0, 0x123, 0x1122334455667788, 2, 3, 4, Dump);
  int Status = (int)0xc000000d;
#ifdef CAPTURE_UNKNOWN
  volatile U64 Unknown = *(volatile U64 *)(Dump + 0x2080);
  (void)Unknown;
#endif
  if (Written == 0x40000 && *(U64 *)Dump == 0x3436554445474150 &&
      *(U32 *)(Dump + 12) == 19041 && *(U32 *)(Dump + 0x38) == 0x123 &&
      *(U64 *)(Dump + 0x40) == 0x1122334455667788 &&
      *(U64 *)(Dump + 0x348 + 248) == *(U64 *)(Context + 248) &&
      *(U64 *)(Dump + 0xf10) == *(U64 *)(Context + 248) &&
      *(U32 *)(Dump + 0x3fffc) == 0x44475254)
    Status = 0;
  ExFreePoolWithTag(Dump, 0x706d7544);
  return Status;
}
