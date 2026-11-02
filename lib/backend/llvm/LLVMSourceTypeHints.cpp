//===- LLVMSourceTypeHints.cpp - Source pointees through opaque pointers
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve bounded display-only pointer chains on their LLVM arguments.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/LLVMSourceTypeHints.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace neverd {
namespace {
bool pointerCarrier(const llvm::Argument &Argument, unsigned Bytes) {
  const auto *Pointer = llvm::dyn_cast<llvm::PointerType>(Argument.getType());
  return Pointer && Pointer->getAddressSpace() == 0 &&
         Argument.getParent()->getParent()->getDataLayout().getPointerSize() ==
             Bytes;
}

bool identifier(llvm::StringRef Name) {
  return !Name.empty() &&
         (llvm::isAlpha(Name.front()) || Name.front() == '_') &&
         std::all_of(Name.begin(), Name.end(),
                     [](char C) { return llvm::isAlnum(C) || C == '_'; });
}
} // namespace

void writeLLVMSourcePointerParameter(llvm::Argument &Argument,
                                     const TypeRef &Type) {
  Argument.getParent()->removeParamAttr(Argument.getArgNo(),
                                        LLVMSourcePointerParameter);
  if (!Type || Type->Kind != NdTypeKind::Ptr ||
      !pointerCarrier(Argument, Type->Size) || !equalSourceTypes(Type, Type))
    return;
  llvm::json::Array Chain;
  for (auto Node = Type; Node; Node = Node->Pointee) {
    llvm::StringRef Kind;
    switch (Node->Kind) {
    case NdTypeKind::Ptr:
      Kind = "ptr";
      break;
    case NdTypeKind::Void:
      Kind = "void";
      break;
    case NdTypeKind::Int:
      Kind = "int";
      break;
    case NdTypeKind::Float:
      Kind = "float";
      break;
    case NdTypeKind::Struct:
      if (Node->IsEnum || !identifier(Node->SourceName))
        return;
      Kind = "record";
      break;
    default:
      return;
    }
    if (!Node->SourceName.empty() && !identifier(Node->SourceName))
      return;
    // A record's name is enough for a pointer spelling. Do not transport a
    // layout or infer one from a prototype's nominal object size.
    Chain.push_back(llvm::json::Array{
        Kind, Node->Kind == NdTypeKind::Struct ? 0 : Node->Size, Node->IsSigned,
        Node->SourceName});
    if (Node->Kind != NdTypeKind::Ptr)
      break;
  }
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  Out << llvm::json::Value(std::move(Chain));
  Argument.addAttr(llvm::Attribute::get(Argument.getContext(),
                                        LLVMSourcePointerParameter, Text));
}

TypeRef readLLVMSourcePointerParameter(const llvm::Argument &Argument) {
  const auto Attr = Argument.getParent()->getAttributes().getParamAttr(
      Argument.getArgNo(), LLVMSourcePointerParameter);
  if (!Attr.isStringAttribute())
    return nullptr;
  auto Parsed = llvm::json::parse(Attr.getValueAsString());
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return nullptr;
  }
  const auto *Chain = Parsed->getAsArray();
  // Source ABI equality checks an opaque record at its owning pointer's depth;
  // the serialized chain also contains that record's leaf entry.
  if (!Chain || Chain->size() < 2 ||
      Chain->size() > limits::ir::kSourceABIEqualTypesDepthLimit + 2u)
    return nullptr;
  TypeRef Result;
  for (size_t Index = Chain->size(); Index; --Index) {
    const auto *Fields = (*Chain)[Index - 1].getAsArray();
    if (!Fields || Fields->size() != 4)
      return nullptr;
    const auto Kind = (*Fields)[0].getAsString();
    const auto Bytes = (*Fields)[1].getAsInteger();
    const auto Signed = (*Fields)[2].getAsBoolean();
    const auto Name = (*Fields)[3].getAsString();
    if (!Kind || !Bytes || !Signed || !Name || *Bytes < 0 ||
        *Bytes > std::numeric_limits<uint16_t>::max() ||
        (!Name->empty() && !identifier(*Name)))
      return nullptr;
    TypeRef Node;
    if (*Kind == "ptr" && Result)
      Node = NdType::makePtr(Result);
    else if (Result)
      return nullptr;
    else if (*Kind == "void")
      Node = NdType::makeVoid();
    else if (*Kind == "int")
      Node = NdType::makeInt(*Bytes, *Signed);
    else if (*Kind == "float")
      Node = NdType::makeFloat(*Bytes);
    else if (*Kind == "record" && !Name->empty() && !*Bytes && !*Signed)
      Node = NdType::makeNamedRecord(Name->str());
    else
      return nullptr;
    Node->Size = *Bytes;
    Node->IsSigned = *Signed;
    Node->SourceName = Name->str();
    Result = std::move(Node);
  }
  return Result->Kind == NdTypeKind::Ptr &&
                 pointerCarrier(Argument, Result->Size) &&
                 equalSourceTypes(Result, Result)
             ? Result
             : nullptr;
}
} // namespace neverd
