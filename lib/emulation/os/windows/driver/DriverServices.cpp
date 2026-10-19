///
/// \file
/// DriverServices.
///

#include "neverd/emulation/DriverServices.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/StringExtras.h"

#include <set>
namespace neverd::emulation {
llvm::Error validateDriverServices(const DriverOptions &O) {
  if (!O.ServiceImages)
    return llvm::Error::success();
  auto Bad = [](const llvm::Twine &T) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "service_images: " + T);
  };
  if (!O.Registry || !O.Version || O.Version->Major != 10 || O.Version->Minor ||
      O.Version->Build != 19041)
    return Bad("requires explicit registry and windows_version 10.0.19041");
  if (O.ServiceImages->size() > 16)
    return Bad("exceeds 16 explicit images");
  std::set<std::string> Names, Paths;
  for (const auto &S : *O.ServiceImages) {
    if (S.ServiceName.empty() || S.ServiceName.size() > 128 ||
        !llvm::all_of(S.ServiceName,
                      [](unsigned char C) {
                        return llvm::isAlnum(C) || C == '_' || C == '-' ||
                               C == '.';
                      }) ||
        llvm::StringRef(S.ServiceName).equals_insensitive(O.ServiceName) ||
        !Names.insert(llvm::StringRef(S.ServiceName).lower()).second)
      return Bad("requires distinct non-primary ASCII service names");
    if (S.ImagePath.empty() || S.ImagePath[0] != '\\' ||
        S.ImagePath.size() > 1024 ||
        !llvm::all_of(
            S.ImagePath,
            [](unsigned char C) { return C >= 32 && C < 127 && C != '%'; }) ||
        !Paths.insert(llvm::StringRef(S.ImagePath).lower()).second)
      return Bad("requires distinct absolute literal guest image paths");
    if (S.Source.empty() || !S.LoadAddress || (S.LoadAddress & 4095) ||
        (S.LoadAddress > 0x7fffffffffffULL &&
         S.LoadAddress < 0xffff800000000000ULL))
      return Bad("requires source and an explicit canonical page-aligned "
                 "load_address");
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
