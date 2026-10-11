//===- MedLLVMRegistrationCxxStack.cpp - Private catch stack lowering -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationCxxStack.h"

#include "../eh/MedLLVMEHHelpers.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86RegistrationCatchStack.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86RegistrationCallback.h"

#include "llvm/Support/Errc.h"

namespace neverd {

llvm::Expected<RegistrationCxxStackPlan> prepareRegistrationCxxStack(
    const MedFunc &Function, const MedBlock &Source, llvm::BasicBlock &Entry,
    llvm::ArrayRef<llvm::BasicBlock *> Body,
    const std::map<std::pair<int, int>, llvm::AllocaInst *> &Slots,
    llvm::ArrayRef<llvm::StoreInst *> Stores,
    const std::map<int, llvm::BasicBlock *> &Blocks) {
  RegistrationCxxStackPlan Plan;
  size_t Work = 0;
  for (const auto &Op : Source.Ops) {
    if (Op.RegistrationRoot !=
        MedOp::RegistrationRootKind::CallbackStackPointer)
      continue;
    const auto Slot = Slots.find({Op.Output.Id, Op.Output.SSAVer});
    if (!hasValidRegistrationRootShape(Op) || Op.Output.SSAVer <= 0 ||
        Op.Addr != Source.StartAddr || Slot == Slots.end() || Plan.Seed)
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "catch stack lost its MedIR seed");
    for (auto *User : Slot->second->users()) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return llvm::createStringError(
            llvm::errc::invalid_argument,
            "catch stack seed exceeded its work budget");
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
          Store && Store->getParent() == &Entry) {
        if (Plan.Seed)
          return llvm::createStringError(llvm::errc::invalid_argument,
                                         "catch stack has multiple seeds");
        Plan.Seed = Store;
      }
    }
  }
  if (!Plan.Seed)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "catch stack has no runtime ESP seed");
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return llvm::createStringError(
            llvm::errc::invalid_argument,
            "catch resume preflight exceeded its work budget");
      if (Op.RegistrationRoot !=
          MedOp::RegistrationRootKind::RestoredCallbackStackPointer)
        continue;
      const auto Coordinate = registrationCallbackStackCoordinate(Function, Op);
      if (!Coordinate)
        return llvm::createStringError(
            llvm::errc::invalid_argument,
            "catch resume lost its stack coordinate");
      if (Coordinate->Entry != Source.StartAddr)
        continue;
      const auto Slot = Slots.find({Op.Output.Id, Op.Output.SSAVer});
      if (Slot == Slots.end() || !Blocks.count(Block.Id))
        return llvm::createStringError(llvm::errc::invalid_argument,
                                       "catch resume lost its SSA definition");
      X86RegistrationCatchStackResume Restore;
      Restore.Offset = Coordinate->Offset;
      Restore.TargetVA = Op.Addr;
      for (auto *User : Slot->second->users()) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return llvm::createStringError(
              llvm::errc::invalid_argument,
              "catch resume seed exceeded its work budget");
        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
            Store && Store->getParent() == Blocks.at(Block.Id)) {
          if (Restore.Seed)
            return llvm::createStringError(llvm::errc::invalid_argument,
                                           "catch resume has multiple seeds");
          Restore.Seed = Store;
        }
      }
      std::set<llvm::BasicBlock *> Sources;
      for (const auto &Return : Function.RegistrationStates->CxxContinuations) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return llvm::createStringError(
              llvm::errc::invalid_argument,
              "catch resume returns exceeded their work budget");
        if (Return.TargetVA != Op.Addr)
          continue;
        for (const auto &State : Function.RegistrationStates->Blocks) {
          if (++Work > limits::kMaxRegistrationEHStateWork)
            return llvm::createStringError(
                llvm::errc::invalid_argument,
                "catch resume states exceeded their work budget");
          for (const auto &Search : State.CxxSearches) {
            if (++Work > limits::kMaxRegistrationEHStateWork)
              return llvm::createStringError(
                  llvm::errc::invalid_argument,
                  "catch resume search exceeded its work budget");
            if (Search.TryIndex != Return.TryIndex)
              continue;
            Work += Function.Blocks.size() + Body.size();
            if (Work > limits::kMaxRegistrationEHStateWork)
              return llvm::createStringError(
                  llvm::errc::invalid_argument,
                  "catch resume blocks exceeded their work budget");
            const auto Original =
                llvm::find_if(Function.Blocks, [&](const auto &B) {
                  return B.StartAddr == State.Range.Begin &&
                         B.EndAddr == State.Range.End;
                });
            if (Search.ExitedCatches || Original == Function.Blocks.end() ||
                !Blocks.count(Original->Id) ||
                !llvm::is_contained(Body, Blocks.at(Original->Id)))
              return llvm::createStringError(
                  llvm::errc::invalid_argument,
                  "nested catch has an ambiguous suspended stack");
            Sources.insert(Blocks.at(Original->Id));
          }
        }
      }
      Restore.Sources.assign(Sources.begin(), Sources.end());
      Plan.Resumes.push_back(std::move(Restore));
    }
  auto Bytes = checkX86RegistrationCatchStack(Entry, Body, *Plan.Seed, Stores,
                                              Plan.Resumes);
  if (!Bytes)
    return Bytes.takeError();
  Plan.Bytes = *Bytes;
  return Plan;
}

void emitRegistrationCxxStack(RegistrationCxxStackPlan &Plan, va_t FunctionVA,
                              va_t CallbackVA) {
  auto &Parent = *Plan.Seed->getFunction();
  auto &Context = Parent.getContext();
  llvm::IRBuilder<> Entry(Parent.getEntryBlock().getTerminator());
  auto *Stack =
      Entry.CreateAlloca(llvm::ArrayType::get(Entry.getInt8Ty(), Plan.Bytes),
                         nullptr, "registration.catch.stack");
  Stack->setAlignment(llvm::Align(16));
  Plan.Allocation = Stack;
  Stack->setMetadata(
      windows_eh_md::RegistrationCatchStackAttachment,
      llvm::MDNode::get(Context,
                        {med_llvm_eh::mdUInt(Context, FunctionVA, 64),
                         med_llvm_eh::mdUInt(Context, CallbackVA, 64)}));
  auto EmitSeed = [&](llvm::StoreInst *Seed, int32_t Offset) {
    llvm::IRBuilder<> B(Seed);
    auto *Address = B.CreateInBoundsGEP(B.getInt8Ty(), Stack,
                                        B.getInt32(Plan.Bytes + Offset));
    Seed->setOperand(0, B.CreatePtrToInt(Address, B.getInt32Ty()));
  };
  EmitSeed(Plan.Seed, 0);
  for (const auto &Resume : Plan.Resumes) {
    EmitSeed(Resume.Seed, Resume.Offset);
    Resume.Seed->setMetadata(
        windows_eh_md::RegistrationRootAttachment,
        llvm::MDNode::get(
            Context,
            {med_llvm_eh::mdUInt(Context, 2, 8),
             med_llvm_eh::mdUInt(Context, FunctionVA, 64),
             med_llvm_eh::mdUInt(Context, Resume.TargetVA, 64),
             med_llvm_eh::mdUInt(Context, CallbackVA, 64),
             med_llvm_eh::mdUInt(Context, uint32_t(Resume.Offset), 32)}));
  }
  Plan.Seed->setMetadata(
      windows_eh_md::RegistrationRootAttachment,
      llvm::MDNode::get(Context,
                        {med_llvm_eh::mdUInt(Context, 1, 8),
                         med_llvm_eh::mdUInt(Context, Plan.Bytes, 64)}));
}

} // namespace neverd
