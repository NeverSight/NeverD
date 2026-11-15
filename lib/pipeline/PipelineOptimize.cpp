//===- PipelineOptimize.cpp - LLVM IR optimization ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LLVM new-pass-manager optimization pipeline applied to lifted modules.
///
//===----------------------------------------------------------------------===//

#include "OptimizationGuards.h"
#include "PipelineSourceOptimization.h"
#include "SemanticConvergence.h"

#include "neverd/Common.h"
#include "neverd/analysis/LLVMPrivateFrameAccesses.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/pass/ir/HelloWorldPass.h"
#include "neverd/pass/ir/obf/BitMaskingPass.h"
#include "neverd/pass/ir/obf/BogusControlFlowPass.h"
#include "neverd/pass/ir/obf/ConstantEncryptionPass.h"
#include "neverd/pass/ir/obf/ConstantPoolingPass.h"
#include "neverd/pass/ir/obf/ControlFlowFlatteningPass.h"
#include "neverd/pass/ir/obf/IndirectBranchPass.h"
#include "neverd/pass/ir/obf/IndirectCallPass.h"
#include "neverd/pass/ir/obf/IndirectGlobalPass.h"
#include "neverd/pass/ir/obf/InstSubstitutionPass.h"
#include "neverd/pass/ir/obf/MBAPass.h"
#include "neverd/pass/ir/obf/OpaquePredicatePass.h"
#include "neverd/pass/ir/obf/ValueLaunderingPass.h"
#include "neverd/pass/ir/simplify/ByteCellScalarizationPass.h"
#include "neverd/pass/ir/simplify/ByteMemoryForwardingPass.h"
#include "neverd/pass/ir/simplify/ControlFlowRecoveryPass.h"
#include "neverd/pass/ir/simplify/SymSimplifyPass.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/StructuralHash.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

namespace {

void addSaturating(uint64_t &Total, uint64_t Delta) {
  constexpr uint64_t Max = std::numeric_limits<uint64_t>::max();
  Total = Delta > Max - Total ? Max : Total + Delta;
}

std::string snapshotFunction(const llvm::Function &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  F.print(OS);
  return Text;
}

std::string snapshotModule(const llvm::Module &M) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  M.print(OS, nullptr);
  return Text;
}

/// Alternates LLVM canonicalization with semantic simplification until their
/// joint state is stable or repeats exactly.  A finite budget is supplied by
/// the caller; zero leaves capability limited only by actual convergence.
class SemanticFixedPointPass
    : public llvm::PassInfoMixin<SemanticFixedPointPass> {
public:
  explicit SemanticFixedPointPass(
      unsigned MaxRounds = 0, SymSimplifyOptions Options = {},
      std::shared_ptr<OptimizationResult> ModuleSink = {})
      : MaxRounds(MaxRounds), Options(std::move(Options)),
        ModuleSink(std::move(ModuleSink)) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);

private:
  unsigned MaxRounds;
  SymSimplifyOptions Options;
  std::shared_ptr<OptimizationResult> ModuleSink;
};

llvm::PreservedAnalyses
SemanticFixedPointPass::run(llvm::Function &F,
                            llvm::FunctionAnalysisManager &FAM) {
  llvm::FunctionPassManager Canonicalize;
  Canonicalize.addPass(llvm::InstCombinePass());
  ByteMemoryForwardingOptions MemoryOptions;
  MemoryOptions.SimplifyNumericMemory = true;
  Canonicalize.addPass(ByteMemoryForwardingPass(MemoryOptions));

  FunctionOptimizationResult Result = driveSemanticConvergence(
      MaxRounds,
      [&] {
        const bool Canonicalized = !Canonicalize.run(F, FAM).areAllPreserved();
        SymSimplifyResult Semantic =
            SymSimplifyPass::simplifyWithResult(F, Options);
        const bool Changed = Canonicalized || Semantic.Rewrites != 0;

        // simplifyWithResult mutates outside the pass manager.  Invalidate
        // before a subsequent round can read analyses describing the old IR.
        if (Semantic.Rewrites != 0)
          FAM.invalidate(F, llvm::PreservedAnalyses::none());

        return ConvergenceRound{
            Changed, std::move(Semantic),
            Changed ? llvm::StructuralHash(F, /*DetailedHash=*/true) : 0};
      },
      [&] { return snapshotFunction(F); });

  if (ModuleSink)
    mergeFunctionOptimizationResult(*ModuleSink, Result);

  return Result.Changed ? llvm::PreservedAnalyses::none()
                        : llvm::PreservedAnalyses::all();
}

} // namespace

void Pipeline::promoteScaffoldingAllocas(llvm::Module &Mod) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassInstrumentationCallbacks PIC;
  llvm::OptNoneInstrumentation OptNone(/*DebugLogging=*/false);
  OptNone.registerCallbacks(PIC);
  llvm::PassBuilder PB(nullptr, llvm::PipelineTuningOptions(), std::nullopt,
                       &PIC);
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // mem2reg only: promotes the per-temp allocas the emitter spills SSA values
  // through (their addresses are never taken) and leaves the address-taken
  // frame alloca in memory.  No SROA / InstCombine / SimplifyCFG, so no
  // value-changing transform runs.
  llvm::FunctionPassManager FPM;
  FPM.addPass(llvm::PromotePass());
  llvm::ModulePassManager MPM;
  MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
  MPM.run(Mod, MAM);
}

static OptimizationResult
runOptimizationPipeline(llvm::Module &Mod,
                        const Pipeline::OptimizationOptions &Options) {
  uint64_t DefinitionCount = 0;
  for (const llvm::Function &F : Mod)
    if (!F.isDeclaration())
      addSaturating(DefinitionCount, 1);

  auto Result = std::make_shared<OptimizationResult>();
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassInstrumentationCallbacks PIC;
  llvm::OptNoneInstrumentation OptNone(/*DebugLogging=*/false);
  OptNone.registerCallbacks(PIC);
  registerOptNoneInterproceduralGuard(PIC);
  llvm::PassBuilder PB(nullptr, llvm::PipelineTuningOptions(), std::nullopt,
                       &PIC);

  if (!Options.Conservative && Options.EnableSemantic &&
      Options.Strength == Pipeline::OptStrength::Deep) {
    PB.registerPipelineEarlySimplificationEPCallback(
        [MaxRounds = Options.MaxRounds, Semantic = Options.Semantic,
         Result](llvm::ModulePassManager &MPM, llvm::OptimizationLevel,
                 llvm::ThinOrFullLTOPhase) {
          llvm::FunctionPassManager FPM;
          FPM.addPass(SemanticFixedPointPass(MaxRounds, Semantic, Result));
          MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
        });
    PB.registerScalarOptimizerLateEPCallback(
        [MaxRounds = Options.MaxRounds, Semantic = Options.Semantic,
         Result](llvm::FunctionPassManager &FPM, llvm::OptimizationLevel) {
          FPM.addPass(SemanticFixedPointPass(MaxRounds, Semantic, Result));
        });
  }

  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::FunctionPassManager Prefix;
  if (!Options.Conservative && Options.EnableControlFlowRecovery) {
    // Before promotion, because that is where a dispatcher's state lives: both
    // a lifted function and a flattened one keep everything in stack slots, the
    // second because no block dominates another once every one of them is
    // reached through the switch.  Recovering the graph first is what lets the
    // promotion below put those values back in registers with the phis the real
    // control flow calls for, instead of phis at the dispatcher.
    Prefix.addPass(ControlFlowRecoveryPass());
  }
  Prefix.addPass(llvm::PromotePass());
  llvm::ModulePassManager PrefixPipeline;
  PrefixPipeline.addPass(
      llvm::createModuleToFunctionPassAdaptor(std::move(Prefix)));
  llvm::PreservedAnalyses PrefixPA = PrefixPipeline.run(Mod, MAM);
  Result->Changed |= !PrefixPA.areAllPreserved();
  if (Options.SourceMap)
    Options.SourceMap->refreshExpressionOrigins();

  // The emitter's per-temp allocas first need promotion; only then do its
  // native stack-address expressions expose a constant frame displacement.
  // Canonicalize exact private accesses before SROA decides whether the frame
  // can be split into scalar homes.
  if (!Options.Conservative && canonicalizePrivateFrameAccesses(Mod)) {
    Result->Changed = true;
    MAM.invalidate(Mod, llvm::PreservedAnalyses::none());
  }
  llvm::FunctionPassManager FramePromotion;
  FramePromotion.addPass(llvm::SROAPass(llvm::SROAOptions::PreserveCFG));
  if (!Options.Conservative) {
    // Let ordinary promotion handle exact scalar homes before reconstructing
    // the overlapping accesses that still remain in byte storage.
    FramePromotion.addPass(ByteMemoryForwardingPass());
    FramePromotion.addPass(llvm::SROAPass(llvm::SROAOptions::PreserveCFG));
    FramePromotion.addPass(ByteCellScalarizationPass());
    FramePromotion.addPass(llvm::SROAPass(llvm::SROAOptions::PreserveCFG));
  }
  // Forward memory-equivalent frame reloads before semantic measurement.
  // MemorySSA preserves intervening stores that may alias the same slot.
  if (!Options.Conservative)
    FramePromotion.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
  llvm::ModulePassManager FramePipeline;
  FramePipeline.addPass(
      llvm::createModuleToFunctionPassAdaptor(std::move(FramePromotion)));
  llvm::PreservedAnalyses FramePA = FramePipeline.run(Mod, MAM);
  Result->Changed |= !FramePA.areAllPreserved();

  if (!Options.Conservative) {
    if (Options.Strength == Pipeline::OptStrength::Source) {
      llvm::FunctionPassManager Source;
      if (Options.SourcePasses & SourceViewOptions::InstCombine)
        Source.addPass(llvm::InstCombinePass());
      if (Options.SourcePasses & SourceViewOptions::EarlyCSE)
        Source.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/true));
      if (Options.EnableSemantic)
        Source.addPass(SemanticFixedPointPass(Options.MaxRounds,
                                              Options.Semantic, Result));
      if (Options.SourcePasses & SourceViewOptions::SimplifyCFG)
        Source.addPass(llvm::SimplifyCFGPass());
      if (Options.SourcePasses & SourceViewOptions::DCE)
        Source.addPass(llvm::DCEPass());
      llvm::ModulePassManager SourcePipeline;
      SourcePipeline.addPass(
          llvm::createModuleToFunctionPassAdaptor(std::move(Source)));
      Result->Changed |= !SourcePipeline.run(Mod, MAM).areAllPreserved();
    } else if (Options.Strength == Pipeline::OptStrength::Thin) {
      llvm::FunctionPassManager Thin;
      if (Options.EnableSemantic)
        Thin.addPass(SemanticFixedPointPass(Options.MaxRounds, Options.Semantic,
                                            Result));
      else
        Thin.addPass(llvm::InstCombinePass());
      Thin.addPass(llvm::SimplifyCFGPass());
      llvm::ModulePassManager ThinPipeline;
      ThinPipeline.addPass(
          llvm::createModuleToFunctionPassAdaptor(std::move(Thin)));
      llvm::PreservedAnalyses ThinPA = ThinPipeline.run(Mod, MAM);
      Result->Changed |= !ThinPA.areAllPreserved();
    } else {
      // A source view shows each function as the machine runs it.  The
      // inliner would copy a callee's body into its callers, and only into
      // those that share the callee's LLVM shard, so the C would change
      // with how the module was split and lose the calls it observed.
      llvm::SmallVector<llvm::WeakVH, 0> Pinned;
      if (Options.KeepFunctionBoundaries)
        for (llvm::Function &F : Mod)
          if (!F.isDeclaration() &&
              !F.hasFnAttribute(llvm::Attribute::NoInline) &&
              !F.hasFnAttribute(llvm::Attribute::AlwaysInline)) {
            F.addFnAttr(llvm::Attribute::NoInline);
            Pinned.push_back(&F);
          }
      // A view's module holds some of the image's functions, so it does not
      // see every store to a writable table of code pointers: what a load
      // finds is not its initializer.  GlobalOpt would otherwise take a
      // function pointer no function here writes for a constant, and turn a
      // call through it into a call to the first function it held, as of a
      // lazily bound `__imp_X` its binder rewrites.
      llvm::SmallVector<llvm::WeakVH, 0> Unsettled;
      if (Options.KeepFunctionBoundaries)
        for (llvm::GlobalVariable &G : Mod.globals())
          if (G.hasLocalLinkage() && G.hasInitializer() && !G.isConstant() &&
              !G.isExternallyInitialized() &&
              parseNdCodePtrSymbol(G.getName())) {
            G.setExternallyInitialized(true);
            Unsettled.push_back(&G);
          }
      llvm::ModulePassManager DefaultPipeline =
          Options.LLVMLevel == llvm::OptimizationLevel::O0
              ? PB.buildO0DefaultPipeline(Options.LLVMLevel)
              : PB.buildPerModuleDefaultPipeline(Options.LLVMLevel);
      llvm::PreservedAnalyses DefaultPA = DefaultPipeline.run(Mod, MAM);
      Result->Changed |= !DefaultPA.areAllPreserved();
      for (llvm::WeakVH &Handle : Pinned)
        if (auto *F = llvm::dyn_cast_or_null<llvm::Function>(Handle))
          F->removeFnAttr(llvm::Attribute::NoInline);
      for (llvm::WeakVH &Handle : Unsettled)
        if (auto *G = llvm::dyn_cast_or_null<llvm::GlobalVariable>(Handle))
          G->setExternallyInitialized(false);
    }
  }

  // Absolute integer addresses retain their bits. Only the pre-optimization
  // private-frame proof above may recover an allocation-relative pointer.
  // Both Deep extension points intentionally contribute semantic work to the
  // same sink.  A function definition is nevertheless one visited function,
  // independent of how many extension points observe it.
  Result->FunctionsVisited = DefinitionCount;

  LLVM_DEBUG(llvm::dbgs() << "pipeline: LLVM optimization applied ("
                          << Result->FunctionsVisited
                          << " functions, conservative=" << Options.Conservative
                          << ", deep="
                          << (Options.Strength == Pipeline::OptStrength::Deep)
                          << ", stop="
                          << optimizationStopReasonName(Result->Stop) << ")\n");
  return *Result;
}

void Pipeline::optimizeModule(llvm::Module &Mod, bool Conservative,
                              OptStrength Strength) {
  OptimizationOptions Options;
  Options.Conservative = Conservative;
  Options.Strength = Strength;
  (void)optimizeModule(Mod, Options);
}

static OptimizationResult
runCheckedOptimization(llvm::Module &Mod,
                       const Pipeline::OptimizationOptions &Options) {
  auto Result = runOptimizationPipeline(Mod, Options);
  {
    llvm::raw_string_ostream Diagnostics(Result.Diagnostic);
    if (llvm::verifyModule(Mod, &Diagnostics)) {
      Result.Changed = false;
      Result.Stop = OptimizationStopReason::VerificationFailed;
      return Result;
    }
  }

  auto CandidateContracts =
      exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!CandidateContracts) {
    Result.Diagnostic = llvm::toString(CandidateContracts.takeError());
    Result.Changed = false;
    Result.Stop = OptimizationStopReason::VerificationFailed;
    return Result;
  }

  if (Options.PostTransformVerifier && !Options.PostTransformVerifier(Mod)) {
    Result.Changed = false;
    Result.Stop = OptimizationStopReason::VerificationFailed;
    Result.Diagnostic = "post-transform verification failed";
    return Result;
  }

  return Result;
}

OptimizationResult
Pipeline::optimizeModule(llvm::Module &Mod,
                         const OptimizationOptions &Options) {
  OptimizationResult Result;
  if (!Options.Conservative && Options.Strength != OptStrength::Thin &&
      Options.Strength != OptStrength::Deep &&
      Options.Strength != OptStrength::Source) {
    Result.Stop = OptimizationStopReason::InputInvalid;
    Result.Diagnostic = "invalid optimization strength";
    return Result;
  }

  {
    llvm::raw_string_ostream Diagnostics(Result.Diagnostic);
    if (llvm::verifyModule(Mod, &Diagnostics)) {
      Result.Stop = OptimizationStopReason::InputInvalid;
      return Result;
    }
  }

  auto Contracts = exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!Contracts) {
    Result.Diagnostic = llvm::toString(Contracts.takeError());
    Result.Stop = OptimizationStopReason::InputInvalid;
    return Result;
  }

  const auto InputHash = llvm::StructuralHash(Mod, /*DetailedHash=*/true);
  llvm::ValueToValueMapTy SourceValues;
  std::unique_ptr<llvm::Module> Candidate =
      llvm::CloneModule(Mod, SourceValues);
  std::optional<LLVMSourceMap> CandidateSources;
  if (Options.SourceMap) {
    CandidateSources = *Options.SourceMap;
    CandidateSources->remap(
        [&](llvm::Value *V) { return SourceValues.lookup(V); });
  }
  auto CandidateOptions = Options;
  CandidateOptions.SourceMap = CandidateSources ? &*CandidateSources : nullptr;
  Result = runCheckedOptimization(*Candidate, CandidateOptions);
  if (Result.Stop == OptimizationStopReason::VerificationFailed)
    return Result;

  // A pass manager's PreservedAnalyses is an invalidation contract, not an
  // exact change report.  Confirm the final candidate itself before replacing
  // Mod: this both catches metadata-only edits that StructuralHash omits and
  // preserves every caller-held IR handle when the transaction is a no-op.
  if (llvm::StructuralHash(*Candidate, /*DetailedHash=*/true) == InputHash &&
      snapshotModule(*Candidate) == snapshotModule(Mod)) {
    Result.Changed = false;
    return Result;
  }

  Result.Changed = true;
  if (CandidateSources)
    *Options.SourceMap = std::move(*CandidateSources);
  Mod = std::move(*Candidate);
  return Result;
}

static Pipeline::OptimizationOptions
sourceOptimizationOptions(LLVMSourceMap *Sources,
                          const SourceViewOptions &View) {
  Pipeline::OptimizationOptions Options;
  Options.SourceMap = Sources;
  Options.EnableSemantic = View.Symbolic;
  Options.Semantic.EnableSynthesis = View.Symbolic;
  Options.Semantic.Synthesis = symbolic::SynthOptions::sourceView();
  Options.Semantic.SynthesisSolver = solver::SolverOptions::sourceView();
  Options.EnableControlFlowRecovery = View.RecoverControlFlow;
  Options.KeepFunctionBoundaries = true;
  switch (View.LLVM) {
  case SourceViewOptions::LLVMOptimization::Basic:
  case SourceViewOptions::LLVMOptimization::Custom:
    Options.Strength = Pipeline::OptStrength::Source;
    Options.SourcePasses =
        View.LLVM == SourceViewOptions::LLVMOptimization::Basic
            ? SourceViewOptions::BasicPasses
            : View.Passes;
    break;
  case SourceViewOptions::LLVMOptimization::O0:
    Options.LLVMLevel = llvm::OptimizationLevel::O0;
    break;
  case SourceViewOptions::LLVMOptimization::O1:
    Options.LLVMLevel = llvm::OptimizationLevel::O1;
    break;
  case SourceViewOptions::LLVMOptimization::O2:
    Options.LLVMLevel = llvm::OptimizationLevel::O2;
    break;
  case SourceViewOptions::LLVMOptimization::O3:
    Options.LLVMLevel = llvm::OptimizationLevel::O3;
    break;
  }
  return Options;
}

bool tryOptimizeOwnedSourceModule(llvm::Module &Mod, LLVMSourceMap *Sources,
                                  const SourceViewOptions &View) {
  auto Input = exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!Input) {
    llvm::consumeError(Input.takeError());
    return false;
  }
  // No IR handles have escaped from this emission, so promotion and later
  // passes can update the source map's tracking handles in place.
  auto Result =
      runCheckedOptimization(Mod, sourceOptimizationOptions(Sources, View));
  return Result.Stop != OptimizationStopReason::VerificationFailed;
}

OptimizationResult
Pipeline::optimizeOrPromoteModule(llvm::Module &Mod, LLVMSourceMap *Sources,
                                  const SourceViewOptions &View) {
  auto Options = sourceOptimizationOptions(Sources, View);
  OptimizationResult Result = optimizeModule(Mod, Options);
  if (Result.Stop == OptimizationStopReason::VerificationFailed) {
    // A source view can still print the verified input of a rejected
    // transaction. Promote that input through the conservative pipeline,
    // retaining its exception contract and the rejection diagnostic. Native
    // optimization callers keep the original failure; no counter is relaxed.
    Options.Conservative = true;
    OptimizationResult Promoted = optimizeModule(Mod, Options);
    if (Promoted.Stop != OptimizationStopReason::InputInvalid &&
        Promoted.Stop != OptimizationStopReason::VerificationFailed)
      Promoted.Diagnostic = "source optimization rejected; retained verified "
                            "input with storage promotion: " +
                            Result.Diagnostic;
    return Promoted;
  }
  if (Result.Stop != OptimizationStopReason::InputInvalid ||
      llvm::verifyModule(Mod))
    return Result;

  const auto InputHash = llvm::StructuralHash(Mod, /*DetailedHash=*/true);
  llvm::ValueToValueMapTy SourceValues;
  std::unique_ptr<llvm::Module> Candidate =
      llvm::CloneModule(Mod, SourceValues);
  std::optional<LLVMSourceMap> CandidateSources;
  if (Sources) {
    CandidateSources = *Sources;
    CandidateSources->remap(
        [&](llvm::Value *V) { return SourceValues.lookup(V); });
  }
  promoteScaffoldingAllocas(*Candidate);
  if (CandidateSources)
    CandidateSources->refreshExpressionOrigins();
  {
    std::string Diagnostic;
    llvm::raw_string_ostream Diagnostics(Diagnostic);
    if (llvm::verifyModule(*Candidate, &Diagnostics)) {
      Result.Diagnostic = std::move(Diagnostic);
      Result.Stop = OptimizationStopReason::VerificationFailed;
      return Result;
    }
  }
  if (llvm::StructuralHash(*Candidate, /*DetailedHash=*/true) == InputHash &&
      snapshotModule(*Candidate) == snapshotModule(Mod))
    return Result;

  if (CandidateSources)
    *Sources = std::move(*CandidateSources);
  Mod = std::move(*Candidate);
  Result.Changed = true;
  return Result;
}

OptimizationResult Pipeline::optimizeModule(llvm::Module &Mod,
                                            bool Conservative,
                                            OptStrength Strength,
                                            unsigned MaxRounds) {
  OptimizationOptions Options;
  Options.Conservative = Conservative;
  Options.Strength = Strength;
  Options.MaxRounds = MaxRounds;
  return optimizeModule(Mod, Options);
}

void Pipeline::runHelloWorldPass(llvm::Module &Mod) {
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);

  HelloWorldPass HW;
  HW.run(Mod, MAM);
}

unsigned Pipeline::runInstSubstitutionPass(llvm::Module &Mod, unsigned Rounds) {
  // inject() does not touch any AnalysisManager, so no PassBuilder setup is
  // needed; it also keeps PassManager templates out of consumer TUs.
  return InstSubstitutionPass::inject(Mod, Rounds);
}

unsigned Pipeline::runConstantEncryptionPass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return ConstantEncryptionPass::inject(Mod);
}

unsigned Pipeline::runOpaquePredicatePass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return OpaquePredicatePass::inject(Mod);
}

unsigned Pipeline::runControlFlowFlatteningPass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return ControlFlowFlatteningPass::inject(Mod);
}

unsigned Pipeline::runBogusControlFlowPass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return BogusControlFlowPass::inject(Mod);
}

unsigned Pipeline::runIndirectBranchPass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return IndirectBranchPass::inject(Mod);
}

unsigned Pipeline::runIndirectCallPass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return IndirectCallPass::inject(Mod);
}

unsigned Pipeline::runMBAPass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return MBAPass::inject(Mod);
}

unsigned Pipeline::runIndirectGlobalPass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return IndirectGlobalPass::inject(Mod);
}

unsigned Pipeline::runValueLaunderingPass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return ValueLaunderingPass::inject(Mod);
}

unsigned Pipeline::runConstantPoolingPass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return ConstantPoolingPass::inject(Mod);
}

unsigned Pipeline::runBitMaskingPass(llvm::Module &Mod) {
  // Like runInstSubstitutionPass: inject() avoids PassManager templates.
  return BitMaskingPass::inject(Mod);
}

Pipeline::ObfuscationCounts
Pipeline::runObfuscationPasses(llvm::Module &Mod,
                               const ObfuscationConfig &Cfg) {
  // Single source of truth for the obfuscation pass order; keep in lockstep
  // with the toggle list in Pipeline::ObfuscationConfig.
  ObfuscationCounts C;
  if (Cfg.InstSubstitution)
    C.Substitution = runInstSubstitutionPass(Mod, Cfg.InstSubstitutionRounds);
  if (Cfg.ConstantEncryption)
    C.ConstEnc = runConstantEncryptionPass(Mod);
  if (Cfg.OpaquePredicate)
    C.OpaquePred = runOpaquePredicatePass(Mod);
  if (Cfg.BogusControlFlow)
    C.Bogus = runBogusControlFlowPass(Mod);
  if (Cfg.ControlFlowFlattening)
    C.Flatten = runControlFlowFlatteningPass(Mod);
  if (Cfg.IndirectBranch)
    C.IndirectBranch = runIndirectBranchPass(Mod);
  if (Cfg.IndirectCall)
    C.IndirectCall = runIndirectCallPass(Mod);
  if (Cfg.MBA)
    C.MBA = runMBAPass(Mod);
  if (Cfg.IndirectGlobal)
    C.IndirectGlobal = runIndirectGlobalPass(Mod);
  if (Cfg.ValueLaunder)
    C.ValueLaunder = runValueLaunderingPass(Mod);
  if (Cfg.ConstantPooling)
    C.ConstPool = runConstantPoolingPass(Mod);
  if (Cfg.BitMasking)
    C.BitMask = runBitMaskingPass(Mod);

  // Once the obfuscator has actually rewritten something, stamp every
  // definition so a later SymSimplifyPass skips this module.  That pass
  // measures away exactly the mixed boolean-arithmetic these passes inject, so
  // without the stamp an obfuscate-then-patch run through a shared pipeline
  // could undo its own payload.  The stamp is module-wide on purpose: which
  // functions a pass touched is not tracked here, and over-skipping the
  // simplifier is free (the patch path does not run it anyway) while
  // under-skipping would let it unpick the obfuscation.
  if (C.total() > 0)
    for (llvm::Function &F : Mod)
      if (!F.isDeclaration())
        F.addFnAttr(kObfuscatedFnAttr);
  return C;
}

} // namespace neverd
