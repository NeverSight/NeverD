typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} NAME;
typedef struct {
  U32 Length;
  void *Root;
  NAME *Name;
  U32 Flags;
  void *Security, *QoS;
} ATTR;
__declspec(dllimport) U32 ZwOpenSection(void **, U32, const ATTR *);
__declspec(dllimport) U32 ZwMapViewOfSection(void *, void *, void **, U64, U64,
                                             U64 *, U64 *, U32, U32, U32);
__declspec(dllimport) U32 ZwUnmapViewOfSection(void *, void *);
__declspec(dllimport) U32 ZwClose(void *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) U32 PsSetCreateProcessNotifyRoutine(
    void (*)(U64, U64, unsigned char), unsigned char);
__declspec(dllimport) unsigned char MmIsAddressValid(void *);
static void *Shared, *Private;
static U32 Count, Bad;
static void Notify(U64 Parent, U64 PID, unsigned char Create) {
  if ((Count == 0 && (Parent != 4 || PID != 100 || !Create)) ||
      (Count == 1 && (Parent != 100 || PID != 200 || !Create)) ||
      (Count == 2 && (PID != 200 || Create)) ||
      (Count == 3 && (PID != 100 || Create)) ||
      !!MmIsAddressValid(Private) != (Count == 0) ||
      *(volatile U32 *)Shared != (Count < 2 ? 0x12345678 : 0x76543210))
    Bad = 1;
  if (Count == 1)
    *(volatile U32 *)Shared = 0x76543210;
  ++Count;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (Bad || (Count && Count != 4) ||
      *(volatile U32 *)Private != (Count ? 0x76543210 : 0x12345678) ||
      PsSetCreateProcessNotifyRoutine(Notify, 1) ||
      ZwUnmapViewOfSection((void *)-1, Shared) ||
      ZwUnmapViewOfSection((void *)-1, Private))
    *(volatile U32 *)0 = 0xbad;
}
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  const U16 Text[] = L"\\BaseNamedObjects\\Shared";
  NAME Name = {sizeof(Text) - 2, sizeof(Text), Text};
  ATTR A = {sizeof(A), 0, &Name, 0x240, 0, 0};
  void *H = 0, *V1 = 0, *V2 = 0;
  U64 Size1 = 0, Size2 = 4096, Offset = 0;
  if (ZwOpenSection(&H, 6, &A) ||
      ZwMapViewOfSection(H, (void *)-1, &V1, 0, 0, &Offset, &Size1, 1, 0, 4) ||
      ZwMapViewOfSection(H, (void *)-1, &V2, 0, 0, &Offset, &Size2, 2, 0, 2) ||
      Size1 != 0x11000 || Size2 != 4096 || V1 == V2 ||
      *(volatile U16 *)V1 != 0x3412 ||
      *(volatile U32 *)((char *)V1 + 0x10000) != 0 || ZwClose(H))
    return (int)0xc000000d;
  *(volatile U32 *)V1 = 0x12345678;
  Shared = V1;
  Private = V2;
  if (*(volatile U32 *)V2 != 0x12345678 ||
      PsSetCreateProcessNotifyRoutine(Notify, 0))
    return (int)0xc000000d;
  DbgPrint("named section shared views passed");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
