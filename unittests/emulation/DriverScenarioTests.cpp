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

#include "llvm/Support/JSON.h"

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

TEST(DriverScenario, ParsesCancellationDelayBoundariesForDataRequests) {
  auto Result = driverOptionsFromScenarioJSON(R"({"requests":[
    {"kind":"read","cancel_after_100ns":0},
    {"kind":"write","cancel_after_100ns":1},
    {"kind":"ioctl","code":0,"cancel_after_100ns":9223372036854775807},
    {"kind":"ioctl","code":0}
  ]})");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Requests.size(), 4u);
  EXPECT_EQ(Result->Requests[0].CancelAfter100ns, 0u);
  EXPECT_EQ(Result->Requests[1].CancelAfter100ns, 1u);
  EXPECT_EQ(Result->Requests[2].CancelAfter100ns, uint64_t(INT64_MAX));
  EXPECT_FALSE(Result->Requests[3].CancelAfter100ns);
  auto Native = driverOptionsFromScenarioJSON("{}", *Result);
  ASSERT_TRUE(bool(Native)) << llvm::toString(Native.takeError());
  EXPECT_EQ(Native->Requests[2].CancelAfter100ns, uint64_t(INT64_MAX));
}

TEST(DriverScenario, ParsesAndRestrictsNeitherUserPageAccessFacts) {
  auto Parsed = driverOptionsFromScenarioJSON(R"({"requests":[
    {"kind":"ioctl","code":"0x222003","input":"0102","output_size":2,
     "user_input_access":"read_only","user_output_access":"no_access"},
    {"kind":"ioctl","code":"0x222003","input":"03","output_size":1,
     "user_input_access":"read_write"},
    {"kind":"read","output_size":4,"user_output_access":"read_only"},
    {"kind":"write","input":"01020304","user_input_access":"no_access"}
  ]})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Requests.size(), 4u);
  EXPECT_EQ(Parsed->Requests[0].UserInputAccess,
            DriverUserPageAccess::ReadOnly);
  EXPECT_EQ(Parsed->Requests[0].UserOutputAccess,
            DriverUserPageAccess::NoAccess);
  EXPECT_EQ(Parsed->Requests[1].UserInputAccess,
            DriverUserPageAccess::ReadWrite);
  EXPECT_FALSE(Parsed->Requests[1].UserOutputAccess);
  EXPECT_EQ(Parsed->Requests[2].UserOutputAccess,
            DriverUserPageAccess::ReadOnly);
  EXPECT_EQ(Parsed->Requests[3].UserInputAccess,
            DriverUserPageAccess::NoAccess);
  auto Native = driverOptionsFromScenarioJSON("{}", *Parsed);
  ASSERT_TRUE(bool(Native)) << llvm::toString(Native.takeError());

  for (
      const char *JSON :
      {R"({"requests":[{"kind":"ioctl","code":"0x222000","input":"01","user_input_access":"read_only"}]})",
       R"({"requests":[{"kind":"ioctl","code":"0x222003","user_input_access":"no_access"}]})",
       R"({"requests":[{"kind":"ioctl","code":"0x222003","input":"01","user_output_access":"read_only"}]})",
       R"({"requests":[{"kind":"read","output_size":1,"user_input_access":"no_access"}]})",
       R"({"requests":[{"kind":"write","input":"01","user_output_access":"no_access"}]})",
       R"({"requests":[{"kind":"ioctl","code":"0x222003","input":"01","user_input_access":"write_only"}]})",
       R"({"requests":[{"kind":"ioctl","code":"0x222003","input":"01","user_input_access":1}]})"}) {
    SCOPED_TRACE(JSON);
    auto Invalid = driverOptionsFromScenarioJSON(JSON);
    ASSERT_FALSE(bool(Invalid));
    EXPECT_NE(llvm::toString(Invalid.takeError()).find("user"),
              std::string::npos);
  }
  Parsed->Requests[0].ControlCode = 0x222000;
  auto InvalidNative = driverOptionsFromScenarioJSON("{}", *Parsed);
  ASSERT_FALSE(bool(InvalidNative));
  EXPECT_NE(llvm::toString(InvalidNative.takeError()).find("user"),
            std::string::npos);
}

TEST(DriverScenario, NativeUserPageAccessEnumsFailBeforeImageLoading) {
  for (int Value : {-1, 255}) {
    for (bool Input : {false, true}) {
      SCOPED_TRACE(Value);
      SCOPED_TRACE(Input);
      DriverRequest Request;
      Request.Kind = DriverRequestKind::DeviceControl;
      Request.ControlCode = 3;
      Request.Input = {1};
      Request.OutputSize = 1;
      auto &Access = Input ? Request.UserInputAccess : Request.UserOutputAccess;
      Access = static_cast<DriverUserPageAccess>(Value);
      DriverOptions Options;
      Options.Requests.push_back(std::move(Request));
      auto Parsed = driverOptionsFromScenarioJSON("{}", Options);
      ASSERT_FALSE(bool(Parsed));
      const auto Diagnostic = llvm::toString(Parsed.takeError());
      EXPECT_NE(
          Diagnostic.find(Input ? "user_input_access" : "user_output_access"),
          std::string::npos);
      auto Native =
          emulateDriver("missing-user-page-access-preflight.sys", Options);
      ASSERT_FALSE(bool(Native));
      EXPECT_EQ(llvm::toString(Native.takeError()), Diagnostic);
    }
  }
}

TEST(DriverScenario, ParsesAndRestrictsPostDispatchUserUnmapping) {
  auto Parsed = driverOptionsFromScenarioJSON(R"({"requests":[
    {"kind":"ioctl","code":"0x222003","input":"01","output_size":1,
     "user_unmap_after_dispatch":true},
    {"kind":"read","output_size":1,
     "user_unmap_after_dispatch":false}
  ]})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Requests.size(), 2u);
  EXPECT_EQ(Parsed->Requests[0].UserUnmapAfterDispatch, true);
  EXPECT_EQ(Parsed->Requests[1].UserUnmapAfterDispatch, false);
  for (
      const char *JSON :
      {R"({"requests":[{"kind":"read","output_size":1,"user_unmap_after_dispatch":1}]})",
       R"({"requests":[{"kind":"read","output_size":1,"user_unmap_after_dispatch":null}]})",
       R"({"requests":[{"kind":"create","user_unmap_after_dispatch":true}]})",
       R"({"requests":[{"kind":"read","user_unmap_after_dispatch":true}]})",
       R"({"requests":[{"kind":"ioctl","code":"0x222000","input":"01","user_unmap_after_dispatch":true}]})"}) {
    SCOPED_TRACE(JSON);
    auto Invalid = driverOptionsFromScenarioJSON(JSON);
    ASSERT_FALSE(bool(Invalid));
    EXPECT_NE(
        llvm::toString(Invalid.takeError()).find("user_unmap_after_dispatch"),
        std::string::npos);
  }
}

TEST(DriverScenario, ParsesRequestorIdentityAndExit) {
  auto Parsed = driverOptionsFromScenarioJSON(R"({"requests":[
    {"kind":"ioctl","code":"0x222003","input":"01","output_size":1,
     "requestor_process_id":4112,"requestor_exit_after_dispatch":true}
  ]})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Requests.size(), 1u);
  EXPECT_EQ(Parsed->Requests[0].RequestorProcessID, 4112u);
  EXPECT_EQ(Parsed->Requests[0].RequestorExitAfterDispatch, true);
  for (
      const char *JSON : {
          R"({"requests":[{"kind":"ioctl","code":"0x222003","input":"01","requestor_process_id":4}]})",
          R"({"requests":[{"kind":"ioctl","code":"0x222003","input":"01","requestor_process_id":1.0}]})",
          R"({"requests":[{"kind":"pnp","device_id":"p","minor":"start","requestor_process_id":4096}]})",
          R"({"requests":[{"kind":"read","output_size":1,"requestor_exit_after_dispatch":1}]})",
          R"({"requests":[{"kind":"create","requestor_exit_after_dispatch":true}]})",
          R"({"requests":[{"kind":"read","output_size":1,"requestor_exit_after_dispatch":true,"user_unmap_after_dispatch":true}]})",
      }) {
    SCOPED_TRACE(JSON);
    auto Invalid = driverOptionsFromScenarioJSON(JSON);
    ASSERT_FALSE(bool(Invalid));
    const std::string Error = llvm::toString(Invalid.takeError());
    EXPECT_TRUE(Error.find("requestor_") != std::string::npos ||
                Error.find("user_unmap_after_dispatch") != std::string::npos)
        << Error;
  }
}

TEST(DriverScenario, DeferredCallbackDrainRequiresTransferAndBoolean) {
  auto Parsed = driverOptionsFromScenarioJSON(R"({"requests":[
    {"kind":"ioctl","code":"0x222000","defer_callback_drain":true},
    {"kind":"read","output_size":1,"defer_callback_drain":false}
  ]})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Requests.size(), 2u);
  EXPECT_TRUE(Parsed->Requests[0].DeferCallbackDrain);
  EXPECT_FALSE(Parsed->Requests[1].DeferCallbackDrain);
  for (
      const char *JSON : {
          R"({"requests":[{"kind":"ioctl","code":"0x222000","defer_callback_drain":1}]})",
          R"({"requests":[{"kind":"create","defer_callback_drain":true}]})",
      }) {
    auto Invalid = driverOptionsFromScenarioJSON(JSON);
    ASSERT_FALSE(bool(Invalid));
    EXPECT_NE(llvm::toString(Invalid.takeError()).find("defer_callback_drain"),
              std::string::npos);
  }
}

TEST(DriverScenario, AsynchronousFileRequiresCreateAndBoolean) {
  auto Parsed = driverOptionsFromScenarioJSON(R"({"requests":[
    {"kind":"create","asynchronous_file":true},
    {"kind":"create","file":1,"asynchronous_file":false}
  ]})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Requests.size(), 2u);
  EXPECT_EQ(Parsed->Requests[0].AsynchronousFile, true);
  EXPECT_EQ(Parsed->Requests[1].AsynchronousFile, false);
  for (
      const char *JSON : {
          R"({"requests":[{"kind":"create","asynchronous_file":1}]})",
          R"({"requests":[{"kind":"ioctl","code":"0x222000","asynchronous_file":false}]})",
          R"({"requests":[{"kind":"pnp","asynchronous_file":true}]})",
      }) {
    auto Invalid = driverOptionsFromScenarioJSON(JSON);
    ASSERT_FALSE(bool(Invalid));
    EXPECT_NE(llvm::toString(Invalid.takeError()).find("asynchronous_file"),
              std::string::npos);
  }
}

TEST(DriverScenario, RejectsCancellationDelayTypesAndOverflow) {
  for (const char *Value :
       {"-1", "0.0", "1.0", "1e0", "true", "false", "null", R"("0")",
        R"("0x0")", "[]", "{}", "9223372036854775808", "18446744073709551615",
        "18446744073709551616", "1e100"}) {
    SCOPED_TRACE(Value);
    auto Result = driverOptionsFromScenarioJSON(
        std::string(R"({"requests":[{"kind":"read","cancel_after_100ns":)") +
        Value + "}]}");
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("cancel_after_100ns"),
              std::string::npos);
  }
}

TEST(DriverScenario, RejectsCancellationOnLifecycleAndUnknownFieldLocations) {
  for (const char *Kind : {"create", "cleanup", "close"}) {
    SCOPED_TRACE(Kind);
    auto Result =
        driverOptionsFromScenarioJSON(std::string(R"({"requests":[{"kind":")") +
                                      Kind + R"(","cancel_after_100ns":0}]})");
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("cancel_after_100ns"),
              std::string::npos);
  }
  for (const char *JSON :
       {R"({"cancel_after_100ns":0})",
        R"({"requests":[{"kind":"read","cancel_after_ns":0}]})",
        R"({"requests":[{"kind":"read","cancel_requested_at_100ns":0}]})"}) {
    SCOPED_TRACE(JSON);
    auto Result = driverOptionsFromScenarioJSON(JSON);
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("unknown field"),
              std::string::npos);
  }
}

TEST(DriverScenario, RejectsDuplicateCancellationFieldsAndEscapedAliases) {
  for (
      const char *JSON :
      {R"({"requests":[{"kind":"read","cancel_after_100ns":0,"cancel_after_100ns":1}]})",
       R"({"requests":[{"kind":"read","cancel_after_100ns":0,"cancel_after_100n\u0073":1}]})"}) {
    SCOPED_TRACE(JSON);
    auto Result = driverOptionsFromScenarioJSON(JSON);
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("duplicate field"),
              std::string::npos);
  }
}

TEST(DriverScenario, CancellationRequestsInheritOrReplaceTheBaseAsAWhole) {
  DriverOptions Base;
  Base.InstructionLimit = 37;
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Read;
  Input.File = 7;
  Input.CancelAfter100ns = 17;
  Base.Requests.push_back(Input);
  auto Unchanged = driverOptionsFromScenarioJSON("{}", Base);
  ASSERT_TRUE(bool(Unchanged)) << llvm::toString(Unchanged.takeError());
  ASSERT_EQ(Unchanged->Requests.size(), 1u);
  EXPECT_EQ(Unchanged->Requests[0].CancelAfter100ns, 17u);
  EXPECT_EQ(Unchanged->Requests[0].File, 7u);
  EXPECT_EQ(Unchanged->InstructionLimit, 37u);

  auto Replaced = driverOptionsFromScenarioJSON(
      R"({"requests":[{"kind":"read"},{"kind":"read","cancel_after_100ns":0}]})",
      Base);
  ASSERT_TRUE(bool(Replaced)) << llvm::toString(Replaced.takeError());
  ASSERT_EQ(Replaced->Requests.size(), 2u);
  EXPECT_FALSE(Replaced->Requests[0].CancelAfter100ns);
  EXPECT_EQ(Replaced->Requests[1].CancelAfter100ns, 0u);
  EXPECT_EQ(Replaced->InstructionLimit, 37u);
  EXPECT_EQ(Base.Requests[0].CancelAfter100ns, 17u);
  auto Cleared = driverOptionsFromScenarioJSON(R"({"requests":[]})", Base);
  ASSERT_TRUE(bool(Cleared)) << llvm::toString(Cleared.takeError());
  EXPECT_TRUE(Cleared->Requests.empty());
  EXPECT_EQ(Base.Requests.size(), 1u);
}

TEST(DriverScenario, NativeInvalidCancellationFailsBeforeImageLoading) {
  for (auto Kind :
       {DriverRequestKind::Create, DriverRequestKind::Cleanup,
        DriverRequestKind::Close, DriverRequestKind::Read,
        DriverRequestKind::Write, DriverRequestKind::DeviceControl}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    DriverRequest Input;
    Input.Kind = Kind;
    const bool Lifecycle = Kind == DriverRequestKind::Create ||
                           Kind == DriverRequestKind::Cleanup ||
                           Kind == DriverRequestKind::Close;
    Input.CancelAfter100ns = Lifecycle ? 0 : uint64_t(INT64_MAX) + 1;
    DriverOptions Options;
    Options.Requests.push_back(Input);
    auto Parsed = driverOptionsFromScenarioJSON("{}", Options);
    ASSERT_FALSE(bool(Parsed));
    EXPECT_NE(llvm::toString(Parsed.takeError()).find("cancel_after_100ns"),
              std::string::npos);
    auto Native = emulateDriver("missing-cancellation-preflight.sys", Options);
    ASSERT_FALSE(bool(Native));
    const auto Diagnostic = llvm::toString(Native.takeError());
    EXPECT_EQ(Diagnostic.find("driver scenario:"), 0u);
    EXPECT_NE(Diagnostic.find("cancel_after_100ns"), std::string::npos);
  }
}

TEST(DriverScenario,
     ReportDistinguishesAbsentZeroAndFullWidthCancellationTime) {
  DriverResult Result;
  Result.Requests.resize(4);
  Result.Requests[1].CancelRequestedAt100ns = 0;
  Result.Requests[2].CancelRequestedAt100ns = uint64_t(INT64_MAX);
  Result.Requests[3].CancelRequestedAt100ns = UINT64_MAX;
  const auto Text = driverResultJSON(Result);
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  const auto *Requests = Root->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 4u);
  for (size_t Index = 0; Index < Requests->size(); ++Index) {
    const auto *Request = (*Requests)[Index].getAsObject();
    ASSERT_NE(Request, nullptr);
    const auto *Time = Request->get("cancel_requested_at_100ns");
    ASSERT_NE(Time, nullptr);
    if (!Index)
      EXPECT_EQ(Time->kind(), llvm::json::Value::Null);
    else
      EXPECT_EQ(Time->getAsUINT64(),
                Result.Requests[Index].CancelRequestedAt100ns);
  }
  EXPECT_NE(Text.find("\"cancel_requested_at_100ns\":18446744073709551615"),
            std::string::npos);
}

DriverRequest userMemoryRequest() {
  DriverRequest Request;
  Request.ControlCode = 0x222003;
  Request.Input.resize(profile::PointerSize * 2);
  Request.OutputSize = profile::PointerSize * 2;
  Request.UserBuffers.push_back(
      {"payload", 16, {0x12, 0x34}, DriverUserPageAccess::ReadOnly});
  Request.UserPointers.push_back(
      {{DriverUserBufferKind::Input, {}, 0},
       {DriverUserBufferKind::Memory, "payload", 0}});
  return Request;
}

TEST(DriverScenario, UserMemoryParsesExplicitNestedPointersAndDefaults) {
  auto Parsed = driverOptionsFromScenarioJSON(R"({"requests":[{
    "kind":"ioctl","code":"0x222003",
    "input":"00000000000000000000000000000000","output_size":16,
    "user_buffers":[
      {"id":"payload","size":16,"input":"1234","access":"read_only"},
      {"id":"input","size":8}
    ],
    "user_pointers":[
      {"source":{"buffer":"input","offset":0},
       "target":{"buffer":"memory","id":"payload","offset":0}},
      {"source":{"buffer":"input","offset":8},
       "target":{"buffer":"memory","id":"payload","offset":16}},
      {"source":{"buffer":"memory","id":"payload","offset":0},
       "target":{"buffer":"input","offset":0}},
      {"source":{"buffer":"output","offset":1},
       "target":{"buffer":"memory","id":"payload","offset":0}}
    ]
  }]})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Requests.size(), 1u);
  const auto &Request = Parsed->Requests.front();
  ASSERT_EQ(Request.UserBuffers.size(), 2u);
  EXPECT_EQ(Request.UserBuffers[0].ID, "payload");
  EXPECT_EQ(Request.UserBuffers[0].Size, 16u);
  EXPECT_EQ(Request.UserBuffers[0].Input, (std::vector<uint8_t>{0x12, 0x34}));
  EXPECT_EQ(Request.UserBuffers[0].Access, DriverUserPageAccess::ReadOnly);
  EXPECT_EQ(Request.UserBuffers[1].ID, "input");
  EXPECT_TRUE(Request.UserBuffers[1].Input.empty());
  EXPECT_EQ(Request.UserBuffers[1].Access, DriverUserPageAccess::ReadWrite);
  ASSERT_EQ(Request.UserPointers.size(), 4u);
  EXPECT_EQ(Request.UserPointers[1].Target.Offset, 16u);
  EXPECT_EQ(Request.UserPointers[2].Source.Kind, DriverUserBufferKind::Memory);
  EXPECT_EQ(Request.UserPointers[2].Target.Kind, DriverUserBufferKind::Input);
  EXPECT_EQ(Request.UserPointers[3].Source.Offset, 1u);
  auto Native = driverOptionsFromScenarioJSON("{}", *Parsed);
  ASSERT_TRUE(bool(Native)) << llvm::toString(Native.takeError());
}

TEST(DriverScenario, UserMemoryRejectsMalformedDeclarationsAndReferences) {
  const std::string Prefix =
      R"({"requests":[{"kind":"ioctl","code":3,"input":"0000000000000000",)";
  for (
      const char *Fields :
      {R"("user_buffers":null)",
       R"("user_buffers":[null])",
       R"("user_buffers":[{"size":1}])",
       R"("user_buffers":[{"id":"x"}])",
       R"("user_buffers":[{"id":"","size":1}])",
       R"("user_buffers":[{"id":"bad id","size":1}])",
       R"("user_buffers":[{"id":"x","size":0}])",
       R"("user_buffers":[{"id":"x","size":1.0}])",
       R"("user_buffers":[{"id":"x","size":65537}])",
       R"("user_buffers":[{"id":"x","size":1,"input":"0102"}])",
       R"("user_buffers":[{"id":"x","size":1,"input":"0"}])",
       R"("user_buffers":[{"id":"x","size":1,"input":null}])",
       R"("user_buffers":[{"id":"x","size":1,"access":"execute"}])",
       R"("user_buffers":[{"id":"x","size":1,"access":false}])",
       R"("user_buffers":[{"id":"x","size":1,"address":4096}])",
       R"("user_buffers":[{"id":"x","size":1},{"id":"x","size":1}])",
       R"("user_buffers":[{"id":"x","id":"y","size":1}])",
       R"("user_pointers":null)",
       R"("user_pointers":[null])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":0}}])",
       R"("user_pointers":[{"source":null,"target":{}}])",
       R"("user_pointers":[{"source":{"buffer":"input"},"target":{"buffer":"input","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":0},"target":{"buffer":"kernel","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":0},"target":{"buffer":"memory","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":0},"target":{"buffer":"memory","id":"missing","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","id":"","offset":0},"target":{"buffer":"input","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":-1},"target":{"buffer":"input","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":0.0},"target":{"buffer":"input","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":4294967296},"target":{"buffer":"input","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":1},"target":{"buffer":"input","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":0},"target":{"buffer":"input","offset":9}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":0},"target":{"buffer":"output","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":0,"extra":0},"target":{"buffer":"input","offset":0}}])",
       R"("user_pointers":[{"source":{"buffer":"input","offset":0},"target":{"buffer":"input","offset":0},"width":4}])"}) {
    SCOPED_TRACE(Fields);
    auto Parsed = driverOptionsFromScenarioJSON(Prefix + Fields + "}]}");
    ASSERT_FALSE(bool(Parsed));
    EXPECT_FALSE(llvm::toString(Parsed.takeError()).empty());
  }
  for (const char *Text :
       {R"({"requests":[{"kind":"create","user_buffers":[]}]})",
        R"({"requests":[{"kind":"close","user_pointers":[]}]})",
        R"({"requests":[{"kind":"ioctl","code":0,"user_buffers":[]}]})",
        R"({"requests":[{"kind":"ioctl","code":1,"user_pointers":[]}]})"}) {
    auto Parsed = driverOptionsFromScenarioJSON(Text);
    ASSERT_FALSE(bool(Parsed));
    EXPECT_NE(llvm::toString(Parsed.takeError()).find("user memory"),
              std::string::npos);
  }
}

TEST(DriverScenario, UserMemoryNativeErrorsPrecedeImageLoading) {
  using Mutator = void (*)(DriverRequest &);
  for (Mutator Change :
       {+[](DriverRequest &R) { R.Kind = DriverRequestKind::Create; },
        +[](DriverRequest &R) { R.ControlCode = 0; },
        +[](DriverRequest &R) { R.UserBuffers[0].Size = 0; },
        +[](DriverRequest &R) { R.UserBuffers[0].Size = 1; },
        +[](DriverRequest &R) { R.UserBuffers[0].ID.clear(); },
        +[](DriverRequest &R) {
          R.UserBuffers[0].Access = static_cast<DriverUserPageAccess>(-1);
        },
        +[](DriverRequest &R) {
          R.UserBuffers.push_back(R.UserBuffers.front());
        },
        +[](DriverRequest &R) {
          R.UserPointers[0].Source.Kind = static_cast<DriverUserBufferKind>(-1);
        },
        +[](DriverRequest &R) { R.UserPointers[0].Source.ID = "payload"; },
        +[](DriverRequest &R) { R.UserPointers[0].Source.Offset = UINT32_MAX; },
        +[](DriverRequest &R) { R.UserPointers[0].Target.Offset = 17; },
        +[](DriverRequest &R) { R.UserPointers[0].Target.ID = "missing"; },
        +[](DriverRequest &R) {
          auto Overlap = R.UserPointers.front();
          Overlap.Source.Offset = 1;
          R.UserPointers.push_back(std::move(Overlap));
        }}) {
    DriverOptions Options;
    auto Request = userMemoryRequest();
    Change(Request);
    Options.Requests.push_back(std::move(Request));
    auto Parsed = driverOptionsFromScenarioJSON("{}", Options);
    ASSERT_FALSE(bool(Parsed));
    const auto Diagnostic = llvm::toString(Parsed.takeError());
    SCOPED_TRACE(Diagnostic);
    EXPECT_FALSE(Diagnostic.empty());
    auto Native = emulateDriver("missing-user-memory-preflight.sys", Options);
    ASSERT_FALSE(bool(Native));
    EXPECT_EQ(llvm::toString(Native.takeError()), Diagnostic);
  }
}

TEST(DriverScenario, UserMemoryUsesRequestLocalIDsAndMatchingTransferBuffers) {
  DriverOptions Options;
  auto Read = userMemoryRequest();
  Read.Kind = DriverRequestKind::Read;
  Read.ControlCode = 0;
  Read.Input.clear();
  Read.UserPointers[0].Source.Kind = DriverUserBufferKind::Output;
  auto Write = userMemoryRequest();
  Write.Kind = DriverRequestKind::Write;
  Write.ControlCode = 0;
  Write.OutputSize = 0;
  Options.Requests = {Read, Write};
  auto Valid = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
  EXPECT_EQ(Valid->Requests[0].UserBuffers[0].ID,
            Valid->Requests[1].UserBuffers[0].ID);
  Options.Requests[0].UserPointers[0].Source.Kind = DriverUserBufferKind::Input;
  auto Invalid = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("source slot"),
            std::string::npos);
  Options.Requests = {Write};
  Options.Requests[0].UserPointers[0].Target = {
      DriverUserBufferKind::Output, {}, 0};
  Invalid = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("target"),
            std::string::npos);
}

TEST(DriverScenario, UserMemoryChargesDeclaredCapacityAndCombinedCounts) {
  DriverOptions Options;
  DriverRequest Request;
  Request.ControlCode = 3;
  for (size_t I = 0;
       I < DriverScenarioTotalBufferLimit / DriverScenarioBufferLimit; ++I)
    Request.UserBuffers.push_back(
        {"buffer" + std::to_string(I), DriverScenarioBufferLimit, {}});
  Options.Requests = {Request};
  auto Valid = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
  Options.Requests[0].Input = {0};
  auto Invalid = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(
      llvm::toString(Invalid.takeError()).find("combined request buffers"),
      std::string::npos);

  Request.UserBuffers.clear();
  for (size_t I = 0; I < DriverScenarioUserBufferLimit / 2; ++I)
    Request.UserBuffers.push_back({"buffer" + std::to_string(I), 1, {}});
  Options.Requests = {Request, Request};
  Valid = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
  Options.Requests.back().UserBuffers.push_back({"extra", 1, {}});
  Invalid = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(
      llvm::toString(Invalid.takeError()).find("combined buffer or pointer"),
      std::string::npos);

  Request.UserBuffers = {{"target", 1, {}}};
  Request.Input.resize(DriverScenarioUserPointerLimit * profile::PointerSize);
  for (size_t I = 0; I < DriverScenarioUserPointerLimit / 2; ++I)
    Request.UserPointers.push_back(
        {{DriverUserBufferKind::Input,
          {},
          static_cast<uint32_t>(I * profile::PointerSize)},
         {DriverUserBufferKind::Memory, "target", 0}});
  Options.Requests = {Request, Request};
  Valid = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
  Options.Requests.back().UserPointers.push_back(
      {{DriverUserBufferKind::Input,
        {},
        static_cast<uint32_t>(DriverScenarioUserPointerLimit / 2 *
                              profile::PointerSize)},
       {DriverUserBufferKind::Memory, "target", 0}});
  Invalid = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(
      llvm::toString(Invalid.takeError()).find("combined buffer or pointer"),
      std::string::npos);
}

TEST(DriverScenario, UserMemoryRevocationAcceptsDeclaredOnlyBuffers) {
  for (const char *Event :
       {"user_unmap_after_dispatch", "requestor_exit_after_dispatch"}) {
    const std::string JSON =
        std::string(R"({"requests":[{"kind":"ioctl","code":3,")") + Event +
        R"(":true,"user_buffers":[{"id":"payload","size":16}]}]})";
    auto Parsed = driverOptionsFromScenarioJSON(JSON);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    EXPECT_TRUE(Parsed->Requests[0].Input.empty());
    EXPECT_EQ(Parsed->Requests[0].OutputSize, 0u);
  }
}

TEST(DriverScenario, UserMemoryReportSeparatesConfigurationAndObservedBacking) {
  DriverResult Result;
  Result.Configuration.Requests.resize(2);
  Result.Configuration.Requests[1] = userMemoryRequest();
  Result.Requests.resize(2);
  Result.Requests[1].UserBuffers.push_back({"payload",
                                            UINT64_MAX - 15,
                                            16,
                                            DriverUserPageAccess::ReadOnly,
                                            true,
                                            {0xab, 0xcd, 0xef}});
  auto Parsed = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  const auto *Configuration = Root->getObject("configuration");
  ASSERT_NE(Configuration, nullptr);
  const auto *Memory = Configuration->getArray("user_memory");
  ASSERT_NE(Memory, nullptr);
  ASSERT_EQ(Memory->size(), 1u);
  const auto *Config = (*Memory)[0].getAsObject();
  ASSERT_NE(Config, nullptr);
  EXPECT_EQ(Config->getInteger("source_request_index"), 1);
  const auto *Buffers = Config->getArray("user_buffers");
  ASSERT_NE(Buffers, nullptr);
  ASSERT_EQ(Buffers->size(), 1u);
  const auto *Buffer = (*Buffers)[0].getAsObject();
  ASSERT_NE(Buffer, nullptr);
  EXPECT_EQ(Buffer->getString("id"), "payload");
  EXPECT_EQ(Buffer->getString("input"), "1234");
  EXPECT_EQ(Buffer->getInteger("size"), 16);
  EXPECT_EQ(Buffer->getString("access"), "read_only");
  EXPECT_EQ(Buffer->get("address"), nullptr);
  const auto *Pointers = Config->getArray("user_pointers");
  ASSERT_NE(Pointers, nullptr);
  ASSERT_EQ(Pointers->size(), 1u);
  const auto *Pointer = (*Pointers)[0].getAsObject();
  ASSERT_NE(Pointer, nullptr);
  const auto *Source = Pointer->getObject("source");
  const auto *Target = Pointer->getObject("target");
  ASSERT_NE(Source, nullptr);
  ASSERT_NE(Target, nullptr);
  EXPECT_EQ(Source->getString("buffer"), "input");
  EXPECT_EQ(Source->get("id"), nullptr);
  EXPECT_EQ(Source->getInteger("offset"), 0);
  EXPECT_EQ(Target->getString("buffer"), "memory");
  EXPECT_EQ(Target->getString("id"), "payload");
  const auto *Requests = Root->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 2u);
  const auto *Empty = (*Requests)[0].getAsObject()->getArray("user_buffers");
  ASSERT_NE(Empty, nullptr);
  EXPECT_TRUE(Empty->empty());
  const auto *Observed = (*Requests)[1].getAsObject()->getArray("user_buffers");
  ASSERT_NE(Observed, nullptr);
  ASSERT_EQ(Observed->size(), 1u);
  const auto *State = (*Observed)[0].getAsObject();
  ASSERT_NE(State, nullptr);
  EXPECT_EQ(State->getString("address"), "0xFFFFFFFFFFFFFFF0");
  EXPECT_EQ(State->getString("backing_hex"), "abcdef");
  EXPECT_EQ(State->getBoolean("revoked"), true);
  EXPECT_EQ(State->getInteger("size"), 16);
  EXPECT_EQ(State->get("input"), nullptr);
  EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"), "");
}

} // namespace
} // namespace neverd::emulation
