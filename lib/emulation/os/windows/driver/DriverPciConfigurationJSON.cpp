///
/// \file
/// DriverPciConfigurationJSON.
///

#include "DriverPciConfigurationJSON.h"

#include "llvm/ADT/StringExtras.h"
namespace neverd::emulation {
llvm::Expected<DriverPciConfiguration>
parseDriverPciConfiguration(const llvm::json::Value &V) {
  auto Invalid = [] {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "invalid pci_configuration object, bus "
                                   "window or bounded function records");
  };
  const auto *O = V.getAsObject();
  if (!O)
    return Invalid();
  for (const auto &[K, V] : *O)
    if (K != "minimum_bus_number" && K != "maximum_bus_number" &&
        K != "devices")
      return Invalid();
  auto Number = [](const llvm::json::Object &O, llvm::StringRef K,
                   uint64_t Max) -> std::optional<uint64_t> {
    auto *V = O.get(K);
    auto N = V ? V->getAsUINT64() : std::nullopt;
    return N && *N <= Max ? N : std::nullopt;
  };
  auto Min = Number(*O, "minimum_bus_number", 0xffffff),
       Max = Number(*O, "maximum_bus_number", 0xffffff);
  const auto *Devices = O->getArray("devices");
  if (!Min || !Max || !Devices || Devices->size() > MaxDriverPciFunctions)
    return Invalid();
  DriverPciConfiguration P;
  P.MinimumBusNumber = *Min;
  P.MaximumBusNumber = *Max;
  for (const auto &V : *Devices) {
    auto *D = V.getAsObject();
    if (!D)
      return Invalid();
    for (const auto &[K, V] : *D)
      if (K != "segment" && K != "bus" && K != "device" && K != "function" &&
          K != "config_hex" && K != "write_mask_hex" &&
          K != "write_one_to_clear_hex")
        return Invalid();
    auto Segment = Number(*D, "segment", 65535), Bus = Number(*D, "bus", 255),
         Device = Number(*D, "device", 31),
         Function = Number(*D, "function", 7);
    if (!Segment || !Bus || !Device || !Function)
      return Invalid();
    DriverPciFunction F;
    F.Segment = *Segment;
    F.Bus = *Bus;
    F.Device = *Device;
    F.Function = *Function;
    auto Bytes = [&](llvm::StringRef Name, std::vector<uint8_t> &Out) -> bool {
      auto Text = D->getString(Name);
      std::string Raw;
      if (!Text || Text->size() != 2 * DriverPciConfigurationBytes ||
          !llvm::tryGetFromHex(*Text, Raw))
        return false;
      Out.assign(Raw.begin(), Raw.end());
      return true;
    };
    if (!Bytes("config_hex", F.Configuration))
      return Invalid();
    if (D->get("write_mask_hex")) {
      F.WriteMask.emplace();
      if (!Bytes("write_mask_hex", *F.WriteMask))
        return Invalid();
    }
    if (D->get("write_one_to_clear_hex")) {
      F.WriteOneToClearMask.emplace();
      if (!Bytes("write_one_to_clear_hex", *F.WriteOneToClearMask))
        return Invalid();
    }
    P.Devices.push_back(std::move(F));
  }
  return P;
}
llvm::json::Value driverPciConfigurationJSON(const DriverPciConfiguration &P) {
  llvm::json::Array Devices;
  for (const auto &D : P.Devices) {
    llvm::json::Object Record{
        {"segment", D.Segment},
        {"bus", D.Bus},
        {"device", D.Device},
        {"function", D.Function},
        {"config_hex", llvm::toHex(D.Configuration, true)}};
    if (D.WriteMask)
      Record["write_mask_hex"] = llvm::toHex(*D.WriteMask, true);
    if (D.WriteOneToClearMask)
      Record["write_one_to_clear_hex"] =
          llvm::toHex(*D.WriteOneToClearMask, true);
    Devices.push_back(std::move(Record));
  }
  return llvm::json::Object{{"minimum_bus_number", P.MinimumBusNumber},
                            {"maximum_bus_number", P.MaximumBusNumber},
                            {"devices", std::move(Devices)}};
}
} // namespace neverd::emulation
