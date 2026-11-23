//===- MIPSControlFlowTests.cpp - Delayed MIPS control regression --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check execution order, byte ownership, annulment and independently entered
/// slots using hand-authored machine words with known architectural outcomes.

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/MIPS/SourceRecordABI.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

using namespace neverd;

namespace {

std::vector<uint8_t> encode(llvm::ArrayRef<uint32_t> Words, Arch Target) {
  std::vector<uint8_t> Bytes;
  for (uint32_t Word : Words)
    for (unsigned I = 0; I < 4; ++I)
      Bytes.push_back(
          uint8_t(Word >> (8 * (archLittleEndian(Target) ? I : 3 - I))));
  return Bytes;
}

BinaryImage image(llvm::ArrayRef<uint32_t> Words, Arch Target) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = isMIPS64(Target) && !isMIPSN32(Target) ? Bitness::Bits64
                                                      : Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Base = Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Base;
  Text.Data = encode(Words, Target);
  Text.Size = Text.FileSz = Text.Data.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(Text));
  return Image;
}

LowFunc build(const BinaryImage &Image) {
  Decoder Decode;
  EXPECT_TRUE(Decode.init(Image));
  CFGBuilder Builder;
  auto Func = Builder.build(Image, Decode, Image.Entry, "delayed");
  auto Error = validateLowInstructionBoundaries(
      Func, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  return Func;
}

class MIPSControl : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPSControl, DelayedUnitOwnsBothInstructionsAndActiveOperandDetails) {
  const auto Code = encode({0x0c000408, 0x8ca2fff4, 0}, GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  DecodedExecutionUnit Unit;
  ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), Code.size(), 0x1000, Unit),
            8);
  ASSERT_TRUE(Unit.HasDelaySlot);
  DecodedInsn Other{};
  ASSERT_EQ(Decode.decodeOne(Code.data() + 8, 4, 0x1008, Other), 4);

  for (const auto *Saved : {&Unit.Head, &Unit.Slot}) {
    DecodedInsn Physical{};
    ASSERT_EQ(Decode.decodeOne(Code.data() + (Saved->Addr - 0x1000), 4,
                               Saved->Addr, Physical),
              4);
    EXPECT_NE(Saved->Raw, Physical.Raw);
    EXPECT_EQ(Saved->Id, Physical.Id);
    ASSERT_NE(Saved->Raw->detail, nullptr);
    ASSERT_NE(Physical.Raw->detail, nullptr);
    const auto &A = *Saved->Raw->detail, &B = *Physical.Raw->detail;
    EXPECT_NE(&A, &B);
    EXPECT_EQ(llvm::ArrayRef(A.regs_read, A.regs_read_count),
              llvm::ArrayRef(B.regs_read, B.regs_read_count));
    EXPECT_EQ(llvm::ArrayRef(A.regs_write, A.regs_write_count),
              llvm::ArrayRef(B.regs_write, B.regs_write_count));
    EXPECT_EQ(llvm::ArrayRef(A.groups, A.groups_count),
              llvm::ArrayRef(B.groups, B.groups_count));
    EXPECT_EQ(A.writeback, B.writeback);
    ASSERT_EQ(A.mips.op_count, B.mips.op_count);
    for (unsigned I = 0; I < B.mips.op_count; ++I) {
      const auto &X = A.mips.operands[I], &Y = B.mips.operands[I];
      ASSERT_EQ(X.type, Y.type);
      EXPECT_EQ(X.access, Y.access);
      switch (X.type) {
      case MIPS_OP_REG:
        EXPECT_EQ(X.reg, Y.reg);
        break;
      case MIPS_OP_IMM:
        EXPECT_EQ(X.imm, Y.imm);
        break;
      case MIPS_OP_MEM:
        EXPECT_EQ(X.mem.base, Y.mem.base);
        EXPECT_EQ(X.mem.disp, Y.mem.disp);
        break;
      default:
        FAIL() << "unexpected operand in JAL/LW fixture";
      }
    }
  }
}

TEST_P(MIPSControl,
       UntrustedDiscoveryKeepsOperandDetailAcrossRepeatedRequests) {
  // The call target has no symbol: verification must lift its JR and slot.
  // Reusing a decoder exercises the former detail-off scan transition.
  auto Image = image({0x0c000404, 0, 0x03e00008, 0, 0x00851021, 0x03e00008, 0},
                     GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  for (unsigned I = 0; I != 16; ++I) {
    Image.VerifiedFunctionEntries.clear();
    FuncDetector Detector;
    const auto Functions = Detector.detect(Image, Decode);
    EXPECT_EQ(Image.VerifiedFunctionEntries.count(0x1010), 1u);
    EXPECT_TRUE(Decode.detailEnabled());
    EXPECT_TRUE(std::any_of(Functions.begin(), Functions.end(),
                            [](const auto &F) { return F.first == 0x1010; }));
  }
}

TEST_P(MIPSControl, DetailOffSnapshotsNeverReadStaleOperandCounts) {
  const auto Code = encode({0x03e00008, 0x24820001}, GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  DecodedExecutionUnit Unit;
  ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), Code.size(), 0x1000, Unit),
            8);
  std::vector<LowOp> Expected;
  const auto ExpectedSlot = Decode.liftUnitToLow(Unit, Expected);
  for (bool Detail : {false, true, false, true}) {
    // Poison the retained allocation before disabling detail. The next scan
    // must not use this count to copy beyond the actual MIPS operand bank.
    DecodedInsn Physical;
    ASSERT_EQ(
        Decode.decodeOneForLift(Code.data(), Code.size(), 0x1000, Physical), 4);
    ASSERT_NE(Physical.Raw->detail, nullptr);
    Physical.Raw->detail->mips.op_count = 255;
    Decode.setDetail(Detail);
    ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), Code.size(), 0x1000, Unit),
              8);
    ASSERT_TRUE(Unit.HasDelaySlot);
    EXPECT_EQ(Unit.Head.Raw->detail != nullptr, Detail);
    EXPECT_EQ(Unit.Slot.Raw->detail != nullptr, Detail);
    std::vector<LowOp> Actual;
    EXPECT_EQ(Decode.liftUnitToLow(Unit, Actual), ExpectedSlot);
    EXPECT_EQ(Actual, Expected);
    Decode.setDetail(true);
  }
}

TEST_P(MIPSControl, SourceReturnRequiresTheWholeIncomingLink) {
  const bool Wide = isMIPS64(GetParam());
  const auto Check = [&](llvm::ArrayRef<uint32_t> Words, bool Preserved) {
    auto Image = image(Words, GetParam());
    Image.MIPS.emplace();
    Image.MIPS->HasDSP = true;
    Image.MIPS->ABI = Wide ? MIPSABI::N64 : MIPSABI::O32;
    const auto Low = build(Image);
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    for (bool WithImage : {false, true}) {
      SCOPED_TRACE(WithImage);
      LowToMedConverter Convert;
      if (WithImage)
        Convert.setBinaryImage(&Image);
      if (Preserved)
        EXPECT_NO_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF));
      else
        EXPECT_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF),
                     LowToMedConversionError);
    }
  };
  constexpr uint32_t ClearRA = 0x0000f825, Return = 0x03e00008;
  Check({Return, 0}, true);
  Check({ClearRA, Return, 0}, false);
  // sw/lw ra,-4(sp) preserves an o32 link and truncates an n64 link.
  Check({0xafbffffc, ClearRA, 0x8fbffffc, Return, 0}, !Wide);
  if (Wide)
    Check({0xffbffff8, ClearRA, 0xdfbffff8, Return, 0}, true);
}

TEST(MIPSRecordSourceEntry, DelaySlotResultKeepsTheCapturedJumpTarget) {
  for (const auto Target :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    auto Record = NdType::makeStruct({NdType::makeFloat(8)});
    Record->Passing = NdRecordPassing::ByValue;
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    Signature.ReturnType = Record;
    ASSERT_TRUE(
        mips::assignRecordSourceABI(Signature, Target, BinaryFormat::ELF));
    for (const bool ClobberLink : {false, true}) {
      // dmtc1 a0,f0 produces the declared result after jr has captured ra.
      const uint32_t Before = ClobberLink ? 0x0000f825 : 0;
      auto Image = image({Before, 0x03e00008, 0x44a40000}, Target);
      Image.MIPS.emplace();
      Image.MIPS->FPABI = 1;
      Image.MIPS->ABI = isMIPSN32(Target) ? MIPSABI::N32 : MIPSABI::N64;
      const auto Low = build(Image);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      std::map<va_t, SourceFunctionTypeHint> Entries{{Image.Entry, Signature}};
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setSourceCallHintsEnabled(true);
      Convert.setSourceEntryTypeHints(&Entries);
      if (ClobberLink)
        EXPECT_THROW(Convert.convert(Low, Target, BinaryFormat::ELF),
                     LowToMedConversionError);
      else
        EXPECT_NO_THROW(Convert.convert(Low, Target, BinaryFormat::ELF));
    }
  }
}

TEST(MIPSRecordSourceEntry, BothMachineResultWordsSurviveTheReturnDelaySlot) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    for (bool CorruptLink : {false, true}) {
      auto Image = image({CorruptLink ? 0x0080f825u : 0u, 0x64020012u,
                          0x03e00008u, 0x64030034u},
                         A);
      Image.MIPS.emplace();
      Image.MIPS->ABI = isMIPSN32(A) ? MIPSABI::N32 : MIPSABI::N64;
      const auto Low = build(Image);
      const std::array<MedReturnReg, 2> Words{
          {{mipsreg::V0, 8, false}, {mipsreg::V1, 8, false}}};
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setMIPSRecordResult(Words);
      if (CorruptLink) {
        EXPECT_THROW(Convert.convert(Low, A, BinaryFormat::ELF),
                     LowToMedConversionError);
        continue;
      }
      auto Med = Convert.convert(Low, A, BinaryFormat::ELF);
      ASSERT_TRUE(Med.ExplicitMIPSRecordResult);
      bool Returned = false;
      for (const auto &Block : Med.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::RETURN) {
            Returned = true;
            ASSERT_EQ(Op.NumInputs, 2);
            EXPECT_EQ(Op.Inputs[0].Size, 8);
            EXPECT_EQ(Op.Inputs[1].Size, 8);
          }
      EXPECT_TRUE(Returned);
    }
  }
}

TEST(MIPSRecordSourceEntry, BoundedCopyMustPreserveThePrivateReturnSlot) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    for (unsigned Variant : {0u, 1u, 2u, 3u}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(Variant);
      // Copy sixteen incoming bytes into a private record, pass its double
      // fields in f12/f13, then return through the saved incoming link.
      std::vector<uint32_t> Words{
          0x67bdffd0, 0xffbf0028, 0x67ac0008, 0x67ad0018, 0x908e0000,
          0x64840001, 0xa18e0000, 0x658c0001, 0x158dfffb, 0,
          0xd7ac0008, 0xd7ad0010, 0x0c000800, 0x6406002a, 0xdfbf0028,
          0x03e00008, 0x67bd0030};
      if (Variant == 1)
        Words[3] = 0x67ad0030; // Loop reaches the saved link.
      if (Variant == 2)
        Words[13] = 0x03a03025; // The call receives the private SP.
      if (Variant == 3)
        Words[3] = 0x00a06825; // Incoming a1 gives an unknown iteration count.
      auto Image = image(Words, A);
      Image.MIPS.emplace();
      Image.MIPS->ABI = isMIPSN32(A) ? MIPSABI::N32 : MIPSABI::N64;
      Image.MIPS->FPABI = 1;
      auto Pair =
          NdType::makeStruct({NdType::makeFloat(8), NdType::makeFloat(8)});
      Pair->Passing = NdRecordPassing::ByValue;
      SourceFunctionTypeHint Sig;
      Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
      Sig.ReturnType = Pair;
      Sig.Parameters.push_back({"value", Pair});
      Sig.Parameters.push_back({"tail", NdType::makeInt(8, false)});
      ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
      std::map<va_t, SourceFunctionTypeHint> Targets{{0x2000, Sig}};
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setSourceCallHintsEnabled(true);
      Convert.setSourceCalleeTypeHints(&Targets);
      const auto Low = build(Image);
      if (Variant == 0)
        EXPECT_NO_THROW(Convert.convert(Low, A, BinaryFormat::ELF));
      else
        EXPECT_THROW(Convert.convert(Low, A, BinaryFormat::ELF),
                     LowToMedConversionError);
    }
  }
}

TEST(MIPSRecordSourceEntry, SinglePrecisionRecordStoresOnlyDefinedResultBytes) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    for (bool ReadFullFPR : {false, true}) {
      SCOPED_TRACE(::testing::Message() << unsigned(A) << '/' << ReadFullFPR);
      // Form the eight-byte integer argument by copying input bytes. After
      // the call, each single-precision result defines four bytes in its FPR.
      std::vector<uint32_t> Words{
          0x67bdffd0, 0xffb00020,
          0xffbf0028, 0x00a08025,
          0x67ac0008, 0x67ad0010,
          0x908e0000, 0x64840001,
          0xa18e0000, 0x658c0001,
          0x158dfffb, 0,
          0xdfa40008, 0x0c000800,
          0x6405002a, ReadFullFPR ? 0xf6000000u : 0xe6000000u,
          0xe6020004, 0xdfbf0028,
          0xdfb00020, 0x03e00008,
          0x67bd0030};
      auto Image = image(Words, A);
      Image.MIPS.emplace();
      Image.MIPS->ABI = isMIPSN32(A) ? MIPSABI::N32 : MIPSABI::N64;
      Image.MIPS->FPABI = 1;
      auto Float = NdType::makeFloat(4);
      auto Pair = NdType::makeStruct({Float, Float});
      Pair->Passing = NdRecordPassing::ByValue;
      SourceFunctionTypeHint Sig;
      Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
      Sig.ReturnType = Pair;
      Sig.Parameters = {{"value", Pair}, {"tail", NdType::makeInt(8, false)}};
      ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
      const std::map<va_t, SourceFunctionTypeHint> Targets{{0x2000, Sig}};
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setSourceCallHintsEnabled(true);
      Convert.setSourceCalleeTypeHints(&Targets);
      auto Low = build(Image);
      if (ReadFullFPR)
        EXPECT_THROW(Convert.convert(Low, A, BinaryFormat::ELF),
                     LowToMedConversionError);
      else
        EXPECT_NO_THROW(Convert.convert(Low, A, BinaryFormat::ELF));
    }
  }
}

TEST(MIPSRecordSourceEntry, SignExtendedRecordWordsUseOnlyTheDefinedHalf) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    const bool Little = archLittleEndian(A);
    for (bool UseOtherHalf : {false, true}) {
      SCOPED_TRACE(::testing::Message() << unsigned(A) << '/' << UseOtherHalf);
      // Load a four-byte input through an unaligned word pair. The call
      // returns its record in one half of v0. Store the selected word and
      // its sign extension through the caller's external pointer.
      std::vector<uint32_t> Words{
          0x67bdffe0,
          0xffbf0018,
          0xffb00010,
          0x00a08025,
          0x00001825,
          0x88830000u + (Little ? 3u : 0u),   // lwl v1,off(a0)
          0x98830000u + (Little ? 0u : 3u),   // lwr v1,off(a0)
          Little ? 0x00032000u : 0x0003203cu, // sll/dsll32 a0,v1,0
          0x0c000800,
          0x6405002a};
      if (Little != UseOtherHalf)
        Words.push_back(0x0002103c); // dsll32 v0,v0,0
      Words.insert(Words.end(),
                   {0x0002103f, 0xfe020000, // dsra32 v0,v0,0; sd v0,0(s0)
                    0xdfb00010, 0xdfbf0018, 0x03e00008, 0x67bd0020});
      auto Image = image(Words, A);
      Image.MIPS.emplace();
      Image.MIPS->ABI = isMIPSN32(A) ? MIPSABI::N32 : MIPSABI::N64;
      auto Word = NdType::makeStruct({NdType::makeInt(4, false)});
      Word->Passing = NdRecordPassing::ByValue;
      SourceFunctionTypeHint Sig;
      Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
      Sig.ReturnType = Word;
      Sig.Parameters = {{"value", Word}, {"tail", NdType::makeInt(8, false)}};
      ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
      const std::map<va_t, SourceFunctionTypeHint> Targets{{0x2000, Sig}};
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setSourceCallHintsEnabled(true);
      Convert.setSourceCalleeTypeHints(&Targets);
      const auto Low = build(Image);
      if (UseOtherHalf)
        EXPECT_THROW(Convert.convert(Low, A, BinaryFormat::ELF),
                     LowToMedConversionError);
      else
        EXPECT_NO_THROW(Convert.convert(Low, A, BinaryFormat::ELF));
    }
  }
}

TEST(MIPSRecordSourceEntry, MergeStoresPreserveOnlyUntouchedSavedLinkBytes) {
  enum class Destination { External, EscapingPointer, Private, SavedLink };
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    for (unsigned Width : {4u, 8u}) {
      for (bool Left : {false, true}) {
        for (auto D : {Destination::External, Destination::EscapingPointer,
                       Destination::Private, Destination::SavedLink}) {
          SCOPED_TRACE(::testing::Message()
                       << unsigned(A) << '/' << Width << '/' << Left << '/'
                       << unsigned(D));
          const bool External =
              D == Destination::External || D == Destination::EscapingPointer;
          const unsigned Base = External ? 16 : 29; // s0 or sp
          const unsigned Data = D == Destination::EscapingPointer ? 29 : 2;
          // With an aligned frame these offsets select exactly one byte.
          // Keep the s0 spill at 8(sp) and the saved link at 24(sp).
          const unsigned Offset = (D == Destination::SavedLink ? 24 : 16) +
                                  (Left != archLittleEndian(A) ? Width - 1 : 0);
          const uint32_t Opcode =
              Width == 4 ? (Left ? 0x2a : 0x2e) : (Left ? 0x2c : 0x2d);
          const uint32_t Store =
              (Opcode << 26) | (Base << 21) | (Data << 16) | Offset;
          auto Image = image({0x67bdffe0, 0xffbf0018, 0xffb00008, 0x00808025,
                              0x0c000800, 0x6404002a, Store, 0xdfb00008,
                              0xdfbf0018, 0x03e00008, 0x67bd0020},
                             A);
          Image.MIPS.emplace();
          Image.MIPS->ABI = isMIPSN32(A) ? MIPSABI::N32 : MIPSABI::N64;
          auto Record = NdType::makeStruct({NdType::makeInt(8, false)});
          Record->Passing = NdRecordPassing::ByValue;
          SourceFunctionTypeHint Sig;
          Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
          Sig.ReturnType = Record;
          Sig.Parameters = {{"value", Record}};
          ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
          const std::map<va_t, SourceFunctionTypeHint> Targets{{0x2000, Sig}};
          LowToMedConverter Convert;
          Convert.setBinaryImage(&Image);
          Convert.setSourceCallHintsEnabled(true);
          Convert.setSourceCalleeTypeHints(&Targets);
          const auto Low = build(Image);
          if (D == Destination::External || D == Destination::Private)
            EXPECT_NO_THROW(Convert.convert(Low, A, BinaryFormat::ELF));
          else
            EXPECT_THROW(Convert.convert(Low, A, BinaryFormat::ELF),
                         LowToMedConversionError);
        }
      }
    }
  }
}

TEST(MIPSRecordSourceEntry, MergeLoadsPreservePrivateLinkForExternalData) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                 Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    const bool Wide = isMIPS64(A);
    const unsigned Width = Wide ? 8 : 4;
    const unsigned Last = Width - 1;
    auto Result = NdType::makeStruct(
        {NdType::makeArray(NdType::makeInt(1, false), 17, 17)});
    Result->Passing = NdRecordPassing::ByValue;
    enum class Input { CallerBytes, RetainedStackPointer, PrivateSpill };
    for (auto Source : {Input::CallerBytes, Input::RetainedStackPointer,
                        Input::PrivateSpill}) {
      SCOPED_TRACE(::testing::Message() << unsigned(A) << '/' << int(Source));
      // The argument must not expose the frame through either the retained
      // register bytes or the loaded bytes. The result starts at sp+16, the
      // private pointer spill at sp+48, and the saved RA at sp+56.
      std::vector<uint32_t> Words{
          Wide ? 0x67bdffc0u : 0x27bdffc0u, Wide ? 0xffbf0038u : 0xafbf0038u,
          Wide ? 0xffb00028u : 0xafb00028u, 0x00808025u, // save s0; move s0,a0
          Source == Input::RetainedStackPointer ? 0x03a02825u : 0x00002825u};
      if (Source == Input::PrivateSpill)
        Words.push_back(Wide ? 0xffbd0030u : 0xafbd0030u); // sd/sw sp,48(sp)
      const unsigned Base = Source == Input::PrivateSpill ? 29 : 4;
      const unsigned Offset = Source == Input::PrivateSpill ? 48 : 0;
      Words.push_back((Wide ? 0x68050000u : 0x88050000u) | (Base << 21) |
                      (Offset + (archLittleEndian(A) ? Last : 0)));
      if (Source == Input::CallerBytes)
        Words.push_back((Wide ? 0x6c850000u : 0x98850000u) |
                        (archLittleEndian(A) ? 0 : Last));
      Words.insert(Words.end(),
                   {0x0c000800u, Wide ? 0x67a40010u : 0x27a40010u});
      // Copy the returned object as a Clang caller does. Rechecking every
      // byte after the hidden result pointer was seen can exhaust the budget.
      for (uint32_t I = 0; I < Result->Size; ++I) {
        Words.push_back(0x93a30010u + I); // lbu v1,16+i(sp)
        Words.push_back(0xa2030000u + I); // sb v1,i(s0)
      }
      Words.insert(Words.end(), {Wide ? 0xdfb00028u : 0x8fb00028u,
                                 Wide ? 0xdfbf0038u : 0x8fbf0038u, 0x03e00008u,
                                 Wide ? 0x67bd0040u : 0x27bd0040u});
      auto Image = image(Words, A);
      Image.MIPS.emplace();
      Image.MIPS->ABI = isMIPSN32(A) ? MIPSABI::N32
                        : Wide       ? MIPSABI::N64
                                     : MIPSABI::O32;
      SourceFunctionTypeHint Sig;
      Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
      Sig.ReturnType = Result;
      Sig.Parameters.push_back({"value", NdType::makeInt(Width, false)});
      ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
      const std::map<va_t, SourceFunctionTypeHint> Targets{{0x2000, Sig}};
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setSourceCallHintsEnabled(true);
      Convert.setSourceCalleeTypeHints(&Targets);
      if (Source != Input::CallerBytes)
        EXPECT_THROW(Convert.convert(build(Image), A, BinaryFormat::ELF),
                     LowToMedConversionError);
      else
        EXPECT_NO_THROW(Convert.convert(build(Image), A, BinaryFormat::ELF));
    }
  }
}

TEST(MIPSRecordSourceEntry, StackResultTracksARecordCopiedInAndOutOfTheFrame) {
  auto Bytes = NdType::makeArray(NdType::makeInt(1, false), 17, 17);
  auto Record = NdType::makeStruct({Bytes});
  Record->Passing = NdRecordPassing::ByValue;
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL}) {
    SourceFunctionTypeHint Sig;
    Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    Sig.ReturnType = Record;
    auto Wide = NdType::makeInt(8, false);
    Sig.Parameters.assign(7, {"word", Wide});
    Sig.Parameters.push_back({"bytes", Record});
    Sig.Parameters.push_back({"tail", Wide});
    ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
    const std::map<va_t, SourceFunctionTypeHint> Targets{{0x2000, Sig}};
    for (uint32_t LinkOffset : {140u, 112u}) {
      SCOPED_TRACE(::testing::Message() << unsigned(A) << '/' << LinkOffset);
      // GCC places the input copy at sp+116 and the hidden result at sp+96.
      // Both counted loops use the branch delay slot to store their byte.
      std::vector<uint32_t> Words{0x27bdff70,
                                  0x27a80074,
                                  0x27a90085,
                                  0xafb00088,
                                  0xafbf0000 | LinkOffset,
                                  0x00a08025,
                                  0x908a0000,
                                  0x25080001,
                                  0x24840001,
                                  0x1509fffc,
                                  0xa10affff,
                                  0x24060002,
                                  0x24070003};
      for (uint32_t I = 16; I < 64; I += 4)
        Words.push_back(0xafa00000 | I); // Subsequent scalar argument halves.
      for (uint32_t I = 0; I < 16; I += 4) {
        Words.push_back(0x8faa0074 + I);
        Words.push_back(0xafaa0040 + I);
      }
      Words.insert(Words.end(),
                   {0x93aa0084, 0xa3aa0050, 0xafa00058, 0xafa0005c, 0x0c000800,
                    0x27a40060, // jal callee; addiu a0,sp,96
                    0x27a80060, 0x27a90071, 0x02002825, 0x910a0000, 0x25080001,
                    0x24a50001, 0x1509fffc, 0xa0aaffff, 0x8fbf0000 | LinkOffset,
                    0x8fb00088, 0x03e00008, 0x27bd0090});
      auto Image = image(Words, A);
      Image.MIPS.emplace();
      Image.MIPS->ABI = MIPSABI::O32;
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setSourceCallHintsEnabled(true);
      Convert.setSourceCalleeTypeHints(&Targets);
      const auto Low = build(Image);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      if (LinkOffset == 112)
        EXPECT_THROW(Convert.convert(Low, A, BinaryFormat::ELF),
                     LowToMedConversionError);
      else
        EXPECT_NO_THROW(Convert.convert(Low, A, BinaryFormat::ELF));
    }
  }
}

TEST(MIPSRecordSourceEntry,
     WordGuardPreservesOutgoingStackBytesAtIndirectCall) {
  auto Bytes = NdType::makeArray(NdType::makeInt(1, false), 17, 17);
  auto Record = NdType::makeStruct({Bytes});
  Record->Passing = NdRecordPassing::ByValue;
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    SourceCallTypeHint Hint;
    Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    Hint.Signature.ReturnType = Record;
    Hint.Signature.Parameters.assign(7, {"word", NdType::makeInt(8, false)});
    Hint.Signature.Parameters.push_back({"bytes", Record});
    ASSERT_TRUE(
        mips::assignRecordSourceABI(Hint.Signature, A, BinaryFormat::ELF));
    for (bool WholeWord : {false, true}) {
      // After the two outgoing stores GCC uses word ADDIU to select the
      // result buffer. The JALR delay slot writes only the record's final byte.
      auto Image =
          image({0x67bdffc0, WholeWord ? 0xffa00000u : 0xa3a00000u, 0xffa00008,
                 0x27a40020, 0x0180f809, 0xa3a00010, 0x03e00008, 0x67bd0040},
                A);
      Image.MIPS.emplace();
      Image.MIPS->ABI = isMIPSN32(A) ? MIPSABI::N32 : MIPSABI::N64;
      auto Low = build(Image);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      std::optional<SourceCallOccurrenceKey> Site;
      LowOp *Check = nullptr;
      for (auto &Block : Low.Blocks)
        for (auto &Op : Block.Ops) {
          if (Op.Opcode == NdOp::INDIR_CALL)
            Site = sourceCallOccurrenceKey(Op);
          if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 2 &&
              Op.Inputs[0].isConst() &&
              Op.Inputs[0].Offset == uint64_t(Intrinsic::MIPSRequireWord))
            Check = &Op;
        }
      ASSERT_TRUE(Site);
      ASSERT_NE(Check, nullptr);
      std::string Error;
      EXPECT_EQ(sourceCallStackArgumentsDefined(Low, Hint, *Site, A, 0, Error),
                WholeWord)
          << Error;
      Check->Inputs[1].Size = 4;
      EXPECT_FALSE(
          sourceCallStackArgumentsDefined(Low, Hint, *Site, A, 0, Error));
    }
  }
}

TEST(MIPSRecordSourceEntry, HiddenResultCannotOverwriteAnyByteOfSavedLink) {
  auto Bytes = NdType::makeArray(NdType::makeInt(1, false), 17, 17);
  auto Record = NdType::makeStruct({Bytes});
  Record->Passing = NdRecordPassing::ByValue;
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                 Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    const bool Wide = isMIPS64(A);
    for (uint32_t Offset : {16u, 40u}) {
      SCOPED_TRACE(Offset);
      // Save RA at sp+56. The result at sp+40 overlaps it by one byte.
      auto Image = image({Wide ? 0x67bdffc0u : 0x27bdffc0u,
                          Wide ? 0xffbf0038u : 0xafbf0038u, 0x0c000800,
                          (Wide ? 0x67a40000u : 0x27a40000u) | Offset,
                          Wide ? 0xdfbf0038u : 0x8fbf0038u, 0x03e00008,
                          Wide ? 0x67bd0040u : 0x27bd0040u},
                         A);
      Image.MIPS.emplace();
      Image.MIPS->ABI = isMIPSN32(A) ? MIPSABI::N32
                        : Wide       ? MIPSABI::N64
                                     : MIPSABI::O32;
      SourceFunctionTypeHint Sig;
      Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
      Sig.ReturnType = Record;
      ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
      const std::map<va_t, SourceFunctionTypeHint> Targets{{0x2000, Sig}};
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setSourceCallHintsEnabled(true);
      Convert.setSourceCalleeTypeHints(&Targets);
      const auto Low = build(Image);
      if (Offset == 16)
        EXPECT_NO_THROW(Convert.convert(Low, A, BinaryFormat::ELF));
      else
        EXPECT_THROW(Convert.convert(Low, A, BinaryFormat::ELF),
                     LowToMedConversionError);
    }
  }
}

TEST(MIPSRecordSourceEntry, PICCallsOnlyObserveArgumentWritesThatReachThem) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                      Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(static_cast<unsigned>(Target));
    const bool Wide = isMIPS64(Target);
    auto Record = NdType::makeStruct({NdType::makeInt(Wide ? 8 : 4, false)});
    Record->Passing = NdRecordPassing::ByValue;
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters.push_back({"value", Record});
    ASSERT_TRUE(
        mips::assignRecordSourceABI(Signature, Target, BinaryFormat::ELF));
    const auto Pieces = sourceABIParameters(Signature);
    ASSERT_EQ(Pieces.size(), 1u);
    EXPECT_EQ(Pieces[0].ByteOffset, 0u);
    EXPECT_EQ(Pieces[0].Location.Kind, SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(Pieces[0].Location.RegisterOffset, mipsreg::r(4));
    EXPECT_EQ(Pieces[0].Location.ValueBytes, Wide ? 8u : 4u);
    std::map<va_t, SourceFunctionTypeHint> Targets{{0x2000, Signature}};

    constexpr uint32_t FrameInA1 = 0x03a02825, FrameInA0 = 0x03a02025;
    const auto Check = [&](uint32_t BeforePIC, uint32_t PICSlot,
                           uint32_t RecordSlot, bool Private) {
      SCOPED_TRACE(BeforePIC);
      SCOPED_TRACE(PICSlot);
      SCOPED_TRACE(RecordSlot);
      // The indirect call can see a1 in its delay slot. The following
      // one-word record call reads only a0, including its own delay slot.
      auto Image = image(
          {Wide ? 0x67bdffe0u : 0x27bdffe0u, Wide ? 0xffbf0018u : 0xafbf001cu,
           BeforePIC, Wide && !isMIPSN32(Target) ? 0xdf990000u : 0x8f990000u,
           0x0320f809, PICSlot, FrameInA1, 0x0c000800, RecordSlot,
           Wide ? 0xdfbf0018u : 0x8fbf001cu, 0x03e00008,
           Wide ? 0x67bd0020u : 0x27bd0020u},
          Target);
      Image.MIPS.emplace();
      Image.MIPS->ABI = isMIPSN32(Target) ? MIPSABI::N32
                        : Wide            ? MIPSABI::N64
                                          : MIPSABI::O32;
      Image.MIPS->FPABI = 1;
      const auto Low = build(Image);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setSourceCallHintsEnabled(true);
      Convert.setSourceCalleeTypeHints(&Targets);
      if (!Private) {
        EXPECT_THROW(Convert.convert(Low, Target, BinaryFormat::ELF),
                     LowToMedConversionError);
        return;
      }
      const auto Med = Convert.convert(Low, Target, BinaryFormat::ELF);
      unsigned PICCalls = 0, RecordCalls = 0;
      for (const auto &Block : Med.Blocks)
        for (const auto &Op : Block.Ops) {
          PICCalls += Op.Opcode == NdOp::INDIR_CALL && !Op.SourceCallHint;
          RecordCalls += Op.Opcode == NdOp::CALL && Op.SourceCallHint &&
                         Op.SourceCallHint->Signature.MIPSRecords;
        }
      EXPECT_EQ(PICCalls, 1u);
      EXPECT_EQ(RecordCalls, 1u);
    };
    constexpr uint32_t SevenInA0 = 0x24040007;
    constexpr uint32_t ElevenInA1 = 0x2405000b;
    Check(0, 0, SevenInA0, true);
    Check(0, FrameInA1, SevenInA0, false);
    Check(0, 0, FrameInA0, false);
    // Only a value that survives the PIC delay slot can expose the frame.
    Check(FrameInA1, ElevenInA1, SevenInA0, true);
    Check(FrameInA1, 0, SevenInA0, false);
  }
}

TEST(MIPSRecordSourceEntry,
     GuardedPICResultPreservesTheSavedLinkAcrossARecordCall) {
  constexpr uint32_t CallOnZero = 0x14400005;    // bnez v0,record_call
  constexpr uint32_t CallOnNonzero = 0x10400005; // beqz v0,record_call
  constexpr uint32_t FrameInA1 = 0x03a02825;     // or a1,sp,zero
  constexpr uint32_t NumericRecord = 0x64040007; // daddiu a0,zero,7
  constexpr uint32_t FrameInRecord = 0x03a02025; // or a0,sp,zero
  constexpr uint32_t PrivateByte = 0xa3a00010;   // sb zero,16(sp)
  constexpr uint32_t LinkByte = 0xa3a00018;      // sb zero,24(sp)
  struct Case {
    const char *Name;
    uint32_t Branch, PICSlot, RecordSlot, Store;
    bool PreservesLink;
  };
  const Case Cases[] = {
      {"equal to zero", CallOnZero, 0, NumericRecord, PrivateByte, true},
      {"unequal to zero", CallOnNonzero, 0, NumericRecord, PrivateByte, false},
      {"frame passed to PIC call", CallOnZero, FrameInA1, NumericRecord,
       PrivateByte, false},
      {"frame passed in record", CallOnZero, 0, FrameInRecord, PrivateByte,
       false},
      {"saved link partly overwritten", CallOnZero, 0, NumericRecord, LinkByte,
       false},
  };
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    auto Record = NdType::makeStruct({NdType::makeInt(8, false)});
    Record->Passing = NdRecordPassing::ByValue;
    SourceFunctionTypeHint Sig;
    Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    Sig.ReturnType = NdType::makeVoid();
    Sig.Parameters = {{"value", Record}};
    ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
    const std::map<va_t, SourceFunctionTypeHint> Targets{{0x2000, Sig}};
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message() << getArchName(A) << ' ' << C.Name);
      // The conditional PIC call observes the opaque result in a0. The typed
      // call at the join observes its own record. The byte store follows both.
      auto Image =
          image({0x67bdffe0, 0xffbf0018, 0x0320f809, 0, C.Branch, 0x00402025,
                 0x3c190002, isMIPSN32(A) ? 0x8f390000u : 0xdf390000u,
                 0x0320f809, C.PICSlot, 0x0c000800, C.RecordSlot, C.Store,
                 0xdfbf0018, 0x03e00008, 0x67bd0020},
                A);
      Image.MIPS.emplace();
      Image.MIPS->ABI = isMIPSN32(A) ? MIPSABI::N32 : MIPSABI::N64;
      Image.MIPS->FPABI = 1;
      const auto Low = build(Image);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setSourceCallHintsEnabled(true);
      Convert.setSourceCalleeTypeHints(&Targets);
      try {
        const auto Med = Convert.convert(Low, A, BinaryFormat::ELF);
        EXPECT_TRUE(C.PreservesLink);
        unsigned PICCalls = 0, RecordCalls = 0;
        for (const auto &Block : Med.Blocks)
          for (const auto &Op : Block.Ops) {
            PICCalls += Op.Opcode == NdOp::INDIR_CALL && !Op.SourceCallHint;
            RecordCalls += Op.Opcode == NdOp::CALL && Op.SourceCallHint &&
                           Op.SourceCallHint->Signature.MIPSRecords;
          }
        EXPECT_EQ(PICCalls, 2u);
        EXPECT_EQ(RecordCalls, 1u);
      } catch (const LowToMedConversionError &Error) {
        EXPECT_FALSE(C.PreservesLink) << Error.what();
        EXPECT_PRED_FORMAT2(testing::IsSubstring, "MIPS source return target",
                            Error.what());
      }
    }
  }
}

TEST(MIPSReturnTargets, NativeCallsRequireAnUnexposedAllocatedReturnSlot) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                      Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    const bool Wide = isMIPS64(Target);
    for (unsigned Variant = 0; Variant != 10; ++Variant) {
      SCOPED_TRACE(static_cast<unsigned>(Target));
      SCOPED_TRACE(Variant);
      // Load a changing external pointer. There is no fixed callee signature.
      std::vector<uint32_t> Words{Wide ? 0x67bdffe0u : 0x27bdffe0u,
                                  Wide ? 0xffbf0018u : 0xafbf001cu,
                                  0x3c190002,
                                  Wide && !isMIPSN32(Target) ? 0xdf390000u
                                                             : 0x8f390000u,
                                  Variant == 1 ? 0x0c000800u : 0x0320f809u,
                                  0x24050025,
                                  Wide ? 0xdfbf0018u : 0x8fbf001cu,
                                  0x03e00008,
                                  Wide ? 0x67bd0020u : 0x27bd0020u};
      if (Variant == 2)
        Words[5] = 0x03a02025; // The call receives the allocated frame in a0.
      if (Variant == 3) {
        // An escape before saving RA remains visible after the exact save.
        Words.insert(Words.begin() + 1,
                     {0x3c010002, Wide ? 0xfc3d0000u : 0xac3d0000u});
      }
      if (Variant == 4)
        Words.insert(Words.begin() + 6,
                     0xa3a0001f); // Partial saved-link write.
      if (Variant == 5) {
        Words[0] = 0; // The saved link lies outside any allocated frame.
        Words[8] = 0;
      }
      if (Variant == 6)
        Words[0] = Wide ? 0x67bdffdf : 0x27bdffdf; // Misaligned call SP.
      if (Variant == 7)
        Words.insert(Words.begin() + 4,
                     Wide ? 0xffbd0000u : 0xafbd0000u); // Escaping stack arg.
      if (Variant == 8)
        Words[3] = 0x03a0c825; // The dynamic target itself exposes SP.
      if (Variant == 9)
        // Beyond the ABI minimum still may be an unknown callee's argument.
        Words.insert(Words.begin() + 4, Wide ? 0xffbd0010u : 0xafbd0010u);
      auto Image = image(Words, Target);
      Image.MIPS.emplace();
      Image.MIPS->HasDSP = true;
      Image.MIPS->ABI = isMIPSN32(Target) ? MIPSABI::N32
                        : Wide            ? MIPSABI::N64
                                          : MIPSABI::O32;
      const auto Low = build(Image);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      for (bool WithImage : {false, true}) {
        SCOPED_TRACE(WithImage);
        LowToMedConverter Convert;
        if (WithImage)
          Convert.setBinaryImage(&Image);
        if (Variant == 0 || (Variant == 1 && WithImage))
          EXPECT_NO_THROW(Convert.convert(Low, Target, BinaryFormat::ELF));
        else
          EXPECT_THROW(Convert.convert(Low, Target, BinaryFormat::ELF),
                       LowToMedConversionError);
      }
    }
  }
}

TEST(MIPSReturnTargets, NativeCallsObserveOnlyReachingArgumentWrites) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                      Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (bool OverwriteLeft : {false, true})
      for (bool OverwriteRight : {false, true})
        for (bool EarlyCall : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << getArchName(Target) << OverwriteLeft << OverwriteRight
                       << EarlyCall);
          const bool Wide = isMIPS64(Target);
          // Both arms must replace the frame pointer before any native call
          // can see it in a0. The right arm sets a0 in the branch delay slot
          // of an ordinary jump to the common call.
          const uint32_t Words[] = {
              Wide ? 0x67bdffe0u : 0x27bdffe0u,
              Wide ? 0xffbf0018u : 0xafbf001cu,
              0x03a02025, // or a0,sp,zero
              EarlyCall ? 0x0320f809u : 0,
              0, // jalr t9; nop
              0x10a00004,
              0, // beq a1,zero,right; nop
              OverwriteLeft ? 0x24040013u : 0,
              0x10000003,
              0, // b join; nop
              0x10000001,
              OverwriteRight ? 0x24040017u : 0,
              0x3c190002, // join: fetch the external callee
              Wide && !isMIPSN32(Target) ? 0xdf390000u : 0x8f390000u,
              0x0320f809,
              0x2405000b, // jalr t9; addiu a1,zero,11
              Wide ? 0xdfbf0018u : 0x8fbf001cu,
              0x03e00008,
              Wide ? 0x67bd0020u : 0x27bd0020u};
          auto Image = image(Words, Target);
          auto Low = build(Image);
          ASSERT_TRUE(Low.hasCompleteLiftCoverage());
          LowToMedConverter Convert;
          if (OverwriteLeft && OverwriteRight && !EarlyCall)
            EXPECT_NO_THROW(Convert.convert(Low, Target, BinaryFormat::ELF));
          else
            EXPECT_THROW(Convert.convert(Low, Target, BinaryFormat::ELF),
                         LowToMedConversionError);
        }
}

TEST(MIPSReturnTargets, UnboundResultReachesTheNextCallUntilOverwritten) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (bool Replace : {false, true}) {
      SCOPED_TRACE(testing::Message() << getArchName(A) << Replace);
      const uint32_t Words[] = {
          0x67bdffe0, 0xffbf0018, // save ra at sp+24
          0x0320f809, 0,          // jalr t9; nop
          0x00402025,             // or a0,v0,zero
          0x3c190002, isMIPSN32(A) ? 0x8f390000u : 0xdf390000u,
          0x0320f809, Replace ? 0x24040013u : 0u, // jalr t9; delay slot
          0xdfbf0018, 0x03e00008,
          0x67bd0020};
      auto Image = image(Words, A);
      auto Low = build(Image);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      if (Replace)
        EXPECT_NO_THROW(Convert.convert(Low, A, BinaryFormat::ELF));
      else
        EXPECT_THROW(Convert.convert(Low, A, BinaryFormat::ELF),
                     LowToMedConversionError);
    }
}

TEST(MIPSReturnTargets, OnlyTheEqualityArmCanForwardAnOpaqueResult) {
  constexpr uint32_t SaveInput = 0x00808025;  // or s0,a0,zero
  constexpr uint32_t SetLiteral = 0x640c0025; // daddiu t0,zero,37
  constexpr uint32_t Forward = 0x00402025;    // or a0,v0,zero
  struct Case {
    const char *Name;
    bool Proven;
    uint32_t Before = 0, After = 0; // writes either side of the producer
    unsigned LHS = 2, RHS = 0;
    bool BEQ = false, CallOnTaken = false, Bypass = false;
    uint32_t CallSlot = 0;
    std::optional<uint32_t> ThirdCallSlot;
  };
  const Case Cases[] = {
      {.Name = "zero", .Proven = true},
      {.Name = "saved input",
       .Proven = true,
       .Before = SaveInput,
       .LHS = 16,
       .RHS = 2,
       .BEQ = true,
       .CallOnTaken = true},
      {.Name = "fresh literal", .Proven = true, .After = SetLiteral, .RHS = 12},
      {.Name = "stale literal",
       .Proven = false,
       .Before = SetLiteral,
       .RHS = 12},
      {.Name = "unequal arm", .Proven = false, .BEQ = true},
      {.Name = "volatile input", .Proven = false, .RHS = 4},
      {.Name = "saved too late",
       .Proven = false,
       .After = SaveInput,
       .RHS = 16},
      {.Name = "frame address",
       .Proven = false,
       .Before = 0x03a08025,
       .RHS = 16},
      {.Name = "low word",
       .Proven = false,
       .After = 0x00026000,
       .LHS = 12}, // sll t0,v0,0
      {.Name = "bypass edge", .Proven = false, .Bypass = true},
      {.Name = "frame in call slot",
       .Proven = false,
       .CallSlot = 0x03a02025}, // or a0,sp,zero
      {.Name = "another frame argument",
       .Proven = false,
       .CallSlot = 0x03a02825},
      {.Name = "another forwarded argument",
       .Proven = true,
       .CallSlot = 0x00402825},
      {.Name = "later unguarded use", .Proven = false, .ThirdCallSlot = 0},
      {.Name = "later overwrite", .Proven = true, .ThirdCallSlot = 0x64040013},
  };
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message() << getArchName(A) << ' ' << C.Name);
      std::vector<uint32_t> Code{
          0x67bdffe0, 0xffbf0018, 0xffb00010,           // save ra and s0
          C.Before,   0x0320f809, 0,          C.After}; // first jalr t9
      const unsigned Bypass = Code.size();
      if (C.Bypass)
        Code.insert(Code.end(), {0x14e00000, Forward}); // bnez a3,call
      const unsigned Compare = Code.size();
      Code.push_back(((C.BEQ ? 4u : 5u) << 26) | (C.LHS << 21) | (C.RHS << 16));
      Code.push_back(C.CallOnTaken ? 0 : Forward);
      if (C.CallOnTaken)
        Code.insert(Code.end(), {0x10000000, 0}); // other arm skips the call
      const unsigned CallBlock = Code.size();
      if (C.CallOnTaken)
        Code.push_back(Forward);
      const auto Call = [&](uint32_t Slot) {
        Code.insert(Code.end(),
                    {0x3c190002, isMIPSN32(A) ? 0x8f390000u : 0xdf390000u,
                     0x0320f809, Slot});
      };
      Call(C.CallSlot);
      if (C.ThirdCallSlot)
        Call(*C.ThirdCallSlot);
      const unsigned Exit = Code.size();
      Code[Compare] |= (C.CallOnTaken ? CallBlock : Exit) - Compare - 1;
      if (C.CallOnTaken)
        Code[Compare + 2] |= Exit - (Compare + 2) - 1;
      if (C.Bypass)
        Code[Bypass] |= CallBlock - Bypass - 1;
      Code.insert(Code.end(), {0xdfb00010, 0xdfbf0018, 0x03e00008, 0x67bd0020});
      const auto Low = build(image(Code, A));
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      try {
        Convert.convert(Low, A, BinaryFormat::ELF);
        EXPECT_TRUE(C.Proven);
      } catch (const LowToMedConversionError &Error) {
        EXPECT_FALSE(C.Proven) << Error.what();
        EXPECT_PRED_FORMAT2(testing::IsSubstring, "MIPS source return target",
                            Error.what());
      }
    }
}

TEST(MIPSReturnTargets, JoinedGuardsAndRecheckedArgumentsUseTheActualWord) {
  const auto Move = [](unsigned Dst, unsigned Src) {
    return (Src << 21) | (Dst << 11) | 0x25u; // or dst,src,zero
  };
  struct Case {
    const char *Name;
    bool Proven;
    uint32_t RightBranch = 4; // beq on the right, bne on the left
    unsigned RightPeer = 17, RightArgument = 2, Checked = 18, Forwarded = 18;
    uint32_t BeforeThird = 0, CheckSlot = 0;
    bool Recheck = true;
  };
  const Case Cases[] = {
      {.Name = "both paths and saved first result", .Proven = true},
      {.Name = "different independent peers", .Proven = true, .RightPeer = 16},
      {.Name = "fresh second result",
       .Proven = true,
       .Checked = 2,
       .Forwarded = 2},
      {.Name = "one unchecked path", .Proven = false, .RightPeer = 2},
      {.Name = "one path forwards a frame address",
       .Proven = false,
       .RightArgument = 29},
      {.Name = "one unequal path", .Proven = false, .RightBranch = 5},
      {.Name = "third call without a check", .Proven = false, .Recheck = false},
      {.Name = "checked register overwritten in branch slot",
       .Proven = false,
       .CheckSlot = Move(18, 2)},
      {.Name = "checked second but forwarded first",
       .Proven = false,
       .Checked = 2},
      {.Name = "checked first but forwarded second",
       .Proven = false,
       .Forwarded = 2},
      {.Name = "volatile after second call", .Proven = false, .Checked = 4},
      {.Name = "only the low word",
       .Proven = false,
       .Checked = 12,
       .BeforeThird = 0x00126000},
      {.Name = "frame address in the final slot",
       .Proven = false,
       .Forwarded = 29},
  };
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message() << getArchName(A) << ' ' << C.Name);
      std::vector<uint32_t> Code{0x67bdffd0,  0xffbf0028, 0xffb00000,
                                 0xffb10008,  0xffb20010, Move(16, 4),
                                 Move(17, 5), 0x0320f809, 0};
      const auto Branch = [&](unsigned Opcode, unsigned LHS, unsigned RHS,
                              uint32_t Slot) {
        const auto I = Code.size();
        Code.insert(Code.end(),
                    {(Opcode << 26) | (LHS << 21) | (RHS << 16), Slot});
        return I;
      };
      const auto TargetHere = [&](size_t I) { Code[I] |= Code.size() - I - 1; };
      const auto Call = [&](uint32_t Slot) {
        Code.insert(Code.end(),
                    {0x3c190002, isMIPSN32(A) ? 0x8f390000u : 0xdf390000u,
                     0x0320f809, Slot});
      };
      const auto Split = Branch(4, 16, 0, Move(18, 2));
      const auto LeftExit = Branch(5, 18, 17, Move(4, 18));
      const auto LeftJoin = Branch(4, 0, 0, 0);
      TargetHere(Split);
      const auto RightJoin =
          Branch(C.RightBranch, C.RightPeer, 2, Move(4, C.RightArgument));
      const auto RightExit = Branch(4, 0, 0, 0);
      TargetHere(LeftJoin);
      TargetHere(RightJoin);
      Call(0);
      Code.push_back(C.BeforeThird);
      const auto LastExit = Branch(5, C.Checked, 16, C.CheckSlot);
      if (!C.Recheck)
        Code[LastExit] = 0;
      Call(Move(4, C.Forwarded));
      for (size_t I : {LeftExit, RightExit})
        TargetHere(I);
      if (C.Recheck)
        TargetHere(LastExit);
      Code.insert(Code.end(), {0xdfb00000, 0xdfb10008, 0xdfb20010, 0xdfbf0028,
                               0x03e00008, 0x67bd0030});
      const auto Low = build(image(Code, A));
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      try {
        Convert.convert(Low, A, BinaryFormat::ELF);
        EXPECT_TRUE(C.Proven);
      } catch (const LowToMedConversionError &Error) {
        EXPECT_FALSE(C.Proven) << Error.what();
        EXPECT_PRED_FORMAT2(testing::IsSubstring, "MIPS source return target",
                            Error.what());
      }
    }
}

TEST(MIPSReturnTargets, ACheckOfOnePhiOnlyQualifiesAnEqualPhi) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned UnrelatedArm : {0u, 1u, 2u, 3u}) {
      SCOPED_TRACE(testing::Message() << getArchName(A) << ' ' << UnrelatedArm);
      const auto SaveSelected = [&](unsigned Arm) {
        return UnrelatedArm == Arm ? 0x02008825u : 0x00408825u; // s1 = s0 or v0
      };
      const std::vector<uint32_t> Code{
          0x67bdffd0,
          0xffbf0028,
          0xffb00000,
          0xffb10008,
          0x00a08025, // save a1 in s0
          0x1080000b,
          0, // beq a0,zero,right
          0x10a00005,
          0, // beq a1,zero,middle
          0x0320f809,
          0, // left result in v0
          0x10000008,
          SaveSelected(1),
          0x0320f809,
          0, // middle result in v0
          0x10000004,
          SaveSelected(2),
          0x0320f809,
          0, // right result in v0
          SaveSelected(3),
          0x16300005,
          0x00402025, // bne s1,s0,exit with move a0,v0 in the slot
          0x3c190002,
          isMIPSN32(A) ? 0x8f390000u : 0xdf390000u,
          0x0320f809,
          0x6405000b, // consume v0 through a0 with a fresh a1
          0xdfb00000,
          0xdfb10008,
          0xdfbf0028,
          0x03e00008,
          0x67bd0030};
      const auto Low = build(image(Code, A));
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      try {
        Convert.convert(Low, A, BinaryFormat::ELF);
        EXPECT_EQ(UnrelatedArm, 0u);
      } catch (const LowToMedConversionError &Error) {
        EXPECT_NE(UnrelatedArm, 0u) << Error.what();
        EXPECT_PRED_FORMAT2(testing::IsSubstring, "MIPS source return target",
                            Error.what());
      }
    }
}

TEST(MIPSReturnTargets, GuardedReloadUsesTheCompleteLiveSpillOnEveryIteration) {
  constexpr uint32_t Spill = 0xffa20028;   // sd v0,40(sp)
  constexpr uint32_t Clear = 0xffa00028;   // sd zero,40(sp)
  constexpr uint32_t Reload = 0xdfb20028;  // ld s2,40(sp)
  constexpr uint32_t Forward = 0xdfa40028; // ld a0,40(sp)
  struct Case {
    const char *Name;
    bool Proven;
    uint32_t Initial = Spill, BeforeProducer = Clear, OnBackedge = Spill;
    uint32_t Load = Reload, AfterLoad = 0, Argument = Forward, BeforeLoad = 0;
  };
  const Case Cases[] = {
      {.Name = "fresh full word", .Proven = true},
      {.Name = "clear after sampling",
       .Proven = true,
       .AfterLoad = Clear,
       .Argument = 0x02402025}, // move a0,s2
      {.Name = "check after a byte write",
       .Proven = true,
       .BeforeLoad = 0xa3a0002c}, // sb zero,44(sp)
      {.Name = "missing entry store", .Proven = false, .Initial = 0},
      {.Name = "open call can read unchecked stack word",
       .Proven = false,
       .BeforeProducer = 0},
      {.Name = "different result on backedge",
       .Proven = false,
       .OnBackedge = 0,
       .Argument = 0x00402025}, // move a0,v0
      {.Name = "narrow comparison", .Proven = false, .Load = 0x8fb20028},
      {.Name = "partial write after sampling",
       .Proven = false,
       .AfterLoad = 0xafa5002c}, // sw a1,44(sp)
      {.Name = "unresolved alias after sampling",
       .Proven = false,
       .AfterLoad = 0xa0400000}, // sb zero,0(v0)
      {.Name = "other stack argument exposes frame",
       .Proven = false,
       .AfterLoad = 0xffbd0020}, // sd sp,32(sp)
      {.Name = "another pointer in delay slot",
       .Proven = false,
       .Argument = 0x03a02025}, // move a0,sp
  };
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message() << getArchName(A) << ' ' << C.Name);
      const auto LoadTarget = isMIPSN32(A) ? 0x8f390000u : 0xdf390000u;
      const uint32_t Code[] = {
          0x67bdffc0,
          0xffbf0038,
          0xffb00000,
          0xffb10008,
          0xffb20010,
          0x00a08025,
          0x00c08825, // save comparison peer and loop count
          0x0320f809,
          0,
          C.Initial,
          0x1220000c,
          0, // beq s1,zero,check
          C.BeforeProducer,
          0x64050025,
          0x64060000, // fresh a1 and a2
          0x3c190002,
          LoadTarget,
          0x0320f809,
          0x64040013,
          C.OnBackedge,
          0x6631ffff,
          0x1620fff6,
          0, // bne s1,zero,repeat
          C.BeforeLoad,
          C.Load,
          C.AfterLoad,
          0x16500006,
          C.Argument, // bne s2,s0,exit
          0x64060000,
          0x3c190002,
          LoadTarget,
          0x0320f809,
          0x6405000b,
          0xdfb00000,
          0xdfb10008,
          0xdfb20010,
          0xdfbf0038,
          0x03e00008,
          0x67bd0040};
      const auto Low = build(image(Code, A));
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      try {
        Convert.convert(Low, A, BinaryFormat::ELF);
        EXPECT_TRUE(C.Proven);
      } catch (const LowToMedConversionError &Error) {
        EXPECT_FALSE(C.Proven) << Error.what();
        EXPECT_PRED_FORMAT2(testing::IsSubstring, "MIPS source return target",
                            Error.what());
      }
    }
}

TEST(MIPSReturnTargets, StackWordHalvesMustMatchTheGuardedArgument) {
  const auto Memory = [](unsigned Opcode, unsigned Reg, unsigned Offset) {
    return (Opcode << 26) | (29u << 21) | (Reg << 16) | Offset;
  };
  const auto Move = [](unsigned Dst, unsigned Src) {
    return (Src << 21) | (Dst << 11) | 0x25u;
  };
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    const unsigned Low = archLittleEndian(A) ? 24 : 28;
    const unsigned High = archLittleEndian(A) ? 28 : 24;
    const auto StoreLow = Memory(43, 2, Low), StoreHigh = Memory(43, 12, High);
    const auto LoadLow = Memory(39, 13, Low), LoadHigh = Memory(35, 14, High);
    const auto Full = Memory(63, 2, 24), Reload = Memory(55, 15, 24);
    const auto Clear = Memory(63, 0, 24), OtherClear = Memory(63, 0, 40);
    const auto Target = isMIPSN32(A) ? 0x8f390000u : 0xdf390000u;
    constexpr uint32_t Split = 0x0002603e; // dsrl32 t0,v0,0
    constexpr uint32_t Shift = 0x000e703c; // dsll32 t2,t2,0
    constexpr uint32_t Join = 0x01ae7825;  // or t3,t1,t2
    const auto Guard = [](unsigned Left, unsigned Right) {
      return 0x14000000u | (Left << 21) | (Right << 16);
    };
    const auto CheckBody = [&](const char *Name, bool Proven,
                               llvm::ArrayRef<uint32_t> Body,
                               unsigned Forwarded) {
      SCOPED_TRACE(testing::Message() << getArchName(A) << ' ' << Name);
      std::vector<uint32_t> Code{
          0x67bdffc0,  0xffbf0038,  0xffb00000, 0xffb10008,
          Move(16, 5), Move(17, 6), 0x0320f809, 0};
      Code.insert(Code.end(), Body.begin(), Body.end());
      Code.insert(Code.end(), {Move(4, Forwarded), 0x3c190002, Target,
                               0x0320f809, 0x6405000b});
      // Guard emits a BNE with an empty displacement to the common exit.
      for (size_t I = 0; I < Code.size(); ++I)
        if ((Code[I] & 0xfc00ffffu) == 0x14000000u)
          Code[I] |= Code.size() - I - 1;
      Code.insert(Code.end(),
                  {0xdfb00000, 0xdfb10008, 0xdfbf0038, 0x03e00008, 0x67bd0040});
      const auto Lifted = build(image(Code, A));
      ASSERT_TRUE(Lifted.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      try {
        Convert.convert(Lifted, A, BinaryFormat::ELF);
        EXPECT_TRUE(Proven);
      } catch (const LowToMedConversionError &Error) {
        EXPECT_FALSE(Proven) << Error.what();
        EXPECT_PRED_FORMAT2(testing::IsSubstring, "MIPS source return target",
                            Error.what());
      }
    };
    const auto Check = [&](const char *Name, bool Proven,
                           llvm::ArrayRef<uint32_t> Before,
                           unsigned Compared = 15, unsigned Forwarded = 2,
                           llvm::ArrayRef<uint32_t> After = {}) {
      std::vector<uint32_t> Body(Before.begin(), Before.end());
      Body.insert(Body.end(), {Guard(Compared, 16), 0});
      Body.insert(Body.end(), After.begin(), After.end());
      CheckBody(Name, Proven, Body, Forwarded);
    };
    Check("doubleword copied before source clear", true,
          {Full, Reload, Memory(63, 15, 40), Clear, Memory(55, 15, 40)});
    Check("word stores then doubleword load", true,
          {Split, StoreLow, StoreHigh, Reload});
    Check("doubleword store then word loads", true,
          {Full, LoadLow, LoadHigh, Shift, Join});
    Check("disjoint addition", true,
          {Full, LoadLow, LoadHigh, Shift, 0x01ae782d});
    Check("disjoint xor", true, {Full, LoadLow, LoadHigh, Shift, 0x01ae7826});
    Check("carry into sampled high half", false,
          {Full, LoadLow, LoadHigh, Shift, 0x35ce0001, 0x01ae782d, 0x000f703e,
           Shift, Join});
    Check("reassemble after guarding original", true, {Full, LoadHigh}, 2, 15,
          {LoadLow, Shift, Join, Clear});
    Check("same half rewritten after sampling", true, {Full, Reload, StoreLow});
    Check("halves held in separate cells", true,
          {Split, StoreLow, Memory(43, 12, 40), LoadLow, Memory(35, 14, 40),
           Shift, Join, Clear, OtherClear});
    Check("open call can read leftover half", false,
          {Split, StoreLow, Memory(43, 12, 40), LoadLow, Memory(35, 14, 40),
           Shift, Join, Clear});
    Check("numeric write removes the only opaque half", true,
          {StoreLow, 0x640d0049, Memory(43, 13, Low)}, 2);
    Check("ordinary byte store after clearing", true,
          {Full, Clear, Memory(40, 0, Low)}, 2);
    Check("exchanged halves", false,
          {Split, Memory(43, 2, High), Memory(43, 12, Low), Reload});
    Check("duplicated low half", false,
          {StoreLow, Memory(43, 2, High), Reload});
    Check("sign extension contaminates high half", false,
          {Full, Memory(35, 13, Low), LoadHigh, Shift, Join});
    Check("sign extension after guarding original", false, {Full, LoadHigh}, 2,
          15, {Memory(35, 13, Low), Shift, Join, Clear});
    Check("only one predecessor supplies the high half", false,
          {Split, StoreLow, 0x12000002, 0, StoreHigh, Reload});
    Check("partial clear of a conditionally stored half", false,
          {0x12000002, 0, StoreLow, Memory(40, 0, Low)}, 2);
    Check("complete clear of a conditionally stored half", true,
          {0x12000002, 0, StoreLow, Memory(43, 0, Low)}, 2);
    Check("exchanged halves checked and forwarded together", true,
          {Full, Memory(39, 13, High), Memory(35, 14, Low), Shift, Join, Clear},
          15, 15);
    Check("byte changed after sampling", false,
          {Full, Reload, Memory(40, 0, High)});
    Check("sample modified before both loads", true,
          {Full, Memory(40, 0, High), LoadLow, LoadHigh, Shift, Join}, 15, 13,
          {Memory(55, 13, 24)});
    // One edge selects a checked result and the other an independent input.
    // The joined word is safe until a later store changes part of it.
    Check("joined safe word", true,
          {0x14500003, 0, 0x10000002, Move(15, 2), Move(15, 16),
           Memory(63, 15, 24)},
          2, 15);
    Check("partial write to joined safe word", false,
          {0x14500003, 0, 0x10000002, Move(15, 2), Move(15, 16),
           Memory(63, 15, 24), Memory(40, 0, Low)},
          2, 15);
    for (unsigned Bytes : {1u, 2u}) {
      SCOPED_TRACE(Bytes);
      std::vector<uint32_t> Body{Full};
      for (unsigned I = 0; I < 8; I += Bytes)
        Body.push_back(Memory(Bytes == 1 ? 40 : 41, 0, 24 + I));
      CheckBody("every byte overwritten", true, Body, 0);
      Body.back() = Body[Body.size() - 2];
      CheckBody("duplicate write leaves a hole", false, Body, 0);
    }
    CheckBody("overlapping writes cover the last byte", true,
              {Full, Memory(43, 0, 24), Memory(41, 0, 28), Memory(41, 0, 30),
               Memory(40, 0, 30)},
              0);
    // The conditional store must cover the last byte on every path.
    CheckBody("only one predecessor clears the last byte", false,
              {Full, Memory(43, 0, 24), Memory(41, 0, 28), Memory(40, 0, 30),
               0x12000002, 0, Memory(40, 0, 31)},
              0);

    // t1:t2 contain the low and high patterns of v0, t0:t3 those of s0.
    const auto SplitPeers = {Full,       LoadLow,     Memory(39, 14, High),
                             Clear,      0x0010603cu, 0x000c603eu,
                             0x0010783eu};
    const auto CheckParts = [&](const char *Name, bool Proven,
                                llvm::ArrayRef<uint32_t> Ops,
                                unsigned Forwarded = 2) {
      std::vector<uint32_t> Body(SplitPeers);
      Body.insert(Body.end(), Ops.begin(), Ops.end());
      CheckBody(Name, Proven, Body, Forwarded);
    };
    const auto LowEqual = Guard(13, 12), HighEqual = Guard(14, 15);
    CheckParts("both halves equal the same input", true,
               {LowEqual, 0, HighEqual, 0});
    CheckParts("high half checked first", true,
               {HighEqual, 0, LowEqual, 0, Shift, Join}, 15);
    CheckParts("both halves zero", true, {Guard(13, 0), 0, Guard(14, 0), 0});
    CheckParts("two distinct literal halves", true,
               {0x640c0007, 0x640f0013, LowEqual, 0, HighEqual, 0});
    CheckParts("all-ones and adjacent literal patterns", true,
               {0x640cffff, 0x000c603c, 0x000c603e, LowEqual, 0, 0x640ffffe,
                0x000f783c, 0x000f783e, HighEqual, 0});
    CheckParts("only the literal low half", false, {0x640c0007, LowEqual, 0});
    CheckParts("only high half checked", false, {HighEqual, 0});
    CheckParts("same low half checked twice", false,
               {LowEqual, 0, LowEqual, 0});
    const auto Binary = [](unsigned Fn, unsigned Dst, unsigned L, unsigned R) {
      return (L << 21) | (R << 16) | (Dst << 11) | Fn;
    };
    const auto LowDiff = Binary(0x26, 12, 12, 13);
    const auto HighDiff = Binary(0x26, 15, 14, 15);
    const auto EitherDiff = Binary(0x25, 24, 15, 12);
    const auto ZeroDiff = Guard(24, 0);
    CheckParts("whole XOR difference", true,
               {Binary(0x26, 24, 2, 16), ZeroDiff, 0});
    CheckParts("whole modular subtraction", true,
               {Binary(0x2f, 24, 16, 2), ZeroDiff, 0});
    CheckParts("folded differences after operand reuse", true,
               {LowDiff, HighDiff, EitherDiff, Move(13, 0), ZeroDiff, 0});
    CheckParts("folded differences and repack", true,
               {LowDiff, HighDiff, EitherDiff, ZeroDiff, 0, Shift, Join}, 15);
    // BEQ selects the right arm. Both arms reach the separate zero test.
    constexpr uint32_t RightArm = 0x12200003, JoinArms = 0x10000002;
    const auto Xor = Binary(0x26, 24, 2, 16);
    const auto Sub = Binary(0x2f, 24, 16, 2);
    CheckParts("each incoming difference proves the same equality", true,
               {RightArm, 0, JoinArms, Xor, Sub, ZeroDiff, 0});
    CheckParts(
        "one incoming difference compares another word", false,
        {RightArm, 0, JoinArms, Xor, Binary(0x2f, 24, 16, 17), ZeroDiff, 0});
    // The fallthrough arm computes the full-width difference in its jump
    // delay slot. The other arm folds the two 32-bit differences before
    // the common zero test of the selected value.
    const auto Mixed = [&](const char *Name, bool Proven, uint32_t Whole,
                           uint32_t First, uint32_t Second, uint32_t Fold) {
      CheckParts(
          Name, Proven,
          {RightArm, 0, 0x10000004, Whole, First, Second, Fold, ZeroDiff, 0});
    };
    Mixed("whole XOR or two folded halves", true, Xor, LowDiff, HighDiff,
          EitherDiff);
    Mixed("whole subtraction or reversed folded halves", true, Sub, HighDiff,
          LowDiff, Binary(0x25, 24, 12, 15));
    CheckParts("folded difference on the fallthrough edge", true,
               {0x12200005, 0, LowDiff, HighDiff, JoinArms, EitherDiff, Xor,
                ZeroDiff, 0});
    CheckParts("whole subtraction or repacked XOR differences", true,
               {RightArm, 0, 0x10000005, Sub, LowDiff, HighDiff,
                Binary(0x3c, 15, 0, 15), EitherDiff, ZeroDiff, 0});
    CheckParts("whole XOR with zero or folded raw halves", true,
               {RightArm, 0, JoinArms, Binary(0x26, 24, 2, 0),
                Binary(0x25, 24, 13, 14), ZeroDiff, 0});
    CheckParts("whole XOR with zero or just the raw low half", false,
               {RightArm, 0, JoinArms, Binary(0x26, 24, 2, 0), Move(24, 13),
                ZeroDiff, 0});
    Mixed("other arm only tests the low difference", false, Sub, LowDiff, 0,
          Move(24, 12));
    Mixed("other arm only tests the high difference", false, Xor, 0, HighDiff,
          Move(24, 15));
    Mixed("low difference occurs twice", false, Xor, LowDiff, Move(15, 12),
          EitherDiff);
    Mixed("folded halves are crossed", false, Sub, Binary(0x26, 12, 12, 14),
          Binary(0x26, 15, 15, 13), EitherDiff);
    Mixed("full-width arm overwrites the sample", false, Move(24, 0), LowDiff,
          HighDiff, EitherDiff);
    CheckParts("folded arm overwrites its completed difference", false,
               {RightArm, 0, 0x10000005, Xor, LowDiff, HighDiff, EitherDiff,
                Move(24, 0), ZeroDiff, 0});
    // A scratch copy of each half changes on the folded edge. Both the
    // scratch zeroes and the original operand equalities hold under the
    // guard; retaining one pair of aliases must not displace the others.
    CheckParts("mixed difference with reused operand copies", true,
               {Move(1, 13), Move(3, 14), RightArm, 0, 0x10000006, Xor, LowDiff,
                HighDiff, EitherDiff, Move(1, 0), Move(3, 0), ZeroDiff, 0});
    CheckParts("intact stack sample of a mixed difference", true,
               {RightArm, 0, 0x10000004, Sub, LowDiff, HighDiff, EitherDiff,
                Memory(63, 24, 40), Move(24, 0), Memory(55, 24, 40), OtherClear,
                ZeroDiff, 0});
    CheckParts("mixed difference changed before testing zero", false,
               {RightArm, 0, 0x10000004, Xor, LowDiff, HighDiff, EitherDiff,
                0x3b180001, ZeroDiff, 0}); // xori t8,t8,1
    CheckParts("word projection of a mixed difference", false,
               {RightArm, 0, 0x10000004, Xor, LowDiff, HighDiff, EitherDiff,
                0x0018c03c, 0x0018c03e, ZeroDiff, 0});
    CheckParts("overwritten difference with an intact saved copy", true,
               {LowDiff, HighDiff, EitherDiff, Move(12, 24), RightArm, 0,
                JoinArms, Move(24, 0), Move(24, 17), Guard(12, 0), 0});
    CheckParts("overwritten difference on one incoming edge", false,
               {LowDiff, HighDiff, EitherDiff, RightArm, 0, JoinArms, 0,
                Move(24, 0), ZeroDiff, 0});
    // Skip the loop or decrement s1 until zero. The final BNE samples the
    // difference after all executed backedges and their delay slots.
    constexpr uint32_t SkipLoop = 0x12200004, Decrement = 0x6631ffff,
                       Backedge = 0x1620fffe;
    // The entry XOR and both arms reach the same successor. Backward jumps
    // revisit the counter test. Forward jumps go straight to the zero test.
    // Keep both arms the same size so swapping them preserves branch offsets.
    constexpr uint32_t InitialArm = 0x32010001, ExitIfDone = 0x1220000e,
                       SelectSecond = 0x10200006, ToggleArm = 0x38210001;
    for (bool Loop : {false, true}) {
      SCOPED_TRACE(Loop ? "two backedges" : "three-way join");
      for (bool ScalarFirst : {true, false}) {
        SCOPED_TRACE(ScalarFirst ? "scalar arm first" : "folded arm first");
        const auto ThreeWay = [&](const char *Name, bool Proven, uint32_t Whole,
                                  uint32_t Fold, uint32_t WholeDelay = 0,
                                  uint32_t FoldDelay = 0) {
          const std::array<uint32_t, 4> Scalar{0, 0, Whole, WholeDelay};
          const std::array<uint32_t, 4> Folded{Binary(0x26, 3, 12, 13),
                                               Binary(0x26, 25, 14, 15), Fold,
                                               FoldDelay};
          const auto &First = ScalarFirst ? Scalar : Folded;
          const auto &Second = ScalarFirst ? Folded : Scalar;
          CheckParts(Name, Proven,
                     {InitialArm, Xor, ExitIfDone, 0, Decrement, SelectSecond,
                      ToggleArm, First[0], First[1], First[2],
                      Loop ? 0x1000fff7u : 0x10000006u, First[3], Second[0],
                      Second[1], Second[2], Loop ? 0x1000fff2u : 0x10000001u,
                      Second[3], ZeroDiff, 0});
        };
        const auto Fold = Binary(0x25, 24, 3, 25);
        constexpr uint32_t FlipDifference = 0x3b180001; // xori t8,t8,1
        ThreeWay("entry XOR, subtraction and folded XOR", true, Sub, Fold);
        ThreeWay("scalar arm compares only low bits", false,
                 Binary(0x26, 24, 12, 13), Fold);
        ThreeWay("scalar arm compares only high bits", false,
                 Binary(0x26, 24, 14, 15), Fold);
        ThreeWay("folded arm compares only low bits", false, Sub, Move(24, 3));
        ThreeWay("folded arm compares only high bits", false, Sub,
                 Move(24, 25));
        ThreeWay("changed scalar difference", false, Sub, Fold, FlipDifference);
        ThreeWay("changed folded difference", false, Sub, Fold, 0,
                 FlipDifference);
      }
    }
    // Both arms enter the latch at index 8. An untaken BNEL skips its
    // slot and jumps over the folded arm to the zero test at index 18.
    const auto ArmLow = Binary(0x26, 3, 12, 13);
    const auto ArmHigh = Binary(0x26, 25, 14, 15);
    const auto SharedLatch = [&](const char *Name, bool Proven, uint32_t Whole,
                                 uint32_t Fold, uint32_t AtLatch,
                                 uint32_t Branch, uint32_t Slot) {
      CheckParts(Name, Proven,
                 {InitialArm, Xor,        0x1220000f, 0,        0x10200009,
                  ToggleArm,  0x10000001, Whole,      AtLatch,  Decrement,
                  Branch,     Slot,       0x10000005, 0,        ArmLow,
                  ArmHigh,    0x1000fff7, Fold,       ZeroDiff, 0});
    };
    constexpr uint32_t LikelyBack = 0x5620fff9, AlwaysSlotBack = 0x1620fff9;
    const auto Fold = Binary(0x25, 24, 3, 25);
    SharedLatch("both arms recompute the difference", true, Sub, Fold, 0,
                LikelyBack, Xor);
    SharedLatch("late arm checks only the low half", false, Sub, Move(24, 3), 0,
                LikelyBack, Xor);
    SharedLatch("late arm checks only the high half", false, Sub, Move(24, 25),
                0, LikelyBack, Xor);
    SharedLatch("early arm checks only the high half", false,
                Binary(0x26, 24, 14, 15), Fold, 0, LikelyBack, Xor);
    SharedLatch("changed difference reaches the untaken exit", false, Sub, Fold,
                0x3b180001, LikelyBack, Xor);
    SharedLatch("taken-only clobber is recomputed before exit", true, Sub, Fold,
                0, LikelyBack, Move(24, 0));
    SharedLatch("ordinary slot clobber reaches the exit", false, Sub, Fold, 0,
                AlwaysSlotBack, Move(24, 0));
    CheckParts("mixed zero evidence through a loop header", true,
               {RightArm, 0, 0x10000004, Sub, LowDiff, HighDiff, EitherDiff,
                SkipLoop, 0, Decrement, Backedge, 0, ZeroDiff, 0});
    CheckParts("loop-invariant folded difference", true,
               {LowDiff, HighDiff, EitherDiff, SkipLoop, 0, Decrement, Backedge,
                0, ZeroDiff, 0});
    CheckParts("backedge delay slot replaces the difference", false,
               {LowDiff, HighDiff, EitherDiff, SkipLoop, 0, Decrement, Backedge,
                Move(24, 0), ZeroDiff, 0});
    CheckParts("loop recomputes an equivalent whole difference", true,
               {Xor, SkipLoop, 0, Decrement, Backedge, Sub, ZeroDiff, 0});
    CheckParts("only a taken backedge replaces the difference", false,
               {Xor, 0x12200006, 0, Decrement, 0x12200003, 0, 0x1000fffc,
                Move(24, 0), ZeroDiff, 0});
    CheckParts("one path bypasses the zero test", false,
               {Xor, 0x12200003, 0, ZeroDiff, 0});
    CheckParts("fold half differences from the preceding block", true,
               {LowDiff, HighDiff, RightArm, 0, JoinArms, 0, Move(12, 12),
                EitherDiff, ZeroDiff, 0});
    CheckParts("one arm changes a byte of the spilled difference", false,
               {LowDiff, HighDiff, EitherDiff, Memory(63, 24, 40), RightArm, 0,
                JoinArms, 0, Memory(40, 0, archLittleEndian(A) ? 40 : 47),
                Memory(55, 24, 40), OtherClear, ZeroDiff, 0});
    CheckParts("matching differences can cancel", false,
               {LowDiff, HighDiff, Binary(0x26, 24, 15, 12), ZeroDiff, 0});
    CheckParts("disjoint differences can mask each other", false,
               {LowDiff, HighDiff, Binary(0x24, 24, 15, 12), ZeroDiff, 0});
    CheckParts("repeated low difference misses high half", false,
               {LowDiff, Binary(0x25, 24, 12, 12), ZeroDiff, 0});
    CheckParts("word subtraction misses high half", false,
               {Binary(0, 12, 0, 2), Binary(0, 15, 0, 16),
                Binary(0x23, 24, 12, 15), ZeroDiff, 0});
    CheckParts("zero high difference can include a borrow", false,
               {Binary(0x2f, 24, 2, 16), 0x0018c03e, ZeroDiff, 0});
    CheckParts(
        "folded guard followed by changed high half", false,
        {LowDiff, HighDiff, EitherDiff, ZeroDiff, Move(14, 13), Shift, Join},
        15);
    CheckParts("intact spilled difference", true,
               {LowDiff, HighDiff, EitherDiff, Memory(63, 24, 40),
                Memory(55, 24, 40), OtherClear, ZeroDiff, 0});
    for (unsigned Load : {35u, 39u})
      CheckParts("word spill of folded half differences", true,
                 {LowDiff, HighDiff, EitherDiff, OtherClear, Memory(43, 24, 40),
                  Memory(Load, 24, 40), ZeroDiff, 0});
    CheckParts("word spill loses the high bits of a whole XOR", false,
               {Binary(0x26, 24, 2, 16), Memory(43, 24, 40), Memory(39, 24, 40),
                OtherClear, ZeroDiff, 0});
    CheckParts(
        "checked zero difference in outgoing slot", true,
        {LowDiff, HighDiff, EitherDiff, Memory(63, 24, 40), ZeroDiff, 0});
    CheckParts(
        "untested difference in outgoing slot", false,
        {LowDiff, HighDiff, EitherDiff, Memory(63, 24, 40), Guard(17, 0), 0},
        0);
    CheckParts("word spill of folded signed word comparisons", true,
               {Binary(0, 12, 0, 12), Binary(0, 13, 0, 13),
                Binary(0, 14, 0, 14), Binary(0, 15, 0, 15), LowDiff, HighDiff,
                EitherDiff, OtherClear, Memory(43, 24, 40), Memory(35, 24, 40),
                ZeroDiff, 0});
    CheckParts("saved difference belongs to a previous call result", false,
               {LowDiff, HighDiff, EitherDiff, Move(16, 24), 0x3c190002, Target,
                0x0320f809, Move(4, 0), Guard(16, 0), 0});
    CheckParts("partial overwrite of spilled difference", false,
               {LowDiff, HighDiff, EitherDiff, Memory(63, 24, 40),
                Memory(40, 0, archLittleEndian(A) ? 40 : 47),
                Memory(55, 24, 40), OtherClear, ZeroDiff, 0});
    CheckParts("halves compared to different inputs", false,
               {LowEqual, 0, 0x0011783e, HighEqual, 0});
    CheckParts("halves compared in the wrong positions", false,
               {Guard(13, 15), 0, Guard(14, 12), 0});
    CheckParts("one path bypasses the high comparison", false,
               {LowEqual, 0, 0x12200003, 0, HighEqual, 0});
    CheckParts("checks on mutually exclusive paths", false,
               {0x12200005, 0, LowEqual, 0, 0x10000003, 0, HighEqual, 0});
    CheckParts("whole check then low check and repack saved pieces", true,
               {Guard(2, 16), Move(2, 0), Guard(13, 17), 0, Shift, Join}, 15);
    CheckParts("both orders around a join", true,
               {0x12200007, 0, LowEqual, 0, HighEqual, 0, 0x10000005, 0,
                HighEqual, 0, LowEqual, 0, Shift, Join},
               15);
    CheckParts("path-specific whole peers followed by a low check", true,
               {0x12200005, 0, Guard(2, 16), 0, 0x10000003, 0, Guard(2, 17), 0,
                Guard(13, 17), Move(2, 0), Shift, Join},
               15);
    CheckParts("low check expired at another call", false,
               {LowEqual, Move(17, 2), 0x3c190002, Target, 0x0320f809,
                Move(4, 0), 0x0011703e, 0x0010783e, HighEqual, 0},
               17);
    CheckParts("whole check then low 16 bits and repack", true,
               {Guard(2, 16), 0, 0x31acffff, Guard(12, 17), 0, Shift, Join},
               15);
    CheckParts("low 16 bits alone and repack", false,
               {0x31acffff, Guard(12, 17), 0, Shift, Join}, 15);
    CheckParts("low check then repack saved pieces", false,
               {Guard(13, 17), 0, Shift, Join}, 15);
    CheckParts("high replaced in equality delay slot", false,
               {LowEqual, 0, HighEqual, Move(14, 13), Shift, Join}, 15);
    Check("mixed calls", false,
          {Move(17, 2), 0x3c190002, Target, 0x0320f809, 0x64040007, 0x0011603e,
           StoreLow, StoreHigh, Reload});
  }
}

TEST(MIPSReturnTargets, ReloadAfterACallRequiresPrivateSpillBytes) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    auto Record = NdType::makeStruct({NdType::makeInt(8, false)});
    Record->Passing = NdRecordPassing::ByValue;
    SourceFunctionTypeHint Sig;
    Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    Sig.ReturnType = NdType::makeVoid();
    Sig.Parameters = {{"value", Record}};
    ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
    const std::map<va_t, SourceFunctionTypeHint> Targets{{0x2000, Sig}};
    for (bool Bound : {false, true}) {
      SCOPED_TRACE(testing::Message() << getArchName(A) << ' ' << Bound);
      auto Image =
          image({0x67bdffc0, 0xffbf0038, 0x0320f809, 0, 0xffa20028, // spill v0
                 0x0c000800, 0x64040007, // call with a numeric argument
                 0xdfac0028, 0x15800005, 0x01802025, // reload, compare, forward
                 0x3c190002, isMIPSN32(A) ? 0x8f390000u : 0xdf390000u,
                 0x0320f809, 0x6405000b, 0xdfbf0038, 0x03e00008, 0x67bd0040},
                A);
      Image.MIPS.emplace();
      Image.MIPS->ABI = isMIPSN32(A) ? MIPSABI::N32 : MIPSABI::N64;
      Image.MIPS->FPABI = 1;
      const auto Low = build(Image);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      Convert.setBinaryImage(&Image);
      Convert.setSourceCallHintsEnabled(true);
      if (Bound) {
        Convert.setSourceCalleeTypeHints(&Targets);
        EXPECT_NO_THROW(Convert.convert(Low, A, BinaryFormat::ELF));
      } else {
        EXPECT_THROW(Convert.convert(Low, A, BinaryFormat::ELF),
                     LowToMedConversionError);
      }
    }
  }
}

TEST(MIPSReturnTargets, ARepeatedCallNeedsItsCurrentWordOnTheBackedge) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (bool SameWord : {false, true}) {
      SCOPED_TRACE(testing::Message() << getArchName(A) << ' ' << SameWord);
      const uint32_t Code[] = {
          0x67bdffd0,
          0xffbf0028,
          0xffb00000,
          0xffb10008,
          0xffb20010,
          0x00a08025,
          0x00c08825,
          0x02002025, // initially pass the saved peer
          0x64060000,
          0x3c190002,
          isMIPSN32(A) ? 0x8f390000u : 0xdf390000u,
          0x0320f809,
          0x64050025, // set a1 in the call's delay slot
          0x12200006,
          0x6631ffff,                  // beq s1,zero,exit
          SameWord ? 0x00409025u : 0u, // s2 holds this or a previous result
          0x16500003,
          0x00402025, // bne s2,s0,exit; move a0,v0
          0x1000fff5,
          0x00409025, // next iteration saves the old return word
          0xdfb00000,
          0xdfb10008,
          0xdfb20010,
          0xdfbf0028,
          0x03e00008,
          0x67bd0030};
      const auto Low = build(image(Code, A));
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      if (SameWord)
        EXPECT_NO_THROW(Convert.convert(Low, A, BinaryFormat::ELF));
      else
        EXPECT_THROW(Convert.convert(Low, A, BinaryFormat::ELF),
                     LowToMedConversionError);
    }
}

TEST_P(MIPSControl, ReusedExecutionUnitClearsAnOldSlotAndFailedHead) {
  const auto Code = encode({0x0c000408, 0x8ca2fff4, 0}, GetParam());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  DecodedExecutionUnit Unit;
  ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), Code.size(), 0x1000, Unit),
            8);
  ASSERT_TRUE(Unit.HasDelaySlot);
  ASSERT_EQ(Decode.decodeUnitForLift(Code.data() + 8, 4, 0x1008, Unit), 4);
  EXPECT_FALSE(Unit.HasDelaySlot);
  EXPECT_EQ(Unit.Slot.Raw, nullptr);
  EXPECT_EQ(Unit.Slot.Size, 0);
  ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), 1, 0x1010, Unit), 0);
  EXPECT_EQ(Unit.Head.Raw, nullptr);
  EXPECT_EQ(Unit.Head.Size, 0);
}

TEST_P(MIPSControl,
       AReinitializedDecoderUsesTheNewInstructionAddressWidthAndAlignment) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  const auto Nop = encode({0}, GetParam());
  DecodedInsn Insn{};
  EXPECT_EQ(Decode.decodeOne(Nop.data(), 4, 0x1002, Insn), 0);
  EXPECT_EQ(Decode.decodeOneLight(Nop.data(), 4, 0x100000000ull, Insn),
            isMIPS64(GetParam()) ? 4 : 0);
  if (!isMIPS64(GetParam())) {
    ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
    const uint8_t Compact[] = {
        uint8_t(archLittleEndian(GetParam()) ? 0 : 0x65),
        uint8_t(archLittleEndian(GetParam()) ? 0x65 : 0)};
    EXPECT_EQ(Decode.decodeOne(Compact, 2, 0xfffffffe, Insn), 2);
    EXPECT_EQ(Decode.decodeOneLight(Compact, 2, 0xffffffff, Insn), 0);
  }
  ASSERT_TRUE(Decode.init(Arch::X64));
  const uint8_t X86Nop = 0x90;
  EXPECT_EQ(Decode.decodeOne(&X86Nop, 1, 0x100000001ull, Insn), 1);
}

TEST_P(MIPSControl, WideCallResultKeepsTheO32RegisterWordOrder) {
  if (isMIPS64(GetParam()))
    return;
  MedFunc Function;
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.back();
  Block.Id = 0;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x1000;
  Call.addInput(MedVar::makeConst(0x2000, 4));
  Call.Output.Kind = MedVar::Reg;
  Call.Output.RegOff = mipsreg::V0;
  Call.Output.Size = 4;
  Call.Output.Id = 1;
  Call.Output.SSAVer = 1;
  Block.Ops.push_back(Call);
  const std::set<va_t> WideCallees = {0x2000};
  modelCallWideIntReturn(Function, GetParam(), &WideCallees);
  ASSERT_EQ(Block.Ops.size(), 3u);
  constexpr uint64_t Result = 0x1122334455667788ull;
  for (unsigned I = 1; I <= 2; ++I) {
    const auto &Extract = Block.Ops[I];
    ASSERT_EQ(Extract.Opcode, NdOp::SUBBYTES);
    ASSERT_EQ(Extract.NumInputs, 2);
    ASSERT_TRUE(Extract.Inputs[1].isConst());
    const uint32_t Word = Result >> (8 * Extract.Inputs[1].ConstVal);
    const bool First = Extract.Output.RegOff == mipsreg::V0;
    const bool High = First != archLittleEndian(GetParam());
    EXPECT_EQ(Word, High ? 0x11223344u : 0x55667788u);
  }
}

TEST_P(MIPSControl, PhysicalDecodeAndOwnedExecutionUnitsRemainDistinct) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  const auto Bytes = encode({0x03e00008, 0x27e20001, 0x24030002}, GetParam());
  DecodedExecutionUnit Unit;
  ASSERT_EQ(Decode.decodeUnitForLift(Bytes.data(), Bytes.size(), 0x1000, Unit),
            8);
  EXPECT_EQ(Unit.Head.Size, 4);
  EXPECT_EQ(Unit.Slot.Size, 4);
  DecodedInsn Other;
  ASSERT_EQ(Decode.decodeOneForLift(Bytes.data() + 8, 4, 0x1008, Other), 4);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Decode.liftToLow(Unit.Head, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
  auto Member = Decode.liftUnitToLow(Unit, Ops);
  ASSERT_TRUE(Member);
  BinaryImage Image;
  Image.Arch = GetParam();
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(mipsreg::RA, 0x3000);
  for (const auto &Op : Ops) {
    if (Op.Opcode == NdOp::RETURN) {
      ASSERT_EQ(Op.NumInputs, 1);
      EXPECT_EQ(Emulator.getRegister(Op.Inputs[0].Offset), 0x3000u);
    } else {
      ASSERT_TRUE(Emulator.step(Op));
    }
  }
  EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 0x3001u);
  EXPECT_EQ(Decode.decodeUnitForLift(Bytes.data(), 4, 0x1000, Unit), 0);
  EXPECT_EQ(Decode.decodeUnitForLift(Bytes.data(), 7, 0x1000, Unit), 0);
}

TEST_P(MIPSControl, ConditionAndIndirectTargetAreCapturedBeforeSlotWrites) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  for (const auto Words : {std::vector<uint32_t>{0x10800003, 0x24040001},
                           std::vector<uint32_t>{0x00800008, 0x24040001}}) {
    const auto Bytes = encode(Words, GetParam());
    DecodedExecutionUnit Unit;
    ASSERT_EQ(
        Decode.decodeUnitForLift(Bytes.data(), Bytes.size(), 0x1000, Unit), 8);
    std::vector<LowOp> Ops;
    ASSERT_TRUE(Decode.liftUnitToLow(Unit, Ops));
    BinaryImage Image;
    Image.Arch = GetParam();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::r(4), 0);
    for (const auto &Op : Ops) {
      if (Op.Opcode == NdOp::COND_BR)
        EXPECT_EQ(Emulator.getRegister(Op.Inputs[1].Offset), 1u);
      else if (Op.Opcode == NdOp::INDIR_BR)
        EXPECT_EQ(Emulator.getRegister(Op.Inputs[0].Offset), 0u);
      else
        ASSERT_TRUE(Emulator.step(Op));
    }
    EXPECT_EQ(Emulator.getRegister(mipsreg::r(4)), 1u);
  }
}

TEST_P(MIPSControl, LinkIsVisibleToSlotAndSlotCannotTransferControl) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  // jal 0x2000; addu a0,ra,zero
  auto Bytes = encode({0x0c000800, 0x03e02021}, GetParam());
  DecodedExecutionUnit Unit;
  ASSERT_EQ(Decode.decodeUnitForLift(Bytes.data(), Bytes.size(), 0x1000, Unit),
            8);
  std::vector<LowOp> Ops;
  ASSERT_TRUE(Decode.liftUnitToLow(Unit, Ops));
  BinaryImage Image;
  Image.Arch = GetParam();
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(mipsreg::RA, 0x3000);
  for (const auto &Op : Ops)
    if (Op.Opcode != NdOp::CALL)
      ASSERT_TRUE(Emulator.step(Op));
  EXPECT_EQ(Emulator.getRegister(mipsreg::r(4)), 0x1008u);
  Bytes = encode({0x0c000800, 0x03e00008}, GetParam());
  ASSERT_EQ(Decode.decodeUnitForLift(Bytes.data(), Bytes.size(), 0x1000, Unit),
            8);
  Ops.clear();
  EXPECT_THROW(Decode.liftUnitToLow(Unit, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST_P(MIPSControl, ZeroEqualityBranchesAlwaysExecuteTheirSlot) {
  for (uint32_t Branch : {0x10000003u, 0x50000003u}) {
    // beq[l] zero,zero,0x1010; addiu v0,zero,7; jr ra; nop; jr ra; nop
    const auto Image =
        image({Branch, 0x24020007, 0x03e00008, 0, 0x03e00008, 0}, GetParam());
    const auto Func = build(Image);
    const auto &Entry = Func.Blocks.front();
    ASSERT_EQ(Entry.Succs.size(), 1u);
    EXPECT_EQ(Func.Blocks[Entry.Succs.front()].StartAddr, 0x1010u);
    ASSERT_EQ(Entry.InstructionBoundaries.size(), 1u);
    EXPECT_TRUE(Entry.InstructionBoundaries.front().DelaySlot);
    EXPECT_EQ(Entry.Ops.back().Opcode, NdOp::BRANCH);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.run(Entry);
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 7u);
  }
}

TEST_P(MIPSControl, LikelyBranchAnnulsSlotBeforeSSA) {
  // beql a0,zero,0x1010; addiu v0,zero,7
  // jr ra; addiu v0,zero,9
  // jr ra; nop
  const auto Image =
      image({0x50800003, 0x24020007, 0x03e00008, 0x24020009, 0x03e00008, 0},
            GetParam());
  const auto Func = build(Image);
  ASSERT_FALSE(Func.Blocks.empty());
  const auto &Entry = Func.Blocks.front();
  ASSERT_EQ(Entry.InstructionBoundaries.size(), 1u);
  EXPECT_TRUE(Entry.InstructionBoundaries.front().DelaySlot);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Entry.InstructionBoundaries.front().ControlFlags,
      LowInstructionControlFlag::InstructionGuard));
  for (unsigned Input : {0u, 1u}) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::r(4), Input);
    Emulator.setRegister(mipsreg::V0, 3);
    Emulator.run(Entry);
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Input ? 3u : 7u);
  }
  LowToMedConverter Converter;
  const auto Med = Converter.convert(Func, GetParam());
  // The slot definition belongs to a distinct guarded path after conversion.
  bool FoundSeven = false;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        FoundSeven |= Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == 7;
  EXPECT_TRUE(FoundSeven);
}

TEST_P(MIPSControl, LikelySkipThreadsJumpsAfterItsAnnulledSlot) {
  // beql a0,zero,0x1010; addiu v0,zero,7; b 0x1018; nop
  // jr ra; addiu v0,v0,11; b 0x1020; nop; jr ra; addiu v0,v0,13
  const auto Low =
      build(image({0x50800003, 0x24020007, 0x10000003, 0, 0x03e00008,
                   0x2442000b, 0x10000001, 0, 0x03e00008, 0x2442000d},
                  GetParam()));
  ASSERT_TRUE(Low.hasCompleteLiftCoverage());
  LowToMedConverter Converter;
  const auto Med = Converter.convert(Low, GetParam());
  const auto &Guard = Med.Blocks.front();
  ASSERT_FALSE(Guard.Ops.empty());
  ASSERT_EQ(Guard.Ops.back().Opcode, NdOp::COND_BR);
  ASSERT_EQ(Guard.Succs.size(), 2u);
  EXPECT_EQ(Guard.Ops.back().Inputs[0].ConstVal, 0x1020u);
  EXPECT_NE(Guard.Succs.front(), Guard.Succs.back());
  for (const int Id : Guard.Succs) {
    const auto &Succ = Med.Blocks.at(Id);
    if (Succ.Role == MedBlockRole::PredicatedEffect) {
      ASSERT_EQ(Succ.Succs.size(), 1u);
      EXPECT_EQ(Med.Blocks.at(Succ.Succs.front()).StartAddr, 0x1010u);
    } else {
      EXPECT_EQ(Succ.StartAddr, 0x1020u);
      ASSERT_FALSE(Succ.Ops.empty());
      EXPECT_EQ(Succ.Ops.back().Opcode, NdOp::RETURN);
    }
  }
}

TEST_P(MIPSControl, LikelySlotStoreIsGuardedInLowAndMedIR) {
  // beql a0,zero,0x1010; sw v0,0(a1); jr ra; nop; jr ra; nop
  auto Image =
      image({0x50800003, 0xaca20000, 0x03e00008, 0, 0x03e00008, 0}, GetParam());
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 16;
  Data.Data.resize(16);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Data));
  const auto Function = build(Image);
  ASSERT_FALSE(Function.Blocks.empty());
  for (unsigned Input : {0u, 1u}) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::r(4), Input);
    Emulator.setRegister(mipsreg::r(5), 0x2000);
    Emulator.setRegister(mipsreg::V0, 0x12345678);
    Emulator.run(Function.Blocks.front());
    LowOp Read;
    Read.Opcode = NdOp::LOAD;
    Read.Output = NdVar::reg(mipsreg::V1, 4);
    Read.addInput(NdVar::cst(0x2000, isMIPS64(GetParam()) ? 8 : 4));
    ASSERT_TRUE(Emulator.step(Read));
    EXPECT_EQ(Emulator.getRegister(mipsreg::V1), Input ? 0u : 0x12345678u);
  }
  LowToMedConverter Converter;
  const auto Med = Converter.convert(Function, GetParam());
  ASSERT_GE(Med.Blocks.size(), 3u);
  int StoreBlock = -1;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::STORE) {
        EXPECT_EQ(StoreBlock, -1);
        StoreBlock = Block.Id;
        EXPECT_EQ(Op.Addr, 0x1004u);
      }
  ASSERT_GE(StoreBlock, 0);
  const auto &Guard = Med.Blocks.front();
  EXPECT_NE(StoreBlock, Guard.Id);
  ASSERT_EQ(Guard.Succs.size(), 2u);
  EXPECT_TRUE(Guard.Succs[0] == StoreBlock || Guard.Succs[1] == StoreBlock);
  ASSERT_FALSE(Guard.Ops.empty());
  EXPECT_EQ(Guard.Ops.back().Opcode, NdOp::COND_BR);
}

TEST_P(MIPSControl, BranchIntoSlotDecodesAnIndependentExecutionPath) {
  // beq a0,zero,0x100c; addiu v0,zero,7
  // jr ra; addiu v0,v0,1
  // jr ra; nop
  const auto Image =
      image({0x10800002, 0x24020007, 0x03e00008, 0x24420001, 0x03e00008, 0},
            GetParam());
  const auto Func = build(Image);
  bool Independent = false;
  bool Owned = false;
  for (const auto &Block : Func.Blocks) {
    if (Block.StartAddr == 0x100c) {
      Independent = true;
      ASSERT_FALSE(Block.InstructionBoundaries.empty());
      EXPECT_EQ(Block.InstructionBoundaries.front().Address, 0x100cu);
      EXPECT_EQ(Block.InstructionBoundaries.front().Size, 4);
    }
    for (const auto &Boundary : Block.InstructionBoundaries)
      Owned |= Boundary.Address == 0x1008 && Boundary.DelaySlot.has_value();
  }
  EXPECT_TRUE(Independent);
  EXPECT_TRUE(Owned);
}

TEST_P(MIPSControl, TailCallRetainsSlotArgumentSetup) {
  // j 0x2000; addiu a1,zero,7
  auto Image = image({0x08000800, 0x24050007}, GetParam());
  Segment Callee;
  Callee.VA = 0x2000;
  Callee.Data = encode({0x03e00008, 0}, GetParam());
  Callee.Size = Callee.FileSz = Callee.Data.size();
  Callee.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(Callee));
  Image.Symbols.push_back(Symbol::makeFunc(0x2000));
  const auto Func = build(Image);
  ASSERT_FALSE(Func.Blocks.empty());
  const auto &Block = Func.Blocks.front();
  ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
  EXPECT_EQ(Block.InstructionBoundaries.front().Control,
            LowInstructionControl::TailCall);
  ASSERT_TRUE(Block.InstructionBoundaries.front().DelaySlot);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  for (const auto &Op : Block.Ops) {
    if (Op.Opcode == NdOp::CALL) {
      EXPECT_EQ(Emulator.getRegister(mipsreg::r(5)), 7u);
      break;
    }
    ASSERT_TRUE(Emulator.step(Op));
  }
}

TEST_P(MIPSControl, HistoricalRetirementKeepsPhysicalAndGuardedBoundaries) {
  auto Image =
      image({0x50800003, 0x8ca20000, 0x03e00008, 0, 0x03e00008, 0}, GetParam());
  Image.MIPS.emplace();
  Image.MIPS->ISARevision = 0;
  Image.MIPS->HistoricalISA = isMIPS64(GetParam()) ? 3 : 2;
  Image.MIPS->ABI = isMIPS64(GetParam()) ? MIPSABI::N64 : MIPSABI::O32;
  const auto Func = build(Image);
  ASSERT_FALSE(Func.Blocks.empty());
  unsigned Begins = 0, Retires = 0;
  for (const auto &Block : Func.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INTRINSIC) {
        Begins +=
            Op.Inputs[0].Offset == unsigned(Intrinsic::MIPSHistoricalBegin);
        Retires +=
            Op.Inputs[0].Offset == unsigned(Intrinsic::MIPSHistoricalRetire);
      }
  EXPECT_EQ(Begins, 6u);
  EXPECT_EQ(Retires, Begins);
  LowToMedConverter Converter;
  const auto Med = Converter.convert(Func, GetParam());
  ASSERT_GE(Med.Blocks.size(), 3u);
  unsigned MedBegins = 0, MedRetires = 0;
  int SlotBlock = -1;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INTRINSIC) {
        if (Op.Inputs[0].ConstVal == unsigned(Intrinsic::MIPSHistoricalBegin)) {
          ++MedBegins;
          if (Op.Addr == 0x1004)
            SlotBlock = Block.Id;
        }
        MedRetires +=
            Op.Inputs[0].ConstVal == unsigned(Intrinsic::MIPSHistoricalRetire);
      }
  EXPECT_EQ(MedBegins, Begins);
  EXPECT_EQ(MedRetires, Retires);
  EXPECT_NE(SlotBlock, -1);
  EXPECT_NE(SlotBlock, Med.Blocks.front().Id);
  EXPECT_EQ(Med.Blocks.front().Ops.back().Opcode, NdOp::COND_BR);
}

INSTANTIATE_TEST_SUITE_P(Profiles, MIPSControl,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

} // namespace
