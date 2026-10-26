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

#include "PipelineIncomingStack.h"
#include "PipelineLowIRDetail.h"
#include "PipelineMedAudit.h"
#include "PipelineRegisterCalls.h"
#include "PipelineSEHRuntimeMemory.h"
#include "PipelineSourceStackAccesses.h"
#include "PipelineTrimStorage.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceCallABI.h"
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
#include <optional>
#include <set>
#include <string>
#include <vector>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

MedStackProjection medStackProjection(const PipelineOptions &Options) {
  return !Options.MedAnalysisOnly && !Options.PatchMode &&
                 (!Options.LiftMode || Options.SourceProjection)
             ? MedStackProjection::Source
             : MedStackProjection::Machine;
}

void prepareMedStackAccesses(MedFunc &Func, const LowFunc &Source,
                             const BinaryImage &Image,
                             MedStackProjection Projection) {
  Func.EntryKind = functionEntryKind(Func, Image.stackEntryKindAt(Func.Entry));
  if (Projection != MedStackProjection::Source)
    return;
  simplifyProvenStackAlignment(Func, Image.Arch, Image.abiFormat(),
                               Func.EntryKind);
  std::string Diagnostic;
  if (!validateSourceCallReturnReads(Func, Image, Diagnostic,
                                     limits::ir::kSourceCallReturnReadWork,
                                     &Source))
    throw LowToMedConversionError(Diagnostic);
  markSourceStackAccesses(Func, Image.Arch, Image.abiFormat(), Func.EntryKind);
}

void prepareRecoveredSourceStackAccesses(const BinaryImage &Image,
                                         PipelineResult &Result) {
  std::map<va_t, const MedFunc *> Callees;
  std::set<va_t> Complete;
  for (const LowFunc &Func : Result.LowFuncs)
    if (Func.hasCompleteLiftCoverage() &&
        Func.UnsafeIndirectBranchAddresses.empty())
      Complete.insert(Func.Entry);
  for (const MedFunc &Func : Result.MedFuncs)
    if (Complete.count(Func.Entry))
      Callees.emplace(Func.Entry, &Func);
  if (Result.OmittedCallees)
    for (const auto &Func : Result.OmittedCallees->Bodies)
      Callees.try_emplace(Func->Entry, Func.get());
  for (MedFunc &Func : Result.MedFuncs)
    if (Complete.count(Func.Entry))
      markUnobservedBorrowedFrameStores(Func, Image.Arch, Image.abiFormat(),
                                        Callees);
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
  auto ConvertOne = [&](LowToMedConverter &Local, size_t I) {
    try {
      const auto Sites = SourceSites.find(Result.LowFuncs[I].Entry);
      Local.setSourceCallSiteHints(Opts.usesSourceDeclarations() &&
                                           Sites != SourceSites.end()
                                       ? &Sites->second
                                       : nullptr);
      const auto Frame = Opts.SourceStackFrames.find(Result.LowFuncs[I].Entry);
      std::optional<LowFunc> Projected;
      if (!Opts.usesMachineMedIR() && Frame != Opts.SourceStackFrames.end()) {
        std::string Error;
        Projected = projectSourceStackFrame(Result.LowFuncs[I], Img.Arch,
                                            Frame->second, Error);
        if (!Projected)
          throw std::runtime_error(Error);
      }
      Result.MedFuncs[I] =
          Local.convert(Projected ? *Projected : Result.LowFuncs[I], Img.Arch,
                        Img.abiFormat());
      Result.MedFuncs[I].AnalystStackFrame = Projected.has_value();
      auto &MF = Result.MedFuncs[I];
      auto &LF = Result.LowFuncs[I];
      trimFuncStorage(MF);
      prepareMedStackAccesses(MF, Projected ? *Projected : LF, Img,
                              medStackProjection(Opts));
      if (medStackProjection(Opts) == MedStackProjection::Source)
        expandSourceRegisterCalls(MF, Img, Result.CallRegisterLeaves);
      MF.OriginalSize = LF.OriginalSize;
      MF.DebugName = LF.DebugName;
      MF.SourceFile = LF.SourceFile;
      MF.SourceLine = LF.SourceLine;
      auto Hint = SourceHints.find(MF.Entry);
      if (Hint != SourceHints.end())
        MF.SourceTypeHint = *Hint->second;
      size_t MedOps = 0;
      for (const auto &Block : MF.Blocks)
        MedOps += Block.Ops.size();
      if (MF.Blocks.size() <= limits::kMaxStructurableMedBlocks &&
          MedOps <= static_cast<size_t>(limits::kMaxSSANodes))
        inferMedTypes(MF, Img.Arch);
    } catch (const LowToMedConversionError &Error) {
      ConversionErrors[I] = Error.what();
    } catch (...) {
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
    }
  };
  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    LowToMedConverter Local;
    Configure(Local);
    for (size_t I; (I = Claim()) < N;)
      ConvertOne(Local, I);
  });

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
    parallelForEachWeighted(ReconvertWeights, [&](auto Claim, size_t N) {
      LowToMedConverter Local;
      Configure(Local);
      for (size_t J; (J = Claim()) < N;)
        ConvertOne(Local, Indices[J]);
    });
  }

  for (size_t I = 0; I < Total; ++I) {
    if (!ConversionErrors[I])
      continue;
    Result.Error = "LowIR to MedIR failed for " + Result.LowFuncs[I].Name +
                   ": " + *ConversionErrors[I];
    Result.Success = false;
    Result.MedFuncs.clear();
    return;
  }

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
#define COPY_SOURCE_FACT(Field) Result.Field = Original.Field
  COPY_SOURCE_FACT(CallArgumentSources);
  COPY_SOURCE_FACT(CallDispatchThunks);
  COPY_SOURCE_FACT(CallEntryArgumentCounts);
  COPY_SOURCE_FACT(CallEntryFloatingArguments);
  COPY_SOURCE_FACT(CallEntryIntegerArguments);
  COPY_SOURCE_FACT(CallEntryVariadicArguments);
  COPY_SOURCE_FACT(OmittedCalleeVariadicArguments);
  COPY_SOURCE_FACT(CallEntryReadGPRs);
  COPY_SOURCE_FACT(CallUnreadGPRs);
  COPY_SOURCE_FACT(CallEntryStackArgs);
  COPY_SOURCE_FACT(CallEntryStackPops);
  COPY_SOURCE_FACT(CallEntryStackSlots);
  COPY_SOURCE_FACT(CallMayWriteGPRs);
  COPY_SOURCE_FACT(CallExitConstants);
  COPY_SOURCE_FACT(CallRegisterLeaves);
  COPY_SOURCE_FACT(CallVariadicFrom);
  COPY_SOURCE_FACT(FormattedCalls);
  COPY_SOURCE_FACT(OmittedCalleeArgumentCounts);
  COPY_SOURCE_FACT(OmittedCalleeFloatingArguments);
  COPY_SOURCE_FACT(OmittedCalleeIntegerArguments);
  COPY_SOURCE_FACT(OmittedCalleeStackArgs);
  COPY_SOURCE_FACT(OmittedCalleeStackSlots);
  COPY_SOURCE_FACT(OmittedCallees);
  COPY_SOURCE_FACT(CallPrototypeArguments);
  COPY_SOURCE_FACT(CallUserPrototypes);
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
