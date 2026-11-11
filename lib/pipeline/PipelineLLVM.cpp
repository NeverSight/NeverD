//===- PipelineLLVM.cpp - Parallel LLVM pipeline emission ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Sharded LLVM IR emission, optimization, and linking.
///
//===----------------------------------------------------------------------===//

#include "PipelineLLVMDetail.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMCallContract.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/StackSizeMain.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

namespace {

/// Coordinates are valid only across serialization of this exact module.
/// They are discarded immediately after parsing its bitcode; transforms and
/// linking use weak LLVM handles again, never stale instruction ordinals.
struct SerializedSources {
  std::map<va_t, std::string> Functions;
  struct Observation {
    va_t Entry;
    sigs::LibraryOccurrence Occurrence;
    std::string Function;
    size_t Instruction;
    bool ExpressionDerived;
  };
  std::vector<Observation> Observations;
};

SerializedSources serializeSources(const LLVMSourceMap &Sources,
                                   const llvm::Module &Module) {
  SerializedSources Result;
  std::map<const llvm::Instruction *, size_t> Ordinals;
  for (const auto &Function : Module) {
    size_t Index = 0;
    for (const auto &Block : Function)
      for (const auto &Instruction : Block)
        Ordinals[&Instruction] = Index++;
  }
  for (const auto &[Entry, Handle] : Sources.Functions)
    if (auto *Function = llvm::dyn_cast_or_null<llvm::Function>(Handle);
        Function && Function->getParent() == &Module)
      Result.Functions.emplace(Entry, Function->getName().str());
  for (const auto &Source : Sources.Observations) {
    auto *Instruction = llvm::dyn_cast_or_null<llvm::Instruction>(Source.Value);
    auto Found = Ordinals.find(Instruction);
    if (!Instruction || Found == Ordinals.end())
      continue;
    Result.Observations.push_back({Source.Function, Source.Occurrence,
                                   Instruction->getFunction()->getName().str(),
                                   Found->second, Source.ExpressionDerived});
  }
  return Result;
}

void restoreSources(const SerializedSources &Sources, llvm::Module &Module,
                    LLVMSourceMap &Result) {
  std::map<std::string, std::vector<llvm::Instruction *>> Instructions;
  for (auto &Function : Module) {
    auto &At = Instructions[Function.getName().str()];
    for (auto &Block : Function)
      for (auto &Instruction : Block)
        At.push_back(&Instruction);
  }
  for (const auto &[Entry, Name] : Sources.Functions)
    if (auto *Function = Module.getFunction(Name))
      Result.Functions[Entry] = Function;
  for (const auto &Source : Sources.Observations) {
    auto Function = Instructions.find(Source.Function);
    if (Function != Instructions.end() &&
        Source.Instruction < Function->second.size())
      Result.Observations.push_back({Source.Entry, Source.Occurrence,
                                     Function->second[Source.Instruction],
                                     Source.ExpressionDerived});
  }
}

bool isFatalOptimizationStop(OptimizationStopReason Stop) {
  return Stop == OptimizationStopReason::InputInvalid ||
         Stop == OptimizationStopReason::VerificationFailed;
}

} // namespace

//===----------------------------------------------------------------------===//
// Parallel LLVM emission + optimization
//===----------------------------------------------------------------------===//

Pipeline::LLVMEmissionResult Pipeline::emitLLVMSharded(
    const std::vector<MedFunc> &Funcs, llvm::LLVMContext &Ctx, Arch TheArch,
    const std::vector<std::pair<va_t, std::string>> &Imports,
    const BinaryImage &Img, BinaryFormat Fmt, bool NoOpt, unsigned NumThreads,
    bool RecordSources, llvm::ArrayRef<sigs::LibraryRecognition> Recognitions,
    const DebugContext *Dbg, const SourceViewOptions &View,
    const std::set<va_t> *PrototypeArgumentCallees,
    bool PreserveUnknownSourceValues,
    llvm::ArrayRef<MedFunc> SourceCalleeDeclarations, bool OmitRefusedBodies,
    bool AnalysisRecoveryForSource) {
  const size_t N = Funcs.size();
  const bool OriginalAnalysisImage =
      AnalysisRecoveryForSource &&
      llvm::any_of(Funcs, [](const MedFunc &F) { return F.AnalysisOnly; });
  NumThreads = std::max(
      1u, std::min<unsigned>(NumThreads, static_cast<unsigned>(N ? N : 1)));
  const pipeline_detail::LLVMShardPlan ShardPlan =
      pipeline_detail::planLLVMEmissionShards(Funcs, &Img);
  auto EmitShard = [&](unsigned S, llvm::LLVMContext &ShardCtx) {
    std::vector<char> Mask(N, 0);
    for (size_t I = 0; I < N; ++I)
      Mask[I] = (ShardPlan.ShardOf[I] == S);
    LLVMEmissionResult Result;
    // Where the unit names what it leaves out, a body that refuses a stale
    // address, or whose contract the emitter refuses before any module,
    // stays declared only, with why, and the shard is emitted again without
    // it: one refusal must not cost the unit every other definition. Each
    // round leaves out at least one body.
    for (;;) {
      MedLLVMEmitter Emitter;
      // The shards' masks partition the unit, and a body left out is noted.
      Emitter.setMaskedBodiesOwnedElsewhere(true);
      Emitter.setPreserveUnknownSourceValues(PreserveUnknownSourceValues);
      Emitter.setAnalysisRecoveryForSource(AnalysisRecoveryForSource);
      Emitter.setOriginalImageStorageForSource(OriginalAnalysisImage);
      Emitter.setSourceCalleeDeclarations(SourceCalleeDeclarations);
      Emitter.setDebugContext(Dbg);
      Emitter.setPrototypeArgumentCallees(PrototypeArgumentCallees);
      Result.Sources.reset();
      if (RecordSources) {
        Result.Sources = std::make_shared<LLVMSourceMap>();
        Emitter.setSourceMap(Result.Sources.get());
      }
      try {
        Result.Module = Emitter.emit(Funcs, ShardCtx, "neverd_output", TheArch,
                                     Imports, &Img, Fmt,
                                     /*MergeableGlobals=*/true, &Mask);
      } catch (const std::exception &) {
        // Any other error, and a refusal the unit cannot name, fails the
        // shard with the emitter's text.
        if (!OmitRefusedBodies || Emitter.refusedBodies().empty())
          throw;
        Result.Module.reset();
      }
      Result.UnhandledValueIntrinsics = Emitter.unhandledValueIntrinsicCount();
      if (Result.Module || !OmitRefusedBodies ||
          Emitter.refusedBodies().empty()) {
        // A unit that cannot leave a refused body out fails with why.
        if (!Result.Module && !Emitter.refusedBodies().empty())
          Result.Error = Emitter.refusedBodies().front().Reason;
        break;
      }
      for (const MedLLVMEmitter::RefusedBody &Refused :
           Emitter.refusedBodies()) {
        Mask[Refused.Index] = 0;
        Result.LeftOut.emplace_back(Refused.Index, Refused.Reason);
      }
    }
    if (Result.Sources)
      Result.Sources->preserveExpressionOrigins(Recognitions);
    return Result;
  };
  return runLLVMShardPipeline(Funcs, Ctx, ShardPlan.NumShards, NumThreads,
                              NoOpt, EmitShard, View, OmitRefusedBodies);
}

Pipeline::LLVMEmissionResult Pipeline::runLLVMShardPipeline(
    const std::vector<MedFunc> &Funcs, llvm::LLVMContext &Ctx,
    unsigned NumShards, unsigned NumThreads, bool NoOpt,
    llvm::function_ref<LLVMEmissionResult(unsigned, llvm::LLVMContext &)> Emit,
    const SourceViewOptions &View, bool OmitRefusedBodies) {
  LLVMEmissionResult Result;
  if (NumShards == 0) {
    Result.Error = "LLVM shard planning failed: no shards";
    return Result;
  }
  NumThreads = std::max(1u, std::min(NumThreads, NumShards));
  struct ShardResult {
    std::string Bitcode;
    std::optional<SerializedSources> Sources;
    uint64_t UnhandledValueIntrinsics = 0;
    bool LLVMVerifierFailed = false;
    std::string Error;
    std::vector<std::pair<size_t, std::string>> LeftOut;
  };
  std::vector<ShardResult> Shards(NumShards);
  auto appendError = [&](const std::string &Error) {
    if (!Result.Error.empty())
      Result.Error += "\n";
    Result.Error += Error;
  };
  std::string Phase = "initialization";
  try {
    // Warm up LLVM's lazily-initialized global state (pass registries, managed
    // statics) single-threaded before the parallel region touches it from many
    // threads at once through optimizeModule.  Once per process suffices.
    static std::once_flag WarmupOnce;
    std::call_once(WarmupOnce, [] {
      llvm::LLVMContext WarmCtx;
      llvm::Module Warm("neverd_warmup", WarmCtx);
      OptimizationOptions Options;
      OptimizationResult Result = optimizeModule(Warm, Options);
      if (isFatalOptimizationStop(Result.Stop))
        llvm::WithColor::warning() << "pipeline: LLVM optimizer warmup failed: "
                                   << optimizationStopMessage(Result) << "\n";
      promoteScaffoldingAllocas(Warm);
    });

    // A shard owns its context, module, counters and diagnostic slot. Cross-
    // context transfer goes through bitcode, and at most NumThreads modules are
    // live at once. Worker callbacks never publish a partial linked module.
    auto runShard = [&](unsigned S) {
      ShardResult &Shard = Shards[S];
      const char *ShardPhase = "emission";
      auto fail = [&](const std::string &Detail) {
        Shard.Error = "LLVM shard " + std::to_string(S) + " " + ShardPhase +
                      " failed: " + Detail;
      };
      try {
        llvm::LLVMContext ShardCtx;
        LLVMEmissionResult Emission = Emit(S, ShardCtx);
        Shard.UnhandledValueIntrinsics = Emission.UnhandledValueIntrinsics;
        Shard.LLVMVerifierFailed = Emission.LLVMVerifierFailed;
        Shard.LeftOut = std::move(Emission.LeftOut);
        auto M = std::move(Emission.Module);
        if (!Emission.Error.empty()) {
          fail(Emission.Error);
          return;
        }
        if (!M) {
          fail("emitter returned no module");
          return;
        }
        ShardPhase = "verification";
        std::string VerifyError;
        llvm::raw_string_ostream VerifyStream(VerifyError);
        if (llvm::verifyModule(*M, &VerifyStream)) {
          Shard.LLVMVerifierFailed = true;
          fail(VerifyError);
          return;
        }
        if (!NoOpt) {
          ShardPhase = "optimization";
          OptimizationResult Optimization =
              optimizeOrPromoteModule(*M, Emission.Sources.get(), View);
          if (Optimization.Stop == OptimizationStopReason::InputInvalid) {
            // The shard was verified above. A blocking native EH contract
            // keeps value-changing passes off this module; the shared helper
            // already promoted its temporary allocas.
          } else if (isFatalOptimizationStop(Optimization.Stop)) {
            Shard.LLVMVerifierFailed = true;
            fail(optimizationStopMessage(Optimization));
            return;
          }
        } else {
          ShardPhase = "canonicalization";
          promoteScaffoldingAllocas(*M);
          if (Emission.Sources)
            Emission.Sources->refreshExpressionOrigins();
        }
        ShardPhase = "post-optimization verification";
        std::string FinalVerifyError;
        llvm::raw_string_ostream FinalVerifyStream(FinalVerifyError);
        if (!normalizeResolvedLLVMCalls(*M, Emission.Sources.get())) {
          fail("resolved call signature mismatch");
          return;
        }
        if (llvm::verifyModule(*M, &FinalVerifyStream)) {
          Shard.LLVMVerifierFailed = true;
          fail(FinalVerifyError);
          return;
        }
        if (!validateResolvedLLVMCallSignatures(*M)) {
          fail("resolved call signature mismatch");
          return;
        }
        ShardPhase = "serialization";
        if (Emission.Sources)
          Shard.Sources = serializeSources(*Emission.Sources, *M);
        llvm::raw_string_ostream Stream(Shard.Bitcode);
        llvm::WriteBitcodeToFile(*M, Stream);
        if (Shard.Bitcode.empty())
          fail("empty bitcode");
      } catch (const std::exception &Error) {
        fail(Error.what());
      } catch (...) {
        fail("unknown exception");
      }
    };
    Phase = "worker execution";
    if (NumThreads == 1) {
      for (unsigned S = 0; S < NumShards; ++S)
        runShard(S);
    } else {
      std::atomic<unsigned> Next{0};
      auto Worker = [&] {
        for (unsigned S;
             (S = Next.fetch_add(1, std::memory_order_relaxed)) < NumShards;)
          runShard(S);
      };
      runWithLargeStackThreads(NumThreads, Worker);
    }

    // Collect owned outcomes only after every worker has finished. Keep errors
    // in shard order, regardless of which worker completed first.
    for (unsigned S = 0; S < NumShards; ++S) {
      const ShardResult &Shard = Shards[S];
      Result.UnhandledValueIntrinsics += Shard.UnhandledValueIntrinsics;
      Result.LLVMVerifierFailed |= Shard.LLVMVerifierFailed;
      Result.LeftOut.insert(Result.LeftOut.end(), Shard.LeftOut.begin(),
                            Shard.LeftOut.end());
      if (!Shard.Error.empty())
        appendError(Shard.Error);
      else if (Shard.Bitcode.empty())
        appendError("LLVM shard " + std::to_string(S) +
                    " serialization failed: missing bitcode");
    }
    if (!Result.Error.empty())
      return Result;

    // Serial link into the caller's context, in shard order for determinism.
    // Each shard's bitcode is released as soon as it is linked.
    auto Linked = std::make_unique<llvm::Module>("neverd_output", Ctx);
    llvm::Linker Linker(*Linked);
    unsigned LinkedShards = 0;
    std::map<va_t, std::string> LinkedFunctionNames;
    for (unsigned S = 0; S < NumShards; ++S) {
      std::string ShardBC = std::move(Shards[S].Bitcode);
      Shards[S].Bitcode.clear();
      Shards[S].Bitcode.shrink_to_fit();
      Phase = "shard " + std::to_string(S) + " bitcode parsing";
      auto Buffer = llvm::MemoryBufferRef(ShardBC, "neverd_shard");
      auto Module = llvm::parseBitcodeFile(Buffer, Ctx);
      if (!Module) {
        Result.Error =
            "LLVM " + Phase + " failed: " + llvm::toString(Module.takeError());
        return Result;
      }
      Phase = "shard " + std::to_string(S) + " linking";
      if (Shards[S].Sources) {
        if (!Result.Sources)
          Result.Sources = std::make_shared<LLVMSourceMap>();
        restoreSources(*Shards[S].Sources, **Module, *Result.Sources);
        LinkedFunctionNames.insert(Shards[S].Sources->Functions.begin(),
                                   Shards[S].Sources->Functions.end());
      }
      if (Linker.linkInModule(std::move(*Module))) {
        Result.Error = "LLVM " + Phase + " failed";
        return Result;
      }
      ++LinkedShards;
    }
    if (LinkedShards != NumShards) {
      Result.Error = "LLVM shard linking failed: incomplete module";
      return Result;
    }
    // A body whose resolved call the declaration another shard linked
    // cannot take is left out, as a shard leaves out a body it refuses, where
    // the unit names what it leaves out: it stays declared, with its source
    // identity, and why is noted.  The body is found by that identity, which
    // a name a shard disambiguated (`write_str_10002F90C`) still carries.
    std::map<va_t, size_t> IndexOf;
    if (OmitRefusedBodies)
      for (size_t I = 0; I < Funcs.size(); ++I)
        IndexOf.emplace(Funcs[I].Entry, I);
    std::vector<ResolvedCallRefusal> Refusals;
    auto *Collect = OmitRefusedBodies ? &Refusals : nullptr;
    bool LeftOutBodies = false;
    const auto Reconciled = [&](bool Kept) {
      if (Kept)
        return true;
      if (!Collect)
        return false;
      for (ResolvedCallRefusal &Refusal : Refusals) {
        llvm::Function &Body = *Refusal.Caller;
        auto Identity = rewrite_source::getOriginalVA(Body);
        if (!Identity) {
          llvm::consumeError(Identity.takeError());
          return false;
        }
        const auto Index = *Identity ? IndexOf.find(**Identity) : IndexOf.end();
        if (Index == IndexOf.end() || Body.isDeclaration())
          return false;
        const va_t Entry = Funcs[Index->second].Entry;
        if (Result.Sources)
          for (auto It = Result.Sources->Functions.begin();
               It != Result.Sources->Functions.end();)
            It = It->second == &Body ? Result.Sources->Functions.erase(It)
                                     : std::next(It);
        LinkedFunctionNames.erase(Entry);
        Body.deleteBody();
        rewrite_source::setOriginalVA(Body, Entry);
        Result.LeftOut.emplace_back(Index->second, std::move(Refusal.Reason));
      }
      Refusals.clear();
      LeftOutBodies = true;
      return true;
    };
    if (!Reconciled(normalizeResolvedLLVMCalls(*Linked, Result.Sources.get(),
                                               false, Collect)) ||
        !Reconciled(
            validateResolvedLLVMCallSignatures(*Linked, false, Collect)) ||
        (LeftOutBodies && !validateResolvedLLVMCallSignatures(*Linked))) {
      Result.Error =
          "LLVM shard linking failed: resolved call signature mismatch";
      return Result;
    }
    // The linker may move a body into an existing declaration and delete its
    // old Function without RAUW. Rebind the explicitly serialized entry/name
    // pairs after this exact link, before any further transformation.
    if (Result.Sources)
      for (const auto &[Entry, Name] : LinkedFunctionNames)
        Result.Sources->Functions[Entry] = Linked->getFunction(Name);
    Phase = "function ordering";

    // Restore the original (address-order) function layout.  Sharding + link
    // order interleaves definitions by shard, but the serial path emits them in
    // Funcs order and downstream consumers rely on that — notably the
    // recompiled object's .text starts with the entry function, which the
    // round-trip harness executes from offset 0.  Stable-sort the module's
    // function list by original index; declarations not in Funcs
    // (imports/stubs) keep their relative order at the end.  This makes the
    // linked module's layout match the serial path.
    {
      std::map<llvm::StringRef, size_t> OrigIdx;
      for (size_t I = 0; I < Funcs.size(); ++I)
        if (!Funcs[I].Name.empty())
          OrigIdx.emplace(llvm::StringRef(Funcs[I].Name), I);
      auto rank = [&](const llvm::Function &F) -> size_t {
        auto It = OrigIdx.find(F.getName());
        return It == OrigIdx.end() ? std::numeric_limits<size_t>::max()
                                   : It->second;
      };
      Linked->getFunctionList().sort(
          [&](const llvm::Function &A, const llvm::Function &B) {
            return rank(A) < rank(B);
          });
    }

    Result.Module = std::move(Linked);
  } catch (const std::exception &Error) {
    appendError("LLVM " + Phase + " failed: " + Error.what());
  } catch (...) {
    appendError("LLVM " + Phase + " failed: unknown exception");
  }
  return Result;
}

} // namespace neverd
