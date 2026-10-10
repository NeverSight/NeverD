//===- COFFRegistrationFrameProof.cpp - PE32 private frame proof ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationFrameProof.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/llvm/RegistrationFrameAddress.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86RegistrationLayout.h"
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
    const RegistrationCxxFrameContract *Cxx) {
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
  struct Cell {
    const llvm::Value *Root;
    int64_t Offset;
    bool operator==(const Cell &) const = default;
    bool operator<(const Cell &Other) const {
      return Root != Other.Root
                 ? std::less<const llvm::Value *>{}(Root, Other.Root)
                 : Offset < Other.Offset;
    }
  };
  const auto &Layout = Parent.getParent()->getDataLayout();
  std::vector<const llvm::Function *> Functions{&Parent};
  Functions.insert(Functions.end(), Callbacks.begin(), Callbacks.end());
  std::map<const llvm::Function *, std::unique_ptr<llvm::DominatorTree>>
      Dominators;
  for (const auto &[Function, Bridge] : ExceptionBridges)
    Dominators.emplace(Function, std::make_unique<llvm::DominatorTree>(
                                     *const_cast<llvm::Function *>(Function)));
  if (Cxx) {
    if (!Cxx->Image || !Cxx->Catch || Cxx->Catch->getFunction() != &Parent ||
        !LogicalFrame || !Cxx->ObjectSize || Cxx->HomeOffset < 0)
      return Reject("incomplete runtime object/frame contract");
    Dominators.try_emplace(&Parent,
                           std::make_unique<llvm::DominatorTree>(
                               *const_cast<llvm::Function *>(&Parent)));
  }
  std::set<const llvm::Value *> Addresses;
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
  std::map<const llvm::Value *, Cell> Located;
  std::function<std::optional<Cell>(const llvm::Value *, unsigned)> Locate,
      LocateImpl;
  Locate = [&](const llvm::Value *Value,
               unsigned Depth) -> std::optional<Cell> {
    if (++Work > limits::kMaxRegistrationEHStateWork || Depth > 128) {
      Exhausted = true;
      return std::nullopt;
    }
    auto Found = Located.find(Value);
    if (Found != Located.end())
      return Found->second;
    auto Where = LocateImpl(Value, Depth);
    if (Where)
      Located.emplace(Value, *Where);
    return Where;
  };
  LocateImpl = [&](const llvm::Value *Value,
                   unsigned Depth) -> std::optional<Cell> {
    if (++Work > limits::kMaxRegistrationEHStateWork || Depth > 128) {
      Exhausted = true;
      return std::nullopt;
    }
    if (const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Value))
      return Cell{Slot, 0};
    if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value);
        Global &&
        (parseNdDataSymbol(Global->getName()) ||
         parseNdCodePtrSymbol(Global->getName()) ||
         (SecurityCookieVA && Global->getName() == "__security_cookie")))
      return Cell{Global, 0};
    if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(Value);
        Call && Call->getIntrinsicID() == llvm::Intrinsic::localrecover &&
        Escape && Call->arg_size() == 3 && Call->getArgOperand(0) == &Parent) {
      const auto *Index =
          llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(2));
      if (Index && Index->getValue().ult(Escape->arg_size()))
        return Locate(Escape->getArgOperand(Index->getZExtValue()), Depth + 1);
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::Operator>(Value);
        Cast && llvm::Instruction::isCast(Cast->getOpcode())) {
      if (const auto *Instruction = llvm::dyn_cast<llvm::CastInst>(Value);
          Instruction && Instruction->hasPoisonGeneratingFlags())
        return std::nullopt;
      const auto *From = Cast->getOperand(0)->getType();
      const auto *To = Value->getType();
      auto FullInteger = [](const llvm::Type *Type) {
        return Type->isIntegerTy() && Type->getIntegerBitWidth() >= 32;
      };
      auto PE32Pointer = [](const llvm::Type *Type) {
        return Type->isPointerTy() && Type->getPointerAddressSpace() == 0;
      };
      switch (Cast->getOpcode()) {
      case llvm::Instruction::PtrToInt:
        if (!PE32Pointer(From) || !FullInteger(To))
          return std::nullopt;
        break;
      case llvm::Instruction::IntToPtr:
        if (!FullInteger(From) || !PE32Pointer(To))
          return std::nullopt;
        break;
      case llvm::Instruction::Trunc:
      case llvm::Instruction::ZExt:
      case llvm::Instruction::SExt:
        if (!FullInteger(From) || !FullInteger(To))
          return std::nullopt;
        break;
      case llvm::Instruction::BitCast:
        if (!PE32Pointer(From) || !PE32Pointer(To))
          return std::nullopt;
        break;
      default:
        return std::nullopt;
      }
      return Locate(Cast->getOperand(0), Depth + 1);
    }
    if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Value)) {
      auto Offset = registration_frame::checkedByteGEPOffset(GEP);
      auto Base = Locate(GEP->getPointerOperand(), Depth + 1);
      if (Base && llvm::isa<llvm::GlobalVariable>(Base->Root)) {
        llvm::APInt Bytes(32, 0);
        if (GEP->getPointerAddressSpace() == 0 &&
            GEP->accumulateConstantOffset(Layout, Bytes))
          Offset = Bytes.getSExtValue();
      }
      if (Base && Offset) {
        Base->Offset += *Offset;
        return Base;
      }
    }
    if (const auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(Value)) {
      const auto *Amount =
          llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(1));
      if (Amount && Amount->getBitWidth() == 32 &&
          Binary->getOpcode() == llvm::Instruction::And &&
          !Binary->hasPoisonGeneratingFlags()) {
        auto Base = Locate(Binary->getOperand(0), Depth + 1);
        const auto *Root =
            Base ? llvm::dyn_cast<llvm::AllocaInst>(Base->Root) : nullptr;
        auto Offset = Root ? alignX86RegistrationOffset(
                                 Base->Offset, Root->getAlign().value(),
                                 uint32_t(Amount->getZExtValue()))
                           : std::nullopt;
        if (Offset)
          return Cell{Root, *Offset};
      }
      if (Amount && Amount->getBitWidth() <= 64 &&
          (Binary->getOpcode() == llvm::Instruction::Add ||
           Binary->getOpcode() == llvm::Instruction::Sub)) {
        auto Base = Locate(Binary->getOperand(0), Depth + 1);
        const int64_t Delta = Amount->getSExtValue();
        if (Base && Delta >= -int64_t(UINT32_MAX) &&
            Delta <= int64_t(UINT32_MAX) &&
            Base->Offset >= -int64_t(UINT32_MAX) &&
            Base->Offset <= int64_t(UINT32_MAX)) {
          // Integer address arithmetic is reduced only at the eventual
          // PE32 pointer boundary. A zero-extended -4 is 0xfffffffc, whose
          // effective byte displacement is still -4, without no-wrap flags.
          const uint32_t Next = uint32_t(Base->Offset) +
                                (Binary->getOpcode() == llvm::Instruction::Add
                                     ? uint32_t(Delta)
                                     : uint32_t(0) - uint32_t(Delta));
          Base->Offset = int64_t(llvm::APInt(32, Next).getSExtValue());
          return Base;
        }
      }
    }
    // Follow every reaching definition of an SSA register/PHI spill. All
    // normal paths must establish the same address before this exact load;
    // an entry zero or an unwritten path cannot stand in for a frame address.
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
      auto Slot = Locate(Load->getPointerOperand(), Depth + 1);
      if (Cxx && Cxx->Reference && Slot && Slot->Root == LogicalFrame &&
          Slot->Offset == Cxx->HomeOffset && Load->getType()->isIntegerTy(32) &&
          !Load->isAtomic() &&
          Dominators.at(&Parent)->dominates(Cxx->Catch, Load))
        return Cell{Cxx->Catch, 0};
      if (!Slot || !llvm::isa<llvm::AllocaInst>(Slot->Root))
        return std::nullopt;
      std::vector<
          std::pair<const llvm::BasicBlock *, const llvm::Instruction *>>
          Pending{{Load->getParent(), Load->getPrevNode()}};
      std::set<std::pair<const llvm::BasicBlock *, const llvm::Instruction *>>
          Seen;
      std::optional<Cell> Common;
      while (!Pending.empty()) {
        auto [Block, Last] = Pending.back();
        Pending.pop_back();
        // The initial prefix ends immediately before the load. A backedge
        // revisits the same block's suffix, which may overwrite this cell.
        if (!Seen.emplace(Block, Last).second)
          continue;
        bool Found = false;
        for (const auto *I = Last; I; I = I->getPrevNode()) {
          if (++Work > limits::kMaxRegistrationEHStateWork) {
            Exhausted = true;
            return std::nullopt;
          }
          const auto *Store = llvm::dyn_cast<llvm::StoreInst>(I);
          if (!Store)
            continue;
          auto Destination = Locate(Store->getPointerOperand(), Depth + 1);
          if (!Destination || Destination->Root != Slot->Root)
            continue;
          const auto Written =
              Layout.getTypeStoreSize(Store->getValueOperand()->getType());
          const auto Read = Layout.getTypeStoreSize(Load->getType());
          if (Written.isScalable() || Read.isScalable())
            return std::nullopt;
          if (Destination->Offset >=
                  Slot->Offset + int64_t(Read.getFixedValue()) ||
              Slot->Offset >=
                  Destination->Offset + int64_t(Written.getFixedValue()))
            continue;
          if (*Destination != *Slot || Written != Read || Store->isAtomic())
            return std::nullopt;
          auto Address = Locate(Store->getValueOperand(), Depth + 1);
          if (!Address || (Common && *Common != *Address))
            return std::nullopt;
          Common = Address;
          Found = true;
          break;
        }
        if (Found)
          continue;
        if (llvm::pred_empty(Block))
          return std::nullopt;
        for (const auto *Pred : llvm::predecessors(Block))
          Pending.emplace_back(Pred, Pred->getTerminator());
      }
      return Common;
    }
    return std::nullopt;
  };
  std::map<Cell, uint64_t> TaintedMemory;
  auto Size = [&](llvm::Type *Type) -> uint64_t {
    if (!Type->isSized())
      return UINT64_MAX;
    const auto Bytes = Layout.getTypeStoreSize(Type);
    return Bytes.isScalable() ? UINT64_MAX : Bytes.getFixedValue();
  };
  auto Bounded = [&](const Cell &Where, uint64_t Bytes) {
    if (Cxx && Where.Root == Cxx->Catch)
      return Cxx->Reference && Where.Offset >= 0 &&
             uint64_t(Where.Offset) <= Cxx->ObjectSize &&
             Bytes <= Cxx->ObjectSize - uint64_t(Where.Offset);
    const auto *Root = llvm::dyn_cast<llvm::AllocaInst>(Where.Root);
    auto Extent = Root ? Root->getAllocationSize(Layout) : std::nullopt;
    return Root && Root->isStaticAlloca() && Extent && !Extent->isScalable() &&
           Where.Offset >= 0 &&
           uint64_t(Where.Offset) <= Extent->getFixedValue() &&
           Bytes <= Extent->getFixedValue() - Where.Offset;
  };
  auto IsAddress = [&](const llvm::Value *Value) {
    return Addresses.count(Value);
  };
  auto CheckImageAccess = [&](const Cell &Where, uint64_t Bytes, bool Write) {
    const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Where.Root);
    auto Base = Global ? parseNdDataSymbol(Global->getName())
                       : std::optional<uint64_t>{};
    if (!Base && Global)
      Base = parseNdCodePtrSymbol(Global->getName());
    if (!Base || *Base > UINT32_MAX || Where.Offset < -int64_t(*Base))
      return false;
    const int64_t Address = int64_t(*Base) + Where.Offset;
    if (Address < 0 || Address > UINT32_MAX ||
        Bytes > uint64_t(UINT32_MAX) + 1 - uint64_t(Address) ||
        !Global->isDeclaration() || !Global->hasExternalLinkage() ||
        Global->isThreadLocal() || Global->getAddressSpace() ||
        Global->hasDLLImportStorageClass())
      return false;
    const Segment *Owner = nullptr;
    for (const auto &Segment : Cxx->Image->Segments) {
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          Segment.VA > UINT32_MAX ||
          Segment.Size > uint64_t(UINT32_MAX) + 1 - Segment.VA)
        return false;
      if (uint64_t(Address) < Segment.VA + Segment.Size &&
          Segment.VA < uint64_t(Address) + Bytes) {
        if (Owner || Address < int64_t(Segment.VA) ||
            uint64_t(Address) - Segment.VA > Segment.Size ||
            Bytes > Segment.Size - (uint64_t(Address) - Segment.VA) ||
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
          if (++Work > limits::kMaxRegistrationEHStateWork)
            return Reject();
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
            if (ChainProtocolReads.count(Load))
              continue;
            auto Where = Locate(Load->getPointerOperand(), 0);
            if (Cxx && Cxx->RuntimeAccesses.count(Load) &&
                (!Where || Where->Root != Cxx->Catch))
              return Reject(
                  "runtime object read lost its checked pointer identity");
            if (Cxx && Where && Where->Root == Cxx->Catch) {
              const auto Access = Cxx->RuntimeAccesses.find(Load);
              if (Access == Cxx->RuntimeAccesses.end() ||
                  Access->second.Write ||
                  Where->Offset != Access->second.Offset || Where->Offset < 0 ||
                  Size(Load->getType()) != Access->second.Width ||
                  uint64_t(Where->Offset) + Access->second.Width >
                      Cxx->ObjectSize ||
                  !Dominators.at(&Parent)->dominates(Cxx->Catch, Load))
                return Reject("runtime object read changed its checked domain");
              continue;
            }
            if (Cxx && Where && Where->Root == Cxx->CallbackStack &&
                !Cxx->CallbackBlocks.count(&Block))
              return Reject("callback stack read outlived its invocation");
            const bool Private =
                Where && llvm::isa<llvm::AllocaInst>(Where->Root);
            if (Cxx && Where && llvm::isa<llvm::GlobalVariable>(Where->Root) &&
                !CheckImageAccess(*Where, Size(Load->getType()), false))
              return Reject("image read lost its original storage identity");
            if (!Private && Where) {
              const auto *Global =
                  llvm::cast<llvm::GlobalVariable>(Where->Root);
              auto Base = parseNdDataSymbol(Global->getName());
              if (!Base)
                Base = parseNdCodePtrSymbol(Global->getName());
              if (!Base && SecurityCookieVA &&
                  Global->getName() == "__security_cookie")
                Base = SecurityCookieVA;
              if (!Base || *Base > UINT32_MAX)
                return Reject();
              const int64_t Address = int64_t(*Base) + Where->Offset;
              const uint64_t Bytes = Size(Load->getType());
              if (!I.getMetadata(
                      windows_eh_md::RegistrationOperationAttachment) ||
                  Address < 0 || Address > UINT32_MAX ||
                  Bytes > uint64_t(UINT32_MAX) + 1 - uint64_t(Address))
                return Reject("unindexed or wrapping image read");
              auto Write =
                  llvm::lower_bound(CallerPCWrites, uint64_t(Address),
                                    [](const auto &Range, uint64_t Begin) {
                                      return Range.End <= Begin;
                                    });
              if (Write != CallerPCWrites.end() &&
                  Write->Begin < uint64_t(Address) + Bytes)
                return Reject("caller-PC image slot is read");
            }
            if (!Where &&
                (IsAddress(Load->getPointerOperand()) ||
                 !I.getMetadata(
                     windows_eh_md::RegistrationOperationAttachment) ||
                 !CallerPCWrites.empty() || Cxx) &&
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
              if (Cxx && Cxx->Reference && Where->Offset == Cxx->HomeOffset &&
                  Dominators.at(&Parent)->dominates(Cxx->Catch, Load)) {
                if (!Load->getType()->isIntegerTy(32))
                  return Reject("runtime reference home changed width");
                Changed |= Addresses.insert(Load).second;
              }
            }
            if (Where)
              for (const auto &[Stored, Bytes] : TaintedMemory) {
                if (++Work > limits::kMaxRegistrationEHStateWork)
                  return Reject();
                if (Stored.Root == Where->Root &&
                    Where->Offset < Stored.Offset + int64_t(Bytes) &&
                    Stored.Offset <
                        Where->Offset + int64_t(Size(Load->getType())))
                  Changed |= Addresses.insert(Load).second;
              }
            continue;
          }
          if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
            auto Where = Locate(Store->getPointerOperand(), 0);
            const uint64_t Bytes = Size(Store->getValueOperand()->getType());
            if (Cxx && Cxx->CallbackStack) {
              const auto Stored = Locate(Store->getValueOperand(), 0);
              if (Where && Where->Root == LogicalFrame &&
                  IsAddress(Store->getValueOperand()) && !Stored)
                return Reject("unproved pointer stored in the source frame");
              if ((Where && Where->Root == Cxx->CallbackStack ||
                   Stored && Stored->Root == Cxx->CallbackStack) &&
                  !Cxx->CallbackBlocks.count(&Block))
                return Reject("callback stack write outlived its invocation");
              if (Stored && Stored->Root == Cxx->CallbackStack && Where &&
                  Where->Root == LogicalFrame &&
                  (Where->Offset != Cxx->SavedStackOffset || Bytes != 4 ||
                   !I.getMetadata(
                       windows_eh_md::RegistrationOperationAttachment)))
                return Reject("callback stack escaped its SavedESP bridge");
            }
            if (Cxx && Where && llvm::isa<llvm::GlobalVariable>(Where->Root) &&
                !CheckImageAccess(*Where, Bytes, true))
              return Reject(
                  "image write lost its writable original storage identity");
            if (Cxx && Cxx->RuntimeAccesses.count(Store) &&
                (!Where || Where->Root != Cxx->Catch))
              return Reject(
                  "runtime object write lost its checked pointer identity");
            if (Cxx && Where && Where->Root == Cxx->Catch) {
              const auto Access = Cxx->RuntimeAccesses.find(Store);
              if (Access == Cxx->RuntimeAccesses.end() ||
                  !Access->second.Write ||
                  Where->Offset != Access->second.Offset || Where->Offset < 0 ||
                  Bytes != Access->second.Width ||
                  uint64_t(Where->Offset) + Access->second.Width >
                      Cxx->ObjectSize ||
                  !Dominators.at(&Parent)->dominates(Cxx->Catch, Store) ||
                  IsAddress(Store->getValueOperand()))
                return Reject(
                    "runtime object write changed its checked domain");
              continue;
            }
            if (Cxx && Cxx->Reference && Where && Where->Root == LogicalFrame &&
                Where->Offset < Cxx->HomeOffset + 4 &&
                Cxx->HomeOffset < Where->Offset + int64_t(Bytes) &&
                Dominators.at(&Parent)->dominates(Cxx->Catch, Store))
              return Reject("runtime reference home was overwritten");
            if (!ImmutableImageRanges.empty() &&
                !IncomingAccesses.count(Store) &&
                (!Where || llvm::isa<llvm::GlobalVariable>(Where->Root))) {
              const auto *Global =
                  Where ? llvm::dyn_cast<llvm::GlobalVariable>(Where->Root)
                        : nullptr;
              auto Base = Global ? parseNdDataSymbol(Global->getName())
                                 : std::optional<uint64_t>{};
              if (!Base && Global)
                Base = parseNdCodePtrSymbol(Global->getName());
              if (!Base && Global && SecurityCookieVA &&
                  Global->getName() == "__security_cookie")
                Base = SecurityCookieVA;
              if (!Base || *Base > UINT32_MAX)
                return Reject("unproved registration image write");
              const int64_t Address = int64_t(*Base) + Where->Offset;
              if (Address < 0 || Address > UINT32_MAX ||
                  Bytes > uint64_t(UINT32_MAX) + 1 - uint64_t(Address))
                return Reject("wrapping registration image write");
              for (const auto &Range : ImmutableImageRanges) {
                if (++Work > limits::kMaxRegistrationEHStateWork)
                  return Reject("image closure exhausted its work budget");
                if (uint64_t(Address) < Range.End &&
                    Range.Begin < uint64_t(Address) + Bytes)
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
            if (IsAddress(Store->getValueOperand())) {
              if (!Where || !Bounded(*Where, Bytes) || Store->isAtomic())
                return Reject();
              auto [It, New] = TaintedMemory.emplace(*Where, Bytes);
              Changed |= New || It->second < Bytes;
              It->second = std::max(It->second, Bytes);
            }
            continue;
          }
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (Call->isInlineAsm() || llvm::isa<llvm::CallBrInst>(Call) ||
                Call->isMustTailCall() ||
                Call->hasFnAttr(llvm::Attribute::ReturnsTwice))
              return Reject();
            const auto *Callee = Call->getCalledFunction();
            if (Cxx) {
              const auto Borrow = Cxx->Borrows.find(Call);
              if (Borrow != Cxx->Borrows.end()) {
                const auto Where = Call->arg_size() == 1
                                       ? Locate(Call->getArgOperand(0), 0)
                                       : std::nullopt;
                if (!Where || Where->Root != LogicalFrame ||
                    Where->Offset != Borrow->second.Offset ||
                    Call->getCallingConv() != llvm::CallingConv::X86_ThisCall)
                  return Reject(
                      "callee borrow changed its source frame object");
                for (const auto *Effects :
                     {&Borrow->second.Reads, &Borrow->second.Writes})
                  for (const auto &Effect : *Effects) {
                    const Cell Access{Where->Root,
                                      Where->Offset + Effect.Begin};
                    if (Effect.Begin >= Effect.End ||
                        !Bounded(Access,
                                 uint64_t(int64_t(Effect.End) - Effect.Begin)))
                      return Reject(
                          "callee borrow leaves its allocated source frame");
                    if (Effects == &Borrow->second.Reads)
                      for (const auto &[Stored, Width] : TaintedMemory) {
                        if (++Work > limits::kMaxRegistrationEHStateWork)
                          return Reject(
                              "callee value proof exhausted its work budget");
                        if (Stored.Root == Access.Root &&
                            Stored.Offset < Where->Offset + Effect.End &&
                            Access.Offset < Stored.Offset + int64_t(Width))
                          return Reject("callee reads a pointer from its "
                                        "scalar object borrow");
                      }
                    if (Cxx->Reference && Effects == &Borrow->second.Writes &&
                        Access.Offset < Cxx->HomeOffset + 4 &&
                        Cxx->HomeOffset < Access.Offset + int64_t(Effect.End) -
                                              Effect.Begin &&
                        Dominators.at(&Parent)->dominates(Cxx->Catch, Call))
                      return Reject(
                          "callee overwrites the runtime reference home");
                  }
                continue;
              }
            }
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
              if (Function != &Parent || !Abnormal ||
                  Abnormal->getBitWidth() != 8 ||
                  Abnormal->getZExtValue() > 1 || !Frame ||
                  Frame->getIntrinsicID() != llvm::Intrinsic::localaddress ||
                  Call->getCallingConv() != Callee->getCallingConv())
                return Reject();
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
          if (Cxx && &I == Cxx->Catch)
            continue;
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
            Changed |= Addresses.insert(&I).second;
          }
        }
  }
  if (Exhausted)
    return Reject("address proof exhausted its work budget");
  if (Cxx) {
    // Recompute definite initialization from actual LLVM stores and the CRT
    // catch write. Callee write summaries are may-effects and never seed bytes.
    std::map<const llvm::Instruction *, std::vector<Cell>> RequiredBytes;
    std::map<const llvm::Instruction *, std::pair<Cell, uint64_t>> Writes;
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
               Where->Root == Cxx->CallbackStack) &&
              !Require(Load, *Where, Size(Load->getType())))
            return Reject("frame read initialization exceeds its work budget");
        }
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
          const auto Where = Locate(Store->getPointerOperand(), 0);
          if (Where && (Where->Root == LogicalFrame ||
                        Where->Root == Cxx->CallbackStack)) {
            if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(
                    Store->getValueOperand()))
              return Reject(
                  "undefined value initializes a source frame object");
            Writes.emplace(
                Store, std::make_pair(
                           *Where, Size(Store->getValueOperand()->getType())));
          }
        }
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
          const auto Borrow = Cxx->Borrows.find(Call);
          if (Borrow != Cxx->Borrows.end())
            for (const auto &Read : Borrow->second.Reads)
              if (!Require(Call,
                           {LogicalFrame, Borrow->second.Offset + Read.Begin},
                           uint64_t(int64_t(Read.End) - Read.Begin)))
                return Reject("callee initialization exceeds its work budget");
        }
      }
    Writes.emplace(Cxx->Catch,
                   std::make_pair(Cell{LogicalFrame, Cxx->HomeOffset},
                                  Cxx->Reference ? 4 : Cxx->ObjectSize));
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
      if (&I == Cxx->Catch && Cxx->CallbackStack)
        for (const auto &[Byte, Index] : Indices) {
          if (++Work > limits::kMaxRegistrationEHStateWork)
            return false;
          if (Byte.Root == Cxx->CallbackStack)
            State.reset(Index);
        }
      const auto Write = Writes.find(&I);
      if (Write == Writes.end())
        return true;
      for (auto It = Indices.lower_bound(Write->second.first);
           It != Indices.end() && It->first.Root == Write->second.first.Root &&
           It->first.Offset <
               Write->second.first.Offset + int64_t(Write->second.second);
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
          if (++Work > limits::kMaxRegistrationEHStateWork || !Seed(State, I))
            return Reject("initialization transfer exhausted its work budget");
          if (llvm::isa<llvm::InvokeInst>(I))
            BeforeUnwind = State;
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
