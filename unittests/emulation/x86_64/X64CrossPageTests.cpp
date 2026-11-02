//===- X64CrossPageTests.cpp - Precise native cross-page RAM access
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <climits>
#include <iterator>
#include <tuple>
#if defined(__linux__) && defined(__x86_64__)
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "../UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_CROSS_PAGE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_CROSS_PAGE_BYTES(Name, ...)                                     \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_CROSS_PAGE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "X64CrossPageCases.def"
#undef NEVERD_CROSS_PAGE_TEXT
#undef NEVERD_CROSS_PAGE_BYTES
#undef NEVERD_CROSS_PAGE_VALUE

enum class Operation { Load, Store, VectorLoad, VectorStore, Update };
struct InstructionCase {
  const char *Name;
  Operation Kind;
  unsigned Width;
  uint64_t Before, After;
  llvm::ArrayRef<uint8_t> Bytes;
  bool reads() const {
    return Kind == Operation::Load || Kind == Operation::VectorLoad ||
           Kind == Operation::Update;
  }
  bool writes() const {
    return Kind == Operation::Store || Kind == Operation::VectorStore ||
           Kind == Operation::Update;
  }
};
#define NEVERD_CROSS_PAGE_INSTRUCTION(Name, Kind, Width, Before, After, ...)   \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64CrossPageCases.def"
#undef NEVERD_CROSS_PAGE_INSTRUCTION
const InstructionCase Cases[] = {
#define NEVERD_CROSS_PAGE_INSTRUCTION(Name, Kind, Width, Before, After, ...)   \
  {#Name, Operation::Kind, Width, Before, After, Name},
#include "X64CrossPageCases.def"
#undef NEVERD_CROSS_PAGE_INSTRUCTION
};
void PrintTo(const InstructionCase &Case, std::ostream *OS) {
  *OS << Case.Name;
}
using Parameter =
    std::tuple<ExecutionBackendKind, ExecutionContract, InstructionCase>;

class X64CrossPage : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  const InstructionCase &testCase() const { return std::get<2>(GetParam()); }
  bool userMode() const {
    return std::get<1>(GetParam()) == ExecutionContract::CheckedUserX64;
  }
  uint64_t Address = 0;
  void SetUp() override {
    auto Backend = createExecutionBackend(std::get<0>(GetParam()),
                                          std::get<1>(GetParam()), Limit);
    if (!Backend) {
      auto Error = Backend.takeError();
      const bool Unavailable = Error.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(Error));
      if (Unavailable &&
          !requireHvf(std::get<0>(GetParam()), GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(Backend->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    // An interposed allocation proves that adjacent virtual pages need not
    // belong to one allocation or consecutive physical pages.
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->map(Alias, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->map(Data + PageSize, PageSize, Read | Write | UserAccessible));
    std::vector<uint8_t> Bytes(testCase().Bytes.begin(),
                               testCase().Bytes.end());
    Bytes.push_back(NopOpcode);
    llvm::cantFail(CPU->write(Code, Bytes));
    Address = Data + PageSize - testCase().Width / 2;
    seed();
  }
  std::array<uint8_t, VectorBytes> original() const {
    std::array<uint8_t, VectorBytes> Bytes{};
    const uint64_t Upper =
        testCase().Kind == Operation::VectorStore ? Low : High;
    for (unsigned I = 0; I < WordBytes; ++I) {
      Bytes[I] = uint8_t(testCase().Before >> (I * CHAR_BIT));
      Bytes[I + WordBytes] = uint8_t(Upper >> (I * CHAR_BIT));
    }
    return Bytes;
  }
  void seed() {
    llvm::cantFail(CPU->write(Address, original()));
    llvm::cantFail(CPU->setReg(X64Register::CX, Address));
    llvm::cantFail(CPU->setReg(
        X64Register::AX, testCase().Kind == Operation::Store ? Low : High));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags));
    llvm::cantFail(CPU->setXmm(0, testCase().Kind == Operation::VectorStore
                                      ? RegisterValue{Low, High}
                                      : RegisterValue{High, Low}));
  }
  std::array<uint8_t, VectorBytes> memory(bool InObserver = false) const {
    std::array<uint8_t, VectorBytes> Bytes{};
    if (InObserver)
      llvm::cantFail(CPU->read(Address, Bytes));
    else
      llvm::cantFail(CPU->snapshotBacking(Address, Bytes));
    return Bytes;
  }
  ExecutionExit run(BackendHooks Hooks = {}) {
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void expectUnchanged() {
    EXPECT_EQ(memory(), original());
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)),
              testCase().Kind == Operation::Store ? Low : High);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0)),
              testCase().Kind == Operation::VectorStore
                  ? (RegisterValue{Low, High})
                  : (RegisterValue{High, Low}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  }
  void expectResult() {
    auto Expected = original();
    if (testCase().writes())
      for (unsigned I = 0; I < testCase().Width; ++I)
        Expected[I] = uint8_t((I < WordBytes ? testCase().After : High) >>
                              ((I % WordBytes) * CHAR_BIT));
    EXPECT_EQ(memory(), Expected);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)),
              testCase().Kind == Operation::Load    ? testCase().After
              : testCase().Kind == Operation::Store ? Low
                                                    : High);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0)),
              testCase().Kind == Operation::VectorLoad ||
                      testCase().Kind == Operation::VectorStore
                  ? (RegisterValue{Low, High})
                  : (RegisterValue{High, Low}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)) & ArithmeticFlags,
              testCase().Kind == Operation::Update ? OverflowFlags : 0u);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)),
              Code + testCase().Bytes.size());
  }
};

TEST_P(X64CrossPage, ExecutesEveryCrossingWithoutAssumingContiguousBacking) {
  for (unsigned Prefix = 1; Prefix < testCase().Width; ++Prefix) {
    SCOPED_TRACE(Prefix);
    Address = Data + PageSize - Prefix;
    seed();
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, uint32_t Size) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, testCase().Width);
      EXPECT_EQ(memory(true), original());
      ++Reads;
    };
    Hooks.Write = [&](uint64_t A, uint32_t Size, uint64_t Value) {
      EXPECT_EQ(A, Address + Writes * WordBytes);
      EXPECT_EQ(Size, std::min<uint64_t>(testCase().Width - Writes * WordBytes,
                                         WordBytes));
      const uint64_t Mask = UINT64_MAX >> ((WordBytes - Size) * CHAR_BIT);
      EXPECT_EQ(Value, (Writes ? High : testCase().After) & Mask);
      EXPECT_EQ(memory(true), original());
      ++Writes;
    };
    auto Exit = run(std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, unsigned(testCase().reads()));
    EXPECT_EQ(Writes,
              testCase().writes()
                  ? unsigned((testCase().Width + WordBytes - 1) / WordBytes)
                  : 0u);
    expectResult();
  }
}

TEST_P(X64CrossPage, ObserverStopsBeforeAnyMemoryRegisterFlagOrPCChange) {
  for (bool OnWrite : {false, true}) {
    if (OnWrite ? !testCase().writes() : !testCase().reads())
      continue;
    seed();
    BackendHooks Hooks;
    if (OnWrite)
      Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { CPU->stop(); };
    else
      Hooks.Read = [&](uint64_t, uint32_t) { CPU->stop(); };
    auto Exit = run(std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectUnchanged();
  }
}

TEST_P(X64CrossPage, SecondPageFaultPreservesTheWritablePrefixAndCPUContext) {
  llvm::cantFail(CPU->protect(Data + PageSize, PageSize, UserAccessible));
  auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
  EXPECT_EQ(Exit.Fault->PC, Code);
  EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
  EXPECT_EQ(Exit.Fault->Size, testCase().Width / 2);
  EXPECT_EQ(Exit.Fault->Access, testCase().reads() ? BackendAccessKind::Read
                                                   : BackendAccessKind::Write);
  expectUnchanged();
}

TEST_P(X64CrossPage, SecondPageReadPermissionCannotAuthorizeAStore) {
  llvm::cantFail(
      CPU->protect(Data + PageSize, PageSize, Read | UserAccessible));
  auto Exit = run();
  if (testCase().writes()) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
    EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Write);
    expectUnchanged();
  } else {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectResult();
  }
}

TEST_P(X64CrossPage, SecondPageSupervisorRightsDoNotAuthorizeUserAccess) {
  llvm::cantFail(CPU->protect(Data + PageSize, PageSize, Read | Write));
  auto Exit = run();
  if (userMode()) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
    EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
    expectUnchanged();
  } else {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectResult();
  }
}

TEST_P(X64CrossPage, UnmappedSecondPageCannotChangeEitherAllocation) {
  auto View = llvm::cantFail(CPU->pinBacking(Address, VectorBytes));
  llvm::cantFail(CPU->addressSpace()->unmap(Data + PageSize, PageSize));
  auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::UnmappedMemory);
  EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
  EXPECT_EQ(Exit.Fault->Size, testCase().Width / 2);
  std::array<uint8_t, VectorBytes> Bytes{};
  llvm::cantFail(View.read(0, Bytes));
  EXPECT_EQ(Bytes, original());
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
}

TEST_P(X64CrossPage, RecoverableSecondPageFaultCanBeConsumedAndRetried) {
  llvm::cantFail(CPU->protect(Data + PageSize, PageSize, UserAccessible));
  BackendHooks Hooks;
  Hooks.RecoverableFault = [&](const BackendFault &Fault) {
    EXPECT_EQ(Fault.Address, Data + PageSize);
    return true;
  };
  auto Exit = run(std::move(Hooks));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault) << Exit.Diagnostic;
  EXPECT_FALSE(CPU->fault());
  EXPECT_NE(llvm::toString(CPU->setReg(X64Register::AX, Low)), "");
  ASSERT_TRUE(CPU->takeRecoverableFault());
  EXPECT_FALSE(CPU->takeRecoverableFault());
  expectUnchanged();
  llvm::cantFail(
      CPU->protect(Data + PageSize, PageSize, Read | Write | UserAccessible));
  ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
  expectResult();
}

TEST_P(X64CrossPage, AdjacentAliasesPreserveBothPhysicalPageIdentities) {
  llvm::cantFail(CPU->mapAlias(Alias + PageSize, Data, PageSize,
                               Read | Write | UserAccessible));
  llvm::cantFail(CPU->mapAlias(Alias + 2 * PageSize, Data + PageSize, PageSize,
                               Read | Write | UserAccessible));
  const uint64_t Original = Address;
  Address = Alias + 2 * PageSize - testCase().Width / 2;
  seed();
  auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  expectResult();
  const auto AliasBytes = memory();
  Address = Original;
  EXPECT_EQ(memory(), AliasBytes);
}

using CopyParameter = std::tuple<ExecutionBackendKind, ExecutionContract>;
class X64CrossPageCopy : public testing::TestWithParam<CopyParameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  const uint64_t Source = Data + PageSize - WordBytes / 2;
  const uint64_t Destination = Alias + PageSize - WordBytes / 2;
  void SetUp() override {
    auto Backend = createExecutionBackend(std::get<0>(GetParam()),
                                          std::get<1>(GetParam()), Limit);
    if (!Backend) {
      auto Error = Backend.takeError();
      const bool Unavailable = Error.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(Error));
      if (Unavailable &&
          !requireHvf(std::get<0>(GetParam()), GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(Backend->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    // Interleave source and destination page owners, as in real shared RAM.
    for (uint64_t Address : {Data, Alias, Data + PageSize, Alias + PageSize})
      llvm::cantFail(
          CPU->map(Address, PageSize, Read | Write | UserAccessible));
    std::vector<uint8_t> Bytes(std::begin(RepeatQword), std::end(RepeatQword));
    Bytes.push_back(NopOpcode);
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->writeInteger(Source, Low, WordBytes));
    llvm::cantFail(CPU->writeInteger(Source + WordBytes, High, WordBytes));
  }
  void seed(bool Backward) {
    llvm::cantFail(CPU->writeInteger(Destination, 0, WordBytes));
    llvm::cantFail(CPU->writeInteger(Destination + WordBytes, 0, WordBytes));
    const uint64_t Offset = Backward ? WordBytes : 0;
    llvm::cantFail(CPU->setReg(X64Register::SI, Source + Offset));
    llvm::cantFail(CPU->setReg(X64Register::DI, Destination + Offset));
    llvm::cantFail(CPU->setReg(X64Register::CX, CopyCount));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS,
                               InitialFlags | (Backward ? DirectionFlag : 0)));
  }
  ExecutionExit run(BackendHooks Hooks = {}) {
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
};

TEST_P(X64CrossPageCopy, CopiesCompleteElementsAcrossIndependentPageOwners) {
  for (bool Backward : {false, true}) {
    seed(Backward);
    auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Destination, WordBytes)), Low);
    EXPECT_EQ(
        llvm::cantFail(CPU->readInteger(Destination + WordBytes, WordBytes)),
        High);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), 0u);
    const uint64_t Offset = Backward ? uint64_t(0) - WordBytes : VectorBytes;
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SI)), Source + Offset);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DI)), Destination + Offset);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)),
              InitialFlags | (Backward ? DirectionFlag : 0));
  }
}

TEST_P(X64CrossPageCopy, SecondWriteStopPreservesExactlyOneCompletedElement) {
  for (bool Backward : {false, true}) {
    seed(Backward);
    unsigned Writes = 0;
    BackendHooks Hooks;
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) {
      if (++Writes == CopyCount)
        CPU->stop();
    };
    auto Exit = run(std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Destination, WordBytes)),
              Backward ? 0u : Low);
    EXPECT_EQ(
        llvm::cantFail(CPU->readInteger(Destination + WordBytes, WordBytes)),
        Backward ? High : 0u);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), CopyCount - 1);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SI)),
              Source + (Backward ? 0 : WordBytes));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DI)),
              Destination + (Backward ? 0 : WordBytes));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
    ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Destination, WordBytes)), Low);
    EXPECT_EQ(
        llvm::cantFail(CPU->readInteger(Destination + WordBytes, WordBytes)),
        High);
  }
}

TEST_P(X64CrossPageCopy, FaultAfterOneElementPreservesRestartStateAndBacking) {
  seed(true);
  auto View = llvm::cantFail(CPU->pinBacking(Destination, VectorBytes));
  llvm::cantFail(CPU->protect(Alias, PageSize, Read | UserAccessible));
  BackendHooks Hooks;
  Hooks.RecoverableFault = [](const BackendFault &) { return true; };
  auto Exit = run(std::move(Hooks));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Address, Destination);
  EXPECT_EQ(Exit.Fault->Size, WordBytes / 2);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), CopyCount - 1);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SI)), Source);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DI)), Destination);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  std::array<uint64_t, 2> Bytes{};
  llvm::cantFail(View.read(
      0, llvm::MutableArrayRef<uint8_t>(
             reinterpret_cast<uint8_t *>(Bytes.data()), sizeof(Bytes))));
  EXPECT_EQ(Bytes, (std::array<uint64_t, 2>{0, High}));
  ASSERT_TRUE(CPU->takeRecoverableFault());
  llvm::cantFail(CPU->protect(Alias, PageSize, Read | Write | UserAccessible));
  ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Destination, WordBytes)), Low);
  EXPECT_EQ(
      llvm::cantFail(CPU->readInteger(Destination + WordBytes, WordBytes)),
      High);
}

TEST_P(X64CrossPageCopy,
       FirstElementFaultCannotPublishAPrefixOrAdvancePointers) {
  seed(false);
  auto View = llvm::cantFail(CPU->pinBacking(Destination, VectorBytes));
  llvm::cantFail(
      CPU->protect(Alias + PageSize, PageSize, Read | UserAccessible));
  auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), CopyCount);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SI)), Source);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DI)), Destination);
  std::array<uint64_t, 2> Bytes{};
  llvm::cantFail(View.read(
      0, llvm::MutableArrayRef<uint8_t>(
             reinterpret_cast<uint8_t *>(Bytes.data()), sizeof(Bytes))));
  EXPECT_EQ(Bytes, (std::array<uint64_t, 2>{}));
}

using DeviceParameter = std::tuple<ExecutionBackendKind, bool, bool>;
class X64CrossPageDevice : public testing::TestWithParam<DeviceParameter> {};
TEST_P(X64CrossPageDevice,
       MixedRAMAndDevicePagesRejectWithoutCallbacksOrEffects) {
  auto Backend = createExecutionBackend(std::get<0>(GetParam()),
                                        ExecutionContract::CheckedX64, Limit);
  if (!Backend) {
    auto Error = Backend.takeError();
    const bool Unavailable = Error.isA<BackendUnavailableError>();
    auto Reason = llvm::toString(std::move(Error));
    if (Unavailable &&
        !requireHvf(std::get<0>(GetParam()), GuestArchitecture::X64))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  auto &CPU = *Backend->CPU;
  const bool DeviceFirst = std::get<1>(GetParam());
  const bool IsWrite = std::get<2>(GetParam());
  unsigned Callbacks = 0, Observations = 0;
  GuestMMIOCallbacks Device;
  Device.Validate = [&](uint64_t, unsigned, bool) {
    ++Callbacks;
    return llvm::Error::success();
  };
  Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++Callbacks;
    return Low;
  };
  Device.Write = [&](uint64_t, unsigned, uint64_t) {
    ++Callbacks;
    return llvm::Error::success();
  };
  llvm::cantFail(CPU.mapMMIO(Data + (DeviceFirst ? 0 : PageSize), PageSize,
                             std::move(Device)));
  llvm::cantFail(
      CPU.map(Data + (DeviceFirst ? PageSize : 0), PageSize, Read | Write));
  llvm::cantFail(CPU.map(Code, PageSize, Read | Write | Execute));
  std::vector<uint8_t> Bytes(IsWrite ? std::begin(Write64) : std::begin(Read64),
                             IsWrite ? std::end(Write64) : std::end(Read64));
  Bytes.push_back(NopOpcode);
  llvm::cantFail(CPU.write(Code, Bytes));
  llvm::cantFail(CPU.setReg(X64Register::CX, Data + PageSize - WordBytes / 2));
  llvm::cantFail(CPU.setReg(X64Register::AX, Low));
  const uint64_t RAM = Data + PageSize - (DeviceFirst ? 0 : WordBytes / 2);
  llvm::cantFail(CPU.writeInteger(RAM, High, WordBytes / 2));
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t, uint32_t) { ++Observations; };
  Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ++Observations; };
  llvm::cantFail(CPU.installHooks(std::move(Hooks)));
  auto Exit = llvm::cantFail(CPU.runUntilExit(Code, Timeout));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
      << Exit.Diagnostic;
  EXPECT_EQ(Callbacks, 0u);
  EXPECT_EQ(Observations, 0u);
  EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::AX)), Low);
  EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::PC)), Code);
  std::array<uint8_t, WordBytes / 2> Actual{};
  llvm::cantFail(CPU.snapshotBacking(RAM, Actual));
  for (unsigned I = 0; I < Actual.size(); ++I)
    EXPECT_EQ(Actual[I], uint8_t(High >> (I * CHAR_BIT)));
}

#if defined(__linux__) && defined(__x86_64__)
struct NativeFaultRecord {
  std::atomic<int> Signal{0}, Code{0};
  std::atomic<uintptr_t> Address{0};
};
static_assert(sizeof(NativeFaultRecord) <= PageSize);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
static_assert(std::atomic<NativeFaultRecord *>::is_always_lock_free);
std::atomic<NativeFaultRecord *> NativeFault{nullptr};
void nativeFault(int Signal, siginfo_t *Info, void *) {
  auto *Record = NativeFault.load(std::memory_order_relaxed);
  if (!Record)
    _exit(OracleSetupFailure);
  Record->Signal.store(Signal, std::memory_order_relaxed);
  Record->Code.store(Info->si_code, std::memory_order_relaxed);
  Record->Address.store(reinterpret_cast<uintptr_t>(Info->si_addr),
                        std::memory_order_relaxed);
  _exit(OracleFaultExit);
}
bool captureNativeFault(NativeFaultRecord &Record) {
  // Install only in the child. A piped system core collector can run even with
  // RLIMIT_CORE=0; record the actual signal/address without invoking it.
  NativeFault.store(&Record, std::memory_order_relaxed);
  struct sigaction Action{};
  Action.sa_sigaction = nativeFault;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  sigset_t Signal;
  sigemptyset(&Signal);
  sigaddset(&Signal, SIGSEGV);
  return sigaction(SIGSEGV, &Action, nullptr) == 0 &&
         sigprocmask(SIG_UNBLOCK, &Signal, nullptr) == 0;
}
void expectNativeFault(pid_t Child, const NativeFaultRecord &Record,
                       const uint8_t *Address) {
  int Status = 0;
  pid_t Waited;
  do {
    Waited = waitpid(Child, &Status, 0);
  } while (Waited < 0 && errno == EINTR);
  ASSERT_EQ(Waited, Child);
  ASSERT_TRUE(WIFEXITED(Status));
  EXPECT_EQ(WEXITSTATUS(Status), OracleFaultExit);
  EXPECT_EQ(Record.Signal.load(), SIGSEGV);
  EXPECT_EQ(Record.Code.load(), SEGV_ACCERR);
  EXPECT_EQ(Record.Address.load(), reinterpret_cast<uintptr_t>(Address));
}
#endif

TEST(X64CrossPageOracle, NativeFaultCannotCommitAnyPrefixStore) {
#if defined(__linux__) && defined(__x86_64__)
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto *DataBytes = static_cast<uint8_t *>(
        mmap(nullptr, OraclePages * PageSize, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(DataBytes, MAP_FAILED);
    auto ReleaseData =
        llvm::scope_exit([&] { munmap(DataBytes, OraclePages * PageSize); });
    auto *Record =
        new (DataBytes + OracleDataPages * PageSize) NativeFaultRecord;
    const uint64_t Offset = PageSize - Case.Width / 2;
    const std::array<uint64_t, 2> Before{Case.Before, High};
    std::memcpy(DataBytes + Offset, Before.data(), sizeof(Before));
    auto *CodeBytes =
        static_cast<uint8_t *>(mmap(nullptr, PageSize, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(CodeBytes, MAP_FAILED);
    auto ReleaseCode = llvm::scope_exit([&] { munmap(CodeBytes, PageSize); });
    std::memcpy(CodeBytes, NativePrefix, sizeof(NativePrefix));
    std::memcpy(CodeBytes + sizeof(NativePrefix), Case.Bytes.data(),
                Case.Bytes.size());
    CodeBytes[sizeof(NativePrefix) + Case.Bytes.size()] = ReturnOpcode;
    ASSERT_EQ(mprotect(CodeBytes, PageSize, PROT_READ | PROT_EXEC), 0);
    const pid_t Child = fork();
    ASSERT_GE(Child, 0);
    if (!Child) {
      if (!captureNativeFault(*Record) ||
          mprotect(DataBytes + PageSize, PageSize, PROT_NONE))
        _exit(OracleSetupFailure);
      using Function =
          void (*)(const uint64_t *, uint64_t, uint64_t, uint8_t *);
      reinterpret_cast<Function>(CodeBytes)(Before.data(), Low, 0,
                                            DataBytes + Offset);
      _exit(OracleUnexpectedReturn);
    }
    expectNativeFault(Child, *Record, DataBytes + PageSize);
    EXPECT_EQ(std::memcmp(DataBytes + Offset, Before.data(), sizeof(Before)),
              0);
  }
#else
  GTEST_SKIP() << NativeOracleUnavailable;
#endif
}

TEST(X64CrossPageOracle, NativeRepeatFaultPreservesCompletedElements) {
#if defined(__linux__) && defined(__x86_64__)
  auto *DataBytes = static_cast<uint8_t *>(
      mmap(nullptr, OraclePages * PageSize, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(DataBytes, MAP_FAILED);
  auto ReleaseData =
      llvm::scope_exit([&] { munmap(DataBytes, OraclePages * PageSize); });
  auto *Record = new (DataBytes + OracleDataPages * PageSize) NativeFaultRecord;
  auto *CodeBytes =
      static_cast<uint8_t *>(mmap(nullptr, PageSize, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(CodeBytes, MAP_FAILED);
  auto ReleaseCode = llvm::scope_exit([&] { munmap(CodeBytes, PageSize); });
  std::memcpy(CodeBytes, NativeRepeat, sizeof(NativeRepeat));
  ASSERT_EQ(mprotect(CodeBytes, PageSize, PROT_READ | PROT_EXEC), 0);
  const std::array<uint64_t, 2> Source{Low, High};
  const pid_t Child = fork();
  ASSERT_GE(Child, 0);
  if (!Child) {
    if (!captureNativeFault(*Record) ||
        mprotect(DataBytes + PageSize, PageSize, PROT_NONE))
      _exit(OracleSetupFailure);
    using Function = void (*)(const uint64_t *, uint8_t *, uint64_t);
    reinterpret_cast<Function>(CodeBytes)(
        Source.data(), DataBytes + PageSize - WordBytes, CopyCount);
    _exit(OracleUnexpectedReturn);
  }
  expectNativeFault(Child, *Record, DataBytes + PageSize);
  std::array<uint64_t, 2> Actual{};
  std::memcpy(Actual.data(), DataBytes + PageSize - WordBytes, sizeof(Actual));
  EXPECT_EQ(Actual, (std::array<uint64_t, 2>{Low, 0}));
#else
  GTEST_SKIP() << NativeOracleUnavailable;
#endif
}

INSTANTIATE_TEST_SUITE_P(
    Transports, X64CrossPageCopy,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::CheckedX64,
                                     ExecutionContract::CheckedUserX64)));
INSTANTIATE_TEST_SUITE_P(
    Transports, X64CrossPageDevice,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool(), testing::Bool()));

INSTANTIATE_TEST_SUITE_P(
    Transports, X64CrossPage,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::CheckedX64,
                                     ExecutionContract::CheckedUserX64),
                     testing::ValuesIn(Cases)));
} // namespace
} // namespace neverd::emulation
