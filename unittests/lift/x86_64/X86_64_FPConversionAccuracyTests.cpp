//===- X86_64_FPConversionAccuracyTests.cpp - Conversion regressions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>

namespace {
using namespace neverd;

template <typename Scalar, typename Integer>
std::pair<uint64_t, uint32_t>
nativeIntegerConversion(uint64_t Bits, uint32_t State, bool Truncate) {
  Scalar Value;
  std::memcpy(&Value, &Bits, sizeof(Value));
  Integer Result;
  const uint32_t Saved = _mm_getcsr();
  if constexpr (sizeof(Scalar) == 4) {
    if (Truncate)
      __asm__ volatile("ldmxcsr %1\n\tcvttss2si %2,%0\n\tstmxcsr %1"
                       : "=&r"(Result), "+m"(State)
                       : "x"(Value)
                       : "memory");
    else
      __asm__ volatile("ldmxcsr %1\n\tcvtss2si %2,%0\n\tstmxcsr %1"
                       : "=&r"(Result), "+m"(State)
                       : "x"(Value)
                       : "memory");
  } else {
    if (Truncate)
      __asm__ volatile("ldmxcsr %1\n\tcvttsd2si %2,%0\n\tstmxcsr %1"
                       : "=&r"(Result), "+m"(State)
                       : "x"(Value)
                       : "memory");
    else
      __asm__ volatile("ldmxcsr %1\n\tcvtsd2si %2,%0\n\tstmxcsr %1"
                       : "=&r"(Result), "+m"(State)
                       : "x"(Value)
                       : "memory");
  }
  _mm_setcsr(Saved);
  return {Result, State};
}
} // namespace
#endif

TEST(X86FPConversionAccuracy, DenormalIntegersRaisePrecisionWithoutDenormal) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (unsigned SourceBytes : {4U, 8U})
    for (unsigned DestinationBytes : {4U, 8U})
      for (bool Truncate : {false, true})
        for (unsigned Rounding : {0U, 1U, 2U, 3U})
          for (bool DAZ : {false, true})
            for (bool DenormalMasked : {false, true})
              for (bool Negative : {false, true}) {
                const uint64_t Bits =
                    1 | (Negative
                             ? (SourceBytes == 4 ? UINT64_C(0x80000000)
                                                 : UINT64_C(0x8000000000000000))
                             : 0);
                // Precision stays masked. An unmasked DM must not fault:
                // FP-to-integer conversions do not raise denormal operand.
                const uint32_t State = 0x1e80 | (Rounding << 13) |
                                       (DAZ ? 0x40 : 0) |
                                       (DenormalMasked ? 0x100 : 0);
                const auto Expected =
                    SourceBytes == 4
                        ? (DestinationBytes == 4
                               ? nativeIntegerConversion<float, uint32_t>(
                                     Bits, State, Truncate)
                               : nativeIntegerConversion<float, uint64_t>(
                                     Bits, State, Truncate))
                        : (DestinationBytes == 4
                               ? nativeIntegerConversion<double, uint32_t>(
                                     Bits, State, Truncate)
                               : nativeIntegerConversion<double, uint64_t>(
                                     Bits, State, Truncate));
                SCOPED_TRACE(testing::Message()
                             << SourceBytes << "->" << DestinationBytes
                             << " state=" << State << " bits=" << Bits
                             << " truncate=" << Truncate);
                ASSERT_EQ(Expected.second & 2, 0U);
                NdOpEmulator Emulator(Image);
                Emulator.setStrictMode(true);
                Emulator.setMXCSR(State);
                std::vector<uint8_t> Source(16, 0);
                std::memcpy(Source.data(), &Bits, SourceBytes);
                Emulator.setRegisterBytes(x86reg::XMM0, Source);
                LowOp Op;
                Op.Opcode = NdOp::INTRINSIC;
                Op.Output = NdVar::tmp(0, 16);
                Op.addInput(NdVar::cst(
                    static_cast<unsigned>(Intrinsic::X86FPConvert), 2));
                Op.addInput(
                    NdVar::cst(makeX86FPConvertControl(
                                   X86FPConvertKind::FloatToSignedInteger,
                                   SourceBytes == 8, DestinationBytes == 8,
                                   Truncate, false,
                                   Truncate ? X86FPRounding::TowardZero
                                            : X86FPRounding::MXCSR,
                                   1),
                               2));
                Op.addInput(NdVar::reg(x86reg::XMM0, 16));
                Op.addInput(NdVar::cst(1, 1));
                ASSERT_TRUE(Emulator.step(Op));
                const auto Result = Emulator.getRegisterBytes(0);
                ASSERT_TRUE(Result);
                uint64_t Actual = 0;
                std::memcpy(&Actual, Result->data(), DestinationBytes);
                EXPECT_EQ(Actual, Expected.first);
                EXPECT_EQ(Emulator.getMXCSR(), Expected.second);
              }
#else
  GTEST_SKIP() << "native scalar conversion oracle requires x64 GCC/Clang";
#endif
}

TEST(X86FPConversionAccuracy, FloatDenormalsRetainOperandExceptionAndDaz) {
  using namespace neverd;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (bool DAZ : {false, true})
    for (bool DenormalMasked : {false, true}) {
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      const uint32_t State =
          0x1e80 | (DAZ ? 0x40 : 0) | (DenormalMasked ? 0x100 : 0);
      Emulator.setMXCSR(State);
      std::vector<uint8_t> Source(16, 0);
      Source[0] = 1;
      Emulator.setRegisterBytes(x86reg::XMM0, Source);
      LowOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.Output = NdVar::tmp(0, 16);
      Op.addInput(
          NdVar::cst(static_cast<unsigned>(Intrinsic::X86FPConvert), 2));
      Op.addInput(NdVar::cst(
          makeX86FPConvertControl(X86FPConvertKind::FloatToFloat, false, true,
                                  false, false, X86FPRounding::MXCSR, 1),
          2));
      Op.addInput(NdVar::reg(x86reg::XMM0, 16));
      Op.addInput(NdVar::cst(1, 1));
      EXPECT_EQ(Emulator.step(Op), DAZ || DenormalMasked);
      EXPECT_EQ(Emulator.getMXCSR(), State | (DAZ ? 0 : 2));
      if (!DAZ && !DenormalMasked)
        EXPECT_FALSE(Emulator.getRegisterBytes(0));
      else {
        const auto Result = Emulator.getRegisterBytes(0);
        ASSERT_TRUE(Result);
        uint64_t Bits = 0;
        std::memcpy(&Bits, Result->data(), 8);
        EXPECT_EQ(Bits, DAZ ? 0 : UINT64_C(0x36a0000000000000)); // exact 2^-149
      }
    }
}

TEST(X86FPConversionContract, SourceAndDestinationWidthsRemainIndependent) {
  using namespace neverd;
  for (auto Id :
       {Intrinsic::X86FPCvtToIntState, Intrinsic::X86FPTruncToIntState})
    for (unsigned Source : {4U, 8U})
      for (unsigned Destination : {4U, 8U}) {
        LowOp Op;
        Op.Opcode = NdOp::INTRINSIC;
        Op.Output = NdVar::tmp(0, Destination + 4);
        Op.addInput(NdVar::cst(static_cast<unsigned>(Id), 2));
        Op.addInput(NdVar::cst(0, Source));
        Op.addInput(NdVar::cst(0x1f80, 4));
        Op.addInput(NdVar::cst(Destination, 4));
        ASSERT_TRUE(
            x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X64)));
        EXPECT_EQ(x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X86)),
                  Destination == 4);
        auto Shape = x86FPStateLowShape(Op, Arch::X64);
        EXPECT_EQ(x86FPStateNumericalSliceSize(Id, Shape, 0, Destination), 0U);
        EXPECT_EQ(x86FPStateHelperLayout(Id, Shape),
                  x86FPConversionLayout(Source, Destination));
        Op.Inputs[3] = NdVar::tmp(1, 4);
        EXPECT_FALSE(
            x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X64)));
        Op.Inputs[3] = NdVar::cst(Destination, 2);
        EXPECT_FALSE(
            x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X64)));
        Op.Inputs[3] = NdVar::cst(16, 4);
        EXPECT_FALSE(
            x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X64)));
        Op.Inputs[3] = NdVar::cst(Destination, 4);
        Op.Inputs[2] = NdVar::cst(0x1f80, 8);
        EXPECT_FALSE(
            x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X64)));
        MedOp Med;
        Med.Opcode = NdOp::INTRINSIC;
        Med.Output = MedVar{.Kind = MedVar::Temp,
                            .TheArch = Arch::X86,
                            .Id = 0,
                            .Size = static_cast<uint16_t>(Destination + 4)};
        Med.addInput(MedVar::makeConst(static_cast<unsigned>(Id), 2));
        Med.addInput(MedVar::makeConst(0, Source));
        Med.addInput(MedVar::makeConst(0x1f80, 4));
        Med.addInput(MedVar::makeConst(Destination, 4));
        EXPECT_EQ(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Med)),
                  Destination == 4);
      }
}

TEST(X86FPConversionContract, AddressAndRegisterTailsMatchTheirRawEncoding) {
  using namespace neverd;
  for (bool Strict : {false, true})
    for (bool Vex : {false, true})
      for (bool Memory : {false, true})
        for (unsigned Mutation : {0U, 1U, 2U}) {
          Decoder Decode;
          ASSERT_TRUE(Decode.init(Arch::X64));
          Decode.setStrict(Strict);
          const std::vector<uint8_t> Bytes = {
              static_cast<uint8_t>(Vex ? 0xc5 : 0xf3),
              static_cast<uint8_t>(Vex ? 0xfa : 0x0f), 0x2d,
              static_cast<uint8_t>(Memory ? 0x00 : 0xc0)};
          DecodedInsn Insn;
          ASSERT_EQ(Decode.decodeOne(Bytes.data(), Bytes.size(), Entry, Insn),
                    Bytes.size());
          auto &Detail = Insn.Raw->detail->x86;
          SCOPED_TRACE(testing::Message()
                       << "strict=" << Strict << " vex=" << Vex
                       << " memory=" << Memory << " mutation=" << Mutation);
          if (Mutation == 0) {
            if (Memory)
              Detail.operands[1].mem.base = X86_REG_RBX;
            else {
              Insn.Raw->bytes[Insn.Raw->size++] = 0x90;
              Insn.Size = Insn.Raw->size;
            }
          } else if (Mutation == 1)
            Detail.addr_size = 4;
          else
            Detail.encoding.disp_size = 1;
          std::vector<LowOp> Ops;
          EXPECT_THROW(Decode.liftToLow(Insn, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
}

TEST(X86FPConversionContract, ReservedVexFieldsRefuseAndI386IgnoresW) {
  using namespace neverd;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true}) {
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Target));
      Decode.setStrict(Strict);
      for (uint8_t P1 : {uint8_t(0xfe), uint8_t(0xf2)}) {
        const uint8_t Bytes[] = {0xc5, P1, 0x2d, 0xc0};
        DecodedInsn Insn;
        if (!Decode.decodeOne(Bytes, sizeof(Bytes), Entry, Insn))
          continue; // The decoder itself may reject reserved fields.
        std::vector<LowOp> Ops;
        EXPECT_THROW(Decode.liftToLow(Insn, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
      const uint8_t Bytes[] = {0xc4, 0xe1, 0xfa, 0x2d, 0xc0};
      DecodedInsn Insn;
      ASSERT_EQ(Decode.decodeOne(Bytes, sizeof(Bytes), Entry, Insn),
                sizeof(Bytes));
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops));
      unsigned Found = 0;
      for (const auto &Op : Ops)
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].isConst() &&
            Op.Inputs[0].Offset ==
                static_cast<unsigned>(Intrinsic::X86FPCvtToIntState)) {
          ++Found;
          EXPECT_EQ(Op.Inputs[1].Size, 4U);
          EXPECT_EQ(Op.Inputs[2].Size, 4U);
          EXPECT_EQ(Op.Inputs[3].Offset, Target == Arch::X86 ? 4U : 8U);
        }
      EXPECT_EQ(Found, 1U);
    }
}

TEST(X86FPConversionContract,
     SupportedAddressAndExtendedRegisterFormsKeepWidths) {
  using namespace neverd;
  struct Fixture {
    Arch Target;
    unsigned Source;
    unsigned Destination;
    std::vector<uint8_t> Bytes;
  };
  const Fixture Cases[] = {
      {Arch::X64, 4, 4, {0xf3, 0x0f, 0x2d, 0xc1}},
      {Arch::X64, 8, 8, {0xf2, 0x4d, 0x0f, 0x2d, 0xc1}},
      {Arch::X64, 4, 8, {0xc4, 0x41, 0xfa, 0x2d, 0xc1}},
      {Arch::X64, 8, 4, {0xc5, 0xfb, 0x2d, 0xc1}},
      {Arch::X64, 4, 4, {0xf3, 0x0f, 0x2d, 0x44, 0x88, 0x10}},
      {Arch::X64, 8, 8, {0xf2, 0x4b, 0x0f, 0x2d, 0x44, 0x88, 0x10}},
      {Arch::X64, 8, 4, {0xf2, 0x0f, 0x2d, 0x05, 0x10, 0, 0, 0}},
      {Arch::X64, 4, 4, {0x67, 0xf3, 0x0f, 0x2d, 0x00}},
      {Arch::X64, 4, 4, {0x64, 0xf3, 0x0f, 0x2d, 0x00}},
      {Arch::X64, 4, 4, {0x65, 0xc5, 0xfa, 0x2d, 0x00}},
      {Arch::X64, 8, 8, {0xc4, 0xa1, 0xfb, 0x2d, 0x44, 0x88, 0x10}},
      {Arch::X86, 4, 4, {0xf3, 0x0f, 0x2d, 0x44, 0x88, 0x10}},
      {Arch::X86, 4, 4, {0x67, 0xc4, 0xe1, 0xfa, 0x2d, 0x00}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(testing::Message()
                 << "arch=" << static_cast<unsigned>(Case.Target)
                 << " bytes=" << testing::PrintToString(Case.Bytes));
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Case.Target));
    Decode.setStrict(true);
    DecodedInsn Insn;
    ASSERT_EQ(
        Decode.decodeOne(Case.Bytes.data(), Case.Bytes.size(), Entry, Insn),
        Case.Bytes.size());
    SCOPED_TRACE(Insn.Raw->op_str);
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops));
    unsigned Found = 0;
    for (const auto &Op : Ops)
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].isConst() &&
          Op.Inputs[0].Offset ==
              static_cast<unsigned>(Intrinsic::X86FPCvtToIntState)) {
        ++Found;
        EXPECT_EQ(Op.Inputs[1].Size, Case.Source);
        EXPECT_EQ(Op.Inputs[2].Size, 4U);
        EXPECT_EQ(Op.Inputs[3].Offset, Case.Destination);
        EXPECT_TRUE(
            x86FPStateShapeIsValid(Intrinsic::X86FPCvtToIntState,
                                   x86FPStateLowShape(Op, Case.Target)));
      }
    EXPECT_EQ(Found, 1U);
  }
}

TEST(X86FPConversionContract, LLVMCRejectsI386WithA64BitIntegerDestination) {
  using namespace neverd;
  llvm::LLVMContext Context;
  llvm::Module Module("conversion", Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
      llvm::Function::ExternalLinkage, "probe", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *State = Builder.CreateAlloca(Builder.getInt32Ty());
  Builder.CreateStore(Builder.getInt32(0x1f80), State);
  auto *AsmType = llvm::FunctionType::get(
      Builder.getInt64Ty(), {Builder.getFloatTy(), State->getType()}, false);
  auto *Asm = llvm::InlineAsm::get(
      AsmType, x86FPStateConversionAsm(Intrinsic::X86FPCvtToIntState, 4),
      X86FPStateConversionConstraints, true);
  auto *Call = Builder.CreateCall(
      AsmType, Asm, {llvm::ConstantFP::get(Builder.getFloatTy(), 1.5), State});
  Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(Context, {}));
  Builder.CreateRetVoid();
  CEmitterOptions Options;
  Options.TheArch = Arch::X86;
  Options.Format = BinaryFormat::ELF;
  EXPECT_DEATH(
      {
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        LLVMCEmitter().emit(Module, Out, Options);
      },
      "x86-32 FP conversion requires a 32-bit integer result");
}

TEST(X86FPConversionContract, UnmaskedAndUnknownStateCannotPublishIntegers) {
  using namespace neverd;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (unsigned Source : {4U, 8U})
    for (unsigned Destination : {4U, 8U})
      for (unsigned Mode : {0U, 1U, 2U, 3U}) {
        SCOPED_TRACE(testing::Message()
                     << Source << "->" << Destination << " mode=" << Mode);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        const std::vector<uint8_t> Old(Destination + 4, 0x59);
        Emulator.setRegisterBytes(0, Old);
        LowOp Op;
        Op.Opcode = NdOp::INTRINSIC;
        Op.Output = NdVar::tmp(0, Destination + 4);
        Op.addInput(NdVar::cst(
            static_cast<unsigned>(Intrinsic::X86FPCvtToIntState), 2));
        Op.addInput(NdVar::cst(Source == 4
                                   ? (Mode == 1 ? 0x3fc00000 : 0x7f800000)
                                   : (Mode == 1 ? UINT64_C(0x3ff8000000000000)
                                                : UINT64_C(0x7ff0000000000000)),
                               Source));
        Op.addInput(Mode == 2 ? NdVar::tmp(1, 4)
                              : NdVar::cst(Mode == 0   ? 0x1f00
                                           : Mode == 1 ? 0x0f80
                                                       : 0x11f80,
                                           4));
        Op.addInput(NdVar::cst(Destination, 4));
        EXPECT_FALSE(Emulator.step(Op));
        EXPECT_EQ(Emulator.getRegisterBytes(0), Old);
        if (Mode < 2)
          EXPECT_EQ(Emulator.getMXCSR(), Mode == 0 ? 0x1f01U : 0x0fa0U);
      }
}

namespace {
const std::vector<uint64_t> &conversionValues(bool IsDouble) {
  static const std::vector<uint64_t> Single = {
      0,          0x80000000, 1,          0x80000001, 0x007fffff, 0x807fffff,
      0x00800000, 0x80800000, 0x3effffff, 0x3f000000, 0x3f000001, 0xbf000000,
      0x3fc00000, 0xbfc00000, 0x40200000, 0xc0200000, 0x4effffff, 0x4f000000,
      0xcf000000, 0xcf000001, 0x5effffff, 0x5f000000, 0xdf000000, 0xdf000001,
      0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00001, 0x7f800001,
      0xffc00001, 0xff800001};
  static const std::vector<uint64_t> Double = {0,
                                               UINT64_C(0x8000000000000000),
                                               1,
                                               UINT64_C(0x8000000000000001),
                                               UINT64_C(0x000fffffffffffff),
                                               UINT64_C(0x800fffffffffffff),
                                               UINT64_C(0x0010000000000000),
                                               UINT64_C(0x8010000000000000),
                                               UINT64_C(0x3fdfffffffffffff),
                                               UINT64_C(0x3fe0000000000000),
                                               UINT64_C(0x3fe0000000000001),
                                               UINT64_C(0xbfe0000000000000),
                                               UINT64_C(0x3ff8000000000000),
                                               UINT64_C(0xbff8000000000000),
                                               UINT64_C(0x4004000000000000),
                                               UINT64_C(0xc004000000000000),
                                               UINT64_C(0x41dfffffffc00000),
                                               UINT64_C(0x41dfffffffe00000),
                                               UINT64_C(0x41e0000000000000),
                                               UINT64_C(0xc1e0000000000000),
                                               UINT64_C(0x43dfffffffffffff),
                                               UINT64_C(0x43e0000000000000),
                                               UINT64_C(0xc3e0000000000000),
                                               UINT64_C(0xc3e0000000000001),
                                               UINT64_C(0x7fefffffffffffff),
                                               UINT64_C(0xffefffffffffffff),
                                               UINT64_C(0x7ff0000000000000),
                                               UINT64_C(0xfff0000000000000),
                                               UINT64_C(0x7ff8000000000001),
                                               UINT64_C(0x7ff0000000000001),
                                               UINT64_C(0xfff8000000000001),
                                               UINT64_C(0xfff0000000000001)};
  return IsDouble ? Double : Single;
}
} // namespace

TEST(X86FPConversionAccuracy, ExplicitIntegerStateMatchesNativeBoundaries) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (auto Id :
       {Intrinsic::X86FPCvtToIntState, Intrinsic::X86FPTruncToIntState})
    for (unsigned Source : {4U, 8U})
      for (unsigned Destination : {4U, 8U})
        for (unsigned Rounding : {0U, 1U, 2U, 3U})
          for (unsigned Control : {0U, 0x40U, 0x8000U, 0x8040U})
            for (unsigned Sticky : {0U, 0x21U})
              for (uint64_t Bits : conversionValues(Source == 8)) {
                const uint32_t State =
                    0x1f80 | (Rounding << 13) | Control | Sticky;
                const bool Truncate = Id == Intrinsic::X86FPTruncToIntState;
                const auto Expected =
                    Source == 4
                        ? (Destination == 4
                               ? nativeIntegerConversion<float, uint32_t>(
                                     Bits, State, Truncate)
                               : nativeIntegerConversion<float, uint64_t>(
                                     Bits, State, Truncate))
                        : (Destination == 4
                               ? nativeIntegerConversion<double, uint32_t>(
                                     Bits, State, Truncate)
                               : nativeIntegerConversion<double, uint64_t>(
                                     Bits, State, Truncate));
                SCOPED_TRACE(testing::Message()
                             << Source << "->" << Destination
                             << " id=" << static_cast<unsigned>(Id)
                             << " state=" << State << " bits=" << Bits);
                NdOpEmulator Emulator(Image);
                Emulator.setStrictMode(true);
                LowOp Op;
                Op.Opcode = NdOp::INTRINSIC;
                Op.Output = NdVar::tmp(0, Destination + 4);
                Op.addInput(NdVar::cst(static_cast<unsigned>(Id), 2));
                Op.addInput(NdVar::cst(Bits, Source));
                Op.addInput(NdVar::cst(State, 4));
                Op.addInput(NdVar::cst(Destination, 4));
                ASSERT_TRUE(Emulator.step(Op));
                const auto Result = Emulator.getRegisterBytes(0);
                ASSERT_TRUE(Result);
                ASSERT_EQ(Result->size(), Destination + 4);
                uint64_t Actual = 0;
                uint32_t Outgoing = 0;
                std::memcpy(&Actual, Result->data(), Destination);
                std::memcpy(&Outgoing, Result->data() + Destination, 4);
                ASSERT_EQ(Actual, Expected.first);
                ASSERT_EQ(Outgoing, Expected.second);
              }
#else
  GTEST_SKIP() << "native scalar conversion oracle requires x64 GCC/Clang";
#endif
}

namespace {
class X86FPConversionExecution : public X86FPStateFixture {
protected:
  bool PrecisionFault = false;

  std::string comparisonDriver(bool IsDouble, bool DeclareProbe, bool Fault,
                               bool NativeCall = false) override {
    if (Fault) {
      auto Text = faultDriver(IsDouble, DeclareProbe, NativeCall);
      const auto Replace = [&](const std::string &Old, const std::string &New) {
        const auto Position = Text.find(Old);
        if (Position != std::string::npos)
          Text.replace(Position, Old.size(), New);
      };
      Replace("EXCEPTION_FLT_DIVIDE_BY_ZERO",
              PrecisionFault ? "EXCEPTION_FLT_INEXACT_RESULT"
                             : "EXCEPTION_FLT_INVALID_OPERATION");
      // Only the byte oracle promises physical RAX. Generated code retains
      // the architectural value in SSA and may allocate another host GPR.
      if (NativeCall)
        Replace("numerical_output_untouched() ? 0 : 1",
                "numerical_output_untouched() && "
                "info->ContextRecord->Rax == UINT64_C(0x8877665544332211) "
                "? 0 : 1");
      Replace("UINT64_C(0x3ff0000000000000)",
              PrecisionFault ? "UINT64_C(0x3ff8000000000000)"
                             : "UINT64_C(0x7ff0000000000000)");
      Replace("0x3f800000", PrecisionFault ? "0x3fc00000" : "0x7f800000");
      Replace("0x1d80", PrecisionFault ? "0x0f80" : "0x1f00");
      Replace("static void on_fault(int signal_number) {\n"
              "  _exit(signal_number == SIGFPE && numerical_output_untouched() "
              "? 0 : 1);\n}",
              "static void on_fault(int signal_number, siginfo_t *info, "
              "void *context) {\n"
              "  (void)context;\n  _exit(signal_number == SIGFPE && "
              "info->si_code == " +
                  std::string(PrecisionFault ? "FPE_FLTRES" : "FPE_FLTINV") +
                  " && numerical_output_untouched() ? 0 : 1);\n}");
      Replace(
          "if (signal(SIGFPE, on_fault) == SIG_ERR) return 77;",
          "struct sigaction action; memset(&action, 0, sizeof(action));\n"
          "  action.sa_sigaction = on_fault; action.sa_flags = SA_SIGINFO;\n"
          "  sigemptyset(&action.sa_mask);\n"
          "  if (sigaction(SIGFPE, &action, 0)) return 77;");
      return Text;
    }
    auto Text = floatingDriver(IsDouble, DeclareProbe);
    const auto First = Text.find("static const");
    const auto Last = Text.find("int main(void)");
    std::string Values = "static const uint64_t values[] = {";
    for (uint64_t Bits : conversionValues(IsDouble))
      Values += "UINT64_C(" + std::to_string(Bits) + "),";
    Values += "};\n";
    if (First != std::string::npos && Last != std::string::npos)
      Text.replace(First, Last - First, Values);
    // Each instruction consumes one scalar. Preserve the full surrounding
    // buffer and compare RAX's upper half after a 32-bit destination write.
    if (!IsDouble) {
      const std::string Copy =
          "memcpy(expected, &values[a], sizeof(values[0]));";
      const auto Position = Text.find(Copy);
      if (Position != std::string::npos)
        Text.replace(Position, Copy.size(), "memcpy(expected, &values[a], 4);");
    }
    const std::string Loop = "b < sizeof(values)/sizeof(values[0])";
    const auto Position = Text.find(Loop);
    if (Position != std::string::npos)
      Text.replace(Position, Loop.size(), "b < 1");
    return Text;
  }

  static std::vector<uint8_t> integerKernel(bool IsDouble, bool WideResult,
                                            bool Truncate, bool Vex,
                                            bool Memory,
                                            bool KeepResult = true) {
    const uint8_t Arg = memoryModRM();
    // Keep a recognizable old RAX for zero-extension and fault controls.
    std::vector<uint8_t> Bytes = {0x48, 0xb8, 0x11, 0x22, 0x33,
                                  0x44, 0x55, 0x66, 0x77, 0x88};
    if (!Memory)
      Bytes.insert(Bytes.end(), {0xf3, 0x0f, 0x6f, Arg}); // MOVDQU XMM0,[arg]
    if (Vex) {
      if (WideResult)
        Bytes.insert(
            Bytes.end(),
            {0xc4, 0xe1, static_cast<uint8_t>(IsDouble ? 0xfb : 0xfa)});
      else
        Bytes.insert(Bytes.end(),
                     {0xc5, static_cast<uint8_t>(IsDouble ? 0xfb : 0xfa)});
    } else {
      Bytes.push_back(IsDouble ? 0xf2 : 0xf3);
      if (WideResult)
        Bytes.push_back(0x48);
      Bytes.push_back(0x0f);
    }
    Bytes.insert(Bytes.end(), {static_cast<uint8_t>(Truncate ? 0x2c : 0x2d),
                               static_cast<uint8_t>(Memory ? Arg : 0xc0)});
    if (KeepResult)
      Bytes.insert(Bytes.end(),
                   {0x48, 0x89, static_cast<uint8_t>(0x40 | Arg), 48});
    Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
    return Bytes;
  }

  void compareForms(bool IsDouble, bool WideResult, bool Vex) {
    if (Vex && !llvm::sys::getHostCPUFeatures().lookup("avx"))
      GTEST_SKIP() << "AVX is required for the VEX native oracle";
    for (bool Truncate : {false, true})
      for (bool Memory : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << "truncate=" << Truncate << " memory=" << Memory);
        compareFloating(
            integerKernel(IsDouble, WideResult, Truncate, Vex, Memory),
            IsDouble);
      }
  }
};
} // namespace

TEST_F(X86FPConversionExecution, SingleTo32MatchesNativeAllRoutes) {
  compareForms(false, false, false);
  compareForms(false, false, true);
}

TEST_F(X86FPConversionExecution, SingleTo64MatchesNativeAllRoutes) {
  compareForms(false, true, false);
  compareForms(false, true, true);
}

TEST_F(X86FPConversionExecution, DoubleTo32MatchesNativeAllRoutes) {
  compareForms(true, false, false);
  compareForms(true, false, true);
}

TEST_F(X86FPConversionExecution, DoubleTo64MatchesNativeAllRoutes) {
  compareForms(true, true, false);
  compareForms(true, true, true);
}

TEST_F(X86FPConversionExecution, DeadIntegerResultsRetainExceptionFlags) {
  for (bool IsDouble : {false, true})
    compareFloating(
        integerKernel(IsDouble, !IsDouble, false, false, false, false),
        IsDouble);
}

TEST_F(X86FPConversionExecution, UnmaskedInvalidAndPrecisionCommitNoResult) {
  for (bool Precision : {false, true})
    for (bool IsDouble : {false, true}) {
      PrecisionFault = Precision;
      compareFloating(integerKernel(IsDouble, !IsDouble, false, false, false),
                      IsDouble, true);
    }
}

TEST_F(X86FPConversionExecution, HighCTypedOperandsAndNamesPreserveRawBits) {
  if (!nativeX64())
    GTEST_SKIP() << "native x64 host required";
  for (bool IsDouble : {false, true}) {
    const unsigned SourceBytes = IsDouble ? 8 : 4;
    const std::string Scalar = IsDouble ? "double" : "float";
    std::vector<HighFunc> Functions;
    std::string Driver =
        "\n#include <immintrin.h>\nint main(void) {\n"
        "uint32_t saved = _mm_getcsr(), state = 0x5f80;\n"
        "float carrier; __builtin_memcpy(&carrier, &state, 4);\n";
    for (bool Truncate : {false, true})
      for (unsigned Destination : {4U, 8U}) {
        const auto Id = Truncate ? Intrinsic::X86FPTruncToIntState
                                 : Intrinsic::X86FPCvtToIntState;
        const auto Value = HighExpr::makeVar(
            MedVar{.Kind = MedVar::Param,
                   .Id = 0,
                   .Size = static_cast<uint16_t>(SourceBytes)},
            NdType::makeFloat(SourceBytes));
        const auto State =
            HighExpr::makeVar(MedVar{.Kind = MedVar::Param, .Id = 1, .Size = 4},
                              NdType::makeFloat(4));
        auto Converted = HighExpr::makeCall(
            "", 0, {Value, State, HighExpr::makeConst(Destination, 4)});
        Converted->IntrinsicId = Id;
        Converted->Type = NdType::makeInt(Destination + 4, false);
        HighFunc Function;
        Function.Name = std::string(Truncate ? "trunc" : "rounded") +
                        std::to_string(Destination);
        Function.ReturnType = Converted->Type;
        Function.Params = {{"value", Value->Type}, {"state", State->Type}};
        HighStmt Return;
        Return.Kind = StmtKind::Return;
        Return.RetVal = Converted;
        Function.Body.push_back(Return);
        Functions.push_back(Function);
        HighFunc Collision;
        Collision.Name = std::string(intrinsicCName(Id)) + "_f" +
                         std::to_string(SourceBytes * 8) + "_i" +
                         std::to_string(Destination * 8);
        Collision.ReturnType = NdType::makeVoid();
        Functions.push_back(Collision);
        Driver +=
            "{ unsigned _BitInt(96) result = " + Function.Name + "((" + Scalar +
            ")1.5, carrier);\n"
            "if ((uint" +
            std::to_string(Destination * 8) +
            "_t)result != " + (Truncate ? "1" : "2") +
            " || (uint32_t)(result >> " + std::to_string(Destination * 8) +
            ") != 0x5fa0) return 1; }\n";
      }
    Driver += "_mm_setcsr(saved); return 0; }\n";
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = hostFormat();
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
    executeSource(Source, Driver);
  }
}

TEST_F(X86FPConversionExecution, LLVMCUnalignedStateAndNamesPreserveRawBits) {
  if (!nativeX64())
    GTEST_SKIP() << "native x64 host required";
  for (unsigned SourceBytes : {4U, 8U})
    for (unsigned Destination : {4U, 8U}) {
      llvm::LLVMContext Context;
      llvm::Module Module("conversion", Context);
      llvm::IRBuilder<> Builder(Context);
      auto *Number = Builder.getIntNTy(Destination * 8);
      auto *Scalar =
          SourceBytes == 4 ? Builder.getFloatTy() : Builder.getDoubleTy();
      auto *Pointer = Builder.getPtrTy();
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(Number, {Pointer}, false),
          llvm::Function::ExternalLinkage, "unaligned_convert", Module);
      Builder.SetInsertPoint(
          llvm::BasicBlock::Create(Context, "entry", Function));
      const auto Id = Intrinsic::X86FPCvtToIntState;
      auto *Asm = llvm::InlineAsm::get(
          llvm::FunctionType::get(Number, {Scalar, Pointer}, false),
          x86FPStateConversionAsm(Id, SourceBytes),
          X86FPStateConversionConstraints, true);
      auto *Call = Builder.CreateCall(
          Asm, {llvm::ConstantFP::get(Scalar, 1.5), Function->getArg(0)});
      Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(Context, {}));
      Builder.CreateRet(Call);
      auto *Collision = llvm::Function::Create(
          llvm::FunctionType::get(Builder.getVoidTy(), false),
          llvm::Function::ExternalLinkage,
          "neverd_x86_cvt" + std::string(SourceBytes == 4 ? "ss" : "sd") +
              "2si_value_f" + std::to_string(SourceBytes * 8) + "_i" +
              std::to_string(Destination * 8),
          Module);
      Builder.SetInsertPoint(
          llvm::BasicBlock::Create(Context, "entry", Collision));
      Builder.CreateRetVoid();
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      Options.Format = hostFormat();
      Options.PreserveLLVMFunctionTypes = true;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
      executeSource(Source, R"(
#include <immintrin.h>
int main(void) {
  uint32_t saved = _mm_getcsr(), state = 0x5f80;
  _Alignas(4) unsigned char memory[8] = {0x59,0,0,0,0,0x73,0x91,0x25};
  __builtin_memcpy(memory + 1, &state, 4);
  uint64_t result = unaligned_convert((void *)(memory + 1));
  __builtin_memcpy(&state, memory + 1, 4);
  _mm_setcsr(saved);
  return result != 2 || state != 0x5fa0 ||
      memory[0] != 0x59 || memory[5] != 0x73 || memory[6] != 0x91 || memory[7] != 0x25;
}
)");
    }
}

TEST_F(X86FPConversionExecution, IntegerReturnAndFloatingArgumentKeepTheirAbi) {
  if (!nativeX64())
    GTEST_SKIP() << "native x64 host required";
  for (bool IsDouble : {false, true})
    for (unsigned Destination : {4U, 8U}) {
      const std::string Scalar = IsDouble ? "double" : "float";
      const std::string Integer =
          "uint" + std::to_string(Destination * 8) + "_t";
      std::vector<uint8_t> Bytes{static_cast<uint8_t>(IsDouble ? 0xf2 : 0xf3)};
      if (Destination == 8)
        Bytes.push_back(0x48);
      Bytes.insert(Bytes.end(), {0x0f, 0x2d, 0xc0, 0xc3});
      auto NativeText = assembly(Bytes);
      for (size_t Position = 0;
           (Position = NativeText.find("isa_probe", Position)) !=
           std::string::npos;)
        NativeText.replace(Position, 9, "native_probe");
      const auto Native = file("abi-native.s");
      const auto Object = file("abi-native.o");
      write(Native, NativeText);
      auto Built = command(Compiler, {"-c", Native, "-o", Object});
      ASSERT_EQ(Built.Status, 0) << Built.Error;
      std::string Driver = "\n#include <stdint.h>\n#include <string.h>\n"
                           "#include <immintrin.h>\nextern " +
                           Integer + " native_probe(" + Scalar +
                           ");\n"
                           "static const uint64_t values[] = {";
      for (uint64_t Bits : conversionValues(IsDouble))
        Driver += "UINT64_C(" + std::to_string(Bits) + "),";
      Driver +=
          "};\nint main(void) {\nuint32_t saved = _mm_getcsr();\n"
          "for (unsigned rounding=0; rounding<4; ++rounding)\n"
          "for (unsigned daz=0; daz<2; ++daz)\n"
          "for (unsigned a=0; a<sizeof(values)/sizeof(values[0]); ++a) {\n"
          "uint32_t state = 0x1f80 | (rounding << 13) | (daz ? 0x40 : 0);\n" +
          Scalar +
          " input; __builtin_memcpy(&input, &values[a], sizeof(input));\n"
          "_mm_setcsr(state);\n" +
          Integer +
          " expected = native_probe(input);\n"
          "uint32_t expected_state = _mm_getcsr();\n"
          "_mm_setcsr(state);\n" +
          Integer +
          " actual = isa_probe(input);\n"
          "uint32_t actual_state = _mm_getcsr();\n_mm_setcsr(saved);\n"
          "if (expected != actual || expected_state != actual_state) "
          "return 1;\n}\nreturn 0;\n}\n";
      for (bool NoOpt : {false, true})
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << Scalar << "->" << Integer << " NoOpt=" << NoOpt
                       << " LLVMC=" << LLVM);
          auto Image = image(Bytes);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.LiftMode = LLVM;
          Options.SourceProjection = LLVM;
          Options.NoOpt = NoOpt;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = {Entry};
          auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          CEmitterOptions Emission;
          Emission.TheArch = Arch::X64;
          Emission.Format = hostFormat();
          Emission.PreserveLLVMFunctionTypes = false;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          auto TypedDriver = Driver;
          if (LLVM) {
            ASSERT_NE(Result.LlvmModule, nullptr);
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, Emission,
                                            nullptr, &Image));
            const auto *Function = Result.LlvmModule->getFunction("isa_probe");
            ASSERT_NE(Function, nullptr);
            ASSERT_EQ(Function->arg_size(), 1U);
            ASSERT_TRUE(Function->getReturnType()->isIntegerTy());
            if (Function->getArg(0)->getType()->isVectorTy()) {
              const std::string Call = Integer + " actual = isa_probe(input);";
              const auto Position = TypedDriver.find(Call);
              ASSERT_NE(Position, std::string::npos);
              TypedDriver.replace(
                  Position, Call.size(),
                  "uint64_t __attribute__((vector_size(16))) incoming = "
                  "{0,0};\n"
                  "__builtin_memcpy(&incoming, &input, sizeof(input));\n" +
                      Integer + " actual = isa_probe(incoming);");
            }
          } else {
            ASSERT_EQ(Result.HighFuncs.size(), 1U);
            const auto &Function = Result.HighFuncs.front();
            ASSERT_EQ(Function.ReturnType->Kind, NdTypeKind::Int);
            ASSERT_EQ(Function.Params.size(), 1U);
            ASSERT_EQ(Function.Params.front().Type->Kind, NdTypeKind::Float);
            ASSERT_EQ(Function.Params.front().Type->Size, IsDouble ? 8U : 4U);
            ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emission));
          }
          const auto C = file("abi-return.c");
          write(C, Source + TypedDriver);
          for (const char *Optimization : {"-O0", "-O2"}) {
            const auto Executable = file("abi-return.exe");
            Built =
                command(Compiler, {Optimization, Object, C, "-o", Executable});
            ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
            const auto Actual = command(Executable, {});
            EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error << Source;
          }
        }
    }
}
