//===- SourceImageBytes.h - Conditional byte metadata -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Encode and validate advisory conditional-byte LLVM metadata.

#ifndef NEVERD_BACKEND_LLVM_SOURCEIMAGEBYTES_H
#define NEVERD_BACKEND_LLVM_SOURCEIMAGEBYTES_H

#include "neverd/ir/SourceImageBytes.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Casting.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace neverd {

inline constexpr char kSourceImageBytesMetadata[] = "neverd.source.image.bytes";

/// Advisory metadata only. LLVM instructions never consume these byte facts.
inline void writeSourceImageBytes(llvm::Function &Function,
                                  const SourceImageByteObservations &Values) {
  Function.setMetadata(kSourceImageBytesMetadata, nullptr);
  if (Values.Complete && Values.Values.empty())
    return;
  auto &Context = Function.getContext();
  auto Integer = [&](uint64_t Value, unsigned Width = 64) -> llvm::Metadata * {
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::IntegerType::get(Context, Width), Value));
  };
  llvm::SmallVector<llvm::Metadata *> Entries{Integer(Values.Complete, 1)};
  for (const auto &Value : Values.Values) {
    llvm::SmallVector<llvm::Metadata *> Inputs;
    for (const auto &Input : Value.Inputs) {
      llvm::Metadata *Fields[] = {Integer(Input.Address),
                                  Integer(Input.Initializer),
                                  Integer(Input.Value, 8)};
      Inputs.push_back(llvm::MDNode::get(Context, Fields));
    }
    llvm::SmallVector<llvm::Metadata *> Fields = {
        Integer(Value.Block), Integer(Value.Instruction),
        Integer(Value.Address),
        llvm::MDString::get(
            Context,
            llvm::StringRef(reinterpret_cast<const char *>(Value.Bytes.data()),
                            Value.Bytes.size())),
        llvm::MDNode::get(Context, Inputs)};
    if (!Value.EntryRegisters.empty()) {
      llvm::SmallVector<llvm::Metadata *> Registers;
      for (const auto &Input : Value.EntryRegisters) {
        llvm::Metadata *Fields[] = {Integer(Input.Offset),
                                    Integer(Input.Bytes, 16),
                                    Integer(Input.Value)};
        Registers.push_back(llvm::MDNode::get(Context, Fields));
      }
      Fields.push_back(llvm::MDNode::get(Context, Registers));
    }
    Entries.push_back(llvm::MDNode::get(Context, Fields));
  }
  Function.setMetadata(kSourceImageBytesMetadata,
                       llvm::MDNode::get(Context, Entries));
}

inline SourceImageByteObservations
readSourceImageBytes(const llvm::Function &Function) {
  const auto *Root = Function.getMetadata(kSourceImageBytesMetadata);
  if (!Root)
    return {};
  const SourceImageByteObservations Invalid{{}, false};
  if (!Root->getNumOperands())
    return Invalid;
  auto Integer = [](const llvm::MDOperand &Operand, unsigned Width = 64) {
    const auto *Number = llvm::mdconst::dyn_extract<llvm::ConstantInt>(Operand);
    return Number && Number->getBitWidth() == Width ? Number : nullptr;
  };
  const auto *Complete = Integer(Root->getOperand(0), 1);
  if (!Complete)
    return Invalid;
  SourceImageByteObservations Result;
  Result.Complete = !Complete->isZero();
  for (unsigned I = 1; I < Root->getNumOperands(); ++I) {
    const auto *Node =
        llvm::dyn_cast_or_null<llvm::MDNode>(Root->getOperand(I));
    if (!Node || (Node->getNumOperands() != 5 && Node->getNumOperands() != 6))
      return Invalid;
    const auto *Block = Integer(Node->getOperand(0));
    const auto *Instruction = Integer(Node->getOperand(1));
    const auto *Address = Integer(Node->getOperand(2));
    const auto *Bytes =
        llvm::dyn_cast_or_null<llvm::MDString>(Node->getOperand(3));
    const auto *Inputs =
        llvm::dyn_cast_or_null<llvm::MDNode>(Node->getOperand(4));
    const auto *Registers =
        Node->getNumOperands() == 6
            ? llvm::dyn_cast_or_null<llvm::MDNode>(Node->getOperand(5))
            : nullptr;
    if (!Block || !Instruction || !Address || !Bytes || !Inputs ||
        Bytes->getLength() < 2 || Bytes->getString().back() ||
        Bytes->getLength() > InvalidVA - Address->getZExtValue() ||
        !Inputs->getNumOperands() ||
        (Node->getNumOperands() == 6 &&
         (!Registers || !Registers->getNumOperands())))
      return Invalid;
    SourceImageBytes Value;
    Value.Block = Block->getZExtValue();
    Value.Instruction = Instruction->getZExtValue();
    Value.Address = Address->getZExtValue();
    Value.Bytes.assign(Bytes->getString().bytes_begin(),
                       Bytes->getString().bytes_end());
    for (const auto &Operand : Inputs->operands()) {
      const auto *Input = llvm::dyn_cast_or_null<llvm::MDNode>(Operand.get());
      if (!Input || Input->getNumOperands() != 3)
        return Invalid;
      const auto *At = Integer(Input->getOperand(0));
      const auto *Initializer = Integer(Input->getOperand(1));
      const auto *Byte = Integer(Input->getOperand(2), 8);
      if (!At || !Initializer || !Byte ||
          (!Value.Inputs.empty() &&
           Value.Inputs.back().Address >= At->getZExtValue()))
        return Invalid;
      Value.Inputs.push_back({At->getZExtValue(), Initializer->getZExtValue(),
                              static_cast<uint8_t>(Byte->getZExtValue())});
    }
    if (Registers)
      for (const auto &Operand : Registers->operands()) {
        const auto *Input = llvm::dyn_cast_or_null<llvm::MDNode>(Operand.get());
        if (!Input || Input->getNumOperands() != 3)
          return Invalid;
        const auto *Offset = Integer(Input->getOperand(0));
        const auto *Size = Integer(Input->getOperand(1), 16);
        const auto *Bits = Integer(Input->getOperand(2));
        if (!Offset || !Size || !Bits || Size->isZero() ||
            Size->getZExtValue() > 8 ||
            Offset->getZExtValue() > UINT64_MAX - Size->getZExtValue() ||
            (Size->getZExtValue() < 8 &&
             Bits->getZExtValue() >> (Size->getZExtValue() * 8)) ||
            (!Value.EntryRegisters.empty() &&
             Value.EntryRegisters.back().Offset +
                     Value.EntryRegisters.back().Bytes >
                 Offset->getZExtValue()))
          return Invalid;
        Value.EntryRegisters.push_back(
            {Offset->getZExtValue(),
             static_cast<uint16_t>(Size->getZExtValue()),
             Bits->getZExtValue()});
      }
    Result.Values.push_back(std::move(Value));
  }
  return Result;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_SOURCEIMAGEBYTES_H
