//===- DarwinLibSystemTests.cpp - C call and kernel argument boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file C ABI normalization, defined vararg bytes and failure atomicity.
#include "DarwinFileTestData.h"
#include "DarwinLibSystemFileTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinEntropy.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"
#include "os/darwin/process/DarwinCallbacks.h"
#include "os/darwin/process/DarwinLibSystem.h"
#include "os/darwin/process/DarwinTLS.h"

namespace neverd::emulation::darwin_model {
namespace {
class LibSystemCPU final : public ExecutionBackend {
  std::shared_ptr<AddressSpace> Space;
  GuestArchitecture ISA;

public:
  std::map<CPURegister, RegisterValue> Registers;
  uint64_t ModeAddress = 0;
  bool FailMode = false;
  unsigned ModeReads = 0;
  std::optional<CPURegister> FailedRegister;
  LibSystemCPU(std::shared_ptr<AddressSpace> Space, GuestArchitecture ISA)
      : Space(std::move(Space)), ISA(ISA) {}
  GuestArchitecture architecture() const override { return ISA; }
  std::shared_ptr<AddressSpace> addressSpace() const override { return Space; }
  llvm::Error bindAddressSpace(std::shared_ptr<AddressSpace>) override {
    return failure("unexpected bind");
  }
  llvm::Expected<RegisterValue> readRegister(CPURegister R) override {
    if (FailedRegister == R)
      return failure("libSystem argument transport");
    if (ISA == GuestArchitecture::X64 && R == CPURegister::X64DX) {
      ++ModeReads;
      if (FailMode)
        return failure("open mode transport");
    }
    return Registers[R];
  }
  llvm::Error writeRegister(CPURegister R, const RegisterValue &V) override {
    Registers[R] = V;
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
    if (ISA == GuestArchitecture::AArch64 && A == ModeAddress) {
      ++ModeReads;
      if (FailMode)
        return failure("open mode transport");
      if (B.size() != 4)
        return failure("open read undefined variadic padding");
    }
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

class DarwinLibSystemTest : public testing::TestWithParam<bool> {
protected:
  static constexpr uint64_t Data = 0x10000, Code = 0x20000, Stack = 0x30000;
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<LibSystemCPU> CPU;
  std::unique_ptr<DarwinMemory> Memory;
  std::unique_ptr<DarwinFiles> Files;
  ProcessOptions Options;
  ProcessImage Image{};
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "C boundary"};
  uint64_t SP = 0, Errno = 0;
  void SetUp() override {
    const auto ISA =
        GetParam() ? GuestArchitecture::X64 : GuestArchitecture::AArch64;
    const uint64_t Page = GetParam() ? 4096 : 16384;
    auto Physical = PhysicalMemory::create(1 << 20);
    ASSERT_TRUE(bool(Physical));
    auto Created = AddressSpace::create(*Physical, 1 << 20);
    ASSERT_TRUE(bool(Created));
    Space = *Created;
    Image.Architecture = ISA;
    Image.Memory = {Page,
                    0x100000000ULL,
                    {{value::LibSystemGate, Page, 5},
                     {value::LibSystemGate + Page, Page, 3}},
                    {{value::LibSystemGate, Page, 5},
                     {value::LibSystemGate + Page, Page, 3}}};
    Image.Imports[value::LibSystemGate] = LibSystemGateKind::Open;
    llvm::cantFail(prepareLibSystem(*Space, Image));
    Errno = value::LibSystemGate + Page;
    llvm::cantFail(Space->writeInteger(Errno, 71, 4));
    llvm::cantFail(Space->map(Data, Page, Read | Write | UserAccessible));
    llvm::cantFail(Space->map(Code, Page, Read | Execute | UserAccessible));
    llvm::cantFail(Space->map(Stack, Page, Read | Write | UserAccessible));
    const uint8_t Path[] = {'/', 'c', 'r', 'e', 'a', 't', 'e', 'd', 0};
    llvm::cantFail(Space->write(Data, Path));
    Options.DarwinFiles = darwin_test::libSystemFileOptions();
    Options.DarwinFiles->Metadata["/"] = darwin_test::creationParentMetadata();
    Options.DarwinFiles->CreationPolicy = darwin_test::CreationPolicy;
    Memory = std::make_unique<DarwinMemory>(*Space, Image.Memory, Options);
    Files = std::make_unique<DarwinFiles>(*Space, Options.DarwinFiles);
    CPU = std::make_unique<LibSystemCPU>(Space, ISA);
    SP = Stack + (GetParam() ? 8 : 0);
    auto ABI = llvm::cantFail(
        IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                   : IntegerCallingConvention::AAPCS64));
    CPU->Registers[ABI.info().StackPointer] = {SP, 0};
    CPU->Registers[ABI.info().Arguments[0]] = {Data, 0};
    CPU->Registers[ABI.info().Arguments[1]] = {0x1122334400000a02ULL, 0};
    CPU->Registers[ABI.info().Arguments[2]] = {
        GetParam() ? 0xaabbccdd123401a1ULL : 0x5566778899abcdefULL, 0};
    if (GetParam())
      llvm::cantFail(Space->writeInteger(SP, Code, 8));
    else {
      CPU->Registers[ABI.info().Link] = {Code, 0};
      CPU->ModeAddress = SP;
      llvm::cantFail(Space->writeInteger(SP, 0xdeadbeef123401a1ULL, 8));
    }
    Result.Architecture = ISA;
  }
  void setArguments(const std::array<uint64_t, 6> &Arguments) {
    auto ABI = llvm::cantFail(
        IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                   : IntegerCallingConvention::AAPCS64));
    CPU->Registers[ABI.info().StackPointer] = {SP, 0};
    for (unsigned I = 0; I != Arguments.size(); ++I)
      CPU->Registers[ABI.info().Arguments[I]] = {Arguments[I], 0};
  }
  llvm::Expected<bool> invoke(LibSystemGateKind Kind = LibSystemGateKind::Open,
                              DarwinCallbacks *Callbacks = nullptr) {
    DarwinEntropy Entropy(Options.DarwinSystem);
    ServiceRequest Request{GetParam() ? ServiceRequestKind::X64Syscall
                                      : ServiceRequestKind::AArch64SVC,
                           value::LibSystemGate, 0,
                           uint16_t(value::LibSystemSVC)};
    return invokeLibSystem(*CPU, *Memory, *Files, Entropy, Image, Kind, Request,
                           Options, Result, Callbacks);
  }
  InitialStack callbackStack() {
    Options.StackSize = Image.Memory.PageSize;
    const uint64_t Base = value::StackTop - Options.StackSize;
    llvm::cantFail(
        Space->map(Base, Options.StackSize, Read | Write | UserAccessible));
    Image.MainEntry = true;
    Image.Plan.Entry = Code;
    Image.HasTLSCallbacks = true;
    Image.ExecutableSpans = {{Code, 16}};
    return {value::StackTop - 256, 2, Data, Data + 16, Data + 32, Data + 48};
  }
  IntegerABI callbackABI() {
    return llvm::cantFail(
        IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                   : IntegerCallingConvention::AAPCS64));
  }
  void balanceCallback() {
    const auto ABI = callbackABI();
    CPU->Registers[ABI.info().StackPointer][0] +=
        ABI.info().returnAddressSize();
  }
  void expectAbsent() {
    EXPECT_TRUE(Result.NativeCalls.empty());
    EXPECT_TRUE(Result.Services.empty());
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
    auto R = Files->handle(ServiceKind::Open, {0, 0, {Data, 0}, std::nullopt},
                           Result);
    ASSERT_TRUE(bool(R));
    ASSERT_TRUE(bool(*R));
    EXPECT_TRUE((**R).Error);
    EXPECT_EQ((**R).Value, 2u);
  }
};

TEST_P(DarwinLibSystemTest, OpenSeparatesModeCarrierFromKernelModeAndMetadata) {
  auto ABI = llvm::cantFail(
      IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                 : IntegerCallingConvention::AAPCS64));
  auto Arguments =
      readLibSystemArguments(*CPU, ABI, SP, LibSystemGateKind::Open);
  ASSERT_TRUE(bool(Arguments)) << llvm::toString(Arguments.takeError());
  EXPECT_EQ(Arguments->Count, 3u);
  const uint64_t Carrier = GetParam() ? 0xaabbccdd123401a1ULL : 0x123401a1ULL;
  EXPECT_EQ(Arguments->Carriers[2], Carrier);
  EXPECT_EQ(Arguments->Kernel[2], 0641u);
  EXPECT_EQ(Arguments->Kernel[1], 0x1122334400000a02ULL);
  EXPECT_EQ(CPU->ModeReads, 1u);
  CPU->ModeReads = 0;
  auto R = invoke();
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_TRUE(*R);
  EXPECT_EQ(CPU->ModeReads, 1u);
  ASSERT_EQ(Result.NativeCalls.size(), 1u);
  EXPECT_EQ(Result.NativeCalls[0].Arguments[2], Carrier);
  EXPECT_EQ(Result.NativeCalls[0].ArgumentCount, 3u);
  EXPECT_EQ(Result.NativeCalls[0].Result, 3u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
  auto Stat = Files->handle(ServiceKind::Fstat64,
                            {0, 0, {3, Data + 256}, std::nullopt}, Result);
  ASSERT_TRUE(bool(Stat));
  ASSERT_TRUE(bool(*Stat));
  EXPECT_FALSE((**Stat).Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Data + 260, 2)), 0100640u);
}
TEST_P(DarwinLibSystemTest, OpenWithoutCreateDoesNotReadOptionalStorage) {
  auto ABI = llvm::cantFail(
      IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                 : IntegerCallingConvention::AAPCS64));
  CPU->Registers[ABI.info().Arguments[1]] = {0x0000020000000000ULL, 0};
  CPU->FailMode = true;
  if (!GetParam())
    llvm::cantFail(Space->unmap(Stack, Image.Memory.PageSize));
  auto Arguments =
      readLibSystemArguments(*CPU, ABI, SP, LibSystemGateKind::Open);
  ASSERT_TRUE(bool(Arguments)) << llvm::toString(Arguments.takeError());
  EXPECT_EQ(Arguments->Count, 2u);
  EXPECT_EQ(Arguments->Kernel[2], 0u);
  auto R = invoke();
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_TRUE(*R);
  EXPECT_EQ(CPU->ModeReads, 0u);
  ASSERT_EQ(Result.NativeCalls.size(), 1u);
  EXPECT_EQ(Result.NativeCalls[0].ArgumentCount, 2u);
  EXPECT_EQ(Result.NativeCalls[0].Result, UINT64_MAX);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 2u);
}
TEST_P(DarwinLibSystemTest, OptionalModeTransportFailureCannotPublishEffects) {
  CPU->FailMode = true;
  auto R = invoke();
  ASSERT_FALSE(bool(R));
  EXPECT_EQ(llvm::toString(R.takeError()), "open mode transport");
  expectAbsent();
  CPU->FailMode = false;
  auto Retry = invoke();
  ASSERT_TRUE(bool(Retry)) << llvm::toString(Retry.takeError());
  ASSERT_TRUE(*Retry);
  ASSERT_EQ(Result.NativeCalls.size(), 1u);
  EXPECT_EQ(Result.NativeCalls.front().Result, 3u);
}
TEST_P(DarwinLibSystemTest, MissingStackBytesKeepTheirDistinctCallBoundary) {
  llvm::cantFail(Space->unmap(Stack, Image.Memory.PageSize));
  auto R = invoke();
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  if (!GetParam())
    expectAbsent(); // Required ARM64 mode was unavailable before call entry.
  else {
    ASSERT_EQ(Result.NativeCalls.size(), 1u); // Required return address failed.
    EXPECT_FALSE(Result.NativeCalls[0].Result);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
  }
}
TEST_P(DarwinLibSystemTest,
       MemoryArgumentsRetainCarriersAndNormalizeCIntegers) {
  auto ABI = llvm::cantFail(
      IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                 : IntegerCallingConvention::AAPCS64));
  const std::array<uint64_t, 6> Original = {
      0x1000000000ULL,       0x100004000ULL,        0xabcdef1200000003ULL,
      0x1122334400001002ULL, 0x55667788ffffffffULL, 0x100000000ULL};
  setArguments(Original);
  auto A = readLibSystemArguments(*CPU, ABI, SP, LibSystemGateKind::Mmap);
  ASSERT_TRUE(bool(A)) << llvm::toString(A.takeError());
  EXPECT_EQ(A->Carriers, Original);
  EXPECT_EQ(A->Count, 6u);
  EXPECT_FALSE(A->Error);
  EXPECT_EQ(A->Kernel[0], Original[0]);
  EXPECT_EQ(A->Kernel[1], Original[1]);
  EXPECT_EQ(A->Kernel[2], 3u);
  EXPECT_EQ(A->Kernel[3], 0x41002u);
  EXPECT_EQ(A->Kernel[4], UINT32_MAX);
  EXPECT_EQ(A->Kernel[5], Original[5]);
  auto P = readLibSystemArguments(*CPU, ABI, SP, LibSystemGateKind::Mprotect);
  ASSERT_TRUE(bool(P));
  EXPECT_EQ(P->Count, 3u);
  EXPECT_EQ(P->Carriers[2], Original[2]);
  EXPECT_EQ(P->Kernel[2], 3u);
  auto U = readLibSystemArguments(*CPU, ABI, SP, LibSystemGateKind::Munmap);
  ASSERT_TRUE(bool(U));
  EXPECT_EQ(U->Count, 2u);
  EXPECT_EQ(U->Kernel[1], Original[1]);
}
TEST_P(DarwinLibSystemTest, MemoryPreflightReturnsErrnoBeforeKernelEffects) {
  const auto Before = Space->mappedBytes();
  for (const std::array<uint64_t, 6> Args :
       {std::array<uint64_t, 6>{Errno, 0, 0x7fffffff, 0x1002, 31, 1},
        std::array<uint64_t, 6>{Errno, Image.Memory.PageSize, 0x7fffffff,
                                0x123400001000ULL, UINT64_MAX, 0}}) {
    setArguments(Args);
    auto R = invoke(LibSystemGateKind::Mmap);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_TRUE(*R);
    ASSERT_FALSE(Result.NativeCalls.empty());
    EXPECT_EQ(Result.NativeCalls.back().Result, UINT64_MAX);
    EXPECT_EQ(Result.NativeCalls.back().ArgumentCount, 6u);
    for (unsigned I = 0; I != Args.size(); ++I)
      EXPECT_EQ(Result.NativeCalls.back().Arguments[I], Args[I]);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 22u);
    EXPECT_EQ(Space->mappedBytes(), Before);
    EXPECT_TRUE(Result.Services.empty());
  }
}
TEST_P(DarwinLibSystemTest, MemoryCallsShareKernelMappingsAndReservations) {
  const auto Page = Image.Memory.PageSize;
  setArguments({0, Page, 0xabcdef1200000003ULL, 0x1122334400001002ULL,
                0x55667788ffffffffULL, 0});
  auto Mapped = invoke(LibSystemGateKind::Mmap);
  ASSERT_TRUE(bool(Mapped)) << llvm::toString(Mapped.takeError());
  ASSERT_TRUE(*Mapped);
  ASSERT_TRUE(Result.NativeCalls.back().Result);
  const auto Address = *Result.NativeCalls.back().Result;
  ASSERT_NE(Address, UINT64_MAX);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Address, 8)), 0u);
  llvm::cantFail(Space->writeInteger(Address, 0x123456789abcdef0ULL, 8));
  setArguments({Address, Page, 0x1122334400000001ULL, 0, 0, 0});
  auto Protected = invoke(LibSystemGateKind::Mprotect);
  ASSERT_TRUE(bool(Protected));
  ASSERT_TRUE(*Protected);
  EXPECT_EQ(Result.NativeCalls.back().Result, 0u);
  EXPECT_TRUE(llvm::cantFail(Space->canAccess(Address, Page, Read)));
  EXPECT_FALSE(llvm::cantFail(Space->canAccess(Address, Page, Write)));
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Address, 8)),
            0x123456789abcdef0ULL);
  setArguments({Address, Page, 0, 0, 0, 0});
  auto Unmapped = invoke(LibSystemGateKind::Munmap);
  ASSERT_TRUE(bool(Unmapped));
  ASSERT_TRUE(*Unmapped);
  EXPECT_EQ(Result.NativeCalls.back().Result, 0u);
  EXPECT_FALSE(llvm::cantFail(Space->canAccess(Address, 1, Read)));
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
  for (auto Reserved : {value::LibSystemGate, Errno}) {
    setArguments({Reserved, Page, 0, 0, 0, 0});
    auto Refused = invoke(LibSystemGateKind::Munmap);
    ASSERT_TRUE(bool(Refused));
    ASSERT_FALSE(*Refused);
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_FALSE(Result.NativeCalls.back().Result);
    EXPECT_TRUE(llvm::cantFail(Space->canAccess(Reserved, Page, Read)));
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
  }
  EXPECT_TRUE(Result.Services.empty());
}
TEST_P(DarwinLibSystemTest, MemoryArgumentFaultPrecedesEvenEarlyCError) {
  auto ABI = llvm::cantFail(
      IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                 : IntegerCallingConvention::AAPCS64));
  setArguments({0, 0, 0, 0, 0, 0});
  CPU->FailedRegister = ABI.info().Arguments[5];
  const auto Before = Space->mappedBytes();
  auto R = invoke(LibSystemGateKind::Mmap);
  ASSERT_FALSE(bool(R));
  EXPECT_EQ(llvm::toString(R.takeError()), "libSystem argument transport");
  EXPECT_TRUE(Result.NativeCalls.empty());
  EXPECT_TRUE(Result.Services.empty());
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
  EXPECT_EQ(Space->mappedBytes(), Before);
}
TEST_P(DarwinLibSystemTest, TLVPreservesRegistersAndCopiesLiveTemplatesOnce) {
  Image.TLS = {{Data + 256, 32, Data + 512}, {Data + 320, 32, Data + 1024}};
  Image.TLSDescriptors = {{Data, {0, 8}}, {Data + 32, {1, 8}}};
  const uint64_t Gate =
      value::LibSystemGate +
      unsigned(LibSystemGateKind::TLVBootstrap) * value::LibSystemGateStride;
  for (unsigned I = 0; I != 2; ++I) {
    const auto D = Data + I * 32;
    llvm::cantFail(Space->writeInteger(D, Gate, 8));
    llvm::cantFail(Space->writeInteger(D + 8, I + 1, 8));
    llvm::cantFail(Space->writeInteger(D + 16, 8, 8));
    llvm::cantFail(Space->writeInteger(Image.TLS[I].Template + 8, 71 + I, 8));
  }
  for (unsigned R = 0; R < unsigned(CPURegister::Invalid); ++R) {
    const auto Register = CPURegister(R);
    if (registerMatches(Register, Image.Architecture) &&
        !CPU->Registers.contains(Register))
      CPU->Registers[Register] = {0x1122334400000000ULL + R,
                                  registerWidth(Register) > 64 ? 0x88776655
                                                               : 0};
  }
  auto ABI = llvm::cantFail(
      IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                 : IntegerCallingConvention::AAPCS64));
  auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
  DarwinTLS Runtime(Image);
  ServiceRequest Request{GetParam() ? ServiceRequestKind::X64Syscall
                                    : ServiceRequestKind::AArch64SVC,
                         Gate, Gate + (GetParam() ? 2 : 4),
                         value::LibSystemSVC};
  for (unsigned I : {0u, 0u, 1u}) {
    CPU->Registers[ABI.info().StackPointer] = {SP, 0};
    CPU->Registers[ABI.info().Arguments[0]] = {Data + I * 32, 0};
    const auto Before = CPU->Registers;
    auto R = Runtime.invoke(*CPU, *Budget, Request, Result);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_TRUE(*R);
    const auto Value = Image.TLS[I].Storage + 8;
    EXPECT_EQ(CPU->Registers[ABI.info().Result][0], Value);
    for (const auto &[Register, Bits] : Before)
      if (Register != ABI.info().Result &&
          Register != ABI.info().StackPointer &&
          Register != CPURegister::X64PC && Register != CPURegister::AArch64PC)
        EXPECT_EQ(CPU->Registers[Register], Bits) << unsigned(Register);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Value, 8)),
              I                                ? 72u
              : Result.NativeCalls.size() == 1 ? 71u
                                               : 99u);
    llvm::cantFail(Space->writeInteger(Value, 99, 8));
    llvm::cantFail(Space->writeInteger(Image.TLS[I].Template + 8, 13, 8));
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
  }
  EXPECT_EQ(Result.NativeCalls.size(), 3u);
  EXPECT_TRUE(Result.Services.empty());
}
TEST_P(DarwinLibSystemTest, TLVRejectsTamperingAndInvalidFramesBeforeCopying) {
  Image.TLS = {{Data + 256, 32, Data + 512}};
  Image.TLSDescriptors = {{Data, {0, 8}}};
  const uint64_t Gate =
      value::LibSystemGate +
      unsigned(LibSystemGateKind::TLVBootstrap) * value::LibSystemGateStride;
  auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
  auto ABI = llvm::cantFail(
      IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                 : IntegerCallingConvention::AAPCS64));
  ServiceRequest Request{GetParam() ? ServiceRequestKind::X64Syscall
                                    : ServiceRequestKind::AArch64SVC,
                         Gate, Gate + (GetParam() ? 2 : 4),
                         value::LibSystemSVC};
  for (unsigned Field = 0; Field != 5; ++Field) {
    DarwinTLS Runtime(Image);
    llvm::cantFail(Space->writeInteger(Data, Gate, 8));
    llvm::cantFail(Space->writeInteger(Data + 8, 1, 8));
    llvm::cantFail(Space->writeInteger(Data + 16, 8, 8));
    llvm::cantFail(Space->writeInteger(Data + 512 + 8, 0xdead, 8));
    CPU->Registers[ABI.info().Arguments[0]] = {Data, 0};
    if (Field < 3)
      llvm::cantFail(Space->writeInteger(Data + Field * 8, UINT64_MAX, 8));
    else if (Field == 3)
      CPU->Registers[ABI.info().Arguments[0]][0] = Data + 1;
    else if (GetParam())
      llvm::cantFail(Space->writeInteger(SP, Data, 8));
    else
      CPU->Registers[ABI.info().Link][0] = Data;
    auto R = Runtime.invoke(*CPU, *Budget, Request, Result);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Data + 512 + 8, 8)), 0xdeadu);
    EXPECT_TRUE(Result.NativeCalls.empty());
  }
}
TEST_P(DarwinLibSystemTest,
       CallbackRegistrationIsVoidAndIgnoresTheCompilerDSOCarrier) {
  auto Stack = callbackStack();
  DarwinCallbacks Callbacks(Image, Options, Stack);
  const auto ABI = callbackABI();
  setArguments({Code + 4, UINT64_MAX, 0x12345678, 0, 0, 0});
  CPU->Registers[ABI.info().Result] = {
      GetParam() ? 0xaabbccdd11223344ULL : Code + 4, 0};
  const auto Before = CPU->Registers;
  CPU->FailedRegister = ABI.info().Arguments[2];
  auto R = invoke(LibSystemGateKind::TLVAtExit, &Callbacks);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_TRUE(*R);
  for (const auto &[Register, Value] : Before)
    if (Register != ABI.info().StackPointer &&
        Register != (GetParam() ? CPURegister::X64PC : CPURegister::AArch64PC))
      EXPECT_EQ(CPU->Registers[Register], Value);
  ASSERT_EQ(Result.NativeCalls.size(), 1u);
  EXPECT_EQ(Result.NativeCalls[0].Name, "__tlv_atexit");
  EXPECT_EQ(Result.NativeCalls[0].ArgumentCount, 2u);
  EXPECT_EQ(Result.NativeCalls[0].Arguments[1], UINT64_MAX);
  EXPECT_FALSE(Result.NativeCalls[0].Result);
  EXPECT_TRUE(Result.NativeCalls[0].ReturnAddress);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
  EXPECT_TRUE(Result.Services.empty());
}
TEST_P(DarwinLibSystemTest,
       CallbackLIFOIncludesNestedRegistrationsBeforeOlderEntries) {
  auto Stack = callbackStack();
  DarwinCallbacks Callbacks(Image, Options, Stack);
  const auto ABI = callbackABI();
  ASSERT_TRUE(bool(Callbacks.start(*CPU)));
  llvm::cantFail(Callbacks.addTLS(*CPU, Code + 4, 11));
  llvm::cantFail(Callbacks.addTLS(*CPU, Code + 8, 22));
  balanceCallback(); // Return from main, retaining its full scalar value
                     // privately.
  CPU->Registers[ABI.info().Result] = {0x123, 0};
  auto Continue = Callbacks.returned(*CPU, value::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue)) << llvm::toString(Continue.takeError());
  ASSERT_TRUE(*Continue);
  EXPECT_EQ(Result.PC, Code + 8);
  EXPECT_EQ(llvm::cantFail(ABI.readArgument(
                *CPU, CPU->Registers[ABI.info().StackPointer][0], 0)),
            22u);
  EXPECT_FALSE(Result.ExitStatus);
  EXPECT_FALSE(Result.ReturnValue);
  llvm::cantFail(Callbacks.addTLS(*CPU, Code + 12, UINT64_MAX));
  for (uint64_t Expected : {Code + 12, Code + 4}) {
    balanceCallback();
    Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
    ASSERT_TRUE(bool(Continue)) << llvm::toString(Continue.takeError());
    ASSERT_TRUE(*Continue);
    EXPECT_EQ(Result.PC, Expected);
    EXPECT_EQ(CPU->Registers[ABI.info().Arguments[0]][0],
              Expected == Code + 12 ? UINT64_MAX : 11u);
  }
  balanceCallback();
  Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue)) << llvm::toString(Continue.takeError());
  EXPECT_FALSE(*Continue);
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited);
  EXPECT_EQ(Result.ExitStatus, 0x23);
  EXPECT_EQ(Result.ReturnValue, 0x123u);
}
TEST_P(DarwinLibSystemTest,
       CallbackCapacityCountsNullsAndAllowsAnInFlightReplacement) {
  auto Stack = callbackStack();
  DarwinCallbacks Callbacks(Image, Options, Stack);
  ASSERT_TRUE(bool(Callbacks.start(*CPU)));
  for (size_t I = 1; I < DarwinCallbacks::TLSCallbackLimit; ++I)
    llvm::cantFail(Callbacks.addTLS(*CPU, 0, UINT64_MAX));
  llvm::cantFail(Callbacks.addTLS(*CPU, Code + 4, 7));
  auto Overflow = Callbacks.addTLS(*CPU, 0, 0);
  ASSERT_TRUE(bool(Overflow));
  EXPECT_NE(llvm::toString(std::move(Overflow)).find("limit"),
            std::string::npos);
  balanceCallback();
  auto Continue = Callbacks.returned(*CPU, value::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  ASSERT_TRUE(*Continue);
  llvm::cantFail(Callbacks.addTLS(*CPU, Code + 4, 9));
  balanceCallback();
  Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  ASSERT_TRUE(*Continue);
  EXPECT_EQ(CPU->Registers[callbackABI().info().Arguments[0]][0], 9u);
  balanceCallback();
  Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  EXPECT_FALSE(*Continue);
}
TEST_P(DarwinLibSystemTest,
       CallbackTargetsCannotUsePaddingPrivateGatesOrForeignMemory) {
  auto Stack = callbackStack();
  DarwinCallbacks Callbacks(Image, Options, Stack);
  for (uint64_t Address :
       {Code + 16, Data, value::ReturnGate, value::LibSystemGate, UINT64_MAX}) {
    auto E = Callbacks.addTLS(*CPU, Address, 0);
    ASSERT_TRUE(bool(E)) << Address;
    llvm::consumeError(std::move(E));
  }
  if (!GetParam()) {
    for (uint64_t Address : {Code + 1, Code + 14}) {
      auto E = Callbacks.addTLS(*CPU, Address, 0);
      ASSERT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
    }
  } else
    llvm::cantFail(Callbacks.addTLS(*CPU, Code + 15, 0));
  llvm::cantFail(
      Space->protect(Code, Image.Memory.PageSize, Read | UserAccessible));
  auto E = Callbacks.addTLS(*CPU, Code, 0);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST_P(DarwinLibSystemTest, CallbackFramesAndLivePermissionsAreAuthenticated) {
  auto Stack = callbackStack();
  DarwinCallbacks Callbacks(Image, Options, Stack);
  ASSERT_TRUE(bool(Callbacks.start(*CPU)));
  auto Forged = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_FALSE(bool(Forged));
  llvm::consumeError(Forged.takeError());
  const auto SPRegister = callbackABI().info().StackPointer;
  const auto OriginalSP = CPU->Registers[SPRegister];
  CPU->Registers[SPRegister][0] -= 16;
  auto Unbalanced = Callbacks.returned(*CPU, value::ReturnGate, Result);
  ASSERT_FALSE(bool(Unbalanced));
  llvm::consumeError(Unbalanced.takeError());
  CPU->Registers[SPRegister] = OriginalSP;
  llvm::cantFail(Callbacks.addTLS(*CPU, Code + 4, 0));
  balanceCallback();
  llvm::cantFail(
      Space->protect(Code, Image.Memory.PageSize, Read | UserAccessible));
  auto Revoked = Callbacks.returned(*CPU, value::ReturnGate, Result);
  ASSERT_FALSE(bool(Revoked));
  llvm::consumeError(Revoked.takeError());
  EXPECT_FALSE(Result.ExitStatus);
  EXPECT_FALSE(Result.ReturnValue);
}
TEST_P(DarwinLibSystemTest,
       NormalExitUsesADeeperFrameAndPreservesOpaqueStackObjects) {
  auto Stack = callbackStack();
  DarwinCallbacks Callbacks(Image, Options, Stack);
  const auto ABI = callbackABI();
  ASSERT_TRUE(bool(Callbacks.start(*CPU)));
  const uint64_t Current = Stack.SP - 512 - (GetParam() ? 8 : 0);
  const uint64_t Object = Current + 16;
  for (unsigned I = 0; I < 8; ++I)
    llvm::cantFail(Space->writeInteger(Object + 8 * I, UINT64_MAX - I, 8));
  llvm::cantFail(Callbacks.addTLS(*CPU, Code + 4, Object));
  CPU->Registers[ABI.info().StackPointer] = {Current, 0};
  auto Continue = Callbacks.exit(*CPU, 0x125, Result);
  ASSERT_TRUE(bool(Continue)) << llvm::toString(Continue.takeError());
  ASSERT_TRUE(*Continue);
  EXPECT_LE(CPU->Registers[ABI.info().StackPointer][0],
            Current - ABI.info().RedZoneSize);
  EXPECT_EQ(CPU->Registers[ABI.info().Arguments[0]][0], Object);
  for (unsigned I = 0; I < 8; ++I)
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Object + 8 * I, 8)),
              UINT64_MAX - I);
  auto Reentrant = Callbacks.exit(*CPU, 1, Result);
  ASSERT_FALSE(bool(Reentrant));
  llvm::consumeError(Reentrant.takeError());
  EXPECT_FALSE(Result.ExitStatus);
  balanceCallback();
  Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  EXPECT_FALSE(*Continue);
  EXPECT_EQ(Result.ExitStatus, 37);
  EXPECT_FALSE(Result.ReturnValue);
}
TEST_P(DarwinLibSystemTest,
       GlobalRegistrationUsesExactCAndCxxArgumentsAndPreservesErrno) {
  auto Stack = callbackStack();
  Image.HasGlobalCallbacks = true;
  const auto ABI = callbackABI();
  for (const auto Kind :
       {LibSystemGateKind::AtExit, LibSystemGateKind::CxaAtExit}) {
    DarwinCallbacks Callbacks(Image, Options, Stack);
    setArguments({Code + 4, 0xfedcba9876543210ULL, UINT64_MAX, 0, 0, 0});
    if (Kind == LibSystemGateKind::AtExit)
      CPU->FailedRegister = ABI.info().Arguments[1];
    auto R = invoke(Kind, &Callbacks);
    CPU->FailedRegister.reset();
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_TRUE(*R);
    EXPECT_EQ(CPU->Registers[ABI.info().Result][0], 0u);
    const auto &Call = Result.NativeCalls.back();
    EXPECT_EQ(Call.Name,
              Kind == LibSystemGateKind::AtExit ? "_atexit" : "___cxa_atexit");
    EXPECT_EQ(Call.ArgumentCount, Kind == LibSystemGateKind::AtExit ? 1u : 3u);
    EXPECT_EQ(Call.Arguments[0], Code + 4);
    if (Kind == LibSystemGateKind::CxaAtExit) {
      EXPECT_EQ(Call.Arguments[1], 0xfedcba9876543210ULL);
      EXPECT_EQ(Call.Arguments[2], UINT64_MAX);
    }
    EXPECT_EQ(Call.Result, 0u);
    EXPECT_TRUE(Call.ReturnAddress);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
    EXPECT_TRUE(Result.Services.empty());
  }
}
TEST_P(DarwinLibSystemTest,
       GlobalsInterleaveCAndCxxLIFOAfterExactlyOneTLSFinalization) {
  auto Stack = callbackStack();
  Image.HasGlobalCallbacks = true;
  DarwinCallbacks Callbacks(Image, Options, Stack);
  const auto ABI = callbackABI();
  ASSERT_TRUE(bool(Callbacks.start(*CPU)));
  llvm::cantFail(Callbacks.addGlobal(*CPU, Code + 4, std::nullopt));
  llvm::cantFail(Callbacks.addGlobal(*CPU, Code + 8, 22));
  llvm::cantFail(Callbacks.addTLS(*CPU, Code + 12, 11));
  balanceCallback();
  CPU->Registers[ABI.info().Result] = {0x125, 0};
  auto Continue = Callbacks.returned(*CPU, value::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  ASSERT_TRUE(*Continue);
  EXPECT_EQ(Result.PC, Code + 12);
  EXPECT_EQ(CPU->Registers[ABI.info().Arguments[0]][0], 11u);
  llvm::cantFail(Callbacks.addGlobal(*CPU, Code + 4, 33));
  balanceCallback();
  Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  ASSERT_TRUE(*Continue);
  EXPECT_EQ(Result.PC, Code + 4);
  EXPECT_EQ(CPU->Registers[ABI.info().Arguments[0]][0], 33u);
  // Native exit already passed its TLS phase; this entry must stay pending.
  llvm::cantFail(Callbacks.addTLS(*CPU, Code + 12, 44));
  llvm::cantFail(Callbacks.addGlobal(*CPU, Code + 8, UINT64_MAX));
  const struct {
    uint64_t Target;
    std::optional<uint64_t> Object;
  } Expected[] = {{Code + 8, UINT64_MAX},
                  {Code + 8, uint64_t(22)},
                  {Code + 4, std::nullopt}};
  for (const auto &[Target, Object] : Expected) {
    balanceCallback();
    Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
    ASSERT_TRUE(bool(Continue));
    ASSERT_TRUE(*Continue);
    EXPECT_EQ(Result.PC, Target);
    // Only the C++ callbacks receive an object argument.
    if (Object)
      EXPECT_EQ(CPU->Registers[ABI.info().Arguments[0]][0], *Object);
    EXPECT_FALSE(Result.ExitStatus);
    EXPECT_FALSE(Result.ReturnValue);
  }
  balanceCallback();
  Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  EXPECT_FALSE(*Continue);
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited);
  EXPECT_EQ(Result.ExitStatus, 37);
  EXPECT_EQ(Result.ReturnValue, 0x125u);
}
TEST_P(DarwinLibSystemTest,
       GlobalQuotaCountsDuplicatesAndReleasesTheActiveEntryBeforeInvocation) {
  auto Stack = callbackStack();
  Image.HasGlobalCallbacks = true;
  Image.HasTLSCallbacks = false;
  DarwinCallbacks Callbacks(Image, Options, Stack);
  ASSERT_TRUE(bool(Callbacks.start(*CPU)));
  for (size_t I = 0; I < DarwinCallbacks::GlobalCallbackLimit; ++I)
    llvm::cantFail(Callbacks.addGlobal(*CPU, Code + 4, std::nullopt));
  auto Overflow = Callbacks.addGlobal(*CPU, Code + 4, std::nullopt);
  ASSERT_TRUE(bool(Overflow));
  EXPECT_NE(llvm::toString(std::move(Overflow)).find("registration limit"),
            std::string::npos);
  balanceCallback();
  auto Continue = Callbacks.returned(*CPU, value::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  ASSERT_TRUE(*Continue);
  llvm::cantFail(Callbacks.addGlobal(*CPU, Code + 8, 0));
  balanceCallback();
  Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  ASSERT_TRUE(*Continue);
  EXPECT_EQ(Result.PC, Code + 8);
  EXPECT_EQ(CPU->Registers[callbackABI().info().Arguments[0]][0], 0u);
  for (size_t I = 1; I < DarwinCallbacks::GlobalCallbackLimit; ++I) {
    balanceCallback();
    Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
    ASSERT_TRUE(bool(Continue));
    ASSERT_TRUE(*Continue);
    EXPECT_EQ(Result.PC, Code + 4);
  }
  balanceCallback();
  Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  EXPECT_FALSE(*Continue);
  auto Completed = Callbacks.addGlobal(*CPU, Code + 4, 0);
  ASSERT_TRUE(bool(Completed));
  EXPECT_NE(llvm::toString(std::move(Completed)).find("unavailable"),
            std::string::npos);
}
TEST_P(DarwinLibSystemTest,
       GlobalTargetsRejectNullPaddingForeignPrivateAndMisalignedEntries) {
  auto Stack = callbackStack();
  Image.HasGlobalCallbacks = true;
  DarwinCallbacks Callbacks(Image, Options, Stack);
  for (uint64_t Address : {uint64_t(0), Code + 16, Data, value::ReturnGate,
                           value::LibSystemGate, UINT64_MAX}) {
    auto E = Callbacks.addGlobal(*CPU, Address, std::nullopt);
    ASSERT_TRUE(bool(E)) << Address;
    llvm::consumeError(std::move(E));
  }
  if (!GetParam()) {
    auto E = Callbacks.addGlobal(*CPU, Code + 1, 0);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  llvm::cantFail(
      Space->protect(Code, Image.Memory.PageSize, Read | UserAccessible));
  auto E = Callbacks.addGlobal(*CPU, Code + 4, UINT64_MAX);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("no longer executable"),
            std::string::npos);
}
TEST_P(DarwinLibSystemTest,
       GlobalFramesRejectForgedReturnsAndRecheckActivePermissions) {
  auto Stack = callbackStack();
  Image.HasGlobalCallbacks = true;
  DarwinCallbacks Callbacks(Image, Options, Stack);
  ASSERT_TRUE(bool(Callbacks.start(*CPU)));
  llvm::cantFail(Callbacks.addGlobal(*CPU, Code + 4, UINT64_MAX));
  balanceCallback();
  auto Continue = Callbacks.returned(*CPU, value::ReturnGate, Result);
  ASSERT_TRUE(bool(Continue));
  ASSERT_TRUE(*Continue);
  const auto Register = callbackABI().info().StackPointer;
  balanceCallback();
  const auto Balanced = CPU->Registers[Register];
  CPU->Registers[Register][0] -= 16;
  auto Forged = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_FALSE(bool(Forged));
  llvm::consumeError(Forged.takeError());
  CPU->Registers[Register] = Balanced;
  llvm::cantFail(
      Space->protect(Code, Image.Memory.PageSize, Read | UserAccessible));
  auto Revoked = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
  ASSERT_FALSE(bool(Revoked));
  llvm::consumeError(Revoked.takeError());
  EXPECT_FALSE(Result.ExitStatus);
  EXPECT_FALSE(Result.ReturnValue);
}
TEST_P(DarwinLibSystemTest,
       GlobalInvocationRechecksPermissionsAfterRegistration) {
  auto Stack = callbackStack();
  Image.HasGlobalCallbacks = true;
  DarwinCallbacks Callbacks(Image, Options, Stack);
  ASSERT_TRUE(bool(Callbacks.start(*CPU)));
  llvm::cantFail(Callbacks.addGlobal(*CPU, Code + 4, 0));
  balanceCallback();
  llvm::cantFail(
      Space->protect(Code, Image.Memory.PageSize, Read | UserAccessible));
  auto Revoked = Callbacks.returned(*CPU, value::ReturnGate, Result);
  ASSERT_FALSE(bool(Revoked));
  llvm::consumeError(Revoked.takeError());
  EXPECT_FALSE(Result.ExitStatus);
  EXPECT_FALSE(Result.ReturnValue);
}
TEST_P(DarwinLibSystemTest,
       StartupCallbacksRecheckPendingTablesAndPrepareMainAfresh) {
  for (bool Mutate : {false, true}) {
    if (Mutate)
      SetUp();
    auto Stack = callbackStack();
    const auto ABI = callbackABI();
    Image.Initializers = {{Data + 512, Code + 4, Code + 4, 8},
                          {Data + 520, Code + 8, Code + 8, 8}};
    llvm::cantFail(Space->writeInteger(Data + 512, Code + 4, 8));
    llvm::cantFail(Space->writeInteger(Data + 520, Code + 8, 8));
    DarwinCallbacks Callbacks(Image, Options, Stack);
    auto Start = Callbacks.start(*CPU);
    ASSERT_TRUE(bool(Start));
    EXPECT_EQ(*Start, Code + 4);
    EXPECT_EQ(CPU->Registers[ABI.info().Arguments[4]][0], Stack.ProgramVars);
    if (Mutate)
      llvm::cantFail(Space->writeInteger(Data + 520, Code + 12, 8));
    balanceCallback();
    auto Continue =
        Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
    if (Mutate) {
      ASSERT_FALSE(bool(Continue));
      EXPECT_NE(llvm::toString(Continue.takeError()).find("table was modified"),
                std::string::npos);
      continue;
    }
    ASSERT_TRUE(bool(Continue));
    ASSERT_TRUE(*Continue);
    EXPECT_EQ(Result.PC, Code + 8);
    balanceCallback();
    Continue = Callbacks.returned(*CPU, DarwinCallbacks::ReturnGate, Result);
    ASSERT_TRUE(bool(Continue));
    ASSERT_TRUE(*Continue);
    EXPECT_EQ(Result.PC, Code);
    EXPECT_EQ(CPU->Registers[ABI.info().Arguments[0]][0], Stack.Argc);
    EXPECT_EQ(CPU->Registers[ABI.info().Arguments[4]][0], 0u);
  }
}
INSTANTIATE_TEST_SUITE_P(Architectures, DarwinLibSystemTest,
                         testing::Values(false, true));
} // namespace
} // namespace neverd::emulation::darwin_model
