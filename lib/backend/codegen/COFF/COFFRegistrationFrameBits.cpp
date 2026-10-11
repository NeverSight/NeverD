//===- COFFRegistrationFrameBits.cpp - PE32 register slice dependencies --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove that a scalar register slice discards a preserved pointer's bits.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationFrameBits.h"

#include "COFFRegistrationFrameStores.h"

#include "neverd/Limits.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include <functional>
#include <map>
#include <set>
#include <utility>

namespace neverd::coff_registration {
std::optional<bool> truncationMayDependOnFrame(
    const llvm::TruncInst &Trunc,
    llvm::function_ref<bool(const llvm::Value *)> IsAddress,
    const RegistrationFrameStores &Stores, size_t &Work) {
  const auto *Source = Trunc.getOperand(0);
  if (!Source->getType()->isIntegerTy() || !Trunc.getType()->isIntegerTy() ||
      Source->getType()->getIntegerBitWidth() > 64 ||
      Trunc.hasPoisonGeneratingFlags())
    return true;
  std::set<const llvm::Value *> Active;
  std::map<std::pair<const llvm::Value *, uint64_t>, bool> Results;
  std::function<std::optional<bool>(const llvm::Value *, llvm::APInt)> Query;
  Query = [&](const llvm::Value *Value,
              llvm::APInt Demand) -> std::optional<bool> {
    if (Work >= limits::kMaxRegistrationEHStateWork || Active.size() >= 128)
      return std::nullopt;
    ++Work;
    if (Demand.isZero() || !IsAddress(Value))
      return false;
    if (!Value->getType()->isIntegerTy(Demand.getBitWidth()))
      return true;
    const auto Key = std::make_pair(Value, Demand.getZExtValue());
    if (const auto Found = Results.find(Key); Found != Results.end())
      return Found->second;
    if (!Active.insert(Value).second)
      return true;
    llvm::scope_exit Pop([&] { Active.erase(Value); });
    auto Compute = [&]() -> std::optional<bool> {
      if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value)) {
        if (Cast->hasPoisonGeneratingFlags() ||
            !Cast->getOperand(0)->getType()->isIntegerTy())
          return true;
        const unsigned Width =
            Cast->getOperand(0)->getType()->getIntegerBitWidth();
        if (Width > 64)
          return true;
        if (Cast->getOpcode() == llvm::Instruction::Trunc)
          return Query(Cast->getOperand(0), Demand.zext(Width));
        if (Cast->getOpcode() == llvm::Instruction::ZExt)
          return Query(Cast->getOperand(0), Demand.trunc(Width));
        if (Cast->getOpcode() == llvm::Instruction::SExt) {
          auto Narrow = Demand.trunc(Width);
          if (!Demand.lshr(Width).isZero())
            Narrow.setBit(Width - 1);
          return Query(Cast->getOperand(0), Narrow);
        }
        return true;
      }
      if (const auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(Value)) {
        if (Binary->hasPoisonGeneratingFlags())
          return true;
        const auto *Right =
            llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(1));
        if (Right && Binary->getOpcode() == llvm::Instruction::And)
          return Query(Binary->getOperand(0), Demand & Right->getValue());
        if (Right && (Binary->getOpcode() == llvm::Instruction::Shl ||
                      Binary->getOpcode() == llvm::Instruction::LShr)) {
          const uint64_t Shift = Right->getValue().getLimitedValue();
          if (Shift >= Demand.getBitWidth())
            return true;
          return Query(Binary->getOperand(0),
                       Binary->getOpcode() == llvm::Instruction::Shl
                           ? Demand.lshr(Shift)
                           : Demand.shl(Shift));
        }
        if (Binary->getOpcode() == llvm::Instruction::Or ||
            Binary->getOpcode() == llvm::Instruction::Xor) {
          const auto Left = Query(Binary->getOperand(0), Demand);
          if (!Left || *Left)
            return Left;
          return Query(Binary->getOperand(1), Demand);
        }
        return true;
      }
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
        if (Load->isAtomic())
          return true;
        const auto Definitions =
            Stores.directDefinitions(Load->getPointerOperand(), Work);
        if (!Definitions || Definitions->empty())
          return true;
        for (const auto *Store : *Definitions) {
          if (Store->isAtomic() ||
              Store->getValueOperand()->getType() != Load->getType())
            return true;
          const auto Depends = Query(Store->getValueOperand(), Demand);
          if (!Depends || *Depends)
            return Depends;
        }
        return false;
      }
      return true;
    };
    const auto Result = Compute();
    if (Result)
      Results.emplace(Key, *Result);
    return Result;
  };
  return Query(Source, llvm::APInt::getLowBitsSet(
                           Source->getType()->getIntegerBitWidth(),
                           Trunc.getType()->getIntegerBitWidth()));
}
} // namespace neverd::coff_registration
