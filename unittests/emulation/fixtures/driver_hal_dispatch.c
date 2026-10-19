typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef const U8 *(*GETTABLE)(U32, const char *, const char *);
__declspec(dllimport) extern const U8 HalDispatchTable[];
__declspec(dllimport) int DbgPrint(const char *, ...);
static void Unload(void *D) {
  (void)D;
  DbgPrint("hal dispatch unload passed");
}
int DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  const U8 *D = HalDispatchTable;
  if (*(const U32 *)D != 4)
    return (int)0xc000000d;
  GETTABLE Query = *(GETTABLE *)(D + 0xa8);
  if (!Query)
    return (int)0xc000000d;
  const U8 *First = Query(0x54445353, 0, 0),
           *Second = Query(0x54445353, "OTHER ", "ALT     ");
  if (!First || !Second || First == Second || *(const U32 *)(First + 24) != 1 ||
      *(const U32 *)(Second + 24) != 2 || Query(0x54445353, 0, 0) != First ||
      Query(0x54445353, "neverd", 0) || Query(0x54445358, 0, 0) ||
      Query(0x54445352, 0, 0))
    return (int)0xc000000d;
  const U8 *Apic = Query(0x43495041, 0, 0);
  if (!Apic || *(const U32 *)(Apic + 4) != 52)
    return (int)0xc000000d;
  const U8 *Mcfg = Query(0x4746434d, 0, 0);
  if (!Mcfg || *(const U32 *)(Mcfg + 4) != 76 ||
      *(const U64 *)(Mcfg + 44) != 0x80000000ULL)
    return (int)0xc000000d;
  U8 Sum = 0;
  for (U32 I = 0; I < *(const U32 *)(First + 4); ++I)
    Sum += First[I];
  if (Sum)
    return (int)0xc000000d;
  *(void (**)(void *))((U8 *)Driver + 0x68) = Unload;
  DbgPrint("hal cached acpi pointer passed");
  return 0;
}
