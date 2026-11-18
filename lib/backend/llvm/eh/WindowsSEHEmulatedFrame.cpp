//===- WindowsSEHEmulatedFrame.cpp - x64 SEH callback frames ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/WindowsSEHEmulatedFrame.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Errc.h"

#include <functional>
#include <limits>
#include <map>
#include <set>

namespace neverd {
namespace {

/// The widest offset an address walk keeps; anything wider is no frame byte.
constexpr unsigned kOffsetBits = 48;

std::optional<int64_t> addOffset(int64_t Base, int64_t Delta) {
  int64_t Sum = 0;
  if (__builtin_add_overflow(Base, Delta, &Sum) ||
      llvm::APInt(64, static_cast<uint64_t>(Sum), /*isSigned=*/true)
              .getSignificantBits() > kOffsetBits)
    return std::nullopt;
  return Sum;
}

std::optional<int64_t> smallConstant(const llvm::Value *Value) {
  const auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(Value);
  if (!Constant || Constant->getBitWidth() > 64 ||
      Constant->getValue().getSignificantBits() > kOffsetBits)
    return std::nullopt;
  return Constant->getSExtValue();
}

bool isPointerWide(const llvm::Type *Type, const llvm::DataLayout &Layout) {
  return Type->isPointerTy() ||
         (Type->isIntegerTy() &&
          Type->getIntegerBitWidth() == Layout.getPointerSizeInBits());
}

/// An alloca only its own loads and stores name, as a lifted register's
/// home.
bool isHome(const llvm::AllocaInst &Slot) {
  for (const llvm::User *User : Slot.users()) {
    if (llvm::isa<llvm::LoadInst>(User))
      continue;
    const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
    if (!Store || Store->getValueOperand() == &Slot)
      return false;
  }
  return true;
}

/// Follows addresses computed from roots by constant offsets, through homes,
/// and records the loads and stores made through them.  A store of such an
/// address anywhere but a home goes to \ref StoresAddress, which follows it
/// or refuses.
class AddressWalk {
public:
  struct Access {
    const llvm::Instruction *Instruction = nullptr;
    int64_t Offset = 0;
    uint64_t Size = 0;
    bool Write = false;
  };

  AddressWalk(const llvm::DataLayout &Layout, size_t &Remaining)
      : Layout(Layout), Remaining(Remaining) {}

  void derive(const llvm::Value *Value, int64_t Offset) {
    const auto [It, Inserted] = Offsets.try_emplace(Value, Offset);
    if (Inserted)
      Work.push_back(Value);
    else if (It->second != Offset)
      Conflict = true;
  }

  /// Runs to a fixed point; false when a use cannot be followed.
  bool run(const std::function<bool(const llvm::StoreInst &, int64_t)>
               &StoresAddress) {
    while (!Work.empty()) {
      if (Conflict)
        return false;
      const llvm::Value *Value = Work.back();
      Work.pop_back();
      const int64_t Offset = Offsets.at(Value);
      for (const llvm::Use &Use : Value->uses()) {
        if (!Remaining)
          return false;
        --Remaining;
        if (!follow(Use, Offset, StoresAddress))
          return false;
      }
    }
    return !Conflict;
  }

  /// Whether every PHI and select an address reaches takes addresses with
  /// one offset, and every home it rests in is written whole.  The walk
  /// follows every use of every address it computes, so an operand it did
  /// not reach is no address: a value that is either the address at one
  /// offset or such an operand reaches memory at that offset or not at all.
  bool consistent() const {
    for (const auto &[Value, Offset] : Offsets) {
      llvm::SmallVector<const llvm::Value *, 4> Operands;
      if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(Value))
        Operands.append(Phi->value_op_begin(), Phi->value_op_end());
      else if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Value))
        Operands = {Select->getTrueValue(), Select->getFalseValue()};
      for (const llvm::Value *Operand : Operands) {
        const auto It = Offsets.find(Operand);
        if (It != Offsets.end() && It->second != Offset)
          return false;
      }
    }
    for (const auto &[Home, Offset] : Homes) {
      std::optional<uint64_t> Width;
      for (const llvm::User *User : Home->users())
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
          const uint64_t Bytes =
              Layout.getTypeStoreSize(Store->getValueOperand()->getType());
          if (Width && *Width != Bytes)
            return false;
          Width = Bytes;
        }
    }
    return true;
  }

  std::map<const llvm::Value *, int64_t> Offsets;
  std::vector<Access> Accesses;
  std::map<const llvm::AllocaInst *, int64_t> Homes;

private:
  bool follow(const llvm::Use &Use, int64_t Offset,
              const std::function<bool(const llvm::StoreInst &, int64_t)>
                  &StoresAddress) {
    const auto *User = llvm::dyn_cast<llvm::Instruction>(Use.getUser());
    if (!User)
      return false;
    const auto Derive = [&](int64_t Delta) {
      const auto Sum = addOffset(Offset, Delta);
      if (!Sum)
        return false;
      derive(User, *Sum);
      return true;
    };
    if (const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(User)) {
      llvm::APInt Constant(Layout.getIndexTypeSizeInBits(GEP->getType()), 0);
      if (Use.getOperandNo() != 0 ||
          !GEP->accumulateConstantOffset(Layout, Constant) ||
          Constant.getSignificantBits() > kOffsetBits)
        return false;
      return Derive(Constant.getSExtValue());
    }
    if (llvm::isa<llvm::BitCastInst, llvm::FreezeInst>(User))
      return Derive(0);
    if (llvm::isa<llvm::PtrToIntInst, llvm::IntToPtrInst>(User))
      return isPointerWide(User->getType(), Layout) && Derive(0);
    if (const auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(User)) {
      const auto Constant =
          smallConstant(Binary->getOperand(1 - Use.getOperandNo()));
      if (!Constant || !isPointerWide(Binary->getType(), Layout))
        return false;
      switch (Binary->getOpcode()) {
      case llvm::Instruction::Add:
        return Derive(*Constant);
      case llvm::Instruction::Sub:
        return Use.getOperandNo() == 0 && Derive(-*Constant);
      case llvm::Instruction::Or:
        return llvm::cast<llvm::PossiblyDisjointInst>(Binary)->isDisjoint() &&
               Derive(*Constant);
      default:
        return false;
      }
    }
    if (llvm::isa<llvm::PHINode>(User))
      return Derive(0);
    if (llvm::isa<llvm::SelectInst>(User))
      return Use.getOperandNo() != 0 && Derive(0);
    if (llvm::isa<llvm::ICmpInst>(User))
      return true;
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User)) {
      Accesses.push_back(
          {Load, Offset, Layout.getTypeStoreSize(Load->getType()), false});
      return true;
    }
    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
      if (Use.getOperandNo() == Store->getPointerOperandIndex()) {
        Accesses.push_back(
            {Store, Offset,
             Layout.getTypeStoreSize(Store->getValueOperand()->getType()),
             true});
        return true;
      }
      // The address itself is stored: into a home, whose loads read it back,
      // or wherever the walk's owner follows it.
      if (const auto *Home =
              llvm::dyn_cast<llvm::AllocaInst>(Store->getPointerOperand());
          Home && isHome(*Home)) {
        const auto [It, Inserted] = Homes.try_emplace(Home, Offset);
        if (!Inserted)
          return It->second == Offset;
        for (const llvm::User *Reader : Home->users())
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Reader))
            derive(Load, Offset);
        return true;
      }
      return StoresAddress(*Store, Offset);
    }
    // A slot's lifetime names it and reaches nothing through it.
    if (const auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(User))
      return Intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_start ||
             Intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_end;
    return false;
  }

  const llvm::DataLayout &Layout;
  size_t &Remaining;
  std::vector<const llvm::Value *> Work;
  bool Conflict = false;
};

} // namespace

llvm::Expected<std::optional<SEHEmulatedFrame>>
findSEHEmulatedFrame(const llvm::Function &Parent, va_t FunctionVA) {
  const auto Reject = [&]() {
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "native SEH emulated frame identity changed in function " +
            Parent.getName().str());
  };
  std::optional<SEHEmulatedFrame> Frame;
  for (const llvm::BasicBlock &Block : Parent)
    for (const llvm::Instruction &Instruction : Block) {
      const llvm::MDNode *Node =
          Instruction.getMetadata(windows_eh_md::SEHEmulatedFrameAttachment);
      if (!Node)
        continue;
      const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
      const auto Integer = [&](unsigned Index) -> std::optional<uint64_t> {
        const auto *Value =
            Index < Node->getNumOperands()
                ? llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
                      Node->getOperand(Index).get())
                : nullptr;
        const auto *Constant =
            Value ? llvm::dyn_cast<llvm::ConstantInt>(Value->getValue())
                  : nullptr;
        if (!Constant || Constant->getBitWidth() != 64)
          return std::nullopt;
        return Constant->getZExtValue();
      };
      const auto Entry = Integer(1);
      if (Frame || !Slot || Node->getNumOperands() != 2 ||
          Integer(0) != FunctionVA || !Entry || !Slot->isStaticAlloca() ||
          Slot->getAddressSpace() || &Block != &Parent.getEntryBlock())
        return Reject();
      const auto Size =
          Slot->getAllocationSize(Parent.getParent()->getDataLayout());
      if (!Size || Size->isScalable() || *Entry > Size->getFixedValue() ||
          Size->getFixedValue() > uint64_t(std::numeric_limits<int32_t>::max()))
        return Reject();
      Frame = SEHEmulatedFrame{Slot, *Entry, Size->getFixedValue()};
    }
  return Frame;
}

std::optional<std::pair<const llvm::Value *, int64_t>>
sehConstantAddress(const llvm::Value &Value, const llvm::DataLayout &Layout) {
  const llvm::Value *At = &Value;
  int64_t Offset = 0;
  for (unsigned Step = 0;
       Step < limits::backend::kSEHEmulatedFrameMaxCallbackUses; ++Step) {
    std::optional<int64_t> Delta;
    const llvm::Value *Next = nullptr;
    if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(At)) {
      llvm::APInt Constant(Layout.getIndexTypeSizeInBits(GEP->getType()), 0);
      if (!GEP->accumulateConstantOffset(Layout, Constant) ||
          Constant.getSignificantBits() > kOffsetBits)
        return std::nullopt;
      Delta = Constant.getSExtValue();
      Next = GEP->getPointerOperand();
    } else if (const auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(At)) {
      Delta = 0;
      Next = Freeze->getOperand(0);
    } else if (const auto *Operator = llvm::dyn_cast<llvm::Operator>(At)) {
      switch (Operator->getOpcode()) {
      case llvm::Instruction::BitCast:
        Delta = 0;
        Next = Operator->getOperand(0);
        break;
      case llvm::Instruction::PtrToInt:
      case llvm::Instruction::IntToPtr:
        if (!isPointerWide(Operator->getType(), Layout) ||
            !isPointerWide(Operator->getOperand(0)->getType(), Layout))
          return std::nullopt;
        Delta = 0;
        Next = Operator->getOperand(0);
        break;
      case llvm::Instruction::Add:
      case llvm::Instruction::Or:
        if (Operator->getOpcode() == llvm::Instruction::Or &&
            !llvm::cast<llvm::PossiblyDisjointInst>(Operator)->isDisjoint())
          return std::nullopt;
        if ((Delta = smallConstant(Operator->getOperand(1))))
          Next = Operator->getOperand(0);
        else if ((Delta = smallConstant(Operator->getOperand(0))))
          Next = Operator->getOperand(1);
        else
          return std::nullopt;
        break;
      case llvm::Instruction::Sub:
        if (!(Delta = smallConstant(Operator->getOperand(1))))
          return std::nullopt;
        Delta = -*Delta;
        Next = Operator->getOperand(0);
        break;
      default:
        break;
      }
    }
    if (!Next)
      return std::pair<const llvm::Value *, int64_t>{At, Offset};
    const auto Sum = addOffset(Offset, *Delta);
    if (!Sum)
      return std::nullopt;
    Offset = *Sum;
    At = Next;
  }
  return std::nullopt;
}

std::optional<std::vector<SEHFrameAccess>>
collectSEHCallbackFrameAccesses(const llvm::Function &Callback) {
  if (Callback.isDeclaration())
    return std::nullopt;
  std::vector<SEHFrameAccess> Accesses;
  if (Callback.arg_size() < 2)
    return Accesses;
  const llvm::DataLayout &Layout = Callback.getParent()->getDataLayout();
  size_t Remaining = limits::backend::kSEHEmulatedFrameMaxCallbackUses;

  // The callback's own frames: allocas other than homes, each with every load
  // and store made through it, once it is known that its address stays.
  std::map<const llvm::AllocaInst *,
           std::optional<std::vector<AddressWalk::Access>>>
      OwnFrames;
  const auto OwnFrame = [&](const llvm::AllocaInst &Slot)
      -> const std::optional<std::vector<AddressWalk::Access>> & {
    const auto [It, Inserted] = OwnFrames.try_emplace(&Slot);
    if (!Inserted)
      return It->second;
    AddressWalk Walk(Layout, Remaining);
    Walk.derive(&Slot, 0);
    if (Walk.run([](const llvm::StoreInst &, int64_t) { return false; }) &&
        Walk.consistent())
      It->second = std::move(Walk.Accesses);
    return It->second;
  };
  struct HeldSlot {
    const llvm::AllocaInst *Frame = nullptr;
    int64_t Offset = 0;
    uint64_t Size = 0;
    int64_t Held = 0;
  };
  std::vector<HeldSlot> Held;

  AddressWalk Walk(Layout, Remaining);
  Walk.derive(Callback.getArg(1), 0);
  const bool Followed = Walk.run([&](const llvm::StoreInst &Store,
                                     int64_t Offset) {
    // The frame kept in a fixed slot of the callback's own frame: what loads
    // that slot whole reads it back.
    const auto Address = sehConstantAddress(*Store.getPointerOperand(), Layout);
    const auto *Frame =
        Address ? llvm::dyn_cast<llvm::AllocaInst>(Address->first) : nullptr;
    if (!Frame || Frame->getFunction() != &Callback || isHome(*Frame))
      return false;
    const auto &Accesses = OwnFrame(*Frame);
    if (!Accesses)
      return false;
    const uint64_t Size =
        Layout.getTypeStoreSize(Store.getValueOperand()->getType());
    Held.push_back({Frame, Address->second, Size, Offset});
    for (const AddressWalk::Access &Access : *Accesses)
      if (!Access.Write && Access.Offset == Address->second &&
          Access.Size == Size)
        Walk.derive(Access.Instruction, Offset);
    return true;
  });
  if (!Followed || !Walk.consistent())
    return std::nullopt;
  // A slot that holds the frame is written and read only whole, and holds
  // it at one offset.
  for (const HeldSlot &Slot : Held)
    for (const AddressWalk::Access &Access : *OwnFrames.at(Slot.Frame)) {
      const int64_t End = Slot.Offset + static_cast<int64_t>(Slot.Size);
      const int64_t AccessEnd =
          Access.Offset + static_cast<int64_t>(Access.Size);
      if (Access.Offset >= End || Slot.Offset >= AccessEnd)
        continue;
      if (Access.Offset != Slot.Offset || Access.Size != Slot.Size)
        return std::nullopt;
      if (const auto *Store =
              llvm::dyn_cast<llvm::StoreInst>(Access.Instruction)) {
        const auto It = Walk.Offsets.find(Store->getValueOperand());
        if (It != Walk.Offsets.end() && It->second != Slot.Held)
          return std::nullopt;
      }
    }
  // What the callback reads through the frame stays in the callback: a
  // stack address the lifted parent keeps there addresses the emulated
  // frame, not the frame the runtime compares (RtlUnwind's TargetFrame,
  // longjmp's buffer), so nothing derived from it reaches a call.  It may
  // reach memory, branches, the return and intrinsics that touch no memory.
  {
    std::set<const llvm::Value *> Read;
    std::vector<const llvm::Value *> Pending;
    std::set<const llvm::AllocaInst *> Spread;
    const auto Note = [&](const llvm::Value *Value) {
      if (Read.insert(Value).second)
        Pending.push_back(Value);
    };
    for (const AddressWalk::Access &Access : Walk.Accesses)
      if (!Access.Write)
        Note(Access.Instruction);
    while (!Pending.empty()) {
      const llvm::Value *Value = Pending.back();
      Pending.pop_back();
      for (const llvm::Use &Use : Value->uses()) {
        if (!Remaining)
          return std::nullopt;
        --Remaining;
        const auto *User = llvm::dyn_cast<llvm::Instruction>(Use.getUser());
        if (!User)
          return std::nullopt;
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(User)) {
          const auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(Call);
          if (!Intrinsic || !Intrinsic->doesNotAccessMemory())
            return std::nullopt;
          Note(Call);
          continue;
        }
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
          if (Use.getOperandNo() == Store->getPointerOperandIndex())
            continue;
          // Kept in a home or the callback's own frame, it is read back.
          const auto Address =
              sehConstantAddress(*Store->getPointerOperand(), Layout);
          const auto *Slot =
              Address ? llvm::dyn_cast<llvm::AllocaInst>(Address->first)
                      : nullptr;
          if (!Slot || Slot->getFunction() != &Callback ||
              !Spread.insert(Slot).second)
            continue;
          if (isHome(*Slot)) {
            for (const llvm::User *Reader : Slot->users())
              if (llvm::isa<llvm::LoadInst>(Reader))
                Note(Reader);
            continue;
          }
          const auto &Kept = OwnFrame(*Slot);
          if (!Kept)
            return std::nullopt;
          for (const AddressWalk::Access &Access : *Kept)
            if (!Access.Write)
              Note(Access.Instruction);
          continue;
        }
        if (llvm::isa<llvm::ReturnInst, llvm::BranchInst, llvm::SwitchInst>(
                User))
          continue;
        Note(User);
      }
    }
  }
  for (const AddressWalk::Access &Access : Walk.Accesses)
    Accesses.push_back({Access.Offset, Access.Size, Access.Write});
  return Accesses;
}

const llvm::Instruction *
findSEHUnorderedAccess(const SEHEmulatedFrame &Frame,
                       llvm::ArrayRef<const llvm::BasicBlock *> Protected) {
  if (!Frame.Slot)
    return nullptr;
  const llvm::DataLayout &Layout = Frame.Slot->getModule()->getDataLayout();
  // Bytes [Offset, Offset + Size) inside one alloca; with \p Private, an
  // alloca no callback can reach.
  const auto Inside = [&](const llvm::Value *Pointer, uint64_t Size,
                          bool Private) {
    const auto Address = sehConstantAddress(*Pointer, Layout);
    const auto *Slot =
        Address ? llvm::dyn_cast<llvm::AllocaInst>(Address->first) : nullptr;
    if (!Slot || (Private && (Slot == Frame.Slot || !isHome(*Slot))))
      return false;
    const auto Extent = Slot->getAllocationSize(Layout);
    return Extent && !Extent->isScalable() && Address->second >= 0 &&
           uint64_t(Address->second) + Size <= Extent->getFixedValue();
  };
  for (const llvm::BasicBlock *Block : Protected)
    for (const llvm::Instruction &Instruction : *Block) {
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction)) {
        if (!Load->isVolatile() &&
            !Inside(Load->getPointerOperand(),
                    Layout.getTypeStoreSize(Load->getType()),
                    /*Private=*/false))
          return Load;
      } else if (const auto *Store =
                     llvm::dyn_cast<llvm::StoreInst>(&Instruction)) {
        if (!Store->isVolatile() &&
            !Inside(
                Store->getPointerOperand(),
                Layout.getTypeStoreSize(Store->getValueOperand()->getType()),
                /*Private=*/true))
          return Store;
      } else if (const auto *Exchange =
                     llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&Instruction)) {
        if (!Exchange->isVolatile())
          return Exchange;
      } else if (const auto *Update =
                     llvm::dyn_cast<llvm::AtomicRMWInst>(&Instruction)) {
        if (!Update->isVolatile())
          return Update;
      } else if (const auto *Memory =
                     llvm::dyn_cast<llvm::MemIntrinsic>(&Instruction)) {
        if (!Memory->isVolatile())
          return Memory;
      }
    }
  return nullptr;
}

} // namespace neverd
