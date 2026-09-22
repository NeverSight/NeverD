//===- driver_direct.c - Original direct and stream I/O fixture -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Freestanding x64 WDM fixture using documented MDL and request layouts.
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
  U8 Prefix[0x30];
  U32 Flags;
} DEVICE_OBJECT;
typedef struct {
  U8 Prefix[0x18];
  void *FsContext;
} FILE_OBJECT;
typedef struct MDL {
  struct MDL *Next;
  U16 Size, MdlFlags;
  U32 Padding;
  void *Process;
  U8 *MappedSystemVa;
  U8 *StartVa;
  U32 ByteCount, ByteOffset;
} MDL;
typedef struct {
  U8 MajorFunction, MinorFunction, Flags, Control;
  U32 Padding;
  union {
    struct {
      U32 OutputLength, OutputPadding;
      U32 InputLength, InputPadding;
      U32 ControlCode, CodePadding;
      void *Type3Input;
    } Ioctl;
    struct {
      U32 Length, LengthPadding;
      U32 Key, KeyPadding;
      U64 ByteOffset;
    } Stream;
  } Parameters;
  DEVICE_OBJECT *Device;
  FILE_OBJECT *File;
} IO_STACK_LOCATION;
typedef struct {
  U8 Prefix[8];
  MDL *MdlAddress;
  U32 Flags, Padding;
  U8 *SystemBuffer;
  U8 ThreadList[0x10];
  NTSTATUS Status;
  U32 StatusPadding;
  U64 Information;
  U8 Middle[0x78];
  IO_STACK_LOCATION *Stack;
} IRP;

_Static_assert(sizeof(MDL) == 0x30, "x64 MDL header");
_Static_assert(__builtin_offsetof(MDL, MappedSystemVa) == 0x18,
               "x64 mapped VA");
_Static_assert(__builtin_offsetof(IRP, Stack) == 0xb8, "x64 IRP stack");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION,
                                  Parameters.Stream.ByteOffset) == 0x18,
               "x64 stream offset");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, File) == 0x30,
               "x64 file object");

__declspec(dllimport) void RtlInitUnicodeString(UNICODE_STRING *, const U16 *);
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER_OBJECT *, U32,
                                              UNICODE_STRING *, U32, U32, U8,
                                              void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void IofCompleteRequest(IRP *, U8);
__declspec(dllimport) void *MmMapLockedPagesSpecifyCache(MDL *, U32, U32,
                                                         void *, U32, U32);
__declspec(dllimport) void MmUnmapLockedPages(void *, MDL *);

static DEVICE_OBJECT *DirectDevice, *BufferedDevice, *NeitherDevice;
static volatile U64 Observed;

// This follows the documented macro contract: an existing system mapping is
// returned directly, otherwise the mapping API is called. No function import
// is substituted for the guest's actual MDL-field reads.
static U8 *SystemAddress(MDL *Mdl, U32 Priority) {
  if (Mdl->MdlFlags & 5)
    return Mdl->MappedSystemVa;
  return (U8 *)MmMapLockedPagesSpecifyCache(Mdl, 0, 1, 0, 0, Priority);
}

static NTSTATUS Dispatch(DEVICE_OBJECT *Device, IRP *Request) {
  IO_STACK_LOCATION *Stack = Request->Stack;
  NTSTATUS Status = 0;
  U64 Information = 0;
  U8 *Buffer = 0;
  U32 Length = 0;
  if (Stack->MajorFunction == 0) {
    Stack->File->FsContext = 0;
    Information = 1;
  } else if (Stack->MajorFunction == 18) {
    Stack->File->FsContext = 0;
  } else if (Stack->MajorFunction == 3 || Stack->MajorFunction == 4) {
    Length = Stack->Parameters.Stream.Length;
    if (Length)
      Buffer = Device == DirectDevice
                   ? SystemAddress(Request->MdlAddress, 0x40000010U)
                   : Request->SystemBuffer;
    if (Stack->Parameters.Stream.Key)
      Status = (NTSTATUS)0xc000000dU;
    else if (Stack->MajorFunction == 4) {
      U64 Sum = Stack->Parameters.Stream.ByteOffset;
      for (U32 I = 0; I < Length; ++I)
        Sum += Buffer[I];
      Stack->File->FsContext = (void *)Sum;
    } else {
      for (U32 I = 0; I < Length; ++I)
        Buffer[I] = (U8)((U64)Stack->File->FsContext +
                         Stack->Parameters.Stream.ByteOffset + I);
    }
    Information = Length;
  } else if (Stack->MajorFunction == 14) {
    const U32 Code = Stack->Parameters.Ioctl.ControlCode;
    const U32 Action = (Code - 0x222000U) >> 2;
    const U32 Method = Code & 3;
    Length = Stack->Parameters.Ioctl.OutputLength;
    if (!Length) {
      if (Request->MdlAddress)
        Status = (NTSTATUS)0xc000000dU;
    } else if (Method) {
      MDL *Mdl = Request->MdlAddress;
      if (!Mdl || Mdl->Next || Mdl->ByteCount != Length ||
          Mdl->ByteOffset >= 4096)
        Status = (NTSTATUS)0xc000000dU;
      else {
        const U32 Priority = Action == 2                    ? 0xc0000010U
                             : Action == 12 || Action == 14 ? 0x10U
                                                            : 0x40000010U;
        Buffer = SystemAddress(Mdl, Priority);
        if (((U64)Buffer & 4095) != Mdl->ByteOffset ||
            SystemAddress(Mdl, Priority) != Buffer)
          Status = (NTSTATUS)0xc000000dU;
        if (Action == 12 || Action == 13) {
          Observed = ((U64 (*)(void))Buffer)();
          if (Observed != 0x5a)
            Status = (NTSTATUS)0xc000000dU;
          Information = Length;
          goto Complete;
        } else if (Action == 14) {
          ((void (*)(void))(Buffer + Length))();
        } else if (Action == 15 && !Mdl->ByteOffset) {
          Status = (NTSTATUS)0xc000000dU;
        } else if (Action == 3) {
          MmUnmapLockedPages(Buffer, Mdl);
          Observed = *(volatile U8 *)Buffer;
        } else if (Action == 4) {
          Buffer[0] = 0x39;
          MmUnmapLockedPages(Buffer, Mdl);
          Buffer = SystemAddress(Mdl, Priority);
          if (Buffer[0] != 0x39)
            Status = (NTSTATUS)0xc000000dU;
        } else if (Action == 5) {
          Observed = *(volatile U64 *)(Mdl + 1); // Physical PFNs are unmodeled.
        } else if (Action == 8) {
          MmMapLockedPagesSpecifyCache(Mdl, 0, 1, 0, 0, Priority);
        } else if (Action == 9) {
          MmUnmapLockedPages(Buffer + 1, Mdl);
        } else if (Action == 10) {
          MmUnmapLockedPages(Buffer, Mdl);
          MmUnmapLockedPages(Buffer, Mdl);
        } else if (Action == 11) {
          // Complete an explicitly unmapped buffer: locked request data lives
          // until completion even after its system mapping has been released.
          Buffer[0] = 0x71;
          MmUnmapLockedPages(Buffer, Mdl);
          Information = Length;
          goto Complete;
        }
        if (Action == 7)
          ((volatile U8 *)Buffer)[Length] = 0x7f;
        for (U32 I = 0; I < Length; ++I)
          Buffer[I] ^= Stack->Parameters.Ioctl.InputLength
                           ? Request->SystemBuffer[0]
                           : 0x5a;
        Information = Length;
        if (Action == 6) {
          Request->Status = 0;
          Request->Information = Information;
          IofCompleteRequest(Request, 0);
          Observed = *(volatile U8 *)Buffer;
          return 0;
        }
      }
    } else {
      Buffer = Request->SystemBuffer;
      for (U32 I = 0; I < Length; ++I)
        Buffer[I] = (U8)I;
      Information = Length;
    }
  }
Complete:
  Request->Status = Status;
  Request->Information = Information;
  IofCompleteRequest(Request, 0);
  return Status;
}

static void Unload(DRIVER_OBJECT *Driver) {
  (void)Driver;
  IoDeleteDevice(NeitherDevice);
  IoDeleteDevice(BufferedDevice);
  IoDeleteDevice(DirectDevice);
}

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *RegistryPath) {
  (void)RegistryPath;
  UNICODE_STRING Name;
  RtlInitUnicodeString(&Name, L"\\Device\\NeverDDirect");
  NTSTATUS Status =
      IoCreateDevice(Driver, 0, &Name, 0x22, 0, 0, (void **)&DirectDevice);
  if (Status < 0)
    return Status;
  DirectDevice->Flags = 0x10;
  RtlInitUnicodeString(&Name, L"\\Device\\NeverDBuffered");
  Status =
      IoCreateDevice(Driver, 0, &Name, 0x22, 0, 0, (void **)&BufferedDevice);
  if (Status < 0)
    return Status;
  BufferedDevice->Flags = 4;
  RtlInitUnicodeString(&Name, L"\\Device\\NeverDNeither");
  Status =
      IoCreateDevice(Driver, 0, &Name, 0x22, 0, 0, (void **)&NeitherDevice);
  if (Status < 0)
    return Status;
  NeitherDevice->Flags = 0;
  Driver->MajorFunction[0] = (void *)Dispatch;
  Driver->MajorFunction[2] = (void *)Dispatch;
  Driver->MajorFunction[3] = (void *)Dispatch;
  Driver->MajorFunction[4] = (void *)Dispatch;
  Driver->MajorFunction[14] = (void *)Dispatch;
  Driver->MajorFunction[18] = (void *)Dispatch;
  Driver->DriverUnload = (void *)Unload;
  return 0;
}
