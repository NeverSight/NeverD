//===- ExportTable.cpp - CLI export table formatting --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reuse compact tool JSON storage for complete human-readable export tables.
///
//===----------------------------------------------------------------------===//

#include "ExportTable.h"

#include "common/transport/Json.h"

#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace neverd::cli {

llvm::Error printExportTable(llvm::StringRef Text, llvm::raw_ostream &Output) {
  try {
    // This is the SDK's shallow array of export records, not an incoming
    // transport frame. Preserve complete tables larger than the frame budget.
    const auto Rows = transport::parseJson(
        std::string_view(Text.data(), Text.size()), Text.size());
    if (!Rows.is_array())
      return llvm::createStringError("Expected an SDK export array");
    for (const auto &Row : Rows) {
      if (!Row.is_object() || !Row.contains("name") ||
          !Row["name"].is_string() || !Row.contains("addr") ||
          !Row["addr"].is_string() || !Row.contains("ordinal") ||
          !Row["ordinal"].is_number_unsigned() ||
          Row["ordinal"].get<uint64_t>() > std::numeric_limits<uint32_t>::max())
        return llvm::createStringError("Invalid SDK export record");
    }
    Output << "\nExports (" << Rows.size() << "):\n";
    Output << llvm::format("  %-40s %-8s %s\n", "Name", "Ordinal", "Addr");
    Output << "  " << std::string(60, '-') << "\n";
    for (const auto &Row : Rows)
      Output << llvm::format(
          "  %-40s %-8lld %s\n",
          Row["name"].get_ref<const std::string &>().c_str(),
          static_cast<long long>(Row["ordinal"].get<uint32_t>()),
          Row["addr"].get_ref<const std::string &>().c_str());
    return llvm::Error::success();
  } catch (const transport::Error &Err) {
    return llvm::createStringError("Invalid SDK export JSON: %s", Err.what());
  }
}

} // namespace neverd::cli
