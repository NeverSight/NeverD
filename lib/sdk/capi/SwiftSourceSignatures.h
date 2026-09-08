#ifndef NEVERD_SDK_CAPI_SWIFTSOURCESIGNATURES_H
#define NEVERD_SDK_CAPI_SWIFTSOURCESIGNATURES_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Swift/SwiftMetadata.h"
#include "neverd/loader/Swift/SwiftMethods.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <stdexcept>

namespace neverd::sdk::swift_source {

inline bool identifier(llvm::StringRef Text) {
  auto Letter = [](char C) {
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_';
  };
  return !Text.empty() && Text.size() <= 1024 && Letter(Text.front()) &&
         std::all_of(Text.begin(), Text.end(), [&](char C) {
           return Letter(C) || (C >= '0' && C <= '9');
         });
}

inline std::string text(const llvm::json::Object &Object, llvm::StringRef Key,
                        bool AllowEmpty = false) {
  auto Value = Object.getString(Key);
  if (!Value || (!AllowEmpty && !identifier(*Value)) || Value->size() > 65536)
    throw std::invalid_argument("invalid Swift signature field: " + Key.str());
  return Value->str();
}

inline SwiftSourceType type(const llvm::json::Object *Object,
                            unsigned Depth = 0) {
  if (!Object || Depth > 8)
    throw std::invalid_argument("missing or excessive Swift type description");
  SwiftSourceType Result;
  Result.Name = text(*Object, "name");
  const auto Kind = text(*Object, "kind");
  const auto Bits = Object->getInteger("bits");
  if (Kind == "void" && Result.Name == "Void") {
    Result.TheKind = SwiftSourceType::Kind::Void;
  } else if (Kind == "bool" && Result.Name == "Bool" && Bits && *Bits == 1) {
    Result.TheKind = SwiftSourceType::Kind::Boolean;
    Result.Bits = 1;
  } else if (Kind == "integer" && Bits &&
             (*Bits == 8 || *Bits == 16 || *Bits == 32 || *Bits == 64)) {
    auto Signed = Object->getBoolean("signed");
    if (!Signed)
      throw std::invalid_argument("Swift integer signedness is missing");
    const std::string Prefix = *Signed ? "Int" : "UInt";
    if (Result.Name != Prefix + std::to_string(*Bits) &&
        !(Result.Name == Prefix && *Bits == 64))
      throw std::invalid_argument(
          "Swift integer spelling disagrees with width");
    Result.TheKind = SwiftSourceType::Kind::Integer;
    Result.Bits = static_cast<unsigned>(*Bits);
    Result.IsSigned = *Signed;
  } else if (Kind == "float" && Bits &&
             ((Result.Name == "Float" && *Bits == 32) ||
              (Result.Name == "Double" && *Bits == 64))) {
    Result.TheKind = SwiftSourceType::Kind::Floating;
    Result.Bits = static_cast<unsigned>(*Bits);
  } else if (Kind == "pointer") {
    Result.TheKind = SwiftSourceType::Kind::Pointer;
    if (Result.Name == "UnsafeRawPointer" ||
        Result.Name == "UnsafeMutableRawPointer") {
      if (Object->get("pointee"))
        throw std::invalid_argument(
            "raw Swift pointer has an unexpected pointee");
    } else if (Result.Name == "UnsafePointer" ||
               Result.Name == "UnsafeMutablePointer") {
      Result.Pointee = std::make_shared<SwiftSourceType>(
          type(Object->getObject("pointee"), Depth + 1));
      if (Result.Pointee->TheKind == SwiftSourceType::Kind::Void)
        throw std::invalid_argument("typed Swift pointer has a void pointee");
    } else {
      throw std::invalid_argument("unsupported Swift pointer declaration");
    }
  } else {
    throw std::invalid_argument("unsupported Swift signature type");
  }
  return Result;
}

inline TypeRef nativeType(const SwiftSourceType &Type) {
  switch (Type.TheKind) {
  case SwiftSourceType::Kind::Void:
    return NdType::makeVoid();
  case SwiftSourceType::Kind::Boolean:
    return NdType::makeInt(1, false);
  case SwiftSourceType::Kind::Integer:
    return NdType::makeInt(Type.Bits / 8, Type.IsSigned);
  case SwiftSourceType::Kind::Floating:
    return NdType::makeFloat(Type.Bits / 8);
  case SwiftSourceType::Kind::Pointer:
    return NdType::makePtr(Type.Pointee ? nativeType(*Type.Pointee)
                                        : NdType::makeVoid());
  }
  return nullptr;
}

inline SwiftSourceSignature
signature(const llvm::json::Object &Object, const BinaryImage &Image,
          const std::vector<SwiftRecoveredType> &NativeTypes = {}) {
  SwiftSourceSignature Result;
  auto Entry = Object.getString("entry");
  auto Symbol = Object.getString("mangled_symbol");
  if (!Entry || !Symbol || Symbol->size() > 65536 ||
      !Entry->consume_front("0x") || Entry->empty() ||
      Entry->getAsInteger(16, Result.Entry) ||
      !Image.isCodeAddress(Result.Entry))
    throw std::invalid_argument("invalid or non-executable Swift symbol entry");
  Result.MangledSymbol = Symbol->str();
  bool Matched = false;
  for (const auto &NativeSymbol : Image.Symbols)
    if (NativeSymbol.IsFunc && NativeSymbol.Addr == Result.Entry &&
        (NativeSymbol.Name == Symbol->str() ||
         NativeSymbol.Name == "_" + Symbol->str() ||
         "_" + NativeSymbol.Name == Symbol->str())) {
      Matched = true;
      break;
    }
  if (!Matched)
    throw std::invalid_argument("Swift symbol does not match its image entry");
  Result.Module = text(Object, "module");
  Result.ContextKind = text(Object, "context_kind");
  Result.ContextName = text(Object, "context_name", true);
  Result.Name = text(Object, "name");
  Result.DeclarationKind =
      Object.getString("declaration_kind").value_or("function").str();
  if (Result.DeclarationKind != "function" &&
      Result.DeclarationKind != "initializer" &&
      Result.DeclarationKind != "getter" && Result.DeclarationKind != "setter")
    throw std::invalid_argument("unsupported Swift source declaration kind");
  if (Result.DeclarationKind == "initializer" &&
      (Result.Name != "init" ||
       (Result.ContextKind != "class" && Result.ContextKind != "struct")))
    throw std::invalid_argument("unsupported Swift source initializer context");
  if ((Result.ContextKind != "global" && Result.ContextKind != "class" &&
       Result.ContextKind != "struct") ||
      (Result.ContextKind == "global" ? !Result.ContextName.empty()
                                      : !identifier(Result.ContextName)))
    throw std::invalid_argument("unsupported Swift declaration context");
  auto Static = Object.getBoolean("is_static");
  auto Mutating = Object.getBoolean("is_mutating");
  auto MutatingKnown = Object.getBoolean("is_mutating_known");
  if (!Static || !Mutating || !MutatingKnown ||
      (Result.ContextKind == "global" && *Static))
    throw std::invalid_argument("missing or inconsistent Swift method flags");
  Result.IsStatic = *Static;
  Result.IsMutating = *Mutating;
  Result.IsMutatingKnown = *MutatingKnown;
  auto Parameters = Object.getArray("parameters");
  auto Labels = Object.getArray("labels");
  if (!Parameters || !Labels || Parameters->size() > 64 ||
      Labels->size() != Parameters->size())
    throw std::invalid_argument("invalid Swift parameter and label counts");
  for (size_t Index = 0; Index < Parameters->size(); ++Index) {
    auto Parameter = (*Parameters)[Index].getAsObject();
    auto Label = (*Labels)[Index].getAsString();
    if (!Parameter || !Label || !identifier(*Label))
      throw std::invalid_argument("invalid Swift parameter declaration");
    const auto Name = text(*Parameter, "name");
    if (Name != "arg" + std::to_string(Index))
      throw std::invalid_argument(
          "Swift parameter positions must remain explicit");
    auto Type = type(Parameter->getObject("type"));
    if (Type.TheKind == SwiftSourceType::Kind::Void)
      throw std::invalid_argument("Swift parameter has void type");
    Result.Parameters.push_back({Name, std::move(Type)});
    Result.Labels.push_back(Label->str());
  }
  const SwiftRecoveredType *ContextType = nullptr;
  for (const auto &Candidate : NativeTypes)
    if (Candidate.Module == Result.Module &&
        Candidate.Name == Result.ContextName &&
        Candidate.Kind == Result.ContextKind &&
        Candidate.Status == "recovered") {
      if (ContextType)
        throw std::invalid_argument("ambiguous Swift context storage metadata");
      ContextType = &Candidate;
    }
  if (ContextType) {
    Result.ContextLayoutKnown = true;
    Result.ContextFields = ContextType->Fields;
  }
  if (Result.ContextKind == "struct" &&
      Result.DeclarationKind == "initializer") {
    auto Declared = Object.getObject("return_type");
    if (Result.IsStatic ||
        (Object.getString("node_kind") != "Allocator" &&
         Object.getString("node_kind") != "Constructor") ||
        !Object.getBoolean("requires_storage_abi_proof").value_or(false) ||
        !Declared || Declared->getString("kind") != "nominal" ||
        Declared->getString("module") != Result.Module ||
        Declared->getString("name") != Result.ContextName ||
        Declared->getString("context_kind") != "struct" || !ContextType ||
        ContextType->Size != 8 || ContextType->Fields.size() != 1 ||
        ContextType->Fields[0].Offset != 0 ||
        !nativeType(ContextType->Fields[0].Type) ||
        nativeType(ContextType->Fields[0].Type)->Size != 8)
      throw std::invalid_argument(
          "Swift value initializer has no proven scalar storage return");
    Result.ReturnType = ContextType->Fields[0].Type;
    Result.ReturnsContextValue = true;
  } else {
    Result.ReturnType = type(Object.getObject("return_type"));
  }
  if (Result.DeclarationKind == "getter" ||
      Result.DeclarationKind == "setter") {
    const bool Setter = Result.DeclarationKind == "setter";
    if (Result.ContextKind == "global" || Result.IsStatic ||
        Object.getString("node_kind") != (Setter ? "Setter" : "Getter") ||
        (Setter ? (Result.Parameters.size() != 1 ||
                   Result.ReturnType.TheKind != SwiftSourceType::Kind::Void)
                : (!Result.Parameters.empty() ||
                   Result.ReturnType.TheKind == SwiftSourceType::Kind::Void)))
      throw std::invalid_argument(
          "inconsistent Swift scalar accessor signature");
  }
  return Result;
}

inline SourceFunctionTypeHint hint(const SwiftSourceSignature &Signature,
                                   Arch Architecture,
                                   bool AllowUnprovenStructSelf = false) {
  if (Architecture != Arch::AArch64 && Architecture != Arch::X64)
    throw std::invalid_argument("unsupported Swift source architecture");
  if (Signature.ContextKind == "struct" && !Signature.IsStatic &&
      !Signature.ReturnsContextValue && !AllowUnprovenStructSelf)
    throw std::invalid_argument(
        "Swift value-type self layout is not established");
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.Architecture = Architecture;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = nativeType(Signature.ReturnType);
  for (const auto &Parameter : Signature.Parameters)
    Hint.Parameters.push_back({Parameter.Name, nativeType(Parameter.Type), {}});
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic))
    throw std::invalid_argument(Diagnostic);
  if (Signature.ContextKind == "class")
    Hint.Parameters.push_back(
        {"swift_self",
         NdType::makePtr(NdType::makeVoid()),
         {SourceABICarrierKind::IntegerRegister,
          Architecture == Arch::AArch64 ? a64reg::X20 : reg::R13, 0, 8}});
  if (!validateSourceABI(Hint, Diagnostic))
    throw std::invalid_argument(Diagnostic);
  return Hint;
}

} // namespace neverd::sdk::swift_source
#endif
