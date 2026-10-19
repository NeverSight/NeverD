// Query authenticated native MCFG fields from explicit registry addresses.
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, Maximum;
  const U16 *Buffer;
} STRING;
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) int ZwOpenKey(void **, U32, const void *);
__declspec(dllimport) int ZwQueryValueKey(void *, const STRING *, U32, void *,
                                          U32, U32 *);
__declspec(dllimport) int ZwClose(void *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
static int Query(void *Key, const U16 *Text, U64 *Output) {
  STRING Name;
  RtlInitUnicodeString(&Name, Text);
  U32 Value[5], Required = 0;
  int Status = ZwQueryValueKey(Key, &Name, 2, Value, sizeof(Value), &Required);
  if (Status || Required != 20 || Value[1] != 11 || Value[2] != 8)
    return 0;
  *Output = (U64)Value[3] | ((U64)Value[4] << 32);
  return 1;
}
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  U64 Attributes[6] = {48, 0, (U64)Path, 0x240, 0, 0};
  void *Key = 0;
  U64 CountAddress = 0, PointerAddress = 0, Expected = 0, Present = 0, Mode = 0;
  if (ZwOpenKey(&Key, 1, Attributes))
    return (int)0xc000000d;
  int Good = Query(Key, (const U16 *)L"McfgCountAddress", &CountAddress) &&
             Query(Key, (const U16 *)L"McfgPointerAddress", &PointerAddress) &&
             Query(Key, (const U16 *)L"McfgExpectedCount", &Expected) &&
             Query(Key, (const U16 *)L"McfgExpectedPresent", &Present) &&
             Query(Key, (const U16 *)L"McfgTestMode", &Mode);
  ZwClose(Key);
  if (!Good || !CountAddress || !PointerAddress)
    return (int)0xc000000d;
  U32 Count = *(volatile U32 *)CountAddress;
  U8 *Table = (U8 *)*(volatile U64 *)PointerAddress;
  if (Count != Expected || (!!Table) != Present)
    return (int)0xc000000d;
  if (Table) {
    U32 Length = *(volatile U32 *)(Table + 4);
    if (*(volatile U32 *)Table != 0x4746434d || Length != 44 + Count * 16)
      return (int)0xc000000d;
    U8 Checksum = 0;
    for (U32 I = 0; I < Length; ++I)
      Checksum += ((volatile U8 *)Table)[I];
    if (Checksum)
      return (int)0xc000000d;
    if (Mode == 3)
      ((volatile U8 *)Table)[0] = Table[0];
    if (Mode == 4)
      DbgPrint("unreachable-padding:%u", ((volatile U8 *)Table)[Length]);
  }
  if (Mode == 1)
    *(volatile U32 *)CountAddress = Count;
  if (Mode == 2)
    *(volatile U64 *)PointerAddress = (U64)Table;
  *(void **)((char *)Driver + 0x68) = Unload;
  DbgPrint(Present ? "mcfg-present-pass" : "mcfg-absent-pass");
  return 0;
}
