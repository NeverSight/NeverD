//===- SwiftMetadata.h - Proven Swift stored-property layouts -----*- C++
//-*-===//
#ifndef NEVERD_LOADER_SWIFT_SWIFTMETADATA_H
#define NEVERD_LOADER_SWIFT_SWIFTMETADATA_H
#include "neverd/loader/Swift/SwiftMethods.h"

#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>
#include <vector>
namespace neverd {
struct BinaryImage;
struct SwiftRecoveredType {
  std::string Module;
  std::string Name;
  std::string Kind;
  va_t Descriptor = 0;
  va_t Metadata = 0;
  uint64_t Size = 0;
  uint64_t Alignment = 0;
  std::vector<SwiftStorageField> Fields;
  std::string Status = "unrecovered";
  std::string Reason;
};
/// Read only bounded file-backed descriptors and individually resolved slots.
/// Generic/resilient or incomplete layouts remain explicit unrecovered rows.
std::vector<SwiftRecoveredType> recoverSwiftTypes(const BinaryImage &Image);

/// Return the exact byte width encoded by a bounded Swift static-property
/// storage symbol when its type is a standard-library scalar. Accessor and
/// unrelated manglings are rejected rather than classified by suffix text.
std::optional<uint64_t>
swiftStaticScalarStorageWidth(llvm::StringRef MangledSymbol);

/// Exact width of a file-private top-level Swift standard-library scalar.
/// The private declaration identity and scalar type must both be present in
/// the structured mangling; unrelated globals are not inferred from a suffix.
std::optional<uint64_t>
swiftPrivateScalarStorageWidth(llvm::StringRef MangledSymbol);
} // namespace neverd
#endif
