//===- AArch64ExclusiveTests.cpp - Exclusive effects across transports ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <array>
#include <climits>
#include <stdexcept>
#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_EXCLUSIVE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_EXCLUSIVE_TEXT(Name, Value) constexpr char Name[] = Value;
#include "AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_VALUE
#undef NEVERD_EXCLUSIVE_TEXT
struct ExclusiveCase {
  const char *Name;
  uint32_t Load, Store;
  unsigned Width, Count;
  unsigned size() const { return Width * Count; }
};
constexpr ExclusiveCase Cases[] = {
#define NEVERD_EXCLUSIVE_CASE(Name, Load, Store, Width, Count)                 \
  {#Name, Load, Store, Width, Count},
#include "AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_CASE
};
using Parameter =
    std::tuple<ExecutionBackendKind, ExecutionContract, ExclusiveCase>;
const char *profile(ExecutionContract Contract) {
  if (Contract == ExecutionContract::Software)
    return SoftwareProfile;
  return Contract == ExecutionContract::CheckedUserAArch64 ? UserProfile
                                                           : SupervisorProfile;
}
void PrintTo(const Parameter &P, std::ostream *OS) {
  *OS << executionBackendName(std::get<0>(P)) << profile(std::get<1>(P))
      << std::get<2>(P).Name;
}
class AArch64Exclusive : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  uint64_t Address = Data;
  const ExclusiveCase &testCase() const { return std::get<2>(GetParam()); }
  ExecutionContract contract() const { return std::get<1>(GetParam()); }
  bool user() const {
    return contract() == ExecutionContract::CheckedUserAArch64;
  }
  void SetUp() override { initialize(); }
  void initialize() {
    auto Created = createExecutionBackend(std::get<0>(GetParam()), contract(),
                                          Limit, GuestArchitecture::AArch64);
    if (!Created) {
      auto E = Created.takeError();
      bool Unavailable = E.isA<BackendUnavailableError>();
      auto Text = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    CPU = std::move(Created->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->mapAlias(Alias, Data, PageSize, Read | Write | UserAccessible));
    const uint32_t Words[] = {testCase().Load, testCase().Store, Clear,   Nop,
                              StoreByte,       ReadWord,         Service, Nop,
                              ZeroCacheBlock};
    std::array<uint8_t, sizeof(Words)> Bytes;
    for (unsigned N = 0; N < std::size(Words); ++N)
      llvm::support::endian::write32le(Bytes.data() + N * InstructionBytes,
                                       Words[N]);
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->writeInteger(Data, Initial, WordBytes));
    llvm::cantFail(CPU->writeInteger(Data + WordBytes, Updated, WordBytes));
    llvm::cantFail(CPU->setReg(AArch64Register::X1, Address));
    llvm::cantFail(CPU->setReg(AArch64Register::X2, InitialStatus));
    llvm::cantFail(CPU->setReg(AArch64Register::NZCV, InitialFlags));
  }
  ExecutionExit run(unsigned Index, BackendHooks Hooks = {}) {
    const uint64_t PC = Code + Index * InstructionBytes;
    Hooks.Instruction = [&, PC](uint64_t At, uint32_t) {
      if (At != PC)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(PC, Timeout));
  }
  void step(unsigned Index, BackendHooks Hooks = {}) {
    auto Exit = run(Index, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)),
              Code + (Index + 1) * InstructionBytes);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialFlags);
  }
  void input() {
    llvm::cantFail(CPU->setReg(AArch64Register::X0, Updated));
    llvm::cantFail(CPU->setReg(AArch64Register::X3, Initial));
    llvm::cantFail(CPU->setReg(AArch64Register::X2, InitialStatus));
  }
  uint64_t status() { return llvm::cantFail(CPU->reg(AArch64Register::X2)); }
  std::array<uint8_t, Granule> memory() {
    std::array<uint8_t, Granule> Bytes;
    llvm::cantFail(CPU->snapshotBacking(Data, Bytes));
    return Bytes;
  }
};

TEST_P(AArch64Exclusive, OriginalWidthsPublishLoadsAndSuccessfulStores) {
  const auto Before = memory();
  step(0);
  for (unsigned N = 0; N < testCase().Count; ++N) {
    uint64_t Expected = 0;
    for (unsigned B = 0; B < testCase().Width; ++B)
      Expected |= uint64_t(Before[N * testCase().Width + B]) << (B * CHAR_BIT);
    EXPECT_EQ(
        llvm::cantFail(CPU->reg(N ? AArch64Register::X3 : AArch64Register::X0)),
        Expected);
  }
  input();
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t At, uint32_t Size, uint64_t Value) {
    ++Writes;
    EXPECT_EQ(status(), InitialStatus);
    std::array<uint8_t, Granule> Bytes;
    llvm::cantFail(CPU->read(Data, Bytes));
    EXPECT_EQ(Bytes, Before);
    EXPECT_EQ(At, Data + (Writes - 1) * WordBytes);
    uint64_t Expected = 0;
    for (unsigned B = 0; B < Size; ++B) {
      unsigned Byte = At - Data + B;
      uint64_t Source = Byte < testCase().Width ? Updated : Initial;
      Expected |=
          uint64_t(uint8_t(Source >> ((Byte % testCase().Width) * CHAR_BIT)))
          << (B * CHAR_BIT);
    }
    EXPECT_EQ(Value, Expected);
  };
  step(1, std::move(Hooks));
  EXPECT_EQ(status(), 0u);
  EXPECT_EQ(Writes, (testCase().size() + WordBytes - 1) / WordBytes);
  auto Expected = Before;
  for (unsigned N = 0; N < testCase().size(); ++N)
    Expected[N] = uint8_t((N < testCase().Width ? Updated : Initial) >>
                          ((N % testCase().Width) * CHAR_BIT));
  EXPECT_EQ(memory(), Expected);
  step(1);
  EXPECT_EQ(status(), 1u); // Successful STXR consumes its local monitor.
  EXPECT_EQ(memory(), Expected);
}

TEST_P(AArch64Exclusive, FailedStoreHasNoDataObservationAndConsumesTheMonitor) {
  const auto Before = memory();
  input();
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ADD_FAILURE(); };
  Hooks.Read = [&](uint64_t, uint32_t) { ADD_FAILURE(); };
  step(1, Hooks);
  EXPECT_EQ(status(), 1u);
  EXPECT_EQ(memory(), Before);
  step(0);
  llvm::cantFail(CPU->setReg(AArch64Register::X1, Data + Granule));
  step(1, Hooks);
  EXPECT_EQ(status(), 1u);
  llvm::cantFail(CPU->setReg(AArch64Register::X1, Data));
  step(1, Hooks);
  EXPECT_EQ(status(), 1u);
  EXPECT_EQ(memory(), Before);
}

TEST_P(AArch64Exclusive,
       UnalignedOperandsWithinOneQuantityMatchPhysicalGranulesAndRetry) {
  for (unsigned Offset = 0; Offset <= Granule - testCase().size(); ++Offset) {
    SCOPED_TRACE(Offset);
    if (contract() == ExecutionContract::Software && Offset % testCase().size())
      continue;
    for (bool AlignedLoad : {false, true}) {
      const auto Before = memory();
      const unsigned LoadOffset = AlignedLoad ? 0 : Offset;
      llvm::cantFail(CPU->setReg(AArch64Register::X1, Data + LoadOffset));
      step(0);
      for (unsigned N = 0; N < testCase().Count; ++N) {
        uint64_t Expected = 0;
        for (unsigned B = 0; B < testCase().Width; ++B)
          Expected |= uint64_t(Before[LoadOffset + N * testCase().Width + B])
                      << (B * CHAR_BIT);
        EXPECT_EQ(llvm::cantFail(
                      CPU->reg(N ? AArch64Register::X3 : AArch64Register::X0)),
                  Expected);
      }
      input();
      llvm::cantFail(CPU->setReg(AArch64Register::X1, Alias + Offset));
      BackendHooks Hooks;
      Hooks.Write = [&](uint64_t Address, uint32_t, uint64_t) {
        EXPECT_EQ(Address, Alias + Offset);
        CPU->stop();
      };
      auto Exit = run(1, Hooks);
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
      EXPECT_EQ(status(), InitialStatus);
      EXPECT_EQ(memory(), Before);
      step(1);
      EXPECT_EQ(status(), 0u);
      auto Expected = Before;
      for (unsigned N = 0; N < testCase().size(); ++N)
        Expected[Offset + N] =
            uint8_t((N < testCase().Width ? Updated : Initial) >>
                    ((N % testCase().Width) * CHAR_BIT));
      EXPECT_EQ(memory(), Expected);
    }
  }
}

TEST_P(AArch64Exclusive,
       FailedMonitorChecksAlignmentAndPermissionsBeforeConditionalFailure) {
  for (bool Stale : {false, true}) {
    for (bool Mapped : {false, true}) {
      llvm::cantFail(CPU->setReg(AArch64Register::X1, Data));
      if (Stale)
        step(0);
      const auto Before = memory();
      llvm::cantFail(CPU->write(Data, Before));
      llvm::cantFail(CPU->protect(Data, PageSize, 0));
      const uint64_t Base = Mapped ? Data : Data + PageSize;
      llvm::cantFail(CPU->setReg(AArch64Register::X1, Base));
      input();
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, uint32_t) { ADD_FAILURE(); };
      Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ADD_FAILURE(); };
      Hooks.RecoverableFault = [&](const BackendFault &Fault) {
        EXPECT_EQ(Fault.Kind, Mapped ? BackendFaultKind::Protection
                                     : BackendFaultKind::UnmappedMemory);
        EXPECT_EQ(Fault.Address, Base);
        return true;
      };
      auto Fault = run(1, Hooks);
      EXPECT_EQ(Fault.Kind, ExecutionExitKind::RecoverableFault);
      ASSERT_TRUE(CPU->takeRecoverableFault());
      EXPECT_EQ(status(), InitialStatus);
      EXPECT_EQ(memory(), Before);
      if (testCase().size() > 1) {
        llvm::cantFail(CPU->setReg(AArch64Register::X1, Base + Granule - 1));
        input();
        Hooks.RecoverableFault = [&](const BackendFault &Fault) {
          EXPECT_EQ(Fault.Kind, BackendFaultKind::Alignment);
          EXPECT_EQ(Fault.Address, Base + Granule - 1);
          return true;
        };
        auto Exit = run(1, Hooks);
        EXPECT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault);
        ASSERT_TRUE(CPU->takeRecoverableFault());
        EXPECT_EQ(status(), InitialStatus);
      }
      llvm::cantFail(
          CPU->protect(Data, PageSize, Read | Write | UserAccessible));
      llvm::cantFail(CPU->setReg(AArch64Register::X1, Data));
      step(1);
      EXPECT_EQ(status(), 1u);
    }
  }
}

TEST_P(AArch64Exclusive,
       AliasesMatchPhysicalBytesAndSameValueWritesInvalidate) {
  step(0);
  input();
  llvm::cantFail(CPU->setReg(AArch64Register::X1, Alias));
  step(1);
  EXPECT_EQ(status(), 0u);
  step(0);
  auto Snapshot = llvm::cantFail(CPU->saveContext());
  auto View = llvm::cantFail(CPU->pinBacking(Data, Granule));
  auto Bytes = memory();
  llvm::cantFail(View.write(0, Bytes));
  llvm::cantFail(CPU->restoreContext(*Snapshot));
  input();
  step(1);
  EXPECT_EQ(status(), 1u);
  EXPECT_EQ(memory(), Bytes);
  step(0);
  llvm::cantFail(CPU->writeInteger(Alias, uint8_t(~Bytes[0]), 1));
  llvm::cantFail(CPU->write(Data, Bytes));
  input();
  step(1);
  EXPECT_EQ(status(),
            1u); // Restoring identical bytes cannot hide interference.
  EXPECT_EQ(memory(), Bytes);
  step(0);
  input();
  llvm::cantFail(CPU->writeInteger(Data + Granule, Initial, WordBytes));
  step(1);
  EXPECT_EQ(status(), 0u);
}

TEST_P(AArch64Exclusive,
       CommittedStoresRetireExecutableAliasesWithinTheSameRun) {
  llvm::cantFail(CPU->unmapAlias(Alias, PageSize));
  llvm::cantFail(
      CPU->mapAlias(Alias, Code, PageSize, Read | Write | UserAccessible));
  const auto Load = testCase().Load, Store = testCase().Store;
  const uint32_t Words[] = {
#define NEVERD_EXCLUSIVE_CODE_WORD(Word) Word,
#include "AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_CODE_WORD
  };
  for (unsigned N = 0; N < std::size(Words); ++N)
    llvm::cantFail(CPU->writeInteger(Code + N * InstructionBytes, Words[N],
                                     InstructionBytes));
  llvm::cantFail(CPU->setReg(AArch64Register::X1, Alias + CodeTargetOffset));
  llvm::cantFail(CPU->setReg(AArch64Register::X6, UpdatedCode));
  llvm::cantFail(CPU->setReg(AArch64Register::X7, 0));
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    if (PC == Code + CodeEndOffset)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(status(), 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X5)), CodeResult);
  EXPECT_EQ(
      llvm::cantFail(CPU->readInteger(Code + CodeTargetOffset, WordBytes)),
      UpdatedCode);
}

TEST_P(AArch64Exclusive,
       EveryClearOptionDiscardsReservationAndSnapshotsCannotUndoWrites) {
  for (unsigned Option = 0; Option <= (OptionMask >> OptionShift); ++Option) {
    llvm::cantFail(CPU->writeInteger(
        Code + 2 * InstructionBytes,
        (Clear & ~OptionMask) | (Option << OptionShift), InstructionBytes));
    step(0);
    input();
    auto Snapshot = llvm::cantFail(CPU->saveContext());
    step(2);
    step(1);
    EXPECT_EQ(status(), 1u);
    llvm::cantFail(CPU->restoreContext(*Snapshot));
    step(1);
    EXPECT_EQ(status(), 0u);
    llvm::cantFail(CPU->restoreContext(*Snapshot));
    step(1);
    EXPECT_EQ(status(), 1u);
  }
}

TEST_P(AArch64Exclusive,
       EachStoreObserverCanCancelAndRetryWithoutLosingReservation) {
  for (unsigned Cancel = 1;
       Cancel <= (testCase().size() + WordBytes - 1) / WordBytes; ++Cancel) {
    step(0);
    input();
    const auto Before = memory();
    unsigned Seen = 0;
    BackendHooks Hooks;
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) {
      if (++Seen == Cancel)
        CPU->stop();
    };
    auto Exit = run(1, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(status(), InitialStatus);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)),
              Code + InstructionBytes);
    EXPECT_EQ(memory(), Before);
    step(1);
    EXPECT_EQ(status(), 0u);
  }
}

TEST_P(AArch64Exclusive,
       AlignmentAndPermissionFaultsKeepRegistersAndClearReservation) {
  for (bool Load : {true, false}) {
    step(0);
    input();
    const auto Before = memory();
    if (testCase().size() > 1) {
      const uint64_t Unaligned = contract() == ExecutionContract::Software
                                     ? Data + 1
                                     : Data + Granule - testCase().size() + 1;
      llvm::cantFail(CPU->setReg(AArch64Register::X1, Unaligned));
      BackendHooks Hooks;
      Hooks.RecoverableFault = [&](const BackendFault &F) {
        EXPECT_EQ(F.Kind, BackendFaultKind::Alignment);
        EXPECT_EQ(F.Cause, BackendFaultCause::OperandAlignment);
        EXPECT_EQ(F.Address, Unaligned);
        EXPECT_EQ(F.Size, testCase().size());
        EXPECT_EQ(F.Access,
                  Load ? BackendAccessKind::Read : BackendAccessKind::Write);
        return true;
      };
      Hooks.Read = [&](uint64_t, uint32_t) { ADD_FAILURE(); };
      Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ADD_FAILURE(); };
      auto Exit = run(Load ? 0 : 1, Hooks);
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault);
      ASSERT_TRUE(CPU->takeRecoverableFault());
      EXPECT_EQ(status(), InitialStatus);
      EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), Updated);
      llvm::cantFail(CPU->setReg(AArch64Register::X1, Data));
      step(1);
      EXPECT_EQ(status(), 1u);
      EXPECT_EQ(memory(), Before);
    }
    step(0);
    input();
    llvm::cantFail(
        CPU->protect(Data, PageSize, UserAccessible | (Load ? Write : Read)));
    BackendHooks Hooks;
    Hooks.RecoverableFault = [&](const BackendFault &F) {
      EXPECT_EQ(F.Kind, BackendFaultKind::Protection);
      EXPECT_EQ(F.Access,
                Load ? BackendAccessKind::Read : BackendAccessKind::Write);
      return true;
    };
    auto Exit = run(Load ? 0 : 1, Hooks);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault);
    ASSERT_TRUE(CPU->takeRecoverableFault());
    EXPECT_EQ(status(), InitialStatus);
    llvm::cantFail(CPU->protect(Data, PageSize, Read | Write | UserAccessible));
    step(1);
    EXPECT_EQ(status(), 1u);
    EXPECT_EQ(memory(), Before);
  }
}

TEST_P(AArch64Exclusive,
       OrdinaryStoresInvalidateAndAddressSpaceSwitchClearsLocalState) {
  step(0);
  input();
  step(4);
  step(1);
  EXPECT_EQ(status(), 1u);
  step(0);
  llvm::cantFail(CPU->bindAddressSpace(CPU->addressSpace()));
  step(1);
  EXPECT_EQ(status(), 1u);
  step(0);
  step(3);
  step(1);
  EXPECT_EQ(status(), 0u);
  if (contract() == ExecutionContract::Software) {
    step(0);
    BackendHooks Hooks;
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { CPU->stop(); };
    auto Exit = run(8, Hooks);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
    step(1);
    EXPECT_EQ(status(), 0u);
    // Bulk zeroing may use an engine fast path; it must still report every
    // affected reservation granule, including writes of identical zero bytes.
    step(8);
    step(0);
    step(8);
    step(1);
    EXPECT_EQ(status(), 1u);
  }
  if (user()) {
    step(0);
    auto Exit = run(6);
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::ServiceRequest);
    ASSERT_TRUE(CPU->takeServiceRequest());
    step(1);
    EXPECT_EQ(status(), 1u);
  }
}

TEST_P(AArch64Exclusive, CancelledLoadRetainsThePreviousReservation) {
  step(0);
  input();
  llvm::cantFail(CPU->setReg(AArch64Register::X1, Data + Granule));
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t, uint32_t) { CPU->stop(); };
  auto Exit = run(0, Hooks);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), Updated);
  llvm::cantFail(CPU->setReg(AArch64Register::X1, Data));
  step(1);
  EXPECT_EQ(status(), 0u);
}

TEST_P(AArch64Exclusive, ObserverExceptionsPublishNoRegisterOrMemoryEffects) {
  for (unsigned Boundary = 0;
       Boundary <= (testCase().size() + WordBytes - 1) / WordBytes;
       ++Boundary) {
    if (Boundary)
      ASSERT_NO_FATAL_FAILURE(initialize());
    step(0);
    input();
    const auto Before = memory();
    unsigned Seen = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, uint32_t) {
      throw std::runtime_error(ObserverFailure);
    };
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) {
      if (++Seen == Boundary)
        throw std::runtime_error(ObserverFailure);
    };
    auto Exit = run(Boundary ? 1 : 0, Hooks);
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure);
    EXPECT_EQ(status(), InitialStatus);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), Updated);
    EXPECT_EQ(memory(), Before);
  }
}

TEST_P(AArch64Exclusive,
       ZeroRegisterStackBaseAndAliasedOperandsUseArchitecturalViews) {
  // First data register, base, store-status destination, second data register.
  for (auto [First, Base, Status, Second] :
       {std::array<unsigned, 4>{0, 1, StatusRegister, SecondRegister},
        {RegisterMask, 1, StatusRegister, SecondRegister},
        {0, RegisterMask, StatusRegister, SecondRegister},
        {0, 1, RegisterMask, SecondRegister},
        {1, 1, StatusRegister, SecondRegister},
        {0, 1, StatusRegister, RegisterMask}}) {
    if (Second == RegisterMask && testCase().Count == 1)
      continue;
    auto Word = [&](bool Store) {
      uint32_t Result = Store ? testCase().Store : testCase().Load;
      Result =
          (Result & ~uint32_t(RegisterMask | (RegisterMask << BaseShift))) |
          First | (Base << BaseShift);
      if (testCase().Count == 2)
        Result = (Result & ~uint32_t(RegisterMask << SecondShift)) |
                 (Second << SecondShift);
      if (Store)
        Result = (Result & ~uint32_t(RegisterMask << StatusShift)) |
                 (Status << StatusShift);
      return Result;
    };
    llvm::cantFail(CPU->writeInteger(Code, Word(false), InstructionBytes));
    llvm::cantFail(CPU->writeInteger(Code + InstructionBytes, Word(true),
                                     InstructionBytes));
    auto BaseRegister =
        Base == RegisterMask ? AArch64Register::SP : AArch64Register(Base);
    // A scalar SP operand need not be 16-byte aligned in the fixed machine.
    Address = Data + testCase().size();
    llvm::cantFail(CPU->setReg(BaseRegister, Address));
    step(0);
    if (First != RegisterMask)
      llvm::cantFail(CPU->setReg(AArch64Register(First), Updated));
    if (Second != RegisterMask && testCase().Count == 2)
      llvm::cantFail(CPU->setReg(AArch64Register(Second), Initial));
    llvm::cantFail(CPU->setReg(BaseRegister, Address));
    std::array<uint64_t, unsigned(AArch64Register::FPSR) + 1> Before;
    for (unsigned R = 0; R < Before.size(); ++R)
      Before[R] = llvm::cantFail(CPU->reg(AArch64Register(R)));
    step(1);
    for (unsigned R = 0; R < Before.size(); ++R) {
      const uint64_t Expected =
          R == unsigned(AArch64Register::PC)      ? Code + 2 * InstructionBytes
          : R == Status && Status != RegisterMask ? 0
                                                  : Before[R];
      EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register(R))), Expected);
    }
    for (unsigned N = 0; N < testCase().Count; ++N) {
      const unsigned R = N ? Second : First;
      const uint64_t Expected =
          R == RegisterMask ? 0
                            : Before[R] & llvm::maskTrailingOnes<uint64_t>(
                                              testCase().Width * CHAR_BIT);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address + N * testCase().Width,
                                                testCase().Width)),
                Expected);
    }
  }
}

TEST_P(AArch64Exclusive,
       UnpredictableRegisterOverlapIsRejectedBeforeObservations) {
  std::vector<std::pair<uint32_t, unsigned>> Words;
  for (unsigned Status : {0u, 1u, unsigned(SecondRegister)}) {
    if (Status == SecondRegister && testCase().Count == 1)
      continue;
    Words.emplace_back(
        (testCase().Store & ~uint32_t(RegisterMask << StatusShift)) |
            (Status << StatusShift),
        1);
  }
  if (testCase().Count == 2)
    Words.emplace_back(testCase().Load & ~uint32_t(RegisterMask << SecondShift),
                       0);
  for (auto [Word, Index] : Words) {
    ASSERT_NO_FATAL_FAILURE(initialize());
    llvm::cantFail(CPU->writeInteger(Code + Index * InstructionBytes, Word,
                                     InstructionBytes));
    input();
    const auto Before = memory();
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, uint32_t) { ADD_FAILURE(); };
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ADD_FAILURE(); };
    auto Exit = run(Index, Hooks);
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
    EXPECT_EQ(status(), InitialStatus);
    EXPECT_EQ(memory(), Before);
  }
}

TEST_P(AArch64Exclusive, TwoCPUsCannotBothCommitTheSameReservation) {
  const auto Alternative = contract() == ExecutionContract::Software
                               ? ExecutionContract::CheckedAArch64
                               : ExecutionContract::Software;
  for (auto OtherContract : {contract(), Alternative}) {
    if (OtherContract != contract() &&
        std::get<0>(GetParam()) != ExecutionBackendKind::Unicorn)
      continue;
    auto Other = llvm::cantFail(createExecutionBackend(
        std::get<0>(GetParam()), OtherContract, CPU->addressSpace(),
        GuestArchitecture::AArch64));
    llvm::cantFail(Other.CPU->setReg(AArch64Register::X1, Alias));
    auto OtherStep = [&](unsigned Index) {
      const uint64_t PC = Code + Index * InstructionBytes;
      BackendHooks Hooks;
      Hooks.Instruction = [&, PC](uint64_t At, uint32_t) {
        if (At != PC)
          Other.CPU->stop();
      };
      llvm::cantFail(Other.CPU->installHooks(std::move(Hooks)));
      auto Exit = llvm::cantFail(Other.CPU->runUntilExit(PC, Timeout));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
    };
    step(0);
    OtherStep(0);
    OtherStep(1);
    EXPECT_EQ(llvm::cantFail(Other.CPU->reg(AArch64Register::X2)), 0u);
    input();
    step(1);
    EXPECT_EQ(status(), 1u);
    step(0);
    step(1);
    EXPECT_EQ(status(), 0u);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64Exclusive,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP),
                     testing::Values(ExecutionContract::CheckedAArch64,
                                     ExecutionContract::CheckedUserAArch64),
                     testing::ValuesIn(Cases)),
    [](const testing::TestParamInfo<Parameter> &P) {
      return std::string(executionBackendName(std::get<0>(P.param))) +
             profile(std::get<1>(P.param)) + std::get<2>(P.param).Name;
    });
INSTANTIATE_TEST_SUITE_P(
    Software, AArch64Exclusive,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn),
                     testing::Values(ExecutionContract::Software),
                     testing::ValuesIn(Cases)),
    [](const testing::TestParamInfo<Parameter> &P) {
      return std::string(executionBackendName(std::get<0>(P.param))) +
             profile(std::get<1>(P.param)) + std::get<2>(P.param).Name;
    });
} // namespace
} // namespace neverd::emulation
