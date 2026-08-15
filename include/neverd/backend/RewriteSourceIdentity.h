//===- RewriteSourceIdentity.h - Original entry provenance ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the format-neutral IR attachment that binds a lifted function to
/// its exact entry in the source image.  The binary-rewrite compiler preserves
/// the IR function identity independently; patchers join the two identities by
/// exact source-function name and never recover an address from symbol spelling.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_REWRITESOURCEIDENTITY_H
#define NEVERD_BACKEND_REWRITESOURCEIDENTITY_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <system_error>

namespace neverd::rewrite_source {

inline constexpr llvm::StringLiteral
    FunctionAttachment("neverd.rewrite.source-identity");
inline constexpr uint32_t SchemaVersion = 1;

enum Operand : unsigned {
  Version = 0,
  OriginalVA = 1,
  OperandCount = 2,
};

inline void setOriginalVA(llvm::Function &Function, uint64_t Address) {
  llvm::LLVMContext &Context = Function.getContext();
  auto UInt = [&](uint64_t Value, unsigned Width) -> llvm::Metadata * {
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::IntegerType::get(Context, Width), Value));
  };
  Function.setMetadata(FunctionAttachment,
                       llvm::MDNode::get(Context,
                                         {UInt(SchemaVersion, 32),
                                          UInt(Address, 64)}));
}

inline llvm::Expected<std::optional<uint64_t>>
getOriginalVA(const llvm::Function &Function) {
  const llvm::MDNode *Node = Function.getMetadata(FunctionAttachment);
  if (!Node)
    return std::optional<uint64_t>();
  if (Node->getNumOperands() != OperandCount)
    return llvm::createStringError(
        std::errc::invalid_argument,
        "rewrite source identity has an invalid operand count");

  auto ReadUInt = [&](unsigned Index, unsigned Width)
      -> std::optional<uint64_t> {
    const auto *Constant = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
        Node->getOperand(Index).get());
    const auto *Integer =
        Constant ? llvm::dyn_cast<llvm::ConstantInt>(Constant->getValue())
                 : nullptr;
    if (!Integer || Integer->getBitWidth() != Width)
      return std::nullopt;
    return Integer->getZExtValue();
  };

  const std::optional<uint64_t> VersionValue = ReadUInt(Version, 32);
  const std::optional<uint64_t> Address = ReadUInt(OriginalVA, 64);
  if (!VersionValue || *VersionValue != SchemaVersion || !Address)
    return llvm::createStringError(
        std::errc::invalid_argument,
        "rewrite source identity has an invalid schema or address");
  return Address;
}

} // namespace neverd::rewrite_source

#endif // NEVERD_BACKEND_REWRITESOURCEIDENTITY_H
