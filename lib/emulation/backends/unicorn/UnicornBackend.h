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
#include "neverd/emulation/CPU.h"

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
  create(uint64_t MemoryLimit,
         GuestArchitecture Architecture = GuestArchitecture::X64);
  static llvm::Expected<std::unique_ptr<UnicornBackend>>
  create(std::shared_ptr<AddressSpace> Space,
         GuestArchitecture Architecture = GuestArchitecture::X64);
  ~UnicornBackend() override;
  std::shared_ptr<AddressSpace> addressSpace() const override;
  llvm::Error bindAddressSpace(std::shared_ptr<AddressSpace> Space) override;
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
  GuestArchitecture architecture() const override;
  std::optional<X64BranchModel> x64BranchModel() const override {
    if (architecture() == GuestArchitecture::X64)
      return X64BranchModel::Intel;
    return std::nullopt;
  }
  llvm::Expected<RegisterValue>
      supportedControlBits(CPURegister) const override;
  llvm::Expected<RegisterValue> readRegister(CPURegister Register) override;
  llvm::Error writeRegister(CPURegister Register,
                            const RegisterValue &Value) override;
  /// Capture the complete Unicorn CPU state, including SIMD and FPU registers.
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override;
  /// Replace an existing snapshot with this backend's current CPU state.
  llvm::Error saveContext(BackendContext &Context) override;
  /// Restore between run() calls. A snapshot cannot recover a faulted CPU.
  llvm::Error restoreContext(const BackendContext &Context) override;
  llvm::Error installHooks(BackendHooks Hooks) override;
  llvm::Error
  setMemoryWriteWatches(const std::vector<MemoryWriteWatch> &Watches) override;
  llvm::Expected<uint32_t> instructionSize(uint64_t Address) override;
  /// A normally stopped CPU can continue. A faulted CPU cannot resume: Unicorn
  /// does not guarantee its internal state after an unhandled execution error.
  llvm::Expected<ExecutionExit>
  runUntilExit(uint64_t PC, uint64_t TimeoutMicroseconds) override;
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
  llvm::Error runImpl(uint64_t PC, uint64_t Timeout, bool &Started);
  struct Impl;
  explicit UnicornBackend(std::unique_ptr<Impl> State);
  std::unique_ptr<Impl> State;
};
} // namespace neverd::emulation
#endif
