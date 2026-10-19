// Owned PCI function, HAL imports and dynamic kernel export use one state.
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  const U16 *Buffer;
} US;
typedef U32 (*BUS_DATA)(U32, U32, U32, void *, U32, U32);
__declspec(dllimport) U32 HalGetBusDataByOffset(U32, U32, U32, void *, U32,
                                                U32);
__declspec(dllimport) U32 HalSetBusDataByOffset(U32, U32, U32, void *, U32,
                                                U32);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const US *);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) void KeLowerIrql(U8);
__declspec(dllimport) int DbgPrint(const char *, ...);
static U8 Buffer[64];
static void Unload(void *Driver) {
  (void)Driver;
  if (HalGetBusDataByOffset(4, 0x203, 5, Buffer, 0x100, 4) == 4 &&
      Buffer[0] == 0xaa && Buffer[3] == 0xdd)
    DbgPrint("PCI unload passed");
}
int DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  U8 Old = KfRaiseIrql(2);
  U32 Count = HalGetBusDataByOffset(4, 0x203, 5, Buffer, 0, 64);
  KeLowerIrql(Old);
  if (Count != 64 || Buffer[0] != 0x34 || Buffer[1] != 0x12 || Buffer[14])
    return (int)0xc000000d;
  static const U16 Name[] = {'H', 'a', 'l', 'G', 'e', 't', 'B', 'u',
                             's', 'D', 'a', 't', 'a', 'B', 'y', 'O',
                             'f', 'f', 's', 'e', 't', 0};
  const US String = {sizeof(Name) - 2, sizeof(Name), 0, Name};
  BUS_DATA Read = (BUS_DATA)MmGetSystemRoutineAddress(&String);
  if (!Read)
    return (int)0xc000000d;
  Buffer[0] = 0xaa;
  Buffer[1] = 0xbb;
  Buffer[2] = 0xcc;
  Buffer[3] = 0xdd;
  if (HalSetBusDataByOffset(4, 0x203, 5, Buffer, 0x100, 4) != 4 ||
      Read(4, 0x203, 5, Buffer, 0x100, 4) != 4 || Buffer[0] != 0xaa ||
      Buffer[3] != 0xdd)
    return (int)0xc000000d;
  Buffer[0] = 5;
  Buffer[1] = 0;
  Buffer[2] = 0xf0;
  Buffer[3] = 0xff;
  if (HalSetBusDataByOffset(4, 0x203, 5, Buffer, 4, 4) != 4 ||
      Read(4, 0x203, 5, Buffer, 4, 4) != 4 || Buffer[0] != 5 || Buffer[1] ||
      Buffer[2] != 6 || Buffer[3])
    return (int)0xc000000d;
  if (Read(4, 0x203, 6, Buffer, 0, 64) != 2 || Buffer[0] != 0xff ||
      Buffer[1] != 0xff)
    return (int)0xc000000d;
  *(void (**)(void *))((U8 *)Driver + 0x68) = Unload;
  DbgPrint("PCI configuration passed");
  return 0;
}
