//===- Pipeline.h - Decompilation pipeline ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the Pipeline class that orchestrates the full decompilation
/// flow: decode -> LowIR -> MedIR -> HighIR (or LLVM IR for patch/lift
/// modes).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_PIPELINE_H
#define NEVERD_PIPELINE_PIPELINE_H

#include "neverd/debug/DebugContext.h"
#include "neverd/evm/EVMIR.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pass/ir/simplify/SymSimplifyPass.h"
#include "neverd/sbf/SBFIR.h"
#include "neverd/solver/SymSynthVerifier.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/OptimizationLevel.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace neverd {

class PipelineTestPeer;

class Decoder;

struct PipelineOptions {
  bool DumpLow = false;
  bool DumpMed = false;
  bool DumpHigh = false;
  bool DumpLlvm = false;
  bool EmitDumpOutput = true;
  bool NoOpt = false;
  bool PatchMode = false;
  bool LiftMode = false;
  size_t MaxFunctions = 0;
  std::string OutputFile;
  evm::Hardfork EVMFork = evm::Hardfork::Latest;
  bool EVMStrict = true;
  sbf::Version SBFVersion = sbf::Version::Auto;
  bool SBFStrict = true;
  /// The runtime a recovered Solana program is described against. It is
  /// separate from SBFVersion because the version comes from the file and this
  /// does not: no chain is named anywhere in a program.
  sbf::RuntimeProfile SBFProfile;
  const sbf::AnchorIdl *SBFIdl = nullptr;
  /// Unit-test-only override for the module jump-table evidence budget.  The
  /// production default remains the compiled safety limit; unlike the former
  /// environment variable, ordinary process state cannot silently alter lift
  /// semantics.
  std::optional<size_t> JumpTableEvidenceBudgetForTesting;
};

enum class PipelineFunctionDisposition {
  Candidate,
  SkippedImportStub,
  SkippedRuntimeScaffold,
  SkippedLimit,
  RejectedLowIR,
  RejectedIncomplete,
  RemovedJumpTableTarget,
  MedIRFailed,
  Accepted,
};

inline const char *
pipelineFunctionDispositionName(PipelineFunctionDisposition Value) {
  switch (Value) {
  case PipelineFunctionDisposition::Candidate:
    return "candidate";
  case PipelineFunctionDisposition::SkippedImportStub:
    return "skipped-import-stub";
  case PipelineFunctionDisposition::SkippedRuntimeScaffold:
    return "skipped-runtime-scaffold";
  case PipelineFunctionDisposition::SkippedLimit:
    return "skipped-limit";
  case PipelineFunctionDisposition::RejectedLowIR:
    return "rejected-low-ir";
  case PipelineFunctionDisposition::RejectedIncomplete:
    return "rejected-incomplete";
  case PipelineFunctionDisposition::RemovedJumpTableTarget:
    return "removed-jump-table-target";
  case PipelineFunctionDisposition::MedIRFailed:
    return "med-ir-failed";
  case PipelineFunctionDisposition::Accepted:
    return "accepted";
  }
  return "unknown";
}

struct PipelineFunctionAudit {
  va_t Entry = 0;
  std::string Name;
  PipelineFunctionDisposition Disposition =
      PipelineFunctionDisposition::Candidate;
  uint64_t DecodedInstructions = 0;
  uint64_t LiftedInstructions = 0;
  std::vector<va_t> DecodeFailures;
  std::vector<va_t> UnsupportedInstructions;
  std::vector<va_t> TruncatedPaths;
  bool HasLowIR = false;
  bool HasMedIR = false;
  bool MedIRVerified = false;
  bool HasLLVMDefinition = false;
};

struct PipelineResult {
  std::vector<LowFunc> LowFuncs;
  std::vector<MedFunc> MedFuncs;
  std::vector<HighFunc> HighFuncs;
  std::unique_ptr<llvm::Module> LlvmModule;
  std::unique_ptr<evm::EVMProgram> EVM;
  std::unique_ptr<sbf::SBFProgram> SBF;
  std::vector<PipelineFunctionAudit> FunctionAudits;
  uint64_t MedIRVerifierFailures = 0;
  uint64_t BackendUnhandledValueIntrinsics = 0;
  bool LLVMVerifierFailed = false;
  std::vector<std::string> LLVMDefinitionNames;
  std::string Error;
  bool Success = false;
};

/// Why observable function optimization stopped.  Values are stable for the
/// public result adapters built on this contract.
enum class OptimizationStopReason : uint8_t {
  Stable = 0,
  CycleDetected = 1,
  BudgetExhausted = 2,
  VerificationFailed = 3,
  InputInvalid = 4,
};
static_assert(
    static_cast<uint8_t>(OptimizationStopReason::Stable) == 0 &&
    static_cast<uint8_t>(OptimizationStopReason::CycleDetected) == 1 &&
    static_cast<uint8_t>(OptimizationStopReason::BudgetExhausted) == 2 &&
    static_cast<uint8_t>(OptimizationStopReason::VerificationFailed) == 3 &&
    static_cast<uint8_t>(OptimizationStopReason::InputInvalid) == 4);

const char *optimizationStopReasonName(OptimizationStopReason Stop);

/// Observable work and stop state for one optimized function.
struct FunctionOptimizationResult {
  bool Changed = false;
  uint64_t SemanticRewrites = 0;
  uint64_t SearchWork = 0;
  solver::ProofStats ProofWork;
  unsigned Rounds = 0;
  OptimizationStopReason Stop = OptimizationStopReason::Stable;
};

/// Module aggregate.  Counters are saturating sums, Rounds is the maximum
/// reached by any definition, and Stop uses the documented severity order.
struct OptimizationResult : FunctionOptimizationResult {
  uint64_t FunctionsVisited = 0;
};

class Pipeline {
public:
  PipelineResult run(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                     const PipelineOptions &Opts = {},
                     DebugContext *Dbg = nullptr);

  /// Run the HelloWorldPass on a module.  Wraps PassBuilder/AnalysisManager
  /// setup so callers avoid instantiating LLVM PassManager templates (which
  /// would create duplicate weak_def symbols when the caller lives in a
  /// different image than libneverd).
  static void runHelloWorldPass(llvm::Module &Mod);

  /// Run the instruction-substitution pass on a module, replacing
  /// integer add/sub/and/or/xor with equivalent instruction sequences for
  /// \p Rounds rounds.  Returns the number of operators substituted.  Like
  /// runHelloWorldPass, this keeps PassManager instantiation inside libneverd.
  static unsigned runInstSubstitutionPass(llvm::Module &Mod,
                                          unsigned Rounds = 1);

  /// Run the constant-encryption pass on a module, replacing integer constant
  /// operands of binary operators / comparisons with run-time-decrypted values.
  /// Returns the number of constant operands encrypted.  Like the others, this
  /// keeps PassManager instantiation inside libneverd.
  static unsigned runConstantEncryptionPass(llvm::Module &Mod);

  /// Run the opaque-predicate pass on a module, guarding basic blocks behind an
  /// always-true predicate the backend cannot fold.  Returns the number of
  /// predicates inserted.  Like the others, keeps PassManager instantiation
  /// inside libneverd.
  static unsigned runOpaquePredicatePass(llvm::Module &Mod);

  /// Run the control-flow flattening pass on a module, rebuilding each
  /// function's CFG as a dispatcher loop.  Returns the number of basic blocks
  /// moved into a dispatcher.  Like the others, keeps PassManager
  /// instantiation inside libneverd.
  static unsigned runControlFlowFlatteningPass(llvm::Module &Mod);

  /// Run the bogus-control-flow pass on a module, growing a dead,
  /// opaque-guarded fake control-flow sub-graph around each basic block.
  /// Returns the number of basic blocks given a bogus sub-graph.  Like the
  /// others, keeps PassManager instantiation inside libneverd.
  static unsigned runBogusControlFlowPass(llvm::Module &Mod);

  /// Run the indirect-branch pass on a module, rewriting two-way conditional
  /// branches into table-driven `indirectbr`.  Returns the number of branches
  /// converted.  Like the others, keeps PassManager instantiation inside
  /// libneverd.
  static unsigned runIndirectBranchPass(llvm::Module &Mod);

  /// Run the indirect-call pass on a module, rewriting direct calls to defined
  /// functions into position-independent indirect calls through an opaque
  /// function pointer.  Returns the number of calls converted.  Like the
  /// others, keeps PassManager instantiation inside libneverd.
  static unsigned runIndirectCallPass(llvm::Module &Mod);

  /// Run the mixed-boolean-arithmetic pass on a module, injecting a
  /// provably-zero MBA term into every integer add/sub/mul/and/or/xor result.
  /// Returns the number of operators wrapped.  Like the others, keeps
  /// PassManager instantiation inside libneverd.
  static unsigned runMBAPass(llvm::Module &Mod);

  /// Run the indirect global-variable pass on a module, rewriting direct
  /// references to defined globals into position-independent indirect addresses
  /// through an opaque pointer.  Returns the number of references made
  /// indirect. Like the others, keeps PassManager instantiation inside
  /// libneverd.
  static unsigned runIndirectGlobalPass(llvm::Module &Mod);

  /// Run the value-laundering pass on a module, routing integer (scalar /
  /// integer-vector) instruction results through a volatile stack slot and
  /// redirecting their uses to the reloaded value.  Returns the number of
  /// values laundered.  Like the others, keeps PassManager instantiation inside
  /// libneverd.
  static unsigned runValueLaunderingPass(llvm::Module &Mod);

  /// Run the constant-pooling pass on a module, moving integer constant
  /// operands of binary operators / comparisons into a pass-created read-only
  /// global pool fetched at run time through an opaque index.  Returns the
  /// number of constant operands pooled.  Like the others, keeps PassManager
  /// instantiation inside libneverd.
  static unsigned runConstantPoolingPass(llvm::Module &Mod);

  /// Run the bit-masking pass on a module, replacing integer (scalar /
  /// integer-vector) results with the bitwise identity `(x & m) | (x & ~m)`
  /// where the two masks come from independent volatile slots.  Returns the
  /// number of values masked.  Like the others, keeps PassManager instantiation
  /// inside libneverd.
  static unsigned runBitMaskingPass(llvm::Module &Mod);

  /// Selects which L1 obfuscation passes runObfuscationPasses applies,
  /// mirroring the per-session toggles the patch entry points read.
  struct ObfuscationConfig {
    bool InstSubstitution = false;
    unsigned InstSubstitutionRounds = 1;
    bool ConstantEncryption = false;
    bool OpaquePredicate = false;
    bool BogusControlFlow = false;
    bool ControlFlowFlattening = false;
    bool IndirectBranch = false;
    bool IndirectCall = false;
    bool MBA = false;
    bool IndirectGlobal = false;
    bool ValueLaunder = false;
    bool ConstantPooling = false;
    bool BitMasking = false;
  };

  /// Per-pass substitution/insertion counts returned by runObfuscationPasses
  /// (each stays 0 when its toggle is off or the pass finds nothing to do).
  struct ObfuscationCounts {
    unsigned Substitution = 0;
    unsigned ConstEnc = 0;
    unsigned OpaquePred = 0;
    unsigned Bogus = 0;
    unsigned Flatten = 0;
    unsigned IndirectBranch = 0;
    unsigned IndirectCall = 0;
    unsigned MBA = 0;
    unsigned IndirectGlobal = 0;
    unsigned ValueLaunder = 0;
    unsigned ConstPool = 0;
    unsigned BitMask = 0;

    unsigned total() const {
      return Substitution + ConstEnc + OpaquePred + Bogus + Flatten +
             IndirectBranch + IndirectCall + MBA + IndirectGlobal +
             ValueLaunder + ConstPool + BitMask;
    }
  };

  /// Apply the enabled obfuscation passes to \p Mod in the canonical order the
  /// patch entry points use: substitution -> constant encryption -> opaque
  /// predicates -> bogus control flow -> control-flow flattening -> indirect
  /// branches -> indirect calls -> MBA -> indirect globals -> value laundering
  /// -> constant pooling -> bit masking.  This is the single source of truth
  /// for that order; the patch C API and the equivalence tests both go through
  /// it so they can never drift apart.  Passes whose toggle is false are
  /// skipped.
  static ObfuscationCounts runObfuscationPasses(llvm::Module &Mod,
                                                const ObfuscationConfig &Cfg);

  /// How much value rewriting the lift path's optimizer is allowed to do.
  ///
  /// Only lift reads this.  The conservative patch order runs no value-changing
  /// transform at any strength, so which one a caller names cannot affect a
  /// patch.
  enum class OptStrength {
    /// Promotion, SROA, the semantic simplifier's joint fixed point with
    /// InstCombine, and one SimplifyCFG: the smallest order that recovers
    /// obfuscated arithmetic.  Kept nameable so a caller that needs the
    /// optimizer's footprint on the emitted IR held to that minimum -- and the
    /// tests that pin what the deeper order adds -- can still ask for it.
    Thin,
    /// The selected LLVM default module pipeline, with the semantic fixed point
    /// injected at its early-simplification and late-scalar extension points.
    /// This is the default because interprocedural and global optimization can
    /// remove redundancy that no function-only pass order can observe.
    Deep,
  };

  /// Controls one transactional module-optimization request.  Deep mode uses
  /// LLVMLevel exactly and injects Semantic at the pipeline's early and late
  /// scalar extension points.  Conservative mode ignores both fields.
  struct OptimizationOptions {
    bool Conservative = false;
    OptStrength Strength = OptStrength::Deep;
    llvm::OptimizationLevel LLVMLevel = llvm::OptimizationLevel::O2;
    SymSimplifyOptions Semantic;
    /// Per SemanticFixedPointPass invocation.  Deep has independent early and
    /// late invocations, so each receives this complete budget.  Zero means
    /// unlimited and still stops on a stable state or exact cycle.
    unsigned MaxRounds = 0;
    /// Optional synchronous policy check after LLVM and exception-contract
    /// verification.  The callable is not owned and must outlive this request.
    llvm::function_ref<bool(const llvm::Module &)> PostTransformVerifier;
  };

  /// Apply the optimization pass order lift uses on every emitted module.  This
  /// is the counterpart to runObfuscationPasses and public for the same reason:
  /// it is the single source of truth for that order, so anything that needs to
  /// reason about what the optimizer does -- above all the tests that pin the
  /// semantic simplifier's joint fixed point with InstCombine -- goes through
  /// it rather than assembling a pass list of its own that could drift.
  ///
  /// \p Conservative selects the patch pipeline's order, which stops after
  /// promotion and SROA and runs no value-changing transform, so a module it
  /// touches keeps the decompiled program's semantics byte for byte.
  ///
  /// \p Strength selects among the lift orders and is ignored when
  /// \p Conservative is set.  It defaults to the deeper one so that lift and
  /// decompile get it without naming it, which is also what keeps a patch
  /// unaffected: the patch entry point passes Conservative and never reaches
  /// the value rewriters this selects between.
  static void optimizeModule(llvm::Module &Mod, bool Conservative = false,
                             OptStrength Strength = OptStrength::Deep);

  /// Apply Options to a same-context clone and commit only after LLVM's
  /// verifier and the optional caller verifier accept the result.  Invalid
  /// input and rejected output leave Mod byte-for-byte unchanged.  An exactly
  /// unchanged candidate is not committed and preserves caller-held IR handles.
  /// A changed successful candidate replaces the module contents, so callers
  /// must reacquire IR handles.
  static OptimizationResult optimizeModule(llvm::Module &Mod,
                                           const OptimizationOptions &Options);

  /// Typed counterpart to the compatibility overload above.  MaxRounds has the
  /// same per-semantic-invocation meaning as OptimizationOptions::MaxRounds.
  static OptimizationResult optimizeModule(llvm::Module &Mod, bool Conservative,
                                           OptStrength Strength,
                                           unsigned MaxRounds);

  /// Serialize the canonical textual IR dumps without writing to global
  /// stdout. High-level APIs use these overloads to return one stable dump
  /// across executable/shared-library boundaries.
  static void dumpLowIR(const std::vector<LowFunc> &Funcs,
                        llvm::raw_ostream &OS);
  static void dumpMedIR(const std::vector<MedFunc> &Funcs,
                        llvm::raw_ostream &OS);
  static void dumpHighIR(const std::vector<HighFunc> &Funcs,
                         llvm::raw_ostream &OS);

private:
  friend class PipelineTestPeer;

  /// Interior basic-block addresses are module-local LLVM blockaddress
  /// constants and cannot be split from their owner into another emission
  /// shard.  Function-entry-only pointer tables remain shard-safe.
  static bool requiresSerialLLVMEmission(const std::vector<MedFunc> &Funcs,
                                         const BinaryImage &Img);
  /// Detect function entries, merge debug symbols, and filter import stubs.
  static std::vector<std::pair<va_t, std::string>>
  detectFunctions(const BinaryImage &Img, Decoder &Dec,
                  const PipelineOptions &Opts, DebugContext *Dbg,
                  PipelineResult &Result);

  /// Phase 1: decode + build LowIR for all candidates in parallel.
  void buildLowIR(const BinaryImage &Img,
                  const std::vector<std::pair<va_t, std::string>> &Candidates,
                  const PipelineOptions &Opts, DebugContext *Dbg,
                  PipelineResult &Result);

  /// Phase 2: convert LowIR -> MedIR in parallel.
  void buildMedIR(const BinaryImage &Img, const PipelineOptions &Opts,
                  PipelineResult &Result);

  /// Shortcut path: MedIR -> LLVM IR (skip HighIR).
  bool runPatchLiftMode(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                        const PipelineOptions &Opts, PipelineResult &Result);

  /// Emit + verify + optimize the MedIR functions in parallel and return the
  /// combined module in \p Ctx.  The functions are split into shards; each
  /// shard is emitted (with link-mergeable globals), verified and optimized in
  /// its own LLVMContext on one of \p NumThreads workers, then the shard
  /// modules are serialized to bitcode and linked into one module in \p Ctx.
  /// Because the emit and (function-local) optimization passes are the
  /// single-threaded bottleneck of lift, this is the main end-to-end speedup.
  /// The result is semantically identical to the serial path: every function is
  /// defined in exactly one shard and per-address globals (crucially the
  /// writable .data/.bss segment globals) merge to one shared object at link
  /// time.  Returns null if any shard cannot be emitted, parsed, or linked, so
  /// callers never mistake an incomplete module for a complete lift.
  /// LLVMVerifierFailed distinguishes invalid shard IR or a transactional
  /// optimizer rejection from the other null-return paths.
  ///
  /// The shard count is derived from the total MedIR work rather than pinned to
  /// \p NumThreads: peak memory is (in-flight shards) x (slice size), so
  /// slicing finely keeps a large binary's unoptimized IR from all being
  /// resident at once while still saturating every worker.
  static std::unique_ptr<llvm::Module> emitLLVMSharded(
      const std::vector<MedFunc> &Funcs, llvm::LLVMContext &Ctx, Arch TheArch,
      const std::vector<std::pair<va_t, std::string>> &Imports,
      const BinaryImage &Img, BinaryFormat Fmt, bool NoOpt, unsigned NumThreads,
      uint64_t &UnhandledValueIntrinsics, bool &LLVMVerifierFailed);

  /// Phase 3: convert MedIR -> HighIR in parallel.
  void buildHighIR(const BinaryImage &Img, const PipelineOptions &Opts,
                   PipelineResult &Result);

  static void
  mergeDebugSymbols(std::vector<std::pair<va_t, std::string>> &FuncEntries,
                    DebugContext &Dbg);
  static void detectThunkStubs(const std::vector<LowFunc> &LowFuncs,
                               std::map<va_t, std::string> &AllFuncNames);
  static std::map<va_t, std::string>
  buildFuncNameMap(const BinaryImage &Img, const PipelineResult &Result);
  static void dumpLowIR(const std::vector<LowFunc> &Funcs);
  static void dumpMedIR(const std::vector<MedFunc> &Funcs);
  static void dumpHighIR(const std::vector<HighFunc> &Funcs);
  /// Promote the emitter's SSA-via-memory scaffolding (per-temp allocas getVar
  /// materializes) to registers WITHOUT any value-changing optimization.  Run
  /// even when the NeverD optimizer is disabled (NoOpt): it is
  /// canonicalization, not optimization — it preserves the decompiled program's
  /// semantics and per-instruction debug attribution while stripping the ~4x
  /// load/store bloat that makes a heavily-unrolled SSE kernel lift to an
  /// 80K-instruction single block (pathological for LLVM codegen).  The
  /// address-taken frame alloca is left in memory; only the temporaries are
  /// promoted.
  static void promoteScaffoldingAllocas(llvm::Module &Mod);
};

} // namespace neverd

#endif // NEVERD_PIPELINE_PIPELINE_H
