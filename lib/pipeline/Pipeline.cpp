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

#include "PipelineHighIRDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/evm/emit/EVMLLVMEmitter.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/loader/BinaryImage.h"
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
      if (Line.size() <= 192) {
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
      while (Count < 32) {
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
  if (!Dec.init(Img.Arch, Img.Mode)) {
    Result.Error = "failed to initialize decoder for architecture " +
                   std::string(getArchName(Img.Arch));
    llvm::WithColor::error() << "pipeline: " << Result.Error << "\n";
    Trace.finish(false);
    return Result;
  }

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
      Result.LowFuncs.erase(std::remove_if(Result.LowFuncs.begin(),
                                           Result.LowFuncs.end(),
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

  // Phase 2: LowIR -> MedIR (parallel).
  Trace.start(NativePipelineTrace::Stage::MedIR);
  buildMedIR(Img, Opts, Result);
  Trace.start(NativePipelineTrace::Stage::NoReturnVerify);
  propagateInternalNoReturn(Result.MedFuncs, Img.Arch);

  if (Opts.DumpMed && Opts.EmitDumpOutput)
    dumpMedIR(Result.MedFuncs);

  if (Result.MedIRVerifierFailures != 0) {
    Result.Error = "MedIR verification failed";
    Result.Success = false;
    Trace.finish(false);
    return Result;
  }

  // If only dumping intermediate IR (LowIR/MedIR), skip LLVM emission
  // entirely.  The dump flags are handled above; return early to avoid
  // hitting the LLVM PassManager (which may crash on certain LLVM builds).
  if ((Opts.DumpLow || Opts.DumpMed) && !Opts.DumpHigh && !Opts.PatchMode &&
      !Opts.LiftMode) {
    Result.Success = true;
    Trace.finish(true);
    return Result;
  }

  // Patch/Lift mode: MedIR -> LLVM IR, skip HighIR.
  if (Opts.PatchMode || Opts.LiftMode) {
    Trace.start(NativePipelineTrace::Stage::LLVMEmission);
    Result.Success = runPatchLiftMode(Img, Ctx, Opts, Result);
    Trace.finish(Result.Success);
    return Result;
  }

  // Phase 3: MedIR -> HighIR (parallel).
  Trace.start(NativePipelineTrace::Stage::HighIR);
  if (!pipeline_detail::runHighIRStage(
          Result, [&] { buildHighIR(Img, Opts, Result); })) {
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

} // namespace neverd
