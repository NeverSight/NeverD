//===- PipelineIncomingStack.cpp - Incoming stack source inputs ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recover scalar stack inputs of complete, frameless read-only leaves.
///
//===----------------------------------------------------------------------===//

#include "PipelineIncomingStack.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
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
using Key = std::tuple<MedVar::VarKind, int, int>;
Key key(const MedVar &Value) { return {Value.Kind, Value.Id, Value.SSAVer}; }

bool supported(NdOp Opcode) {
  return Opcode == NdOp::COPY || Opcode == NdOp::INT_ADD ||
         Opcode == NdOp::LOAD || Opcode == NdOp::RETURN;
}
} // namespace

std::optional<SourceFunctionTypeHint>
inferIncomingStackLeaf(const BinaryImage &Image, const LowFunc &Low,
                       const MedFunc &Med) {
  if ((Image.Arch != Arch::X64 && Image.Arch != Arch::AArch64) ||
      (Image.Format != BinaryFormat::COFF &&
       Image.Format != BinaryFormat::ELF) ||
      Image.IsRelocatable || Image.Bits != Bitness::Bits64 ||
      !Image.isCodeAddress(Low.Entry) || Low.Entry != Med.Entry ||
      !Low.hasCompleteLiftCoverage() || !Low.LiftedInstructionCount ||
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
  for (const auto &Op : LB.Ops) {
    if (!supported(Op.Opcode) || Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (Op.Output.isReg() && Op.Output.Size &&
         (Op.Output.Offset != TRI.IntReturnReg ||
          Op.Output.Size != TRI.PointerSize)))
      return {};
  }

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
  std::set<int64_t> Inputs;
  auto AddInput = [&](int64_t Offset) {
    if (Offset < Minimum || Offset % TRI.PointerSize ||
        Offset > limits::ir::kMaxSourceStackArgumentBytes - TRI.PointerSize)
      return false;
    Inputs.insert(Offset);
    return Inputs.size() <= limits::ir::kMaxSourceABIParameters;
  };
  enum class Role { Scalar, Address };
  std::map<Key, Role> Values;
  auto Read = [&](const MedVar &Value) -> std::optional<Role> {
    if (Value.Size != TRI.PointerSize)
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
      if (AddInput(Offset))
        return Role::Scalar;
    }
    return {};
  };
  bool Returned = false;
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
      continue; // Other entry placeholders establish no input value.
    }
    std::optional<Role> Result;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
      Result = Read(Op.Inputs[0]);
    else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2) {
      const auto A = Read(Op.Inputs[0]), B = Read(Op.Inputs[1]);
      if (A == Role::Scalar && B == Role::Scalar)
        Result = Role::Scalar;
      else if (A && B && *A != *B && Frame.contains(key(Op.Output)))
        Result = Role::Address;
    } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
               Read(Op.Inputs[0]) == Role::Address) {
      const auto Offset = Frame.find(key(Op.Inputs[0]));
      if (Offset != Frame.end() && AddInput(Offset->second))
        Result = Role::Scalar;
    } else if (Op.Opcode == NdOp::RETURN && Op.NumInputs == 1 &&
               Read(Op.Inputs[0]) == Role::Scalar) {
      Returned = true;
      continue;
    }
    if (!Result || Op.Output.Size != TRI.PointerSize ||
        (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp) ||
        !Values.emplace(key(Op.Output), *Result).second)
      return {};
  }
  if (!Returned || Inputs.empty())
    return {};
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Architecture = Image.Arch;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = NdType::makeInt(TRI.PointerSize, false);
  Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                         TRI.IntReturnReg, 0, TRI.PointerSize};
  for (int64_t Offset : Inputs) {
    SourceParameterTypeHint Parameter;
    Parameter.Name = "stack_arg" + std::to_string(Hint.Parameters.size());
    Parameter.Type = Hint.ReturnType;
    Parameter.Location = {SourceABICarrierKind::Stack, 0, Offset,
                          TRI.PointerSize};
    Hint.Parameters.push_back(std::move(Parameter));
  }
  std::string Diagnostic;
  return validateSourceABI(Hint, Diagnostic)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}
} // namespace neverd
