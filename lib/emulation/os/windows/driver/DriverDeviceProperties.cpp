//===- DriverDeviceProperties.cpp - Bounded PDO query data validation -----===//
///
/// \file
/// Bounded PDO query data validation.
///

#include "neverd/emulation/DriverDeviceProperties.h"

#include <set>

namespace neverd::emulation {
llvm::Error validateDriverDeviceProperties(
    const std::optional<std::vector<DriverDeviceProperty>> &Properties) {
  if (!Properties)
    return llvm::Error::success();
  auto Invalid = [](const llvm::Twine &Text) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "device_properties: " + Text);
  };
  if (Properties->size() > 18)
    return Invalid("exceeds the supported property count");
  enum class Kind { String, MultiString, Dword, Guid };
  std::set<uint32_t> Seen;
  size_t Total = 0;
  for (const auto &P : *Properties) {
    if (!Seen.insert(P.Property).second)
      return Invalid("duplicate property");
    std::optional<Kind> Type;
    switch (P.Property) {
#define NEVERD_DEVICE_PROPERTY(Name, Value, TypeName)                          \
  case Value:                                                                  \
    Type = Kind::TypeName;                                                     \
    break;
#include "DriverDeviceProperties.def"
#undef NEVERD_DEVICE_PROPERTY
    default:
      return Invalid(
          "unsupported property or independently owned resource/name");
    }
    if (!P.Bytes) {
      if (P.Property != 1 && P.Property != 2 && P.Property != 17)
        return Invalid("missing-value observations support HardwareID, "
                       "CompatibleIDs and UINumber only");
      continue;
    }
    const auto &B = *P.Bytes;
    if (B.size() > MaxDriverDevicePropertyBytes - Total)
      return Invalid("exceeds the per-device byte limit");
    Total += B.size();
    if (*Type == Kind::Dword && B.size() != 4)
      return Invalid("DWORD properties require four bytes");
    if (*Type == Kind::Guid && B.size() != 16)
      return Invalid("GUID properties require sixteen bytes");
    const size_t End = *Type == Kind::String        ? 2
                       : *Type == Kind::MultiString ? 4
                                                    : 0;
    if (End && (B.size() < End || B.size() % 2))
      return Invalid("UTF-16 property requires its complete terminator");
    for (size_t I = 0; I < End; ++I)
      if (B[B.size() - 1 - I])
        return Invalid("UTF-16 property is not terminated");
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
