//===- RegistrationSharedCall.cpp - Checked C++ helper occurrences --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/RegistrationSharedCall.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/ir/low/LowIR.h"

#include <algorithm>

namespace neverd {
namespace {

bool hasChainOccurrence(const LowFunc &Source, va_t Address, va_t End, int Seq,
                        RegistrationChainAccess::Kind Kind) {
  if (Source.RegistrationStates->ChainAccesses.size() >
      limits::kMaxRegistrationEHRecords)
    return false;
  const RegistrationChainAccess *Match = nullptr;
  for (const auto &Access : Source.RegistrationStates->ChainAccesses)
    if (Access.Address == Address) {
      if (Match || Access.EndAddress != End || Access.OpSeq != Seq ||
          Access.AccessKind != Kind)
        return false;
      Match = &Access;
    }
  return Match;
}

bool matchesEntry(const BinaryImage &Image, const LowFunc &Source,
                  const LowBlock &Entry,
                  const coff_loader::X86CxxPrologueCall &Call) {
  if (Source.Entry != Call.Caller.Begin || Entry.StartAddr != Source.Entry ||
      Entry.EndAddr != Call.Caller.End || !Entry.Preds.empty() ||
      !Entry.ExceptionalPreds.empty() ||
      Entry.InstructionBoundaries.size() != 3 || Entry.Ops.size() > 64)
    return false;
  const auto Root =
      Source.OrdinaryModuleAnalysisRoots.upper_bound(Source.Entry);
  if (Root != Source.OrdinaryModuleAnalysisRoots.end() &&
      *Root < Call.Caller.End)
    return false;
  Decoder Dec;
  if (!Dec.init(Image))
    return false;
  Dec.setStrict(true);
  std::vector<LowOp> Expected;
  va_t Address = Call.Caller.Begin;
  for (unsigned I = 0; I != 3; ++I) {
    const auto &Boundary = Entry.InstructionBoundaries[I];
    const bool IsCall = I == 2;
    const auto *Bytes = Image.readVA(Address, Call.Caller.End - Address);
    DecodedInsn Insn;
    if (!Bytes ||
        !Dec.decodeOne(Bytes, Call.Caller.End - Address, Address, Insn) ||
        Boundary.Address != Address || Boundary.Size != Insn.Size ||
        Boundary.Mode != InstructionMode::Default ||
        Boundary.TargetMode != LowInstructionTargetMode::Preserve ||
        Boundary.Control != (IsCall ? LowInstructionControl::Call
                                    : LowInstructionControl::None) ||
        Boundary.ControlFlags != (IsCall ? LowInstructionControlFlag::Call
                                         : LowInstructionControlFlag::None) ||
        (IsCall ? Boundary.Immediate != Call.Helper.Begin
                : Boundary.Immediate.has_value()) ||
        Boundary.FirstOp != Expected.size())
      return false;
    const auto Relocations = instructionRelocations(Image, Insn);
    try {
      Dec.liftToLow(Insn, Expected, Relocations.Addresses, Relocations.Scalars);
    } catch (const UnliftedInstruction &) {
      return false;
    }
    if (Boundary.OpCount != Expected.size() - Boundary.FirstOp)
      return false;
    Address += Insn.Size;
  }
  return Address == Call.Caller.End && Entry.Ops == Expected;
}

} // namespace

std::optional<coff_loader::X86CxxPrologueCall>
getCheckedX86RegistrationCxxProloguePrefix(const BinaryImage &Image,
                                           const LowFunc &Source) {
  if (Image.Arch != Arch::X86 || !Image.isCOFF() || !Source.ExceptionMetadata ||
      Source.Blocks.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  const auto Call = coff_loader::getCheckedX86CxxPrologueCall(
      Image, *Source.ExceptionMetadata);
  if (!Call)
    return std::nullopt;
  const LowBlock *Entry = nullptr;
  for (const auto &Block : Source.Blocks)
    if (Block.StartAddr == Source.Entry) {
      if (Entry)
        return std::nullopt;
      Entry = &Block;
    }
  return Entry && matchesEntry(Image, Source, *Entry, *Call) ? Call
                                                             : std::nullopt;
}

std::optional<coff_loader::X86CxxPrologueCall>
getCheckedX86RegistrationCxxPrologueCall(const BinaryImage &Image,
                                         const LowFunc &Source) {
  if (Image.Arch != Arch::X86 || !Image.isCOFF() || !Source.ExceptionMetadata ||
      !Source.RegistrationStates || !Source.RegistrationStates->Complete ||
      !Source.RegistrationStates->CallbackStatesComplete ||
      !Source.RegistrationStates->RegistrationLifetimeComplete ||
      !Source.RegistrationStates->ChainOperationsComplete ||
      !Source.RegistrationStates->CxxPrologue)
    return std::nullopt;
  const auto Call = getCheckedX86RegistrationCxxProloguePrefix(Image, Source);
  if (!Call)
    return std::nullopt;
  const auto &Receipt = *Source.RegistrationStates->CxxPrologue;
  const auto Entry = std::find_if(
      Source.Blocks.begin(), Source.Blocks.end(),
      [&](const auto &Block) { return Block.StartAddr == Source.Entry; });
  if (Receipt.Address != Call->CallVA ||
      Receipt.EndAddress != Call->Caller.End ||
      Receipt.HelperVA != Call->Helper.Begin ||
      Receipt.StackOffset != Call->PostCallStackOffset ||
      Entry == Source.Blocks.end() || Entry->Ops.back().Seq != Receipt.OpSeq ||
      !hasChainOccurrence(Source, Call->CallVA, Call->Caller.End, Receipt.OpSeq,
                          RegistrationChainAccess::Kind::Install))
    return std::nullopt;
  return Call;
}

std::optional<coff_loader::X86CxxEpilogueCall>
getCheckedX86RegistrationCxxEpilogueCall(const BinaryImage &Image,
                                         const LowFunc &Source,
                                         const LowBlock &Block) {
  if (!Source.ExceptionMetadata || !Source.RegistrationStates ||
      !Source.RegistrationStates->Complete ||
      !Source.RegistrationStates->CallbackStatesComplete ||
      !Source.RegistrationStates->RegistrationLifetimeComplete ||
      !Source.RegistrationStates->ChainOperationsComplete ||
      !Source.RegistrationStates->CxxPrologue || Block.Ops.empty() ||
      Block.InstructionBoundaries.empty())
    return std::nullopt;
  const auto &Op = Block.Ops.back();
  const auto &Boundary = Block.InstructionBoundaries.back();
  if (Op.Opcode != NdOp::CALL ||
      !hasChainOccurrence(Source, Op.Addr, Block.EndAddr, Op.Seq,
                          RegistrationChainAccess::Kind::Remove))
    return std::nullopt;
  const auto Call = coff_loader::getCheckedX86CxxEpilogueCall(
      Image, *Source.ExceptionMetadata, Op.Addr);
  if (!Call || Call->Caller.End != Block.EndAddr ||
      Boundary.Address != Op.Addr || Boundary.Size != 5 ||
      Boundary.FirstOp != Block.Ops.size() - 1 || Boundary.OpCount != 1 ||
      Boundary.Mode != InstructionMode::Default ||
      Boundary.TargetMode != LowInstructionTargetMode::Preserve ||
      Boundary.Control != LowInstructionControl::Call ||
      Boundary.ControlFlags != LowInstructionControlFlag::Call ||
      Boundary.Immediate != Call->Helper.Begin)
    return std::nullopt;
  const auto Root = Source.OrdinaryModuleAnalysisRoots.upper_bound(Op.Addr);
  if (Root != Source.OrdinaryModuleAnalysisRoots.end() &&
      *Root < Call->Caller.End)
    return std::nullopt;
  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  Dec.setStrict(true);
  const auto *Bytes = Image.readVA(Op.Addr, 5);
  DecodedInsn Insn;
  if (!Bytes || !Dec.decodeOne(Bytes, 5, Op.Addr, Insn) || Insn.Size != 5)
    return std::nullopt;
  const auto Relocations = instructionRelocations(Image, Insn);
  std::vector<LowOp> Expected;
  try {
    Dec.liftToLow(Insn, Expected, Relocations.Addresses, Relocations.Scalars);
  } catch (const UnliftedInstruction &) {
    return std::nullopt;
  }
  if (Expected.size() != 1 || Expected.front() != Op)
    return std::nullopt;
  return Call;
}

} // namespace neverd
