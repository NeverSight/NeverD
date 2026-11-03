//===- X86LegacySegmentsEmulator.cpp - Bound native operands --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Apply one explicit protected segment cache to one concrete native operand.
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/X86/X86NativeMemory.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd {

bool NdOpEmulator::stepX86LegacySegment(
    const LowOp &Op, const X86LegacySegmentCache &Cache,
    std::optional<X86FullSegmentLimitChoice> Choice) {
  if (Img.Arch != Arch::X86 || Op.Opcode != NdOp::INTRINSIC ||
      Op.NumInputs < 2 || Op.NumInputs > kLowOperationInputs ||
      !Op.Inputs[0].isConst() || !Op.Inputs[Op.NumInputs - 1].isConst() ||
      !x86NativeMemoryHasSegmentOffset(
          static_cast<Intrinsic>(Op.Inputs[0].Offset), Op.NumInputs,
          Op.Inputs[Op.NumInputs - 1].Offset))
    return false;
  X86OperandSegment = &Cache;
  X86OperandSegmentChoice = Choice;
  llvm::scope_exit Clear([&] {
    X86OperandSegment = nullptr;
    X86OperandSegmentChoice.reset();
  });
  return step(Op);
}

std::optional<uint64_t>
NdOpEmulator::resolveX86NativeMemoryAddress(const LowOp &Op, unsigned Prefix,
                                            uint64_t Offset, uint16_t Bytes,
                                            bool Write) const {
  if (!Prefix)
    return resolveMemoryAddress(Op, Offset);
  if (!X86OperandSegment || Offset > UINT32_MAX)
    return std::nullopt;
  // Instruction shape validation has already admitted only these prefixes.
  const auto Segment = Prefix == 0x26   ? X86SegmentRegister::ES
                       : Prefix == 0x2e ? X86SegmentRegister::CS
                                        : X86SegmentRegister::SS;
  auto Result = checkX86LegacySegmentAccess(
      *X86OperandSegment,
      {Segment,
       Write ? X86SegmentAccessKind::Write : X86SegmentAccessKind::Read,
       uint32_t(Offset), Bytes, X86OperandSegmentChoice});
  if (!Result) {
    llvm::consumeError(Result.takeError());
    return std::nullopt;
  }
  return Result->FaultVector ? std::nullopt
                             : std::optional<uint64_t>(Result->Linear);
}

} // namespace neverd
