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
        R"({"requests":[{"kind":"unknown"}]})",
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

TEST(DriverScenario, ParsesIndependentFilesDirectBuffersAndReadWriteOffsets) {
  auto Result = driverOptionsFromScenarioJSON(R"({
    "kernel_exports":{"ExAllocatePool2":true,"OptionalKernelRoutine":false},
    "requests":[
      {"kind":"create","file":7},
      {"kind":"write","file":7,"input":"1234","byte_offset":"0x100000000"},
      {"kind":"read","file":9,"output_size":2,"byte_offset":17},
      {"kind":"ioctl","code":"0x222001","input":"ab",
       "direct_input":"cdef","output_size":8,"file":7}
    ]})");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Requests.size(), 4u);
  EXPECT_EQ(Result->Requests[1].Kind, DriverRequestKind::Write);
  EXPECT_EQ(Result->Requests[1].File, 7u);
  EXPECT_EQ(Result->Requests[1].ByteOffset, 0x100000000ULL);
  EXPECT_EQ(Result->Requests[2].Kind, DriverRequestKind::Read);
  EXPECT_EQ(Result->Requests[2].File, 9u);
  EXPECT_EQ(Result->Requests[2].ByteOffset, 17u);
  EXPECT_EQ(Result->Requests[3].DirectInput,
            (std::vector<uint8_t>{0xcd, 0xef}));
  EXPECT_TRUE(Result->KernelExports.at("ExAllocatePool2"));
  EXPECT_FALSE(Result->KernelExports.at("OptionalKernelRoutine"));
}

TEST(DriverScenario, RejectsConflictingTransferParametersAndExportInventories) {
  for (
      const char *Text :
      {R"({"requests":[{"kind":"read","input":""}]})",
       R"({"requests":[{"kind":"write","output_size":0}]})",
       R"({"requests":[{"kind":"write","direct_input":""}]})",
       R"({"requests":[{"kind":"close","byte_offset":0}]})",
       R"({"requests":[{"kind":"read","byte_offset":-1}]})",
       R"({"requests":[{"kind":"read","byte_offset":"0x8000000000000000"}]})",
       R"({"requests":[{"kind":"read","byte_offset":"0x7fffffffffffffff","output_size":1}]})",
       R"({"requests":[{"kind":"write","byte_offset":9223372036854775807,"input":"ab"}]})",
       R"({"requests":[{"kind":"read","file":4294967296}]})",
       R"({"requests":[{"kind":"ioctl","code":1,"direct_input":"aabb","output_size":1}]})",
       R"({"requests":[{"kind":"ioctl","code":0,"direct_input":"aa","output_size":1}]})",
       R"({"requests":[{"kind":"ioctl","code":3,"direct_input":"aa","output_size":1}]})",
       R"({"kernel_exports":[]})", R"({"kernel_exports":{"Foo":1}})",
       R"({"kernel_exports":{"":true}})",
       R"({"kernel_exports":{"Foo":true,"Foo":false}})"}) {
    SCOPED_TRACE(Text);
    auto Result = driverOptionsFromScenarioJSON(Text);
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_FALSE(llvm::toString(Result.takeError()).empty());
  }
}

TEST(DriverScenario, NativeOptionsShareTransferAndAggregateBudgetValidation) {
  DriverOptions Base;
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Read;
  Request.Input = {1};
  Base.Requests.push_back(Request);
  auto Result = driverOptionsFromScenarioJSON("{}", Base);
  ASSERT_FALSE(static_cast<bool>(Result));
  llvm::consumeError(Result.takeError());
  Base.Requests.clear();
  Request.Kind = DriverRequestKind::DeviceControl;
  Request.ControlCode = 1;
  Request.Input.clear();
  Request.OutputSize = DriverScenarioBufferLimit;
  Request.DirectInput.assign(DriverScenarioBufferLimit, 1);
  Base.Requests.assign(5, Request);
  Result = driverOptionsFromScenarioJSON("{}", Base);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("512 KiB"),
            std::string::npos);
}

TEST(DriverScenario, NativeInvalidTransfersFailBeforeImageLoading) {
  DriverRequest Read;
  Read.Kind = DriverRequestKind::Read;
  Read.ByteOffset = INT64_MAX;
  Read.OutputSize = 1;
  DriverRequest Write;
  Write.Kind = DriverRequestKind::Write;
  Write.ByteOffset = INT64_MAX;
  Write.Input = {0xab};
  DriverRequest Buffered;
  Buffered.ControlCode = 0;
  Buffered.OutputSize = 1;
  Buffered.DirectInput = {0xab};
  for (const auto &Request : {Read, Write, Buffered}) {
    DriverOptions Options;
    Options.Requests.push_back(Request);
    auto Result =
        emulateDriver("missing-scenario-preflight-image.sys", Options);
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_EQ(llvm::toString(Result.takeError()).find("driver scenario:"), 0u);
  }
}

TEST(DriverScenario, NativeDeviceNamesSharePrintableASCIIValidation) {
  for (unsigned char Byte : {0, 0x1f, 0x7f, 0x80}) {
    SCOPED_TRACE(static_cast<unsigned>(Byte));
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Create;
    Request.Device = "\\Device\\Name";
    Request.Device += static_cast<char>(Byte);
    DriverOptions Options;
    Options.Requests.push_back(Request);
    auto Result =
        emulateDriver("missing-scenario-preflight-image.sys", Options);
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("printable ASCII"),
              std::string::npos);
  }
}

TEST(DriverScenario, AcceptsOffsetBoundariesAndDefersNeitherExecutionSupport) {
  auto Result = driverOptionsFromScenarioJSON(R"({"requests":[
    {"kind":"read","byte_offset":"0x7fffffffffffffff","output_size":0},
    {"kind":"read","byte_offset":"0x7ffffffffffffffe","output_size":1},
    {"kind":"write","byte_offset":"0x7fffffffffffffff","input":""},
    {"kind":"write","byte_offset":"0x7ffffffffffffffe","input":"ab"},
    {"kind":"ioctl","code":3,"output_size":1}
  ]})");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Requests.size(), 5u);
  EXPECT_EQ(Result->Requests.back().ControlCode, 3u);
  auto Native = driverOptionsFromScenarioJSON("{}", *Result);
  ASSERT_TRUE(static_cast<bool>(Native)) << llvm::toString(Native.takeError());
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
