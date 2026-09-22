//===- KernelFramework.h - Guest KMDF bindings and objects ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Session-owned KMDF identities, typed contexts and guest callback lifetimes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELFRAMEWORK_H
#define NEVERD_EMULATION_WINDOWS_KERNELFRAMEWORK_H

#include "../GuestMemory.h"
#include "KernelExportRegistry.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace neverd::emulation {
namespace framework {
#define NEVERD_FRAMEWORK_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "KernelFrameworkValues.def"
#undef NEVERD_FRAMEWORK_VALUE
} // namespace framework

class KernelFramework {
public:
  using Allocate = std::function<llvm::Expected<uint64_t>(uint64_t)>;
  using Validate = std::function<llvm::Error(uint64_t, uint32_t, bool)>;
  using Release = std::function<llvm::Error(uint64_t, uint64_t)>;
  struct GuestCall {
    uint64_t Token = 0;
    uint64_t PC = 0;
    std::vector<uint64_t> Arguments;
  };
  KernelFramework(GuestMemory &Memory, KernelExportRegistry &Exports,
                  Allocate AllocateStorage, Validate ValidateAccess,
                  Release ReleaseStorage)
      : Memory(Memory), Exports(Exports), AllocateStorage(AllocateStorage),
        ValidateAccess(ValidateAccess), ReleaseStorage(ReleaseStorage) {}

  void configure(uint64_t Driver, uint64_t RegistryPath,
                 std::string ServiceName);
  static std::optional<unsigned>
  argumentCount(const KernelExportRegistry::Export &Export);
  llvm::Expected<uint64_t> call(const KernelExportRegistry::Export &Export,
                                llvm::ArrayRef<uint64_t> Arguments,
                                uint8_t IRQL);
  std::optional<GuestCall> takeGuestCall();
  /// Resume one suspended framework operation after its actual guest callback.
  llvm::Expected<std::optional<uint64_t>> finishGuestCall(uint64_t Token,
                                                          uint64_t Result);
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;
  bool hasLiveBinding() const;

private:
  GuestMemory &Memory;
  KernelExportRegistry &Exports;
  Allocate AllocateStorage;
  Validate ValidateAccess;
  Release ReleaseStorage;
  uint64_t Driver = 0, RegistryPath = 0;
  std::string ServiceName;
  struct Region {
    uint64_t Size;
    bool Writable;
    bool Opaque;
    bool Freed = false;
  };
  std::map<uint64_t, Region> Regions;
  struct Binding {
    uint64_t Info = 0, Globals = 0, Table = 0, Module = 0;
    uint64_t DriverHandle = 0, RegistryCopy = 0;
    uint64_t UnloadCallback = 0;
    std::vector<uint8_t> RegistryBytes;
    bool Unloaded = false, Unbinding = false, Unbound = false;
  };
  std::map<uint64_t, Binding> Bindings;
  struct Context {
    uint64_t Address = 0, Size = 0;
    uint64_t Cleanup = 0, Destroy = 0;
  };
  struct Attributes {
    uint64_t Parent = 0, Cleanup = 0, Destroy = 0;
    uint64_t Type = 0, ContextSize = 0;
  };
  enum class AttributesUse { Driver, Object, AdditionalContext };
  using AttributeResult = std::variant<Attributes, uint32_t>;
  struct Object {
    uint64_t Binding = 0, Parent = 0;
    bool DriverObject = false, Deleting = false, Cleaned = false;
    bool DestroyEligible = false;
    uint64_t References = 0;
    std::map<uint64_t, Context> Contexts;
    std::vector<uint64_t> ContextOrder;
    std::vector<uint64_t> Children;
  };
  std::map<uint64_t, Object> Objects;
  enum class StepKind {
    Callback,
    Cleaned,
    TryDestroy,
    Destroy,
    BeginDriverDelete,
    FinishBindingUnbind,
    DriverUnloaded
  };
  struct Step {
    StepKind Kind;
    uint64_t Object;
    uint64_t PC = 0;
  };
  struct Continuation {
    std::vector<Step> Steps;
    size_t Index = 0;
  };
  uint64_t NextContinuation = 1;
  std::map<uint64_t, Continuation> Continuations;
  std::optional<GuestCall> PendingCall;

  llvm::Expected<uint64_t> read(uint64_t Address, unsigned Width = 8);
  llvm::Expected<std::vector<uint8_t>> readRegistryPath(uint64_t Address);
  llvm::Error writable(uint64_t Address, uint32_t Size);
  llvm::Expected<uint64_t> allocate(uint64_t Size, bool Writable, bool Opaque);
  llvm::Error retire(uint64_t Address);
  llvm::Expected<uint64_t> bind(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> unbind(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error finishUnbind(Binding &B);
  llvm::Expected<AttributeResult> attributes(uint64_t Address,
                                             AttributesUse Use);
  llvm::Expected<uint64_t> createObject(uint64_t Globals, const Attributes &A,
                                        bool IsDriver);
  llvm::Expected<uint64_t> addContext(Object &O, const Attributes &A);
  llvm::Expected<uint64_t> createDriver(Binding &B,
                                        llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error planDelete(uint64_t Handle, std::vector<Step> &Steps);
  llvm::Expected<std::optional<uint64_t>> advance(uint64_t Token);
  llvm::Expected<uint64_t> start(std::vector<Step> Steps);
};
} // namespace neverd::emulation
#endif
