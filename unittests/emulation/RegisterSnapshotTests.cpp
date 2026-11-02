//===- RegisterSnapshotTests.cpp - Read-only CPU register observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify complete stopped-register inventories and all-or-error snapshots.
///
//===----------------------------------------------------------------------===//

#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include <set>
#include <tuple>

namespace neverd::emulation {
namespace {
class RegisterReader final : public ExecutionBackend {
  llvm::Error unexpected() const {
    ADD_FAILURE() << "register observation touched non-register state";
    return llvm::createStringError("unexpected operation");
  }

public:
  GuestArchitecture ISA;
  std::optional<CPURegister> Fail, Oversize;
  mutable std::vector<CPURegister> Reads;
  bool SavedSupported = false;
  explicit RegisterReader(GuestArchitecture ISA) : ISA(ISA) {}
  GuestArchitecture architecture() const override { return ISA; }
  std::shared_ptr<AddressSpace> addressSpace() const override { return {}; }
  llvm::Expected<RegisterValue> readRegister(CPURegister R) override {
    return value(R);
  }
  std::unique_ptr<BackendContext> saved() {
    return makeContext(std::make_unique<BackendContext::Storage>());
  }
  llvm::Expected<RegisterValue> value(CPURegister R) const {
    Reads.push_back(R);
    if (R == Fail)
      return llvm::createStringError("authored register read failure");
    if (R == Oversize)
      return RegisterValue{UINT64_MAX, UINT64_MAX};
    return RegisterValue{unsigned(R), registerWidth(R) > 64 ? 0xabcd : 0u};
  }
  llvm::Error bindAddressSpace(std::shared_ptr<AddressSpace>) override {
    return unexpected();
  }
  llvm::Error writeRegister(CPURegister, const RegisterValue &) override {
    return unexpected();
  }
  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return unexpected();
  }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return unexpected();
  }
  llvm::Error read(uint64_t, llvm::MutableArrayRef<uint8_t>) override {
    return unexpected();
  }
  llvm::Error write(uint64_t, llvm::ArrayRef<uint8_t>) override {
    return unexpected();
  }
  llvm::Error fetch(uint64_t, llvm::MutableArrayRef<uint8_t>) override {
    return unexpected();
  }
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override {
    return unexpected();
  }
  llvm::Error saveContext(BackendContext &) override { return unexpected(); }
  llvm::Error restoreContext(const BackendContext &) override {
    return unexpected();
  }
  llvm::Error installHooks(BackendHooks) override { return unexpected(); }
  llvm::Expected<ExecutionExit> runUntilExit(uint64_t, uint64_t) override {
    return unexpected();
  }
  bool timedOut() const override { return false; }
  void stop() override { llvm::consumeError(unexpected()); }
  bool hasMemoryFault() const override { return false; }
  bool hasDeviceError() const override { return false; }
  std::optional<BackendFault> fault() const override { return {}; }
  std::optional<BackendFault> takeRecoverableFault() override {
    llvm::consumeError(unexpected());
    return {};
  }
  bool executable(uint64_t) const override { return false; }

protected:
  llvm::Expected<RegisterValue>
  readContextRegister(const BackendContext &Context,
                      CPURegister R) const override {
    if (!SavedSupported)
      return ExecutionBackend::readContextRegister(Context, R);
    return value(R);
  }
};
TEST(RegisterSnapshot,
     CompleteInventoryHasUniqueStableNamesAndFullWidthValues) {
  for (auto ISA : {GuestArchitecture::AArch64, GuestArchitecture::X64}) {
    RegisterReader CPU(ISA);
    auto Snapshot = llvm::cantFail(CPU.readRegisters());
    std::set<std::string> Names;
    for (const auto &R : Snapshot) {
      ASSERT_NE(registerName(R.Register), nullptr);
      EXPECT_TRUE(Names.insert(registerName(R.Register)).second);
      EXPECT_TRUE(registerMatches(R.Register, ISA));
      EXPECT_EQ(R.Value[0], unsigned(R.Register));
      EXPECT_EQ(R.Value[1], registerWidth(R.Register) > 64 ? 0xabcdu : 0u);
    }
    if (ISA == GuestArchitecture::AArch64) {
      EXPECT_EQ(Names.size(), 71u);
      for (unsigned I = 0; I < 31; ++I)
        EXPECT_TRUE(Names.contains("X" + std::to_string(I)));
      for (unsigned I = 0; I < 32; ++I)
        EXPECT_TRUE(Names.contains("V" + std::to_string(I)));
      for (auto Name : {"SP", "PC", "NZCV", "TPIDR_EL0", "TPIDRRO_EL0",
                        "TPIDR_EL1", "FPCR", "FPSR"})
        EXPECT_TRUE(Names.contains(Name));
    } else {
      for (unsigned I = 0; I < 8; ++I)
        EXPECT_TRUE(Names.contains("FP" + std::to_string(I)));
      EXPECT_TRUE(Names.contains("MXCSR"));
    }
  }
  EXPECT_EQ(registerName(CPURegister::Invalid), nullptr);
}
TEST(RegisterSnapshot, EveryReadFailureRejectsTheWholeSnapshotWithoutMutation) {
  for (auto ISA : {GuestArchitecture::AArch64, GuestArchitecture::X64}) {
    const auto Registers = architectureRegisters(ISA);
    for (size_t I = 0; I < Registers.size(); ++I) {
      RegisterReader CPU(ISA);
      CPU.Fail = Registers[I];
      auto Snapshot = CPU.readRegisters();
      ASSERT_FALSE(bool(Snapshot));
      EXPECT_EQ(llvm::toString(Snapshot.takeError()),
                "authored register read failure");
      EXPECT_EQ(CPU.Reads.size(), I + 1);
    }
    RegisterReader CPU(ISA);
    CPU.Oversize = Registers.front();
    auto Snapshot = CPU.readRegisters();
    ASSERT_FALSE(bool(Snapshot));
    llvm::consumeError(Snapshot.takeError());
    EXPECT_EQ(CPU.Reads.size(), 1u);
  }
}
TEST(RegisterSnapshot, SavedContextsUseTheSameAtomicWidthCheckedInventory) {
  for (auto ISA : {GuestArchitecture::AArch64, GuestArchitecture::X64}) {
    RegisterReader CPU(ISA);
    auto Context = CPU.saved();
    auto Unsupported = CPU.readRegisters(*Context);
    ASSERT_FALSE(bool(Unsupported));
    EXPECT_EQ(llvm::toString(Unsupported.takeError()),
              "Saved CPU register observation is unsupported by this backend");
    EXPECT_TRUE(CPU.Reads.empty());
    CPU.SavedSupported = true;
    const auto Expected = llvm::cantFail(CPU.readRegisters());
    EXPECT_EQ(llvm::cantFail(CPU.readRegisters(*Context)), Expected);
    for (const auto &R : Expected) {
      CPU.Reads.clear();
      CPU.Fail = R.Register;
      auto Failed = CPU.readRegisters(*Context);
      ASSERT_FALSE(bool(Failed));
      EXPECT_EQ(llvm::toString(Failed.takeError()),
                "authored register read failure");
      EXPECT_EQ(CPU.Reads.back(), R.Register);
    }
    CPU.Fail.reset();
    CPU.Oversize = Expected.front().Register;
    auto Invalid = CPU.readRegisters(*Context);
    ASSERT_FALSE(bool(Invalid));
    llvm::consumeError(Invalid.takeError());
  }
}
class LiveRegisterSnapshot
    : public testing::TestWithParam<
          std::tuple<GuestArchitecture, ExecutionBackendKind>> {};
TEST_P(LiveRegisterSnapshot, PreservesEveryModeledRegisterAcrossRepeatedReads) {
  const auto [ISA, Backend] = GetParam();
  auto B = createExecutionBackend(Backend,
                                  ISA == GuestArchitecture::AArch64
                                      ? ExecutionContract::CheckedUserAArch64
                                      : ExecutionContract::CheckedUserX64,
                                  1024 * 1024, ISA);
  if (!B) {
    auto E = B.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Message = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(Backend, ISA))
      GTEST_SKIP() << Message;
    FAIL() << Message;
  }
  std::vector<RegisterSnapshot> Expected;
  if (ISA == GuestArchitecture::AArch64)
    for (CPURegister R : architectureRegisters(ISA)) {
      RegisterValue Value{0xfedcba9876540000ull + unsigned(R), 0};
      if (registerWidth(R) == 128)
        Value[1] = 0x0123456789abcdefull - unsigned(R);
      if (R == CPURegister::AArch64PC || R == CPURegister::AArch64SP)
        Value[0] = 0x21000000 + unsigned(R) * 16;
      if (R == CPURegister::AArch64NZCV)
        Value[0] = 0xa0000000;
      if (R == CPURegister::AArch64FPCR)
        Value[0] = 0x00400000;
      if (R == CPURegister::AArch64FPSR)
        Value[0] = 0x0800001f;
      ASSERT_EQ(llvm::toString(B->CPU->writeRegister(R, Value)), "");
      Expected.push_back({R, Value});
    }
  const auto First = llvm::cantFail(B->CPU->readRegisters());
  if (!Expected.empty())
    EXPECT_EQ(First, Expected);
  ASSERT_EQ(First.size(), architectureRegisters(ISA).size());
  for (const auto &R : First)
    EXPECT_EQ(llvm::cantFail(B->CPU->readRegister(R.Register)), R.Value);
  EXPECT_EQ(llvm::cantFail(B->CPU->readRegisters()), First);
  auto Context = llvm::cantFail(B->CPU->saveContext());
  EXPECT_EQ(llvm::cantFail(B->CPU->readRegisters(*Context)), First);
}
INSTANTIATE_TEST_SUITE_P(
    Backends, LiveRegisterSnapshot,
    testing::Combine(
        testing::Values(GuestArchitecture::AArch64, GuestArchitecture::X64),
        testing::Values(ExecutionBackendKind::Unicorn,
                        ExecutionBackendKind::HVF, ExecutionBackendKind::KVM,
                        ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
