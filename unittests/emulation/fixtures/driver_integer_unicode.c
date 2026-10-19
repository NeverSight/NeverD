//===- driver_integer_unicode.c - Native integer conversion client -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  U16 *Buffer;
} STRING;
__declspec(dllimport) int RtlInt64ToUnicodeString(U64, U32, STRING *);
__declspec(dllimport) int RtlIntegerToUnicodeString(U32, U32, STRING *);
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(STRING *);
__declspec(dllimport) void *memset(void *, int, U64);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
typedef struct {
  U64 Value;
  U32 Base, Capacity, Status, Length;
  const char *Hex;
  U8 Wide;
} CASE;
#define WIDTH_RtlInt64ToUnicodeString 1
#define WIDTH_RtlIntegerToUnicodeString 0
static const CASE Cases[] = {
#define NEVERD_INTEGER_UNICODE_CASE(API, Value, Base, Cap, Status, Length,     \
                                    Hex)                                       \
  {Value, Base, Cap, Status, Length, Hex, WIDTH_##API},
#include "../IntegerUnicodeClientCases.def"
#undef NEVERD_INTEGER_UNICODE_CASE
};
static U8 unhex(char C) {
  return C <= '9' ? (U8)(C - '0') : (U8)(C - 'a' + 10);
}
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *RegistryPath) {
  (void)RegistryPath;
  for (U32 N = 0; N < sizeof(Cases) / sizeof(Cases[0]); ++N) {
    const CASE *C = &Cases[N];
    U8 Bytes[160];
    memset(Bytes, 0xa5, sizeof(Bytes));
    STRING Output = {0x1234, (U16)C->Capacity, 0x5a5a5a5a, (U16 *)Bytes};
    U32 Status =
        C->Wide ? RtlInt64ToUnicodeString(C->Value, C->Base, &Output)
                : RtlIntegerToUnicodeString((U32)C->Value, C->Base, &Output);
    if (Status != C->Status || Output.Length != C->Length ||
        Output.MaximumLength != C->Capacity || Output.Padding != 0x5a5a5a5a ||
        Output.Buffer != (U16 *)Bytes)
      return (int)0xc000000dU;
    for (U32 I = 0; I < sizeof(Bytes); ++I)
      if (Bytes[I] !=
          (U8)((unhex(C->Hex[I * 2]) << 4) | unhex(C->Hex[I * 2 + 1])))
        return (int)0xc000000dU;
  }
  STRING Name;
  RtlInitUnicodeString(&Name, L"RtlInt64ToUnicodeString");
  int (*Convert)(U64, U32, STRING *) = MmGetSystemRoutineAddress(&Name);
  U16 Text[32];
  STRING Output = {0x1234, sizeof(Text), 0, Text};
  if (!Convert || Convert(~0ULL, 10, &Output) || Output.Length != 40 ||
      Text[0] != '1' || Text[19] != '5' || Text[20])
    return (int)0xc000000dU;
  *(void **)((U8 *)Driver + 0x68) = Unload;
  DbgPrint("integer Unicode conversion passed");
  return 0;
}
