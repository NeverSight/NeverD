//===- Safety.cpp - Memory-safety audit and hunt entry points ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/Safety.h"

#include "ProcessInputReplay.h"
#include "SafetyJSON.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/safety/InterprocReachability.h"
#include "neverd/safety/SafetyScan.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <exception>
#include <new>
#include <set>
#include <tuple>
#include <unordered_map>

using namespace neverd;
using namespace neverd::safety;

namespace {

void describeImage(const AnalysisInput &In, SafetyReport &R) {
  if (!In.Img)
    return;
  R.Format = In.Img->getFormatName();
  R.Arch = getArchName(In.Img->Arch);
}

std::optional<std::string> checkPipelineCoverage(const PipelineResult &Result,
                                                 const BinaryImage *Img,
                                                 bool &BudgetHit);

std::optional<std::string> validateAnalysisInput(const AnalysisInput &In,
                                                 bool &BudgetHit) {
  if (!In.Img)
    return "safety analysis requires a loaded image";
  if (!In.MedFuncs || In.MedFuncs->empty())
    return "safety analysis requires recovered MedIR";
  if (!In.LowFuncs || In.LowFuncs->empty())
    return "safety analysis requires recovered LowIR";
  if (!In.ValidatedPipeline)
    return "safety analysis requires validated pipeline coverage";
  if (In.ValidatedPipeline->SourceImage != In.Img)
    return "safety analysis received pipeline artifacts from another image";
  if (&In.ValidatedPipeline->MedFuncs != In.MedFuncs ||
      &In.ValidatedPipeline->LowFuncs != In.LowFuncs)
    return "safety analysis received detached pipeline artifacts";
  if (std::optional<std::string> Error =
          checkPipelineCoverage(*In.ValidatedPipeline, In.Img, BudgetHit))
    return Error;
  for (const LowFunc &F : *In.LowFuncs)
    if (!F.hasCompleteLiftCoverage())
      return "safety analysis received incomplete LowIR coverage";
  return std::nullopt;
}

std::optional<std::string> checkPipelineCoverage(const PipelineResult &Result,
                                                 const BinaryImage *Img,
                                                 bool &BudgetHit) {
  if (Img && Result.SourceImage != Img)
    return "safety pipeline artifacts do not belong to the supplied image";
  using Disposition = PipelineFunctionDisposition;
  std::set<va_t> AcceptedEntries;
  std::set<va_t> RemovedJumpTableEntries;
  std::set<va_t> AbsorbedChunkEntries;
  for (const PipelineFunctionAudit &Audit : Result.FunctionAudits) {
    bool Incomplete = false;
    switch (Audit.Disposition) {
    case Disposition::Candidate:
    case Disposition::SkippedLimit:
    case Disposition::RejectedLowIR:
    case Disposition::RejectedIncomplete:
    case Disposition::RejectedUnwindlessNonLeaf:
    case Disposition::MedIRFailed:
      Incomplete = true;
      break;
    case Disposition::Accepted:
      if (!AcceptedEntries.insert(Audit.Entry).second)
        return "incomplete safety lift inventory";
      Incomplete = !Audit.HasLowIR || !Audit.HasMedIR || !Audit.MedIRVerified ||
                   !Audit.DecodeFailures.empty() ||
                   !Audit.UnsupportedInstructions.empty() ||
                   !Audit.TruncatedPaths.empty() ||
                   Audit.LiftedInstructions != Audit.DecodedInstructions;
      break;
    case Disposition::SkippedImportStub:
    case Disposition::SkippedRuntimeScaffold:
      break;
    case Disposition::RemovedJumpTableTarget:
      if (!RemovedJumpTableEntries.insert(Audit.Entry).second)
        return "incomplete safety lift inventory";
      break;
    case Disposition::AbsorbedFunctionChunk:
      if (!AbsorbedChunkEntries.insert(Audit.Entry).second)
        return "incomplete safety lift inventory";
      break;
    }
    if (!Incomplete)
      continue;
    BudgetHit = Audit.Disposition == Disposition::SkippedLimit;
    std::string Error =
        "incomplete safety lift at 0x" + llvm::utohexstr(Audit.Entry) + " (" +
        pipelineFunctionDispositionName(Audit.Disposition) + ")";
    if (Audit.Disposition == Disposition::MedIRFailed &&
        !Audit.Diagnostic.empty())
      Error += " " + llvm::json::fixUTF8(Audit.Diagnostic);
    return Error;
  }
  std::set<va_t> LowEntries;
  for (const LowFunc &F : Result.LowFuncs) {
    if (!LowEntries.insert(F.Entry).second)
      return "incomplete safety lift inventory";
    std::set<int> BlockIds;
    for (const LowBlock &Block : F.Blocks)
      if (!BlockIds.insert(Block.Id).second)
        return "incomplete safety lift at 0x" + llvm::utohexstr(F.Entry) +
               " (duplicate-block-identity)";
  }
  std::unordered_map<va_t, const MedFunc *> MedByEntry;
  MedByEntry.reserve(Result.MedFuncs.size());
  for (const MedFunc &F : Result.MedFuncs) {
    if (!MedByEntry.try_emplace(F.Entry, &F).second)
      return "incomplete safety lift inventory";
    std::set<int> BlockIds;
    for (const MedBlock &Block : F.Blocks)
      if (!BlockIds.insert(Block.Id).second)
        return "incomplete safety lift at 0x" + llvm::utohexstr(F.Entry) +
               " (duplicate-block-identity)";
  }
  if (AcceptedEntries.empty() || AcceptedEntries != LowEntries ||
      AcceptedEntries.size() != MedByEntry.size() ||
      !std::all_of(AcceptedEntries.begin(), AcceptedEntries.end(),
                   [&](va_t Entry) { return MedByEntry.contains(Entry); }))
    return "incomplete safety lift inventory";
  // An absorbed chunk must really be lifted inside an accepted function and
  // must not be a function the image itself names.
  for (va_t Chunk : AbsorbedChunkEntries) {
    if (AcceptedEntries.count(Chunk) != 0)
      return "incomplete safety lift inventory";
    if (Img && Img->hasAuthenticatedFunctionEntryAt(Chunk))
      return "incomplete safety lift at 0x" + llvm::utohexstr(Chunk) +
             " (absorbed-function-chunk)";
    const bool Lifted = std::any_of(
        Result.LowFuncs.begin(), Result.LowFuncs.end(), [&](const LowFunc &F) {
          return std::any_of(
              F.Blocks.begin(), F.Blocks.end(),
              [&](const LowBlock &Block) { return Block.StartAddr == Chunk; });
        });
    if (!Lifted)
      return "incomplete safety lift at 0x" + llvm::utohexstr(Chunk) +
             " (absorbed-function-chunk)";
  }
  for (va_t RemovedEntry : RemovedJumpTableEntries) {
    if (AcceptedEntries.count(RemovedEntry) != 0)
      return "incomplete safety lift inventory";
    if (Img && Img->hasAuthenticatedFunctionEntryAt(RemovedEntry))
      return "incomplete safety lift at 0x" + llvm::utohexstr(RemovedEntry) +
             " (removed-jump-table-target)";

    const auto canonical = [&](va_t Address) {
      return Img ? normalizeCodeAddress(Address, Img->Arch, Img->Mode)
                 : Address;
    };
    const va_t CanonicalEntry = canonical(RemovedEntry);
    bool IsPublishedInteriorTarget = false;
    for (const LowFunc &F : Result.LowFuncs) {
      const bool IsTableTarget = std::any_of(
          F.JumpTables.begin(), F.JumpTables.end(),
          [&](const JumpTable &Table) {
            return !Table.MutatedUnsafe &&
                   std::any_of(
                       Table.Targets.begin(), Table.Targets.end(),
                       [&](va_t T) { return canonical(T) == CanonicalEntry; });
          });
      if (!IsTableTarget)
        continue;
      for (const LowBlock &Block : F.Blocks)
        if (std::any_of(Block.InstructionBoundaries.begin(),
                        Block.InstructionBoundaries.end(),
                        [&](const LowInstructionBoundary &Boundary) {
                          return canonical(Boundary.Address) == CanonicalEntry;
                        })) {
          IsPublishedInteriorTarget = true;
          break;
        }
      if (IsPublishedInteriorTarget)
        break;
    }
    if (!IsPublishedInteriorTarget)
      return "incomplete safety lift at 0x" + llvm::utohexstr(RemovedEntry) +
             " (removed-jump-table-target)";
  }
  for (const MedFunc &F : Result.MedFuncs) {
    std::set<std::pair<int, int>> CallInfos;
    for (const MedCallInfo &Call : F.CallInfos) {
      const std::pair<int, int> Key{Call.BlockId, Call.OpIdx};
      if (!CallInfos.insert(Key).second)
        return "incomplete safety lift at 0x" + llvm::utohexstr(F.Entry) +
               " (incomplete-call-inventory)";
      const MedOp *CallOp = nullptr;
      for (const MedBlock &Block : F.Blocks)
        if (Block.Id == Call.BlockId && Call.OpIdx >= 0 &&
            static_cast<size_t>(Call.OpIdx) < Block.Ops.size()) {
          CallOp = &Block.Ops[static_cast<size_t>(Call.OpIdx)];
          break;
        }
      if (!CallOp ||
          (CallOp->Opcode != NdOp::CALL &&
           CallOp->Opcode != NdOp::INDIR_CALL) ||
          Call.IsIndirect != (CallOp->Opcode == NdOp::INDIR_CALL) ||
          (!Call.IsIndirect &&
           (CallOp->NumInputs == 0 || !CallOp->Inputs[0].isConst() ||
            Call.TargetAddr != CallOp->Inputs[0].ConstVal)))
        return "incomplete safety lift at 0x" + llvm::utohexstr(F.Entry) +
               " (incomplete-call-inventory)";
    }
    for (const MedBlock &Block : F.Blocks)
      for (size_t OpIdx = 0; OpIdx < Block.Ops.size(); ++OpIdx) {
        const MedOp &Op = Block.Ops[OpIdx];
        if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
            CallInfos.count({Block.Id, static_cast<int>(OpIdx)}) == 0)
          return "incomplete safety lift at 0x" + llvm::utohexstr(Op.Addr) +
                 " (incomplete-call-inventory)";
      }
  }
  for (const LowFunc &Low : Result.LowFuncs) {
    const MedFunc &Med = *MedByEntry.at(Low.Entry);
    using CallIdentity = std::tuple<va_t, int, unsigned, va_t>;
    std::vector<CallIdentity> LowCalls;
    std::vector<CallIdentity> MedCalls;
    std::set<std::pair<va_t, int>> LowCallOccurrences;
    std::set<std::pair<va_t, int>> MedCallOccurrences;
    for (const LowBlock &Block : Low.Blocks)
      for (const LowOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          if (Op.Seq < 0)
            return "incomplete safety lift at 0x" + llvm::utohexstr(Op.Addr) +
                   " (missing-call-provenance)";
          if (!LowCallOccurrences.insert({Op.Addr, Op.Seq}).second)
            return "incomplete safety lift at 0x" + llvm::utohexstr(Op.Addr) +
                   " (duplicate-call-provenance)";
          if (Op.Opcode == NdOp::CALL &&
              (Op.NumInputs == 0 || !Op.Inputs[0].isConst()))
            return "incomplete safety lift at 0x" + llvm::utohexstr(Op.Addr) +
                   " (incomplete-call-inventory)";
          va_t Target = 0;
          if (Op.Opcode == NdOp::CALL) {
            Target = Op.Inputs[0].Offset;
            // ELFv2 can branch to a symbol's local entry. MedIR names the
            // same callee by its global entry after verifying the call ABI.
            if (Img)
              Target = Img->ppc64GlobalEntry(Target);
          }
          LowCalls.emplace_back(Op.Addr, Op.Seq,
                                static_cast<unsigned>(Op.Opcode), Target);
        }
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          if (Op.OriginSeq < 0)
            return "incomplete safety lift at 0x" + llvm::utohexstr(Op.Addr) +
                   " (missing-call-provenance)";
          if (!MedCallOccurrences.insert({Op.Addr, Op.OriginSeq}).second)
            return "incomplete safety lift at 0x" + llvm::utohexstr(Op.Addr) +
                   " (duplicate-call-provenance)";
          if (Op.Opcode == NdOp::CALL &&
              (Op.NumInputs == 0 || !Op.Inputs[0].isConst()))
            return "incomplete safety lift at 0x" + llvm::utohexstr(Op.Addr) +
                   " (incomplete-call-inventory)";
          const va_t Target =
              Op.Opcode == NdOp::CALL ? Op.Inputs[0].ConstVal : 0;
          MedCalls.emplace_back(Op.Addr, Op.OriginSeq,
                                static_cast<unsigned>(Op.Opcode), Target);
        }
    std::sort(LowCalls.begin(), LowCalls.end());
    std::sort(MedCalls.begin(), MedCalls.end());
    if (LowCalls != MedCalls) {
      const va_t CallVA =
          LowCalls.empty() ? Low.Entry : std::get<0>(LowCalls.front());
      return "incomplete safety lift at 0x" + llvm::utohexstr(CallVA) +
             " (incomplete-call-inventory)";
    }
  }
  for (const LowFunc &F : Result.LowFuncs) {
    std::set<va_t> ResolvedIndirectBranches;
    for (const JumpTable &Table : F.JumpTables)
      if (!Table.MutatedUnsafe && !Table.Targets.empty())
        ResolvedIndirectBranches.insert(Table.InsnAddr);
    for (const LowBlock &Block : F.Blocks)
      for (const LowOp &Op : Block.Ops) {
        if (Op.Opcode == NdOp::INDIR_BR &&
            ResolvedIndirectBranches.count(Op.Addr) == 0)
          return "incomplete safety lift at 0x" + llvm::utohexstr(Op.Addr) +
                 " (unresolved-indirect-branch)";
        // This operation returns a physical address selected by the historical
        // MIPS MMU. The static scan has no translation state for that access.
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].isConst() &&
            Op.Inputs[0].Offset ==
                static_cast<uint64_t>(Intrinsic::MIPSHistoricalAddressCheck))
          return "incomplete safety lift at 0x" + llvm::utohexstr(Op.Addr) +
                 " (unresolved-memory-translation)";
      }
  }
  for (const MedFunc &F : Result.MedFuncs)
    for (const MedCallInfo &Call : F.CallInfos) {
      if (!Call.IsIndirect)
        continue;
      va_t CallVA = F.Entry;
      for (const MedBlock &Block : F.Blocks)
        if (Block.Id == Call.BlockId && Call.OpIdx >= 0 &&
            static_cast<size_t>(Call.OpIdx) < Block.Ops.size()) {
          CallVA = Block.Ops[static_cast<size_t>(Call.OpIdx)].Addr;
          break;
        }
      if (Call.TargetAddr == 0)
        return "incomplete safety lift at 0x" + llvm::utohexstr(CallVA) +
               " (unresolved-indirect-call)";
      if (!Img)
        continue;
      const va_t Target =
          normalizeCodeAddress(Call.TargetAddr, Img->Arch, Img->Mode);
      const Segment *TargetSegment = Img->getSegmentFor(Target);
      if (!TargetSegment || !TargetSegment->isExecutable() ||
          Img->isImportStubAt(Target) || AcceptedEntries.count(Target) != 0)
        continue;
      return "incomplete safety lift at 0x" + llvm::utohexstr(Target) +
             " (unresolved-internal-call)";
    }
  if (Img)
    for (const LowFunc &F : Result.LowFuncs)
      for (const LowBlock &Block : F.Blocks)
        for (const LowOp &Op : Block.Ops) {
          if (Op.Opcode != NdOp::CALL || Op.NumInputs == 0 ||
              !Op.Inputs[0].isConst())
            continue;
          const va_t Target = Img->ppc64GlobalEntry(
              normalizeCodeAddress(Op.Inputs[0].Offset, Img->Arch, Img->Mode));
          const Segment *TargetSegment = Img->getSegmentFor(Target);
          if (!TargetSegment || !TargetSegment->isExecutable() ||
              Img->isImportStubAt(Target) || AcceptedEntries.count(Target) != 0)
            continue;
          return "incomplete safety lift at 0x" + llvm::utohexstr(Target) +
                 " (unresolved-internal-call)";
        }
  return std::nullopt;
}

void analyzeTrack(AnalyzerResult &Result, const AnalysisInput &In,
                  const SinkCatalog &Cat, const SafetyBudgets &Budgets,
                  const InterprocResult &Interproc,
                  const std::vector<SinkSite> &Sites,
                  llvm::function_ref<bool(Track)> Canceled) {
  SafetyReport &R = Result.Report;
  auto stopped = [&] {
    if (Canceled && Canceled(R.Origin)) {
      Result.State = ScanState::Canceled;
      return true;
    }
    return false;
  };
  if (stopped())
    return;
  R.BudgetHit = Interproc.BudgetHit;
  if (R.Origin == Track::Audit) {
    for (const SinkSite &Site : Sites)
      if (Site.Kind == SinkKind::Alloc || Site.Kind == SinkKind::Realloc)
        ++R.Scanned;
    R.Findings = auditMemory(In, Cat, Budgets, stopped);
    for (Finding &F : R.Findings) {
      Interproc.annotate(F);
      R.BudgetHit |= F.BudgetHit;
    }
  } else {
    for (const SinkSite &Site : Sites) {
      if (stopped())
        break;
      const MedFunc *F = In.findMedFunc(Site.FuncEntry);
      if (!F)
        continue;
      auto Found = huntSink(In, Cat, Budgets, *F, Site);
      if (!Found)
        continue;
      ++R.Scanned;
      if (!Found->SkipReason.empty())
        ++R.Skipped;
      R.BudgetHit |= Found->BudgetHit;
      Interproc.annotate(*Found);
      R.Findings.push_back(std::move(*Found));
    }
  }
  if (Result.State != ScanState::Canceled) {
    R.AnalysisComplete = true;
    Result.State =
        R.BudgetHit ? ScanState::BudgetExhausted : ScanState::Completed;
  }
}

ScanReport analyze(const AnalysisInput &In, const SinkCatalog &Cat,
                   const SafetyBudgets &Budgets, ScanSelection Selection,
                   llvm::function_ref<bool(Track)> Canceled,
                   bool IsolateFailures) {
  ScanReport Scan(Selection);
  bool BudgetHit = false;
  auto Error = validateAnalysisInput(In, BudgetHit);
  for (auto &Result : Scan.Analyzers) {
    describeImage(In, Result.Report);
    if (Error && Result.State != ScanState::Disabled) {
      Result.Report.Error = *Error;
      if (BudgetHit)
        Result.State = ScanState::BudgetExhausted;
    }
  }
  if (Error)
    return Scan;
  const InterprocResult Interproc = analyzeInterprocedural(In, Cat, Budgets);
  AnalysisInput Effective = In;
  Effective.ParameterFlows = &Interproc.Parameters;
  const auto Sites = scanSinks(Effective, Cat);
  for (auto &Result : Scan.Analyzers) {
    if (Result.State == ScanState::Disabled)
      continue;
    auto Run = [&] {
      analyzeTrack(Result, Effective, Cat, Budgets, Interproc, Sites, Canceled);
    };
    if (!IsolateFailures) {
      Run();
      continue;
    }
    try {
      Run();
    } catch (const std::bad_alloc &) {
      Result.State = ScanState::Failed;
      Result.Report.Error = "analyzer allocation failed";
    } catch (const std::exception &E) {
      Result.State = ScanState::Failed;
      Result.Report.Error = llvm::json::fixUTF8(E.what());
    } catch (...) {
      Result.State = ScanState::Failed;
      Result.Report.Error = "unexpected analyzer failure";
    }
  }
  return Scan;
}
} // namespace

std::optional<std::string>
neverd::safety::validatePipelineCoverage(const PipelineResult &Result,
                                         const BinaryImage *Img) {
  bool BudgetHit = false;
  return checkPipelineCoverage(Result, Img, BudgetHit);
}

ScanReport neverd::safety::runScan(const AnalysisInput &In,
                                   const SinkCatalog &Cat,
                                   const SafetyBudgets &Budgets,
                                   ScanSelection Selection,
                                   llvm::function_ref<bool(Track)> Canceled) {
  return analyze(In, Cat, Budgets, Selection, Canceled, true);
}

SafetyReport neverd::safety::runHunt(const AnalysisInput &In,
                                     const SinkCatalog &Cat,
                                     const SafetyBudgets &Budgets) {
  auto Scan = analyze(In, Cat, Budgets, ScanSelection::Hunt, nullptr, false);
  return std::move(Scan.Analyzers.back().Report);
}

llvm::json::Object neverd::safety::findingJson(const Finding &F) {
  using namespace llvm;

  auto vaHex = [](va_t Addr) { return "0x" + utohexstr(Addr); };
  auto bytesHex = [](const std::vector<uint8_t> &Bytes) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::string Result;
    Result.reserve(2 + Bytes.size() * 2);
    Result = "0x";
    for (uint8_t Byte : Bytes) {
      Result.push_back(Digits[Byte >> 4]);
      Result.push_back(Digits[Byte & 0x0f]);
    }
    return Result;
  };

  json::Object Ev;
  if (!F.SkipReason.empty())
    Ev["skip_reason"] = F.SkipReason;
  if (!F.Constraints.empty())
    Ev["constraints"] = F.Constraints;
  if (!F.Witness.empty()) {
    json::Object Concrete;
    json::Array CandidateValues;
    for (const auto &[K, V] : F.Witness) {
      // Preserve the legacy object without allowing a custom source name to
      // overwrite a derived key such as copy_length.  The array is the
      // lossless representation and also preserves duplicate source names.
      if (Concrete.find(K) == Concrete.end())
        Concrete[K] = V;
      CandidateValues.push_back(json::Object{{"name", K}, {"value", V}});
    }
    Ev["concrete_input"] = std::move(Concrete);
    Ev["candidate_values"] = std::move(CandidateValues);
  }
  if (!F.SymbolicModel.empty()) {
    json::Array Model;
    for (const SolverAssignment &Assignment : F.SymbolicModel)
      Model.push_back(
          json::Object{{"id", int64_t(Assignment.Id)},
                       {"name", Assignment.Name},
                       {"width", int64_t(Assignment.Width)},
                       {"value_hex", Assignment.ValueHex},
                       {"origin", Assignment.Fresh ? "fresh" : "input"}});
    Ev["symbolic_model"] = std::move(Model);
  }
  if (!F.Witness.empty() || !F.SymbolicModel.empty() || F.Replay ||
      !F.ReplayReason.empty()) {
    std::optional<std::string> ReplayError;
    if (F.Replay)
      ReplayError = validateProcessInputReplay(*F.Replay, F.SymbolicModel);
    const bool Replayable = F.Replay.has_value() && !ReplayError;
    Ev["replayable"] = Replayable;

    json::Object Replay{{"adapter", "process-input-v1"}};
    if (Replayable) {
      std::vector<uint32_t> QueryVariables = F.Replay->QueryVariables;
      std::sort(QueryVariables.begin(), QueryVariables.end());
      json::Array SerializedQueryVariables;
      for (uint32_t Id : QueryVariables)
        SerializedQueryVariables.push_back(int64_t(Id));
      Replay["query_variables"] = std::move(SerializedQueryVariables);

      json::Array Inputs;
      for (const ReplayInput &Input : F.Replay->Inputs) {
        json::Object Serialized{
            {"kind", toString(Input.Kind)},
            {"call_va", vaHex(Input.CallVA)},
            {"seq", Input.Seq},
            {"invocation", int64_t(Input.Invocation)},
            {"offset", int64_t(Input.Offset)},
            {"bytes_hex", bytesHex(Input.Bytes)},
            {"eof_after", Input.EOFAfter},
            {"terminator_implicit", Input.TerminatorImplicit}};
        if (Input.Kind == ReplayInputKind::Environment)
          Serialized["name"] = Input.Name;
        if (!Input.Bindings.empty()) {
          json::Array Bindings;
          for (const ReplayBinding &Binding : Input.Bindings)
            Bindings.push_back(
                json::Object{{"assignment_id", int64_t(Binding.AssignmentId)},
                             {"role", toString(Binding.Role)},
                             {"offset", int64_t(Binding.ByteOffset)}});
          Serialized["bindings"] = std::move(Bindings);
        }
        Inputs.push_back(std::move(Serialized));
      }
      Replay["inputs"] = std::move(Inputs);
    } else if (ReplayError) {
      Replay["reason"] = *ReplayError;
    } else if (!F.ReplayReason.empty()) {
      Replay["reason"] = F.ReplayReason;
    } else if (!F.SymbolicModel.empty()) {
      Replay["reason"] =
          "symbolic query variables lack explicit process-input bindings";
    } else {
      Replay["reason"] = "no exact process-input replay plan";
    }
    Ev["replay"] = std::move(Replay);
  }

  json::Object O;
  O["class"] = toString(F.Class);
  O["function"] = F.Function;
  O["function_va"] = vaHex(F.FuncEntry);
  O["name"] = F.Name;
  O["name_source"] = toString(F.Source);
  O["call_va"] = vaHex(F.CallVA);
  if (!F.SourceLoc.empty())
    O["source"] = F.SourceLoc;
  O["sink"] = F.Sink;
  if (F.ArgIndex >= 0)
    O["arg_index"] = F.ArgIndex;
  O["flow"] = toString(F.Flow);
  O["verdict"] = toString(F.TheVerdict);
  O["confidence"] = toString(F.TheConfidence);
  if (F.Capacity) {
    O["capacity"] = *F.Capacity;
    O["capacity_kind"] = F.CapacityKind == CapacityPrecision::TypedBufferExact
                             ? "exact"
                             : "upper_bound";
    O["capacity_precision"] = toString(F.CapacityKind);
  }
  if (!F.Detail.empty())
    O["detail"] = F.Detail;
  if (!F.Corroboration.empty())
    O["corroboration"] = F.Corroboration;
  json::Object Reachability{
      {"status", toString(F.Reachability.Status)},
      {"attacker_control", toString(F.Reachability.AttackerControl)},
      {"budget_hit", F.Reachability.BudgetHit}};
  if (F.Reachability.EntryVA) {
    Reachability["entry"] =
        json::Object{{"va", vaHex(*F.Reachability.EntryVA)},
                     {"name", F.Reachability.EntryName},
                     {"kind", toString(F.Reachability.Kind)}};
  }
  if (!F.Reachability.CallChain.empty()) {
    json::Array Chain;
    for (const ReachabilityCall &Call : F.Reachability.CallChain)
      Chain.push_back(
          json::Object{{"caller_va", vaHex(Call.CallerVA)},
                       {"call_va", vaHex(Call.CallVA)},
                       {"callee_va", vaHex(Call.CalleeVA)},
                       {"kind", Call.Indirect ? "indirect" : "direct"}});
    Reachability["call_chain"] = std::move(Chain);
  }
  if (!F.Reachability.Reason.empty())
    Reachability["reason"] = F.Reachability.Reason;
  O["reachability"] = std::move(Reachability);
  O["evidence"] = std::move(Ev);
  return O;
}
