//===- DriverWDMResourceTests.cpp - Genuine WDK register-bank access ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute original WDK scalar and buffer accessors against declared physical
/// register banks, including mapping ownership and restart persistence.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_RESOURCE_FIXTURE
std::vector<const char *> images() {
  std::vector<const char *> Images{NEVERD_WDM_RESOURCE_FIXTURE};
#ifdef NEVERD_WDM_RESOURCE_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_RESOURCE_CFG_FIXTURE);
#endif
  return Images;
}

DriverPnpDevice device(unsigned Index) {
  DriverPnpDevice Device;
  Device.ID = "register" + std::to_string(Index);
  Device.Bus = DriverBusKind::RegisterBank;
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  DriverMemoryResource Resource;
  Resource.ID = "bank0";
  Resource.RawStart = 0x200000000ULL + uint64_t(Index) * 0x100000;
  Resource.TranslatedStart = 0x300000000ULL + uint64_t(Index) * 0x100000;
  Resource.Length = 0x1000;
  constexpr auto RW = DriverRegisterAccess::ReadWrite;
  Resource.Registers = {{0, 1, RW, 0x12},
                        {2, 2, RW, 0x3456},
                        {4, 4, RW, 0x789abcde},
                        {8, 4, DriverRegisterAccess::ReadOnly, 0x10203040},
                        {0x10, 4, RW, 1},
                        {0x14, 4, RW, 2},
                        {0x18, 4, RW, 3},
                        {0x1c, 4, RW, 4},
                        {0xffc, 4, RW, 5}};
  Device.Resources.push_back(Resource);
  return Device;
}

DriverOptions options(char Mode = 'S') {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDResources") + Mode;
  Options.LoadAddress = 0x190000000;
  Options.Unload = true;
  Options.PnpDevices.push_back(device(0));
  return Options;
}

DriverRequest pnp(DevicePnpRequest Minor, uint64_t Delay = 0,
                  uint32_t Status = 0, llvm::StringRef ID = "register0") {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = ID.str();
  Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{Status, Delay}};
  return Request;
}

DriverRequest file(DriverRequestKind Kind, uint32_t File = 1,
                   llvm::StringRef ID = "register0") {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.File = File;
  if (Kind == DriverRequestKind::Create)
    Request.DeviceID = ID.str();
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = 0x222000;
    Request.OutputSize = 32;
  }
  return Request;
}

void fileCycle(DriverOptions &Options, uint32_t File = 1,
               llvm::StringRef ID = "register0") {
  for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
                    DriverRequestKind::Cleanup, DriverRequestKind::Close})
    Options.Requests.push_back(file(Kind, File, ID));
}

void remove(DriverOptions &Options, llvm::StringRef ID = "register0") {
  Options.Requests.push_back(pnp(DevicePnpRequest::QueryRemove, 0, 0, ID));
  Options.Requests.push_back(pnp(DevicePnpRequest::Remove, 7, 0, ID));
}

size_t apiCount(const DriverResult &Result, llvm::StringRef Name) {
  return std::count_if(Result.Calls.begin(), Result.Calls.end(),
                       [Name](const auto &Call) { return Call.Name == Name; });
}

size_t messageIndex(const DriverResult &Result, llvm::StringRef Text) {
  for (size_t I = 0; I < Result.Messages.size(); ++I)
    if (Result.Messages[I].find(Text.str()) != std::string::npos)
      return I;
  return Result.Messages.size();
}

void clean(const DriverResult &Result, size_t Devices = 1) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_EQ(Result.NTStatus, 0u);
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_TRUE(Result.Devices.empty());
  ASSERT_EQ(Result.PnpDevices.size(), Devices);
  for (const auto &Device : Result.PnpDevices) {
    EXPECT_EQ(Device.AddDeviceStatus, 0u);
    EXPECT_EQ(Device.PnpState, DevicePnpState::Removed);
    EXPECT_FALSE(Device.ProviderPresent);
  }
  for (const auto &Request : Result.Requests)
    EXPECT_TRUE(Request.Completed);
  for (const auto &Message : Result.Messages)
    EXPECT_EQ(Message.find("WDM resources: failure"), std::string::npos)
        << Message;
}

void snapshot(const DriverRequestResult &Request, unsigned Starts) {
  const std::vector<uint32_t> Values{
      0x12 + Starts,   0x3456 + Starts, 0x789abcde + Starts, 0x10203040,
      1 + 10 * Starts, 2 + 10 * Starts, 3 + 10 * Starts,     4 + 10 * Starts};
  std::vector<uint8_t> Bytes;
  for (uint32_t Value : Values)
    for (unsigned Shift = 0; Shift != 32; Shift += 8)
      Bytes.push_back(uint8_t(Value >> Shift));
  EXPECT_EQ(Request.IOStatus, 0u);
  EXPECT_EQ(Request.Information, 32u);
  EXPECT_EQ(Request.Output, Bytes);
}

void invalid(char Mode) {
  for (const auto *Image : images()) {
    SCOPED_TRACE(Image);
    SCOPED_TRACE(Mode);
    auto Options = options(Mode);
    Options.Requests.push_back(pnp(DevicePnpRequest::Start));
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, Mode == 'T' ? DriverStopReason::MemoryFault
                                        : DriverStopReason::ModelError)
        << Result->Diagnostic;
    EXPECT_FALSE(Result->Diagnostic.empty());
    EXPECT_FALSE(Result->UnloadCompleted);
    EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 0u);
    EXPECT_EQ(messageIndex(*Result, "mapped unit=1"), Result->Messages.size());
  }
}

TEST(DriverWDMResource,
     PackedListsScalarRepAndAliasesExecuteNormalCfgAndRebased) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = options();
      Options.LoadAddress = Address;
      Options.Requests.push_back(pnp(DevicePnpRequest::Start, 11));
      fileCycle(Options);
      remove(Options);
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      ASSERT_EQ(Result->Requests.size(), 7u);
      snapshot(Result->Requests[2], 1);
      ASSERT_TRUE(Result->Requests[0].Pnp);
      EXPECT_EQ(Result->Requests[0].Pnp->BusCompletedAt100ns, 11u);
      EXPECT_LT(
          messageIndex(*Result, "lower completed unit=1 status=0x00000000"),
          messageIndex(*Result, "mapped unit=1 start=1"));
      EXPECT_EQ(apiCount(*Result, "MmMapIoSpace"), 1u);
      EXPECT_EQ(apiCount(*Result, "MmMapIoSpaceEx"), 2u);
      EXPECT_EQ(apiCount(*Result, "MmUnmapIoSpace"), 3u);
      EXPECT_EQ(apiCount(*Result, "READ_REGISTER_ULONG"), 0u);
      EXPECT_EQ(apiCount(*Result, "WRITE_REGISTER_BUFFER_ULONG"), 0u);
      bool TailMap = false;
      for (const auto &Call : Result->Calls)
        if (Call.Name == "MmMapIoSpaceEx") {
          ASSERT_EQ(Call.Arguments.size(), 3u);
          if (Call.Arguments[0] == 0x300000ffcULL) {
            TailMap = true;
            EXPECT_EQ(Call.Arguments[1], 4u);
            EXPECT_EQ(Call.Arguments[2], 0x202u);
          }
        }
      EXPECT_TRUE(TailMap);
    }
}

TEST(DriverWDMResource,
     StopRestartPreservesPhysicalRegistersAndRecreatesMappings) {
  for (const auto *Image : images()) {
    SCOPED_TRACE(Image);
    auto Options = options();
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        file(DriverRequestKind::Create),
                        file(DriverRequestKind::DeviceControl),
                        pnp(DevicePnpRequest::QueryStop),
                        pnp(DevicePnpRequest::Stop),
                        file(DriverRequestKind::DeviceControl),
                        pnp(DevicePnpRequest::Start, 13),
                        file(DriverRequestKind::DeviceControl),
                        file(DriverRequestKind::Cleanup),
                        file(DriverRequestKind::Close)};
    remove(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_EQ(Result->Requests.size(), 12u);
    snapshot(Result->Requests[2], 1);
    EXPECT_EQ(Result->Requests[5].IOStatus, 0xc00000a3u);
    EXPECT_TRUE(Result->Requests[5].Output.empty());
    snapshot(Result->Requests[7], 2);
    EXPECT_EQ(apiCount(*Result, "MmMapIoSpace"), 2u);
    EXPECT_EQ(apiCount(*Result, "MmUnmapIoSpace"), 6u);
    EXPECT_LT(messageIndex(*Result, "unmapped unit=1"),
              messageIndex(*Result, "mapped unit=1 start=2"));
  }
}

TEST(DriverWDMResource, TwoPdoBanksRemainIndependentAcrossOneDeviceRestart) {
  for (const auto *Image : images()) {
    SCOPED_TRACE(Image);
    auto Options = options();
    Options.PnpDevices.push_back(device(1));
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        pnp(DevicePnpRequest::Start, 3, 0, "register1"),
                        pnp(DevicePnpRequest::QueryStop),
                        pnp(DevicePnpRequest::Stop),
                        pnp(DevicePnpRequest::Start)};
    fileCycle(Options, 1, "register0");
    fileCycle(Options, 2, "register1");
    remove(Options, "register0");
    remove(Options, "register1");
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 2);
    ASSERT_EQ(Result->Requests.size(), 17u);
    snapshot(Result->Requests[6], 2);
    snapshot(Result->Requests[10], 1);
    EXPECT_EQ(apiCount(*Result, "MmMapIoSpace"), 3u);
    EXPECT_EQ(apiCount(*Result, "MmUnmapIoSpace"), 9u);
  }
}

TEST(DriverWDMResource, FailedLowerStartDoesNotMapOrFabricateRegisterAccess) {
  for (const auto *Image : images()) {
    SCOPED_TRACE(Image);
    auto Options = options();
    Options.Requests = {pnp(DevicePnpRequest::Start, 9, 0xc0000001),
                        pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    EXPECT_EQ(Result->Requests[0].IOStatus, 0xc0000001u);
    EXPECT_EQ(apiCount(*Result, "MmMapIoSpace"), 0u);
    EXPECT_EQ(apiCount(*Result, "MmMapIoSpaceEx"), 0u);
    EXPECT_EQ(apiCount(*Result, "MmUnmapIoSpace"), 0u);
    EXPECT_LT(messageIndex(*Result, "failed lower START did not map"),
              Result->Messages.size());
  }
}

TEST(DriverWDMResource,
     SurpriseAllowsUnmapAndFileCleanupWithoutHardwareAccess) {
  for (const auto *Image : images()) {
    SCOPED_TRACE(Image);
    auto Options = options();
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        file(DriverRequestKind::Create),
                        pnp(DevicePnpRequest::SurpriseRemoval, 7),
                        file(DriverRequestKind::DeviceControl),
                        file(DriverRequestKind::Cleanup),
                        file(DriverRequestKind::Close),
                        pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    EXPECT_EQ(Result->Requests[3].IOStatus, 0xc000000eu);
    EXPECT_EQ(apiCount(*Result, "MmUnmapIoSpace"), 3u);
  }
}

TEST(DriverWDMResource,
     ExactRegisterWidthAlignmentAndDeclaredOffsetsAreEnforced) {
  for (char Mode : {'Q', 'U', 'G'})
    invalid(Mode);
}

TEST(DriverWDMResource,
     ReadOnlyMappingAndReadOnlyRegisterRejectActualGuestWrites) {
  for (char Mode : {'R', 'O'})
    invalid(Mode);
}

TEST(DriverWDMResource,
     UnknownPhysicalAddressRawAddressAndCachePolicyFailClearly) {
  for (char Mode : {'P', 'B', 'K', 'X'})
    invalid(Mode);
}

TEST(DriverWDMResource,
     PartialUnmapAndStaleVirtualAddressCannotBypassOwnership) {
  for (char Mode : {'H', 'T'})
    invalid(Mode);
}
#else
TEST(DriverWDMResource, GenuineWdkFixtureRequiresExplicitConfiguration) {
  GTEST_SKIP()
      << "Set NEVERD_WDM_RESOURCE_FIXTURE and optional "
         "NEVERD_WDM_RESOURCE_CFG_FIXTURE to execute the real WDK fixture";
}
#endif
} // namespace
} // namespace neverd::emulation
