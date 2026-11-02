//===- X86_64_X87ConvertStateTests.cpp - x87 conversion state -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe original x87 integer/IEEE stores, their rounding and status.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/ir/X86/X87BCD.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/high/X86/X87ContextShape.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include <bit>

namespace {
LowOp contextStep(uint32_t Selector, uint64_t Address = 0) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = Entry;
  Op.addInput(NdVar::cst(unsigned(Intrinsic::X87ContextStep), 2));
  Op.addInput(NdVar::reg(x86reg::X87ContextHandle, 8));
  Op.addInput(NdVar::cst(Selector, 4));
  Op.addInput(NdVar::cst(Address, 8));
  Op.addInput(NdVar::cst(0, 8));
  Op.addInput(NdVar::cst(Entry, 8));
  if (const auto Spec = x87ContextInstruction(Selector))
    if (Spec->WritesAX || Spec->WritesFlags)
      Op.Output = NdVar::tmp(90001, Spec->WritesAX ? 2 : 8);
  return Op;
}

TEST(X87ContextContract, LowMedHighRefuseForgedHandlesAndTruncatedSelectors) {
  for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Op = contextStep(0xe3db);
    switch (Mutation) {
    case 1:
      Op.Inputs[1] = NdVar::cst(0x2000, 8);
      break;
    case 2:
      Op.Inputs[2].Offset |= UINT64_C(1) << 32;
      break;
    case 3:
      Op.Inputs[1].Size = 4;
      break;
    case 4:
      Op.Inputs[4].Size = 4;
      break;
    case 5:
      Op.NumInputs = 5;
      break;
    case 6:
      Op.Output = NdVar::tmp(90001, 8);
      break;
    case 7:
      Op.MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 8:
      Op.MemoryAddressSpace = static_cast<NdMemoryAddressSpace>(255);
      break;
    case 9:
      Op.Inputs[2].Offset = 0xe5db;
      break;
    case 10:
      Op.Inputs[3].Offset = 1;
      break;
    case 11:
      Op.Inputs[5] = NdVar::reg(90002, 8);
      break;
    case 12:
      Op.Inputs[5].Size = 4;
      break;
    case 13:
      Op.Inputs[2] = NdVar::reg(90002, 4);
      break;
    case 14:
      Op.Inputs[0].Size = 4;
      break;
    }
    EXPECT_EQ(x87ContextLowShapeIsValid(Op, Arch::X64), Mutation == 0);
    MedOp Med;
    Med.Opcode = Op.Opcode;
    Med.Output.Size = Op.Output.Size;
    Med.MemoryOrdering = Op.MemoryOrdering;
    Med.MemoryAddressSpace = Op.MemoryAddressSpace;
    for (unsigned I = 0; I < Op.NumInputs; ++I) {
      MedVar Input;
      if (Op.Inputs[I].isConst())
        Input = MedVar::makeConst(Op.Inputs[I].Offset, Op.Inputs[I].Size);
      else {
        Input.Kind = MedVar::Temp;
        Input.Id = I;
        Input.Size = Op.Inputs[I].Size;
      }
      Med.addInput(Input);
    }
    EXPECT_EQ(x87ContextMedShapeIsValid(Med, Arch::X64), Mutation == 0);
    if (Mutation !=
        14) { // HighIR stores the ID in a typed enum, not an operand.
      HighExpr High;
      High.Kind = ExprKind::Call;
      High.IntrinsicId = Intrinsic::X87ContextStep;
      High.Type = Op.Output.Size ? NdType::makeInt(Op.Output.Size, false)
                                 : NdType::makeVoid();
      High.MemoryOrdering = Op.MemoryOrdering;
      High.MemoryAddressSpace = Op.MemoryAddressSpace;
      for (unsigned I = 1; I < Op.NumInputs; ++I) {
        if (Op.Inputs[I].isConst())
          High.Operands.push_back(
              HighExpr::makeConst(Op.Inputs[I].Offset, Op.Inputs[I].Size));
        else
          High.Operands.push_back(HighExpr::makeVar(
              Med.Inputs[I], NdType::makeInt(Op.Inputs[I].Size, false)));
      }
      EXPECT_EQ(x87ContextHighShapeIsValid(High, Arch::X64), Mutation == 0);
    }
    EXPECT_FALSE(x87ContextLowShapeIsValid(Op, Arch::X86));
  }
}

TEST(X87ContextConcrete, UnknownIncomingStateCannotInventInitializedTags) {
#if !(defined(__x86_64__) || defined(_M_X64)) ||                               \
    !(defined(__clang__) || defined(__GNUC__))
  GTEST_SKIP() << "native x64 GCC/Clang context executor required";
#endif
  BinaryImage Img;
  Img.Arch = Arch::X64;
  for (bool Strict : {false, true}) {
    NdOpEmulator E(Img);
    E.setStrictMode(Strict);
    EXPECT_FALSE(
        E.step(contextStep(0xe8d9))); // FLD1 needs incoming stack tags.
    EXPECT_FALSE(E.getX87Context());
    ASSERT_TRUE(E.step(contextStep(0xe3db))); // FNINIT is nonwaiting.
    ASSERT_TRUE(E.step(contextStep(0xe8d9)));
    EXPECT_EQ(E.getRegister(x86reg::FPU_SW), 0x3800);
    EXPECT_FALSE(E.getX87Context()); // Empty retained payloads remain unknown.
    const std::vector<uint8_t> One = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x3f};
    EXPECT_EQ(E.getRegisterBytes(x86reg::ST7), One);
    E.reset();
    EXPECT_FALSE(E.step(contextStep(0xe8d9)));
  }
}

TEST(X87ContextConcrete, SavesAndSkippedRestoresNeverAuthenticateUnknownBits) {
#if !(defined(__x86_64__) || defined(_M_X64)) ||                               \
    !(defined(__clang__) || defined(__GNUC__))
  GTEST_SKIP() << "native x64 GCC/Clang context executor required";
#endif
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Segment Data;
  Data.VA = 0x3000;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.assign(0x200, 0);
  Data.Size = Data.FileSz = Data.Data.size();
  Img.Segments.push_back(Data);
  // fnsave [rax]: DD /6 stores all eight registers, empty ones included.
  const uint32_t Fnsave = 0xdd | (0x30U << 8) | X87ContextMemory |
                          (0x30U << X87ContextOriginalModRMShift);
  std::array<uint8_t, 512> State{};
  State[0] = 0x7f;
  State[1] = 3;
  for (bool Strict : {false, true}) {
    NdOpEmulator E(Img);
    E.setStrictMode(Strict);
    ASSERT_TRUE(E.setX86LinearAddressBits(48));
    // Authenticated payloads may be saved.
    ASSERT_TRUE(E.setX87Context(State));
    EXPECT_TRUE(E.step(contextStep(Fnsave, Data.VA))) << Strict;
    // A direct register write forgets the authenticated payloads, and FNINIT
    // from that unknown context retains them unknown.
    ASSERT_TRUE(E.setX87Context(State));
    E.setRegister(x86reg::FPU_SW, 0);
    ASSERT_TRUE(E.step(contextStep(0xe3db)));
    EXPECT_FALSE(E.step(contextStep(Fnsave, Data.VA))) << Strict;
    EXPECT_FALSE(E.getX87Context());
  }
  // A full-state restore the lenient tracer skips leaves no context to trust.
  NdOpEmulator E(Img);
  E.setStrictMode(false);
  ASSERT_TRUE(E.setX87Context(State));
  LowOp Restore;
  Restore.Opcode = NdOp::INTRINSIC;
  Restore.Addr = Entry;
  Restore.addInput(NdVar::cst(unsigned(Intrinsic::Fxrstor64Mem), 2));
  Restore.addInput(NdVar::cst(Data.VA, 8));
  EXPECT_TRUE(E.step(Restore));
  EXPECT_FALSE(E.getX87Context());
  EXPECT_FALSE(E.step(contextStep(0xe8d9))); // FLD1 needs the restored tags.
}

TEST(X87ContextConcrete,
     LegacyScalarWritesCannotLeaveAnAuthenticatedStaleSnapshot) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::array<uint8_t, 512> State{};
  State[0] = 0x7f;
  State[1] = 3;
  for (bool Strict : {false, true}) {
    NdOpEmulator E(Img);
    E.setStrictMode(Strict);
    ASSERT_TRUE(E.setX87Context(State));
    for (Intrinsic Id : {Intrinsic::X87WriteControl, Intrinsic::X87Fninit,
                         Intrinsic::X87Fnclex}) {
      LowOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.addInput(NdVar::cst(unsigned(Id), 2));
      if (Id == Intrinsic::X87WriteControl)
        Op.addInput(NdVar::cst(0x77f, 2));
      EXPECT_FALSE(E.step(Op));
      EXPECT_EQ(E.getX87Context(), State);
    }
    for (const auto Dest :
         {NdVar::reg(x86reg::ST0, 10), NdVar::reg(x86reg::FPU_CW, 2),
          NdVar::reg(x86reg::FPU_SW, 2)}) {
      LowOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = Dest;
      Write.addInput(NdVar::cst(0, Dest.Size));
      EXPECT_FALSE(E.step(Write));
      EXPECT_EQ(E.getX87Context(), State);
    }
    E.setRegister(x86reg::FPU_CW, 0x77f);
    EXPECT_FALSE(E.getX87Context());
    EXPECT_FALSE(E.step(contextStep(0xe8d9)));
  }
}

TEST(X87ContextConcrete, PendingExceptionsAndMalformedShapesRefuseAtomically) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::array<uint8_t, 512> State{};
  State[0] = 0x7e;
  State[1] = 3;
  State[2] = 1;
  State[3] = 0x38;
  State[4] = 0x80;
  for (bool Strict : {false, true})
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      NdOpEmulator E(Img);
      E.setStrictMode(Strict);
      ASSERT_TRUE(E.setX87Context(State));
      auto Op = contextStep(0xe0d9);
      switch (Mutation) {
      case 1:
        Op.NumInputs = 5;
        break;
      case 2:
        Op.Inputs[2].Size = 8;
        break;
      case 3:
        Op.Inputs[2] = NdVar::reg(90002, 4);
        break;
      case 4:
        Op.Inputs[2].Offset |= 1u << 28;
        break;
      case 5:
        Op.Inputs[2].Offset |= 1u << X87ContextOriginalModRMShift;
        break;
      case 6:
        Op.Inputs[3].Offset = 1;
        break;
      case 7:
        Op.Output = NdVar::tmp(90001, 8);
        break;
      case 8:
        Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
        break;
      default:
        break;
      }
      EXPECT_FALSE(E.step(Op)) << Mutation << ':' << Strict;
      EXPECT_EQ(E.getX87Context(), State);
      EXPECT_FALSE(E.getRegister(90001));
      EXPECT_FALSE(E.skips().any());
    }
}

TEST(X87ContextConcrete, InvalidPackedBCDDigitsCannotBecomeExactNativeResults) {
  // FBLD does not check the digits in hardware; A..F produce an undefined
  // numerical result. No current execution profile qualifies those inputs.
  const uint32_t Fbld = 0xdf | (0x20U << 8) | X87ContextMemory |
                        (0x20U << X87ContextOriginalModRMShift);
  for (unsigned Digit = 0; Digit < 18; ++Digit)
    for (unsigned Value = 10; Value < 16; ++Value)
      for (bool Strict : {false, true})
        for (bool Reference : {false, true}) {
          SCOPED_TRACE(Digit);
          SCOPED_TRACE(Value);
          SCOPED_TRACE(Strict);
          SCOPED_TRACE(Reference);
          BinaryImage Img;
          Img.Arch = Arch::X64;
          Segment Data;
          Data.VA = 0x2000;
          Data.Size = Data.FileSz = 10;
          Data.Flags = SegmentFlags::Readable;
          Data.Data.assign(10, 0);
          Data.Data[Digit / 2] = Value << ((Digit & 1) * 4);
          Img.Segments.push_back(Data);
          NdOpEmulator E(Img);
          E.setStrictMode(Strict);
          E.setX87NativeReferenceMode(Reference);
          E.setLoadCollect(true);
          ASSERT_TRUE(E.setX86LinearAddressBits(48));
          std::array<uint8_t, 512> State{};
          State[0] = 0x7f;
          State[1] = 3;
          ASSERT_TRUE(E.setX87Context(State));
          EXPECT_FALSE(E.step(contextStep(Fbld, Data.VA)));
          EXPECT_EQ(E.getX87Context(), State);
          EXPECT_FALSE(E.skips().any());
          EXPECT_TRUE(E.getLoadRecords().empty());
        }
}

TEST(X87BCDContract, OnlyTheEighteenDecimalDigitsConstrainAdmission) {
  std::array<uint8_t, 11> Bytes{};
  for (unsigned Sign = 0; Sign != 256; ++Sign) {
    Bytes[9] = Sign;
    EXPECT_TRUE(x87PackedBCDIsValid({Bytes.data(), 10}));
  }
  EXPECT_FALSE(x87PackedBCDIsValid(Bytes));
  EXPECT_FALSE(x87PackedBCDIsValid({Bytes.data(), 9}));
  EXPECT_FALSE(x87PackedBCDIsValid({}));
}

TEST(X87ContextConcrete, PackedBCDLoadPreservesAllDigitsAndSignedZero) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  const uint32_t Fbld = 0xdf | (0x20U << 8) | X87ContextMemory |
                        (0x20U << X87ContextOriginalModRMShift);
  for (uint64_t Magnitude :
       {UINT64_C(0), UINT64_C(1), UINT64_C(9), UINT64_C(10),
        UINT64_C(123456789012345678), UINT64_C(999999999999999999)})
    for (unsigned Sign : {0U, 0x7fU, 0x80U, 0xffU})
      for (unsigned PC : {0U, 2U, 3U})
        for (unsigned RC = 0; RC != 4; ++RC)
          for (unsigned Top = 0; Top != 8; ++Top) {
            SCOPED_TRACE(Magnitude);
            SCOPED_TRACE(Sign);
            SCOPED_TRACE(Top);
            SCOPED_TRACE(PC * 4 + RC);
            BinaryImage Img;
            Img.Arch = Arch::X64;
            Segment Data;
            Data.VA = 0x2000;
            Data.Size = Data.FileSz = 10;
            Data.Flags = SegmentFlags::Readable;
            Data.Data.assign(10, 0);
            auto Digits = Magnitude;
            for (unsigned I = 0; I != 9; ++I) {
              Data.Data[I] = Digits % 10;
              Digits /= 10;
              Data.Data[I] |= (Digits % 10) << 4;
              Digits /= 10;
            }
            Data.Data[9] = Sign;
            Img.Segments.push_back(Data);
            NdOpEmulator E(Img);
            E.setStrictMode(true);
            ASSERT_TRUE(E.setX86LinearAddressBits(48));
            std::array<uint8_t, 512> State{};
            State[0] = 0x7f;
            State[1] = PC | (RC << 2);
            State[3] = Top << 3;
            ASSERT_TRUE(E.setX87Context(State));
            ASSERT_TRUE(E.step(contextStep(Fbld, Data.VA)));
            const unsigned Destination = (Top + 7) & 7;
            uint64_t Significand = 0;
            uint16_t Exponent = (Sign & 0x80) << 8;
            if (Magnitude) {
              const unsigned Shift = std::countl_zero(Magnitude);
              Significand = Magnitude << Shift;
              Exponent |= 0x3fff + 63 - Shift;
            }
            std::vector<uint8_t> Expected(10);
            std::memcpy(Expected.data(), &Significand, 8);
            std::memcpy(Expected.data() + 8, &Exponent, 2);
            EXPECT_EQ(E.getRegisterBytes(x86reg::stReg(Destination)), Expected);
            const auto Actual = E.getX87Context();
            ASSERT_TRUE(Actual);
            EXPECT_EQ((*Actual)[4], 1U << Destination);
            EXPECT_EQ(((*Actual)[3] >> 3) & 7, Destination);
            EXPECT_EQ((*Actual)[0], State[0]);
            EXPECT_EQ((*Actual)[1], State[1]);
            EXPECT_FALSE((*Actual)[2] & 0x3f);
            EXPECT_FALSE(E.skips().any());
          }
#else
  GTEST_SKIP() << "native x64 GCC/Clang context executor required";
#endif
}

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
std::array<uint8_t, 512> nativeFistp(std::array<uint8_t, 512> State,
                                     uint64_t &Memory) {
  alignas(16) uint8_t Host[512], Active[512];
  __asm__ volatile("fxsave64 (%0)" ::"r"(Host) : "memory");
  std::memcpy(Active, State.data(), 512);
  std::memcpy(Active + 24, Host + 24, 8);
  std::memcpy(Active + 160, Host + 160, 352);
  __asm__ volatile(
      "fxrstor64 (%0)\n\tfistpq (%1)\n\tfxsave64 (%0)\n\tfxrstor64 (%2)" ::"r"(
          Active),
      "r"(&Memory), "r"(Host)
      : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)",
        "st(7)", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
        "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
  std::memcpy(State.data(), Active, 24);
  std::memcpy(State.data() + 32, Active + 32, 128);
  return State;
}
#endif

TEST(X87ContextConcrete, FistpMatchesNativeFullStateForEveryMaskAndRounding) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 8;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.assign(8, 0xa5);
  Img.Segments.push_back(Data);
  for (unsigned Masks = 0; Masks < 64; ++Masks)
    for (unsigned RC = 0; RC < 4; ++RC)
      for (unsigned PC : {0U, 2U, 3U})
        for (unsigned Kind = 0; Kind < 6; ++Kind)
          for (bool Strict : {false, true}) {
            SCOPED_TRACE(std::to_string(Masks) + ':' + std::to_string(RC) +
                         ':' + std::to_string(PC) + ':' + std::to_string(Kind));
            std::array<uint8_t, 512> State{};
            const uint16_t CW = 0x40 | Masks | (PC << 8) | (RC << 10);
            std::memcpy(State.data(), &CW, 2);
            State[3] = 0x38;
            State[4] = 0x80;
            const uint64_t Significand[] = {UINT64_C(0xc000000000000000),
                                            UINT64_C(0x8000000000000000),
                                            UINT64_C(0x8000000000000000),
                                            UINT64_C(0x8000000000000001),
                                            1,
                                            0};
            const uint16_t Exponent[] = {0x3fff, 0x403e, 0x7fff,
                                         0x7fff, 0,      0x8000};
            std::memcpy(State.data() + 32, &Significand[Kind], 8);
            std::memcpy(State.data() + 40, &Exponent[Kind], 2);
            uint64_t Expected = UINT64_C(0xa5a5a5a5a5a5a5a5);
            const auto Native = nativeFistp(State, Expected);
            NdOpEmulator E(Img);
            E.setStrictMode(Strict);
            ASSERT_TRUE(E.setX86LinearAddressBits(48));
            ASSERT_TRUE(E.setX87Context(State));
            ASSERT_TRUE(
                E.step(contextStep(0xdf | (0x38 << 8) | X87ContextMemory |
                                       (0x38 << X87ContextOriginalModRMShift),
                                   0x2000)));
            const auto Actual = E.getX87Context();
            ASSERT_TRUE(Actual);
            EXPECT_TRUE(std::equal(Native.begin(), Native.begin() + 5,
                                   Actual->begin()));
            for (unsigned I = 0; I < 8; ++I)
              EXPECT_TRUE(std::equal(Native.begin() + 32 + I * 16,
                                     Native.begin() + 42 + I * 16,
                                     Actual->begin() + 32 + I * 16));
            LowOp Load;
            Load.Opcode = NdOp::LOAD;
            Load.Output = NdVar::tmp(90003, 8);
            Load.addInput(NdVar::cst(0x2000, 8));
            ASSERT_TRUE(E.step(Load));
            EXPECT_EQ(E.getRegister(90003), Expected);
          }
#else
  GTEST_SKIP() << "native x64 GCC/Clang oracle required";
#endif
}

class X87ConvertFixture : public X86FPStateFixture {
protected:
  // These oracles cover the owned context on every host, including System V
  // ones whose convention keeps typed long double values.
  X87ConvertFixture() { X87ContextOverride = true; }
  bool ObserveContext = false, ResumeAfterException = false;
  bool ObserveFlags = false;
  bool ObserveEnvironment = false;
  bool ObserveEntryLoop = false;
  bool ObserveEntryState = false;
  bool ObserveBCDSource = false;
  unsigned RestoredEnvironmentBytes = 0;
  std::string comparisonDriver(bool, bool DeclareProbe, bool,
                               bool = false) override {
    std::string Text = ObserveContext ? "#define FULL_CONTEXT 1\n"
                                      : "#define FULL_CONTEXT 0\n";
    Text += ObserveFlags ? "#define OBSERVE_FLAGS 1\n"
                         : "#define OBSERVE_FLAGS 0\n";
    Text += ObserveEnvironment ? "#define OBSERVE_ENVIRONMENT 1\n"
                               : "#define OBSERVE_ENVIRONMENT 0\n";
    Text += ObserveEntryLoop ? "#define OBSERVE_ENTRY_LOOP 1\n"
                             : "#define OBSERVE_ENTRY_LOOP 0\n";
    Text += ObserveEntryState ? "#define OBSERVE_ENTRY_STATE 1\n"
                              : "#define OBSERVE_ENTRY_STATE 0\n";
    Text += ObserveBCDSource ? "#define OBSERVE_BCD_SOURCE 1\n"
                             : "#define OBSERVE_BCD_SOURCE 0\n";
    Text += "#define RESTORED_ENVIRONMENT_BYTES " +
            std::to_string(RestoredEnvironmentBytes) + "\n";
    Text += R"(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern void native_probe(uintptr_t);
)";
    if (DeclareProbe)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += R"x87(
int main(void) {
  uint16_t saved;
  __asm__ volatile("fnstcw %0" : "=m"(saved));
  _Alignas(16) unsigned char baseline[512];
  __asm__ volatile("fninit\n\tfxsave64 (%0)" :: "r"(baseline) : "memory");
#define RESTORE_BASELINE()                                                     \
  __asm__ volatile("fxrstor64 (%0)" ::"r"(baseline)                            \
                   : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)",       \
                     "st(5)", "st(6)", "st(7)", "xmm0", "xmm1", "xmm2",        \
                     "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",   \
                     "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15")
    const struct {
      uint64_t sig;
      uint16_t exp;
    } values[] = {{0, 0},
                  {0, 0x8000},
                  {UINT64_C(0x8000000000000000), 0x3fff},
                  {UINT64_C(0x8000000000000000), 0xbfff},
                  {UINT64_C(0xc000000000000000), 0x3fff},
                  {UINT64_C(0xc000000000000000), 0xbfff},
                  {UINT64_C(0xa000000000000000), 0x4000},
                  {UINT64_C(0xa000000000000000), 0xc000},
                  {UINT64_C(0x8000000000000000), 0x3ffe},
                  {UINT64_C(0x8000000000000000), 0xbffe},
                  {UINT64_C(0x8000000000000001), 0x3fff},
                  {UINT64_C(0x8000000000000001), 0xbfff},
                  {UINT64_C(0xfffe000000000000), 0x400d},
                  {UINT64_C(0x8000000000000000), 0x400e},
                  {UINT64_C(0x8000000000000000), 0xc00e},
                  {UINT64_C(0xfffffffe00000000), 0x401d},
                  {UINT64_C(0x8000000000000000), 0x401e},
                  {UINT64_C(0x8000000000000000), 0xc01e},
                  {UINT64_C(0xfffffffffffffffe), 0x403d},
                  {UINT64_C(0x8000000000000000), 0x403e},
                  {UINT64_C(0x8000000000000000), 0xc03e},
                  {UINT64_C(0x8000000000000000), 0x7fff},
                  {UINT64_C(0x8000000000000000), 0xffff},
                  {UINT64_C(0xc123456789abcdef), 0x7fff},
                  {UINT64_C(0x8000000000000001), 0x7fff},
                  {UINT64_C(0x8000000000000000), 1},
                  {1, 0},
                  {UINT64_C(0x8000000000000801), 0x3fff},
                  {UINT64_C(0x8000000000000000), 0x43ff},
                  {UINT64_C(0x8000000000000000), 0x3bcd}};
    unsigned comparisons = 0;
    for (unsigned rc = 0; rc < 4; ++rc)
      for (unsigned pc = 0; pc < 4; ++pc) {
        if (pc == 1)
          continue;
        for (unsigned masks = (FULL_CONTEXT ? 0 : 63); masks < 64; ++masks) {
          const uint16_t control = 0x0040 | (pc << 8) | (rc << 10) | masks;
          for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); ++v) {
           for (unsigned flags = 0; flags < (OBSERVE_FLAGS ? 8 : 1); ++flags) {
            for (unsigned entry_full = 0; entry_full < (OBSERVE_ENTRY_STATE ? 2 : 1); ++entry_full) {
            if (OBSERVE_ENTRY_STATE) {
              // Set real caller state; the probe itself has no FNINIT, FLDCW
              // or FLD that could manufacture its incoming slot or tags.
              memcpy(baseline, &control, 2);
              baseline[2] = baseline[3] = 0;
              baseline[4] = (unsigned char)entry_full;
              memcpy(baseline + 32, &values[v].sig, 8);
              memcpy(baseline + 40, &values[v].exp, 2);
            }
            _Alignas(16) unsigned char expected[768], actual[768];
            memset(expected, 0xa5, sizeof(expected));
            memcpy(expected, &control, 2);
            memcpy(expected + 2, &saved, 2);
            memcpy(expected + 8, &values[v].sig, 8);
            memcpy(expected + 16, &values[v].exp, 2);
            if (OBSERVE_BCD_SOURCE) {
              const uint64_t magnitudes[] = {0, 1, 9, 10,
                UINT64_C(123456789012345678), UINT64_C(999999999999999999)};
              uint64_t digits = magnitudes[(v / 2) % 6];
              for (unsigned i = 0; i < 9; ++i) {
                expected[8+i] = digits % 10; digits /= 10;
                expected[8+i] |= (digits % 10) << 4; digits /= 10;
              }
              expected[17] = 0x7f | ((v & 1) << 7);
            }
            if (OBSERVE_FLAGS)
              expected[24] = 2 | (flags & 1) | ((flags & 2) << 1) |
                             ((flags & 4) << 4) | (flags & 1 ? 0x90 : 0);
            if (OBSERVE_ENTRY_LOOP)
              expected[24] = 3;
            memcpy(actual, expected, sizeof(actual));
            RESTORE_BASELINE();
            native_probe((uintptr_t)expected);
            RESTORE_BASELINE();
            isa_probe((uintptr_t)actual);
            __asm__ volatile("fninit\n\tfldcw %0" ::"m"(saved) : "memory");
            if (FULL_CONTEXT) {
              // Code/data addresses relocate between original and generated
              // programs. Compare CW, SW, FTW and all retained binary80 slots;
              // this does not claim equality of caller-saved compiler XMM
              // values.
              if (memcmp(expected + 128, actual + 128, 5)) {
                puts("FP control/status/tag mismatch");
                return 1;
              }
              if (memcmp(expected + 134, actual + 134, 2)) {
                puts("last x87 opcode mismatch");
                return 1;
              }
              for (unsigned slot = 0; slot < 8; ++slot)
                if (memcmp(expected + 160 + slot * 16, actual + 160 + slot * 16,
                           10)) {
                  printf("FP payload mismatch cw=%04x value=%u slot=%u\n",
                         control, v, slot);
                  return 1;
                }
              memset(expected + 128, 0, 512);
              memset(actual + 128, 0, 512);
            }
            if (OBSERVE_ENVIRONMENT) {
              // Logical instruction addresses relocate; the complete legacy
              // control/status/tag words remain directly comparable.
              memset(expected + 108, 0, 16);
              memset(actual + 108, 0, 16);
            }
            if (RESTORED_ENVIRONMENT_BYTES) {
              // Saved instruction/data pointers relocate, but control, status,
              // complete tags and every saved binary80 payload must agree.
              const unsigned header = RESTORED_ENVIRONMENT_BYTES == 14 ||
                                              RESTORED_ENVIRONMENT_BYTES == 94
                                          ? 14 : 28;
              const unsigned pointer_offset = header == 14 ? 6 : 12;
              memset(expected + 640 + pointer_offset, 0, header-pointer_offset);
              memset(actual + 640 + pointer_offset, 0, header-pointer_offset);
            }
            if (memcmp(expected, actual, sizeof(actual))) {
              printf("cw=%04x input=%04x:%llx expected=", control,
                     values[v].exp, (unsigned long long)values[v].sig);
              for (unsigned i = 32; i < 52; ++i)
                printf("%02x", expected[i]);
              printf(" actual=");
              for (unsigned i = 32; i < 52; ++i)
                printf("%02x", actual[i]);
              puts("");
              return 1;
            }
            ++comparisons;
            }
           }
          }
        }
      }
    printf("%u x87 conversion/status/control comparisons\n", comparisons);
    return 0;
  }
)x87";
    return Text;
  }
  void check(unsigned Kind, unsigned Width, bool Full = false,
             bool Resume = false) {
    ObserveContext = Full;
    ResumeAfterException = Resume;
    ObserveFlags = Kind >= 10 && Kind <= 15;
    ObserveEnvironment = Kind == 17;
    ObserveEntryLoop = Kind == 19;
    ObserveEntryState = Kind >= 33 && Kind <= 35;
    ObserveBCDSource = Kind == 37 || Kind == 39;
    RestoredEnvironmentBytes = Kind == 28   ? 28
                               : Kind == 29 ? 14
                               : Kind == 30 ? 108
                               : Kind == 31 ? 94
                                            : 0;
    const uint8_t Base = memoryModRM() & 7;
    std::vector<uint8_t> Bytes = {
        0xdb, 0xe3, 0xd9, uint8_t(0x28 | Base), 0xdb, uint8_t(0x68 | Base),
        8}; // reset; control; raw binary80 source
    uint8_t Opcode = 0xdf, ModRM = 0;
    if (Kind == 0) { // FIST
      Opcode = Width == 2 ? 0xdf : 0xdb;
      ModRM = 0x50;
    } else if (Kind == 1) { // FISTP
      Opcode = Width == 4 ? 0xdb : 0xdf;
      ModRM = Width == 8 ? 0x78 : 0x58;
    } else if (Kind == 2) { // FISTTP
      Opcode = Width == 2 ? 0xdf : Width == 4 ? 0xdb : 0xdd;
      ModRM = 0x48;
    } else { // FST / FSTP
      Opcode = Width == 4 ? 0xd9 : 0xdd;
      ModRM = Kind == 3 ? 0x50 : 0x58;
    }
    if (Kind == 36 || Kind == 38) {
      if (Kind == 38)
        Bytes.insert(Bytes.end(), {0xdd, 0xc0});                   // FFREE ST0
      Bytes.insert(Bytes.end(), {0xdf, uint8_t(0x70 | Base), 32}); // FBSTP
    } else if (ObserveBCDSource) {
      Bytes = {0xdb, 0xe3, 0xd9, uint8_t(0x28 | Base)}; // reset; control
      if (Kind == 39)
        for (unsigned I = 0; I != 8; ++I)
          Bytes.insert(Bytes.end(), {0xd9, 0xe8}); // fill all eight tags
      Bytes.insert(Bytes.end(), {0xdf, uint8_t(0x60 | Base), 8}); // FBLD
    } else if (Kind == 40) {
      Bytes.insert(Bytes.end(), {0xd9, 0xe8, 0xdf, 0xc1}); // FFREEP ST1
    } else if (Kind == 41) {
      Bytes.insert(Bytes.end(), {0xd9, 0xf6}); // FDECSTP
    } else if (Kind >= 42 && Kind <= 53) {
      // These compare the same physical CPU, including its selected
      // transcendental/reduction behavior. They do not establish portable
      // exactness or cross-CPU identity for implementation-dependent results.
      static constexpr uint8_t Operations[] = {0xfe, 0xff, 0xfb, 0xf2,
                                               0xf3, 0xf0, 0xf1, 0xf9,
                                               0xfd, 0xf4, 0xf8, 0xf5};
      if (Kind == 46 || (Kind >= 48 && Kind <= 50) || Kind >= 52)
        Bytes.insert(Bytes.end(), {0xd9, 0xe8, 0xd9, 0xc9}); // st1=1
      Bytes.insert(Bytes.end(), {0xd9, Operations[Kind - 42]});
    } else if (ObserveEntryState) {
      const uint8_t Operation = Kind == 33 ? 0xe0 : Kind == 34 ? 0xe1 : 0xe5;
      Bytes = {0xd9, Operation};
    } else if (Kind == 32) {
      Bytes = {0xdb, 0xe3, 0xd9, 0xe8, 0xeb, 0, 0xd9, 0xe5};
      AdditionalCodeRoots.insert(Entry + 6);
    } else if (Kind >= 28 && Kind <= 31) {
      // Store a pending precision/invalid exception, save its environment,
      // reset, then restore without executing another waiting instruction.
      Bytes.insert(Bytes.end(), {0xdf, uint8_t(0x78 | Base), 32}); // FISTP
      const bool Small = Kind == 29 || Kind == 31;
      const bool Registers = Kind >= 30;
      const auto environment = [&](bool Restore) {
        if (Small)
          Bytes.push_back(0x66);
        Bytes.insert(Bytes.end(), {uint8_t(Registers ? 0xdd : 0xd9),
                                   uint8_t((Restore ? 0xa0 : 0xb0) | Base),
                                   0x80, 0x02, 0, 0}); // [argument + 640]
      };
      environment(false); // FNSTENV/FNSAVE
      Bytes.insert(Bytes.end(), {0xdb, 0xe3});
      environment(true); // FLDENV/FRSTOR
    } else if (Kind >= 20 && Kind <= 27) {
      if (Kind <= 25) {
        static constexpr uint8_t ModRMs[] = {0xc1, 0xc9, 0xe1,
                                             0xe9, 0xf1, 0xf9};
        Bytes.insert(Bytes.end(), {0xd9, 0xe8, 0xd8, ModRMs[Kind - 20]});
      } else
        Bytes.insert(Bytes.end(), {0xd9, uint8_t(Kind == 26 ? 0xfa : 0xfc)});
    } else if (Kind == 19) {
      Bytes = {0xd9, 0xe8, 0xfe, uint8_t(0x48 | Base), 24, 0x75, 0xf9};
    } else if (Kind == 5 || Kind == 6 || Kind == 17) {
      Bytes.insert(Bytes.end(),
                   {0x0f, 0x6f, uint8_t(0x40 | Base), 8,    // movq mm0,[arg+8]
                    0x0f, 0xef, 0xc8,                       // pxor mm1,mm0
                    0x0f, 0x7f, uint8_t(0x48 | Base), 32}); // store mm1
      if (Kind == 6)
        Bytes.insert(Bytes.end(), {0x0f, 0x77, 0xd9, 0xe8}); // emms; fld1
      if (Kind == 17)
        Bytes.insert(Bytes.end(), {0xd9, uint8_t(0x70 | Base), 96}); // fnstenv
    } else if (Kind == 7) {
      for (unsigned I = 0; I < 7; ++I)
        Bytes.insert(Bytes.end(), {0xd9, 0xe8}); // Fill the remaining slots.
      Bytes.insert(Bytes.end(), {0xdf, uint8_t(0x68 | Base), 8});
    } else if (Kind == 8 || Kind == 9) {
      Bytes.insert(Bytes.end(),
                   {0xdd, 0xd8, 0xd9, uint8_t(Kind == 8 ? 0xe0 : 0xe1)});
    } else if (Kind == 16 || Kind == 18) {
      // Keep one x87 value live across a returning callee that pushes another.
      // The normal GPR call frame is independent of the private x87 handle.
      Bytes.insert(Bytes.end(),
                   {0x48, 0x83, 0xec,
                    uint8_t(hostFormat() == BinaryFormat::COFF ? 40 : 8)});
      const unsigned Call = Bytes.size();
      Bytes.insert(Bytes.end(), {0xe8, 0, 0, 0, 0});
      Bytes.insert(Bytes.end(),
                   {0x48, 0x83, 0xc4,
                    uint8_t(hostFormat() == BinaryFormat::COFF ? 40 : 8), 0xde,
                    0xc1});
      Bytes.insert(Bytes.end(), {0x31, 0xc0}); // Defined GPR return carrier.
      // Fill the displacement after the observation/cleanup epilogue below.
      AdditionalFunctionEntries = {Entry + Call};
    } else if (Kind >= 10) {
      Bytes.insert(Bytes.end(),
                   {0xd9, 0xe8, // second stack value
                    0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 1, // OF=1
                    0x8a, uint8_t(0x60 | Base), 24, 0x9e}); // flags from AH
      const uint8_t CompareEscape = Kind == 10   ? 0xda
                                    : Kind >= 14 ? 0xdf
                                                 : 0xdb;
      const uint8_t CompareModRM = Kind <= 11 ? 0xc1 : (Kind & 1) ? 0xe9 : 0xf1;
      Bytes.insert(Bytes.end(), {CompareEscape, CompareModRM, 0x9f, 0x0f, 0x90,
                                 0xc0, // LAHF; SETO AL
                                 0x66, 0x89, uint8_t(0x40 | Base), 64});
    } else
      Bytes.insert(Bytes.end(), {Opcode, uint8_t(ModRM | Base), 32});
    Bytes.insert(Bytes.end(),
                 {0xdf, 0xe0, 0x66, 0x25, uint8_t(Full ? 0xff : 0x3f),
                  uint8_t(Full ? 0xff : 0x02), 0x66, 0x89, uint8_t(0x40 | Base),
                  48});
    if (!Full && (Kind == 0 || Kind == 3))
      Bytes.insert(Bytes.end(), {0xdb, uint8_t(0x78 | Base), 64});
    if (Full) {
      if (Resume)
        Bytes.insert(Bytes.end(), {0xdb, 0xe2, 0xdb, uint8_t(0x78 | Base), 64});
      Bytes.insert(Bytes.end(),
                   {0x48, 0x0f, 0xae, uint8_t(0x80 | Base), 0x80, 0, 0, 0});
    }
    Bytes.insert(Bytes.end(), {0xd9, uint8_t(0x78 | Base), 50});
    if (Full)
      Bytes.insert(Bytes.end(),
                   {0xdb, 0xe3}); // nonwaiting clear before restoring caller CW
    Bytes.insert(Bytes.end(), {0xd9, uint8_t(0x68 | Base), 2, 0xc3});
    if (Kind == 16 || Kind == 18) {
      const unsigned Call = *AdditionalFunctionEntries.begin() - Entry;
      const int32_t Displacement = Bytes.size() - (Call + 5);
      std::memcpy(Bytes.data() + Call + 1, &Displacement, 4);
      AdditionalFunctionEntries = {Entry + Bytes.size()};
      if (Kind == 18) {
        Bytes.insert(Bytes.end(),
                     {0x48, 0x83, 0xec,
                      uint8_t(hostFormat() == BinaryFormat::COFF ? 40 : 8),
                      0xe8, 5, 0, 0, 0, 0x48, 0x83, 0xc4,
                      uint8_t(hostFormat() == BinaryFormat::COFF ? 40 : 8),
                      0xc3});
        AdditionalFunctionEntries.insert(Entry + Bytes.size());
      }
      Bytes.insert(Bytes.end(), {0xd9, 0xe8, 0xc3});
    }
    compareFloating(Bytes, false);
  }
};

TEST_F(X87ConvertFixture,
       NarrowReturnExcludesOnlyUndemandedUnknownRegisterSlice) {
  for (unsigned ReturnBytes : {2U, 8U}) {
    HighFunc F;
    F.Entry = Entry;
    F.Name = "known_low";
    F.ReturnType = NdType::makeInt(ReturnBytes, false);
    F.Params = {{"low", NdType::makeInt(2, false)}};
    MedVar Low;
    Low.Kind = MedVar::Param;
    Low.TheArch = Arch::X64;
    Low.Id = 0;
    Low.Size = 2;
    Low.RegOff = kNoParamReg;
    MedVar Unknown;
    Unknown.Kind = MedVar::Reg;
    Unknown.TheArch = Arch::X64;
    Unknown.Id = 1;
    Unknown.Size = 8;
    Unknown.RegOff = x86reg::RAX;
    auto High = HighExpr::makeBinop(
        NdOp::SUBBYTES, HighExpr::makeVar(Unknown, NdType::makeInt(8, false)),
        HighExpr::makeConst(2, 4));
    High->Type = NdType::makeInt(6, false);
    auto Value = HighExpr::makeBinop(NdOp::CONCAT, High,
                                     HighExpr::makeVar(Low, F.Params[0].Type));
    Value->Type = NdType::makeInt(8, false);
    HighStmt Ret;
    Ret.Kind = StmtKind::Return;
    Ret.RetVal = Value;
    F.Body = {Ret};
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = hostFormat();
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
    retain("narrow-return-" + std::to_string(ReturnBytes) + ".c", Source);
    if (ReturnBytes == 2) {
      EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos);
      executeSource(Source, R"(
int main(void) {
  for (unsigned i=0;i<65536;++i)
    if (known_low((uint16_t)i) != i) return 1;
  return 0;
}

)");
    } else {
      // Demanding the high bits still produces an explicit refusal; a low
      // return must not teach the emitter to replace every unknown with zero.
      EXPECT_NE(Source.find("__builtin_trap"), std::string::npos);
    }
  }
}

TEST_F(X87ConvertFixture, NarrowReturnRetainsEffectsInDiscardedHighSlice) {
  HighFunc F;
  F.Entry = Entry;
  F.Name = "known_low_with_effect";
  F.ReturnType = NdType::makeInt(2, false);
  F.Params = {{"low", NdType::makeInt(2, false)}};
  MedVar Low;
  Low.Kind = MedVar::Param;
  Low.TheArch = Arch::X64;
  Low.Id = 0;
  Low.Size = 2;
  Low.RegOff = kNoParamReg;
  auto Effect = HighExpr::makeCall("observe_high", 0x2000, {});
  Effect->Type = NdType::makeInt(8, false);
  auto High =
      HighExpr::makeBinop(NdOp::SUBBYTES, Effect, HighExpr::makeConst(2, 4));
  High->Type = NdType::makeInt(6, false);
  auto Value = HighExpr::makeBinop(NdOp::CONCAT, High,
                                   HighExpr::makeVar(Low, F.Params[0].Type));
  Value->Type = NdType::makeInt(8, false);
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = Value;
  F.Body = {Ret};
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = hostFormat();
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  retain("narrow-return-effect.c", Source);
  executeSource(Source, R"(
static unsigned observations;
int64_t observe_high(void) { ++observations; return -1; }
int main(void) {
  for (unsigned i=0;i<65536;++i) {
    if (known_low_with_effect((uint16_t)i) != i) return 1;
    if (observations != i+1) return 2;
  }
  return 0;
}
)");
}

TEST_F(X87ConvertFixture, IndependentRootHasCaptureAndIncomingEdgePublication) {
  const std::vector<uint8_t> Bytes = {0xdb, 0xe3, 0xd9, 0xe8, 0xeb,
                                      0,    0xd9, 0xe5, 0xc3};
  AdditionalCodeRoots.insert(Entry + 6);
  auto Image = image(Bytes);
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  CFGBuilder Builder;
  auto Low = Builder.build(Image, D, Entry, "root_context");
  ASSERT_TRUE(Low.hasCompleteInstructionLift());
  bool Captured = false, Published = false;
  for (const auto &B : Low.Blocks) {
    for (const auto &Op : B.Ops) {
      if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
          !Op.Inputs[0].isConst())
        continue;
      const auto Id = Intrinsic(Op.Inputs[0].Offset);
      Captured |= B.StartAddr == Entry + 6 && Id == Intrinsic::X87ContextBegin;
      Published |= B.StartAddr == Entry && Id == Intrinsic::X87ContextExport &&
                   Op.Addr == Entry + 4;
    }
  }
  EXPECT_TRUE(Captured);
  EXPECT_TRUE(Published);
}

TEST_F(X87ConvertFixture, VoidCalleeContractRetainsEffectsAndUnknownResults) {
  for (unsigned ReturnBytes : {2U, 8U}) {
    HighFunc Callee;
    Callee.Entry = 0x2000;
    Callee.Name = "no_value";
    Callee.ReturnType = NdType::makeInt(8, false);
    Callee.ReturnsNoValue = true;
    HighStmt Observe;
    Observe.Kind = StmtKind::Call;
    Observe.CallExpr = HighExpr::makeCall("observe_void", 0x3000, {});
    Observe.CallExpr->Type = NdType::makeInt(4, true);
    HighStmt Leave;
    Leave.Kind = StmtKind::Return;
    Callee.Body = {Observe, Leave};
    HighFunc Caller;
    Caller.Entry = Entry;
    Caller.Name = "low_after_void";
    Caller.ReturnType = NdType::makeInt(ReturnBytes, false);
    Caller.Params = {{"low", NdType::makeInt(2, false)}};
    MedVar Low;
    Low.Kind = MedVar::Param;
    Low.TheArch = Arch::X64;
    Low.Id = 0;
    Low.Size = 2;
    Low.RegOff = kNoParamReg;
    MedVar Undefined;
    Undefined.Kind = MedVar::Temp;
    Undefined.TheArch = Arch::X64;
    Undefined.Id = 1;
    Undefined.Size = 8;
    HighStmt Call;
    Call.Kind = StmtKind::Assign;
    Call.Dst = HighExpr::makeVar(Undefined, NdType::makeInt(8, false));
    Call.Val = HighExpr::makeCall("no_value", Callee.Entry, {});
    Call.Val->Type = NdType::makeInt(8, false);
    auto High = HighExpr::makeBinop(NdOp::SUBBYTES, Call.Dst,
                                    HighExpr::makeConst(2, 4));
    High->Type = NdType::makeInt(6, false);
    auto Joined = HighExpr::makeBinop(
        NdOp::CONCAT, High, HighExpr::makeVar(Low, Caller.Params[0].Type));
    Joined->Type = NdType::makeInt(8, false);
    HighStmt Ret;
    Ret.Kind = StmtKind::Return;
    Ret.RetVal = Joined;
    Caller.Body = {Call, Ret};
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = hostFormat();
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(HighCEmitter().emit({Caller, Callee}, OS, Options));
    retain("void-contract-" + std::to_string(ReturnBytes) + ".c", Source);
    EXPECT_EQ(Source.find("= no_value("), std::string::npos);
    if (ReturnBytes == 2) {
      EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos);
      executeSource(Source, R"(
static unsigned observations;
int observe_void(void) { ++observations; return 0; }
int main(void) {
  for(unsigned i=0;i<65536;++i) {
    if(low_after_void((uint16_t)i)!=i) return 1;
    if(observations!=i+1) return 2;
  }
  return 0;
}
)");
    } else
      EXPECT_NE(Source.find("__builtin_trap"), std::string::npos);
  }
}

TEST_F(X87ConvertFixture, FnstswReturnIsAValueEvenThoughContextIsMutable) {
  if (!nativeX64())
    GTEST_SKIP() << "native x64 status observer required";
  const std::vector<uint8_t> Bytes = {0xdf, 0xe0, 0xc3};
  auto Image = image(Bytes);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {Entry};
  const auto Result = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 1u);
  EXPECT_FALSE(Result.HighFuncs[0].ReturnsNoValue);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Emit;
  Emit.TheArch = Arch::X64;
  Emit.Format = hostFormat();
  Emit.Image = &Image;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Emit));
  executeSource(Source, R"(
int main(void) {
  uint16_t expected;
  __asm__ volatile("fninit\n\tfld1\n\tfnstsw %0" : "=m"(expected) : : "memory","st");
  const uint16_t actual=isa_probe();
  __asm__ volatile("fninit" : : : "memory","st");
  return actual!=expected;
}
)");
}

TEST_F(X87ConvertFixture, FistWord) { check(0, 2); }
TEST_F(X87ConvertFixture, FistDword) { check(0, 4); }
TEST_F(X87ConvertFixture, FistpWord) { check(1, 2); }
TEST_F(X87ConvertFixture, FistpDword) { check(1, 4); }
TEST_F(X87ConvertFixture, FistpQword) { check(1, 8); }
TEST_F(X87ConvertFixture, FisttpWord) { check(2, 2); }
TEST_F(X87ConvertFixture, FisttpDword) { check(2, 4); }
TEST_F(X87ConvertFixture, FisttpQword) { check(2, 8); }
TEST_F(X87ConvertFixture, FstSingle) { check(3, 4); }
TEST_F(X87ConvertFixture, FstDouble) { check(3, 8); }
TEST_F(X87ConvertFixture, FstpSingle) { check(4, 4); }
TEST_F(X87ConvertFixture, FstpDouble) { check(4, 8); }
TEST_F(X87ConvertFixture, FistpQwordEveryExceptionMaskRetainsFullState) {
  check(1, 8, true);
}
TEST_F(X87ConvertFixture, FisttpWordEveryExceptionMaskRetainsFullState) {
  check(2, 2, true);
}
TEST_F(X87ConvertFixture, FstpSingleEveryExceptionMaskRetainsFullState) {
  check(4, 4, true);
}
TEST_F(X87ConvertFixture, FstDoubleEveryExceptionMaskRetainsFullState) {
  check(3, 8, true);
}
TEST_F(X87ConvertFixture, FnclexResumesAfterFistpWithoutInventingAPop) {
  check(1, 8, true, true);
}
TEST_F(X87ConvertFixture, MMXWritesPreservePhysicalAliasesAndPublishTags) {
  check(5, 8, true);
}
TEST_F(X87ConvertFixture, EMMSAllowsX87PushAfterMMXWithoutStackOverflow) {
  check(6, 8, true);
}
TEST_F(X87ConvertFixture, FildFullStackKeepsSuppressedPushAndIndefiniteValue) {
  check(7, 8, true);
}
TEST_F(X87ConvertFixture, FchsEmptyStackKeepsMaskedAndUnmaskedState) {
  check(8, 8, true);
}
TEST_F(X87ConvertFixture, FabsEmptyStackKeepsMaskedAndUnmaskedState) {
  check(9, 8, true);
}
TEST_F(X87ConvertFixture, FcmovbKeepsIncomingArithmeticFlags) {
  check(10, 8, true);
}
TEST_F(X87ConvertFixture, FcmovnbKeepsIncomingArithmeticFlags) {
  check(11, 8, true);
}
TEST_F(X87ConvertFixture, FcomiCommitsFlagsOnlyAfterSuccessfulCompare) {
  check(12, 8, true);
}
TEST_F(X87ConvertFixture, FucomiCommitsFlagsOnlyAfterSuccessfulCompare) {
  check(13, 8, true);
}
TEST_F(X87ConvertFixture, FcomipDoesNotInventAPopOnUnmaskedInvalid) {
  check(14, 8, true);
}
TEST_F(X87ConvertFixture, FucomipDoesNotInventAPopOnUnmaskedInvalid) {
  check(15, 8, true);
}
TEST_F(X87ConvertFixture, ReturningCallRetainsCallerStateAndImportsCalleePush) {
  check(16, 8, true);
}
TEST_F(X87ConvertFixture, FNSTENVAfterMMXPreservesCompleteLegacyTagWord) {
  check(17, 8, true);
}
TEST_F(X87ConvertFixture, WrapperWithoutLocalX87TransportsItsCalleeContext) {
  check(18, 8, true);
}
TEST_F(X87ConvertFixture, EntryBackedgeRetainsItsLiveContextAcrossIterations) {
  check(19, 8, true);
}
TEST_F(X87ConvertFixture, FaddRetainsAllPrecisionAndExceptionState) {
  check(20, 8, true);
}
TEST_F(X87ConvertFixture, FmulRetainsAllPrecisionAndExceptionState) {
  check(21, 8, true);
}
TEST_F(X87ConvertFixture, FsubRetainsAllPrecisionAndExceptionState) {
  check(22, 8, true);
}
TEST_F(X87ConvertFixture, FsubrRetainsAllPrecisionAndExceptionState) {
  check(23, 8, true);
}
TEST_F(X87ConvertFixture, FdivRetainsAllPrecisionAndExceptionState) {
  check(24, 8, true);
}
TEST_F(X87ConvertFixture, FdivrRetainsAllPrecisionAndExceptionState) {
  check(25, 8, true);
}
TEST_F(X87ConvertFixture, FsqrtRetainsAllPrecisionAndExceptionState) {
  check(26, 8, true);
}
TEST_F(X87ConvertFixture, FrndintRetainsAllPrecisionAndExceptionState) {
  check(27, 8, true);
}
TEST_F(X87ConvertFixture, FldenvRestoresPendingExceptionsAndCompleteTags) {
  check(28, 8, true);
}
TEST_F(X87ConvertFixture, Fldenv16RestoresPendingExceptionsAndCompleteTags) {
  check(29, 8, true);
}
TEST_F(X87ConvertFixture, FrstorRestoresPendingExceptionsTagsAndRawPayloads) {
  check(30, 8, true);
}
TEST_F(X87ConvertFixture, Frstor16RestoresPendingExceptionsTagsAndRawPayloads) {
  check(31, 8, true);
}
TEST_F(X87ConvertFixture, OrdinaryEdgePublishesStateBeforeIndependentRoot) {
  check(32, 8, true);
}
TEST_F(X87ConvertFixture, EntryFchsCapturesEmptyAndFullIncomingSlots) {
  check(33, 8, true);
}
TEST_F(X87ConvertFixture, EntryFabsCapturesEmptyAndFullIncomingSlots) {
  check(34, 8, true);
}
TEST_F(X87ConvertFixture, EntryFxamCapturesEmptyAndFullIncomingSlots) {
  check(35, 8, true);
}
TEST_F(X87ConvertFixture, FbstpPreservesRoundingMemoryAndUnmaskedNoPop) {
  check(36, 10, true);
}
TEST_F(X87ConvertFixture, FbldPreservesAllDigitsSignAndControl) {
  check(37, 10, true);
}
TEST_F(X87ConvertFixture, FbstpEmptyStackPreservesMaskedAndUnmaskedFaults) {
  check(38, 10, true);
}
TEST_F(X87ConvertFixture, FbldFullStackPreservesMaskedAndUnmaskedFaults) {
  check(39, 10, true);
}
TEST_F(X87ConvertFixture,
       FfreepEmptiesBothTagsWithoutChangingRetainedPayloads) {
  check(40, 10, true);
}
TEST_F(X87ConvertFixture, FdecstpRotatesWithoutChangingTagsOrPayloads) {
  check(41, 10, true);
}
TEST_F(X87ConvertFixture, FsinRetainsSelectedHostResultAndState) {
  check(42, 10, true);
}
TEST_F(X87ConvertFixture, FcosRetainsSelectedHostResultAndState) {
  check(43, 10, true);
}
TEST_F(X87ConvertFixture, FsincosRetainsSelectedHostResultsAndConditionalPush) {
  check(44, 10, true);
}
TEST_F(X87ConvertFixture, FptanRetainsSelectedHostResultAndConditionalPush) {
  check(45, 10, true);
}
TEST_F(X87ConvertFixture, FpatanRetainsSelectedHostResultAndPop) {
  check(46, 10, true);
}
TEST_F(X87ConvertFixture, F2xm1RetainsSelectedHostResultAndState) {
  check(47, 10, true);
}
TEST_F(X87ConvertFixture, Fyl2xRetainsSelectedHostResultAndPop) {
  check(48, 10, true);
}
TEST_F(X87ConvertFixture, Fyl2xp1RetainsSelectedHostResultAndPop) {
  check(49, 10, true);
}
TEST_F(X87ConvertFixture, FscaleRetainsSelectedHostResultAndState) {
  check(50, 10, true);
}
TEST_F(X87ConvertFixture, FxtractRetainsSelectedHostResultsAndPush) {
  check(51, 10, true);
}
TEST_F(X87ConvertFixture, FpremRetainsSelectedHostReductionAndConditionBits) {
  check(52, 10, true);
}
TEST_F(X87ConvertFixture, Fprem1RetainsSelectedHostReductionAndConditionBits) {
  check(53, 10, true);
}
} // namespace
