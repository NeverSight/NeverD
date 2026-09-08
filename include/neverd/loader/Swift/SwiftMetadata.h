//===- SwiftMetadata.h - Proven Swift stored-property layouts -----*- C++
//-*-===//
#ifndef NEVERD_LOADER_SWIFT_SWIFTMETADATA_H
#define NEVERD_LOADER_SWIFT_SWIFTMETADATA_H
#include "neverd/loader/Swift/SwiftMethods.h"

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
} // namespace neverd
#endif
