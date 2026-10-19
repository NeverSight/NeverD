//===- KernelFirmwareQueryTests.cpp - Raw SMBIOS query semantics
//-----------===//
///
/// \file
/// Raw SMBIOS query semantics.
///

#include "SmbiosTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
TEST(DriverSmbios, StrictTransportAndReportRetainExactGuestBytes) {
  auto F = smbiosfixture::table();
  const auto Text =
      "{\"smbios\":{\"raw_hex\":\"" + llvm::toHex(F.Raw, true) + "\"}}";
  auto O = driverOptionsFromScenarioJSON(Text);
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->Smbios);
  EXPECT_EQ(O->Smbios->Raw, F.Raw);
  auto Preserved = driverOptionsFromScenarioJSON("{}", *O);
  ASSERT_TRUE(bool(Preserved));
  EXPECT_EQ(Preserved->Smbios->Raw, F.Raw);
  DriverResult R;
  R.Configuration = *Preserved;
  auto JSON = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(JSON));
  EXPECT_EQ(JSON->getAsObject()
                ->getObject("configuration")
                ->getObject("smbios")
                ->getString("raw_hex"),
            llvm::toHex(F.Raw, true));
  for (const char *Bad :
       {"null", "{}", "{\"raw_hex\":0}", "{\"raw_hex\":\"xx\"}",
        "{\"raw_hex\":\"0\"}", "{\"raw_hex\":\"\",\"extra\":1}"}) {
    auto P =
        driverOptionsFromScenarioJSON(std::string("{\"smbios\":") + Bad + "}");
    ASSERT_FALSE(bool(P)) << Bad;
    EXPECT_NE(llvm::toString(P.takeError()).find("smbios"), std::string::npos);
  }
}
TEST(DriverSmbios, MalformedFramingAndTypedInputsFailBeforePublication) {
  std::vector<DriverSmbios> Bad;
  auto Add = [&](auto Mutate) {
    auto F = smbiosfixture::table();
    Mutate(F.Raw);
    Bad.push_back(std::move(F));
  };
  Add([](auto &B) { B[4] ^= 1; });
  Add([](auto &B) { B[1] = 4; });
  Add([](auto &B) { B[9] = 3; });
  Add([](auto &B) { B.back() = 1; });
  Add([](auto &B) { B[B.size() - 6] = 126; });
  Add([](auto &B) { B[B.size() - 5] = 5; });
  Add([](auto &B) { B[B.size() - 4] = B[B.size() - 3] = 0; });
  Bad.push_back({});
  auto Large = smbiosfixture::table();
  Large.Raw.resize(firmware::SmbiosMaxBytes + 1);
  Bad.push_back(std::move(Large));
  for (const auto &F : Bad) {
    auto E = validateDriverSmbios(F);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    DriverOptions O;
    O.Smbios = F;
    auto P = driverOptionsFromScenarioJSON("{}", O);
    ASSERT_FALSE(bool(P));
    llvm::consumeError(P.takeError());
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend));
    DriverResult R;
    KernelModel M(**Backend, R);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    E = M.initialize(I, O);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(M.driverObject(), 0u);
  }
}
class KernelFirmwareQuery : public testing::Test {
protected:
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t Buffer = 0x60000000, Returned = 0x60002000;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void initialize(bool Provided = true, bool Version = true) {
    auto B = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(B));
    Memory = std::move(*B);
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverOptions O;
    if (Version)
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    if (Provided)
      O.Smbios = smbiosfixture::table();
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    ok(Model->initialize(I, O));
    // The owner must retain its own immutable copy of the supplied bytes.
    if (O.Smbios)
      O.Smbios->Raw.assign(20, 0xff);
    ok(Memory->map(Buffer, 0x3000, Read | Write));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void request(uint32_t Action = 1, uint32_t ID = 0,
               uint32_t Claimed = UINT32_MAX) {
    std::vector<uint8_t> B(4096, 0xa5);
    llvm::support::endian::write32le(B.data(), 0x52534d42);
    llvm::support::endian::write32le(B.data() + 4, Action);
    llvm::support::endian::write32le(B.data() + 8, ID);
    llvm::support::endian::write32le(B.data() + 12, Claimed);
    ok(Memory->write(Buffer, B));
    ok(Memory->writeInteger(Returned, 0xfeedbeef, 4));
  }
  uint64_t get(uint64_t At, unsigned Width = 4) {
    auto R = Memory->readInteger(At, Width);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  uint64_t query(uint32_t Length, const char *Name = "ZwQuerySystemInformation",
                 uint64_t Out = UINT64_MAX) {
    auto R = Model->call(
        Name, {76, Buffer, Length, Out == UINT64_MAX ? Returned : Out});
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
};
TEST_F(KernelFirmwareQuery,
       GetNegotiatesActualOuterCapacityAndCopiesExactBytes) {
  initialize();
  auto F = smbiosfixture::table();
  for (const char *Name :
       {"NtQuerySystemInformation", "ZwQuerySystemInformation"})
    for (uint32_t Capacity :
         {0u, 3u, uint32_t(F.Raw.size() - 1), uint32_t(F.Raw.size()),
          uint32_t(F.Raw.size() + 19)}) {
      request(1, 0x12345678, Capacity == 0 ? UINT32_MAX : 0);
      EXPECT_EQ(query(Capacity + 16, Name),
                Capacity >= F.Raw.size() ? 0u : 0xc0000023u);
      EXPECT_EQ(get(Returned), F.Raw.size() + 16);
      EXPECT_EQ(get(Buffer + 12), F.Raw.size());
      EXPECT_EQ(get(Buffer), 0x52534d42u);
      EXPECT_EQ(get(Buffer + 8), 0x12345678u); // Native RSMB ignores TableID.
      std::vector<uint8_t> Actual(F.Raw.size());
      ok(Memory->read(Buffer + 16, Actual));
      if (Capacity >= F.Raw.size())
        EXPECT_EQ(Actual, F.Raw);
      else
        EXPECT_EQ(Actual, std::vector<uint8_t>(F.Raw.size(), 0xa5));
      EXPECT_EQ(get(Buffer + 16 + F.Raw.size(), 1), 0xa5u);
    }
}
TEST_F(KernelFirmwareQuery,
       EnumerationHasOneIDAndInvalidActionRetainsCapacity) {
  initialize();
  for (uint32_t Capacity : {0, 3, 4, 16}) {
    request(0);
    EXPECT_EQ(query(16 + Capacity), Capacity < 4 ? 0xc0000023u : 0u);
    EXPECT_EQ(get(Returned), 20u);
    EXPECT_EQ(get(Buffer + 12), 4u);
    EXPECT_EQ(get(Buffer + 16), Capacity < 4 ? 0xa5a5a5a5u : 0u);
    EXPECT_EQ(get(Buffer + 20), 0xa5a5a5a5u);
  }
  for (uint32_t Action : {2u, UINT32_MAX}) {
    request(Action);
    EXPECT_EQ(query(19), 0xc00000efu);
    EXPECT_EQ(get(Returned), 19u);
    EXPECT_EQ(get(Buffer + 12), 3u);
    EXPECT_EQ(get(Buffer + 16), 0xa5a5a5a5u);
  }
}
TEST_F(KernelFirmwareQuery, ShortOuterLengthsDoNotDereferenceRequest) {
  initialize(false);
  for (uint32_t Length : {0, 1, 15}) {
    auto R = Model->call("ZwQuerySystemInformation", {76, 0, Length, Returned});
    ASSERT_TRUE(bool(R));
    EXPECT_EQ(*R, 0xc0000004u);
    EXPECT_EQ(get(Returned), 16u);
  }
}
TEST_F(KernelFirmwareQuery, MissingFirmwareProviderAndVersionAreExplicit) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    initialize(Mode != 0, Mode != 1);
    request();
    if (Mode == 2)
      ok(Memory->writeInteger(Buffer, 0x4649524d,
                              4)); // FIRM remains unmodeled.
    auto R =
        Model->call("ZwQuerySystemInformation", {76, Buffer, 19, Returned});
    ASSERT_FALSE(bool(R));
    const auto Text = llvm::toString(R.takeError());
    EXPECT_NE(Text.find(Mode == 0   ? "explicit smbios"
                        : Mode == 1 ? "19041"
                                    : "RSMB"),
              std::string::npos);
    EXPECT_EQ(get(Returned), 0xfeedbeefu);
    EXPECT_EQ(get(Buffer + 12), UINT32_MAX);
  }
}
TEST_F(KernelFirmwareQuery, WritePreflightIsAtomicAndReturnLengthWritesLast) {
  initialize();
  request();
  ok(Memory->protect(Returned, 4096, Read));
  auto R =
      Model->call("ZwQuerySystemInformation", {76, Buffer, 4096, Returned});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(Buffer + 12), UINT32_MAX);
  EXPECT_EQ(get(Buffer + 16), 0xa5a5a5a5u);
  ok(Memory->protect(Returned, 4096, Read | Write));
  request();
  EXPECT_EQ(query(4096, "ZwQuerySystemInformation", Buffer + 16), 0u);
  EXPECT_EQ(get(Buffer + 16), smbiosfixture::table().Raw.size() + 16);
  request();
  EXPECT_EQ(query(19, "ZwQuerySystemInformation", 0), 0xc0000023u);
  EXPECT_EQ(get(Buffer + 12), smbiosfixture::table().Raw.size());
  EXPECT_EQ(get(Returned), 0xfeedbeefu);
}
TEST_F(KernelFirmwareQuery, ProtectedPayloadAndRaisedIRQLDoNotCommitLengths) {
  initialize();
  // Header ends at a page boundary; required payload starts on a read-only
  // page. A too-small query does not touch that page, a full query must fail.
  Buffer += 4096 - 16;
  request();
  ok(Memory->protect(0x60001000, 4096, Read));
  EXPECT_EQ(query(19), 0xc0000023u);
  ok(Memory->writeInteger(Buffer + 12, UINT32_MAX, 4));
  ok(Memory->writeInteger(Returned, 0xfeedbeef, 4));
  auto R =
      Model->call("ZwQuerySystemInformation", {76, Buffer, 4096, Returned});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(Buffer + 12), UINT32_MAX);
  EXPECT_EQ(get(Returned), 0xfeedbeefu);
  R = Model->call("KfRaiseIrql", {2});
  ASSERT_TRUE(bool(R));
  R = Model->call("ZwQuerySystemInformation", {76, Buffer, 19, Returned});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("PASSIVE_LEVEL"),
            std::string::npos);
  EXPECT_EQ(get(Buffer + 12), UINT32_MAX);
}
TEST(KernelFirmwareQueryCaller, CompiledTwoPhaseClientParsesRecordsAndUnloads) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.Smbios = smbiosfixture::table();
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_smbios.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u);
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      unsigned Queries = 0;
      for (const auto &C : R->Calls)
        if (C.Name == "ZwQuerySystemInformation")
          ++Queries;
      EXPECT_EQ(Queries, 3u);
    }
}
} // namespace
} // namespace neverd::emulation
