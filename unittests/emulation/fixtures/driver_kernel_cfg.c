// Original client of a reference kernel's read-only 19041 CFG bitmap.
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  const U16 *Buffer;
} STRING;
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const STRING *);
static const U16 Dispatch[] = {'_', 'g', 'u', 'a', 'r', 'd', '_', 'd',
                               'i', 's', 'p', 'a', 't', 'c', 'h', '_',
                               'i', 'c', 'a', 'l', 'l', 0};
static const U16 TargetName[] = {'K', 'e', 'G', 'e', 't', 'C', 'u', 'r', 'r',
                                 'e', 'n', 't', 'I', 'r', 'q', 'l', 0};
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  STRING Name;
  RtlInitUnicodeString(&Name, Dispatch);
  unsigned char *Code = MmGetSystemRoutineAddress(&Name);
  if (!Code)
    return (int)0xc000000d;
  volatile U64 *Slot = (void *)(Code + 7 + *(int *)(Code + 3));
  U64 Bitmap = *Slot;
  if (!Bitmap)
    return (int)0xc000000d;
  RtlInitUnicodeString(&Name, TargetName);
  U64 Target = (U64)MmGetSystemRoutineAddress(&Name);
  if (!Target)
    return (int)0xc000000d;
  volatile U64 *Word = (void *)(Bitmap + (Target >> 9) * 8);
  U64 Flags = *Word;
  if (!(Flags & (1ULL << ((Target >> 3) & 63))))
    return (int)0xc000000d;
#ifdef WRITE_KERNEL_CFG
  *Word = 0;
#endif
  return 0;
}
