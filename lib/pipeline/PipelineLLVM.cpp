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

#include "neverd/Limits.h"
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
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

namespace {

bool isFatalOptimizationStop(OptimizationStopReason Stop) {
  return Stop == OptimizationStopReason::InputInvalid ||
         Stop == OptimizationStopReason::VerificationFailed;
}

} // namespace

pipeline_detail::LLVMShardPlan
pipeline_detail::planLLVMEmissionShards(const std::vector<MedFunc> &Funcs,
                                        unsigned NumThreads) {
  struct WorkUnit {
    uint64_t Weight = 0;
    size_t FirstFunction = 0;
    std::vector<size_t> Functions;
  };

  const size_t N = Funcs.size();
  LLVMShardPlan Plan;
  Plan.ShardOf.assign(N, 0);

  std::vector<WorkUnit> Units;
  Units.reserve(N);
  std::map<va_t, size_t> UnitForNativeFuncInfo;
  uint64_t TotalWeight = 0;
  for (size_t I = 0; I < N; ++I) {
    uint64_t Weight = 1;
    for (const auto &B : Funcs[I].Blocks)
      Weight += B.Ops.size() + B.Phis.size();
    TotalWeight += Weight;

    va_t NativeFuncInfoVA = 0;
    if (Funcs[I].ExceptionMetadata && Funcs[I].ExceptionMetadata->Cxx)
      NativeFuncInfoVA = Funcs[I].ExceptionMetadata->Cxx->NativeFuncInfoVA;

    size_t UnitIndex = Units.size();
    if (NativeFuncInfoVA != 0) {
      auto [It, Inserted] =
          UnitForNativeFuncInfo.emplace(NativeFuncInfoVA, UnitIndex);
      if (!Inserted)
        UnitIndex = It->second;
    }
    if (UnitIndex == Units.size()) {
      WorkUnit Unit;
      Unit.FirstFunction = I;
      Units.push_back(std::move(Unit));
    }
    Units[UnitIndex].Weight += Weight;
    Units[UnitIndex].Functions.push_back(I);
  }

  const size_t NumUnits = Units.size();
  NumThreads = std::max(
      1u, std::min<unsigned>(NumThreads,
                             static_cast<unsigned>(NumUnits ? NumUnits : 1)));

  // Slice size, not core count, is what bounds peak memory here: a shard holds
  // its own LLVMContext plus its slice in the emitter's pre-mem2reg form (an
  // alloca + load/store per temp, several times the optimized IR), and every
  // in-flight shard's module is live at once.  Pinning one shard per core --
  // the original scheme -- therefore held the WHOLE program's unoptimized IR
  // in memory simultaneously, which is what exhausts a 32-bit address space on
  // a multi-megabyte input (issue #10).  Sizing shards to a work budget instead
  // keeps every core busy while capping the concurrent set at threads x budget,
  // and costs only a few more link steps: a shard's duplicated declarations
  // and globals are negligible next to its function bodies.
  const uint64_t PerShard =
      std::max<uint64_t>(1, TotalWeight / std::max(1u, NumThreads));
  uint64_t Budget = std::min<uint64_t>(PerShard, limits::kMaxShardOps);
  // A 32-bit host has 2-4 GB of address space for everything, so keep the
  // in-flight set far smaller there than on a 64-bit host.
  if constexpr (sizeof(void *) == 4)
    Budget = std::min<uint64_t>(Budget, limits::kMaxShardOps / 4);
  unsigned NumShards = static_cast<unsigned>(
      std::min<uint64_t>(NumUnits, (TotalWeight + Budget - 1) / Budget));
  NumShards = std::max(NumShards, NumThreads);
  NumShards = std::max(
      1u, std::min<unsigned>(NumShards,
                             static_cast<unsigned>(NumUnits ? NumUnits : 1)));
  Plan.NumShards = NumShards;

  // Longest-processing-time bin packing operates on indivisible work units.
  // Ordinary functions are singleton units. Every C++ EH contribution sharing
  // a nonzero native FuncInfo identity is accumulated before sorting, so no
  // shard boundary can split one logical native function group.
  std::vector<size_t> Order(NumUnits);
  for (size_t I = 0; I < NumUnits; ++I)
    Order[I] = I;
  std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
    return Units[A].Weight != Units[B].Weight
               ? Units[A].Weight > Units[B].Weight
               : Units[A].FirstFunction < Units[B].FirstFunction;
  });

  using Bin = std::pair<uint64_t, unsigned>;
  std::priority_queue<Bin, std::vector<Bin>, std::greater<Bin>> Load;
  for (unsigned S = 0; S < NumShards; ++S)
    Load.emplace(0, S);
  for (size_t UnitIndex : Order) {
    auto [ShardWeight, Shard] = Load.top();
    Load.pop();
    for (size_t FunctionIndex : Units[UnitIndex].Functions)
      Plan.ShardOf[FunctionIndex] = Shard;
    Load.emplace(ShardWeight + Units[UnitIndex].Weight, Shard);
  }
  return Plan;
}

//===----------------------------------------------------------------------===//
// Parallel LLVM emission + optimization
//===----------------------------------------------------------------------===//

Pipeline::LLVMEmissionResult Pipeline::emitLLVMSharded(
    const std::vector<MedFunc> &Funcs, llvm::LLVMContext &Ctx, Arch TheArch,
    const std::vector<std::pair<va_t, std::string>> &Imports,
    const BinaryImage &Img, BinaryFormat Fmt, bool NoOpt, unsigned NumThreads) {
  const size_t N = Funcs.size();
  NumThreads = std::max(
      1u, std::min<unsigned>(NumThreads, static_cast<unsigned>(N ? N : 1)));
  const pipeline_detail::LLVMShardPlan ShardPlan =
      pipeline_detail::planLLVMEmissionShards(Funcs, NumThreads);
  auto EmitShard = [&](unsigned S, llvm::LLVMContext &ShardCtx) {
    std::vector<char> Mask(N, 0);
    for (size_t I = 0; I < N; ++I)
      Mask[I] = (ShardPlan.ShardOf[I] == S);
    MedLLVMEmitter Emitter;
    LLVMEmissionResult Result;
    Result.Module = Emitter.emit(Funcs, ShardCtx, "neverd_output", TheArch,
                                 Imports, &Img, Fmt,
                                 /*MergeableGlobals=*/true, &Mask);
    Result.UnhandledValueIntrinsics = Emitter.unhandledValueIntrinsicCount();
    return Result;
  };
  return runLLVMShardPipeline(Funcs, Ctx, ShardPlan.NumShards, NumThreads,
                              NoOpt, EmitShard);
}

Pipeline::LLVMEmissionResult Pipeline::runLLVMShardPipeline(
    const std::vector<MedFunc> &Funcs, llvm::LLVMContext &Ctx,
    unsigned NumShards, unsigned NumThreads, bool NoOpt,
    llvm::function_ref<LLVMEmissionResult(unsigned, llvm::LLVMContext &)>
        Emit) {
  LLVMEmissionResult Result;
  if (NumShards == 0) {
    Result.Error = "LLVM shard planning failed: no shards";
    return Result;
  }
  NumThreads = std::max(1u, std::min(NumThreads, NumShards));
  struct ShardResult {
    std::string Bitcode;
    uint64_t UnhandledValueIntrinsics = 0;
    bool LLVMVerifierFailed = false;
    std::string Error;
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
        llvm::WithColor::warning()
            << "pipeline: LLVM optimizer warmup failed: "
            << optimizationStopReasonName(Result.Stop) << "\n";
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
          OptimizationOptions Options;
          OptimizationResult Optimization = optimizeModule(*M, Options);
          if (isFatalOptimizationStop(Optimization.Stop)) {
            Shard.LLVMVerifierFailed = true;
            fail(optimizationStopReasonName(Optimization.Stop));
            return;
          }
        } else {
          ShardPhase = "canonicalization";
          promoteScaffoldingAllocas(*M);
        }
        ShardPhase = "serialization";
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
