//===- CheckedAArch64Backend.h - Checked ARM64 execution ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CHECKEDAARCH64BACKEND_H
#define NEVERD_EMULATION_CHECKEDAARCH64BACKEND_H
#include "../../core/CheckedBackend.h"
#include "AArch64Machine.h"
namespace neverd::emulation {
class CheckedAArch64Backend final : public CheckedBackend {
public:
  static llvm::Expected<std::unique_ptr<ExecutionBackend>>
  create(std::unique_ptr<MemoryProjection> Memory,
         std::unique_ptr<AArch64Machine> Machine, bool UserMode = false);
  GuestArchitecture architecture() const override {
    return GuestArchitecture::AArch64;
  }
  llvm::Expected<RegisterValue> readRegister(CPURegister) override;
  llvm::Error writeRegister(CPURegister, const RegisterValue &) override;
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override;
  llvm::Error saveContext(BackendContext &) override;
  llvm::Error restoreContext(const BackendContext &) override;
  llvm::Error bindAddressSpace(std::shared_ptr<AddressSpace>) override;

private:
  llvm::Expected<RegisterValue> readContextRegister(const BackendContext &,
                                                    CPURegister) const override;
  CheckedAArch64Backend(bool UserMode)
      : CheckedBackend(aarch64::InstructionBytes, aarch64::InstructionBytes,
                       UserMode) {}
  struct SavedState : BackendContext::Storage {
    AArch64MachineState CPU;
    std::shared_ptr<RAMReservation> Exclusive;
  };
  bool canonicalRange(uint64_t, uint64_t) const override;
  bool supportsDeviceMappings() const override { return !UserMode; }
  uint64_t programCounter() const override {
    return CPU.reg(AArch64Register::PC);
  }
  void setProgramCounter(uint64_t PC) override {
    CPU.reg(AArch64Register::PC) = PC;
  }
  llvm::Error execute(const cs_insn &) override;
  llvm::Expected<bool> executeAtomic(const cs_insn &);
  void onGuestException() override { Exclusive.reset(); }
  std::optional<ServiceRequest>
  decodeServiceRequest(const cs_insn &) const override;
  AArch64MachineState CPU;
  std::shared_ptr<RAMReservation> Exclusive;
  std::unique_ptr<AArch64Machine> Machine;
};
} // namespace neverd::emulation
#endif
