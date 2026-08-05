//===- NeverDCAPIBench.cpp - C API: benchmark support ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Structured pipeline benchmark: runs all IR stages and returns JSON
/// timing data.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/Support/Parallel.h"
#include "neverd/lift/AArch64Lifter.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

const char *neverd_bench_run(neverd_session_t Sess, const char *InputPath,
                             int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  using Clock = std::chrono::steady_clock;
  using MsT = std::chrono::milliseconds;
  auto T0Total = Clock::now();

  PipelineOptions LowOpts;
  LowOpts.DumpLow = true;
  LowOpts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;

  auto T0 = Clock::now();
  Pipeline LowPipe;
  R.Result = LowPipe.run(R.Img, R.LLVMCtx, LowOpts);
  long LowMs = std::chrono::duration_cast<MsT>(Clock::now() - T0).count();

  int FuncCount = static_cast<int>(R.Result.LowFuncs.size());
  int LowBlocks = 0, LowOps = 0;
  for (auto &LF : R.Result.LowFuncs) {
    LowBlocks += static_cast<int>(LF.Blocks.size());
    for (auto &B : LF.Blocks)
      LowOps += static_cast<int>(B.Ops.size());
  }

  PipelineOptions MedOpts;
  MedOpts.DumpMed = true;
  MedOpts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;

  T0 = Clock::now();
  Pipeline MedPipe;
  auto MedResult = MedPipe.run(R.Img, R.LLVMCtx, MedOpts);
  long MedMs = std::chrono::duration_cast<MsT>(Clock::now() - T0).count();

  int MedOpsTotal = 0;
  for (auto &MF : MedResult.MedFuncs)
    for (auto &B : MF.Blocks)
      MedOpsTotal += static_cast<int>(B.Ops.size());

  PipelineOptions HighOpts;
  HighOpts.DumpHigh = true;
  HighOpts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;

  T0 = Clock::now();
  Pipeline HighPipe;
  auto HighResult = HighPipe.run(R.Img, R.LLVMCtx, HighOpts);
  long HighMs = std::chrono::duration_cast<MsT>(Clock::now() - T0).count();

  int HighStmts = 0;
  for (auto &HF : HighResult.HighFuncs)
    HighStmts += static_cast<int>(HF.Body.size());

  PipelineOptions LiftOpts;
  LiftOpts.LiftMode = true;
  LiftOpts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;

  llvm::LLVMContext LlvmCtx;
  T0 = Clock::now();
  Pipeline LlvmPipe;
  auto LlvmResult = LlvmPipe.run(R.Img, LlvmCtx, LiftOpts);
  long LlvmMs = std::chrono::duration_cast<MsT>(Clock::now() - T0).count();

  int LlvmFuncs = 0;
  if (LlvmResult.LlvmModule)
    for (auto &F : *LlvmResult.LlvmModule)
      if (!F.isDeclaration())
        ++LlvmFuncs;

  long TotalMs =
      std::chrono::duration_cast<MsT>(Clock::now() - T0Total).count();

  llvm::json::Object Root;
  Root["func_count"] = FuncCount;
  Root["import_count"] = static_cast<int64_t>(R.Img.Imports.size());

  int64_t StringCount = 0;
  for (auto &Seg : R.Img.Segments) {
    if (Seg.isExecutable())
      continue;
    const uint8_t *Data = Seg.Data.data();
    size_t Len = Seg.Data.size();
    size_t RunLen = 0;
    for (size_t I = 0; I <= Len; ++I) {
      uint8_t B = (I < Len) ? Data[I] : 0;
      bool IsPrintable = (B >= 0x20 && B < 0x7F) || B == '\t' || B == '\n';
      if (IsPrintable) {
        ++RunLen;
      } else {
        if (RunLen >= 4 && B == 0)
          ++StringCount;
        RunLen = 0;
      }
    }
  }
  Root["string_count"] = StringCount;
  Root["low_time_ms"] = LowMs;
  Root["med_time_ms"] = MedMs;
  Root["high_time_ms"] = HighMs;
  Root["llvm_time_ms"] = LlvmMs;
  Root["total_time_ms"] = TotalMs;
  Root["low_blocks"] = LowBlocks;
  Root["low_ops"] = LowOps;
  Root["med_ops"] = MedOpsTotal;
  Root["high_stmts"] = HighStmts;
  Root["llvm_funcs"] = LlvmFuncs;

  llvm::json::Array Funcs;
  for (auto &LF : R.Result.LowFuncs) {
    llvm::json::Object FObj;
    FObj["name"] = LF.Name;
    FObj["entry"] = vaHex(LF.Entry);
    FObj["blocks"] = static_cast<int64_t>(LF.Blocks.size());
    int Ops = 0;
    for (auto &B : LF.Blocks)
      Ops += static_cast<int>(B.Ops.size());
    FObj["ops"] = Ops;
    Funcs.push_back(std::move(FObj));
  }
  Root["functions"] = std::move(Funcs);

  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}

// ===--------------------------------------------------------------------===//
// Raw decode-throughput benchmark
// ===--------------------------------------------------------------------===//

namespace {

const char *archShortName(Arch A) {
  switch (A) {
  case Arch::X64:
    return "x86-64";
  case Arch::X86:
    return "i386";
  case Arch::AArch64:
    return "aarch64";
  case Arch::ARM:
    return "arm";
  default:
    return "unknown";
  }
}

} // namespace

const char *neverd_bench_decode(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }

  struct Span {
    const uint8_t *Data;
    size_t Len;
    va_t VA;
  };

  // Cap the bytes fed to the timed passes so the benchmark stays fast on very
  // large images (e.g. a statically-linked shared library with tens of MB of
  // __text) while still decoding a representative slice.  A full linear decode
  // is bounded work, but at single-threaded capstone throughput 80 MB of code
  // is tens of seconds per pass — far more than a throughput probe needs.
  constexpr uint64_t kByteBudget = 16ull << 20; // 16 MiB

  std::vector<Span> Execs;
  uint64_t ExecBytes = 0;   // total executable bytes in the image
  uint64_t BenchBytes = 0;  // bytes actually fed to the timed passes
  for (const auto &Seg : S->Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;
    ExecBytes += Seg.Data.size();
    if (BenchBytes >= kByteBudget)
      continue;
    size_t Take = Seg.Data.size();
    if (BenchBytes + Take > kByteBudget)
      Take = static_cast<size_t>(kByteBudget - BenchBytes);
    Execs.push_back({Seg.Data.data(), Take, Seg.VA});
    BenchBytes += Take;
  }
  if (Execs.empty()) {
    S->setError("no executable bytes to decode");
    return nullptr;
  }

  using Clock = std::chrono::steady_clock;

  // Run \p Pass repeatedly and keep the best (minimum) wall time, so a noisy
  // scheduler tick does not distort the throughput of a small image.  Stops at
  // 100 repeats or once 200ms of total measurement has elapsed.
  auto timePass = [&](auto Pass) -> std::pair<uint64_t, double> {
    uint64_t Insns = 0;
    double BestNs = 0.0;
    int Reps = 0;
    auto Overall = Clock::now();
    do {
      auto T0 = Clock::now();
      Insns = Pass();
      double Ns =
          std::chrono::duration<double, std::nano>(Clock::now() - T0).count();
      if (BestNs == 0.0 || Ns < BestNs)
        BestNs = Ns;
      ++Reps;
    } while (Reps < 100 && std::chrono::duration<double, std::milli>(
                               Clock::now() - Overall)
                                   .count() < 200.0);
    return {Insns, BestNs};
  };

  Decoder DFull;
  if (!DFull.init(S->Img.Arch, S->Img.Mode)) {
    S->setError("decoder init failed");
    return nullptr;
  }
  DFull.setDetail(true);
  auto fullPass = [&]() -> uint64_t {
    uint64_t N = 0;
    for (const auto &Sp : Execs) {
      size_t Off = 0;
      va_t Cur = Sp.VA;
      while (Off < Sp.Len) {
        DecodedInsn DI;
        int Sz = DFull.decodeOne(Sp.Data + Off, Sp.Len - Off, Cur, DI);
        if (Sz <= 0) {
          Off += 1;
          Cur += 1;
          continue;
        }
        Off += static_cast<size_t>(Sz);
        Cur += static_cast<va_t>(Sz);
        ++N;
      }
    }
    return N;
  };

  Decoder DLight;
  if (!DLight.init(S->Img.Arch, S->Img.Mode)) {
    S->setError("decoder init failed");
    return nullptr;
  }
  DLight.setDetail(false);
  auto lightPass = [&]() -> uint64_t {
    uint64_t N = 0;
    for (const auto &Sp : Execs) {
      size_t Off = 0;
      va_t Cur = Sp.VA;
      while (Off < Sp.Len) {
        DecodedInsn DI;
        int Sz = DLight.decodeOneLight(Sp.Data + Off, Sp.Len - Off, Cur, DI);
        if (Sz <= 0) {
          Off += 1;
          Cur += 1;
          continue;
        }
        Off += static_cast<size_t>(Sz);
        Cur += static_cast<va_t>(Sz);
        ++N;
      }
    }
    return N;
  };

  // Native-first single-threaded pass: the real per-function decode path
  // (CFGBuilder uses decodeOneForLift), which routes the common instruction
  // classes through the Capstone-free fixed-width decoder and falls back to
  // Capstone only for the rare tail.  On AArch64 this is the throughput the
  // lift pipeline actually sees; on other targets decodeOneForLift == decodeOne
  // so this simply mirrors the full pass.
  Decoder DLift;
  if (!DLift.init(S->Img.Arch, S->Img.Mode)) {
    S->setError("decoder init failed");
    return nullptr;
  }
  DLift.setDetail(true);
  auto liftPass = [&]() -> uint64_t {
    uint64_t N = 0;
    for (const auto &Sp : Execs) {
      size_t Off = 0;
      va_t Cur = Sp.VA;
      while (Off < Sp.Len) {
        DecodedInsn DI;
        int Sz = DLift.decodeOneForLift(Sp.Data + Off, Sp.Len - Off, Cur, DI);
        if (Sz <= 0) {
          Off += 1;
          Cur += 1;
          continue;
        }
        Off += static_cast<size_t>(Sz);
        Cur += static_cast<va_t>(Sz);
        ++N;
      }
    }
    return N;
  };

  auto [FullInsns, FullNs] = timePass(fullPass);
  auto [LightInsns, LightNs] = timePass(lightPass);
  auto [LiftInsns, LiftNs] = timePass(liftPass);

  // Multi-threaded full-detail throughput.  The pipeline decodes one function
  // per worker thread with a per-thread capstone handle (see buildLowIR /
  // scanCallTargets), so the aggregate decode rate — not the single-core rate
  // above — is what a real run sees.  Slice the executable spans into
  // fixed-size chunks handed out by an atomic work-stealing counter, each
  // worker owning its own Decoder, mirroring the pipeline's decode topology.
  // x86 chunk boundaries fall mid-instruction, so a chunk resyncs from its
  // start exactly like the pipeline's per-function decode; the instruction
  // count is a throughput proxy and need not equal the linear-sweep count.
  constexpr size_t kMtChunk = 64ull << 10;
  struct DecChunk {
    const uint8_t *Data;
    size_t Len;
    va_t VA;
  };
  std::vector<DecChunk> Chunks;
  for (const auto &Sp : Execs) {
    for (size_t Off = 0; Off < Sp.Len; Off += kMtChunk) {
      size_t CLen = std::min(kMtChunk, Sp.Len - Off);
      Chunks.push_back({Sp.Data + Off, CLen, Sp.VA + Off});
    }
  }
  const unsigned MtThreads = std::max(1u, workerThreadCount());
  auto mtPass = [&]() -> uint64_t {
    std::atomic<size_t> Next{0};
    std::atomic<uint64_t> Total{0};
    auto Worker = [&]() {
      Decoder LocalDec;
      if (!LocalDec.init(S->Img.Arch, S->Img.Mode))
        return;
      LocalDec.setDetail(true);
      uint64_t Local = 0;
      for (size_t CI; (CI = Next.fetch_add(1, std::memory_order_relaxed)) <
                      Chunks.size();) {
        const auto &Ck = Chunks[CI];
        size_t Off = 0;
        va_t Cur = Ck.VA;
        while (Off < Ck.Len) {
          DecodedInsn DI;
          int Sz = LocalDec.decodeOne(Ck.Data + Off, Ck.Len - Off, Cur, DI);
          if (Sz <= 0) {
            Off += 1;
            Cur += 1;
            continue;
          }
          Off += static_cast<size_t>(Sz);
          Cur += static_cast<va_t>(Sz);
          ++Local;
        }
      }
      Total.fetch_add(Local, std::memory_order_relaxed);
    };
    std::vector<std::thread> Pool;
    Pool.reserve(MtThreads);
    for (unsigned T = 0; T < MtThreads; ++T)
      Pool.emplace_back(Worker);
    for (auto &T : Pool)
      T.join();
    return Total.load(std::memory_order_relaxed);
  };
  // Multi-threaded native-first aggregate: the same per-thread work-stealing
  // topology, but each worker decodes via decodeOneForLift (native fast path +
  // Capstone fallback) — the rate a real parallel lift achieves.
  auto mtLiftPass = [&]() -> uint64_t {
    std::atomic<size_t> Next{0};
    std::atomic<uint64_t> Total{0};
    auto Worker = [&]() {
      Decoder LocalDec;
      if (!LocalDec.init(S->Img.Arch, S->Img.Mode))
        return;
      LocalDec.setDetail(true);
      uint64_t Local = 0;
      for (size_t CI; (CI = Next.fetch_add(1, std::memory_order_relaxed)) <
                      Chunks.size();) {
        const auto &Ck = Chunks[CI];
        size_t Off = 0;
        va_t Cur = Ck.VA;
        while (Off < Ck.Len) {
          DecodedInsn DI;
          int Sz =
              LocalDec.decodeOneForLift(Ck.Data + Off, Ck.Len - Off, Cur, DI);
          if (Sz <= 0) {
            Off += 1;
            Cur += 1;
            continue;
          }
          Off += static_cast<size_t>(Sz);
          Cur += static_cast<va_t>(Sz);
          ++Local;
        }
      }
      Total.fetch_add(Local, std::memory_order_relaxed);
    };
    std::vector<std::thread> Pool;
    Pool.reserve(MtThreads);
    for (unsigned T = 0; T < MtThreads; ++T)
      Pool.emplace_back(Worker);
    for (auto &T : Pool)
      T.join();
    return Total.load(std::memory_order_relaxed);
  };

  auto [MtInsns, MtNs] = timePass(mtPass);
  auto [MtLiftInsns, MtLiftNs] = timePass(mtLiftPass);

  auto perSec = [](uint64_t Count, double Ns) -> double {
    return Ns > 0.0 ? static_cast<double>(Count) / (Ns / 1e9) : 0.0;
  };
  auto mbPerSec = [](uint64_t Bytes, double Ns) -> double {
    return Ns > 0.0 ? (static_cast<double>(Bytes) / (Ns / 1e9)) / 1e6 : 0.0;
  };

  llvm::json::Object Root;
  Root["arch"] = archShortName(S->Img.Arch);
  Root["exec_bytes"] = static_cast<int64_t>(ExecBytes);
  Root["bench_bytes"] = static_cast<int64_t>(BenchBytes);
  Root["insns"] = static_cast<int64_t>(FullInsns);
  Root["full_detail_ns"] = FullNs;
  Root["light_ns"] = LightNs;
  Root["full_detail_insns_per_sec"] =
      static_cast<int64_t>(perSec(FullInsns, FullNs));
  Root["light_insns_per_sec"] =
      static_cast<int64_t>(perSec(LightInsns, LightNs));
  Root["full_detail_mb_per_sec"] = mbPerSec(BenchBytes, FullNs);
  Root["light_mb_per_sec"] = mbPerSec(BenchBytes, LightNs);
  Root["detail_off_speedup"] = LightNs > 0.0 ? FullNs / LightNs : 0.0;

  // Native-first lift-decode (the path CFGBuilder actually uses): on AArch64
  // this bypasses Capstone's non-scaling DFA for the common classes.
  Root["lift_decode_insns_per_sec"] =
      static_cast<int64_t>(perSec(LiftInsns, LiftNs));
  Root["lift_decode_mb_per_sec"] = mbPerSec(BenchBytes, LiftNs);
  // Single-threaded speedup of the native-first path over full Capstone decode.
  Root["lift_decode_speedup"] = LiftNs > 0.0 ? FullNs / LiftNs : 0.0;

  // Multi-threaded aggregate (the rate a real pipeline decode achieves).
  Root["mt_threads"] = static_cast<int64_t>(MtThreads);
  Root["mt_full_detail_ns"] = MtNs;
  Root["mt_full_detail_insns_per_sec"] =
      static_cast<int64_t>(perSec(MtInsns, MtNs));
  Root["mt_full_detail_mb_per_sec"] = mbPerSec(BenchBytes, MtNs);
  // Wall-clock speedup of the multi-threaded pass over the single-threaded
  // full-detail pass on the same bytes; ceiling is mt_threads.
  Root["mt_scaling"] = MtNs > 0.0 ? FullNs / MtNs : 0.0;

  // Multi-threaded native-first aggregate (the parallel lift-decode rate).
  Root["mt_lift_decode_ns"] = MtLiftNs;
  Root["mt_lift_decode_insns_per_sec"] =
      static_cast<int64_t>(perSec(MtLiftInsns, MtLiftNs));
  Root["mt_lift_decode_mb_per_sec"] = mbPerSec(BenchBytes, MtLiftNs);
  // Aggregate speedup of the parallel native-first pass over single-threaded
  // full Capstone decode.
  Root["mt_lift_decode_speedup"] = MtLiftNs > 0.0 ? FullNs / MtLiftNs : 0.0;

  if (S->Img.Arch == Arch::AArch64) {
    uint64_t BlHitsSink = 0;
    auto blPass = [&]() -> uint64_t {
      uint64_t Words = 0;
      uint64_t Hits = 0;
      for (const auto &Sp : Execs) {
        va_t AlignedVA = (Sp.VA + 3) & ~static_cast<va_t>(3);
        size_t Start = static_cast<size_t>(AlignedVA - Sp.VA);
        for (size_t Off = Start; Off + 4 <= Sp.Len; Off += 4) {
          const uint8_t *P = Sp.Data + Off;
          uint32_t Word = static_cast<uint32_t>(P[0]) |
                          (static_cast<uint32_t>(P[1]) << 8) |
                          (static_cast<uint32_t>(P[2]) << 16) |
                          (static_cast<uint32_t>(P[3]) << 24);
          if (AArch64Lifter::decodeBranchLinkTarget(Word, Sp.VA + Off) !=
              InvalidVA)
            ++Hits;
          ++Words;
        }
      }
      BlHitsSink += Hits;
      return Words;
    };
    auto [BlWords, BlNs] = timePass(blPass);
    // Force the classification work to be observed so the optimizer cannot
    // elide the decode calls the pass is meant to measure.
    volatile uint64_t DceGuard = BlHitsSink;
    (void)DceGuard;
    Root["aarch64_blscan_words_per_sec"] =
        static_cast<int64_t>(perSec(BlWords, BlNs));
    Root["aarch64_blscan_ns"] = BlNs;
    // How much faster the fixed-width BL scan traverses the same executable
    // bytes than the full-detail capstone sweep the scan used to run.
    Root["aarch64_blscan_speedup"] = BlNs > 0.0 ? FullNs / BlNs : 0.0;
  }

  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}
