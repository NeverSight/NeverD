//===- RegistrationFrameProof.cpp - PE32 private frame proof ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate the complete current frame use closure for every native consumer.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/RegistrationFrameProof.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/llvm/RegistrationFrameBits.h"
#include "neverd/backend/llvm/RegistrationFrameProofAddress.h"
#include "neverd/backend/llvm/RegistrationFrameRuntime.h"
#include "neverd/backend/llvm/RegistrationFrameStores.h"
#include "neverd/backend/llvm/RegistrationFrameTaint.h"
#include "neverd/backend/llvm/RegistrationImageProof.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86RegistrationLayout.h"
#include "neverd/backend/llvm/X86RegistrationSEHJump.h"
#include "neverd/ir/RegistrationCall.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionCommon.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Errc.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd::coff_registration {

llvm::Error validateFramePrivacy(
    const llvm::Function &Parent,
    llvm::ArrayRef<const llvm::Function *> Callbacks,
    const llvm::AllocaInst *LogicalFrame,
    const std::map<const llvm::Function *, const llvm::StoreInst *>
        &ExceptionBridges,
    const std::set<const llvm::Instruction *> &IncomingAccesses,
    llvm::ArrayRef<ExceptionAddressRange> CallerPCWrites,
    const std::set<const llvm::Instruction *> &ChainProtocolReads,
    va_t SecurityCookieVA,
    llvm::ArrayRef<ExceptionAddressRange> ImmutableImageRanges,
    const RegistrationCxxFrameContract *Cxx,
    const RegistrationSEHExceptionContract *SEH,
    const RegistrationSEHJumpContract *Jumps,
    const RegistrationSEHScopeState *Scopes,
    const RegistrationSEHParentExitContract *ParentExits,
    const RegistrationSEHLocalUnwindContract *LocalUnwinds,
    const std::set<const llvm::LoadInst *> &ThreadReads) {
  const llvm::Instruction *CurrentInstruction = nullptr;
  auto Reject = [&](llvm::StringRef Detail = {}) {
    std::string Message = "coff registration patch: synthetic frame has an "
                          "unproved LLVM escape or access";
    llvm::raw_string_ostream Stream(Message);
    if (!Detail.empty())
      Stream << " (" << Detail << ')';
    if (CurrentInstruction) {
      Stream << " in " << CurrentInstruction->getFunction()->getName() << ": ";
      CurrentInstruction->print(Stream);
    }
    return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                   Message.c_str());
  };
  using registration_frame::Cell;
  const auto &Layout = Parent.getParent()->getDataLayout();
  std::vector<const llvm::Function *> Functions{&Parent};
  Functions.insert(Functions.end(), Callbacks.begin(), Callbacks.end());
  std::map<const llvm::Function *, std::unique_ptr<llvm::DominatorTree>>
      Dominators;
  for (const auto &[Function, Bridge] : ExceptionBridges)
    Dominators.emplace(Function, std::make_unique<llvm::DominatorTree>(
                                     *const_cast<llvm::Function *>(Function)));
  std::map<const llvm::Value *, const RegistrationCxxCatchFrameContract *>
      RuntimeObjects, CallbackStacks;
  std::map<const llvm::BasicBlock *, const RegistrationCxxCatchFrameContract *>
      Invocations;
  if (Cxx) {
    // A cleanup-only parent has no runtime catch objects. Its independently
    // replayed destructor borrows still require the complete frame privacy
    // and definite-initialization proof below.
    if (!Cxx->Image || !LogicalFrame ||
        (Cxx->Catches.empty() && Cxx->Borrows.empty()))
      return Reject("incomplete runtime object/frame contract");
    for (const auto &Catch : Cxx->Catches) {
      if (!Catch.Catch || Catch.Catch->getFunction() != &Parent ||
          Catch.HomeOffset < 0 ||
          (!Catch.ObjectSize && (Catch.HomeOffset || Catch.Reference)) ||
          !RuntimeObjects.emplace(Catch.Catch, &Catch).second ||
          (Catch.CallbackStack &&
           !CallbackStacks.emplace(Catch.CallbackStack, &Catch).second))
        return Reject("incomplete runtime catch/frame contract");
      for (const auto *Block : Catch.CallbackBlocks)
        if (!Invocations.emplace(Block, &Catch).second)
          return Reject(
              "callback block belongs to multiple runtime invocations");
    }
    for (const auto &[Instruction, Access] : Cxx->RuntimeAccesses)
      if (!RuntimeObjects.count(Access.Catch) ||
          !RuntimeObjects.at(Access.Catch)->Reference)
        return Reject("runtime object access has no reference catch owner");
    Dominators.try_emplace(&Parent,
                           std::make_unique<llvm::DominatorTree>(
                               *const_cast<llvm::Function *>(&Parent)));
  }
  auto CatchAt = [&](const llvm::Instruction &I) {
    const auto Found = Invocations.find(I.getParent());
    return Found == Invocations.end() ? nullptr : Found->second;
  };
  std::set<const llvm::Value *> Addresses;
  if (SEH)
    for (const auto &[Function, Bridge] : SEH->Bridges)
      Addresses.insert(Bridge->getValueOperand());
  const llvm::IntrinsicInst *Escape = nullptr;
  for (const auto *Function : Functions) {
    if (Function != &Parent && Function->arg_size() == 2)
      Addresses.insert(Function->getArg(1));
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (llvm::isa<llvm::AllocaInst>(I))
          Addresses.insert(&I);
        if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I)) {
          const auto ID = Call->getIntrinsicID();
          if (ID == llvm::Intrinsic::localescape && Function == &Parent)
            Escape = Call;
          if (ID == llvm::Intrinsic::localrecover ||
              ID == llvm::Intrinsic::frameaddress ||
              ID == llvm::Intrinsic::eh_recoverfp ||
              ID == llvm::Intrinsic::localaddress)
            Addresses.insert(Call);
        }
      }
  }
  size_t Work = 0;
  bool Exhausted = false;
  RegistrationFrameStores Stores;
  if (!Stores.build(Functions, Work))
    return Reject("private store/lifetime index has unsupported storage or "
                  "exhausted its work budget");
  std::set<const llvm::CallBase *> CookieCalls;
  std::vector<registration_frame::CookieDispatch> CookieDispatches;
  const bool HasCookies =
      Cxx && (!Cxx->CookieCalls.empty() || !Cxx->CookieDispatches.empty());
  if (HasCookies) {
    if (!SecurityCookieVA || SecurityCookieVA > UINT32_MAX - 3 || SEH ||
        Jumps || Scopes || ParentExits || LocalUnwinds)
      return Reject("cookie protocol has no independent C++ frame domain");
    for (const auto &Check : Cxx->CookieCalls) {
      if (++Work > limits::kMaxRegistrationEHStateWork || !Check.Call ||
          !Check.Call->getParent() || Check.Call->getFunction() != &Parent ||
          !Stores.contains(*Check.Call) ||
          !registration_frame::hasExactCookieCallABI(*Check.Call,
                                                     Check.CheckVA) ||
          Cxx->Borrows.count(Check.Call) ||
          Cxx->ImageWrites.count(Check.Call) ||
          !CookieCalls.insert(Check.Call).second)
        return Reject("cookie call has no unique current conditional ABI");
    }
    for (const auto &[Before, Guards] : Cxx->CookieDispatches) {
      const auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(Before);
      if (++Work > limits::kMaxRegistrationEHStateWork || !Call ||
          !Call->getParent() || Call->getFunction() != &Parent ||
          !Stores.contains(*Call) || CookieCalls.count(Call) || Guards.empty())
        return Reject("cookie dispatch has no current ordinary call");
      std::set<std::tuple<Cell, Cell, va_t>> Unique;
      for (const auto &Guard : Guards)
        if (++Work > limits::kMaxRegistrationEHStateWork ||
            Guard.Slot.Root != LogicalFrame ||
            Guard.XORBase.Root != LogicalFrame ||
            !Unique.emplace(Guard.Slot, Guard.XORBase, Guard.CheckVA).second)
          return Reject("cookie guard has no unique logical frame coordinate");
      CookieDispatches.push_back({Before, Guards});
    }
  }
  auto Bounded = [&](const Cell &Where, uint64_t Bytes) {
    if (const auto Found = RuntimeObjects.find(Where.Root);
        Found != RuntimeObjects.end()) {
      const auto &Catch = *Found->second;
      return Catch.Reference && Where.Offset >= 0 &&
             uint64_t(Where.Offset) <= Catch.ObjectSize &&
             Bytes <= Catch.ObjectSize - uint64_t(Where.Offset);
    }
    const auto *Root = llvm::dyn_cast<llvm::AllocaInst>(Where.Root);
    auto Extent = Root ? Root->getAllocationSize(Layout) : std::nullopt;
    return Root && Root->isStaticAlloca() && Extent && !Extent->isScalable() &&
           Where.Offset >= 0 &&
           uint64_t(Where.Offset) <= Extent->getFixedValue() &&
           Bytes <= Extent->getFixedValue() - Where.Offset;
  };
  auto ProjectLoad = [&](const llvm::LoadInst &Load,
                         registration_frame::AddressProof::Lookup Lookup)
      -> std::optional<Cell> {
    if (SEH)
      if (const auto *Root = SEH->pointerResult(Load))
        return Cell{Root, 0};
    auto Slot = Lookup(Load.getPointerOperand());
    const auto *Catch = CatchAt(Load);
    const auto *Home = Slot && Slot->Root == LogicalFrame
                           ? activeCxxReferenceHome(Cxx, Catch, Slot->Offset)
                           : nullptr;
    if (Home && Load.getType()->isIntegerTy(32) && !Load.isAtomic() &&
        Dominators.at(&Parent)->dominates(Home->Catch, &Load))
      return Cell{Home->Catch, 0};
    return std::nullopt;
  };
  auto Predecessors =
      [&](const llvm::BasicBlock *Block, const llvm::BasicBlock *Pred,
          llvm::SmallVectorImpl<const llvm::BasicBlock *> &Edges) {
        if (ParentExits && ParentExits->Dispatch &&
            Pred == ParentExits->Dispatch->getParent()) {
          const auto Resumes = ParentExits->ResumptionPredecessors.find(Block);
          if (Resumes == ParentExits->ResumptionPredecessors.end())
            return false;
          for (const auto *Transfer : Resumes->second)
            Edges.push_back(Transfer);
        } else
          Edges.push_back(Pred);
        return true;
      };
  auto ObjectBounds = [&](const Cell &Where) {
    if (llvm::isa<llvm::AllocaInst>(Where.Root) ||
        RuntimeObjects.count(Where.Root))
      return Bounded(Where, 0);
    // Opaque image symbols denote mapped storage whose bounds are checked
    // independently below, rather than a compiler allocation's declared type.
    return llvm::isa<llvm::GlobalVariable>(Where.Root);
  };
  std::function<bool(const Cell &, uint64_t)> SEHCallbacksPreserve;
  auto PreservesMemory = [&](const llvm::Instruction &I, const Cell &Slot,
                             uint64_t Bytes) {
    auto Disjoint = [&](const llvm::Value *Root, int64_t Offset,
                        uint64_t Width) {
      if (Offset < -int64_t(UINT32_MAX) || Offset > int64_t(UINT32_MAX) ||
          Width > UINT32_MAX)
        return false;
      return Root != Slot.Root || Offset >= Slot.Offset + int64_t(Bytes) ||
             Slot.Offset >= Offset + int64_t(Width);
    };
    if (const auto *Pad = llvm::dyn_cast<llvm::CatchPadInst>(&I)) {
      const auto Found = RuntimeObjects.find(Pad);
      if (Found == RuntimeObjects.end()) {
        const auto *Filter =
            Pad->arg_size() == 1
                ? llvm::dyn_cast<llvm::Function>(Pad->getArgOperand(0))
                : nullptr;
        return !Cxx && SEH && Scopes && Scopes->CatchReturns.count(Pad) &&
               Pad->getFunction() == &Parent && Filter &&
               llvm::is_contained(Callbacks, Filter) && SEHCallbacksPreserve &&
               SEHCallbacksPreserve(Slot, Bytes);
      }
      const auto &Catch = *Found->second;
      return Slot.Root != Catch.CallbackStack &&
             (!Catch.ObjectSize ||
              Disjoint(LogicalFrame, Catch.HomeOffset,
                       Catch.Reference ? 4 : Catch.ObjectSize));
    }
    if (const auto *Return = llvm::dyn_cast<llvm::CatchReturnInst>(&I)) {
      // The checked x86 SEH personality lowers this return to a branch.
      // Do not extend that property to C++ exception-object destruction or
      // to an unbound pad. Actual dispatch/callback writes are checked at
      // their own occurrences in this same reaching-definition walk.
      if (Cxx || !SEH || !Scopes || Return->getFunction() != &Parent)
        return false;
      const auto Found = Scopes->CatchReturns.find(Return->getCatchPad());
      return Found != Scopes->CatchReturns.end() && Found->second == Return;
    }
    const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
    if (!Call || Call->isInlineAsm() || llvm::isa<llvm::CallBrInst>(Call) ||
        llvm::isa<llvm::IntrinsicInst>(Call))
      return false;
    // This conditional frame-use assumption is discharged by the complete
    // cookie protocol below. Its fresh queries override every known checker:
    // unresolved checks cannot preserve memory merely through this hook.
    if (CookieCalls.count(Call))
      return true;
    if (Cxx) {
      const auto Found = Cxx->Borrows.find(Call);
      if (Found != Cxx->Borrows.end()) {
        const auto &Borrow = Found->second;
        if (Borrow.Offset < -int64_t(UINT32_MAX) ||
            Borrow.Offset > int64_t(UINT32_MAX))
          return false;
        const auto *Root = Borrow.Root ? Borrow.Root : LogicalFrame;
        for (const auto *Effects : {&Borrow.Writes, &Borrow.Initialized})
          for (const auto &Write : *Effects) {
            if (Work >= limits::kMaxRegistrationEHStateWork) {
              Exhausted = true;
              return false;
            }
            ++Work;
            if (Write.Begin >= Write.End ||
                !Disjoint(Root, Borrow.Offset + int64_t(Write.Begin),
                          uint64_t(int64_t(Write.End) - Write.Begin)))
              return false;
          }
        return true;
      }
    }
    if (Jumps) {
      if (Jumps->NoFrameBorrowCalls.count(Call))
        return !Cxx && SEH && Jumps->Calls.count(Call) &&
               SEHCallbacksPreserve && SEHCallbacksPreserve(Slot, Bytes);
      if (const auto Found = Jumps->RuntimeOutputs.find(Call);
          Found != Jumps->RuntimeOutputs.end()) {
        const auto &Output = Found->second;
        return !Cxx && Output.Frame == LogicalFrame && Output.Bytes &&
               Disjoint(Output.Frame, Output.Offset, Output.Bytes) &&
               SEHCallbacksPreserve && SEHCallbacksPreserve(Slot, Bytes);
      }
    }
    if ((LocalUnwinds && LocalUnwinds->Calls.count(Call)) ||
        (Jumps && Jumps->Calls.count(Call)) ||
        (ParentExits && ParentExits->Calls.count(Call)))
      return false;
    if (llvm::is_contained(Callbacks, Call->getCalledFunction()))
      return !Cxx && SEH && SEHCallbacksPreserve &&
             SEHCallbacksPreserve(Slot, Bytes);
    if (Scopes && Scopes->PhysicalWrites.count(Call))
      return false;
    // This is conditional on the complete use closure below: an ordinary
    // call cannot receive or retain any frame address. Calls admitted by a
    // separate frame protocol need that protocol's write projection instead.
    // Do not consult the growing Addresses set here: memoized answers must
    // not depend on the order of the taint fixed point.
    return true;
  };
  registration_frame::AddressProof AddressOwner(
      Parent, Escape, SecurityCookieVA, Stores, Work, Exhausted, ProjectLoad,
      Predecessors, ObjectBounds, PreservesMemory);
  auto Locate = [&](const llvm::Value *Value, unsigned Depth) {
    return AddressOwner.locate(Value, Depth);
  };
  std::map<std::pair<Cell, uint64_t>, bool> CallbackPreservation;
  SEHCallbacksPreserve = [&](const Cell &Slot, uint64_t Bytes) {
    const auto [Result, New] =
        CallbackPreservation.emplace(std::make_pair(Slot, Bytes), false);
    if (!New)
      return Result->second;
    // SEH dispatch and explicit finally calls can reach filters or termination
    // callbacks. Close over every current callback, not just the chosen one.
    // The use closure below independently checks the parent-frame call ABI.
    // In-progress queries stay false, so a cyclic memory dependency
    // cannot prove its own preservation. Unknown calls require a separate
    // transitive callback write contract.
    for (const auto *Callback : Callbacks)
      for (const auto &Block : *Callback)
        for (const auto &I : Block) {
          if (Work >= limits::kMaxRegistrationEHStateWork) {
            Exhausted = true;
            return false;
          }
          ++Work;
          if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
            const auto Destination = Locate(Store->getPointerOperand(), 0);
            const auto Written =
                Layout.getTypeStoreSize(Store->getValueOperand()->getType());
            if (!Destination || Store->isAtomic() || Written.isScalable() ||
                Written.getFixedValue() > UINT32_MAX || Bytes > UINT32_MAX ||
                Destination->Offset < -int64_t(UINT32_MAX) ||
                Destination->Offset > int64_t(UINT32_MAX) ||
                Slot.Offset < -int64_t(UINT32_MAX) ||
                Slot.Offset > int64_t(UINT32_MAX))
              return false;
            if (Destination->Root == Slot.Root &&
                Destination->Offset < Slot.Offset + int64_t(Bytes) &&
                Slot.Offset <
                    Destination->Offset + int64_t(Written.getFixedValue()))
              return false;
          } else if (!llvm::isa<llvm::LoadInst>(I) &&
                     (llvm::isa<llvm::CallBase, llvm::CatchPadInst>(I) ||
                      I.mayWriteToMemory())) {
            if (!llvm::isa<llvm::IntrinsicInst>(I) ||
                !AddressOwner.preservesMemory(I, Slot, Bytes))
              return false;
          }
        }
    Result->second = true;
    return true;
  };
  auto LiveAt = [&](const Cell &Where, const llvm::Instruction &Access) {
    const auto *Root = llvm::dyn_cast<llvm::AllocaInst>(Where.Root);
    return !Root || AddressOwner.isAliveAt(*Root, Access);
  };
  RegistrationFrameTaint TaintedMemory;
  std::set<const llvm::StoreInst *> TaintedStores;
  auto Size = [&](llvm::Type *Type) -> uint64_t {
    if (!Type->isSized())
      return UINT64_MAX;
    const auto Bytes = Layout.getTypeStoreSize(Type);
    return Bytes.isScalable() ? UINT64_MAX : Bytes.getFixedValue();
  };
  auto IsAddress = [&](const llvm::Value *Value) {
    return Addresses.count(Value);
  };
  auto CheckImageAccess = [&](const Cell &Where, uint64_t Bytes, bool Write) {
    const auto Access =
        registration_frame::imageRange(Where, Bytes, SecurityCookieVA);
    if (!Access)
      return false;
    const Segment *Owner = nullptr;
    for (const auto &Segment : Cxx->Image->Segments) {
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          Segment.VA > UINT32_MAX ||
          Segment.Size > uint64_t(UINT32_MAX) + 1 - Segment.VA)
        return false;
      if (Access->Begin < Segment.VA + Segment.Size &&
          Segment.VA < Access->End) {
        if (Owner || Access->Begin < Segment.VA ||
            Access->End > Segment.VA + Segment.Size ||
            (Write &&
             (!Segment.isWritable() || Segment.ReadOnlyAfterRelocations)))
          return false;
        Owner = &Segment;
      }
    }
    return Owner && (Write || Owner->isReadable());
  };
  bool Changed = true;
  while (Changed && !Exhausted) {
    Changed = false;
    for (const auto *Function : Functions)
      for (const auto &Block : *Function)
        for (const auto &I : Block) {
          CurrentInstruction = &I;
          const auto *Catch = CatchAt(I);
          if (++Work > limits::kMaxRegistrationEHStateWork)
            return Reject();
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
            auto Where = Locate(Load->getPointerOperand(), 0);
            if (Where && !LiveAt(*Where, *Load))
              return Reject("frame read is outside its allocation lifetime");
            // The caller binds these exact reads to freshly replayed source
            // and checks their real Self-derived addresses independently.
            if (ThreadReads.count(Load))
              continue;
            if (ChainProtocolReads.count(Load))
              continue;
            if (SEH && SEH->NullReads.count(Load))
              continue;
            if (SEH && (SEH->Reads.count(Load) ||
                        (Where && SEH->Roots.count(Where->Root)))) {
              if (!Where || !SEH->validRead(*Load, Where->Root, Where->Offset))
                return Reject("SEH exception read changed its runtime field");
              const auto *Bridge = SEH->Reads.at(Load).Bridge;
              if (!Dominators.at(Function)->dominates(Bridge, Load))
                return Reject("SEH exception read precedes its invocation");
              if (SEH->pointerResult(*Load))
                Changed |= Addresses.insert(Load).second;
              continue;
            }
            if (Cxx && Cxx->RuntimeAccesses.count(Load) &&
                (!Where || Where->Root != Cxx->RuntimeAccesses.at(Load).Catch))
              return Reject(
                  "runtime object read lost its checked pointer identity");
            if (Cxx && Where && RuntimeObjects.count(Where->Root)) {
              const auto Access = Cxx->RuntimeAccesses.find(Load);
              const auto *Owner = RuntimeObjects.at(Where->Root);
              if (!Catch || !isActiveCxxCatch(Owner->Catch, Catch->Catch) ||
                  Access == Cxx->RuntimeAccesses.end() ||
                  Access->second.Catch != Where->Root || Access->second.Write ||
                  Where->Offset != Access->second.Offset || Where->Offset < 0 ||
                  Size(Load->getType()) != Access->second.Width ||
                  uint64_t(Where->Offset) + Access->second.Width >
                      Owner->ObjectSize ||
                  !Dominators.at(&Parent)->dominates(Owner->Catch, Load))
                return Reject("runtime object read changed its checked domain");
              continue;
            }
            if (Where && CallbackStacks.count(Where->Root) &&
                CallbackStacks.at(Where->Root) != Catch)
              return Reject("callback stack read outlived its invocation");
            if (Jumps && Where && Where->Root == LogicalFrame &&
                Jumps->readsSourceBuffer(Where->Offset, Size(Load->getType())))
              return Reject("source reads physical jump buffer contents");
            const bool Private =
                Where && llvm::isa<llvm::AllocaInst>(Where->Root);
            if (Cxx && Where && llvm::isa<llvm::GlobalVariable>(Where->Root) &&
                !CheckImageAccess(*Where, Size(Load->getType()), false))
              return Reject("image read lost its original storage identity");
            if (!Private && Where) {
              const auto Read = registration_frame::imageRange(
                  *Where, Size(Load->getType()), SecurityCookieVA);
              if (!I.getMetadata(
                      windows_eh_md::RegistrationOperationAttachment) ||
                  !Read)
                return Reject("unindexed or wrapping image read");
              auto Write =
                  llvm::lower_bound(CallerPCWrites, Read->Begin,
                                    [](const auto &Range, uint64_t Begin) {
                                      return Range.End <= Begin;
                                    });
              if (Write != CallerPCWrites.end() && Write->Begin < Read->End)
                return Reject("caller-PC image slot is read");
            }
            if (!Where &&
                (IsAddress(Load->getPointerOperand()) ||
                 !I.getMetadata(
                     windows_eh_md::RegistrationOperationAttachment) ||
                 !CallerPCWrites.empty() || Cxx || SEH) &&
                !IncomingAccesses.count(Load)) {
              const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(
                  Load->getPointerOperand());
              const auto *Runtime = GEP ? llvm::dyn_cast<llvm::IntrinsicInst>(
                                              GEP->getPointerOperand())
                                        : nullptr;
              llvm::APInt Offset(32, 0);
              if (Function == &Parent || !Runtime ||
                  Runtime->getIntrinsicID() != llvm::Intrinsic::frameaddress ||
                  !GEP->accumulateConstantOffset(Layout, Offset) ||
                  Offset.getSExtValue() != -20 ||
                  !Load->getType()->isPointerTy())
                return Reject("unproved nonlocal read");
            }
            if (Private &&
                (!Bounded(*Where, Size(Load->getType())) || Load->isAtomic()))
              return Reject();
            if (Where && Where->Root == LogicalFrame) {
              auto Bridge = ExceptionBridges.find(Function);
              if (Bridge != ExceptionBridges.end() &&
                  !Dominators.at(Function)->dominates(Bridge->second, Load))
                return Reject();
              const auto *Home =
                  activeCxxReferenceHome(Cxx, Catch, Where->Offset);
              if (Home &&
                  Dominators.at(&Parent)->dominates(Home->Catch, Load)) {
                if (!Load->getType()->isIntegerTy(32))
                  return Reject("runtime reference home changed width");
                Changed |= Addresses.insert(Load).second;
              }
            }
            if (Private && !IsAddress(Load)) {
              const auto Tainted = TaintedMemory.overlaps(
                  Where->Root, Where->Offset, Size(Load->getType()), Work);
              if (!Tainted)
                return Reject("frame taint lookup exhausted its work budget");
              if (*Tainted)
                Changed |= Addresses.insert(Load).second;
            }
            continue;
          }
          if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
            auto Where = Locate(Store->getPointerOperand(), 0);
            if (Where && !LiveAt(*Where, *Store))
              return Reject("frame write is outside its allocation lifetime");
            if (ParentExits && ParentExits->ChainWrites.count(Store))
              continue;
            const uint64_t Bytes = Size(Store->getValueOperand()->getType());
            if (Scopes && Scopes->CallbackWrites.count(Store) &&
                (!Where || Where->Root != LogicalFrame ||
                 Where->Offset != Scopes->Offset ||
                 !Scopes->Writes.count(Store) ||
                 Bytes != Scopes->Writes.at(Store)))
              return Reject(
                  "callback state write changed its source frame cell");
            if (Scopes && Scopes->Enabled) {
              const bool State = Scopes->Writes.count(Store);
              const bool Overlap =
                  Where && Where->Root == LogicalFrame &&
                  Where->Offset < Scopes->Offset + 4 &&
                  (Where->Offset >= Scopes->Offset ||
                   Bytes > uint64_t(Scopes->Offset - Where->Offset));
              if ((State && (!Where || Where->Root != LogicalFrame ||
                             Where->Offset != Scopes->Offset ||
                             Bytes != Scopes->Writes.at(Store))) ||
                  (Overlap && !State))
                return Reject("dynamic scope state storage changed");
            }
            if (SEH && Where &&
                (SEH->Roots.count(Where->Root) ||
                 SEH->clobbersPointers(*Store, Where->Root, Where->Offset,
                                       Bytes)))
              return Reject("SEH exception runtime storage is overwritten");
            if (!CallbackStacks.empty()) {
              const auto Stored = Locate(Store->getValueOperand(), 0);
              if (Where && Where->Root == LogicalFrame &&
                  IsAddress(Store->getValueOperand()) && !Stored)
                return Reject("unproved pointer stored in the source frame");
              for (const auto &Address : {Where, Stored})
                if (Address && CallbackStacks.count(Address->Root) &&
                    CallbackStacks.at(Address->Root) != Catch &&
                    !(Cxx->SavedStackRestores.count(Store) && Catch &&
                      isActiveCxxCatch(CallbackStacks.at(Address->Root)->Catch,
                                       Catch->Catch)))
                  return Reject("callback stack write outlived its invocation");
              if (Stored && CallbackStacks.count(Stored->Root) && Where &&
                  Where->Root == LogicalFrame &&
                  (Where->Offset != Cxx->SavedStackOffset || Bytes != 4 ||
                   (!I.getMetadata(
                        windows_eh_md::RegistrationOperationAttachment) &&
                    !Cxx->SavedStackRestores.count(Store))))
                return Reject("callback stack escaped its SavedESP bridge");
            }
            if (Cxx && Where && llvm::isa<llvm::GlobalVariable>(Where->Root) &&
                !CheckImageAccess(*Where, Bytes, true))
              return Reject(
                  "image write lost its writable original storage identity");
            if (Cxx && Cxx->RuntimeAccesses.count(Store) &&
                (!Where || Where->Root != Cxx->RuntimeAccesses.at(Store).Catch))
              return Reject(
                  "runtime object write lost its checked pointer identity");
            if (Cxx && Where && RuntimeObjects.count(Where->Root)) {
              const auto Access = Cxx->RuntimeAccesses.find(Store);
              const auto *Owner = RuntimeObjects.at(Where->Root);
              if (!Catch || !isActiveCxxCatch(Owner->Catch, Catch->Catch) ||
                  Access == Cxx->RuntimeAccesses.end() ||
                  Access->second.Catch != Where->Root ||
                  !Access->second.Write ||
                  Where->Offset != Access->second.Offset || Where->Offset < 0 ||
                  Bytes != Access->second.Width ||
                  uint64_t(Where->Offset) + Access->second.Width >
                      Owner->ObjectSize ||
                  !Dominators.at(&Parent)->dominates(Owner->Catch, Store) ||
                  IsAddress(Store->getValueOperand()))
                return Reject(
                    "runtime object write changed its checked domain");
              continue;
            }
            const auto *Home =
                Where && Where->Root == LogicalFrame
                    ? activeCxxReferenceHome(Cxx, Catch, Where->Offset, Bytes)
                    : nullptr;
            if (Home && Dominators.at(&Parent)->dominates(Home->Catch, Store))
              return Reject("runtime reference home was overwritten");
            if (!Cxx && !ImmutableImageRanges.empty() &&
                !IncomingAccesses.count(Store) &&
                (!Where || llvm::isa<llvm::GlobalVariable>(Where->Root))) {
              const auto Written = Where ? registration_frame::imageRange(
                                               *Where, Bytes, SecurityCookieVA)
                                         : std::nullopt;
              if (!Written)
                return Reject("unproved registration image write");
              for (const auto &Range : ImmutableImageRanges) {
                if (++Work > limits::kMaxRegistrationEHStateWork)
                  return Reject("image closure exhausted its work budget");
                if (Written->overlaps(Range))
                  return Reject(
                      "registration scope table or cookie is written");
              }
            }
            if ((Where && llvm::isa<llvm::AllocaInst>(Where->Root) &&
                 (!Bounded(*Where, Bytes) || Store->isAtomic())) ||
                (!Where && IsAddress(Store->getPointerOperand()) &&
                 !IncomingAccesses.count(Store)))
              return Reject();
            if (Where && Where->Root == LogicalFrame) {
              auto Bridge = ExceptionBridges.find(Function);
              if (Bridge != ExceptionBridges.end() && Bridge->second != Store &&
                  !Dominators.at(Function)->dominates(Bridge->second, Store))
                return Reject();
            }
            if (IsAddress(Store->getValueOperand()) &&
                TaintedStores.insert(Store).second) {
              if (!Where || !Bounded(*Where, Bytes) || Store->isAtomic())
                return Reject();
              const auto Grew =
                  TaintedMemory.insert(Where->Root, Where->Offset, Bytes, Work);
              if (!Grew)
                return Reject("frame taint update exhausted its work budget");
              Changed |= *Grew;
            }
            continue;
          }
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (LocalUnwinds && LocalUnwinds->Calls.count(Call))
              continue;
            if (Jumps && Jumps->Calls.count(Call))
              continue;
            if (ParentExits && ParentExits->Calls.count(Call))
              continue;
            if (Call->isInlineAsm() || llvm::isa<llvm::CallBrInst>(Call) ||
                Call->isMustTailCall() ||
                Call->hasFnAttr(llvm::Attribute::ReturnsTwice))
              return Reject();
            // Defer only this exact source-bound sink. Its encoded/decoded
            // operands remain tainted in every store, call and other use.
            if (CookieCalls.count(Call))
              continue;
            const auto *Callee = Call->getCalledFunction();
            if (Cxx) {
              const auto Borrow = Cxx->Borrows.find(Call);
              if (Borrow != Cxx->Borrows.end()) {
                const auto &Checked = Borrow->second;
                const auto *Root = Checked.Root ? Checked.Root : LogicalFrame;
                const auto Runtime = RuntimeObjects.find(Root);
                if (Runtime != RuntimeObjects.end() &&
                    (!Runtime->second->Reference || Checked.Initializer ||
                     Checked.ThrowInfoVA || !Checked.Initialized.empty() ||
                     !Catch ||
                     !isActiveCxxCatch(Runtime->second->Catch, Catch->Catch)))
                  return Reject("callee borrows an inactive runtime object");
                const bool Throw = Checked.ThrowInfoVA != 0;
                const auto Where =
                    Call->arg_size() ==
                            (Throw ? 2u : 1u + Checked.ScalarStackWords)
                        ? Locate(Call->getArgOperand(0), 0)
                        : std::nullopt;
                if (!Where || Where->Root != Root ||
                    Where->Offset != Checked.Offset ||
                    Call->getCallingConv() !=
                        (Throw ? llvm::CallingConv::X86_StdCall
                               : llvm::CallingConv::X86_ThisCall) ||
                    (CallbackStacks.count(Root) &&
                     CallbackStacks.at(Root) != Catch))
                  return Reject(
                      "callee borrow changed its source frame object");
                if (Checked.ScalarStackWords)
                  for (unsigned Index = 1; Index < Call->arg_size(); ++Index) {
                    const auto *Argument = Call->getArgOperand(Index);
                    if (!Argument->getType()->isIntegerTy(32) ||
                        IsAddress(Argument) ||
                        llvm::isa<llvm::UndefValue, llvm::PoisonValue>(
                            Argument))
                      return Reject("member argument is not a defined scalar");
                  }
                if (Throw) {
                  const auto Table = Locate(Call->getArgOperand(1), 0);
                  const auto *Global =
                      Table ? llvm::dyn_cast<llvm::GlobalVariable>(Table->Root)
                            : nullptr;
                  const auto Base = Global
                                        ? parseNdDataSymbol(Global->getName())
                                        : std::nullopt;
                  if (!Table || !Base || *Base > UINT32_MAX ||
                      int64_t(*Base) + Table->Offset !=
                          int64_t(Checked.ThrowInfoVA) ||
                      !CheckImageAccess(*Table, 16, false))
                    return Reject(
                        "direct throw changed its original ThrowInfo");
                }
                for (const auto *Effects :
                     {&Checked.Reads, &Checked.Writes, &Checked.Initialized})
                  for (const auto &Effect : *Effects) {
                    const Cell Access{Where->Root,
                                      Where->Offset + Effect.Begin};
                    if (++Work > limits::kMaxRegistrationEHStateWork ||
                        Effect.Begin >= Effect.End ||
                        !Bounded(Access,
                                 uint64_t(int64_t(Effect.End) - Effect.Begin)))
                      return Reject(
                          "callee borrow leaves its allocated source frame");
                    if (!LiveAt(Access, *Call))
                      return Reject(
                          "callee borrow is outside its allocation lifetime");
                    if (Effects == &Borrow->second.Reads &&
                        !Checked.Initializer) {
                      const auto Tainted = TaintedMemory.overlaps(
                          Access.Root, Access.Offset,
                          uint64_t(int64_t(Effect.End) - Effect.Begin), Work);
                      if (!Tainted)
                        return Reject(
                            "callee value proof exhausted its work budget");
                      if (*Tainted)
                        return Reject("callee reads a pointer from its "
                                      "scalar object borrow");
                    }
                    const auto *Home =
                        Effects == &Borrow->second.Writes &&
                                Access.Root == LogicalFrame
                            ? activeCxxReferenceHome(
                                  Cxx, Catch, Access.Offset,
                                  uint64_t(int64_t(Effect.End) - Effect.Begin))
                            : nullptr;
                    if (Home &&
                        Dominators.at(&Parent)->dominates(Home->Catch, Call))
                      return Reject(
                          "callee overwrites the runtime reference home");
                  }
                continue;
              }
            }
            if (Scopes && Scopes->PhysicalWrites.count(Call))
              continue;
            if (Callee && Callee->isIntrinsic()) {
              switch (Callee->getIntrinsicID()) {
              case llvm::Intrinsic::stacksave:
              case llvm::Intrinsic::stackrestore:
              case llvm::Intrinsic::read_register:
              case llvm::Intrinsic::write_register:
              case llvm::Intrinsic::returnaddress:
              case llvm::Intrinsic::addressofreturnaddress:
              case llvm::Intrinsic::eh_sjlj_setjmp:
              case llvm::Intrinsic::eh_sjlj_longjmp:
                return Reject();
              case llvm::Intrinsic::localescape:
              case llvm::Intrinsic::localrecover:
              case llvm::Intrinsic::frameaddress:
              case llvm::Intrinsic::eh_recoverfp:
              case llvm::Intrinsic::localaddress:
              case llvm::Intrinsic::lifetime_start:
              case llvm::Intrinsic::lifetime_end:
              case llvm::Intrinsic::sideeffect:
              case llvm::Intrinsic::seh_scope_begin:
              case llvm::Intrinsic::seh_scope_end:
                continue;
              default:
                break;
              }
              if (Call->mayReadOrWriteMemory())
                return Reject();
            }
            if (Callee && llvm::is_contained(Callbacks, Callee)) {
              const auto *Abnormal = Call->arg_size() == 2
                                         ? llvm::dyn_cast<llvm::ConstantInt>(
                                               Call->getArgOperand(0))
                                         : nullptr;
              const auto *Frame = Call->arg_size() == 2
                                      ? llvm::dyn_cast<llvm::IntrinsicInst>(
                                            Call->getArgOperand(1))
                                      : nullptr;
              if (!Abnormal || Abnormal->getBitWidth() != 8 ||
                  Abnormal->getZExtValue() > 1 ||
                  Call->getCallingConv() != Callee->getCallingConv())
                return Reject();
              if (Function == &Parent) {
                if (!Frame ||
                    Frame->getIntrinsicID() != llvm::Intrinsic::localaddress)
                  return Reject("ordinary finally call changed its normal "
                                "parent-frame ABI");
              } else {
                // An outlined finally forwards only its physical parent
                // frame. Source occurrence, near return and callee ownership
                // are authenticated separately by bindSEHFinallyCalls.
                const auto *ParentFrame =
                    getX86RegistrationSEHJumpParentFrame(*Function, Parent);
                if (Cxx || !SEH || !Abnormal->isZero() ||
                    !Function->getReturnType()->isVoidTy() ||
                    Function->arg_size() != 2 || !ParentFrame ||
                    Call->getArgOperand(1) != ParentFrame ||
                    !llvm::isa<llvm::CallInst>(Call) ||
                    !Call->getMetadata(
                        windows_eh_md::RegistrationFinallyCallAttachment))
                  return Reject();
              }
              continue;
            }
            if (llvm::any_of(Call->operands(), [&](const auto &Use) {
                  return IsAddress(Use.get());
                }))
              return Reject();
            continue;
          }
          if (llvm::isa<llvm::AtomicRMWInst>(I) ||
              llvm::isa<llvm::AtomicCmpXchgInst>(I))
            return Reject();
          if (RuntimeObjects.count(&I)) {
            const auto &Catch = *RuntimeObjects.at(&I);
            if (Catch.ObjectSize &&
                !LiveAt({LogicalFrame, Catch.HomeOffset}, I))
              return Reject(
                  "runtime catch write is outside its allocation lifetime");
            continue;
          }
          if (I.isTerminator()) {
            if (llvm::any_of(I.operands(), [&](const auto &Use) {
                  return IsAddress(Use.get());
                }))
              return Reject();
            continue;
          }
          if (!I.getType()->isVoidTy() &&
              llvm::any_of(I.operands(), [&](const auto &Use) {
                return IsAddress(Use.get());
              })) {
            if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I);
                Cast && Cast->hasPoisonGeneratingFlags())
              return Reject();
            if (const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I);
                GEP && (GEP->isInBounds() || GEP->hasNoUnsignedSignedWrap())) {
              auto Where = Locate(GEP, 0);
              if (!Where || !Bounded(*Where, 0))
                return Reject();
            }
            if (const auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(&I);
                Binary &&
                (Binary->hasNoSignedWrap() || Binary->hasNoUnsignedWrap()))
              return Reject();
            if (const auto *Trunc = llvm::dyn_cast<llvm::TruncInst>(&I)) {
              const auto Depends =
                  truncationMayDependOnFrame(*Trunc, IsAddress, Stores, Work);
              if (!Depends)
                return Reject("register slice proof exhausted its work budget");
              if (!*Depends)
                continue;
            }
            Changed |= Addresses.insert(&I).second;
          }
        }
  }
  if (Exhausted)
    return Reject("address proof exhausted its work budget");
  if (Cxx) {
    // The complete use closure above proves these runtime stores stay within
    // their original stack/exception objects. Image effects of calls require
    // their separate source replay; the frame-preservation hook is
    // insufficient.
    auto SourcePreservesImage = [&](const llvm::Instruction &I,
                                    const ExceptionAddressRange &Range) {
      // localescape records compiler frame offsets; the checked C++ frame
      // and complete callback closure own its escaped storage, not the image.
      if (&I == Escape)
        return true;
      if (llvm::isa<llvm::StoreInst>(I))
        return IncomingAccesses.count(&I) || Cxx->RuntimeAccesses.count(&I);
      if (llvm::isa<llvm::CatchPadInst>(I))
        return RuntimeObjects.count(&I) != 0;
      // A source-bound catch return completes the checked runtime invocation.
      // Its exception copy/destruction effects are included in the throw call
      // summaries, which the whole-image query still checks independently.
      if (const auto *Return = llvm::dyn_cast<llvm::CatchReturnInst>(&I))
        return Cxx->CatchReturns.count(Return) &&
               RuntimeObjects.count(Return->getCatchPad());
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
      if (!Call || Call->isInlineAsm() || llvm::isa<llvm::CallBrInst>(Call) ||
          llvm::isa<llvm::IntrinsicInst>(Call))
        return false;
      const auto Effects = Cxx->ImageWrites.find(Call);
      if (Effects == Cxx->ImageWrites.end())
        return false;
      for (const auto &Write : Effects->second) {
        if (Work >= limits::kMaxRegistrationEHStateWork) {
          Exhausted = true;
          return false;
        }
        ++Work;
        if (!Write.isValid() || Write.End > uint64_t(UINT32_MAX) + 1 ||
            Write.overlaps(Range))
          return false;
      }
      return true;
    };
    if (HasCookies) {
      registration_frame::CookieProofContext Context{
          Parent,           Escape,
          Stores,           Functions,
          SecurityCookieVA, ProjectLoad,
          Predecessors,     ObjectBounds,
          PreservesMemory,  SourcePreservesImage};
      CurrentInstruction = nullptr;
      if (!registration_frame::proveCookieProtocol(
              *Cxx->Image, Context, Cxx->CookieCalls, CookieDispatches, Work,
              Exhausted))
        return Reject("C++ current cookie protocol is not established");
    }
    // Only the completed protocol above authorizes successful checker effects
    // for the remaining immutable runtime image. Ordinary calls retain their
    // independently replayed source summaries.
    auto PreservesImage = [&](const llvm::Instruction &I,
                              const ExceptionAddressRange &Range) {
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
      return (Call && CookieCalls.count(Call)) ||
             SourcePreservesImage(I, Range);
    };
    registration_frame::ImageProof ImageOwner(AddressOwner, Functions,
                                              SecurityCookieVA, Work, Exhausted,
                                              PreservesImage);
    for (const auto &Range : ImmutableImageRanges) {
      CurrentInstruction = nullptr;
      if (!ImageOwner.preserves(Range))
        return Reject("C++ current writes do not preserve the original "
                      "runtime image");
    }
    // Recompute definite initialization from actual LLVM stores and the CRT
    // catch write. Only independently proved constructor must-writes seed
    // bytes after calls; ordinary callee write summaries remain may-effects.
    std::map<const llvm::Instruction *, std::vector<Cell>> RequiredBytes;
    std::map<const llvm::Instruction *, std::vector<std::pair<Cell, uint64_t>>>
        Writes;
    std::set<Cell> Bytes;
    auto Require = [&](const llvm::Instruction *I, Cell Address,
                       uint64_t Size) {
      if (Work > limits::kMaxRegistrationEHStateWork ||
          Size > limits::kMaxRegistrationEHStateWork - Work)
        return false;
      Work += Size;
      for (uint64_t Index = 0; Index != Size; ++Index) {
        const Cell Byte{Address.Root, Address.Offset + int64_t(Index)};
        RequiredBytes[I].push_back(Byte);
        Bytes.insert(Byte);
      }
      return true;
    };
    for (const auto &Block : Parent)
      for (const auto &I : Block) {
        CurrentInstruction = &I;
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return Reject("initialization proof exhausted its work budget");
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
          const auto Where = Locate(Load->getPointerOperand(), 0);
          if (Where &&
              (Where->Root == LogicalFrame ||
               CallbackStacks.count(Where->Root)) &&
              !Require(Load, *Where, Size(Load->getType())))
            return Reject("frame read initialization exceeds its work budget");
        }
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
          const auto Where = Locate(Store->getPointerOperand(), 0);
          if (Where && (Where->Root == LogicalFrame ||
                        CallbackStacks.count(Where->Root))) {
            if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(
                    Store->getValueOperand()))
              return Reject(
                  "undefined value initializes a source frame object");
            Writes[Store].emplace_back(
                *Where, Size(Store->getValueOperand()->getType()));
          }
        }
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
          const auto Borrow = Cxx->Borrows.find(Call);
          if (Borrow != Cxx->Borrows.end()) {
            const auto &Checked = Borrow->second;
            const auto *Root = Checked.Root ? Checked.Root : LogicalFrame;
            if (RuntimeObjects.count(Root))
              continue;
            if (Checked.Initializer)
              for (const auto &Write : Checked.Initialized) {
                if (++Work > limits::kMaxRegistrationEHStateWork)
                  return Reject(
                      "constructor initialization exceeds its work budget");
                Writes[Call].emplace_back(
                    Cell{Root, Checked.Offset + Write.Begin},
                    uint64_t(int64_t(Write.End) - Write.Begin));
              }
            else
              for (const auto &Read : Checked.Reads)
                if (!Require(Call, {Root, Checked.Offset + Read.Begin},
                             uint64_t(int64_t(Read.End) - Read.Begin)))
                  return Reject(
                      "callee initialization exceeds its work budget");
          }
        }
      }
    for (const auto &Catch : Cxx->Catches)
      if (Catch.ObjectSize)
        Writes[Catch.Catch].emplace_back(Cell{LogicalFrame, Catch.HomeOffset},
                                         Catch.Reference ? 4
                                                         : Catch.ObjectSize);
    std::map<Cell, unsigned> Indices;
    for (Cell Byte : Bytes)
      Indices.emplace(Byte, Indices.size());
    using Initialized = llvm::BitVector;
    std::map<const llvm::BasicBlock *, Initialized> Normal, Unwind;
    for (const auto &Block : Parent) {
      Normal.emplace(&Block, Initialized(Bytes.size(), true));
      Unwind.emplace(&Block, Initialized(Bytes.size(), true));
    }
    auto Incoming = [&](const llvm::BasicBlock &Block) {
      Initialized State(Bytes.size(), &Block != &Parent.getEntryBlock());
      if (&Block != &Parent.getEntryBlock())
        for (const auto *Predecessor : llvm::predecessors(&Block)) {
          const auto *Invoke =
              llvm::dyn_cast<llvm::InvokeInst>(Predecessor->getTerminator());
          State &= Invoke && Invoke->getUnwindDest() == &Block
                       ? Unwind.at(Predecessor)
                       : Normal.at(Predecessor);
        }
      return State;
    };
    auto Seed = [&](Initialized &State, const llvm::Instruction &I) {
      auto Reset = [&](const llvm::Value *Root) {
        for (const auto &[Byte, Index] : Indices) {
          if (++Work > limits::kMaxRegistrationEHStateWork)
            return false;
          if (Byte.Root == Root)
            State.reset(Index);
        }
        return true;
      };
      // A start creates uninitialized contents even if the object was already
      // alive. Ending and starting it again cannot revive an old must-write.
      if (const auto *Lifetime = llvm::dyn_cast<llvm::LifetimeIntrinsic>(&I))
        if (!Reset(Lifetime->getArgOperand(0)))
          return false;
      const auto Found = RuntimeObjects.find(&I);
      if (Found != RuntimeObjects.end() && Found->second->CallbackStack)
        if (!Reset(Found->second->CallbackStack))
          return false;
      const auto Write = Writes.find(&I);
      if (Write == Writes.end())
        return true;
      for (const auto &[Address, Width] : Write->second)
        for (auto It = Indices.lower_bound(Address);
             It != Indices.end() && It->first.Root == Address.Root &&
             It->first.Offset < Address.Offset + int64_t(Width);
             ++It) {
          if (++Work > limits::kMaxRegistrationEHStateWork)
            return false;
          State.set(It->second);
        }
      return true;
    };
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (const auto &Block : Parent) {
        if (Work > limits::kMaxRegistrationEHStateWork ||
            Bytes.size() > limits::kMaxRegistrationEHStateWork - Work)
          return Reject("initialization joins exhausted their work budget");
        Work += Bytes.size();
        auto State = Incoming(Block);
        Initialized BeforeUnwind = State;
        for (const auto &I : Block) {
          if (llvm::isa<llvm::InvokeInst>(I))
            BeforeUnwind = State;
          if (++Work > limits::kMaxRegistrationEHStateWork || !Seed(State, I))
            return Reject("initialization transfer exhausted its work budget");
        }
        if (Normal.at(&Block) != State || Unwind.at(&Block) != BeforeUnwind) {
          Normal[&Block] = std::move(State);
          Unwind[&Block] = std::move(BeforeUnwind);
          Changed = true;
        }
      }
    }
    for (const auto &Block : Parent) {
      auto State = Incoming(Block);
      for (const auto &I : Block) {
        CurrentInstruction = &I;
        const auto Required = RequiredBytes.find(&I);
        if (Required != RequiredBytes.end())
          for (Cell Byte : Required->second) {
            if (++Work > limits::kMaxRegistrationEHStateWork ||
                !State.test(Indices.at(Byte)))
              return Reject("source frame read or callee borrow is not "
                            "initialized on every path");
          }
        if (!Seed(State, I))
          return Reject("initialization replay exhausted its work budget");
      }
    }
  }
  return llvm::Error::success();
}
} // namespace neverd::coff_registration
