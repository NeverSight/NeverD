//===- AArch64DecodeOccurrenceTests.cpp - Operand detail ownership -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Classification, complete native detail and borrowed-buffer lifetimes must
/// remain distinct at the AArch64 lift boundary, including PAC/CPA operations.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/AArch64/AArch64NativeDecode.h"
#include "neverd/decode/Decoder.h"
#include "neverd/decode/NativeOperands.h"
#include "neverd/lift/AArch64/AArch64Lifter.h"
#include "neverd/support/BinaryEncoding.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

using namespace neverd;

namespace {

#define NEVERD_AARCH64_DECODE_VALUE(Type, Name, Value)                         \
  constexpr Type Name = Value;
#include "AArch64DecodeOccurrenceFixtures.def"

struct DecodeCase {
  const char *Name;
  uint32_t Word;
  uint32_t Replacement;
};
constexpr DecodeCase Cases[] = {
#define NEVERD_AARCH64_DECODE_CASE(Name, Word, Replacement)                    \
  {Name, Word, Replacement},
#include "AArch64DecodeOccurrenceFixtures.def"
};

enum class Route { Full, Fast, Light };
constexpr Route Routes[] = {Route::Full, Route::Fast, Route::Light};

int decode(Decoder &D, Route Path, const uint8_t *Bytes, size_t Size,
           va_t Address, DecodedInsn &I) {
  switch (Path) {
  case Route::Full:
    return D.decodeOne(Bytes, Size, Address, I);
  case Route::Fast:
    return D.decodeOneForLift(Bytes, Size, Address, I);
  case Route::Light:
    return D.decodeOneLight(Bytes, Size, Address, I);
  }
  return 0;
}

int decode(Decoder &D, Route Path, uint32_t Word, DecodedInsn &I) {
  std::array<uint8_t, kWordBytes> Bytes;
  writeLE<uint32_t>(Bytes.data(), Word);
  return decode(D, Path, Bytes.data(), Bytes.size(), kEntry, I);
}

void expectSameOps(const std::vector<LowOp> &Actual,
                   const std::vector<LowOp> &Expected) {
  ASSERT_EQ(Actual.size(), Expected.size());
  for (size_t Index = 0; Index != Actual.size(); ++Index) {
    const auto &A = Actual[Index];
    const auto &E = Expected[Index];
    EXPECT_EQ(A.Opcode, E.Opcode);
    EXPECT_EQ(A.Output, E.Output);
    EXPECT_EQ(A.Addr, E.Addr);
    EXPECT_EQ(A.Seq, E.Seq);
    EXPECT_EQ(A.MemoryOrdering, E.MemoryOrdering);
    ASSERT_EQ(A.NumInputs, E.NumInputs);
    for (unsigned Input = 0; Input != A.NumInputs; ++Input)
      EXPECT_EQ(A.Inputs[Input], E.Inputs[Input]);
  }
}

void expectRefusal(Decoder &D, const DecodedInsn &I) {
  std::vector<LowOp> Ops(1);
  Ops.front().Opcode = NdOp::RETURN;
  try {
    D.liftToLow(I, Ops);
    ADD_FAILURE();
  } catch (const UnliftedInstruction &Error) {
    EXPECT_EQ(Error.getAddr(), I.Addr);
  }
  ASSERT_EQ(Ops.size(), 1u);
  EXPECT_EQ(Ops.front().Opcode, NdOp::RETURN);
}

// A copied cs_insn alone still borrows detail. Only this two-object snapshot
// owns the decoded operands independently of Decoder's reusable allocation.
struct OwnedInsn {
  cs_detail Detail;
  cs_insn Raw;
  DecodedInsn Instruction;

  explicit OwnedInsn(const DecodedInsn &I)
      : Detail(*I.Raw->detail), Raw(*I.Raw), Instruction(I) {
    Raw.detail = &Detail;
    Instruction.Raw = &Raw;
  }
};

} // namespace

TEST(AArch64DecodeOccurrence, ClassificationNeverReusesOldOperands) {
  for (const auto &Case : Cases)
    for (Route Path : Routes)
      for (bool Strict : {false, true})
        for (bool Warm : {false, true})
          for (bool Detail : {false, true})
            for (bool Text : {false, true}) {
              SCOPED_TRACE(Case.Name);
              SCOPED_TRACE(static_cast<unsigned>(Path));
              SCOPED_TRACE(Strict);
              SCOPED_TRACE(Warm);
              SCOPED_TRACE(Detail);
              SCOPED_TRACE(Text);
              Decoder Reference;
              ASSERT_TRUE(Reference.init(Arch::AArch64));
              DecodedInsn Original;
              ASSERT_EQ(decode(Reference, Route::Full, Case.Word, Original),
                        kWordBytes);
              std::vector<LowOp> Expected;
              ASSERT_NO_THROW(Reference.liftToLow(Original, Expected));
              cs_insn Native{};
              cs_detail NativeDetail{};
              const bool HasNativeDetail =
                  a64native::tryDecode(Case.Word, kEntry, Native, NativeDetail);

              Decoder D;
              ASSERT_TRUE(D.init(Arch::AArch64));
              D.setStrict(Strict);
              DecodedInsn I;
              if (Warm)
                ASSERT_EQ(decode(D, Path, Case.Replacement, I), kWordBytes);
              D.setDetail(Detail);
              D.setText(Text);
              ASSERT_EQ(decode(D, Path, Case.Word, I), kWordBytes);
              ASSERT_EQ(I.Id, Original.Id);
              if (Detail &&
                  (Text || (Path == Route::Fast && HasNativeDetail))) {
                std::vector<LowOp> Actual;
                EXPECT_NO_THROW(D.liftToLow(I, Actual));
                expectSameOps(Actual, Expected);
              } else {
                expectRefusal(D, I);
              }
              D.setDetail(true);
              D.setText(true);
              ASSERT_EQ(decode(D, Path, Case.Word, I), kWordBytes);
              std::vector<LowOp> Restored;
              EXPECT_NO_THROW(D.liftToLow(I, Restored));
              expectSameOps(Restored, Expected);
            }
}

TEST(AArch64DecodeOccurrence,
     BorrowedRecordsExpireEvenAtTheSameAddressAndWord) {
  for (const auto &Case : Cases)
    for (Route Path : Routes)
      for (bool Strict : {false, true}) {
        SCOPED_TRACE(Case.Name);
        Decoder D;
        ASSERT_TRUE(D.init(Arch::AArch64));
        D.setStrict(Strict);
        DecodedInsn Original;
        ASSERT_EQ(decode(D, Path, Case.Word, Original), kWordBytes);
        OwnedInsn Owned(Original);
        auto ShallowRaw = *Original.Raw;
        auto Shallow = Original;
        Shallow.Raw = &ShallowRaw;
        std::vector<LowOp> Expected;
        ASSERT_NO_THROW(D.liftToLow(Original, Expected));

        DecodedInsn Replacement;
        ASSERT_EQ(decode(D, Path, Case.Replacement, Replacement), kWordBytes);
        D.setDetail(false);
        ASSERT_EQ(decode(D, Path, Case.Word, Replacement), kWordBytes);
        expectRefusal(D, Original);
        expectRefusal(D, Shallow);
        expectRefusal(D, Replacement);
        std::vector<LowOp> Actual;
        EXPECT_NO_THROW(D.liftToLow(Owned.Instruction, Actual));
        expectSameOps(Actual, Expected);

        D.setDetail(true);
        ASSERT_EQ(decode(D, Path, Case.Word, Replacement), kWordBytes);
        expectRefusal(D, Original);
        expectRefusal(D, Shallow);
        Actual.clear();
        EXPECT_NO_THROW(D.liftToLow(Replacement, Actual));
        expectSameOps(Actual, Expected);
      }
}

TEST(AArch64DecodeOccurrence, PrefixFollowerBelongsToOneBorrowedOccurrence) {
  for (Route Path : Routes)
    for (uint32_t Follower : {kOtherFollower, kInvalidWord}) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::AArch64));
      std::array<uint8_t, kPairBytes> Bytes;
      writeLE<uint32_t>(Bytes.data(), kPrefix);
      writeLE<uint32_t>(Bytes.data() + kWordBytes, kFollower);
      DecodedInsn Original;
      ASSERT_EQ(decode(D, Path, Bytes.data(), Bytes.size(), kEntry, Original),
                kWordBytes);
      OwnedInsn Owned(Original);
      std::vector<LowOp> Expected;
      ASSERT_NO_THROW(D.liftToLow(Original, Expected));
      writeLE<uint32_t>(Bytes.data() + kWordBytes, Follower);
      DecodedInsn New;
      ASSERT_EQ(decode(D, Path, Bytes.data(), Bytes.size(), kEntry, New),
                kWordBytes);
      expectRefusal(D, Original);
      std::vector<LowOp> Actual;
      EXPECT_NO_THROW(D.liftToLow(Owned.Instruction, Actual));
      expectSameOps(Actual, Expected);
      if (Follower == kInvalidWord) {
        expectRefusal(D, New);
      } else {
        Actual.clear();
        EXPECT_NO_THROW(D.liftToLow(New, Actual));
        expectSameOps(Actual, Expected);
      }
    }
}

TEST(AArch64DecodeOccurrence, ConfigurationChangesOnlyAffectFutureDecodes) {
  for (const auto &Case : Cases)
    for (Route Path : Routes) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::AArch64));
      DecodedInsn I;
      ASSERT_EQ(decode(D, Path, Case.Word, I), kWordBytes);
      std::vector<LowOp> Expected;
      ASSERT_NO_THROW(D.liftToLow(I, Expected));
      D.setDetail(false);
      D.setText(false);
      EXPECT_TRUE(D.hasAArch64OperandDetail(I));
      std::vector<LowOp> Actual;
      EXPECT_NO_THROW(D.liftToLow(I, Actual));
      expectSameOps(Actual, Expected);
    }
}

TEST(AArch64DecodeOccurrence, FailedAttemptsInvalidateBorrowedResults) {
  for (Route Path : Routes)
    for (size_t Size = 0; Size <= kWordBytes; ++Size) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::AArch64));
      DecodedInsn Original;
      ASSERT_EQ(decode(D, Path, Cases[0].Word, Original), kWordBytes);
      OwnedInsn Owned(Original);
      auto Failed = Original;
      std::array<uint8_t, kWordBytes> Invalid;
      writeLE<uint32_t>(Invalid.data(), kInvalidWord);
      ASSERT_EQ(decode(D, Path, Invalid.data(), Size, kEntry, Failed), 0);
      EXPECT_EQ(Failed.Raw, nullptr);
      EXPECT_FALSE(Failed.AArch64Occurrence);
      EXPECT_FALSE(Failed.AArch64Prefix);
      expectRefusal(D, Failed);
      expectRefusal(D, Original);
      std::vector<LowOp> Actual;
      EXPECT_NO_THROW(D.liftToLow(Owned.Instruction, Actual));
      EXPECT_FALSE(Actual.empty());
    }
}

TEST(AArch64DecodeOccurrence, OnlyOwnedSnapshotsSurviveDecoderReplacement) {
  for (Route Path : Routes) {
    std::optional<Decoder> Storage;
    Storage.emplace();
    ASSERT_TRUE(Storage->init(Arch::AArch64));
    DecodedInsn Original;
    ASSERT_EQ(decode(*Storage, Path, Cases[0].Word, Original), kWordBytes);
    OwnedInsn Owned(Original);
    auto ShallowRaw = *Original.Raw;
    auto Shallow = Original;
    Shallow.Raw = &ShallowRaw;
    std::vector<LowOp> Expected;
    ASSERT_NO_THROW(Storage->liftToLow(Original, Expected));

    // Reset/init can reuse the same Capstone allocation.
    Storage->reset();
    expectRefusal(*Storage, Original);
    EXPECT_FALSE(Storage->isAlignmentNop(Original));
    EXPECT_FALSE(Storage->isFunctionTerminator(Original));
    EXPECT_FALSE(Storage->isResumableTrap(Original));
    EXPECT_FALSE(Storage->returnsToCaller(Original));
    EXPECT_EQ(Storage->directCallTarget(Original), InvalidVA);
    const auto NoOperands =
        describeNativeOperands(*Storage, Original, Arch::AArch64);
    EXPECT_TRUE(NoOperands.Immediates.empty());
    EXPECT_TRUE(NoOperands.Registers.empty());
    ASSERT_TRUE(Storage->init(Arch::X64));
    expectRefusal(*Storage, Original);
    expectRefusal(*Storage, Owned.Instruction);
    ASSERT_TRUE(Storage->init(Arch::AArch64));
    DecodedInsn New;
    ASSERT_EQ(decode(*Storage, Path, Cases[0].Word, New), kWordBytes);
    expectRefusal(*Storage, Original);
    expectRefusal(*Storage, Shallow);
    std::vector<LowOp> Actual;
    EXPECT_NO_THROW(Storage->liftToLow(Owned.Instruction, Actual));
    expectSameOps(Actual, Expected);

    Original = New;
    // Reconstruct a Decoder at the very same address. The producer identity
    // must not be inferred from a recycled object or instruction address.
    Storage.reset();
    Storage.emplace();
    ASSERT_TRUE(Storage->init(Arch::AArch64));
    ASSERT_EQ(decode(*Storage, Path, Cases[0].Word, New), kWordBytes);
    expectRefusal(*Storage, Original);
    EXPECT_FALSE(Storage->isFunctionTerminator(Original));
    EXPECT_FALSE(Storage->returnsToCaller(Original));
    EXPECT_EQ(Storage->directCallTarget(Original), InvalidVA);
    EXPECT_FALSE(Storage->hasAArch64OperandDetail(Original));
    Actual.clear();
    EXPECT_NO_THROW(Storage->liftToLow(Owned.Instruction, Actual));
    expectSameOps(Actual, Expected);

    Decoder Other;
    ASSERT_TRUE(Other.init(Arch::AArch64));
    expectRefusal(Other, New);
    Actual.clear();
    EXPECT_NO_THROW(Other.liftToLow(Owned.Instruction, Actual));
    expectSameOps(Actual, Expected);
  }
}

TEST(AArch64DecodeOccurrence, ClassificationKeepsRawFactsWithoutOldOperands) {
  for (const auto &Case : Cases)
    for (Route Path : Routes) {
      SCOPED_TRACE(Case.Name);
      Decoder D;
      ASSERT_TRUE(D.init(Arch::AArch64));
      DecodedInsn I;
      ASSERT_EQ(decode(D, Path, Case.Word, I), kWordBytes);
      const bool Terminator = D.isFunctionTerminator(I);
      const bool Nop = D.isAlignmentNop(I);
      const bool Trap = D.isResumableTrap(I);
      const auto Returns = D.returnsToCaller(I);
      const va_t Target = D.directCallTarget(I);
      ASSERT_EQ(decode(D, Path, Case.Replacement, I), kWordBytes);
      D.setDetail(false);
      D.setText(false);
      ASSERT_EQ(decode(D, Path, Case.Word, I), kWordBytes);
      EXPECT_FALSE(D.hasAArch64OperandDetail(I));
      EXPECT_EQ(D.isFunctionTerminator(I), Terminator);
      EXPECT_EQ(D.isAlignmentNop(I), Nop);
      EXPECT_EQ(D.isResumableTrap(I), Trap);
      EXPECT_EQ(D.returnsToCaller(I), Returns);
      EXPECT_EQ(D.directCallTarget(I), Target);
      for (bool Registers : {false, true}) {
        const auto Operands =
            describeNativeOperands(D, I, Arch::AArch64, Registers);
        EXPECT_TRUE(Operands.Immediates.empty());
        EXPECT_TRUE(Operands.Registers.empty());
      }
      const auto Expired = I;
      ASSERT_EQ(decode(D, Path, Case.Replacement, I), kWordBytes);
      EXPECT_FALSE(D.isFunctionTerminator(Expired));
      EXPECT_FALSE(D.isAlignmentNop(Expired));
      EXPECT_FALSE(D.isResumableTrap(Expired));
      EXPECT_FALSE(D.returnsToCaller(Expired));
      EXPECT_EQ(D.directCallTarget(Expired), InvalidVA);
    }
}

TEST(AArch64DecodeOccurrence, RawCallDisplacementDoesNotDependOnOperandDetail) {
  for (Route Path : Routes)
    for (bool Detail : {false, true})
      for (bool Text : {false, true}) {
        Decoder D;
        ASSERT_TRUE(D.init(Arch::AArch64));
        DecodedInsn I;
        ASSERT_EQ(decode(D, Path, kOtherCall, I), kWordBytes);
        D.setDetail(Detail);
        D.setText(Text);
        ASSERT_EQ(decode(D, Path, kCall, I), kWordBytes);
        EXPECT_EQ(D.directCallTarget(I), kCallTarget);
        auto Raw = *I.Raw;
        Raw.detail = nullptr;
        EXPECT_EQ(AArch64Lifter::directCallTarget(&Raw), kCallTarget);
        Raw.id = AARCH64_INS_INVALID;
        EXPECT_EQ(AArch64Lifter::directCallTarget(&Raw), InvalidVA);
      }
}

TEST(AArch64DecodeOccurrence, IncompleteAndChangedRecordsRefuseBeforeOutput) {
  enum class Change { Address, Size, Id, Word, MissingEvidence, NullRaw };
  for (const auto &Case : Cases)
    for (const auto Kind :
         {Change::Address, Change::Size, Change::Id, Change::Word,
          Change::MissingEvidence, Change::NullRaw}) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::AArch64));
      DecodedInsn I;
      ASSERT_EQ(decode(D, Route::Full, Case.Word, I), kWordBytes);
      OwnedInsn Owned(I);
      auto &Changed = Owned.Instruction;
      switch (Kind) {
      case Change::Address:
        Changed.Addr += kWordBytes;
        break;
      case Change::Size:
        Changed.Size += kWordBytes;
        break;
      case Change::Id:
        Changed.Id = AARCH64_INS_INVALID;
        break;
      case Change::Word:
        writeLE<uint32_t>(Owned.Raw.bytes, Case.Replacement);
        break;
      case Change::MissingEvidence:
        Changed.AArch64Occurrence.reset();
        break;
      case Change::NullRaw:
        Changed.Raw = nullptr;
        break;
      }
      expectRefusal(D, Changed);
      EXPECT_FALSE(D.hasAArch64OperandDetail(Changed));
    }
  for (bool Strict : {false, true}) {
    AArch64Lifter Lifter(Arch::AArch64);
    Lifter.setStrict(Strict);
    std::vector<LowOp> Ops;
    EXPECT_THROW(Lifter.lift(nullptr, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}

TEST(AArch64DecodeOccurrence, MissingDetailNeverDropsAnInstruction) {
  for (const auto &Case : Cases)
    for (bool Strict : {false, true}) {
      SCOPED_TRACE(Case.Name);
      Decoder D;
      ASSERT_TRUE(D.init(Arch::AArch64));
      D.setStrict(Strict);
      DecodedInsn Original;
      ASSERT_EQ(decode(D, Route::Full, Case.Word, Original), kWordBytes);
      auto Raw = *Original.Raw;
      Raw.detail = nullptr;
      auto Missing = Original;
      Missing.Raw = &Raw;
      expectRefusal(D, Missing);
      AArch64Lifter Lifter(Arch::AArch64);
      Lifter.setStrict(Strict);
      std::vector<LowOp> Ops(1);
      Ops.front().Opcode = NdOp::RETURN;
      EXPECT_THROW(Lifter.lift(&Raw, Ops), UnliftedInstruction);
      ASSERT_EQ(Ops.size(), 1u);
      EXPECT_EQ(Ops.front().Opcode, NdOp::RETURN);
    }
}

TEST(AArch64DecodeOccurrence, OperandQueriesKeepTheOccurrenceArchitecture) {
  for (Route Path : Routes) {
    Decoder D;
    ASSERT_TRUE(D.init(Arch::AArch64));
    DecodedInsn I;
    ASSERT_EQ(decode(D, Path, Cases[0].Word, I), kWordBytes);
    OwnedInsn Owned(I);
    for (Arch Architecture : {Arch::Unknown, Arch::X64, Arch::X86, Arch::ARM})
      for (bool Registers : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        const auto Operands =
            describeNativeOperands(D, I, Architecture, Registers);
        ASSERT_TRUE(Operands.Immediates.empty());
        ASSERT_TRUE(Operands.Registers.empty());
      }
    const auto Operands = describeNativeOperands(D, I, Arch::AArch64);
    EXPECT_FALSE(Operands.Immediates.empty());
    D.reset();
    ASSERT_TRUE(D.init(Arch::X64));
    // Reject the mismatched architecture before reading a freed borrowed
    // instruction or interpreting an owned AArch64 detail as an x64 union.
    for (const auto &Instruction : {I, Owned.Instruction})
      for (Arch Architecture :
           {Arch::Unknown, Arch::X64, Arch::X86, Arch::ARM, Arch::AArch64}) {
        const auto Missing =
            describeNativeOperands(D, Instruction, Architecture);
        EXPECT_TRUE(Missing.Immediates.empty());
        EXPECT_TRUE(Missing.Registers.empty());
      }
  }
}

TEST(AArch64DecodeOccurrence, RefusalClearsPriorArchitectureEvidence) {
  // Seed real evidence from an earlier x64 lift. A failed AArch64 attempt
  // must not leave that unrelated complete certificate available to callers.
  Decoder SeedDecoder;
  ASSERT_TRUE(SeedDecoder.init(Arch::X64));
  std::array<uint8_t, sizeof(kX64And)> SeedBytes;
  writeLE<uint16_t>(SeedBytes.data(), kX64And);
  DecodedInsn Seed;
  ASSERT_EQ(
      SeedDecoder.decodeOne(SeedBytes.data(), SeedBytes.size(), kEntry, Seed),
      SeedBytes.size());
  LowInstructionUndefinedEffects SeedUndefined;
  LowInstructionPreservedState SeedPreserved;
  std::vector<LowOp> SeedOps;
  ASSERT_NO_THROW(SeedDecoder.liftToLow(Seed, SeedOps, {}, {}, &SeedUndefined,
                                        &SeedPreserved));
  ASSERT_EQ(SeedUndefined.Coverage, LowUndefinedCoverage::Complete);
  ASSERT_FALSE(SeedUndefined.Effects.empty());
  ASSERT_NE(SeedPreserved.Audit, LowPreservedStateAudit::Missing);

  enum class Failure { Classification, Expired, Reset, WrongArchitecture };
  for (Route Path : Routes)
    for (bool Strict : {false, true})
      for (Failure Kind : {Failure::Classification, Failure::Expired,
                           Failure::Reset, Failure::WrongArchitecture}) {
        SCOPED_TRACE(static_cast<unsigned>(Path));
        SCOPED_TRACE(Strict);
        SCOPED_TRACE(static_cast<unsigned>(Kind));
        Decoder D;
        ASSERT_TRUE(D.init(Arch::AArch64));
        D.setStrict(Strict);
        DecodedInsn I;
        ASSERT_EQ(decode(D, Path, Cases[0].Word, I), kWordBytes);
        switch (Kind) {
        case Failure::Classification:
          D.setDetail(false);
          ASSERT_EQ(decode(D, Path, Cases[0].Word, I), kWordBytes);
          break;
        case Failure::Expired: {
          DecodedInsn Next;
          ASSERT_EQ(decode(D, Path, Cases[0].Replacement, Next), kWordBytes);
          break;
        }
        case Failure::Reset:
          D.reset();
          break;
        case Failure::WrongArchitecture:
          ASSERT_TRUE(D.init(Arch::X64));
          break;
        }
        for (bool WithUndefined : {false, true})
          for (bool WithPreserved : {false, true}) {
            auto Undefined = SeedUndefined;
            auto Preserved = SeedPreserved;
            auto Ops = SeedOps;
            EXPECT_THROW(D.liftToLow(I, Ops, {}, {},
                                     WithUndefined ? &Undefined : nullptr,
                                     WithPreserved ? &Preserved : nullptr),
                         UnliftedInstruction);
            expectSameOps(Ops, SeedOps);
            if (WithUndefined) {
              EXPECT_EQ(Undefined.Coverage, LowUndefinedCoverage::Missing);
              EXPECT_EQ(Undefined.OpCount, 0u);
              EXPECT_TRUE(Undefined.OperationDigest.empty());
              EXPECT_TRUE(Undefined.Effects.empty());
            }
            if (WithPreserved)
              EXPECT_EQ(Preserved, LowInstructionPreservedState{});
          }
      }
}
