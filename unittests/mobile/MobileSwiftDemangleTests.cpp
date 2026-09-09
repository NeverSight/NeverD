//===- MobileSwiftDemangleTests.cpp - Builtin Swift signature recovery ----===//
#include "MobileIOSInternal.h"
#include "gtest/gtest.h"

using namespace neverd::mobile;
using namespace neverd::mobile::ios;

TEST(MobileSwiftDemangle, MatchesIndependentCompilerFixtureSignatures) {
  auto path = pathFromUTF8(NEVERD_MOBILE_FIXTURES) / "swift-signatures.json";
  auto fixture = parseJSON(readFile(path, 1024 * 1024), "Swift oracle");
  const auto &cases = array(object(fixture, "Swift oracle"), "cases");
  ASSERT_EQ(cases.size(), 49u);
  for (const auto &v : cases) {
    const auto &expected = object(v, "Swift oracle signature");
    auto symbol = requiredString(expected, "mangled_symbol");
    SCOPED_TRACE(symbol);
    auto actual = swiftSignature(requiredString(expected, "entry"), symbol);
    EXPECT_EQ(jsonText(Value(std::move(actual))), jsonText(v));
  }
}

TEST(MobileSwiftDemangle, InventoryUsesBuiltinAndPreservesAliasIdentities) {
  Array symbols{
      Object{{"name", "$s4Demo6answers5Int32VyF"}, {"address", "0x1000"}},
      Object{{"name", "_$s4Demo6answers5Int32VyF"}, {"address", "0x1000"}},
      Object{{"name", "$s4Demo3BoxCMn"}, {"address", "0x2000"}},
      Object{{"name", "$s4Demo6answers5Int32Vy"}, {"address", "0x3000"}}};
  Budget budget;
  auto inventory = swiftSignatures(symbols, 8, budget);
  EXPECT_EQ(number(inventory, "symbol_count"), 4);
  EXPECT_EQ(number(inventory, "method_count"), 2);
  EXPECT_EQ(number(inventory, "supported_signature_count"), 2);
  EXPECT_EQ(number(inventory, "unclassified_symbol_count"), 1);
  const auto &methods = array(inventory, "methods");
  ASSERT_EQ(methods.size(), 2u);
  EXPECT_EQ(str(object(methods[0], "method"), "entry"), "0x1000");
  EXPECT_EQ(str(object(methods[1], "method"), "entry"), "0x1000");
  EXPECT_NE(str(object(methods[0], "method"), "mangled_symbol"),
            str(object(methods[1], "method"), "mangled_symbol"));
  auto *demangler = inventory.getObject("demangler");
  ASSERT_NE(demangler, nullptr);
  EXPECT_EQ(str(*demangler, "execution"), "builtin");
  EXPECT_EQ(str(*demangler, "name"), "llvm-swift-demangle");
  EXPECT_EQ(str(*demangler, "version"), "6.3.3");
  EXPECT_FALSE(inventory.get("logs"));
}

TEST(MobileSwiftDemangle, KeepsUnsupportedLanguageFormsInCallableCoverage) {
  // Independently compiled declarations in module SignatureKinds: generic<T>,
  // asynchronous(Int32), throwing(Int32), and unicode_lambda(Int32).
  for (auto symbol : {"_$s14SignatureKinds7genericyxxlF",
                      "_$s14SignatureKinds12asynchronousys5Int32VADYaF",
                      "_$s14SignatureKinds8throwingys5Int32VADKF",
                      "_$s14SignatureKinds0012unicode__Fcgys5Int32VADF"}) {
    SCOPED_TRACE(symbol);
    auto signature = swiftSignature("0x1000", symbol);
    EXPECT_EQ(str(signature, "classification"), "callable");
    EXPECT_EQ(str(signature, "node_kind"), "Function");
    EXPECT_EQ(str(signature, "status"), "unsupported");
    EXPECT_FALSE(str(signature, "reason").empty());
  }
}

TEST(MobileSwiftDemangle, EmptyArgumentTupleDoesNotRequireLabelList) {
  for (auto symbol : {"$s4Demo6answers5Int32VyF", "$s4Demo3BoxCACycfc"}) {
    SCOPED_TRACE(symbol);
    auto signature = swiftSignature("0x1000", symbol);
    EXPECT_EQ(str(signature, "status"), "supported")
        << str(signature, "reason");
    EXPECT_TRUE(array(signature, "labels").empty());
    EXPECT_TRUE(array(signature, "parameters").empty());
  }
}

TEST(MobileSwiftDemangle, RejectsBadInputsWithoutLosingFollowingSymbols) {
  std::string nul = "$s4Demo6answers5Int32VyF";
  nul.push_back('\0');
  Array symbols{
      Object{{"name", "$s" + std::string(8000, 'a')}, {"address", "0x1000"}},
      Object{{"name", nul}, {"address", "0x1001"}},
      Object{{"name", "$s4Demo6answers5Int32VyF\n"}, {"address", "0x1002"}},
      Object{{"name", "$s4Demo6answers5Int32VyF"}, {"address", "0x1003"}}};
  Budget budget;
  auto inventory = swiftSignatures(symbols, 8, budget);
  EXPECT_EQ(number(inventory, "symbol_count"), 4);
  EXPECT_EQ(number(inventory, "supported_signature_count"), 1);
  EXPECT_EQ(number(inventory, "unclassified_symbol_count"), 3);
  EXPECT_EQ(str(object(array(inventory, "methods")[0], "method"), "entry"),
            "0x1003");
  EXPECT_EQ(str(swiftSignature("not-an-address", "$s4Demo6answers5Int32VyF"),
                "classification"),
            "unknown");
  EXPECT_EQ(str(swiftSignature("0x1000", "$s4Demo6answers5Int32VyF", 4),
                "classification"),
            "unknown");
}

TEST(MobileSwiftDemangle, AppliesInventoryByteFileAndDeadlineBudgets) {
  Array symbols{
      Object{{"name", "$s4Demo6answers5Int32VyF"}, {"address", "0x1000"}},
      Object{{"name", "$s4Demo6answers5Int32VyF"}, {"address", "0x1001"}}};
  Budget bytes;
  bytes.limits.max_bytes = 30;
  EXPECT_THROW(swiftSignatures(symbols, 8, bytes), Error);
  Budget files;
  files.limits.max_files = 1;
  EXPECT_THROW(swiftSignatures(symbols, 8, files), Error);
  Budget expired;
  expired.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  EXPECT_THROW(swiftSignatures(symbols, 8, expired), Error);
}
