//===- kernel_api.h - Kernel declarations missing from MinGW's DDK --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// ABI declarations checked against the Microsoft WDK and phnt. These
/// supplement the generated library; they do not model a routine's effects.

#ifndef NEVERD_SCRIPTS_WINDOWS_TYPES_KERNEL_API_H
#define NEVERD_SCRIPTS_WINDOWS_TYPES_KERNEL_API_H

// https://learn.microsoft.com/windows-hardware/drivers/ddi/ndis/ns-ndis-_ndis_configuration_object
typedef struct _NDIS_CONFIGURATION_OBJECT {
  NDIS_OBJECT_HEADER Header;
  NDIS_HANDLE NdisHandle;
  ULONG Flags;
} NDIS_CONFIGURATION_OBJECT, *PNDIS_CONFIGURATION_OBJECT;

// https://learn.microsoft.com/windows-hardware/drivers/ddi/ndis/nf-ndis-ndisopenconfigurationex
NDIS_STATUS NTAPI NdisOpenConfigurationEx(
    PNDIS_CONFIGURATION_OBJECT ConfigObject, PNDIS_HANDLE ConfigurationHandle);

// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-mmmapiospaceex
PVOID NTAPI MmMapIoSpaceEx(PHYSICAL_ADDRESS PhysicalAddress,
                           SIZE_T NumberOfBytes, ULONG Protect);

// Native API declarations: phnt ntrtl.h and the kernel's exported PEB query.
// The PEB and transaction remain opaque unless another header defines them.
struct _PEB *NTAPI PsGetProcessPeb(PEPROCESS Process);
WCHAR NTAPI RtlAnsiCharToUnicodeChar(PUCHAR *SourceCharacter);
typedef struct _KTRANSACTION *PKTRANSACTION;

// The information-class enum is supplied from phnt by the generator.
NTSTATUS NTAPI ZwQuerySystemInformation(
    SYSTEM_INFORMATION_CLASS SystemInformationClass, PVOID SystemInformation,
    ULONG SystemInformationLength, PULONG ReturnLength);

#endif // NEVERD_SCRIPTS_WINDOWS_TYPES_KERNEL_API_H
