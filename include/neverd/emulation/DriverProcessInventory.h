//===- DriverProcessInventory.h - Declared guest process metadata ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declared guest process metadata.
///

#ifndef NEVERD_EMULATION_DRIVERPROCESSINVENTORY_H
#define NEVERD_EMULATION_DRIVERPROCESSINVENTORY_H
#include "neverd/emulation/DriverProcessEvents.h"
#include "neverd/emulation/DriverProcessImages.h"
#include "neverd/emulation/DriverProcessMemory.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {
struct DriverOptions;
inline constexpr size_t DriverProcessImageFileNameSize = 15;
struct DriverProcessInformation {
  uint32_t ProcessID = 0, ParentID = 0;
  std::string ImageName;
  // Optional observation for a borrowed external thread whose executable
  // entry is otherwise outside this driver session. Zero is an explicit value.
  std::optional<uint64_t> BorrowedThreadStart;
  // ETHREAD.StartAddress for a borrowed user thread, distinct from its Win32
  // start above. Class-5 KernelMode output exposes this entry on 19041.
  std::optional<uint64_t> BorrowedThreadSystemStart;
  // Exact native short-name bytes. Encoding, truncation and NUL termination
  // are provider observations; the UTF-16 query name does not infer them.
  std::optional<std::array<uint8_t, DriverProcessImageFileNameSize>>
      ImageFileName;
#define NEVERD_PROCESS_COUNTER(Member, JSON, Type, Default, Offset, Width)     \
  Type Member = Default;
#include "neverd/emulation/DriverProcessInventory.def"
#undef NEVERD_PROCESS_COUNTER
  // Independent EPROCESS.SectionBaseAddress observation. Zero is a supplied
  // null value, distinct from an unknown field; this does not map image bytes.
  std::optional<uint64_t> SectionBaseAddress;
  // Explicit finite process RAM catalogue, independent of request-owned
  // buffers and section views. Omission leaves this provider unavailable.
  std::optional<std::vector<DriverProcessMemoryRegion>> Memory;
  // Explicit independent SEC_IMAGE file snapshots. An image-looking RAM
  // region never supplies this provenance. No host files are opened.
  std::vector<DriverProcessImageMapping> Images;
  // Independent EPROCESS.Peb observation. A null pointer is explicit; the
  // pointed-to user bytes are supplied by Memory, never inferred here.
  std::optional<uint64_t> PebAddress;
};
using DriverProcessInventory = std::vector<DriverProcessInformation>;
inline constexpr size_t MaxDriverProcessInventory = 128;
llvm::Error validateDriverProcessInventory(const DriverOptions &);
// Shared initial membership for scenario preflight and the runtime lifecycle.
// A declared future CREATE contributes metadata, not an already-live process.
std::vector<uint32_t>
initialDriverProcessIDs(llvm::ArrayRef<DriverProcessEvent>,
                        llvm::ArrayRef<DriverRequest>,
                        const std::optional<DriverProcessInventory> &);
} // namespace neverd::emulation
#endif
