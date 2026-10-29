//===- PipelinePatchLift.cpp - Patch/lift pipeline path ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Machine ABI preparation shared by analysis and the MedIR-to-LLVM route.
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"
#include "PipelineFunctionNames.h"
#include "PipelineLLVMDetail.h"
#include "PipelineMedAudit.h"
#include "PipelineReturnModelingDetail.h"
#include "PipelineSourceStackAccesses.h"

#include "neverd/backend/llvm/LLVMCallContract.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

namespace {

bool isFatalOptimizationStop(OptimizationStopReason Stop) {
  return Stop == OptimizationStopReason::InputInvalid ||
         Stop == OptimizationStopReason::VerificationFailed;
}

std::string functionAdmissionDiagnostic(va_t Entry,
                                        const PipelineFunctionAudit *Audit) {
  std::string Message =
      "function 0x" + llvm::utohexstr(Entry) + " was not admitted";
  if (!Audit)
    return Message;
  Message += " (" +
             std::string(pipelineFunctionDispositionName(Audit->Disposition)) +
             ")";
  if (!Audit->Diagnostic.empty())
    Message += "; " + Audit->Diagnostic;
  else if (!Audit->DecodeFailures.empty())
    Message += "; decode failure at 0x" +
               llvm::utohexstr(Audit->DecodeFailures.front());
  else if (!Audit->UnsupportedInstructions.empty())
    Message += "; unsupported instruction at 0x" +
               llvm::utohexstr(Audit->UnsupportedInstructions.front());
  return Message;
}

/// Whether every requested function reaches LLVM. Names the first that does
/// not in \p Error.
bool admitsRequestedFunctions(const BinaryImage &Img,
                              const PipelineOptions &Opts,
                              const PipelineResult &Result,
                              const char *ModeName, std::string &Error) {
  std::set<va_t> AdmittedEntries;
  for (const auto &Function : Result.MedFuncs)
    AdmittedEntries.insert(
        normalizeCodeAddress(Function.Entry, Img.Arch, Img.Mode));
  for (va_t Requested : Opts.OnlyFunctionEntries) {
    const va_t Entry = normalizeCodeAddress(Requested, Img.Arch, Img.Mode);
    if (AdmittedEntries.count(Entry))
      continue;
    const auto Audit =
        std::find_if(Result.FunctionAudits.begin(), Result.FunctionAudits.end(),
                     [&](const PipelineFunctionAudit &Candidate) {
                       return normalizeCodeAddress(Candidate.Entry, Img.Arch,
                                                   Img.Mode) == Entry;
                     });
    Error = "selected " + std::string(ModeName) + " " +
            functionAdmissionDiagnostic(
                Requested,
                Audit != Result.FunctionAudits.end() ? &*Audit : nullptr);
    return false;
  }
  return true;
}

/// An empty module is not a successful lift of bodies every one of which was
/// refused. Names the first such refusal in \p Error.
bool admitsSomeFunction(const PipelineResult &Result, const char *ModeName,
                        std::string &Error) {
  if (!Result.MedFuncs.empty())
    return true;
  for (const auto &Audit : Result.FunctionAudits)
    if (Audit.Disposition == PipelineFunctionDisposition::RejectedLowIR ||
        Audit.Disposition == PipelineFunctionDisposition::RejectedIncomplete ||
        Audit.Disposition == PipelineFunctionDisposition::MedIRFailed) {
      Error = "no " + std::string(ModeName) + " functions were admitted; " +
              functionAdmissionDiagnostic(Audit.Entry, &Audit);
      return false;
    }
  return true;
}

} // namespace

bool Pipeline::requiresSerialLLVMEmission(const std::vector<MedFunc> &Funcs,
                                          const BinaryImage &Img) {
  if (Img.CodePtrRelocSlots.empty() || Img.getPointerSize() == 0)
    return false;
  std::set<va_t> FunctionEntries;
  for (const MedFunc &Func : Funcs)
    FunctionEntries.insert(Func.Entry);
  for (va_t Slot : Img.CodePtrRelocSlots) {
    const uint8_t *P = Img.readVA(Slot, Img.getPointerSize());
    if (!P)
      continue;
    const va_t Target = normalizeCodeAddress(
        static_cast<va_t>(readPtr(P, Img.is64Bit())), Img.Arch, Img.Mode);
    if (!FunctionEntries.count(Target))
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// runPatchLiftMode — MedIR -> LLVM IR shortcut
//===----------------------------------------------------------------------===//

bool Pipeline::runPatchLiftMode(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                                const PipelineOptions &Opts,
                                PipelineResult &Result,
                                const DebugContext *Dbg) {
  [[maybe_unused]] const char *ModeName = Opts.MedAnalysisOnly ? "analysis"
                                          : Opts.PatchMode     ? "patch"
                                                               : "lift";
  LLVM_DEBUG(llvm::dbgs() << "pipeline: " << ModeName
                          << " mode -- shared machine ABI preparation\n");

  // A selected parent may fail instruction admission while its automatically
  // included cleanup bodies survive. Emitting those bodies is not a successful
  // export of the requested entry. Analysis views retain their partial results.
  if (!admitsRequestedFunctions(Img, Opts, Result, ModeName, Result.Error))
    return false;
  // A whole-module request can reject every body before reaching LLVM.
  // Intentional runtime/import skips and read-only analysis retain their
  // existing outcome.
  if (!Opts.MedAnalysisOnly &&
      !admitsSomeFunction(Result, ModeName, Result.Error))
    return false;
  if (Opts.PatchMode)
    for (const MedFunc &Function : Result.MedFuncs) {
      bool NeedsAdapter = Function.ConsumesX87Argument;
      for (const MedBlock &Block : Function.Blocks)
        for (const MedOp &Op : Block.Ops)
          NeedsAdapter |= Op.X87ArgumentInput != 0 && !Op.X87ImportArguments;
      if (NeedsAdapter) {
        Result.Error =
            "patch output requires a native x87 argument adapter for "
            "function 0x" +
            llvm::utohexstr(Function.Entry);
        return false;
      }
    }
  for (const MedFunc &Function : Result.MedFuncs)
    if (Function.ConsumesX87Argument &&
        (Img.CodeRefTargets.count(Function.Entry) ||
         Img.findExportAt(Function.Entry) || Img.Entry == Function.Entry)) {
      Result.Error =
          "native entry/address use requires an x87 argument adapter for "
          "function 0x" +
          llvm::utohexstr(Function.Entry);
      return false;
    }

  auto AllFuncNames = buildFuncNameMap(Img, Result);
  if (Opts.SourceProjection)
    for (auto &[Address, Name] :
         pipeline_detail::debugFunctionNames(Result, AllFuncNames, Dbg))
      AllFuncNames[Address] = std::move(Name);

  // Infer types for every function first: call-ABI recovery
  // (recoverModuleCallAbi) bounds each call by its callee's inferred
  // signature, its return class (FP-in-vector-register vs integer) and its
  // argument counts.
  for (auto &MF : Result.MedFuncs)
    if (!MF.SourceParametersBound)
      inferMedTypes(MF, Img.Arch);

  // A callee that merely forwards an indirect scalar-FP call has no explicit
  // post-call D0/V0 read: the native RET returns whatever BLR left in V0, so
  // its own body alone looks like an integer-returning function.  A direct
  // caller provides the missing ABI evidence when LowToMed routes that call's
  // result through V0 and a scalar S0/D0 view is consumed downstream.  Feed
  // that observed width back into the callee before call-ABI recovery; the
  // forwarder's final indirect call can then be rewired from X0 to V0.
  if (Img.Arch == Arch::AArch64) {
    const auto &TRI = getTargetRegInfo(Img.Arch);
    std::map<va_t, MedFunc *> ByEntry;
    for (auto &MF : Result.MedFuncs)
      ByEntry[MF.Entry] = &MF;
    for (const auto &Caller : Result.MedFuncs) {
      for (const auto &Blk : Caller.Blocks) {
        for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
          const MedOp &Call = Blk.Ops[OI];
          if (Call.Opcode != NdOp::CALL || Call.NumInputs < 1 ||
              !Call.Inputs[0].isConst() || Call.Output.Kind != MedVar::Reg ||
              Call.Output.RegOff != TRI.FPReturnReg)
            continue;
          auto CalleeIt = ByEntry.find(Call.Inputs[0].ConstVal);
          if (CalleeIt == ByEntry.end())
            continue;
          uint16_t ScalarSize = 0;
          for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
            const MedOp &Use = Blk.Ops[J];
            for (uint8_t I = 0; I < Use.NumInputs; ++I) {
              const MedVar &V = Use.Inputs[I];
              if (V.Kind != MedVar::Reg || V.RegOff != Call.Output.RegOff ||
                  V.Id != Call.Output.Id || V.SSAVer != Call.Output.SSAVer ||
                  (V.Size != 4 && V.Size != 8))
                continue;
              if (ScalarSize == 0 || V.Size < ScalarSize)
                ScalarSize = V.Size;
            }
            if (Use.Output.Kind == MedVar::Reg &&
                Use.Output.RegOff == Call.Output.RegOff &&
                Use.Output.SSAVer != Call.Output.SSAVer)
              break;
          }
          if (ScalarSize != 0)
            CalleeIt->second->ReturnType = NdType::makeFloat(ScalarSize);
        }
      }
    }
  }

  modelWideIntReturns(Img, Result, medStackProjection(Opts));

  recoverStructReturnFromCallers(Img, Result);

  propagateStructReturnForwarderShapes(Img, Result);

  recoverStructReturnFromBody(Img, Result,
                              StructReturnEvidence::CallersAndBody);

  recoverVectorReturnFromCallers(Img, Result);

  materializeKnownStructReturnCallSites(Img, Result);

  bindFloatCallResults(Img, Result);

  // LLVM-to-C uses the same declared source returns as HighC. Binary lifting
  // keeps its machine contract; a display prototype cannot authorize a
  // different executable return ABI.
  settleReturnContracts(Img, Result, Opts.SourceProjection ? Dbg : nullptr);

  recoverModuleCallAbi(Img, Result, AllFuncNames,
                       Opts.SourceProjection && !Opts.PatchMode,
                       Opts.SourceProjection && !Opts.PatchMode);

  if (Opts.SourceProjection && !Opts.PatchMode)
    refineSourcePointerParameters(Img, Result, Dbg);

  remodelStructReturnForwarderCalls(Img, Result);

  // Variadic callees: size each one's overflow stack-parameter list from its
  // now recovered call sites, append the trailing stack parameters, and pad
  // every call to that arity.  The emitter spills these into the frame headroom
  // so the unchanged va_arg walk reads the caller's overflow arguments.
  finalizeVariadicCallees(Result.MedFuncs, Img.Arch, Img.abiFormat());
  if (Img.Arch == Arch::ARM)
    propagateForwardedPointerParams(Result.MedFuncs, Img.Arch);
  // Late ABI remodelling may reconvert a function from LowIR.  Refresh the
  // interprocedural facts before either serial or sharded LLVM emission.
  propagateInternalNoReturn(Result.MedFuncs, Img.Arch, Img.UserNoReturn,
                            importCallAddresses(Img));

  for (const MedFunc &Function : Result.MedFuncs)
    for (const MedBlock &Block : Function.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        if (!Block.Ops[I].X87ArgumentInput)
          continue;
        const auto Call =
            llvm::find_if(Function.CallInfos, [&](const MedCallInfo &CI) {
              return CI.BlockId == Block.Id && CI.OpIdx == I;
            });
        if (Call == Function.CallInfos.end() || Call->Args.empty() ||
            llvm::any_of(Call->Args, [](const MedVar &Arg) {
              return Arg.Kind == MedVar::Unspecified;
            })) {
          Result.Error = "checked x87 call is missing an ordinary argument in "
                         "function 0x" +
                         llvm::utohexstr(Function.Entry);
          return false;
        }
      }

  if (!recordMedIRVerification(Result, "pipeline-backend-input")) {
    Result.Error = "MedIR verification failed before backend emission";
    return false;
  }

  if (Opts.MedAnalysisOnly)
    return true;

  if (medStackProjection(Opts) == MedStackProjection::Source) {
    prepareRecoveredSourceStackAccesses(Img, Result, Result.LowFuncs);
    // The proof can refuse a requested function, or every one.
    if (!refuseObservedCallReturnReads(Img, Result, refusalFailsRun(Opts),
                                       Dbg) ||
        !admitsRequestedFunctions(Img, Opts, Result, ModeName, Result.Error) ||
        !admitsSomeFunction(Result, ModeName, Result.Error))
      return false;
  }

  std::vector<std::pair<va_t, std::string>> ImportMap;
  for (const auto &[Addr, Name] : Img.getImportAddressNames())
    ImportMap.emplace_back(Addr, Name);

  // Parallel emit + optimize (the two single-threaded phases that dominate
  // lift).  Only for lift mode — the patch backend depends on the serial path's
  // single-module, internal-linkage globals.  Worker count comes from the
  // shared pool setting, so NEVERD_THREADS / setWorkerThreadCount() throttle
  // this phase too: it is by far the most memory-hungry one, and capping it was
  // previously impossible (it read hardware_concurrency() directly).  Small
  // inputs stay serial: below 8 functions the shard
  // emit/verify/optimize/link setup outweighs the memory and parallelism gains.
  // A large input still takes this path with one worker: the work-budgeted
  // shards then run serially, which is what makes NEVERD_THREADS=1 actually
  // bound the transient LLVM emission working set.
  unsigned Workers = std::max(
      1u, std::min<unsigned>(workerThreadCount(),
                             static_cast<unsigned>(Result.MedFuncs.size())));
  bool UseShards = !Opts.PatchMode && !Result.InterpreterMachineSourceABI &&
                   Result.MedFuncs.size() >= 8 &&
                   !requiresSerialLLVMEmission(Result.MedFuncs, Img);

  // The bodies the module declares only, by index into MedFuncs, with why.
  std::vector<std::pair<size_t, std::string>> LeftOut;
  if (UseShards) {
    LLVMEmissionResult Emission =
        emitLLVMSharded(Result.MedFuncs, Ctx, Img.Arch, ImportMap, Img,
                        Img.abiFormat(), Opts.NoOpt, Workers,
                        Opts.RetainLLVMSourceObservations ||
                            !Result.LibraryRecognitions.empty(),
                        Result.LibraryRecognitions, Dbg,
                        SourceViewOptions{.Symbolic = Opts.SymbolicLLVMSource},
                        &Result.CallPrototypeArguments,
                        Opts.SourceProjection && !Opts.PatchMode,
                        Result.OmittedCalleeDeclarations,
                        Opts.OmitRefusedLLVMBodies && !Opts.PatchMode);
    Result.BackendUnhandledValueIntrinsics = Emission.UnhandledValueIntrinsics;
    Result.LLVMVerifierFailed = Emission.LLVMVerifierFailed;
    Result.LlvmModule = std::move(Emission.Module);
    Result.LLVMSources = std::move(Emission.Sources);
    if (!Result.LlvmModule) {
      Result.Error = std::move(Emission.Error);
      return false;
    }
    LeftOut = std::move(Emission.LeftOut);
  } else {
    // A recovered machine wrapper uses fixed guest addresses. The original
    // image remains available to recovery proofs and reports, but must not
    // authorize source globals or rebased LOAD/STORE addresses. This recovery
    // route has one call-free function, with its source ABI already bound.
    const BinaryImage *EmissionImage =
        Result.InterpreterMachineSourceABI ? nullptr : &Img;
    // As a shard does where the unit names what it leaves out, a body that
    // refuses a stale address, or whose contract the emitter refuses before
    // any module, stays declared only and the rest are emitted again without
    // it. A patch rewrites every function it lifts, so there one refusal
    // still fails the lift.
    const bool OmitRefused = Opts.OmitRefusedLLVMBodies && !Opts.PatchMode &&
                             !Result.InterpreterMachineSourceABI;
    std::vector<char> Mask;
    uint64_t UnhandledValueIntrinsics = 0;
    for (;;) {
      MedLLVMEmitter MedEmitter;
      // The mask holds only bodies the unit notes left out.
      MedEmitter.setMaskedBodiesOwnedElsewhere(!Mask.empty());
      MedEmitter.setPreserveUnknownSourceValues(Opts.SourceProjection &&
                                                !Opts.PatchMode);
      MedEmitter.setSourceCalleeDeclarations(Result.OmittedCalleeDeclarations);
      MedEmitter.setDebugContext(Dbg);
      MedEmitter.setPrototypeArgumentCallees(&Result.CallPrototypeArguments);
      Result.LLVMSources.reset();
      if (Opts.RetainLLVMSourceObservations ||
          !Result.LibraryRecognitions.empty()) {
        Result.LLVMSources = std::make_shared<LLVMSourceMap>();
        MedEmitter.setSourceMap(Result.LLVMSources.get());
      }
      try {
        Result.LlvmModule = MedEmitter.emit(
            Result.MedFuncs, Ctx, "neverd_output", Img.Arch, ImportMap,
            EmissionImage, Img.abiFormat(), /*MergeableGlobals=*/false,
            Mask.empty() ? nullptr : &Mask);
      } catch (const std::exception &Error) {
        // Keep semantic refusals inside the pipeline result, as the sharded
        // path does. No partially observed module may escape through the
        // SDK.
        Result.LlvmModule.reset();
        Result.LLVMSources.reset();
        if (!OmitRefused || MedEmitter.refusedBodies().empty()) {
          Result.Error = Error.what();
          return false;
        }
      }
      UnhandledValueIntrinsics = MedEmitter.unhandledValueIntrinsicCount();
      if (Result.LlvmModule || !OmitRefused ||
          MedEmitter.refusedBodies().empty())
        break;
      if (Mask.empty())
        Mask.assign(Result.MedFuncs.size(), 1);
      for (const MedLLVMEmitter::RefusedBody &Refused :
           MedEmitter.refusedBodies()) {
        Mask[Refused.Index] = 0;
        LeftOut.emplace_back(Refused.Index, Refused.Reason);
      }
    }
    Result.BackendUnhandledValueIntrinsics = UnhandledValueIntrinsics;
    if (Result.LLVMSources)
      Result.LLVMSources->preserveExpressionOrigins(Result.LibraryRecognitions);

    if (!Result.LlvmModule) {
      Result.Error = "LLVM emission failed";
      return false;
    }

    std::string VerifyErr;
    llvm::raw_string_ostream VES(VerifyErr);
    if (llvm::verifyModule(*Result.LlvmModule, &VES)) {
      Result.LLVMVerifierFailed = true;
      Result.Error =
          "LLVM verification failed before optimization: " + VerifyErr;
      llvm::WithColor::warning()
          << "pipeline: LLVM verification failed before optimization: "
          << VerifyErr << "\n";
      return false;
    }
    if (!Opts.NoOpt) {
      OptimizationOptions Options;
      Options.SourceMap = Result.LLVMSources.get();
      Options.Conservative = Opts.PatchMode;
      const bool SourceOnly = Opts.SourceProjection && !Opts.PatchMode;
      OptimizationResult Optimization =
          SourceOnly
              ? optimizeOrPromoteModule(
                    *Result.LlvmModule, Result.LLVMSources.get(),
                    SourceViewOptions{.Symbolic = Opts.SymbolicLLVMSource})
              : optimizeModule(*Result.LlvmModule, Options);
      // Match the source SDK and sharded C route: a verified analysis module
      // keeps its explicit incomplete native EH contract and only promotes
      // temporary allocas. Binary rewriting still requires native lowering.
      if (isFatalOptimizationStop(Optimization.Stop) &&
          !(SourceOnly &&
            Optimization.Stop == OptimizationStopReason::InputInvalid)) {
        Result.LLVMVerifierFailed = true;
        Result.Error = std::string("LLVM optimization failed: ") +
                       optimizationStopReasonName(Optimization.Stop);
        return false;
      }
    } else {
      // Even with the NeverD optimizer disabled, promote the emitter's
      // memory-SSA scaffolding to registers.  This is semantics-preserving
      // canonicalization (not optimization): it strips the per-temp load/store
      // bloat so a heavily-unrolled -O2 SSE kernel does not lift to an
      // ~80K-instruction single block that is pathological for LLVM codegen and
      // times out under parallel test load.
      promoteScaffoldingAllocas(*Result.LlvmModule);
      if (Result.LLVMSources)
        Result.LLVMSources->refreshExpressionOrigins();
    }
  }

  Result.LLVMDefinitionNames.clear();
  for (const auto &Function : *Result.LlvmModule)
    if (!Function.isDeclaration())
      Result.LLVMDefinitionNames.push_back(Function.getName().str());
  std::sort(Result.LLVMDefinitionNames.begin(),
            Result.LLVMDefinitionNames.end());

  for (auto &Audit : Result.FunctionAudits) {
    if (!Audit.HasMedIR)
      continue;
    Audit.HasLLVMDefinition =
        std::binary_search(Result.LLVMDefinitionNames.begin(),
                           Result.LLVMDefinitionNames.end(), Audit.Name);
  }
  for (const auto &[Index, Reason] : LeftOut)
    for (auto &Audit : Result.FunctionAudits)
      if (Audit.Entry == Result.MedFuncs[Index].Entry)
        Audit.LLVMDiagnostic =
            Reason.empty() ? "its body refused LLVM emission" : Reason;

  std::string FinalVerifyError;
  llvm::raw_string_ostream FinalVerifyStream(FinalVerifyError);
  if (!normalizeResolvedLLVMCalls(*Result.LlvmModule,
                                  Result.LLVMSources.get())) {
    Result.Error =
        "final LLVM verification failed: resolved call signature mismatch";
    return false;
  }
  Result.LLVMVerifierFailed =
      llvm::verifyModule(*Result.LlvmModule, &FinalVerifyStream);
  if (Result.LLVMVerifierFailed) {
    Result.Error = "final LLVM verification failed: " + FinalVerifyError;
    llvm::WithColor::warning() << "pipeline: " << Result.Error << "\n";
    return false;
  }
  if (!validateResolvedLLVMCallSignatures(*Result.LlvmModule)) {
    Result.Error =
        "final LLVM verification failed: resolved call signature mismatch";
    return false;
  }

  return true;
}

} // namespace neverd
