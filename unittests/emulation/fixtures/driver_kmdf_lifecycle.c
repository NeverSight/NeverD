//===- driver_kmdf_lifecycle.c - Genuine WDK framework fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original KMDF code compiled against unmodified WDK 1.33 headers and linked
/// through the genuine wdfdriverentry.lib FxDriverEntry entry point. Microsoft
/// headers and libraries remain external, optional validation dependencies.
///
//===----------------------------------------------------------------------===//

#include <ntddk.h>
#include <wdf.h>

#define ABI_OFFSET(Type, Member, Offset)                                       \
  _Static_assert(__builtin_offsetof(Type, Member) == Offset,                   \
                 #Type "." #Member " x64 ABI")

_Static_assert(sizeof(void *) == 8, "x64 fixture");
_Static_assert(sizeof(WDF_DRIVER_GLOBALS) == 56, "driver globals ABI");
ABI_OFFSET(WDF_DRIVER_GLOBALS, Driver, 0);
ABI_OFFSET(WDF_DRIVER_GLOBALS, DriverFlags, 8);
ABI_OFFSET(WDF_DRIVER_GLOBALS, DriverTag, 12);
ABI_OFFSET(WDF_DRIVER_GLOBALS, DriverName, 16);
ABI_OFFSET(WDF_DRIVER_GLOBALS, DisplaceDriverUnload, 48);
_Static_assert(sizeof(WDF_DRIVER_CONFIG) == 32, "driver config ABI");
ABI_OFFSET(WDF_DRIVER_CONFIG, EvtDriverDeviceAdd, 8);
ABI_OFFSET(WDF_DRIVER_CONFIG, EvtDriverUnload, 16);
ABI_OFFSET(WDF_DRIVER_CONFIG, DriverInitFlags, 24);
ABI_OFFSET(WDF_DRIVER_CONFIG, DriverPoolTag, 28);
_Static_assert(sizeof(WDF_OBJECT_ATTRIBUTES) == 56, "object attributes ABI");
ABI_OFFSET(WDF_OBJECT_ATTRIBUTES, EvtCleanupCallback, 8);
ABI_OFFSET(WDF_OBJECT_ATTRIBUTES, EvtDestroyCallback, 16);
ABI_OFFSET(WDF_OBJECT_ATTRIBUTES, ExecutionLevel, 24);
ABI_OFFSET(WDF_OBJECT_ATTRIBUTES, SynchronizationScope, 28);
ABI_OFFSET(WDF_OBJECT_ATTRIBUTES, ParentObject, 32);
ABI_OFFSET(WDF_OBJECT_ATTRIBUTES, ContextSizeOverride, 40);
ABI_OFFSET(WDF_OBJECT_ATTRIBUTES, ContextTypeInfo, 48);
_Static_assert(sizeof(WDF_OBJECT_CONTEXT_TYPE_INFO) == 40, "context type ABI");
ABI_OFFSET(WDF_OBJECT_CONTEXT_TYPE_INFO, ContextName, 8);
ABI_OFFSET(WDF_OBJECT_CONTEXT_TYPE_INFO, ContextSize, 16);
ABI_OFFSET(WDF_OBJECT_CONTEXT_TYPE_INFO, UniqueType, 24);
ABI_OFFSET(WDF_OBJECT_CONTEXT_TYPE_INFO, EvtDriverGetUniqueContextType, 32);
_Static_assert(WdfDriverInitNonPnpDriver == 1, "non-PnP flag ABI");
_Static_assert(WdfExecutionLevelInheritFromParent == 1,
               "execution default ABI");
_Static_assert(WdfSynchronizationScopeInheritFromParent == 1,
               "synchronization default ABI");
_Static_assert(WdfDriverCreateTableIndex == 116, "driver create table slot");
_Static_assert(WdfObjectGetTypedContextWorkerTableIndex == 202,
               "typed context table slot");
_Static_assert(WdfObjectAllocateContextTableIndex == 203,
               "allocate context table slot");
_Static_assert(WdfObjectContextGetObjectTableIndex == 204,
               "context object table slot");
_Static_assert(WdfObjectReferenceActualTableIndex == 205,
               "reference table slot");
_Static_assert(WdfObjectDereferenceActualTableIndex == 206,
               "dereference table slot");
_Static_assert(WdfObjectCreateTableIndex == 207, "object create table slot");
_Static_assert(WdfObjectDeleteTableIndex == 208, "object delete table slot");

typedef struct {
  ULONG Cookie;
  ULONG Unloads;
  ULONG Cleanups;
} LIFECYCLE_DRIVER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LIFECYCLE_DRIVER_CONTEXT, DriverContext);

typedef struct {
  ULONG Cookie;
  ULONG Cleanups;
} LIFECYCLE_CHILD_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LIFECYCLE_CHILD_CONTEXT, ChildContext);

typedef struct {
  ULONG Cookie;
  ULONG Reserved;
} LIFECYCLE_EXTRA_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LIFECYCLE_EXTRA_CONTEXT, ExtraContext);

static WDFDRIVER CreatedDriver;
static ULONG ChildCleanups;
static ULONG ChildDestroys;
static ULONG Failures;
static BOOLEAN ExerciseNestedWait;
static BOOLEAN EntryWillFail;

typedef struct {
  KEVENT Event;
  PIO_WORKITEM Item;
} NESTED_WAIT_CONTEXT;

static void Check(BOOLEAN Condition, ULONG Code) {
  if (!Condition) {
    ++Failures;
    DbgPrint("KMDF lifecycle: failure %lu\n", Code);
  }
}

static void NestedCleanup(WDFOBJECT Object) {
  LARGE_INTEGER Interval;
  (void)Object;
  Interval.QuadPart = -7;
  Check(KeDelayExecutionThread(KernelMode, FALSE, &Interval) == STATUS_SUCCESS,
        70);
  Check(WdfGetDriver() == CreatedDriver, 71);
  DbgPrint("KMDF nested: delayed cleanup resumed\n");
}

static void SignalWorker(PDEVICE_OBJECT Device, PVOID Parameter) {
  NESTED_WAIT_CONTEXT *Wait = Parameter;
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDFOBJECT Object = NULL;
  (void)Device;
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.EvtCleanupCallback = NestedCleanup;
  Check(NT_SUCCESS(WdfObjectCreate(&Attributes, &Object)), 72);
  if (Object != NULL)
    WdfObjectDelete(Object);
  DbgPrint("KMDF nested: worker resumed\n");
  KeSetEvent(&Wait->Event, IO_NO_INCREMENT, FALSE);
  IoFreeWorkItem(Wait->Item);
}

static void ChildCleanup(WDFOBJECT Object) {
  LIFECYCLE_CHILD_CONTEXT *Context = ChildContext(Object);
  Check(Context != NULL && Context->Cookie == 0x43484c44, 10);
  Check(WdfGetDriver() == CreatedDriver, 11);
  Check(WdfObjectContextGetObject(Context) == Object, 12);
  if (Context != NULL)
    ++Context->Cleanups;
  ++ChildCleanups;
  DbgPrint("KMDF lifecycle: child cleanup\n");
  if (ExerciseNestedWait) {
    NESTED_WAIT_CONTEXT Wait;
    PDEVICE_OBJECT Device = NULL;
    UNICODE_STRING Name;
    RtlInitUnicodeString(&Name, L"\\Device\\NeverDKmdfWait");
    Check(NT_SUCCESS(IoCreateDevice(WdfDriverWdmGetDriverObject(CreatedDriver),
                                    0, &Name, FILE_DEVICE_UNKNOWN, 0, FALSE,
                                    &Device)),
          75);
    if (Device == NULL)
      return;
    KeInitializeEvent(&Wait.Event, NotificationEvent, FALSE);
    Wait.Item = IoAllocateWorkItem(Device);
    Check(Wait.Item != NULL, 76);
    if (Wait.Item == NULL) {
      IoDeleteDevice(Device);
      return;
    }
    IoQueueWorkItem(Wait.Item, SignalWorker, DelayedWorkQueue, &Wait);
    Check(KeWaitForSingleObject(&Wait.Event, Executive, KernelMode, FALSE,
                                NULL) == STATUS_SUCCESS,
          73);
    Check(ChildContext(Object) == Context && Context->Cookie == 0x43484c44, 74);
    DbgPrint("KMDF nested: parent cleanup resumed\n");
    IoDeleteDevice(Device);
  }
}

static void ChildDestroy(WDFOBJECT Object) {
  LIFECYCLE_CHILD_CONTEXT *Context = ChildContext(Object);
  LIFECYCLE_EXTRA_CONTEXT *Extra = ExtraContext(Object);
  Check(Context != NULL && Context->Cookie == 0x43484c44 &&
            Context->Cleanups == 1,
        20);
  Check(Extra != NULL && Extra->Cookie == 0x45585452, 21);
  Check(ChildCleanups == 1 && ChildDestroys == 0, 22);
  ++ChildDestroys;
  DbgPrint("KMDF lifecycle: child destroy\n");
}

static void DriverUnload(WDFDRIVER Driver) {
  LIFECYCLE_DRIVER_CONTEXT *Context = DriverContext(Driver);
  Check(Driver == CreatedDriver && WdfGetDriver() == Driver, 30);
  Check(Context != NULL && Context->Cookie == 0x44525652, 31);
  Check(ChildCleanups == 1 && ChildDestroys == 1, 32);
  Check(!EntryWillFail, 33);
  if (Context != NULL)
    ++Context->Unloads;
  DbgPrint("KMDF lifecycle: driver unload\n");
}

static void DriverCleanup(WDFOBJECT Object) {
  LIFECYCLE_DRIVER_CONTEXT *Context = DriverContext(Object);
  Check(Object == (WDFOBJECT)CreatedDriver && WdfGetDriver() == CreatedDriver,
        40);
  Check(Context != NULL && Context->Cookie == 0x44525652 &&
            Context->Unloads == (EntryWillFail ? 0u : 1u) &&
            Context->Cleanups == 0,
        41);
  if (Context != NULL)
    ++Context->Cleanups;
  DbgPrint("KMDF lifecycle: driver cleanup\n");
}

static void DriverDestroy(WDFOBJECT Object) {
  LIFECYCLE_DRIVER_CONTEXT *Context = DriverContext(Object);
  Check(Object == (WDFOBJECT)CreatedDriver, 50);
  Check(Context != NULL && Context->Cookie == 0x44525652 &&
            Context->Unloads == (EntryWillFail ? 0u : 1u) &&
            Context->Cleanups == 1,
        51);
  Check(WdfObjectContextGetObject(Context) == Object, 52);
  DbgPrint("KMDF lifecycle: driver destroy\n");
}

static NTSTATUS ForbiddenDeviceAdd(WDFDRIVER Driver,
                                   PWDFDEVICE_INIT DeviceInit) {
  (void)Driver;
  (void)DeviceInit;
  DbgPrint("KMDF lifecycle: forbidden non-PnP add-device callback\n");
  return STATUS_UNSUCCESSFUL;
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                     PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG Config;
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDF_OBJECT_ATTRIBUTES ExtraAttributes;
  LIFECYCLE_DRIVER_CONTEXT *Context;
  LIFECYCLE_CHILD_CONTEXT *ChildData;
  LIFECYCLE_EXTRA_CONTEXT *Extra = NULL;
  PVOID Duplicate = NULL;
  WDFOBJECT Child = NULL;
  WDFDRIVER Driver = NULL;
  NTSTATUS Status;
  WCHAR Marker =
      RegistryPath->Length >= sizeof(WCHAR)
          ? RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1]
          : L'0';
  ExerciseNestedWait = Marker == L'W';
  EntryWillFail = Marker == L'E';

  WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
  Config.DriverInitFlags = WdfDriverInitNonPnpDriver;
  Config.EvtDriverUnload = DriverUnload;
  Config.DriverPoolTag = 0x4e444b4d;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes,
                                          LIFECYCLE_DRIVER_CONTEXT);
  Attributes.EvtCleanupCallback = DriverCleanup;
  Attributes.EvtDestroyCallback = DriverDestroy;
  if (Marker == L'S')
    Config.Size = 0;
  if (Marker == L'A')
    Config.EvtDriverDeviceAdd = ForbiddenDeviceAdd;
  Status = WdfDriverCreate(DriverObject, RegistryPath, &Attributes, &Config,
                           &Driver);
  if (!NT_SUCCESS(Status))
    return Status;
  CreatedDriver = Driver;
  Check(Driver != NULL && WdfGetDriver() == Driver, 1);
  Context = DriverContext(Driver);
  Check(Context != NULL && Context->Cookie == 0 && Context->Unloads == 0 &&
            Context->Cleanups == 0,
        2);
  if (Context == NULL)
    return STATUS_UNSUCCESSFUL;
  Context->Cookie = 0x44525652;
  Check(WdfObjectContextGetObject(Context) == (WDFOBJECT)Driver, 3);
  DbgPrint("KMDF lifecycle: driver created\n");

  if (Marker == L'U') {
    GUID Query = {0};
    (void)WdfObjectQuery((WDFOBJECT)Driver, &Query, 0, NULL);
    DbgPrint("KMDF lifecycle: forbidden unmodeled query returned\n");
    return STATUS_UNSUCCESSFUL;
  }

  if (Marker == L'D') {
    WDFDRIVER Other = NULL;
    Status = WdfDriverCreate(DriverObject, RegistryPath, NULL, &Config, &Other);
    Check(Status == STATUS_DRIVER_INTERNAL_ERROR && WdfGetDriver() == Driver,
          4);
    DbgPrint("KMDF lifecycle: duplicate driver checked\n");
  }

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, LIFECYCLE_CHILD_CONTEXT);
  Attributes.ParentObject = (WDFOBJECT)Driver;
  Attributes.EvtCleanupCallback = ChildCleanup;
  Attributes.EvtDestroyCallback = ChildDestroy;
  Status = WdfObjectCreate(&Attributes, &Child);
  if (!NT_SUCCESS(Status))
    return Status;
  ChildData = ChildContext(Child);
  Check(ChildData != NULL && ChildData->Cookie == 0 && ChildData->Cleanups == 0,
        5);
  Check(DriverContext(Child) == NULL, 6);
  if (ChildData == NULL)
    return STATUS_UNSUCCESSFUL;
  ChildData->Cookie = 0x43484c44;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&ExtraAttributes,
                                          LIFECYCLE_EXTRA_CONTEXT);
  Status = WdfObjectAllocateContext(Child, &ExtraAttributes, (PVOID *)&Extra);
  if (!NT_SUCCESS(Status))
    return Status;
  Check(Extra != NULL && Extra->Cookie == 0 && Extra->Reserved == 0, 7);
  if (Extra == NULL)
    return STATUS_UNSUCCESSFUL;
  Extra->Cookie = 0x45585452;
  Status = WdfObjectAllocateContext(Child, &ExtraAttributes, &Duplicate);
  Check(Status == STATUS_OBJECT_NAME_EXISTS && Duplicate == Extra &&
            ExtraContext(Child) == Extra && Extra->Cookie == 0x45585452,
        8);

  // R intentionally violates the documented reference/dereference pairing.
  if (Marker == L'R') {
    WdfObjectDereference(Child);
    DbgPrint("KMDF lifecycle: forbidden unpaired dereference returned\n");
    return STATUS_UNSUCCESSFUL;
  }
  WdfObjectReference(Child);
  WdfObjectDelete(Child);
  Check(ChildCleanups == 1 && ChildDestroys == 0, 60);
  Check(ChildContext(Child) == ChildData && ChildData->Cleanups == 1, 61);
  if (Marker == L'P') {
    Status = WdfObjectAllocateContext(Child, &ExtraAttributes, &Duplicate);
    Check(Status == STATUS_DELETE_PENDING, 62);
    DbgPrint("KMDF lifecycle: delete pending checked\n");
  }
  WdfObjectDereference(Child);
  Check(ChildCleanups == 1 && ChildDestroys == 1, 63);
  DbgPrint("KMDF lifecycle: child released\n");

  // F deliberately uses a handle after the final reference was released.
  if (Marker == L'F') {
    (void)ChildContext(Child);
    DbgPrint("KMDF lifecycle: forbidden stale handle returned\n");
    return STATUS_UNSUCCESSFUL;
  }
  return Failures == 0 && !EntryWillFail ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
