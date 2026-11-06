//===- X86DebugRules.h - Instruction debug status -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared nontransactional instruction debug controls, matching and status.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_X86_X86DEBUGRULES_H
#define NEVERD_IR_X86_X86DEBUGRULES_H

#include <array>
#include <cstdint>

namespace neverd {

/// Intel's fault-class RF rule and the clear-RF result observed through VMX
/// are separate bindings. Absence refuses an actual GD cause, not other MOVs.
enum class X86GeneralDetectFrame : uint8_t { FaultResume, ClearResume };

/// The comparator profiles own canonical instruction comparators (RW=LEN=0).
/// All profiles clear inactive B0..B3 on #DB. Single-step traps also report
/// enabled comparators at the next PC unless restored RF inhibits them.
/// These are explicit target choices, never inferred from the executing host.
enum class X86InstructionDebugProfile : uint8_t {
  InstructionStep,
  ExecuteBreakpoints,
  /// MOV-DR with preserved arithmetic flags, fixed DR6 BLD/RTM, and GD.
  /// Other debug extensions and data/I/O comparators remain unbound.
  DebugRegisters,
};

inline bool
x86InstructionDebugValid(uint64_t DR6, uint64_t DR7, uint64_t DebugCTL,
                         X86InstructionDebugProfile Profile =
                             X86InstructionDebugProfile::InstructionStep) {
  const bool Controls =
      Profile == X86InstructionDebugProfile::InstructionStep ? DR7 == 0x400
      : Profile == X86InstructionDebugProfile::ExecuteBreakpoints
          ? (DR7 & ~uint64_t{0x3ff}) == 0x400
          : Profile == X86InstructionDebugProfile::DebugRegisters &&
                (DR7 & ~uint64_t{0x23ff}) == 0x400 &&
                (DR6 & 0x10800) == 0x10800;
  return Controls && DebugCTL == 0 && (DR6 & 0xffffffff00000000ULL) == 0 &&
         (DR6 & 0xfffe07f0u) == 0xfffe07f0u && !(DR6 & 0x1000);
}

/// Address is the original linear instruction start, before paging. Width
/// normalization belongs to the architecture's address owner. RF and the
/// incoming MOV/POP-SS window suppress the fault, without postponing a cause.
template <typename Address>
inline uint8_t x86ExecutionBreakpoints(const std::array<Address, 4> &DR,
                                       uint64_t DR7, uint64_t LinearAddress,
                                       uint64_t Flags, bool SSBlocked) {
  if ((Flags & 0x10000) || SSBlocked)
    return 0;
  uint8_t Matches = 0;
  for (unsigned I = 0; I != DR.size(); ++I)
    if ((DR7 & (uint64_t{3} << (I * 2))) && DR[I] == LinearAddress)
      Matches |= 1u << I;
  return Matches;
}

/// Inactive B0..B3 clear, RTM becomes one, sticky status survives. ICEBP does
/// not set BS merely because entry TF was set. Historical BLD/RTM inputs
/// require a target capable of representing those statuses.
inline uint64_t x86InstructionDebugStatus(uint64_t DR6, bool SingleStep,
                                          uint8_t Breakpoints = 0) {
  return (DR6 & ~uint64_t{0xf}) | 0x10000 | (SingleStep ? 0x4000 : 0) |
         Breakpoints;
}

} // namespace neverd
#endif // NEVERD_IR_X86_X86DEBUGRULES_H
