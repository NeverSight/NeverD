//===- windows_rdbss_provider_calls.cpp - Original RDBSS callers ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original import-only callers compiled against the pinned Microsoft WDK.
///
//===----------------------------------------------------------------------===//
extern "C" {
#include <rxprocs.h>

// mrx.h requires the base RDBSS declarations above.
#include <mrx.h>

__declspec(dllexport) NTSTATUS __cdecl
UseRdbssRegister(PRDBSS_DEVICE_OBJECT *Device, PDRIVER_OBJECT Driver,
                 PMINIRDR_DISPATCH Dispatch, ULONG Controls,
                 PUNICODE_STRING Name, ULONG ExtensionSize, DEVICE_TYPE Type,
                 ULONG Characteristics) {
  return RxRegisterMinirdr(Device, Driver, Dispatch, Controls, Name,
                           ExtensionSize, Type, Characteristics);
}

__declspec(dllexport) NTSTATUS __cdecl UseRdbssStart(PRX_CONTEXT Context,
                                                     PBOOLEAN PostToFsp) {
  return RxStartMinirdr(Context, PostToFsp);
}

__declspec(dllexport) void __cdecl
UseRdbssUnregister(PRDBSS_DEVICE_OBJECT Device) {
  RxpUnregisterMinirdr(Device);
}

__declspec(dllexport) PRX_CONTEXT __cdecl
UseRdbssCreateContext(PIRP Irp, PRDBSS_DEVICE_OBJECT Device, ULONG Flags) {
  return RxCreateRxContext(Irp, Device, Flags);
}

__declspec(dllexport) NTSTATUS __cdecl UseRdbssInitializeDispatcher(void) {
  return RxInitializeDispatcher();
}

__declspec(dllexport) NTSTATUS __cdecl UseRdbssInitializeTimer(void) {
  return RxInitializeRxTimer();
}

__declspec(dllexport) NTSTATUS __cdecl UseRdbssTearDownDispatcher(void) {
  return RxTearDownDispatcher();
}
}
