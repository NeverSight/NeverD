#ifndef NEVERD_LOADER_OBJC_OBJCBLOCKS_H
#define NEVERD_LOADER_OBJC_OBJCBLOCKS_H

#include "neverd/ir/SourceTypeHint.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <optional>

namespace neverd {
struct BinaryImage;

/// The extended descriptor describes storage/ownership, not original field
/// names or scalar C types. NonObjectBytes may include several values/padding.
struct ObjCBlockCaptureRange {
  enum class Kind { NonObjectBytes, Strong, Byref, Weak, Unretained, Unknown };
  Kind StorageKind = Kind::Unknown;
  uint64_t Offset = 0;
  uint64_t Size = 0;
};

struct ObjCBlockDescriptor {
  va_t Address = 0;
  uint32_t Flags = 0;
  uint64_t LiteralSize = 0;
  va_t CopyHelper = 0;
  va_t DisposeHelper = 0;
  va_t SignatureAddress = 0;
  std::string Signature;
  /// Small values are inline ownership counts, not pointers.
  uint64_t LayoutValue = 0;
  std::vector<uint8_t> LayoutBytes;
  std::vector<ObjCBlockCaptureRange> Captures;
  std::optional<SourceFunctionTypeHint> InvokeTypeHint;
  std::vector<std::string> Limitations;
};

struct ObjCBlockLiteral {
  enum class Kind { Global, Stack };
  Kind StorageKind = Kind::Global;
  va_t Address = 0;
  va_t InvokeEntry = 0;
  ObjCBlockDescriptor Descriptor;
};

/// Parse the scalar invoke signature, including its hidden block-object
/// parameter. A method parameter encoded @? alone supplies no such signature.
std::optional<SourceFunctionTypeHint>
parseObjCBlockSignature(llvm::StringRef Signature, Arch Architecture,
                        std::string &Diagnostic);

/// Read a descriptor at an exact address. Stack literal construction must be
/// established independently before its flags/address may be passed here.
std::optional<ObjCBlockDescriptor>
readObjCBlockDescriptor(const BinaryImage &Image, va_t Address, uint32_t Flags,
                        std::string &Diagnostic);

/// Read a complete file-backed literal. Its own address is never substituted
/// with the value loaded from its isa field, even when that field is a bind.
std::optional<ObjCBlockLiteral> readObjCBlockLiteral(const BinaryImage &Image,
                                                     va_t Address,
                                                     std::string &Diagnostic);

/// Discover static globals from exact concrete-block import storage slots.
/// No raw address scan or symbol-name resemblance supplies identity.
std::vector<ObjCBlockLiteral>
findObjCGlobalBlocks(const BinaryImage &Image,
                     std::map<va_t, std::string> *RejectedCandidates = nullptr);
} // namespace neverd
#endif
