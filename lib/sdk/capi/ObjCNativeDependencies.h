#ifndef NEVERD_SDK_CAPI_OBJCNATIVEDEPENDENCIES_H
#define NEVERD_SDK_CAPI_OBJCNATIVEDEPENDENCIES_H

#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace neverd::sdk {

struct NativeSourceDependencyEvidence {
  struct Call {
    va_t Caller, Block, Instruction, Target;
    bool Indirect;
  };
  static constexpr size_t MaxCalls = 1000000;
  std::vector<Call> Calls;
  std::set<va_t> Roots, MissingFunctions;
  bool InventoryComplete = true;
  bool TargetsComplete = true;
};

/// Both inference and reporting traverse the same supported method roots and
/// direct native edges. Indirect calls remain recorded but never add a guessed
/// target. Reporting can run against the final result without changing hints.
inline std::set<va_t>
walkObjCNativeDependencies(const BinaryImage &Image,
                           const PipelineResult &Result,
                           NativeSourceDependencyEvidence *Evidence = nullptr,
                           const std::set<va_t> &CallbackRoots = {}) {
  if (Result.SourceImage != &Image)
    throw std::invalid_argument(
        "native source dependency evidence belongs to another image");
  if (Evidence)
    *Evidence = {};
  std::map<va_t, const LowFunc *> Low;
  for (const auto &Function : Result.LowFuncs)
    Low.emplace(Function.Entry, &Function);
  std::vector<va_t> Pending;
  for (const auto &Method : Image.ObjCMethods)
    if (Method.TypeHint && Method.Status == "supported") {
      Pending.push_back(Method.Implementation);
      if (Evidence)
        Evidence->Roots.insert(Method.Implementation);
    }
  std::set<va_t> Seen;
  std::set<va_t> Targets;
  Targets.insert(CallbackRoots.begin(), CallbackRoots.end());
  Pending.insert(Pending.end(), CallbackRoots.begin(), CallbackRoots.end());
  while (!Pending.empty()) {
    const auto Entry = Pending.back();
    Pending.pop_back();
    if (!Seen.insert(Entry).second)
      continue;
    const auto Found = Low.find(Entry);
    if (Found == Low.end()) {
      if (Evidence) {
        Evidence->MissingFunctions.insert(Entry);
        Evidence->InventoryComplete = false;
      }
      continue;
    }
    for (const auto &Block : Found->second->Blocks)
      for (const auto &Operation : Block.Ops) {
        if (Evidence && (Operation.Opcode == NdOp::CALL ||
                         Operation.Opcode == NdOp::INDIR_CALL)) {
          const bool Direct = Operation.Opcode == NdOp::CALL &&
                              Operation.NumInputs &&
                              Operation.Inputs[0].isConst();
          Evidence->TargetsComplete &= Direct;
          if (Evidence->Calls.size() < NativeSourceDependencyEvidence::MaxCalls)
            Evidence->Calls.push_back({Entry, Block.StartAddr, Operation.Addr,
                                       Direct ? Operation.Inputs[0].Offset : 0,
                                       !Direct});
          else
            Evidence->InventoryComplete = false;
        }
        if (Operation.Opcode == NdOp::CALL && Operation.NumInputs &&
            Operation.Inputs[0].isConst() &&
            Image.isCodeAddress(Operation.Inputs[0].Offset)) {
          const va_t Target = Operation.Inputs[0].Offset;
          Targets.insert(Target);
          Pending.push_back(Target);
        }
      }
  }
  if (Evidence)
    Evidence->TargetsComplete &= Evidence->InventoryComplete;
  return Targets;
}

/// Symbols never create a callee or an ABI declaration. Accepted candidates
/// must be re-lifted with their explicit ABI before they can become evidence.
inline size_t inferObjCNativeDependencies(
    const BinaryImage &Image, const PipelineResult &Result,
    PipelineOptions &Options, std::map<va_t, std::string> &Diagnostics,
    const std::set<va_t> &CallbackRoots = {},
    const std::set<va_t> &CallOnlyTargets = {},
    const std::map<va_t, HighFunc> *SourceRefinements = nullptr) {
  const auto Targets =
      walkObjCNativeDependencies(Image, Result, nullptr, CallbackRoots);
  std::map<va_t, const LowFunc *> Low;
  std::map<va_t, const MedFunc *> Med;
  std::map<va_t, const HighFunc *> High;
  std::map<va_t, const PipelineFunctionAudit *> Audits;
  std::set<va_t> IntegerPairReturns;
  for (const auto &Function : Result.LowFuncs) {
    Low.emplace(Function.Entry, &Function);
    const auto Observed =
        observedNativeIntegerPairReturns(Function, Image.Arch);
    IntegerPairReturns.insert(Observed.begin(), Observed.end());
  }
  for (const auto &Function : Result.MedFuncs)
    Med.emplace(Function.Entry, &Function);
  for (const auto &Function : Result.HighFuncs)
    High.emplace(Function.Entry, &Function);
  for (const auto &Audit : Result.FunctionAudits)
    Audits.emplace(Audit.Entry, &Audit);
  size_t Added = 0;
  for (va_t Target : Targets) {
    // Some compiler thunks expose a source-level callee contract which is
    // intentionally narrower than the machine body's live-in contract. Their
    // call-only hint must not be promoted back into an entry SourceTypeHint.
    if (CallOnlyTargets.count(Target))
      continue;
    if (const auto Existing = Options.SourceTypeHints.find(Target);
        Existing != Options.SourceTypeHints.end()) {
      const auto Found = High.find(Target);
      const HighFunc *RefinementFunction =
          Found == High.end() ? nullptr : Found->second;
      if (SourceRefinements)
        if (const auto Bound = SourceRefinements->find(Target);
            Bound != SourceRefinements->end())
          RefinementFunction = &Bound->second;
      const auto M = Med.find(Target);
      const auto A = Audits.find(Target);
      if (IntegerPairReturns.count(Target) && Found != High.end() &&
          M != Med.end() && A != Audits.end())
        if (auto Pair = refineNativeIntegerPairReturnHint(
                *M->second, *Found->second, *A->second)) {
          Existing->second = std::move(*Pair);
          ++Added;
          continue;
        }
      if (RefinementFunction && A != Audits.end())
        if (auto Refined =
                refineNativeSourceTypeHint(*RefinementFunction, *A->second)) {
          Existing->second = std::move(*Refined);
          ++Added;
        }
      continue;
    }
    const auto M = Med.find(Target);
    const auto L = Low.find(Target);
    const auto H = High.find(Target);
    const auto A = Audits.find(Target);
    if (L == Low.end() || M == Med.end() || H == High.end() ||
        A == Audits.end()) {
      Diagnostics[Target] = "native callee has no complete pipeline evidence";
      continue;
    }
    if (M->second->SourceTypeHint || H->second->SourceTypeHint)
      continue;
    auto Hint = inferNativeSourceTypeHint(
        Image, *M->second, *H->second, *A->second, Diagnostics[Target],
        L->second, IntegerPairReturns.count(Target));
    if (Hint) {
      Options.SourceTypeHints.emplace(Target, std::move(*Hint));
      ++Added;
    }
  }
  return Added;
}
} // namespace neverd::sdk
#endif
