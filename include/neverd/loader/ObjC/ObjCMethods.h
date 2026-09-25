#ifndef NEVERD_LOADER_OBJC_OBJCMETHODS_H
#define NEVERD_LOADER_OBJC_OBJCMETHODS_H

#include "neverd/ir/SourceTypeHint.h"

#include <optional>

namespace neverd {
struct BinaryImage;

struct ObjCIvar {
  std::string Name;
  std::string TypeEncoding;
  va_t MetadataAddress = 0;
  va_t OffsetAddress = 0;
  /// Missing when the runtime initializes the offset slot after loading.
  /// The metadata identity remains useful without a static instance layout.
  std::optional<uint32_t> Offset;
  /// Zero denotes a runtime-sized field, not an empty storage range.
  uint32_t Size = 0;
  uint32_t Alignment = 0;
};

struct ObjCClass {
  std::string Name;
  va_t Address = 0;
  va_t SuperclassAddress = 0;
  std::string SuperclassName;
  bool RootClass = false;
  std::string InheritanceStatus = "unresolved";
  uint32_t InstanceStart = 0;
  uint32_t InstanceSize = 0;
  std::string IvarStatus = "unresolved";
  std::vector<ObjCIvar> Ivars;
};

struct ObjCSourceReference {
  enum class Kind { Selector, Class, Metaclass, IvarOffset, Protocol };
  Kind TheKind = Kind::Selector;
  va_t Address = 0;
  uint16_t Size = 8;
  std::string Name;
  std::string ClassName;
};

struct ObjCMethod {
  va_t Implementation = 0;
  va_t MetadataAddress = 0;
  va_t ClassAddress = 0;
  va_t CategoryAddress = 0;
  std::string ClassName;
  std::string CategoryName;
  std::string Selector;
  std::string TypeEncoding;
  std::string Status;
  bool IsClassMethod = false;
  /// Validated declaration ABI, independent of runtime override order.
  /// Colliding declarations may retain this hint for their separate IMP
  /// bodies; it never selects a winner for Objective-C message dispatch.
  std::optional<SourceFunctionTypeHint> TypeHint;
  std::vector<std::string> Diagnostics;
};

inline bool objcMethodHasSourceBody(const ObjCMethod &Method) {
  return Method.TypeHint &&
         (Method.Status == "supported" ||
          Method.Status == "ambiguous_dispatch");
}

/// A protocol describes a call contract, never an executable implementation.
struct ObjCProtocolMethod {
  va_t MetadataAddress = 0;
  std::string Selector;
  std::string TypeEncoding;
  std::string Status;
  bool IsClassMethod = false;
  bool IsOptional = false;
  std::optional<SourceFunctionTypeHint> TypeHint;
};

struct ObjCProtocol {
  va_t Address = 0;
  std::string Name;
  std::string Status;
  std::vector<va_t> AdoptedProtocols;
  std::vector<ObjCProtocolMethod> Methods;
};

/// A compiler declaration of accessors, including properties whose methods
/// are supplied dynamically. This record never supplies an implementation.
struct ObjCProperty {
  enum class OwnerKind { Class, Category, Protocol };
  OwnerKind Owner = OwnerKind::Class;
  va_t OwnerAddress = 0;
  va_t MetadataAddress = 0;
  std::string OwnerName;
  std::string ClassName;
  std::string Name;
  std::string Attributes;
  std::string TypeEncoding;
  std::string Getter;
  std::string Setter;
  bool IsClassProperty = false;
  bool ReadOnly = false;
  bool IsOptional = false;
  std::string Status = "invalid_metadata";
  std::optional<SourceFunctionTypeHint> GetterTypeHint;
  std::optional<SourceFunctionTypeHint> SetterTypeHint;
};

/// Read bounded Objective-C runtime records from the loader's resolved image.
/// Unsupported/malformed metadata is diagnosed and never applied as a type
/// hint. Valid executable IMPs become function discovery seeds. The operation
/// is idempotent; no companion file supplies declarations or trust state.
void parseObjCMethods(BinaryImage &Img);

/// Recover storage and exact runtime reference slots after method metadata.
/// These identities are for source relocation, not rewrite safety evidence.
void parseObjCStorage(BinaryImage &Img);

} // namespace neverd
#endif
