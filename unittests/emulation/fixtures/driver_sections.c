//===- driver_sections.c - Physical views, inherited VADs and rundown
//------===//
// NeverD Decompiler
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  U64 Buffer;
} US;
typedef struct {
  U32 Length;
  U64 Root, Name;
  U32 Flags;
  U64 Security, QoS;
} OA;
#define API __declspec(dllimport)
API U64 ExAllocatePoolWithTag(U32, U64, U32);
API void ExFreePoolWithTag(U64, U32);
API U64 MmGetPhysicalAddress(U64);
API U8 MmIsAddressValid(U64);
API U32 ZwOpenSection(U64 *, U32, OA *);
API U32 ZwMapViewOfSection(U64, U64, U64 *, U64, U64, U64 *, U64 *, U32, U32,
                           U32);
API U32 ZwUnmapViewOfSection(U64, U64);
API U32 ZwClose(U64);
API U32 ZwQueryObject(U64, U32, void *, U32, U32 *);
API void ProbeForRead(U64, U64, U32);
API void ProbeForWrite(U64, U64, U32);
API U32 PsSetCreateProcessNotifyRoutine(void (*)(U64, U64, U8), U8);
API U64 PsGetCurrentProcessId(void);
static U64 Pool, Retained, PrivateView, ByteOffset;
static U32 Count, Bad;
static volatile U32 *value(U64 View) {
  return (volatile U32 *)(View + ByteOffset);
}
static void notify(U64 Parent, U64 PID, U8 Create) {
  U64 Actor = PsGetCurrentProcessId();
  if (Parent != 4 && Parent != 100)
    Bad = 1;
  if ((Count == 0 && (PID != 100 || !Create || Actor != 4)) ||
      (Count == 1 && (PID != 200 || !Create || Actor != 100)) ||
      (Count == 2 && (PID != 200 || Create || Actor != 200)) ||
      (Count == 3 && (PID != 100 || Create || Actor != 100)))
    Bad = 1;
  ProbeForRead(Retained + ByteOffset, 4, 4);
  if (!!MmIsAddressValid(PrivateView) != (Count == 0))
    Bad = 1;
  if (*value(Retained) != (Count < 2 ? 0x87654321U : 0x8899aabbU))
    Bad = 1;
  if (Count == 1)
    *value(Retained) = 0x8899aabbU;
  ++Count;
}
static void unload(U64 Driver) {
  (void)Driver;
  if (Bad || Count != 4 || *value(Retained) != 0x8899aabbU ||
      *value(PrivateView) != 0x8899aabbU ||
      PsSetCreateProcessNotifyRoutine(notify, 1) ||
      ZwUnmapViewOfSection(~(U64)0, PrivateView + 1) ||
      ZwUnmapViewOfSection(~(U64)0, Retained + 1))
    *(volatile U32 *)0 = 0xbad;
  ExFreePoolWithTag(Pool, 0x54455354);
}
U32 DriverEntry(U64 Driver, U64 Registry) {
  static const U16 Name[] = u"\\device\\physicalmemory";
  static const U16 Expected[] = u"\\Device\\PhysicalMemory";
  US S = {sizeof(Name) - 2, sizeof(Name), (U64)Name};
  OA O = {48, 0, (U64)&S, 0x40, 0, 0};
  U64 H = 0, Physical, Offset, Size, Other = 0;
  U32 Need = 0, I;
  struct {
    US Name;
    U16 Data[64];
  } Info;
  (void)Registry;
  *(U64 *)(Driver + 0x68) = (U64)unload;
  Pool = ExAllocatePoolWithTag(0, 0x2000, 0x54455354);
  if (!Pool)
    return 0xc0ffee01U;
  *(volatile U32 *)Pool = 0x12345678U;
  Physical = MmGetPhysicalAddress(Pool);
  ByteOffset = Physical & 0xfff;
  Offset = Physical & ~(U64)0xfff;
  if (ZwOpenSection(&H, 0xf001f, &O))
    return 0xc0ffee02U;
  Size = 4 + ByteOffset;
  if (ZwMapViewOfSection(H, ~(U64)0, &Retained, 0, 0, &Offset, &Size, 1, 0,
                         4) ||
      Size != 0x1000 || (Retained & 0xffff) != (Offset & 0xffff))
    return 0xc0ffee03U;
  Size = 4 + ByteOffset;
  if (ZwMapViewOfSection(H, ~(U64)0, &Other, 0, 0, &Offset, &Size, 2, 0, 2))
    return 0xc0ffee04U;
  if (*value(Other) != 0x12345678U)
    return 0xc0ffee05U;
  ProbeForWrite(Retained + ByteOffset, 4, 4);
  *value(Retained) = 0x87654321U;
  if (*(volatile U32 *)Pool != 0x87654321U || *value(Other) != 0x87654321U)
    return 0xc0ffee06U;
  if (ZwQueryObject(H, 1, &Info, sizeof(Info), &Need) ||
      Info.Name.Length != sizeof(Expected) - 2 || Need != 16 + sizeof(Expected))
    return 0xc0ffee07U;
  for (I = 0; I < (sizeof(Expected) / 2) - 1; ++I)
    if (((U16 *)Info.Name.Buffer)[I] != Expected[I])
      return 0xc0ffee08U;
  if (ZwClose(H) || *value(Other) != 0x87654321U)
    return 0xc0ffee09U;
  PrivateView = Other;
  if (PsSetCreateProcessNotifyRoutine(notify, 0))
    return 0xc0ffee0aU;
  return 0;
}
