//===- X86InstructionCoverageTests.cpp - Real encoding coverage ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode the pinned Capstone x86 MC corpus and verify strict lifting or an
/// explicit architectural refusal. Count actual lifted encodings separately.
/// This does not claim complete ISA, emitter or executable semantic coverage.
//===----------------------------------------------------------------------===//

#include "../x86/X86LegacyAdmissionFixtures.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Lifter.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace neverd;

TEST(X86InstructionCoverage, RefusalsKeepThePriorLowIRInBothPolicies) {
  // The bad destination follows a real address calculation and memory read,
  // so rejection must roll back an already-started instruction transaction.
  const std::vector<std::vector<uint8_t>> Cases = {
      {0x90},
      {0x8b, 0x04, 0x08},
      {0x0f, 0x01, 0xd8}, // VMRUN has no owned continuation.
      {0x31, 0xc0}};
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (unsigned Route = 0; Route != 3; ++Route)
        for (unsigned Kind = 0; Kind != Cases.size(); ++Kind) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(Strict);
          SCOPED_TRACE(Route);
          SCOPED_TRACE(Kind);
          Decoder D;
          ASSERT_TRUE(D.init(A));
          D.setStrict(Strict);
          const auto &Bytes = Cases[Kind];
          DecodedInsn I{};
          const auto Size =
              Route == 0 ? D.decodeOne(Bytes.data(), Bytes.size(), 0x1000, I)
              : Route == 1
                  ? D.decodeOneLight(Bytes.data(), Bytes.size(), 0x1000, I)
                  : D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I);
          ASSERT_EQ(Size, Bytes.size());
          if (Kind == 0)
            I.Raw->id = X86_INS_INVALID;
          if (Kind == 1)
            I.Raw->detail->x86.operands[0].reg = X86_REG_INVALID;
          if (Kind == 3)
            I.Raw->detail->x86.op_count = 1;
          LowOp Prior;
          Prior.Opcode = NdOp::COPY;
          Prior.Addr = 0xff0;
          Prior.Output = NdVar::tmp(123, 4);
          Prior.addInput(NdVar::scalar(42, 4));
          std::vector<LowOp> Ops{Prior};
          LowInstructionUndefinedEffects Undefined;
          Undefined.Coverage = LowUndefinedCoverage::Complete;
          Undefined.OpCount = 99;
          LowInstructionPreservedState Preserved;
          Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
          EXPECT_THROW(D.liftToLow(I, Ops, {}, {}, &Undefined, &Preserved),
                       UnliftedInstruction);
          ASSERT_EQ(Ops.size(), 1u);
          EXPECT_EQ(Ops.front().Opcode, Prior.Opcode);
          EXPECT_EQ(Ops.front().Addr, Prior.Addr);
          EXPECT_EQ(Ops.front().Output, Prior.Output);
          ASSERT_EQ(Ops.front().NumInputs, Prior.NumInputs);
          EXPECT_EQ(Ops.front().Inputs[0], Prior.Inputs[0]);
          EXPECT_EQ(Undefined.Coverage, LowUndefinedCoverage::Unsupported);
          EXPECT_EQ(Undefined.OpCount, 0u);
          EXPECT_TRUE(Undefined.Effects.empty());
          EXPECT_TRUE(Undefined.OperationDigest.empty());
          EXPECT_EQ(Preserved, LowInstructionPreservedState{});
        }
}

TEST(X86InstructionCoverage, MissingDetailIsAnExplicitFailureInBothPolicies) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true}) {
      Decoder D;
      ASSERT_TRUE(D.init(A));
      D.setStrict(Strict);
      const uint8_t Byte = 0x90;
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOne(&Byte, 1, 0x1000, I), 1);
      cs_insn Raw = *I.Raw;
      Raw.detail = nullptr;
      I.Raw = &Raw;
      std::vector<LowOp> Ops;
      EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
      EXPECT_THROW(D.liftToLow({}, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
}

TEST(X86InstructionCoverage, ArchitecturalNopsRemainSupportedInBothPolicies) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (const std::vector<uint8_t> &Bytes : {std::vector<uint8_t>{0x90},
                                                {0x66, 0x90},
                                                {0x67, 0x90},
                                                {0x0f, 0x1f, 0x00}}) {
        Decoder D;
        ASSERT_TRUE(D.init(A));
        D.setStrict(Strict);
        DecodedInsn I{};
        ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
                  Bytes.size());
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        ASSERT_EQ(Ops.size(), 1u);
        EXPECT_EQ(Ops.front().Opcode, NdOp::NOP);
      }
}

TEST(X86InstructionCoverage, StandalonePrefixesCannotBecomeNops) {
  struct Prefix {
    uint8_t Byte;
    unsigned Id;
  };
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (unsigned Route = 0; Route != 3; ++Route)
        for (const auto &P :
             {Prefix{0x66, X86_INS_DATA16}, Prefix{0xf0, X86_INS_LOCK},
              Prefix{0xf3, X86_INS_REP}, Prefix{0xf2, X86_INS_REPNE},
              Prefix{0x48, X86_INS_REX64}}) {
          SCOPED_TRACE(unsigned(A));
          SCOPED_TRACE(Strict);
          SCOPED_TRACE(Route);
          SCOPED_TRACE(P.Id);
          Decoder D;
          ASSERT_TRUE(D.init(A));
          D.setStrict(Strict);
          const auto Decode = [&](const uint8_t *Bytes, size_t Size,
                                  DecodedInsn &I) {
            return Route == 0   ? D.decodeOne(Bytes, Size, 0x1000, I)
                   : Route == 1 ? D.decodeOneLight(Bytes, Size, 0x1000, I)
                                : D.decodeOneForLift(Bytes, Size, 0x1000, I);
          };
          if (A == Arch::X64 || P.Id != X86_INS_REX64) {
            DecodedInsn Truncated{};
            EXPECT_EQ(Decode(&P.Byte, 1, Truncated), 0u);
          }
          // Check the lifter boundary independently of decoder rejection.
          // Even a stale or forged prefix ID must not acquire NOP semantics.
          const uint8_t Nop = 0x90;
          DecodedInsn I{};
          ASSERT_EQ(Decode(&Nop, 1, I), 1u);
          I.Raw->id = P.Id;
          LowInstructionUndefinedEffects Undefined;
          Undefined.Coverage = LowUndefinedCoverage::Complete;
          LowInstructionPreservedState Preserved;
          Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
          std::vector<LowOp> Ops;
          EXPECT_THROW(D.liftToLow(I, Ops, {}, {}, &Undefined, &Preserved),
                       UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
          EXPECT_EQ(Undefined.Coverage, LowUndefinedCoverage::Unsupported);
          EXPECT_EQ(Preserved, LowInstructionPreservedState{});
        }
}

TEST(X86InstructionCoverage, RexRDoesNotCreateExtraDebugRegisters) {
  for (unsigned Index = 0; Index < 8; ++Index)
    for (bool Write : {false, true})
      for (bool ForLift : {false, true}) {
        const std::vector<uint8_t> Bytes = {0x44, 0x0f,
                                            uint8_t(Write ? 0x23 : 0x21),
                                            uint8_t(0xc0 + 8 * Index)};
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X64));
        DecodedInsn I{};
        const auto Length =
            ForLift ? D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I)
                    : D.decodeOne(Bytes.data(), Bytes.size(), 0x1000, I);
        if (!Length)
          continue;
        ASSERT_EQ(Length, Bytes.size());
        std::vector<LowOp> Ops;
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
}

TEST(X86InstructionCoverage, MpxRequiresOwnedBoundsStateAndFaultSemantics) {
  struct Fixture {
    std::vector<uint8_t> Bytes;
    unsigned Id;
  };
  const Fixture Cases[] = {
      {{0xf3, 0x0f, 0x1a, 0xc0}, X86_INS_BNDCL},
      {{0xf3, 0x0f, 0x1a, 0x00}, X86_INS_BNDCL},
      {{0xf2, 0x0f, 0x1a, 0xc0}, X86_INS_BNDCU},
      {{0xf2, 0x0f, 0x1a, 0x00}, X86_INS_BNDCU},
      {{0xf2, 0x0f, 0x1b, 0xc0}, X86_INS_BNDCN},
      {{0xf2, 0x0f, 0x1b, 0x00}, X86_INS_BNDCN},
      {{0xf3, 0x0f, 0x1b, 0x00}, X86_INS_BNDMK},
      {{0x66, 0x0f, 0x1a, 0xc1}, X86_INS_BNDMOV},
      {{0x66, 0x0f, 0x1a, 0x00}, X86_INS_BNDMOV},
      {{0x66, 0x0f, 0x1b, 0x00}, X86_INS_BNDMOV},
      {{0x0f, 0x1a, 0x04, 0x08}, X86_INS_BNDLDX},
      {{0x0f, 0x1b, 0x04, 0x08}, X86_INS_BNDSTX},
  };
  for (auto A : {Arch::X86, Arch::X64})
    for (const auto &F : Cases)
      for (unsigned Register = 0; Register != 4; ++Register)
        for (bool ForLift : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(F.Id);
          SCOPED_TRACE(Register);
          SCOPED_TRACE(ForLift);
          auto Bytes = F.Bytes;
          Bytes[Bytes[0] == 0x0f ? 2 : 3] |= Register << 3;
          Decoder D;
          ASSERT_TRUE(D.init(A));
          DecodedInsn I{};
          ASSERT_EQ(
              ForLift
                  ? D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I)
                  : D.decodeOne(Bytes.data(), Bytes.size(), 0x1000, I),
              Bytes.size());
          ASSERT_EQ(I.Id, F.Id);
          std::vector<LowOp> Ops;
          EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
}

TEST(X86InstructionCoverage, PinnedDecoderCorpusHasExplicitLiftResults) {
  const std::filesystem::path Directory = NEVERD_X86_MC_DIRECTORY;
  ASSERT_TRUE(std::filesystem::is_directory(Directory));
  size_t Samples = 0;
  size_t Refused = 0;
  std::set<unsigned> Ids;
  for (const auto &File : std::filesystem::directory_iterator(Directory)) {
    if (File.path().extension() != ".cs")
      continue;
    std::ifstream Input(File.path());
    ASSERT_TRUE(Input);
    std::string Line;
    ASSERT_TRUE(std::getline(Input, Line));
    Arch A = Line.find("CS_MODE_64") != std::string::npos   ? Arch::X64
             : Line.find("CS_MODE_32") != std::string::npos ? Arch::X86
                                                            : Arch::Unknown;
    if (A == Arch::Unknown)
      continue; // 16-bit mode has no NeverD target.
    Decoder D;
    ASSERT_TRUE(D.init(A));
    unsigned Number = 1;
    while (std::getline(Input, Line)) {
      ++Number;
      if (!Line.starts_with("0x"))
        continue;
      const auto Equal = Line.find('=');
      ASSERT_NE(Equal, std::string::npos);
      std::string Encoding = Line.substr(0, Equal);
      std::replace(Encoding.begin(), Encoding.end(), ',', ' ');
      std::istringstream Hex(Encoding);
      std::vector<uint8_t> Bytes;
      unsigned Byte;
      while (Hex >> std::hex >> Byte) {
        ASSERT_LE(Byte, 255u);
        Bytes.push_back(Byte);
      }
      SCOPED_TRACE(File.path().filename().string() + ":" +
                   std::to_string(Number));
      SCOPED_TRACE(Line);
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
                Bytes.size());
      std::vector<LowOp> Ops;
      const bool LegacyGap =
          std::ranges::any_of(test::X86FarPointerEncodings,
                              [&](const auto &E) { return E.Id == I.Id; }) ||
          std::ranges::any_of(test::X86ThreeDNowEncodings,
                              [&](const auto &E) { return E.Id == I.Id; });
      // These instructions need the owned x87 state, even with no explicit
      // operands. The corpus census deliberately uses the unowned entrypoint.
      const bool UnownedX87State =
          I.Id == X86_INS_FNOP || I.Id == X86_INS_EMMS || I.Id == X86_INS_FEMMS;
      // Far, interrupt and syscall returns take the ordinary lift path through
      // analysis-only terminal intrinsics. This census does not qualify native
      // restoration or continuation. SYSENTER and RSM enter through an
      // MSR/SMRAM target with no named operand and stay refused.
      if (I.Id == X86_INS_SYSENTER || I.Id == X86_INS_RSM || LegacyGap ||
          UnownedX87State) {
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        ++Refused;
        continue;
      }
      EXPECT_NO_THROW(D.liftToLow(I, Ops));
      Ids.insert(I.Id);
      ++Samples;
    }
  }
  EXPECT_GT(Samples, 0u);
  RecordProperty("encoding_samples", Samples);
  RecordProperty("instruction_ids", Ids.size());
  RecordProperty("explicitly_refused_samples", Refused);
}
