//===- RegistrationCallStacks.cpp - Protocol CALL stacks -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Place PE32 EH3/EH4 CALL stacks by the frame protocol where the
/// registration-state proof is incomplete.
///
//===----------------------------------------------------------------------===//

#include "RegistrationCallStacks.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include <limits>
#include <optional>

namespace neverd::x86_registration {
namespace {
bool charge(size_t &Remaining, size_t Count) {
  if (Count > Remaining) {
    Remaining = 0;
    return false;
  }
  Remaining -= Count;
  return true;
}

/// The constant \p Value names at \p User: a literal, or a temporary that the
/// same instruction copies from one before address folding.
std::optional<uint64_t> instructionConstant(const MedBlock &Block,
                                            const MedOp &User,
                                            const MedVar &Value) {
  if (Value.isConst())
    return Value.ConstVal;
  if (Value.Kind != MedVar::Temp)
    return std::nullopt;
  for (const MedOp &Op : Block.Ops)
    if (Op.Addr == User.Addr && Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
        Op.Inputs[0].isConst() && Op.Output.Kind == Value.Kind &&
        Op.Output.Id == Value.Id && Op.Output.SSAVer == Value.SSAVer)
      return Op.Inputs[0].ConstVal;
  return std::nullopt;
}

/// The one value every offset names, if each is known.
std::optional<int64_t>
agreed(const std::vector<std::optional<int64_t>> &Offsets, size_t Begin,
       size_t End) {
  std::optional<int64_t> Result;
  for (size_t I = Begin; I < End; ++I) {
    if (!Offsets[I] || (Result && *Result != *Offsets[I]))
      return std::nullopt;
    Result = Offsets[I];
  }
  return Result;
}
} // namespace

std::vector<ProvenStackOffset> savedStackReloads(
    const MedFunc &Func, const LowFunc &Source, const BinaryImage &Image,
    const std::map<const MedOp *, int64_t> &Accesses, size_t &Remaining) {
  if (Image.Arch != Arch::X86 || !Source.ExceptionMetadata ||
      !Source.ExceptionMetadata->Registration)
    return {};
  const auto &EH = *Source.ExceptionMetadata;
  const auto &Chain = *EH.Registration;
  if (Chain.RealignedFrame ||
      (EH.Encoding != ExceptionEncoding::X86ScopeTableEH3 &&
       EH.Encoding != ExceptionEncoding::X86ScopeTableEH4))
    return {};
  const int64_t Width = getTargetRegInfo(Image.Arch).PointerSize;
  // The record the prologue pushes: the handler, scope table and seeded
  // level one, two and three words above the node FS:[0] links. Each atomic
  // write's address is checked too: it has no STORE view among the accesses.
  std::set<int64_t> Nodes[3];
  const uint64_t Fields[3] = {Chain.HandlerVA, Chain.ScopeTableVA,
                              uint32_t(Chain.SeededTryLevel.value_or(0))};
  std::vector<MedVar> Exchanges;
  std::vector<int64_t> Sizes;
  for (const MedBlock &Block : Func.Blocks) {
    if (!charge(Remaining, 1 + Block.Ops.size()))
      return {};
    for (const MedOp &Op : Block.Ops) {
      if (Op.Dead)
        continue;
      if ((Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
           Op.Opcode == NdOp::ATOMIC_CMPXCHG) &&
          Op.NumInputs) {
        Exchanges.push_back(Op.Inputs[0]);
        Sizes.push_back(Op.Output.Size);
        continue;
      }
      const auto Begin = Accesses.find(&Op);
      if (Op.Opcode != NdOp::STORE || Begin == Accesses.end() ||
          Op.Inputs[1].Size != Width ||
          Begin->second < std::numeric_limits<int64_t>::min() + 3 * Width)
        continue;
      const auto Value = instructionConstant(Block, Op, Op.Inputs[1]);
      for (int64_t Word = 0; Word < 3; ++Word)
        if (Value && (*Value & UINT32_MAX) == Fields[Word])
          Nodes[Word].insert(Begin->second - (Word + 1) * Width);
    }
  }
  if (!Chain.SeededTryLevel || Nodes[0].size() != 1 || Nodes[1] != Nodes[0] ||
      !Nodes[2].count(*Nodes[0].begin()))
    return {};
  // EH3 and EH4 keep SavedESP, then the exception pointers, in the two words
  // below the node.
  const int64_t Field = *Nodes[0].begin() - 2 * Width;
  const auto Overlaps = [&](int64_t Begin, int64_t Size) {
    return Begin < Field + Width && Field < Begin + Size;
  };
  if (!Exchanges.empty()) {
    const auto Exchanged = entryStackOffsets(Func, Exchanges, Image.Arch,
                                             Image.abiFormat(), Func.EntryKind);
    for (size_t I = 0; I < Exchanged.size(); ++I)
      if (Exchanged[I] && Overlaps(*Exchanged[I], Sizes[I]))
        return {};
  }
  std::vector<MedVar> Saved;
  if (!charge(Remaining, 2 * Accesses.size()))
    return {};
  for (const auto &[Op, Begin] : Accesses) {
    if (Op->Opcode != NdOp::STORE || !Overlaps(Begin, Op->Inputs[1].Size))
      continue;
    if (Begin != Field || Op->Inputs[1].Size != Width)
      return {};
    Saved.push_back(Op->Inputs[1]);
  }
  if (Saved.empty())
    return {};
  const auto Stack =
      agreed(entryStackOffsets(Func, Saved, Image.Arch, Image.abiFormat(),
                               Func.EntryKind),
             0, Saved.size());
  if (!Stack)
    return {};
  std::vector<ProvenStackOffset> Seeds;
  for (const auto &[Op, Begin] : Accesses)
    if (Op->Opcode == NdOp::LOAD && Begin == Field && Op->Output.Size == Width)
      Seeds.push_back({Op, *Stack});
  return Seeds;
}

std::set<const MedOp *>
privateCallbackCalls(const MedFunc &Func, const BinaryImage &Image,
                     const std::map<const MedOp *, MedVar> &Calls,
                     size_t &Remaining) {
  if (Image.Arch != Arch::X86 || Calls.empty())
    return {};
  // RegistrationRoots defines each runtime callback's ESP at its entry.
  std::vector<const MedOp *> Roots;
  for (const MedBlock &Block : Func.Blocks) {
    if (!charge(Remaining, 1 + Block.Ops.size()))
      return {};
    for (const MedOp &Op : Block.Ops)
      if (Op.RegistrationRoot ==
              MedOp::RegistrationRootKind::CallbackStackPointer &&
          Op.Addr == Block.StartAddr && hasValidRegistrationRootShape(Op) &&
          !Op.Dead)
        Roots.push_back(&Op);
  }
  std::vector<const MedOp *> Pending;
  std::vector<MedVar> Stacks;
  for (const auto &[Call, Stack] : Calls) {
    Pending.push_back(Call);
    Stacks.push_back(Stack);
  }
  std::set<const MedOp *> Result;
  for (const MedOp *Root : Roots) {
    if (!charge(Remaining, Stacks.size()))
      return {};
    // A stack mixing this root with another origin has no offset from it.
    const auto Private = stackOffsetsRelativeToDefinition(
        Func, Stacks, *Root, Image.Arch, Image.abiFormat());
    for (size_t I = 0; I < Pending.size(); ++I)
      if (Private[I] && *Private[I] <= 0 &&
          *Private[I] >= -int64_t(limits::kMaxSourceCallBorrowedBytes))
        Result.insert(Pending[I]);
  }
  return Result;
}

} // namespace neverd::x86_registration
