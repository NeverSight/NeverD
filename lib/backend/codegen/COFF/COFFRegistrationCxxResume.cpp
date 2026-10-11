//===- COFFRegistrationCxxResume.cpp - Suspended callback definitions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind edited LLVM resume seeds to exact checked source stack coordinates.
//===----------------------------------------------------------------------===//
#include "COFFRegistrationCxxIRProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/RegistrationFrameAddress.h"
#include "neverd/ir/med/X86RegistrationCallback.h"

#include "llvm/IR/Dominators.h"

namespace neverd::coff_registration {
llvm::Error bindCxxCatchResumes(CxxIRControlProof &Proof, const MedFunc &Source,
                                const llvm::Function &Function) {
  struct Definition {
    RegistrationCallbackStackCoordinate Coordinate;
    const CxxIRCatch *Catch;
    const llvm::BasicBlock *Block;
  };
  std::map<va_t, Definition> Expected;
  const auto &Tries = Source.ExceptionMetadata->Cxx->TryBlocks;
  size_t Work = 0;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("C++ resume source scan exceeded its work budget");
      if (Op.RegistrationRoot !=
          MedOp::RegistrationRootKind::RestoredCallbackStackPointer)
        continue;
      const auto Coordinate = registrationCallbackStackCoordinate(Source, Op);
      const CxxIRCatch *Owner = nullptr;
      if (Coordinate)
        for (const auto &[Identity, Catch] : Proof.Catches) {
          if (++Work > limits::kMaxRegistrationEHStateWork)
            return rejectIR("C++ resume owners exceeded their work budget");
          if (Tries[Identity.first].Handlers[Identity.second].HandlerVA ==
              Coordinate->Entry)
            Owner = &Catch;
        }
      const llvm::BasicBlock *Entry = nullptr;
      for (const auto &State : Source.RegistrationStates->Blocks) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("C++ resume segments exceeded their work budget");
        if (State.Range.Begin == Block.StartAddr &&
            State.Range.End == Block.EndAddr &&
            Proof.Segments.count(State.BlockId))
          Entry = Proof.Segments.at(State.BlockId).Enter->getParent();
      }
      if (!Coordinate || !Owner || !Owner->Stack || !Entry ||
          !Owner->Blocks.count(Entry) ||
          !Expected.emplace(Op.Addr, Definition{*Coordinate, Owner, Entry})
               .second)
        return rejectIR("C++ resume has no unique source stack definition");
    }
  llvm::DominatorTree Dominators(const_cast<llvm::Function &>(Function));
  std::set<const llvm::AllocaInst *> Destinations;
  for (const auto &Block : Function)
    for (const auto &I : Block) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("C++ resume LLVM scan exceeded its work budget");
      const auto *MD = I.getMetadata(windows_eh_md::RegistrationRootAttachment);
      if (!MD || metadataInteger(*MD, 0, 8) != 2)
        continue;
      const auto Target = metadataInteger(*MD, 2, 64);
      const auto Found = Target ? Expected.find(*Target) : Expected.end();
      const auto *Seed = llvm::dyn_cast<llvm::StoreInst>(&I);
      if (Found == Expected.end() || !Seed || Seed->isAtomic() ||
          Seed->isVolatile() || &Block != Found->second.Block ||
          MD->getNumOperands() != 5 ||
          metadataInteger(*MD, 1, 64) != Source.Entry ||
          metadataInteger(*MD, 3, 64) != Found->second.Coordinate.Entry ||
          metadataInteger(*MD, 4, 32) !=
              uint32_t(Found->second.Coordinate.Offset))
        return rejectIR("C++ resume seed changed its exact source identity");
      const auto &Definition = Found->second;
      const auto *Value =
          llvm::dyn_cast<llvm::PtrToIntInst>(Seed->getValueOperand());
      const auto *Address = Value ? llvm::dyn_cast<llvm::GetElementPtrInst>(
                                        Value->getPointerOperand())
                                  : nullptr;
      const auto Size = Definition.Catch->Stack->getAllocationSize(
          Function.getParent()->getDataLayout());
      const auto *Destination =
          llvm::dyn_cast<llvm::AllocaInst>(Seed->getPointerOperand());
      if (!Size || Size->isScalable() || !Value ||
          !Value->getType()->isIntegerTy(32) || !Address ||
          Address->getPointerOperand() != Definition.Catch->Stack ||
          int64_t(Size->getFixedValue()) + Definition.Coordinate.Offset < 0 ||
          registration_frame::checkedByteGEPOffset(Address) !=
              int64_t(Size->getFixedValue()) + Definition.Coordinate.Offset ||
          !Destination || !Destinations.insert(Destination).second ||
          !Destination->isStaticAlloca() || Destination->getAddressSpace() ||
          Destination->getParent() != &Function.getEntryBlock() ||
          Destination->getAllocationSize(
              Function.getParent()->getDataLayout()) !=
              llvm::TypeSize::getFixed(4) ||
          !Destination->getAllocatedType()->isIntegerTy(32))
        return rejectIR("C++ resume changed its suspended stack address");
      for (const auto *User : Destination->users()) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("C++ resume uses exceeded their work budget");
        if (User == Seed)
          continue;
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
          const auto *Zero =
              llvm::dyn_cast<llvm::ConstantInt>(Store->getValueOperand());
          if (Store->getParent() == &Function.getEntryBlock() && Zero &&
              Zero->isZero() && Zero->getType()->isIntegerTy(32) &&
              !Store->isAtomic() && !Store->isVolatile() &&
              Store->getPointerOperand() == Destination)
            continue;
        }
        const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
        if (!Load || !Definition.Catch->Blocks.count(Load->getParent()) ||
            Load->isAtomic() || Load->isVolatile() ||
            !Dominators.dominates(Seed, Load))
          return rejectIR("C++ resumed ESP escaped its suspended invocation");
      }
      Expected.erase(Found);
    }
  return Expected.empty()
             ? llvm::Error::success()
             : rejectIR("C++ callback omitted a checked resume seed");
}
} // namespace neverd::coff_registration
