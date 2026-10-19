// Walk the kernel's original PE exports and call the discovered modeled API.
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
__declspec(dllimport) int ZwQuerySystemInformation(U32, void *, U32, U32 *);
static __declspec(align(8)) U8 Modules[1024];
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  if (ZwQuerySystemInformation(11, Modules, sizeof(Modules), (void *)0))
    return (int)0xc000000d;
  const U8 *Base = (const U8 *)*(U64 *)(Modules + 24);
  if (*(const U16 *)Base != 0x5a4d)
    return (int)0xc000000d;
  const U8 *PE = Base + *(const U32 *)(Base + 0x3c);
  if (*(const U32 *)PE != 0x4550 || *(const U16 *)(PE + 24) != 0x20b)
    return (int)0xc000000d;
  const U32 *Directory = (const U32 *)(Base + *(const U32 *)(PE + 0x88));
  const U32 *Functions = (const U32 *)(Base + Directory[7]);
  const U32 *Names = (const U32 *)(Base + Directory[8]);
  const U16 *Ordinals = (const U16 *)(Base + Directory[9]);
  static const char Target[] = "KeGetCurrentIrql";
  for (U32 I = 0; I < Directory[6]; ++I) {
    const char *Name = (const char *)(Base + Names[I]);
    U32 J = 0;
    while (Target[J] && Name[J] == Target[J])
      ++J;
    if (!Target[J] && !Name[J]) {
      if (Ordinals[I] >= Directory[5])
        return (int)0xc000000d;
      U8 (*Routine)(void) = (void *)(Base + Functions[Ordinals[I]]);
      return Routine() ? (int)0xc000000d : 0;
    }
  }
  return (int)0xc000000d;
}
