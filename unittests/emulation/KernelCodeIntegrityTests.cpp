///
/// \file
/// KernelCodeIntegrityTests.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
TEST(DriverCodeIntegrityScenario, Uint32ObservationsPreserveZeroAndOpaqueBits) {
  for (uint32_t Value : {0u, 1u, 0x401u, 0x200000u, UINT32_MAX}) {
    auto O = driverOptionsFromScenarioJSON(
        "{\"code_integrity_options\":" + std::to_string(Value) + "}");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    EXPECT_EQ(O->CodeIntegrityOptions, Value);
    auto P = driverOptionsFromScenarioJSON("{}", *O);
    ASSERT_TRUE(bool(P));
    EXPECT_EQ(P->CodeIntegrityOptions, Value);
    DriverResult R;
    R.Configuration = *P;
    auto J = llvm::json::parse(driverResultJSON(R));
    ASSERT_TRUE(bool(J));
    EXPECT_EQ(J->getAsObject()
                  ->getObject("configuration")
                  ->getInteger("code_integrity_options"),
              int64_t(Value));
  }
  auto O = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(bool(O));
  EXPECT_FALSE(O->CodeIntegrityOptions.has_value());
  for (const char *Bad :
       {"null", "true", "-1", "4294967296", "1.5", "\"0\"", "[]", "{}"}) {
    auto P = driverOptionsFromScenarioJSON(
        std::string("{\"code_integrity_options\":") + Bad + "}");
    ASSERT_FALSE(bool(P));
    EXPECT_NE(llvm::toString(P.takeError()).find("code_integrity_options"),
              std::string::npos);
  }
}
class KernelCodeIntegrity : public testing::Test {
protected:
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t Buffer = 0x60000000, Returned = 0x60002000;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void initialize(std::optional<uint32_t> Value, unsigned Build = 19041) {
    Model.reset();
    Memory.reset();
    Buffer = 0x60000000;
    auto B = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(B));
    Memory = std::move(*B);
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverOptions O;
    if (Build)
      O.Version = DriverOptions::WindowsVersion{10, 0, Build};
    O.CodeIntegrityOptions = Value;
    O.SecurePciEnabled = true;
    O.DmaGuardPolicyEnabled = true;
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    ok(Model->initialize(I, O));
    O.CodeIntegrityOptions = ~Value.value_or(0);
    ok(Memory->map(Buffer, 0x3000, Read | Write));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void prepare(uint32_t Declared = 8) {
    ok(Memory->writeInteger(Buffer, Declared, 4));
    ok(Memory->writeInteger(Buffer + 4, 0xa5a5a5a5, 4));
    ok(Memory->writeInteger(Buffer + 8, 0x123456789abcdef0ULL, 8));
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
  uint64_t query(uint32_t Length,
                 const char *Name = "ZwQuerySystemInformation") {
    auto R = Model->call(Name, {103, Buffer, Length, Returned});
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
};
TEST_F(KernelCodeIntegrity,
       ExactQueryCopiesFinalDWORDAndPreservesCallerFields) {
  for (uint32_t Value : {0u, 0x401u, 0x200000u, UINT32_MAX}) {
    initialize(Value);
    for (const char *Name :
         {"ZwQuerySystemInformation", "NtQuerySystemInformation"}) {
      prepare();
      EXPECT_EQ(query(8, Name), 0u);
      EXPECT_EQ(get(Buffer), 8u);
      EXPECT_EQ(get(Buffer + 4), Value);
      EXPECT_EQ(get(Buffer + 8, 8), 0x123456789abcdef0ULL);
      EXPECT_EQ(get(Returned), 8u);
      EXPECT_TRUE(llvm::StringRef(Model->callDetail())
                      .starts_with("code_integrity_options=0x"));
    }
  }
}
TEST_F(KernelCodeIntegrity, BothLengthsMustMatchAndErrorsPreserveOptions) {
  initialize(0);
  for (uint32_t Length : {0, 1, 7, 8, 9, 16})
    for (uint32_t Declared : {0, 7, 8, 16}) {
      if (Length == 8 && Declared == 8)
        continue;
      prepare(Declared);
      EXPECT_EQ(query(Length), 0xc0000004u);
      EXPECT_EQ(get(Returned), 8u);
      EXPECT_EQ(get(Buffer), Declared);
      EXPECT_EQ(get(Buffer + 4), 0xa5a5a5a5u);
    }
  auto R = Model->call("ZwQuerySystemInformation", {103, 0, 0, Returned});
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0xc0000004u);
  R = Model->call("ZwQuerySystemInformation", {103, 0, 16, Returned});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
}
TEST_F(KernelCodeIntegrity,
       UnknownProviderStateAndVersionsNeverInferHostFlags) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    initialize(Mode == 0 ? std::nullopt : std::optional<uint32_t>(0),
               Mode == 1   ? 0
               : Mode == 2 ? 22621
                           : 19041);
    prepare();
    auto R =
        Model->call("ZwQuerySystemInformation", {103, Buffer, 8, Returned});
    ASSERT_FALSE(bool(R));
    EXPECT_NE(
        llvm::toString(R.takeError())
            .find(Mode == 0 ? "requires code_integrity_options" : "19041"),
        std::string::npos);
    EXPECT_EQ(get(Buffer + 4), 0xa5a5a5a5u);
    EXPECT_EQ(get(Returned), 0xfeedbeefu);
    if (Mode == 0) {
      R = Model->call("ZwQuerySystemInformation", {103, 0, 0, Returned});
      ASSERT_FALSE(bool(R));
      llvm::consumeError(R.takeError());
    }
  }
}
TEST_F(KernelCodeIntegrity, ReadOnlyLengthIsAllowedButOutputPreflightIsAtomic) {
  initialize(0x401);
  Buffer += 4092;
  prepare();
  ok(Memory->protect(0x60000000, 4096, Read));
  EXPECT_EQ(query(8), 0u);
  EXPECT_EQ(get(Buffer + 4), 0x401u);
  ok(Memory->protect(0x60000000, 4096, Read | Write));
  prepare();
  ok(Memory->protect(Returned, 4096, Read));
  auto R = Model->call("ZwQuerySystemInformation", {103, Buffer, 8, Returned});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(Buffer + 4), 0xa5a5a5a5u);
  EXPECT_EQ(get(Returned), 0xfeedbeefu);
  ok(Memory->protect(Returned, 4096, Read | Write));
  prepare();
  ok(Memory->protect(0x60001000, 4096, Read));
  R = Model->call("ZwQuerySystemInformation", {103, Buffer, 8, Returned});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(Returned), 0xfeedbeefu);
  // Mismatched length only reads the caller Length, not the protected options.
  EXPECT_EQ(query(16), 0xc0000004u);
  EXPECT_EQ(get(Returned), 8u);
}
TEST_F(KernelCodeIntegrity, ReturnLengthIsOptionalLastAndUsesCapturedInput) {
  initialize(0x401);
  prepare();
  auto R =
      Model->call("ZwQuerySystemInformation", {103, Buffer, 8, Buffer + 4});
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0u);
  EXPECT_EQ(get(Buffer + 4), 8u);
  prepare(0);
  R = Model->call("ZwQuerySystemInformation", {103, Buffer, 8, Buffer});
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0xc0000004u);
  EXPECT_EQ(get(Buffer), 8u);
  EXPECT_EQ(get(Buffer + 4), 0xa5a5a5a5u);
  prepare();
  R = Model->call("ZwQuerySystemInformation", {103, Buffer, 0x100000008ULL, 0});
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0u);
  EXPECT_EQ(get(Buffer + 4), 0x401u);
  EXPECT_EQ(get(Returned), 0xfeedbeefu);
}
TEST_F(KernelCodeIntegrity, ModeAndIRQLRemainExplicitWithoutChangingCaller) {
  initialize(0);
  prepare();
  ok(Model->setUserRequestContext(true, 0x3456));
  auto R = Model->call("NtQuerySystemInformation", {103, Buffer, 8, Returned});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("UserMode"), std::string::npos);
  EXPECT_EQ(query(8), 0u);
  R = Model->call("ExGetPreviousMode", {});
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 1u);
  prepare();
  R = Model->call("KfRaiseIrql", {2});
  ASSERT_TRUE(bool(R));
  R = Model->call("ZwQuerySystemInformation", {103, Buffer, 8, Returned});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("PASSIVE_LEVEL"),
            std::string::npos);
  EXPECT_EQ(get(Buffer + 4), 0xa5a5a5a5u);
}
TEST(KernelCodeIntegrityCaller, RealClientChecksLengthsMasksOverlapAndUnloads) {
  for (uint32_t Value : {0u, 0x401u, 0x200000u, UINT32_MAX})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
      for (bool Preemptive : {false, true}) {
        DriverOptions O;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.Contract = Contract;
        O.CodeIntegrityOptions = Value;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_code_integrity.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u);
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        EXPECT_EQ(R->Messages, std::vector<std::string>{"CI options=" +
                                                        std::to_string(Value)});
      }
}
} // namespace
} // namespace neverd::emulation
