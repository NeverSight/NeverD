//===- X64Machine.h - Host-independent checked machine state -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_X64MACHINE_H
#define NEVERD_EMULATION_ARCH_X64MACHINE_H
#include "../../core/ExecutionBackend.h"
#include "../../core/MemoryLayout.h"

namespace neverd::emulation {
namespace x64 {
#define NEVERD_X64_MACHINE_VALUE(Name, Value)                                  \
  inline constexpr uint64_t Name = Value;
#include "X64Machine.def"
#undef NEVERD_X64_MACHINE_VALUE
inline bool canonical(uint64_t Address) {
  return Address <= UserMax || Address >= KernelMin;
}
} // namespace x64
class PhysicalMemory;
llvm::Expected<uint64_t> buildX64PageTables(PhysicalMemory &Memory,
                                            uint64_t PreviousRoot);
struct X64MachineState {
  std::array<uint64_t, unsigned(X64Register::SS) + 1> Registers{};
  uint64_t GSBase = 0;
  std::array<ExecutionBackend::XmmValue, x64::XmmCount> Xmm{};
  uint64_t &reg(X64Register R) { return Registers[unsigned(R)]; }
  uint64_t reg(X64Register R) const { return Registers[unsigned(R)]; }
};
/// Native execution of one already admitted integer instruction. No OS models,
/// instruction decoding, memory ownership or lifecycle decisions belong here.
/// The v1 contract excludes x87/SIMD instructions, privileged instructions,
/// debug/flag manipulation and any instruction with unbounded execution.
class X64Machine {
public:
  virtual ~X64Machine() = default;
  virtual llvm::Error step(X64MachineState &State, uint64_t PageTableRoot) = 0;
};
llvm::Expected<std::unique_ptr<X64Machine>> createKvmMachine(uint8_t *Backing,
                                                             uint64_t Size);
llvm::Expected<std::unique_ptr<X64Machine>> createWhpMachine(uint8_t *Backing,
                                                             uint64_t Size);
} // namespace neverd::emulation
#endif
