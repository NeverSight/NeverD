//===- X64BitStringTests.cpp - Bit-string addresses and native effects ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "../UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_BIT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_BIT_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_BIT_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_BIT_CASE(Name, Size, Writes, Immediate, ...)                    \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64BitStringCases.def"
#undef NEVERD_BIT_CASE
#undef NEVERD_BIT_BYTES
#undef NEVERD_BIT_TEXT
#undef NEVERD_BIT_VALUE

struct BitCase {
  const char *Name;
  unsigned Size;
  bool Writes, Immediate;
  llvm::ArrayRef<uint8_t> Bytes;
};
constexpr BitCase Cases[] = {
#define NEVERD_BIT_CASE(Name, Size, Writes, Immediate, ...)                    \
  {#Name, Size, Writes, Immediate, Name},
#include "X64BitStringCases.def"
#undef NEVERD_BIT_CASE
};
struct BitIndex {
  int64_t Value, WordOffset, DWordOffset, QWordOffset;
  int64_t offset(unsigned Size) const {
    return Size == sizeof(uint16_t)   ? WordOffset
           : Size == sizeof(uint32_t) ? DWordOffset
                                      : QWordOffset;
  }
};
constexpr BitIndex Indices[] = {
#define NEVERD_BIT_INDEX(Value, Word, DWord, QWord) {Value, Word, DWord, QWord},
#include "X64BitStringCases.def"
#undef NEVERD_BIT_INDEX
};
struct Parameter {
  const char *Name;
  ExecutionBackendKind Backend;
  bool User;
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.Name; }
constexpr Parameter Parameters[] = {
#define NEVERD_BIT_BACKEND(Name, Backend, User)                                \
  {#Name, ExecutionBackendKind::Backend, User},
#include "X64BitStringCases.def"
#undef NEVERD_BIT_BACKEND
};
enum class CrossingFault { Unmapped, ReadDenied, WriteDenied };

struct HostState {
  uint64_t Index, Flags, Base;
};
static_assert(offsetof(HostState, Flags) == WordBytes);
static_assert(offsetof(HostState, Base) == 2 * WordBytes);

void runHost(llvm::ArrayRef<uint8_t> Instruction, HostState &State) {
#if defined(__x86_64__) || defined(_M_X64)
  std::vector<uint8_t> Bytes(std::begin(OraclePrefix), std::end(OraclePrefix));
#ifdef _WIN32
  Bytes.insert(Bytes.end(), std::begin(Win64Argument), std::end(Win64Argument));
#else
  Bytes.insert(Bytes.end(), std::begin(SysVArgument), std::end(SysVArgument));
#endif
  Bytes.insert(Bytes.end(), std::begin(OracleLoad), std::end(OracleLoad));
  Bytes.insert(Bytes.end(), Instruction.begin(), Instruction.end());
  Bytes.insert(Bytes.end(), std::begin(OracleSave), std::end(OracleSave));
  std::error_code EC;
  auto Block = llvm::sys::Memory::allocateMappedMemory(
      PageSize, nullptr,
      llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
  ASSERT_FALSE(bool(EC)) << EC.message();
  auto Release = llvm::scope_exit(
      [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
  std::memcpy(Block.base(), Bytes.data(), Bytes.size());
  EC = llvm::sys::Memory::protectMappedMemory(
      Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
  ASSERT_FALSE(bool(EC)) << EC.message();
  llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Bytes.size());
  reinterpret_cast<void (*)(HostState *)>(Block.base())(&State);
#else
  ADD_FAILURE() << OracleUnavailable;
#endif
}

class X64BitString : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::array<uint8_t, BufferBytes> Before{};
  void SetUp() override { resetCPU(); }
  void resetCPU() {
    CPU.reset();
    auto B = createExecutionBackend(GetParam().Backend,
                                    GetParam().User
                                        ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedX64,
                                    Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(GetParam().Backend, GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->mapAlias(Alias, Data, PageSize, Read | Write | UserAccessible));
    for (size_t I = 0; I < Before.size(); ++I)
      Before[I] = uint8_t(Seed + I * PatternStep);
  }
  uint64_t reg(X64Register R) { return llvm::cantFail(CPU->reg(R)); }
  void seed(uint64_t Index, uint64_t Base = Data + BaseOffset) {
    llvm::cantFail(CPU->write(Data, Before));
    llvm::cantFail(CPU->setReg(X64Register::DX, Index));
    llvm::cantFail(CPU->setReg(X64Register::CX, Base));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
  }
  ExecutionExit run(llvm::ArrayRef<uint8_t> Instruction,
                    BackendHooks Hooks = {}) {
    std::vector<uint8_t> Bytes(Instruction.begin(), Instruction.end());
    Bytes.push_back(Nop);
    auto Failed = [](llvm::Error E) {
      auto Reason = llvm::toString(std::move(E));
      ADD_FAILURE() << Reason;
      return ExecutionExit{ExecutionExitKind::BackendFailure, std::nullopt,
                           Reason};
    };
    if (auto E = CPU->write(Code, Bytes))
      return Failed(std::move(E));
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    if (auto E = CPU->installHooks(std::move(Hooks)))
      return Failed(std::move(E));
    auto Exit = CPU->runUntilExit(Code, Timeout);
    if (!Exit)
      return Failed(Exit.takeError());
    return std::move(*Exit);
  }
  void expectBytes(llvm::ArrayRef<uint8_t> Expected) {
    std::array<uint8_t, BufferBytes> Actual{};
    llvm::cantFail(CPU->addressSpace()->read(Data, Actual));
    EXPECT_EQ(llvm::ArrayRef(Actual), Expected);
    llvm::cantFail(CPU->addressSpace()->read(Alias, Actual));
    EXPECT_EQ(llvm::ArrayRef(Actual), Expected);
  }
  std::array<uint8_t, WordBytes> word(uint64_t Address) {
    std::array<uint8_t, WordBytes> Bytes{};
    llvm::cantFail(CPU->addressSpace()->read(Address, Bytes));
    return Bytes;
  }
  void mapCrossingPages() {
    // Interpose another physical allocation before the adjacent virtual page.
    llvm::cantFail(CPU->map(Stack, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->map(Data + PageSize, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->mapAlias(Alias + PageSize, Data + PageSize, PageSize,
                                 Read | Write | UserAccessible));
  }
  void checkMemory(const BitCase &C, bool Locked, uint64_t Index,
                   int64_t Offset) {
    SCOPED_TRACE(C.Name);
    SCOPED_TRACE(Index);
    SCOPED_TRACE(Locked);
    std::vector<uint8_t> Bytes;
    if (Locked)
      Bytes.push_back(Lock);
    Bytes.insert(Bytes.end(), C.Bytes.begin(), C.Bytes.end());
    alignas(WordBytes) auto Expected = Before;
    HostState Host{Index, Flags,
                   reinterpret_cast<uintptr_t>(Expected.data() + BaseOffset)};
    runHost(Bytes, Host);
    ASSERT_FALSE(HasFatalFailure());
    seed(Index);
    const uint64_t Address = Data + BaseOffset + (C.Immediate ? 0 : Offset);
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, C.Size);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t A, unsigned Size, uint64_t Value) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, C.Size);
      uint64_t ExpectedValue = 0;
      for (unsigned I = 0; I < C.Size; ++I)
        ExpectedValue |= uint64_t(Expected[Address - Data + I])
                         << (I * CHAR_BIT);
      EXPECT_EQ(Value, ExpectedValue);
      expectBytes(Before);
      EXPECT_EQ(reg(X64Register::FLAGS), Flags);
      EXPECT_EQ(reg(X64Register::PC), Code);
      ++Writes;
    };
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(Writes, unsigned(C.Writes));
    expectBytes(Expected);
    EXPECT_EQ(reg(X64Register::FLAGS) & DefinedFlags,
              Host.Flags & DefinedFlags);
    EXPECT_EQ(reg(X64Register::DX), Index);
    EXPECT_EQ(reg(X64Register::CX), Data + BaseOffset);
    EXPECT_EQ(reg(X64Register::PC), Code + Bytes.size());
  }
};

TEST_P(X64BitString, MemoryResultsAndEffectiveAddressesMatchHost) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases)
    for (bool Locked : {false, true}) {
      if (Locked && !C.Writes)
        continue;
      for (const auto &Index : Indices) {
        checkMemory(C, Locked, uint64_t(Index.Value), Index.offset(C.Size));
        ASSERT_FALSE(HasFatalFailure());
      }
    }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64BitString, RegisterResultsMatchHostAndPreserveUntouchedBits) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    std::vector<uint8_t> Bytes(C.Bytes.begin(), C.Bytes.end());
    Bytes[Bytes.size() - (C.Immediate ? ImmediateModRM : LastModRM)] |=
        RegisterMode;
    for (const auto &Index : Indices) {
      HostState Host{uint64_t(Index.Value), Flags, TargetBefore};
      runHost(Bytes, Host);
      ASSERT_FALSE(HasFatalFailure());
      seed(uint64_t(Index.Value), TargetBefore);
      unsigned Accesses = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
      const auto Exit = run(Bytes, std::move(Hooks));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(Accesses, 0u);
      EXPECT_EQ(reg(X64Register::CX), Host.Base);
      EXPECT_EQ(reg(X64Register::DX), Host.Index);
      EXPECT_EQ(reg(X64Register::FLAGS) & DefinedFlags,
                Host.Flags & DefinedFlags);
      expectBytes(Before);
    }
  }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64BitString, NarrowIndicesIgnoreUpperRegisterBits) {
#if defined(__x86_64__) || defined(_M_X64)
  for (const auto &C : Cases) {
    if (C.Immediate || C.Size == WordBytes)
      continue;
    for (const auto &Index : Indices) {
      const auto Mask = UINT64_MAX >> ((WordBytes - C.Size) * CHAR_BIT);
      const auto Value = (NarrowIndex & ~Mask) | (uint64_t(Index.Value) & Mask);
      checkMemory(C, C.Writes, Value, Index.offset(C.Size));
      ASSERT_FALSE(HasFatalFailure());
    }
  }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64BitString, ReadAndResultStopsRetainOriginalRAMAndCPU) {
  for (const auto &C : Cases) {
    if (!C.Writes)
      continue;
    for (bool Locked : {false, true}) {
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(Locked);
      std::vector<uint8_t> Bytes;
      if (Locked)
        Bytes.push_back(Lock);
      Bytes.insert(Bytes.end(), C.Bytes.begin(), C.Bytes.end());
      for (bool ReadStop : {true, false}) {
        seed(UINT64_MAX);
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t, unsigned) {
          ++Reads;
          if (ReadStop)
            CPU->stop();
        };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) {
          expectBytes(Before);
          ++Writes;
          CPU->stop();
        };
        const auto Exit = run(Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(Writes, unsigned(!ReadStop));
        EXPECT_EQ(reg(X64Register::DX), UINT64_MAX);
        EXPECT_EQ(reg(X64Register::CX), Data + BaseOffset);
        EXPECT_EQ(reg(X64Register::FLAGS), Flags);
        EXPECT_EQ(reg(X64Register::PC), Code);
        expectBytes(Before);
      }
    }
  }
}

TEST_P(X64BitString, ResultObserverExceptionDiscardsNativeEffects) {
  seed(UINT64_MAX);
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t, unsigned, uint64_t) {
    expectBytes(Before);
    throw std::runtime_error(ObserverException);
  };
  EXPECT_EQ(run(Set64Register, std::move(Hooks)).Kind,
            ExecutionExitKind::BackendFailure);
  expectBytes(Before);
  EXPECT_EQ(reg(X64Register::PC), Code);
  EXPECT_EQ(reg(X64Register::FLAGS), Flags);
}

TEST_P(X64BitString, ProtectionChecksUseTheAdjustedWordRatherThanTheBitBase) {
  seed(UINT64_MAX, Data + PageSize);
  llvm::cantFail(
      CPU->writeInteger(Data + PageSize - WordBytes, TargetBefore, WordBytes));
  const auto Original = word(Data + PageSize - WordBytes);
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
  const auto Exit = run(Set64Register, std::move(Hooks));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
  EXPECT_EQ(Exit.Fault->Address, Data + PageSize - WordBytes);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Write);
  EXPECT_EQ(Writes, 0u);
  EXPECT_EQ(word(Data + PageSize - WordBytes), Original);
  EXPECT_EQ(reg(X64Register::PC), Code);
  EXPECT_EQ(reg(X64Register::FLAGS), Flags);
  expectBytes(Before);
}

TEST_P(X64BitString, MissingNegativeOffsetWordRaisesAReadFault) {
  seed(UINT64_MAX, Data);
  const auto Exit = run(Test64Register);
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Address, Data - WordBytes);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
  EXPECT_EQ(reg(X64Register::PC), Code);
  EXPECT_EQ(reg(X64Register::FLAGS), Flags);
  expectBytes(Before);
}

TEST_P(X64BitString, ReadOnlyWordCanPrecedeAnUnmappedBase) {
  seed(UINT64_MAX, Data + PageSize);
  const uint64_t Address = Data + PageSize - WordBytes;
  llvm::cantFail(CPU->writeInteger(Address, TargetAfter, WordBytes));
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  unsigned Reads = 0, Writes = 0;
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t A, unsigned Size) {
    EXPECT_EQ(A, Address);
    EXPECT_EQ(Size, WordBytes);
    ++Reads;
  };
  Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
  const auto Exit = run(Test64Register, std::move(Hooks));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Reads, 1u);
  EXPECT_EQ(Writes, 0u);
  EXPECT_EQ(reg(X64Register::FLAGS) & DefinedFlags, DefinedFlags);
  EXPECT_EQ(reg(X64Register::CX), Data + PageSize);
  EXPECT_EQ(reg(X64Register::PC), Code + sizeof(Test64Register));
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)), TargetAfter);
  expectBytes(Before);
}

TEST_P(X64BitString, CrossPageResultsAndStopsMatchHostWithIndependentBacking) {
#if defined(__x86_64__) || defined(_M_X64)
  mapCrossingPages();
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    for (unsigned Prefix = 1; Prefix < C.Size; ++Prefix) {
      SCOPED_TRACE(Prefix);
      const uint64_t Address = Data + PageSize - Prefix;
      for (bool Stop : {false, true}) {
        if (Stop && !C.Writes)
          continue;
        seed(UINT64_MAX, Address + (C.Immediate ? 0 : C.Size));
        llvm::cantFail(CPU->writeInteger(Address, TargetBefore, WordBytes));
        const auto Original = word(Address);
        auto Expected = Original;
        HostState Host{UINT64_MAX, Flags,
                       reinterpret_cast<uintptr_t>(Expected.data()) +
                           (C.Immediate ? 0 : C.Size)};
        runHost(C.Bytes, Host);
        ASSERT_FALSE(HasFatalFailure());
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t A, unsigned Size) {
          EXPECT_EQ(A, Address);
          EXPECT_EQ(Size, C.Size);
          EXPECT_EQ(word(Address), Original);
          ++Reads;
        };
        Hooks.Write = [&](uint64_t A, unsigned Size, uint64_t Value) {
          EXPECT_EQ(A, Address);
          EXPECT_EQ(Size, C.Size);
          uint64_t ExpectedValue = 0;
          for (unsigned I = 0; I < C.Size; ++I)
            ExpectedValue |= uint64_t(Expected[I]) << (I * CHAR_BIT);
          EXPECT_EQ(Value, ExpectedValue);
          EXPECT_EQ(word(Address), Original);
          EXPECT_EQ(reg(X64Register::PC), Code);
          EXPECT_EQ(reg(X64Register::FLAGS), Flags);
          ++Writes;
          if (Stop)
            CPU->stop();
        };
        const auto Exit = run(C.Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(Writes, unsigned(C.Writes));
        EXPECT_EQ(word(Address), Stop ? Original : Expected);
        EXPECT_EQ(word(Alias + PageSize - Prefix), Stop ? Original : Expected);
        EXPECT_EQ(reg(X64Register::PC), Code + (Stop ? 0 : C.Bytes.size()));
        EXPECT_EQ(reg(X64Register::FLAGS) & DefinedFlags,
                  (Stop ? Flags : Host.Flags) & DefinedFlags);
        EXPECT_EQ(reg(X64Register::DX), UINT64_MAX);
        EXPECT_EQ(reg(X64Register::CX), Address + (C.Immediate ? 0 : C.Size));
      }
    }
  }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64BitString, CrossPageFaultPreservesBothAllocationsAndCPU) {
  mapCrossingPages();
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    for (unsigned Prefix = 1; Prefix < C.Size; ++Prefix) {
      SCOPED_TRACE(Prefix);
      const uint64_t Address = Data + PageSize - Prefix;
      for (auto Fault : {CrossingFault::Unmapped, CrossingFault::ReadDenied,
                         CrossingFault::WriteDenied}) {
        if (Fault == CrossingFault::WriteDenied && !C.Writes)
          continue;
        SCOPED_TRACE(int(Fault));
        seed(UINT64_MAX, Address + (C.Immediate ? 0 : C.Size));
        llvm::cantFail(CPU->writeInteger(Address, TargetBefore, WordBytes));
        const auto Original = word(Address);
        if (Fault == CrossingFault::Unmapped)
          llvm::cantFail(CPU->addressSpace()->unmap(Data + PageSize, PageSize));
        else
          llvm::cantFail(CPU->protect(
              Data + PageSize, PageSize,
              UserAccessible |
                  (Fault == CrossingFault::WriteDenied ? Read : 0)));
        unsigned Writes = 0;
        BackendHooks Hooks;
        Hooks.RecoverableFault = [](const BackendFault &) { return true; };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
        const auto Exit = run(C.Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
            << Exit.Diagnostic;
        ASSERT_TRUE(Exit.Fault);
        ASSERT_TRUE(CPU->takeRecoverableFault());
        EXPECT_FALSE(CPU->takeRecoverableFault());
        EXPECT_EQ(Exit.Fault->Kind, Fault == CrossingFault::Unmapped
                                        ? BackendFaultKind::UnmappedMemory
                                        : BackendFaultKind::Protection);
        EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
        EXPECT_EQ(Exit.Fault->Size, C.Size - Prefix);
        EXPECT_EQ(Exit.Fault->Access, Fault == CrossingFault::WriteDenied
                                          ? BackendAccessKind::Write
                                          : BackendAccessKind::Read);
        EXPECT_EQ(Writes, 0u);
        EXPECT_EQ(word(Alias + PageSize - Prefix), Original);
        EXPECT_EQ(reg(X64Register::FLAGS), Flags);
        EXPECT_EQ(reg(X64Register::PC), Code);
        EXPECT_EQ(reg(X64Register::CX), Address + (C.Immediate ? 0 : C.Size));
        EXPECT_EQ(reg(X64Register::DX), UINT64_MAX);
        if (Fault == CrossingFault::Unmapped)
          llvm::cantFail(CPU->mapAlias(Data + PageSize, Alias + PageSize,
                                       PageSize,
                                       Read | Write | UserAccessible));
        else
          llvm::cantFail(CPU->protect(Data + PageSize, PageSize,
                                      Read | Write | UserAccessible));
      }
    }
  }
}

TEST_P(X64BitString, RejectedLockFormsHaveNoEffectsOrObservations) {
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    for (bool Register : {false, true}) {
      resetCPU();
      ASSERT_FALSE(HasFatalFailure());
      ASSERT_FALSE(IsSkipped());
      seed(UINT64_MAX, Data + BaseOffset + (C.Writes && !Register ? 1 : 0));
      const auto Base = reg(X64Register::CX);
      std::vector<uint8_t> Bytes{Lock};
      Bytes.insert(Bytes.end(), C.Bytes.begin(), C.Bytes.end());
      if (Register)
        Bytes[Bytes.size() - (C.Immediate ? ImmediateModRM : LastModRM)] |=
            RegisterMode;
      unsigned Observations = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Observations; };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
      const auto Exit = run(Bytes, std::move(Hooks));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
          << Exit.Diagnostic;
      EXPECT_EQ(Observations, 0u);
      EXPECT_EQ(reg(X64Register::PC), Code);
      EXPECT_EQ(reg(X64Register::FLAGS), Flags);
      EXPECT_EQ(reg(X64Register::CX), Base);
      EXPECT_EQ(reg(X64Register::DX), UINT64_MAX);
      expectBytes(Before);
    }
  }
}

TEST_P(X64BitString, DeviceOperandsRejectBeforeCallbacks) {
  unsigned Callbacks = 0;
  GuestMMIOCallbacks Device;
  Device.Validate = [&](uint64_t, unsigned, bool) {
    ++Callbacks;
    return llvm::Error::success();
  };
  Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++Callbacks;
    return TargetBefore;
  };
  Device.Write = [&](uint64_t, unsigned, uint64_t) {
    ++Callbacks;
    return llvm::Error::success();
  };
  if (GetParam().User) {
    auto Error = CPU->mapMMIO(Stack, PageSize, Device);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Callbacks, 0u);
    return;
  }
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    resetCPU();
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_FALSE(IsSkipped());
    auto Error = CPU->mapMMIO(Stack, PageSize, Device);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    seed(UINT64_MAX, Stack + BaseOffset);
    unsigned Observations = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) { ++Observations; };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Observations; };
    const auto Exit = run(C.Bytes, std::move(Hooks));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(Callbacks, 0u);
    EXPECT_EQ(Observations, 0u);
    EXPECT_EQ(reg(X64Register::PC), Code);
    EXPECT_EQ(reg(X64Register::FLAGS), Flags);
    expectBytes(Before);
  }
}

TEST_P(X64BitString, AddressSizeWrappingPrecedesFSAndGSBases) {
  for (uint64_t Segment : {uint64_t(0), SegmentBase})
    llvm::cantFail(CPU->map(Segment + WrappedAddress + WordBytes - PageSize,
                            PageSize, Read | Write | UserAccessible));
  for (uint8_t Prefix : {uint8_t(0), uint8_t(FSPrefix), uint8_t(GSPrefix)}) {
    seed(UINT64_MAX, NarrowBase);
    const uint64_t Address = WrappedAddress + (Prefix ? SegmentBase : 0);
    llvm::cantFail(CPU->writeInteger(Address, TargetBefore, WordBytes));
    llvm::cantFail(CPU->setReg(X64Register::FSBase, SegmentBase));
    llvm::cantFail(CPU->setReg(X64Register::GSBase, SegmentBase));
    std::vector<uint8_t> Bytes = {uint8_t(AddressPrefix)};
    if (Prefix)
      Bytes.push_back(Prefix);
    Bytes.insert(Bytes.end(), std::begin(Set64Register),
                 std::end(Set64Register));
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, WordBytes);
    };
    Hooks.Write = [&](uint64_t A, unsigned Size, uint64_t Value) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, WordBytes);
      EXPECT_EQ(Value, TargetAfter);
    };
    const auto Exit = run(Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)),
              TargetAfter);
    EXPECT_EQ(reg(X64Register::CX), NarrowBase);
    EXPECT_EQ(reg(X64Register::DX), UINT64_MAX);
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64BitString,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
