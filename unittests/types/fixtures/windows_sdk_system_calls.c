//===- windows_sdk_system_calls.c - Original Windows SDK callers ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Cross-target callers of system APIs using the Microsoft headers and import
/// libraries. The x86 compilation uses the SDK libraries' stdcall profile.
///
//===----------------------------------------------------------------------===//

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <ktmw32.h>
#include <sensorsdef.h>
#include <sensorsutils.h>
#include <snmp.h>
#include <webauthn.h>
// clang-format on

__declspec(dllexport) BOOL UseIcmpHandle(HANDLE Handle) {
  return IcmpCloseHandle(Handle);
}

__declspec(dllexport) BOOL UseTransactionClock(HANDLE Manager,
                                               LARGE_INTEGER *Clock) {
  return GetCurrentClockTransactionManager(Manager, Clock);
}

__declspec(dllexport) BOOL UseTransactionManagerId(HANDLE Manager, GUID *Id) {
  return GetTransactionManagerId(Manager, Id);
}

__declspec(dllexport) NTSTATUS UseSensorSerialization(
    PSENSOR_COLLECTION_LIST Collection, ULONG *Size, BYTE **Bytes) {
  return CollectionsListAllocateBufferAndSerialize(Collection, Size, Bytes);
}

__declspec(dllexport) void UseSnmpRelease(void *Buffer) {
  SnmpUtilMemFree(Buffer);
}

__declspec(dllexport) HRESULT UsePlatformAuthenticator(BOOL *Available) {
  return WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable(Available);
}
