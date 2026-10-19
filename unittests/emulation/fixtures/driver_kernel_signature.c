// Scan original reference code and call the unique discovered address.
// The harness supplies observed bytes and a bounded executable scan window.
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
__declspec(dllimport) int ZwQuerySystemInformation(U32, void *, U32, U32 *);
static volatile struct {
  U32 StartRVA, WindowBytes, SignatureBytes, CallOffset;
  U8 Signature[16];
} SignatureRequest = {0x5a5a5a5a, 0x0badcafe, 0x13579bdf, 0x2468ace0, {0}};
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
  const U32 Size = *(const U32 *)(PE + 24 + 56);
  const U32 Start = SignatureRequest.StartRVA;
  const U32 Window = SignatureRequest.WindowBytes;
  const U32 Length = SignatureRequest.SignatureBytes;
  if (!Length || Length > 16 || Window < Length || Window > 4096 ||
      Start > Size || Window > Size - Start)
    return (int)0xc000000d;
  const U8 *Found = (void *)0;
  for (U32 I = 0; I <= Window - Length; ++I) {
    U32 J = 0;
    while (J < Length && Base[Start + I + J] == SignatureRequest.Signature[J])
      ++J;
    if (J == Length) {
      if (Found)
        return (int)0xc0000035;
      Found = Base + Start + I;
    }
  }
  if (!Found)
    return (int)0xc0000225;
  U8 (*Routine)(void) = (void *)(Found + SignatureRequest.CallOffset);
  return Routine() ? (int)0xc000000d : 0;
}
