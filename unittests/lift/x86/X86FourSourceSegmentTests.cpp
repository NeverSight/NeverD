//===- X86FourSourceSegmentTests.cpp - i386 four-source segments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve CS/ES/SS offsets and the all-or-nothing masked m128 access.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86FourSource.h"
#include "X86NativeSegmentFixture.h"

#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/low/X86/X86FourSourceShape.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace {
constexpr std::array<uint8_t, 6> Opcodes{0x9a, 0xaa, 0x9b, 0xab, 0x52, 0x53};
constexpr std::array<unsigned, 6> Ids{X86_INS_V4FMADDPS, X86_INS_V4FNMADDPS,
                                      X86_INS_V4FMADDSS, X86_INS_V4FNMADDSS,
                                      X86_INS_VP4DPWSSD, X86_INS_VP4DPWSSDS};

TEST(X86FourSourceSegment, EverySegmentRetainsItsOffsetAcrossDecodeRoutes) {
  unsigned Checks = 0;
  for (unsigned Kind = 0; Kind < Opcodes.size(); ++Kind)
    for (uint8_t Prefix : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
      for (bool Narrow : {false, true})
        for (unsigned Base = 0; Base < 3; ++Base)
          for (bool Zero : {false, true})
            for (bool Strict : {false, true}) {
              std::vector<uint8_t> Bytes;
              if (Prefix)
                Bytes.push_back(Prefix);
              if (Narrow)
                Bytes.push_back(0x67);
              const auto C = *x86FourSourceControl(Kind);
              Bytes.insert(
                  Bytes.end(),
                  {0x62, 0xf2, 0x5f,
                   uint8_t(0x0a | (C.scalar() ? 0 : 0x40) | (Zero ? 0x80 : 0)),
                   Opcodes[Kind]});
              if (Narrow) {
                // BX+SI, BP+SI, BP+disp8(0).
                Bytes.push_back(Base == 0 ? 0x08 : Base == 1 ? 0x0a : 0x4e);
                if (Base == 2)
                  Bytes.push_back(0);
              } else {
                // EAX, ESP, EBP+disp8(0).
                Bytes.push_back(Base == 0 ? 0x08 : Base == 1 ? 0x0c : 0x4d);
                if (Base)
                  Bytes.push_back(Base == 1 ? 0x24 : 0);
              }
              const unsigned Expected =
                  Prefix == 0x26 || Prefix == 0x2e || Prefix == 0x36 ? Prefix
                  : !Prefix && Base                                  ? 0x36
                                                                     : 0;
              Decoder D;
              ASSERT_TRUE(D.init(Arch::X86));
              D.setStrict(Strict);
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(testing::PrintToString(Bytes));
                DecodedInsn I{};
                const auto Size =
                    Route == 0
                        ? D.decodeOne(Bytes.data(), Bytes.size(), Entry, I)
                    : Route == 1
                        ? D.decodeOneLight(Bytes.data(), Bytes.size(), Entry, I)
                        : D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry,
                                             I);
                ASSERT_EQ(Size, Bytes.size());
                ASSERT_EQ(I.Id, Ids[Kind]);
                std::vector<LowOp> Ops;
                ASSERT_NO_THROW(D.liftToLow(I, Ops));
                const auto It =
                    std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
                      return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                             Intrinsic(Op.Inputs[0].Offset) ==
                                 Intrinsic::X86FourSourceMemory;
                    });
                ASSERT_NE(It, Ops.end());
                const auto Actual = x86FourSourceControl(It->Inputs[5].Offset);
                ASSERT_TRUE(Actual);
                EXPECT_EQ(Actual->SegmentPrefix, Expected);
                EXPECT_EQ(Actual->Kind, C.Kind);
                EXPECT_EQ(Actual->Zero, Zero);
                EXPECT_FALSE(Actual->StackReference);
                EXPECT_EQ(It->Inputs[5].Size, Expected ? 2 : 1);
                EXPECT_TRUE(x86FourSourceShapeIsValid(
                    x86FourSourceLowShape(*It, Arch::X86)));
                ++Checks;
              }
            }
  EXPECT_EQ(Checks, 3024u);
  RecordProperty("decoder_lift_checks", Checks);
}

LowOp four(unsigned Kind, bool Zero, unsigned Prefix) {
  const auto C = *x86FourSourceControl(Kind);
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(900, C.bytes());
  Op.addInput(NdVar::cst(unsigned(Intrinsic::X86FourSourceMemory), 2));
  Op.addInput(NdVar::tmp(901, 8));
  Op.addInput(NdVar::tmp(902, C.bytes()));
  Op.addInput(NdVar::tmp(903, X86FourSourceCount * C.sourceBytes()));
  Op.addInput(NdVar::cst(0, C.maskBytes()));
  Op.addInput(NdVar::scalar(Kind | (Zero ? X86FourSourceZero : 0) |
                                (Prefix << X86FourSourceSegmentShift),
                            2));
  return Op;
}

TEST(X86FourSourceSegment, ExtendedControlsCannotAcquireAnotherModeOrSegment) {
  for (unsigned Kind = 0; Kind < Opcodes.size(); ++Kind)
    for (unsigned Prefix : {0x26u, 0x2eu, 0x36u}) {
      const auto Op = four(Kind, false, Prefix);
      EXPECT_TRUE(
          x86FourSourceShapeIsValid(x86FourSourceLowShape(Op, Arch::X86)));
      EXPECT_FALSE(
          x86FourSourceShapeIsValid(x86FourSourceLowShape(Op, Arch::X64)));
      for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
        auto Bad = Op;
        switch (Mutation) {
        case 0:
          Bad.Inputs[5].Size = 1;
          break;
        case 1:
          Bad.Inputs[5].Offset |= X86FourSourceStackReference;
          break;
        case 2:
          Bad.Inputs[5].Offset = Kind;
          break;
        case 3:
          Bad.Inputs[5].Offset = 0x6400 | Kind;
          break;
        case 4:
          Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
          break;
        case 5:
          Bad.Inputs[5].Provenance = ConstantAddressProvenance::Address;
          break;
        }
        EXPECT_FALSE(
            x86FourSourceShapeIsValid(x86FourSourceLowShape(Bad, Arch::X86)));
      }
    }
}

TEST(X86FourSourceSegment, ConcreteAccessNeedsDescriptorsOnlyForActiveLanes) {
  for (unsigned Kind = 0; Kind < Opcodes.size(); ++Kind)
    for (unsigned Prefix : {0x26u, 0x2eu, 0x36u})
      for (bool Zero : {false, true})
        for (unsigned Mask : {0u, 1u}) {
          auto Op = four(Kind, Zero, Prefix);
          const auto C = *x86FourSourceControl(Op.Inputs[5].Offset);
          Op.Inputs[4] = NdVar::cst(Mask, C.maskBytes());
          BinaryImage Image;
          Image.Arch = Arch::X86;
          Segment Data;
          Data.VA = 0x4000;
          Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
          Data.Data.resize(64, 0x31);
          Data.Size = Data.FileSz = 64;
          Image.Segments.push_back(std::move(Data));
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setLoadCollect(true);
          E.setRegisterBytes(900, std::vector<uint8_t>(C.bytes(), 0xa5));
          if (!Zero || C.scalar() || Mask)
            E.setRegisterBytes(902, std::vector<uint8_t>(C.bytes(), 0x17));
          if (Mask) {
            E.setRegister(901, 0x4000);
            E.setRegisterBytes(
                903,
                std::vector<uint8_t>(X86FourSourceCount * C.sourceBytes(), 0));
          }
          EXPECT_EQ(E.step(Op), Mask == 0);
          EXPECT_TRUE(E.getLoadRecords().empty());
          const auto Result = E.getRegisterBytes(900);
          ASSERT_TRUE(Result);
          auto Expected = std::vector<uint8_t>(C.bytes(), Mask   ? 0xa5
                                                          : Zero ? 0
                                                                 : 0x17);
          if (!Mask && Zero && C.scalar())
            std::fill(Expected.begin() + 4, Expected.end(), 0x17);
          EXPECT_EQ(*Result, Expected);
          EXPECT_EQ(E.getMXCSR(), 0x1f80u);
        }
}

using X86FourSourceSegmentSource = X86NativeSegmentFixture;

TEST_F(X86FourSourceSegmentSource, AllSixKindsKeepSegmentsAndRawOffsets) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (uint8_t Prefix : {0x26, 0x2e, 0x36})
      for (unsigned Kind = 0; Kind < Opcodes.size(); ++Kind)
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << unsigned(Format) << '/' << unsigned(Prefix) << '/'
                       << Kind << '/' << NoOpt);
          const auto C = *x86FourSourceControl(Kind);
          const bool Parameter = Kind & 1;
          const bool RegisterAddress = Parameter || NoOpt;
          std::vector<uint8_t> Bytes{0xba, 5, 0, 0, 0, 0xc5, 0xf8, 0x92, 0xd2};
          // VEX.128 zeroes the complete destination and source registers.
          for (unsigned Reg : {1u, 4u, 5u, 6u, 7u})
            Bytes.insert(Bytes.end(), {0xc5, uint8_t(0x81 | ((~Reg & 15) << 3)),
                                       0xef, uint8_t(0xc0 | (Reg << 3) | Reg)});
          if (Parameter)
            Bytes.insert(Bytes.end(), {0x8b, 0x44, 0x24, 4});
          else if (RegisterAddress)
            Bytes.insert(Bytes.end(), {0xb8, 0, 0x40, 0, 0});
          Bytes.insert(
              Bytes.end(),
              {Prefix, 0x62, 0xf2, 0x5f,
               uint8_t(0x0a | (C.scalar() ? 0 : 0x40) | (Parameter ? 0x80 : 0)),
               Opcodes[Kind], uint8_t(RegisterAddress ? 0x08 : 0x0d)});
          if (!RegisterAddress)
            Bytes.insert(Bytes.end(), {0, 0x40, 0, 0});
          if (RegisterAddress && !Parameter)
            Bytes.insert(Bytes.end(), {0x8b, 0x00, 0xc3});
          else
            Bytes.insert(Bytes.end(), {0xa1, 0, 0x40, 0, 0, 0xc3});
          auto Image = segmentImage(Bytes, Format, "four_segment");
          ASSERT_NO_FATAL_FAILURE(checkPipelines(
              Image, NoOpt, Prefix, Ids[Kind], !Parameter,
              [](Arch A,
                 const llvm::CallInst &Call) -> std::optional<unsigned> {
                if (const auto P = x86FourSourceCallPlan(A, Call))
                  return P->Prefix;
                return std::nullopt;
              }));
        }
  EXPECT_EQ(Objects, 540u);
  RecordProperty("native_objects", Objects);
}
} // namespace
