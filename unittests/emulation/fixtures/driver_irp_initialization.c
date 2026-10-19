// Original freestanding WDM client: the driver sets Status but deliberately
// leaves Information at the ordinary kernel initializer's value.
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} NAME;
typedef struct {
  U8 Prefix[0x68];
  void (*Unload)(void *);
  void *Major[28];
} DRIVER;
typedef struct {
  U8 Prefix[0x30];
  NTSTATUS Status;
  U32 Padding;
  U64 Information;
} IRP;
_Static_assert(__builtin_offsetof(IRP, Information) == 0x38, "x64 IoStatus");
__declspec(dllimport) void RtlInitUnicodeString(NAME *, const U16 *);
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER *, U32, NAME *, U32, U32,
                                              U8, void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void IofCompleteRequest(IRP *, U8);
static void *Device;
static NTSTATUS Dispatch(void *Target, IRP *Request) {
  NTSTATUS Status = Target == Device ? 0 : (NTSTATUS)0xc000000d;
  Request->Status = Status;
  IofCompleteRequest(Request, 0);
  return Status;
}
static void Unload(void *Driver) {
  (void)Driver;
  IoDeleteDevice(Device);
}
// Preserve an original DIR64 relocation for the rebased execution cases.
static NTSTATUS (*volatile DispatchPointer)(void *, IRP *) = Dispatch;
NTSTATUS DriverEntry(DRIVER *Driver, const void *Path) {
  (void)Path;
  NAME Name;
  RtlInitUnicodeString(&Name, L"\\Device\\NeverDInitIRP");
  NTSTATUS Status = IoCreateDevice(Driver, 0, &Name, 0x22, 0, 0, &Device);
  if (Status < 0)
    return Status;
  for (U32 I = 0; I < 28; ++I)
    Driver->Major[I] = (void *)DispatchPointer;
  Driver->Unload = Unload;
  return 0;
}
