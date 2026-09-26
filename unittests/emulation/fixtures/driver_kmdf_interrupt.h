//===- driver_kmdf_interrupt.h - Genuine WDF interrupt callbacks ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DRIVER_KMDF_INTERRUPT_H
#define NEVERD_DRIVER_KMDF_INTERRUPT_H

#include "driver_kmdf_interrupt_test.h"

ABI_SLOT(WdfInterruptCreate, 141);
ABI_SLOT(WdfInterruptQueueDpcForIsr, 142);
ABI_SLOT(WdfInterruptSynchronize, 143);
ABI_SLOT(WdfInterruptAcquireLock, 144);
ABI_SLOT(WdfInterruptReleaseLock, 145);
ABI_SLOT(WdfInterruptEnable, 146);
ABI_SLOT(WdfInterruptDisable, 147);
ABI_SLOT(WdfInterruptWdmGetInterrupt, 148);
ABI_SLOT(WdfInterruptGetInfo, 149);
ABI_SLOT(WdfInterruptGetDevice, 151);
ABI_SLOT(WdfInterruptQueueWorkItemForIsr, 416);

#define INTERRUPT_FIELD(Type, Field, Offset)                                   \
  C_ASSERT(FIELD_OFFSET(Type, Field) == Offset)

C_ASSERT(sizeof(WDF_INTERRUPT_CONFIG) == 104);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, SpinLock, 8);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, ShareVector, 16);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, FloatingSave, 20);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, AutomaticSerialization, 21);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, EvtInterruptIsr, 24);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, EvtInterruptDpc, 32);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, EvtInterruptEnable, 40);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, EvtInterruptDisable, 48);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, EvtInterruptWorkItem, 56);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, InterruptRaw, 64);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, InterruptTranslated, 72);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, WaitLock, 80);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, PassiveHandling, 88);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, ReportInactiveOnPowerDown, 92);
INTERRUPT_FIELD(WDF_INTERRUPT_CONFIG, CanWakeDevice, 96);
C_ASSERT(sizeof(WDF_INTERRUPT_INFO) == 64);
INTERRUPT_FIELD(WDF_INTERRUPT_INFO, TargetProcessorSet, 16);
INTERRUPT_FIELD(WDF_INTERRUPT_INFO, MessageNumber, 28);
INTERRUPT_FIELD(WDF_INTERRUPT_INFO, Vector, 32);
INTERRUPT_FIELD(WDF_INTERRUPT_INFO, Irql, 36);
INTERRUPT_FIELD(WDF_INTERRUPT_INFO, Mode, 40);
INTERRUPT_FIELD(WDF_INTERRUPT_INFO, Polarity, 44);
INTERRUPT_FIELD(WDF_INTERRUPT_INFO, MessageSignaled, 48);
INTERRUPT_FIELD(WDF_INTERRUPT_INFO, ShareDisposition, 49);
INTERRUPT_FIELD(WDF_INTERRUPT_INFO, Group, 56);
#undef INTERRUPT_FIELD

static WDFDEVICE InterruptDevice;
static WDFINTERRUPT InterruptHandles[KmdfInterruptMessages];
static ULONG InterruptIsrCount, InterruptDeferredCount, InterruptSyncCount;
static ULONG InterruptEnableCount, InterruptDisableCount;
static WDFREQUEST InterruptRequest;
static BOOLEAN InterruptFailure;

typedef struct {
  WDFREQUEST Request;
  KIRQL CompletionIrql;
  BOOLEAN Cleaned;
} INTERRUPT_REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(INTERRUPT_REQUEST_CONTEXT,
                                   InterruptRequestContext);
static INTERRUPT_REQUEST_CONTEXT *InterruptCompletionContext;

static BOOLEAN UsesFrameworkInterrupts(VOID) {
  return ServiceMode == KmdfInterruptLine || ServiceMode == KmdfInterruptMsi ||
         ServiceMode == KmdfInterruptPassive ||
         ServiceMode == KmdfInterruptEnableFailure ||
         ServiceMode == KmdfInterruptPrepare ||
         ServiceMode == KmdfInterruptSerialization ||
         ServiceMode == KmdfInterruptPassiveCleanup ||
         ServiceMode == KmdfInterruptPassiveMsi ||
         ServiceMode == KmdfInterruptPassivePowerWait;
}

static BOOLEAN InterruptIsPassive(VOID) {
  return ServiceMode == KmdfInterruptPassive ||
         ServiceMode == KmdfInterruptPassiveMsi ||
         ServiceMode == KmdfInterruptPassivePowerWait;
}

static ULONG InterruptCount(VOID) {
  return ServiceMode == KmdfInterruptMsi ||
                 ServiceMode == KmdfInterruptPassiveMsi
             ? KmdfInterruptMessages
             : 1;
}

static ULONG InterruptIndex(WDFINTERRUPT Interrupt) {
  ULONG Index;
  for (Index = 0; Index < InterruptCount(); ++Index)
    if (InterruptHandles[Index] == Interrupt)
      return Index;
  InterruptFailure = TRUE;
  return 0;
}

static VOID InterruptCheck(BOOLEAN Condition) {
  if (!Condition) {
    InterruptFailure = TRUE;
    DbgPrint("KMDF interrupt: failure\n");
  }
}

static KIRQL InterruptExecutionIrql(ULONG Index) {
  return InterruptIsPassive() ? PASSIVE_LEVEL : KmdfInterruptIrql + Index;
}

static NTSTATUS InterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE Device) {
  const ULONG Index = InterruptIndex(Interrupt);
  InterruptCheck(Device == InterruptDevice &&
                 WdfInterruptGetDevice(Interrupt) == Device &&
                 WdfInterruptWdmGetInterrupt(Interrupt) != NULL &&
                 KeGetCurrentIrql() == InterruptExecutionIrql(Index));
  ++InterruptEnableCount;
  DbgPrint("KMDF interrupt: enable %lu\n", Index);
  return ServiceMode == KmdfInterruptEnableFailure ? STATUS_UNSUCCESSFUL
                                                   : STATUS_SUCCESS;
}

static NTSTATUS InterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE Device) {
  const ULONG Index = InterruptIndex(Interrupt);
  InterruptCheck(Device == InterruptDevice &&
                 KeGetCurrentIrql() == InterruptExecutionIrql(Index));
  ++InterruptDisableCount;
  DbgPrint("KMDF interrupt: disable %lu\n", Index);
  return STATUS_SUCCESS;
}

static NTSTATUS InterruptPostEntry(WDFDEVICE Device,
                                   WDF_POWER_DEVICE_STATE PreviousState) {
  UNREFERENCED_PARAMETER(PreviousState);
  InterruptCheck(Device == InterruptDevice &&
                 KeGetCurrentIrql() == PASSIVE_LEVEL &&
                 InterruptEnableCount > InterruptDisableCount);
  DbgPrint("KMDF interrupt: post entry\n");
  return InterruptFailure ? STATUS_INVALID_DEVICE_STATE : STATUS_SUCCESS;
}

static NTSTATUS InterruptPreExit(WDFDEVICE Device,
                                 WDF_POWER_DEVICE_STATE TargetState) {
  UNREFERENCED_PARAMETER(TargetState);
  InterruptCheck(Device == InterruptDevice &&
                 KeGetCurrentIrql() == PASSIVE_LEVEL &&
                 InterruptEnableCount > InterruptDisableCount);
  DbgPrint("KMDF interrupt: pre exit\n");
  if (ServiceMode == KmdfInterruptPassivePowerWait) {
    LARGE_INTEGER Delay;
    Delay.QuadPart = -KmdfInterruptPowerDelay100ns;
    InterruptCheck(
        NT_SUCCESS(KeDelayExecutionThread(KernelMode, FALSE, &Delay)));
    InterruptCheck(KeGetCurrentIrql() == PASSIVE_LEVEL);
  }
  return InterruptFailure ? STATUS_INVALID_DEVICE_STATE : STATUS_SUCCESS;
}

static BOOLEAN InterruptSynchronize(WDFINTERRUPT Interrupt,
                                    WDFCONTEXT Context) {
  const ULONG Index = InterruptIndex(Interrupt);
  InterruptCheck(Context == &InterruptSyncCount &&
                 KeGetCurrentIrql() == InterruptExecutionIrql(Index));
  ++InterruptSyncCount;
  if (InterruptCount() > 1 && !Index) {
    InterruptCheck(WdfInterruptSynchronize(InterruptHandles[1],
                                           InterruptSynchronize,
                                           Context) == KmdfInterruptSyncResult);
    InterruptCheck(KeGetCurrentIrql() == InterruptExecutionIrql(Index));
  }
  return KmdfInterruptSyncResult;
}

static VOID InterruptRequestCleanup(WDFOBJECT Object) {
  INTERRUPT_REQUEST_CONTEXT *Context = InterruptRequestContext(Object);
  LARGE_INTEGER Delay;
  InterruptCheck(Context->Request == (WDFREQUEST)Object &&
                 Context->CompletionIrql == DISPATCH_LEVEL &&
                 !Context->Cleaned && KeGetCurrentIrql() == PASSIVE_LEVEL);
  DbgPrint("KMDF interrupt: request cleanup begin\n");
  Delay.QuadPart = -KmdfInterruptDelay100ns;
  InterruptCheck(NT_SUCCESS(KeDelayExecutionThread(KernelMode, FALSE, &Delay)));
  InterruptCheck(KeGetCurrentIrql() == PASSIVE_LEVEL);
  Context->Cleaned = TRUE;
  DbgPrint("KMDF interrupt: request cleanup end\n");
}

static VOID InterruptRequestDestroy(WDFOBJECT Object) {
  INTERRUPT_REQUEST_CONTEXT *Context = InterruptRequestContext(Object);
  InterruptCheck(Context->Cleaned && KeGetCurrentIrql() == PASSIVE_LEVEL);
  DbgPrint("KMDF interrupt: request destroy\n");
}

static VOID InterruptDeferred(WDFINTERRUPT Interrupt,
                              WDFOBJECT AssociatedObject) {
  ULONG *Output;
  NTSTATUS Status;
  InterruptCheck(AssociatedObject == InterruptDevice &&
                 WdfInterruptGetDevice(Interrupt) == InterruptDevice &&
                 KeGetCurrentIrql() ==
                     (InterruptIsPassive() ? PASSIVE_LEVEL : DISPATCH_LEVEL));
  ++InterruptDeferredCount;
  DbgPrint("KMDF interrupt: deferred %lu\n", InterruptIndex(Interrupt));
  if (InterruptDeferredCount != InterruptCount())
    return;
  Status = WdfRequestRetrieveOutputBuffer(
      InterruptRequest, KmdfInterruptSnapshotWords * sizeof(ULONG),
      (PVOID *)&Output, NULL);
  if (NT_SUCCESS(Status)) {
    Output[0] = InterruptIsrCount;
    Output[1] = InterruptDeferredCount;
    Output[2] = InterruptSyncCount;
    Output[3] = InterruptEnableCount;
    if (InterruptFailure)
      Status = STATUS_INVALID_DEVICE_STATE;
  }
  if (InterruptCompletionContext)
    InterruptCompletionContext->CompletionIrql = KeGetCurrentIrql();
  WdfRequestCompleteWithInformation(
      InterruptRequest, Status,
      NT_SUCCESS(Status) ? KmdfInterruptSnapshotWords * sizeof(ULONG) : 0);
  InterruptRequest = NULL;
  InterruptCompletionContext = NULL;
}

static BOOLEAN InterruptIsr(WDFINTERRUPT Interrupt, ULONG MessageID) {
  const ULONG Index = InterruptIndex(Interrupt);
  InterruptCheck(KeGetCurrentIrql() == InterruptExecutionIrql(Index) &&
                 MessageID == Index);
  ++InterruptIsrCount;
  DbgPrint("KMDF interrupt: ISR %lu\n", MessageID);
  if (InterruptIsPassive()) {
    LARGE_INTEGER Delay;
    Delay.QuadPart = -KmdfInterruptDelay100ns;
    InterruptCheck(
        NT_SUCCESS(KeDelayExecutionThread(KernelMode, FALSE, &Delay)));
    InterruptCheck(WdfInterruptQueueWorkItemForIsr(Interrupt));
    InterruptCheck(!WdfInterruptQueueWorkItemForIsr(Interrupt));
  } else {
    InterruptCheck(WdfInterruptQueueDpcForIsr(Interrupt));
    InterruptCheck(!WdfInterruptQueueDpcForIsr(Interrupt));
  }
  return TRUE;
}

static NTSTATUS CreateFrameworkInterrupts(WDFDEVICE Device, WDFCMRESLIST Raw,
                                          WDFCMRESLIST Translated) {
  ULONG Index;
  InterruptDevice = Device;
  for (Index = 0; Index < InterruptCount(); ++Index) {
    WDF_INTERRUPT_CONFIG Config;
    NTSTATUS Status;
    WDF_INTERRUPT_CONFIG_INIT(&Config, InterruptIsr,
                              InterruptIsPassive() ? NULL : InterruptDeferred);
    Config.PassiveHandling = InterruptIsPassive();
    Config.AutomaticSerialization = ServiceMode == KmdfInterruptSerialization;
    Config.EvtInterruptEnable = InterruptEnable;
    Config.EvtInterruptDisable = InterruptDisable;
    if (InterruptIsPassive())
      Config.EvtInterruptWorkItem = InterruptDeferred;
    if (Raw && Translated) {
      Config.InterruptRaw = WdfCmResourceListGetDescriptor(Raw, 0);
      Config.InterruptTranslated =
          WdfCmResourceListGetDescriptor(Translated, 0);
    }
    Status = WdfInterruptCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES,
                                &InterruptHandles[Index]);
    if (!NT_SUCCESS(Status))
      return Status;
  }
  return STATUS_SUCCESS;
}

static VOID InterruptIoControl(WDFREQUEST Request, size_t OutputLength) {
  ULONG Index;
  InterruptIsrCount = InterruptDeferredCount = InterruptSyncCount = 0;
  if (OutputLength < KmdfInterruptSnapshotWords * sizeof(ULONG)) {
    WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
    return;
  }
  if (ServiceMode == KmdfInterruptPassiveCleanup) {
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes,
                                            INTERRUPT_REQUEST_CONTEXT);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    Attributes.EvtCleanupCallback = InterruptRequestCleanup;
    Attributes.EvtDestroyCallback = InterruptRequestDestroy;
    Status = WdfObjectAllocateContext(Request, &Attributes,
                                      (PVOID *)&InterruptCompletionContext);
    if (!NT_SUCCESS(Status)) {
      WdfRequestComplete(Request, Status);
      return;
    }
    InterruptCompletionContext->Request = Request;
  }
  for (Index = 0; Index < InterruptCount(); ++Index) {
    const WDFINTERRUPT Interrupt = InterruptHandles[Index];
    WDF_INTERRUPT_INFO Info;
    WDF_INTERRUPT_INFO_INIT(&Info);
    WdfInterruptGetInfo(Interrupt, &Info);
    InterruptCheck(Info.Vector == KmdfInterruptVector + Index &&
                   Info.Irql == InterruptExecutionIrql(Index) &&
                   Info.TargetProcessorSet == 1 && Info.Group == 0 &&
                   Info.MessageNumber == Index &&
                   !!Info.MessageSignaled == (InterruptCount() > 1));
    WdfInterruptAcquireLock(Interrupt);
    InterruptCheck(KeGetCurrentIrql() == InterruptExecutionIrql(Index));
    WdfInterruptReleaseLock(Interrupt);
    InterruptCheck(KeGetCurrentIrql() == PASSIVE_LEVEL);
    InterruptCheck(WdfInterruptSynchronize(Interrupt, InterruptSynchronize,
                                           &InterruptSyncCount) ==
                   KmdfInterruptSyncResult);
    InterruptCheck(KeGetCurrentIrql() == PASSIVE_LEVEL);
    WdfInterruptDisable(Interrupt);
    WdfInterruptEnable(Interrupt);
  }
  InterruptRequest = Request;
}

#endif
