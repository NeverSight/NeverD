//===- DriverScenarioTests.cpp - Bounded scenario parser contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validates scenario input before driver loading or execution.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

namespace neverd::emulation {
namespace {

TEST(DriverScenario, ParsesOrderedLifecycleWithoutChangingExecutionBudgets) {
  DriverOptions Base;
  Base.InstructionLimit = 37;
  Base.ServiceName = "ScenarioTest";
  auto Result = driverOptionsFromScenarioJSON(R"({
    "load_address":"0x190000000", "unload":true,
    "requests":[
      {"kind":"create","device":"\\Device\\Demo"},
      {"kind":"ioctl","code":"0x222000","input":"00abFF","output_size":8},
      {"kind":"cleanup"}, {"kind":"close"}
    ]})",
                                              Base);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->InstructionLimit, 37u);
  EXPECT_EQ(Result->ServiceName, "ScenarioTest");
  EXPECT_EQ(Result->LoadAddress, 0x190000000ULL);
  EXPECT_TRUE(Result->Unload);
  ASSERT_EQ(Result->Requests.size(), 4u);
  EXPECT_EQ(Result->Requests[0].Kind, DriverRequestKind::Create);
  EXPECT_EQ(Result->Requests[0].Device, "\\Device\\Demo");
  EXPECT_EQ(Result->Requests[1].Kind, DriverRequestKind::DeviceControl);
  EXPECT_EQ(Result->Requests[1].ControlCode, 0x222000u);
  EXPECT_EQ(Result->Requests[1].Input, (std::vector<uint8_t>{0, 0xab, 0xff}));
  EXPECT_EQ(Result->Requests[1].OutputSize, 8u);
  EXPECT_EQ(Result->Requests[2].Kind, DriverRequestKind::Cleanup);
  EXPECT_EQ(Result->Requests[3].Kind, DriverRequestKind::Close);
  EXPECT_TRUE(Base.Requests.empty());
}

TEST(DriverScenario, RejectsUnknownFieldsTypesAndRequestKinds) {
  for (const char *Text :
       {"[]", "null", R"({"unknown":true})", R"({"unload":1})",
        R"({"requests":null})", R"({"requests":[null]})",
        R"({"requests":[{"kind":"read"}]})",
        R"({"requests":[{"kind":"create","typo":0}]})",
        R"({"requests":[{"kind":"close","input":""}]})",
        R"({"requests":[{"kind":"create","device":""}]})",
        R"({"requests":[{"kind":"create","device":"\u0000"}]})"}) {
    SCOPED_TRACE(Text);
    auto Result = driverOptionsFromScenarioJSON(Text);
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_FALSE(llvm::toString(Result.takeError()).empty());
  }
}

TEST(DriverScenario, RejectsInexactOrOutOfRangeCodesAndBufferSizes) {
  for (const char *Fields :
       {R"("code":-1)", R"("code":4294967296)", R"("code":1.0)",
        R"("code":"222000")", R"("code":"0x100000000")",
        R"("code":0,"output_size":-1)", R"("code":0,"output_size":1.0)",
        R"("code":0,"output_size":65537)", R"("code":0,"input":"0")",
        R"("code":0,"input":"0xff")", R"("code":0,"input":"zz")"}) {
    SCOPED_TRACE(Fields);
    auto Result = driverOptionsFromScenarioJSON(
        std::string(R"({"requests":[{"kind":"ioctl",)") + Fields + "}]}");
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_FALSE(llvm::toString(Result.takeError()).empty());
  }
}

TEST(DriverScenario, RejectsDuplicateKeysIncludingEscapedAliases) {
  for (const char *Text : {R"({"unload":true,"unload":false})",
                           R"({"requests":[{"kind":"create","kind":"close"}]})",
                           R"({"unload":true,"unlo\u0061d":false})"}) {
    auto Result = driverOptionsFromScenarioJSON(Text);
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("duplicate field"),
              std::string::npos);
  }
}

TEST(DriverScenario, RejectsInvalidLoadAddressWithoutTruncation) {
  for (const char *Address : {"0", "null", R"("1234")", R"("0x")",
                              R"("0x10000000000000000")", R"("0x-1")"}) {
    auto Result = driverOptionsFromScenarioJSON(
        std::string("{\"load_address\":") + Address + "}");
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_FALSE(llvm::toString(Result.takeError()).empty());
  }
}

TEST(DriverScenario, EnforcesRequestCountAndAggregateBufferBudget) {
  for (bool TooManyRequests : {false, true}) {
    std::string JSON = "{\"requests\":[";
    const unsigned Count = TooManyRequests ? 65 : 9;
    for (unsigned I = 0; I < Count; ++I) {
      if (I)
        JSON += ',';
      JSON += TooManyRequests
                  ? R"({"kind":"create"})"
                  : R"({"kind":"ioctl","code":0,"output_size":65536})";
    }
    JSON += "]}";
    auto Result = driverOptionsFromScenarioJSON(JSON);
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_FALSE(llvm::toString(Result.takeError()).empty());
  }
}

TEST(DriverScenario, EnforcesIndividualInputAndTextBudgets) {
  for (bool TextBudget : {false, true}) {
    const std::string JSON =
        TextBudget ? std::string(DriverScenarioJSONLimit + 1, ' ')
                   : std::string(
                         R"({"requests":[{"kind":"ioctl","code":0,"input":")") +
                         std::string((DriverScenarioBufferLimit + 1) * 2, 'a') +
                         "\"}]}";
    auto Result = driverOptionsFromScenarioJSON(JSON);
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_FALSE(llvm::toString(Result.takeError()).empty());
  }
}

} // namespace
} // namespace neverd::emulation
