// Reproduce the sample's read-only route from an AVL export's original code
// through the CFG dispatch helper to the shared authenticated CFG bitmap.
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, Maximum;
  U32 Padding;
  const U16 *Buffer;
} US;
__declspec(dllimport) void RtlInsertElementGenericTableFullAvl(void);
__declspec(dllimport) void RtlInitUnicodeString(US *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const US *);
__declspec(dllimport) int DbgPrint(const char *, ...);
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  const U8 *Code = (const U8 *)(U64)&RtlInsertElementGenericTableFullAvl;
  const U8 *Dispatch = 0;
  for (U32 I = 0; I + 7 <= 256; ++I)
    if (Code[I] == 0xe8 && Code[I + 5] == 0x48 && Code[I + 6] == 0x8b) {
      Dispatch = Code + I + 5 + *(const int *)(Code + I + 1);
      break;
    }
  if (!Dispatch)
    return (int)0xc0000225;
  const U64 *Slot = 0;
  for (U32 I = 0; I + 7 <= 256; ++I)
    if (Dispatch[I] == 0x4c && Dispatch[I + 1] == 0x8b &&
        Dispatch[I + 2] == 0x1d) {
      Slot = (const U64 *)(Dispatch + I + 7 + *(const int *)(Dispatch + I + 3));
      break;
    }
  if (!Slot || !*Slot)
    return (int)0xc0000225;
  US Name;
  RtlInitUnicodeString(&Name, L"KeGetCurrentIrql");
  U64 Target = (U64)MmGetSystemRoutineAddress(&Name);
  if (!Target)
    return (int)0xc0000225;
  const U64 *Word = (const U64 *)(*Slot + (Target >> 9) * 8);
  if (!(*Word & (1ULL << ((Target >> 3) & 63))))
    return (int)0xc000000d;
  DbgPrint("original avl code cfg scan passed");
  return 0;
}
