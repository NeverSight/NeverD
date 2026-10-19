typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned char U8;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  void *Buffer;
} STRING;
__declspec(dllimport) void RtlUpperString(STRING *, const STRING *);
__declspec(dllimport) U8 RtlUpperChar(U8);
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const STRING *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
#define NEVERD_ANSI_UPPER_EXPECTED(...)                                        \
  static const U8 Expected[] = {__VA_ARGS__};
#include "../AnsiUpperExpected.def"
#undef NEVERD_ANSI_UPPER_EXPECTED
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  U8 Input[256], Output[258];
  for (U32 I = 0; I < 256; ++I) {
    Input[I] = (U8)I;
    Output[I] = 0xa5;
  }
  Output[256] = 0xa5;
  Output[257] = 0x5a;
  STRING Source = {256, 256, 0, Input},
         Destination = {0, 256, 0x12345678, Output};
  RtlUpperString(&Destination, &Source);
  for (U32 I = 0; I < 256; ++I)
    if (Output[I] != Expected[I] || RtlUpperChar((U8)I) != Expected[I])
      return (int)0xc000000d;
  if (Destination.Length != 256 || Destination.MaximumLength != 256 ||
      Destination.Padding != 0x12345678 || Output[256] != 0xa5 ||
      Output[257] != 0x5a)
    return (int)0xc000000d;
  STRING Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"RtlUpperString");
  void (*Dynamic)(STRING *, const STRING *) = MmGetSystemRoutineAddress(&Name);
  if (!Dynamic)
    return (int)0xc000000d;
  U8 Overlap[5] = {'a', 'b', 'c', 'd', 0xa5};
  Source = (STRING){4, 4, 0, Overlap};
  Destination = (STRING){0, 4, 0, Overlap + 1};
  Dynamic(&Destination, &Source);
  if (Destination.Length != 4 || Overlap[0] != 'a' || Overlap[1] != 'A' ||
      Overlap[2] != 'A' || Overlap[3] != 'A' || Overlap[4] != 'A')
    return (int)0xc000000d;
  Source = (STRING){0, 0, 0, 0};
  Destination = (STRING){1, 0, 0, 0};
  RtlUpperString(&Destination, &Source);
  if (Destination.Length)
    return (int)0xc000000d;
  DbgPrint("ANSI uppercase byte contract passed");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
