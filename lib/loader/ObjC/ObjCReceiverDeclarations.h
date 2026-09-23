#ifndef NEVERD_LOADER_OBJC_RECEIVER_DECLARATIONS_H
#define NEVERD_LOADER_OBJC_RECEIVER_DECLARATIONS_H

#include "neverd/ir/SourceTypeHint.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <optional>

namespace neverd {
struct BinaryImage;

namespace objc {
struct ReceiverMemberDeclaration {
  std::optional<SourceFunctionTypeHint> Signature;
  std::string ReturnClass;
  bool ReturnsReceiverType = false;
  /// Explicit Objective-C argument index (including self and _cmd) to the
  /// compiler-declared object class stored through an object-pointer slot.
  std::map<unsigned, std::string> OutParameterClasses;
};

/// Facts for one declared class or protocol, including its categories.
/// An empty superclass denotes a declared root; no value means that no class
/// interface contributed hierarchy evidence. Unsupported members stay present.
struct ReceiverDeclarations {
  bool Present = false;
  bool Complete = true;
  std::optional<std::string> Superclass;
  std::vector<std::string> Protocols;
  std::vector<ReceiverMemberDeclaration> Members;
};

/// Match a concrete class owner to the exact SDK framework install name.
/// Categories and merely active frameworks do not establish export ownership.
bool sdkClassImportProvider(Arch Architecture, llvm::StringRef Class,
                            llvm::StringRef Module);

ReceiverDeclarations sdkReceiverDeclarations(const BinaryImage &Image,
                                             llvm::StringRef Name,
                                             bool Protocol, bool ClassMethod,
                                             llvm::StringRef Selector);
std::vector<std::string> sdkReceiverSubclasses(const BinaryImage &Image,
                                               llvm::StringRef Name);

/// Return one selector-wide compiler-declared object-pointer pointee class.
/// Every active SDK declaration of the selector must agree at this parameter.
std::optional<std::string>
sdkSelectorOutParameterClass(const BinaryImage &Image, llvm::StringRef Selector,
                             unsigned Parameter);
} // namespace objc
} // namespace neverd
#endif
