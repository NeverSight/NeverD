//===- X64MachineFlags.h - Shared packed flags semantics -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_X64_MACHINE_FLAGS_H
#define NEVERD_ANALYSIS_X64_MACHINE_FLAGS_H

#include "neverd/analysis/InterpreterMachineStateProfile.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <optional>
#include <utility>

namespace neverd::analysis::detail {

/// Explicit native flags profiles. Intel SDM 253667-093, Table 4-20 and 64-bit
/// POPFQ pseudocode (4-408/409), PUSHFQ (4-528/529). RF is cleared; TF/AC
/// are architecturally writable but deliberately excluded by V1 profiles.
/// CPL=0 also writes IF/IOPL. KernelX64NoFaultV1 explicitly requires IOPL=0
/// throughout the admitted execution, so a nonzero popped IOPL is refused.
struct X64MachineFlags {
  static constexpr uint32_t SemanticsVersion = 1;
  static constexpr uint64_t SplitMask = 0xcd5;
  static constexpr uint64_t UserSystemWriteMask =
      (uint64_t{1} << 14) | (uint64_t{1} << 21);
  static constexpr uint64_t IOPLMask = uint64_t{3} << 12;
  static constexpr uint64_t KernelSystemWriteMask =
      UserSystemWriteMask | (uint64_t{1} << 9) | IOPLMask;
  static constexpr uint64_t UserRejectedWriteMask =
      (uint64_t{1} << 8) | (uint64_t{1} << 18);
  static constexpr uint64_t KernelRejectedWriteMask =
      UserRejectedWriteMask | IOPLMask;
  static constexpr uint64_t ResumeMask = uint64_t{1} << 16;
  static constexpr uint64_t AlignmentCheckMask = uint64_t{1} << 18;
  static constexpr uint64_t SnapshotClearMask =
      ResumeMask | (uint64_t{1} << 17);
  static constexpr uint64_t EntryMask =
      SplitMask | UserSystemWriteMask | (uint64_t{1} << 9) | 2;
  static constexpr std::pair<uint64_t, unsigned> Flags[] = {
      {x86reg::CF, 0}, {x86reg::PF, 2},  {x86reg::AF, 4}, {x86reg::ZF, 6},
      {x86reg::SF, 7}, {x86reg::DF, 10}, {x86reg::OF, 11}};

  static bool validEntry(uint64_t Image) {
    return (Image & ~EntryMask) == 0 && (Image & 2) != 0;
  }

  static bool supportsProfile(InterpreterMachineStateProfile Profile) {
    return Profile == InterpreterMachineStateProfile::UserX64NoFaultV1 ||
           isKernelX64MachineStateProfile(Profile);
  }

  static constexpr uint64_t entryMask(InterpreterMachineStateProfile Profile) {
    return EntryMask |
           (Profile == InterpreterMachineStateProfile::KernelX64SmapNoFaultV2
                ? AlignmentCheckMask
                : 0);
  }
};

/// Intel SDM 325383-084US, CLI/STI: CPL=0 clears/sets IF and preserves
/// other flags. The kernel profile excludes asynchronous/debug delivery and
/// privilege transitions, including observation of STI's interrupt inhibition.
inline std::optional<bool>
x64InterruptFlagValue(const LowOp &Op, InterpreterMachineStateProfile Profile) {
  if (!isKernelX64MachineStateProfile(Profile) ||
      Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 1 || Op.Output.Size ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2)
    return std::nullopt;
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Cli))
    return false;
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Sti))
    return true;
  return std::nullopt;
}

/// Intel SDM 253666-089, CLAC and 253667-080, STAC: at CPL=0 with SMAP
/// available, only AC changes. This admission is separate from the V1
/// domains, which require AC=0 throughout their execution.
inline std::optional<bool>
x64AccessCheckFlagValue(const LowOp &Op,
                        InterpreterMachineStateProfile Profile) {
  if (Profile != InterpreterMachineStateProfile::KernelX64SmapNoFaultV2 ||
      Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 1 || Op.Output.Size ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2)
    return std::nullopt;
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Clac))
    return false;
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Stac))
    return true;
  return std::nullopt;
}

struct X64MachineFlagsTransition {
  std::vector<LowOp> Ops;
  /// A canonical byte Boolean. Consumers must either record sticky failure
  /// (source ABI) or prove both executions cannot set it (native proof).
  std::optional<NdVar> Rejected;
};

/// A pure lowering shared by source generation and native proof. It owns only
/// the system portion; original lifter ops still pack/scatter scalar flags and
/// perform the actual stack accesses. Temporary allocation must be disjoint
/// from all supplied values. No host flags or path assumption enters this code.
inline std::optional<X64MachineFlagsTransition>
lowerX64MachineFlags(const LowOp &IntrinsicOp, NdVar System,
                     llvm::function_ref<NdVar(uint16_t)> Temporary,
                     InterpreterMachineStateProfile Profile =
                         InterpreterMachineStateProfile::UserX64NoFaultV1) {
  if (!X64MachineFlags::supportsProfile(Profile) ||
      IntrinsicOp.Opcode != NdOp::INTRINSIC ||
      IntrinsicOp.MemoryOrdering != NdMemoryOrdering::None ||
      IntrinsicOp.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !IntrinsicOp.NumInputs || !IntrinsicOp.Inputs[0].isConst() ||
      IntrinsicOp.Inputs[0].Size != 2)
    return std::nullopt;
  const auto Kind = IntrinsicOp.Inputs[0].Offset;
  const bool Push = Kind == static_cast<uint64_t>(Intrinsic::Pushf);
  const auto Interrupt = x64InterruptFlagValue(IntrinsicOp, Profile);
  const auto AccessCheck = x64AccessCheckFlagValue(IntrinsicOp, Profile);
  if (!Interrupt && !AccessCheck &&
      (Push ? (IntrinsicOp.NumInputs != 1 || !IntrinsicOp.Output.isTemp() ||
               IntrinsicOp.Output.Size != 8)
            : (Kind != static_cast<uint64_t>(Intrinsic::Popf) ||
               IntrinsicOp.NumInputs != 2 || IntrinsicOp.Output.Size ||
               IntrinsicOp.Inputs[1].Size != 8)))
    return std::nullopt;
  X64MachineFlagsTransition Result;
  const auto Emit = [&](NdOp Code, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Code;
    Op.Output = Output;
    Op.Addr = IntrinsicOp.Addr;
    Op.Seq = static_cast<int>(Result.Ops.size());
    for (NdVar Input : Inputs)
      Op.addInput(Input);
    Result.Ops.push_back(Op);
  };
  const auto Constant = [](uint64_t Value) { return NdVar::scalar(Value, 8); };
  if (Interrupt) {
    constexpr uint64_t IF = uint64_t{1} << 9;
    Emit(*Interrupt ? NdOp::INT_OR : NdOp::INT_AND, System,
         {System, Constant(*Interrupt ? IF : ~IF)});
  } else if (AccessCheck) {
    const uint64_t AC = X64MachineFlags::AlignmentCheckMask;
    Emit(*AccessCheck ? NdOp::INT_OR : NdOp::INT_AND, System,
         {System, Constant(*AccessCheck ? AC : ~AC)});
  } else if (Push) {
    Emit(NdOp::INT_AND, IntrinsicOp.Output,
         {System, Constant(~X64MachineFlags::SnapshotClearMask)});
  } else {
    const NdVar Image = IntrinsicOp.Inputs[1];
    const bool Kernel = isKernelX64MachineStateProfile(Profile);
    const bool Smap =
        Profile == InterpreterMachineStateProfile::KernelX64SmapNoFaultV2;
    const uint64_t AC = Smap ? X64MachineFlags::AlignmentCheckMask : 0;
    const uint64_t WriteMask = (Kernel ? X64MachineFlags::KernelSystemWriteMask
                                       : X64MachineFlags::UserSystemWriteMask) |
                               AC;
    const uint64_t RejectMask =
        (Kernel ? X64MachineFlags::KernelRejectedWriteMask
                : X64MachineFlags::UserRejectedWriteMask) &
        ~AC;
    const NdVar BadBits = Temporary(8), Bad = Temporary(1);
    Emit(NdOp::INT_AND, BadBits, {Image, Constant(RejectMask)});
    Emit(NdOp::INT_NOTEQUAL, Bad, {BadBits, Constant(0)});
    Result.Rejected = Bad;
    const NdVar Kept = Temporary(8), Changed = Temporary(8);
    Emit(NdOp::INT_AND, Kept,
         {System, Constant(~(WriteMask | X64MachineFlags::ResumeMask))});
    Emit(NdOp::INT_AND, Changed, {Image, Constant(WriteMask)});
    Emit(NdOp::INT_OR, System, {Kept, Changed});
  }
  return Result;
}

} // namespace neverd::analysis::detail

#endif
