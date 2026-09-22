//===- driver_mdl.c - Driver-owned nonpaged MDL execution fixture ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original freestanding WDM fixture for descriptor ownership and pool aliases.
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
  U32 OutputLength, OutputPadding;
  U32 InputLength, InputPadding;
  U32 ControlCode;
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
_Static_assert(__builtin_offsetof(IRP, Stack) == 0xb8, "x64 IRP stack");

__declspec(dllimport) void RtlInitUnicodeString(UNICODE_STRING *, const U16 *);
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER_OBJECT *, U32,
                                              UNICODE_STRING *, U32, U32, U8,
                                              void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void IofCompleteRequest(IRP *, U8);
__declspec(dllimport) U8 *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) U8 *ExAllocatePool2(U64, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) MDL *IoAllocateMdl(void *, U32, U8, U8, IRP *);
__declspec(dllimport) void IoFreeMdl(MDL *);
__declspec(dllimport) void MmBuildMdlForNonPagedPool(MDL *);
__declspec(dllimport) void *MmMapLockedPagesSpecifyCache(MDL *, U32, U32,
                                                         void *, U32, U32);
__declspec(dllimport) void MmUnmapLockedPages(void *, MDL *);
// Also exercise the modeled helper entry, independently of real macro reads.
__declspec(dllimport) U8 *MmGetSystemAddressForMdlSafe(MDL *, U32);

static const U32 Tag = 0x4d646c54;
static DEVICE_OBJECT *Device;
static U8 *Pool;
static MDL *Descriptor;
static volatile U64 Observed;
static U8 LeakDescriptor;

static U8 *SystemAddress(MDL *Mdl, U32 Priority) {
  if (Mdl->MdlFlags & 5)
    return Mdl->MappedSystemVa;
  return (U8 *)MmMapLockedPagesSpecifyCache(Mdl, 0, 1, 0, 0, Priority);
}

static NTSTATUS Dispatch(DEVICE_OBJECT *Ignored, IRP *Request) {
  (void)Ignored;
  NTSTATUS Status = 0;
  U64 Information = 0;
  IO_STACK_LOCATION *Stack = Request->Stack;
  if (Stack->MajorFunction == 0) {
    Information = 1;
  } else if (Stack->MajorFunction == 14) {
    const U32 Action = (Stack->ControlCode - 0x222000U) >> 2;
    U8 *Buffer = SystemAddress(Descriptor, 0xc0000010U);
    MDL *Temporary;
    U8 Local[32];
    U8 *Other;
    switch (Action) {
    case 0:
      if (Buffer != Pool + 17 ||
          MmGetSystemAddressForMdlSafe(Descriptor, 0xc0000010U) != Buffer)
        Status = (NTSTATUS)0xc000000dU;
      Request->SystemBuffer[0] = Buffer[0];
      Request->SystemBuffer[1] = Buffer[8192];
      ++Buffer[0]; // Existing pool mapping ignores new no-write requests.
      Pool[0] = 0x19;
      Pool[12287] = 0x29; // A partial descriptor does not restrict pool access.
      Information = 2;
      break;
    case 1:
      IoFreeMdl(Descriptor);
      Descriptor = 0;
      Buffer[0] = 0x61;
      if (Pool[17] != 0x61)
        Status = (NTSTATUS)0xc000000dU;
      break;
    case 2:
      Temporary = IoAllocateMdl(Local, sizeof(Local), 0, 0, 0);
      IoFreeMdl(Temporary); // Allocating a descriptor does not probe its bytes.
      Temporary = IoAllocateMdl((void *)0x12345678, 64, 0, 0, 0);
      IoFreeMdl(Temporary);
      break;
    case 3:
      Other = ExAllocatePoolWithTag(1, 64, Tag);
      MmBuildMdlForNonPagedPool(IoAllocateMdl(Other, 64, 0, 0, 0));
      break;
    case 4:
      MmBuildMdlForNonPagedPool(IoAllocateMdl(Local, sizeof(Local), 0, 0, 0));
      break;
    case 5:
      MmBuildMdlForNonPagedPool(IoAllocateMdl(Pool + 12280, 16, 0, 0, 0));
      break;
    case 6:
      Temporary = IoAllocateMdl(Pool, 16, 0, 0, 0);
      Observed = (U64)MmGetSystemAddressForMdlSafe(Temporary, 16);
      break;
    case 7:
      MmMapLockedPagesSpecifyCache(Descriptor, 0, 1, 0, 0, 16);
      break;
    case 8:
      MmUnmapLockedPages(Buffer, Descriptor);
      break;
    case 9:
      Observed = *(volatile U64 *)(Descriptor + 1);
      break;
    case 10:
      ((volatile MDL *)Descriptor)->ByteCount = 4;
      break;
    case 11:
      IoFreeMdl(Descriptor);
      Observed = ((volatile MDL *)Descriptor)->ByteCount;
      break;
    case 12:
      IoFreeMdl(Descriptor);
      IoFreeMdl(Descriptor);
      break;
    case 13:
      ExFreePoolWithTag(Pool, Tag);
      Observed = *(volatile U8 *)Buffer;
      break;
    case 14:
      ExFreePoolWithTag(Pool, Tag);
      Observed = (U64)MmGetSystemAddressForMdlSafe(Descriptor, 16);
      break;
    case 15:
      ExFreePoolWithTag(Pool, Tag);
      Pool = 0;
      IoFreeMdl(Descriptor);
      Descriptor = 0;
      break;
    case 16:
      IoAllocateMdl(Pool, 16, 0, 0, Request);
      break;
    case 17:
      IoAllocateMdl(Pool, 16, 1, 0, 0);
      break;
    case 18:
      IoAllocateMdl(Pool, 16, 0, 1, 0);
      break;
    case 19:
      IoFreeMdl(Request->MdlAddress);
      break;
    case 20:
      LeakDescriptor = 1;
      break;
    case 21:
    case 22:
    case 31:
      Other = Action == 31
                  ? ExAllocatePoolWithTag(0, 64, Tag)
                  : ExAllocatePool2(Action == 22 ? 0x100 : 0x40, 64, Tag);
      Temporary = IoAllocateMdl(Other + 3, 32, 0, 0, 0);
      MmBuildMdlForNonPagedPool(Temporary);
      if (SystemAddress(Temporary, 0xc0000010U) != Other + 3 ||
          (Action != 31 && Other[3]))
        Status = (NTSTATUS)0xc000000dU;
      SystemAddress(Temporary, 0xc0000010U)[0] = 0x75;
      if (Other[3] != 0x75)
        Status = (NTSTATUS)0xc000000dU;
      IoFreeMdl(Temporary);
      ExFreePoolWithTag(Other, Tag);
      break;
    case 23:
      Temporary = IoAllocateMdl(Pool, 16, 0, 0, 0);
      ExFreePoolWithTag(Pool, Tag);
      MmBuildMdlForNonPagedPool(Temporary);
      break;
    case 24:
      Observed = (U64)((volatile MDL *)Descriptor)->Process;
      break;
    case 25:
      IoAllocateMdl(Pool, 0, 0, 0, 0);
      break;
    case 26:
      IoAllocateMdl((void *)(~(U64)0 - 1), 16, 0, 0, 0);
      break;
    case 27:
      MmBuildMdlForNonPagedPool(Descriptor);
      break;
    case 28:
      ((volatile MDL *)Descriptor)->Next = Descriptor;
      break;
    case 29:
      IoFreeMdl((MDL *)Pool);
      break;
    case 30:
      IoAllocateMdl(Pool, 0x100001, 0, 0, 0);
      break;
    }
  }
  Request->Status = Status;
  Request->Information = Information;
  IofCompleteRequest(Request, 0);
  return Status;
}

static void Unload(DRIVER_OBJECT *Driver) {
  (void)Driver;
  if (Descriptor && !LeakDescriptor)
    IoFreeMdl(Descriptor);
  if (Pool)
    ExFreePoolWithTag(Pool, Tag);
  IoDeleteDevice(Device);
}

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *RegistryPath) {
  (void)RegistryPath;
  UNICODE_STRING Name;
  RtlInitUnicodeString(&Name, L"\\Device\\NeverDMDL");
  NTSTATUS Status =
      IoCreateDevice(Driver, 0, &Name, 0x22, 0, 0, (void **)&Device);
  if (Status < 0)
    return Status;
  Device->Flags = 4;
  Pool = ExAllocatePoolWithTag(512, 12288, Tag);
  if (!Pool)
    return (NTSTATUS)0xc000009aU;
  Pool[17] = 0x41;
  Pool[8209] = 0x42;
  Descriptor = IoAllocateMdl(Pool + 17, 8193, 0, 0, 0);
  if (!Descriptor)
    return (NTSTATUS)0xc000009aU;
  if (Descriptor->Next || Descriptor->Size != 0x48 ||
      Descriptor->ByteOffset != 17 || Descriptor->ByteCount != 8193 ||
      Descriptor->StartVa != Pool)
    return (NTSTATUS)0xc000000dU;
  MmBuildMdlForNonPagedPool(Descriptor);
  if (!(Descriptor->MdlFlags & 4) ||
      SystemAddress(Descriptor, 0xc0000010U) != Pool + 17)
    return (NTSTATUS)0xc000000dU;
  // Two MDLs over the same bytes retain one authoritative pool allocation.
  MDL *Alias = IoAllocateMdl(Pool + 17, 8, 0, 0, 0);
  MmBuildMdlForNonPagedPool(Alias);
  SystemAddress(Alias, 16)[1] = 0x51;
  IoFreeMdl(Alias);
  if (SystemAddress(Descriptor, 16)[1] != 0x51)
    return (NTSTATUS)0xc000000dU;
  Driver->MajorFunction[0] = (void *)Dispatch;
  Driver->MajorFunction[2] = (void *)Dispatch;
  Driver->MajorFunction[14] = (void *)Dispatch;
  Driver->MajorFunction[18] = (void *)Dispatch;
  Driver->DriverUnload = (void *)Unload;
  return 0;
}
