//===- DarwinEntropyTests.cpp - Replay admission, copyout and lifetime
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinEntropy.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"
#include "os/darwin/kernel/DarwinSystem.h"

#include "neverd/emulation/AddressSpace.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
namespace {
class EntropyMemory : public GuestMemory {
  GuestMemory &Memory;

public:
  mutable unsigned Accesses = 0;
  unsigned Writes = 0, FailAccess = 0, FailWrite = 0;
  explicit EntropyMemory(GuestMemory &Memory) : Memory(Memory) {}
  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return failure("unexpected map");
  }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return failure("unexpected protect");
  }
  llvm::Expected<bool> canAccess(uint64_t A, uint64_t N,
                                 unsigned P) const override {
    if (++Accesses == FailAccess)
      return failure("entropy access transport");
    return Memory.canAccess(A, N, P);
  }
  llvm::Error read(uint64_t, llvm::MutableArrayRef<uint8_t>) override {
    return failure("unexpected read");
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    if (++Writes == FailWrite)
      return failure("entropy write transport");
    return Memory.write(A, B);
  }
};
class DarwinEntropyTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000, Output = Base + 17;
  uint64_t Page;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinSystemOptions> Options;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only test"};
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 2, Read | Write | UserAccessible)));
    fill();
  }
  void fill() {
    ASSERT_FALSE(
        bool(Space->write(Base, std::vector<uint8_t>(Page * 2, 0xa5))));
  }
  void configure(std::initializer_list<std::vector<uint8_t>> Reads) {
    Options.emplace();
    Options->EntropyReads = std::vector<std::vector<uint8_t>>(Reads);
  }
  std::vector<uint8_t> bytes() {
    std::vector<uint8_t> Out(Page * 2);
    EXPECT_FALSE(bool(Space->read(Base, Out)));
    return Out;
  }
  std::optional<ServiceResult> invoke(DarwinEntropy &Entropy, uint64_t Address,
                                      uint64_t Count) {
    Result.Diagnostic.clear();
    auto R = Entropy.handle(*Space, {0, 500, {Address, Count}, std::nullopt},
                            Result);
    EXPECT_TRUE(bool(R)) << (R ? "" : llvm::toString(R.takeError()));
    return R ? *R : std::nullopt;
  }
  void returned(const std::optional<ServiceResult> &R, uint64_t Value = 0) {
    ASSERT_TRUE(R);
    EXPECT_EQ(R->Value, Value);
    EXPECT_EQ(R->Error, Value != 0);
  }
};

TEST_P(DarwinEntropyTest, ZeroAndFullWidthInvalidLengthsNeedNoMemoryOrInput) {
  configure({{0x71}});
  DarwinEntropy Entropy(Options);
  EntropyMemory Memory(*Space);
  Memory.FailAccess = Memory.FailWrite = 1;
  for (auto Address : {uint64_t(0), uint64_t(1), Output, value::UserLimit,
                       UINT64_MAX, uint64_t(1) << 63}) {
    for (auto Count : {uint64_t(0), uint64_t(257), uint64_t(1) << 32,
                       (uint64_t(1) << 32) | 1, UINT64_MAX}) {
      auto R = Entropy.handle(Memory, {0, 500, {Address, Count}, std::nullopt},
                              Result);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      returned(*R, Count ? 22 : 0);
    }
  }
  EXPECT_EQ(Memory.Accesses, 0u);
  EXPECT_EQ(Memory.Writes, 0u);
  returned(invoke(Entropy, Output, 1));
  EXPECT_EQ(bytes()[Output - Base], 0x71);
  Options.reset();
  DarwinEntropy Missing(Options);
  returned(invoke(Missing, UINT64_MAX, 0));
  returned(invoke(Missing, UINT64_MAX, 257), 22);
}

TEST_P(DarwinEntropyTest, ExactRecordsPreserveBytesCanariesAndOwnerIsolation) {
  std::vector<uint8_t> First(256);
  for (size_t I = 0; I != First.size(); ++I)
    First[I] = uint8_t(I);
  configure({First, {0xff, 0, 0x80}});
  DarwinEntropy Entropy(Options), Independent(Options);
  const auto Cross = Base + Page - 127;
  returned(invoke(Entropy, Cross, 256));
  returned(invoke(Entropy, Output, 3));
  auto Expected = std::vector<uint8_t>(Page * 2, 0xa5);
  std::copy(First.begin(), First.end(), Expected.begin() + Page - 127);
  Expected[17] = 0xff;
  Expected[18] = 0;
  Expected[19] = 0x80;
  EXPECT_EQ(bytes(), Expected);
  EXPECT_FALSE(invoke(Entropy, Output, 3));
  EXPECT_EQ(Result.Diagnostic, diagnostic::EntropyExhausted);
  fill();
  returned(invoke(Independent, Cross, 256));
  Expected.assign(Page * 2, 0xa5);
  std::copy(First.begin(), First.end(), Expected.begin() + Page - 127);
  EXPECT_EQ(bytes(), Expected);
  EXPECT_EQ((*Options->EntropyReads)[0], First);
}

TEST_P(DarwinEntropyTest, AdmissionRefusalsPrecedeBadAddressAndPreserveRecord) {
  for (unsigned Configuration = 0; Configuration != 3; ++Configuration) {
    Options.reset();
    if (Configuration == 1)
      Options.emplace();
    if (Configuration == 2)
      configure({});
    DarwinEntropy Entropy(Options);
    EXPECT_FALSE(invoke(Entropy, UINT64_MAX, 1));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, Configuration == 2
                                     ? diagnostic::EntropyExhausted
                                     : diagnostic::EntropyObservation);
  }
  configure({{0x10, 0, 0x30}});
  DarwinEntropy Entropy(Options);
  EXPECT_FALSE(invoke(Entropy, UINT64_MAX, 2));
  EXPECT_EQ(Result.Diagnostic, diagnostic::EntropyLength);
  EXPECT_EQ(bytes(), std::vector<uint8_t>(Page * 2, 0xa5));
  returned(invoke(Entropy, Output, 3));
  auto Expected = std::vector<uint8_t>(Page * 2, 0xa5);
  Expected[17] = 0x10;
  Expected[18] = 0;
  Expected[19] = 0x30;
  EXPECT_EQ(bytes(), Expected);
}

TEST_P(DarwinEntropyTest, WholeFaultConsumesExactlyOneAdmittedRecord) {
  for (auto Address : {uint64_t(0), uint64_t(1), value::UserLimit, UINT64_MAX,
                       Base + Page * 2}) {
    fill();
    configure({{0x11, 0x22, 0x33}, {0x44}});
    DarwinEntropy Entropy(Options);
    returned(invoke(Entropy, Address, 3), 14);
    EXPECT_EQ(bytes(), std::vector<uint8_t>(Page * 2, 0xa5));
    returned(invoke(Entropy, Output, 1));
    EXPECT_EQ(bytes()[17], 0x44);
    EXPECT_FALSE(invoke(Entropy, Output, 1));
    EXPECT_EQ(Result.Diagnostic, diagnostic::EntropyExhausted);
  }
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  configure({{0x55}, {0x66}});
  DarwinEntropy Entropy(Options);
  returned(invoke(Entropy, Base + Page, 1), 14);
  returned(invoke(Entropy, Output, 1));
  EXPECT_EQ(bytes()[17], 0x66);
}

TEST_P(DarwinEntropyTest, PartialOutputRefusesBeforeBytesOrRecordAdvance) {
  configure({std::vector<uint8_t>(16, 0x31), std::vector<uint8_t>(16, 0x42)});
  DarwinEntropy Entropy(Options);
  EXPECT_FALSE(invoke(Entropy, Base + Page * 2 - 8, 16));
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Result.Diagnostic, diagnostic::EntropyPartialOutput);
  EXPECT_EQ(bytes(), std::vector<uint8_t>(Page * 2, 0xa5));
  returned(invoke(Entropy, Output, 16));
  auto Expected = std::vector<uint8_t>(Page * 2, 0xa5);
  std::fill_n(Expected.begin() + 17, 16, 0x31);
  EXPECT_EQ(bytes(), Expected);
}

TEST_P(DarwinEntropyTest, TransportErrorsNeverInventErrnoOrAdvanceReplay) {
  for (unsigned At = 0; At != 3; ++At) {
    fill();
    configure({std::vector<uint8_t>(16, 0x31), std::vector<uint8_t>(16, 0x42)});
    DarwinEntropy Entropy(Options);
    EntropyMemory Memory(*Space);
    if (At < 2)
      Memory.FailAccess = At + 1;
    else
      Memory.FailWrite = 1;
    auto R = Entropy.handle(
        Memory, {0, 500, {Base + Page - 8, 16}, std::nullopt}, Result);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()),
              At < 2 ? "entropy access transport" : "entropy write transport");
    EXPECT_EQ(bytes(), std::vector<uint8_t>(Page * 2, 0xa5));
    returned(invoke(Entropy, Output, 16));
    auto Expected = std::vector<uint8_t>(Page * 2, 0xa5);
    std::fill_n(Expected.begin() + 17, 16, 0x31);
    EXPECT_EQ(bytes(), Expected);
  }
}

class EntropyCPU final : public ExecutionBackend {
  std::shared_ptr<AddressSpace> Space;
  GuestArchitecture ISA;

public:
  std::map<CPURegister, RegisterValue> Registers;
  unsigned Writes = 0, FailRegisterWrite = 0;
  EntropyCPU(std::shared_ptr<AddressSpace> Space, GuestArchitecture ISA)
      : Space(std::move(Space)), ISA(ISA) {}
  GuestArchitecture architecture() const override { return ISA; }
  std::shared_ptr<AddressSpace> addressSpace() const override { return Space; }
  llvm::Error bindAddressSpace(std::shared_ptr<AddressSpace>) override {
    return failure("unexpected bind");
  }
  llvm::Expected<RegisterValue> readRegister(CPURegister R) override {
    return Registers[R];
  }
  llvm::Error writeRegister(CPURegister R,
                            const RegisterValue &Value) override {
    if (++Writes == FailRegisterWrite)
      return failure("entropy return register transport");
    Registers[R] = Value;
    return llvm::Error::success();
  }
  llvm::Error map(uint64_t A, uint64_t N, unsigned P) override {
    return Space->map(A, N, P);
  }
  llvm::Error protect(uint64_t A, uint64_t N, unsigned P) override {
    return Space->protect(A, N, P);
  }
  llvm::Expected<bool> canAccess(uint64_t A, uint64_t N,
                                 unsigned P) const override {
    return Space->canAccess(A, N, P);
  }
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) override {
    return Space->read(A, B);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    return Space->write(A, B);
  }
  llvm::Error fetch(uint64_t, llvm::MutableArrayRef<uint8_t>) override {
    return failure("unexpected fetch");
  }
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override {
    return failure("unexpected save");
  }
  llvm::Error saveContext(BackendContext &) override {
    return failure("unexpected save");
  }
  llvm::Error restoreContext(const BackendContext &) override {
    return failure("unexpected restore");
  }
  llvm::Error installHooks(BackendHooks) override {
    return failure("unexpected hooks");
  }
  bool timedOut() const override { return false; }
  void stop() override {}
  bool hasMemoryFault() const override { return false; }
  bool hasDeviceError() const override { return false; }
  std::optional<BackendFault> fault() const override { return std::nullopt; }
  std::optional<BackendFault> takeRecoverableFault() override {
    return std::nullopt;
  }
  bool executable(uint64_t) const override { return false; }
};

TEST_P(DarwinEntropyTest, ServiceHandoffKeepsEffectsOnReturnRegisterFailure) {
  using enum CPURegister;
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    const bool X64 = ISA == GuestArchitecture::X64;
    for (bool Fault : {false, true}) {
      const unsigned ReturnWrites = X64 ? (Fault ? 5 : 6) : 4;
      for (unsigned FailAt = 1; FailAt <= ReturnWrites; ++FailAt) {
        SCOPED_TRACE(FailAt);
        fill();
        ProcessOptions O;
        O.DarwinSystem.emplace();
        O.DarwinSystem->EntropyReads =
            std::vector<std::vector<uint8_t>>{{0x31}, {0x42}};
        EntropyCPU CPU(Space, ISA);
        DarwinMemory Memory(*Space, {Page, value::MinimumAddress, {}}, O);
        DarwinFiles Files(CPU, O.DarwinFiles);
        DarwinEntropy Entropy(O.DarwinSystem);
        const auto Number = X64 ? 0x12345678020001f4ULL : 0x12345678000001f4ULL;
        CPU.Registers[X64 ? X64AX : AArch64X16] = {Number, 0};
        CPU.Registers[X64 ? X64DI : AArch64X0] = {Fault ? 0 : Output, 0};
        CPU.Registers[X64 ? X64SI : AArch64X1] = {1, 0};
        CPU.Registers[X64 ? X64DX : AArch64X2] = {0x1122334455667788ULL, 0};
        const ServiceRequest Request{X64 ? ServiceRequestKind::X64Syscall
                                         : ServiceRequestKind::AArch64SVC,
                                     0x200000, 0x200004,
                                     uint16_t(X64 ? 0 : 0x80)};
        auto Event = readService(CPU, Request);
        ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
        EXPECT_EQ(Event->Number, Number);
        EXPECT_EQ(Event->Arguments[2], 0x1122334455667788ULL);
        auto R = handleService(CPU, Memory, Files, Entropy, *Event, O, Result);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        ASSERT_TRUE(*R);
        returned(*R, Fault ? 14 : 0);
        EXPECT_EQ((**R).Convention, ServiceConvention::BSD);
        EXPECT_EQ(bytes()[17], Fault ? 0xa5 : 0x31);
        CPU.FailRegisterWrite = FailAt;
        EXPECT_EQ(llvm::toString(returnService(CPU, Request, **R)),
                  "entropy return register transport");
        CPU.FailRegisterWrite = 0;
        // The next different record proves that neither return failure nor
        // whole EFAULT rolls the completed service back.
        Event->Arguments[0] = Output;
        auto Next =
            handleService(CPU, Memory, Files, Entropy, *Event, O, Result);
        ASSERT_TRUE(bool(Next)) << llvm::toString(Next.takeError());
        returned(*Next);
        EXPECT_EQ(bytes()[17], 0x42);
        ASSERT_FALSE(bool(returnService(CPU, Request, **Next)));
        EXPECT_EQ(CPU.Registers[X64 ? X64PC : AArch64PC][0], Request.NextPC);
        EXPECT_EQ(CPU.Registers[X64 ? X64DX : AArch64X1][0], 0u);
        Event->Number = X64 ? 0x010001f4 : uint64_t(uint32_t(-500));
        auto Mach =
            handleService(CPU, Memory, Files, Entropy, *Event, O, Result);
        ASSERT_TRUE(bool(Mach)) << llvm::toString(Mach.takeError());
        EXPECT_FALSE(*Mach);
        EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
      }
    }
  }
}

TEST(DarwinEntropyOptions, FiniteNonemptyRecordsShareTypedValidation) {
  DarwinSystemOptions O;
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.EntropyReads.emplace();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.EntropyReads->assign(256, std::vector<uint8_t>(256, 0));
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.EntropyReads->push_back({1});
  EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
            diagnostic::EntropyOption);
  O.EntropyReads->assign(1, {});
  EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
            diagnostic::EntropyOption);
  O.EntropyReads->assign(1, std::vector<uint8_t>(257, 0));
  EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
            diagnostic::EntropyOption);
}

INSTANTIATE_TEST_SUITE_P(OSPages, DarwinEntropyTest,
                         testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
