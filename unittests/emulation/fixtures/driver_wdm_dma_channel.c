//===- driver_wdm_dma_channel.c - Genuine WDK DMA channel fixture -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original bus-master channel driver using genuine WDK function pointers.
/// Real callback actions, aggregate flushes and explicit bus transactions.
//===----------------------------------------------------------------------===//
#include <intrin.h>
#include <ntddk.h>

#define OFFSET(Type, Field, Value)                                             \
  _Static_assert(__builtin_offsetof(Type, Field) == Value, #Type "." #Field)
_Static_assert(sizeof(DMA_ADAPTER) == 16 && _Alignof(DMA_ADAPTER) == 8 &&
                   sizeof(SCATTER_GATHER_LIST) == 16 &&
                   sizeof(SCATTER_GATHER_ELEMENT) == 24 &&
                   sizeof(PHYSICAL_ADDRESS) == 8 && sizeof(BOOLEAN) == 1,
               "genuine x64 DMA ABI");
OFFSET(DMA_ADAPTER, Version, 0);
OFFSET(DMA_ADAPTER, Size, 2);
OFFSET(DMA_ADAPTER, DmaOperations, 8);
OFFSET(DEVICE_DESCRIPTION, Version, 0);
OFFSET(DEVICE_DESCRIPTION, Master, 4);
OFFSET(DEVICE_DESCRIPTION, ScatterGather, 5);
OFFSET(DEVICE_DESCRIPTION, Dma32BitAddresses, 8);
OFFSET(DEVICE_DESCRIPTION, Dma64BitAddresses, 11);
OFFSET(DEVICE_DESCRIPTION, MaximumLength, 32);
OFFSET(DEVICE_DESCRIPTION, DmaPort, 36);
OFFSET(DMA_OPERATIONS, PutDmaAdapter, 8);
OFFSET(DMA_OPERATIONS, AllocateCommonBuffer, 16);
OFFSET(DMA_OPERATIONS, FreeCommonBuffer, 24);
OFFSET(DMA_OPERATIONS, GetDmaAlignment, 72);
OFFSET(DMA_OPERATIONS, GetScatterGatherList, 88);
OFFSET(DMA_OPERATIONS, PutScatterGatherList, 96);
OFFSET(SCATTER_GATHER_LIST, Elements, 16);
OFFSET(SCATTER_GATHER_ELEMENT, Address, 0);
OFFSET(SCATTER_GATHER_ELEMENT, Length, 8);
OFFSET(SCATTER_GATHER_ELEMENT, Reserved, 16);
_Static_assert(DEVICE_DESCRIPTION_VERSION1 == 1 &&
                   sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == 20 &&
                   _Alignof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == 4,
               "genuine description version and packed resources");

#define DMA_BYTES 32u
#define DMA_POOL_TAG 0x6843444Eu
#define DMA_IOCTL 0x222000u
OFFSET(DEVICE_OBJECT, CurrentIrp, 32);
OFFSET(DMA_OPERATIONS, AllocateAdapterChannel, 32);
OFFSET(DMA_OPERATIONS, FlushAdapterBuffers, 40);
OFFSET(DMA_OPERATIONS, FreeAdapterChannel, 48);
OFFSET(DMA_OPERATIONS, FreeMapRegisters, 56);
OFFSET(DMA_OPERATIONS, MapTransfer, 64);
_Static_assert(sizeof(IO_ALLOCATION_ACTION) == 4 && KeepObject == 1 &&
                   DeallocateObject == 2 && DeallocateObjectKeepRegisters == 3,
               "genuine 32-bit channel callback return");

typedef struct {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT Lower;
  PDMA_ADAPTER Adapter;
  ULONG MapRegisters;
  ULONG Reserved;
  PVOID Registers;
  PUCHAR Pool;
  PUCHAR Buffer;
  PMDL Mdl;
  PUCHAR Common;
  PHYSICAL_ADDRESS CommonLogical;
  PSCATTER_GATHER_LIST List;
  PHYSICAL_ADDRESS Logical[2];
  ULONG Lengths[2];
  KEVENT Event;
  KDPC Dpc;
  PKINTERRUPT Interrupt;
  PIRP Pending;
  ULONG Unit;
  ULONG Starts;
  ULONG Callbacks;
  ULONG ISRs;
  ULONG Operation;
  ULONG Fragments;
  KIRQL IRQL;
  BOOLEAN Ready;
  BOOLEAN Started;
  BOOLEAN Surprise;
  BOOLEAN RemovePending;
  BOOLEAN WriteToDevice;
} CHANNEL_EXTENSION;

static UCHAR Mode;
static ULONG Units;
static ULONG Live;

static BOOLEAN Check(BOOLEAN Condition, ULONG Code) {
  if (!Condition)
    DbgPrint("WDM channel: failure %lu\n", Code);
  return Condition;
}

static void CheckIRQL(KIRQL Expected, ULONG Code) {
  ULONG64 Raw;
  __asm__ volatile("movq %%cr8, %0" : "=r"(Raw));
  Check(KeGetCurrentIrql() == Expected && (KIRQL)Raw == Expected, Code);
}

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static void CompleteTransfer(CHANNEL_EXTENSION *Extension) {
  PIRP Irp = Extension->Pending;
  PULONG Values = Irp->AssociatedIrp.SystemBuffer;
  Values[0] = Extension->Unit;
  Values[1] = Extension->Callbacks;
  Values[2] = Extension->ISRs;
  Values[3] = Extension->Operation;
  for (ULONG I = 0; I < DMA_BYTES; ++I)
    ((PUCHAR)Values)[16 + I] = Extension->Buffer[I];
  Extension->Pending = NULL;
  Extension->Self->CurrentIrp = NULL;
  Complete(Irp, STATUS_SUCCESS, 16 + DMA_BYTES);
  DbgPrint("WDM channel: completed unit=%lu\n", Extension->Unit);
}

static void MapOperation(CHANNEL_EXTENSION *Extension) {
  ULONG Done = 0;
  Extension->Fragments = 0;
  while (Done < DMA_BYTES) {
    ULONG Length = DMA_BYTES - Done;
    PHYSICAL_ADDRESS Address = Extension->Adapter->DmaOperations->MapTransfer(
        Extension->Adapter, Extension->Mdl, Extension->Registers,
        (PUCHAR)MmGetMdlVirtualAddress(Extension->Mdl) + Done, &Length,
        Extension->WriteToDevice);
    Check(Length != 0 && Length <= DMA_BYTES - Done &&
              Extension->Fragments < 2,
          10);
    ULONG Fragment = Extension->Fragments++;
    Extension->Logical[Fragment] = Address;
    Extension->Lengths[Fragment] = Length;
    Check(Mode == 'J' || Length == ((Mode == 'N' || Mode == 'O') ? DMA_BYTES
                         : (Fragment == 0 ? 8u : 24u)),
          11);
    Done += Length;
  }
  ++Extension->Operation;
  Extension->Ready = TRUE;
  DbgPrint("WDM channel: mapped unit=%lu operation=%lu fragments=%lu\n",
           Extension->Unit, Extension->Operation, Extension->Fragments);
}

IO_ALLOCATION_ACTION ChannelControl(PDEVICE_OBJECT Device, PIRP Irp,
                                    PVOID Registers, PVOID Context) {
  CHANNEL_EXTENSION *Extension = Context;
  CheckIRQL(DISPATCH_LEVEL, 20);
  Check(Device == Extension->Self && Irp == Extension->Pending &&
            Registers != NULL && Extension->Registers == NULL,
        21);
  if (Mode == 'Q')
    Check(Device->CurrentIrp == NULL, 22);
  else
    Check(Device->CurrentIrp == Irp, 23);
  Extension->Registers = Registers;
  ++Extension->Callbacks;
  DbgPrint("WDM channel: callback unit=%lu snapshot=%u\n", Extension->Unit,
           Irp == Extension->Pending);
  if (Mode == 'I') {
    Extension->Registers = NULL;
    CompleteTransfer(Extension);
    DbgPrint("WDM channel: callback completed before action\n");
    return DeallocateObject;
  }
  if (Mode == 'F')
    Extension->Adapter->DmaOperations->FreeMapRegisters(
        Extension->Adapter, Registers, Extension->Reserved);
  if (Mode == 'E')
    Extension->Adapter->DmaOperations->AllocateAdapterChannel(
        Extension->Adapter, Device, Extension->Reserved, ChannelControl,
        Extension);
  if (Mode == 'K')
    return KeepObject;
  if (Mode == 'A')
    return (IO_ALLOCATION_ACTION)4;
  MapOperation(Extension);
  if (Mode == 'M')
    IoFreeMdl(Extension->Mdl);
  if (Mode == 'B')
    ExFreePoolWithTag(Extension->Pool, DMA_POOL_TAG);
  if (Mode == 'J')
    Complete(Irp, STATUS_SUCCESS, 0);
  if (Mode == 'V')
    Check(*(volatile UCHAR *)Extension->Buffer == 0, 24);
  return DeallocateObjectKeepRegisters;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&ChannelControl),
                                            PDRIVER_CONTROL),
               "genuine four-argument action callback");

// Original assembly leaves nonzero high RAX bits after a genuine i32 return.
__attribute__((naked)) IO_ALLOCATION_ACTION
ChannelControlHighBits(PDEVICE_OBJECT Device, PIRP Irp, PVOID Registers,
                       PVOID Context) {
  __asm__ volatile("subq $40, %rsp\n\t"
                   "callq ChannelControl\n\t"
                   "movabsq $0xaabbccdd00000000, %rcx\n\t"
                   "orq %rcx, %rax\n\t"
                   "addq $40, %rsp\n\t"
                   "retq");
}

static void ListControl(PDEVICE_OBJECT Device, PIRP Irp,
                        PSCATTER_GATHER_LIST List, PVOID Context) {
  CHANNEL_EXTENSION *Extension = Context;
  CheckIRQL(DISPATCH_LEVEL, 30);
  Check(Device == Extension->Self && Irp == NULL && List != NULL &&
            List->NumberOfElements == 1 && List->Elements[0].Length == 16,
        31);
  Extension->List = List;
  DbgPrint("WDM channel: SG callback unit=%lu\n", Extension->Unit);
}

static void BeginChannel(CHANNEL_EXTENSION *Extension) {
  KeFlushIoBuffers(Extension->Mdl, !Extension->WriteToDevice, TRUE);
  if (Mode == 'Q') {
    NTSTATUS Status = Extension->Adapter->DmaOperations->GetScatterGatherList(
        Extension->Adapter, Extension->Self, Extension->Mdl,
        (PUCHAR)MmGetMdlVirtualAddress(Extension->Mdl) + 8, 16, ListControl,
        Extension, TRUE);
    Check(Status == STATUS_SUCCESS && Extension->List != NULL, 32);
  }
  Extension->Self->CurrentIrp = Extension->Pending;
  NTSTATUS Status = Extension->Adapter->DmaOperations->AllocateAdapterChannel(
      Extension->Adapter, Extension->Self,
      Mode == 'X' ? Extension->MapRegisters + 1 : Extension->Reserved,
      Mode == 'H' ? ChannelControlHighBits : ChannelControl, Extension);
  if (Mode == 'X') {
    Check(Status == STATUS_INSUFFICIENT_RESOURCES &&
              Extension->Callbacks == 0 && Extension->Registers == NULL,
          33);
    CompleteTransfer(Extension);
    return;
  }
  Check(Status == STATUS_SUCCESS, 34);
  if (Mode == 'Q') {
    Check(Extension->Callbacks == 0 && Extension->Registers == NULL, 35);
    Extension->Self->CurrentIrp = NULL;
    DbgPrint("WDM channel: queued with original IRP; field cleared\n");
    Extension->Adapter->DmaOperations->PutScatterGatherList(
        Extension->Adapter, Extension->List, TRUE);
    Extension->List = NULL;
  }
  DbgPrint("WDM channel: allocate returned unit=%lu\n", Extension->Unit);
}

static void Dpc(PKDPC Object, PVOID Context, PVOID Argument1, PVOID Argument2) {
  CHANNEL_EXTENSION *Extension = Context;
  CheckIRQL(DISPATCH_LEVEL, 40);
  Check(Object == &Extension->Dpc && Argument2 == Extension, 41);
  if ((ULONG_PTR)Argument1 == 1) {
    BeginChannel(Extension);
    return;
  }
  Check((ULONG_PTR)Argument1 == 2 && Extension->Ready, 42);
  Check(Extension->Adapter->DmaOperations->FlushAdapterBuffers(
            Extension->Adapter, Extension->Mdl, Extension->Registers,
            MmGetMdlVirtualAddress(Extension->Mdl),
            Mode == 'P' ? DMA_BYTES - 1 : DMA_BYTES,
            Mode == 'D' ? !Extension->WriteToDevice : Extension->WriteToDevice),
        43);
  Extension->Ready = FALSE;
  DbgPrint("WDM channel: flushed unit=%lu operation=%lu\n", Extension->Unit,
           Extension->Operation);
  if (Mode == 'U' && Extension->Operation == 1) {
    for (ULONG I = 0; I < DMA_BYTES; ++I)
      Check(Extension->Buffer[I] == (UCHAR)(0xa0 + I), 44);
    KeFlushIoBuffers(Extension->Mdl, TRUE, TRUE);
    MapOperation(Extension);
    return;
  }
  Extension->Adapter->DmaOperations->FreeMapRegisters(
      Extension->Adapter, Extension->Registers,
      Mode == 'C' ? Extension->Reserved - 1 : Extension->Reserved);
  Extension->Registers = NULL;
  DbgPrint("WDM channel: freed registers unit=%lu\n", Extension->Unit);
  CompleteTransfer(Extension);
}

static BOOLEAN Isr(PKINTERRUPT Interrupt, PVOID Context) {
  CHANNEL_EXTENSION *Extension = Context;
  CheckIRQL(Extension->IRQL, 50);
  Check(Interrupt == Extension->Interrupt && Extension->Started, 51);
  ++Extension->ISRs;
  if (!Extension->Pending || !Extension->Ready)
    return FALSE;
  Check(KeInsertQueueDpc(&Extension->Dpc, (PVOID)2, Extension), 52);
  DbgPrint("WDM channel: ISR unit=%lu\n", Extension->Unit);
  return TRUE;
}

static NTSTATUS Completion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  CHANNEL_EXTENSION *Extension = Context;
  Check(Device == Extension->Self && Irp->IoStatus.Information == 0, 50);
  CheckIRQL(PASSIVE_LEVEL, 51);
  KeSetEvent(&Extension->Event, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS ForwardWait(CHANNEL_EXTENSION *Extension, PIRP Irp) {
  KeClearEvent(&Extension->Event);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, Completion, Extension, TRUE, TRUE, TRUE);
  NTSTATUS Status = IoCallDriver(Extension->Lower, Irp);
  if (Status == STATUS_PENDING)
    Check(KeWaitForSingleObject(&Extension->Event, Executive, KernelMode, FALSE,
                                NULL) == STATUS_SUCCESS,
          52);
  return Irp->IoStatus.Status;
}

static NTSTATUS AcquireAdapter(CHANNEL_EXTENSION *Extension) {
  DEVICE_DESCRIPTION Description = {0};
  Description.Version = DEVICE_DESCRIPTION_VERSION1;
  Description.Master = TRUE;
  Description.ScatterGather = Mode != 'N' && Mode != 'O';
  Description.Dma32BitAddresses = TRUE;
  Description.Dma64BitAddresses = TRUE;
  Description.InterfaceType = Internal;
  Description.MaximumLength = PAGE_SIZE * 2;
  Extension->Reserved = Mode == 'O' ? 1u : 2u;
  Extension->WriteToDevice = Mode == 'R';
  Extension->Adapter = IoGetDmaAdapter(Extension->PDO, &Description,
                                      &Extension->MapRegisters);
  if (!Extension->Adapter)
    return STATUS_INSUFFICIENT_RESOURCES;
  Check(Extension->Adapter->Version == 1 && Extension->MapRegisters >= 2, 60);
  DbgPrint("WDM channel: adapter unit=%lu\n", Extension->Unit);
  return STATUS_SUCCESS;
}

static void Stop(CHANNEL_EXTENSION *Extension) {
  CheckIRQL(PASSIVE_LEVEL, 70);
  Check(Extension->Pending == NULL && Extension->Registers == NULL &&
            Extension->List == NULL,
        71);
  if (Extension->Interrupt) {
    IoDisconnectInterrupt(Extension->Interrupt);
    Extension->Interrupt = NULL;
  }
  if (Extension->Common) {
    Extension->Adapter->DmaOperations->FreeCommonBuffer(
        Extension->Adapter, 16, Extension->CommonLogical, Extension->Common,
        TRUE);
    Extension->Common = NULL;
  }
  if (Extension->Mdl) {
    IoFreeMdl(Extension->Mdl);
    Extension->Mdl = NULL;
  }
  if (Extension->Pool) {
    ExFreePoolWithTag(Extension->Pool, DMA_POOL_TAG);
    Extension->Pool = NULL;
  }
  if (Extension->Adapter) {
    PDMA_ADAPTER Adapter = Extension->Adapter;
    Extension->Adapter = NULL;
    Adapter->DmaOperations->PutDmaAdapter(Adapter);
  }
  Extension->Started = FALSE;
  DbgPrint("WDM channel: stopped unit=%lu\n", Extension->Unit);
}

static NTSTATUS Start(CHANNEL_EXTENSION *Extension, PCM_RESOURCE_LIST Raw,
                      PCM_RESOURCE_LIST Translated) {
  Check(Raw != NULL && Translated != NULL && Raw != Translated &&
            Raw->Count == 1 && Translated->Count == 1,
        80);
  PCM_PARTIAL_RESOURCE_LIST Resources = &Translated->List[0].PartialResourceList;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR Interrupt = NULL;
  for (ULONG I = 0; I < Resources->Count; ++I)
    if (Resources->PartialDescriptors[I].Type == CmResourceTypeInterrupt)
      Interrupt = &Resources->PartialDescriptors[I];
  if (!Check(Interrupt != NULL &&
                 Interrupt->ShareDisposition == CmResourceShareDeviceExclusive &&
                 Interrupt->Flags == CM_RESOURCE_INTERRUPT_LATCHED &&
                 Interrupt->u.Interrupt.Affinity == 1,
             81))
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  NTSTATUS Status = STATUS_SUCCESS;
  if (!Extension->Adapter)
    Status = AcquireAdapter(Extension);
  if (!NT_SUCCESS(Status))
    return Status;
  Extension->Pool = ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE * 2,
                                     DMA_POOL_TAG);
  if (!Extension->Pool)
    return STATUS_INSUFFICIENT_RESOURCES;
  Check(((ULONG_PTR)Extension->Pool & (PAGE_SIZE - 1)) == 0, 82);
  Extension->Buffer = Extension->Pool + PAGE_SIZE - 8;
  for (ULONG I = 0; I < DMA_BYTES; ++I)
    Extension->Buffer[I] = (UCHAR)(0x20 + Extension->Unit + I);
  Extension->Mdl = IoAllocateMdl(Extension->Buffer, DMA_BYTES, FALSE, FALSE, NULL);
  if (!Extension->Mdl)
    return STATUS_INSUFFICIENT_RESOURCES;
  MmBuildMdlForNonPagedPool(Extension->Mdl);
  if (Mode == 'Q') {
    Extension->Common = Extension->Adapter->DmaOperations->AllocateCommonBuffer(
        Extension->Adapter, 16, &Extension->CommonLogical, TRUE);
    if (!Extension->Common)
      return STATUS_INSUFFICIENT_RESOURCES;
    for (ULONG I = 0; I < 16; ++I)
      Extension->Common[I] = (UCHAR)(0x80 + I);
  }
  Extension->IRQL = (KIRQL)Interrupt->u.Interrupt.Level;
  Status = IoConnectInterrupt(
      &Extension->Interrupt, Isr, Extension, NULL, Interrupt->u.Interrupt.Vector,
      Extension->IRQL, Extension->IRQL, Latched, FALSE, 1, FALSE);
  if (!NT_SUCCESS(Status))
    return Status;
  ++Extension->Starts;
  Extension->Started = TRUE;
  DbgPrint("WDM channel: started unit=%lu\n", Extension->Unit);
  return STATUS_SUCCESS;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp) {
  CHANNEL_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  UCHAR Minor = Stack->MinorFunction;
  CheckIRQL(PASSIVE_LEVEL, 90);
  Check(Stack->FileObject == NULL, 91);
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  if (Minor == IRP_MN_REMOVE_DEVICE) {
    PDEVICE_OBJECT Lower = Extension->Lower;
    Stop(Extension);
    IoSkipCurrentIrpStackLocation(Irp);
    NTSTATUS Status = IoCallDriver(Lower, Irp);
    IoDetachDevice(Lower);
    --Live;
    IoDeleteDevice(Device);
    return Status;
  }
  if (Minor == IRP_MN_STOP_DEVICE || Minor == IRP_MN_SURPRISE_REMOVAL) {
    if (Minor == IRP_MN_SURPRISE_REMOVAL)
      Extension->Surprise = TRUE;
    Stop(Extension);
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Extension->Lower, Irp);
  }
  NTSTATUS Status = ForwardWait(Extension, Irp);
  if (Minor == IRP_MN_START_DEVICE) {
    if (NT_SUCCESS(Status))
      Status = Start(Extension, Stack->Parameters.StartDevice.AllocatedResources,
                     Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
    if (!NT_SUCCESS(Status))
      Stop(Extension);
  } else if (Minor == IRP_MN_QUERY_REMOVE_DEVICE) {
    Extension->RemovePending = NT_SUCCESS(Status);
  } else if (Minor == IRP_MN_CANCEL_REMOVE_DEVICE) {
    Extension->RemovePending = FALSE;
  }
  return Complete(Irp, Status, 0);
}

static NTSTATUS DispatchPower(PDEVICE_OBJECT Device, PIRP Irp) {
  CHANNEL_EXTENSION *Extension = Device->DeviceExtension;
  PoStartNextPowerIrp(Irp);
  IoSkipCurrentIrpStackLocation(Irp);
  return PoCallDriver(Extension->Lower, Irp);
}

static NTSTATUS DispatchFile(PDEVICE_OBJECT Device, PIRP Irp) {
  CHANNEL_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  CheckIRQL(PASSIVE_LEVEL, 100);
  Check(Stack->FileObject && Stack->FileObject->DeviceObject == Extension->PDO,
        101);
  if (Stack->MajorFunction == IRP_MJ_CLEANUP ||
      Stack->MajorFunction == IRP_MJ_CLOSE)
    return Complete(Irp, STATUS_SUCCESS, 0);
  if (Extension->Surprise)
    return Complete(Irp, STATUS_NO_SUCH_DEVICE, 0);
  if (!Extension->Started)
    return Complete(Irp, STATUS_DEVICE_NOT_READY, 0);
  if (Stack->MajorFunction == IRP_MJ_CREATE)
    return Complete(Irp, Extension->RemovePending ? STATUS_DELETE_PENDING
                                                 : STATUS_SUCCESS,
                    0);
  if (Stack->Parameters.DeviceIoControl.IoControlCode !=
          (Mode == 'J' ? DMA_IOCTL + METHOD_OUT_DIRECT : DMA_IOCTL) ||
      Stack->Parameters.DeviceIoControl.OutputBufferLength < 16 + DMA_BYTES)
    return Complete(Irp, STATUS_INVALID_PARAMETER, 0);
  Check(Extension->Pending == NULL, 102);
  if (Mode == 'J') {
    IoFreeMdl(Extension->Mdl);
    Extension->Mdl = Irp->MdlAddress;
  }
  Extension->Pending = Irp;
  IoMarkIrpPending(Irp);
  Check(KeInsertQueueDpc(&Extension->Dpc, (PVOID)1, Extension), 103);
  DbgPrint("WDM channel: pending unit=%lu\n", Extension->Unit);
  return STATUS_PENDING;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT PDO) {
  PDEVICE_OBJECT Device = NULL;
  NTSTATUS Status = IoCreateDevice(Driver, sizeof(CHANNEL_EXTENSION), NULL,
                                   FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                   FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  CHANNEL_EXTENSION *Extension = Device->DeviceExtension;
  Extension->Self = Device;
  Extension->PDO = PDO;
  Extension->Unit = ++Units;
  KeInitializeEvent(&Extension->Event, NotificationEvent, FALSE);
  KeInitializeDpc(&Extension->Dpc, Dpc, Extension);
  Status = AcquireAdapter(Extension);
  if (!NT_SUCCESS(Status)) {
    IoDeleteDevice(Device);
    return Status;
  }
  Extension->Lower = IoAttachDeviceToDeviceStack(Device, PDO);
  if (!Extension->Lower) {
    Extension->Adapter->DmaOperations->PutDmaAdapter(Extension->Adapter);
    IoDeleteDevice(Device);
    return STATUS_NO_SUCH_DEVICE;
  }
  ++Live;
  Device->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}

static void Unload(PDRIVER_OBJECT Driver) {
  Check(Live == 0 && Driver->DeviceObject == NULL, 110);
  DbgPrint("WDM channel: unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path) {
  Mode = 'S';
  if (Path->Length >= sizeof(WCHAR)) {
    WCHAR Last = Path->Buffer[Path->Length / sizeof(WCHAR) - 1];
    if (Last >= 'A' && Last <= 'Z')
      Mode = (UCHAR)Last;
  }
  Driver->DriverExtension->AddDevice = AddDevice;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
  Driver->MajorFunction[IRP_MJ_POWER] = DispatchPower;
  Driver->MajorFunction[IRP_MJ_CREATE] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLOSE] = DispatchFile;
  DbgPrint("WDM channel: entry mode=%c\n", Mode);
  return STATUS_SUCCESS;
}
