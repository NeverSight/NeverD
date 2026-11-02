//===- MMIOAtomicTests.cpp - Device effects and original CPU results ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "core/ExecutionDiagnostics.h"
#include "core/MMIOAtomicTransaction.h"
#include "core/MemoryLayout.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <climits>
#include <future>
#include <mutex>
#include <stdexcept>

namespace neverd::emulation {
namespace {
#define NEVERD_MMIO_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MMIO_TEST_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_MMIO_X64_BYTES(Name, ...)                                       \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "MMIOAtomicCases.def"
#undef NEVERD_MMIO_TEST_VALUE
#undef NEVERD_MMIO_TEST_TEXT
#undef NEVERD_MMIO_X64_BYTES
#define NEVERD_PARALLEL_BYTES(Name, ...)                                       \
  constexpr uint8_t Parallel##Name[] = {__VA_ARGS__};
#include "ParallelExecutionCases.def"
#undef NEVERD_PARALLEL_BYTES

struct Bank {
  std::recursive_mutex Mutex;
  std::array<uint8_t, Wide> Bytes{};
  unsigned Generation = 0, Prepares = 0, Commits = 0, Legacy = 0;
  bool FailPrepare = false, FailCommit = false, ThrowCommit = false;
  bool BadPreview = false;
  std::function<void()> OnValidate, OnPrepare, OnCommit;
  void reset() {
    llvm::support::endian::write64le(Bytes.data(), Original);
    llvm::support::endian::write64le(Bytes.data() + Word, Operand);
    ++Generation;
    Prepares = Commits = Legacy = 0;
  }
  GuestMMIOCallbacks callbacks() {
    GuestMMIOCallbacks C;
    C.Validate = [this](uint64_t Offset, uint64_t Size, bool) -> llvm::Error {
      std::lock_guard Lock(Mutex);
      if (Offset || !llvm::isPowerOf2_64(Size) || Size > Bytes.size())
        return diagnostic::error(InvalidPreview);
      if (OnValidate)
        OnValidate();
      return llvm::Error::success();
    };
    C.Read = [this](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      ++Legacy;
      return diagnostic::error(LegacyAccess);
    };
    C.Write = [this](uint64_t, unsigned, uint64_t) {
      ++Legacy;
      return diagnostic::error(LegacyAccess);
    };
    C.PrepareAtomic =
        [this](uint64_t Offset,
               unsigned Size) -> llvm::Expected<GuestMMIOPreparedAtomic> {
      std::lock_guard Lock(Mutex);
      ++Prepares;
      if (FailPrepare)
        return diagnostic::error(InjectedFailure);
      if (OnPrepare)
        OnPrepare();
      const unsigned Version = Generation;
      auto Consumed = std::make_shared<bool>(false);
      return GuestMMIOPreparedAtomic{
          std::vector<uint8_t>(Bytes.begin() + Offset,
                               Bytes.begin() + Offset +
                                   (BadPreview ? 0 : Size)),
          [this, Offset, Size, Version,
           Consumed](llvm::ArrayRef<uint8_t> Value) -> llvm::Error {
            std::lock_guard Lock(Mutex);
            if (std::exchange(*Consumed, true) || Version != Generation ||
                Value.size() != Size)
              return diagnostic::error(InvalidPreview);
            if (FailCommit)
              return diagnostic::error(InjectedFailure);
            if (ThrowCommit)
              throw std::runtime_error(InjectedFailure);
            if (OnCommit)
              OnCommit();
            llvm::copy(Value, Bytes.begin() + Offset);
            ++Generation;
            ++Commits;
            return llvm::Error::success();
          }};
    };
    return C;
  }
};
struct Profile {
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
};
void PrintTo(const Profile &P, std::ostream *OS) {
  *OS << executionBackendName(P.Backend) << guestArchitectureName(P.ISA);
}
const Profile Profiles[] = {
#define NEVERD_PARALLEL_PROFILE(Backend, ISA)                                  \
  {ExecutionBackendKind::Backend, GuestArchitecture::ISA},
#include "ParallelExecutionCases.def"
#undef NEVERD_PARALLEL_PROFILE
};
class MMIOAtomic : public testing::TestWithParam<Profile> {
protected:
  Bank Device;
  std::unique_ptr<ExecutionBackend> CPU;
  bool x64() const { return GetParam().ISA == GuestArchitecture::X64; }
  llvm::Expected<std::unique_ptr<ExecutionBackend>> create() {
    ExecutionConfiguration Config;
    Config.Backend = GetParam().Backend;
    Config.Architecture = GetParam().ISA;
    Config.Contract = x64() ? ExecutionContract::CheckedX64
                            : ExecutionContract::CheckedAArch64;
    Config.RequiredFeatures = ExecutionFeature::MMIOAtomics;
    auto Created = createExecutionBackend(Config, Limit);
    if (!Created)
      return Created.takeError();
    return std::move(Created->CPU);
  }
  void SetUp() override {
    auto Next = create();
    if (!Next) {
      auto E = Next.takeError();
      bool Unavailable = false;
      std::string Reason;
      E = llvm::handleErrors(
          std::move(E), [&](const BackendUnavailableError &Failure) {
            Unavailable = Failure.availability() !=
                          BackendAvailability::InitializationFailed;
            Reason = Failure.reason().str();
          });
      if (E)
        Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(*Next);
    llvm::cantFail(CPU->map(Code, memory::PageSize, Read | Write | Execute));
    llvm::cantFail(CPU->mapMMIO(Data, memory::PageSize, Device.callbacks()));
    llvm::cantFail(CPU->mapMMIO(Alias, memory::PageSize, Device.callbacks()));
    Device.reset();
  }
  std::vector<uint8_t> bytes(uint32_t Word) {
    std::vector<uint8_t> Bytes(ArmInstruction);
    llvm::support::endian::write32le(Bytes.data(), Word);
    return Bytes;
  }
  void program(ExecutionBackend &C, llvm::ArrayRef<uint8_t> Instruction) {
    auto Bytes = Instruction.vec();
    if (x64())
      Bytes.push_back(X64Nop);
    else {
      auto Nop = bytes(ArmNop);
      Bytes.insert(Bytes.end(), Nop.begin(), Nop.end());
    }
    llvm::cantFail(C.write(Code, Bytes));
    BackendHooks Hooks;
    Hooks.Instruction = [&C, End = Code + Instruction.size()](uint64_t PC,
                                                              unsigned) {
      if (PC == End)
        C.stop();
    };
    llvm::cantFail(C.installHooks(std::move(Hooks)));
  }
  void input(ExecutionBackend &C, unsigned Width = Word, bool Pair = false,
             bool Compare = false, bool Match = true) {
    if (x64()) {
      llvm::cantFail(C.setReg(X64Register::AX, Match ? Original : ~Original));
      llvm::cantFail(C.setReg(X64Register::DX, Operand));
      llvm::cantFail(C.setReg(X64Register::BX, Operand));
      llvm::cantFail(C.setReg(X64Register::CX, Pair ? Original : Data));
      llvm::cantFail(C.setReg(X64Register::SI, Data));
      llvm::cantFail(C.setReg(X64Register::FLAGS, X64Flags));
      if (Pair && Width == Word)
        llvm::cantFail(
            C.setReg(X64Register::DX, Original >> (CHAR_BIT * Word / 2)));
    } else {
      for (unsigned N = 0; N < ArmRegisterCount; ++N)
        llvm::cantFail(C.setReg(
            AArch64Register(unsigned(AArch64Register::X0) + N), Operand + N));
      auto Read = [&](unsigned Offset) {
        uint64_t V = 0;
        for (unsigned N = 0; N < Width; ++N)
          V |= uint64_t(Device.Bytes[Offset + N]) << (N * CHAR_BIT);
        return V;
      };
      llvm::cantFail(
          C.setReg(AArch64Register::X0, Compare ? Read(0) ^ !Match : Operand));
      llvm::cantFail(
          C.setReg(AArch64Register::X1, Pair ? Read(Width) : Original));
      llvm::cantFail(C.setReg(AArch64Register::X2, Operand));
      llvm::cantFail(C.setReg(AArch64Register::X3, Original));
      llvm::cantFail(C.setReg(AArch64Register::X4, Data));
      llvm::cantFail(C.setReg(AArch64Register::NZCV, ArmFlags));
    }
  }
  std::vector<uint64_t> state(ExecutionBackend &C) {
    std::vector<uint64_t> Result;
    if (x64()) {
      for (unsigned N = 0; N <= unsigned(X64Register::SS); ++N)
        Result.push_back(llvm::cantFail(C.reg(X64Register(N))));
    } else {
      for (unsigned N = 0; N <= unsigned(AArch64Register::FPSR); ++N)
        Result.push_back(llvm::cantFail(C.reg(AArch64Register(N))));
    }
    return Result;
  }
  void run(ExecutionBackend &C,
           ExecutionExitKind Kind = ExecutionExitKind::Stopped) {
    auto Exit = C.runUntilExit(Code, Timeout);
    ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
    EXPECT_EQ(Exit->Kind, Kind) << Exit->Diagnostic;
  }
  void setupSwap(bool Pair = false) {
    program(*CPU,
            x64() ? (Pair ? llvm::ArrayRef(ComparePair) : llvm::ArrayRef(Swap))
                  : llvm::ArrayRef(bytes(Pair ? ArmComparePair : ArmSwap)));
    input(*CPU, x64() && Pair ? Wide : Word, Pair, Pair);
    llvm::cantFail(
        CPU->writeRegister(x64() ? CPURegister::X64PC : CPURegister::AArch64PC,
                           RegisterValue{Code, 0}));
  }
};

TEST_P(MMIOAtomic, OriginalInstructionResultsMatchRAMAcrossAllAtomicFamilies) {
  auto RAM = llvm::cantFail(create());
  llvm::cantFail(RAM->map(Code, memory::PageSize, Read | Write | Execute));
  llvm::cantFail(RAM->map(Data, memory::PageSize, Read | Write));
  auto Compare = [&](llvm::ArrayRef<uint8_t> Instruction, unsigned Width,
                     bool Pair, bool CAS, bool Match) {
    Device.reset();
    program(*CPU, Instruction);
    program(*RAM, Instruction);
    input(*CPU, Width, Pair, CAS, Match);
    input(*RAM, Width, Pair, CAS, Match);
    llvm::cantFail(RAM->write(Data, Device.Bytes));
    run(*RAM);
    run(*CPU);
    std::array<uint8_t, Wide> Expected;
    llvm::cantFail(RAM->read(Data, Expected));
    EXPECT_EQ(Device.Bytes, Expected);
    EXPECT_EQ(state(*CPU), state(*RAM));
    EXPECT_EQ(Device.Commits, 1u);
    EXPECT_EQ(Device.Legacy, 0u);
  };
  if (x64()) {
#define NEVERD_ATOMIC_CASE(Name, Width, CAS, ...)                              \
  do {                                                                         \
    SCOPED_TRACE(#Name);                                                       \
    const uint8_t Encoding[] = {__VA_ARGS__};                                  \
    for (bool Locked : {false, true}) {                                        \
      auto Instruction = llvm::ArrayRef(Encoding).vec();                       \
      if (Locked)                                                              \
        Instruction.insert(Instruction.begin(), X64Lock);                      \
      for (bool Match : {true, false})                                         \
        Compare(Instruction, Width, false, CAS, Match);                        \
    }                                                                          \
  } while (false);
#include "x86_64/X64AtomicCases.def"
#undef NEVERD_ATOMIC_CASE
#define NEVERD_WIDE_ATOMIC_CASE(Name, Width, Locked, ...)                      \
  do {                                                                         \
    SCOPED_TRACE(#Name);                                                       \
    const uint8_t Encoding[] = {__VA_ARGS__};                                  \
    for (bool Match : {true, false})                                           \
      Compare(Encoding, Width, true, true, Match);                             \
  } while (false);
#include "x86_64/X64WideAtomicCases.def"
#undef NEVERD_WIDE_ATOMIC_CASE
#define NEVERD_MMIO_X64_UPDATE(Name, Width, ...)                               \
  do {                                                                         \
    SCOPED_TRACE(#Name);                                                       \
    const uint8_t Encoding[] = {__VA_ARGS__};                                  \
    Compare(Encoding, Width, false, false, true);                              \
  } while (false);
#include "MMIOAtomicCases.def"
#undef NEVERD_MMIO_X64_UPDATE
  } else {
    enum class Family {
      Compare,
      ComparePair,
      Add,
      Clear,
      Xor,
      Set,
      SignedMin,
      SignedMax,
      UnsignedMin,
      UnsignedMax,
      Swap
    };
#define NEVERD_ATOMIC_CASE(Name, Kind, Width, Count, Word, Expected)           \
  do {                                                                         \
    SCOPED_TRACE(#Name);                                                       \
    constexpr bool CAS = Family::Kind == Family::Compare ||                    \
                         Family::Kind == Family::ComparePair;                  \
    for (bool Match : {true, false})                                           \
      Compare(bytes(Word), Width, Count == 2, CAS, Match);                     \
  } while (false);
#include "aarch64/AArch64AtomicCases.def"
#undef NEVERD_ATOMIC_CASE
  }
}

TEST_P(MMIOAtomic, StopsBeforeCommitPreserveCPUAndBothWideWords) {
  for (unsigned Boundary = 0; Boundary < 5; ++Boundary) {
    Device.reset();
    setupSwap(true);
    const auto Before = state(*CPU);
    const auto OriginalBytes = Device.Bytes;
    auto Stop = [&] { CPU->stop(); };
    Device.OnValidate = Boundary == 0 ? Stop : std::function<void()>{};
    Device.OnPrepare = Boundary == 1 ? Stop : std::function<void()>{};
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) {
      if (Boundary == 2)
        CPU->stop();
    };
    unsigned Words = 0;
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) {
      EXPECT_EQ(state(*CPU), Before);
      EXPECT_EQ(Device.Bytes, OriginalBytes);
      if (++Words == Boundary - 2)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    run(*CPU);
    EXPECT_EQ(Device.Commits, 0u);
    EXPECT_EQ(Device.Legacy, 0u);
    EXPECT_EQ(Device.Bytes, OriginalBytes);
    EXPECT_EQ(state(*CPU), Before);
  }
}
TEST_P(MMIOAtomic, SuccessfulCommitWinsARacingStop) {
  setupSwap();
  Device.OnCommit = [&] { CPU->stop(); };
  run(*CPU);
  EXPECT_EQ(Device.Commits, 1u);
  EXPECT_EQ(llvm::support::endian::read64le(Device.Bytes.data()), Operand);
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(
                x64() ? CPURegister::X64PC : CPURegister::AArch64PC))[0],
            Code + (x64() ? sizeof(Swap) : ArmInstruction));
}
TEST_P(MMIOAtomic, MissingProviderNeverCallsLegacyReadOrWrite) {
  llvm::cantFail(CPU->unmapMMIO(Data, memory::PageSize));
  auto Callbacks = Device.callbacks();
  Callbacks.PrepareAtomic = {};
  llvm::cantFail(CPU->mapMMIO(Data, memory::PageSize, std::move(Callbacks)));
  setupSwap();
  auto Before = state(*CPU);
  unsigned Observations = 0;
  Device.OnValidate = [&] { ++Observations; };
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t, unsigned) { ++Observations; };
  Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  run(*CPU, ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Observations, 0u);
  EXPECT_EQ(Device.Legacy, 0u);
  EXPECT_EQ(Device.Prepares, 0u);
  EXPECT_EQ(state(*CPU), Before);
}
TEST_P(MMIOAtomic, IdenticalValueWritesInvalidateOldPreviews) {
  setupSwap();
  const auto Before = state(*CPU);
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Device.Generation; };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  run(*CPU, ExecutionExitKind::DeviceFailure);
  EXPECT_EQ(Device.Commits, 0u);
  EXPECT_EQ(state(*CPU), Before);
}
TEST_P(MMIOAtomic, ProviderFailuresAndExceptionsHaveNoEffects) {
  for (unsigned Failure = 0; Failure < 4; ++Failure) {
    if (Failure) {
      CPU = llvm::cantFail(create());
      llvm::cantFail(CPU->map(Code, memory::PageSize, Read | Write | Execute));
      llvm::cantFail(CPU->mapMMIO(Data, memory::PageSize, Device.callbacks()));
    }
    Device.reset();
    Device.FailPrepare = Failure == 0;
    Device.FailCommit = Failure == 1;
    Device.ThrowCommit = Failure == 2;
    Device.BadPreview = Failure == 3;
    setupSwap();
    const auto Before = state(*CPU);
    const auto Bytes = Device.Bytes;
    run(*CPU, ExecutionExitKind::DeviceFailure);
    EXPECT_EQ(Device.Commits, 0u);
    EXPECT_EQ(Device.Bytes, Bytes);
    EXPECT_EQ(state(*CPU), Before);
  }
}
TEST_P(MMIOAtomic, UnalignedOperandsNeverReachDeviceCallbacks) {
  setupSwap();
  llvm::cantFail(
      CPU->writeRegister(x64() ? CPURegister::X64SI : CPURegister::AArch64X4,
                         RegisterValue{Data + 1, 0}));
  const auto Before = state(*CPU);
  const auto Bytes = Device.Bytes;
  auto Exit = CPU->runUntilExit(Code, Timeout);
  ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
  EXPECT_EQ(Exit->Kind, x64() ? ExecutionExitKind::UnsupportedOperation
                              : ExecutionExitKind::GuestFault);
  if (!x64()) {
    ASSERT_TRUE(Exit->Fault);
    EXPECT_EQ(Exit->Fault->Kind, BackendFaultKind::Alignment);
    EXPECT_EQ(Exit->Fault->Address, Data + 1);
    EXPECT_EQ(Exit->Fault->Size, Word);
  }
  EXPECT_EQ(Device.Prepares, 0u);
  EXPECT_EQ(Device.Legacy, 0u);
  EXPECT_EQ(Device.Bytes, Bytes);
  EXPECT_EQ(state(*CPU), Before);
}
TEST_P(MMIOAtomic, ObserverExceptionsPreserveCPUAndDevice) {
  setupSwap();
  const auto Before = state(*CPU);
  const auto Bytes = Device.Bytes;
  BackendHooks Hooks;
  Hooks.Write = [](uint64_t, unsigned, uint64_t) {
    throw std::runtime_error(InjectedFailure);
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  run(*CPU, ExecutionExitKind::BackendFailure);
  EXPECT_EQ(Device.Commits, 0u);
  EXPECT_EQ(Device.Bytes, Bytes);
  EXPECT_EQ(state(*CPU), Before);
}
TEST_P(MMIOAtomic, RepeatedAliasTransactionsRetirePrivateOperands) {
  for (uint64_t Address : {Data, Alias, Data}) {
    setupSwap();
    llvm::cantFail(
        CPU->writeRegister(x64() ? CPURegister::X64SI : CPURegister::AArch64X4,
                           RegisterValue{Address, 0}));
    run(*CPU);
    EXPECT_EQ(llvm::support::endian::read64le(Device.Bytes.data()), Operand);
  }
  EXPECT_EQ(Device.Commits, 3u);
  // Replace the device VA with real RAM. A stale private operand mapping must
  // not survive retirement and steal the following ordinary RAM transaction.
  llvm::cantFail(CPU->unmapMMIO(Data, memory::PageSize));
  llvm::cantFail(CPU->map(Data, memory::PageSize, Read | Write));
  llvm::cantFail(CPU->writeInteger(Data, Original, Word));
  setupSwap();
  run(*CPU);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, Word)), Operand);
  EXPECT_EQ(Device.Commits, 3u);
}
TEST_P(MMIOAtomic, ParallelCPUsCommitEveryDeviceUpdateExactlyOnce) {
  llvm::support::endian::write64le(Device.Bytes.data(), 0);
  const auto Program = x64() ? llvm::ArrayRef(ParallelX64Contend)
                             : llvm::ArrayRef(ParallelArmContend);
  llvm::cantFail(CPU->write(Code, Program));
  ExecutionConfiguration Config;
  Config.Backend = GetParam().Backend;
  Config.Architecture = GetParam().ISA;
  Config.Contract =
      x64() ? ExecutionContract::CheckedX64 : ExecutionContract::CheckedAArch64;
  Config.RequiredFeatures =
      ExecutionFeature::MMIOAtomics | ExecutionFeature::ParallelCPUs;
  std::array<std::unique_ptr<ExecutionBackend>, Contenders> CPUs;
  for (unsigned N = 0; N < Contenders; ++N) {
    auto Created = createExecutionBackend(Config, CPU->addressSpace());
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    CPUs[N] = std::move(Created->CPU);
    auto Set = [&](CPURegister R, uint64_t V) {
      llvm::cantFail(CPUs[N]->writeRegister(R, RegisterValue{V, 0}));
    };
    Set(x64() ? CPURegister::X64BX : CPURegister::AArch64X3, N ? Alias : Data);
    Set(x64() ? CPURegister::X64CX : CPURegister::AArch64X4, Iterations);
    if (!x64())
      Set(CPURegister::AArch64X1, 1);
    BackendHooks Hooks;
    Hooks.Instruction = [Ptr = CPUs[N].get(),
                         End = Code + Program.size() -
                               (x64() ? 1 : ArmInstruction)](uint64_t PC,
                                                             unsigned) {
      if (PC == End)
        Ptr->stop();
    };
    llvm::cantFail(CPUs[N]->installHooks(std::move(Hooks)));
  }
  std::promise<void> Start;
  auto Ready = Start.get_future().share();
  std::array<std::future<llvm::Expected<ExecutionExit>>, Contenders> Runs;
  for (unsigned N = 0; N < Contenders; ++N)
    Runs[N] = std::async(std::launch::async, [&, N] {
      Ready.wait();
      return CPUs[N]->runUntilExit(Code, Timeout);
    });
  Start.set_value();
  for (auto &Run : Runs) {
    auto Exit = Run.get();
    ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
    EXPECT_EQ(Exit->Kind, ExecutionExitKind::Stopped) << Exit->Diagnostic;
  }
  EXPECT_EQ(llvm::support::endian::read64le(Device.Bytes.data()),
            Contenders * Iterations);
  EXPECT_EQ(Device.Commits, Contenders * Iterations);
  EXPECT_EQ(Device.Legacy, 0u);
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, MMIOAtomic,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &I) {
                           return std::string(
                                      executionBackendName(I.param.Backend)) +
                                  guestArchitectureName(I.param.ISA);
                         });
} // namespace
} // namespace neverd::emulation
