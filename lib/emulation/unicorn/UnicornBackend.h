//===- UnicornBackend.h - Private CPU backend -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private CPU backend.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_UNICORNBACKEND_H
#define NEVERD_EMULATION_UNICORNBACKEND_H
#include "../GuestMemory.h"
#include "../X64Registers.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
namespace neverd::emulation {
enum class BackendFaultKind {
#define NEVERD_UNICORN_FAULT_KIND(Name, Spelling) Name,
#include "UnicornFaults.def"
#undef NEVERD_UNICORN_FAULT_KIND
};
enum class BackendAccessKind {
#define NEVERD_UNICORN_ACCESS_KIND(Name, Spelling) Name,
#include "UnicornFaults.def"
#undef NEVERD_UNICORN_ACCESS_KIND
};
const char *backendFaultKindName(BackendFaultKind Kind);
const char *backendAccessKindName(BackendAccessKind Kind);
struct BackendFault {
  BackendFaultKind Kind;
  uint64_t PC = 0;
  /// Memory-event address and access size, not a decoded operand extent.
  /// Unicorn may split a memory access at a page boundary.
  std::optional<uint64_t> Address;
  std::optional<uint64_t> Size;
  std::optional<BackendAccessKind> Access;
  std::optional<uint32_t> Interrupt;
};
struct BackendHooks {
  std::function<void(uint64_t, uint32_t)> Instruction;
  std::function<void(uint64_t, uint32_t)> Read;
  std::function<void(uint64_t, uint32_t, uint64_t)> Write;
  std::function<void(uint64_t, uint32_t, const char *)> Fault;
  std::function<void(uint32_t)> Interrupt;
  std::function<void()> InvalidInstruction;
};
/// An owning CPU snapshot associated with exactly one backend instance.
/// Guest memory and hooks are shared by all contexts and are never rolled back.
/// The snapshot may outlive its backend, but can no longer be used afterward.
class BackendContext final {
public:
  ~BackendContext();
  BackendContext(BackendContext &&) noexcept;
  BackendContext &operator=(BackendContext &&) noexcept;
  BackendContext(const BackendContext &) = delete;
  BackendContext &operator=(const BackendContext &) = delete;

private:
  friend class UnicornBackend;
  struct Impl;
  explicit BackendContext(std::unique_ptr<Impl> State);
  std::unique_ptr<Impl> State;
};
/// Executes against the GuestMemory virtual address space, without an MMU
/// model.
class UnicornBackend final : public GuestMemory {
public:
  static llvm::Expected<std::unique_ptr<UnicornBackend>>
  create(uint64_t MemoryLimit);
  ~UnicornBackend() override;
  llvm::Error map(uint64_t Address, uint64_t Size,
                  unsigned Permissions) override;
  llvm::Error protect(uint64_t Address, uint64_t Size,
                      unsigned Permissions) override;
  llvm::Error mapMMIO(uint64_t Address, uint64_t Size,
                      GuestMMIOCallbacks Callbacks) override;
  llvm::Error unmapMMIO(uint64_t Address, uint64_t Size) override;
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) override;
  llvm::Error write(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes) override;
  llvm::Error validateBacking(uint64_t Address, uint64_t Size) const override;
  llvm::Error readBacking(uint64_t Address,
                          llvm::MutableArrayRef<uint8_t> Bytes) override;
  llvm::Error writeBacking(uint64_t Address,
                           llvm::ArrayRef<uint8_t> Bytes) override;
  llvm::Error fetch(uint64_t Address, llvm::MutableArrayRef<uint8_t> Bytes);
  llvm::Expected<uint64_t> reg(X64Register Register);
  llvm::Error setReg(X64Register Register, uint64_t Value);
  /// Capture the complete Unicorn CPU state, including SIMD and FPU registers.
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext();
  /// Replace an existing snapshot with this backend's current CPU state.
  llvm::Error saveContext(BackendContext &Context);
  /// Restore between run() calls. A snapshot cannot recover a faulted CPU.
  llvm::Error restoreContext(const BackendContext &Context);
  llvm::Error installHooks(BackendHooks Hooks);
  /// A normally stopped CPU can continue. A faulted CPU cannot resume: Unicorn
  /// does not guarantee its internal state after an unhandled execution error.
  llvm::Error run(uint64_t PC, uint64_t TimeoutMicroseconds);
  bool timedOut() const;
  void stop();
  bool hasMemoryFault() const;
  /// A known MMIO transaction rejection, distinct from callback exceptions.
  bool hasDeviceError() const;
  /// The first CPU or checked GuestMemory fault survives later observations.
  std::optional<BackendFault> fault() const;
  bool executable(uint64_t Address) const;

private:
  struct Impl;
  explicit UnicornBackend(std::unique_ptr<Impl> State);
  std::unique_ptr<Impl> State;
};
} // namespace neverd::emulation
#endif
