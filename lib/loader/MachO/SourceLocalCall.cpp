#include "SourceLocalCall.h"

#include "../SourceUnwind.h"
#include "MachOLocalFunction.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/Support/Endian.h"

namespace neverd {
bool sourceLeafCodeRange(const BinaryImage &Image, va_t Entry, uint32_t Size) {
  if (!Size ||
      Size > ::neverd::limits::loader::
                 kSourceLocalCallSourceLeafCodeRangeSizeLimit ||
      Entry > UINT64_MAX - Size || !readImmutableCodeBytes(Image, Entry, Size))
    return false;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc && Symbol.Addr > Entry && Symbol.Addr - Entry < Size)
      return false;
  for (const auto &Metadata : Image.ExceptionMetadata.Functions)
    if (((Metadata.CodeRange.Begin < Entry + Size &&
          Metadata.CodeRange.End > Entry) ||
         (Metadata.CodeRange.Begin >= Entry &&
          Metadata.CodeRange.Begin < Entry + Size)) &&
        (Metadata.CodeRange.End <= Metadata.CodeRange.Begin ||
         !isPlainSourceUnwind(Metadata)))
      return false;
  return true;
}

bool sourceLocalLeafRange(const BinaryImage &Image, va_t Entry, uint32_t Size) {
  return sourceLeafCodeRange(Image, Entry, Size) &&
         isMachOLocalFunctionRange(Image, Entry, Size,
                                   MachOLocalFunctionAliases::SameAddress);
}

SourceLocalCalls sourceLocalCalls(const BinaryImage &Image,
                                  const LowFunc &Caller) {
  SourceLocalCalls Result;
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable ||
      !Caller.DecodedInstructionCount || !Caller.hasCompleteLiftCoverage() ||
      Caller.Blocks.empty() ||
      Caller.Blocks.size() >
          ::neverd::limits::loader::kSourceLocalCallsCallerBlocksSizeLimit)
    return Result;
  if (auto Error = validateLowInstructionBoundaries(
          Caller, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return Result;
  }
  size_t Remaining =
      ::neverd::limits::loader::kSourceLocalCallSourceLocalCallsRemaining;
  std::set<SourceCallOccurrenceKey> Seen;
  uint64_t Boundaries = 0;
  for (const auto &Block : Caller.Blocks) {
    Boundaries += Block.InstructionBoundaries.size();
    for (const auto &Op : Block.Ops) {
      if (!Remaining--)
        return {};
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const auto Site = sourceCallOccurrenceKey(Op);
        if (!Site || !Seen.insert(*Site).second)
          return {};
      }
    }
  }
  if (Boundaries != Caller.DecodedInstructionCount)
    return {};
  for (const auto &Block : Caller.Blocks) {
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (!Remaining--)
        return {};
      if (Boundary.Control != LowInstructionControl::Call ||
          Boundary.ControlFlags != LowInstructionControlFlag::Call ||
          Boundary.Size != 4 || Boundary.OpCount != 2 ||
          Boundary.Mode != InstructionMode::Default ||
          Boundary.TargetMode != LowInstructionTargetMode::Preserve ||
          !Boundary.Immediate || Boundary.Address % 4 ||
          Boundary.Address > UINT64_MAX - 4)
        continue;
      const auto &Link = Block.Ops[Boundary.FirstOp];
      const auto &Call = Block.Ops[Boundary.FirstOp + 1];
      const auto Key = sourceCallOccurrenceKey(Call);
      if (!Key || !Key->StaticTarget || Key->Opcode != NdOp::CALL ||
          Key->Instruction != Boundary.Address || Key->Sequence != 1 ||
          *Key->StaticTarget != *Boundary.Immediate ||
          Call.Output != NdVar::reg(a64reg::X0, 8) || Call.NumInputs != 1 ||
          Call.MemoryOrdering != NdMemoryOrdering::None ||
          Call.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Link.Opcode != NdOp::COPY || Link.Seq != 0 ||
          Link.Addr != Boundary.Address || Link.NumInputs != 1 ||
          Link.Output != NdVar::reg(a64reg::X30, 8) ||
          !Link.Inputs[0].isConst() || Link.Inputs[0].Size != 8 ||
          Link.Inputs[0].Offset != Boundary.Address + 4 ||
          Link.MemoryOrdering != NdMemoryOrdering::None ||
          Link.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      const auto Bytes = readImmutableCodeBytes(Image, Boundary.Address, 4);
      if (!Bytes)
        continue;
      const auto Word = llvm::support::endian::read32le(Bytes->data());
      if (!branch::A64BranchLink.matches(Word) ||
          branch::a64BranchTarget(Word, Boundary.Address) !=
              *Key->StaticTarget ||
          *Key->StaticTarget == Caller.Entry)
        continue;
      Result.emplace(*Key, Word);
    }
  }
  return Result;
}
} // namespace neverd
