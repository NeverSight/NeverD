//===- X64WordLaneTests.cpp - Checked SSE2 word insertion/extraction ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64VectorTestSupport.h"

#include <stdexcept>

namespace neverd::emulation {
namespace {
using namespace vector_test;
constexpr Input Words{{0x0123456789abcdefULL, 0xfedcba9876543210ULL},
                      {0xa5a5f00ddead8001ULL, 0x0123456789abcdefULL}};
constexpr unsigned Controls[] = {0, 1, 2, 3, 4, 5, 6, 7, 0x80, 0xf9, 0xff};
constexpr unsigned Registers[] = {0, 7, 8, 15};
constexpr unsigned ElementBytes = 2;

// Original SSE2 encodings, independent of the admission inventory. PEXTRW
// reverses the ModRM roles: reg selects the GPR and r/m selects the XMM.
std::vector<uint8_t> instruction(bool Extract, bool Memory, unsigned Vector,
                                 bool ExtendedGPR, unsigned Control,
                                 bool RexW = false) {
  const unsigned General = ExtendedGPR ? 15 : 0;
  const unsigned Reg = Extract ? General : Vector;
  const unsigned RM = Memory ? 1 : Extract ? Vector : General;
  std::vector<uint8_t> Bytes{0x66};
  const unsigned Rex =
      0x40 | (RexW ? 8 : 0) | (Reg >= 8 ? 4 : 0) | (RM >= 8 ? 1 : 0);
  if (Rex != 0x40)
    Bytes.push_back(Rex);
  Bytes.push_back(0x0f);
  Bytes.push_back(Extract ? 0xc5 : 0xc4);
  Bytes.push_back((Memory ? 0 : 0xc0) | ((Reg & 7) << 3) | (RM & 7));
  Bytes.push_back(Control);
  return Bytes;
}

uint64_t word(const RegisterValue &V, unsigned Control) {
  const unsigned Lane = Control & 7;
  return (V[Lane / 4] >> (16 * (Lane % 4))) & UINT16_MAX;
}
void insert(RegisterValue &V, unsigned Control, uint64_t Value) {
  const unsigned Lane = Control & 7;
  const unsigned Shift = 16 * (Lane % 4);
  V[Lane / 4] = (V[Lane / 4] & ~(uint64_t(UINT16_MAX) << Shift)) |
                ((Value & UINT16_MAX) << Shift);
}

class X64WordLane : public X64VectorTest {
protected:
  void initialize(unsigned Vector, uint64_t Address = Data) {
    seed(Words, Address);
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->setXmm(Vector, Words.Left));
    llvm::cantFail(CPU->setReg(X64Register::AX, Words.Right[0]));
    llvm::cantFail(CPU->setReg(X64Register::R15, Words.Right[0]));
  }
  void mapSecondPage() {
    llvm::cantFail(
        CPU->map(Data + PageSize, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->mapAlias(Alias + PageSize, Data + PageSize, PageSize,
                                 Read | Write | UserAccessible));
  }
  void checkRegisters(bool Extract) {
    for (unsigned Vector : Registers)
      for (unsigned Control : Controls)
        for (bool Extended : {false, true})
          for (bool RexW : {false, true}) {
            SCOPED_TRACE(Vector);
            SCOPED_TRACE(Control);
            SCOPED_TRACE(Extended);
            SCOPED_TRACE(RexW);
            initialize(Vector);
            auto Expected = snapshot();
            if (Extract)
              Expected[Extended ? CPURegister::X64R15 : CPURegister::X64AX] = {
                  word(Words.Left, Control), 0};
            else
              insert(Expected[vectorRegister(GuestArchitecture::X64, Vector)],
                     Control, Words.Right[0]);
            unsigned Accesses = 0;
            BackendHooks Hooks;
            Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
            Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
            const auto Bytes =
                instruction(Extract, false, Vector, Extended, Control, RexW);
            const auto Exit = run(Bytes, std::move(Hooks));
            ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
            Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
            EXPECT_EQ(snapshot(), Expected);
            EXPECT_EQ(Accesses, 0u);
          }
  }
};

TEST_P(X64WordLane, InsertRegisterPreservesOtherLanesAndState) {
  checkRegisters(false);
}
TEST_P(X64WordLane, ExtractRegisterZeroExtendsAndPreservesSource) {
  checkRegisters(true);
}

TEST_P(X64WordLane, MemoryReadsExactlyTwoBytesIncludingPageEndAndUnaligned) {
  for (unsigned Vector : Registers)
    for (unsigned Control : Controls)
      for (uint64_t Offset : {uint64_t(1), PageSize - ElementBytes}) {
        initialize(Vector, Data + Offset);
        llvm::cantFail(
            CPU->writeInteger(Data + Offset, Words.Right[0], ElementBytes));
        auto Expected = snapshot();
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t Address, unsigned Size) {
          EXPECT_EQ(Address, Data + Offset);
          EXPECT_EQ(Size, ElementBytes);
          EXPECT_EQ(snapshot(), Expected);
          ++Reads;
        };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
        // The next page is absent: a widened read would fault at page end.
        const auto Bytes = instruction(false, true, Vector, false, Control);
        const auto Exit = run(Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        insert(Expected[vectorRegister(GuestArchitecture::X64, Vector)],
               Control, Words.Right[0]);
        Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
        EXPECT_EQ(snapshot(), Expected);
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(Writes, 0u);
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + Offset, ElementBytes)),
                  Words.Right[0] & UINT16_MAX);
      }
}

TEST_P(X64WordLane, CrossPageFaultsPreserveStateAndRetry) {
  enum class Mapping { Absent, Denied, SupervisorOnly };
  for (unsigned Page : {0u, 1u})
    for (auto Kind :
         {Mapping::Absent, Mapping::Denied, Mapping::SupervisorOnly}) {
      if (Kind == Mapping::SupervisorOnly && !GetParam().User)
        continue;
      reset();
      ASSERT_FALSE(HasFatalFailure());
      mapSecondPage();
      const uint64_t Address = Data + PageSize - 1;
      initialize(15, Address);
      llvm::cantFail(CPU->writeInteger(Address, Words.Right[0], ElementBytes));
      const auto Before = snapshot();
      const uint64_t PageAddress = Data + Page * PageSize;
      if (Kind == Mapping::Absent)
        llvm::cantFail(CPU->addressSpace()->unmap(PageAddress, PageSize));
      else
        llvm::cantFail(CPU->protect(
            PageAddress, PageSize,
            Kind == Mapping::Denied ? Write | UserAccessible : Read | Write));
      BackendHooks Hooks;
      Hooks.RecoverableFault = [](const BackendFault &) { return true; };
      const auto Bytes = instruction(false, true, 15, false, 0xff);
      const auto Exit = run(Bytes, std::move(Hooks));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
          << Exit.Diagnostic;
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->Address, Page ? PageAddress : Address);
      EXPECT_EQ(Exit.Fault->Size, 1u);
      EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
      EXPECT_EQ(Exit.Fault->Kind, Kind == Mapping::Absent
                                      ? BackendFaultKind::UnmappedMemory
                                      : BackendFaultKind::Protection);
      EXPECT_EQ(snapshot(), Before);
      ASSERT_TRUE(CPU->takeRecoverableFault());
      if (Kind == Mapping::Absent)
        llvm::cantFail(CPU->mapAlias(PageAddress, Alias + Page * PageSize,
                                     PageSize, Read | Write | UserAccessible));
      else
        llvm::cantFail(
            CPU->protect(PageAddress, PageSize, Read | Write | UserAccessible));
      const auto Retry = run(Bytes);
      ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
      auto Expected = Before;
      insert(Expected[CPURegister::X64V15], 0xff, Words.Right[0]);
      Expected[CPURegister::X64PC] = {Code + Bytes.size(), 0};
      EXPECT_EQ(snapshot(), Expected);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, ElementBytes)),
                Words.Right[0] & UINT16_MAX);
    }
}

TEST_P(X64WordLane, ObserverCancellationPrecedesRegisterEffects) {
  for (bool Fail : {false, true}) {
    reset();
    ASSERT_FALSE(HasFatalFailure());
    initialize(15);
    const auto Before = snapshot();
    BackendHooks Hooks;
    unsigned Reads = 0;
    Hooks.Read = [&](uint64_t Address, unsigned Size) {
      EXPECT_EQ(Address, Data);
      EXPECT_EQ(Size, ElementBytes);
      ++Reads;
      if (Fail)
        throw std::runtime_error("word insertion observer failure");
      CPU->stop();
    };
    const auto Exit =
        run(instruction(false, true, 15, false, 7), std::move(Hooks));
    EXPECT_EQ(Exit.Kind, Fail ? ExecutionExitKind::BackendFailure
                              : ExecutionExitKind::Stopped);
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(Reads, 1u);
  }
}

TEST_P(X64WordLane, UnsupportedNeighborFormsRejectBeforeDataEffects) {
  const std::vector<std::vector<uint8_t>> Forms{
      {0x0f, 0xc4, 0x01, 0xff},                   // MMX PINSRW
      {0x0f, 0xc5, 0xc0, 0xff},                   // MMX PEXTRW
      {0xc5, 0xf9, 0xc4, 0x01, 0xff},             // VPINSRW
      {0xc5, 0xf9, 0xc5, 0xc0, 0xff},             // VPEXTRW
      {0x62, 0xf1, 0x7d, 0x08, 0xc4, 0x01, 0xff}, // EVEX VPINSRW
      {0x66, 0x0f, 0x3a, 0x15, 0xc0, 0xff},       // SSE4.1 PEXTRW
      {0x66, 0x0f, 0x3a, 0x15, 0x01, 0xff},       // SSE4.1 m16
      {0x66, 0x0f, 0x3a, 0x20, 0x01, 0xff},       // PINSRB
      {0x66, 0x0f, 0x3a, 0x22, 0x01, 0xff},       // PINSRD
      {0x66, 0x48, 0x0f, 0x3a, 0x22, 0x01, 0xff}, // PINSRQ
      {0x66, 0x0f, 0x3a, 0x14, 0x01, 0xff},       // PEXTRB
      {0x66, 0x0f, 0x3a, 0x16, 0x01, 0xff},       // PEXTRD
      {0x66, 0x48, 0x0f, 0x3a, 0x16, 0x01, 0xff}, // PEXTRQ
      {0xf0, 0x66, 0x0f, 0xc4, 0x01, 0xff}};      // LOCK PINSRW
  for (const auto &Bytes : Forms) {
    reset();
    ASSERT_FALSE(HasFatalFailure());
    initialize(0);
    const auto Before = snapshot();
    unsigned Accesses = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, unsigned) { ++Accesses; };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Accesses; };
    const auto Exit = run(Bytes, std::move(Hooks));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), Before);
    EXPECT_EQ(Accesses, 0u);
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64WordLane,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
