//===- RegistrationSEHThread.cpp - PE32 thread read identity -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind runtime thread fields to source loads and actual generated addresses.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"
#include "neverd/backend/llvm/RegistrationFrameProofAddress.h"
#include "neverd/backend/llvm/RegistrationFrameStores.h"
#include "neverd/backend/llvm/X86RegistrationSEHThread.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"

namespace neverd {

std::optional<std::set<const llvm::LoadInst *>> getCheckedX86SEHThreadReads(
    const MedFunc &Source, const llvm::Function &Parent,
    const std::map<std::pair<va_t, int>, const llvm::Instruction *> &Operations,
    bool Volatile) {
  using Thread = RegistrationSEHThreadAccess;
  using registration_frame::Cell;
  const auto Expected = getX86RegistrationSEHThreadAccesses(Source);
  if (!Expected)
    return std::nullopt;
  std::set<const llvm::LoadInst *> Result, Roots;
  for (const auto &[Identity, Access] : *Expected) {
    const auto Found = Operations.find(Identity);
    const auto *Load =
        Found == Operations.end()
            ? nullptr
            : llvm::dyn_cast_or_null<llvm::LoadInst>(Found->second);
    if (!Load || Load->getFunction() != &Parent ||
        !Load->getType()->isIntegerTy(Thread::WordBytes * 8) ||
        Load->isAtomic() || Load->isVolatile() != Volatile ||
        Load->getAlign() != llvm::Align(1) || !Result.insert(Load).second)
      return std::nullopt;
    const auto *Pointer = Load->getPointerOperand();
    if (Access.Field == Thread::Kind::Self) {
      const auto *Cast = llvm::dyn_cast<llvm::ConstantExpr>(Pointer);
      const auto *Offset =
          Cast && Cast->getOpcode() == llvm::Instruction::IntToPtr
              ? llvm::dyn_cast<llvm::ConstantInt>(Cast->getOperand(0))
              : nullptr;
      if (Pointer->getType()->getPointerAddressSpace() !=
              llvmX86MemoryAddressSpace(NdMemoryAddressSpace::X86FS) ||
          !Offset ||
          (Offset->getBitWidth() != Thread::WordBytes * 8 &&
           Offset->getBitWidth() != sizeof(va_t) * 8) ||
          Offset->getZExtValue() != Thread::SelfOffset)
        return std::nullopt;
      Roots.insert(Load);
    } else if (Pointer->getType()->getPointerAddressSpace() != 0)
      return std::nullopt;
  }
  if (Result.empty())
    return Result;
  size_t Work = 0;
  bool Exhausted = false;
  coff_registration::RegistrationFrameStores Stores;
  if (!Stores.build({&Parent}, Work))
    return std::nullopt;
  auto Project =
      [&](const llvm::LoadInst &Load,
          registration_frame::AddressProof::Lookup) -> std::optional<Cell> {
    return Roots.count(&Load) ? std::optional<Cell>{{&Load, 0}} : std::nullopt;
  };
  registration_frame::AddressProof Addresses(Parent, nullptr, 0, Stores, Work,
                                             Exhausted, Project);
  if (!Addresses.hasPE32Layout())
    return std::nullopt;
  llvm::DominatorTree Dominators(*const_cast<llvm::Function *>(&Parent));
  for (const auto &[Identity, Access] : *Expected) {
    if (Access.Field == Thread::Kind::Self)
      continue;
    const auto *Load = llvm::cast<llvm::LoadInst>(Operations.at(Identity));
    const auto *Root = Operations.at({Access.RootAddress, Access.RootOpSeq});
    const auto Where = Addresses.locate(Load->getPointerOperand());
    if (Exhausted || !Where || Where->Root != Root ||
        Where->Offset != Thread::StackBaseOffset ||
        !Dominators.dominates(Root, Load))
      return std::nullopt;
  }
  return Result;
}

} // namespace neverd
