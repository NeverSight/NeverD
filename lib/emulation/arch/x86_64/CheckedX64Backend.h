//===- CheckedX64Backend.h - Checked x64 execution-----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_CHECKEDX64BACKEND_H
#define NEVERD_EMULATION_ARCH_CHECKEDX64BACKEND_H
#include "../../core/PhysicalMemory.h"
#include "X64Machine.h"

#include "neverd/emulation/ExecutionBackend.h"

#include <atomic>
#include <capstone/capstone.h>

namespace neverd::emulation {
class CheckedX64Backend final : public ExecutionBackend {
public:
  static llvm::Expected<std::unique_ptr<ExecutionBackend>>
  create(ExecutionBackendKind Kind, uint64_t Limit);
  ~CheckedX64Backend() override;
  llvm::Error map(uint64_t, uint64_t, unsigned) override;
  llvm::Error mapAlias(uint64_t, uint64_t, uint64_t, unsigned) override;
  llvm::Error unmapAlias(uint64_t, uint64_t) override;
  llvm::Error replaceAliases(llvm::ArrayRef<GuestAliasRange>,
                             llvm::ArrayRef<GuestAliasMapping>) override;
  llvm::Error protect(uint64_t, uint64_t, unsigned) override;
  llvm::Error read(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error write(uint64_t, llvm::ArrayRef<uint8_t>) override;
  llvm::Error fetch(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Expected<bool> canAccess(uint64_t, uint64_t, unsigned) const override;
  llvm::Error validateBacking(uint64_t, uint64_t) const override;
  llvm::Error readBacking(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error writeBacking(uint64_t, llvm::ArrayRef<uint8_t>) override;
  llvm::Error snapshotBacking(uint64_t,
                              llvm::MutableArrayRef<uint8_t>) override;
  llvm::Expected<uint64_t> reg(X64Register) override;
  llvm::Error setReg(X64Register, uint64_t) override;
  llvm::Error setGSBase(uint64_t) override;
  llvm::Expected<XmmValue> xmm(unsigned) override;
  llvm::Error setXmm(unsigned, const XmmValue &) override;
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override;
  llvm::Error saveContext(BackendContext &) override;
  llvm::Error restoreContext(const BackendContext &) override;
  llvm::Error installHooks(BackendHooks) override;
  llvm::Error run(uint64_t, uint64_t) override;
  void stop() override { StopRequested.store(true); }
  bool timedOut() const override { return TimedOut; }
  bool hasMemoryFault() const override {
    return FirstFault && FirstFault->Access.has_value();
  }
  bool hasDeviceError() const override { return false; }
  bool executable(uint64_t Address) const override {
    return !Memory->check(Address, 1, Execute);
  }
  std::optional<BackendFault> fault() const override { return FirstFault; }
  std::optional<BackendFault> takeRecoverableFault() override;

private:
  CheckedX64Backend() = default;
  struct SavedState : BackendContext::Storage {
    X64MachineState CPU;
  };
  llvm::Error mutableMemory() const;
  llvm::Error access(uint64_t, uint64_t, unsigned, bool Recoverable = false);
  llvm::Error execute(const cs_insn &Instruction);
  llvm::Expected<uint64_t> operandRegister(unsigned Register) const;
  std::unique_ptr<PhysicalMemory> Memory;
  std::unique_ptr<X64Machine> Machine;
  X64MachineState CPU;
  uint64_t PageTableRoot = 0;
  BackendHooks Hooks;
  csh Decoder = 0;
  std::shared_ptr<const void> Identity = std::make_shared<unsigned char>(0);
  std::optional<BackendFault> FirstFault, RecoverableFault;
  bool Running = false, TimedOut = false;
  std::atomic<bool> StopRequested{false};
};
} // namespace neverd::emulation
#endif
