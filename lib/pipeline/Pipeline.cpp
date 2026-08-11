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

#include "PipelineReturnModeling.h"

#include "neverd/Limits.h"
#include "neverd/Support/Diagnostic.h"
#include "neverd/Support/Parallel.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/evm/Analyzer.h"
#include "neverd/evm/Bytecode.h"
#include "neverd/evm/LLVMEmitter.h"
#include "neverd/sbf/Analyzer.h"
#include "neverd/sbf/LLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/libc/LibCNames.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

//===----------------------------------------------------------------------===//
// buildLowIR helpers
//===----------------------------------------------------------------------===//

namespace {

bool hasRealOps(const LowFunc &Func) {
  for (auto &Blk : Func.Blocks)
    for (auto &Op : Blk.Ops)
      if (Op.Opcode != NdOp::NOP)
        return true;
  return false;
}

// Both IRs are built by repeated push_back and then kept alive for the rest of
// the run, so every block carries whatever slack its last geometric growth left
// behind -- across tens of thousands of blocks that slack is a sizeable
// fraction of the pipeline's resident set.  Trimming each function once, on the
// worker that just finished building it, costs one copy of data already in
// cache and is invisible next to decode/SSA.
template <typename Func> void trimFuncStorage(Func &F) {
  for (auto &B : F.Blocks) {
    B.Ops.shrink_to_fit();
    B.Succs.shrink_to_fit();
    B.Preds.shrink_to_fit();
    if constexpr (requires { B.Phis; })
      B.Phis.shrink_to_fit();
  }
  F.Blocks.shrink_to_fit();
}

void annotateDebugInfo(LowFunc &Func, DebugContext &Dbg) {
  auto FSym = Dbg.resolveFunction(Func.Entry);
  if (!FSym)
    return;
  Func.DebugName = FSym->Name;
  Func.SourceFile = FSym->DeclLoc.File;
  Func.SourceLine = FSym->DeclLoc.Line;
  if (FSym->Size > 0)
    Func.OriginalSize = FSym->Size;
}

// Variadic overflow parameters are finalized only after call recovery.  Keep
// their interim arity open so neither recovery pass truncates the caller's
// recovered stack tail to the currently known fixed prefix.
int callRecoveryTotalArity(const MedFunc &Func, int MaxParamIndex) {
  return Func.IsVariadic ? limits::kMaxCallArgs : MaxParamIndex + 1;
}

bool recordMedIRVerification(PipelineResult &Result, const char *PassName) {
  std::map<va_t, PipelineFunctionAudit *> AuditByEntry;
  for (auto &Audit : Result.FunctionAudits)
    AuditByEntry[Audit.Entry] = &Audit;

  Result.MedIRVerifierFailures = 0;
  for (size_t I = 0; I < Result.LowFuncs.size(); ++I) {
    const LowFunc &LF = Result.LowFuncs[I];
    const bool HasMed = I < Result.MedFuncs.size() &&
                        Result.MedFuncs[I].Entry == LF.Entry &&
                        !Result.MedFuncs[I].Blocks.empty();
    const bool Verified = HasMed && verifyMedFunc(Result.MedFuncs[I], PassName);
    if (!Verified)
      ++Result.MedIRVerifierFailures;

    auto It = AuditByEntry.find(LF.Entry);
    if (It == AuditByEntry.end())
      continue;
    PipelineFunctionAudit &Audit = *It->second;
    Audit.HasMedIR = HasMed;
    Audit.MedIRVerified = Verified;
    if (!Verified)
      Audit.Disposition = PipelineFunctionDisposition::MedIRFailed;
  }
  return Result.MedIRVerifierFailures == 0;
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// buildLowIR — Phase 1
//===----------------------------------------------------------------------===//

void Pipeline::buildLowIR(
    const BinaryImage &Img,
    const std::vector<std::pair<va_t, std::string>> &Candidates,
    const PipelineOptions &Opts, DebugContext *Dbg, PipelineResult &Result) {
  const size_t Total = Candidates.size();
  std::vector<LowFunc> AllLow(Total);

  // The set of all detected function entries lets each CFG builder recognise an
  // unconditional `jmp` to *another* function as a tail call (call + ret)
  // rather than following it and fusing the callee into this function's CFG.
  std::set<va_t> FuncEntries;
  for (const auto &C : Candidates)
    FuncEntries.insert(C.first);

  // Decode cost tracks a function's instruction count, which is unknown before
  // the recursive-descent build runs.  Candidates are address-sorted, so the
  // byte gap to the next entry is a cheap upper-bound proxy for a function's
  // size; scheduling the largest gaps first keeps one giant function from
  // being claimed last and serializing the tail.  The gap is clamped so an
  // outsized cross-segment gap (data between the last function and the segment
  // end) does not distort the ordering — it is only a scheduling hint and never
  // affects the decoded result.
  constexpr uint64_t kMaxDecodeWeight = 1ull << 20;
  std::vector<uint64_t> Weight(Total, kMaxDecodeWeight);
  for (size_t I = 0; I + 1 < Total; ++I) {
    uint64_t Gap = Candidates[I + 1].first - Candidates[I].first;
    Weight[I] = std::min(Gap, kMaxDecodeWeight);
  }

  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    Decoder LocalDec;
    if (!LocalDec.init(Img.Arch, Img.Mode))
      return;
    CFGBuilder LocalCFG;
    LocalCFG.setKnownFuncEntries(&FuncEntries);
    for (size_t I; (I = Claim()) < N;) {
      AllLow[I] = LocalCFG.build(Img, LocalDec, Candidates[I].first,
                                 Candidates[I].second);
      trimFuncStorage(AllLow[I]);
    }
  });

  // Merge each function's relocation-free PC-relative code references (x86
  // same-section `lea rip` function pointers) into the image so the emitter
  // symbolizes them.  Done single-threaded after the parallel build to avoid a
  // data race on the shared set.
  for (const auto &LF : AllLow)
    for (va_t Ref : LF.CodeRefTargets)
      Img.CodeRefTargets.insert(Ref);

  size_t FuncCount = 0;
  for (size_t I = 0; I < Total; ++I) {
    auto &Low = AllLow[I];
    auto AuditIt =
        std::find_if(Result.FunctionAudits.begin(), Result.FunctionAudits.end(),
                     [&](const PipelineFunctionAudit &Audit) {
                       return Audit.Entry == Candidates[I].first;
                     });
    if (AuditIt != Result.FunctionAudits.end()) {
      AuditIt->DecodedInstructions = Low.DecodedInstructionCount;
      AuditIt->LiftedInstructions = Low.LiftedInstructionCount;
      AuditIt->DecodeFailures = Low.DecodeFailureAddresses;
      AuditIt->UnsupportedInstructions = Low.UnsupportedInstructionAddresses;
      AuditIt->TruncatedPaths = Low.TruncatedPathAddresses;
    }

    if (Opts.MaxFunctions > 0 && FuncCount >= Opts.MaxFunctions) {
      if (AuditIt != Result.FunctionAudits.end())
        AuditIt->Disposition = PipelineFunctionDisposition::SkippedLimit;
      continue;
    }
    if (!hasRealOps(Low)) {
      if (AuditIt != Result.FunctionAudits.end())
        AuditIt->Disposition =
            Low.hasCompleteInstructionLift()
                ? PipelineFunctionDisposition::RejectedLowIR
                : PipelineFunctionDisposition::RejectedIncomplete;
      continue;
    }
    // Only an incomplete *lift* disqualifies a function.  A path that left the
    // mapped image is recorded in the audit but is not a defect in what was
    // recovered, and rejecting it would drop a function whose every
    // instruction lifted cleanly.
    if (!Low.hasCompleteInstructionLift()) {
      if (AuditIt != Result.FunctionAudits.end())
        AuditIt->Disposition = PipelineFunctionDisposition::RejectedIncomplete;
      continue;
    }

    if (Dbg && Dbg->hasInfo())
      annotateDebugInfo(Low, *Dbg);
    if (Low.OriginalSize == 0)
      Low.OriginalSize = Low.computedSize();

    Result.LowFuncs.push_back(std::move(Low));
    if (AuditIt != Result.FunctionAudits.end())
      AuditIt->HasLowIR = true;
    ++FuncCount;
  }
}

//===----------------------------------------------------------------------===//
// buildMedIR — Phase 2
//===----------------------------------------------------------------------===//

void Pipeline::buildMedIR(const BinaryImage &Img,
                          const PipelineOptions & /*Opts*/,
                          PipelineResult &Result) {
  auto Phase2Start = std::chrono::steady_clock::now();

  const size_t Total = Result.LowFuncs.size();
  Result.MedFuncs.resize(Total);

  // Per-callee callee-cleanup pop (x86 `ret imm`, the i386 SysV sret hidden-
  // pointer pop) so each caller's CALL to such a callee gets a post-call stack-
  // pointer correction during low->med translation.  Built once (read-only) and
  // shared across the parallel converters.
  std::map<va_t, int> CalleePop;
  for (const auto &LF : Result.LowFuncs)
    if (LF.CalleePopBytes > 0)
      CalleePop[LF.Entry] = LF.CalleePopBytes;

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

  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    LowToMedConverter Local;
    Local.setCalleePopMap(&CalleePop);
    Local.setStackProbeSlots(&StackProbeSlots);
    for (size_t I; (I = Claim()) < N;) {
      try {
        Result.MedFuncs[I] =
            Local.convert(Result.LowFuncs[I], Img.Arch, Img.Format);
        auto &MF = Result.MedFuncs[I];
        auto &LF = Result.LowFuncs[I];
        trimFuncStorage(MF);
        MF.OriginalSize = LF.OriginalSize;
        MF.DebugName = LF.DebugName;
        MF.SourceFile = LF.SourceFile;
        MF.SourceLine = LF.SourceLine;
      } catch (...) {
        syncWarning() << "pipeline: low->med threw on "
                      << Result.LowFuncs[I].Name << "\n";
        Result.MedFuncs[I] = MedFunc{};
      }
    }
  });

  recordMedIRVerification(Result, "pipeline-med-final");

  [[maybe_unused]] auto Elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - Phase2Start)
          .count();
  LLVM_DEBUG(llvm::dbgs() << "pipeline: LowIR -> MedIR took " << Elapsed
                          << "ms (" << Result.MedFuncs.size()
                          << " functions)\n");
}

/// Count FP arguments a callee takes from its entry-block live-in self-copies
/// (`COPY D0,D0; COPY D1,D1; ...` before any real body).  Used to prime
/// CalleeFPArity for intra-module callees (e.g. `mkD2`) before recoverCallAbi
/// runs, so a tail-call struct-return forwarder (`fwdD2`) can recover its
/// forwarded d0/d1 live-ins (KnownFPCallee gate in recoverCallAbi).
static int countEntryLiveInFPArgs(const MedFunc &MF, const TargetRegInfo &TRI) {
  if (MF.Blocks.empty())
    return 0;
  int Count = 0;
  for (const auto &O : MF.Blocks.front().Ops) {
    if (O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
        O.Inputs[0].Kind == MedVar::Reg &&
        O.Inputs[0].RegOff == O.Output.RegOff &&
        TRI.isFPArgReg(O.Output.RegOff))
      ++Count;
    else if (O.Opcode == NdOp::COPY && TRI.isLinkRegister(O.Output.RegOff))
      continue;
    else
      break;
  }
  return Count;
}

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

  // Serial link into the caller's context, in shard order for determinism.
  // Each shard's bitcode is released as soon as it is linked: all of them
  // together are a second copy of the program that would otherwise stay
  // resident until the whole link finishes.
  auto Linked = std::make_unique<llvm::Module>("neverd_output", Ctx);
  llvm::Linker L(*Linked);
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
      continue;
    }
    if (L.linkInModule(std::move(*MOr)))
      llvm::WithColor::warning() << "pipeline: shard " << S << " link failed\n";
  }

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

//===----------------------------------------------------------------------===//
// runPatchLiftMode — MedIR -> LLVM IR shortcut
//===----------------------------------------------------------------------===//

bool Pipeline::runPatchLiftMode(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                                const PipelineOptions &Opts,
                                PipelineResult &Result) {
  [[maybe_unused]] const char *ModeName = Opts.PatchMode ? "patch" : "lift";
  LLVM_DEBUG(llvm::dbgs() << "pipeline: " << ModeName
                          << " mode -- MedIR -> LLVM IR, skipping HighIR\n");

  auto AllFuncNames = buildFuncNameMap(Img, Result);

  // Each callee's integer register-argument count and total integer-argument
  // count (register + stack), so a forwarder's call ABI is bounded by the arity
  // its target actually takes — the register count gates passed-through
  // register arguments, the total count gates passed-through stack arguments of
  // a tail call (see recoverCallAbi). Infer types for every function first so
  // each caller's call-ABI recovery can consult its callees' return class
  // (FP-in-vector-register vs integer) and FP-argument count — both derived
  // from the callee's inferred signature.
  for (auto &MF : Result.MedFuncs)
    inferMedTypes(MF, Img.Arch);

  modelWideIntReturns(Img, Result);

  recoverStructReturnFromCallers(Img, Result);

  propagateStructReturnForwarderShapes(Img, Result);

  recoverStructReturnFromBody(Img, Result);

  materializeKnownStructReturnCallSites(Img, Result);

  std::map<va_t, int> CalleeRegArity;
  std::map<va_t, int> CalleeTotalArity;
  std::map<va_t, int> CalleeFPArity;
  std::map<va_t, std::vector<uint64_t>> CalleeFPRegs;
  std::map<va_t, bool> CalleeReturnsVec;
  std::map<va_t, bool> CalleeHasSret;
  std::map<va_t, bool> CalleeIsVariadic;
  {
    const auto &TRI = getTargetRegInfo(Img.Arch);
    // On x86-64 a floating-point return lands in XMM0 (a vector register); on
    // ARM/AArch64 the lifter models it in the integer return register, so only
    // the former needs the call result routed to the FP return register.
    const bool FPRetInVecReg = TRI.isVectorReg(TRI.fpReturnModelReg());
    for (const auto &MF : Result.MedFuncs) {
      int MaxRegIdx = -1, MaxIdx = -1;
      // The exact FP-argument register offsets, in ABI order.  ARM `float` args
      // land in the single-width S registers (s0,s1,..) and `double` args in
      // the D registers (d0,d1,..); recording the layout lets the caller
      // recover FP arguments at the registers the callee actually reads (s1 !=
      // d1).
      std::vector<uint64_t> FPRegs;
      const uint64_t IRR = TRI.indirectResultReg();
      bool HasSret = false;
      for (const auto &P : MF.Params) {
        if (IRR != 0 && P.RegOff == IRR) {
          // Hidden indirect-result (sret) pointer (AArch64 x8): not an ordinary
          // integer/FP/stack argument; recorded separately.
          HasSret = true;
        } else if (P.RegOff != kNoParamReg && TRI.isFPArgReg(P.RegOff)) {
          // Floating-point/vector argument register: counted separately.
          FPRegs.push_back(P.RegOff);
        } else if (P.RegOff != kNoParamReg) {
          if (int Idx = TRI.regToArgIdx(P.RegOff); Idx > MaxRegIdx)
            MaxRegIdx = Idx;
          MaxIdx = std::max(MaxIdx, TRI.regToArgIdx(P.RegOff));
        } else if (P.Kind == MedVar::Param) {
          // Stack parameter (detectStackParams / detectCdeclStackParams): its
          // Id is the argument index.
          MaxIdx = std::max(MaxIdx, P.Id);
        }
      }
      std::sort(FPRegs.begin(), FPRegs.end());
      CalleeRegArity[MF.Entry] = MaxRegIdx + 1;
      CalleeTotalArity[MF.Entry] = callRecoveryTotalArity(MF, MaxIdx);
      CalleeHasSret[MF.Entry] = HasSret;
      CalleeIsVariadic[MF.Entry] = MF.IsVariadic;
      int FpArity = static_cast<int>(FPRegs.size());
      // Params are not recovered yet (recoverCallAbi runs later), so fall back
      // to entry live-in self-copies for intra-module FP callees like `mkD2`.
      // Without this, tail-call struct-return forwarders (`fwdD2`) miss the
      // KnownFPCallee gate and pass 0.0 for forwarded d0/d1.
      if (FpArity == 0) {
        FpArity = countEntryLiveInFPArgs(MF, TRI);
        if (FpArity > 0)
          FPRegs.assign(TRI.FPParamRegs.begin(),
                        TRI.FPParamRegs.begin() + FpArity);
      }
      CalleeFPArity[MF.Entry] = FpArity;
      CalleeFPRegs[MF.Entry] = std::move(FPRegs);
      CalleeReturnsVec[MF.Entry] = FPRetInVecReg && MF.ReturnType &&
                                   MF.ReturnType->Kind == NdTypeKind::Float;
    }
  }

  // A pure tail-call forwarder `T f(args){return g(args);}` lowers at -O2 to a
  // lone `b g`.  It carries its incoming arguments straight into the call, so
  // it has no parameters of its own here and the per-function FP arity above is
  // 0. Inherit the callee g's scalar-FP return type and FP argument arity for
  // such a forwarder so (a) the function's return type is floating-point
  // (recoverCallAbi rewires the tail call's result register to the FP return
  // register) and (b) a CALLER of it recovers the forwarded FP arguments
  // instead of padding 0.0.  The callee g is a libc import (signature from
  // libcArity) or an intra-module function (signature from its recovered
  // MedFunc).  Gated to a genuine forwarder whose FP argument registers are
  // live-in (never written), so a function that locally computes the callee's
  // FP arguments is left untouched.
  {
    const auto &TRI = getTargetRegInfo(Img.Arch);
    std::map<va_t, const MedFunc *> ByEntry;
    for (const auto &MF : Result.MedFuncs)
      ByEntry[MF.Entry] = &MF;
    if (!TRI.FPParamRegs.empty())
      for (auto &MF : Result.MedFuncs) {
        // Only a pure forwarder, which has no parameters of its own recovered
        // yet (its incoming arguments flow straight into the tail call).
        if (!MF.Params.empty())
          continue;
        const MedOp *CallOp = nullptr;
        for (const auto &Blk : MF.Blocks) {
          for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
            const auto &Op = Blk.Ops[I];
            if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
                !Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Reg)
              continue;
            const auto &Ret = Blk.Ops[I + 1];
            if (Ret.Opcode == NdOp::RETURN && Ret.NumInputs >= 1 &&
                Ret.Inputs[0].Kind == MedVar::Reg &&
                Ret.Inputs[0].RegOff == Op.Output.RegOff) {
              CallOp = &Op;
              break;
            }
          }
          if (CallOp)
            break;
        }
        if (!CallOp)
          continue;
        va_t Target = CallOp->Inputs[0].ConstVal;
        // Resolve the callee's integer + FP argument counts and scalar-FP
        // return width (libc import via libcArity, intra-module via its
        // recovered MedFunc).  All three must reach the forwarder's signature,
        // or a caller misassembles the forwarded arguments (a mixed `double
        // f(int n,double x) {return g(n,x);}` with only the FP arity propagated
        // would disagree with its own declared int+FP signature).
        int IntArgs = 0, FpArgs = 0;
        uint16_t FpRetSize = 0; // 0 = callee does not return a scalar FP value
        if (const Import *Imp = Img.findImportAt(Target)) {
          if (auto Sig =
                  libc::libcArity(llvm::StringRef(Imp->Name).ltrim('_'))) {
            if (!Sig->FpRetComplex) {
              IntArgs = Sig->IntArgs;
              FpArgs = Sig->FpArgs;
              bool ScalarFPRet = (Sig->FpArgs > 0 && Sig->IntArgs == 0) ||
                                 Sig->FpRet || Sig->FpRetLongDouble;
              if (ScalarFPRet)
                FpRetSize = Sig->FpIsFloat ? 4 : 8;
            }
          }
        } else if (auto It = ByEntry.find(Target); It != ByEntry.end()) {
          const MedFunc *G = It->second;
          IntArgs = CalleeRegArity[Target]; // g's integer-argument count
          FpArgs = CalleeFPArity[Target];   // g's FP-argument count
          if (FpArgs == 0) {
            FpArgs = countEntryLiveInFPArgs(*G, TRI);
            if (FpArgs > 0) {
              CalleeFPArity[Target] = FpArgs;
              CalleeFPRegs[Target] = std::vector<uint64_t>(
                  TRI.FPParamRegs.begin(), TRI.FPParamRegs.begin() + FpArgs);
            }
          }
          if (G->ReturnType && G->ReturnType->Kind == NdTypeKind::Float &&
              G->MultiReturn.empty())
            FpRetSize = G->ReturnType->Size ? G->ReturnType->Size : 8;
        }
        if (IntArgs <= 0 && FpArgs <= 0 && FpRetSize == 0)
          continue; // nothing to inherit
        int NFp =
            std::min<int>(FpArgs, static_cast<int>(TRI.FPParamRegs.size()));
        int NInt =
            std::min<int>(IntArgs, static_cast<int>(TRI.IntParamRegs.size()));
        // Every forwarded argument register must be live-in (genuine forwarder,
        // not a function that computes the callee's arguments locally).  The
        // forwarding CALL itself is excluded: its output is the integer return
        // register, which on AArch64/x86-64 aliases the first integer argument
        // register (x0 / rax-vs-rdi differ, but x0==arg0==ret on AArch64), so
        // the call's result write must not be mistaken for an argument write.
        auto regWrittenInF = [&](uint64_t Reg) {
          for (const auto &B : MF.Blocks) {
            for (const auto &O : B.Ops)
              if (&O != CallOp && O.Output.Kind == MedVar::Reg &&
                  O.Output.Size > 0 && O.Output.RegOff == Reg)
                return true;
            for (const auto &Ph : B.Phis)
              if (Ph.Output.Kind == MedVar::Reg && Ph.Output.RegOff == Reg)
                return true;
          }
          return false;
        };
        bool AllLiveIn = true;
        for (int K = 0; K < NFp && AllLiveIn; ++K)
          if (regWrittenInF(TRI.FPParamRegs[K]))
            AllLiveIn = false;
        for (int K = 0; K < NInt && AllLiveIn; ++K)
          if (regWrittenInF(TRI.IntParamRegs[K]))
            AllLiveIn = false;
        if (!AllLiveIn)
          continue;
        if (NFp > 0) {
          CalleeFPArity[MF.Entry] = NFp;
          CalleeFPRegs[MF.Entry] = std::vector<uint64_t>(
              TRI.FPParamRegs.begin(), TRI.FPParamRegs.begin() + NFp);
        }
        if (NInt > 0) {
          CalleeRegArity[MF.Entry] = NInt;
          if (CalleeTotalArity[MF.Entry] < NInt)
            CalleeTotalArity[MF.Entry] = NInt;
        }
        // Inherit a scalar FP return so the function and its callers treat the
        // result as floating-point; recoverCallAbi rewires the tail call's
        // result register to the FP return register.
        if (FpRetSize) {
          MF.ReturnType = NdType::makeFloat(FpRetSize);
          CalleeReturnsVec[MF.Entry] = TRI.isVectorReg(TRI.fpReturnModelReg());
        }
      }
  }

  for (auto &MF : Result.MedFuncs)
    recoverCallAbi(MF, Img.Arch, AllFuncNames, &Img, &CalleeRegArity,
                   &CalleeTotalArity, &CalleeFPArity, &CalleeReturnsVec,
                   &CalleeFPRegs, &CalleeHasSret, &CalleeIsVariadic);

  remodelStructReturnForwarderCalls(Img, Result);

  // i386 two-pass call recovery: the first pass promotes forwarder register
  // params (PromoteParams).  Recompute CalleeRegArity from the now-promoted
  // params and re-run: forwarders now have CalleeRegArgs > 0, so the cdecl
  // clearing (CalleeRegArgs == 0) no longer fires for them, while true cdecl
  // callees remain at 0 and get their stack arguments correctly indexed.
  if (Img.Arch == Arch::X86) {
    const auto &TRI2 = getTargetRegInfo(Img.Arch);
    std::map<va_t, int> CRA2, CTA2;
    for (const auto &MF : Result.MedFuncs) {
      int MaxRI = -1, MaxI = -1;
      for (const auto &P : MF.Params) {
        if (P.RegOff != kNoParamReg && TRI2.regToArgIdx(P.RegOff) >= 0) {
          MaxRI = std::max(MaxRI, TRI2.regToArgIdx(P.RegOff));
          MaxI = std::max(MaxI, TRI2.regToArgIdx(P.RegOff));
        } else if (P.Kind == MedVar::Param)
          MaxI = std::max(MaxI, P.Id);
      }
      CRA2[MF.Entry] = MaxRI + 1;
      CTA2[MF.Entry] = callRecoveryTotalArity(MF, MaxI);
    }
    for (auto &MF : Result.MedFuncs)
      recoverCallAbi(MF, Img.Arch, AllFuncNames, &Img, &CRA2, &CTA2,
                     &CalleeFPArity, &CalleeReturnsVec, &CalleeFPRegs,
                     &CalleeHasSret, &CalleeIsVariadic);
  }

  // Variadic callees: size each one's overflow stack-parameter list from its
  // now recovered call sites, append the trailing stack parameters, and pad
  // every call to that arity.  The emitter spills these into the frame headroom
  // so the unchanged va_arg walk reads the caller's overflow arguments.
  finalizeVariadicCallees(Result.MedFuncs, Img.Arch, Img.Format);

  if (!recordMedIRVerification(Result, "pipeline-backend-input")) {
    Result.Error = "MedIR verification failed before backend emission";
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
  bool UseShards = !Opts.PatchMode && Result.MedFuncs.size() >= 8;

  if (UseShards) {
    Result.LlvmModule = emitLLVMSharded(
        Result.MedFuncs, Ctx, Img.Arch, ImportMap, Img, Img.Format, Opts.NoOpt,
        Workers, Result.BackendUnhandledValueIntrinsics);
    if (!Result.LlvmModule)
      return false;
  } else {
    MedLLVMEmitter MedEmitter;
    Result.LlvmModule = MedEmitter.emit(Result.MedFuncs, Ctx, "neverd_output",
                                        Img.Arch, ImportMap, &Img, Img.Format);
    Result.BackendUnhandledValueIntrinsics =
        MedEmitter.unhandledValueIntrinsicCount();

    if (!Result.LlvmModule)
      return false;

    std::string VerifyErr;
    llvm::raw_string_ostream VES(VerifyErr);
    if (!llvm::verifyModule(*Result.LlvmModule, &VES)) {
      if (!Opts.NoOpt)
        optimizeModule(*Result.LlvmModule, Opts.PatchMode);
      else
        // Even with the NeverD optimizer disabled, promote the emitter's
        // memory-SSA scaffolding to registers.  This is semantics-preserving
        // canonicalization (not optimization): it strips the per-temp
        // load/store bloat so a heavily-unrolled -O2 SSE kernel does not lift
        // to an ~80K-instruction single block that is pathological for LLVM
        // codegen and times out under parallel test load.
        promoteScaffoldingAllocas(*Result.LlvmModule);
    } else {
      llvm::WithColor::warning()
          << "skipping optimization: " << VerifyErr << "\n";
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

  std::string FinalVerifyError;
  llvm::raw_string_ostream FinalVerifyStream(FinalVerifyError);
  Result.LLVMVerifierFailed =
      llvm::verifyModule(*Result.LlvmModule, &FinalVerifyStream);
  if (Result.LLVMVerifierFailed) {
    llvm::WithColor::warning()
        << "pipeline: final LLVM verification failed: " << FinalVerifyError
        << "\n";
    return false;
  }

  return true;
}

//===----------------------------------------------------------------------===//
// buildHighIR — Phase 3
//===----------------------------------------------------------------------===//

void Pipeline::buildHighIR(const BinaryImage &Img,
                           const PipelineOptions & /*Opts*/,
                           PipelineResult &Result) {
  auto AllFuncNames = buildFuncNameMap(Img, Result);

  detectThunkStubs(Result.LowFuncs, AllFuncNames);

  const size_t Total = Result.MedFuncs.size();
  Result.HighFuncs.resize(Total);

  // Weight each function by its MedIR op count so the heaviest structurings
  // start first and the tail stays balanced (see parallelForEachWeighted).
  std::vector<uint64_t> Weight(Total, 1);
  for (size_t I = 0; I < Total; ++I) {
    uint64_t W = 1;
    for (const auto &B : Result.MedFuncs[I].Blocks)
      W += B.Ops.size() + B.Phis.size();
    Weight[I] = W;
  }

  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    MedToHighConverter Local;
    Local.setFuncNames(&AllFuncNames);
    for (size_t FI; (FI = Claim()) < N;) {
      if (FI < Result.LowFuncs.size())
        Local.setJumpTables(Result.LowFuncs[FI].JumpTables);
      else
        Local.setJumpTables({});
      try {
        Result.HighFuncs[FI] = Local.convert(Result.MedFuncs[FI], Img.Arch);
        auto &HF = Result.HighFuncs[FI];
        auto &MF = Result.MedFuncs[FI];
        HF.OriginalSize = MF.OriginalSize;
        HF.DebugName = MF.DebugName;
        HF.SourceFile = MF.SourceFile;
        HF.SourceLine = MF.SourceLine;
      } catch (...) {
        syncWarning() << "pipeline: med->high threw on "
                      << Result.MedFuncs[FI].Name << "\n";
        Result.HighFuncs[FI] = HighFunc{};
      }
    }
  });
}

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
    auto Normalized =
        evm::normalizeBytecode(Img.Raw, Source, SourceIsRuntime,
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
