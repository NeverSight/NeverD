//===- RegistrationFrameXor.cpp - PE32 cookie value candidates ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode XOR recipes without treating an unproved memory read as a value.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/RegistrationFrameXor.h"

#include "neverd/Limits.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/llvm/RegistrationImageProof.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"

#include <utility>

namespace neverd::registration_frame {
namespace {
bool fullWord(const llvm::Type *Type) {
  return Type->isIntegerTy() && Type->getIntegerBitWidth() >= 32 &&
         Type->getIntegerBitWidth() <= 64;
}
bool pe32Pointer(const llvm::Type *Type) {
  return Type->isPointerTy() && Type->getPointerAddressSpace() == 0;
}
} // namespace

std::optional<XorValue> XorProof::evaluate(const llvm::Value *Value,
                                           unsigned Depth) {
  if (Exhausted || Work >= limits::kMaxRegistrationEHStateWork ||
      Depth > limits::backend::kRegistrationFrameAddressLocateDepthLimit) {
    Exhausted = true;
    return std::nullopt;
  }
  ++Work;
  if (!Addresses.hasPE32Layout() ||
      (!fullWord(Value->getType()) && !pe32Pointer(Value->getType())))
    return std::nullopt;
  const auto Found = Values.find(Value);
  if (Found != Values.end())
    return Found->second;
  if (!Active.insert(Value).second)
    return std::nullopt;
  llvm::scope_exit Pop([&] { Active.erase(Value); });
  auto Result = evaluateImpl(Value, Depth);
  if (!Exhausted)
    Values.emplace(Value, Result);
  return Result;
}

std::optional<XorValue> XorProof::evaluateImpl(const llvm::Value *Value,
                                               unsigned Depth) {
  if (const auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(Value))
    return XorValue{uint32_t(Constant->getZExtValue()), false, {}};
  if (const auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(Value))
    return evaluate(Freeze->getOperand(0), Depth + 1);
  if (const auto *Cast = llvm::dyn_cast<llvm::Operator>(Value);
      Cast && llvm::Instruction::isCast(Cast->getOpcode())) {
    if (const auto *I = llvm::dyn_cast<llvm::CastInst>(Value);
        I && I->hasPoisonGeneratingFlags())
      return std::nullopt;
    const auto *From = Cast->getOperand(0)->getType();
    const auto *To = Value->getType();
    switch (Cast->getOpcode()) {
    case llvm::Instruction::PtrToInt:
      if (!pe32Pointer(From) || !fullWord(To))
        return std::nullopt;
      break;
    case llvm::Instruction::IntToPtr:
      if (!fullWord(From) || !pe32Pointer(To))
        return std::nullopt;
      break;
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
      if (!fullWord(From) || !fullWord(To))
        return std::nullopt;
      break;
    case llvm::Instruction::BitCast:
      if (!pe32Pointer(From) || !pe32Pointer(To))
        return std::nullopt;
      break;
    default:
      return std::nullopt;
    }
    return evaluate(Cast->getOperand(0), Depth + 1);
  }
  if (const auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(Value)) {
    if (Binary->hasPoisonGeneratingFlags())
      return std::nullopt;
    if (Binary->getOpcode() == llvm::Instruction::Xor) {
      auto Left = evaluate(Binary->getOperand(0), Depth + 1);
      auto Right = evaluate(Binary->getOperand(1), Depth + 1);
      if (!Left || !Right)
        return std::nullopt;
      Left->Constant ^= Right->Constant;
      Left->Cookie ^= Right->Cookie;
      for (const auto &Term : Right->Addresses) {
        if (Work >= limits::kMaxRegistrationEHStateWork) {
          Exhausted = true;
          return std::nullopt;
        }
        ++Work;
        if (!Left->Addresses.erase(Term))
          Left->Addresses.insert(Term);
      }
      return Left;
    }
  }
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
    if (Load->isAtomic())
      return std::nullopt;
    const auto Slot = Addresses.locate(Load->getPointerOperand());
    const auto Range = Slot ? imageRange(*Slot, 4, CookieVA) : std::nullopt;
    if (CookieVA && Range && Range->Begin == CookieVA &&
        Load->getType()->isIntegerTy(32))
      return XorValue{0, true, {}};
    std::optional<XorValue> Common;
    auto Accept = [&](const llvm::StoreInst &Store) {
      auto Recipe = evaluate(Store.getValueOperand(), Depth + 1);
      if (!Recipe || (Common && *Common != *Recipe))
        return false;
      Common = std::move(Recipe);
      return true;
    };
    return Addresses.visitReachingStores(*Load, Accept, Depth) ? Common
                                                               : std::nullopt;
  }
  if (auto Address = Addresses.locate(Value))
    return XorValue{0, false, {*Address}};
  return std::nullopt;
}

std::optional<XorValue> XorProof::readWordAt(const Cell &Slot,
                                             const llvm::Instruction &Before) {
  if (!Addresses.hasPE32Layout())
    return std::nullopt;
  std::optional<XorValue> Common;
  auto Accept = [&](const llvm::StoreInst &Store) {
    auto Recipe = evaluate(Store.getValueOperand());
    if (!Recipe || (Common && *Common != *Recipe))
      return false;
    Common = std::move(Recipe);
    return true;
  };
  return Addresses.visitReachingStoresAt(Slot, 4, Before, Accept)
             ? Common
             : std::nullopt;
}

} // namespace neverd::registration_frame
