//===- MedLLVMRegistrationSourceSEH.cpp - x86 SEH for printed C ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Printed C states each scope of an x86 `_except_handler3` or
/// `_except_handler4` frame as a `__try`.  The decompile view reads from the
/// frame's own code which try level each stretch of it runs at, which code only
/// a handler's or a filter's runtime entry runs, and which `call` runs a
/// `__finally` body on the normal path (RegistrationDisplayFacts).  This builds
/// the WinEH pads a private LLVM-to-C module prints from those facts:
///
/// - Each call runs under the scope of its try level, and each scope's blocks
///   are bracketed by `llvm.seh.try.begin` and `llvm.seh.try.end`.
/// - A `__finally` body is one subroutine of the function: the runtime enters
///   it while unwinding and each normal-path `call` runs it.  Its cleanup pad
///   holds the body, and each `call` becomes a copy of it, which C's
///   `__finally` prints apart from the pad as the normal termination.
/// - An `__except` filter becomes a source helper with explicit captures and
///   exception pointers; a constant EXCEPTION_EXECUTE_HANDLER needs no helper.
/// - `_local_unwind2` and `_local_unwind4` become the exited scopes' cleanup
///   bodies, in unwind order, under each body's enclosing scope.
///
/// Anything else keeps the function's SEH unlowered: printed C then says so.
///
//===----------------------------------------------------------------------===//

#include "../eh/MedLLVMEHHelpers.h"
#include "MedLLVMSourceSEHFilter.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/llvm/LLVMSourceBody.h"
#include "neverd/backend/llvm/LLVMSourceSEHFilter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/X86/LLVMX86AddressSpaces.h"
#include "neverd/ir/RegistrationDisplay.h"
#include "neverd/ir/RegistrationState.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {
namespace {

using Stretch = RegistrationDisplayFacts::Stretch;
/// No scope: a level below zero, as the table's sentinels are.
constexpr int32_t kNoScope = -1;

/// The stretches holding any byte of [Begin, End), in address order.
llvm::ArrayRef<Stretch> stretchesIn(const RegistrationDisplayFacts &Facts,
                                    va_t Begin, va_t End) {
  const llvm::ArrayRef<Stretch> All(Facts.Stretches);
  const auto First = llvm::partition_point(
      All, [&](const Stretch &S) { return S.Range.End <= Begin; });
  auto Last = First;
  while (Last != All.end() && Last->Range.Begin < End)
    ++Last;
  return All.slice(static_cast<size_t>(First - All.begin()),
                   static_cast<size_t>(Last - First));
}

/// The scopes code at \p Level runs under, innermost first.  The table's
/// enclosing levels decrease, so the walk ends.
std::vector<int32_t>
scopesAt(int32_t Level, const std::vector<RegistrationScopeRecord> &Scopes) {
  std::vector<int32_t> Chain;
  while (Level >= 0 && static_cast<size_t>(Level) < Scopes.size() &&
         Chain.size() < Scopes.size()) {
    Chain.push_back(Level);
    Level = Scopes[static_cast<size_t>(Level)].EnclosingLevel;
  }
  return Chain;
}

/// The innermost scope every level of \p Levels runs under.
int32_t
innermostCommonScope(const std::vector<int32_t> &Levels,
                     const std::vector<RegistrationScopeRecord> &Scopes) {
  if (Levels.empty())
    return kNoScope;
  for (const int32_t Candidate : scopesAt(Levels.front(), Scopes))
    if (llvm::all_of(Levels, [&](int32_t Level) {
          return llvm::is_contained(scopesAt(Level, Scopes), Candidate);
        }))
      return Candidate;
  return kNoScope;
}

/// The value a filter whose code is only \p Block returns, when it computes
/// nothing but a constant: no load, store or call.
std::optional<uint64_t> constantFilterValue(const MedBlock &Block) {
  if (!Block.Succs.empty() || !Block.Phis.empty())
    return std::nullopt;
  std::map<std::pair<int, int>, uint64_t> Known;
  for (const MedOp &Op : Block.Ops) {
    switch (Op.Opcode) {
    case NdOp::NOP:
      continue;
    case NdOp::COPY:
      // The runtime entry's own frame pointer.
      if (Op.RegistrationRoot != MedOp::RegistrationRootKind::None)
        continue;
      if (Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
          Op.Output.Kind == MedVar::Reg) {
        Known[{Op.Output.Id, Op.Output.SSAVer}] = Op.Inputs[0].ConstVal;
        continue;
      }
      return std::nullopt;
    case NdOp::RETURN: {
      if (Op.NumInputs < 1)
        return std::nullopt;
      const MedVar &Value = Op.Inputs[0];
      if (Value.isConst())
        return Value.ConstVal;
      if (Value.Kind != MedVar::Reg)
        return std::nullopt;
      const auto It = Known.find({Value.Id, Value.SSAVer});
      if (It == Known.end())
        return std::nullopt;
      return It->second;
    }
    default:
      return std::nullopt;
    }
  }
  return std::nullopt;
}

bool mayUnwind(const llvm::CallInst &Call) {
  return !Call.doesNotThrow() && !Call.isMustTailCall() &&
         !llvm::isa<llvm::IntrinsicInst>(Call);
}

/// Whether every value \p Blocks define is read only inside them, so a copy of
/// the blocks reads its own.
bool keepsItsValues(llvm::ArrayRef<llvm::BasicBlock *> Blocks) {
  const std::set<const llvm::BasicBlock *> Within(Blocks.begin(), Blocks.end());
  for (llvm::BasicBlock *Block : Blocks)
    for (llvm::Instruction &Inst : *Block)
      for (const llvm::User *User : Inst.users()) {
        const auto *Reader = llvm::dyn_cast<llvm::Instruction>(User);
        if (!Reader || !Within.count(Reader->getParent()))
          return false;
      }
  return true;
}

/// Whether the result of \p Call, which leaves nothing in the result
/// register, goes only into variable slots.
bool resultOnlyStored(const llvm::CallInst &Call) {
  return llvm::all_of(Call.users(), [&](const llvm::User *User) {
    const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
    return Store && Store->getValueOperand() == &Call &&
           llvm::isa<llvm::AllocaInst>(Store->getPointerOperand());
  });
}

/// Remove \p Call, whose result is nothing: the slots it went into keep what
/// they held, as the register keeps what the callee left.
void eraseNoValueCall(llvm::CallInst &Call,
                      med_llvm_eh::SourceCallAddressMap &Addresses) {
  for (llvm::User *User : llvm::make_early_inc_range(Call.users()))
    llvm::cast<llvm::Instruction>(User)->eraseFromParent();
  Addresses.erase(&Call);
  Call.eraseFromParent();
}

/// Replace \p Ret with \p Replacement and drop what only computed its value.
void replaceReturn(llvm::ReturnInst &Ret, llvm::Instruction *Replacement) {
  llvm::Value *Value = Ret.getReturnValue();
  Replacement->insertBefore(Ret.getIterator());
  Ret.eraseFromParent();
  if (Value)
    llvm::RecursivelyDeleteTriviallyDeadInstructions(Value);
}

} // namespace

bool MedLLVMEmitter::emitSourceX86RegistrationSEH(
    const MedFunc &Func, llvm::Function &Parent,
    const std::map<int, llvm::BasicBlock *> &BlockMap,
    const std::map<llvm::BasicBlock *, std::pair<int, int>> &PHICopyEdges) {
  if (!AnalysisRecoveryForSource || TargetArch != Arch::X86 ||
      TargetFormat != BinaryFormat::COFF || !Func.ExceptionMetadata ||
      !Func.RegistrationStates || !Func.RegistrationStates->Display)
    return false;
  if (Parent.hasPersonalityFn())
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  const auto Source = classifyWindowsEHNativeSource(
      EH, TargetArch, TargetFormat,
      WindowsEHNativeCapability::SourceIRLowering);
  if (!Source.canLowerNativeIR() ||
      Source.Model != WindowsEHNativeSourceModel::X86RegistrationSEH)
    return false;
  if (!med_llvm_eh::collectExactSourceCallAddresses(Parent, CallSiteAddrs))
    return false;
  const med_llvm_eh::I32ModuleFlagState AsyncFlagState =
      med_llvm_eh::classifyI32ModuleFlag(*Mod, "eh-asynch",
                                         llvm::Module::Warning, 1);
  if (AsyncFlagState == med_llvm_eh::I32ModuleFlagState::Conflict)
    return false;
  const bool EH4 = EH.Personality == ExceptionPersonality::ExceptHandler4;
  const llvm::StringRef PersonalityName =
      EH4 ? "_except_handler4" : "_except_handler3";
  if (const llvm::GlobalValue *Named = Mod->getNamedValue(PersonalityName);
      Named && !llvm::isa<llvm::Function>(Named))
    return false;

  const RegistrationChainInfo &Chain = *EH.Registration;
  const RegistrationDisplayFacts &Facts = *Func.RegistrationStates->Display;
  const std::vector<RegistrationScopeRecord> &Scopes = Chain.Scopes;

  std::map<va_t, const MedBlock *> ByStart;
  std::map<int, const MedBlock *> ById;
  for (const MedBlock &Block : Func.Blocks) {
    ByStart.emplace(Block.StartAddr, &Block);
    ById.emplace(Block.Id, &Block);
  }
  std::set<va_t> FinallyRoots;
  for (const RegistrationScopeRecord &Scope : Scopes)
    if (Scope.IsFinally)
      FinallyRoots.insert(Scope.HandlerVA);

  // The layout order every walk that names a new block follows: pointer order
  // differs from run to run.
  std::vector<llvm::BasicBlock *> Layout;
  for (llvm::BasicBlock &Block : Parent)
    Layout.push_back(&Block);

  // The Med block each LLVM block emits: a block's own, a PHI copy on an edge
  // out of it, or one emission split off it.
  std::map<const llvm::BasicBlock *, int> HomeOf;
  std::map<int, llvm::BasicBlock *> Mapped;
  for (const auto &[Id, Block] : BlockMap)
    if (Block && Block->getParent() == &Parent && ById.count(Id)) {
      HomeOf.emplace(Block, Id);
      Mapped.emplace(Id, Block);
    }
  for (const auto &[Block, Edge] : PHICopyEdges)
    if (Block && Block->getParent() == &Parent && ById.count(Edge.first))
      HomeOf.emplace(Block, Edge.first);
  for (llvm::BasicBlock *Start : Layout) {
    if (!Mapped.count(HomeOf.count(Start) ? HomeOf.at(Start) : -1) ||
        Mapped.at(HomeOf.at(Start)) != Start)
      continue;
    std::vector<const llvm::BasicBlock *> Work{Start};
    while (!Work.empty()) {
      const llvm::BasicBlock *Block = Work.back();
      Work.pop_back();
      for (const llvm::BasicBlock *Succ : llvm::successors(Block))
        if (HomeOf.emplace(Succ, HomeOf.at(Block)).second)
          Work.push_back(Succ);
    }
  }
  std::map<int, unsigned> EmittedBlocks;
  for (const auto &[Block, Id] : HomeOf)
    if (!PHICopyEdges.count(const_cast<llvm::BasicBlock *>(Block)))
      ++EmittedBlocks[Id];
  auto LLVMBlocksOf = [&](const std::set<int> &Ids) {
    std::vector<llvm::BasicBlock *> Blocks;
    for (llvm::BasicBlock *Block : Layout)
      if (const auto Home = HomeOf.find(Block);
          Home != HomeOf.end() && Ids.count(Home->second))
        Blocks.push_back(Block);
    return Blocks;
  };

  std::map<va_t, llvm::CallInst *> CallAt;
  for (const auto &[Call, Address] : CallSiteAddrs)
    if (Call->getFunction() == &Parent)
      CallAt.emplace(Address, const_cast<llvm::CallInst *>(Call));

  // What each Med block runs at.  The normal path's call of a `__finally` body
  // and a local unwind can change the level inside a block: the code after
  // the call runs at `After`.
  struct BlockPlan {
    int32_t Level = kNoScope;
    int32_t After = kNoScope;
    va_t Owner = 0;
    const MedOp *Special = nullptr;
    llvm::CallInst *Call = nullptr;
    std::vector<size_t> UnwindScopes;
  };
  std::map<int, BlockPlan> Plans;
  for (const MedBlock &Block : Func.Blocks) {
    if (!Mapped.count(Block.Id) || Block.StartAddr >= Block.EndAddr)
      continue;
    const llvm::ArrayRef<Stretch> In =
        stretchesIn(Facts, Block.StartAddr, Block.EndAddr);
    if (In.empty())
      continue;
    BlockPlan Plan;
    std::set<va_t> Owners;
    for (const Stretch &S : In)
      Owners.insert(S.Owner);
    if (Owners.size() != 1)
      return false;
    Plan.Owner = *Owners.begin();
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL || !Facts.NoValueCalls.count(Op.Addr))
        continue;
      const bool Finally = Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
                           FinallyRoots.count(Op.Inputs[0].ConstVal);
      if (!Finally && !Facts.LocalUnwinds.count(Op.Addr))
        continue;
      if (Plan.Special)
        return false;
      Plan.Special = &Op;
    }
    // A local unwind leaves the level it unwinds to from the next
    // instruction on; a body's call runs at the level it changes to.
    va_t Boundary = Block.EndAddr;
    if (Plan.Special) {
      Boundary = Plan.Special->Addr;
      if (Facts.LocalUnwinds.count(Plan.Special->Addr)) {
        Boundary = Block.EndAddr;
        for (const MedOp &Op : Block.Ops)
          if (Op.Addr > Plan.Special->Addr) {
            Boundary = Op.Addr;
            break;
          }
      }
      const auto Found = CallAt.find(Plan.Special->Addr);
      if (Found == CallAt.end() ||
          Found->second->getParent() != Mapped.at(Block.Id) ||
          !Found->second->getNextNode())
        return false;
      Plan.Call = Found->second;
      if (EmittedBlocks[Block.Id] != 1)
        return false;
    }
    // Code that cannot raise runs the same under any level: a part of only
    // that takes the level it leaves at.
    auto LevelOf = [&](bool BeforeBoundary) -> std::optional<int32_t> {
      std::set<std::vector<int32_t>> Loud;
      const Stretch *Last = nullptr;
      for (const Stretch &S : In) {
        const bool Part =
            BeforeBoundary ? S.Range.Begin < Boundary : S.Range.End > Boundary;
        if (!Part)
          continue;
        Last = &S;
        if (!S.Quiet)
          Loud.insert(S.Levels);
      }
      if (Loud.empty())
        return Last ? innermostCommonScope(Last->Levels, Scopes) : kNoScope;
      if (Loud.size() != 1 || Loud.begin()->size() != 1)
        return std::nullopt;
      return Loud.begin()->front();
    };
    const auto Before = LevelOf(true);
    if (!Before)
      return false;
    Plan.Level = *Before;
    Plan.After = Plan.Level;
    if (Plan.Special && Boundary < Block.EndAddr) {
      const auto After = LevelOf(false);
      if (!After)
        return false;
      Plan.After = *After;
    }
    if (Plan.Level >= static_cast<int32_t>(Scopes.size()) ||
        Plan.After >= static_cast<int32_t>(Scopes.size()))
      return false;
    if (Plan.Special && Facts.LocalUnwinds.count(Plan.Special->Addr)) {
      int32_t Level = Plan.Level;
      while (Level >= 0 && Level != Plan.After) {
        Plan.UnwindScopes.push_back(static_cast<size_t>(Level));
        Level = Scopes[static_cast<size_t>(Level)].EnclosingLevel;
      }
      if (Level != Plan.After && !(Level < 0 && Plan.After < 0))
        return false;
    }
    Plans.emplace(Block.Id, Plan);
  }
  auto PlanOf = [&](int Id) -> const BlockPlan * {
    const auto It = Plans.find(Id);
    return It == Plans.end() ? nullptr : &It->second;
  };

  // The scopes, and what each needs.
  struct Region {
    size_t Index = 0;
    const RegistrationScopeRecord *Scope = nullptr;
    int Parent = -1;
    size_t Depth = 0;
    /// `__finally`: the body's blocks and the normal path's calls of it.
    std::vector<llvm::BasicBlock *> Body;
    llvm::BasicBlock *BodyEntry = nullptr;
    std::vector<int> BodyCalls;
    /// `__except`: the handler, and the filter's blocks that go.
    llvm::BasicBlock *Handler = nullptr;
    std::vector<llvm::BasicBlock *> FilterBlocks;
    va_t FilterRoot = 0;
    llvm::BasicBlock *UnwindDest = nullptr;
    std::set<llvm::BasicBlock *> Blocks;
  };
  std::vector<Region> Regions(Scopes.size());
  std::set<llvm::BasicBlock *> Funclets;
  std::set<llvm::BasicBlock *> Removed;
  std::set<va_t> ClaimedBodies;
  struct FilterPlan {
    X86RegistrationCallbackRequest Request;
    std::optional<x86_registration::SourceFilterPlan> Outline;
    std::optional<uint32_t> Constant;
    llvm::Function *Function = nullptr;
    std::vector<llvm::Value *> Captures;
  };
  std::map<va_t, FilterPlan> Filters;
  size_t FilterWork = 0;
  for (size_t K = 0; K < Scopes.size(); ++K) {
    const RegistrationScopeRecord &Scope = Scopes[K];
    Region &R = Regions[K];
    R.Index = K;
    R.Scope = &Scope;
    R.Parent = Scope.EnclosingLevel >= 0 ? Scope.EnclosingLevel : -1;
    R.Depth = scopesAt(static_cast<int32_t>(K), Scopes).size();
    const auto Root = ByStart.find(Scope.HandlerVA);
    if (Root == ByStart.end() || !Mapped.count(Root->second->Id))
      return false;
    const int RootId = Root->second->Id;
    if (Scope.IsFinally) {
      if (!ClaimedBodies.insert(Scope.HandlerVA).second)
        return false;
      std::set<int> Body;
      for (const auto &[Id, Plan] : Plans)
        if (Plan.Owner == Scope.HandlerVA)
          Body.insert(Id);
      if (!Body.count(RootId))
        return false;
      for (const int Id : Body) {
        const MedBlock &Block = *ById.at(Id);
        const BlockPlan &Plan = *PlanOf(Id);
        if (Plan.Special || Plan.Level != Scope.EnclosingLevel)
          return false;
        for (const int Succ : Block.Succs)
          if (!Body.count(Succ))
            return false;
        for (const int Pred : Block.Preds)
          if (!Body.count(Pred) || Id == RootId)
            return false;
        for (const MedOp &Op : Block.Ops)
          if (Op.Opcode == NdOp::RETURN && !Facts.FinallyReturns.count(Op.Addr))
            return false;
      }
      R.Body = LLVMBlocksOf(Body);
      R.BodyEntry = Mapped.at(RootId);
      if (!llvm::pred_empty(R.BodyEntry) ||
          llvm::isa<llvm::PHINode>(R.BodyEntry->front()) ||
          !keepsItsValues(R.Body))
        return false;
      const std::set<llvm::BasicBlock *> Within(R.Body.begin(), R.Body.end());
      for (llvm::BasicBlock *Block : R.Body) {
        for (llvm::BasicBlock *Succ : llvm::successors(Block))
          if (!Within.count(Succ))
            return false;
        if (Block != R.BodyEntry)
          for (llvm::BasicBlock *Pred : llvm::predecessors(Block))
            if (!Within.count(Pred))
              return false;
        if (Block->isEHPad())
          return false;
      }
      Funclets.insert(R.Body.begin(), R.Body.end());
      for (const auto &[Id, Plan] : Plans) {
        if (!Plan.Special || Facts.LocalUnwinds.count(Plan.Special->Addr) ||
            Plan.Special->Inputs[0].ConstVal != Scope.HandlerVA)
          continue;
        if (Body.count(Id))
          return false;
        if (!resultOnlyStored(*Plan.Call))
          return false;
        R.BodyCalls.push_back(Id);
      }
      // Every ordinary entry must be one of the calls planned above. Moving
      // the body into a pad cannot leave a call to an unrepresented helper,
      // nor can a clause represent an entry into the middle of that body.
      for (const MedBlock &Block : Func.Blocks)
        for (const MedOp &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
              Op.Inputs[0].isConst() && llvm::any_of(Body, [&](int Id) {
                const MedBlock &B = *ById.at(Id);
                return Op.Inputs[0].ConstVal >= B.StartAddr &&
                       Op.Inputs[0].ConstVal < B.EndAddr;
              })) {
            const BlockPlan *Caller = PlanOf(Block.Id);
            if (Op.Inputs[0].ConstVal != Scope.HandlerVA || !Caller ||
                Caller->Special != &Op || Facts.LocalUnwinds.count(Op.Addr))
              return false;
          }
      continue;
    }
    const BlockPlan *HandlerPlan = PlanOf(RootId);
    if (!HandlerPlan || HandlerPlan->Owner != Scope.HandlerVA)
      return false;
    R.Handler = Mapped.at(RootId);
    if (!llvm::pred_empty(R.Handler) ||
        llvm::isa<llvm::PHINode>(R.Handler->front()))
      return false;
    const auto Filter = ByStart.find(Scope.FilterVA);
    if (Filter == ByStart.end() || !Mapped.count(Filter->second->Id))
      return false;
    std::set<int> Body;
    for (const auto &[Id, Plan] : Plans)
      if (Plan.Owner == Scope.FilterVA) {
        if (Plan.Special)
          return false;
        Body.insert(Id);
      }
    if (!Body.count(Filter->second->Id))
      return false;
    R.FilterBlocks = LLVMBlocksOf(Body);
    if (!keepsItsValues(R.FilterBlocks))
      return false;
    const auto Value =
        Body.size() == 1 ? constantFilterValue(*Filter->second) : std::nullopt;
    if (Value) {
      const std::set<llvm::BasicBlock *> Within(R.FilterBlocks.begin(),
                                                R.FilterBlocks.end());
      for (llvm::BasicBlock *Block : R.FilterBlocks)
        for (llvm::BasicBlock *Pred : llvm::predecessors(Block))
          if (!Within.count(Pred))
            return false;
    }
    if (!Value || *Value != 1) {
      R.FilterRoot = Scope.FilterVA;
      auto [It, Inserted] = Filters.try_emplace(Scope.FilterVA);
      if (Inserted) {
        auto &Request = It->second.Request;
        Request.Entry = Mapped.at(Filter->second->Id);
        Request.Blocks = R.FilterBlocks;
        Request.Name = Parent.getName().str() + ".seh_filter." +
                       std::to_string(Scope.FilterVA);
        if (Value) {
          It->second.Constant = static_cast<uint32_t>(*Value);
        } else {
          if (FrameAlloca) {
            const int64_t Cell =
                int64_t(FrameEntrySPOffset) +
                registration_display::exceptionPointersSlot(Facts, Chain);
            if (Cell < 0)
              return false;
            Request.Frame.SyntheticFrame = FrameAlloca;
            Request.Frame.ExceptionPointersOffset = static_cast<uint64_t>(Cell);
          }
          auto Prepared = x86_registration::prepareSourceFilter(Parent, Request,
                                                                FilterWork);
          if (!Prepared) {
            llvm::consumeError(Prepared.takeError());
            return false;
          }
          It->second.Outline = std::move(*Prepared);
        }
      }
    }
    // A callback invoked by ordinary code still needs its original entry.
    for (const auto &Block : Func.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst() &&
            llvm::any_of(Body, [&](int Id) {
              const auto &Part = *ById.at(Id);
              return Op.Inputs[0].ConstVal >= Part.StartAddr &&
                     Op.Inputs[0].ConstVal < Part.EndAddr;
            }))
          return false;
    Removed.insert(R.FilterBlocks.begin(), R.FilterBlocks.end());
  }
  for (const auto &[Id, Plan] : Plans)
    if (Plan.Special && Facts.LocalUnwinds.count(Plan.Special->Addr) &&
        !resultOnlyStored(*Plan.Call))
      return false;
  for (const auto &[Block, Id] : HomeOf)
    if (!Block->getTerminator())
      return false;

  // Charge the complete expansion before changing the parent. One large
  // cleanup called at many sites must not multiply the source IR unchecked.
  std::vector<size_t> BodySizes(Regions.size());
  for (const Region &R : Regions)
    for (const llvm::BasicBlock *Block : R.Body)
      BodySizes[R.Index] += Block->size();
  size_t Remaining = limits::backend::kMaxSourceX86SEHClonedInstructions;
  auto Charge = [&](size_t Index) {
    if (BodySizes[Index] > Remaining)
      return false;
    Remaining -= BodySizes[Index];
    return true;
  };
  for (const Region &R : Regions)
    for (size_t I = 0; I < R.BodyCalls.size(); ++I) {
      if (!Charge(R.Index))
        return false;
    }
  for (const auto &[Id, Plan] : Plans)
    for (const size_t K : Plan.UnwindScopes)
      if (Regions[K].Scope->IsFinally && !Charge(K))
        return false;

  for (const auto &[Root, Filter] : Filters) {
    if (Filter.Constant)
      continue;
    const auto &Plan = *Filter.Outline;
    size_t Cost =
        Plan.Callback.Recipes.size() + Plan.Callback.PrivateSlots.size() +
        2 * Plan.Initializers.size() + 3 * Plan.Callback.Arguments.size();
    for (const auto *Block : Filter.Request.Blocks)
      Cost += Block->size();
    if (Cost > Remaining)
      return false;
    Remaining -= Cost;
  }

  // Commit.  Nothing below refuses.
  // A function without synthetic storage can begin directly in its protected
  // block. New capture homes must be initialized before a try-begin invoke,
  // whose exceptional edge already uses them. Keep static allocas in entry.
  if (HomeOf.count(&Parent.getEntryBlock()) &&
      llvm::any_of(Filters, [](const auto &Entry) {
        return Entry.second.Outline && !Entry.second.Outline->Captures.empty();
      })) {
    auto *OldEntry = &Parent.getEntryBlock();
    auto *Setup = llvm::BasicBlock::Create(*Ctx, "seh.filter.captures", &Parent,
                                           OldEntry);
    auto *Branch = llvm::UncondBrInst::Create(OldEntry, Setup);
    for (auto &Instruction : llvm::make_early_inc_range(*OldEntry))
      if (llvm::isa<llvm::AllocaInst>(Instruction))
        Instruction.moveBefore(Branch->getIterator());
  }
  llvm::FunctionCallee Personality = Mod->getOrInsertFunction(
      PersonalityName,
      llvm::FunctionType::get(llvm::Type::getInt32Ty(*Ctx), {}, true));
  Parent.setPersonalityFn(llvm::cast<llvm::Constant>(Personality.getCallee()));
  for (auto &[Root, Filter] : Filters) {
    if (!Filter.Constant) {
      auto Outlined =
          x86_registration::commitSourceFilter(Parent, *Filter.Outline);
      Filter.Function = Outlined.Function;
      Filter.Captures = std::move(Outlined.Captures);
      continue;
    }
    llvm::IRBuilder<> Builder(*Ctx);
    Filter.Function = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getInt32Ty(), {Builder.getPtrTy()},
                                false),
        llvm::GlobalValue::InternalLinkage, Filter.Request.Name, Mod);
    Filter.Function->addFnAttr(LLVMSourceBody);
    Filter.Function->addFnAttr(LLVMSourceSEHFilter);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(*Ctx, "entry", Filter.Function));
    Builder.CreateRet(Builder.getInt32(*Filter.Constant));
  }

  const std::set<llvm::BasicBlock *> RemovedSet(Removed.begin(), Removed.end());
  std::map<llvm::BasicBlock *, int32_t> LevelOfBlock;
  for (llvm::BasicBlock *Block : Layout) {
    const auto Home = HomeOf.find(Block);
    if (Home == HomeOf.end() || Funclets.count(Block) ||
        RemovedSet.count(Block))
      continue;
    if (const BlockPlan *Plan = PlanOf(Home->second))
      LevelOfBlock[Block] =
          PHICopyEdges.count(Block) ? Plan->After : Plan->Level;
  }

  // Ordinary calls and explicit local unwinds execute their own copies. The
  // writer's finally guard runs the pad only on an exception: a normal C goto
  // cannot stand in for the runtime's local unwind.
  auto CopyBody = [&](const Region &R, llvm::BasicBlock *Next, int32_t Level,
                      llvm::StringRef Suffix) {
    llvm::ValueToValueMapTy Copies;
    llvm::SmallVector<llvm::BasicBlock *, 8> Cloned;
    for (llvm::BasicBlock *Block : R.Body) {
      llvm::BasicBlock *Copy =
          llvm::CloneBasicBlock(Block, Copies, Suffix, &Parent);
      Copy->moveBefore(Next);
      Copies[Block] = Copy;
      Cloned.push_back(Copy);
      LevelOfBlock[Copy] = Level;
    }
    llvm::remapInstructionsInBlocks(Cloned, Copies);
    for (llvm::BasicBlock *Copy : Cloned)
      for (llvm::Instruction &Inst : llvm::make_early_inc_range(*Copy)) {
        Inst.setMetadata(language_eh_md::InternalSourceCallAttachment, nullptr);
        if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(&Inst))
          replaceReturn(*Ret, llvm::UncondBrInst::Create(Next));
      }
    return llvm::cast<llvm::BasicBlock>(Copies[R.BodyEntry]);
  };
  auto ReplaceCall = [&](llvm::CallInst &Call, llvm::BasicBlock *First) {
    llvm::BasicBlock *Site = Call.getParent();
    Site->getTerminator()->eraseFromParent();
    eraseNoValueCall(Call, CallSiteAddrs);
    llvm::UncondBrInst::Create(First, Site);
  };
  for (Region &R : Regions)
    for (const int Id : R.BodyCalls) {
      const BlockPlan &Plan = *PlanOf(Id);
      llvm::CallInst *Call = Plan.Call;
      llvm::BasicBlock *Site = Call->getParent();
      llvm::BasicBlock *After = Site->splitBasicBlock(
          Call->getNextNode(), Site->getName() + ".finally.after");
      LevelOfBlock[After] = Plan.After;
      ReplaceCall(*Call, CopyBody(R, After, Plan.After, ".normal"));
    }
  for (const auto &[Id, Plan] : Plans) {
    if (!Plan.Special || !Facts.LocalUnwinds.count(Plan.Special->Addr))
      continue;
    llvm::CallInst *Call = Plan.Call;
    llvm::BasicBlock *Site = Call->getParent();
    llvm::BasicBlock *After = Site->splitBasicBlock(
        Call->getNextNode(), Site->getName() + ".unwound");
    LevelOfBlock[After] = Plan.After;
    llvm::BasicBlock *Next = After;
    for (const size_t K : llvm::reverse(Plan.UnwindScopes))
      if (Regions[K].Scope->IsFinally)
        Next = CopyBody(Regions[K], Next, Scopes[K].EnclosingLevel,
                        ".local_unwind");
    ReplaceCall(*Call, Next);
  }

  for (llvm::BasicBlock *Block : Removed) {
    for (auto &Instruction : *Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
        CallSiteAddrs.erase(Call);
    Block->dropAllReferences();
  }
  for (llvm::BasicBlock *Block : Removed) {
    LevelOfBlock.erase(Block);
    Block->eraseFromParent();
  }

  // The pads, outer scopes first so an inner one unwinds to its parent's.
  std::vector<size_t> Order(Regions.size());
  for (size_t K = 0; K < Order.size(); ++K)
    Order[K] = K;
  llvm::stable_sort(Order, [&](size_t A, size_t B) {
    return Regions[A].Depth < Regions[B].Depth;
  });
  auto *TokenNone = llvm::ConstantTokenNone::get(*Ctx);
  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  auto ToInvoke = [&](llvm::CallInst *Call, llvm::BasicBlock *Unwind) {
    llvm::BasicBlock *Block = Call->getParent();
    llvm::BasicBlock *Next = Block->splitBasicBlock(
        Call->getNextNode(), Block->getName() + ".seh.cont");
    if (const auto Level = LevelOfBlock.find(Block);
        Level != LevelOfBlock.end())
      LevelOfBlock[Next] = Level->second;
    llvm::SmallVector<llvm::Value *, 8> Args;
    for (llvm::Use &Arg : Call->args())
      Args.push_back(Arg.get());
    llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
    Call->getOperandBundlesAsDefs(Bundles);
    llvm::Instruction *Branch = Block->getTerminator();
    auto *Invoke = llvm::InvokeInst::Create(
        Call->getFunctionType(), Call->getCalledOperand(), Next, Unwind, Args,
        Bundles, Call->getName(), Branch->getIterator());
    Invoke->setCallingConv(Call->getCallingConv());
    Invoke->setAttributes(Call->getAttributes());
    Invoke->setDebugLoc(Call->getDebugLoc());
    Invoke->copyMetadata(*Call);
    Call->replaceAllUsesWith(Invoke);
    CallSiteAddrs.erase(Call);
    Call->eraseFromParent();
    Branch->eraseFromParent();
  };
  // The calls of \p Blocks that may unwind, in layout order.
  auto CallsIn = [&](llvm::ArrayRef<llvm::BasicBlock *> Blocks) {
    std::vector<llvm::CallInst *> Calls;
    for (llvm::BasicBlock *Block : Blocks)
      for (llvm::Instruction &Inst : *Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
            Call && mayUnwind(*Call) && Call->getNextNode())
          Calls.push_back(Call);
    return Calls;
  };
  for (const size_t K : Order) {
    Region &R = Regions[K];
    llvm::BasicBlock *ParentDest =
        R.Parent >= 0 ? Regions[static_cast<size_t>(R.Parent)].UnwindDest
                      : nullptr;
    const std::string Suffix = std::to_string(K);
    if (R.Scope->IsFinally) {
      auto *PadBB = llvm::BasicBlock::Create(
          *Ctx, "seh.finally.dispatch." + Suffix, &Parent);
      llvm::IRBuilder<> B(PadBB);
      llvm::CleanupPadInst *Pad =
          B.CreateCleanupPad(TokenNone, {}, "seh.finally.pad");
      B.CreateBr(R.BodyEntry);
      // The funclet follows its pad, past the function's own code.
      for (llvm::BasicBlock *Block : R.Body)
        Block->moveAfter(&Parent.back());
      std::vector<llvm::ReturnInst *> Returns;
      for (llvm::BasicBlock *Block : R.Body)
        if (auto *Ret =
                llvm::dyn_cast<llvm::ReturnInst>(Block->getTerminator()))
          Returns.push_back(Ret);
      for (llvm::ReturnInst *Ret : Returns)
        replaceReturn(*Ret, llvm::CleanupReturnInst::Create(Pad, ParentDest));
      // What the body calls unwinds where the pad does.
      if (ParentDest)
        for (llvm::CallInst *Call : CallsIn(R.Body))
          ToInvoke(Call, ParentDest);
      R.UnwindDest = PadBB;
      continue;
    }
    auto *Dispatch =
        llvm::BasicBlock::Create(*Ctx, "seh.catch.dispatch." + Suffix, &Parent);
    llvm::IRBuilder<> DB(Dispatch);
    auto *Switch =
        DB.CreateCatchSwitch(TokenNone, ParentDest, 1, "seh.catch.switch");
    auto *PadBB =
        llvm::BasicBlock::Create(*Ctx, "seh.catch.pad." + Suffix, &Parent);
    Switch->addHandler(PadBB);
    llvm::IRBuilder<> PB(PadBB);
    llvm::SmallVector<llvm::Value *, 4> Arguments;
    if (R.FilterRoot) {
      const auto &Filter = Filters.at(R.FilterRoot);
      Arguments.push_back(Filter.Function);
      Arguments.append(Filter.Captures.begin(), Filter.Captures.end());
    } else {
      Arguments.push_back(llvm::ConstantPointerNull::get(PtrTy));
    }
    llvm::CatchPadInst *Pad =
        PB.CreateCatchPad(Switch, Arguments, "seh.catch.pad.token");
    PB.CreateCatchRet(Pad, R.Handler);
    R.UnwindDest = Dispatch;
  }

  // Each call runs under the scope of its level.
  uint64_t Protected = 0;
  {
    std::vector<llvm::BasicBlock *> Guarded;
    for (llvm::BasicBlock &Block : Parent)
      if (const auto Level = LevelOfBlock.find(&Block);
          Level != LevelOfBlock.end() && Level->second >= 0)
        Guarded.push_back(&Block);
    for (llvm::CallInst *Call : CallsIn(Guarded)) {
      const int32_t Level = LevelOfBlock.at(Call->getParent());
      ToInvoke(Call, Regions[static_cast<size_t>(Level)].UnwindDest);
      ++Protected;
    }
  }

  // Each scope's blocks, bracketed as emitNativeSEH brackets a range.
  for (const auto &[Block, Level] : LevelOfBlock)
    for (const int32_t Index : scopesAt(Level, Scopes))
      Regions[static_cast<size_t>(Index)].Blocks.insert(Block);
  llvm::Function *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_try_begin);
  llvm::Function *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_try_end);
  auto IsNormalSuccessor = [](llvm::Instruction *Term, unsigned Index) {
    if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Term))
      return Index == 0 && Invoke->getNormalDest();
    return !llvm::isa<llvm::CatchSwitchInst, llvm::CleanupReturnInst>(Term);
  };
  llvm::DenseMap<const llvm::BasicBlock *, size_t> LayoutIndex;
  for (const llvm::BasicBlock &BB : Parent)
    LayoutIndex.try_emplace(&BB, LayoutIndex.size());
  for (const size_t K : Order) {
    Region &R = Regions[K];
    if (R.Blocks.empty())
      continue;
    llvm::SmallVector<llvm::BasicBlock *, 16> Blocks(R.Blocks.begin(),
                                                     R.Blocks.end());
    llvm::sort(Blocks,
               [&](const llvm::BasicBlock *A, const llvm::BasicBlock *B) {
                 return LayoutIndex.lookup(A) < LayoutIndex.lookup(B);
               });
    auto InParents = [&](llvm::BasicBlock *Marker) {
      for (int P = R.Parent; P >= 0; P = Regions[static_cast<size_t>(P)].Parent)
        Regions[static_cast<size_t>(P)].Blocks.insert(Marker);
    };
    struct Edge {
      llvm::Instruction *Term;
      llvm::BasicBlock *Target;
      unsigned Index;
    };
    unsigned Boundary = 0;
    llvm::BasicBlock *FunctionEntry = &Parent.getEntryBlock();
    llvm::SmallVector<Edge, 8> Entries;
    bool EntersAtStart = false;
    for (llvm::BasicBlock *Target : Blocks) {
      bool HasNormalPred = false;
      for (llvm::BasicBlock *Pred : llvm::predecessors(Target)) {
        llvm::Instruction *Term = Pred->getTerminator();
        for (unsigned I = 0; I < Term->getNumSuccessors(); ++I) {
          if (Term->getSuccessor(I) != Target || !IsNormalSuccessor(Term, I))
            continue;
          HasNormalPred = true;
          if (!R.Blocks.count(Pred))
            Entries.push_back({Term, Target, I});
        }
      }
      EntersAtStart |= Target == FunctionEntry && !HasNormalPred;
    }
    for (const Edge &E : Entries) {
      auto *BeginBB =
          llvm::BasicBlock::Create(*Ctx,
                                   "seh.try.begin." + std::to_string(K) + "." +
                                       std::to_string(Boundary++),
                                   &Parent, E.Target);
      E.Term->setSuccessor(E.Index, BeginBB);
      llvm::IRBuilder<>(BeginBB).CreateInvoke(TryBegin, E.Target, R.UnwindDest);
      InParents(BeginBB);
    }
    if (EntersAtStart) {
      llvm::BasicBlock *OldEntry = &Parent.getEntryBlock();
      auto *BeginBB =
          llvm::BasicBlock::Create(*Ctx,
                                   "seh.try.begin." + std::to_string(K) + "." +
                                       std::to_string(Boundary++),
                                   &Parent, OldEntry);
      llvm::IRBuilder<>(BeginBB).CreateInvoke(TryBegin, OldEntry, R.UnwindDest);
      InParents(BeginBB);
    }
    llvm::SmallVector<Edge, 8> Exits;
    llvm::SmallVector<llvm::ReturnInst *, 4> Returns;
    for (llvm::BasicBlock *From : Blocks) {
      llvm::Instruction *Term = From->getTerminator();
      if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Term)) {
        Returns.push_back(Ret);
        continue;
      }
      for (unsigned I = 0; I < Term->getNumSuccessors(); ++I)
        if (IsNormalSuccessor(Term, I) &&
            !R.Blocks.count(Term->getSuccessor(I)))
          Exits.push_back({Term, Term->getSuccessor(I), I});
    }
    for (const Edge &E : Exits) {
      auto *EndBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.end." + std::to_string(K) + "." + std::to_string(Boundary++),
          &Parent, E.Target);
      E.Term->setSuccessor(E.Index, EndBB);
      llvm::IRBuilder<>(EndBB).CreateInvoke(TryEnd, E.Target, R.UnwindDest);
      InParents(EndBB);
    }
    for (llvm::ReturnInst *Ret : Returns) {
      llvm::BasicBlock *From = Ret->getParent();
      auto *EndBB =
          llvm::BasicBlock::Create(*Ctx,
                                   "seh.try.end." + std::to_string(K) +
                                       ".ret." + std::to_string(Boundary++),
                                   &Parent);
      auto *RetBB = llvm::BasicBlock::Create(
          *Ctx, "seh.try.ret." + std::to_string(K), &Parent);
      llvm::Value *Value = Ret->getReturnValue();
      Ret->eraseFromParent();
      llvm::IRBuilder<>(From).CreateBr(EndBB);
      llvm::IRBuilder<>(EndBB).CreateInvoke(TryEnd, RetBB, R.UnwindDest);
      llvm::IRBuilder<> RB(RetBB);
      if (Value)
        RB.CreateRet(Value);
      else
        RB.CreateRetVoid();
      InParents(EndBB);
    }
  }

  // Installing and removing the registration record is what `__try` says.
  for (llvm::BasicBlock &Block : Parent)
    for (llvm::Instruction &Inst : llvm::make_early_inc_range(Block))
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst);
          Store && isLLVMX86FSAddressSpace(Store->getPointerAddressSpace()) &&
          llvm::PatternMatch::match(Store->getPointerOperand(),
                                    llvm::PatternMatch::m_Zero())) {
        llvm::Value *Value = Store->getValueOperand();
        Store->eraseFromParent();
        llvm::RecursivelyDeleteTriviallyDeadInstructions(Value);
      }

  if (AsyncFlagState == med_llvm_eh::I32ModuleFlagState::Absent)
    Mod->addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  exception_rewrite::setContract(
      Parent, exception_rewrite::SourceState::Complete,
      exception_rewrite::LoweringState::Complete, Protected, Protected,
      /*SkippedPads=*/0);
  return true;
}

} // namespace neverd
