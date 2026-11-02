//===- kernel_netio.h - Shared kernel networking types -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// MinGW keeps these shared socket types in its user-mode winsock2.h. Supply
/// their Windows layouts without bringing user-mode service declarations into
/// a driver. Derived from mingw-w64's public-domain winsock2.h; its notice is
/// in LICENSES/mingw-w64-headers.txt.

#ifndef NEVERD_SCRIPTS_WINDOWS_TYPES_KERNEL_NETIO_H
#define NEVERD_SCRIPTS_WINDOWS_TYPES_KERNEL_NETIO_H

#include <psdk_inc/_ip_types.h>

typedef USHORT ADDRESS_FAMILY;
typedef struct sockaddr_storage {
  ADDRESS_FAMILY ss_family;
  CHAR __ss_pad1[6];
  __declspec(align(8)) __int64 __ss_align;
  CHAR __ss_pad2[112];
} SOCKADDR_STORAGE, *PSOCKADDR_STORAGE;

typedef struct _SOCKET_ADDRESS {
  PSOCKADDR lpSockaddr;
  int iSockaddrLength;
} SOCKET_ADDRESS, *PSOCKET_ADDRESS;

// netioapi.h needs the shared IP definitions, not Routing/RAS's user APIs.
#define __ROUTING_IPRTRMIB_H__
#include <netioapi.h>

#endif // NEVERD_SCRIPTS_WINDOWS_TYPES_KERNEL_NETIO_H
