//===- SourceFrameBytes.h - Advisory stack byte metadata --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Carry display observations through LLVM without authorizing transformations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_SOURCEFRAMEBYTES_H
#define NEVERD_BACKEND_LLVM_SOURCEFRAMEBYTES_H

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceFrameBytes.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace neverd {

inline constexpr char kSourceFrameBytesMetadata[] = "neverd.source.frame.bytes";

inline void writeSourceFrameBytes(llvm::Function &Function,
                                  const std::vector<SourceFrameBytes> &Values) {
  Function.setMetadata(kSourceFrameBytesMetadata, nullptr);
  if (Values.empty())
    return;
  auto &Context = Function.getContext();
  auto *Integer = llvm::Type::getInt64Ty(Context);
  llvm::SmallVector<llvm::Metadata *> Entries;
  for (const auto &Value : Values) {
    llvm::Metadata *Fields[] = {
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(Integer, Value.Instruction)),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::getSigned(Integer, Value.FrameOffset)),
        llvm::MDString::get(
            Context,
            llvm::StringRef(reinterpret_cast<const char *>(Value.Bytes.data()),
                            Value.Bytes.size()))};
    Entries.push_back(llvm::MDNode::get(Context, Fields));
  }
  Function.setMetadata(kSourceFrameBytesMetadata,
                       llvm::MDNode::get(Context, Entries));
}

inline std::vector<SourceFrameBytes>
readSourceFrameBytes(const llvm::Function &Function) {
  const auto *Root = Function.getMetadata(kSourceFrameBytesMetadata);
  if (!Root ||
      Root->getNumOperands() > limits::ir::kSourceFrameTextObservations)
    return {};
  std::vector<SourceFrameBytes> Result;
  for (const auto &Operand : Root->operands()) {
    const auto *Node = llvm::dyn_cast_or_null<llvm::MDNode>(Operand.get());
    if (!Node || Node->getNumOperands() != 3)
      return {};
    const auto *Address =
        llvm::mdconst::dyn_extract<llvm::ConstantInt>(Node->getOperand(0));
    const auto *Offset =
        llvm::mdconst::dyn_extract<llvm::ConstantInt>(Node->getOperand(1));
    const auto *Bytes =
        llvm::dyn_cast_or_null<llvm::MDString>(Node->getOperand(2));
    if (!Address || !Offset || !Bytes || Address->getBitWidth() != 64 ||
        Offset->getBitWidth() != 64 || Offset->getSExtValue() >= 0 ||
        Offset->getSExtValue() < -limits::ir::kSourceFrameAnalysisMaxFrame ||
        Bytes->getLength() > limits::ir::kSourceFrameTextBytes ||
        Bytes->getLength() > uint64_t(-Offset->getSExtValue()))
      return {};
    SourceFrameBytes Value;
    Value.Instruction = Address->getZExtValue();
    Value.FrameOffset = Offset->getSExtValue();
    Value.Bytes.assign(Bytes->getString().bytes_begin(),
                       Bytes->getString().bytes_end());
    Result.push_back(std::move(Value));
  }
  return Result;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_SOURCEFRAMEBYTES_H
