//===- DriverProcessEvents.h - Declared process lifecycle -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declared process lifecycle.
///

#ifndef NEVERD_EMULATION_DRIVERPROCESSEVENTS_H
#define NEVERD_EMULATION_DRIVERPROCESSEVENTS_H
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace neverd::emulation {
struct DriverRequest;
struct DriverRegistryEvent;
// Explicit ordinary Win32 process-creation observations. The executable file
// has an opaque borrowed identity; FILE_OBJECT field access needs a file owner.
struct DriverProcessCreateInfo {
  bool FileOpenNameAvailable = false;
  std::string ImageFileName;
  std::optional<std::string> CommandLine;
};
struct DriverProcessEvent {
  enum class Kind { Create, Exit };
  Kind Type = Kind::Create;
  uint32_t ProcessID = 0, ParentID = 0, CreatorProcessID = 4;
  // Events execute in array order at this request boundary; requests.size()
  // denotes the boundary after all requests, before unload.
  size_t BeforeRequest = 0;
  // Exit-only observation, visible before exit callbacks. Omission leaves
  // the terminal status unknown; it does not imply successful termination.
  std::optional<uint32_t> ExitStatus;
  std::optional<DriverProcessCreateInfo> CreateInfo;
};
inline constexpr size_t MaxDriverProcessEvents = 256;
struct DriverProcessEventResult {
  DriverProcessEvent Event;
  size_t SourceIndex = 0;
  uint32_t ActorProcessID = 0;
  std::vector<uint64_t> DeliveredRoutines;
  bool Completed = false;
  std::optional<uint32_t> CreationStatus;
};
llvm::Error validateDriverProcessEvents(
    llvm::ArrayRef<DriverProcessEvent> Events,
    llvm::ArrayRef<DriverRequest> Requests,
    llvm::ArrayRef<uint32_t> InitialProcesses = {},
    llvm::ArrayRef<DriverRegistryEvent> RegistryEvents = {});
} // namespace neverd::emulation
#endif
