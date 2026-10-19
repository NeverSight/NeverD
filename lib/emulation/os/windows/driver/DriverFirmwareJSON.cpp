//===- DriverFirmwareJSON.cpp - Strict MCFG scenario transport
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strict MCFG scenario transport.
///

#include "DriverFirmwareJSON.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
constexpr llvm::StringLiteral PresentField = "present",
                              TableField = "table_hex";
llvm::Error invalid(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), "pci_mcfg: %s",
                                 Text);
}
} // namespace
llvm::Expected<DriverPciMcfg>
parseDriverPciMcfg(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid("expected an object");
  for (const auto &KV : *Object)
    if (llvm::StringRef(KV.first) != PresentField &&
        llvm::StringRef(KV.first) != TableField)
      return invalid("unknown field");
  auto Present = Object->getBoolean(PresentField);
  if (!Present)
    return invalid("present must be a boolean");
  DriverPciMcfg Facts;
  Facts.Present = *Present;
  if (Facts.Present) {
    auto Hex = Object->getString(TableField);
    if (!Hex || Hex->size() > firmware::McfgMaxBytes * 2)
      return invalid("present table requires bounded table_hex");
    std::string Bytes;
    if (!llvm::tryGetFromHex(*Hex, Bytes))
      return invalid("invalid table_hex");
    Facts.Table.assign(Bytes.begin(), Bytes.end());
  } else if (Object->get(TableField))
    return invalid("absent table cannot have table_hex");
  auto Count = validateDriverPciMcfg(Facts);
  if (!Count)
    return Count.takeError();
  return Facts;
}
llvm::json::Object driverPciMcfgJSON(const DriverPciMcfg &Facts) {
  llvm::json::Object Object{{PresentField, Facts.Present}};
  if (Facts.Present)
    Object[TableField] = llvm::toHex(Facts.Table, true);
  return Object;
}
llvm::Expected<DriverSmbios> parseDriverSmbios(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || Object->size() != 1 || !Object->get("raw_hex"))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "smbios: expected only raw_hex");
  auto Hex = Object->getString("raw_hex");
  std::string Bytes;
  if (!Hex || Hex->size() > firmware::SmbiosMaxBytes * 2 ||
      !llvm::tryGetFromHex(*Hex, Bytes))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "smbios: invalid or oversized raw_hex");
  DriverSmbios Facts;
  Facts.Raw.assign(Bytes.begin(), Bytes.end());
  if (auto E = validateDriverSmbios(Facts))
    return std::move(E);
  return Facts;
}
llvm::json::Object driverSmbiosJSON(const DriverSmbios &Facts) {
  return llvm::json::Object{{"raw_hex", llvm::toHex(Facts.Raw, true)}};
}
llvm::Expected<DriverAcpiTables>
parseDriverAcpiTables(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  const auto *Tables = Object ? Object->getArray("tables_hex") : nullptr;
  if (!Object || Object->size() != 1 || !Tables ||
      Tables->size() > firmware::AcpiMaxTables)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "acpi_tables: expected bounded tables_hex array only");
  DriverAcpiTables Facts;
  size_t Total = 0;
  for (const auto &Table : *Tables) {
    auto Hex = Table.getAsString();
    std::string Bytes;
    if (!Hex || Hex->size() > (firmware::AcpiMaxBytes - Total) * 2 ||
        !llvm::tryGetFromHex(*Hex, Bytes))
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "acpi_tables: invalid or oversized table hex");
    Total += Bytes.size();
    Facts.Tables.emplace_back(Bytes.begin(), Bytes.end());
  }
  if (auto E = validateDriverAcpiTables(Facts))
    return std::move(E);
  return Facts;
}
llvm::json::Object driverAcpiTablesJSON(const DriverAcpiTables &Facts) {
  llvm::json::Array Tables;
  for (const auto &Table : Facts.Tables)
    Tables.push_back(llvm::toHex(Table, true));
  return llvm::json::Object{{"tables_hex", std::move(Tables)}};
}
} // namespace neverd::emulation
