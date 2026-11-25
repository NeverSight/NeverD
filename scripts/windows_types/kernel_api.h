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

// The ReactOS process manager documents the exported PEB query.
// Other headers may complete the PEB and transaction types.
struct _PEB *NTAPI PsGetProcessPeb(PEPROCESS Process);
// The Microsoft driver documentation supplies this declaration outside wdm.h.
// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-rtlansistringtounicodestring
WCHAR NTAPI RtlAnsiCharToUnicodeChar(PUCHAR *SourceCharacter);
typedef struct _KTRANSACTION *PKTRANSACTION;

// The kernel declaration and information classes come from phnt. The callable
// signature also matches the documented ntdll entry.
// https://learn.microsoft.com/windows/win32/sysinfo/zwquerysysteminformation
NTSTATUS NTAPI ZwQuerySystemInformation(
    SYSTEM_INFORMATION_CLASS SystemInformationClass, PVOID SystemInformation,
    ULONG SystemInformationLength, PULONG ReturnLength);

#endif // NEVERD_SCRIPTS_WINDOWS_TYPES_KERNEL_API_H
