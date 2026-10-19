// Original client of the NETIO kernel exports; no host network access.
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int STATUS;
__declspec(dllimport) STATUS GetIfTable2Ex(U32, void **);
__declspec(dllimport) void FreeMibTable(void *);
__declspec(dllimport) int DbgPrint(const char *, ...);
static U32 word(const U8 *P) { return *(const U32 *)P; }
static U64 quad(const U8 *P) { return *(const U64 *)P; }
static void Unload(void *D) {
  (void)D;
  DbgPrint("netio unload passed");
}
STATUS DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  void *Invalid = (void *)0x1234;
  if ((U32)GetIfTable2Ex(3, &Invalid) != 0xc000000d ||
      Invalid != (void *)0x1234)
    return (STATUS)0xc000000d;
  for (U32 L = 0; L < 3; ++L) {
    U8 *Table = 0, *Other = 0;
    if (GetIfTable2Ex(L, (void **)&Table) || !Table || word(Table) != 2 ||
        word(Table + 4) || word(Table + 8 + 2 * 1352))
      return (STATUS)0xc000000d;
    for (U32 I = 0; I < 2; ++I) {
      const U8 *R = Table + 8 + I * 1352;
      if (quad(R) != ((U64)6 << 48 | (U64)(I + 1) << 24) ||
          word(R + 8) != I + 1 || word(R + 0x420) != 6 || R[0x424] != 2 ||
          R[0x429] != I + 1 || word(R + 0x484) != (L == 1 ? 1 : 2) ||
          quad(R + 0x4b8) != (L == 2 ? 0 : 1000 * (L + 1) + 0x4b8 + I + 1) ||
          R[0x481] != 0xa5 || R[0x4a4] != 0x5a)
        return (STATUS)0xc000000d;
    }
    Table[8 + 0x424] = 0x77;
    if (GetIfTable2Ex(L, (void **)&Other) || !Other || Other == Table ||
        Other[8 + 0x424] != 2)
      return (STATUS)0xc000000d;
    FreeMibTable(Other);
    FreeMibTable(Table);
  }
  *(void (**)(void *))((U8 *)Driver + 0x68) = Unload;
  DbgPrint("netio interface observations passed");
  return 0;
}
