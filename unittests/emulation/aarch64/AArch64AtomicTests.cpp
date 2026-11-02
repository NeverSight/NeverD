//===- AArch64AtomicTests.cpp - Atomic effects across CPU backends --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <array>
#include <climits>
#include <stdexcept>
#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_ATOMIC_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_ATOMIC_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_ATOMIC_CODE_VALUE NEVERD_ATOMIC_VALUE
#include "AArch64AtomicCases.def"
#include "AArch64AtomicCode.def"
#undef NEVERD_ATOMIC_VALUE
#undef NEVERD_ATOMIC_TEXT
#undef NEVERD_ATOMIC_CODE_VALUE
enum class Family {
#define NEVERD_ATOMIC_FAMILY(Name) Name,
#include "AArch64AtomicCases.def"
#undef NEVERD_ATOMIC_FAMILY
};
struct AtomicCase {
  const char *Name;
  Family Kind;
  unsigned Width, Count;
  uint32_t Word;
  uint64_t Expected;
  unsigned size() const { return Width * Count; }
  bool compare() const {
    return Kind == Family::Compare || Kind == Family::ComparePair;
  }
};
constexpr AtomicCase Cases[] = {
#define NEVERD_ATOMIC_CASE(Name, Kind, Width, Count, Word, Expected)           \
  {#Name, Family::Kind, Width, Count, Word, Expected},
#include "AArch64AtomicCases.def"
#undef NEVERD_ATOMIC_CASE
};
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
};
constexpr Profile Profiles[] = {
#define NEVERD_ATOMIC_PROFILE(Name, Backend, Contract)                         \
  {#Name, ExecutionBackendKind::Backend, ExecutionContract::Contract},
#include "AArch64AtomicCases.def"
#undef NEVERD_ATOMIC_PROFILE
};
struct Group {
  const char *Name;
  Family Kind;
};
constexpr Group Groups[] = {
#define NEVERD_ATOMIC_FAMILY(Name) {#Name, Family::Name},
#include "AArch64AtomicCases.def"
#undef NEVERD_ATOMIC_FAMILY
};
using Parameter = std::tuple<Profile, Group>;
class AArch64Atomic : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override { initialize(); }
  void initialize() {
    const auto &P = std::get<0>(GetParam());
    auto Created = createExecutionBackend(P.Backend, P.Contract, Limit,
                                          GuestArchitecture::AArch64);
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
        CPU->map(Code, PageBytes, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageBytes, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->mapAlias(Alias, Data, PageBytes, Read | Write | UserAccessible));
  }
  void instruction(uint32_t Word) {
    std::array<uint8_t, 2 * InstructionBytes> Bytes;
    llvm::support::endian::write32le(Bytes.data(), Word);
    llvm::support::endian::write32le(Bytes.data() + InstructionBytes, Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
  }
  void input(const AtomicCase &C) {
    instruction(C.Word);
    for (unsigned R = 0; R <= RegisterMask; ++R)
      llvm::cantFail(
          CPU->setReg(AArch64Register(unsigned(AArch64Register::X0) + R),
                      InitialStatus + R));
    llvm::cantFail(CPU->writeInteger(Data, Initial, WordBytes));
    llvm::cantFail(CPU->writeInteger(Data + WordBytes, Operand, WordBytes));
    llvm::cantFail(CPU->setReg(
        AArch64Register::X0,
        C.compare() ? llvm::cantFail(CPU->readInteger(Data, C.Width))
                    : Operand));
    llvm::cantFail(CPU->setReg(
        AArch64Register::X1,
        C.Count == 2 ? llvm::cantFail(CPU->readInteger(Data + C.Width, C.Width))
                     : InitialStatus));
    llvm::cantFail(CPU->setReg(AArch64Register::X2, Operand));
    llvm::cantFail(CPU->setReg(AArch64Register::X3, Initial));
    llvm::cantFail(CPU->setReg(AArch64Register::X4, Alias));
    llvm::cantFail(CPU->setReg(AArch64Register::NZCV, InitialFlags));
  }
  std::array<uint8_t, Granule> memory() {
    std::array<uint8_t, Granule> Bytes{};
    llvm::cantFail(CPU->snapshotBacking(Data, Bytes));
    return Bytes;
  }
  std::array<uint64_t, RegisterMask + 1> registers() {
    std::array<uint64_t, RegisterMask + 1> Values;
    for (unsigned R = 0; R < Values.size(); ++R)
      Values[R] = llvm::cantFail(
          CPU->reg(AArch64Register(unsigned(AArch64Register::X0) + R)));
    return Values;
  }
  bool selected(const AtomicCase &C) const {
    return C.Kind == std::get<1>(GetParam()).Kind;
  }
  ExecutionExit run(BackendHooks Hooks = {}) {
    Hooks.Instruction = [&](uint64_t At, uint32_t) {
      if (At != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
};

TEST_P(AArch64Atomic, OriginalWidthsAndOrderingVariantsPublishAtomicResults) {
  for (const auto &C : Cases) {
    if (C.Kind != std::get<1>(GetParam()).Kind)
      continue;
    SCOPED_TRACE(C.Name);
    input(C);
    const auto Before = memory();
    auto ExpectedRegisters = registers();
    const uint64_t Mask = llvm::maskTrailingOnes<uint64_t>(C.Width * CHAR_BIT);
    const uint64_t OldLow = llvm::cantFail(CPU->readInteger(Data, C.Width));
    const uint64_t OldHigh =
        llvm::cantFail(CPU->readInteger(Data + C.Width, C.Width));
    const unsigned Result = C.compare() ? 0 : TargetRegister;
    ExpectedRegisters[Result] = OldLow;
    if (C.Count == 2)
      ExpectedRegisters[Result + 1] = OldHigh;
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t At, uint32_t Size) {
      ++Reads;
      EXPECT_EQ(At, Alias);
      EXPECT_EQ(Size, C.size());
    };
    Hooks.Write = [&](uint64_t At, uint32_t Size, uint64_t Value) {
      EXPECT_EQ(At, Alias + Writes * WordBytes);
      std::array<uint8_t, Granule> Observed;
      llvm::cantFail(CPU->read(Data, Observed));
      EXPECT_EQ(Observed, Before);
      EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)), Code);
      uint64_t Expected = 0;
      for (unsigned B = 0; B < Size; ++B) {
        const unsigned Offset = Writes * WordBytes + B;
        const uint64_t Source = Offset < C.Width ? C.Expected : Initial;
        Expected |= uint64_t(uint8_t(Source >> (Offset % C.Width * CHAR_BIT)))
                    << (B * CHAR_BIT);
      }
      EXPECT_EQ(Value, Expected);
      ++Writes;
    };
    auto Exit = run(Hooks);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)),
              Code + InstructionBytes);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialFlags);
    EXPECT_EQ(registers(), ExpectedRegisters);
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(Writes, (C.size() + WordBytes - 1) / WordBytes);
    const auto After = memory();
    for (unsigned B = C.size(); B < Granule; ++B)
      EXPECT_EQ(After[B], Before[B]);
    EXPECT_EQ(llvm::cantFail(CPU->reg(C.compare() ? AArch64Register::X0
                                                  : AArch64Register::X2)),
              OldLow);
    if (C.Count == 2)
      EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X1)), OldHigh);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, C.Width)), C.Expected);
    if (C.Count == 2)
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + C.Width, C.Width)),
                Initial & Mask);
  }
}

TEST_P(AArch64Atomic, CompareMismatchReturnsBothOldWordsWithoutChangingBytes) {
  for (const auto &C : Cases) {
    if (!selected(C) || !C.compare())
      continue;
    for (unsigned N = 0; N < C.Count; ++N) {
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(N);
      input(C);
      const auto Register = N ? AArch64Register::X1 : AArch64Register::X0;
      const auto Old = llvm::cantFail(CPU->reg(Register));
      llvm::cantFail(CPU->setReg(Register, Old ^ 1));
      const auto Before = memory();
      auto Expected = registers();
      Expected[N] = Old;
      auto Exit = run();
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(memory(), Before);
      EXPECT_EQ(registers(), Expected);
    }
  }
}

TEST_P(AArch64Atomic, ResultAliasesAndZeroRegisterPreserveOriginalInputs) {
  for (const auto &C : Cases) {
    if (!selected(C))
      continue;
    for (unsigned Result :
         {0u, unsigned(BaseRegister), unsigned(RegisterMask)}) {
      // CAS writes the comparison register; moving the replacement register
      // tests a different operation, covered separately by pair/zero tests.
      if (C.compare())
        continue;
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(Result);
      input(C);
      instruction((C.Word & ~uint32_t(RegisterMask)) | Result);
      auto Expected = registers();
      if (Result != RegisterMask)
        Expected[Result] = llvm::cantFail(CPU->readInteger(Data, C.Width));
      auto Exit = run();
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(registers(), Expected);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, C.Width)), C.Expected);
    }
    input(C);
    instruction((C.Word & ~(uint32_t(RegisterMask) << BaseShift)) |
                (uint32_t(RegisterMask) << BaseShift));
    llvm::cantFail(CPU->setReg(AArch64Register::SP, Alias));
    auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::SP)), Alias);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, C.Width)), C.Expected);
  }
}

TEST_P(AArch64Atomic, EveryObserverBoundaryCanCancelWithoutPartialPublication) {
  for (const auto &C : Cases) {
    if (!selected(C))
      continue;
    for (unsigned Boundary = 0;
         Boundary <= (C.size() + WordBytes - 1) / WordBytes; ++Boundary) {
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(Boundary);
      input(C);
      const auto Before = memory();
      const auto Registers = registers();
      BackendHooks Hooks;
      unsigned Seen = 0;
      Hooks.Read = [&](uint64_t, uint32_t) {
        if (!Boundary)
          CPU->stop();
      };
      Hooks.Write = [&](uint64_t, uint32_t, uint64_t) {
        if (++Seen == Boundary)
          CPU->stop();
      };
      auto Exit = run(Hooks);
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(memory(), Before);
      EXPECT_EQ(registers(), Registers);
      EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)), Code);
      Exit = run();
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, C.Width)), C.Expected);
    }
  }
}

TEST_P(AArch64Atomic, ObserverExceptionsKeepAllUnpublishedState) {
  for (const auto &C : Cases) {
    if (!selected(C))
      continue;
    for (unsigned Boundary = 0;
         Boundary <= (C.size() + WordBytes - 1) / WordBytes; ++Boundary) {
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(Boundary);
      ASSERT_NO_FATAL_FAILURE(initialize());
      input(C);
      const auto Before = memory();
      const auto Registers = registers();
      BackendHooks Hooks;
      unsigned Seen = 0;
      Hooks.Read = [&](uint64_t, uint32_t) {
        if (!Boundary)
          throw std::runtime_error(ObserverFailure);
      };
      Hooks.Write = [&](uint64_t, uint32_t, uint64_t) {
        if (++Seen == Boundary)
          throw std::runtime_error(ObserverFailure);
      };
      auto Exit = run(Hooks);
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure);
      EXPECT_EQ(memory(), Before);
      EXPECT_EQ(registers(), Registers);
      EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)), Code);
    }
  }
}

TEST_P(AArch64Atomic, AlignmentAndWritePermissionsPrecedeAnyCommit) {
  for (const auto &C : Cases) {
    if (!selected(C))
      continue;
    for (unsigned Permissions : {unsigned(Read), 0u})
      for (bool Unaligned : {false, true}) {
        if (Unaligned && C.size() == 1)
          continue;
        SCOPED_TRACE(C.Name);
        SCOPED_TRACE(Permissions);
        SCOPED_TRACE(Unaligned);
        input(C);
        if (C.compare())
          llvm::cantFail(CPU->setReg(AArch64Register::X0, 0));
        const auto Before = memory();
        const uint64_t Address =
            Alias + (Unaligned ? Granule - C.size() + 1 : 0);
        llvm::cantFail(CPU->setReg(AArch64Register::X4, Address));
        const auto Registers = registers();
        llvm::cantFail(
            CPU->protect(Alias, PageBytes, Permissions | UserAccessible));
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t, uint32_t) { EXPECT_FALSE(Unaligned); };
        Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ADD_FAILURE(); };
        Hooks.RecoverableFault = [&](const BackendFault &F) {
          EXPECT_EQ(F.Kind, Unaligned ? BackendFaultKind::Alignment
                                      : BackendFaultKind::Protection);
          EXPECT_EQ(F.Address, Address);
          EXPECT_EQ(F.Size, C.size());
          EXPECT_EQ(F.Access, !Unaligned && !Permissions
                                  ? BackendAccessKind::Read
                                  : BackendAccessKind::Write);
          EXPECT_EQ(F.PC, Code);
          return true;
        };
        auto Exit = run(Hooks);
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
            << Exit.Diagnostic;
        ASSERT_TRUE(CPU->takeRecoverableFault());
        EXPECT_EQ(memory(), Before);
        EXPECT_EQ(registers(), Registers);
        EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)), Code);
        llvm::cantFail(
            CPU->protect(Alias, PageBytes, Read | Write | UserAccessible));
      }
  }
}

TEST_P(AArch64Atomic, UnalignedOperandUsesTheSelectedFeatureModel) {
  for (const auto &C : Cases) {
    if (!selected(C) || C.size() == Granule)
      continue;
    for (unsigned Offset = 0; Offset <= Granule - C.size(); ++Offset) {
      if (std::get<0>(GetParam()).Contract == ExecutionContract::Software &&
          Offset % C.size())
        continue;
      SCOPED_TRACE(C.Name);
      SCOPED_TRACE(Offset);
      input(C);
      llvm::cantFail(CPU->writeInteger(Data + Offset, Initial, C.Width));
      if (C.Count == 2)
        llvm::cantFail(
            CPU->writeInteger(Data + Offset + C.Width, Operand, C.Width));
      if (C.compare()) {
        llvm::cantFail(CPU->setReg(AArch64Register::X0, Initial));
        if (C.Count == 2)
          llvm::cantFail(CPU->setReg(AArch64Register::X1, Operand));
      }
      llvm::cantFail(CPU->setReg(AArch64Register::X4, Alias + Offset));
      auto Exit = run();
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + Offset, C.Width)),
                C.Expected);
    }
  }
}

TEST_P(AArch64Atomic,
       SameValueWritesInvalidateOtherCPUReservationsAndSnapshots) {
  const auto &P = std::get<0>(GetParam());
  for (const auto &C : Cases) {
    if (!selected(C))
      continue;
    SCOPED_TRACE(C.Name);
    input(C);
    // Zero is unchanged for every RMW family with a zero source. A failed
    // compare also selects the documented old-value writeback policy.
    llvm::cantFail(CPU->writeInteger(Data, 0, WordBytes));
    llvm::cantFail(CPU->writeInteger(Data + WordBytes, 0, WordBytes));
    llvm::cantFail(CPU->setReg(AArch64Register::X0, C.compare() ? 1 : 0));
    auto Other = llvm::cantFail(createExecutionBackend(
        P.Backend,
        P.Contract == ExecutionContract::Software
            ? ExecutionContract::CheckedAArch64
            : P.Contract,
        CPU->addressSpace(), GuestArchitecture::AArch64));
    const uint64_t OtherCode = Code + Granule;
    const uint32_t Words[] = {ExclusiveLoad, Nop, ExclusiveStore, Nop};
    std::array<uint8_t, sizeof(Words)> Bytes;
    for (unsigned N = 0; N < std::size(Words); ++N)
      llvm::support::endian::write32le(Bytes.data() + N * InstructionBytes,
                                       Words[N]);
    llvm::cantFail(CPU->write(OtherCode, Bytes));
    llvm::cantFail(Other.CPU->setReg(AArch64Register::X1, Data));
    auto OtherStep = [&](unsigned Index) {
      const uint64_t PC = OtherCode + Index * InstructionBytes;
      BackendHooks Hooks;
      Hooks.Instruction = [&, PC](uint64_t At, uint32_t) {
        if (At != PC)
          Other.CPU->stop();
      };
      llvm::cantFail(Other.CPU->installHooks(Hooks));
      EXPECT_EQ(llvm::cantFail(Other.CPU->runUntilExit(PC, Timeout)).Kind,
                ExecutionExitKind::Stopped);
    };
    OtherStep(0);
    auto Snapshot = llvm::cantFail(Other.CPU->saveContext());
    BackendHooks Hooks;
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { CPU->stop(); };
    EXPECT_EQ(run(Hooks).Kind, ExecutionExitKind::Stopped);
    OtherStep(2);
    EXPECT_EQ(llvm::cantFail(Other.CPU->reg(AArch64Register::X2)), 0u);
    OtherStep(0);
    Snapshot = llvm::cantFail(Other.CPU->saveContext());
    EXPECT_EQ(run().Kind, ExecutionExitKind::Stopped);
    llvm::cantFail(Other.CPU->restoreContext(*Snapshot));
    OtherStep(2);
    EXPECT_EQ(llvm::cantFail(Other.CPU->reg(AArch64Register::X2)), 1u);
  }
}

TEST_P(AArch64Atomic,
       PairZeroRegisterAndOverlappingComparisonUseOriginalInputs) {
  for (const auto &C : Cases) {
    if (!selected(C) || !C.compare())
      continue;
    SCOPED_TRACE(C.Name);
    input(C);
    // CAS[P] may compare and replace with the same original register(s).
    instruction(C.Word & ~uint32_t(RegisterMask));
    const auto Before = memory();
    auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(memory(), Before);
    input(C);
    const unsigned Source = C.Count == 2 ? RegisterMask - 1 : RegisterMask;
    instruction((C.Word & ~(uint32_t(RegisterMask) << SourceShift)) |
                (Source << SourceShift));
    // XZR is a zero comparison operand, never SP. A pair may end in XZR.
    const unsigned LastOffset = (C.Count - 1) * C.Width;
    llvm::cantFail(CPU->writeInteger(Data + LastOffset, 0, C.Width));
    const auto Old = llvm::cantFail(CPU->readInteger(Data, C.Width));
    if (C.Count == 2)
      llvm::cantFail(CPU->setReg(AArch64Register::X30, Old));
    const auto SP = llvm::cantFail(CPU->reg(AArch64Register::SP));
    Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, C.Width)), C.Expected);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::SP)), SP);
    if (C.Count == 2)
      EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X30)), Old);
  }
}

TEST_P(AArch64Atomic, OddPairRegisterEncodingIsRejectedBeforeAnyAccess) {
  for (const auto &C : Cases) {
    if (!selected(C) || C.Count != 2)
      continue;
    for (unsigned Shift : {0u, unsigned(SourceShift)}) {
      ASSERT_NO_FATAL_FAILURE(initialize());
      SCOPED_TRACE(C.Name);
      input(C);
      instruction(C.Word | (1u << Shift));
      const auto Before = memory();
      const auto Registers = registers();
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, uint32_t) { ADD_FAILURE(); };
      Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ADD_FAILURE(); };
      auto Exit = run(Hooks);
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
      EXPECT_EQ(memory(), Before);
      EXPECT_EQ(registers(), Registers);
      EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)), Code);
    }
  }
}

TEST_P(AArch64Atomic,
       FlagTransfersPreserveReservedBitsAndZeroRegisterSemantics) {
  for (uint64_t Value = 0; Value <= FlagsMask;
       Value += FlagsMask & ~(FlagsMask - 1)) {
    // All reserved input bits must be ignored by MSR; MRS zeroes them.
    llvm::cantFail(CPU->setReg(AArch64Register::X0, Value | ~FlagsMask));
    instruction(WriteNZCV);
    ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), Value);
    instruction(ReadNZCV);
    ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X2)), Value);
    const auto Registers = registers();
    instruction((ReadNZCV & ~RegisterMask) | RegisterMask);
    ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(registers(), Registers);
    instruction(WriteNZCV | RegisterMask);
    ASSERT_EQ(run().Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), 0u);
    EXPECT_EQ(registers(), Registers);
  }
}

TEST_P(AArch64Atomic, CommittedWritesRetireExecutableAliasesWithinTheSameRun) {
  const struct {
    Family Kind;
    uint64_t Source, Result;
  } Patches[] = {
#define NEVERD_ATOMIC_CODE_SOURCE(Kind, Source, Result)                        \
  {Family::Kind, Source, Result},
#include "AArch64AtomicCode.def"
#undef NEVERD_ATOMIC_CODE_SOURCE
  };
  llvm::cantFail(CPU->unmapAlias(Alias, PageBytes));
  llvm::cantFail(
      CPU->mapAlias(Alias, Code, PageBytes, Read | Write | UserAccessible));
  for (const auto &Patch : Patches)
    for (const auto &C : Cases) {
      if (!selected(C) || C.Kind != Patch.Kind)
        continue;
      SCOPED_TRACE(C.Name);
      input(C);
      const auto AtomicWord = C.Word;
      const uint32_t Words[] = {
#define NEVERD_ATOMIC_CODE_WORD(Word) Word,
#include "AArch64AtomicCode.def"
#undef NEVERD_ATOMIC_CODE_WORD
      };
      for (unsigned N = 0; N < std::size(Words); ++N)
        llvm::cantFail(CPU->writeInteger(Code + N * InstructionBytes, Words[N],
                                         InstructionBytes));
      const uint64_t Target = Alias + CodeTargetOffset;
      const auto Low = llvm::cantFail(CPU->readInteger(Target, C.Width));
      const auto High =
          llvm::cantFail(CPU->readInteger(Target + C.Width, C.Width));
      llvm::cantFail(
          CPU->setReg(AArch64Register::X0, C.compare() ? Low : Patch.Source));
      llvm::cantFail(CPU->setReg(AArch64Register::X1, High));
      llvm::cantFail(CPU->setReg(AArch64Register::X2, Patch.Source));
      llvm::cantFail(CPU->setReg(AArch64Register::X3, High));
      llvm::cantFail(CPU->setReg(AArch64Register::X4, Target));
      BackendHooks Hooks;
      Hooks.Instruction = [&](uint64_t PC, uint32_t) {
        if (PC == Code + CodeEndOffset)
          CPU->stop();
      };
      llvm::cantFail(CPU->installHooks(Hooks));
      auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X5)), Patch.Result);
      EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialFlags);
    }
}
INSTANTIATE_TEST_SUITE_P(Backends, AArch64Atomic,
                         testing::Combine(testing::ValuesIn(Profiles),
                                          testing::ValuesIn(Groups)),
                         [](const auto &P) {
                           return std::string(std::get<0>(P.param).Name) +
                                  NameSeparator + std::get<1>(P.param).Name;
                         });
} // namespace
} // namespace neverd::emulation
