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

#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

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
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// Parallel LLVM emission + optimization
//===----------------------------------------------------------------------===//

std::unique_ptr<llvm::Module> Pipeline::emitLLVMSharded(
    const std::vector<MedFunc> &Funcs, llvm::LLVMContext &Ctx, Arch TheArch,
    const std::vector<std::pair<va_t, std::string>> &Imports,
    const BinaryImage &Img, BinaryFormat Fmt, bool NoOpt, unsigned NumThreads,
    uint64_t &UnhandledValueIntrinsics) {
  UnhandledValueIntrinsics = 0;
  const size_t N = Funcs.size();
  NumThreads = std::max(
      1u, std::min<unsigned>(NumThreads, static_cast<unsigned>(N ? N : 1)));

  std::vector<uint64_t> Weight(N, 0);
  uint64_t TotalWeight = 0;
  for (size_t I = 0; I < N; ++I) {
    uint64_t W = 1;
    for (const auto &B : Funcs[I].Blocks)
      W += B.Ops.size() + B.Phis.size();
    Weight[I] = W;
    TotalWeight += W;
  }

  // Slice size, not core count, is what bounds peak memory here: a shard holds
  // its own LLVMContext plus its slice in the emitter's pre-mem2reg form (an
  // alloca + load/store per temp, several times the optimized IR), and every
  // in-flight shard's module is live at once.  Pinning one shard per core --
  // the original scheme -- therefore held the WHOLE program's unoptimized IR in
  // memory simultaneously, which is what exhausts a 32-bit address space on a
  // multi-megabyte input (issue #10).  Sizing shards to a work budget instead
  // keeps every core busy while capping the concurrent set at
  // threads x budget, and costs only a few more link steps: a shard's
  // duplicated declarations/globals are negligible next to its function bodies.
  const uint64_t PerShard =
      std::max<uint64_t>(1, TotalWeight / std::max(1u, NumThreads));
  uint64_t Budget = std::min<uint64_t>(PerShard, limits::kMaxShardOps);
  // A 32-bit host has 2-4 GB of address space for everything, so keep the
  // in-flight set far smaller there than on a 64-bit host.
  if constexpr (sizeof(void *) == 4)
    Budget = std::min<uint64_t>(Budget, limits::kMaxShardOps / 4);
  unsigned NumShards = static_cast<unsigned>(
      std::min<uint64_t>(N, (TotalWeight + Budget - 1) / Budget));
  NumShards = std::max(NumShards, NumThreads);
  NumShards = std::max(1u, std::min<unsigned>(NumShards, N ? N : 1));

  // Assign each function to a shard by longest-processing-time bin packing:
  // sort by emitted-work weight (op count is a good proxy for both emit and
  // per-function optimization cost) descending, then greedily place each into
  // the currently least-loaded shard.  A few very large functions otherwise
  // dominate one shard's wall time and cap the speedup; LPT keeps shards even.
  // The assignment is stored as one shard index per function (not a mask per
  // shard) so the bookkeeping stays O(N) however finely the work is sliced.
  std::vector<unsigned> ShardOf(N, 0);
  {
    std::vector<size_t> Order(N);
    for (size_t I = 0; I < N; ++I)
      Order[I] = I;
    std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
      return Weight[A] != Weight[B] ? Weight[A] > Weight[B] : A < B;
    });
    // Least-loaded-first via a min-heap keyed on (load, shard index); scanning
    // every shard per function would be O(N * NumShards) now that shards are
    // sized by work rather than capped at the core count.
    using Bin = std::pair<uint64_t, unsigned>;
    std::priority_queue<Bin, std::vector<Bin>, std::greater<Bin>> Load;
    for (unsigned S = 0; S < NumShards; ++S)
      Load.emplace(0, S);
    for (size_t Idx : Order) {
      auto [L, S] = Load.top();
      Load.pop();
      ShardOf[Idx] = S;
      Load.emplace(L + Weight[Idx], S);
    }
  }

  // Warm up LLVM's lazily-initialized global state (pass registries, managed
  // statics) single-threaded before the parallel region touches it from many
  // threads at once through optimizeModule.  Once per process suffices.
  static std::once_flag WarmupOnce;
  std::call_once(WarmupOnce, [] {
    llvm::LLVMContext WarmCtx;
    llvm::Module Warm("neverd_warmup", WarmCtx);
    optimizeModule(Warm, /*Conservative=*/false);
    promoteScaffoldingAllocas(Warm);
  });

  // Each shard emits its slice + all declarations, verifies, optimizes, and
  // serializes to bitcode in its own context (LLVMContext is not thread-safe,
  // so cross-context transfer goes through bitcode).  Work is claimed
  // atomically, so an uneven shard never idles a worker and only NumThreads
  // shard modules exist at any instant.
  std::vector<std::string> BC(NumShards);
  std::vector<uint64_t> ShardUnhandled(NumShards, 0);
  auto runShard = [&](unsigned S) {
    std::vector<char> Mask(N, 0);
    for (size_t I = 0; I < N; ++I)
      Mask[I] = (ShardOf[I] == S);
    llvm::LLVMContext ShardCtx;
    MedLLVMEmitter Em;
    auto M = Em.emit(Funcs, ShardCtx, "neverd_output", TheArch, Imports, &Img,
                     Fmt, /*MergeableGlobals=*/true, &Mask);
    ShardUnhandled[S] = Em.unhandledValueIntrinsicCount();
    if (!M)
      return;
    std::string VErr;
    llvm::raw_string_ostream VOS(VErr);
    if (!llvm::verifyModule(*M, &VOS)) {
      if (!NoOpt)
        optimizeModule(*M, /*Conservative=*/false);
      else
        promoteScaffoldingAllocas(*M);
    }
    llvm::raw_string_ostream OS(BC[S]);
    llvm::WriteBitcodeToFile(*M, OS);
  };
  if (NumThreads <= 1) {
    for (unsigned S = 0; S < NumShards; ++S)
      runShard(S);
  } else {
    std::atomic<unsigned> Next{0};
    std::vector<std::thread> Pool;
    Pool.reserve(NumThreads);
    for (unsigned T = 0; T < NumThreads; ++T)
      Pool.emplace_back([&] {
        for (unsigned S;
             (S = Next.fetch_add(1, std::memory_order_relaxed)) < NumShards;)
          runShard(S);
      });
    for (auto &T : Pool)
      T.join();
  }

  for (uint64_t Count : ShardUnhandled)
    UnhandledValueIntrinsics += Count;

  bool HadShardFailure = false;
  for (unsigned S = 0; S < NumShards; ++S)
    if (BC[S].empty()) {
      llvm::WithColor::warning()
          << "pipeline: shard " << S << " emission failed\n";
      HadShardFailure = true;
    }

  // Serial link into the caller's context, in shard order for determinism.
  // Each shard's bitcode is released as soon as it is linked: all of them
  // together are a second copy of the program that would otherwise stay
  // resident until the whole link finishes.
  auto Linked = std::make_unique<llvm::Module>("neverd_output", Ctx);
  llvm::Linker L(*Linked);
  unsigned LinkedShards = 0;
  for (unsigned S = 0; S < NumShards; ++S) {
    std::string ShardBC = std::move(BC[S]);
    BC[S].clear();
    BC[S].shrink_to_fit();
    if (ShardBC.empty())
      continue;
    auto Buf = llvm::MemoryBufferRef(ShardBC, "neverd_shard");
    auto MOr = llvm::parseBitcodeFile(Buf, Ctx);
    if (!MOr) {
      llvm::WithColor::warning()
          << "pipeline: shard " << S
          << " bitcode parse failed: " << llvm::toString(MOr.takeError())
          << "\n";
      HadShardFailure = true;
      continue;
    }
    if (L.linkInModule(std::move(*MOr))) {
      llvm::WithColor::warning() << "pipeline: shard " << S << " link failed\n";
      return nullptr;
    } else {
      ++LinkedShards;
    }
  }

  if (HadShardFailure || LinkedShards != NumShards)
    return nullptr;

  // Restore the original (address-order) function layout.  Sharding + link
  // order interleaves definitions by shard, but the serial path emits them in
  // Funcs order and downstream consumers rely on that — notably the recompiled
  // object's .text starts with the entry function, which the round-trip harness
  // executes from offset 0.  Stable-sort the module's function list by original
  // index; declarations not in Funcs (imports/stubs) keep their relative order
  // at the end.  This makes the linked module's layout match the serial path.
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

  return Linked;
}

} // namespace neverd
