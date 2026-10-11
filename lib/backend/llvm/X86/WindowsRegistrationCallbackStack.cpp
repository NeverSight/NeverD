//===- WindowsRegistrationCallbackStack.cpp - Private callback stack ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "WindowsRegistrationFramePrivate.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/RegistrationFrameAddress.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/MathExtras.h"

#include <deque>
#include <limits>

namespace neverd::x86_registration {

bool isPrivateSlotInitializer(const llvm::User *User,
                              const llvm::AllocaInst &Slot) {
  const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
  const auto *Zero =
      Store ? llvm::dyn_cast<llvm::ConstantInt>(Store->getValueOperand())
            : nullptr;
  return Store && Zero && Zero->isZero() &&
         Store->getPointerOperand() == &Slot &&
         Store->getValueOperand()->getType() == Slot.getAllocatedType() &&
         !Store->isAtomic() && !Store->isVolatile() &&
         Store->getParent() == &Slot.getFunction()->getEntryBlock();
}

// A private stack is valid only for bounded scratch cells below entry ESP.
// Reject observable addresses, return-address reads and unproved SSA copies.
// The producer still owns the source entry-state proof; this checks the actual
// LLVM uses that would be redirected to the new allocation.
llvm::Error checkPrivateStack(
    const llvm::DataLayout &Layout, llvm::BasicBlock &Entry,
    const std::set<llvm::BasicBlock *> &Body,
    const std::map<llvm::StoreInst *, X86RegistrationRootKind> &Seeds,
    X86RegistrationCallbackFrame &Frame,
    std::set<llvm::AllocaInst *> &PrivateSlots, size_t &WorkUsed,
    const std::set<llvm::StoreInst *> *SourceStores) {
  std::map<llvm::Value *, int64_t> Offsets;
  std::vector<llvm::Value *> Work;
  struct ScratchAccess {
    int64_t Offset;
    uint64_t Size;
    bool Write;
  };
  std::map<llvm::Instruction *, ScratchAccess> Accesses;
  bool Invalid = false;
  bool Exhausted = false;
  int64_t MinOffset = 0;
  auto Charge = [&](size_t Amount) {
    if (Amount > limits::kMaxRegistrationEHStateWork - WorkUsed) {
      Exhausted = true;
      return false;
    }
    WorkUsed += Amount;
    return true;
  };
  auto Add = [&](llvm::Value *Value, int64_t Offset) {
    if (!Charge(1))
      return;
    auto [It, Inserted] = Offsets.emplace(Value, Offset);
    MinOffset = std::min(MinOffset, Offset);
    if (Frame.InferStackBounds && Offset > 0)
      Invalid = true;
    Invalid |= !Inserted && It->second != Offset;
    if (Inserted)
      Work.push_back(Value);
  };
  auto Loads = [&](llvm::AllocaInst *Slot) {
    for (llvm::User *User : Slot->users()) {
      if (!Charge(1))
        break;
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(User))
        Add(Load, 0);
    }
  };
  for (const auto &[Definition, Kind] : Seeds)
    if (Kind == X86RegistrationRootKind::StackPointer)
      Loads(llvm::cast<llvm::AllocaInst>(Definition->getPointerOperand()));
  auto CheckAccess = [&](llvm::Type *Type, int64_t Offset, llvm::Align Align) {
    const llvm::TypeSize Size = Layout.getTypeStoreSize(Type);
    return !Size.isScalable() && Offset < 0 &&
           Offset >= -int64_t(*Frame.StackPointerOffset) &&
           Size.getFixedValue() <= uint64_t(-Offset) && Align.value() <= 16 &&
           (int64_t(*Frame.StackPointerOffset) + Offset) % Align.value() == 0;
  };
  while (!Work.empty() && !Invalid && !Exhausted) {
    if (!Charge(1))
      break;
    llvm::Value *Value = Work.back();
    Work.pop_back();
    const int64_t Offset = Offsets.at(Value);
    for (llvm::Use &Use : Value->uses()) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("private callback stack exceeds the work budget");
      auto *User = llvm::dyn_cast<llvm::Instruction>(Use.getUser());
      if (!User || !Body.count(User->getParent()))
        return reject("private callback stack address escapes its body");
      if (auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(User)) {
        const auto *Constant =
            llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(1));
        if (!Constant || Use.getOperandNo() != 0 ||
            Binary->hasPoisonGeneratingFlags() ||
            (Binary->getOpcode() != llvm::Instruction::Add &&
             Binary->getOpcode() != llvm::Instruction::Sub) ||
            Constant->getBitWidth() > 64)
          return reject(
              "private callback stack address has unproved arithmetic");
        const int64_t Delta = Constant->getSExtValue();
        // Keep the proof in the bounded signed PE32 displacement domain.
        if (Delta < -int64_t(UINT32_MAX) || Delta > int64_t(UINT32_MAX))
          return reject("private callback stack displacement is unbounded");
        const int64_t Next =
            Offset +
            (Binary->getOpcode() == llvm::Instruction::Add ? Delta : -Delta);
        if (Next < -int64_t(Frame.StackBytes) || Next > Frame.StackBytes)
          return reject("private callback stack displacement is out of bounds");
        Add(User, Next);
        continue;
      }
      if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(User)) {
        auto IsFullAddress = [](llvm::Type *Type) {
          if (const auto *Pointer = llvm::dyn_cast<llvm::PointerType>(Type))
            return Pointer->getAddressSpace() == 0;
          return Type->isIntegerTy(32) || Type->isIntegerTy(64);
        };
        if (Cast->hasPoisonGeneratingFlags() ||
            !IsFullAddress(Cast->getSrcTy()) ||
            !IsFullAddress(Cast->getDestTy()))
          return reject(
              "private callback stack has an unsupported address cast");
        Add(User, Offset);
        continue;
      }
      if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(User)) {
        auto Delta = registration_frame::checkedByteGEPOffset(GEP);
        if (Use.getOperandNo() != 0 || !Delta)
          return reject("private callback stack has an unproved GEP");
        const int64_t Next = Offset + *Delta;
        if (Next < -int64_t(Frame.StackBytes) || Next > Frame.StackBytes)
          return reject("private callback stack GEP is out of bounds");
        if (GEP->hasNoUnsignedWrap() ||
            ((GEP->isInBounds() || GEP->hasNoUnsignedSignedWrap()) &&
             (int64_t(*Frame.StackPointerOffset) + Next < 0 ||
              int64_t(*Frame.StackPointerOffset) + Next > Frame.StackBytes)))
          return reject(
              "private callback stack GEP has unproved no-wrap bounds");
        Add(User, Next);
        continue;
      }
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(User)) {
        if (Use.getOperandNo() != 0 ||
            !CheckAccess(Load->getType(), Offset, Load->getAlign()))
          return reject("private callback stack reads an unproved entry cell");
        Accesses.emplace(
            Load, ScratchAccess{
                      Offset,
                      Layout.getTypeStoreSize(Load->getType()).getFixedValue(),
                      false});
        continue;
      }
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
        if (Use.getOperandNo() == 1) {
          if (!CheckAccess(Store->getValueOperand()->getType(), Offset,
                           Store->getAlign()))
            return reject("private callback stack write is out of bounds");
          Accesses.emplace(
              Store,
              ScratchAccess{
                  Offset,
                  Layout.getTypeStoreSize(Store->getValueOperand()->getType())
                      .getFixedValue(),
                  true});
          continue;
        }
        if (SourceStores && SourceStores->count(Store))
          continue;
        auto *Slot =
            llvm::dyn_cast<llvm::AllocaInst>(Store->getPointerOperand());
        if (!Slot || !Slot->isStaticAlloca() ||
            (!Slot->getAllocatedType()->isIntegerTy(32) &&
             !Slot->getAllocatedType()->isIntegerTy(64) &&
             !Slot->getAllocatedType()->isPointerTy()) ||
            !llvm::cast<llvm::ConstantInt>(Slot->getArraySize())->isOne() ||
            Store->getValueOperand()->getType() != Slot->getAllocatedType() ||
            Store->isVolatile() || Store->isAtomic())
          return reject("private callback stack address is stored externally");
        for (llvm::User *SlotUser : Slot->users()) {
          if (!Charge(1))
            return reject(
                "private callback stack SSA uses exceed the work budget");
          auto *Load = llvm::dyn_cast<llvm::LoadInst>(SlotUser);
          if (SlotUser == Store || isPrivateSlotInitializer(SlotUser, *Slot))
            continue;
          if (!Load || Load->getType() != Slot->getAllocatedType() ||
              !Body.count(Load->getParent()) || Load->isVolatile() ||
              Load->isAtomic() ||
              (Store->getParent() != &Entry &&
               Store->getParent() != Load->getParent()) ||
              (Store->getParent() == Load->getParent() &&
               !Store->comesBefore(Load)))
            return reject("private callback stack SSA copy has unproved uses");
          Add(Load, Offset);
        }
        PrivateSlots.insert(Slot);
        continue;
      }
      return reject("private callback stack address has an observable use");
    }
  }
  if (Exhausted)
    return reject("private callback stack values exceed the work budget");
  if (Invalid)
    return reject("private callback stack has inconsistent SSA offsets");
  if (Frame.InferStackBounds) {
    const uint64_t Bytes = llvm::alignTo(uint64_t(-MinOffset), uint64_t(16));
    if (Bytes > limits::kMaxRegistrationEHStateWork)
      return reject("inferred private callback stack exceeds the work budget");
    // A seed used only to transport entry ESP still needs a valid private
    // pointer. It cannot be dereferenced at offset zero by the checks above.
    Frame.StackBytes = static_cast<uint32_t>(std::max<uint64_t>(Bytes, 16));
    Frame.StackPointerOffset = Frame.StackBytes;
  }

  // Memory below the dispatcher's incoming ESP is not automatically scratch:
  // a read must be preceded by writes on every callback path. Solve definite
  // byte initialization from the callback root, including backedges.
  std::set<int64_t> Universe;
  for (const auto &[Instruction, Access] : Accesses)
    for (uint64_t I = 0; I < Access.Size; ++I) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("private callback scratch proof exceeds the work budget");
      Universe.insert(Access.Offset + I);
    }
  std::vector<llvm::BasicBlock *> OrderedBlocks;
  std::map<llvm::BasicBlock *, std::vector<ScratchAccess>> OrderedAccesses;
  std::map<llvm::BasicBlock *, std::set<int64_t>> Incoming, Outgoing;
  std::deque<llvm::BasicBlock *> Pending;
  std::set<llvm::BasicBlock *> Queued;
  for (llvm::BasicBlock &Block : *Entry.getParent()) {
    if (!Charge(1))
      return reject(
          "private callback scratch block scan exceeds the work budget");
    if (!Body.count(&Block))
      continue;
    OrderedBlocks.push_back(&Block);
    for (llvm::Instruction &Instruction : Block) {
      if (!Charge(1))
        return reject("private callback scratch instruction scan exceeds the "
                      "work budget");
      if (auto It = Accesses.find(&Instruction); It != Accesses.end())
        OrderedAccesses[&Block].push_back(It->second);
    }
    if (!Charge(Universe.size()))
      return reject("private callback scratch state exceeds the work budget");
    Outgoing[&Block] = Universe;
    Pending.push_back(&Block);
    Queued.insert(&Block);
  }
  while (!Pending.empty()) {
    if (!Charge(1))
      return reject(
          "private callback scratch worklist exceeds the work budget");
    llvm::BasicBlock *Block = Pending.front();
    Pending.pop_front();
    Queued.erase(Block);
    std::set<int64_t> In;
    if (Block != &Entry) {
      if (!Charge(Universe.size()))
        return reject(
            "private callback scratch state copy exceeds the work budget");
      In = Universe;
      for (llvm::BasicBlock *Pred : llvm::predecessors(Block)) {
        if (!Charge(1))
          return reject(
              "private callback scratch predecessors exceed the work budget");
        for (auto It = In.begin(); It != In.end();) {
          if (!Charge(1))
            return reject(
                "private callback scratch flow exceeds the work budget");
          if (!Outgoing.at(Pred).count(*It))
            It = In.erase(It);
          else
            ++It;
        }
      }
    }
    if (!Charge(In.size()))
      return reject(
          "private callback scratch incoming copy exceeds the work budget");
    Incoming[Block] = In;
    for (const ScratchAccess &Access : OrderedAccesses[Block]) {
      if (!Charge(1 + (Access.Write ? Access.Size : 0)))
        return reject("private callback scratch stores exceed the work budget");
      if (Access.Write)
        for (uint64_t I = 0; I < Access.Size; ++I)
          In.insert(Access.Offset + I);
    }
    if (!Charge(In.size() + Outgoing[Block].size()))
      return reject(
          "private callback scratch comparison exceeds the work budget");
    if (Outgoing[Block] != In) {
      Outgoing[Block] = std::move(In);
      for (llvm::BasicBlock *Successor : llvm::successors(Block)) {
        if (!Charge(1))
          return reject(
              "private callback scratch successors exceed the work budget");
        if (Queued.insert(Successor).second)
          Pending.push_back(Successor);
      }
    }
  }
  for (llvm::BasicBlock *Block : OrderedBlocks) {
    if (!Charge(1 + Incoming.at(Block).size()))
      return reject(
          "private callback scratch final state exceeds the work budget");
    auto Initialized = Incoming.at(Block);
    for (const ScratchAccess &Access : OrderedAccesses[Block]) {
      if (!Charge(1 + Access.Size))
        return reject("private callback scratch reads exceed the work budget");
      for (uint64_t I = 0; I < Access.Size; ++I)
        if (Access.Write)
          Initialized.insert(Access.Offset + I);
        else if (!Initialized.count(Access.Offset + I))
          return reject(
              "private callback scratch read is not definitely initialized");
    }
  }
  return llvm::Error::success();
}

} // namespace neverd::x86_registration
