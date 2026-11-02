//===- ExportTableTests.cpp - Complete CLI export text -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Export-table spelling, full-width ordinals and unbounded SDK table size.
///
//===----------------------------------------------------------------------===//

#include "ExportTable.h"

#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>
#include <string>
#include <utility>

namespace {

const std::string Columns =
    "  Name                                     Ordinal  Addr\n"
    "  ------------------------------------------------------------\n";

TEST(ExportTable, Empty) {
  std::string Text;
  llvm::raw_string_ostream Output(Text);
  auto Err = neverd::cli::printExportTable("[]", Output);
  ASSERT_FALSE(Err) << llvm::toString(std::move(Err));
  EXPECT_EQ(Text, "\nExports (0):\n" + Columns);
}

TEST(ExportTable, RowsWidthsEscapesAndDuplicateOrder) {
  constexpr char JSON[] = R"([
    {"name":"a\"\\\t\n\r\u0008\u000c","ordinal":0,"addr":"0x0"},
    {"name":"\u4e2d\ud83d\ude80\ufffd","ordinal":4294967295,
     "addr":"0xffffffffffffffff"},
    {"name":"prefix\u0000hidden","ordinal":7,"addr":"0x2"},
    {"name":"prefix\u0000hidden","ordinal":7,"addr":"0x2"}
  ])";
  std::string Text;
  llvm::raw_string_ostream Output(Text);
  auto Err = neverd::cli::printExportTable(JSON, Output);
  ASSERT_FALSE(Err) << llvm::toString(std::move(Err));
  EXPECT_EQ(Text, "\nExports (4):\n" + Columns + "  a\"\\\t\n\r\b\f" +
                      std::string(32, ' ') + " 0        0x0\n  " +
                      "\xe4\xb8\xad\xf0\x9f\x9a\x80\xef\xbf\xbd" +
                      std::string(30, ' ') +
                      " 4294967295 0xffffffffffffffff\n  prefix" +
                      std::string(34, ' ') + " 7        0x2\n  prefix" +
                      std::string(34, ' ') + " 7        0x2\n");
}

TEST(ExportTable, CompleteTableLargerThanTransportFrame) {
  const std::string Name(8 * 1024 * 1024 + 1, 'x');
  const std::string JSON =
      "[{\"addr\":\"0x0\",\"name\":\"" + Name + "\",\"ordinal\":0}]";
  std::string Text;
  llvm::raw_string_ostream Output(Text);
  auto Err = neverd::cli::printExportTable(JSON, Output);
  ASSERT_FALSE(Err) << llvm::toString(std::move(Err));
  EXPECT_EQ(Text,
            "\nExports (1):\n" + Columns + "  " + Name + " 0        0x0\n");
}

TEST(ExportTable, RejectsMalformedResponsesBeforePrinting) {
  for (const auto *JSON :
       {"", "[", "{}", "[null]",
        R"([{"name":"x","ordinal":0,"addr":"0x0"},{}])",
        R"([{"name":3,"ordinal":0,"addr":"0x0"}])",
        R"([{"name":"x","ordinal":-1,"addr":"0x0"}])",
        R"([{"name":"x","ordinal":4294967296,"addr":"0x0"}])",
        R"([{"name":"x","ordinal":0.5,"addr":"0x0"}])",
        R"([{"name":"x","ordinal":0,"addr":3}])",
        R"([{"name":"x","name":"y","ordinal":0,"addr":"0x0"}])",
        "[{\"name\":\"\xff\",\"ordinal\":0,\"addr\":\"0x0\"}]"}) {
    std::string Text;
    llvm::raw_string_ostream Output(Text);
    auto Err = neverd::cli::printExportTable(JSON, Output);
    EXPECT_TRUE(bool(Err)) << JSON;
    llvm::consumeError(std::move(Err));
    EXPECT_TRUE(Text.empty()) << JSON;
  }
}

} // namespace
