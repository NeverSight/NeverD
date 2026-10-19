typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned short W;
typedef struct {
  W Length, MaximumLength;
  W *Buffer;
} STRING;
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const W *);
__declspec(dllimport) int IoCreateDevice(void *, U32, STRING *, U32, U32,
                                         unsigned char, void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) int DbgPrint(const char *, ...);
__declspec(dllimport) void IofCompleteRequest(void *, unsigned char);
static void *Device, *Buffer;
int DriverEntry(void *, STRING *);
static void *volatile EntryAnchor = (void *)DriverEntry;
static int Dispatch(void *D, void *Packet) {
  unsigned char *P = (unsigned char *)Packet;
  unsigned char *Stack = *(unsigned char **)(P + 0xb8);
  int Status = 0;
  U64 Information = 0;
  if (D != Device)
    Status = (int)0xc000000d;
  else if (Stack[0] == 14) {
    if (*(U32 *)(Stack + 0x18) != 0x222000 || *(U32 *)(Stack + 8) < 4)
      Status = (int)0xc000000d;
    else {
      **(U32 **)(P + 0x18) = *(U32 *)Buffer;
      Information = 4;
      DbgPrint("child service dispatch executed");
    }
  } else if (Stack[0] != 0 && Stack[0] != 2 && Stack[0] != 18)
    Status = (int)0xc0000010;
  *(int *)(P + 0x30) = Status;
  *(U64 *)(P + 0x38) = Information;
  IofCompleteRequest(Packet, 0);
  return Status;
}
static void Unload(void *D) {
  (void)D;
  if (Device)
    IoDeleteDevice(Device);
  if (Buffer)
    ExFreePoolWithTag(Buffer, 0x53455256);
  DbgPrint("child service unload executed");
}
int DriverEntry(void *D, STRING *Path) {
  if (EntryAnchor != (void *)DriverEntry)
    return (int)0xc000000d;
  if (!Path || !Path->Length || !Path->Buffer)
    return (int)0xc000000d;
#ifdef FAIL_ENTRY
  DbgPrint("child failure entry executed");
  return (int)0xc0000022;
#else
  STRING Name;
  RtlInitUnicodeString(&Name, (const W *)L"\\Device\\NeverDServiceChild");
  int Status = IoCreateDevice(D, 16, &Name, 0x22, 0, 0, &Device);
  if (Status)
    return Status;
  Buffer = ExAllocatePoolWithTag(0x200, 32, 0x53455256);
  if (!Buffer) {
    IoDeleteDevice(Device);
    return (int)0xc000009a;
  }
  *(U32 *)Buffer = 0x19041;
  for (U32 I = 0; I < 28; ++I)
    if (I == 0 || I == 2 || I == 14 || I == 18)
      ((U64 *)D)[0x70 / 8 + I] = (U64)Dispatch;
#ifndef NO_UNLOAD
  ((U64 *)D)[0x68 / 8] = (U64)Unload;
#endif
  DbgPrint("child service entry executed");
  return 0;
#endif
}
