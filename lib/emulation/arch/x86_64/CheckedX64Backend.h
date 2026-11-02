//===- CheckedX64Backend.h - Checked x64 execution-----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_CHECKEDX64BACKEND_H
#define NEVERD_EMULATION_ARCH_CHECKEDX64BACKEND_H
#include "../../core/CheckedBackend.h"
#include "X64Machine.h"
#include "X64PageTables.h"

#include "neverd/emulation/ExecutionBackend.h"

#include "llvm/ADT/BitVector.h"

#include <atomic>
#include <capstone/capstone.h>
#include <map>
#include <vector>

namespace neverd::emulation {
struct X64Exception;
class CheckedX64Backend final : public CheckedBackend {
public:
  static llvm::Expected<std::unique_ptr<ExecutionBackend>>
  create(std::unique_ptr<MemoryProjection> Memory,
         std::unique_ptr<X64Machine> Machine, bool UserMode = false,
         bool SIMDExceptions = false, bool Direct = false);
  GuestArchitecture architecture() const override {
    return GuestArchitecture::X64;
  }
  bool supportsSIMDExceptions() const override { return SIMDExceptions; }
  void setExecutionWatches(const std::vector<ExecutionWatch> &) override;
  std::optional<X64BranchModel> x64BranchModel() const override {
    return Machine->branchModel();
  }
  llvm::Expected<RegisterValue>
      supportedControlBits(CPURegister) const override;
  llvm::Expected<RegisterValue> readRegister(CPURegister) override;
  llvm::Error writeRegister(CPURegister, const RegisterValue &) override;
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override;
  llvm::Error saveContext(BackendContext &) override;
  llvm::Error restoreContext(const BackendContext &) override;

private:
  llvm::Expected<RegisterValue> readContextRegister(const BackendContext &,
                                                    CPURegister) const override;
  CheckedX64Backend(bool UserMode, bool SIMDExceptions, bool Direct)
      : CheckedBackend(x64::MaxInstructionBytes, 1, UserMode, Direct),
        SIMDExceptions(SIMDExceptions) {}
  bool permitsMXCSR(uint64_t Value) const {
    return supportsSIMDExceptions() ||
           (Value & x64::InitialMXCSR) == x64::InitialMXCSR;
  }
  bool canonicalRange(uint64_t, uint64_t) const override;
  bool supportsDeviceMappings() const override { return !UserMode && !Direct; }
  uint64_t programCounter() const override { return CPU.reg(X64Register::PC); }
  void setProgramCounter(uint64_t PC) override {
    CPU.reg(X64Register::PC) = PC;
    // A public entry resumes from the published architectural state. A REP
    // fault restores the flags at the start of this uninterrupted execution.
    StringRestart.reset();
    leaveWatchedRegion();
  }
  struct SavedState : BackendContext::Storage {
    X64MachineState CPU;
  };
  llvm::Error execute(const cs_insn &Instruction) override;
  llvm::Error
  withExecutionOwner(llvm::function_ref<llvm::Error()> Loop) override {
    return Machine->withExecutionOwner(Loop);
  }
  llvm::Error executeDirect() override;
  /// Retire one instruction through a temporary watch overlay, then rearm it.
  llvm::Error stepWatchedInstruction(bool WatchWrites = true,
                                     bool WatchInitialization = true);
  llvm::Error stepInitializationStore(uint64_t FaultAddress);
  llvm::Error stepDirectInstruction(uint64_t Root);
  /// Run decoded control-flow regions between private execution stops. A
  /// temporary write guard invalidates the plan before code can change.
  llvm::Error runWatchedRegion(std::optional<uint64_t> &OpenPage);
  llvm::Expected<std::vector<uint64_t>> regionStops(uint64_t PC);
  void leaveWatchedRegion();
  llvm::Error
  publishDirectException(const X64Exception &Raised, bool AllowSplit,
                         bool &Resume, bool WritesProtected = true,
                         std::optional<uint64_t> OpenPage = std::nullopt,
                         bool InitializationProtected = true);
  std::optional<uint64_t> RegionPage, RegionPhysical;
  X64PageTableCache WatchTables;
  std::vector<ExecutionWatch> FetchWatchPages;
  using RegionStopMap =
      std::map<uint64_t, std::shared_ptr<const std::vector<uint64_t>>>;
  RegionStopMap RegionStops;
  struct SavedRegion {
    std::array<uint8_t, x64::PageSize> Bytes;
    llvm::BitVector Decoded;
    RegionStopMap Stops;
  };
  std::array<uint8_t, x64::PageSize> RegionBytes;
  llvm::BitVector RegionDecoded{unsigned(x64::PageSize)};
  bool RegionDirty = false;
  std::map<uint64_t, SavedRegion> SavedRegions;
  std::optional<ServiceRequest>
  decodeServiceRequest(const cs_insn &) const override;
  llvm::Expected<uint64_t> operandRegister(unsigned Register) const;
  llvm::Expected<uint64_t> operandAddress(const cs_insn &, const cs_x86_op &,
                                          uint64_t Offset = 0) const;
  struct Access {
    uint64_t Address;
    unsigned Size, Permission;
    uint64_t Value;
    std::optional<unsigned> Update = std::nullopt;
    uint64_t High = 0;
    bool Deferred = false;
  };
  llvm::Error prepareStack(const cs_insn &, bool Push,
                           std::vector<Access> &) const;
  static unsigned implicitStackWidth(const cs_x86 &);
  llvm::Error executeEnter(const cs_insn &);
  void setOperandRegister(unsigned Register, uint64_t Value);
  std::shared_ptr<MemoryProjection::Device> deviceAt(uint64_t Address) const;
  llvm::Error validateDevice(const MemoryProjection::Device &, uint64_t,
                             unsigned, bool);
  llvm::Error deviceTransfer(const cs_insn &, uint64_t, unsigned, unsigned,
                             uint64_t);
  llvm::Error deviceAtomic(const cs_insn &, uint64_t, unsigned);
  enum class StringOperation { Move, Store, Load, Compare, Scan };
  struct StringRestartState {
    uint64_t PC, Flags;
  };
  std::optional<StringRestartState> StringRestart;
  llvm::Error executeString(const cs_insn &, unsigned Size, StringOperation);
  llvm::Error deviceResult(llvm::Error E);
  template <typename Function> auto deviceCallback(Function Call) {
    try {
      return Call();
    } catch (...) {
      DeviceFailed = true;
      throw;
    }
  }
  std::unique_ptr<X64Machine> Machine;
  X64MachineState CPU;
  const bool SIMDExceptions;
};
} // namespace neverd::emulation
#endif
