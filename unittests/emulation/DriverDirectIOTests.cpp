//===- DriverDirectIOTests.cpp - Direct and stream I/O semantics ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest execution tests for request-owned MDLs, stream I/O and file identity.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {

std::filesystem::path directFixture() {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "driver_direct.sys";
}

DriverRequest request(DriverRequestKind Kind, uint32_t File = 0) {
  DriverRequest Value;
  Value.Kind = Kind;
  Value.File = File;
  return Value;
}

DriverRequest create(const char *Device = "\\Device\\NeverDDirect",
                     uint32_t File = 0) {
  auto Value = request(DriverRequestKind::Create, File);
  Value.Device = Device;
  return Value;
}

void close(DriverOptions &Options, uint32_t File = 0) {
  Options.Requests.push_back(request(DriverRequestKind::Cleanup, File));
  Options.Requests.push_back(request(DriverRequestKind::Close, File));
  Options.Unload = true;
}

DriverOptions directScenario(unsigned Method = 2, unsigned Action = 0) {
  DriverOptions Options;
  Options.Requests.push_back(create());
  auto IO = request(DriverRequestKind::DeviceControl);
  IO.ControlCode = 0x222000 | (Action << 2) | Method;
  IO.Input = {0x10};
  IO.DirectInput = {1, 2, 3};
  IO.OutputSize = 5;
  Options.Requests.push_back(std::move(IO));
  close(Options);
  return Options;
}

::testing::AssertionResult completed(const DriverResult &Result) {
  if (Result.Stop != DriverStopReason::Returned || !Result.UnloadCompleted)
    return ::testing::AssertionFailure() << Result.Diagnostic;
  for (const auto &Request : Result.Requests)
    if (!Request.Completed || Request.DispatchStatus != 0 ||
        Request.IOStatus != 0)
      return ::testing::AssertionFailure()
             << "request did not complete successfully";
  return ::testing::AssertionSuccess();
}

TEST(DriverDirectIO, InAndOutDirectKeepBothBuffersAndRealMacroReads) {
  for (unsigned Method : {1u, 2u}) {
    SCOPED_TRACE(Method);
    auto Result = emulateDriver(directFixture(), directScenario(Method));
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    ASSERT_TRUE(completed(*Result));
    ASSERT_EQ(Result->Requests.size(), 4u);
    EXPECT_EQ(Result->Requests[1].Output,
              (std::vector<uint8_t>{0x11, 0x12, 0x13, 0x10, 0x10}));
    EXPECT_EQ(std::count_if(Result->Calls.begin(), Result->Calls.end(),
                            [](const DriverAPIEvent &Call) {
                              return Call.Name ==
                                     "MmMapLockedPagesSpecifyCache";
                            }),
              1);
  }
}

TEST(DriverDirectIO, ExplicitNoWriteRestrictsEitherTransferMethod) {
  for (unsigned Method : {1u, 2u}) {
    auto Result = emulateDriver(directFixture(), directScenario(Method, 2));
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("MdlMappingNoWrite"), std::string::npos);
    ASSERT_EQ(Result->Requests.size(), 2u);
    EXPECT_FALSE(Result->Requests[1].Completed);
  }
}

TEST(DriverDirectIO, UnmappingRevokesTheGuestAddress) {
  auto Result = emulateDriver(directFixture(), directScenario(2, 3));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE(Result->Stop == DriverStopReason::MemoryFault ||
              Result->Stop == DriverStopReason::ModelError)
      << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_FALSE(Result->UnloadCompleted);
}

TEST(DriverDirectIO, ExecutePermissionFollowsTheMappingNoExecuteFlag) {
  for (unsigned Action : {12u, 13u}) {
    auto Options = directScenario(2, Action);
    // Original x64 guest code: mov eax, 0x5a; ret.
    Options.Requests[1].DirectInput = {0xb8, 0x5a, 0, 0, 0, 0xc3};
    Options.Requests[1].OutputSize = 6;
    auto Result = emulateDriver(directFixture(), Options);
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    if (Action == 12) {
      ASSERT_TRUE(completed(*Result));
      EXPECT_EQ(Result->Requests[1].Output, Options.Requests[1].DirectInput);
    } else {
      EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault)
          << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), 2u);
      EXPECT_FALSE(Result->Requests[1].Completed);
    }
  }
}

TEST(DriverDirectIO, ExecutableMappingDoesNotExposePagePadding) {
  auto Result = emulateDriver(directFixture(), directScenario(2, 14));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("MDL byte range"), std::string::npos);
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
}

TEST(DriverDirectIO, NonPageAlignedMDLSpansMultiplePagesWithoutLosingData) {
  auto Options = directScenario(2, 15);
  Options.InstructionLimit = 500000;
  auto &IO = Options.Requests[1];
  IO.OutputSize = 8193;
  IO.DirectInput.resize(IO.OutputSize);
  for (size_t I = 0; I < IO.DirectInput.size(); ++I)
    IO.DirectInput[I] = static_cast<uint8_t>(I);
  auto Expected = IO.DirectInput;
  for (auto &Byte : Expected)
    Byte ^= 0x10;
  auto Result = emulateDriver(directFixture(), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_TRUE(completed(*Result));
  EXPECT_EQ(Result->Requests[1].Information, 8193u);
  EXPECT_EQ(Result->Requests[1].Output, Expected);
}

TEST(DriverDirectIO, UnmapThenRemapPreservesTheLockedData) {
  auto Result = emulateDriver(directFixture(), directScenario(2, 4));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_TRUE(completed(*Result));
  EXPECT_EQ(Result->Requests[1].Output,
            (std::vector<uint8_t>{0x29, 0x12, 0x13, 0x10, 0x10}));
}

TEST(DriverDirectIO, CompletionRetainsDataAfterExplicitUnmapping) {
  auto Result = emulateDriver(directFixture(), directScenario(2, 11));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_TRUE(completed(*Result));
  EXPECT_EQ(Result->Requests[1].Output,
            (std::vector<uint8_t>{0x71, 2, 3, 0, 0}));
}

TEST(DriverDirectIO, PhysicalPageNumbersRemainExplicitlyUnmodeled) {
  auto Result = emulateDriver(directFixture(), directScenario(2, 5));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("PFN"), std::string::npos);
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
}

TEST(DriverDirectIO, CompletionExpiresTheSystemMapping) {
  auto Result = emulateDriver(directFixture(), directScenario(2, 6));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE(Result->Stop == DriverStopReason::MemoryFault ||
              Result->Stop == DriverStopReason::ModelError)
      << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_TRUE(Result->Requests[1].Completed);
  EXPECT_EQ(Result->Requests[1].Output,
            (std::vector<uint8_t>{0x11, 0x12, 0x13, 0x10, 0x10}));
  EXPECT_FALSE(Result->UnloadCompleted);
}

TEST(DriverDirectIO, PagePaddingIsNotPartOfTheMDLByteRange) {
  auto Result = emulateDriver(directFixture(), directScenario(2, 7));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("MDL byte range"), std::string::npos);
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
}

TEST(DriverDirectIO, RejectsDuplicateMappingAndInvalidUnmapOwnership) {
  for (unsigned Action : {8u, 9u, 10u}) {
    SCOPED_TRACE(Action);
    auto Result = emulateDriver(directFixture(), directScenario(2, Action));
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    ASSERT_EQ(Result->Requests.size(), 2u);
    EXPECT_FALSE(Result->Requests[1].Completed);
  }
}

TEST(DriverDirectIO, ZeroLengthDirectRequestsHaveNoMDL) {
  for (unsigned Method : {1u, 2u}) {
    auto Options = directScenario(Method);
    Options.Requests[1].OutputSize = 0;
    Options.Requests[1].DirectInput.clear();
    auto Result = emulateDriver(directFixture(), Options);
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    ASSERT_TRUE(completed(*Result));
    EXPECT_EQ(Result->Requests[1].Information, 0u);
    EXPECT_TRUE(Result->Requests[1].Output.empty());
    EXPECT_EQ(std::count_if(Result->Calls.begin(), Result->Calls.end(),
                            [](const DriverAPIEvent &Call) {
                              return Call.Name ==
                                     "MmMapLockedPagesSpecifyCache";
                            }),
              0);
  }
}

TEST(DriverDirectIO, BufferedAndDirectStreamsHonorOffsetsAndTransferredBytes) {
  for (const char *Device :
       {"\\Device\\NeverDDirect", "\\Device\\NeverDBuffered"}) {
    SCOPED_TRACE(Device);
    DriverOptions Options;
    Options.Requests.push_back(create(Device));
    auto Write = request(DriverRequestKind::Write);
    Write.Input = {1, 2, 3};
    Write.ByteOffset = 5;
    Options.Requests.push_back(std::move(Write));
    auto Read = request(DriverRequestKind::Read);
    Read.OutputSize = 4;
    Read.ByteOffset = 7;
    Options.Requests.push_back(std::move(Read));
    close(Options);
    auto Result = emulateDriver(directFixture(), Options);
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    ASSERT_TRUE(completed(*Result));
    ASSERT_EQ(Result->Requests.size(), 5u);
    EXPECT_EQ(Result->Requests[1].Information, 3u);
    EXPECT_TRUE(Result->Requests[1].Output.empty());
    EXPECT_EQ(Result->Requests[2].Information, 4u);
    EXPECT_EQ(Result->Requests[2].Output,
              (std::vector<uint8_t>{18, 19, 20, 21}));
    EXPECT_EQ(Result->Requests[2].ByteOffset, 7u);
  }
}

TEST(DriverDirectIO, InterleavedFilesKeepIndependentContextsAndLifetimes) {
  DriverOptions Options;
  Options.Requests = {create("\\Device\\NeverDDirect", 7),
                      create("\\Device\\NeverDDirect", 9)};
  auto Write = request(DriverRequestKind::Write, 7);
  Write.Input = {1, 2};
  Write.ByteOffset = 2;
  Options.Requests.push_back(Write);
  Write.File = 9;
  Write.Input = {8};
  Write.ByteOffset = 3;
  Options.Requests.push_back(Write);
  auto Read = request(DriverRequestKind::Read, 7);
  Read.OutputSize = 2;
  Read.ByteOffset = 4;
  Options.Requests.push_back(Read);
  close(Options, 7);
  Read.File = 9;
  Read.ByteOffset = 5;
  Options.Requests.push_back(Read);
  close(Options, 9);
  auto Result = emulateDriver(directFixture(), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_TRUE(completed(*Result));
  ASSERT_EQ(Result->Requests.size(), 10u);
  EXPECT_EQ(Result->Requests[4].File, 7u);
  EXPECT_EQ(Result->Requests[4].Output, (std::vector<uint8_t>{9, 10}));
  EXPECT_EQ(Result->Requests[7].File, 9u);
  EXPECT_EQ(Result->Requests[7].Output, (std::vector<uint8_t>{16, 17}));
}

TEST(DriverDirectIO, ZeroLengthStreamsCompleteWithoutAMapping) {
  for (const char *Device :
       {"\\Device\\NeverDDirect", "\\Device\\NeverDBuffered"}) {
    DriverOptions Options;
    Options.Requests = {create(Device), request(DriverRequestKind::Write),
                        request(DriverRequestKind::Read)};
    close(Options);
    auto Result = emulateDriver(directFixture(), Options);
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    ASSERT_TRUE(completed(*Result));
    EXPECT_EQ(Result->Requests[1].Information, 0u);
    EXPECT_EQ(Result->Requests[2].Information, 0u);
  }
}

TEST(DriverDirectIO, NeitherStreamAccessStopsBeforeDispatch) {
  DriverOptions Options;
  Options.Requests.push_back(create("\\Device\\NeverDNeither"));
  auto Read = request(DriverRequestKind::Read);
  Read.OutputSize = 4;
  Options.Requests.push_back(Read);
  auto Result = emulateDriver(directFixture(), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("DO_BUFFERED_IO"), std::string::npos);
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_EQ(Result->Requests[1].IRP, 0u);
}
} // namespace
} // namespace neverd::emulation
