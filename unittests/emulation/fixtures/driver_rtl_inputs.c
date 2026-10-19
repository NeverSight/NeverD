typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned char U8;
typedef unsigned long long U64;
typedef struct {
  U16 Length, Maximum;
  U32 Padding;
  const U16 *Buffer;
} STRING;
#define API __declspec(dllimport)
API U32 RtlUnicodeStringToInteger(STRING *, U32, U32 *);
API U32 RtlCharToInteger(const char *, U32, U32 *);
API U32 RtlxAnsiStringToUnicodeSize(STRING *);
API U32 RtlGUIDFromString(STRING *, void *);
API U8 RtlPrefixUnicodeString(STRING *, STRING *, U8);
API U32 RtlAppendUnicodeToString(STRING *, const U16 *);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  const U16 Number[] = L" -0b10001tail";
  STRING Input = {sizeof(Number) - 2, sizeof(Number), 0, Number};
  U32 Value = 0;
  if (RtlUnicodeStringToInteger(&Input, 0, &Value) || Value != (U32)-17)
    return (int)0xc000000d;
  if (RtlUnicodeStringToInteger(&Input, 3, &Value) != 0xc000000d || Value)
    return (int)0xc000000d;
  Value = 0xa5a5a5a5;
  if (RtlCharToInteger("-0o77", 0, &Value) || Value != (U32)-63)
    return (int)0xc000000d;
  if (RtlCharToInteger("12", 3, &Value) != 0xc000000d || Value != (U32)-63)
    return (int)0xc000000d;
  const char Ansi[] = "a\0bc";
  STRING A = {4, 0, 0, (const U16 *)Ansi};
  if (RtlxAnsiStringToUnicodeSize(&A) != 10)
    return (int)0xc000000d;
  const U16 GUID[] = L"{01234567-89AB-CDEF-0123-456789ABCDE!}";
  Input.Length = sizeof(GUID) - 2;
  Input.Maximum = sizeof(GUID);
  Input.Buffer = GUID;
  U64 Result[2] = {~(U64)0, ~(U64)0};
  if (RtlGUIDFromString(&Input, Result) != 0xc000000d ||
      Result[0] != 0xcdef89ab01234567ULL || Result[1] != ~(U64)0)
    return (int)0xc000000d;
  const U16 Prefix[] = L"Ab", Text[] = L"aBc";
  STRING P = {4, 6, 0, Prefix}, T = {6, 8, 0, Text};
  if (!RtlPrefixUnicodeString(&P, &T, 1) || RtlPrefixUnicodeString(&P, &T, 0))
    return (int)0xc000000d;
  U16 Buffer[5] = {'a', 0xa5a5, 0xa5a5, 0xa5a5, 0xa5a5};
  STRING D = {2, 8, 0, Buffer};
  if (RtlAppendUnicodeToString(&D, L"bc") || D.Length != 6 ||
      Buffer[0] != 'a' || Buffer[1] != 'b' || Buffer[2] != 'c' || Buffer[3] ||
      Buffer[4] != 0xa5a5)
    return (int)0xc000000d;
  if (RtlAppendUnicodeToString(&D, L"de") != 0xc0000023 || D.Length != 6)
    return (int)0xc000000d;
  *(void **)((U8 *)Driver + 0x68) = Unload;
  return 0;
}
