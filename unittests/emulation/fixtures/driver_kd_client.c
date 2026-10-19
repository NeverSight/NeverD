// Original driver client: consume a complete KD record, then release its dump.
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) void RtlCaptureContext(void *);
__declspec(dllimport) void RtlCopyMemory(void *, const void *, U64);
__declspec(dllimport) U8 MmIsAddressValid(void *);
__declspec(dllimport) U32 KeCapturePersistentThreadState(const void *, void *,
                                                         U32, U64, U64, U64,
                                                         U64, void *);
static U8 Saved[0x380];
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  __declspec(align(16)) U8 Context[1232];
  RtlCaptureContext(Context);
  U8 *Dump = ExAllocatePoolWithTag(0, 0x40000, 0x706d7544);
  if (!Dump)
    return (int)0xc000009a;
  KeCapturePersistentThreadState(Context, (void *)0, 0, 0, 0, 0, 0, Dump);
  RtlCopyMemory(Saved, Dump + 0x2080, sizeof(Saved));
  if (*(U32 *)(Saved + 0x10) != 0x4742444b ||
      *(U32 *)(Saved + 0x14) != sizeof(Saved) ||
      *(U64 *)(Saved + 0x18) != 0xfffff80000000000ULL ||
      *(U64 *)(Saved + 0x138) != 4096 ||
      *(U64 *)(Saved + 0x48) < 0xfffff80000000000ULL)
    return (int)0xc000000d;
#ifdef READ_UNKNOWN_GLOBAL
  void *List = (void *)*(U64 *)(Saved + 0x48);
  if (!MmIsAddressValid(List))
    return (int)0xc000000d;
  volatile U64 Unknown = *(volatile U64 *)List;
  (void)Unknown;
#endif
  ExFreePoolWithTag(Dump, 0x64796e64); // Mirrors the sample's differing tag.
  return 0;
}
