// Original import-only fixture, compiled against the pinned Microsoft WDK.
#include <ntddk.h>

// Each provider requires the WDK base declarations first.
#include <portcls.h>
#include <sdport.h>
#include <strmini.h>

// Stable public fixture names; imported APIs retain the WDK's conventions.
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
