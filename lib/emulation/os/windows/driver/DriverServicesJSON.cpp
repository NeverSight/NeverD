///
/// \file
/// DriverServicesJSON.
///

#include "DriverServicesJSON.h"
namespace neverd::emulation {
llvm::Expected<std::vector<DriverServiceImage>>
parseDriverServiceImages(const llvm::json::Value &V) {
  auto Bad = []() {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "service_images requires an array of "
        "service_name/image_path/source/load_address objects");
  };
  const auto *A = V.getAsArray();
  if (!A || A->size() > 16)
    return Bad();
  std::vector<DriverServiceImage> R;
  for (const auto &V : *A) {
    const auto *S = V.getAsObject();
    if (!S || S->size() != 4 || !S->getString("service_name") ||
        !S->getString("image_path") || !S->getString("source") ||
        !S->get("load_address"))
      return Bad();
    auto B = S->get("load_address")->getAsUINT64();
    if (!B)
      return Bad();
    R.push_back({S->getString("service_name")->str(),
                 S->getString("image_path")->str(),
                 std::filesystem::u8path(S->getString("source")->str()), *B});
  }
  return R;
}
llvm::json::Value
driverServiceImagesJSON(const std::vector<DriverServiceImage> &S) {
  llvm::json::Array A;
  for (const auto &I : S)
    A.push_back(llvm::json::Object{{"service_name", I.ServiceName},
                                   {"image_path", I.ImagePath},
                                   {"source", I.Source.generic_string()},
                                   {"load_address", I.LoadAddress}});
  return A;
}
} // namespace neverd::emulation
