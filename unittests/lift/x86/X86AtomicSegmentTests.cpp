//===- X86AtomicSegmentTests.cpp - i386 RAO segment ownership -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve CS/ES/SS offsets without acquiring ordinary image-pointer roles.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86APXAtomic.h"
#include "X86NativeSegmentFixture.h"

#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace {
constexpr std::array<Intrinsic, 4> Ids{
    Intrinsic::X86LegacyRaoAdd, Intrinsic::X86LegacyRaoAnd,
    Intrinsic::X86LegacyRaoOr, Intrinsic::X86LegacyRaoXor};
constexpr std::array<unsigned, 4> InsnIds{X86_INS_AADD, X86_INS_AAND,
                                          X86_INS_AOR, X86_INS_AXOR};
constexpr std::array<uint8_t, 4> Mandatory{0, 0x66, 0xf2, 0xf3};

std::vector<uint8_t> encoding(unsigned Kind, uint8_t Prefix, bool Narrow,
                              unsigned Base) {
  std::vector<uint8_t> Bytes;
  if (Prefix)
    Bytes.push_back(Prefix);
  if (Narrow)
    Bytes.push_back(0x67);
  if (Mandatory[Kind])
    Bytes.push_back(Mandatory[Kind]);
  Bytes.insert(Bytes.end(), {0x0f, 0x38, 0xfc});
  // The source is EDX. Addresses are EAX/ESP/EBP or BX+SI/BP+SI/BP.
  if (Narrow) {
    Bytes.push_back(Base == 0 ? 0x10 : Base == 1 ? 0x12 : 0x56);
    if (Base == 2)
      Bytes.push_back(0);
  } else {
    Bytes.push_back(Base == 0 ? 0x10 : Base == 1 ? 0x14 : 0x55);
    if (Base)
      Bytes.push_back(Base == 1 ? 0x24 : 0);
  }
  return Bytes;
}

TEST(X86AtomicSegment, EverySegmentRetainsItsOffsetAcrossDecodeRoutes) {
  unsigned Checks = 0;
  for (unsigned Kind = 0; Kind < Ids.size(); ++Kind)
    for (uint8_t Prefix : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
      for (bool Narrow : {false, true})
        for (unsigned Base = 0; Base < 3; ++Base)
          for (bool Strict : {false, true}) {
            const auto Bytes = encoding(Kind, Prefix, Narrow, Base);
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
                  Route == 0 ? D.decodeOne(Bytes.data(), Bytes.size(), Entry, I)
                  : Route == 1
                      ? D.decodeOneLight(Bytes.data(), Bytes.size(), Entry, I)
                      : D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry,
                                           I);
              ASSERT_EQ(Size, Bytes.size());
              ASSERT_EQ(I.Id, InsnIds[Kind]);
              std::vector<LowOp> Ops;
              ASSERT_NO_THROW(D.liftToLow(I, Ops));
              const auto It =
                  std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
                    return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                           isX86AtomicIntrinsic(Intrinsic(Op.Inputs[0].Offset));
                  });
              ASSERT_NE(It, Ops.end());
              EXPECT_EQ(Intrinsic(It->Inputs[0].Offset), Ids[Kind]);
              const auto S = apxAtomicLowShape(*It, Arch::X86);
              EXPECT_TRUE(intrinsicApxAtomicShapeIsValid(Ids[Kind], S));
              EXPECT_EQ(S.segmentPrefix(), Expected);
              EXPECT_FALSE(S.stackReference());
              EXPECT_EQ(It->NumInputs, Expected ? 4 : 3);
              if (Expected)
                EXPECT_EQ(It->Inputs[3].Size, 1);
              ++Checks;
            }
          }
  EXPECT_EQ(Checks, 1008u);
  RecordProperty("decoder_lift_checks", Checks);
}

LowOp atomic(unsigned Kind, unsigned Prefix) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
  Op.addInput(NdVar::cst(unsigned(Ids[Kind]), 2));
  Op.addInput(NdVar::scalar(0x4000, 8));
  Op.addInput(NdVar::scalar(0x87654321, 4));
  Op.addInput(NdVar::scalar(Prefix, 1));
  return Op;
}

TEST(X86AtomicSegment, ControlsRequireI386LegacyRaoAndOneSegment) {
  unsigned Refusals = 0;
  for (unsigned Kind = 0; Kind < Ids.size(); ++Kind)
    for (unsigned Prefix : {0x26u, 0x2eu, 0x36u}) {
      const auto Op = atomic(Kind, Prefix);
      EXPECT_TRUE(intrinsicApxAtomicShapeIsValid(
          Ids[Kind], apxAtomicLowShape(Op, Arch::X86)));
      EXPECT_TRUE(intrinsicApxAtomicShapeIsValid(
          Ids[Kind], apxAtomicLowShape(Op, Arch::Unknown)));
      for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
        auto Bad = Op;
        Arch A = Arch::X86;
        switch (Mutation) {
        case 0:
          A = Arch::X64;
          break;
        case 1:
          Bad.Inputs[3].Size = 2;
          break;
        case 2:
          Bad.Inputs[3].Offset = 0;
          break;
        case 3:
          Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
          break;
        case 4:
          Bad.Inputs[3].Offset = 0x64;
          break;
        case 5:
          Bad.Inputs[3].Offset |= X86AtomicStackReference;
          break;
        case 6:
          Bad.Inputs[3].Provenance = ConstantAddressProvenance::Address;
          break;
        case 7:
          Bad.Inputs[2].Size = 8;
          break;
        case 8:
          Bad.Inputs[0].Offset = unsigned(Intrinsic::ApxRaoAdd);
          break;
        case 9:
          Bad.Inputs[3].Offset |= 0x100;
          break;
        }
        EXPECT_FALSE(intrinsicApxAtomicShapeIsValid(
            Intrinsic(Bad.Inputs[0].Offset), apxAtomicLowShape(Bad, A)));
        ++Refusals;
      }
    }
  EXPECT_EQ(Refusals, 120u);
  RecordProperty("malformed_contract_refusals", Refusals);
}

TEST(X86AtomicSegment,
     ConcreteAccessWithoutDescriptorsPreservesMemoryAndSource) {
  for (unsigned Kind = 0; Kind < Ids.size(); ++Kind)
    for (unsigned Prefix : {0x26u, 0x2eu, 0x36u}) {
      auto Op = atomic(Kind, Prefix);
      Op.Inputs[2] = NdVar::reg(x86reg::RDX, 4);
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Segment Data;
      Data.VA = 0x4000;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(4, 0x31);
      Data.Size = Data.FileSz = 4;
      Image.Segments.push_back(std::move(Data));
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setLoadCollect(true);
      E.setRegister(x86reg::RDX, 0x87654321);
      EXPECT_FALSE(E.step(Op));
      EXPECT_TRUE(E.getLoadRecords().empty());
      EXPECT_EQ(E.getRegister(x86reg::RDX), 0x87654321u);
      LowOp Load;
      Load.Opcode = NdOp::LOAD;
      Load.Output = NdVar::tmp(1000, 4);
      Load.addInput(NdVar::scalar(0x4000, 8));
      ASSERT_TRUE(E.step(Load));
      EXPECT_EQ(E.getRegister(1000), 0x31313131u);
    }
}

TEST(X86AtomicSegment, AddressWrappingPrecedesSegmentSelection) {
  unsigned Checks = 0;
  for (unsigned Kind = 0; Kind < Ids.size(); ++Kind)
    for (uint8_t Prefix : {0x26, 0x2e, 0x36})
      for (bool Narrow : {false, true}) {
        auto Bytes = encoding(Kind, Prefix, Narrow, 0);
        if (!Narrow) {
          Bytes.back() |= 0x40;
          Bytes.push_back(0x30);
        }
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X86));
        DecodedInsn I{};
        ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I),
                  Bytes.size());
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        BinaryImage Image;
        Image.Arch = Arch::X86;
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(x86reg::RAX, UINT64_C(0x1fffffff0));
        E.setRegister(x86reg::RBX, 0x1234fff0);
        E.setRegister(x86reg::RSI, 0xabcd0030);
        E.setRegister(x86reg::RDX, 0x87654321);
        bool Found = false;
        for (const auto &Op : Ops) {
          if (Op.Opcode == NdOp::INTRINSIC) {
            ASSERT_EQ(Intrinsic(Op.Inputs[0].Offset), Ids[Kind]);
            const auto &Address = Op.Inputs[1];
            const auto Offset = Address.isConst()
                                    ? std::optional<uint64_t>(Address.Offset)
                                    : E.getRegister(Address.Offset);
            EXPECT_EQ(Offset, 0x20u);
            Found = true;
            ++Checks;
            break;
          }
          ASSERT_TRUE(E.step(Op));
        }
        ASSERT_TRUE(Found);
      }
  EXPECT_EQ(Checks, 24u);
  RecordProperty("wrapped_offsets", Checks);
}

using X86AtomicSegmentSource = X86NativeSegmentFixture;

TEST_F(X86AtomicSegmentSource, AllFourKindsKeepSegmentsAndRawOffsets) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (uint8_t Prefix : {0x26, 0x2e, 0x36})
      for (unsigned Kind = 0; Kind < Ids.size(); ++Kind)
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << unsigned(Format) << '/' << unsigned(Prefix) << '/'
                       << Kind << '/' << NoOpt);
          const unsigned Mode = (Kind + unsigned(NoOpt)) % 3;
          const bool Parameter = Mode == 2;
          const bool RegisterAddress = Mode != 0;
          std::vector<uint8_t> Bytes{0xba, 0x21, 0x43, 0x65, 0x87};
          if (Parameter)
            Bytes.insert(Bytes.end(), {0x8b, 0x44, 0x24, 4});
          else if (RegisterAddress)
            Bytes.insert(Bytes.end(), {0xb8, 0, 0x40, 0, 0});
          Bytes.push_back(Prefix);
          if (Mandatory[Kind])
            Bytes.push_back(Mandatory[Kind]);
          Bytes.insert(Bytes.end(), {0x0f, 0x38, 0xfc,
                                     uint8_t(RegisterAddress ? 0x10 : 0x15)});
          if (!RegisterAddress)
            Bytes.insert(Bytes.end(), {0, 0x40, 0, 0});
          if (RegisterAddress && !Parameter)
            Bytes.insert(Bytes.end(), {0x8b, 0x00, 0xc3});
          else
            Bytes.insert(Bytes.end(), {0xa1, 0, 0x40, 0, 0, 0xc3});
          auto Image = segmentImage(Bytes, Format, "atomic_segment");
          ASSERT_NO_FATAL_FAILURE(checkPipelines(
              Image, NoOpt, Prefix, InsnIds[Kind], !Parameter,
              [](Arch A,
                 const llvm::CallInst &Call) -> std::optional<unsigned> {
                if (const auto P = x86APXAtomicCallPlan(A, Call))
                  return P->SegmentPrefix;
                return std::nullopt;
              }));
        }
  EXPECT_EQ(Objects, 360u);
  RecordProperty("native_objects", Objects);
}
} // namespace
