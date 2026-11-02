//===- AArch64StructureMemoryTests.cpp - Checked NEON structure RAM -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x400000, Data = 0x800000, Alias = 0xc00000;
constexpr unsigned PageSize = 4096, Guard = 16, SnapshotBytes = 96;
enum Layout { Sequential, Interleaved, Lane, Replicate };
struct StructureCase {
  const char *Name;
  Layout Form;
  bool Load;
  unsigned Element, Registers, Width, Index, First, Base;
  int64_t Writeback;
  uint32_t Word;
  unsigned bytes() const {
    return Registers * ((Form == Lane || Form == Replicate) ? Element : Width);
  }
  // Map each memory byte to its register byte without decoding the word.
  std::pair<unsigned, unsigned> source(unsigned Offset) const {
    unsigned Register, Byte;
    if (Form == Sequential) {
      Register = Offset / Width;
      Byte = Offset % Width;
    } else if (Form == Interleaved) {
      Register = (Offset / Element) % Registers;
      Byte = (Offset / (Element * Registers)) * Element + Offset % Element;
    } else {
      Register = Offset / Element;
      Byte = Index * Element + Offset % Element;
    }
    return {(First + Register) % 32, Byte};
  }
};
const StructureCase Cases[] = {
#define NEVERD_ARM_STRUCTURE_CASE(Name, Form, Load, Element, Registers, Width, \
                                  Index, First, Base, Writeback, Word)         \
  {#Name, Form,  Load, Element,   Registers, Width,                            \
   Index, First, Base, Writeback, Word},
#include "AArch64StructureMemoryCases.def"
#undef NEVERD_ARM_STRUCTURE_CASE
};
void PrintTo(const StructureCase &C, std::ostream *OS) { *OS << C.Name; }
using Parameter = std::tuple<ExecutionBackendKind, bool, StructureCase>;
struct State {
  std::array<uint64_t, unsigned(AArch64Register::FPSR) + 1> Scalars;
  std::array<RegisterValue, 32> Vectors;
};
uint8_t byte(RegisterValue Value, unsigned Index) {
  return uint8_t(Value[Index / 8] >> ((Index % 8) * 8));
}
void setByte(RegisterValue &Value, unsigned Index, uint8_t Byte) {
  const unsigned Shift = (Index % 8) * 8;
  Value[Index / 8] = (Value[Index / 8] & ~(uint64_t(0xff) << Shift)) |
                     (uint64_t(Byte) << Shift);
}
class AArch64StructureMemory : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  uint64_t Address = Data + PageSize - 1;
  State Before;
  const StructureCase &testCase() const { return std::get<2>(GetParam()); }
  AArch64Register baseRegister() const {
    return testCase().Base == 31 ? AArch64Register::SP
                                 : AArch64Register(testCase().Base);
  }
  void SetUp() override {
    const auto Backend = std::get<0>(GetParam());
    auto Created = createExecutionBackend(
        Backend,
        std::get<1>(GetParam()) ? ExecutionContract::CheckedUserAArch64
                                : ExecutionContract::CheckedAArch64,
        4 * 1024 * 1024, GuestArchitecture::AArch64);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Text = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(Backend, GuestArchitecture::AArch64))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    CPU = std::move(Created->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    // Keep adjacent virtual pages physically separate.
    llvm::cantFail(
        CPU->map(Data + 4 * PageSize, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->map(Data + PageSize, PageSize, Read | Write | UserAccessible));
    seed();
  }
  State state() const {
    State S;
    for (unsigned I = 0; I < S.Scalars.size(); ++I)
      S.Scalars[I] = llvm::cantFail(CPU->reg(AArch64Register(I)));
    for (unsigned I = 0; I < S.Vectors.size(); ++I)
      S.Vectors[I] = llvm::cantFail(
          CPU->readRegister(vectorRegister(GuestArchitecture::AArch64, I)));
    return S;
  }
  std::array<uint8_t, SnapshotBytes> original() const {
    std::array<uint8_t, SnapshotBytes> Bytes;
    for (unsigned I = 0; I < Bytes.size(); ++I)
      Bytes[I] = uint8_t(0x93 + I * 29);
    return Bytes;
  }
  void seed() {
    uint8_t Bytes[8];
    llvm::support::endian::write32le(Bytes, testCase().Word);
    llvm::support::endian::write32le(Bytes + 4, 0xd503201f);
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->write(Address - Guard, original()));
    for (unsigned I = 0; I < 31; ++I)
      llvm::cantFail(
          CPU->setReg(AArch64Register(I), 0x987654321000ULL + 0x101 * I));
    llvm::cantFail(CPU->setReg(AArch64Register::X3, uint64_t(-19)));
    llvm::cantFail(CPU->setReg(AArch64Register::SP, Data + 2 * PageSize - 16));
    llvm::cantFail(CPU->setReg(baseRegister(), Address));
    llvm::cantFail(CPU->setReg(AArch64Register::PC, Code));
    llvm::cantFail(CPU->setReg(AArch64Register::NZCV, 0xb0000000));
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 0x03400000));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 0x08000002));
    llvm::cantFail(CPU->setReg(AArch64Register::TPIDR_EL0, 0x24680000));
    for (unsigned I = 0; I < 32; ++I) {
      RegisterValue V{};
      for (unsigned B = 0; B < 16; ++B)
        setByte(V, B, uint8_t(7 + 17 * I + 11 * B));
      llvm::cantFail(
          CPU->writeRegister(vectorRegister(GuestArchitecture::AArch64, I), V));
    }
    Before = state();
  }
  std::array<uint8_t, SnapshotBytes> memory(bool Observer = false) const {
    std::array<uint8_t, SnapshotBytes> Bytes;
    if (Observer)
      llvm::cantFail(CPU->read(Address - Guard, Bytes));
    else
      llvm::cantFail(CPU->snapshotBacking(Address - Guard, Bytes));
    return Bytes;
  }
  void expectResult(bool Executed) {
    auto Expected = Before;
    auto Bytes = original();
    const auto &C = testCase();
    if (Executed) {
      Expected.Scalars[unsigned(AArch64Register::PC)] += 4;
      Expected.Scalars[unsigned(baseRegister())] += uint64_t(C.Writeback);
      if (C.Load && C.Form != Lane)
        for (unsigned I = 0; I < C.Registers; ++I)
          Expected.Vectors[(C.First + I) % 32] = {};
      for (unsigned I = 0; I < C.bytes(); ++I) {
        const auto [Register, Byte] = C.source(I);
        if (!C.Load)
          Bytes[Guard + I] = byte(Before.Vectors[Register], Byte);
        else if (C.Form == Replicate)
          for (unsigned B = Byte; B < C.Width; B += C.Element)
            setByte(Expected.Vectors[Register], B, Bytes[Guard + I]);
        else
          setByte(Expected.Vectors[Register], Byte, Bytes[Guard + I]);
      }
    }
    const auto Actual = state();
    EXPECT_EQ(Actual.Scalars, Expected.Scalars);
    EXPECT_EQ(Actual.Vectors, Expected.Vectors);
    EXPECT_EQ(memory(), Bytes);
    uint8_t Instruction[4];
    llvm::cantFail(CPU->snapshotBacking(Code, Instruction));
    EXPECT_EQ(llvm::support::endian::read32le(Instruction), C.Word);
  }
  ExecutionExit run(BackendHooks Hooks = {}) {
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, 1000000));
  }
};
TEST_P(AArch64StructureMemory, EveryCrossingUsesExactOrderedElements) {
  const auto &C = testCase();
  for (unsigned Prefix = 1; Prefix <= C.bytes(); ++Prefix) {
    SCOPED_TRACE(Prefix);
    Address = Data + PageSize - Prefix;
    seed();
    unsigned Offset = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, uint32_t Size) {
      EXPECT_TRUE(C.Load);
      EXPECT_EQ(A, Address + Offset);
      EXPECT_EQ(Size, C.Element);
      EXPECT_EQ(memory(true), original());
      Offset += Size;
    };
    Hooks.Write = [&](uint64_t A, uint32_t Size, uint64_t Value) {
      EXPECT_FALSE(C.Load);
      EXPECT_EQ(A, Address + Offset);
      EXPECT_EQ(Size, C.Element);
      uint64_t Expected = 0;
      for (unsigned B = 0; B < Size; ++B) {
        const auto [Register, Byte] = C.source(Offset + B);
        Expected |= uint64_t(byte(Before.Vectors[Register], Byte)) << (B * 8);
      }
      EXPECT_EQ(Value, Expected);
      EXPECT_EQ(memory(true), original());
      Offset += Size;
    };
    const auto Exit = run(std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Offset, C.bytes());
    expectResult(true);
  }
}
TEST_P(AArch64StructureMemory, StopAtEveryElementRetainsFullStateAndRAM) {
  for (unsigned StopAt = 1; StopAt <= testCase().bytes() / testCase().Element;
       ++StopAt) {
    seed();
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
TEST_P(AArch64StructureMemory, DeniedFinalByteCannotPublishAnyPrefix) {
  Address = Data + PageSize - testCase().bytes() + 1;
  seed();
  llvm::cantFail(CPU->protect(Data + PageSize, PageSize, UserAccessible));
  const auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
  expectResult(false);
}
TEST_P(AArch64StructureMemory, DoesNotAccessBeyondTheLastElement) {
  Address = Data + PageSize - testCase().bytes();
  seed();
  llvm::cantFail(CPU->protect(Data + PageSize, PageSize, UserAccessible));
  const auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  expectResult(true);
}
TEST_P(AArch64StructureMemory, AliasesObserveCommittedBytesAndContextCanRetry) {
  llvm::cantFail(
      CPU->mapAlias(Alias, Data, 2 * PageSize, Read | Write | UserAccessible));
  auto Context = llvm::cantFail(CPU->saveContext());
  const auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  expectResult(true);
  std::array<uint8_t, SnapshotBytes> Aliased;
  llvm::cantFail(CPU->snapshotBacking(Alias + Address - Data - Guard, Aliased));
  EXPECT_EQ(Aliased, memory());
  llvm::cantFail(CPU->restoreContext(*Context));
  llvm::cantFail(CPU->write(Address - Guard, original()));
  const auto Retry = run();
  ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
  expectResult(true);
}
TEST_P(AArch64StructureMemory, WrappingFootprintIsRejectedBeforeObservers) {
  // A single byte cannot wrap. Its out-of-range address is a normal fault.
  if (testCase().bytes() == 1)
    return;
  llvm::cantFail(
      CPU->setReg(baseRegister(), UINT64_MAX - testCase().bytes() + 2));
  Before = state();
  unsigned Seen = 0;
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t, uint32_t) { ++Seen; };
  Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ++Seen; };
  const auto Exit = run(std::move(Hooks));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Seen, 0u);
  expectResult(false);
}
INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64StructureMemory,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool(), testing::ValuesIn(Cases)));

struct ReservedCase {
  const char *Name;
  uint32_t Word;
};
const ReservedCase ReservedCases[] = {
#define NEVERD_ARM_STRUCTURE_RESERVED(Name, Word) {#Name, Word},
#include "AArch64StructureMemoryCases.def"
#undef NEVERD_ARM_STRUCTURE_RESERVED
};
void PrintTo(const ReservedCase &C, std::ostream *OS) { *OS << C.Name; }
class AArch64StructureReserved
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, bool, ReservedCase>> {};
TEST_P(AArch64StructureReserved, RejectsBeforeMemoryObserversOrCPUEntry) {
  const auto [Backend, User, C] = GetParam();
  auto Created =
      createExecutionBackend(Backend,
                             User ? ExecutionContract::CheckedUserAArch64
                                  : ExecutionContract::CheckedAArch64,
                             4 * 1024 * 1024, GuestArchitecture::AArch64);
  if (!Created) {
    auto E = Created.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    auto Text = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(Backend, GuestArchitecture::AArch64))
      GTEST_SKIP() << Text;
    FAIL() << Text;
  }
  auto &CPU = Created->CPU;
  llvm::cantFail(
      CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
  uint8_t Bytes[4];
  llvm::support::endian::write32le(Bytes, C.Word);
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->setReg(AArch64Register::X1, Data));
  unsigned Seen = 0;
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t, uint32_t) { ++Seen; };
  Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ++Seen; };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, 1000000));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Seen, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)), Code);
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X1)), Data);
}
INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64StructureReserved,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool(), testing::ValuesIn(ReservedCases)));
} // namespace
} // namespace neverd::emulation
