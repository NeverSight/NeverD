//===- PipelineMedIR.cpp - MedIR pipeline stage --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LowIR-to-MedIR conversion for the decompilation pipeline.
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"
#include "PipelineIncomingStack.h"
#include "PipelineLowIRDetail.h"
#include "PipelineMedAudit.h"
#include "PipelineRegisterCalls.h"
#include "PipelineSEHRuntimeMemory.h"
#include "PipelineSourceStackAccesses.h"
#include "PipelineTrimStorage.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedCallReturnReads.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Diagnostic.h"
#include "neverd/support/Parallel.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

MedStackProjection medStackProjection(const PipelineOptions &Options) {
  return !Options.MedAnalysisOnly && !Options.PatchMode &&
                 (!Options.LiftMode || Options.SourceProjection)
             ? MedStackProjection::Source
             : MedStackProjection::Machine;
}

bool refusalFailsRun(const PipelineOptions &Options) {
  return !Options.OnlyFunctionEntries.empty() && !Options.OmitRefusedFunctions;
}

void prepareMedStackAccesses(MedFunc &Func, const LowFunc &Source,
                             const BinaryImage &Image,
                             MedStackProjection Projection,
                             const MedCallReturnRanges *CallReturns,
                             bool WaitForCalleeFacts,
                             bool CallFreeMachineSource) {
  Func.EntryKind = functionEntryKind(Func, Image.stackEntryKindAt(Func.Entry));
  if (Projection != MedStackProjection::Source)
    return;
  simplifyProvenStackAlignment(Func, Image.Arch, Image.abiFormat(),
                               Func.EntryKind);
  // Loads of CALL return slots wait for settled calls and returns
  // (refuseObservedCallReturnReads); other refusals are final now.
  std::string Diagnostic;
  Func.CallReturnReads.clear();
  bool NeedsCalleeFacts = false;
  if (!CallFreeMachineSource &&
      !preserveSourceCallReturnWrites(
          Func, Image, Diagnostic, limits::ir::kSourceCallReturnReadWork,
          &Source, CallReturns, &Func.CallReturnReads,
          WaitForCalleeFacts ? &NeedsCalleeFacts : nullptr))
    throw LowToMedConversionError(Diagnostic);
  // A callee's return range may bound the dynamic stack below such a CALL and
  // prove the load. Retry with those facts instead of deferring it now.
  if (NeedsCalleeFacts && !Func.CallReturnReads.empty())
    throw LowToMedConversionError(
        "source frame in " + Func.Name +
        ": a CALL return-slot proof waits for callee return ranges");
  markSourceStackAccesses(Func, Image.Arch, Image.abiFormat(), Func.EntryKind,
                          &Image);
}

MedCallReturnRanges sourceCallReturnRanges(const BinaryImage &Image,
                                           const PipelineResult &Result) {
  MedCallReturnRanges Ranges;
  if (!getTargetRegInfo(Image.Arch).ReturnOperandIsValue)
    return Ranges;
  const size_t Total = Result.LowFuncs.size();
  std::vector<std::optional<MedIntegerReturnRange>> Proven(Total);
  std::vector<uint64_t> Weight(Total, 1);
  for (size_t I = 0; I < Total; ++I)
    for (const auto &Block : Result.LowFuncs[I].Blocks)
      Weight[I] += Block.Ops.size();
  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    for (size_t I; (I = Claim()) < N;)
      Proven[I] =
          integerReturnRange(Result.MedFuncs[I], Result.LowFuncs[I], Image);
  });
  for (size_t I = 0; I < Total; ++I)
    if (Proven[I])
      Ranges.try_emplace(Result.LowFuncs[I].Entry, *Proven[I]);
  return Ranges;
}

void prepareRecoveredSourceStackAccesses(const BinaryImage &Image,
                                         PipelineResult &Result,
                                         llvm::ArrayRef<LowFunc> Originals) {
  std::map<va_t, const MedFunc *> Callees;
  std::set<va_t> Complete;
  std::map<va_t, llvm::ArrayRef<LowBlock>> OriginalBlocks;
  for (const LowFunc &Func : Originals)
    if (Func.hasCompleteLiftCoverage() &&
        Func.UnsafeIndirectBranchAddresses.empty()) {
      Complete.insert(Func.Entry);
      if (!Func.Blocks.empty() && Func.Blocks.front().StartAddr == Func.Entry &&
          Func.Blocks.front().Preds.empty())
        OriginalBlocks.emplace(Func.Entry, Func.Blocks);
    }
  if (Result.OmittedCallees)
    for (const auto &[Entry, Blocks] :
         Result.OmittedCallees->SourceOriginalBlocks)
      if (Blocks)
        OriginalBlocks.try_emplace(Entry, *Blocks);
  for (const MedFunc &Func : Result.MedFuncs)
    if (Complete.count(Func.Entry))
      Callees.emplace(Func.Entry, &Func);
  if (Result.OmittedCallees)
    for (const auto &Func : Result.OmittedCallees->Bodies)
      Callees.try_emplace(Func->Entry, Func.get());
  for (MedFunc &Func : Result.MedFuncs) {
    if (Complete.count(Func.Entry))
      markUnobservedBorrowedFrameStores(Func, Image.Arch, Image.abiFormat(),
                                        Callees);
    markUndefinedScratchStores(Func, Image.Arch, Image.abiFormat());
  }
  if (Image.Arch != Arch::X64 || Image.abiFormat() != BinaryFormat::COFF)
    return;
  std::set<va_t> Referenced;
  std::vector<va_t> Pending;
  const auto Reference = [&](va_t Target) {
    if (!Result.CallPrototypeArguments.count(Target) &&
        !Result.CallUserPrototypes.count(Target) &&
        Referenced.insert(Target).second)
      Pending.push_back(Target);
  };
  for (const auto &Func : Result.MedFuncs)
    for (const auto &Call : Func.CallInfos)
      if (!Call.IsIndirect && !Call.SourceCallHint &&
          !Call.ArgumentsInCalleeRegisterOrder)
        Reference(Call.TargetAddr);
  // Follow only bodies already admitted by the bounded signature reader.
  // Reading a callee's source inputs never starts an unbounded image walk.
  for (size_t I = 0; I < Pending.size(); ++I)
    if (const auto At = Callees.find(Pending[I]); At != Callees.end())
      for (const auto &Block : At->second->Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL && Op.NumInputs &&
              Op.Inputs[0].isConst() && !Op.SourceCallHint)
            Reference(Op.Inputs[0].ConstVal);
  std::vector<const MedFunc *> Sources;
  std::vector<uint64_t> Weights;
  const auto Arguments =
      getTargetRegInfo(Image.Arch).integerArgumentLayout(Image.abiFormat());
  for (const auto &[Entry, Func] : Callees) {
    if (!Referenced.count(Entry))
      continue;
    bool Candidate = Func->Params.size() > Arguments.Registers.size();
    uint64_t Weight = 1;
    for (const auto &Block : Func->Blocks) {
      Weight += Block.Ops.size();
      for (const auto &Op : Block.Ops) {
        Candidate |= forwardsSourceEntryRegister(Op, Arguments);
        if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2)
          Candidate |= Op.Inputs[1].Kind == MedVar::Reg &&
                       Op.Inputs[1].SSAVer == 0 &&
                       Arguments.registerIndex(Op.Inputs[1].RegOff) >= 0;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
          Candidate |=
              Op.Inputs[0].Kind == MedVar::Reg && Op.Inputs[0].SSAVer == 0 &&
              Op.Inputs[0].Size < getTargetRegInfo(Image.Arch).PointerSize &&
              Arguments.registerIndex(Op.Inputs[0].RegOff) >= 0;
      }
    }
    if (Candidate) {
      Sources.push_back(Func);
      Weights.push_back(Weight);
    }
  }
  // Cached bodies are immutable. Prepare each candidate once; independent
  // functions and each dependency wave run in weighted parallel batches.
  std::vector<std::optional<MedFunc>> Prepared(Sources.size());
  std::vector<std::set<const MedOp *>> OverwrittenSpills(Sources.size());
  parallelForEachWeighted(Weights, [&](auto Claim, size_t N) {
    for (size_t I; (I = Claim()) < N;) {
      const auto &Source = *Sources[I];
      auto &Func = Prepared[I].emplace(Source);
      markSourceStackAccesses(Func, Image.Arch, Image.abiFormat(),
                              Func.EntryKind, &Image, &OverwrittenSpills[I]);
      // Selected bodies also carry this phase's complete borrowed-frame proof.
      // Keep that source fact, but never promote it into an overwrite receipt.
      if (Complete.count(Source.Entry))
        for (size_t B = 0; B < Source.Blocks.size(); ++B)
          for (size_t O = 0; O < Source.Blocks[B].Ops.size(); ++O)
            Func.Blocks[B].Ops[O].PrologueSave |=
                Source.Blocks[B].Ops[O].PrologueSave;
      Sources[I] = &Func;
    }
  });
  std::map<va_t, std::set<size_t>> Callers;
  for (size_t I = 0; I < Sources.size(); ++I)
    for (const auto &Block : Sources[I]->Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst())
          Callers[Op.Inputs[0].ConstVal].insert(I);
  std::vector<size_t> Work;
  for (size_t I = 0; I < Sources.size(); ++I)
    Work.push_back(I);
  std::vector<size_t> Remaining(Sources.size(),
                                limits::ir::kSourceEntrySpillWork);
  SourceCalleeParameterReads Facts;
  SourceCalleeParameterOverwrites Overwrites;
  std::set<va_t> DeclaredCallees = Result.CallPrototypeArguments;
  if (Result.OmittedCalleeIntegerArguments)
    for (const auto &[Target, Signature] :
         *Result.OmittedCalleeIntegerArguments)
      DeclaredCallees.insert(Target);
  // Fixed import arity tables also own native stack bounds, without needing
  // a complete typed source prototype. An import rebound to an image routine
  // must use that routine's independently checked contract instead.
  const auto ImportDeclarations = [&](const auto &Arguments) {
    for (const auto &[Target, Count] : Arguments) {
      const auto Own = Result.CallArgumentSources.find(Target);
      if (Count >= 0 && Image.findImportAt(Target) &&
          (Own == Result.CallArgumentSources.end() || Own->second == Target))
        DeclaredCallees.insert(Target);
    }
  };
  ImportDeclarations(Result.CallEntryStackArgs);
  if (Result.OmittedCalleeStackArgs)
    ImportDeclarations(*Result.OmittedCalleeStackArgs);
  for (const auto &[Target, Signature] : Result.CallEntryVariadicArguments)
    DeclaredCallees.erase(Target);
  if (Result.OmittedCalleeVariadicArguments)
    for (const auto &[Target, Signature] :
         *Result.OmittedCalleeVariadicArguments)
      DeclaredCallees.erase(Target);
  std::vector<std::optional<std::set<unsigned>>> StackReads(Sources.size());
  while (!Work.empty()) {
    std::vector<uint64_t> WaveWeights;
    for (size_t I : Work)
      WaveWeights.push_back(Weights[I]);
    std::vector<SourceRegisterParameterReads> Reads(Work.size());
    std::vector<std::set<unsigned>> Destroyed(Work.size());
    parallelForEachWeighted(WaveWeights, [&](auto Claim, size_t N) {
      for (size_t W; (W = Claim()) < N;) {
        const size_t I = Work[W];
        Reads[W] = sourceRegisterParameterReads(*Sources[I], Facts,
                                                Remaining[I], Overwrites);
        if (const auto At = OriginalBlocks.find(Sources[I]->Entry);
            At != OriginalBlocks.end() && !At->second.empty()) {
          // This original-CFG fact is independent of the changing source
          // register summaries. Reuse it across this phase's dependency waves.
          if (!StackReads[I])
            StackReads[I] = unobservedSourceStackParameters(
                *Sources[I], At->second, DeclaredCallees, Remaining[I], &Image);
          for (unsigned P : *StackReads[I])
            Reads[W].emplace(P, 0);
          Destroyed[W] = sourceRegisterInputOverwrites(
              *Sources[I], At->second, Reads[W], Overwrites,
              OverwrittenSpills[I], Remaining[I]);
          // A proved original destruction also rules out a source input.
          // This includes the GS case, whose register proof cannot discard
          // even a transient spill observed by its exceptional cookie check.
          for (unsigned P : Destroyed[W])
            Reads[W].emplace(P, 0);
        }
      }
    });
    std::set<size_t> Next;
    for (size_t W = 0; W < Work.size(); ++W) {
      const va_t Entry = Sources[Work[W]]->Entry;
      auto &Known = Facts[Entry];
      bool Changed = false;
      for (const auto &[P, Bytes] : Reads[W]) {
        auto [At, Added] = Known.emplace(P, Bytes);
        if (Added || Bytes < At->second) {
          At->second = Bytes;
          Changed = true;
        }
      }
      for (unsigned P : Destroyed[W])
        Changed |= Overwrites[Entry].insert(P).second;
      if (Changed)
        if (const auto At = Callers.find(Entry); At != Callers.end())
          for (size_t I : At->second)
            if (Remaining[I])
              Next.insert(I);
    }
    Work.assign(Next.begin(), Next.end());
  }
  std::vector<uint64_t> CallerWeights;
  for (const auto &Func : Result.MedFuncs) {
    uint64_t Weight = 1;
    for (const auto &Block : Func.Blocks)
      Weight += Block.Ops.size();
    CallerWeights.push_back(Weight);
  }
  parallelForEachWeighted(CallerWeights, [&](auto Claim, size_t N) {
    for (size_t I; (I = Claim()) < N;)
      replaceKnownSourceCallArguments(Result.MedFuncs[I], Facts);
  });
}

void omitRefusedFunctions(PipelineResult &Result,
                          const std::map<size_t, std::string> &Refused) {
  if (Refused.empty())
    return;
  std::map<va_t, size_t> Audits;
  for (size_t A = 0; A < Result.FunctionAudits.size(); ++A)
    Audits.try_emplace(Result.FunctionAudits[A].Entry, A);
  for (const auto &[I, Diagnostic] : Refused) {
    // A refused conversion leaves no MedFunc identity; LowIR keeps it.
    const LowFunc &Low = Result.LowFuncs[I];
    const auto [Found, Missing] =
        Audits.try_emplace(Low.Entry, Result.FunctionAudits.size());
    if (Missing) {
      PipelineFunctionAudit Added;
      Added.Entry = Low.Entry;
      Added.Name = Low.Name;
      Added.HasLowIR = true;
      Result.FunctionAudits.push_back(std::move(Added));
    }
    PipelineFunctionAudit &Audit = Result.FunctionAudits[Found->second];
    Audit.Disposition = PipelineFunctionDisposition::MedIRFailed;
    Audit.HasMedIR = false;
    Audit.Diagnostic = Diagnostic;
  }
  // Later stages read both lists by the same index. Each keeps its order.
  auto Omit = [&](auto &List) {
    size_t Kept = 0;
    auto Next = Refused.begin();
    for (size_t I = 0; I < List.size(); ++I) {
      if (Next != Refused.end() && Next->first == I) {
        ++Next;
        continue;
      }
      if (Kept != I)
        List[Kept] = std::move(List[I]);
      ++Kept;
    }
    List.erase(List.begin() + Kept, List.end());
  };
  Omit(Result.LowFuncs);
  Omit(Result.MedFuncs);
}

namespace {
/// The integer arguments each function of this run that \p Pending calls
/// directly can be passed: its emitted parameters, which both C routes pass
/// a defined callee exactly. They are the MedIR parameters this run settled
/// and those its debug declaration adds. A callee this proof may still
/// refuse, a variadic one, one a source type hint declares, or one whose
/// declaration places parameters off the platform convention is left out.
std::map<va_t, unsigned> settledCalleeIntegerArity(
    const BinaryImage &Image, const PipelineResult &Result,
    const std::vector<size_t> &Pending, const DebugContext *Dbg) {
  std::set<va_t> Targets;
  for (const size_t I : Pending)
    for (const MedBlock &Block : Result.MedFuncs[I].Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst())
          Targets.insert(Op.Inputs[0].ConstVal);
  const llvm::ArrayRef<uint64_t> Registers =
      getTargetRegInfo(Image.Arch).integerParamRegs(Image.abiFormat());
  std::map<va_t, unsigned> Arity;
  for (const MedFunc &Callee : Result.MedFuncs) {
    if (!Targets.count(Callee.Entry) || !Callee.CallReturnReads.empty() ||
        Callee.IsVariadic || Callee.SourceTypeHint)
      continue;
    unsigned Count = 0;
    for (const MedVar &Param : Callee.Params)
      for (size_t K = 0; K < Registers.size(); ++K)
        if (Param.RegOff == Registers[K])
          Count = std::max(Count, static_cast<unsigned>(K + 1));
    if (const auto Declared =
            Dbg ? Dbg->resolveFunction(Callee.Entry) : std::nullopt) {
      if (Declared->IsVariadic || !Declared->PlatformConvention ||
          !Declared->ParamEntryLocations.empty())
        continue;
      Count = std::max(Count, static_cast<unsigned>(Declared->Params.size()));
    }
    Arity.emplace(Callee.Entry, Count);
  }
  return Arity;
}
} // namespace

bool refuseObservedCallReturnReads(const BinaryImage &Image,
                                   PipelineResult &Result, bool FailsRun,
                                   const DebugContext *Dbg) {
  std::vector<size_t> Pending;
  for (size_t I = 0; I < Result.MedFuncs.size(); ++I)
    if (!Result.MedFuncs[I].CallReturnReads.empty())
      Pending.push_back(I);
  if (Pending.empty())
    return true;
  const std::map<va_t, unsigned> Arity =
      settledCalleeIntegerArity(Image, Result, Pending, Dbg);
  std::vector<std::optional<std::string>> Observations(Pending.size());
  parallelForEach(Pending.size(), [&](auto Claim, size_t Total) {
    for (size_t J; (J = Claim()) < Total;) {
      std::string Diagnostic;
      if (!validateObservedCallReturnReads(
              Result.MedFuncs[Pending[J]], Image, Diagnostic,
              limits::ir::kSourceCallReturnReadWork, &Arity))
        Observations[J] = std::move(Diagnostic);
    }
  });
  std::map<size_t, std::string> Refused;
  for (size_t J = 0; J < Pending.size(); ++J) {
    MedFunc &Func = Result.MedFuncs[Pending[J]];
    Func.CallReturnReads.clear();
    if (!Observations[J])
      continue;
    std::string Error =
        "source MedIR failed for " + Func.Name + ": " + *Observations[J];
    if (FailsRun) {
      Result.Error = std::move(Error);
      return false;
    }
    Refused.emplace(Pending[J], std::move(Error));
  }
  omitRefusedFunctions(Result, Refused);
  return true;
}

//===----------------------------------------------------------------------===//
// buildMedIR — Phase 2
//===----------------------------------------------------------------------===//

void Pipeline::buildMedIR(const BinaryImage &Img, const PipelineOptions &Opts,
                          PipelineResult &Result) {
  auto Phase2Start = std::chrono::steady_clock::now();

  const size_t Total = Result.LowFuncs.size();
  Result.MedFuncs.resize(Total);
  // Each worker owns one element; publish errors in stable function order
  // only after all workers have joined.
  std::vector<std::optional<std::string>> ConversionErrors(Total);

  // Runtime metadata is a source-rendering hint, not a rewrite ABI contract.
  // Patch/lift and safety evidence keep their existing independent semantics.
  std::map<va_t, const SourceFunctionTypeHint *> SourceHints;
  if (Result.InterpreterMachineSourceABI && Result.LowFuncs.size() == 1)
    SourceHints.emplace(Result.LowFuncs.front().Entry,
                        &*Result.InterpreterMachineSourceABI);
  if (Opts.usesSourceDeclarations())
    for (const auto &[Entry, Hint] : Opts.SourceTypeHints)
      SourceHints.emplace(Entry, &Hint);
  if (!Opts.usesMachineMedIR()) {
    for (const auto &Method : Img.ObjCMethods)
      if (objcMethodHasSourceBody(Method))
        SourceHints.emplace(Method.Implementation, &*Method.TypeHint);
  }

  std::map<va_t, SourceFunctionTypeHint> SourceEntryHints;
  for (const auto &[Entry, Hint] : SourceHints)
    SourceEntryHints.emplace(Entry, *Hint);
  auto SourceCalleeHints = SourceEntryHints;
  auto SourceSites = Opts.usesSourceDeclarations()
                         ? Opts.SourceCallSiteHints
                         : decltype(Opts.SourceCallSiteHints){};
  const auto SEHRuntimeMemory =
      collectLowSEHRuntimeMemory(Img, Result.LowFuncs);
  std::map<va_t, const LowFunc *> SourceCalleeFunctions;
  for (const auto &F : Result.LowFuncs)
    SourceCalleeFunctions.emplace(F.Entry, &F);
  if (Opts.usesSourceDeclarations())
    for (const auto &[Entry, Hint] : Opts.SourceCalleeTypeHints)
      SourceCalleeHints.insert_or_assign(Entry, Hint);

  // Per-callee callee-cleanup pop (x86 `ret imm`, the i386 SysV sret hidden-
  // pointer pop) so each caller's CALL to such a callee gets a post-call stack-
  // pointer correction during low->med translation.  Built once (read-only) and
  // shared across the parallel converters.
  std::map<va_t, int> CalleePop;
  for (const auto &LF : Result.LowFuncs)
    if (LF.CalleePopBytes > 0)
      CalleePop[LF.Entry] = LF.CalleePopBytes;
  // A stdcall import pops its arguments too: through its stub or its slot,
  // as its prototype or its stub's decoration counts them.  A stub lifted as
  // a function keeps what its own code says.
  for (const auto &[Address, Bytes] : Result.CallEntryStackPops)
    CalleePop.try_emplace(Address, Bytes);

  // GOT/pointer-slot VAs holding a stack-probe import (____chkstk_darwin).
  // Apple clang guards a large frame with a GOT-indirect probe call in the
  // prologue; the low->med converter clears that call's spurious x0 output
  // before SSA so it does not kill the live-in argument registers (see
  // setStackProbeSlots).  Built once (read-only) and shared across the parallel
  // converters.  Empty on non-Mach-O (ImportPtrSlots is only populated there)
  // -> no-op.
  std::set<va_t> StackProbeSlots;
  for (const auto &[SlotVA, SymName] : Img.ImportPtrSlots) {
    if (isDarwinStackProbeName(SymName))
      StackProbeSlots.insert(SlotVA);
  }

  // Weight each function by its LowIR op count so the heaviest translations
  // start first and the tail stays balanced (see parallelForEachWeighted).
  std::vector<uint64_t> Weight(Total, 1);
  for (size_t I = 0; I < Total; ++I) {
    uint64_t W = 1;
    for (const auto &B : Result.LowFuncs[I].Blocks)
      W += B.Ops.size();
    Weight[I] = W;
  }

  // The runtime routines a call may name, read once for the run, or once
  // for every run that shares the analysis cache.
  std::shared_ptr<const PipelineAnalysisCache::SymbolInventory> Inventory;
  std::map<va_t, Intrinsic> OwnRuntimeRoutines;
  const std::map<va_t, Intrinsic> *RuntimeRoutines = &OwnRuntimeRoutines;
  if (PipelineAnalysisCache *Shared = sharedAnalysisCache(Img, Opts)) {
    Inventory = PipelineAnalysisCache::SymbolInventory::of(Img, *Shared);
    RuntimeRoutines = &Inventory->runtimeRoutineCalls(Img);
  } else {
    OwnRuntimeRoutines = runtimeRoutineCallsOf(Img, Img.Arch);
  }

  auto Configure = [&](LowToMedConverter &Local) {
    Local.setBinaryImage(&Img);
    Local.setRuntimeRoutineCalls(RuntimeRoutines);
    Local.setSourceCallHintsEnabled(Opts.usesSourceDeclarations());
    Local.setSourceCallRuntimeHintsEnabled(!Opts.usesMachineMedIR());
    Local.setSourceEntryTypeHints(&SourceEntryHints);
    Local.setSourceCalleeTypeHints(&SourceCalleeHints);
    Local.setSourceCalleeFunctions(&SourceCalleeFunctions);
    Local.setSEHFrameCallees(&SourceCalleeFunctions);
    Local.setSEHSourceProjectionEnabled(!Opts.usesMachineMedIR());
    Local.setSEHRuntimeMemory(&SEHRuntimeMemory);
    Local.setObjCBlockCaptureCallFields(&Opts.ObjCBlockCaptureFields);
    Local.setObjCBlockParameterReceivers(&Opts.ObjCBlockParameterReceivers);
    Local.setObjCBlockCaptureReceivers(&Opts.ObjCBlockCaptureReceivers);
    Local.setCalleePopMap(&CalleePop);
    Local.setCallMayWriteGPRs(&Result.CallMayWriteGPRs);
    Local.setCallExitConstants(&Result.CallExitConstants);
    Local.setCallEntryReadGPRs(&Result.CallEntryReadGPRs);
    Local.setCallUnreadGPRs(&Result.CallUnreadGPRs);
    Local.setCallArgumentSources(&Result.CallArgumentSources);
    Local.setCallEntryStackArgs(&Result.CallEntryStackArgs);
    Local.setCallEntryStackSlots(&Result.CallEntryStackSlots);
    Local.setCallEntryArgumentCounts(&Result.CallEntryArgumentCounts);
    Local.setCallEntryFloatingArguments(&Result.CallEntryFloatingArguments);
    Local.setCallEntryIntegerArguments(&Result.CallEntryIntegerArguments);
    Local.setCallEntryVariadicArguments(&Result.CallEntryVariadicArguments);
    Local.setUserPrototypeCallees(&Result.CallUserPrototypes);
    Local.setCallDispatchThunks(&Result.CallDispatchThunks);
    Local.setCallVariadicFrom(&Result.CallVariadicFrom);
    Local.setFormattedCalls(&Result.FormattedCalls);
    Local.setStackProbeSlots(&StackProbeSlots);
  };
  // A projected LowIR body must remain alive until the source stack checks.
  // Each conversion owns its slot; no worker reads another worker's MedIR.
  std::vector<std::unique_ptr<LowFunc>> ProjectedFunctions(Total);
  std::vector<uint8_t> Converted(Total, 0);
  std::vector<uint8_t> PendingStackProof(Total, 0);
  auto FallbackOne = [&](size_t I) {
    Converted[I] = 0;
    ProjectedFunctions[I].reset();
    syncWarning() << "pipeline: low->med threw on " << Result.LowFuncs[I].Name
                  << "\n";
    const LowFunc &LF = Result.LowFuncs[I];
    MedFunc Fallback;
    Fallback.Name = LF.Name;
    Fallback.Entry = LF.Entry;
    Fallback.OriginalSize = LF.OriginalSize;
    Fallback.DebugName = LF.DebugName;
    Fallback.SourceFile = LF.SourceFile;
    Fallback.SourceLine = LF.SourceLine;
    Fallback.ExceptionMetadata = LF.ExceptionMetadata;
    Fallback.JumpTables = LF.JumpTables;
    for (const auto &LB : LF.Blocks) {
      MedBlock MB;
      MB.Id = LB.Id;
      MB.StartAddr = LB.StartAddr;
      MB.EndAddr = LB.EndAddr;
      MB.Succs = LB.Succs;
      MB.Preds = LB.Preds;
      Fallback.Blocks.push_back(std::move(MB));
    }
    Result.MedFuncs[I] = std::move(Fallback);
  };
  auto ConvertOne = [&](LowToMedConverter &Local, size_t I) {
    Converted[I] = 0;
    PendingStackProof[I] = 0;
    try {
      const auto Sites = SourceSites.find(Result.LowFuncs[I].Entry);
      Local.setSourceCallSiteHints(Opts.usesSourceDeclarations() &&
                                           Sites != SourceSites.end()
                                       ? &Sites->second
                                       : nullptr);
      const auto Frame = Opts.SourceStackFrames.find(Result.LowFuncs[I].Entry);
      auto &Projected = ProjectedFunctions[I];
      Projected.reset();
      if (!Opts.usesMachineMedIR() && Frame != Opts.SourceStackFrames.end()) {
        std::string Error;
        auto FrameSource = projectSourceStackFrame(Result.LowFuncs[I], Img.Arch,
                                                   Frame->second, Error);
        if (!FrameSource)
          throw std::runtime_error(Error);
        Projected = std::make_unique<LowFunc>(std::move(*FrameSource));
      }
      Result.MedFuncs[I] =
          Local.convert(Projected ? *Projected : Result.LowFuncs[I], Img.Arch,
                        Img.abiFormat());
      Result.MedFuncs[I].AnalystStackFrame = bool(Projected);
      auto &MF = Result.MedFuncs[I];
      auto &LF = Result.LowFuncs[I];
      trimFuncStorage(MF);
      MF.OriginalSize = LF.OriginalSize;
      MF.DebugName = LF.DebugName;
      MF.SourceFile = LF.SourceFile;
      MF.SourceLine = LF.SourceLine;
      auto Hint = SourceHints.find(MF.Entry);
      if (Hint != SourceHints.end())
        MF.SourceTypeHint = *Hint->second;
      Converted[I] = 1;
    } catch (const LowToMedConversionError &Error) {
      ConversionErrors[I] = Error.what();
    } catch (...) {
      FallbackOne(I);
    }
  };
  MedCallReturnRanges CallReturns;
  bool CalleeFactsReady = false;
  auto FinishOne = [&](size_t I) {
    if (!Converted[I] || ConversionErrors[I])
      return;
    bool StackPrepared = false;
    try {
      auto &MF = Result.MedFuncs[I];
      const auto &Projected = ProjectedFunctions[I];
      prepareMedStackAccesses(MF, Projected ? *Projected : Result.LowFuncs[I],
                              Img, medStackProjection(Opts),
                              CalleeFactsReady ? &CallReturns : nullptr,
                              !CalleeFactsReady,
                              Result.InterpreterMachineSourceABI.has_value() &&
                                  Result.LowFuncs.size() == 1);
      StackPrepared = true;
      if (medStackProjection(Opts) == MedStackProjection::Source)
        expandSourceRegisterCalls(MF, Img, Result.CallRegisterLeaves);
      size_t MedOps = 0;
      for (const auto &Block : MF.Blocks)
        MedOps += Block.Ops.size();
      if (MF.Blocks.size() <= limits::kMaxStructurableMedBlocks &&
          MedOps <= static_cast<size_t>(limits::kMaxSSANodes))
        inferMedTypes(MF, Img.Arch);
    } catch (const LowToMedConversionError &Error) {
      ConversionErrors[I] = Error.what();
      PendingStackProof[I] = !StackPrepared;
    } catch (...) {
      FallbackOne(I);
    }
    if (!PendingStackProof[I])
      ProjectedFunctions[I].reset();
  };
  auto CompleteStackProofs = [&] {
    std::vector<size_t> Pending;
    std::vector<uint64_t> PendingWeight;
    for (size_t I = 0; I < Total; ++I)
      if (PendingStackProof[I]) {
        Pending.push_back(I);
        PendingWeight.push_back(Weight[I]);
      }
    if (Pending.empty())
      return;
    // Ordinary local proofs already completed the other functions. Only a
    // missing stack proof asks for the complete, immutable callee table, after
    // every conversion has joined. A second refusal remains a real error.
    CallReturns = sourceCallReturnRanges(Img, Result);
    CalleeFactsReady = true;
    parallelForEachWeighted(PendingWeight, [&](auto Claim, size_t N) {
      for (size_t J; (J = Claim()) < N;) {
        const size_t I = Pending[J];
        ConversionErrors[I].reset();
        PendingStackProof[I] = 0;
        FinishOne(I);
        ProjectedFunctions[I].reset();
      }
    });
  };
  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    LowToMedConverter Local;
    Configure(Local);
    for (size_t I; (I = Claim()) < N;) {
      ConvertOne(Local, I);
      FinishOne(I);
    }
  });
  CompleteStackProofs();

  // Discover bounded incoming-stack leaves from this conversion, then only
  // reconvert their definitions and exact callers. Call operands must
  // be present before SSA; adding a C parameter after lowering loses the
  // caller's store and cannot restore the physical CALL return-address bias.
  if (Opts.usesSourceDeclarations() && supportsIncomingStackSources(Img) &&
      std::none_of(ConversionErrors.begin(), ConversionErrors.end(),
                   [](const auto &Error) { return Error.has_value(); })) {
    std::set<va_t> ExplicitCallTargets;
    for (const auto &[Caller, Sites] : SourceSites)
      for (const auto &[Site, Hint] : Sites)
        ExplicitCallTargets.insert(Hint.TargetAddress);
    std::set<va_t> Inferred;
    std::set<size_t> Affected;
    std::map<va_t, size_t> LocalDefinitions;
    for (size_t I = 0; I < Total; ++I) {
      const auto &Low = Result.LowFuncs[I];
      if (SourceCalleeHints.count(Low.Entry) ||
          ExplicitCallTargets.count(Low.Entry) ||
          Result.CallUserPrototypes.count(Low.Entry) ||
          Result.MedFuncs[I].AnalystStackFrame)
        continue;
      auto Hint = inferIncomingStackLeaf(Img, Low, Result.MedFuncs[I]);
      if (!Hint)
        continue;
      // A candidate must also pass the shared source-carrier binder before
      // any caller starts depending on its physical locations.
      MedFunc Bound = Result.MedFuncs[I];
      Bound.SourceTypeHint = *Hint;
      inferMedTypes(Bound, Img.Arch);
      if (!Bound.SourceParametersBound || !Bound.SourceTypeHint)
        continue;
      SourceEntryHints.emplace(Low.Entry, *Hint);
      SourceCalleeHints.emplace(Low.Entry, std::move(*Hint));
      SourceHints.emplace(Low.Entry, &SourceEntryHints.at(Low.Entry));
      Inferred.insert(Low.Entry);
      LocalDefinitions.emplace(Low.Entry, I);
      Affected.insert(I);
    }
    // A register operand can have one exact local value without being a
    // static machine operand. Keep its original occurrence as the binding key;
    // numeric agreement alone never supplies a callee signature.
    std::vector<std::map<SourceCallOccurrenceKey, va_t>> CallTargets(Total);
    for (size_t I = 0; I < Total; ++I) {
      const auto &Low = Result.LowFuncs[I];
      CallTargets[I] = constantRegisterCallTargets(Low);
      for (const auto &Block : Low.Blocks)
        for (const auto &Op : Block.Ops)
          if (const auto Site = sourceCallOccurrenceKey(Op);
              Site && Site->Opcode == NdOp::CALL && Site->StaticTarget)
            CallTargets[I].emplace(*Site, *Site->StaticTarget);
    }
    std::set<va_t> OmittedTargets;
    for (const auto &Calls : CallTargets)
      for (const auto &[Site, Target] : Calls)
        if (!SourceCalleeFunctions.count(Target) &&
            !SourceCalleeHints.count(Target) &&
            !ExplicitCallTargets.count(Target) &&
            !Result.CallUserPrototypes.count(Target))
          OmittedTargets.insert(Target);
    for (auto &[Entry, Hint] :
         readIncomingStackCallees(Img, Result, OmittedTargets)) {
      SourceCalleeHints.emplace(Entry, std::move(Hint));
      Inferred.insert(Entry);
    }
    // An automatically declared input must be supplied by every caller in
    // this request. A partial/unwritten outgoing word is not a scalar value.
    // Withdraw the candidate as a unit rather than publish incompatible
    // definition and call signatures in a whole-module source projection.
    std::set<va_t> Unavailable;
    for (size_t I = 0; I < Total; ++I)
      for (const auto &[Site, Target] : CallTargets[I]) {
        if (!Inferred.count(Target))
          continue;
        SourceCallTypeHint Hint;
        Hint.Signature = SourceCalleeHints.at(Target);
        const auto Bias =
            sourceCallReturnAddressBias(&Img, Img.Arch, Site.Instruction);
        std::string Diagnostic;
        if (!Bias ||
            !sourceCallStackArgumentsDefined(Result.LowFuncs[I], Hint, Site,
                                             Img.Arch, *Bias, Diagnostic))
          Unavailable.insert(Target);
      }
    for (va_t Entry : Unavailable) {
      Inferred.erase(Entry);
      SourceCalleeHints.erase(Entry);
      SourceHints.erase(Entry);
      SourceEntryHints.erase(Entry);
      if (auto Local = LocalDefinitions.find(Entry);
          Local != LocalDefinitions.end())
        Affected.erase(Local->second);
    }
    for (size_t I = 0; I < Total && !Inferred.empty(); ++I)
      for (const auto &[Site, Target] : CallTargets[I]) {
        if (!Inferred.count(Target))
          continue;
        SourceCallTypeHint Hint;
        Hint.CallKind = SourceCallTypeHint::Kind::Native;
        Hint.TargetAddress = Target;
        Hint.TargetName = Img.getFunctionNameAt(Target);
        Hint.Signature = SourceCalleeHints.at(Target);
        if (SourceSites[Result.LowFuncs[I].Entry]
                .try_emplace(Site, std::move(Hint))
                .second)
          Affected.insert(I);
      }
    const std::vector<size_t> Indices(Affected.begin(), Affected.end());
    std::vector<uint64_t> ReconvertWeights;
    for (size_t I : Indices)
      ReconvertWeights.push_back(Weight[I]);
    CallReturns.clear();
    parallelForEachWeighted(ReconvertWeights, [&](auto Claim, size_t N) {
      LowToMedConverter Local;
      Configure(Local);
      for (size_t J; (J = Claim()) < N;) {
        ConvertOne(Local, Indices[J]);
        FinishOne(Indices[J]);
      }
    });
    CompleteStackProofs();
  }

  // A run restricted to requested functions fails with the first one MedIR
  // refuses, as asking for that function should (refusalFailsRun). Any other
  // run names each refused function in its audit and converts the others.
  std::map<size_t, std::string> Refused;
  for (size_t I = 0; I < Total; ++I) {
    if (!ConversionErrors[I])
      continue;
    std::string Error = "LowIR to MedIR failed for " + Result.LowFuncs[I].Name +
                        ": " + *ConversionErrors[I];
    if (refusalFailsRun(Opts)) {
      Result.Error = std::move(Error);
      Result.Success = false;
      Result.MedFuncs.clear();
      return;
    }
    Refused.emplace(I, std::move(Error));
  }
  omitRefusedFunctions(Result, Refused);

  recordMedIRVerification(Result, "pipeline-med-final");

  [[maybe_unused]] auto Elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - Phase2Start)
          .count();
  LLVM_DEBUG(llvm::dbgs() << "pipeline: LowIR -> MedIR took " << Elapsed
                          << "ms (" << Result.MedFuncs.size()
                          << " functions)\n");
}

} // namespace neverd

neverd::PipelineResult neverd::Pipeline::rebuildSourceCalls(
    const BinaryImage &Img, const PipelineOptions &Opts,
    const PipelineResult &Original, DebugContext *Dbg) {
  PipelineResult Result;
  if (Opts.usesMachineMedIR() ||
      (Opts.SourceCallSiteHints.empty() && Opts.SourceStackFrames.empty())) {
    Result.Error =
        "Call-site declarations require an explicit private source variant";
    return Result;
  }
  Result.SourceImage = &Img;
  for (const auto &Function : Original.LowFuncs)
    if (Opts.OnlyFunctionEntries.count(Function.Entry))
      Result.LowFuncs.push_back(Function);
  if (Result.LowFuncs.empty()) {
    Result.Error = "The original machine operations are unavailable";
    return Result;
  }
  copyCallSignatureFacts(Original, Result);
#define COPY_SOURCE_FACT(Field) Result.Field = Original.Field
  COPY_SOURCE_FACT(CallDispatchThunks);
  COPY_SOURCE_FACT(CallEntryStackPops);
  COPY_SOURCE_FACT(CallRegisterLeaves);
  COPY_SOURCE_FACT(CallVariadicFrom);
  COPY_SOURCE_FACT(FormattedCalls);
  COPY_SOURCE_FACT(OmittedCallees);
  COPY_SOURCE_FACT(CallFloatReturns);
#undef COPY_SOURCE_FACT
  Result.Success = true;
  buildMedIR(Img, Opts, Result);
  if (!Result.Success || Result.MedIRVerifierFailures) {
    Result.Success = false;
    return Result;
  }
  buildHighIR(Img, Opts, Result, Dbg);
  return Result;
}
