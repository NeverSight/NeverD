#ifndef NEVERD_SDK_CAPI_SWIFTRUNTIMESIGNATURES_H
#define NEVERD_SDK_CAPI_SWIFTRUNTIMESIGNATURES_H

#include "SwiftSourceSignatures.h"

#include "neverd/loader/Swift/SwiftRuntimeSource.h"

namespace neverd::sdk::swift_source {

/// This adapter requests an effect proof, not an ordinary void-return ABI.
/// The original declaration remains an initializer returning its nominal type.
inline std::optional<SwiftRuntimeSourceRequest> emptyValueInitializerRequest(
    const llvm::json::Object &Object, const BinaryImage &Image,
    const std::vector<SwiftRecoveredType> &NativeTypes) {
  if (Object.getString("declaration_kind") != "initializer" ||
      Object.getString("context_kind") != "struct" ||
      Object.getString("node_kind") != "Allocator" ||
      !Object.getBoolean("requires_storage_abi_proof").value_or(false))
    return std::nullopt;
  const SwiftRecoveredType *Context = nullptr;
  for (const auto &T : NativeTypes) {
    if (T.Module != Object.getString("module").value_or("") ||
        T.Name != Object.getString("context_name").value_or("") ||
        T.Kind != "struct")
      continue;
    if (Context)
      throw std::invalid_argument("ambiguous Swift empty initializer context");
    Context = &T;
  }
  if (!Context || Context->Status != "recovered" ||
      !Context->Reason.empty() || !Context->Descriptor || !Context->Metadata ||
      Context->Size != 0 || Context->Alignment != 1 || !Context->Fields.empty())
    return std::nullopt;
  const auto *Declared = Object.getObject("return_type");
  const auto *Parameters = Object.getArray("parameters");
  const auto *Labels = Object.getArray("labels");
  if (!Declared || Declared->getString("kind") != "nominal" ||
      Declared->getString("module") != Context->Module ||
      Declared->getString("name") != Context->Name ||
      Declared->getString("context_kind") != "struct" || !Parameters ||
      !Parameters->empty() || !Labels || !Labels->empty() ||
      Object.getString("name") != "init" ||
      Object.getBoolean("is_static") != false ||
      Object.getBoolean("is_mutating") != false ||
      Object.getBoolean("is_mutating_known") != true ||
      Object.get("runtime_source_kind") || Object.get("property_type"))
    throw std::invalid_argument("invalid Swift empty value initializer schema");
  auto Fields = Object;
  Fields["declaration_kind"] = "function";
  Fields["return_type"] =
      llvm::json::Object{{"kind", "void"}, {"name", "Void"}};
  SwiftRuntimeSourceRequest Request;
  Request.Kind = SwiftRuntimeSourceKind::EmptyValueInitializer;
  Request.Signature = signature(Fields, Image, NativeTypes);
  Request.Signature.DeclarationKind = "initializer";
  return Request;
}

inline bool sameRuntimePropertyType(const SwiftSourceType &A,
                                    const SwiftSourceType &B,
                                    unsigned Depth = 0) {
  return Depth <= 8 && A.TheKind == B.TheKind && A.Name == B.Name &&
         A.Bits == B.Bits && A.IsSigned == B.IsSigned &&
         bool(A.Pointee) == bool(B.Pointee) &&
         (!A.Pointee ||
          sameRuntimePropertyType(*A.Pointee, *B.Pointee, Depth + 1));
}

/// Read only a request for native evidence. Neither demangler classification
/// nor this schema validation establishes that a compiler body was recovered.
inline SwiftRuntimeSourceRequest
runtimeRequest(const llvm::json::Object &Object, const BinaryImage &Image,
               const std::vector<SwiftRecoveredType> &NativeTypes = {}) {
  if (Object.getString("declaration_kind") != "runtime" ||
      !Object.getBoolean("requires_runtime_source_proof").value_or(false))
    throw std::invalid_argument("missing Swift compiler source proof request");
  const auto Kind = Object.getString("runtime_source_kind").value_or("");
  const auto Node = Object.getString("node_kind").value_or("");
  SwiftRuntimeSourceRequest Request;
  bool Allocator = false, Property = false;
  if (Kind == "allocating_initializer" && Node == "Allocator") {
    Request.Kind = SwiftRuntimeSourceKind::AllocatingInitializer;
    Allocator = true;
  } else if (Kind == "destructor" && Node == "Destructor") {
    Request.Kind = SwiftRuntimeSourceKind::TrivialDestructor;
  } else if (Kind == "deallocator" && Node == "Deallocator") {
    Request.Kind = SwiftRuntimeSourceKind::DeallocatingDestructor;
  } else if (Kind == "type_metadata_accessor" &&
             Node == "TypeMetadataAccessFunction") {
    Request.Kind = SwiftRuntimeSourceKind::TypeMetadataAccessor;
  } else if (Kind == "modify_accessor" && Node == "ModifyAccessor") {
    Request.Kind = SwiftRuntimeSourceKind::ModifyAccessor;
    Property = true;
  } else if (Kind == "modify_resume" && Node == "CoroutineContinuation" &&
             Object.getString("continuation_of") == "ModifyAccessor") {
    auto Suffix = Object.getString("compiler_suffix").value_or("");
    if (!Suffix.consume_front(".resume.") || Suffix.empty() ||
        Suffix.size() > 10 ||
        !std::all_of(Suffix.begin(), Suffix.end(),
                     [](char C) { return C >= '0' && C <= '9'; }))
      throw std::invalid_argument("invalid Swift modify continuation identity");
    Request.Kind = SwiftRuntimeSourceKind::ModifyResume;
    Property = true;
  } else {
    throw std::invalid_argument("inconsistent Swift compiler source kind");
  }

  // Reuse the exact native symbol/context/type schema; the temporary kind is
  // only a parser adapter. No source hint or executable emission is produced.
  auto Fields = Object;
  Fields["declaration_kind"] = Allocator ? "initializer" : "function";
  Request.Signature = signature(Fields, Image, NativeTypes);
  auto &S = Request.Signature;
  S.DeclarationKind = "runtime";
  if (Request.Kind == SwiftRuntimeSourceKind::ModifyResume &&
      !llvm::StringRef(S.MangledSymbol)
           .ends_with(Object.getString("compiler_suffix").value_or("")))
    throw std::invalid_argument(
        "Swift continuation suffix disagrees with its native symbol");
  if (S.IsStatic || S.IsMutating || S.ContextKind == "global" ||
      ((Allocator ||
        Request.Kind == SwiftRuntimeSourceKind::TrivialDestructor ||
        Request.Kind == SwiftRuntimeSourceKind::DeallocatingDestructor) &&
       S.ContextKind != "class"))
    throw std::invalid_argument("unsupported Swift compiler source context");
  if (Allocator) {
    if (S.Name != "init" ||
        S.ReturnType.TheKind != SwiftSourceType::Kind::Pointer ||
        S.ReturnType.Name != "UnsafeMutableRawPointer" || S.ReturnType.Pointee)
      throw std::invalid_argument(
          "invalid Swift allocating initializer schema");
  } else {
    if (!S.Parameters.empty() || !S.Labels.empty() ||
        S.ReturnType.TheKind != SwiftSourceType::Kind::Void)
      throw std::invalid_argument(
          "compiler request cannot assert a native callable ABI");
    if (!Property &&
        S.Name != (Request.Kind == SwiftRuntimeSourceKind::TypeMetadataAccessor
                       ? "typeMetadata"
                       : "deinit"))
      throw std::invalid_argument("invalid Swift compiler declaration name");
  }
  if (Property) {
    const auto Declared = type(Object.getObject("property_type"));
    const SwiftStorageField *Found = nullptr;
    for (const auto &Field : S.ContextFields)
      if (Field.Name == S.Name) {
        if (Found)
          throw std::invalid_argument("ambiguous Swift yielded field identity");
        Found = &Field;
      }
    if (!S.ContextLayoutKnown || !Found || !Found->IsMutable ||
        !sameRuntimePropertyType(Declared, Found->Type))
      throw std::invalid_argument(
          "Swift yielded property lacks matching native mutable storage");
  } else if (Object.get("property_type")) {
    throw std::invalid_argument(
        "unexpected property type on Swift compiler request");
  }
  return Request;
}

} // namespace neverd::sdk::swift_source
#endif
