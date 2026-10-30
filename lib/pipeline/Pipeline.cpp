//===- Pipeline.cpp - Decompilation pipeline -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Full decompilation pipeline orchestration.
///
//===----------------------------------------------------------------------===//

#include "neverd/pipeline/Pipeline.h"

#include "PipelineCallAbiDetail.h"
#include "PipelineHighIRDetail.h"
#include "PipelineLowIRDetail.h"
#include "PipelineSourceStackAccesses.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/analysis/InterpreterMachineState.h"
#include "neverd/decode/Decoder.h"
#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/evm/emit/EVMLLVMEmitter.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/emit/SBFLLVMEmitter.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

namespace {

class NativePipelineTrace {
public:
  enum class Stage {
    Decoder,
    FunctionDetection,
    LowIR,
    CandidateCleanup,
    MedIR,
    NoReturnVerify,
    MachineMedIR,
    HighIR,
    LLVMEmission
  };

private:
  using Clock = std::chrono::steady_clock;
  using Milliseconds = std::chrono::milliseconds;
  unsigned Invocation = 0;
  Stage Current = Stage::Decoder;
  bool Active = false;
  Clock::time_point Started;

  const char *name() const noexcept {
    switch (Current) {
    case Stage::Decoder:
      return "decoder";
    case Stage::FunctionDetection:
      return "function_detection";
    case Stage::LowIR:
      return "low_ir";
    case Stage::CandidateCleanup:
      return "candidate_cleanup";
    case Stage::MedIR:
      return "med_ir";
    case Stage::NoReturnVerify:
      return "noreturn_verify";
    case Stage::MachineMedIR:
      return "machine_med_ir";
    case Stage::HighIR:
      return "high_ir";
    case Stage::LLVMEmission:
      return "llvm_emission";
    }
    return "unknown";
  }

  Milliseconds::rep elapsed() const noexcept {
    const int SavedErrno = errno;
    const auto Elapsed =
        std::chrono::duration_cast<Milliseconds>(Clock::now() - Started)
            .count();
    errno = SavedErrno;
    return Elapsed;
  }

  void record(const char *Event, Milliseconds::rep Elapsed) noexcept {
    const int SavedErrno = errno;
    try {
      llvm::SmallString<192> Line;
      llvm::raw_svector_ostream OS(Line);
      OS << "[neverd-pipeline-stage] invocation=" << Invocation
         << " stage=" << name() << " event=" << Event
         << " elapsed_ms=" << Elapsed << '\n';
      if (Line.size() <=
          ::neverd::limits::pipeline::kPipelineRecordLineSizeLimit) {
        llvm::raw_fd_ostream Sink(2, /*shouldClose=*/false,
                                  /*unbuffered=*/true);
        llvm::scope_exit ClearError([&Sink]() noexcept { Sink.clear_error(); });
        Sink.write(Line.data(), Line.size());
        Sink.flush();
      }
    } catch (...) {
      // Diagnostic failures must not replace the pipeline result or exception.
    }
    errno = SavedErrno;
  }

public:
  NativePipelineTrace() noexcept {
    const int SavedErrno = errno;
    const char *Value = std::getenv("NEVERD_NATIVE_PHASES");
    if (Value && Value[0] == '1' && Value[1] == '\0') {
      // Only diagnostics are capped. Each admitted run can emit at most seven
      // stage pairs, including one of the mutually exclusive final branches.
      static std::atomic<unsigned> Used{0};
      unsigned Count = Used.load(std::memory_order_relaxed);
      while (
          Count <
          ::neverd::limits::pipeline::kPipelineNativePipelineTraceCountLimit) {
        if (Used.compare_exchange_strong(Count, Count + 1,
                                         std::memory_order_relaxed)) {
          Invocation = Count + 1;
          break;
        }
      }
    }
    errno = SavedErrno;
  }
  NativePipelineTrace(const NativePipelineTrace &) = delete;
  NativePipelineTrace &operator=(const NativePipelineTrace &) = delete;

  ~NativePipelineTrace() noexcept {
    if (Active)
      record("aborted", elapsed());
  }

  void finish(bool Success) noexcept {
    if (!Active)
      return;
    Active = false;
    record(Success ? "completed" : "failed", elapsed());
  }

  void start(Stage Next) noexcept {
    if (!Invocation)
      return;
    const int SavedErrno = errno;
    finish(true);
    Current = Next;
    Started = Clock::now();
    Active = true;
    record("begin", 0);
    errno = SavedErrno;
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// Pipeline::run — orchestration
//===----------------------------------------------------------------------===//

PipelineResult Pipeline::run(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                             const PipelineOptions &Opts, DebugContext *Dbg) {
  PipelineResult Result;
  Result.SourceImage = &Img;
  if (Opts.MedAnalysisOnly &&
      (Opts.PatchMode || Opts.LiftMode || Opts.DeferHighIR || Opts.DeferMedIR ||
       Opts.DumpHigh || Opts.DumpLlvm || Opts.SourceProjection ||
       Opts.InterpreterSpecialization || Img.Arch == Arch::EVM ||
       Img.Arch == Arch::SBF)) {
    Result.Error = "MedIR analysis requires a native input and no output mode";
    return Result;
  }
  // Restricted runs of an image read the same callees for their signatures.
  if (Opts.AnalysisCache &&
      (!Opts.OnlyFunctionEntries.empty() || Opts.MaxFunctions != 0)) {
    auto &Memo = Opts.AnalysisCache->CalleeSignatures;
    if (!Memo)
      Memo = std::make_shared<OmittedCalleeMemo>();
    Result.CalleeSignatureMemo = Memo;
  }

  if (!analysis::interpreterMachineSourceABIInfo(Opts.InterpreterMachineABI)) {
    Result.Error = "unknown interpreter machine source ABI";
    return Result;
  }
  if (Opts.InterpreterMachineABI !=
          analysis::InterpreterMachineSourceABIKind::ScalarV1 &&
      (!Opts.InterpreterSpecialization ||
       !Opts.InterpreterSpecialization->ExplicitMachineState)) {
    Result.Error = "extended source ABI requires explicit machine recovery";
    return Result;
  }
  if (Opts.InterpreterSpecialization &&
      (Opts.PatchMode || Opts.OnlyFunctionEntries.size() != 1 ||
       Img.Arch != Arch::X64)) {
    Result.Error =
        "interpreter recovery requires one x64 entry and source/lift mode";
    return Result;
  }

  if (Img.Arch == Arch::EVM) {
    // The loader kept the container rather than the executable remainder,
    // because unwrapping it walks a constructor whose instruction boundaries
    // the hardfork decides. Redoing that walk here is what makes the session's
    // fork the one that answered, instead of whichever fork the loader
    // happened to default to.
    evm::BytecodeLoadOptions LoadOptions;
    LoadOptions.Fork = Opts.EVMFork;
    const bool SourceIsRuntime = Img.EVM && Img.EVM->SourceIsRuntime;
    const auto Source =
        Img.EVM ? Img.EVM->Source : evm::BytecodeSourceKind::Raw;
    auto Normalized = evm::normalizeBytecode(Img.Raw, Source, SourceIsRuntime,
                                             /*SourceName=*/{}, LoadOptions);
    if (!Normalized) {
      Result.Error = llvm::toString(Normalized.takeError());
      return Result;
    }
    if (llvm::Error Undecodable = evm::checkDecodable(*Normalized)) {
      Result.Error = llvm::toString(std::move(Undecodable));
      return Result;
    }

    evm::AnalyzeOptions EVMOptions;
    EVMOptions.Fork = Opts.EVMFork;
    EVMOptions.Strict = Opts.EVMStrict;
    auto Program = evm::analyze(Normalized->Code, EVMOptions);
    if (!Program) {
      Result.Error = llvm::toString(Program.takeError());
      return Result;
    }
    Result.EVM = std::make_unique<evm::EVMProgram>(std::move(*Program));
    if (Opts.EmitDumpOutput) {
      if (Opts.DumpLow)
        llvm::outs() << evm::dumpLowIR(Result.EVM->Low);
      if (Opts.DumpMed)
        llvm::outs() << evm::dumpMedIR(Result.EVM->Med);
      if (Opts.DumpHigh)
        llvm::outs() << evm::dumpHighIR(Result.EVM->High);
    }
    if (Opts.LiftMode || Opts.PatchMode || Opts.DumpLlvm) {
      auto Module = evm::emitLLVM(*Result.EVM, Ctx);
      if (!Module) {
        Result.Error = llvm::toString(Module.takeError());
        return Result;
      }
      Result.LlvmModule = std::move(*Module);
    }
    Result.Success = true;
    return Result;
  }

  if (Img.Arch == Arch::SBF) {
    if (Opts.PatchMode) {
      Result.Error =
          "sbf: binary patching is not supported; use lift or decompile";
      return Result;
    }
    sbf::AnalyzeOptions SBFOptions;
    SBFOptions.VersionOverride = Opts.SBFVersion;
    SBFOptions.Strict = Opts.SBFStrict;
    SBFOptions.Profile = Opts.SBFProfile;
    SBFOptions.Idl = Opts.SBFIdl;
    auto Program = sbf::analyze(Img, SBFOptions);
    if (!Program) {
      Result.Error = llvm::toString(Program.takeError());
      return Result;
    }
    Result.SBF = std::make_unique<sbf::SBFProgram>(std::move(*Program));
    if (Opts.EmitDumpOutput) {
      if (Opts.DumpLow)
        llvm::outs() << sbf::dumpLowIR(Result.SBF->Low);
      if (Opts.DumpMed)
        llvm::outs() << sbf::dumpMedIR(Result.SBF->Med);
      if (Opts.DumpHigh)
        llvm::outs() << sbf::dumpHighIR(Result.SBF->High);
    }
    if (Opts.LiftMode || Opts.DumpLlvm) {
      auto Module = sbf::emitLLVM(*Result.SBF, Ctx);
      if (!Module) {
        Result.Error = llvm::toString(Module.takeError());
        return Result;
      }
      Result.LlvmModule = std::move(*Module);
    }
    Result.Success = true;
    return Result;
  }

  NativePipelineTrace Trace;
  Trace.start(NativePipelineTrace::Stage::Decoder);
  Decoder Dec;
  if (!Dec.init(Img)) {
    Result.Error = "failed to initialize decoder for architecture " +
                   std::string(getArchName(Img.Arch));
    llvm::WithColor::error() << "pipeline: " << Result.Error << "\n";
    Trace.finish(false);
    return Result;
  }

  if (Opts.InterpreterSpecialization) {
    Trace.start(NativePipelineTrace::Stage::LowIR);
    if (Opts.InterpreterSpecialization->hasRawEnvironment()) {
      Result.Error =
          "machine-state source does not support raw environment domains";
      Trace.finish(false);
      return Result;
    }
    if (!Opts.InterpreterSpecialization->X64TerminalImports.empty()) {
      Result.Error =
          "machine-state source does not support terminal import domains";
      Trace.finish(false);
      return Result;
    }
    if (Opts.InterpreterSpecialization->X64LegacyGSState) {
      Result.Error = "machine-state recovery requires a versioned legacy GS "
                     "source interface";
      Trace.finish(false);
      return Result;
    }
    if (Opts.InterpreterSpecialization->X64MXCSRMask) {
      Result.Error =
          "machine-state recovery requires a versioned MXCSR source interface";
      Trace.finish(false);
      return Result;
    }
    if (Opts.InterpreterSpecialization->X64CR8State) {
      Result.Error =
          "machine-state recovery requires a versioned task-priority "
          "source interface";
      Trace.finish(false);
      return Result;
    }
    if (Opts.InterpreterSpecialization->X64SegmentedReads ||
        Opts.InterpreterSpecialization->X64SegmentedWrites) {
      Result.Error = "machine-state recovery requires a versioned FS/GS "
                     "base source interface";
      Trace.finish(false);
      return Result;
    }
    const va_t Entry = *Opts.OnlyFunctionEntries.begin();
    Result.InterpreterRecovery = analysis::specializeBinaryInterpreter(
        Img, Entry, *Opts.InterpreterSpecialization);
    auto &Recovery = *Result.InterpreterRecovery;
    if (!Recovery.complete()) {
      Result.Error = "interpreter recovery: " + Recovery.Diagnostic;
      Trace.finish(false);
      return Result;
    }
    if (Opts.InterpreterSpecialization->ExplicitMachineState) {
      auto Wrapped = analysis::wrapInterpreterMachineSourceX64(
          Recovery.Residual, Opts.InterpreterMachineABI, Img.Format,
          analysis::InterpreterMachineStateProfile::UserX64NoFaultV1,
          Opts.InterpreterSpecialization->EntryFrameAlignment,
          !Opts.PatchMode && !Opts.LiftMode && !Opts.DumpLlvm
              ? analysis::InterpreterMachineStateLayout::SharedGPRExit
              : analysis::InterpreterMachineStateLayout::InlineReturns);
      if (!Wrapped) {
        Result.Error =
            "machine-state recovery: " + llvm::toString(Wrapped.takeError());
        Trace.finish(false);
        return Result;
      }
      Result.InterpreterMachineSourceABI = std::move(Wrapped->SourceABI);
      Result.LowFuncs.push_back(std::move(Wrapped->Function));
    } else {
      Result.LowFuncs.push_back(Recovery.Residual);
    }
    PipelineFunctionAudit Audit;
    Audit.Entry = Entry;
    Audit.Name = Recovery.Residual.Name;
    Audit.HasLowIR = true;
    Result.FunctionAudits.push_back(std::move(Audit));
  } else {
    Trace.start(NativePipelineTrace::Stage::FunctionDetection);
    auto Candidates = detectFunctions(Img, Dec, Opts, Dbg, Result);

    // Phase 1: Build LowIR (parallel).
    Trace.start(NativePipelineTrace::Stage::LowIR);
    buildLowIR(Img, Candidates, Opts, Dbg, Result);

    Trace.start(NativePipelineTrace::Stage::CandidateCleanup);
    // Remove spurious functions whose entry coincides with a jump-table
    // target of another function.  The function detector may promote
    // call-scan targets that are actually switch-case destinations.
    {
      std::set<va_t> JTTargets;
      for (auto &LF : Result.LowFuncs)
        for (auto &JT : LF.JumpTables)
          for (va_t T : JT.Targets)
            JTTargets.insert(T);

      if (!JTTargets.empty()) {
        for (const auto &LF : Result.LowFuncs) {
          if (!JTTargets.count(LF.Entry) || !LF.JumpTables.empty())
            continue;
          auto AuditIt = std::find_if(Result.FunctionAudits.begin(),
                                      Result.FunctionAudits.end(),
                                      [&](const PipelineFunctionAudit &Audit) {
                                        return Audit.Entry == LF.Entry;
                                      });
          if (AuditIt != Result.FunctionAudits.end()) {
            AuditIt->Disposition =
                PipelineFunctionDisposition::RemovedJumpTableTarget;
            AuditIt->HasLowIR = false;
          }
        }
        size_t Before = Result.LowFuncs.size();
        Result.LowFuncs.erase(
            std::remove_if(Result.LowFuncs.begin(), Result.LowFuncs.end(),
                           [&](const LowFunc &LF) {
                             return JTTargets.count(LF.Entry) &&
                                    LF.JumpTables.empty();
                           }),
            Result.LowFuncs.end());
        size_t Removed = Before - Result.LowFuncs.size();
        if (Removed > 0)
          LLVM_DEBUG(llvm::dbgs()
                     << "pipeline: removed " << Removed
                     << " spurious functions (jump-table targets)\n");
      }
    }

    // A padding-boundary guess that another function actually decodes is
    // part of that function, including resumable fallthrough (see
    // BinaryImage::boundaryGuessFunctionStarts).  Keep it as a separate
    // function only when nothing absorbed it.
    {
      // A run that shares the analysis cache read them from it (buildLowIR).
      PipelineAnalysisCache *Shared = sharedAnalysisCache(Img, Opts);
      const std::shared_ptr<const PipelineAnalysisCache::SymbolInventory>
          Inventory =
              Shared ? PipelineAnalysisCache::SymbolInventory::of(Img, *Shared)
                     : nullptr;
      std::set<va_t> OwnGuesses;
      if (!Inventory)
        OwnGuesses = Img.boundaryGuessFunctionStarts();
      const std::set<va_t> &Guesses =
          Inventory ? Inventory->BoundaryGuesses : OwnGuesses;
      std::set<va_t> Absorbed;
      if (!Guesses.empty())
        for (const auto &LF : Result.LowFuncs)
          for (const auto &Block : LF.Blocks)
            for (const auto &Insn : Block.InstructionBoundaries)
              if (Insn.Address != LF.Entry && Guesses.count(Insn.Address))
                Absorbed.insert(Insn.Address);
      // Missing unwind metadata is not evidence that decoded code is data.
      // Only an actual CFG owner can absorb a guessed entry. Instruction
      // completeness and exception-rewrite admission remain separate checks.
      if (!Absorbed.empty()) {
        for (auto &Audit : Result.FunctionAudits) {
          if (Absorbed.count(Audit.Entry)) {
            Audit.Disposition =
                PipelineFunctionDisposition::AbsorbedFunctionChunk;
            Audit.HasLowIR = false;
          }
        }
        Result.LowFuncs.erase(std::remove_if(Result.LowFuncs.begin(),
                                             Result.LowFuncs.end(),
                                             [&](const LowFunc &LF) {
                                               return Absorbed.count(LF.Entry);
                                             }),
                              Result.LowFuncs.end());
      }
    }
  }

  for (const auto &LF : Result.LowFuncs) {
    auto AuditIt =
        std::find_if(Result.FunctionAudits.begin(), Result.FunctionAudits.end(),
                     [&](const PipelineFunctionAudit &Audit) {
                       return Audit.Entry == LF.Entry;
                     });
    if (AuditIt != Result.FunctionAudits.end())
      AuditIt->Disposition = PipelineFunctionDisposition::Accepted;
  }

  if (Opts.DumpLow && Opts.EmitDumpOutput)
    dumpLowIR(Result.LowFuncs);

  // A native view of the control-flow graphs alone reads nothing after
  // LowIR.  A deeper view of the same run takes the stages below then
  // (resumeMedIR), which keep this order.
  if (Opts.DeferMedIR && !Opts.DumpLow && !Opts.DumpMed && !Opts.DumpHigh &&
      !Opts.PatchMode && !Opts.LiftMode && !Opts.InterpreterSpecialization) {
    Result.MedIRDeferred = true;
    Result.Success = true;
    Trace.finish(true);
    return Result;
  }

  // Phase 2: LowIR -> MedIR (parallel).
  Trace.start(NativePipelineTrace::Stage::MedIR);
  buildMedIR(Img, Opts, Result);
  if (!Result.Error.empty()) {
    Result.Success = false;
    Trace.finish(false);
    return Result;
  }
  Trace.start(NativePipelineTrace::Stage::NoReturnVerify);
  propagateInternalNoReturn(Result.MedFuncs, Img.Arch, Img.UserNoReturn,
                            importCallAddresses(Img));

  // One read-only proof boundary serves both C routes. Neither annotation nor
  // its display name becomes input to ABI recovery or instruction semantics.
  recognizeLibraries(Img, Opts, Result, Dbg);

  if (Opts.DumpMed && Opts.EmitDumpOutput)
    dumpMedIR(Result.MedFuncs);

  if (Result.MedIRVerifierFailures != 0 &&
      (Opts.usesMachineMedIR() || Opts.InterpreterSpecialization)) {
    Result.Error = "MedIR verification failed";
    Result.Success = false;
    Trace.finish(false);
    return Result;
  }

  // If only dumping intermediate IR (LowIR/MedIR), skip LLVM emission
  // entirely.  The dump flags are handled above; return early to avoid
  // hitting the LLVM PassManager (which may crash on certain LLVM builds).
  if ((Opts.DumpLow || Opts.DumpMed) && !Opts.DumpHigh &&
      !Opts.usesMachineMedIR()) {
    Result.Success = true;
    Trace.finish(true);
    return Result;
  }

  // Machine ABI preparation is shared by analysis and LLVM emission.
  if (Opts.usesMachineMedIR()) {
    Trace.start(Opts.MedAnalysisOnly
                    ? NativePipelineTrace::Stage::MachineMedIR
                    : NativePipelineTrace::Stage::LLVMEmission);
    Result.Success = runPatchLiftMode(Img, Ctx, Opts, Result, Dbg);
    Trace.finish(Result.Success);
    return Result;
  }

  // Source and analysis views share ABI/return recovery. Views that do not
  // consume HighIR postpone only that projection, retaining those proofs.
  if (Opts.DeferHighIR && !Opts.DumpHigh) {
    Trace.start(NativePipelineTrace::Stage::HighIR);
    prepareDeferredSource(Img, Result, Dbg, sharedAnalysisCache(Img, Opts),
                          refusalFailsRun(Opts));
    Trace.finish(Result.Success);
    return Result;
  }

  // Phase 3: MedIR -> HighIR (parallel).
  Trace.start(NativePipelineTrace::Stage::HighIR);
  if (!pipeline_detail::runHighIRStage(
          Result, [&] { buildHighIR(Img, Opts, Result, Dbg); })) {
    Trace.finish(false);
    return Result;
  }

  if (Opts.DumpHigh && Opts.EmitDumpOutput)
    dumpHighIR(Result.HighFuncs);

  LLVM_DEBUG(llvm::dbgs() << "pipeline: HighIR ready ("
                          << Result.HighFuncs.size()
                          << " functions, for C emission)\n");

  Result.Success = true;
  Trace.finish(true);
  return Result;
}

void Pipeline::prepareDeferredSource(const BinaryImage &Img,
                                     PipelineResult &Result, DebugContext *Dbg,
                                     PipelineAnalysisCache *Cache,
                                     bool FailsRun) {
  try {
    prepareSourceMedIR(Img, Result, Dbg, Cache);
    if (!refuseObservedCallReturnReads(Img, Result, FailsRun, Dbg))
      throw std::runtime_error(Result.Error);
    Result.HighIRDeferred = true;
    Result.Success = true;
  } catch (const std::exception &Error) {
    Result.Success = false;
    Result.Error =
        std::string("source MedIR preparation failed: ") + Error.what();
  } catch (...) {
    Result.Success = false;
    Result.Error = "source MedIR preparation failed: unknown exception";
  }
}

bool Pipeline::resumeMedIR(const BinaryImage &Img, const PipelineOptions &Opts,
                           PipelineResult &Result, DebugContext *Dbg) {
  if (!Result.Success || !Result.MedIRDeferred)
    return Result.Success;
  if (Result.SourceImage != &Img) {
    Result.Success = false;
    Result.Error = "deferred MedIR belongs to a different image";
    return false;
  }
  Result.MedIRDeferred = false;
  Result.Success = false;
  // The stages run() takes after LowIR for a native source or analysis view.
  NativePipelineTrace Trace;
  Trace.start(NativePipelineTrace::Stage::MedIR);
  buildMedIR(Img, Opts, Result);
  if (!Result.Error.empty()) {
    Trace.finish(false);
    return false;
  }
  Trace.start(NativePipelineTrace::Stage::NoReturnVerify);
  propagateInternalNoReturn(Result.MedFuncs, Img.Arch, Img.UserNoReturn,
                            importCallAddresses(Img));
  recognizeLibraries(Img, Opts, Result, Dbg);
  Trace.start(NativePipelineTrace::Stage::HighIR);
  if (Opts.DeferHighIR)
    prepareDeferredSource(Img, Result, Dbg, sharedAnalysisCache(Img, Opts),
                          refusalFailsRun(Opts));
  else
    Result.Success = pipeline_detail::runHighIRStage(
        Result, [&] { buildHighIR(Img, Opts, Result, Dbg); });
  Trace.finish(Result.Success);
  return Result.Success;
}

} // namespace neverd
