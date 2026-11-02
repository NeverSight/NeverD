//===- X64WideAtomicTests.cpp - Wide compare-exchange execution ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64Exception.h"
#include "arch/x86_64/X64Machine.h"
#include "backends/MachineFactories.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Memory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(_M_X64)
#include <intrin.h>
#elif defined(__x86_64__)
#include <cpuid.h>
#endif

namespace neverd::emulation {
namespace {
#define NEVERD_WIDE_ATOMIC_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_WIDE_ATOMIC_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_WIDE_ATOMIC_BYTES(Name, ...)                                    \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64WideAtomicCases.def"
#undef NEVERD_WIDE_ATOMIC_BYTES
#undef NEVERD_WIDE_ATOMIC_TEXT
#undef NEVERD_WIDE_ATOMIC_VALUE
struct Instruction {
  const char *Name;
  unsigned Width;
  bool Locked;
  std::vector<uint8_t> Bytes;
};
const Instruction Instructions[] = {
#define NEVERD_WIDE_ATOMIC_CASE(Name, Width, Locked, ...)                      \
  {#Name, Width, Locked, {__VA_ARGS__}},
#include "X64WideAtomicCases.def"
#undef NEVERD_WIDE_ATOMIC_CASE
};
struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
  ExecutionContract Contract;
};
constexpr Backend Backends[] = {
#define NEVERD_WIDE_ATOMIC_BACKEND(Name, Kind, Contract)                       \
  {#Name, ExecutionBackendKind::Kind, ExecutionContract::Contract},
#include "X64WideAtomicCases.def"
#undef NEVERD_WIDE_ATOMIC_BACKEND
};
struct Parameter {
  Backend B;
  Instruction I;
  std::string name() const { return std::string(B.Name) + '_' + I.Name; }
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.name(); }
std::vector<Parameter> parameters() {
  std::vector<Parameter> Result;
  for (const auto &B : Backends)
    for (const auto &I : Instructions)
      Result.push_back({B, I});
  return Result;
}
using State = std::map<CPURegister, RegisterValue>;
class X64WideAtomic : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint8_t> Bytes;
  uint64_t Address = Data;
  const Instruction &instruction() const { return GetParam().I; }
  bool userMode() const {
    return GetParam().B.Contract == ExecutionContract::CheckedUserX64;
  }
  void SetUp() override { initialize(); }
  void initialize() {
    auto B =
        createExecutionBackend(GetParam().B.Kind, GetParam().B.Contract, Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, Page, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->write(Code, std::vector<uint8_t>(Page, Nop)));
    llvm::cantFail(CPU->map(Data, Page, Read | Write | UserAccessible));
    // Deliberately use different physical owners for a split operand.
    llvm::cantFail(CPU->map(Data + Page, Page, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->mapAlias(Alias, Data, Page * 2, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->write(Data, std::vector<uint8_t>(Page * 2, GuardValue)));
    Bytes = instruction().Bytes;
    llvm::cantFail(CPU->write(Code, Bytes));
    Address = Data;
    for (unsigned R = unsigned(X64Register::AX);
         R <= unsigned(X64Register::R15); ++R)
      llvm::cantFail(CPU->setReg(X64Register(R), OriginalLow + R));
    seed(0);
  }
  void seed(unsigned Mismatch) {
    const bool Pair64 = instruction().Width == Word;
    uint64_t Low = Pair64 ? AddressHigh | uint32_t(OriginalLow) : OriginalLow;
    uint64_t High =
        Pair64 ? AddressHigh | (OriginalLow >> HalfBits) : OriginalHigh;
    if (Mismatch == 1)
      Low ^= 1;
    if (Mismatch == 2)
      High ^= 1;
    llvm::cantFail(CPU->setReg(X64Register::AX, Low));
    llvm::cantFail(CPU->setReg(X64Register::DX, High));
    llvm::cantFail(CPU->setReg(X64Register::BX, ExchangeLow));
    llvm::cantFail(CPU->setReg(X64Register::CX, ExchangeHigh));
    llvm::cantFail(CPU->setReg(X64Register::SI, Address));
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
    llvm::cantFail(CPU->writeInteger(Address, OriginalLow, Word));
    llvm::cantFail(CPU->writeInteger(Address + Word, OriginalHigh, Word));
  }
  State snapshot() {
    State Result;
    for (unsigned R = unsigned(CPURegister::X64AX);
         R <= unsigned(CPURegister::X64FP7); ++R)
      if (registerMatches(CPURegister(R), GuestArchitecture::X64))
        Result[CPURegister(R)] =
            llvm::cantFail(CPU->readRegister(CPURegister(R)));
    return Result;
  }
  std::vector<uint8_t> memory() {
    std::vector<uint8_t> Result(Page * 2);
    llvm::cantFail(CPU->addressSpace()->read(Alias, Result));
    return Result;
  }
  uint64_t expectedLow(bool Match) const {
    return !Match ? OriginalLow
           : instruction().Width == Wide
               ? ExchangeLow
               : (uint64_t(uint32_t(ExchangeHigh)) << HalfBits) |
                     uint32_t(ExchangeLow);
  }
  State expected(State Before, bool Match) {
    Before[CPURegister::X64PC][0] += Bytes.size();
    Before[CPURegister::X64FLAGS][0] =
        Match ? Flags | ZeroFlag : Flags & ~ZeroFlag;
    if (!Match) {
      Before[CPURegister::X64AX][0] =
          instruction().Width == Wide ? OriginalLow : uint32_t(OriginalLow);
      Before[CPURegister::X64DX][0] =
          instruction().Width == Wide ? OriginalHigh : OriginalLow >> HalfBits;
    }
    return Before;
  }
  ExecutionExit run(BackendHooks H = {}) {
    H.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code) {
        EXPECT_EQ(PC, Code + Bytes.size());
        CPU->stop();
      }
    };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
};

TEST_P(X64WideAtomic,
       ComparisonResultsPublishTogetherAndObserversSeeOriginalState) {
  for (unsigned Mismatch : {0u, 1u, 2u}) {
    seed(Mismatch);
    const auto Before = snapshot();
    const auto RAM = memory();
    unsigned Reads = 0, Writes = 0;
    BackendHooks H;
    H.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, instruction().Width);
      ++Reads;
    };
    H.Write = [&](uint64_t A, unsigned Size, uint64_t Value) {
      EXPECT_EQ(A, Address + Writes * Word);
      EXPECT_EQ(Size, Word);
      EXPECT_EQ(Value, Writes ? (Mismatch ? OriginalHigh : ExchangeHigh)
                              : expectedLow(!Mismatch));
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(memory(), RAM);
      ++Writes;
    };
    const auto Exit = run(std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(Writes, instruction().Width / Word);
    EXPECT_EQ(snapshot(), expected(Before, !Mismatch));
    auto Want = RAM;
    llvm::support::endian::write64le(Want.data(), expectedLow(!Mismatch));
    if (instruction().Width == Wide)
      llvm::support::endian::write64le(Want.data() + Word,
                                       Mismatch ? OriginalHigh : ExchangeHigh);
    EXPECT_EQ(memory(), Want);
  }
}

TEST_P(X64WideAtomic, EveryObserverBoundaryCanCancelOrThrowWithoutPublishing) {
  for (bool Throw : {false, true})
    for (unsigned Boundary = 0; Boundary <= instruction().Width / Word;
         ++Boundary) {
      initialize();
      const auto Before = snapshot();
      const auto RAM = memory();
      unsigned Observations = 0;
      auto Observe = [&] {
        EXPECT_EQ(snapshot(), Before);
        EXPECT_EQ(memory(), RAM);
        if (Observations++ == Boundary) {
          if (Throw)
            throw std::runtime_error(ObserverFailure);
          CPU->stop();
        }
      };
      BackendHooks H;
      H.Read = [&](uint64_t, unsigned) { Observe(); };
      H.Write = [&](uint64_t, unsigned, uint64_t) { Observe(); };
      EXPECT_EQ(run(std::move(H)).Kind, Throw
                                            ? ExecutionExitKind::BackendFailure
                                            : ExecutionExitKind::Stopped);
      EXPECT_EQ(Observations, Boundary + 1);
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(memory(), RAM);
      if (!Throw) {
        EXPECT_EQ(run().Kind, ExecutionExitKind::Stopped);
        EXPECT_EQ(snapshot(), expected(Before, true));
      }
    }
}

TEST_P(X64WideAtomic, MissingAndDeniedOperandsAreWriteFaultsEvenOnMismatch) {
  for (bool Missing : {false, true})
    for (unsigned Mismatch : {0u, 1u, 2u}) {
      initialize();
      seed(Mismatch);
      const auto Before = snapshot();
      const auto RAM = memory();
      if (Missing)
        llvm::cantFail(CPU->addressSpace()->unmap(Data, Page));
      else
        llvm::cantFail(CPU->protect(Data, Page, Read | UserAccessible));
      unsigned Writes = 0;
      BackendHooks H;
      H.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
      H.RecoverableFault = [&](const BackendFault &F) {
        EXPECT_EQ(F.Access, BackendAccessKind::Write);
        EXPECT_EQ(F.Kind, Missing ? BackendFaultKind::UnmappedMemory
                                  : BackendFaultKind::Protection);
        EXPECT_EQ(F.Address, Address);
        EXPECT_EQ(snapshot(), Before);
        EXPECT_EQ(memory(), RAM);
        return true;
      };
      EXPECT_EQ(run(std::move(H)).Kind, ExecutionExitKind::RecoverableFault);
      EXPECT_EQ(Writes, 0u);
      ASSERT_TRUE(CPU->takeRecoverableFault());
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(memory(), RAM);
      if (Missing)
        llvm::cantFail(
            CPU->mapAlias(Data, Alias, Page, Read | Write | UserAccessible));
      else
        llvm::cantFail(CPU->protect(Data, Page, Read | Write | UserAccessible));
      EXPECT_EQ(run().Kind, ExecutionExitKind::Stopped);
      EXPECT_EQ(snapshot(), expected(Before, !Mismatch));
    }
}

TEST_P(X64WideAtomic, AlignmentPrecedesMemoryAndAllowsAddressRepair) {
  for (bool Missing : {false, true}) {
    initialize();
    llvm::cantFail(CPU->setReg(X64Register::SI, Address + 1));
    const auto Before = snapshot();
    const auto RAM = memory();
    if (Missing)
      llvm::cantFail(CPU->addressSpace()->unmap(Data, Page));
    else
      llvm::cantFail(CPU->protect(Data, Page, UserAccessible));
    unsigned Observations = 0;
    BackendHooks H;
    H.Read = [&](uint64_t, unsigned) { ++Observations; };
    H.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
    H.RecoverableFault = [](const BackendFault &) { return true; };
    const auto Exit = run(std::move(H));
    if (instruction().Width == Wide) {
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault);
      const auto Fault = CPU->takeRecoverableFault();
      ASSERT_TRUE(Fault);
      EXPECT_EQ(Fault->Interrupt, GeneralProtection);
      EXPECT_EQ(Fault->ErrorCode, 0u);
      EXPECT_EQ(Fault->Cause, BackendFaultCause::OperandAlignment);
      EXPECT_EQ(Observations, 0u);
    } else if (instruction().Locked) {
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
      EXPECT_EQ(Observations, 0u);
      EXPECT_EQ(snapshot(), Before);
      EXPECT_EQ(memory(), RAM);
      continue;
    } else {
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault);
      ASSERT_TRUE(CPU->takeRecoverableFault());
    }
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(), RAM);
    if (Missing)
      llvm::cantFail(
          CPU->mapAlias(Data, Alias, Page, Read | Write | UserAccessible));
    else
      llvm::cantFail(CPU->protect(Data, Page, Read | Write | UserAccessible));
    llvm::cantFail(CPU->setReg(X64Register::SI, Address));
    const auto Repaired = snapshot();
    EXPECT_EQ(run().Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(snapshot(), expected(Repaired, true));
  }
}

TEST_P(X64WideAtomic,
       AddressWrappingSegmentsAndPhysicalAliasesUseTheOriginalOperand) {
  for (uint8_t Prefix : {uint8_t(0), uint8_t(FS), uint8_t(GS)}) {
    initialize();
    Bytes.insert(Bytes.begin(), AddressPrefix);
    if (Prefix)
      Bytes.insert(Bytes.begin(), Prefix);
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->setReg(X64Register::SI,
                               AddressHigh | (Alias - (Prefix ? Segment : 0))));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FSBase, {Segment, 0}));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64GSBase, {Segment, 0}));
    const auto Before = snapshot();
    unsigned Writes = 0;
    BackendHooks H;
    H.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Alias);
      EXPECT_EQ(Size, instruction().Width);
    };
    H.Write = [&](uint64_t A, unsigned, uint64_t) {
      EXPECT_EQ(A, Alias + Writes++ * Word);
    };
    EXPECT_EQ(run(std::move(H)).Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(snapshot(), expected(Before, true));
    EXPECT_EQ(Writes, instruction().Width / Word);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, Word)), expectedLow(true));
  }
}

TEST_P(X64WideAtomic, WholeOperandPermissionsProtectCrossPageResults) {
  initialize();
  Address =
      Data + Page -
      (instruction().Width == Wide || instruction().Locked ? instruction().Width
                                                           : Word / 2);
  seed(0);
  const auto Before = snapshot();
  const auto RAM = memory();
  const bool Crosses = Address + instruction().Width > Data + Page;
  const uint64_t Guard = Crosses ? Data + Page : Data;
  llvm::cantFail(CPU->protect(Guard, Page, Read | UserAccessible));
  BackendHooks H;
  H.RecoverableFault = [](const BackendFault &) { return true; };
  EXPECT_EQ(run(std::move(H)).Kind, ExecutionExitKind::RecoverableFault);
  const auto Fault = CPU->takeRecoverableFault();
  ASSERT_TRUE(Fault);
  EXPECT_EQ(Fault->Address, Crosses ? Data + Page : Address);
  EXPECT_EQ(Fault->Access, BackendAccessKind::Write);
  EXPECT_EQ(snapshot(), Before);
  EXPECT_EQ(memory(), RAM);
  llvm::cantFail(CPU->protect(Guard, Page, Read | Write | UserAccessible));
  EXPECT_EQ(run().Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(snapshot(), expected(Before, true));
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, Word)), expectedLow(true));
}

TEST_P(X64WideAtomic, GuestPrivilegeAndContextRestorationRetainCommittedRAM) {
  const auto Saved = llvm::cantFail(CPU->saveContext());
  const auto Before = snapshot();
  llvm::cantFail(CPU->protect(Data, Page, Read | Write));
  BackendHooks H;
  H.RecoverableFault = [](const BackendFault &) { return true; };
  const auto Exit = run(std::move(H));
  if (userMode()) {
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault);
    const auto Fault = CPU->takeRecoverableFault();
    ASSERT_TRUE(Fault);
    EXPECT_EQ(Fault->Access, BackendAccessKind::Write);
    EXPECT_EQ(snapshot(), Before);
    llvm::cantFail(CPU->protect(Data, Page, Read | Write | UserAccessible));
    EXPECT_EQ(run().Kind, ExecutionExitKind::Stopped);
  } else
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(snapshot(), expected(Before, true));
  const auto Committed = memory();
  llvm::cantFail(CPU->restoreContext(*Saved));
  EXPECT_EQ(snapshot(), Before);
  EXPECT_EQ(memory(), Committed);
  // The restored old comparand must now fail against the new RAM.
  EXPECT_EQ(run().Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)) & ZeroFlag, 0u);
  EXPECT_EQ(memory(), Committed);
}

TEST_P(X64WideAtomic, RejectedRepeatPrefixesHaveNoEffects) {
  constexpr uint8_t Prefixes[] = {
#define NEVERD_WIDE_ATOMIC_REJECTED_PREFIX(Value) Value,
#include "X64WideAtomicCases.def"
#undef NEVERD_WIDE_ATOMIC_REJECTED_PREFIX
  };
  for (uint8_t Prefix : Prefixes) {
    initialize();
    Bytes.insert(Bytes.begin(), Prefix);
    llvm::cantFail(CPU->write(Code, Bytes));
    const auto Before = snapshot();
    const auto RAM = memory();
    unsigned Observations = 0;
    BackendHooks H;
    H.Read = [&](uint64_t, unsigned) { ++Observations; };
    H.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
    EXPECT_EQ(run(std::move(H)).Kind, ExecutionExitKind::UnsupportedOperation);
    EXPECT_EQ(Observations, 0u);
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(memory(), RAM);
  }
}

TEST_P(X64WideAtomic, DeviceOperandsNeverEnterCallbacksOrPublishEffects) {
  unsigned Reads = 0, Writes = 0, Observations = 0;
  GuestMMIOCallbacks DeviceHooks;
  DeviceHooks.Validate = [&](uint64_t, unsigned, bool) {
    ++Observations;
    return llvm::Error::success();
  };
  DeviceHooks.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++Reads;
    return OriginalLow;
  };
  DeviceHooks.Write = [&](uint64_t, unsigned, uint64_t) {
    ++Writes;
    return llvm::Error::success();
  };
  auto Mapped = CPU->mapMMIO(Device, Page, std::move(DeviceHooks));
  if (userMode()) {
    EXPECT_TRUE(bool(Mapped));
    llvm::consumeError(std::move(Mapped));
    return;
  }
  ASSERT_FALSE(bool(Mapped)) << llvm::toString(std::move(Mapped));
  llvm::cantFail(CPU->setReg(X64Register::SI, Device));
  const auto Before = snapshot();
  const auto RAM = memory();
  BackendHooks H;
  H.Read = [&](uint64_t, unsigned) { ++Observations; };
  H.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
  EXPECT_EQ(run(std::move(H)).Kind, ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(Writes, 0u);
  EXPECT_EQ(Observations, 0u);
  EXPECT_EQ(snapshot(), Before);
  EXPECT_EQ(memory(), RAM);
}

TEST_P(X64WideAtomic, ResultsMatchIndependentOriginalHostInstructions) {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned Features = 0;
#if defined(_M_X64)
  int Registers[4];
  __cpuid(Registers, CPUIDLeaf);
  Features = Registers[2];
#else
  unsigned A, B, D;
  __get_cpuid(CPUIDLeaf, &A, &B, &Features, &D);
#endif
  if (!(Features & CX16Feature))
    GTEST_SKIP() << OracleUnavailable;
  std::vector<uint8_t> Oracle(std::begin(OraclePrefix), std::end(OraclePrefix));
#ifdef _WIN32
  Oracle.insert(Oracle.end(), std::begin(Win64Argument),
                std::end(Win64Argument));
#else
  Oracle.insert(Oracle.end(), std::begin(SysVArgument), std::end(SysVArgument));
#endif
  Oracle.insert(Oracle.end(), std::begin(OracleLoad), std::end(OracleLoad));
  Oracle.insert(Oracle.end(), Bytes.begin(), Bytes.end());
  Oracle.insert(Oracle.end(), std::begin(OracleSave), std::end(OracleSave));
  std::error_code EC;
  auto Block = llvm::sys::Memory::allocateMappedMemory(
      Page, nullptr, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE,
      EC);
  ASSERT_FALSE(bool(EC)) << EC.message();
  auto Release = llvm::scope_exit(
      [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
  std::memcpy(Block.base(), Oracle.data(), Oracle.size());
  EC = llvm::sys::Memory::protectMappedMemory(
      Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
  ASSERT_FALSE(bool(EC)) << EC.message();
  llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Oracle.size());
  const auto Execute = reinterpret_cast<void (*)(uint64_t *)>(Block.base());
  for (unsigned Mismatch : {0u, 1u, 2u}) {
    seed(Mismatch);
    alignas(Wide) std::array<uint64_t, Wide / Word> RAM{OriginalLow,
                                                        OriginalHigh};
    const auto Before = snapshot();
    std::array<uint64_t, PacketWords> Packet{
        Before.at(CPURegister::X64AX)[0],
        Before.at(CPURegister::X64DX)[0],
        ExchangeLow,
        ExchangeHigh,
        Flags,
        reinterpret_cast<uint64_t>(RAM.data())};
    Execute(Packet.data());
    auto Want = Before;
    Want[CPURegister::X64AX][0] = Packet[0];
    Want[CPURegister::X64DX][0] = Packet[1];
    Want[CPURegister::X64BX][0] = Packet[2];
    Want[CPURegister::X64CX][0] = Packet[3];
    // The host fixes IF. All admitted arithmetic/direction flags are compared.
    Want[CPURegister::X64FLAGS][0] = Packet[4] & Flags;
    Want[CPURegister::X64PC][0] += Bytes.size();
    EXPECT_EQ(run().Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(snapshot(), Want);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, Word)), RAM[0]);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address + Word, Word)), RAM[1]);
  }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64WideAtomic,
                         testing::ValuesIn(parameters()),
                         [](const testing::TestParamInfo<Parameter> &P) {
                           return P.param.name();
                         });

// Bypass the shared instruction admission. Real processor faults independently
// establish write access and alignment priority for successful/failed compares.
class NativeX64WideAtomic : public testing::TestWithParam<Parameter> {};
TEST_P(NativeX64WideAtomic, HardwareFaultsRetainStateAndClassifyWriteAccess) {
  const auto &P = GetParam();
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  auto Created = P.B.Kind == ExecutionBackendKind::KVM
                     ? createKvmMachine(*Memory)
                     : createWhpMachine(*Memory);
  if (!Created) {
    auto E = Created.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  auto Machine = std::move(*Created);
  llvm::cantFail(
      Memory->map(Code, Page, Read | Write | Execute | UserAccessible));
  llvm::cantFail(Memory->map(Data, Page, Read | UserAccessible));
  llvm::cantFail(Memory->write(Code, P.I.Bytes));
  std::vector<uint64_t> Addresses{Data, Alias};
  if (P.I.Width == Word && !P.I.Locked)
    Addresses.push_back(Data + Page - Word / 2);
  for (uint64_t A : Addresses)
    for (unsigned Mismatch : {0u, 1u})
      for (bool Misaligned : {false, true}) {
        if (Misaligned && P.I.Width != Wide)
          continue;
        X64MachineState State;
        State.UserMode = P.B.Contract == ExecutionContract::CheckedUserX64;
        State.reg(X64Register::PC) = Code;
        State.reg(X64Register::SI) = A + Misaligned;
        State.reg(X64Register::FLAGS) = Flags;
        State.reg(X64Register::AX) = Mismatch;
        State.reg(X64Register::BX) = ExchangeLow;
        State.reg(X64Register::CX) = ExchangeHigh;
        const auto Before = State;
        llvm::cantFail(Memory->beginRun());
        auto Release = llvm::scope_exit([&] { Memory->endRun(); });
        const auto Root = llvm::cantFail(buildX64PageTables(
            *Memory, State.UserMode, Machine->requiresExceptionMonitor()));
        bool Caught = false;
        auto E = Machine->step(State, Root,
                               {std::chrono::steady_clock::now() +
                                std::chrono::microseconds(Timeout)});
        auto Remaining = llvm::handleErrors(
            std::move(E), [&](const X64ExceptionError &Fault) {
              Caught = true;
              EXPECT_EQ(Fault.exception().Vector,
                        Misaligned ? GeneralProtection : PageFault);
              if (Misaligned) {
                EXPECT_EQ(Fault.exception().ErrorCode, 0u);
                EXPECT_EQ(Fault.exception().FaultAddress, std::nullopt);
              } else {
                ASSERT_TRUE(Fault.exception().ErrorCode);
                EXPECT_EQ(*Fault.exception().ErrorCode & WriteFault,
                          WriteFault);
                EXPECT_EQ(Fault.exception().FaultAddress, A);
              }
            });
        ASSERT_FALSE(bool(Remaining)) << llvm::toString(std::move(Remaining));
        ASSERT_TRUE(Caught);
        EXPECT_EQ(State, Before);
      }
}
std::vector<Parameter> nativeParameters() {
  auto Result = parameters();
  std::erase_if(Result, [](const Parameter &P) {
    return P.B.Kind == ExecutionBackendKind::Unicorn ||
           P.B.Contract == ExecutionContract::Legacy;
  });
  return Result;
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, NativeX64WideAtomic,
                         testing::ValuesIn(nativeParameters()),
                         [](const testing::TestParamInfo<Parameter> &P) {
                           return P.param.name();
                         });
} // namespace
} // namespace neverd::emulation
