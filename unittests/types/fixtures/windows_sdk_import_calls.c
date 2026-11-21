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

#include <winsock2.h>
#include <windows.h>

#include <diagnosticdataquery.h>
#include <icm.h>
#include <mscat.h>
#include <physicalmonitorenumerationapi.h>
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
