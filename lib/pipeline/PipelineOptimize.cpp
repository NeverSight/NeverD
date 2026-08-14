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

#include "SemanticConvergence.h"

#include "neverd/Common.h"
#include "neverd/backend/ExceptionRewriteContract.h"
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
#include "neverd/pass/ir/simplify/ControlFlowRecoveryPass.h"
#include "neverd/pass/ir/simplify/SymSimplifyPass.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/StructuralHash.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include <limits>
#include <memory>
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

/// After SROA, bare negative inttoptr addresses (e.g.
/// `inttoptr (i32 -4 to ptr)`) can appear for stack slots that were
/// previously frame-relative.  Rewrite them to `frame_base + disp`.
static bool fixLiftedStackPointers(llvm::Module &Mod) {
  bool Changed = false;
  for (llvm::Function &F : Mod) {
    if (F.isDeclaration())
      continue;

    llvm::Value *FrameBase = nullptr;
    for (llvm::BasicBlock &BB : F) {
      if (!BB.getName().starts_with(kFrameSetupBlock))
        continue;
      for (llvm::Instruction &I : BB) {
        if (auto *Trunc = llvm::dyn_cast<llvm::TruncInst>(&I)) {
          if (auto *PTI =
                  llvm::dyn_cast<llvm::PtrToIntInst>(Trunc->getOperand(0)))
            if (PTI->getName() == kRspInitValue)
              FrameBase = Trunc;
        }
        if (!FrameBase) {
          if (auto *PTI = llvm::dyn_cast<llvm::PtrToIntInst>(&I))
            if (PTI->getName() == kRspInitValue)
              FrameBase = PTI;
        }
      }
    }
    if (!FrameBase)
      continue;

    llvm::LLVMContext &Ctx = F.getContext();
    for (llvm::BasicBlock &BB : F) {
      for (llvm::Instruction &I : BB) {
        auto FixIntToPtr = [&](llvm::Instruction *User, unsigned OpNo) -> bool {
          llvm::Value *PtrOp = User->getOperand(OpNo);
          llvm::Value *Addr = PtrOp;
          if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(PtrOp)) {
            if (CE->getOpcode() != llvm::Instruction::IntToPtr)
              return false;
            Addr = CE->getOperand(0);
          }
          auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Addr);
          if (!CI)
            return false;
          int64_t Disp = CI->getSExtValue();
          if (Disp >= 0)
            return false;

          llvm::IRBuilder<> FixB(User);
          llvm::Value *Base = FrameBase;
          if (Base->getType() != CI->getType())
            Base = FixB.CreateIntCast(Base, CI->getType(), true, "sp_cast");
          llvm::Value *Off = llvm::ConstantInt::get(
              CI->getType(), static_cast<uint64_t>(Disp), true);
          llvm::Value *PtrInt = FixB.CreateAdd(Base, Off);
          llvm::Value *Fixed = FixB.CreateIntToPtr(
              PtrInt, llvm::PointerType::get(Ctx, 0), "stackptr");
          User->setOperand(OpNo, Fixed);
          return true;
        };

        if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I))
          Changed |= FixIntToPtr(LI, llvm::LoadInst::getPointerOperandIndex());
        else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I))
          Changed |= FixIntToPtr(SI, llvm::StoreInst::getPointerOperandIndex());
      }
    }
  }
  return Changed;
}

void Pipeline::promoteScaffoldingAllocas(llvm::Module &Mod) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // mem2reg only: promotes the per-temp allocas the emitter spills SSA values
  // through (their addresses are never taken) and leaves the address-taken
  // frame alloca in memory.  No SROA / InstCombine / SimplifyCFG, so no
  // value-changing transform runs and fixLiftedStackPointers (an SROA cleanup)
  // is unneeded.
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
  llvm::PassBuilder PB;

  if (!Options.Conservative &&
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
  if (!Options.Conservative) {
    // Before promotion, because that is where a dispatcher's state lives: both
    // a lifted function and a flattened one keep everything in stack slots, the
    // second because no block dominates another once every one of them is
    // reached through the switch.  Recovering the graph first is what lets the
    // promotion below put those values back in registers with the phis the real
    // control flow calls for, instead of phis at the dispatcher.
    Prefix.addPass(ControlFlowRecoveryPass());
  }
  Prefix.addPass(llvm::PromotePass());
  Prefix.addPass(llvm::SROAPass(llvm::SROAOptions::PreserveCFG));
  llvm::ModulePassManager PrefixPipeline;
  PrefixPipeline.addPass(
      llvm::createModuleToFunctionPassAdaptor(std::move(Prefix)));
  llvm::PreservedAnalyses PrefixPA = PrefixPipeline.run(Mod, MAM);
  Result->Changed |= !PrefixPA.areAllPreserved();

  if (!Options.Conservative) {
    if (Options.Strength == Pipeline::OptStrength::Thin) {
      llvm::FunctionPassManager Thin;
      Thin.addPass(
          SemanticFixedPointPass(Options.MaxRounds, Options.Semantic, Result));
      Thin.addPass(llvm::SimplifyCFGPass());
      llvm::ModulePassManager ThinPipeline;
      ThinPipeline.addPass(
          llvm::createModuleToFunctionPassAdaptor(std::move(Thin)));
      llvm::PreservedAnalyses ThinPA = ThinPipeline.run(Mod, MAM);
      Result->Changed |= !ThinPA.areAllPreserved();
    } else {
      llvm::ModulePassManager DefaultPipeline =
          Options.LLVMLevel == llvm::OptimizationLevel::O0
              ? PB.buildO0DefaultPipeline(Options.LLVMLevel)
              : PB.buildPerModuleDefaultPipeline(Options.LLVMLevel);
      llvm::PreservedAnalyses DefaultPA = DefaultPipeline.run(Mod, MAM);
      Result->Changed |= !DefaultPA.areAllPreserved();
    }
  }

  Result->Changed |= fixLiftedStackPointers(Mod);
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

OptimizationResult
Pipeline::optimizeModule(llvm::Module &Mod,
                         const OptimizationOptions &Options) {
  OptimizationResult Result;
  if (!Options.Conservative && Options.Strength != OptStrength::Thin &&
      Options.Strength != OptStrength::Deep) {
    Result.Stop = OptimizationStopReason::InputInvalid;
    return Result;
  }

  if (llvm::verifyModule(Mod)) {
    Result.Stop = OptimizationStopReason::InputInvalid;
    return Result;
  }

  auto Contracts = exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!Contracts) {
    llvm::consumeError(Contracts.takeError());
    Result.Stop = OptimizationStopReason::InputInvalid;
    return Result;
  }

  const auto InputHash = llvm::StructuralHash(Mod, /*DetailedHash=*/true);
  std::unique_ptr<llvm::Module> Candidate = llvm::CloneModule(Mod);
  Result = runOptimizationPipeline(*Candidate, Options);
  if (llvm::verifyModule(*Candidate)) {
    Result.Changed = false;
    Result.Stop = OptimizationStopReason::VerificationFailed;
    return Result;
  }

  auto CandidateContracts =
      exception_rewrite::validateExceptionRewriteContracts(*Candidate);
  if (!CandidateContracts) {
    llvm::consumeError(CandidateContracts.takeError());
    Result.Changed = false;
    Result.Stop = OptimizationStopReason::VerificationFailed;
    return Result;
  }

  if (Options.PostTransformVerifier &&
      !Options.PostTransformVerifier(*Candidate)) {
    Result.Changed = false;
    Result.Stop = OptimizationStopReason::VerificationFailed;
    return Result;
  }

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
  Mod = std::move(*Candidate);
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
