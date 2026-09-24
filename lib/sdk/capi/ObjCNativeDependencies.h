#ifndef NEVERD_SDK_CAPI_OBJCNATIVEDEPENDENCIES_H
#define NEVERD_SDK_CAPI_OBJCNATIVEDEPENDENCIES_H

#include "SwiftMangledSourceABI.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

#include <map>
#include <optional>
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

// Propagate demand only across an exact direct tail forwarder. This selects a
// callee for a later full two-register definition proof; it does not declare
// either function's ABI or certify its body.
inline std::optional<va_t>
forwardedNativeIntegerPairTarget(const BinaryImage &Image,
                                 const LowFunc &Function) {
  if ((Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Function.Blocks.size() != 1 || Function.Blocks[0].Ops.size() < 2)
    return std::nullopt;
  const auto &Ops = Function.Blocks[0].Ops;
  const auto &Call = Ops[Ops.size() - 2];
  const auto &Return = Ops.back();
  const auto &TRI = getTargetRegInfo(Image.Arch);
  if (Call.Opcode != NdOp::CALL || Call.NumInputs != 1 ||
      !Call.Inputs[0].isConst() || Call.Inputs[0].Size != 8 ||
      !Image.isCodeAddress(Call.Inputs[0].Offset) ||
      Call.Inputs[0].Offset == Function.Entry || !Call.Output.isReg() ||
      Call.Output.Offset != TRI.IntReturnReg || Call.Output.Size != 8 ||
      Return.Opcode != NdOp::RETURN || Return.NumInputs != 1 ||
      !Return.Inputs[0].isReg() ||
      Return.Inputs[0].Offset != TRI.IntReturnReg ||
      Return.Inputs[0].Size != 8 || Return.Addr != Call.Addr)
    return std::nullopt;
  return Call.Inputs[0].Offset;
}

/// Symbols never create a callee or an ABI declaration. Accepted candidates
/// must be re-lifted with their explicit ABI before they can become evidence.
inline size_t inferObjCNativeDependencies(
    const BinaryImage &Image, const PipelineResult &Result,
    PipelineOptions &Options, std::map<va_t, std::string> &Diagnostics,
    const std::set<va_t> &CallbackRoots = {},
    const std::set<va_t> &CallOnlyTargets = {},
    const std::map<va_t, HighFunc> *SourceRefinements = nullptr,
    const NativeSourceCalleeContracts *CalleeContracts = nullptr) {
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
  std::vector<va_t> PairDemand(IntegerPairReturns.begin(),
                               IntegerPairReturns.end());
  for (size_t Index = 0; Index < PairDemand.size(); ++Index) {
    const auto Found = Low.find(PairDemand[Index]);
    if (Found == Low.end())
      continue;
    const auto Forward =
        forwardedNativeIntegerPairTarget(Image, *Found->second);
    if (Forward && Low.count(*Forward) &&
        IntegerPairReturns.insert(*Forward).second)
      PairDemand.push_back(*Forward);
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
          if (!equalSourceABIs(Existing->second, *Pair)) {
            Existing->second = std::move(*Pair);
            ++Added;
          }
          continue;
        }
      if (RefinementFunction && A != Audits.end())
        if (auto Refined =
                refineNativeSourceTypeHint(*RefinementFunction, *A->second)) {
          if (!equalSourceABIs(Existing->second, *Refined)) {
            Existing->second = std::move(*Refined);
            ++Added;
          }
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
    // A closed, compiler-observed Swift function shape supplies its own
    // source ABI. Native scalar inference must not erase its second return
    // word or reinterpret its stack and swiftcc argument carriers.
    const bool CompleteMangledAudit =
        A->second->Entry == Target &&
        A->second->Disposition == PipelineFunctionDisposition::Accepted &&
        A->second->HasLowIR && A->second->HasMedIR &&
        A->second->MedIRVerified && A->second->DecodedInstructions &&
        A->second->DecodedInstructions == A->second->LiftedInstructions &&
        A->second->DecodeFailures.empty() &&
        A->second->UnsupportedInstructions.empty() &&
        A->second->TruncatedPaths.empty();
    if (CompleteMangledAudit) {
      if (auto Mangled = swiftMangledStringBundleSourceABI(
              Image, Target, IntegerPairReturns.count(Target))) {
        Options.SourceTypeHints.emplace(Target, std::move(*Mangled));
        ++Added;
        continue;
      }
      if (auto Mangled = swiftMangledObjCBoolMemberSourceABI(Image, Target)) {
        Options.SourceTypeHints.emplace(Target, std::move(*Mangled));
        ++Added;
        continue;
      }
      if (auto Mangled =
              swiftMangledZeroArgClassInitializerSourceABI(Image, Target)) {
        Options.SourceTypeHints.emplace(Target, std::move(*Mangled));
        ++Added;
        continue;
      }
    }
    auto Hint = inferNativeSourceTypeHint(
        Image, *M->second, *H->second, *A->second, Diagnostics[Target],
        L->second, IntegerPairReturns.count(Target), CalleeContracts);
    if (Hint) {
      Options.SourceTypeHints.emplace(Target, std::move(*Hint));
      ++Added;
    }
  }
  return Added;
}
} // namespace neverd::sdk
#endif
