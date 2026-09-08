#ifndef NEVERD_SDK_CAPI_SWIFTSOURCEPROPERTYMODEL_H
#define NEVERD_SDK_CAPI_SWIFTSOURCEPROPERTYMODEL_H

#include "neverd/loader/Swift/SwiftMethods.h"

namespace neverd::sdk::swift_source {
inline std::string typeSpelling(const SwiftSourceType &Type) {
  return "Swift." + Type.Name +
         (Type.Pointee ? "<" + typeSpelling(*Type.Pointee) + ">" : "");
}
inline bool isAccessor(const SwiftSourceSignature &Signature) {
  return Signature.DeclarationKind == "getter" ||
         Signature.DeclarationKind == "setter";
}
inline const SwiftSourceType *
propertyType(const SwiftSourceSignature &Signature) {
  if (Signature.ContextKind != "class" && Signature.ContextKind != "struct")
    return nullptr;
  if (Signature.DeclarationKind == "getter" && Signature.Parameters.empty() &&
      Signature.ReturnType.TheKind != SwiftSourceType::Kind::Void)
    return &Signature.ReturnType;
  if (Signature.DeclarationKind == "setter" &&
      Signature.Parameters.size() == 1 &&
      Signature.ReturnType.TheKind == SwiftSourceType::Kind::Void &&
      Signature.Parameters[0].Type.TheKind != SwiftSourceType::Kind::Void)
    return &Signature.Parameters[0].Type;
  return nullptr;
}
} // namespace neverd::sdk::swift_source

#endif
