//===- LLVMCallContract.cpp - Physical call argument bindings -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve recorded register banks and stack byte locations across source
/// call projections without depending on the machine-code LLVM emitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/LLVMCallContract.h"

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <string>

namespace neverd {
namespace {
constexpr auto ArgumentBanks = LLVMArgumentBanksAttribute;
} // namespace

std::optional<std::pair<unsigned, unsigned>>
getLLVMArgumentBanks(const llvm::Function &Function) {
  const auto Attribute = Function.getFnAttribute(ArgumentBanks);
  if (!Attribute.isStringAttribute())
    return std::nullopt;
  auto Parts = Attribute.getValueAsString().split(',');
  unsigned Integers, Floating;
  if (Parts.first.getAsInteger(10, Integers) ||
      Parts.second.getAsInteger(10, Floating) ||
      Integers > Function.arg_size() ||
      Floating > Function.arg_size() - Integers)
    return std::nullopt;
  return std::pair{Integers, Floating};
}

std::optional<std::pair<unsigned, unsigned>>
getLLVMArgumentBanks(const llvm::CallBase &Call) {
  const auto *Node = Call.getMetadata(ArgumentBanks);
  if (!Node || Node->getNumOperands() < 2)
    return std::nullopt;
  const auto *Integers =
      llvm::mdconst::dyn_extract<llvm::ConstantInt>(Node->getOperand(0));
  const auto *Floating =
      llvm::mdconst::dyn_extract<llvm::ConstantInt>(Node->getOperand(1));
  if (!Integers || !Floating || Integers->getBitWidth() != 32 ||
      Floating->getBitWidth() != 32 ||
      Integers->getZExtValue() > Call.arg_size() ||
      Floating->getZExtValue() > Call.arg_size() - Integers->getZExtValue())
    return std::nullopt;
  return std::pair{static_cast<unsigned>(Integers->getZExtValue()),
                   static_cast<unsigned>(Floating->getZExtValue())};
}

LLVMArgumentStackLayout getLLVMArgumentStackLayout(const llvm::CallBase &Call) {
  const auto *Node = Call.getMetadata(ArgumentBanks);
  if (!Node || Node->getNumOperands() < 3)
    return {};
  const auto *Slot =
      llvm::mdconst::dyn_extract<llvm::ConstantInt>(Node->getOperand(2));
  if (!Slot || Slot->getBitWidth() != 32)
    return {};
  LLVMArgumentStackLayout Result;
  for (unsigned I = 3; I < Node->getNumOperands(); ++I) {
    const auto *Offset =
        llvm::mdconst::dyn_extract<llvm::ConstantInt>(Node->getOperand(I));
    if (!Offset || Offset->getBitWidth() != 64)
      return {};
    Result.Offsets.push_back(Offset->getSExtValue());
  }
  Result.SlotBytes = static_cast<unsigned>(Slot->getZExtValue());
  return Result;
}

void setLLVMArgumentBanks(llvm::Function &Function, unsigned Integers,
                          unsigned Floating) {
  Function.addFnAttr(ArgumentBanks,
                     std::to_string(Integers) + "," + std::to_string(Floating));
}

void setLLVMArgumentBanks(llvm::CallBase &Call, unsigned Integers,
                          unsigned Floating,
                          llvm::ArrayRef<int64_t> StackOffsets,
                          unsigned SlotBytes) {
  llvm::IRBuilder<> Builder(Call.getContext());
  llvm::SmallVector<llvm::Metadata *> Operands = {
      llvm::ConstantAsMetadata::get(Builder.getInt32(Integers)),
      llvm::ConstantAsMetadata::get(Builder.getInt32(Floating))};
  if (!StackOffsets.empty() && SlotBytes > 0) {
    Operands.push_back(
        llvm::ConstantAsMetadata::get(Builder.getInt32(SlotBytes)));
    for (int64_t Offset : StackOffsets)
      Operands.push_back(
          llvm::ConstantAsMetadata::get(Builder.getInt64(Offset)));
  }
  Call.setMetadata(ArgumentBanks,
                   llvm::MDNode::get(Call.getContext(), Operands));
}

std::optional<IntegerCallArguments>
bindLLVMIntegerCallArguments(const llvm::CallBase &Call,
                             const IntegerCallArguments &Layout, Arch A,
                             BinaryFormat F) {
  const auto *Metadata = Call.getMetadata(ArgumentBanks);
  if (!Metadata)
    return Layout.Positions <= Call.arg_size() ? std::optional(Layout)
                                               : std::nullopt;
  const auto Banks = getLLVMArgumentBanks(Call);
  const auto Physical = getTargetRegInfo(A).integerArgumentLayout(F);
  if (!Banks || Banks->second || !Physical.SlotBytes)
    return std::nullopt;
  const unsigned Registers = A == Arch::X86 ? 0 : Physical.Registers.size();
  const auto Stack = getLLVMArgumentStackLayout(Call);
  const bool Located = Metadata->getNumOperands() > 2;
  if (Located && (Stack.SlotBytes != Physical.SlotBytes ||
                  Stack.Offsets.size() != Call.arg_size() - Banks->first))
    return std::nullopt;
  IntegerCallArguments Bound = Layout;
  Bound.Positions = 0;
  const auto Bind = [&](unsigned &Index, unsigned Bytes) {
    if (Index < Registers) {
      if (Index >= Banks->first)
        return false;
    } else {
      const uint64_t Offset = uint64_t(Index - Registers) * Physical.SlotBytes;
      unsigned Slot = Index - Registers;
      if (Located) {
        const auto Found =
            llvm::find(Stack.Offsets, static_cast<int64_t>(Offset));
        if (Found == Stack.Offsets.end() ||
            llvm::count(Stack.Offsets, static_cast<int64_t>(Offset)) != 1)
          return false;
        Slot = static_cast<unsigned>(Found - Stack.Offsets.begin());
      }
      if (Slot >= Call.arg_size() - Banks->first)
        return false;
      Index = Banks->first + Slot;
    }
    auto *Type = Call.getArgOperand(Index)->getType();
    const unsigned Bits =
        Type->isIntegerTy() ? Type->getIntegerBitWidth()
        : Type->isPointerTy()
            ? Call.getModule()->getDataLayout().getPointerTypeSizeInBits(Type)
            : 0;
    if (Bits < Bytes * 8 || Bits > Physical.SlotBytes * 8)
      return false;
    Bound.Positions = std::max(Bound.Positions, Index + 1);
    return true;
  };
  if (Bound.ResultPointer &&
      !Bind(*Bound.ResultPointer, getTargetRegInfo(A).PointerSize))
    return std::nullopt;
  for (auto &Parameter : Bound.Parameters)
    for (auto &Piece : Parameter.Pieces)
      if (Piece.Floating || !Bind(Piece.Argument, Piece.Bytes))
        return std::nullopt;
  return Bound;
}

} // namespace neverd
