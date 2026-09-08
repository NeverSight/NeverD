#ifndef NEVERD_LOADER_OBJC_OBJCMETHODS_H
#define NEVERD_LOADER_OBJC_OBJCMETHODS_H

#include "neverd/ir/SourceTypeHint.h"

#include <optional>

namespace neverd {
struct BinaryImage;

struct ObjCClass {
  std::string Name;
  va_t Address = 0;
  va_t SuperclassAddress = 0;
  std::string SuperclassName;
  bool RootClass = false;
  std::string InheritanceStatus = "unresolved";
};

struct ObjCMethod {
  va_t Implementation = 0;
  va_t MetadataAddress = 0;
  va_t ClassAddress = 0;
  std::string ClassName;
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

} // namespace neverd
#endif
