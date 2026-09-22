//===- driver_io.c - Original software WDM lifecycle fixture --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Freestanding x64 driver exercising public WDM request structures.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;

typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  const U16 *Buffer;
} UNICODE_STRING;

typedef struct {
  U8 Prefix[0x68];
  void *DriverUnload;
  void *MajorFunction[28];
} DRIVER_OBJECT;

typedef struct {
  U8 Prefix[0x18];
  void *FsContext;
} FILE_OBJECT;

typedef struct {
  U8 MajorFunction, MinorFunction, Flags, Control;
  U32 Padding;
  U32 OutputLength, OutputPadding;
  U32 InputLength, InputPadding;
  U32 ControlCode, CodePadding;
  void *Type3Input;
  void *Device;
  FILE_OBJECT *File;
} IO_STACK_LOCATION;

typedef struct {
  U8 Prefix[0x18];
  U8 *SystemBuffer;
  U8 ThreadList[0x10];
  NTSTATUS Status;
  U32 StatusPadding;
  U64 Information;
  U8 Middle[0x78];
  IO_STACK_LOCATION *Stack;
} IRP;

_Static_assert(__builtin_offsetof(IRP, Status) == 0x30, "x64 IoStatus");
_Static_assert(__builtin_offsetof(IRP, Stack) == 0xb8, "x64 IRP stack pointer");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, File) == 0x30,
               "x64 stack file object");

__declspec(dllimport) void RtlInitUnicodeString(UNICODE_STRING *, const U16 *);
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER_OBJECT *, U32,
                                              UNICODE_STRING *, U32, U32, U8,
                                              void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) NTSTATUS IoCreateSymbolicLink(UNICODE_STRING *,
                                                    UNICODE_STRING *);
__declspec(dllimport) NTSTATUS IoDeleteSymbolicLink(UNICODE_STRING *);
__declspec(dllimport) void IofCompleteRequest(IRP *, U8);

static void *Device;
static UNICODE_STRING DeviceName, LinkName;
static const U16 *SavedRegistryBuffer;

static NTSTATUS Dispatch(void *Target, IRP *Request) {
  IO_STACK_LOCATION *Stack = Request->Stack;
  NTSTATUS Status = 0;
  U64 Information = 0;
  if (Target != Device || Stack->Device != Device || !Stack->File)
    Status = (NTSTATUS)0xc000000dU;
  else if (Stack->MajorFunction == 0) {
    Stack->File->FsContext = Device;
    Information = 1;
  } else if (Stack->MajorFunction == 18) {
    if (Stack->File->FsContext != Device)
      Status = (NTSTATUS)0xc000000dU;
    Stack->File->FsContext = 0;
  } else if (Stack->MajorFunction == 2) {
    if (Stack->File->FsContext)
      Status = (NTSTATUS)0xc000000dU;
  } else if (Stack->MajorFunction == 14) {
    if (Stack->ControlCode == 0x222004U)
      return 0; // Deliberately invalid: no completion.
    if (Stack->ControlCode == 0x222008U)
      return 0x103; // Requires asynchronous execution, outside this profile.
    if (Stack->ControlCode == 0x222010U) {
      // Deliberately invalid: this borrowed DriverEntry buffer has expired.
      Request->SystemBuffer[0] =
          (U8) * (const volatile U16 *)SavedRegistryBuffer;
      Information = 1;
    } else if (Stack->ControlCode == 0x22200cU)
      Information = (U64)Stack->OutputLength + 1;
    else if (Stack->ControlCode != 0x222000U ||
             Stack->File->FsContext != Device)
      Status = (NTSTATUS)0xc0000010U;
    else if (Stack->OutputLength < Stack->InputLength)
      Status = (NTSTATUS)0xc0000023U;
    else {
      for (U32 I = 0; I < Stack->InputLength; ++I)
        Request->SystemBuffer[I] ^= 0x5a;
      Information = Stack->InputLength;
    }
  } else
    Status = (NTSTATUS)0xc0000010U;
  Request->Status = Status;
  Request->Information = Information;
  IofCompleteRequest(Request, 0);
  return Status;
}

static void Unload(DRIVER_OBJECT *Driver) {
  (void)Driver;
  IoDeleteSymbolicLink(&LinkName);
  IoDeleteDevice(Device);
}

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *RegistryPath) {
  SavedRegistryBuffer = RegistryPath->Buffer;
  RtlInitUnicodeString(&DeviceName, L"\\Device\\NeverDIO");
  RtlInitUnicodeString(&LinkName, L"\\DosDevices\\NeverDIO");
  NTSTATUS Status =
      IoCreateDevice(Driver, 0, &DeviceName, 0x22, 0x100, 0, &Device);
  if (Status < 0)
    return Status;
  Status = IoCreateSymbolicLink(&LinkName, &DeviceName);
  if (Status < 0) {
    IoDeleteDevice(Device);
    return Status;
  }
  Driver->MajorFunction[0] = (void *)Dispatch;
  Driver->MajorFunction[2] = (void *)Dispatch;
  Driver->MajorFunction[14] = (void *)Dispatch;
  Driver->MajorFunction[18] = (void *)Dispatch;
  Driver->DriverUnload = (void *)Unload;
  return 0;
}
