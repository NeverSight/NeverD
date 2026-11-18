//===- MedLLVMNativeSEH.cpp - Native SEH lowering -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native LLVM lowering for supported Windows SEH functions.
///
//===----------------------------------------------------------------------===//

#include "MedLLVMEHHelpers.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

using med_llvm_eh::mdUInt;

namespace {

/// Whether \p Lifted, a callback lifted for printed C, takes a prefix of what
/// the runtime passes as \p Expected names it: a callback lifts with the
/// arguments it reads.  The abnormal-termination flag is an integer, the
/// exception pointers and the establisher frame pointers or pointer-wide
/// integers.  The dispositions a filter returns are integers.
bool hasSourceSEHCallbackShape(const llvm::FunctionType &Lifted,
                               const llvm::FunctionType &Expected,
                               unsigned PointerBits) {
  const bool Filter = Expected.getParamType(0)->isPointerTy();
  if (Lifted.isVarArg() || Lifted.getNumParams() > 2 ||
      (Filter && !Lifted.getReturnType()->isIntegerTy()))
    return false;
  for (unsigned I = 0; I < Lifted.getNumParams(); ++I) {
    llvm::Type *Param = Lifted.getParamType(I);
    if (Filter || I == 1
            ? !Param->isPointerTy() && !Param->isIntegerTy(PointerBits)
            : !Param->isIntegerTy() || Param->getIntegerBitWidth() > 64)
      return false;
  }
  return true;
}

bool hasSupportedSEHCallbackABI(const llvm::Function &Callback,
                                const llvm::FunctionType *ExpectedType,
                                Arch TargetArch, bool SourceView) {
  if (SourceView) {
    if (!hasSourceSEHCallbackShape(*Callback.getFunctionType(), *ExpectedType,
                                   getTargetRegInfo(TargetArch).PointerSize *
                                       8))
      return false;
  } else if (TargetArch != Arch::X64 ||
             Callback.getFunctionType() != ExpectedType) {
    return false;
  }
  if (Callback.getCallingConv() != llvm::CallingConv::C)
    return false;

  // Lifted image functions use ordinary external linkage.  Source-produced
  // outlined helpers commonly use local definitions.  Discardable, weak, and
  // available-externally bodies cannot represent an address-backed callback
  // with stable identity.
  return Callback.hasExternalLinkage() ||
         (Callback.hasLocalLinkage() && !Callback.isDeclaration());
}

/// \p MayNotReturn admits a callback that never returns: a `__finally` that
/// leaves by `longjmp` or ends the program.  The printed C calls it and says
/// the path ends there; native output, whose clauses are written again from
/// what the optimizer leaves, does not take one.
bool canPreserveSEHCallbackAcrossOptimization(const llvm::Function &Callback,
                                              bool MayNotReturn = false) {
  if (Callback.hasFnAttribute(llvm::Attribute::AlwaysInline) ||
      (!MayNotReturn && Callback.hasFnAttribute(llvm::Attribute::NoReturn)) ||
      Callback.hasFnAttribute(llvm::Attribute::ReturnsTwice) ||
      Callback.hasFnAttribute(llvm::Attribute::Speculatable))
    return false;
  if (Callback.doesNotThrow() && Callback.onlyReadsMemory() &&
      (Callback.willReturn() ||
       Callback.hasFnAttribute(llvm::Attribute::MustProgress)))
    return false;
  if (Callback.isDeclaration())
    return true;
  return !Callback.hasFnAttribute(llvm::Attribute::OptimizeForSize) &&
         !Callback.hasFnAttribute(llvm::Attribute::MinSize) &&
         !Callback.hasFnAttribute(llvm::Attribute::OptimizeForDebugging);
}

/// Whether \p Callback, a Windows x64 termination handler or filter lifted
/// from the image, reads the EstablisherFrame the runtime passes it in
/// \p FrameRegister: a value derived from that register's entry value reaches
/// memory, a call, a branch or the return.  A prologue save keeps the register
/// for the epilogue, and emission leaves it out (MedOp::PrologueSave).
bool sehCallbackReadsFrame(const MedFunc &Callback, uint64_t FrameRegister) {
  using Key = std::pair<int, int>;
  auto KeyOf = [](const MedVar &Value) { return Key{Value.Id, Value.SSAVer}; };
  std::set<Key> Defined;
  for (const MedBlock &Block : Callback.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      Defined.insert(KeyOf(Phi.Output));
    for (const MedOp &Op : Block.Ops) {
      if (Op.Dead || Op.Output.isConst() || !Op.Output.Size ||
          (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
           Op.Inputs[0] == Op.Output))
        continue;
      Defined.insert(KeyOf(Op.Output));
      for (const MedVar &Output : Op.IntrinsicOutputs)
        Defined.insert(KeyOf(Output));
    }
  }
  auto IsFrame = [&](const MedVar &Value) {
    if (Value.isConst() || Value.RegOff != FrameRegister)
      return false;
    if (Value.Kind == MedVar::Param)
      return true;
    return Value.Kind == MedVar::Reg && Value.SSAVer == 0 &&
           !Defined.count(KeyOf(Value));
  };
  auto Observes = [](NdOp Opcode) {
    switch (Opcode) {
    case NdOp::LOAD:
    case NdOp::STORE:
    case NdOp::ATOMIC_XCHG:
    case NdOp::ATOMIC_ADD:
    case NdOp::ATOMIC_CMPXCHG:
    case NdOp::INTRINSIC:
    case NdOp::BRANCH:
    case NdOp::COND_BR:
    case NdOp::INDIR_BR:
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
    case NdOp::RETURN:
      return true;
    default:
      return false;
    }
  };
  std::map<Key, std::vector<const MedOp *>> OpUsers;
  std::map<Key, std::vector<const PhiNode *>> PhiUsers;
  std::set<Key> Derived;
  std::vector<Key> Work;
  auto Note = [&](const MedVar &Value) {
    if (IsFrame(Value) && Derived.insert(KeyOf(Value)).second)
      Work.push_back(KeyOf(Value));
  };
  for (const MedBlock &Block : Callback.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &Arm : Phi.Args)
        if (!Arm.second.isConst()) {
          PhiUsers[KeyOf(Arm.second)].push_back(&Phi);
          Note(Arm.second);
        }
    for (const MedOp &Op : Block.Ops) {
      if (Op.Dead)
        continue;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst()) {
          OpUsers[KeyOf(Op.Inputs[I])].push_back(&Op);
          Note(Op.Inputs[I]);
        }
    }
  }
  while (!Work.empty()) {
    const Key Value = Work.back();
    Work.pop_back();
    for (const MedOp *Op : OpUsers[Value]) {
      if (Observes(Op->Opcode)) {
        if (Op->PrologueSave && !Callback.RegistrationStates)
          continue;
        return true;
      }
      if (!Op->Output.isConst() && Op->Output.Size &&
          Derived.insert(KeyOf(Op->Output)).second)
        Work.push_back(KeyOf(Op->Output));
    }
    for (const PhiNode *Phi : PhiUsers[Value])
      if (Derived.insert(KeyOf(Phi->Output)).second)
        Work.push_back(KeyOf(Phi->Output));
  }
  return false;
}

} // namespace

bool MedLLVMEmitter::sehCallbackReadsOnlyArguments(va_t Entry) {
  const auto [Memo, Inserted] =
      SEHCallbackReadsOnlyArguments.try_emplace(Entry, false);
  if (!Inserted)
    return Memo->second;
  const auto Body = SEHCallbackBodies.find(Entry);
  if (Body == SEHCallbackBodies.end() || Body->second->Blocks.empty())
    return false;
  const MedFunc &Callback = *Body->second;
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  std::set<uint64_t> Parameters;
  for (const MedVar &Param : Callback.Params)
    if ((Param.Kind == MedVar::Param || Param.Kind == MedVar::Reg) &&
        Param.RegOff != kNoParamReg)
      Parameters.insert(Param.RegOff);
  // What the callback holds on entry besides its arguments, its stack and its
  // return address is the parent's, restored by the runtime.  It reads that
  // state only where a value derived from it reaches memory, a call, a branch
  // or the return; a seed or a copy nothing uses reads nothing.  A prologue
  // save only keeps a callee-saved register for its epilogue, and emission
  // leaves it out (MedOp::PrologueSave).
  using Key = std::pair<int, int>;
  auto KeyOf = [](const MedVar &Value) { return Key{Value.Id, Value.SSAVer}; };
  // A register at version 0 is an entry value only where the callback does
  // not define it, as emission reads it (MedLLVMEmitter::getVar).  The
  // identity copy that seeds an entry value defines nothing.
  std::set<Key> Defined;
  for (const MedBlock &Block : Callback.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      Defined.insert(KeyOf(Phi.Output));
    for (const MedOp &Op : Block.Ops) {
      if (Op.Dead || Op.Output.isConst() || !Op.Output.Size ||
          (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
           Op.Inputs[0] == Op.Output))
        continue;
      Defined.insert(KeyOf(Op.Output));
      for (const MedVar &Output : Op.IntrinsicOutputs)
        Defined.insert(KeyOf(Output));
    }
  }
  auto Parent = [&](const MedVar &Value) {
    if (Value.Kind == MedVar::Unspecified)
      return true;
    if (Value.Kind != MedVar::Reg || Value.SSAVer != 0 ||
        Defined.count(KeyOf(Value)) || Parameters.count(Value.RegOff) ||
        TRI.isStackPointer(Value.RegOff))
      return false;
    return !TRI.isLinkRegister(Value.RegOff) || Value.Size != TRI.PointerSize;
  };
  auto Observes = [](NdOp Opcode) {
    switch (Opcode) {
    case NdOp::LOAD:
    case NdOp::STORE:
    case NdOp::ATOMIC_XCHG:
    case NdOp::ATOMIC_ADD:
    case NdOp::ATOMIC_CMPXCHG:
    case NdOp::INTRINSIC:
    case NdOp::BRANCH:
    case NdOp::COND_BR:
    case NdOp::INDIR_BR:
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
    case NdOp::RETURN:
      return true;
    default:
      return false;
    }
  };
  std::map<Key, std::vector<const MedOp *>> OpUsers;
  std::map<Key, std::vector<const PhiNode *>> PhiUsers;
  std::set<Key> Derived;
  std::vector<Key> Work;
  auto Note = [&](const MedVar &Value) {
    if (!Value.isConst() && Parent(Value) &&
        Derived.insert(KeyOf(Value)).second)
      Work.push_back(KeyOf(Value));
  };
  for (const MedBlock &Block : Callback.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &Arm : Phi.Args)
        if (!Arm.second.isConst()) {
          PhiUsers[KeyOf(Arm.second)].push_back(&Phi);
          Note(Arm.second);
        }
    for (const MedOp &Op : Block.Ops) {
      if (Op.Dead)
        continue;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst()) {
          OpUsers[KeyOf(Op.Inputs[I])].push_back(&Op);
          Note(Op.Inputs[I]);
        }
    }
  }
  while (!Work.empty()) {
    const Key Value = Work.back();
    Work.pop_back();
    for (const MedOp *Op : OpUsers[Value]) {
      if (Observes(Op->Opcode)) {
        if (Op->PrologueSave && !Callback.RegistrationStates)
          continue;
        return false;
      }
      if (!Op->Output.isConst() && Op->Output.Size &&
          Derived.insert(KeyOf(Op->Output)).second)
        Work.push_back(KeyOf(Op->Output));
    }
    for (const PhiNode *Phi : PhiUsers[Value])
      if (Derived.insert(KeyOf(Phi->Output)).second)
        Work.push_back(KeyOf(Phi->Output));
  }
  Memo->second = true;
  return true;
}

bool MedLLVMEmitter::emitNativeSEH(
    const MedFunc &Func, llvm::Function &LLVMFunc,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
  if (!Func.ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  // Printed C calls a callback with the arguments it reads.  ARM and AArch64
  // also hand a callback the parent's callee-saved registers, which a call
  // cannot pass, so there a callback must read nothing else of the parent.
  const bool SourceView = AnalysisRecoveryForSource;
  const bool RestoresRegisters =
      TargetArch == Arch::ARM || TargetArch == Arch::AArch64;
  const WindowsEHNativeSourceClassification Source =
      classifyWindowsEHNativeSource(
          EH, TargetArch, TargetFormat,
          SourceView ? WindowsEHNativeCapability::SourceIRLowering
                     : WindowsEHNativeCapability::IRLowering);
  if (!Source.canLowerNativeIR() ||
      Source.Model != WindowsEHNativeSourceModel::SEH)
    return false;

  if (!med_llvm_eh::collectExactSourceCallAddresses(LLVMFunc, CallSiteAddrs))
    return false;
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *PersonalityTy = llvm::FunctionType::get(I32Ty, {}, true);
  if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
          *Mod, "__C_specific_handler", PersonalityTy))
    return false;
  const med_llvm_eh::I32ModuleFlagState AsyncFlagState =
      med_llvm_eh::classifyI32ModuleFlag(*Mod, "eh-asynch",
                                         llvm::Module::Warning, 1);
  if (AsyncFlagState == med_llvm_eh::I32ModuleFlagState::Conflict)
    return false;

  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  auto *FilterCallbackTy =
      llvm::FunctionType::get(I32Ty, {PtrTy, PtrTy}, false);
  auto *FinallyCallbackTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(*Ctx), {llvm::Type::getInt8Ty(*Ctx), PtrTy}, false);

  struct Region {
    const SEHScopeRecord *Scope = nullptr;
    ExceptionAddressRange SemanticRange;
    llvm::BasicBlock *Handler = nullptr;
    llvm::Function *Callback = nullptr;
    llvm::BasicBlock *UnwindDest = nullptr;
    int Parent = -1;
    std::set<llvm::BasicBlock *> Blocks;
    uint32_t SourceIndex = 0;
    llvm::mc_rewrite::RewriteWinEHSemanticToken SemanticToken;
  };

  auto FunctionAt = [&](va_t Address) -> llvm::Function * {
    return resolveLiftedFunctionEntry(Address);
  };
  // C prints a filter whose lifted body returns no value as returning void
  // (MedFunc::ReturnsNoValue), which leaves its __except no disposition.
  auto FilterReturnsValue = [&](va_t Entry) {
    const auto Body = SEHCallbackBodies.find(Entry);
    return Body == SEHCallbackBodies.end() || !Body->second->ReturnsNoValue;
  };
  auto BlockAt = [&](va_t Address) -> llvm::BasicBlock * {
    for (const MedBlock &Block : Func.Blocks) {
      if (Block.StartAddr != Address)
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      if (It == OriginalBlockMap.end() || !It->second ||
          It->second->getParent() != &LLVMFunc)
        return nullptr;
      return It->second;
    }
    return nullptr;
  };

  std::vector<Region> Regions;
  Regions.reserve(EH.SEH->Scopes.size());
  if (EH.SEH->Scopes.size() > std::numeric_limits<uint32_t>::max())
    return false;
  for (size_t ScopeIndex = 0; ScopeIndex < EH.SEH->Scopes.size();
       ++ScopeIndex) {
    const SEHScopeRecord &Scope = EH.SEH->Scopes[ScopeIndex];
    const std::optional<ExceptionAddressRange> SemanticRange =
        getSemanticSEHGuardedRange(Scope, TargetArch, EH.CodeRange);
    if (Scope.ParseStatus != ExceptionParseStatus::Complete || !SemanticRange)
      return false;

    Region R;
    R.Scope = &Scope;
    R.SemanticRange = *SemanticRange;
    R.SourceIndex = static_cast<uint32_t>(ScopeIndex);
    const auto SemanticToken = windows_eh_semantics::getSEHScopeSemanticToken(
        EH, TargetArch, R.SourceIndex, /*AllowRepeatedRanges=*/true,
        /*AllowCookieCheck=*/SourceView);
    if (!SemanticToken)
      return false;
    R.SemanticToken = *SemanticToken;
    if (Scope.Kind == SEHScopeKind::Finally) {
      R.Callback = FunctionAt(Scope.FilterOrFinallyVA);
      if (!R.Callback || R.Callback == &LLVMFunc ||
          !hasSupportedSEHCallbackABI(*R.Callback, FinallyCallbackTy,
                                      TargetArch, SourceView) ||
          !canPreserveSEHCallbackAcrossOptimization(
              *R.Callback, /*MayNotReturn=*/SourceView) ||
          (SourceView && RestoresRegisters &&
           !sehCallbackReadsOnlyArguments(Scope.FilterOrFinallyVA)))
        return false;
    } else {
      R.Handler = BlockAt(Scope.HandlerVA);
      if (!R.Handler || R.SemanticRange.contains(Scope.HandlerVA))
        return false;
      if (Scope.Kind == SEHScopeKind::Filter) {
        R.Callback = FunctionAt(Scope.FilterOrFinallyVA);
        if (!R.Callback || R.Callback == &LLVMFunc ||
            !hasSupportedSEHCallbackABI(*R.Callback, FilterCallbackTy,
                                        TargetArch, SourceView) ||
            !canPreserveSEHCallbackAcrossOptimization(*R.Callback) ||
            (SourceView && !FilterReturnsValue(Scope.FilterOrFinallyVA)) ||
            (SourceView && RestoresRegisters &&
             !sehCallbackReadsOnlyArguments(Scope.FilterOrFinallyVA)))
          return false;
      }
    }

    bool HasBegin = false;
    bool HasEnd = false;
    for (const MedBlock &Block : Func.Blocks) {
      ExceptionAddressRange BlockRange{Block.StartAddr, Block.EndAddr};
      if (!BlockRange.isValid())
        continue;
      if (R.SemanticRange.overlaps(BlockRange) &&
          !R.SemanticRange.contains(BlockRange))
        return false;
      if (!R.SemanticRange.contains(BlockRange))
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      if (It == OriginalBlockMap.end() || !It->second ||
          It->second->getParent() != &LLVMFunc)
        return false;
      R.Blocks.insert(It->second);
      HasBegin |= Block.StartAddr == R.SemanticRange.Begin;
      HasEnd |= Block.EndAddr == R.SemanticRange.End;
    }
    // Native output rebuilds the table, so its bounds must be block bounds.
    // Printed C protects the blocks: a bound in bytes no block holds (the
    // byte clang-cl ends a range at, past its last call's return address)
    // guards no code.
    if (R.Blocks.empty() || (!SourceView && (!HasBegin || !HasEnd)))
      return false;
    Regions.push_back(std::move(R));
  }

  // Native WinEH can express nested or disjoint intervals.  Crossing
  // intervals have no unwind-parent relation.  Rows over one interval nest by
  // their order, the earlier inside the later, because __C_specific_handler
  // scans the table from its first row.  Both routes take that order; the
  // table the code generator writes again repeats a range for each nesting
  // level, which output ties back to these scopes.
  for (size_t I = 0; I < Regions.size(); ++I) {
    for (size_t J = I + 1; J < Regions.size(); ++J) {
      const ExceptionAddressRange &A = Regions[I].SemanticRange;
      const ExceptionAddressRange &B = Regions[J].SemanticRange;
      if (!A.overlaps(B))
        continue;
      if (!A.contains(B) && !B.contains(A))
        return false;
    }
  }
  std::stable_sort(Regions.begin(), Regions.end(),
                   [](const Region &A, const Region &B) {
                     if (A.SemanticRange.size() != B.SemanticRange.size())
                       return A.SemanticRange.size() > B.SemanticRange.size();
                     if (A.SemanticRange.Begin != B.SemanticRange.Begin)
                       return A.SemanticRange.Begin < B.SemanticRange.Begin;
                     // Rows over one range: the later row is the outer scope.
                     return A.SourceIndex > B.SourceIndex;
                   });
  for (size_t I = 0; I < Regions.size(); ++I) {
    uint64_t ParentSize = std::numeric_limits<uint64_t>::max();
    for (size_t J = 0; J < I; ++J) {
      if (!Regions[J].SemanticRange.contains(Regions[I].SemanticRange))
        continue;
      // The nearest holder is the smallest, and of rows over one range the
      // one placed last.
      if (Regions[J].SemanticRange.size() <= ParentSize) {
        Regions[I].Parent = static_cast<int>(J);
        ParentSize = Regions[J].SemanticRange.size();
      }
    }
  }

  // Complete every check that can reject the lowering before creating the
  // personality, declarations, funclets, or CFG edges. The commit phase below
  // consumes only this immutable plan and has no failure exits.
  auto IsMayUnwindCall = [](const llvm::CallInst &Call) {
    return !Call.doesNotThrow() && !Call.isMustTailCall() &&
           !llvm::isa<llvm::IntrinsicInst>(Call);
  };
  struct CallPlan {
    llvm::CallInst *Call = nullptr;
    /// The region whose pad the call unwinds to; -1 for a termination handler
    /// call that no scope of this frame protects.
    int Region = -1;
    va_t SourceAddress = 0;
    /// The region a local unwind's termination handler call ends, which the
    /// call has left; -1 for any other call.
    int Ended = -1;
  };
  auto IsProperAncestor = [&](int Ancestor, int Of) {
    for (int At = Regions[static_cast<size_t>(Of)].Parent; At >= 0;
         At = Regions[static_cast<size_t>(At)].Parent)
      if (At == Ancestor)
        return true;
    return false;
  };
  // MedIR spells a local unwind as a call of each termination handler it
  // runs, at the runtime call's address (MedSEHLocalUnwindExit).  The runtime
  // calls the handler of row k with its scan already past k, so what the
  // handler raises meets only the later rows that guard the call: the call
  // stands outside scope k and inside those.  Both routes place it there;
  // native IR also marks each such call (LocalUnwindHandlerCall), which the
  // output gate places by the row it ends.
  std::map<va_t, const MedSEHLocalUnwindExit *> LocalUnwinds;
  for (const MedSEHLocalUnwindExit &Exit : Func.SEHLocalUnwindExits)
    if (!Exit.Handlers.empty() &&
        !LocalUnwinds.emplace(Exit.CallAddr, &Exit).second)
      return false;
  struct LocalUnwindProgress {
    size_t Next = 0;
    const llvm::BasicBlock *Block = nullptr;
    const llvm::CallInst *Last = nullptr;
  };
  std::map<const MedSEHLocalUnwindExit *, LocalUnwindProgress> LocalUnwindCalls;

  std::set<const llvm::CallInst *> ProtectedCalls;
  for (const Region &R : Regions)
    for (llvm::BasicBlock *Block : R.Blocks) {
      if (!Block || Block->getParent() != &LLVMFunc)
        return false;
      for (const llvm::Instruction &Inst : *Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
            Call && IsMayUnwindCall(*Call))
          ProtectedCalls.insert(Call);
    }

  llvm::SmallVector<CallPlan, 16> CallPlans;
  std::set<const llvm::CallInst *> PlannedCalls;
  std::set<va_t> PlannedAddresses;
  for (const MedBlock &MedBB : Func.Blocks) {
    auto BBIt = OriginalBlockMap.find(MedBB.Id);
    if (BBIt == OriginalBlockMap.end())
      continue;
    llvm::BasicBlock *InitialBB = BBIt->second;
    if (!InitialBB || InitialBB->getParent() != &LLVMFunc)
      return false;

    int Innermost = -1;
    uint64_t InnermostSize = std::numeric_limits<uint64_t>::max();
    for (size_t I = 0; I < Regions.size(); ++I) {
      if (!Regions[I].Blocks.count(InitialBB) ||
          Regions[I].SemanticRange.size() > InnermostSize)
        continue;
      Innermost = static_cast<int>(I);
      InnermostSize = Regions[I].SemanticRange.size();
    }
    if (Innermost < 0)
      continue;

    // The local unwind whose handler calls this block has reached, and the
    // last call met: an exit's calls follow one another to the block's end.
    const MedSEHLocalUnwindExit *Leaving = nullptr;
    const llvm::CallInst *Previous = nullptr;
    for (llvm::Instruction &Inst : *InitialBB) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
      if (!Call || !IsMayUnwindCall(*Call))
        continue;
      const llvm::CallInst *Before = std::exchange(Previous, Call);
      auto AddressIt = CallSiteAddrs.find(Call);
      if (Call->getParent() != InitialBB || !Call->getNextNode() ||
          AddressIt == CallSiteAddrs.end() ||
          !EH.CodeRange.contains(AddressIt->second) ||
          !PlannedCalls.insert(Call).second)
        return false;
      const va_t Address = AddressIt->second;
      const auto Unwind = LocalUnwinds.find(Address);
      if (Unwind == LocalUnwinds.end()) {
        if (Leaving || !PlannedAddresses.insert(Address).second)
          return false;
        CallPlans.push_back({Call, Innermost, Address, /*Ended=*/-1});
        continue;
      }
      const MedSEHLocalUnwindExit &Exit = *Unwind->second;
      LocalUnwindProgress &Progress = LocalUnwindCalls[&Exit];
      if ((Leaving && Leaving != &Exit) || PlannedAddresses.count(Address) ||
          Progress.Next >= Exit.Handlers.size() ||
          (Progress.Block && Progress.Block != InitialBB) ||
          (Progress.Last && Progress.Last != Before))
        return false;
      Leaving = &Exit;
      const MedSEHLocalUnwindExit::Handler &Handler =
          Exit.Handlers[Progress.Next];
      int Ended = -1;
      for (size_t I = 0; I < Regions.size(); ++I)
        if (Regions[I].SourceIndex == Handler.ScopeIndex)
          Ended = static_cast<int>(I);
      if (Ended < 0)
        return false;
      const Region &Scope = Regions[static_cast<size_t>(Ended)];
      if (Scope.Scope->Kind != SEHScopeKind::Finally ||
          Scope.Scope->FilterOrFinallyVA != Handler.Entry ||
          Call->getCalledFunction() != Scope.Callback ||
          !Scope.Blocks.count(InitialBB))
        return false;
      // The rows the scan has yet to meet are the scopes around the one this
      // handler ends, and no others.
      for (size_t I = 0; I < Regions.size(); ++I)
        if ((Regions[I].SourceIndex > Handler.ScopeIndex &&
             Regions[I].SemanticRange.contains(Exit.CallAddr)) !=
            IsProperAncestor(static_cast<int>(I), Ended))
          return false;
      Progress.Next += 1;
      Progress.Block = InitialBB;
      Progress.Last = Call;
      CallPlans.push_back({Call, Scope.Parent, Address, Ended});
    }
  }
  // Every call at a local unwind's address is one of its handler calls,
  // placed above, and an exit that was placed was placed whole.
  std::map<const MedSEHLocalUnwindExit *, size_t> LocalUnwindCallCounts;
  if (!LocalUnwinds.empty())
    for (const llvm::BasicBlock &Block : LLVMFunc)
      for (const llvm::Instruction &Inst : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
        if (!Call || !IsMayUnwindCall(*Call))
          continue;
        const auto AddressIt = CallSiteAddrs.find(Call);
        if (AddressIt == CallSiteAddrs.end())
          continue;
        if (const auto Unwind = LocalUnwinds.find(AddressIt->second);
            Unwind != LocalUnwinds.end())
          ++LocalUnwindCallCounts[Unwind->second];
      }
  for (const auto &[Address, Exit] : LocalUnwinds) {
    const auto Progress = LocalUnwindCalls.find(Exit);
    const size_t Placed =
        Progress == LocalUnwindCalls.end() ? 0 : Progress->second.Next;
    if (LocalUnwindCallCounts[Exit] != Placed ||
        (Placed != 0 && Placed != Exit->Handlers.size()))
      return false;
  }
  if (PlannedCalls != ProtectedCalls)
    return false;
  const uint64_t ProtectedCallCount = static_cast<uint64_t>(llvm::count_if(
      CallPlans, [](const CallPlan &Plan) { return Plan.Region >= 0; }));

  // The asynchronous marker pass walks every protected terminator and its
  // successors. Unterminated emitter intermediates are rejected atomically.
  for (const Region &R : Regions)
    for (llvm::BasicBlock *Block : R.Blocks)
      if (!Block->getTerminator())
        return false;

  llvm::FunctionCallee Personality =
      Mod->getOrInsertFunction("__C_specific_handler", PersonalityTy);
  LLVMFunc.setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));

  // Filter/finally callbacks are address-backed ABI objects, not ordinary
  // implementation details of the lifted parent.  Native reconstruction
  // authenticates the exact direct callee against the source scope table, so
  // allowing an optimizer to inline, summarize, or erase the body/call would
  // destroy information the output gate must retain.  optnone prevents a
  // trivial definition from acquiring a no-write/nounwind/willreturn summary
  // that makes an outer finally call dead; eh-asynch separately preserves the
  // nested invoke edge.  Add these only after the atomic preflight has
  // completed; the commit path below has no failure exits.
  for (const Region &R : Regions) {
    if (!R.Callback)
      continue;
    if (!R.Callback->hasFnAttribute(llvm::Attribute::NoInline))
      R.Callback->addFnAttr(llvm::Attribute::NoInline);
    if (!R.Callback->isDeclaration())
      R.Callback->addFnAttr(llvm::Attribute::OptimizeNone);
  }

  // A callback lifted from the image reads its parent's locals at the offsets
  // the original frame gave them, EstablisherFrame + k.  The lifted parent
  // keeps that frame: FrameAlloca, whose byte FrameEntrySPOffset stands for
  // the entry stack pointer.  Native output passes a callback that reads its
  // frame the address there of the frame the runtime gave the original, the
  // certified bytes below the entry stack pointer.  Any other callback keeps
  // llvm.localaddress(), which the output gate refuses for one that uses it.
  const uint64_t FrameArgumentRegister =
      TargetArch == Arch::X64
          ? getTargetRegInfo(TargetArch).integerParamRegs(TargetFormat)[1]
          : 0;
  auto EmulatedFrameOffset = [&](va_t Entry) -> std::optional<uint64_t> {
    if (SourceView || TargetArch != Arch::X64 || !FrameAlloca ||
        EH.FrameRegister != 0 ||
        FrameAlloca->getParent() != &LLVMFunc.getEntryBlock() ||
        !FrameAlloca->isStaticAlloca())
      return std::nullopt;
    const auto Bytes = Func.SEHTerminationFrameBytes.find(Entry);
    if (Bytes == Func.SEHTerminationFrameBytes.end() ||
        Bytes->second > FrameEntrySPOffset)
      return std::nullopt;
    const auto Body = SEHCallbackBodies.find(Entry);
    if (Body != SEHCallbackBodies.end() && !Body->second->Blocks.empty() &&
        !sehCallbackReadsFrame(*Body->second, FrameArgumentRegister))
      return std::nullopt;
    return FrameEntrySPOffset - Bytes->second;
  };
  bool UsesEmulatedFrame = false;

  auto *TokenNone = llvm::ConstantTokenNone::get(*Ctx);
  for (size_t I = 0; I < Regions.size(); ++I) {
    Region &R = Regions[I];
    llvm::BasicBlock *ParentDest =
        R.Parent >= 0 ? Regions[static_cast<size_t>(R.Parent)].UnwindDest
                      : nullptr;
    std::string Suffix = std::to_string(I);
    if (R.Scope->Kind == SEHScopeKind::Finally) {
      auto *PadBB = llvm::BasicBlock::Create(
          *Ctx, "seh.finally.dispatch." + Suffix, &LLVMFunc);
      llvm::IRBuilder<> B(PadBB);
      auto *Pad = B.CreateCleanupPad(TokenNone, {}, "seh.finally.pad");
      if (!med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, R.SemanticToken))
        llvm_unreachable("prevalidated SEH semantic token was rejected");
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, windows_eh_md::NativeProvenanceModel::SEH,
          windows_eh_md::NativeProvenanceRole::RegionDispatch,
          EH.CodeRange.Begin, /*SourceVA=*/0, R.SourceIndex, /*Clause=*/0, Pad,
          R.Scope->FilterOrFinallyVA, static_cast<uint32_t>(R.Scope->Kind));
      // A callback lifted for printed C takes the flag and the frame it reads,
      // at the widths it reads them; x64's has the runtime's own type.
      llvm::FunctionType *CallbackTy = R.Callback->getFunctionType();
      llvm::SmallVector<llvm::Value *, 2> Args;
      if (CallbackTy->getNumParams() > 0)
        Args.push_back(llvm::ConstantInt::get(CallbackTy->getParamType(0), 1));
      if (CallbackTy->getNumParams() > 1) {
        llvm::Value *Frame = nullptr;
        if (const auto Offset =
                EmulatedFrameOffset(R.Scope->FilterOrFinallyVA)) {
          Frame =
              B.CreateInBoundsGEP(llvm::Type::getInt8Ty(*Ctx), FrameAlloca,
                                  B.getInt64(*Offset), "seh.establisher.frame");
          UsesEmulatedFrame = true;
        } else
          Frame = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
              Mod, llvm::Intrinsic::localaddress));
        if (CallbackTy->getParamType(1)->isIntegerTy())
          Frame = B.CreatePtrToInt(Frame, CallbackTy->getParamType(1));
        Args.push_back(Frame);
      }
      llvm::SmallVector<llvm::Value *, 1> BundleInputs{Pad};
      llvm::OperandBundleDef Funclet("funclet", BundleInputs);
      // Printed C: a handler that leaves by a local unwind into this function
      // on every way out resumes it at one block. The pad unwinds to a catch
      // whose pad returns there, which keeps that block's code; LLVM C prints
      // a `goto` to it at the end of the `__finally` (HandlerResume).
      llvm::BasicBlock *Unwind = ParentDest;
      if (SourceView)
        if (const auto Resume =
                Func.SEHHandlerResumes.find(R.Scope->FilterOrFinallyVA);
            Resume != Func.SEHHandlerResumes.end())
          if (llvm::BasicBlock *ResumeAt = BlockAt(Resume->second)) {
            auto *ResumeBB = llvm::BasicBlock::Create(
                *Ctx, "seh.resume.dispatch." + Suffix, &LLVMFunc);
            llvm::IRBuilder<> RB(ResumeBB);
            auto *Resumes = RB.CreateCatchSwitch(TokenNone, ParentDest, 1,
                                                 "seh.resume.switch");
            Resumes->setMetadata(
                windows_eh_md::HandlerResume,
                llvm::MDTuple::get(
                    *Ctx, {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                              llvm::Type::getInt64Ty(*Ctx), Resume->second))}));
            auto *ResumePadBB = llvm::BasicBlock::Create(
                *Ctx, "seh.resume.pad." + Suffix, &LLVMFunc);
            Resumes->addHandler(ResumePadBB);
            llvm::IRBuilder<> RPB(ResumePadBB);
            auto *ResumePad =
                RPB.CreateCatchPad(Resumes,
                                   {llvm::ConstantPointerNull::get(
                                       llvm::cast<llvm::PointerType>(PtrTy))},
                                   "seh.resume.token");
            RPB.CreateCatchRet(ResumePad, ResumeAt);
            Unwind = ResumeBB;
          }
      if (ParentDest) {
        // Every unwind edge out of the pad goes where its cleanupret does.
        auto *ContinueBB = llvm::BasicBlock::Create(
            *Ctx, "seh.finally.continue." + Suffix, &LLVMFunc);
        B.CreateInvoke(CallbackTy, R.Callback, ContinueBB, Unwind, Args,
                       {Funclet});
        llvm::IRBuilder<> ContinueBuilder(ContinueBB);
        ContinueBuilder.CreateCleanupRet(Pad, Unwind);
      } else {
        B.CreateCall(CallbackTy, R.Callback, Args, {Funclet});
        B.CreateCleanupRet(Pad, Unwind);
      }
      R.UnwindDest = PadBB;
      continue;
    }

    auto *Dispatch = llvm::BasicBlock::Create(
        *Ctx, "seh.catch.dispatch." + Suffix, &LLVMFunc);
    llvm::IRBuilder<> DB(Dispatch);
    auto *Switch =
        DB.CreateCatchSwitch(TokenNone, ParentDest, 1, "seh.catch.switch");
    auto *PadBB =
        llvm::BasicBlock::Create(*Ctx, "seh.catch.pad." + Suffix, &LLVMFunc);
    Switch->addHandler(PadBB);
    llvm::IRBuilder<> PB(PadBB);
    llvm::Value *Filter =
        R.Scope->Kind == SEHScopeKind::CatchAll
            ? static_cast<llvm::Value *>(llvm::ConstantPointerNull::get(
                  llvm::cast<llvm::PointerType>(PtrTy)))
            : static_cast<llvm::Value *>(R.Callback);
    auto *Pad = PB.CreateCatchPad(Switch, {Filter}, "seh.catch.pad.token");
    // The handler reads the exception code the pad receives.
    llvm::Value *Code = nullptr;
    auto ExceptionCode = [&]() {
      if (!Code)
        Code = PB.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                                 Mod, llvm::Intrinsic::eh_exceptioncode),
                             {Pad});
      return Code;
    };
    if (auto Slot = SEHExceptionCodeSlots.find(R.Scope->HandlerVA);
        Slot != SEHExceptionCodeSlots.end())
      PB.CreateStore(ExceptionCode(), Slot->second);
    // A handler block ordinary flow also enters merges RAX in PHIs; this
    // entry supplies their exception-code incoming.
    if (CurMedFunc)
      for (const MedBlock &Block : CurMedFunc->Blocks)
        if (Block.StartAddr == R.Scope->HandlerVA)
          for (const PhiNode &Phi : Block.Phis)
            if (Phi.ExceptionalEntry)
              setVar(Phi.Output,
                     PB.CreateZExtOrTrunc(
                         ExceptionCode(),
                         llvm::Type::getIntNTy(*Ctx, Phi.Output.Size * 8)),
                     PB);
    if (!med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, R.SemanticToken))
      llvm_unreachable("prevalidated SEH semantic token was rejected");
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        PB, windows_eh_md::NativeProvenanceModel::SEH,
        windows_eh_md::NativeProvenanceRole::RegionDispatch, EH.CodeRange.Begin,
        R.Scope->HandlerVA, R.SourceIndex, /*Clause=*/0, Pad,
        R.Scope->Kind == SEHScopeKind::Filter ? R.Scope->FilterOrFinallyVA : 0,
        static_cast<uint32_t>(R.Scope->Kind));
    PB.CreateCatchRet(Pad, R.Handler);
    llvm::IRBuilder<> HandlerBuilder(&*R.Handler->getFirstInsertionPt());
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        HandlerBuilder, windows_eh_md::NativeProvenanceModel::SEH,
        windows_eh_md::NativeProvenanceRole::HandlerTarget, EH.CodeRange.Begin,
        R.Scope->HandlerVA, R.SourceIndex, /*Clause=*/0);
    R.UnwindDest = Dispatch;
  }

  // Replace each planned call with an invoke to its innermost active region.
  // Non-call hardware faults are covered by the asynchronous markers below.
  for (const CallPlan &Plan : CallPlans) {
    llvm::CallInst *Call = Plan.Call;
    if (Plan.Ended >= 0) {
      // The handler call and what follows leave the scope the handler ends
      // and every scope inside it.
      llvm::BasicBlock *Before = Call->getParent();
      llvm::BasicBlock *After = Before->splitBasicBlock(
          Call->getIterator(), Before->getName() + ".seh.unwind");
      for (size_t I = 0; I < Regions.size(); ++I)
        if (Regions[I].Blocks.count(Before) &&
            IsProperAncestor(static_cast<int>(I), Plan.Ended))
          Regions[I].Blocks.insert(After);
      // Native IR says which calls these are and which scope each ends, for
      // the output gate.
      if (!SourceView) {
        llvm::IRBuilder<> MarkBuilder(Call);
        med_llvm_eh::emitWindowsEHProvenanceAnchor(
            MarkBuilder, windows_eh_md::NativeProvenanceModel::SEH,
            windows_eh_md::NativeProvenanceRole::LocalUnwindHandlerCall,
            EH.CodeRange.Begin, Plan.SourceAddress,
            Regions[static_cast<size_t>(Plan.Ended)].SourceIndex,
            /*Clause=*/0);
      }
      if (Plan.Region < 0)
        continue;
    }
    llvm::BasicBlock *CallBB = Call->getParent();
    llvm::Instruction *Next = Call->getNextNode();
    assert(CallBB && Next && "preflighted SEH call changed before commit");
    llvm::BasicBlock *Cont =
        CallBB->splitBasicBlock(Next, CallBB->getName() + ".seh.cont");
    auto *OldBranch = CallBB->getTerminator();
    assert(OldBranch && "split block must have a branch");
    llvm::SmallVector<llvm::Value *, 8> Args;
    for (llvm::Use &Arg : Call->args())
      Args.push_back(Arg.get());
    llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
    Call->getOperandBundlesAsDefs(Bundles);
    auto *Invoke = llvm::InvokeInst::Create(
        Call->getFunctionType(), Call->getCalledOperand(), Cont,
        Regions[static_cast<size_t>(Plan.Region)].UnwindDest, Args, Bundles,
        Call->getName(), OldBranch->getIterator());
    llvm::IRBuilder<> ProvenanceBuilder(Invoke);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        ProvenanceBuilder, windows_eh_md::NativeProvenanceModel::SEH,
        windows_eh_md::NativeProvenanceRole::ProtectedInvoke,
        EH.CodeRange.Begin, Plan.SourceAddress,
        Regions[static_cast<size_t>(Plan.Region)].SourceIndex, /*Clause=*/0);
    Invoke->setCallingConv(Call->getCallingConv());
    Invoke->setAttributes(Call->getAttributes());
    Invoke->setDebugLoc(Call->getDebugLoc());
    Invoke->copyMetadata(*Call);
    Call->replaceAllUsesWith(Invoke);
    CallSiteAddrs.erase(Call);
    Call->eraseFromParent();
    OldBranch->eraseFromParent();
    for (Region &R : Regions)
      if (R.Blocks.count(CallBB))
        R.Blocks.insert(Cont);
  }

  llvm::Function *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_try_begin);
  llvm::Function *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_try_end);
  auto IsNormalSuccessor = [](llvm::Instruction *Term, unsigned Index) {
    if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Term))
      return Index == 0 && Invoke->getNormalDest();
    if (llvm::isa<llvm::CatchSwitchInst, llvm::CleanupReturnInst>(Term))
      return false;
    return true;
  };
  struct RangeTargetPlan {
    llvm::InvokeInst *Marker = nullptr;
    windows_eh_md::NativeProvenanceRole Role =
        windows_eh_md::NativeProvenanceRole::RangeEnterTarget;
    va_t SourceVA = 0;
    uint32_t Region = 0;
    uint32_t Boundary = 0;
  };
  llvm::SmallVector<RangeTargetPlan, 16> RangeTargets;
  auto PlanRangeTarget =
      [&](llvm::InvokeInst *Marker, windows_eh_md::NativeProvenanceRole Role,
          va_t SourceVA, uint32_t Region, uint32_t Boundary) {
        RangeTargets.push_back({Marker, Role, SourceVA, Region, Boundary});
      };

  // A region's block set iterates in address order, which varies from run to
  // run. Marker names, boundary provenance and the blocks appended for
  // returns follow the layout instead. Outer regions are placed first, so a
  // region's set holds only these blocks when it is placed.
  llvm::DenseMap<const llvm::BasicBlock *, size_t> LayoutIndex;
  for (const llvm::BasicBlock &BB : LLVMFunc)
    LayoutIndex.try_emplace(&BB, LayoutIndex.size());

  // Outer-first placement produces the required nesting order on a shared
  // boundary: outer.begin -> inner.begin -> body -> inner.end -> outer.end.
  for (size_t RegionIndex = 0; RegionIndex < Regions.size(); ++RegionIndex) {
    Region &R = Regions[RegionIndex];
    llvm::SmallVector<llvm::BasicBlock *, 16> Blocks(R.Blocks.begin(),
                                                     R.Blocks.end());
    llvm::sort(Blocks,
               [&](const llvm::BasicBlock *A, const llvm::BasicBlock *B) {
                 return LayoutIndex.lookup(A) < LayoutIndex.lookup(B);
               });
    struct EntryEdge {
      llvm::BasicBlock *Pred = nullptr;
      llvm::BasicBlock *Target = nullptr;
      unsigned SuccessorIndex = 0;
    };
    llvm::SmallVector<EntryEdge, 8> Entries;
    bool EntryWithoutPred = false;
    llvm::BasicBlock *FunctionEntry = &LLVMFunc.getEntryBlock();
    for (llvm::BasicBlock *Target : Blocks) {
      bool HasNormalPred = false;
      for (llvm::BasicBlock *Pred : llvm::predecessors(Target)) {
        llvm::Instruction *Term = Pred->getTerminator();
        for (unsigned I = 0; I < Term->getNumSuccessors(); ++I) {
          if (Term->getSuccessor(I) != Target || !IsNormalSuccessor(Term, I))
            continue;
          HasNormalPred = true;
          if (!R.Blocks.count(Pred))
            Entries.push_back({Pred, Target, I});
        }
      }
      EntryWithoutPred |= Target == FunctionEntry && !HasNormalPred;
    }

    unsigned Marker = 0;
    for (const EntryEdge &Edge : Entries) {
      const uint32_t Boundary = Marker++;
      auto *BeginBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.begin." + std::to_string(RegionIndex) + "." +
              std::to_string(Boundary),
          &LLVMFunc, Edge.Target);
      Edge.Pred->getTerminator()->setSuccessor(Edge.SuccessorIndex, BeginBB);
      llvm::IRBuilder<> B(BeginBB);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, windows_eh_md::NativeProvenanceModel::SEH,
          windows_eh_md::NativeProvenanceRole::RangeEnter, EH.CodeRange.Begin,
          R.Scope->GuardedRange.Begin, R.SourceIndex, Boundary);
      auto *Marker = B.CreateInvoke(TryBegin, Edge.Target, R.UnwindDest);
      PlanRangeTarget(Marker,
                      windows_eh_md::NativeProvenanceRole::RangeEnterTarget,
                      R.Scope->GuardedRange.Begin, R.SourceIndex, Boundary);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(BeginBB);
    }
    if (EntryWithoutPred) {
      const uint32_t Boundary = Marker++;
      auto *OldEntry = &LLVMFunc.getEntryBlock();
      auto *BeginBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.begin." + std::to_string(RegionIndex) + "." +
              std::to_string(Boundary),
          &LLVMFunc, OldEntry);
      llvm::IRBuilder<> B(BeginBB);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, windows_eh_md::NativeProvenanceModel::SEH,
          windows_eh_md::NativeProvenanceRole::RangeEnter, EH.CodeRange.Begin,
          R.Scope->GuardedRange.Begin, R.SourceIndex, Boundary);
      auto *Marker = B.CreateInvoke(TryBegin, OldEntry, R.UnwindDest);
      PlanRangeTarget(Marker,
                      windows_eh_md::NativeProvenanceRole::RangeEnterTarget,
                      R.Scope->GuardedRange.Begin, R.SourceIndex, Boundary);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(BeginBB);
    }

    struct ExitEdge {
      llvm::Instruction *Term = nullptr;
      llvm::BasicBlock *Target = nullptr;
      unsigned SuccessorIndex = 0;
    };
    llvm::SmallVector<ExitEdge, 8> Exits;
    llvm::SmallVector<llvm::ReturnInst *, 4> Returns;
    for (llvm::BasicBlock *Source : Blocks) {
      llvm::Instruction *Term = Source->getTerminator();
      if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Term)) {
        Returns.push_back(Ret);
        continue;
      }
      for (unsigned I = 0; I < Term->getNumSuccessors(); ++I) {
        llvm::BasicBlock *Target = Term->getSuccessor(I);
        if (IsNormalSuccessor(Term, I) && !R.Blocks.count(Target))
          Exits.push_back({Term, Target, I});
      }
    }

    Marker = 0;
    for (const ExitEdge &Edge : Exits) {
      const uint32_t Boundary = Marker++;
      auto *EndBB = llvm::BasicBlock::Create(*Ctx,
                                             "seh.try.end." +
                                                 std::to_string(RegionIndex) +
                                                 "." + std::to_string(Boundary),
                                             &LLVMFunc, Edge.Target);
      Edge.Term->setSuccessor(Edge.SuccessorIndex, EndBB);
      llvm::IRBuilder<> B(EndBB);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, windows_eh_md::NativeProvenanceModel::SEH,
          windows_eh_md::NativeProvenanceRole::RangeExit, EH.CodeRange.Begin,
          R.Scope->GuardedRange.End, R.SourceIndex, Boundary);
      auto *Marker = B.CreateInvoke(TryEnd, Edge.Target, R.UnwindDest);
      PlanRangeTarget(Marker,
                      windows_eh_md::NativeProvenanceRole::RangeExitTarget,
                      R.Scope->GuardedRange.End, R.SourceIndex, Boundary);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(EndBB);
    }
    for (llvm::ReturnInst *Ret : Returns) {
      const uint32_t Boundary = Marker++;
      llvm::BasicBlock *Source = Ret->getParent();
      auto *EndBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.end." + std::to_string(RegionIndex) + ".ret." +
              std::to_string(Boundary),
          &LLVMFunc);
      auto *RetBB = llvm::BasicBlock::Create(
          *Ctx, "seh.try.ret." + std::to_string(RegionIndex), &LLVMFunc);
      llvm::Value *ReturnValue = Ret->getReturnValue();
      Ret->eraseFromParent();
      llvm::IRBuilder<> SourceBuilder(Source);
      SourceBuilder.CreateBr(EndBB);
      llvm::IRBuilder<> EndBuilder(EndBB);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          EndBuilder, windows_eh_md::NativeProvenanceModel::SEH,
          windows_eh_md::NativeProvenanceRole::RangeExit, EH.CodeRange.Begin,
          R.Scope->GuardedRange.End, R.SourceIndex, Boundary);
      auto *Marker = EndBuilder.CreateInvoke(TryEnd, RetBB, R.UnwindDest);
      llvm::IRBuilder<> RetBuilder(RetBB);
      if (ReturnValue)
        RetBuilder.CreateRet(ReturnValue);
      else
        RetBuilder.CreateRetVoid();
      PlanRangeTarget(Marker,
                      windows_eh_md::NativeProvenanceRole::RangeExitTarget,
                      R.Scope->GuardedRange.End, R.SourceIndex, Boundary);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(EndBB);
    }
  }

  // Inner scopes may retarget an outer marker's normal edge while their
  // boundary blocks are inserted. Bind target provenance only after the full
  // nesting graph is stable so every anchor names the marker's final normal
  // destination rather than an intermediate source block.
  for (const RangeTargetPlan &Target : RangeTargets) {
    assert(Target.Marker && Target.Marker->getNormalDest() &&
           "committed SEH range marker must have a normal destination");
    llvm::BasicBlock *Destination = Target.Marker->getNormalDest();
    llvm::IRBuilder<> TargetBuilder(&*Destination->getFirstInsertionPt());
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        TargetBuilder, windows_eh_md::NativeProvenanceModel::SEH, Target.Role,
        EH.CodeRange.Begin, Target.SourceVA, Target.Region, Target.Boundary);
  }

  // A callback that reads the emulated frame sees at a fault every store
  // made before it in protected code, and LLVM takes no plain load or store
  // for one that can raise: each such access becomes volatile, as clang's
  // -EHa makes those of a __try, except a register home's, which no callback
  // can name.  The output gate finds the frame by its mark.
  if (UsesEmulatedFrame) {
    FrameAlloca->setMetadata(
        windows_eh_md::SEHEmulatedFrameAttachment,
        llvm::MDNode::get(*Ctx, {mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                                 mdUInt(*Ctx, FrameEntrySPOffset, 64)}));
    std::set<llvm::BasicBlock *> Protected;
    for (const Region &R : Regions)
      Protected.insert(R.Blocks.begin(), R.Blocks.end());
    auto *True = llvm::ConstantInt::getTrue(*Ctx);
    const auto IsHome = [&](const llvm::Value *Pointer) {
      const llvm::Value *Base = Pointer->stripPointerCasts();
      return Base != FrameAlloca && llvm::isa<llvm::AllocaInst>(Base);
    };
    for (llvm::BasicBlock *Block : Protected)
      for (llvm::Instruction &Instruction : *Block) {
        if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction)) {
          if (!IsHome(Load->getPointerOperand()))
            Load->setVolatile(true);
        } else if (auto *Store =
                       llvm::dyn_cast<llvm::StoreInst>(&Instruction)) {
          if (!IsHome(Store->getPointerOperand()))
            Store->setVolatile(true);
        } else if (auto *Exchange =
                       llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&Instruction)) {
          Exchange->setVolatile(true);
        } else if (auto *Update =
                       llvm::dyn_cast<llvm::AtomicRMWInst>(&Instruction)) {
          Update->setVolatile(true);
        } else if (auto *Memory =
                       llvm::dyn_cast<llvm::MemIntrinsic>(&Instruction)) {
          Memory->setVolatile(True);
        }
      }
  }

  if (AsyncFlagState == med_llvm_eh::I32ModuleFlagState::Absent)
    Mod->addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  const llvm::StringRef NativeKind =
      TargetArch == Arch::X64
          ? "seh-x64-native"
          : (TargetArch == Arch::ARM ? "seh-arm32-native"
                                     : "seh-aarch64-native");
  LLVMFunc.setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(
          *Ctx, {mdUInt(*Ctx, 1, 1), llvm::MDString::get(*Ctx, NativeKind)}));
  exception_rewrite::setContract(
      LLVMFunc, exception_rewrite::SourceState::Complete,
      exception_rewrite::LoweringState::Complete, ProtectedCallCount,
      ProtectedCallCount, /*SkippedPads=*/0);
  return true;
}

} // namespace neverd
