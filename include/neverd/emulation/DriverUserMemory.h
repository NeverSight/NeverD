//===- DriverUserMemory.h - Explicit request user memory -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declared user buffers and explicit pointer slots, independent of a
/// driver's private transfer layout. Addresses are always guest addresses.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERUSERMEMORY_H
#define NEVERD_EMULATION_DRIVERUSERMEMORY_H

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::emulation {

enum class DriverUserPageAccess {
#define NEVERD_DRIVER_USER_PAGE_ACCESS(Name, Spelling) Name,
#include "neverd/emulation/DriverUserPageAccess.def"
#undef NEVERD_DRIVER_USER_PAGE_ACCESS
};

enum class DriverUserBufferKind {
#define NEVERD_DRIVER_USER_BUFFER_KIND(Name, Spelling) Name,
#include "neverd/emulation/DriverUserMemory.def"
#undef NEVERD_DRIVER_USER_BUFFER_KIND
};

struct DriverUserBuffer {
  std::string ID;
  uint32_t Size = 0;
  /// Initial bytes; remaining storage is zero-initialized.
  std::vector<uint8_t> Input;
  DriverUserPageAccess Access = DriverUserPageAccess::ReadWrite;
};

struct DriverUserBufferRef {
  DriverUserBufferKind Kind = DriverUserBufferKind::Input;
  /// Present only for Memory; identifiers are local to one request.
  std::string ID;
  uint32_t Offset = 0;
};

struct DriverUserPointer {
  DriverUserBufferRef Source;
  DriverUserBufferRef Target;
};

struct DriverUserBufferResult {
  std::string ID;
  uint64_t Address = 0;
  uint32_t Size = 0;
  DriverUserPageAccess Access = DriverUserPageAccess::ReadWrite;
  bool Revoked = false;
  /// Diagnostic backing snapshot, including after original VA revocation.
  /// These bytes do not imply caller-visible output or CPU access rights.
  std::vector<uint8_t> Backing;
};

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERUSERMEMORY_H
