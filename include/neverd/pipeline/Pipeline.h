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
#include "neverd/sbf/SBFIR.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace neverd {

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
};

struct PipelineResult {
  std::vector<LowFunc> LowFuncs;
  std::vector<MedFunc> MedFuncs;
  std::vector<HighFunc> HighFuncs;
  std::unique_ptr<llvm::Module> LlvmModule;
  std::unique_ptr<evm::EVMProgram> EVM;
  std::unique_ptr<sbf::SBFProgram> SBF;
  std::string Error;
  bool Success = false;
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
  /// Detect function entries, merge debug symbols, and filter import stubs.
  static std::vector<std::pair<va_t, std::string>>
  detectFunctions(const BinaryImage &Img, Decoder &Dec,
                  const PipelineOptions &Opts, DebugContext *Dbg);

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
  /// time.  Returns null only if no shard produced a module.
  ///
  /// The shard count is derived from the total MedIR work rather than pinned to
  /// \p NumThreads: peak memory is (in-flight shards) x (slice size), so
  /// slicing finely keeps a large binary's unoptimized IR from all being
  /// resident at once while still saturating every worker.
  static std::unique_ptr<llvm::Module>
  emitLLVMSharded(const std::vector<MedFunc> &Funcs, llvm::LLVMContext &Ctx,
                  Arch TheArch,
                  const std::vector<std::pair<va_t, std::string>> &Imports,
                  const BinaryImage &Img, BinaryFormat Fmt, bool NoOpt,
                  unsigned NumThreads);

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
  static void optimizeModule(llvm::Module &Mod, bool Conservative = false);
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
