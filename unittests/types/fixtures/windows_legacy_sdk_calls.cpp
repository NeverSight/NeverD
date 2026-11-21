//===- windows_legacy_sdk_calls.cpp - Archived Microsoft SDK callers -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Import-only callers compiled with the original AllJoyn and P2P headers.
///
//===----------------------------------------------------------------------===//

#include <winsock2.h>
#include <windows.h>

#include <alljoyn_c/version.h>
#include <p2p.h>

extern "C" {
__declspec(dllexport) uint32_t __cdecl UseAllJoynVersion(void) {
  return alljoyn_getnumericversion();
}

__declspec(dllexport) HRESULT __cdecl
UsePeerGroupStartup(WORD Version, PPEER_VERSION_DATA Data) {
  return PeerGroupStartup(Version, Data);
}

__declspec(dllexport) HRESULT __cdecl
UsePeerGraphConnect(HGRAPH Graph, PCWSTR Peer, PPEER_ADDRESS Address,
                    ULONGLONG *Connection) {
  return PeerGraphConnect(Graph, Peer, Address, Connection);
}
}
