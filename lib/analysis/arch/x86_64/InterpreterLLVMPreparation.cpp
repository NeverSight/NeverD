//===- InterpreterLLVMPreparation.cpp - Bound LLVM proof inputs ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64UserFlags.h"

#include "neverd/analysis/arch/x86_64/InterpreterLLVMRefinement.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace neverd::analysis {
namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

bool charge(uint64_t &Remaining, uint64_t Count) {
  if (Count > Remaining)
    return false;
  Remaining -= Count;
  return true;
}

llvm::Expected<InterpreterLLVMRefinementPreservation>
normalizePreservation(const InterpreterLLVMRefinementPreservation &Request,
                      uint64_t &Work) {
  if (!charge(Work, Request.ModeledRegisters.size()) ||
      (Request.NativeState && !charge(Work, 1)))
    return invalid("LLVM proof preservation metadata budget exhausted");
  InterpreterLLVMRefinementPreservation Result;
  Result.NativeState = Request.NativeState;
  Result.ModeledRegisters.push_back({x86reg::RSP, 8});
  // Keep the omitted-request path's contract order and work unchanged.
  if (Request.ModeledRegisters.empty())
    return Result;
  constexpr uint64_t GPRBytes = offsetof(InterpreterMachineStateX64V1, RFlags);
  // Bound fixed coverage initialization and the complete output scan before
  // either traversal. Every input byte and emitted range has its own charge.
  if (!charge(Work, 2 * GPRBytes))
    return invalid("LLVM proof preservation coverage budget exhausted");
  std::array<bool, GPRBytes> Covered{};
  for (const auto &Range : Request.ModeledRegisters) {
    if (!Range.Bytes || Range.Offset >= GPRBytes ||
        Range.Bytes > GPRBytes - Range.Offset)
      return invalid("LLVM proof preservation requires nonempty modeled GPR "
                     "ranges without wrapping");
    if (!charge(Work, Range.Bytes))
      return invalid("LLVM proof preservation byte budget exhausted");
    for (uint64_t I = 0; I != Range.Bytes; ++I)
      Covered[Range.Offset + I] = true;
  }
  // RSP is already required in full. Removing its redundant requested bytes
  // cannot weaken that obligation. Other unions never bridge an uncovered byte.
  for (uint64_t Word = 0; Word != GPRBytes; Word += 8) {
    if (Word == x86reg::RSP)
      continue;
    uint64_t Byte = Word;
    while (Byte != Word + 8) {
      if (!Covered[Byte]) {
        ++Byte;
        continue;
      }
      const auto Begin = Byte;
      while (Byte != Word + 8 && Covered[Byte])
        ++Byte;
      if (!charge(Work, 1))
        return invalid("LLVM proof preservation output budget exhausted");
      Result.ModeledRegisters.push_back(
          {Begin, static_cast<uint16_t>(Byte - Begin)});
    }
  }
  return Result;
}

llvm::Error canonicalEntry(InterpreterMachineStateModel &Model,
                           uint64_t MaxOperations, uint64_t MaxBlocks,
                           uint64_t &Work) {
  auto &F = Model.Function;
  if (F.Blocks.empty() || F.Blocks.size() >= MaxBlocks ||
      !charge(Work, F.Blocks.size()) ||
      !charge(Work, Model.Instructions.size()))
    return invalid("canonical entry block or metadata budget exhausted");
  for (const auto *Roots :
       {&F.ModuleAnalysisRoots, &F.OrdinaryModuleAnalysisRoots}) {
    if (!charge(Work, Roots->size()))
      return invalid("canonical entry root budget exhausted");
    if (std::any_of(Roots->begin(), Roots->end(),
                    [&](va_t Root) { return Root != F.Entry; }))
      return invalid("canonical entry cannot replace additional roots");
  }
  uint64_t Operations = MaxOperations;
  if (!charge(Operations, 3) || !charge(Work, 3))
    return invalid("canonical entry operation budget exhausted");
  size_t EntryIndex = F.Blocks.size();
  int MaxId = -1;
  va_t LastEnd = 0;
  std::set<int> BlockIds;
  std::set<va_t> BlockAddresses;
  std::set<va_t> Targets;
  for (size_t I = 0; I != F.Blocks.size(); ++I) {
    const auto &B = F.Blocks[I];
    if (!charge(Work, B.Ops.size()) ||
        !charge(Work, B.InstructionBoundaries.size()) ||
        !charge(Work, B.Succs.size()) || !charge(Work, B.Preds.size()) ||
        !charge(Operations, B.Ops.size()))
      return invalid("canonical entry traversal or operation budget exhausted");
    if (B.Id < 0 || !BlockIds.insert(B.Id).second ||
        !BlockAddresses.insert(B.StartAddr).second)
      return invalid("canonical entry requires unique block identities");
    if (B.StartAddr == F.Entry) {
      if (EntryIndex != F.Blocks.size() || !B.Preds.empty())
        return invalid("canonical entry requires one predecessor-free anchor");
      EntryIndex = I;
    }
    MaxId = std::max(MaxId, B.Id);
    LastEnd = std::max(LastEnd, B.EndAddr);
    for (const auto &IB : B.InstructionBoundaries) {
      if (IB.Address > InvalidVA - IB.Size)
        return invalid("canonical entry instruction address wraps");
      LastEnd = std::max(LastEnd, IB.Address + IB.Size);
    }
    for (const auto &Op : B.Ops)
      if ((Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) &&
          Op.NumInputs && Op.Inputs[0].isConst())
        Targets.insert(Op.Inputs[0].Offset);
  }
  if (EntryIndex == F.Blocks.size() || MaxId == std::numeric_limits<int>::max())
    return invalid("canonical entry has no available block identity");
  const auto TargetId = F.Blocks[EntryIndex].Id;
  for (const auto &B : F.Blocks) {
    if (!charge(Work, 1) || !charge(Work, B.Succs.size()) ||
        !charge(Work, B.Preds.size()))
      return invalid("canonical entry edge budget exhausted");
    for (const auto *Edges : {&B.Succs, &B.Preds})
      for (int Id : *Edges)
        if (!BlockIds.count(Id))
          return invalid("canonical entry cannot repair a dangling graph edge");
    if (B.hasSucc(TargetId) || Targets.count(F.Entry))
      return invalid("canonical entry anchor has an incoming transfer");
  }
  // Place the new instruction beyond existing instruction spans, and do not
  // turn a previously unresolved direct target into a new entry backedge.
  while (Targets.count(LastEnd)) {
    if (!charge(Work, 1) || LastEnd == InvalidVA)
      return invalid("canonical entry address search exhausted");
    ++LastEnd;
  }
  if (LastEnd == InvalidVA)
    return invalid("canonical entry has no available instruction address");
  const auto OldEntry = F.Entry;
  LowBlock Prefix;
  Prefix.Id = MaxId + 1;
  Prefix.StartAddr = LastEnd;
  Prefix.EndAddr = LastEnd + 1;
  Prefix.Succs = {TargetId};
  const auto Emit = [&](NdOp Code, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Code;
    Op.Output = Output;
    Op.Addr = LastEnd;
    Op.Seq = static_cast<int>(Prefix.Ops.size());
    for (auto Input : Inputs)
      Op.addInput(Input);
    Prefix.Ops.push_back(Op);
  };
  const auto Flags = NdVar::reg(128, 8);
  Emit(NdOp::INT_AND, Flags,
       {Flags, NdVar::scalar(detail::X64UserFlags::EntryMask, 8)});
  Emit(NdOp::INT_OR, Flags, {Flags, NdVar::scalar(2, 8)});
  Emit(NdOp::BRANCH, {}, {NdVar::scalar(OldEntry, 8)});
  LowInstructionBoundary IB;
  IB.Address = LastEnd;
  IB.Size = 1;
  IB.OpCount = Prefix.Ops.size();
  IB.Control = LowInstructionControl::Branch;
  IB.ControlFlags = LowInstructionControlFlag::Branch;
  IB.Immediate = OldEntry;
  Prefix.InstructionBoundaries = {IB};
  LowInstructionUndefinedEffects Effects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.OpCount = IB.OpCount;
  Effects.OperationDigest = lowUndefinedOperationDigest(Prefix.Ops);
  Model.Instructions.push_back({Prefix.Id, IB, std::move(Effects)});
  F.Blocks[EntryIndex].Preds.push_back(Prefix.Id);
  F.Entry = LastEnd;
  for (auto *Roots : {&F.ModuleAnalysisRoots, &F.OrdinaryModuleAnalysisRoots})
    if (Roots->erase(OldEntry))
      Roots->insert(F.Entry);
  F.Blocks.push_back(std::move(Prefix));
  return validateLowInstructionBoundaries(
      F, LowInstructionBoundaryRequirement::Required);
}
} // namespace

llvm::Expected<InterpreterLLVMRefinementModels>
prepareInterpreterLLVMRefinement(
    const LowFunc &Residual, llvm::StringRef LLVMIR,
    llvm::StringRef FunctionName, const LowIRIndependenceFrame &Frame,
    const InterpreterLLVMRefinementLimits &Limits,
    const InterpreterLLVMRefinementPreservation &Preservation) {
  if (LLVMIR.empty() || FunctionName.empty())
    return invalid("LLVM proof requires IR text and a selected function");
  if (LLVMIR.size() > Limits.MaxIRBytes ||
      FunctionName.size() > Limits.MaxIRBytes)
    return invalid("LLVM proof input byte budget exhausted");
  if (Frame.RootRegister.Offset != 32 || Frame.RootRegister.Bytes != 8 ||
      Frame.Begin > 0 || Frame.End < 8)
    return invalid("LLVM proof requires an entry RSP frame and return slot");
  if (Frame.EntryAlignment && !Frame.EntryAlignment->valid())
    return invalid("LLVM proof requires a valid entry frame alignment");
  const uint64_t FrameBytes = uint64_t(Frame.End) - uint64_t(Frame.Begin);
  if (FrameBytes > Limits.NativeProof.Execution.MaxFrameBytes ||
      FrameBytes > Limits.LLVMProof.Execution.MaxFrameBytes ||
      Frame.ExcludedAddressRanges.size() >
          Limits.NativeProof.Execution.MaxInstructions ||
      Frame.ExcludedAddressRanges.size() >
          Limits.LLVMProof.Execution.MaxInstructions)
    return invalid("LLVM proof frame metadata budget exhausted");
  uint64_t Work = Limits.MaxPreparationItems;
  if (!charge(Work, Frame.ExcludedAddressRanges.size()))
    return invalid("LLVM proof preparation budget exhausted");
  for (const auto &Range : Frame.ExcludedAddressRanges)
    if (Range.Begin >= Range.End)
      return invalid("LLVM proof frame exclusion is empty or wraps");
  auto Required = normalizePreservation(Preservation, Work);
  if (!Required)
    return Required.takeError();

  // Hash and parse the same owned bytes, not a reprinted or normalized module.
  const std::string Text = LLVMIR.str(), Name = FunctionName.str();
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(Text, Diagnostic, Context);
  if (!Module)
    return invalid("invalid LLVM proof text: " + Diagnostic.getMessage());
  if (llvm::verifyModule(*Module))
    return invalid("invalid LLVM proof module");
  const auto *Function = Module->getFunction(Name);
  if (!Function || Function->isDeclaration())
    return invalid("LLVM proof function is missing or has no body");
  auto Left = modelInterpreterMachineStateX64(
      Residual, InterpreterMachineStateProfile::UserX64NoFaultV1,
      Limits.MaxMachineStateOperations);
  if (!Left)
    return Left.takeError();
  auto Right = modelLLVMInterpreterMachineStateX64(*Function, Limits.LLVMModel);
  if (!Right)
    return Right.takeError();
  if (auto Error = canonicalEntry(*Left, Limits.MaxMachineStateOperations,
                                  Limits.MaxMachineStateOperations, Work))
    return std::move(Error);
  if (auto Error = canonicalEntry(*Right, Limits.LLVMModel.MaxOperations,
                                  Limits.LLVMModel.MaxBlocks, Work))
    return std::move(Error);
  InterpreterLLVMRefinementModels Result;
  Result.Residual = std::move(*Left);
  Result.LLVM = std::move(*Right);
  Result.Contract = llvmInterpreterMachineStateContract();
  Result.Contract.Frame = Frame;
  Result.Preservation = std::move(*Required);
  Result.Contract.PreservedRegisters.insert(
      Result.Contract.PreservedRegisters.end(),
      Result.Preservation.ModeledRegisters.begin(),
      Result.Preservation.ModeledRegisters.end());
  Result.Contract.PreservedFrameRanges = {{0, 8}};
  llvm::SHA256 Hash;
  Hash.update(Text);
  Result.LLVMIRDigest = llvm::toHex(Hash.final());
  Result.FunctionName = Name;
  return Result;
}
} // namespace neverd::analysis
