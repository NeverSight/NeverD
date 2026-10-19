//===- DriverProcessMemoryJSON.cpp - Strict process RAM observations ------===//
// NeverD Decompiler
///
/// \file
/// Strict process RAM observations.
///

#include "DriverProcessMemoryJSON.h"

#include "llvm/ADT/StringExtras.h"
namespace neverd::emulation {
llvm::Expected<std::vector<DriverProcessImageMapping>>
parseDriverProcessImages(const llvm::json::Value &V) {
  auto Invalid = [] {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "process images require bounded records with mapped_file, device_name, "
        "file_bytes_hex and optional base");
  };
  const auto *Array = V.getAsArray();
  if (!Array || Array->size() > MaxDriverProcessImages)
    return Invalid();
  std::vector<DriverProcessImageMapping> Out;
  for (const auto &Value : *Array) {
    const auto *O = Value.getAsObject();
    if (!O)
      return Invalid();
    DriverProcessImageMapping Image;
    const auto Name = O->getString("mapped_file"),
               Hex = O->getString("file_bytes_hex"),
               Device = O->getString("device_name");
    std::string Bytes;
    if (!Name || !Hex || !Device ||
        Hex->size() > MaxDriverProcessImageFileBytes * 2 ||
        !llvm::tryGetFromHex(*Hex, Bytes))
      return Invalid();
    Image.MappedFile = Name->str();
    Image.DeviceName = Device->str();
    Image.FileBytes.assign(Bytes.begin(), Bytes.end());
    for (const auto &[Key, Field] : *O) {
      if (Key == "mapped_file" || Key == "file_bytes_hex" ||
          Key == "device_name")
        continue;
      if (Key != "base" || !Field.getAsUINT64())
        return Invalid();
      Image.Base = *Field.getAsUINT64();
    }
    Out.push_back(std::move(Image));
  }
  return Out;
}
llvm::json::Value
driverProcessImagesJSON(const std::vector<DriverProcessImageMapping> &Images) {
  llvm::json::Array Out;
  for (const auto &Image : Images)
    Out.push_back(llvm::json::Object{
        {"base", Image.Base},
        {"mapped_file", Image.MappedFile},
        {"device_name", Image.DeviceName},
        {"file_bytes_hex",
         llvm::toHex(llvm::ArrayRef(Image.FileBytes), true)}});
  return Out;
}
llvm::Expected<std::vector<DriverProcessMemoryRegion>>
parseDriverProcessMemory(const llvm::json::Value &V) {
  auto Invalid = [] {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "process memory requires bounded region records with base, size, "
        "optional bytes_hex and access");
  };
  const auto *A = V.getAsArray();
  if (!A || A->size() > MaxDriverProcessMemoryRegions)
    return Invalid();
  std::vector<DriverProcessMemoryRegion> Regions;
  for (const auto &Entry : *A) {
    const auto *O = Entry.getAsObject();
    if (!O)
      return Invalid();
    auto Base = O->get("base"), Size = O->get("size");
    if (!Base || !Size || !Base->getAsUINT64() || !Size->getAsUINT64() ||
        *Size->getAsUINT64() > UINT32_MAX)
      return Invalid();
    DriverProcessMemoryRegion R;
    R.Base = *Base->getAsUINT64();
    R.Size = uint32_t(*Size->getAsUINT64());
    for (const auto &[Key, Value] : *O) {
      if (Key == "base" || Key == "size")
        continue;
      if (Key == "bytes_hex") {
        auto Text = Value.getAsString();
        std::string Bytes;
        if (!Text || Text->size() > 2 * 1024 * 1024 ||
            !llvm::tryGetFromHex(*Text, Bytes))
          return Invalid();
        R.Bytes.assign(Bytes.begin(), Bytes.end());
      } else if (Key == "access") {
        auto Text = Value.getAsString();
        if (!Text)
          return Invalid();
        bool Found = false;
#define NEVERD_DRIVER_USER_PAGE_ACCESS(Name, Spelling)                         \
  if (*Text == Spelling) {                                                     \
    R.Access = DriverUserPageAccess::Name;                                     \
    Found = true;                                                              \
  }
#include "neverd/emulation/DriverUserPageAccess.def"
#undef NEVERD_DRIVER_USER_PAGE_ACCESS
        if (!Found)
          return Invalid();
      } else
        return Invalid();
    }
    Regions.push_back(std::move(R));
  }
  return Regions;
}
llvm::json::Value
driverProcessMemoryJSON(const std::vector<DriverProcessMemoryRegion> &Regions) {
  llvm::json::Array Out;
  for (const auto &R : Regions) {
    const char *Access = nullptr;
    switch (R.Access) {
#define NEVERD_DRIVER_USER_PAGE_ACCESS(Name, Spelling)                         \
  case DriverUserPageAccess::Name:                                             \
    Access = Spelling;                                                         \
    break;
#include "neverd/emulation/DriverUserPageAccess.def"
#undef NEVERD_DRIVER_USER_PAGE_ACCESS
    }
    Out.push_back(llvm::json::Object{{"base", R.Base},
                                     {"size", R.Size},
                                     {"bytes_hex", llvm::toHex(R.Bytes)},
                                     {"access", Access ? Access : "unknown"}});
  }
  return Out;
}
} // namespace neverd::emulation
