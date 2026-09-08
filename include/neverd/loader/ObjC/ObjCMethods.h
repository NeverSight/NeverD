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
  uint32_t Offset = 0;
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
  enum class Kind { Selector, Class, Metaclass, IvarOffset };
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
  std::optional<SourceFunctionTypeHint> TypeHint;
  std::vector<std::string> Diagnostics;
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
