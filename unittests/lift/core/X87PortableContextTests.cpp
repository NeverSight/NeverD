//===- X87PortableContextTests.cpp - Portable x87 state tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Raw payload/provenance, physical TOP mapping, transactional rejection and
/// independent original-instruction observations. Undefined fields are named.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/X87Capabilities.h"
#include "neverd/ir/X87Context.h"
#include "neverd/ir/low/X87Arithmetic.h"
#include "neverd/ir/low/X87PortableContext.h"

#include <algorithm>
#include <array>
#include <random>

#ifdef NEVERD_X87_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#include "unicorn/x86.h"
#endif

using namespace neverd;

namespace {
using Image = std::array<uint8_t, 512>;
using Outcome = X87PortableOutcome;
constexpr uint64_t IntegerBit = UINT64_C(1) << 63;
constexpr uint64_t GuestIP = 0x100000;
constexpr uint64_t GuestDP = 0x200000;
uint16_t word(const Image &S, unsigned Offset = 0) {
  return uint16_t(S[Offset]) | (uint16_t(S[Offset + 1]) << 8);
}
void putWord(Image &S, unsigned Offset, uint16_t Value) {
  S[Offset] = Value;
  S[Offset + 1] = Value >> 8;
}
uint64_t address(const Image &S, unsigned Offset) {
  uint64_t Value = 0;
  for (unsigned I = 0; I != 8; ++I)
    Value |= uint64_t(S[Offset + I]) << (8 * I);
  return Value;
}
X87Binary80Bytes bits(uint64_t Significand, uint16_t Exponent) {
  return X87Binary80{Significand, Exponent}.bytes();
}
void put(Image &S, unsigned Logical, const X87Binary80Bytes &Value) {
  std::copy(Value.begin(), Value.end(), S.begin() + 32 + Logical * 16);
}
X87Binary80Bytes get(const Image &S, unsigned Logical) {
  X87Binary80Bytes Value{};
  std::copy_n(S.begin() + 32 + Logical * 16, Value.size(), Value.begin());
  return Value;
}
Image initial(unsigned Top = 0, uint8_t Tags = 0xff) {
  Image S{};
  putWord(S, 0, 0x037f);
  putWord(S, 2, Top << 11);
  putWord(S, 6, 0x0571);
  S[4] = Tags;
  // FXSAVE64-compatible MXCSR for the independent reference's FXRSTOR64.
  putWord(S, 24, 0x1f80);
  for (unsigned I = 0; I != 8; ++I)
    put(S, I, bits(IntegerBit + ((Top + I) & 7), 0x3fff));
  return S;
}
uint32_t mem(unsigned Escape, unsigned Group) {
  const unsigned ModRM = Group << 3;
  return Escape | (ModRM << 8) | X87ContextMemory |
         ((ModRM | 2) << X87ContextOriginalModRMShift);
}
Outcome step(uint32_t Selector, Image &S, uint8_t &Known,
             std::span<uint8_t> Memory = {}) {
  uint64_t Output = 0;
  return executePortableX87Context(Selector, S, Known, Memory, GuestIP, GuestDP,
                                   Output);
}

TEST(X87PortableContext, InitializePreservesPhysicalPayloadAndProvenance) {
  for (unsigned Top = 0; Top != 8; ++Top) {
    auto S = initial(Top);
    putWord(S, 2, (Top << 11) | 0x82ff);
    uint8_t Known = 0x35;
    ASSERT_EQ(step(0xe3db, S, Known), Outcome::Completed);
    EXPECT_EQ(Known, 0x35);
    EXPECT_EQ(word(S), 0x037f);
    EXPECT_EQ(word(S, 2), 0);
    EXPECT_EQ(S[4], 0);
    EXPECT_EQ(word(S, 6), 0);
    EXPECT_EQ(address(S, 8), 0);
    EXPECT_EQ(address(S, 16), 0);
    for (unsigned Physical = 0; Physical != 8; ++Physical)
      EXPECT_EQ(get(S, Physical), bits(IntegerBit + Physical, 0x3fff));
  }
}

TEST(X87PortableContext, RawTransportRetainsEveryEncodingAndEmptyPayload) {
  for (unsigned Top = 0; Top != 8; ++Top)
    for (const auto &Raw :
         {bits(0, 0x8000), bits(1, 0), bits(IntegerBit, 0), bits(1, 0x4000),
          bits(UINT64_C(0xc000000000000123), 0xffff),
          bits(UINT64_C(0x8000000000001234), 0x7fff)}) {
      auto S = initial(Top, 0);
      uint8_t Known = 0;
      auto Operand = Raw;
      ASSERT_EQ(step(mem(0xdb, 5), S, Known, Operand), Outcome::Completed);
      EXPECT_EQ((word(S, 2) >> 11) & 7, (Top + 7) & 7);
      EXPECT_EQ(S[4], 1U << ((Top + 7) & 7));
      EXPECT_EQ(Known, S[4]);
      EXPECT_EQ(get(S, 0), Raw);
      EXPECT_EQ(word(S, 2) & 0x027f, 0);
      EXPECT_EQ(address(S, 8), GuestIP);
      EXPECT_EQ(address(S, 16), GuestDP);
      EXPECT_EQ(word(S, 6), 0x0571);
      Operand.fill(0x55);
      ASSERT_EQ(step(mem(0xdb, 7), S, Known, Operand), Outcome::Completed);
      EXPECT_EQ(Operand, Raw);
      EXPECT_EQ((word(S, 2) >> 11) & 7, Top);
      EXPECT_EQ(S[4], 0);
      EXPECT_EQ(Known, 1U << ((Top + 7) & 7));
      EXPECT_EQ(get(S, 7), Raw); // Pop retains the now-empty physical slot.
    }
}

TEST(X87PortableContext, RegisterCopiesExchangeAndPopUsePhysicalIdentity) {
  for (unsigned Top = 0; Top != 8; ++Top) {
    auto S = initial(Top);
    uint8_t Known = 0xff;
    const auto Before = S;
    ASSERT_EQ(step(0xcbd9, S, Known), Outcome::Completed); // FXCH ST3
    EXPECT_EQ(get(S, 0), get(Before, 3));
    EXPECT_EQ(get(S, 3), get(Before, 0));
    ASSERT_EQ(step(0xdcdd, S, Known), Outcome::Completed); // FSTP ST4
    EXPECT_EQ(get(S, 3), get(Before, 3));
    EXPECT_EQ(S[4], uint8_t(0xff & ~(1U << Top)));
    ASSERT_EQ(step(0xc2d9, S, Known), Outcome::Completed); // FLD ST2
    EXPECT_EQ((word(S, 2) >> 11) & 7, Top);
    EXPECT_EQ(get(S, 0), get(Before, 0));
    EXPECT_EQ(S[4], 0xff);
  }
}

TEST(X87PortableContext, ArithmeticFormsPreserveOperandOrderAndPop) {
  const auto Two = bits(IntegerBit, 0x4000);
  const auto Seven = bits(UINT64_C(0xe000000000000000), 0x4001);
  for (unsigned Top = 0; Top != 8; ++Top)
    for (unsigned Escape : {0xd8U, 0xdcU, 0xdeU})
      for (unsigned ModRM : {0xc3U, 0xcbU, 0xe3U, 0xebU, 0xf3U, 0xfbU}) {
        auto S = initial(Top);
        put(S, 0, Two);
        put(S, 3, Seven);
        uint8_t Known = 0xff;
        const auto Selector = Escape | (ModRM << 8);
        const auto Kind = x87InstructionForm(Selector)->PortableOperation;
        const bool Reverse = Kind == X87PortableOperation::ReverseSubtract ||
                             Kind == X87PortableOperation::ReverseDivide;
        auto Left = Escape == 0xd8 ? Two : Seven;
        auto Right = Escape == 0xd8 ? Seven : Two;
        if (Reverse)
          std::swap(Left, Right);
        const auto Numeric =
            (ModRM & 0x38) == 0     ? X87ArithmeticOperation::Add
            : (ModRM & 0x38) == 8   ? X87ArithmeticOperation::Multiply
            : (ModRM & 0x38) < 0x30 ? X87ArithmeticOperation::Subtract
                                    : X87ArithmeticOperation::Divide;
        auto Expected =
            evaluateFiniteX87Arithmetic(Numeric, Left, Right, 0x37f);
        ASSERT_TRUE(Expected);
        ASSERT_EQ(step(Selector, S, Known), Outcome::Completed);
        EXPECT_EQ(get(S, Escape == 0xd8   ? 0
                         : Escape == 0xde ? 2
                                          : 3),
                  Expected->Value);
        EXPECT_EQ((word(S, 2) >> 11) & 7, (Top + (Escape == 0xde)) & 7);
        EXPECT_EQ(S[4], Escape == 0xde ? uint8_t(0xff & ~(1U << Top)) : 0xff);
      }
}

TEST(X87PortableContext, RefusalsLeaveAllStateOutputAndMemoryUntouched) {
  struct Refusal {
    uint32_t Selector;
    uint16_t CW;
    uint16_t SW;
    uint8_t Tags;
    uint8_t Known;
    Outcome Expected;
  };
  const Refusal Cases[] = {
      {0xdeadbeef, 0x37f, 0, 0xff, 0xff, Outcome::InvalidInstruction},
      {0xfad9, 0x37f, 0, 0xff, 0xff, Outcome::UnsupportedInstruction},
      {0xc1d8, 0x17f, 0, 0xff, 0xff, Outcome::InvalidControl},
      {0xc1d8, 0x37e, 0, 0xff, 0xff, Outcome::UnmaskedControl},
      {0xc1d8, 0x37f, 0, 0xff, 0xfd, Outcome::UnknownPayload},
      {0xc1d8, 0x37f, 0, 0xfe, 0xff, Outcome::StackFault},
      {0xc0d9, 0x37f, 0, 0xff, 0xff, Outcome::StackFault},
      {mem(0xdb, 5), 0x37f, 0x80, 0, 0, Outcome::PendingException},
      {mem(0xdb, 5), 0x37f, 0, 0, 0, Outcome::InvalidMemory},
      {0xe3db | X87ContextWaitPrefix, 0x37e, 1, 0, 0,
       Outcome::PendingException},
  };
  for (const auto &C : Cases) {
    auto S = initial(0, C.Tags);
    putWord(S, 0, C.CW);
    putWord(S, 2, C.SW);
    const auto Before = S;
    auto Known = C.Known;
    uint64_t Output = 0x12345678;
    // Empty memory is deliberate: pending #MF must precede even size checks.
    EXPECT_EQ(executePortableX87Context(C.Selector, S, Known, {}, GuestIP,
                                        GuestDP, Output),
              C.Expected);
    EXPECT_EQ(S, Before);
    EXPECT_EQ(Known, C.Known);
    EXPECT_EQ(Output, 0x12345678);
  }
  for (const auto &Unsupported :
       {bits(IntegerBit, 0x7fff), bits(UINT64_C(0xc000000000000321), 0xffff),
        bits(UINT64_C(0x8000000000003210), 0x7fff), bits(1, 0x4000)}) {
    auto S = initial();
    put(S, 0, Unsupported);
    const auto Before = S;
    uint8_t Known = 0xff;
    EXPECT_EQ(step(0xc1d8, S, Known), Outcome::UnsupportedValue);
    EXPECT_EQ(S, Before);
  }
  auto S =
      initial(0, 0); // FSTP stack underflow cannot modify the store buffer.
  const auto Before = S;
  uint8_t Known = 0xff;
  X87Binary80Bytes Memory{};
  Memory.fill(0x5a);
  const auto BeforeMemory = Memory;
  EXPECT_EQ(step(mem(0xdb, 7), S, Known, Memory), Outcome::StackFault);
  EXPECT_EQ(S, Before);
  EXPECT_EQ(Memory, BeforeMemory);
}

TEST(X87PortableContext, WaitingAndNonwaitingControlPreserveDefinedEffects) {
  auto S = initial(5);
  putWord(S, 0, 0x37e);
  putWord(S, 2, 0xedff);
  uint8_t Known = 0;
  uint64_t Output = 0;
  ASSERT_EQ(
      executePortableX87Context(0xe0df, S, Known, {}, GuestIP, GuestDP, Output),
      Outcome::Completed);
  EXPECT_EQ(Output, 0xedff);
  EXPECT_EQ(step(0x9b, S, Known), Outcome::PendingException);
  ASSERT_EQ(step(0xe2db, S, Known), Outcome::Completed);
  EXPECT_EQ(word(S, 2), 0x6d00);
  EXPECT_EQ(Known, 0);
  EXPECT_EQ(step(0x9b, S, Known), Outcome::Completed);
  std::array<uint8_t, 2> Memory{0x3f, 0x0a};
  ASSERT_EQ(step(mem(0xd9, 5), S, Known, Memory), Outcome::Completed);
  EXPECT_EQ(word(S), 0x0a7f);
  Memory.fill(0);
  ASSERT_EQ(step(mem(0xd9, 7), S, Known, Memory), Outcome::Completed);
  EXPECT_EQ(Memory, (std::array<uint8_t, 2>{0x7f, 0x0a}));
  for (unsigned PC : {0U, 2U, 3U})
    for (unsigned RC = 0; RC != 4; ++RC) {
      Memory = {0x7f, uint8_t(PC | (RC << 2))};
      EXPECT_EQ(step(mem(0xd9, 5), S, Known, Memory), Outcome::Completed);
      EXPECT_EQ(word(S), 0x7f | (PC << 8) | (RC << 10));
    }
}

#ifdef NEVERD_X87_UNICORN_REFERENCE
TEST(X87PortableContext, OriginalInstructionsMatchDefinedStateAcrossAllTOP) {
  uc_engine *UC = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &UC), UC_ERR_OK);
  struct Close {
    uc_engine *UC;
    ~Close() { uc_close(UC); }
  } Guard{UC};
  ASSERT_EQ(uc_ctl_set_cpu_model(UC, UC_CPU_X86_HASWELL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(UC, 0x1000, 4096, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(UC, 0x2000, 4096, UC_PROT_ALL), UC_ERR_OK);
  // MOV EAX,0x2000; MOV EDX,0x2200; FXRSTOR64 [RAX]; instruction;
  // FXSAVE64 [RAX]. Each observation starts with exactly the same raw image.
  std::array<uint8_t, 20> Code{0xb8, 0,    0x20, 0,    0,    0xba, 0,
                               0x22, 0,    0,    0x48, 0x0f, 0xae, 0x08,
                               0xd8, 0xc1, 0x48, 0x0f, 0xae, 0x00};
  std::mt19937_64 Random(0x871677);
  unsigned Observations = 0;
  for (const auto &Form : x87InstructionForms()) {
    if (Form.PortableOperation == X87PortableOperation::None ||
        Form.PortableOperation == X87PortableOperation::Wait ||
        Form.PortableOperation == X87PortableOperation::LoadControl ||
        Form.PortableOperation == X87PortableOperation::StoreStatus)
      continue;
    for (unsigned Top = 0; Top != 8; ++Top)
      for (unsigned PC : {0U, 2U, 3U})
        for (unsigned RC = 0; RC != 4; ++RC) {
          const unsigned ModRM =
              Form.FirstModRM + (Form.LastModRM != Form.FirstModRM ? 3 : 0);
          const uint32_t Selector = Form.Memory ? mem(Form.Escape, ModRM >> 3)
                                                : Form.Escape | (ModRM << 8);
          SCOPED_TRACE(testing::Message()
                       << Form.ID << ':' << Top << ':' << PC << ':' << RC);
          auto S = initial(Top);
          putWord(S, 0, 0x7f | (PC << 8) | (RC << 10));
          for (unsigned I = 0; I != 8; ++I)
            put(S, I,
                bits(Random() | IntegerBit,
                     uint16_t(0x3f00 + Random() % 512) | (Random() & 0x8000)));
          if (Form.PortableOperation == X87PortableOperation::LoadRaw ||
              Form.PortableOperation == X87PortableOperation::LoadInteger ||
              Form.PortableOperation == X87PortableOperation::LoadRegister)
            S[4] &= ~(1U << ((Top + 7) & 7));
          auto Expected = S;
          uint8_t Known = 0xff;
          auto Memory = bits(IntegerBit + Random() % 8192, 0x3f04);
          const auto Spec = *x87ContextInstruction(Selector);
          auto Operand = std::span(Memory).first(Spec.MemoryBytes);
          ASSERT_EQ(uc_mem_write(UC, 0x2000, Expected.data(), Expected.size()),
                    UC_ERR_OK);
          ASSERT_EQ(uc_mem_write(UC, 0x2200, Memory.data(), Memory.size()),
                    UC_ERR_OK);
          Code[14] = Form.Escape;
          Code[15] = ModRM | (Form.Memory ? 2 : 0);
          ASSERT_EQ(uc_mem_write(UC, 0x1000, Code.data(), Code.size()),
                    UC_ERR_OK);
          ASSERT_EQ(uc_ctl_remove_cache(UC, 0x1000, 0x1000 + Code.size()),
                    UC_ERR_OK);
          ASSERT_EQ(uc_emu_start(UC, 0x1000, 0x1000 + Code.size(), 0, 8),
                    UC_ERR_OK);
          ASSERT_EQ(uc_mem_read(UC, 0x2000, Expected.data(), Expected.size()),
                    UC_ERR_OK);
          auto ExpectedMemory = Memory;
          ASSERT_EQ(
              uc_mem_read(UC, 0x2200, ExpectedMemory.data(), Memory.size()),
              UC_ERR_OK);
          ASSERT_EQ(step(Selector, S, Known, Operand), Outcome::Completed);
          EXPECT_EQ(word(S), word(Expected));
          // C0/C2/C3 are undefined here. This pinned Unicorn/QEMU reference
          // does not propagate SoftFloat's flags to FSW (fpu_helper.c), and
          // does not implement C1 rounding. Do not call this exception
          // evidence.
          EXPECT_EQ(word(S, 2) & 0xb8c0, word(Expected, 2) & 0xb8c0);
          EXPECT_EQ(S[4], Expected[4]);
          for (unsigned I = 0; I != 8; ++I)
            EXPECT_EQ(get(S, I), get(Expected, I));
          EXPECT_EQ(Memory, ExpectedMemory);
          if (x87ContextUpdatesInstructionPointer(Selector)) {
            EXPECT_EQ(address(S, 8), GuestIP);
            EXPECT_EQ(address(Expected, 8), 0x100e);
            if (Form.Memory) {
              EXPECT_EQ(address(S, 16), GuestDP);
              // do_xsave_fpu in this reference writes FDP as zero. The guest
              // address fixture above is independent of that missing field.
              EXPECT_EQ(address(Expected, 16), 0);
            }
          }
          ++Observations;
        }
  }
  RecordProperty("observations", Observations);
  RecordProperty("reference", "Unicorn x86-64 Haswell original instructions");
  RecordProperty("excluded",
                 "undefined C0/C2/C3/FOP/register FDP; QEMU C1, arithmetic "
                 "exception flags and FDP serialization");
}
#endif
} // namespace
