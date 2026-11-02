//===- ImportCallee.cpp - Cached import pointer evidence ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reuse exact private-frame byte evidence when a caller caches an import.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/ImportCallee.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"

#include <utility>
#include <vector>

namespace neverd {
namespace {

std::optional<SourceFunctionTypeHint>
frameSignature(Arch A, BinaryFormat F, const FloatingCallArguments &Signature) {
  const auto Types = floatingCallTypes(A, F, Signature);
  if (!Types)
    return std::nullopt;
  const auto Placement =
      placeSourceParameters(A, F, Types->Result, Types->Parameters);
  if (!Placement || Placement->ResultPointer)
    return std::nullopt;
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  Hint.Architecture = A;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = Types->Result;
  const auto &TRI = getTargetRegInfo(A);
  if (Signature.ReturnElements) {
    const unsigned Bytes = Signature.ReturnBytes / Signature.ReturnElements;
    if (Signature.ReturnElements > TRI.FPReturnRegs.size())
      return std::nullopt;
    for (unsigned I = 0; I < Signature.ReturnElements; ++I)
      Hint.ReturnComponents.push_back({SourceABICarrierKind::FloatingRegister,
                                       TRI.FPReturnRegs[I], 0,
                                       uint16_t(Bytes)});
  } else {
    Hint.ReturnLocation = {SourceABICarrierKind::FloatingRegister,
                           TRI.FPReturnReg, 0, Signature.ReturnBytes};
  }
  size_t Scalar = 0;
  for (size_t I = 0; I < Types->Parameters.size(); ++I) {
    SourceParameterTypeHint Parameter;
    Parameter.Type = Types->Parameters[I];
    if (!Signature.ParameterElements.empty() &&
        Signature.ParameterElements[I]) {
      for (unsigned J = 0; J < Signature.ParameterElements[I]; ++J) {
        const auto &Part = Signature.Scalars[Scalar++];
        Parameter.Components.push_back(
            {Part.Kind, Part.Register, Part.StackOffset, Part.Bytes});
      }
    } else {
      if (Placement->Parameters[I].size() != 1 ||
          Placement->Parameters[I][0].Indirect)
        return std::nullopt;
      Parameter.Location = Placement->Parameters[I][0].Location;
      ++Scalar;
    }
    Hint.Parameters.push_back(std::move(Parameter));
  }
  std::string Error;
  return validateSourceABI(Hint, Error) ? std::optional(std::move(Hint))
                                        : std::nullopt;
}
} // namespace

ImportedCallSlotFacts
importedCallSlotFacts(const LowFunc &Function, const BinaryImage &Image,
                      const std::map<va_t, FloatingCallArguments> &Signatures,
                      const std::map<va_t, va_t> *ArgumentSources) {
  ImportedCallSlotFacts Result;
  if (Image.Arch != Arch::AArch64 || Signatures.empty())
    return Result;
  const auto Format = Image.abiFormat();
  if (Format != BinaryFormat::COFF && Format != BinaryFormat::ELF &&
      Format != BinaryFormat::MachO)
    return Result;
  size_t Remaining = limits::ir::kSourceFrameAnalysisRemaining;
  const auto Preserved =
      getTargetRegInfo(Image.Arch).callPreservedRanges(Format);
  std::map<va_t, std::optional<SourceFunctionTypeHint>> Declarations;
  NativeSourceCalls Calls;
  std::vector<std::pair<const LowBlock *, size_t>> Candidates;
  std::set<NativeSourceCallKey> Occurrences;
  for (const auto &Block : Function.Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      if (!Remaining)
        return {};
      --Remaining;
      if (const auto Site = nativeSourceCallKey(Block.Ops[I])) {
        if (!Occurrences.insert(*Site).second)
          return {};
        Candidates.emplace_back(&Block, I);
      }
    }
  LoadedCallSlotProof Proof;
  Proof.WorkRemaining = &Remaining;
  Proof.Preserves = [&](const LowOp &Call, const NdVar &Value) {
    const auto Site = nativeSourceCallKey(Call);
    if (!Site || !Calls.count(*Site) || !Value.isReg() ||
        Value.Offset > UINT64_MAX - Value.Size)
      return false;
    for (const auto &Range : Preserved)
      if (Value.Offset >= Range.Offset &&
          Value.Offset + Value.Size <= Range.Offset + Range.Bytes)
        return true;
    return false;
  };
  bool Changed = true;
  while (Changed && Remaining) {
    Changed = false;
    for (const auto &[BlockPointer, I] : Candidates) {
      const auto &Block = *BlockPointer;
      if (!Remaining)
        return Result;
      --Remaining;
      const auto &Op = Block.Ops[I];
      const auto Site = nativeSourceCallKey(Op);
      if (!Site || Calls.count(*Site))
        continue;
      std::map<ImportedCallSlotFacts::Operation, SourceFrameDefinition> Reloads;
      Proof.Reload = [&](int BlockId,
                         size_t Index) -> std::optional<SourceFrameDefinition> {
        const auto Source = sourceFrameLoadedDefinition(
            Function, Image.Arch, Calls, BlockId, Index, Format, &Remaining);
        if (!Source)
          return std::nullopt;
        Reloads[{BlockId, Index}] = Source->Definition;
        return Source->Definition;
      };
      LoadedCallSlotProof RegistersOnly;
      RegistersOnly.Preserves = Proof.Preserves;
      RegistersOnly.WorkRemaining = &Remaining;
      auto Slot = Op.Inputs[0].isConst()
                      ? std::optional<va_t>(Op.Inputs[0].Offset)
                      : loadedCallSlot(Function, Block, I, Op.Inputs[0],
                                       &RegistersOnly);
      if (!Slot && !Op.Inputs[0].isConst())
        Slot = loadedCallSlot(Function, Block, I, Op.Inputs[0], &Proof);
      if (!Slot || importCalleeName(Image, *Slot).empty() ||
          importOwnRoutine(Image, ArgumentSources, *Slot))
        continue;
      const auto Signature = Signatures.find(*Slot);
      if (Signature == Signatures.end())
        continue;
      auto [Declaration, Inserted] = Declarations.try_emplace(*Slot);
      if (Inserted)
        Declaration->second =
            frameSignature(Image.Arch, Format, Signature->second);
      if (!Declaration->second)
        continue;
      NativeSourceCallContract Contract;
      Contract.Signature = &*Declaration->second;
      Calls.emplace(*Site, Contract);
      Result.Calls[{Block.Id, I}] = *Slot;
      Result.Reloads.insert(Reloads.begin(), Reloads.end());
      Changed = true;
    }
  }
  return Result;
}
} // namespace neverd
