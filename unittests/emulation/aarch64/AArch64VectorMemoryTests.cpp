//===- AArch64VectorMemoryTests.cpp - Complete checked SIMD RAM effects ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <climits>
#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_AARCH64_FP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "AArch64FPCases.def"
#undef NEVERD_AARCH64_FP_VALUE
#define NEVERD_ARM_VECTOR_MEMORY_VALUE(Name, Value)                            \
  constexpr uint64_t Name = Value;
#include "AArch64VectorMemoryCases.def"
#undef NEVERD_ARM_VECTOR_MEMORY_VALUE
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
};
const Profile Profiles[] = {
#define NEVERD_ARM_CROSS_PROFILE(Name, Backend, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, ExecutionContract::Contract},
#include "AArch64CrossPageCases.def"
#undef NEVERD_ARM_CROSS_PROFILE
};
struct InstructionCase {
  const char *Name;
  bool Load;
  unsigned Width, Count, First, Second;
  int64_t Offset, Writeback;
  uint32_t Word;
  unsigned bytes() const { return Width * Count; }
  unsigned observers() const {
    return Load ? Count : Count * ((Width + WordBytes - 1) / WordBytes);
  }
};
const InstructionCase Cases[] = {
#define NEVERD_ARM_VECTOR_MEMORY_INSTRUCTION(Name, Load, Width, Count, First,  \
                                             Second, Offset, Writeback, Word)  \
  {#Name, Load, Width, Count, First, Second, Offset, Writeback, Word},
#include "AArch64VectorMemoryCases.def"
#undef NEVERD_ARM_VECTOR_MEMORY_INSTRUCTION
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
void PrintTo(const InstructionCase &C, std::ostream *OS) { *OS << C.Name; }

class AArch64VectorMemory
    : public testing::TestWithParam<std::tuple<Profile, InstructionCase>> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  uint64_t Address = 0;
  const InstructionCase &testCase() const { return std::get<1>(GetParam()); }
  void SetUp() override {
    const auto &P = std::get<0>(GetParam());
    auto Created = createExecutionBackend(P.Backend, P.Contract, Limit,
                                          GuestArchitecture::AArch64);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(P.Backend, GuestArchitecture::AArch64))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    CPU = std::move(Created->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->map(Interposed, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->map(Data + PageSize, PageSize, Read | Write | UserAccessible));
    uint8_t Bytes[WordBytes];
    llvm::support::endian::write32le(Bytes, testCase().Word);
    llvm::support::endian::write32le(Bytes + sizeof(uint32_t), Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    Address = Data + PageSize - testCase().bytes() / RegisterCount;
    seed();
  }
  uint64_t base() const { return Address - uint64_t(testCase().Offset); }
  std::array<uint8_t, MaxBytes> original() const {
    std::array<uint8_t, MaxBytes> Bytes{};
    const uint64_t Values[] = {FirstMemoryLow, FirstMemoryHigh, SecondMemoryLow,
                               SecondMemoryHigh};
    for (unsigned Index = 0; Index < std::size(Values); ++Index)
      llvm::support::endian::write64le(Bytes.data() + Index * WordBytes,
                                       Values[Index]);
    return Bytes;
  }
  RegisterValue initial(unsigned Index) const {
    return Index == testCase().First
               ? RegisterValue{InitialVectorLow, InitialVectorHigh}
               : RegisterValue{SecondVectorLow, SecondVectorHigh};
  }
  void seed() {
    llvm::cantFail(CPU->write(Address, original()));
    for (auto Index : {testCase().First, testCase().Second})
      llvm::cantFail(CPU->writeRegister(
          vectorRegister(GuestArchitecture::AArch64, Index), initial(Index)));
    llvm::cantFail(CPU->setReg(AArch64Register::X1, base()));
    llvm::cantFail(CPU->setReg(AArch64Register::NZCV, InitialNZCV));
  }
  std::array<uint8_t, MaxBytes> memory(bool Observer = false) const {
    std::array<uint8_t, MaxBytes> Bytes{};
    if (Observer)
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
  void expectCPU(bool Executed) {
    auto Bytes = original();
    unsigned Operand = 0;
    for (auto Index : {testCase().First, testCase().Second}) {
      auto Expected = initial(Index);
      if (Executed && testCase().Load && Operand < testCase().Count) {
        Expected = {};
        for (unsigned Byte = 0; Byte < testCase().Width; ++Byte)
          Expected[Byte / WordBytes] |=
              uint64_t(Bytes[Operand * testCase().Width + Byte])
              << ((Byte % WordBytes) * CHAR_BIT);
      }
      EXPECT_EQ(llvm::cantFail(CPU->readRegister(
                    vectorRegister(GuestArchitecture::AArch64, Index))),
                Expected)
          << Index;
      ++Operand;
    }
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X1)),
              base() + (Executed ? uint64_t(testCase().Writeback) : 0));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)),
              Code + (Executed ? sizeof(uint32_t) : 0));
  }
  void expectResult(bool Executed) {
    auto Expected = original();
    if (Executed && !testCase().Load) {
      const RegisterValue Values[] = {initial(testCase().First),
                                      initial(testCase().Second)};
      for (unsigned Operand = 0; Operand < testCase().Count; ++Operand)
        for (unsigned Byte = 0; Byte < testCase().Width; ++Byte)
          Expected[Operand * testCase().Width + Byte] =
              uint8_t(Values[Operand][Byte / WordBytes] >>
                      ((Byte % WordBytes) * CHAR_BIT));
    }
    EXPECT_EQ(memory(), Expected);
    expectCPU(Executed);
  }
};
TEST_P(AArch64VectorMemory,
       ExecutesEveryCrossingWithOrderedFullWidthObservers) {
  for (unsigned Prefix = 1; Prefix < testCase().bytes(); ++Prefix) {
    SCOPED_TRACE(Prefix);
    Address = Data + PageSize - Prefix;
    seed();
    unsigned Seen = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, uint32_t Size) {
      EXPECT_EQ(A, Address + Seen * testCase().Width);
      EXPECT_EQ(Size, testCase().Width);
      EXPECT_EQ(memory(true), original());
      ++Seen;
    };
    Hooks.Write = [&](uint64_t A, uint32_t Size, uint64_t Value) {
      const unsigned Words = (testCase().Width + WordBytes - 1) / WordBytes;
      const unsigned Operand = Seen / Words, Lane = Seen % Words;
      EXPECT_EQ(A, Address + Operand * testCase().Width + Lane * WordBytes);
      EXPECT_EQ(Size, std::min<uint64_t>(testCase().Width, WordBytes));
      const auto Initial =
          initial(Operand ? testCase().Second : testCase().First);
      const uint64_t Mask = UINT64_MAX >> ((WordBytes - Size) * CHAR_BIT);
      EXPECT_EQ(Value, Initial[Lane] & Mask);
      EXPECT_EQ(memory(true), original());
      ++Seen;
    };
    const auto Exit = run(std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Seen, testCase().observers());
    expectResult(true);
  }
}
TEST_P(AArch64VectorMemory, StopAtEveryObserverRetainsRAMVectorsAndWriteback) {
  for (unsigned StopAt = 1; StopAt <= testCase().observers(); ++StopAt) {
    SCOPED_TRACE(StopAt);
    unsigned Seen = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, uint32_t) {
      if (++Seen == StopAt)
        CPU->stop();
    };
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) {
      if (++Seen == StopAt)
        CPU->stop();
    };
    const auto Exit = run(std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Seen, StopAt);
    expectResult(false);
  }
}
TEST_P(AArch64VectorMemory, DeniedSecondPageCannotPublishAPrefix) {
  llvm::cantFail(CPU->protect(Data + PageSize, PageSize, UserAccessible));
  const auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
  expectResult(false);
}
TEST_P(AArch64VectorMemory,
       SharedPhysicalAliasesObserveTheCompleteCommittedStore) {
  llvm::cantFail(CPU->mapAlias(Alias, Data, RegisterCount * PageSize,
                               Read | Write | UserAccessible));
  const auto Offset = Address - Data;
  const auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  expectResult(true);
  std::array<uint8_t, MaxBytes> Aliased{};
  llvm::cantFail(CPU->snapshotBacking(Alias + Offset, Aliased));
  EXPECT_EQ(Aliased, memory());
}
TEST_P(AArch64VectorMemory, RestoredContextReinstallsVectorInputsBeforeEntry) {
  auto Context = llvm::cantFail(CPU->saveContext());
  const auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  llvm::cantFail(CPU->restoreContext(*Context));
  llvm::cantFail(CPU->write(Address, original()));
  const auto Retry = run();
  ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
  expectResult(true);
}
INSTANTIATE_TEST_SUITE_P(Profiles, AArch64VectorMemory,
                         testing::Combine(testing::ValuesIn(Profiles),
                                          testing::ValuesIn(Cases)));
} // namespace
} // namespace neverd::emulation
