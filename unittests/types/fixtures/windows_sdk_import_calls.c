//===- windows_sdk_import_calls.c - Original SDK import callers -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Import-only C callers for the current SDK's module-scoped APIs.
///
//===----------------------------------------------------------------------===//

#define PSAPI_VERSION 1
#include <winsock2.h>
#include <windows.h>

#include <d3d10.h>
#include <diagnosticdataquery.h>
#include <icm.h>
#include <mscat.h>
#include <ncryptprotect.h>
#include <oleacc.h>
#include <physicalmonitorenumerationapi.h>
#include <processsnapshot.h>
#include <psapi.h>
#include <wsdapi.h>

__declspec(dllexport) BOOL __cdecl UseColorProfile(HPROFILE Profile,
                                                   PPROFILEHEADER Header) {
  return GetColorProfileHeader(Profile, Header);
}

__declspec(dllexport) BOOL __cdecl UseCatalog(HANDLE Catalog) {
  return CryptCATClose(Catalog);
}

__declspec(dllexport) BOOL __cdecl UsePhysicalMonitor(HANDLE Monitor) {
  return DestroyPhysicalMonitor(Monitor);
}

__declspec(dllexport) HRESULT __cdecl
UseDiagnosticSession(HDIAGNOSTIC_DATA_QUERY_SESSION Session) {
  return DdqCloseSession(Session);
}

__declspec(dllexport) HRESULT __cdecl UseDeviceElement(WSDXML_ELEMENT *Element) {
  return WSDXMLCleanupElement(Element);
}

__declspec(dllexport) HRESULT __cdecl UseDataBlob(SIZE_T Size, LPD3D10BLOB *Blob) {
  return D3D10CreateBlob(Size, Blob);
}

__declspec(dllexport) SECURITY_STATUS __cdecl
UseProtectionDescriptor(NCRYPT_DESCRIPTOR_HANDLE Descriptor) {
  return NCryptCloseProtectionDescriptor(Descriptor);
}

__declspec(dllexport) UINT __cdecl UseRoleText(DWORD Role, LPWSTR Text, UINT Length) {
  return GetRoleTextW(Role, Text, Length);
}

__declspec(dllexport) BOOL __cdecl UseProcessModules(
    HANDLE Process, HMODULE *Modules, DWORD Bytes, LPDWORD Needed, DWORD Filter) {
  return EnumProcessModulesEx(Process, Modules, Bytes, Needed, Filter);
}

__declspec(dllexport) DWORD __cdecl UseReleaseSnapshot(HANDLE Process, HPSS Snapshot) {
  return PssFreeSnapshot(Process, Snapshot);
}
