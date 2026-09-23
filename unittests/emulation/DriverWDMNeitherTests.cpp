//===- DriverWDMNeitherTests.cpp - Genuine WDK user-buffer execution -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute actual WDK packet, probe, CPU exception and user MDL paths.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "neverd/emulation/DriverSession.h"

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
DriverOptions options(uint32_t Code, uint64_t Address) {
  DriverOptions Options;
  Options.LoadAddress = Address;
  Options.Unload = true;
  auto Request = [&](DriverRequestKind Kind) {
    DriverRequest Item;
    Item.Kind = Kind;
    Item.Device = "\\Device\\NeverDNeither";
    Options.Requests.push_back(std::move(Item));
  };
  Request(DriverRequestKind::Create);
  Request(DriverRequestKind::DeviceControl);
  auto &IO = Options.Requests.back();
  IO.ControlCode = Code;
  IO.Input = {1, 2, 3, 4};
  IO.OutputSize = 4;
  Request(DriverRequestKind::Cleanup);
  Request(DriverRequestKind::Close);
  return Options;
}

TEST(DriverWDMNeither, OriginalWDKHandlersObserveRealUserBytesAndFaultSites) {
  const std::pair<uint32_t, std::vector<uint8_t>> Cases[]{
      {0x222003, {0x5b, 0x58, 0x59, 0x5e}},
      {0x222007, {0xa1}},
      {0x22200b, {4}},
      {0x22200f, {2}},
      {0x222013, {2, 3, 4, 5}},
      {0x222017, {0xa6}}};
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       })
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (const auto &[Code, Output] : Cases) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Address);
        SCOPED_TRACE(Code);
        auto Result = emulateDriver(Image, options(Code, Address));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), 4u);
        EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
        EXPECT_EQ(Result->Requests[1].Output, Output);
        EXPECT_TRUE(Result->Requests[1].Completed);
        EXPECT_TRUE(Result->UnloadCompleted);
        EXPECT_FALSE(Result->Fault);
      }
}
#endif
} // namespace
} // namespace neverd::emulation
