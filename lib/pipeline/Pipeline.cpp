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

#include "neverd/decode/Decoder.h"
#include "neverd/evm/Analyzer.h"
#include "neverd/evm/Bytecode.h"
#include "neverd/evm/LLVMEmitter.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/sbf/Analyzer.h"
#include "neverd/sbf/LLVMEmitter.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

//===----------------------------------------------------------------------===//
// Pipeline::run — orchestration
//===----------------------------------------------------------------------===//

PipelineResult Pipeline::run(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                             const PipelineOptions &Opts, DebugContext *Dbg) {
  PipelineResult Result;

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

  Decoder Dec;
  if (!Dec.init(Img.Arch, Img.Mode)) {
    llvm::WithColor::error() << "pipeline: failed to init decoder\n";
    return Result;
  }

  auto Candidates = detectFunctions(Img, Dec, Opts, Dbg, Result);

  // Phase 1: Build LowIR (parallel).
  buildLowIR(Img, Candidates, Opts, Dbg, Result);

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
  buildMedIR(Img, Opts, Result);

  if (Opts.DumpMed && Opts.EmitDumpOutput)
    dumpMedIR(Result.MedFuncs);

  if (Result.MedIRVerifierFailures != 0) {
    Result.Error = "MedIR verification failed";
    Result.Success = false;
    return Result;
  }

  // If only dumping intermediate IR (LowIR/MedIR), skip LLVM emission
  // entirely.  The dump flags are handled above; return early to avoid
  // hitting the LLVM PassManager (which may crash on certain LLVM builds).
  if ((Opts.DumpLow || Opts.DumpMed) && !Opts.DumpHigh && !Opts.PatchMode) {
    Result.Success = true;
    return Result;
  }

  // Patch/Lift mode: MedIR -> LLVM IR, skip HighIR.
  if (Opts.PatchMode || Opts.LiftMode) {
    Result.Success = runPatchLiftMode(Img, Ctx, Opts, Result);
    return Result;
  }

  // Phase 3: MedIR -> HighIR (parallel).
  buildHighIR(Img, Opts, Result);

  if (Opts.DumpHigh && Opts.EmitDumpOutput)
    dumpHighIR(Result.HighFuncs);

  LLVM_DEBUG(llvm::dbgs() << "pipeline: HighIR ready ("
                          << Result.HighFuncs.size()
                          << " functions, for C emission)\n");

  Result.Success = true;
  return Result;
}

} // namespace neverd
