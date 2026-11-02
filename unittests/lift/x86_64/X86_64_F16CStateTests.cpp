//===- X86_64_F16CStateTests.cpp - F16C native completion checks -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original F16C bytes versus complete numerical, memory and MXCSR effects.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/ir/high/X86/X86FPStateShape.h"

#include <array>
#include <type_traits>

namespace {
struct HalfForm {
  bool Widen;
  unsigned Width;
  bool Memory;
  unsigned Immediate;
};
std::vector<HalfForm> halfForms() {
  std::vector<HalfForm> Forms;
  for (unsigned Width : {16U, 32U})
    for (bool Memory : {false, true}) {
      Forms.push_back({true, Width, Memory, 0});
      for (unsigned Immediate : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 255U})
        Forms.push_back({false, Width, Memory, Immediate});
    }
  return Forms;
}
std::string halfFormName(const HalfForm &F) {
  return std::string(F.Widen ? "Widen" : "Narrow") +
         std::to_string(F.Width * 8) + (F.Memory ? "Memory" : "Register") +
         "I" + std::to_string(F.Immediate);
}
LowOp halfPrimitive(bool Widen, unsigned Width, unsigned Immediate) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(2, Widen ? Width : 16);
  Op.addInput(NdVar::cst(unsigned(Intrinsic::F16CConvert), 2));
  Op.addInput(NdVar::cst(unsigned(Widen ? F16CConvertKind::HalfToSingle
                                        : F16CConvertKind::SingleToHalf),
                         1));
  Op.addInput(NdVar::tmp(1, Widen ? Width / 2 : Width));
  Op.addInput(NdVar::cst(Immediate, 1));
  return Op;
}
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
using HalfNativeResult = std::pair<std::vector<uint8_t>, uint32_t>;
template <unsigned Width, unsigned Immediate>
__attribute__((target("avx,f16c"))) HalfNativeResult
nativeNarrow(llvm::ArrayRef<uint8_t> Input, uint32_t State) {
  const unsigned Saved = _mm_getcsr();
  using SourceType = std::conditional_t<Width == 32, __m256i, __m128i>;
  SourceType Source{};
  __m128i Converted;
  std::memcpy(&Source, Input.data(), Input.size());
  __asm__ volatile("ldmxcsr %1\n\tvcvtps2ph %3,%2,%0\n\tstmxcsr %1"
                   : "=&x"(Converted), "+m"(State)
                   : "x"(Source), "i"(Immediate)
                   : "memory");
  _mm_setcsr(Saved);
  std::vector<uint8_t> Result(16);
  std::memcpy(Result.data(), &Converted, 16);
  return {Result, State};
}
template <unsigned Width>
__attribute__((target("avx,f16c"))) HalfNativeResult
nativeWiden(llvm::ArrayRef<uint8_t> Input, uint32_t State) {
  const unsigned Saved = _mm_getcsr();
  __m128i Source{};
  using ResultType = std::conditional_t<Width == 32, __m256i, __m128i>;
  ResultType Converted;
  std::memcpy(&Source, Input.data(), Input.size());
  __asm__ volatile("ldmxcsr %1\n\tvcvtph2ps %2,%0\n\tstmxcsr %1"
                   : "=&x"(Converted), "+m"(State)
                   : "x"(Source)
                   : "memory");
  _mm_setcsr(Saved);
  std::vector<uint8_t> Result(Width);
  std::memcpy(Result.data(), &Converted, Width);
  return {Result, State};
}
template <unsigned Width, size_t... I>
constexpr auto nativeNarrowFunctions(std::index_sequence<I...>) {
  return std::array{&nativeNarrow<Width, I>...};
}
constexpr auto Narrow128 =
    nativeNarrowFunctions<16>(std::make_index_sequence<256>{});
constexpr auto Narrow256 =
    nativeNarrowFunctions<32>(std::make_index_sequence<256>{});
#endif

TEST(F16CConcrete, DirectedNarrowingBitsAndStateMatchHardware) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  if (!llvm::sys::getHostCPUFeatures().lookup("f16c"))
    GTEST_SKIP() << "Native F16C required";
  const uint32_t Inputs[] = {
      0,          0x80000000, 1,          0x007fffff, 0x00800000, 0x33000000,
      0x33800000, 0x33c00000, 0x387fdfff, 0x387fe000, 0x387fffff, 0x38800000,
      0x3f801000, 0xbf801000, 0x477fdfff, 0x477fe000, 0x477ff000, 0x47800000,
      0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00031, 0x7f800071};
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Width : {16U, 32U})
    for (unsigned Immediate = 0; Immediate < 256; ++Immediate)
      for (unsigned Environment = 0; Environment < 32; ++Environment)
        for (unsigned I = 0; I < std::size(Inputs); ++I) {
          const unsigned State = 0x1f80 | ((Environment & 3) << 13) |
                                 ((Environment & 4) ? 0x40 : 0) |
                                 ((Environment & 8) ? 0x8000 : 0) |
                                 ((Environment & 16) ? 0x25 : 0);
          std::vector<uint8_t> Source(Width);
          for (unsigned Lane = 0; Lane < Width / 4; ++Lane)
            std::memcpy(Source.data() + Lane * 4,
                        &Inputs[(I + Lane * 3) % std::size(Inputs)], 4);
          const auto Expected =
              (Width == 32 ? Narrow256 : Narrow128)[Immediate](Source, State);
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setMXCSR(State);
          E.setRegisterBytes(1, Source);
          const auto Op = halfPrimitive(false, Width, Immediate);
          SCOPED_TRACE(testing::Message() << Width << ':' << Immediate << ':'
                                          << Environment << ':' << I);
          ASSERT_TRUE(E.step(Op));
          ASSERT_TRUE(E.getRegisterBytes(2));
          ASSERT_EQ(*E.getRegisterBytes(2), Expected.first);
          ASSERT_EQ(E.getMXCSR(), Expected.second);
        }
#else
  GTEST_SKIP() << "Native x64 GCC/Clang F16C oracle required";
#endif
}
TEST(F16CConcrete, UnmaskedInvalidPrecedesOtherLanePrecision) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  const uint32_t Values[] = {0x3f801000, 0x7f800071, 0, 0};
  std::vector<uint8_t> Source(sizeof(Values));
  std::memcpy(Source.data(), Values, sizeof(Values));
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setMXCSR(0x1f00);
  E.setRegisterBytes(1, Source);
  E.setRegisterBytes(2, std::vector<uint8_t>(16, 0xa7));
  EXPECT_FALSE(E.step(halfPrimitive(false, 16, 0)));
  EXPECT_EQ(E.getMXCSR(), 0x1f01U);
  EXPECT_EQ(E.getRegisterBytes(2), std::vector<uint8_t>(16, 0xa7));
}

LowOp halfStateOp(Intrinsic Id, unsigned Control, unsigned Immediate,
                  uint32_t State) {
  const bool Store = Id == Intrinsic::X86F16CStoreState,
             Read = Id == Intrinsic::X86F16CReadState;
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(2, Store ? 4 : x86F16CDestinationBytes(Control) + 4);
  Op.addInput(NdVar::cst(unsigned(Id), 2));
  Op.addInput(NdVar::tmp(1, Store || Read ? 8 : x86F16CSourceBytes(Control)));
  if (Store)
    Op.addInput(NdVar::tmp(3, x86F16CSourceBytes(Control)));
  Op.addInput(NdVar::cst(Control, 1));
  Op.addInput(NdVar::cst(Immediate, 1));
  Op.addInput(NdVar::cst(State, 4));
  return Op;
}
TEST(F16CConcrete, EveryHalfEncodingMatchesHardwareIncludingNanPayloads) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  if (!llvm::sys::getHostCPUFeatures().lookup("f16c"))
    GTEST_SKIP() << "Native F16C required";
  BinaryImage Image;
  Image.Arch = Arch::X64;
  for (unsigned Width : {16U, 32U})
    for (unsigned Env = 0; Env < 8; ++Env)
      for (unsigned Bits = 0; Bits < 65536; ++Bits) {
        const unsigned State = 0x1f80 | ((Env & 1) ? 0x40 : 0) |
                               ((Env & 2) ? 0x8000 : 0) |
                               ((Env & 4) ? 0x25 : 0);
        std::vector<uint8_t> Input(Width / 2);
        for (unsigned Lane = 0; Lane < Width / 4; ++Lane) {
          uint16_t Value = Bits + Lane * 977;
          std::memcpy(Input.data() + Lane * 2, &Value, 2);
        }
        const auto Native = Width == 32 ? nativeWiden<32>(Input, State)
                                        : nativeWiden<16>(Input, State);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegisterBytes(1, Input);
        const auto Op =
            halfStateOp(Intrinsic::X86F16CState, Width == 32 ? 2 : 0, 0, State);
        SCOPED_TRACE(testing::Message() << Width << ':' << Env << ':' << Bits);
        ASSERT_TRUE(E.step(Op));
        const auto Out = E.getRegisterBytes(2);
        ASSERT_TRUE(Out);
        ASSERT_TRUE(
            std::equal(Native.first.begin(), Native.first.end(), Out->begin()));
        ASSERT_EQ(E.getMXCSR(), Native.second);
        uint32_t OutState;
        std::memcpy(&OutState, Out->data() + Width, 4);
        ASSERT_EQ(OutState, Native.second);
      }
#else
  GTEST_SKIP() << "Native x64 GCC/Clang F16C oracle required";
#endif
}
TEST(F16CConcrete, RandomSingleBitsMatchNativeForEveryRoundingSelector) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  if (!llvm::sys::getHostCPUFeatures().lookup("f16c"))
    GTEST_SKIP() << "Native F16C required";
  BinaryImage Image;
  Image.Arch = Arch::X64;
  uint64_t Random = UINT64_C(0x87abc1973ab468e1);
  for (unsigned Width : {16U, 32U})
    for (unsigned Imm = 0; Imm < 8; ++Imm)
      for (unsigned Env = 0; Env < 32; ++Env)
        for (unsigned I = 0; I < 256; ++I) {
          const unsigned State =
              0x1f80 | ((Env & 3) << 13) | ((Env & 4) ? 0x40 : 0) |
              ((Env & 8) ? 0x8000 : 0) | ((Env & 16) ? 0x25 : 0);
          std::vector<uint8_t> Input(Width);
          for (unsigned Lane = 0; Lane < Width / 4; ++Lane) {
            Random ^= Random << 13;
            Random ^= Random >> 7;
            Random ^= Random << 17;
            uint32_t Bits = Random;
            std::memcpy(Input.data() + Lane * 4, &Bits, 4);
          }
          const auto Native =
              (Width == 32 ? Narrow256 : Narrow128)[Imm](Input, State);
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegisterBytes(1, Input);
          const auto Op = halfStateOp(Intrinsic::X86F16CState,
                                      Width == 32 ? 3 : 1, Imm, State);
          SCOPED_TRACE(testing::Message()
                       << Width << ':' << Imm << ':' << Env << ':' << I);
          ASSERT_TRUE(E.step(Op));
          const auto Out = E.getRegisterBytes(2);
          ASSERT_TRUE(Out);
          ASSERT_TRUE(std::equal(Native.first.begin(), Native.first.end(),
                                 Out->begin()));
          ASSERT_EQ(E.getMXCSR(), Native.second);
        }
#else
  GTEST_SKIP() << "Native x64 GCC/Clang F16C oracle required";
#endif
}
TEST(F16CContract, CheckedReadsAndStoresRetainExtentPermissionsAndStateOrder) {
  for (unsigned Control = 0; Control < 4; ++Control)
    for (auto Space :
         {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
          NdMemoryAddressSpace::X86GS})
      for (unsigned Fault = 0; Fault < 7; ++Fault) {
        const bool Store = x86F16CIsNarrow(Control);
        const auto Id =
            Store ? Intrinsic::X86F16CStoreState : Intrinsic::X86F16CReadState;
        const unsigned Bytes =
            Store ? x86F16CStoreBytes(Control) : x86F16CSourceBytes(Control);
        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        Segment Segment;
        Segment.VA = 0x2010;
        Segment.Size = Segment.FileSz =
            Bytes + (Fault == 4 ? 1 : 0) - (Fault == 2 ? 1 : 0);
        Segment.Data.assign(Segment.Size, Store ? 0xa7 : 0);
        Segment.Flags =
            Fault == 1
                ? (Store ? SegmentFlags::Readable : SegmentFlags::Writable)
                : SegmentFlags::Readable | SegmentFlags::Writable;
        Image.Segments.push_back(Segment);
        for (bool Strict : {false, true}) {
          NdOpEmulator E(Image);
          E.setStrictMode(Strict);
          E.setMXCSR(0x3f80);
          if (Fault != 3)
            E.setX86LinearAddressBits(48);
          if (Space != NdMemoryAddressSpace::Default && Fault != 3)
            E.setMemoryAddressSpaceBase(Space, Fault == 6 ? 0x9000 : 0x1000);
          E.setRegister(
              1,
              Fault == 5 ? UINT64_C(0x0000800000000000)
              : Fault == 6 && Space != NdMemoryAddressSpace::Default
                  ? UINT64_C(0x2010) - UINT64_C(0x9000)
                  : (Space == NdMemoryAddressSpace::Default ? 0x2010 : 0x1010) +
                        (Fault == 4 ? 1 : 0));
          std::vector<uint8_t> Source(x86F16CSourceBytes(Control));
          uint32_t Nan = 0x7f800071;
          if (Store)
            std::memcpy(Source.data(), &Nan, 4);
          E.setRegisterBytes(3, Source);
          auto Op = halfStateOp(Id, Control, 0, 0x1f80);
          Op.MemoryAddressSpace = Space;
          const bool Complete = Fault == 0 || Fault == 4 || Fault == 6;
          const std::vector<uint8_t> Before(Op.Output.Size, 0xa5);
          E.setRegisterBytes(2, Before);
          SCOPED_TRACE(testing::Message() << Control << ':' << unsigned(Space)
                                          << ':' << Fault << ':' << Strict);
          EXPECT_EQ(E.step(Op), Complete);
          if (Store && !Complete) {
            LowOp Read;
            Read.Opcode = NdOp::INTRINSIC;
            Read.Output = NdVar::tmp(9, 4);
            Read.addInput(NdVar::cst(unsigned(Intrinsic::X86ReadMXCSR), 2));
            EXPECT_FALSE(E.step(Read));
            EXPECT_FALSE(E.getRegister(9));
          } else
            EXPECT_EQ(E.getMXCSR(), Store        ? 0x1f81U
                                    : Fault == 3 ? 0x3f80U
                                                 : 0x1f80U);
          if (!Complete)
            EXPECT_EQ(E.getRegisterBytes(2), Before);
          for (unsigned Byte = 0; Byte < Segment.Size; ++Byte) {
            if (!Complete && Fault != 1) {
              LowOp Load;
              Load.Opcode = NdOp::LOAD;
              Load.Output = NdVar::tmp(8, 1);
              Load.addInput(NdVar::cst(Segment.VA + Byte, 8));
              ASSERT_TRUE(E.step(Load));
              EXPECT_EQ(E.getRegister(8), Segment.Data[Byte]);
            }
          }
        }
      }
}
TEST(F16CContract, CompetingStoreFaultsDoNotInventExactState) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Segment S;
  S.VA = 0x2000;
  S.Size = S.FileSz = 16;
  S.Data.assign(16, 0xa7);
  S.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(S);
  for (bool Writable : {false, true})
    for (unsigned State : {0x1f00U, 0x0f80U, 0x1780U}) {
      const uint32_t Bits = State == 0x1f00   ? 0x7f800071
                            : State == 0x0f80 ? 0x3f801000
                                              : 0x33800000;
      const unsigned Flags = State == 0x1f00   ? 1
                             : State == 0x0f80 ? 0x20
                                               : 0x10;
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setMXCSR(0x3f80);
      E.setX86LinearAddressBits(48);
      E.setRegister(1, Writable ? 0x2000 : UINT64_MAX);
      std::vector<uint8_t> Source(16);
      std::memcpy(Source.data(), &Bits, 4);
      E.setRegisterBytes(3, Source);
      E.setRegister(2, 0xa7a7a7a7);
      EXPECT_FALSE(
          E.step(halfStateOp(Intrinsic::X86F16CStoreState, 1, 0, State)));
      if (Writable)
        EXPECT_EQ(E.getMXCSR(), State | Flags);
      LowOp Read;
      Read.Opcode = NdOp::INTRINSIC;
      Read.Output = NdVar::tmp(9, 4);
      Read.addInput(NdVar::cst(unsigned(Intrinsic::X86ReadMXCSR), 2));
      EXPECT_EQ(E.step(Read), Writable);
      if (Writable)
        EXPECT_EQ(E.getRegister(9), State | Flags);
      else
        EXPECT_FALSE(E.getRegister(9));
      EXPECT_EQ(E.getRegister(2), 0xa7a7a7a7U);
    }
}
TEST(F16CContract, FailedStoresDoNotInventMaskedResultStatus) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Segment S;
  S.VA = 0x2000;
  S.Size = S.FileSz = 16;
  S.Data.assign(16, 0xa7);
  S.Flags = SegmentFlags::Readable;
  Image.Segments.push_back(S);
  for (uint32_t Bits :
       {0x7f800071U, 1U, 0x3f801000U, 0x33000000U, 0x7f7fffffU}) {
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setX86LinearAddressBits(48);
    E.setRegister(1, 0x2000);
    std::vector<uint8_t> Input(16);
    std::memcpy(Input.data(), &Bits, 4);
    E.setRegisterBytes(3, Input);
    E.setRegister(2, 0xa7a7a7a7);
    EXPECT_FALSE(
        E.step(halfStateOp(Intrinsic::X86F16CStoreState, 1, 0, 0x1f80)));
    EXPECT_EQ(E.getRegister(2), 0xa7a7a7a7U);
    LowOp Read;
    Read.Opcode = NdOp::INTRINSIC;
    Read.Output = NdVar::tmp(9, 4);
    Read.addInput(NdVar::cst(unsigned(Intrinsic::X86ReadMXCSR), 2));
    EXPECT_FALSE(E.step(Read));
    EXPECT_FALSE(E.getRegister(9));
  }
}
TEST(F16CContract, ShapesPreserveRoleIndicesAndRawPackedCarriers) {
  for (Intrinsic Id : {Intrinsic::X86F16CState, Intrinsic::X86F16CReadState,
                       Intrinsic::X86F16CStoreState})
    for (unsigned Control = 0; Control < 4; ++Control) {
      if ((Id == Intrinsic::X86F16CReadState && (Control & 1)) ||
          (Id == Intrinsic::X86F16CStoreState && !(Control & 1)))
        continue;
      const bool Store = Id == Intrinsic::X86F16CStoreState;
      auto Op = halfStateOp(Id, Control, 0, 0x1f80);
      EXPECT_TRUE(
          x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X64)));
      EXPECT_EQ(x86FPStateNumericalSliceSize(
                    Id, x86FPStateLowShape(Op, Arch::X64), 0, 4),
                0U);
      for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
        auto Bad = Op;
        switch (Mutation) {
        case 0:
          Bad.Inputs[Store ? 3 : 2] = NdVar::tmp(9, 1);
          break;
        case 1:
          Bad.Inputs[Store ? 3 : 2].Offset |= 4;
          break;
        case 2:
          Bad.Inputs[Store ? 4 : 3].Size = 2;
          break;
        case 3:
          Bad.Inputs[Store ? 5 : 4].Size = 8;
          break;
        case 4:
          Bad.Output.Size++;
          break;
        case 5:
          Bad.NumInputs--;
          break;
        case 6:
          Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
          break;
        case 7:
          Bad.Inputs[0].Size = 4;
          break;
        }
        EXPECT_FALSE(
            x86FPStateShapeIsValid(Id, x86FPStateLowShape(Bad, Arch::X64)));
      }
      MedOp Med;
      Med.Opcode = NdOp::INTRINSIC;
      Med.Output = {.Kind = MedVar::Temp,
                    .TheArch = Arch::X64,
                    .Id = 2,
                    .Size = Op.Output.Size};
      std::vector<ExprPtr> Operands;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        Med.addInput(
            MedVar::makeConst(Op.Inputs[I].isConst() ? Op.Inputs[I].Offset : 0,
                              Op.Inputs[I].Size));
        if (I)
          Operands.push_back(HighExpr::makeConst(
              Op.Inputs[I].isConst() ? Op.Inputs[I].Offset : 0,
              Op.Inputs[I].Size));
      }
      EXPECT_TRUE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Med)));
      Med.Inputs[1].TheArch = Arch::ARM;
      EXPECT_FALSE(x86FPStateShapeIsValid(Id, x86FPStateMedShape(Med)));
      auto Call = HighExpr::makeCall("", 0, std::move(Operands));
      Call->IntrinsicId = Id;
      Call->Type = NdType::makeInt(Op.Output.Size, false);
      EXPECT_TRUE(
          x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
      Call->Operands[Store ? 1 : 0]->Type =
          NdType::makeFloat(Op.Inputs[Store ? 2 : 1].Size);
      EXPECT_FALSE(
          x86FPStateShapeIsValid(Id, x86FPStateHighShape(*Call, Arch::X64)));
    }
}
// A widening memory form reads only its half-precision source, not the
// single-precision width of its result; a narrowing store reads nothing.
TEST(F16CContract, MemoryFormsReadOnlyTheirHalfSource) {
  for (unsigned Control : {0U, 2U})
    EXPECT_EQ(x86FPStateMemoryBytes(Intrinsic::X86F16CReadState,
                                    x86F16CDestinationBytes(Control) + 4,
                                    Control),
              Control ? 16U : 8U);
  for (unsigned Control : {1U, 3U})
    EXPECT_EQ(x86FPStateMemoryBytes(Intrinsic::X86F16CStoreState, 4, Control),
              0U);
}
TEST(F16CContract, RawEncodingAndDecodedMetadataAgreeForAllImmediateBytes) {
  for (unsigned Width : {16U, 32U})
    for (bool Memory : {false, true})
      for (bool Narrow : {false, true})
        for (unsigned Imm = 0; Imm < (Narrow ? 256U : 1U); ++Imm) {
          std::vector<uint8_t> Bytes{0xc4, uint8_t(Narrow ? 0xe3 : 0xe2),
                                     uint8_t(0x79 | (Width == 32 ? 4 : 0)),
                                     uint8_t(Narrow ? 0x1d : 0x13),
                                     uint8_t(Memory   ? 0x40
                                             : Narrow ? 0xc8
                                                      : 0xc1)};
          if (Memory)
            Bytes.push_back(16);
          if (Narrow)
            Bytes.push_back(Imm);
          Decoder Decode;
          ASSERT_TRUE(Decode.init(Arch::X64));
          DecodedInsn Insn;
          ASSERT_EQ(
              Decode.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Insn),
              int(Bytes.size()));
          std::vector<LowOp> Ops;
          ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops));
          unsigned Found = 0;
          for (const auto &Op : Ops)
            if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                isX86F16CStateIntrinsic(
                    static_cast<Intrinsic>(Op.Inputs[0].Offset))) {
              ++Found;
              EXPECT_TRUE(x86FPStateShapeIsValid(
                  static_cast<Intrinsic>(Op.Inputs[0].Offset),
                  x86FPStateLowShape(Op, Arch::X64)));
            }
          EXPECT_EQ(Found, 1U);
          if (Imm && Imm != 255)
            continue;
          for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
            DecodedInsn Bad;
            ASSERT_EQ(
                Decode.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Bad),
                int(Bytes.size()));
            auto &X = Bad.Raw->detail->x86;
            switch (Mutation) {
            case 0:
              X.avx_sae = true;
              break;
            case 1:
              X.operands[0].size = 64;
              break;
            case 2:
              X.operands[1].size = 64;
              break;
            case 3:
              X.modrm ^= 1;
              break;
            case 4:
              X.operands[0].avx_zero_opmask = true;
              break;
            case 5:
              X.operands[1].avx_bcast = X86_AVX_BCAST_4;
              break;
            case 6:
              X.encoding.imm_offset ^= 1;
              break;
            case 7:
              X.avx_rm = X86_AVX_RM_RD;
              break;
            case 8:
              Bad.Raw->size = 16;
              break;
            }
            std::vector<LowOp> Rejected;
            EXPECT_THROW(Decode.liftToLow(Bad, Rejected), UnliftedInstruction);
          }
        }
}

TEST(F16CContract, OwnedAssemblyAuthenticatesTypesSegmentsAndCompletion) {
  for (Intrinsic Id : {Intrinsic::X86F16CState, Intrinsic::X86F16CReadState,
                       Intrinsic::X86F16CStoreState})
    for (unsigned Control = 0; Control < 4; ++Control) {
      const bool Store = Id == Intrinsic::X86F16CStoreState,
                 Read = Id == Intrinsic::X86F16CReadState;
      if ((Store && !(Control & 1)) || (Read && (Control & 1)))
        continue;
      for (auto Space :
           {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
            NdMemoryAddressSpace::X86GS}) {
        if (!Store && !Read && Space != NdMemoryAddressSpace::Default)
          continue;
        for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
          llvm::LLVMContext C;
          llvm::Module Module("f16c-owned", C);
          llvm::IRBuilder<> B(C);
          const unsigned Layout = x86FPRoundStateLayout(
              x86F16CSourceBytes(Control), Control, Control & 1 ? 7 : 0, Space);
          llvm::Type *Return =
              Store ? B.getVoidTy() : x86F16CResultType(C, Control);
          std::vector<llvm::Type *> Types;
          if (Store || Read)
            Types.push_back(B.getPtrTy(
                Mutation == 1 ? 999 : llvmX86MemoryAddressSpace(Space)));
          if (!Read)
            Types.push_back(Mutation == 1 ? B.getInt64Ty()
                                          : x86F16CSourceType(C, Control));
          Types.push_back(B.getPtrTy(Mutation == 2 ? 256 : 0));
          if (Mutation == 3)
            Return = B.getInt64Ty();
          auto *F = llvm::Function::Create(
              llvm::FunctionType::get(Return, Types, false),
              llvm::Function::ExternalLinkage, "f16c", Module);
          B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
          auto Text = x86F16CAsm(Id, Layout);
          auto Constraints = std::string(x86F16CConstraints(Id));
          if (Mutation == 4)
            Text += "\n\tnop";
          if (Mutation == 5)
            Constraints.resize(Constraints.find(",~{memory}"));
          auto *Asm =
              llvm::InlineAsm::get(F->getFunctionType(), Text, Constraints,
                                   Mutation != 6, Mutation == 7,
                                   Mutation == 8 ? llvm::InlineAsm::AD_Intel
                                                 : llvm::InlineAsm::AD_ATT);
          std::vector<llvm::Value *> Args;
          for (auto &A : F->args())
            Args.push_back(&A);
          auto *Call = B.CreateCall(Asm, Args);
          if (Mutation != 9)
            Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(C, {}));
          if (Return->isVoidTy())
            B.CreateRetVoid();
          else
            B.CreateRet(Call);
          if (!Mutation)
            EXPECT_EQ(classifyX86FPStateAsm(*Call),
                      std::make_optional(std::pair{Id, Layout}));
          else if (Mutation == 9)
            EXPECT_FALSE(classifyX86FPStateAsm(*Call));
          else
            EXPECT_DEATH(classifyX86FPStateAsm(*Call),
                         "invalid x86 FP state assembly");
        }
      }
    }
}

class F16CFixture : public X86FPStateFixture {
protected:
  HalfForm Form{true, 16, false, 0};
  bool KeepResult = true;
  unsigned SourceOffset = 64;
  unsigned DestinationOffset = 32;
  unsigned SourceRegister = 1;
  unsigned FaultMode = 0;
  bool IndirectMemory = false;
  bool GSMemory = false;
  std::vector<uint8_t> kernel() {
#if defined(_WIN32)
    constexpr unsigned Arg = 1;
#else
    constexpr unsigned Arg = 7;
#endif
    std::vector<uint8_t> Bytes{0xc5, 0xfe, 0x6f, uint8_t(Arg)};
    if (!Form.Widen || !Form.Memory)
      Bytes.insert(Bytes.end(), {0xc5, 0xfe, 0x6f,
                                 uint8_t(0x40 | (SourceRegister << 3) | Arg),
                                 uint8_t(SourceOffset)});
    if (IndirectMemory)
      Bytes.insert(Bytes.end(),
                   {0x48, 0x8b, uint8_t(0x80 | Arg), 144, 0, 0, 0});
    if (GSMemory)
      Bytes.push_back(0x65);
    Bytes.insert(Bytes.end(), {0xc4, uint8_t(Form.Widen ? 0xe2 : 0xe3),
                               uint8_t(0x79 | (Form.Width == 32 ? 4 : 0)),
                               uint8_t(Form.Widen ? 0x13 : 0x1d)});
    if (Form.Memory && IndirectMemory)
      Bytes.push_back(uint8_t(Form.Widen ? 0 : SourceRegister << 3));
    else if (Form.Memory)
      Bytes.insert(
          Bytes.end(),
          {uint8_t(0x40 | (Form.Widen ? 0 : SourceRegister << 3) | Arg),
           uint8_t(Form.Widen ? SourceOffset : DestinationOffset)});
    else
      Bytes.push_back(uint8_t(Form.Widen ? 0xc0 | SourceRegister
                                         : 0xc0 | (SourceRegister << 3)));
    if (!Form.Widen)
      Bytes.push_back(uint8_t(Form.Immediate));
    if (KeepResult)
      Bytes.insert(Bytes.end(), {0xc5, 0xfe, 0x7f, uint8_t(0x40 | Arg), 96});
    Bytes.insert(Bytes.end(),
                 {0x0f, 0xae, uint8_t(0x98 | Arg), 128, 0, 0, 0, 0xc3});
    return Bytes;
  }
  std::string conversionFaultDriver(bool Declare, bool NativeCall) {
    const bool ReadFault = FaultMode == 6;
    const bool Page = FaultMode >= 6;
    const bool Precision = FaultMode == 1 || FaultMode == 9;
    const bool Tiny = FaultMode == 2 || FaultMode == 10;
    const unsigned State = FaultMode == 1   ? 0x0f80
                           : FaultMode == 2 ? 0x1780
                           : FaultMode == 3 ? 0x1e80
                           : FaultMode >= 8 ? 0x1f80
                                            : 0x1f00;
    const unsigned Flags = ReadFault         ? 0
                           : Precision       ? 0x20
                           : FaultMode == 2  ? 0x10
                           : FaultMode == 3  ? 2
                           : FaultMode == 10 ? 0
                                             : 1;
    const uint32_t Bits = Form.Widen                    ? 0x7c71
                          : Precision || FaultMode == 4 ? 0x3f801000
                          : Tiny                        ? 0x33800000
                          : FaultMode == 3              ? 1
                                                        : 0x7f800071;
    std::string Text =
        "#define _GNU_SOURCE\n#include <stdint.h>\n#include "
        "<stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include "
        "<immintrin.h>\nextern void native_probe(uintptr_t);\n";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += "#define UNCERTAIN_FLAGS " +
            std::to_string(FaultMode >= 7 ? Flags : 0) +
            "\n#define COMPETING_FAULTS " + std::to_string(FaultMode == 7) +
            "\n#define MEMORY_FAULT " +
            std::to_string(ReadFault || FaultMode >= 8) +
            "\n#define PAGE_TEST " + std::to_string(Page) +
            "\n#define CROSS_PAGE " +
            std::to_string(ReadFault || FaultMode == 9 || FaultMode == 10) +
            "\n#define READ_TEST " + std::to_string(ReadFault) +
            "\n#define READ_ONLY " + std::to_string(FaultMode == 11) +
            "\n#define ACCESS_BYTES " + std::to_string(Form.Width / 2) +
            "\n#define EXPECTED_FLOAT_FLAG " + std::to_string(Flags) +
            "\n#define EXPECTED_STATE " + std::to_string(State | Flags) + "u\n";
    Text += R"(
static unsigned char actual[160], before[160];
static unsigned char *operand, *prefix;
static unsigned char prefix_before[4];
static void check(unsigned state, uintptr_t address, int access_fault) {
  /* Repeated identical original bytes expose both IE and PE states at #PF.
     Preserve all other state and fault atomicity; the concrete model refuses
     an exact MXCSR read rather than choosing one observed outcome. */
  if((state^EXPECTED_STATE)&(access_fault ? ~(unsigned)UNCERTAIN_FLAGS : ~0u)) {
    fprintf(stderr,"expected=%08x actual=%08x\n",EXPECTED_STATE,state); _Exit(2);
  }
  if(memcmp(actual,before,160)) _Exit(3);
  if(prefix && memcmp(prefix,prefix_before,READ_TEST ? 2 : 4)) _Exit(4);
  if(access_fault && (address<(uintptr_t)operand || address>=(uintptr_t)operand+ACCESS_BYTES)) _Exit(5);
  _Exit(0);
}
#ifdef _WIN32
#include <windows.h>
static LONG CALLBACK observe(EXCEPTION_POINTERS *info) {
  DWORD code=info->ExceptionRecord->ExceptionCode;
  int access_fault=code==EXCEPTION_ACCESS_VIOLATION;
  /* Windows reports SIMD denormal #XM as invalid operation; MXCSR retains DE. */
  DWORD expected_float=EXPECTED_FLOAT_FLAG==1 || EXPECTED_FLOAT_FLAG==2 ? EXCEPTION_FLT_INVALID_OPERATION : EXPECTED_FLOAT_FLAG==16 ? EXCEPTION_FLT_UNDERFLOW : EXCEPTION_FLT_INEXACT_RESULT;
  int floating_fault=code==expected_float;
  if(COMPETING_FAULTS ? !access_fault && !floating_fault : MEMORY_FAULT ? !access_fault : !floating_fault) return EXCEPTION_CONTINUE_SEARCH;
  if((info->ContextRecord->MxCsr^EXPECTED_STATE)&(access_fault ? ~(unsigned)UNCERTAIN_FLAGS : ~0u)) {
    fprintf(stderr,"rip=%016llx xmm0=%016llx/%016llx FltSaveMxCsr=%08lx\n",
      (unsigned long long)info->ContextRecord->Rip,
      (unsigned long long)info->ContextRecord->Xmm0.High,
      (unsigned long long)info->ContextRecord->Xmm0.Low,
      info->ContextRecord->FltSave.MxCsr);
  }
  check(info->ContextRecord->MxCsr,access_fault ? (uintptr_t)info->ExceptionRecord->ExceptionInformation[1] : 0,access_fault);
  return EXCEPTION_CONTINUE_SEARCH;
}
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ucontext.h>
static void observe(int signal,siginfo_t *info,void *context) {
  if(COMPETING_FAULTS ? signal!=SIGSEGV && signal!=SIGFPE : signal!=(MEMORY_FAULT ? SIGSEGV : SIGFPE)) _Exit(6);
  ucontext_t *uc=context; check(uc->uc_mcontext.fpregs->mxcsr,(uintptr_t)info->si_addr,signal==SIGSEGV);
}
#endif
int main(void) {
  for(unsigned i=0;i<160;++i)actual[i]=(unsigned char)(0x59u+i*37u);
)";
    Text += "  memset(actual+64,0,32); uint32_t bits=" + std::to_string(Bits) +
            "u; memcpy(actual+64,&bits," + std::to_string(Form.Widen ? 2 : 4) +
            ");\n";
    if (FaultMode == 4)
      Text += "  uint32_t nan=0x7f800071;memcpy(actual+68,&nan,4);\n";
    Text += R"(
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  if(!AddVectoredExceptionHandler(1,observe))return 10;
  if(PAGE_TEST) {
    SYSTEM_INFO si;GetSystemInfo(&si);size_t page=si.dwPageSize;
    unsigned char *base=VirtualAlloc(0,2*page,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!base)return 11; memset(base,0xa7,2*page);
    operand=CROSS_PAGE ? base+page-(READ_TEST ? 2 : 4) : base+page;
    if(READ_TEST) {uint16_t nan=0x7c71;memcpy(operand,&nan,2);}
    if(CROSS_PAGE) {prefix=operand;memcpy(prefix_before,prefix,READ_TEST ? 2 : 4);}
    DWORD old;if(!VirtualProtect(base+page,page,READ_ONLY ? PAGE_READONLY : PAGE_NOACCESS,&old))return 12;
  }
#else
  struct sigaction sa={0};sa.sa_sigaction=observe;sa.sa_flags=SA_SIGINFO;sigemptyset(&sa.sa_mask);
  if(sigaction(MEMORY_FAULT ? SIGSEGV : SIGFPE,&sa,0))return 10;
  if(COMPETING_FAULTS && sigaction(SIGSEGV,&sa,0))return 10;
  if(PAGE_TEST) {
    size_t page=sysconf(_SC_PAGESIZE);unsigned char *base=mmap(0,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(base==MAP_FAILED)return 11;memset(base,0xa7,2*page);
    operand=CROSS_PAGE ? base+page-(READ_TEST ? 2 : 4) : base+page;
    if(READ_TEST) {uint16_t nan=0x7c71;memcpy(operand,&nan,2);}
    if(CROSS_PAGE) {prefix=operand;memcpy(prefix_before,prefix,READ_TEST ? 2 : 4);}
    if(mprotect(base+page,page,READ_ONLY ? PROT_READ : PROT_NONE))return 12;
  }
#endif
  if(PAGE_TEST) memcpy(actual+144,&operand,sizeof(operand));
  memcpy(before,actual,160);
)";
    Text += "  _mm_setcsr(" + std::to_string(State) + ");\n";
    Text += NativeCall ? "  native_probe((uintptr_t)actual);\n"
                       : "  isa_probe((uintptr_t)actual);\n";
    return Text + "  return 99; /* required exception absent */\n}\n";
  }

  std::string comparisonDriver(bool, bool Declare, bool Fault,
                               bool NativeCall = false) override {
    if (Fault)
      return conversionFaultDriver(Declare, NativeCall);
    std::string Text = "#include <stdint.h>\n#include <stdio.h>\n#include "
                       "<string.h>\n#include <immintrin.h>\nextern void "
                       "native_probe(uintptr_t);\n";
    if (Declare)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += "static const uint32_t values[] = {";
    Text += Form.Widen
                ? "0,0x8000,1,0x3ff,0x400,0x3c00,0xbc00,0x7bff,0x7c00,0xfc00,"
                  "0x7e31,0x7c71"
                : "0,0x80000000,1,0x007fffff,0x00800000,0x33000000,0x33800000,"
                  "0x33c00000,0x387fdfff,0x387fe000,0x387fffff,0x38800000,"
                  "0x3f801000,0xbf801000,0x477fdfff,0x477fe000,0x477ff000,"
                  "0x47800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,"
                  "0x7fc00031,0x7f800071";
    Text += R"(
};
int main(void) {
  unsigned saved=_mm_getcsr();
  for(unsigned environment=0;environment<32;++environment)
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i) {
      _Alignas(64) unsigned char expected[160],actual[160];
      for(unsigned j=0;j<160;++j) expected[j]=(unsigned char)(0x59u+j*37u);
)";
    Text += "      for(unsigned lane=0;lane<" + std::to_string(Form.Width / 4) +
            ";++lane) memcpy(expected+" + std::to_string(SourceOffset) +
            "+lane*" + std::to_string(Form.Widen ? 2 : 4) +
            ", &values[(i+lane*3)%(sizeof(values)/sizeof(values[0]))]," +
            std::to_string(Form.Widen ? 2 : 4) + ");\n";
    Text += R"(
      memcpy(actual,expected,160);
      unsigned state=0x1f80|((environment&3)<<13)|((environment&4)?0x40:0)|
                     ((environment&8)?0x8000:0)|((environment&16)?0x25:0);
      _mm_setcsr(state); native_probe((uintptr_t)expected);
      unsigned expected_state=_mm_getcsr();
      _mm_setcsr(state); isa_probe((uintptr_t)actual);
      unsigned actual_state=_mm_getcsr(); _mm_setcsr(saved);
      if(expected_state!=actual_state || memcmp(expected,actual,160)) {
        printf("input=%u state=%08x expected=%08x actual=%08x\n",i,state,expected_state,actual_state);
        for(unsigned j=0;j<160;++j)
          if(expected[j]!=actual[j]) printf("byte%u expected=%02x actual=%02x\n",j,expected[j],actual[j]);
        return 1;
      }
    }
  return 0;
}
)";
    if (GSMemory) {
      const auto Replace = [&](const std::string &Old, const std::string &New) {
        const auto Index = Text.find(Old);
        EXPECT_NE(Index, std::string::npos);
        if (Index != std::string::npos)
          Text.replace(Index, Old.size(), New);
      };
      Replace("#include <immintrin.h>",
              "#include <immintrin.h>\n#include <intrin.h>");
      Replace("      memcpy(actual,expected,160);",
              "      uintptr_t "
              "target=(uintptr_t)(expected+32)-(uintptr_t)__readgsqword(0x30);"
              "memcpy(expected+144,&target,8);\n      "
              "memcpy(actual,expected,160);");
      Replace("      _mm_setcsr(state); isa_probe((uintptr_t)actual);",
              "      "
              "target=(uintptr_t)(actual+32)-(uintptr_t)__readgsqword(0x30);"
              "memcpy(actual+144,&target,8);\n      _mm_setcsr(state); "
              "isa_probe((uintptr_t)actual);");
      Replace("      if(expected_state!=actual_state",
              "      memcpy(actual+144,expected+144,8);\n      "
              "if(expected_state!=actual_state");
    }
    return Text;
  }
  void check() {
    if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("f16c"))
      GTEST_SKIP() << "F16C-enabled x64 host required";
    compareFloating(kernel(), false);
  }
};
class F16CForms : public F16CFixture,
                  public testing::WithParamInterface<HalfForm> {};
TEST_P(F16CForms, NativeCompletionMatchesEveryRoute) {
  Form = GetParam();
  check();
}
INSTANTIATE_TEST_SUITE_P(VexForms, F16CForms, testing::ValuesIn(halfForms()),
                         [](const testing::TestParamInfo<HalfForm> &I) {
                           return halfFormName(I.param);
                         });
TEST_F(F16CFixture, DiscardedNumericalResultRetainsInvalid) {
  KeepResult = false;
  check();
}
TEST_F(F16CFixture, MemoryOperandsAllowUnalignedAccessWithExactStoreBounds) {
  SourceOffset = 65;
  DestinationOffset = 33;
  for (unsigned Width : {16U, 32U})
    for (bool Widen : {false, true}) {
      Form = {Widen, Width, true, 0};
      check();
    }
}
TEST_F(F16CFixture, RegisterSourcesCanAliasTheCompleteDestination) {
  SourceRegister = 0;
  for (unsigned Width : {16U, 32U})
    for (bool Widen : {false, true}) {
      Form = {Widen, Width, false, 0};
      check();
    }
}
TEST_F(F16CFixture, DiscardedNarrowResultRetainsPrecisionAndInvalid) {
  KeepResult = false;
  Form = {false, 32, false, 0};
  check();
}
TEST_F(F16CFixture, UnmaskedExceptionsPreserveOutputsAndLanePriority) {
#if defined(_WIN32) || defined(__linux__)
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("f16c"))
    GTEST_SKIP() << "Native F16C required";
  for (FaultMode = 0; FaultMode < 6; ++FaultMode) {
    Form = {FaultMode == 5, 32, FaultMode == 5, 0};
    SCOPED_TRACE(FaultMode);
    compareFloating(kernel(), false, true);
  }
#else
  GTEST_SKIP() << "Fault-context oracle requires Windows/Linux x64";
#endif
}
TEST_F(F16CFixture, CompleteReadFaultPrecedesInvalidEvenWithDiscardedResult) {
#if defined(_WIN32) || defined(__linux__)
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("f16c"))
    GTEST_SKIP() << "Native F16C required";
  FaultMode = 6;
  IndirectMemory = true;
  KeepResult = false;
  for (unsigned Width : {16U, 32U}) {
    Form = {true, Width, true, 0};
    compareFloating(kernel(), false, true);
  }
#else
  GTEST_SKIP() << "Fault-context oracle requires Windows/Linux x64";
#endif
}
TEST_F(F16CFixture, StoreFaultsPreserveOutputsAndAccessBounds) {
#if defined(_WIN32) || defined(__linux__)
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("f16c"))
    GTEST_SKIP() << "Native F16C required";
  IndirectMemory = true;
  for (FaultMode = 7; FaultMode < 12; ++FaultMode)
    for (unsigned Width : {16U, 32U}) {
      if (FaultMode == 9)
        continue;
      Form = {false, Width, true, 0};
      SCOPED_TRACE(testing::Message() << FaultMode << ':' << Width);
      compareFloating(kernel(), false, true);
    }
#else
  GTEST_SKIP() << "Fault-context oracle requires Windows/Linux x64";
#endif
}
TEST_F(F16CFixture, InexactCrossPageStoreRetainsFaultAtomicity) {
#if defined(_WIN32) || defined(__linux__)
  if (!nativeX64() || !llvm::sys::getHostCPUFeatures().lookup("f16c"))
    GTEST_SKIP() << "Native F16C required";
  FaultMode = 9;
  IndirectMemory = true;
  for (unsigned Width : {16U, 32U}) {
    Form = {false, Width, true, 0};
    compareFloating(kernel(), false, true);
  }
#else
  GTEST_SKIP() << "Fault-context oracle requires Windows/Linux x64";
#endif
}
TEST_F(F16CFixture, GSStoresUsePrivateAllocatedTargetsAndPreserveSegments) {
#if defined(_WIN32)
  GSMemory = true;
  IndirectMemory = true;
  for (unsigned Width : {16U, 32U}) {
    Form = {false, Width, true, 0};
    check();
  }
#else
  GTEST_SKIP() << "This native GS-base oracle uses the Windows x64 TEB";
#endif
}
} // namespace
