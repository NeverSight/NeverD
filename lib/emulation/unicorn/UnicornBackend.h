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
#include "../core/ExecutionBackend.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
namespace neverd::emulation {
/// Executes against the GuestMemory virtual address space, without an MMU
/// model.
class UnicornBackend final : public ExecutionBackend {
public:
  static llvm::Expected<std::unique_ptr<UnicornBackend>>
  create(uint64_t MemoryLimit);
  ~UnicornBackend() override;
  llvm::Error map(uint64_t Address, uint64_t Size,
                  unsigned Permissions) override;
  llvm::Error mapAlias(uint64_t Address, uint64_t Source, uint64_t Size,
                       unsigned Permissions) override;
  llvm::Error unmapAlias(uint64_t Address, uint64_t Size) override;
  llvm::Error replaceAliases(llvm::ArrayRef<GuestAliasRange> Remove,
                             llvm::ArrayRef<GuestAliasMapping> Add) override;
  llvm::Error protect(uint64_t Address, uint64_t Size,
                      unsigned Permissions) override;
  llvm::Error mapMMIO(uint64_t Address, uint64_t Size,
                      GuestMMIOCallbacks Callbacks) override;
  llvm::Error unmapMMIO(uint64_t Address, uint64_t Size) override;
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) override;
  llvm::Error write(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes) override;
  llvm::Error validateBacking(uint64_t Address, uint64_t Size) const override;
  llvm::Expected<bool> canAccess(uint64_t Address, uint64_t Size,
                                 unsigned Permissions) const override;
  llvm::Error readBacking(uint64_t Address,
                          llvm::MutableArrayRef<uint8_t> Bytes) override;
  llvm::Error writeBacking(uint64_t Address,
                           llvm::ArrayRef<uint8_t> Bytes) override;
  llvm::Error snapshotBacking(uint64_t Address,
                              llvm::MutableArrayRef<uint8_t> Bytes) override;
  llvm::Error fetch(uint64_t Address,
                    llvm::MutableArrayRef<uint8_t> Bytes) override;
  llvm::Expected<uint64_t> reg(X64Register Register) override;
  llvm::Error setReg(X64Register Register, uint64_t Value) override;
  /// Set the model-owned x64 processor environment base.
  llvm::Error setGSBase(uint64_t Address) override;
  using XmmValue = std::array<uint64_t, 2>;
  llvm::Expected<XmmValue> xmm(unsigned Register) override;
  llvm::Error setXmm(unsigned Register, const XmmValue &Value) override;
  /// Capture the complete Unicorn CPU state, including SIMD and FPU registers.
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override;
  /// Replace an existing snapshot with this backend's current CPU state.
  llvm::Error saveContext(BackendContext &Context) override;
  /// Restore between run() calls. A snapshot cannot recover a faulted CPU.
  llvm::Error restoreContext(const BackendContext &Context) override;
  llvm::Error installHooks(BackendHooks Hooks) override;
  /// A normally stopped CPU can continue. A faulted CPU cannot resume: Unicorn
  /// does not guarantee its internal state after an unhandled execution error.
  llvm::Error run(uint64_t PC, uint64_t TimeoutMicroseconds) override;
  bool timedOut() const override;
  void stop() override;
  bool hasMemoryFault() const override;
  /// A known MMIO transaction rejection, distinct from callback exceptions.
  bool hasDeviceError() const override;
  /// The first CPU or checked GuestMemory fault survives later observations.
  std::optional<BackendFault> fault() const override;
  std::optional<BackendFault> takeRecoverableFault() override;
  bool executable(uint64_t Address) const override;

private:
  struct Impl;
  explicit UnicornBackend(std::unique_ptr<Impl> State);
  std::unique_ptr<Impl> State;
};
} // namespace neverd::emulation
#endif
