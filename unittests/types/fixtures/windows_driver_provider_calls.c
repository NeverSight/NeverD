// Original import-only fixture, compiled against the pinned Microsoft WDK.
#include <ntddk.h>

// Each provider requires the WDK base declarations first.
#define NDIS685
#include <ndis.h>
#include <ndis/poll.h>
#include <portcls.h>
#include <sdport.h>
#include <strmini.h>
#include <usb.h>
#include <usbcamdi.h>
#include <usbpmapi.h>
#include <wpprecorder.h>

// Stable public fixture names; imported APIs retain the WDK's conventions.
__declspec(dllexport) void __cdecl UseNetworkPoll(NDIS_POLL_HANDLE Poll) {
  NdisRequestPoll(Poll, NULL);
}

__declspec(dllexport) void __cdecl UseRecorderLog(PVOID Control, RECORDER_LOG Log) {
  imp_WppRecorderLogDelete(Control, Log);
}

__declspec(dllexport) NTSTATUS __cdecl UseUSBPowerClient(USBPM_CLIENT Client) {
  return UsbPm_Deregister(Client);
}

__declspec(dllexport) ULONG __cdecl
UseSDConfig(PSD_MINIPORT Miniport, UCHAR Offset, PUCHAR Buffer, ULONG Length) {
  return SdPortGetPciConfigSpace(Miniport, Offset, Buffer, Length);
}

__declspec(dllexport) NTSTATUS __cdecl
UseAudioAdapter(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Device,
                PCPFNSTARTDEVICE Start, ULONG MaxObjects, ULONG ExtensionSize) {
  return PcAddAdapterDevice(Driver, Device, Start, MaxObjects, ExtensionSize);
}

__declspec(dllexport) void __cdecl UseStreamClock(HANDLE Clock,
                                                  PHW_TIME_CONTEXT Context) {
  StreamClassQueryMasterClockSync(Clock, Context);
}

__declspec(dllexport) NTSTATUS __cdecl
UseCameraValue(HANDLE Handle, PWCHAR Name, ULONG NameLength, PVOID Data,
               ULONG DataLength) {
  return USBCAMD_GetRegistryKeyValue(Handle, Name, NameLength, Data, DataLength);
}
