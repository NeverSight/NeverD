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

#include "neverd/Common.h"
#include "neverd/pass/ir/obf/BitMaskingPass.h"
#include "neverd/pass/ir/obf/BogusControlFlowPass.h"
#include "neverd/pass/ir/obf/ConstantEncryptionPass.h"
#include "neverd/pass/ir/obf/ConstantPoolingPass.h"
#include "neverd/pass/ir/obf/ControlFlowFlatteningPass.h"
#include "neverd/pass/ir/HelloWorldPass.h"
#include "neverd/pass/ir/obf/IndirectBranchPass.h"
#include "neverd/pass/ir/obf/IndirectCallPass.h"
#include "neverd/pass/ir/obf/IndirectGlobalPass.h"
#include "neverd/pass/ir/obf/InstSubstitutionPass.h"
#include "neverd/pass/ir/obf/MBAPass.h"
#include "neverd/pass/ir/obf/OpaquePredicatePass.h"
#include "neverd/pass/ir/simplify/ControlFlowRecoveryPass.h"
#include "neverd/pass/ir/simplify/SymSimplifyPass.h"
#include "neverd/pass/ir/obf/ValueLaunderingPass.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include <string>
#include <utility>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

namespace {

/// Runs canonicalization and semantic measurement to their joint fixed point.
///
/// Neither transform reaches that point alone.  An expression mixing `+ - *`
/// with `& | ^ ~` is a fixed point for every rule-driven simplifier -- being
/// immune to peephole rewriting is what the obfuscation is for -- so
/// InstCombine stops in front of it.  The measurement walks straight through
/// it, but what it leaves behind is ordinary arithmetic that wants folding, and
/// folding it can expose a further region in measurable shape.  Running the
/// pair once therefore reaches neither transform's fixed point; running them in
/// alternation until a measurement finds nothing left to shorten reaches both.
///
/// Termination does not rest on the round cap.  A measurement is only written
/// back when it materializes strictly fewer instructions than it replaces, so
/// every productive round strictly shrinks the function and there can only be
/// finitely many.  The structural fingerprint below stops the one case that
/// argument does not cover -- the two transforms trading the same edit back and
/// forth.  There is no round limit to make an otherwise reachable fixed point
/// depend on how many layers happened to expose one another.
class SemanticFixedPointPass
    : public llvm::PassInfoMixin<SemanticFixedPointPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);

private:
  /// An exact snapshot used only after a productive round.  Stopping on a hash
  /// collision would make reachability of the fixed point probabilistic; text
  /// equality costs more, but a repeated state is then an actual cycle.
  static std::string snapshot(const llvm::Function &F);
};

std::string SemanticFixedPointPass::snapshot(const llvm::Function &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  F.print(OS);
  return Text;
}

llvm::PreservedAnalyses
SemanticFixedPointPass::run(llvm::Function &F,
                            llvm::FunctionAnalysisManager &FAM) {
  // One manager, run repeatedly: InstCombine carries no state across runs, and
  // rebuilding it every round would be the only cost this loop adds to a
  // function that has nothing to measure.
  llvm::FunctionPassManager Canonicalize;
  Canonicalize.addPass(llvm::InstCombinePass());

  llvm::SmallVector<std::string, 8> Seen;
  bool Changed = false;
  bool ResidueUnfolded = false;

  for (;;) {
    // This wrapper is itself a pass and must report changes made by the nested
    // canonicalizer even when semantic measurement finds nothing afterwards.
    // Returning `all()` after InstCombine changed the function would let a
    // caller keep analyses computed for the old IR.
    Changed |= !Canonicalize.run(F, FAM).areAllPreserved();
    ResidueUnfolded = false;

    unsigned Rewritten = SymSimplifyPass::simplify(F);
    if (Rewritten == 0)
      break;
    Changed = true;
    // The canonicalization that just ran folded the previous round's result;
    // this round's has not been folded yet, so a loop that stops here owes one
    // more run of InstCombine.
    ResidueUnfolded = true;

    // simplify() rewrote the function outside any pass manager, so everything
    // cached about it is stale before the next InstCombine reads it.
    FAM.invalidate(F, llvm::PreservedAnalyses::none());

    std::string State = snapshot(F);
    if (llvm::is_contained(Seen, State))
      break;
    Seen.push_back(std::move(State));
  }

  if (ResidueUnfolded)
    Changed |= !Canonicalize.run(F, FAM).areAllPreserved();

  return Changed ? llvm::PreservedAnalyses::none()
                 : llvm::PreservedAnalyses::all();
}

} // namespace

/// After SROA, bare negative inttoptr addresses (e.g.
/// `inttoptr (i32 -4 to ptr)`) can appear for stack slots that were
/// previously frame-relative.  Rewrite them to `frame_base + disp`.
static void fixLiftedStackPointers(llvm::Module &Mod) {
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
          FixIntToPtr(LI, llvm::LoadInst::getPointerOperandIndex());
        else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I))
          FixIntToPtr(SI, llvm::StoreInst::getPointerOperandIndex());
      }
    }
  }
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

void Pipeline::optimizeModule(llvm::Module &Mod, bool Conservative) {
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

  llvm::FunctionPassManager FPM;
  if (!Conservative) {
    // Before promotion, because that is where a dispatcher's state lives: both
    // a lifted function and a flattened one keep everything in stack slots, the
    // second because no block dominates another once every one of them is
    // reached through the switch.  Recovering the graph first is what lets the
    // promotion below put those values back in registers with the phis the real
    // control flow calls for, instead of phis at the dispatcher.
    FPM.addPass(ControlFlowRecoveryPass());
  }
  FPM.addPass(llvm::PromotePass());
  FPM.addPass(llvm::SROAPass(llvm::SROAOptions::PreserveCFG));
  if (!Conservative) {
    // InstCombine canonicalizes by shape, which is what a measurement reads
    // best; the measurement collapses the mixed boolean-arithmetic InstCombine
    // cannot touch; the arithmetic that leaves is what InstCombine folds best.
    // They are driven in alternation rather than paired once, because each
    // shortened layer can put a further one into measurable shape.
    FPM.addPass(SemanticFixedPointPass());
    FPM.addPass(llvm::SimplifyCFGPass());
  }
  llvm::ModulePassManager MPM;
  MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
  MPM.run(Mod, MAM);

  fixLiftedStackPointers(Mod);

  [[maybe_unused]] size_t Count = 0;
  for (auto &F : Mod)
    if (!F.isDeclaration())
      ++Count;

  LLVM_DEBUG(llvm::dbgs() << "pipeline: LLVM optimization applied (" << Count
                          << " functions, conservative=" << Conservative
                          << ")\n");
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
