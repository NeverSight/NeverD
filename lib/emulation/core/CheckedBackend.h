//===- CheckedBackend.h - Shared checked execution lifecycle -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CHECKEDBACKEND_H
#define NEVERD_EMULATION_CHECKEDBACKEND_H
#include "MemoryProjection.h"

#include "neverd/emulation/CPU.h"

#include <atomic>
#include <capstone/capstone.h>
#include <chrono>
#include <memory>
#include <vector>
namespace neverd::emulation {
/// Shared pre-effect fault, observer, memory and bounded execution semantics.
/// ISA admission and architectural state belong to the derived architecture.
class CheckedBackend : public ExecutionBackend {
public:
  ~CheckedBackend() override;
  std::shared_ptr<AddressSpace> addressSpace() const override {
    return Memory->addressSpace();
  }
  llvm::Error bindAddressSpace(std::shared_ptr<AddressSpace> Space) override;
  llvm::Error map(uint64_t, uint64_t, unsigned) override;
  llvm::Error mapAlias(uint64_t, uint64_t, uint64_t, unsigned) override;
  llvm::Error unmapAlias(uint64_t, uint64_t) override;
  llvm::Error replaceAliases(llvm::ArrayRef<GuestAliasRange>,
                             llvm::ArrayRef<GuestAliasMapping>) override;
  llvm::Error protect(uint64_t, uint64_t, unsigned) override;
  llvm::Error mapMMIO(uint64_t, uint64_t, GuestMMIOCallbacks) override;
  llvm::Error unmapMMIO(uint64_t, uint64_t) override;
  llvm::Error read(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error write(uint64_t, llvm::ArrayRef<uint8_t>) override;
  llvm::Error fetch(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Expected<bool> canAccess(uint64_t, uint64_t, unsigned) const override;
  llvm::Error validateBacking(uint64_t, uint64_t) const override;
  llvm::Error readBacking(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error writeBacking(uint64_t, llvm::ArrayRef<uint8_t>) override;
  llvm::Error snapshotBacking(uint64_t,
                              llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error installHooks(BackendHooks) override;
  llvm::Error
  setMemoryWriteWatches(const std::vector<MemoryWriteWatch> &Watches) override;
  llvm::Expected<uint32_t> instructionSize(uint64_t Address) override;
  void setExecutionWatches(const std::vector<ExecutionWatch> &W) override {
    // A new epoch forces the direct page tables to be rebuilt with this set
    // marked non-executable; the checked path reads the set per instruction.
    ExecutionWatches = W;
    ++WatchEpoch;
  }
  llvm::Expected<ExecutionExit> runUntilExit(uint64_t, uint64_t) override;
  void stop() override { StopRequested.store(true); }
  bool timedOut() const override { return TimedOut; }
  bool hasMemoryFault() const override {
    return FirstFault && FirstFault->Access.has_value();
  }
  bool hasDeviceError() const override { return DeviceFailed; }
  bool executable(uint64_t Address) const override {
    auto Result = Memory->addressSpace()->canAccess(
        Address, 1, executionPermissions(Execute));
    if (!Result) {
      llvm::consumeError(Result.takeError());
      return false;
    }
    return *Result;
  }
  std::optional<BackendFault> fault() const override { return FirstFault; }
  std::optional<BackendFault> takeRecoverableFault() override;
  bool supportsResourcePause() const override { return !Direct; }
  std::optional<ResourcePause> pendingResourcePause() const override {
    return ResourcePending;
  }
  llvm::Error acknowledgeResourcePause() override;
  std::optional<ServiceRequest> pendingServiceRequest() const override {
    return PendingService;
  }
  std::optional<ServiceRequest> takeServiceRequest() override;

protected:
  CheckedBackend(unsigned MaxInstructionBytes, unsigned InstructionAlignment,
                 bool UserMode, bool Direct = false);
  llvm::Error initializeDecoder(cs_arch, cs_mode);
  llvm::Expected<bool> decodeInstruction(uint64_t PC);
  const cs_insn &decodedInstruction() const { return *Decoded; }
  virtual bool canonicalRange(uint64_t, uint64_t) const = 0;
  virtual bool supportsDeviceMappings() const { return false; }
  virtual uint64_t programCounter() const = 0;
  virtual void setProgramCounter(uint64_t PC) = 0;
  virtual llvm::Error execute(const cs_insn &) = 0;
  virtual llvm::Error
  withExecutionOwner(llvm::function_ref<llvm::Error()> Loop) {
    return Loop();
  }
  virtual void onGuestException() {}
  /// A direct contract admits nothing. The transport runs guest code until
  /// the processor raises an event, which this call publishes as a pending
  /// service request or a fault before it returns.
  virtual llvm::Error executeDirect();
  virtual std::optional<ServiceRequest>
  decodeServiceRequest(const cs_insn &) const = 0;
  /// Context capture may occur in an instruction observer, but no mutation or
  /// snapshot may discard a terminal or unconsumed execution outcome.
  llvm::Error checkExecutionState(bool AllowResourceContinuation = false) const;
  llvm::Error mutableMemory() const;
  llvm::Error access(uint64_t, uint64_t, unsigned, bool Recoverable = false,
                     bool Guest = false);
  llvm::Error raiseFault(BackendFault, bool Recoverable);
  /// Whether \p Address lies in a current execution watch. A direct run uses
  /// this to tell a watched-page fetch fault from a real one.
  bool executionWatched(uint64_t Address) const;
  unsigned executionPermissions(unsigned P) const {
    return P | (UserMode ? UserAccessible : 0);
  }
  const bool UserMode;
  const bool Direct;
  std::unique_ptr<MemoryProjection> Memory;
  BackendHooks Hooks;
  csh Decoder = 0;
  std::shared_ptr<const void> Identity = std::make_shared<unsigned char>(0);
  std::optional<BackendFault> FirstFault, RecoverableFault;
  std::optional<ResourcePause> ResourcePending;
  std::optional<uint64_t> ResourceResumePC;
  std::optional<ServiceRequest> PendingService;
  bool Running = false, TimedOut = false;
  bool DeviceFailed = false;
  std::atomic<bool> StopRequested{false};
  std::chrono::steady_clock::time_point Deadline;
  // Watches a direct run marks non-executable, and a counter that changes
  // whenever the set does, so the page-table projection is rebuilt for it.
  std::vector<ExecutionWatch> ExecutionWatches;
  uint64_t WatchEpoch = 0;

private:
  struct InstructionCache;
  std::unique_ptr<InstructionCache> CachedInstructions;
  // Address and device admission are fixed by this backend's contract. Only
  // a successful check on the same live space and mapping epoch is reusable.
  std::weak_ptr<AddressSpace> ValidatedMappingSpace;
  uint64_t ValidatedMappingGeneration = 0;
  llvm::Error runImpl(uint64_t PC, bool &Started, bool &BackendFailed);
  // Bytes and permissions are refreshed on every step; cached detail owns
  // its complete instruction and is reused only after matching those bytes.
  // The execution lease rejects recursive entry before this storage is touched.
  std::vector<uint8_t> InstructionBytes;
  cs_insn *Decoded = nullptr;
  unsigned InstructionAlignment;
  std::atomic_flag Entered = ATOMIC_FLAG_INIT;
};
} // namespace neverd::emulation
#endif
