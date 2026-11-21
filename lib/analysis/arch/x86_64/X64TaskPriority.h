//===- X64TaskPriority.h - Explicit task-priority transitions ------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Pure scalar lowering of retained CPL0 task-priority reads and writes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_X64_TASK_PRIORITY_H
#define NEVERD_ANALYSIS_X64_TASK_PRIORITY_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <optional>
#include <vector>

namespace neverd::analysis::detail {

enum class X64TaskPriorityOperation { Read, Write };

struct X64TaskPriorityAccess {
  X64TaskPriorityOperation Operation;
  uint8_t GPR;
  bool Locked;
};

inline std::optional<X64TaskPriorityAccess>
decodeX64TaskPriority(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < 4 || Bytes[Bytes.size() - 3] != 0x0f)
    return std::nullopt;
  const auto Opcode = Bytes[Bytes.size() - 2], ModRM = Bytes.back();
  if ((Opcode != 0x20 && Opcode != 0x22) || (ModRM & 0x38))
    return std::nullopt;
  uint8_t Rex = 0;
  bool Locked = false;
  for (auto Byte : Bytes.drop_back(3)) {
    if ((Byte & 0xf0) == 0x40) {
      Rex = Byte;
      continue;
    }
    Rex = 0;
    switch (Byte) {
    case 0xf0:
      Locked = true;
      break;
    case 0xf2:
    case 0xf3:
      Locked = false;
      break;
    case 0x26:
    case 0x2e:
    case 0x36:
    case 0x3e:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
      break;
    default:
      return std::nullopt;
    }
  }
  if (!(Rex & 4))
    return std::nullopt;
  return X64TaskPriorityAccess{Opcode == 0x22 ? X64TaskPriorityOperation::Write
                                              : X64TaskPriorityOperation::Read,
                               uint8_t((ModRM & 7) | ((Rex & 1) << 3)), Locked};
}

inline std::optional<X64TaskPriorityOperation>
taskPriorityOperation(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default || !Op.NumInputs ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2)
    return std::nullopt;
  const auto Scalar = [](NdVar V) {
    return V.Size == 8 && (V.isConst() || ((V.isReg() || V.isTemp()) &&
                                           V.Offset <= InvalidVA - 7));
  };
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::ReadCr8) &&
      Op.NumInputs == 1 && !Op.Output.isConst() && Scalar(Op.Output))
    return X64TaskPriorityOperation::Read;
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::WriteCr8) &&
      Op.NumInputs == 2 && !Op.Output.Size && Scalar(Op.Inputs[1]))
    return X64TaskPriorityOperation::Write;
  return std::nullopt;
}

struct X64TaskPriorityTransition {
  std::vector<LowOp> Ops;
  /// A byte Boolean: a nonzero value denotes a reserved-bit write, not a
  /// truncated valid value. The prior state is preserved in that case.
  std::optional<NdVar> Rejected;
};

/// Intel SDM, MOV to/from control registers and Vol.3A 12.8.6.1: CR8 is a
/// four-bit task-priority class; reserved-bit writes fault. The caller supplies
/// CPL0, a bounded entry state, no asynchronous updates and ordinary memory
/// without APIC/TPR aliases. Only modeled writes may change this state.
/// State and allocated temporaries must be disjoint from guest operands and
/// from each other.
/// Consumers must prove Rejected is zero or report a failed execution; this
/// scalar transition alone supplies no privilege or nonfaulting certificate.
inline std::optional<X64TaskPriorityTransition>
lowerX64TaskPriority(const LowOp &Op, NdVar State,
                     llvm::function_ref<NdVar(uint16_t)> Temporary) {
  const auto Kind = taskPriorityOperation(Op);
  if (!Kind || (!State.isReg() && !State.isTemp()) || State.Size != 8 ||
      State.Offset > InvalidVA - 7)
    return std::nullopt;
  X64TaskPriorityTransition Result;
  const auto Emit = [&](NdOp Code, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
    LowOp Scalar;
    Scalar.Opcode = Code;
    Scalar.Output = Output;
    Scalar.Addr = Op.Addr;
    Scalar.Seq = static_cast<int>(Result.Ops.size());
    for (auto Input : Inputs)
      Scalar.addInput(Input);
    Result.Ops.push_back(Scalar);
  };
  if (*Kind == X64TaskPriorityOperation::Read) {
    Emit(NdOp::COPY, Op.Output, {State});
  } else {
    const auto Bad = Temporary(1);
    Emit(NdOp::INT_LESS, Bad, {NdVar::scalar(15, 8), Op.Inputs[1]});
    Emit(NdOp::SELECT, State, {Bad, State, Op.Inputs[1]});
    Result.Rejected = Bad;
  }
  return Result;
}

} // namespace neverd::analysis::detail

#endif
