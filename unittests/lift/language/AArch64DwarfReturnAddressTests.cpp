//===- AArch64DwarfReturnAddressTests.cpp - Static RA CFI semantics -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bytes exercise static RA state and deliberate unknowns.
//===----------------------------------------------------------------------===//

#include "AArch64DwarfTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/loader/MachO/MachOExceptions.h"
#include "neverd/loader/SourceUnwind.h"

#include <string>
#include <utility>

namespace {
using namespace neverd;
using namespace neverd::aarch64_dwarf_test;
using namespace neverd::dwarf_eh;

TEST(AArch64DwarfReturnAddress, AbsoluteStatesRetainKeyRecipeAndSigningPC) {
  for (const char *Augmentation :
       {kOrdinary, kBKey, kNonHint, kBKeyNonHint, kNonHintBeforeEncoding}) {
    SCOPED_TRACE(Augmentation);
    const auto Parsed = decode(frame(kStates, {}, Augmentation));
    ASSERT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Complete);
    ASSERT_EQ(Parsed.CIEs.size(), 1u);
    ASSERT_EQ(Parsed.FDEs.size(), 1u);
    const auto SP = query(Parsed, 0);
    ASSERT_EQ(SP.Status, AArch64RAQueryStatus::Known);
    ASSERT_TRUE(SP.State);
    EXPECT_EQ(SP.State->Kind, AArch64RASigningKind::SP);
    EXPECT_FALSE(SP.State->SigningPC);
    const auto PC = query(Parsed, kCodeAlign);
    ASSERT_EQ(PC.Status, AArch64RAQueryStatus::Known);
    ASSERT_TRUE(PC.State);
    EXPECT_EQ(PC.State->Kind, AArch64RASigningKind::SPAndPC);
    ASSERT_TRUE(PC.State->SigningPC);
    EXPECT_EQ(*PC.State->SigningPC, kText);
    const std::string Aug(Augmentation);
    EXPECT_EQ(PC.State->Key, Aug.find(kBAugmentation) == std::string::npos
                                 ? AArch64RAKey::A
                                 : AArch64RAKey::B);
    EXPECT_EQ(PC.State->Recipe, Aug.find(kNAugmentation) == std::string::npos
                                    ? AArch64RARecipe::HintPACM
                                    : AArch64RARecipe::NonHintPAuthLR);
    const auto Unsigned = query(Parsed, kCodeAlign * 2);
    ASSERT_TRUE(Unsigned.State);
    EXPECT_EQ(Unsigned.State->Kind, AArch64RASigningKind::Unsigned);
    EXPECT_FALSE(Unsigned.State->SigningPC);
  }
}

TEST(AArch64DwarfReturnAddress, LegacyTogglesCaptureTheCurrentCodeLocation) {
  const auto Parsed = decode(frame(kLegacy));
  ASSERT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Complete);
  const auto SP = query(Parsed, 0);
  ASSERT_TRUE(SP.State);
  EXPECT_EQ(SP.State->Kind, AArch64RASigningKind::SP);
  const auto Unsigned = query(Parsed, kCodeAlign);
  ASSERT_TRUE(Unsigned.State);
  EXPECT_EQ(Unsigned.State->Kind, AArch64RASigningKind::Unsigned);
  const auto PC = query(Parsed, kCodeAlign * 2);
  ASSERT_TRUE(PC.State);
  EXPECT_EQ(PC.State->SigningPC, kText + kCodeAlign * 2);
  const auto End = query(Parsed, kCodeAlign * 3);
  ASSERT_TRUE(End.State);
  EXPECT_EQ(End.State->Kind, AArch64RASigningKind::Unsigned);
}

TEST(AArch64DwarfReturnAddress, CIEAndFDERememberRestoreTheWholeSigningRecipe) {
  const auto Parsed = decode(frame(kRestoreInitialRemember, kInitialRemember));
  ASSERT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Complete);
  const auto Before = query(Parsed, 0);
  ASSERT_TRUE(Before.State);
  EXPECT_EQ(Before.State->SigningPC, kText);
  const auto After = query(Parsed, kCodeAlign);
  ASSERT_TRUE(After.State);
  EXPECT_EQ(After.State->Kind, AArch64RASigningKind::SP);
  EXPECT_FALSE(After.State->SigningPC);

  const auto Nested = decode(frame(kNested, kInitialSP));
  const auto Restored = query(Nested, kCodeAlign * 2);
  ASSERT_TRUE(Restored.State);
  EXPECT_EQ(Restored.State->SigningPC, kText);
  const auto Initial = query(Nested, kCodeAlign * 3);
  ASSERT_TRUE(Initial.State);
  EXPECT_EQ(Initial.State->Kind, AArch64RASigningKind::SP);
}

TEST(AArch64DwarfReturnAddress, RuntimeRulesStayUnknownUntilAnExplicitRestore) {
  for (llvm::ArrayRef<uint8_t> Program :
       {llvm::ArrayRef<uint8_t>(kDynamic),
        llvm::ArrayRef<uint8_t>(kExpression)}) {
    const auto Parsed = decode(frame(Program, kInitialSP));
    ASSERT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Complete);
    const auto Dynamic = query(Parsed, 0);
    EXPECT_EQ(Dynamic.Status, AArch64RAQueryStatus::Unknown);
    EXPECT_FALSE(Dynamic.State);
    const auto Restored = query(Parsed, kCodeAlign);
    ASSERT_TRUE(Restored.State);
    EXPECT_EQ(Restored.State->Kind, AArch64RASigningKind::SP);
  }
  const auto Mixed = decode(frame(kMixed));
  EXPECT_EQ(query(Mixed, 0).Status, AArch64RAQueryStatus::Malformed);
}

TEST(AArch64DwarfReturnAddress, SetLocUsesTheFDEBaseIncludingAddressZero) {
  for (uint64_t Base : {uint64_t(0), kText}) {
    language_eh_test::ByteBuilder Program;
    Program.u8(kSetLocation);
    Program.u64(Base + kCodeAlign * 2);
    for (uint8_t Byte : absoluteState(kSignedWithPC, -1))
      Program.u8(Byte);
    const auto Parsed =
        decode(frame(Program.data(), {}, kOrdinary, kCodeAlign, Base));
    ASSERT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Complete);
    ASSERT_EQ(Parsed.FDEs.front().Instructions.size(), 2u);
    EXPECT_EQ(Parsed.FDEs.front().Instructions.back().CodeOffset,
              kCodeAlign * 2);
    const auto Before = query(Parsed, 0);
    ASSERT_TRUE(Before.State);
    EXPECT_EQ(Before.State->Kind, AArch64RASigningKind::Unsigned);
    const auto After = query(Parsed, kCodeAlign * 2);
    ASSERT_TRUE(After.State);
    EXPECT_EQ(After.State->SigningPC, Base + kCodeAlign);
    auto Unchanged = Parsed;
    Unchanged.FDEs.front().Instructions.front().Offset =
        static_cast<int64_t>(Base);
    EXPECT_EQ(query(Unchanged, 0).Status, AArch64RAQueryStatus::Malformed);
    const auto Initial = decode(frame({}, Program.data()));
    EXPECT_EQ(Initial.ParseStatus, ExceptionParseStatus::Partial);
    EXPECT_EQ(query(Initial, 0).Status, AArch64RAQueryStatus::Unsupported);
  }
  language_eh_test::ByteBuilder Unchanged;
  Unchanged.u8(kSetLocation);
  Unchanged.u64(kText);
  EXPECT_EQ(decode(frame(Unchanged.data())).ParseStatus,
            ExceptionParseStatus::Malformed);
  language_eh_test::ByteBuilder BeforeBase;
  BeforeBase.u8(kSetLocation);
  BeforeBase.u64(kText - 1);
  const auto Invalid = decode(frame(BeforeBase.data()));
  EXPECT_EQ(Invalid.ParseStatus, ExceptionParseStatus::Malformed);
  language_eh_test::ByteBuilder Backward;
  Backward.u8(kSetLocation);
  Backward.u64(kText + kCodeAlign * 2);
  Backward.u8(kSetLocation);
  Backward.u64(kText + kCodeAlign);
  EXPECT_EQ(decode(frame(Backward.data())).ParseStatus,
            ExceptionParseStatus::Malformed);
}

TEST(AArch64DwarfReturnAddress,
     MalformedLEBAndScaledOffsetsCannotPublishState) {
  for (llvm::ArrayRef<uint8_t> Program :
       {llvm::ArrayRef<uint8_t>(kMissingState),
        llvm::ArrayRef<uint8_t>(kMissingOffset),
        llvm::ArrayRef<uint8_t>(kTruncatedState),
        llvm::ArrayRef<uint8_t>(kTruncatedOffset),
        llvm::ArrayRef<uint8_t>(kLongOffset)}) {
    const auto Parsed = decode(frame(Program));
    EXPECT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Malformed);
    EXPECT_EQ(query(Parsed, 0).Status, AArch64RAQueryStatus::Malformed);
  }
  for (int64_t Offset : {INT64_MIN, INT64_MAX}) {
    const auto Parsed = decode(frame(absoluteState(kSignedWithPC, Offset)));
    EXPECT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Malformed);
    EXPECT_EQ(query(Parsed, 0).Status, AArch64RAQueryStatus::Malformed);
  }
  // Ignored offsets for unsigned/SP states are consumed but never multiplied.
  for (uint64_t State : {kUnsignedState, kSignedWithSP}) {
    const auto Parsed = decode(frame(absoluteState(State, INT64_MIN)));
    EXPECT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(query(Parsed, 0).Status, AArch64RAQueryStatus::Known);
  }
}

TEST(AArch64DwarfReturnAddress, ReservedStatesRemainPartialAndRetainOperands) {
  for (bool InCIE : {false, true}) {
    const llvm::ArrayRef<uint8_t> Program(kReservedContinuation);
    const auto Parsed =
        InCIE ? decode(frame({}, Program)) : decode(frame(Program));
    ASSERT_EQ(Parsed.CIEs.size(), 1u);
    ASSERT_EQ(Parsed.FDEs.size(), 1u);
    EXPECT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Partial);
    EXPECT_EQ(Parsed.CIEs.front().ParseStatus,
              InCIE ? ExceptionParseStatus::Partial
                    : ExceptionParseStatus::Complete);
    EXPECT_EQ(Parsed.FDEs.front().ParseStatus, ExceptionParseStatus::Partial);
    const auto &Instructions = InCIE ? Parsed.CIEs.front().InitialInstructions
                                     : Parsed.FDEs.front().Instructions;
    ASSERT_EQ(Instructions.size(), 3u);
    EXPECT_EQ(Instructions.front().Kind, CFIOpKind::SetRAState);
    EXPECT_EQ(Instructions.front().RAState, kReservedOperands[0]);
    EXPECT_EQ(llvm::ArrayRef<uint8_t>(Instructions.front().OperandBytes),
              llvm::ArrayRef<uint8_t>(kReservedOperands));
    EXPECT_EQ(Instructions.back().CodeOffset, kCodeAlign);
    EXPECT_EQ(Instructions.back().Kind, CFIOpKind::SetRAState);
    EXPECT_EQ(query(Parsed, 0).Status, AArch64RAQueryStatus::Unsupported);
  }
}

TEST(AArch64DwarfReturnAddress,
     DarwinReservedStateCannotBecomePlainSourceUnwind) {
  for (bool Reserved : {false, true})
    for (bool InCIE : {false, true}) {
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::MachO;
      Image.Base = kText;
      Segment Text;
      Text.Name = kMachOTextSegment;
      Text.VA = kText;
      Text.Size = kRange;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Image.Segments.push_back(std::move(Text));
      const llvm::ArrayRef<uint8_t> Program =
          Reserved ? llvm::ArrayRef<uint8_t>(kReserved)
                   : llvm::ArrayRef<uint8_t>(kInitialSP);
      Section Frames;
      Frames.Name = kMachOFrameSection;
      Frames.VA = kFrame;
      Frames.Data = InCIE ? frame({}, Program) : frame(Program);
      Frames.Size = Frames.Data.size();
      Image.Sections.push_back(std::move(Frames));

      macho_unwind::parseDarwinExceptions(Image);
      ASSERT_EQ(Image.ExceptionMetadata.Functions.size(), 1u);
      const auto &Function = Image.ExceptionMetadata.Functions.front();
      ASSERT_TRUE(Function.Dwarf);
      EXPECT_EQ(Function.Encoding, ExceptionEncoding::DwarfFDE);
      EXPECT_EQ(Function.CodeRange.Begin, kText);
      EXPECT_EQ(Function.CodeRange.End, kText + kRange);
      EXPECT_EQ(Function.ParseStatus, Reserved
                                          ? ExceptionParseStatus::Partial
                                          : ExceptionParseStatus::Complete);
      EXPECT_EQ(isPlainSourceUnwind(Function), !Reserved);
      if (Reserved) {
        ASSERT_TRUE(Function.DecodeProvenance);
        EXPECT_EQ(Function.DecodeProvenance->Structural.ParseStatus,
                  ExceptionParseStatus::Partial);
      }
    }
}

TEST(AArch64DwarfReturnAddress, ZeroCodeAlignmentCannotProduceAState) {
  for (uint64_t State : {kUnsignedState, kSignedWithSP, kSignedWithPC}) {
    const auto Parsed =
        decode(frame(absoluteState(State, 0), {}, kOrdinary, 0));
    EXPECT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Malformed);
    EXPECT_TRUE(Parsed.CIEs.empty());
    EXPECT_TRUE(Parsed.FDEs.empty());
    auto Valid = decode(frame(absoluteState(State, 0)));
    ASSERT_EQ(Valid.CIEs.size(), 1u);
    ASSERT_EQ(Valid.FDEs.size(), 1u);
    Valid.CIEs.front().CodeAlignmentFactor = 0;
    EXPECT_EQ(query(Valid, 0).Status, AArch64RAQueryStatus::Malformed);
  }
}

TEST(AArch64DwarfReturnAddress, SigningPCArithmeticIsCheckedAtBothBounds) {
  for (const auto &[Base, Offset] :
       {std::pair{uint64_t(0), INT64_MIN},
        std::pair{UINT64_MAX - kRange, INT64_MAX}}) {
    const auto Parsed = decode(
        frame(absoluteState(kSignedWithPC, Offset), {}, kOrdinary, 1, Base));
    ASSERT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(query(Parsed, 0).Status, AArch64RAQueryStatus::Malformed);
  }
  const auto Zero = decode(
      frame(absoluteState(kSignedWithPC, -int64_t(kText)), {}, kOrdinary, 1));
  const auto State = query(Zero, 0);
  ASSERT_TRUE(State.State);
  ASSERT_TRUE(State.State->SigningPC);
  EXPECT_EQ(*State.State->SigningPC, 0u);
}

TEST(AArch64DwarfReturnAddress, QueryValidatesWholeProgramsAndResourceBounds) {
  const auto Late = decode(frame(kLateUnderflow));
  EXPECT_EQ(query(Late, 0).Status, AArch64RAQueryStatus::Malformed);
  const auto Reserved = decode(frame(kReserved));
  EXPECT_EQ(query(Reserved, 0).Status, AArch64RAQueryStatus::Unsupported);
  const auto Unknown = decode(frame(kOpaque));
  EXPECT_EQ(query(Unknown, 0).Status, AArch64RAQueryStatus::Unsupported);
  auto Parsed = decode(frame(kRestoreInitialRemember, kInitialRemember));
  AArch64RAQueryLimits Limits;
  Limits.MaxInstructions = Parsed.CIEs.front().InitialInstructions.size() +
                           Parsed.FDEs.front().Instructions.size();
  Limits.MaxRememberDepth = 1;
  EXPECT_EQ(query(Parsed, 0, Limits).Status, AArch64RAQueryStatus::Known);
  --Limits.MaxInstructions;
  EXPECT_EQ(query(Parsed, 0, Limits).Status,
            AArch64RAQueryStatus::ResourceLimit);
  ++Limits.MaxInstructions;
  Limits.MaxRememberDepth = 0;
  EXPECT_EQ(query(Parsed, 0, Limits).Status,
            AArch64RAQueryStatus::ResourceLimit);
  EXPECT_EQ(query(Parsed, kRange).Status, AArch64RAQueryStatus::OutOfRange);
  EXPECT_EQ(evaluateAArch64ReturnAddressState(Arch::AArch64,
                                              Parsed.CIEs.front(),
                                              Parsed.FDEs.front(), kText - 1)
                .Status,
            AArch64RAQueryStatus::OutOfRange);
  Parsed.FDEs.front().CIESectionOffset++;
  EXPECT_EQ(query(Parsed, 0).Status, AArch64RAQueryStatus::Malformed);
}

TEST(AArch64DwarfReturnAddress,
     OtherArchitecturesDoNotAcquireAArch64Semantics) {
  for (Arch Architecture : {Arch::X64, Arch::ARM, Arch::Unknown}) {
    for (llvm::ArrayRef<uint8_t> Program :
         {llvm::ArrayRef<uint8_t>(kWindowSave),
          llvm::ArrayRef<uint8_t>(kWindowSaveWithPC),
          llvm::ArrayRef<uint8_t>(kInitialSP)}) {
      const auto Parsed = decode(frame(Program), Architecture);
      EXPECT_EQ(Parsed.ParseStatus, ExceptionParseStatus::Partial);
      ASSERT_EQ(Parsed.FDEs.size(), 1u);
      ASSERT_EQ(Parsed.FDEs.front().Instructions.size(), 1u);
      EXPECT_EQ(Parsed.FDEs.front().Instructions.front().Kind,
                CFIOpKind::Opaque);
      EXPECT_EQ(evaluateAArch64ReturnAddressState(Architecture,
                                                  Parsed.CIEs.front(),
                                                  Parsed.FDEs.front(), kText)
                    .Status,
                AArch64RAQueryStatus::Unsupported);
    }
    const auto Augmented = decode(frame({}, {}, kBKeyNonHint), Architecture);
    EXPECT_EQ(Augmented.ParseStatus, ExceptionParseStatus::Partial);
    ASSERT_EQ(Augmented.CIEs.size(), 1u);
    EXPECT_FALSE(Augmented.CIEs.front().HasPointerAuth);
    EXPECT_FALSE(Augmented.CIEs.front().UsesNonHintPAuthLR);
  }
}

} // namespace
