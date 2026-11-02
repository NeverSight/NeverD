//===- PipelineIncomingStack.cpp - Incoming stack source inputs ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recover scalar stack inputs of complete leaves with restored frames.
///
//===----------------------------------------------------------------------===//

#include "PipelineIncomingStack.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Limits.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {
using Key = std::tuple<MedVar::VarKind, int, uint16_t, int>;
Key key(const MedVar &Value) {
  return {Value.Kind, Value.Id, Value.Size, Value.SSAVer};
}

bool supported(NdOp Opcode) {
  return Opcode == NdOp::COPY || Opcode == NdOp::INT_ADD ||
         Opcode == NdOp::INT_SUB || Opcode == NdOp::INT_ZEXT ||
         Opcode == NdOp::INT_SEXT || Opcode == NdOp::SUBBYTES ||
         Opcode == NdOp::LOAD || Opcode == NdOp::STORE ||
         Opcode == NdOp::RETURN;
}
bool supported(const LowOp &Op, const TargetRegInfo &TRI) {
  // LowIR retains arithmetic flag calculations even when ordinary source
  // conversion proves them unobserved. Reuse the frame owner's total-bit
  // transfer policy; the MedIR role walk still rejects every live unsupported
  // operation or leaked frame value. Direction state is not an arithmetic
  // result and must not disappear behind this source declaration.
  return (supported(Op.Opcode) || isSourceFrameScalarTransfer(Op.Opcode)) &&
         Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         (!Op.Output.isReg() || !Op.Output.Size ||
          (TRI.isFlag(Op.Output.Offset, Op.Output.Size) &&
           Op.Output.Offset != TRI.FlagDF) ||
          (Op.Output.Offset == TRI.IntReturnReg &&
           Op.Output.Size <= TRI.PointerSize) ||
          ((Op.Output.Offset == TRI.StackPointer ||
            TRI.isFrameOrLinkReg(Op.Output.Offset)) &&
           Op.Output.Size == TRI.PointerSize)) &&
         (Op.Opcode != NdOp::RETURN ||
          (Op.NumInputs == 1 &&
           Op.Inputs[0] == NdVar::reg(TRI.LinkRegister ? TRI.LinkRegister
                                                       : TRI.IntReturnReg,
                                      TRI.PointerSize)));
}
} // namespace

bool supportsIncomingStackSources(const BinaryImage &Image) {
  return (Image.Arch == Arch::X64 || Image.Arch == Arch::AArch64) &&
         (Image.Format == BinaryFormat::COFF ||
          Image.Format == BinaryFormat::ELF) &&
         Image.Bits == Bitness::Bits64 && !Image.IsRelocatable;
}

bool mayReadIncomingStackLeaf(const BinaryImage &Image, va_t Entry,
                              Decoder &Dec) {
  if (!supportsIncomingStackSources(Image))
    return false;
  if (const auto *Exception = Image.ExceptionMetadata.findFunction(Entry);
      Exception && (Exception->Personality != ExceptionPersonality::None ||
                    Exception->ParseStatus != ExceptionParseStatus::Complete))
    return false;
  const auto Segment = std::find_if(
      Image.Segments.begin(), Image.Segments.end(),
      [&](const auto &Segment) { return Segment.contains(Entry); });
  if (Segment == Image.Segments.end())
    return false;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  size_t Offset = Entry - Segment->VA;
  size_t Work = 0;
  const auto End = Image.getFunctionMetadataEnd(Entry);
  for (size_t Step = 0; Step < limits::pipeline::kIncomingStackLeafOperations;
       ++Step) {
    if (Offset >= Segment->Data.size() ||
        (End != InvalidVA && Segment->VA + Offset >= End) ||
        !Dec.selectMode(Image, Segment->VA + Offset))
      return false;
    DecodedInsn Instruction{};
    if (!Dec.decodeOneForLift(Segment->Data.data() + Offset,
                              Segment->Data.size() - Offset,
                              Segment->VA + Offset, Instruction) ||
        !Instruction.Size)
      return false;
    std::vector<LowOp> Ops;
    try {
      Dec.liftToLow(Instruction, Ops);
    } catch (const UnliftedInstruction &) {
      return false;
    }
    if (Ops.empty() ||
        Ops.size() > limits::pipeline::kIncomingStackLeafOperations - Work)
      return false;
    Work += Ops.size();
    for (const auto &Op : Ops)
      if (!supported(Op, TRI))
        return false;
    if (Ops.back().Opcode == NdOp::RETURN)
      return true;
    if (Dec.isFunctionTerminator(Instruction))
      return false;
    Offset += Instruction.Size;
  }
  return false;
}

std::optional<SourceFunctionTypeHint>
inferIncomingStackLeaf(const BinaryImage &Image, const LowFunc &Low,
                       const MedFunc &Med) {
  if (!supportsIncomingStackSources(Image) || !Image.isCodeAddress(Low.Entry) ||
      Low.Entry != Med.Entry || !Low.hasCompleteLiftCoverage() ||
      !Low.LiftedInstructionCount ||
      !Low.UnsafeIndirectBranchAddresses.empty() || Low.CalleePopBytes ||
      std::any_of(Low.ModuleAnalysisRoots.begin(),
                  Low.ModuleAnalysisRoots.end(),
                  [&](va_t Root) { return Root != Low.Entry; }) ||
      std::any_of(Low.OrdinaryModuleAnalysisRoots.begin(),
                  Low.OrdinaryModuleAnalysisRoots.end(),
                  [&](va_t Root) { return Root != Low.Entry; }) ||
      Low.Blocks.size() != 1 || Med.Blocks.size() != 1 || Med.SourceTypeHint ||
      Med.SourceParametersBound || Med.SkippedSSA || !Med.CallInfos.empty() ||
      !Med.CallClobbers.empty() || Med.IsVariadic || !Med.MultiReturn.empty() ||
      Med.FPReturnViaX87 || Med.DoesNotReturn ||
      Med.EntryKind != StackEntryKind::Call)
    return {};
  if (const auto *Exception = Image.ExceptionMetadata.findFunction(Low.Entry);
      Exception && (Exception->Personality != ExceptionPersonality::None ||
                    Exception->ParseStatus != ExceptionParseStatus::Complete))
    return {};
  const auto &LB = Low.Blocks.front();
  const auto &MB = Med.Blocks.front();
  if (LB.StartAddr != Low.Entry || MB.StartAddr != Med.Entry ||
      !LB.Preds.empty() || !LB.Succs.empty() || !LB.ExceptionalPreds.empty() ||
      !LB.ExceptionalSuccs.empty() || !MB.Preds.empty() || !MB.Succs.empty() ||
      !MB.Phis.empty() || !MB.ExceptionalPreds.empty() ||
      !MB.ExceptionalSuccs.empty() || LB.Ops.empty() || MB.Ops.empty() ||
      LB.Ops.back().Opcode != NdOp::RETURN ||
      MB.Ops.back().Opcode != NdOp::RETURN ||
      LB.Ops.size() > limits::pipeline::kIncomingStackLeafOperations ||
      MB.Ops.size() > limits::pipeline::kIncomingStackLeafOperations)
    return {};
  const auto &TRI = getTargetRegInfo(Image.Arch);
  for (const auto &Op : LB.Ops)
    if (!supported(Op, TRI))
      return {};

  // Use the shared entry-stack proof for every address. The value walk below
  // only checks use roles; it neither derives offsets nor guesses aliases.
  std::vector<MedVar> Queries;
  for (const auto &Op : MB.Ops) {
    Queries.push_back(Op.Output);
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      Queries.push_back(Op.Inputs[I]);
  }
  const auto Offsets = entryStackOffsets(Med, Queries, Image.Arch,
                                         Image.abiFormat(), Med.EntryKind);
  std::map<Key, int64_t> Frame;
  for (size_t I = 0; I < Queries.size(); ++I)
    if (Offsets[I])
      Frame.emplace(key(Queries[I]), *Offsets[I]);
  const auto Layout = TRI.integerArgumentLayout(Med.CC == CallingConv::Win64);
  const int RegisterCount = static_cast<int>(Layout.Registers.size());
  const int64_t Minimum = Image.Arch == Arch::X64 ? TRI.PointerSize : 0;
  std::map<int64_t, uint16_t> Inputs;
  auto AddInput = [&](int64_t Offset, uint16_t Size) {
    if (Size != 1 && Size != 2 && Size != 4 && Size != TRI.PointerSize)
      return false;
    if (Offset < Minimum || Offset % TRI.PointerSize ||
        Offset > limits::ir::kMaxSourceStackArgumentBytes - Size)
      return false;
    Inputs[Offset] = std::max(Inputs[Offset], Size);
    return Inputs.size() <= limits::ir::kMaxSourceABIParameters;
  };
  enum class Role { Scalar, Address, Saved };
  std::map<Key, Role> Values;
  auto Read = [&](const MedVar &Value) -> std::optional<Role> {
    if (!Value.Size || Value.Size > TRI.PointerSize)
      return {};
    if (Value.isConst())
      return Role::Scalar;
    if (auto It = Values.find(key(Value)); It != Values.end())
      return It->second;
    if (Value.Kind == MedVar::Param && Value.RegOff == kNoParamReg &&
        Value.Id >= RegisterCount &&
        Value.Id - RegisterCount <=
            limits::ir::kMaxSourceStackArgumentBytes / Layout.SlotBytes) {
      const int64_t Offset =
          Layout.EntryStackBase +
          int64_t(Value.Id - RegisterCount) * Layout.SlotBytes;
      if (AddInput(Offset, Value.Size))
        return Role::Scalar;
    }
    return {};
  };
  bool Returned = false;
  std::optional<MedVar> IntegerResult;
  for (const auto &Op : MB.Ops) {
    if (Returned || !supported(Op.Opcode) ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !Op.IntrinsicOutputs.empty())
      return {};
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
        Op.Output == Op.Inputs[0]) {
      if (Op.Output.Kind == MedVar::Reg &&
          Op.Output.RegOff == TRI.StackPointer &&
          Op.Output.Size == TRI.PointerSize && Op.Output.SSAVer == 0 &&
          Frame.contains(key(Op.Output)) && Frame.at(key(Op.Output)) == 0)
        Values.emplace(key(Op.Output), Role::Address);
      else if (Op.Output.Kind == MedVar::Reg &&
               Op.Output.Size == TRI.PointerSize &&
               isEntryLiveInValue(Med, Op.Output, Op.Output.RegOff) &&
               TRI.isFrameOrLinkReg(Op.Output.RegOff))
        Values.emplace(key(Op.Output), Role::Saved);
      continue; // Other entry placeholders establish no input value.
    }
    std::optional<Role> Result;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
      Result = Read(Op.Inputs[0]);
    else if ((Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT) &&
             Op.NumInputs == 1 && Read(Op.Inputs[0]) == Role::Scalar)
      Result = Role::Scalar;
    else if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
             Read(Op.Inputs[0]) == Role::Scalar && Op.Inputs[1].isConst() &&
             Op.Inputs[1].ConstVal <= Op.Inputs[0].Size &&
             Op.Output.Size <= Op.Inputs[0].Size - Op.Inputs[1].ConstVal)
      Result = Role::Scalar;
    else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
             Op.NumInputs == 2) {
      const auto A = Read(Op.Inputs[0]), B = Read(Op.Inputs[1]);
      if (A == Role::Scalar && B == Role::Scalar)
        Result = Role::Scalar;
      else if (((A == Role::Address && B == Role::Scalar) ||
                (Op.Opcode == NdOp::INT_ADD && A == Role::Scalar &&
                 B == Role::Address)) &&
               Frame.contains(key(Op.Output)))
        Result = Role::Address;
    } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
               Read(Op.Inputs[0]) == Role::Address) {
      const auto Offset = Frame.find(key(Op.Inputs[0]));
      if (Offset != Frame.end()) {
        if (Offset->second < 0)
          Result = Role::Saved;
        else if (AddInput(Offset->second, Op.Output.Size))
          Result = Role::Scalar;
      }
    } else if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
               Op.Inputs[1].Size == TRI.PointerSize &&
               Read(Op.Inputs[0]) == Role::Address &&
               Read(Op.Inputs[1]) == Role::Saved) {
      const auto Offset = Frame.find(key(Op.Inputs[0]));
      if (Offset == Frame.end() || Offset->second >= 0)
        return {};
      continue;
    } else if (Op.Opcode == NdOp::RETURN && Op.NumInputs == 1) {
      // AArch64 LowIR retains LR as RETURN's control target. It is not the
      // source result. Prove that control is unchanged and that a distinct,
      // complete X0 definition supplies the source result before declaring it.
      if (Image.Arch == Arch::AArch64) {
        if ((!isEntryLiveInValue(Med, Op.Inputs[0], TRI.LinkRegister) &&
             Read(Op.Inputs[0]) != Role::Saved) ||
            !IntegerResult || Read(*IntegerResult) != Role::Scalar)
          return {};
      } else if (Read(Op.Inputs[0]) != Role::Scalar)
        return {};
      Returned = true;
      continue;
    }
    // Scalar views retain exactly the observed bytes. Frame addresses and
    // saved registers still require complete pointer-width identities.
    if (!Result || !Op.Output.Size || Op.Output.Size > TRI.PointerSize ||
        (*Result != Role::Scalar && Op.Output.Size != TRI.PointerSize) ||
        (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp) ||
        !Values.emplace(key(Op.Output), *Result).second)
      return {};
    if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == TRI.IntReturnReg)
      IntegerResult = Op.Output;
  }
  if (!Returned || Inputs.empty())
    return {};
  const std::vector<std::pair<int64_t, uint16_t>> Sorted(Inputs.begin(),
                                                         Inputs.end());
  // The role walk cannot authorize a restored machine frame. Authenticate
  // the original LowIR with the shared byte-identity proof, including the
  // format-specific preserved bank and every declared incoming byte.
  auto Hint = incomingStackSourceHint(Image.Arch, Sorted);
  if (!Hint || !restoresNativeSourceState(Low, Image.Arch, {}, nullptr, &*Hint,
                                          Image.abiFormat()))
    return {};
  return Hint;
}

std::optional<SourceFunctionTypeHint>
incomingStackSourceHint(Arch Architecture,
                        llvm::ArrayRef<std::pair<int64_t, uint16_t>> Inputs) {
  if (Inputs.empty())
    return {};
  const auto &TRI = getTargetRegInfo(Architecture);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Architecture = Architecture;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = NdType::makeInt(TRI.PointerSize, false);
  Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                         TRI.IntReturnReg, 0, TRI.PointerSize};
  for (const auto &[Offset, Size] : Inputs) {
    SourceParameterTypeHint Parameter;
    Parameter.Name = "stack_arg" + std::to_string(Hint.Parameters.size());
    Parameter.Type = NdType::makeInt(Size, false);
    Parameter.Location = {SourceABICarrierKind::Stack, 0, Offset, Size};
    Hint.Parameters.push_back(std::move(Parameter));
  }
  std::string Diagnostic;
  return validateSourceABI(Hint, Diagnostic)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}
} // namespace neverd
