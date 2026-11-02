//===- X64IntegerTests.cpp - Scalar integer widths and implicit outputs -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation {
namespace {
#define NEVERD_INTEGER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_INTEGER_MULTIPLY(Name, Width, Memory, ...)                      \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_INTEGER_EXTEND(Name, Input, Output, High, ...)                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_INTEGER_SCAN(Name, Width, Reverse, Memory, ...)                 \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_SCAN
#undef NEVERD_INTEGER_EXTEND
#undef NEVERD_INTEGER_MULTIPLY
#undef NEVERD_INTEGER_VALUE
struct Parameter {
  const char *Name;
  ExecutionBackendKind Backend;
  bool User;
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.Name; }
const Parameter Parameters[] = {
#define NEVERD_INTEGER_BACKEND(Name, Backend, User)                            \
  {#Name, ExecutionBackendKind::Backend, User},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_BACKEND
};
struct Multiply {
  const char *Name;
  unsigned Size;
  bool Memory;
  llvm::ArrayRef<uint8_t> Bytes;
};
const Multiply Multiplies[] = {
#define NEVERD_INTEGER_MULTIPLY(Name, Width, Memory, ...)                      \
  {#Name, Width, Memory, Name},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_MULTIPLY
};
struct Product {
  unsigned Size;
  bool Overflow;
  uint64_t Input, Low, High;
};
const Product Products[] = {
#define NEVERD_INTEGER_PRODUCT(Width, Overflow, Input, Low, High)              \
  {Width, Overflow, Input, Low, High},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_PRODUCT
};
struct Extend {
  const char *Name;
  uint64_t Input, Low, High;
  llvm::ArrayRef<uint8_t> Bytes;
};
const Extend Extensions[] = {
#define NEVERD_INTEGER_EXTEND(Name, Input, Low, High, ...)                     \
  {#Name, Input, Low, High, Name},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_EXTEND
};
struct Scan {
  const char *Name;
  unsigned Size;
  bool Reverse, Memory;
  llvm::ArrayRef<uint8_t> Bytes;
};
const Scan Scans[] = {
#define NEVERD_INTEGER_SCAN(Name, Width, Reverse, Memory, ...)                 \
  {#Name, Width, Reverse, Memory, Name},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_SCAN
};
class X64Integer : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override { resetCPU(); }
  void resetCPU() {
    auto B = createExecutionBackend(GetParam().Backend,
                                    GetParam().User
                                        ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedX64,
                                    Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(GetParam().Backend, GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
  }
  void seed(uint64_t Accumulator) {
    llvm::cantFail(CPU->setReg(X64Register::AX, Accumulator));
    llvm::cantFail(CPU->setReg(X64Register::DX, HighSeed));
    llvm::cantFail(CPU->setReg(X64Register::BX, Multiplier));
    llvm::cantFail(CPU->setReg(X64Register::CX, Data));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
  }
  uint64_t reg(X64Register R) { return llvm::cantFail(CPU->reg(R)); }
  ExecutionExit run(llvm::ArrayRef<uint8_t> Bytes, BackendHooks Hooks = {}) {
    llvm::cantFail(CPU->write(Code, Bytes));
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
};
TEST_P(X64Integer, MultiplyPreservesWidthsAndDefinedFlags) {
  for (const auto &Instruction : Multiplies)
    for (const auto &Expected : Products) {
      if (Expected.Size != Instruction.Size)
        continue;
      SCOPED_TRACE(Instruction.Name);
      SCOPED_TRACE(Expected.Overflow);
      seed(Expected.Input);
      llvm::cantFail(CPU->writeInteger(Data, Multiplier, Instruction.Size));
      unsigned Reads = 0, Writes = 0;
      BackendHooks H;
      H.Read = [&](uint64_t Address, uint32_t Size) {
        EXPECT_EQ(Address, Data);
        EXPECT_EQ(Size, Instruction.Size);
        ++Reads;
      };
      H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
      const auto Exit = run(Instruction.Bytes, std::move(H));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(reg(X64Register::AX), Expected.Low);
      EXPECT_EQ(reg(X64Register::DX), Expected.High);
      EXPECT_EQ(reg(X64Register::BX), Multiplier);
      EXPECT_EQ(reg(X64Register::CX), Data);
      // Only CF and OF are architecturally defined for MUL.
      EXPECT_EQ(reg(X64Register::FLAGS) & CarryOverflow,
                Expected.Overflow ? CarryOverflow : 0);
      EXPECT_EQ(reg(X64Register::PC), Code + Instruction.Bytes.size());
      EXPECT_EQ(Reads, unsigned(Instruction.Memory));
      EXPECT_EQ(Writes, 0u);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, Instruction.Size)),
                Multiplier);
    }
}
TEST_P(X64Integer, SignExtensionKeepsFlagsAndUntouchedRegisterBits) {
  for (const auto &Instruction : Extensions) {
    SCOPED_TRACE(Instruction.Name);
    seed(Instruction.Input);
    const auto Exit = run(Instruction.Bytes);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(reg(X64Register::AX), Instruction.Low);
    EXPECT_EQ(reg(X64Register::DX), Instruction.High);
    EXPECT_EQ(reg(X64Register::BX), Multiplier);
    EXPECT_EQ(reg(X64Register::CX), Data);
    EXPECT_EQ(reg(X64Register::FLAGS), Flags);
    EXPECT_EQ(reg(X64Register::PC), Code + Instruction.Bytes.size());
  }
}
TEST_P(X64Integer, ReadObserverStopsBeforeImplicitRegisterEffects) {
  for (const auto &Instruction : Multiplies) {
    if (!Instruction.Memory)
      continue;
    SCOPED_TRACE(Instruction.Name);
    seed(Multiplier);
    llvm::cantFail(CPU->writeInteger(Data, Multiplier, Instruction.Size));
    unsigned Reads = 0;
    BackendHooks H;
    H.Read = [&](uint64_t Address, uint32_t Size) {
      EXPECT_EQ(Address, Data);
      EXPECT_EQ(Size, Instruction.Size);
      ++Reads;
      CPU->stop();
    };
    const auto Exit = run(Instruction.Bytes, std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(reg(X64Register::AX), Multiplier);
    EXPECT_EQ(reg(X64Register::DX), HighSeed);
    EXPECT_EQ(reg(X64Register::FLAGS), Flags);
    EXPECT_EQ(reg(X64Register::PC), Code);
  }
}
TEST_P(X64Integer, MissingSourcePagePreservesBothProductHalves) {
  seed(Multiplier);
  const auto Address = Data + PageSize - sizeof(uint64_t) + 1;
  llvm::cantFail(CPU->setReg(X64Register::CX, Address));
  const auto Exit = run(MulMemory64);
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
  EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
  EXPECT_EQ(reg(X64Register::AX), Multiplier);
  EXPECT_EQ(reg(X64Register::DX), HighSeed);
  EXPECT_EQ(reg(X64Register::FLAGS), Flags);
  EXPECT_EQ(reg(X64Register::PC), Code);
}
TEST_P(X64Integer, BitScanPreservesWidthsAndDefinedFlags) {
  constexpr uint64_t Inputs[] = {0,
                                 1,
                                 2,
                                 0x8100,
                                 0x80010000,
                                 0x8000000000000000ULL,
                                 0x1020408102040800ULL,
                                 UINT64_MAX};
  for (const auto &I : Scans)
    for (uint64_t Input : Inputs)
      for (uint64_t InitialFlags : {Flags, Flags ^ ZeroFlag}) {
        SCOPED_TRACE(I.Name);
        SCOPED_TRACE(Input);
        seed(HighSeed);
        llvm::cantFail(CPU->setReg(X64Register::BX, Input));
        llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags));
        llvm::cantFail(CPU->writeInteger(Data, Input, I.Size));
        unsigned Reads = 0, Writes = 0;
        BackendHooks H;
        H.Read = [&](uint64_t Address, uint32_t Size) {
          EXPECT_EQ(Address, Data);
          EXPECT_EQ(Size, I.Size);
          ++Reads;
        };
        H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
        const auto Exit = run(I.Bytes, std::move(H));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        const uint64_t Mask = UINT64_MAX >> (64 - 8 * I.Size);
        const uint64_t Value = Input & Mask;
        if (Value) {
          unsigned Index = 0;
          for (unsigned Bit = 0; Bit < 8 * I.Size; ++Bit)
            if (Value & (uint64_t(1) << Bit)) {
              Index = Bit;
              if (!I.Reverse)
                break;
            }
          const uint64_t Expected =
              I.Size == 2 ? (HighSeed & ~Mask) | Index : Index;
          EXPECT_EQ(reg(X64Register::AX), Expected);
        }
        // A zero source defines ZF, but Intel does not define the destination.
        // Execute the original instruction and test only portable outputs;
        // never replace the selected processor's destination or other flags.
        EXPECT_EQ(reg(X64Register::FLAGS) & ~BitScanUndefinedFlags,
                  (InitialFlags & ~(BitScanUndefinedFlags | ZeroFlag)) |
                      (Value ? 0 : ZeroFlag));
        EXPECT_EQ(reg(X64Register::BX), Input);
        EXPECT_EQ(reg(X64Register::CX), Data);
        EXPECT_EQ(reg(X64Register::DX), HighSeed);
        EXPECT_EQ(reg(X64Register::PC), Code + I.Bytes.size());
        EXPECT_EQ(Reads, unsigned(I.Memory));
        EXPECT_EQ(Writes, 0u);
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, I.Size)), Value);
      }
}
TEST_P(X64Integer, BitScanAliasedExtendedRegisterKeepsUntouchedBits) {
  struct Alias {
    unsigned Size;
    bool Reverse;
    std::vector<uint8_t> Bytes;
  };
  const Alias Cases[] = {{2, false, {0x66, 0x45, 0x0f, 0xbc, 0xd2}},
                         {4, false, {0x45, 0x0f, 0xbc, 0xd2}},
                         {8, false, {0x4d, 0x0f, 0xbc, 0xd2}},
                         {2, true, {0x66, 0x45, 0x0f, 0xbd, 0xd2}},
                         {4, true, {0x45, 0x0f, 0xbd, 0xd2}},
                         {8, true, {0x4d, 0x0f, 0xbd, 0xd2}}};
  constexpr uint64_t Input = 0x8000000080008100ULL;
  for (const auto &I : Cases) {
    SCOPED_TRACE(I.Size);
    SCOPED_TRACE(I.Reverse);
    seed(HighSeed);
    llvm::cantFail(CPU->setReg(X64Register::R10, Input));
    const auto Exit = run(I.Bytes);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    const uint64_t Index = I.Reverse ? I.Size * 8 - 1 : 8;
    EXPECT_EQ(reg(X64Register::R10),
              I.Size == 2 ? (Input & ~uint64_t(0xffff)) | Index : Index);
    EXPECT_EQ(reg(X64Register::AX), HighSeed);
    EXPECT_EQ(reg(X64Register::FLAGS) & ~BitScanUndefinedFlags,
              Flags & ~(BitScanUndefinedFlags | ZeroFlag));
    EXPECT_EQ(reg(X64Register::PC), Code + I.Bytes.size());
  }
}
TEST_P(X64Integer, BitScanReadStopPreservesDestinationAndFlags) {
  for (const auto &I : Scans) {
    if (!I.Memory)
      continue;
    SCOPED_TRACE(I.Name);
    seed(HighSeed);
    llvm::cantFail(CPU->writeInteger(Data, Multiplier, I.Size));
    unsigned Reads = 0;
    BackendHooks H;
    H.Read = [&](uint64_t Address, uint32_t Size) {
      EXPECT_EQ(Address, Data);
      EXPECT_EQ(Size, I.Size);
      ++Reads;
      CPU->stop();
    };
    const auto Exit = run(I.Bytes, std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(reg(X64Register::AX), HighSeed);
    EXPECT_EQ(reg(X64Register::FLAGS), Flags);
    EXPECT_EQ(reg(X64Register::PC), Code);
  }
}
TEST_P(X64Integer, BitScanCrossPageReadChecksCompleteSpan) {
  llvm::cantFail(
      CPU->map(Data + PageSize, PageSize, Read | Write | UserAccessible));
  for (const auto &I : Scans) {
    if (!I.Memory)
      continue;
    SCOPED_TRACE(I.Name);
    const uint64_t Address = Data + PageSize - I.Size + 1;
    seed(HighSeed);
    llvm::cantFail(CPU->setReg(X64Register::CX, Address));
    llvm::cantFail(CPU->writeInteger(Address, 0x8100, I.Size));
    unsigned Reads = 0;
    BackendHooks H;
    H.Read = [&](uint64_t A, uint32_t Size) {
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, I.Size);
      ++Reads;
    };
    const auto Exit = run(I.Bytes, std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    const uint64_t Index = I.Reverse ? 15 : 8;
    EXPECT_EQ(reg(X64Register::AX),
              I.Size == 2 ? (HighSeed & ~uint64_t(0xffff)) | Index : Index);
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, I.Size)), 0x8100u);
  }
}
TEST_P(X64Integer, BitScanMemoryFaultPreservesDestinationAndFlags) {
  for (const auto &I : Scans) {
    if (!I.Memory)
      continue;
    for (bool Protected : {false, true}) {
      SCOPED_TRACE(I.Name);
      SCOPED_TRACE(Protected);
      resetCPU();
      ASSERT_FALSE(HasFatalFailure());
      ASSERT_FALSE(IsSkipped());
      seed(HighSeed);
      const uint64_t Address = Data + PageSize - I.Size + 1;
      llvm::cantFail(CPU->setReg(X64Register::CX, Address));
      if (Protected)
        llvm::cantFail(CPU->map(Data + PageSize, PageSize, UserAccessible));
      const auto Exit = run(I.Bytes);
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
      EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
      EXPECT_EQ(Exit.Fault->Size, 1u);
      EXPECT_EQ(Exit.Fault->Kind, Protected ? BackendFaultKind::Protection
                                            : BackendFaultKind::UnmappedMemory);
      EXPECT_EQ(reg(X64Register::AX), HighSeed);
      EXPECT_EQ(reg(X64Register::CX), Address);
      EXPECT_EQ(reg(X64Register::FLAGS), Flags);
      EXPECT_EQ(reg(X64Register::PC), Code);
    }
  }
}
TEST_P(X64Integer, BitScanLockPrefixRejectsWithoutDataObservations) {
  for (const auto &I : Scans) {
    SCOPED_TRACE(I.Name);
    resetCPU();
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_FALSE(IsSkipped());
    seed(HighSeed);
    std::vector<uint8_t> Bytes{0xf0};
    Bytes.insert(Bytes.end(), I.Bytes.begin(), I.Bytes.end());
    unsigned Observations = 0;
    BackendHooks H;
    H.Read = [&](uint64_t, uint32_t) { ++Observations; };
    H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Observations; };
    const auto Exit = run(Bytes, std::move(H));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(Observations, 0u);
    EXPECT_EQ(reg(X64Register::AX), HighSeed);
    EXPECT_EQ(reg(X64Register::FLAGS), Flags);
    EXPECT_EQ(reg(X64Register::PC), Code);
  }
}
TEST_P(X64Integer, BitScanDeviceSourceRejectsWithoutCallbacks) {
  unsigned Callbacks = 0;
  GuestMMIOCallbacks Device;
  Device.Validate = [&](uint64_t, unsigned, bool) {
    ++Callbacks;
    return llvm::Error::success();
  };
  Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++Callbacks;
    return 1;
  };
  Device.Write = [&](uint64_t, unsigned, uint64_t) {
    ++Callbacks;
    return llvm::Error::success();
  };
  const uint64_t Address = Data + PageSize;
  if (GetParam().User) {
    auto E = CPU->mapMMIO(Address, PageSize, Device);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(Callbacks, 0u);
    return;
  }
  for (const auto &I : Scans) {
    if (!I.Memory)
      continue;
    SCOPED_TRACE(I.Name);
    resetCPU();
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_FALSE(IsSkipped());
    llvm::cantFail(CPU->mapMMIO(Address, PageSize, Device));
    seed(HighSeed);
    llvm::cantFail(CPU->setReg(X64Register::CX, Address));
    unsigned Reads = 0;
    BackendHooks H;
    H.Read = [&](uint64_t, uint32_t) { ++Reads; };
    const auto Exit = run(I.Bytes, std::move(H));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(Callbacks, 0u);
    EXPECT_EQ(Reads, 0u);
    EXPECT_EQ(reg(X64Register::AX), HighSeed);
    EXPECT_EQ(reg(X64Register::FLAGS), Flags);
    EXPECT_EQ(reg(X64Register::PC), Code);
  }
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64Integer,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
