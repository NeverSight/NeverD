///
/// \file
/// DriverNetworkInterfacesJSON.
///

#include "DriverNetworkInterfacesJSON.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
namespace neverd::emulation {
namespace {
constexpr const char *Names[] = {"normal", "raw", "normal_without_statistics"};
}
llvm::Expected<DriverNetworkInterfaces>
parseDriverNetworkInterfaces(const llvm::json::Value &V) {
  auto Error = [] {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "network_interfaces requires normal/raw/normal_without_statistics "
        "arrays of 1352-byte row hex");
  };
  const auto *O = V.getAsObject();
  if (!O || O->empty() || O->size() > 3)
    return Error();
  DriverNetworkInterfaces R;
  for (const auto &[Key, Value] : *O) {
    unsigned Level = 0;
    while (Level < 3 && Key != Names[Level])
      ++Level;
    const auto *Rows = Value.getAsArray();
    if (Level == 3 || !Rows || Rows->size() > DriverNetworkMaxRows)
      return Error();
    R.Levels[Level].emplace();
    for (const auto &Row : *Rows) {
      const auto Hex = Row.getAsString();
      if (!Hex || Hex->size() != DriverNetworkRowSize * 2 ||
          !llvm::all_of(*Hex, llvm::isHexDigit))
        return Error();
      const auto Bytes = llvm::fromHex(*Hex);
      R.Levels[Level]->emplace_back(Bytes.begin(), Bytes.end());
    }
  }
  return R;
}
llvm::json::Value
driverNetworkInterfacesJSON(const DriverNetworkInterfaces &V) {
  llvm::json::Object R;
  for (unsigned I = 0; I < 3; ++I)
    if (V.Levels[I]) {
      llvm::json::Array Rows;
      for (const auto &Row : *V.Levels[I])
        Rows.push_back(llvm::toHex(Row, true));
      R[Names[I]] = std::move(Rows);
    }
  return R;
}
} // namespace neverd::emulation
