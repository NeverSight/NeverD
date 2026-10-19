typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned char U8;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} NAME;
__declspec(dllimport) U16 RtlAnsiCharToUnicodeChar(U8 **);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const NAME *);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) void KeLowerIrql(U8);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  U8 Input[128];
  for (U32 I = 0; I < 128; ++I)
    Input[I] = (U8)I;
  U8 *Cursor = Input;
  for (U32 I = 0; I < 128; ++I)
    if (RtlAnsiCharToUnicodeChar(&Cursor) != I || Cursor != Input + I + 1 ||
        Input[I] != I)
      return (int)0xc000000d;
  const U16 Text[] = L"RtlAnsiCharToUnicodeChar";
  NAME Name = {sizeof(Text) - 2, sizeof(Text), Text};
  U16 (*Convert)(U8 **) = (U16 (*)(U8 **))MmGetSystemRoutineAddress(&Name);
  if (!Convert)
    return (int)0xc000000d;
  Cursor = Input + 92;
  U8 Previous = KfRaiseIrql(1);
  U16 Result = Convert(&Cursor);
  KeLowerIrql(Previous);
  if (Result != '\\' || Cursor != Input + 93)
    return (int)0xc000000d;
  DbgPrint("ANSI character conversion passed");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
