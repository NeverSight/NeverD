typedef unsigned short U16;
typedef unsigned int U32;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  const void *Buffer;
} STRING;
__declspec(dllimport) int RtlCompareString(const STRING *, const STRING *,
                                           unsigned char);
__declspec(dllimport) int RtlAppendStringToString(STRING *, const STRING *);
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const STRING *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
#define NEVERD_ANSI_COMPARE_CASE(Name, L, LL, R, RL, Flag, Expected)           \
  {                                                                            \
    STRING Left = {LL, 0, 0, L}, Right = {RL, 0, 0, R};                        \
    if (RtlCompareString(&Left, &Right, Flag) != (Expected))                   \
      return (int)0xc000000d;                                                  \
  }
#include "../AnsiCompareCases.def"
#undef NEVERD_ANSI_COMPARE_CASE
  char Output[9] = {'a', 'b', '*', '*', '*', '*', '*', '*', '*'};
  STRING Destination = {2, 8, 0x12345678, Output}, Source = {2, 0, 0, "CD"};
  if (RtlAppendStringToString(&Destination, &Source) ||
      Destination.Length != 4 || Output[0] != 'a' || Output[1] != 'b' ||
      Output[2] != 'C' || Output[3] != 'D' || Output[4] != '*')
    return (int)0xc000000d;
  STRING Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"RtlCompareString");
  int (*Compare)(const STRING *, const STRING *, unsigned char) =
      MmGetSystemRoutineAddress(&Name);
  Source = (STRING){4, 0, 0, "ABCD"};
  if (!Compare || Compare(&Destination, &Source, 1))
    return (int)0xc000000d;
  Source = (STRING){5, 0, 0, "xxxxx"};
  if ((U32)RtlAppendStringToString(&Destination, &Source) != 0xc0000023 ||
      Destination.Length != 4)
    return (int)0xc000000d;
  Source = (STRING){0, 0, 0, 0};
  if (RtlAppendStringToString(0, &Source) ||
      RtlAppendStringToString(&Destination, &Destination) ||
      Destination.Length != 8 || Destination.MaximumLength != 8 ||
      Destination.Padding != 0x12345678 || Output[8] != '*')
    return (int)0xc000000d;
  DbgPrint("ANSI compare/append passed");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
