//===- X86_64_UndefinedEffectsTests.cpp - arbitrary-result metadata -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowUndefinedEffects.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstdint>
#include <vector>

using namespace neverd;

namespace {

struct InstructionCase {
  const char *Name;
  std::vector<uint8_t> Bytes;
};

class X86UndefinedEffects : public testing::Test {
protected:
  Decoder Dec;

  void SetUp() override { ASSERT_TRUE(Dec.init(Arch::X64)); }

  bool decode(const std::vector<uint8_t> &Bytes, DecodedInsn &Insn) {
    return Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn) ==
           static_cast<int>(Bytes.size());
  }
};

void expectSameOps(llvm::ArrayRef<LowOp> Expected,
                   llvm::ArrayRef<LowOp> Actual) {
  ASSERT_EQ(Expected.size(), Actual.size());
  for (size_t I = 0; I < Expected.size(); ++I) {
    SCOPED_TRACE(I);
    const LowOp &A = Expected[I];
    const LowOp &B = Actual[I];
    EXPECT_EQ(A.Opcode, B.Opcode);
    EXPECT_EQ(A.MemoryOrdering, B.MemoryOrdering);
    EXPECT_EQ(A.MemoryAddressSpace, B.MemoryAddressSpace);
    EXPECT_EQ(A.Output, B.Output);
    EXPECT_EQ(A.Addr, B.Addr);
    EXPECT_EQ(A.Seq, B.Seq);
    ASSERT_EQ(A.NumInputs, B.NumInputs);
    for (uint8_t J = 0; J < A.NumInputs; ++J)
      EXPECT_EQ(A.Inputs[J], B.Inputs[J]);
  }
}

LowInstructionUndefinedEffects staleEffects() {
  LowInstructionUndefinedEffects Effects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.OpCount = 99;
  Effects.OperationDigest = "stale operation digest";
  Effects.Effects.push_back(
      {99, NdVar::reg(x86reg::AF, 1), 0, 1, NdVar::scalar(1, 1)});
  Effects.Diagnostic = "stale certificate";
  return Effects;
}

void expectCleared(const LowInstructionUndefinedEffects &Effects) {
  EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_NE(Effects.OpCount, 99U);
  EXPECT_NE(Effects.OperationDigest, "stale operation digest");
  EXPECT_NE(Effects.Diagnostic, "stale certificate");
}

TEST_F(X86UndefinedEffects, LogicalInstructionsProduceOnlyAuxiliaryCarryBit) {
  const InstructionCase Cases[] = {
      {"and eax, ecx", {0x21, 0xc8}}, {"or eax, ecx", {0x09, 0xc8}},
      {"xor eax, ecx", {0x31, 0xc8}}, {"test eax, ecx", {0x85, 0xc8}},
      {"xor eax, eax", {0x31, 0xc0}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    std::vector<LowOp> PlainOps, CertifiedOps;
    Dec.liftToLow(Insn, PlainOps);
    LowInstructionUndefinedEffects Effects;
    Dec.liftToLow(Insn, CertifiedOps, {}, {}, &Effects);
    expectSameOps(PlainOps, CertifiedOps);
    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
    EXPECT_EQ(Effects.OpCount, CertifiedOps.size());
    EXPECT_EQ(Effects.OperationDigest,
              lowUndefinedOperationDigest(CertifiedOps));
    ASSERT_EQ(Effects.Effects.size(), 1U);
    const LowUndefinedEffect &Effect = Effects.Effects.front();
    EXPECT_EQ(Effect.Output, NdVar::reg(x86reg::AF, 1));
    EXPECT_EQ(Effect.BitOffset, 0U);
    EXPECT_EQ(Effect.BitCount, 1U);
    EXPECT_FALSE(Effect.When.has_value());
    EXPECT_GT(Effect.AfterOp, 0U);
    EXPECT_LE(Effect.AfterOp, Effects.OpCount);
  }
}

TEST_F(X86UndefinedEffects, DefinedAndPreservedFlagsCreateNoArbitraryProducer) {
  const InstructionCase Cases[] = {
      {"add eax, ecx", {0x01, 0xc8}},
      {"adc eax, ecx", {0x11, 0xc8}},
      {"sub eax, ecx", {0x29, 0xc8}},
      {"sbb eax, ecx", {0x19, 0xc8}},
      {"cmp eax, ecx", {0x39, 0xc8}},
      {"mov eax, ecx", {0x89, 0xc8}},
      {"BMI2 SHLX", {0xc4, 0xe2, 0x71, 0xf7, 0xc0}},
      {"nop", {0x90}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    std::vector<LowOp> PlainOps, CertifiedOps;
    Dec.liftToLow(Insn, PlainOps);
    auto Effects = staleEffects();
    Dec.liftToLow(Insn, CertifiedOps, {}, {}, &Effects);
    expectSameOps(PlainOps, CertifiedOps);
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
    EXPECT_EQ(Effects.OpCount, CertifiedOps.size());
    EXPECT_EQ(Effects.OperationDigest,
              lowUndefinedOperationDigest(CertifiedOps));
    EXPECT_TRUE(Effects.Effects.empty());
    EXPECT_NE(Effects.Diagnostic, "stale certificate");
  }
}

TEST_F(X86UndefinedEffects,
       BoundariesExcludePrefixOpsAndIncludeFinalZeroExtend) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x31, 0xc0}, Insn)); // xor eax, eax
  std::vector<LowOp> StandaloneOps;
  LowInstructionUndefinedEffects Standalone;
  Dec.liftToLow(Insn, StandaloneOps, {}, {}, &Standalone);
  ASSERT_FALSE(StandaloneOps.empty());
  const LowOp &Last = StandaloneOps.back();
  EXPECT_EQ(Last.Opcode, NdOp::INT_ZEXT);
  EXPECT_EQ(Last.Output, NdVar::reg(x86reg::RAX, 8));
  ASSERT_EQ(Last.NumInputs, 1U);
  EXPECT_EQ(Last.Inputs[0], NdVar::reg(x86reg::RAX, 4));

  LowOp Prefix;
  Prefix.Opcode = NdOp::NOP;
  Prefix.Addr = 0x800;
  const std::vector<LowOp> PrefixOps(3, Prefix);
  auto AppendedOps = PrefixOps;
  LowInstructionUndefinedEffects Appended;
  Dec.liftToLow(Insn, AppendedOps, {}, {}, &Appended);
  expectSameOps(
      PrefixOps,
      llvm::ArrayRef<LowOp>(AppendedOps).take_front(PrefixOps.size()));
  expectSameOps(
      StandaloneOps,
      llvm::ArrayRef<LowOp>(AppendedOps).drop_front(PrefixOps.size()));
  EXPECT_EQ(Appended.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_EQ(Appended.OpCount, StandaloneOps.size());
  EXPECT_EQ(Appended.OperationDigest, Standalone.OperationDigest);
  EXPECT_EQ(
      Appended.OperationDigest,
      lowUndefinedOperationDigest(
          llvm::ArrayRef<LowOp>(AppendedOps).drop_front(PrefixOps.size())));
  ASSERT_EQ(Standalone.Effects.size(), 1U);
  ASSERT_EQ(Appended.Effects.size(), 1U);
  EXPECT_EQ(Appended.Effects.front().AfterOp,
            Standalone.Effects.front().AfterOp);
  EXPECT_LE(Appended.Effects.front().AfterOp, Appended.OpCount);
}

TEST_F(X86UndefinedEffects, UnauditedInstructionsDoNotClaimCompleteCoverage) {
  const InstructionCase Cases[] = {
      {"rcl eax, cl", {0xd3, 0xd0}},
      {"bsf eax, ecx", {0x0f, 0xbc, 0xc1}},
      {"apx inc ndd nf", {0x62, 0xec, 0xf5, 0x14, 0xff, 0xc3}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
  }
}

TEST_F(X86UndefinedEffects, LegacyShiftCountsPublishCompleteEvidence) {
  const InstructionCase Cases[] = {
      {"shl al, cl", {0xd2, 0xe0}},
      {"shr ax, cl", {0x66, 0xd3, 0xe8}},
      {"sar eax, cl", {0xd3, 0xf8}},
      {"shl rcx, cl", {0x48, 0xd3, 0xe1}},
      {"shr ah, cl", {0xd2, 0xec}},
      {"sar byte [rbx], cl", {0xd2, 0x3b}},
      {"shl eax, 0", {0xc1, 0xe0, 0}},
      {"shr ax, 16", {0x66, 0xc1, 0xe8, 16}},
      {"sar rax, 1", {0x48, 0xd1, 0xf8}},
      {"sal /6 eax, cl", {0xd3, 0xf0}},
      {"sal /6 r9, 3", {0x49, 0xc1, 0xf1, 3}},
      {"sal /6 byte [rbx], 1", {0xd0, 0x33}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    std::vector<LowOp> PlainOps, CertifiedOps;
    Dec.liftToLow(Insn, PlainOps);
    auto Effects = staleEffects();
    Dec.liftToLow(Insn, CertifiedOps, {}, {}, &Effects);
    expectSameOps(PlainOps, CertifiedOps);
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
        << Effects.Diagnostic;
    EXPECT_EQ(Effects.OpCount, CertifiedOps.size());
    EXPECT_EQ(Effects.OperationDigest,
              lowUndefinedOperationDigest(CertifiedOps));
  }
}

TEST_F(X86UndefinedEffects, ShiftUnauditedPrefixesClearAllPartialEvidence) {
  const InstructionCase Cases[] = {
      {"duplicate operand prefix", {0x66, 0x66, 0xd3, 0xe0}},
      {"REP shift", {0xf3, 0xd3, 0xe0}},
      {"REPNZ shift", {0xf2, 0xd3, 0xe0}},
      {"unused REX.R opcode extension", {0x44, 0xd3, 0xe0}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    if (!decode(Case.Bytes, Insn))
      continue; // Decoder refusal is also an explicit boundary.
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  }
}

TEST_F(X86UndefinedEffects, CompatibilityShiftAliasDoesNotRelabelOtherGroups) {
  for (const auto [ModRM, Id] : {std::pair{0xf0, X86_INS_SHR},
                                 {0xf0, X86_INS_ROL},
                                 {0xf8, X86_INS_SHL}}) {
    DecodedInsn Insn{};
    ASSERT_TRUE(decode({0xd3, uint8_t(ModRM)}, Insn));
    Insn.Raw->id = Id;
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
  }
}

TEST_F(X86UndefinedEffects, XaddUnauditedFormsClearAllPartialEvidence) {
  const InstructionCase Cases[] = {
      {"segmented memory XADD", {0x64, 0x0f, 0xc1, 0x08}},
      {"duplicate LOCK memory XADD", {0xf0, 0xf0, 0x0f, 0xc1, 0x08}},
      {"LOCK register XADD", {0xf0, 0x0f, 0xc1, 0xc8}},
      {"REP XADD", {0xf3, 0x0f, 0xc1, 0xc8}},
      {"REPNZ XADD", {0xf2, 0x0f, 0xc1, 0xc8}},
      {"duplicate operand prefix", {0x66, 0x66, 0x0f, 0xc1, 0xc8}},
      {"duplicate REX", {0x48, 0x48, 0x0f, 0xc1, 0xc8}},
      {"duplicate memory address prefix", {0x67, 0x67, 0x0f, 0xc1, 0x08}},
      {"legacy prefix after REX", {0x48, 0x66, 0x0f, 0xc1, 0x08}},
      {"unused REX.X", {0x42, 0x0f, 0xc1, 0xc8}},
      {"address prefix", {0x67, 0x0f, 0xc1, 0xc8}},
      {"segment prefix", {0x64, 0x0f, 0xc1, 0xc8}},
      {"REX2 XADD", {0xd5, 0x08, 0x0f, 0xc1, 0xc8}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    if (!decode(Case.Bytes, Insn))
      continue;
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    try {
      Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
    } catch (const UnliftedInstruction &) {
      expectCleared(Effects);
      continue;
    }
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
  }
}

TEST_F(X86UndefinedEffects, XaddChangedEncodingAndOperandsRemainUnaudited) {
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode({0x0f, 0xc1, 0xc8}, Insn));
    auto &X = Insn.Raw->detail->x86;
    switch (Mutation) {
    case 0:
      X.operands[0].reg = X86_REG_EDX;
      break;
    case 1:
      X.operands[1].reg = X86_REG_EDX;
      break;
    case 2:
      X.operands[0].reg = X86_REG_AX;
      X.operands[0].size = 2;
      break;
    case 3:
      X.operands[1].reg = X86_REG_RCX;
      X.operands[1].size = 8;
      break;
    case 4:
      X.opcode[1] = 0xc0;
      break;
    case 5:
      Insn.Raw->bytes[1] = 0xc0;
      break;
    case 6:
      X.encoding.modrm_offset = 1;
      break;
    case 7:
      X.modrm ^= 1;
      break;
    case 8:
      X.prefix[2] = 0x66;
      break;
    case 9:
      X.rex = 0x48;
      break;
    case 10:
      X.encoding.imm_size = 1;
      break;
    case 11:
      X.op_count = 1;
      break;
    }
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    if (X.op_count < 2) {
      EXPECT_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects),
                   UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
      EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Unsupported);
      expectCleared(Effects);
      continue;
    }
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
  }
  // Any REX selects the low-byte bank. REX.W still cannot widen 0F C0.
  for (unsigned Operand : {0u, 1u}) {
    DecodedInsn Insn{};
    ASSERT_TRUE(
        decode({0x48, 0x0f, 0xc0, uint8_t(Operand ? 0xe0 : 0xc4)}, Insn));
    Insn.Raw->detail->x86.operands[Operand].reg = X86_REG_AH;
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
  }
}

TEST_F(X86UndefinedEffects, MemoryXaddChangedAddressDetailsRemainUnaudited) {
  for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
    SCOPED_TRACE(Mutation);
    DecodedInsn Insn{};
    // XADD word [R13 + R10 * 2 - 157],R8W.
    ASSERT_TRUE(decode(
        {0x66, 0x47, 0x0f, 0xc1, 0x84, 0x55, 0x63, 0xff, 0xff, 0xff}, Insn));
    auto &X = Insn.Raw->detail->x86;
    switch (Mutation) {
    case 0:
      X.operands[0].mem.base = X86_REG_R12;
      break;
    case 1:
      X.operands[0].mem.index = X86_REG_R11;
      break;
    case 2:
      X.operands[0].mem.scale = 4;
      break;
    case 3:
      ++X.operands[0].mem.disp;
      break;
    case 4:
      X.operands[0].mem.segment = X86_REG_FS;
      break;
    case 5:
      X.operands[0].size = 4;
      break;
    case 6:
      X.operands[1].reg = X86_REG_R9W;
      break;
    case 7:
      X.operands[1].size = 4;
      break;
    case 8:
      X.addr_size = 4;
      break;
    case 9:
      X.encoding.disp_offset = 5;
      break;
    case 10:
      X.encoding.disp_size = 2;
      break;
    case 11:
      X.encoding.imm_size = 1;
      break;
    case 12:
      X.encoding.modrm_offset = 3;
      break;
    case 13:
      X.sib ^= 0x40;
      break;
    case 14:
      X.modrm ^= 1;
      break;
    case 15:
      Insn.Raw->bytes[5] ^= 0x40;
      break;
    case 16:
      Insn.Raw->bytes[6] ^= 1;
      break;
    case 17:
      ++X.disp;
      break;
    case 18:
      --Insn.Raw->size;
      break;
    case 19:
      X.prefix[3] = 0x67;
      break;
    }
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
  }
}

TEST_F(X86UndefinedEffects, DoubleShiftUnauditedFormsClearPartialEvidence) {
  const InstructionCase Cases[] = {
      {"memory SHLD", {0x0f, 0xa5, 0x08}},
      {"memory SHRD", {0x66, 0x0f, 0xac, 0x08, 17}},
      {"LOCK", {0xf0, 0x0f, 0xa5, 0xc8}},
      {"REP", {0xf3, 0x0f, 0xad, 0xc8}},
      {"duplicate operand prefix", {0x66, 0x66, 0x0f, 0xa5, 0xc8}},
      {"duplicate REX", {0x48, 0x48, 0x0f, 0xad, 0xc8}},
      {"unused REX.X", {0x42, 0x0f, 0xa5, 0xc8}},
      {"address prefix", {0x67, 0x0f, 0xad, 0xc8}},
      {"segment prefix", {0x64, 0x0f, 0xa5, 0xc8}},
      {"REX2", {0xd5, 0x08, 0x0f, 0xa5, 0xc8}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    if (!decode(Case.Bytes, Insn))
      continue;
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
    EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
    expectCleared(Effects);
  }
}

TEST_F(X86UndefinedEffects, DoubleShiftChangedFormsDoNotPublishDraftEffects) {
  for (bool Immediate : {false, true})
    for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Immediate << '/' << Mutation);
      DecodedInsn Insn{};
      auto Bytes = std::vector<uint8_t>{0x66, 0x0f,
                                        uint8_t(Immediate ? 0xa4 : 0xa5), 0xc8};
      if (Immediate)
        Bytes.push_back(17);
      ASSERT_TRUE(decode(Bytes, Insn));
      auto &X = Insn.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        X.operands[0].reg = X86_REG_DX;
        break;
      case 1:
        X.operands[1].reg = X86_REG_DX;
        break;
      case 2:
        X.operands[0].size = 4;
        break;
      case 3:
        X.operands[1].size = 4;
        break;
      case 4:
        X.operands[2].size = 2;
        break;
      case 5:
        if (Immediate)
          X.operands[2].imm = 16;
        else
          X.operands[2].reg = X86_REG_DL;
        break;
      case 6:
        Insn.Raw->id = X86_INS_SHRD;
        break;
      case 7:
        X.opcode[1] ^= 8;
        break;
      case 8:
        X.modrm ^= 1;
        break;
      case 9:
        X.encoding.modrm_offset = 1;
        break;
      case 10:
        X.prefix[2] = 0;
        break;
      case 11:
        X.rex = 0x48;
        break;
      case 12:
        X.encoding.imm_offset = Immediate ? 3 : 1;
        break;
      case 13:
        X.encoding.disp_size = 1;
        break;
      case 14:
        X.op_count = 2;
        break;
      }
      std::vector<LowOp> Ops;
      auto Effects = staleEffects();
      if (X.op_count < 3) {
        EXPECT_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects),
                     UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Unsupported);
        expectCleared(Effects);
        continue;
      }
      ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
      EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
      expectCleared(Effects);
    }
}

TEST_F(X86UndefinedEffects, BitAndRotateAuditBoundariesClearPartialEvidence) {
  const InstructionCase Cases[] = {
      {"duplicate memory bit segment", {0x65, 0x65, 0x0f, 0xb3, 0x08}},
      {"LOCK read-only bit test", {0xf0, 0x0f, 0xa3, 0x08}},
      {"HLE locked memory bit base", {0xf2, 0xf0, 0x0f, 0xb3, 0x08}},
      {"REP bit test", {0xf3, 0x0f, 0xa3, 0xc8}},
      {"duplicate bit-test operand prefix", {0x66, 0x66, 0x0f, 0xb3, 0xc8}},
      {"unused bit-test REX.X", {0x42, 0x0f, 0xb3, 0xc8}},
      {"unused immediate REX.R", {0x44, 0x0f, 0xba, 0xf0, 3}},
      {"duplicate rotate operand prefix", {0x66, 0x66, 0xd3, 0xc0}},
      {"REP rotate", {0xf3, 0xd3, 0xc8}},
      {"unused rotate REX.R", {0x44, 0xd3, 0xc0}},
      {"rotate through carry", {0xd3, 0xd0}},
      {"reverse rotate through carry", {0xd3, 0xd8}},
      {"APX rotate", {0x62, 0xf4, 0x7c, 0x08, 0xd3, 0xc0}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    if (!decode(Case.Bytes, Insn))
      continue;
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    try {
      Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
    } catch (const UnliftedInstruction &) {
      expectCleared(Effects);
      continue;
    }
    EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
    expectCleared(Effects);
  }
}

TEST_F(X86UndefinedEffects, BitTestMutatedEncodingAndOperandsRemainUnaudited) {
  for (bool Immediate : {false, true})
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Immediate << '/' << Mutation);
      DecodedInsn Insn{};
      ASSERT_TRUE(decode(Immediate ? std::vector<uint8_t>{0x0f, 0xba, 0xf0, 129}
                                   : std::vector<uint8_t>{0x0f, 0xb3, 0xc8},
                         Insn));
      auto &X = Insn.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        X.operands[0].reg = X86_REG_EDX;
        break;
      case 1:
        X.operands[0].reg = X86_REG_AX;
        X.operands[0].size = 2;
        break;
      case 2:
        if (Immediate)
          X.operands[1].imm = 128;
        else
          X.operands[1].reg = X86_REG_EDX;
        break;
      case 3:
        X.operands[1].size = 8;
        if (!Immediate)
          X.operands[1].reg = X86_REG_RCX;
        break;
      case 4:
        X.encoding.modrm_offset = 1;
        break;
      case 5:
        X.prefix[2] = 0x66;
        break;
      case 6:
        X.opcode[1] = 0xa3;
        break;
      case 7:
        X.op_count = 1;
        break;
      case 8:
        Insn.Raw->bytes[2] ^= 8;
        break;
      }
      std::vector<LowOp> Ops;
      auto Effects = staleEffects();
      if (X.op_count < 2) {
        EXPECT_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects),
                     UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Unsupported);
        expectCleared(Effects);
        continue;
      }
      ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
      EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
      expectCleared(Effects);
    }
}

TEST_F(X86UndefinedEffects, MemoryBitTestBindsAddressAndImmediateDetails) {
  for (bool Immediate : {false, true})
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
      DecodedInsn Insn{};
      ASSERT_TRUE(decode(
          Immediate
              ? std::vector<uint8_t>{0x66, 0x65, 0x0f, 0xba, 0x74, 0x4b, 0xfd,
                                     0x12}
              : std::vector<uint8_t>{0x65, 0x48, 0x0f, 0xb3, 0x4c, 0x53, 0xfd},
          Insn));
      auto &X = Insn.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        X.operands[0].mem.base = X86_REG_RDX;
        break;
      case 1:
        X.operands[0].mem.index = X86_REG_R8;
        break;
      case 2:
        X.operands[0].mem.scale = 8;
        break;
      case 3:
        X.operands[0].mem.disp += 1;
        break;
      case 4:
        X.operands[0].mem.segment = X86_REG_FS;
        break;
      case 5:
        X.encoding.disp_offset -= 1;
        break;
      case 6:
        X.encoding.imm_offset += 1;
        break;
      case 7:
        X.sib ^= 1;
        break;
      case 8:
        X.prefix[1] = 0;
        break;
      }
      std::vector<LowOp> Ops;
      auto Effects = staleEffects();
      ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
      EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
      expectCleared(Effects);
    }
}

TEST_F(X86UndefinedEffects, ScalarCountMutatedWidthAndOpcodeRemainUnaudited) {
  for (uint8_t ModRM : {0xe0, 0xc0, 0xc8})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(Mutation);
      DecodedInsn Insn{};
      ASSERT_TRUE(decode({0xd3, ModRM}, Insn)); // SHL/ROL/ROR EAX,CL.
      auto &X = Insn.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        X.operands[1].reg = X86_REG_DL;
        break;
      case 1:
        X.operands[1].reg = X86_REG_ECX;
        X.operands[1].size = 4;
        break;
      case 2:
        X.operands[0].reg = X86_REG_AX;
        X.operands[0].size = 2;
        break;
      case 3:
        X.operands[0].reg = X86_REG_ECX;
        break;
      case 4:
        Insn.Raw->bytes[1] = 0xe8;
        break; // Actual group is SHR.
      case 5:
        X.encoding.modrm_offset = 0;
        break;
      case 6:
        X.op_count = 1;
        break;
      case 7:
        X.operands[1].type = X86_OP_IMM;
        X.operands[1].imm = 1;
        break;
      }
      std::vector<LowOp> Ops;
      auto Effects = staleEffects();
      if (X.op_count < 2) {
        EXPECT_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects),
                     UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Unsupported);
        expectCleared(Effects);
        continue;
      }
      ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
      EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
      expectCleared(Effects);
    }
  DecodedInsn Immediate{};
  ASSERT_TRUE(decode({0xc1, 0xe0, 2}, Immediate));
  Immediate.Raw->detail->x86.operands[1].imm = 1;
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  Dec.liftToLow(Immediate, Ops, {}, {}, &Effects);
  expectCleared(Effects);
}

TEST_F(X86UndefinedEffects, PermissiveUnsupportedFailureClearsOldCertificate) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x90}, Insn));
  Insn.Raw->id = X86_INS_INVALID;
  Dec.setStrict(false);
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  EXPECT_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects), UnliftedInstruction);
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Unsupported);
  expectCleared(Effects);
  EXPECT_TRUE(Ops.empty());
  EXPECT_EQ(Effects.OpCount, 0u);
}

TEST_F(X86UndefinedEffects, StrictFailureClearsOldCertificateBeforeThrowing) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x90}, Insn));
  Insn.Raw->id = X86_INS_INVALID;
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  EXPECT_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects), UnliftedInstruction);
  expectCleared(Effects);
}

TEST_F(X86UndefinedEffects, MissingOperandDetailClearsOldCertificate) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x31, 0xc0}, Insn));
  cs_insn WithoutDetail = *Insn.Raw;
  WithoutDetail.detail = nullptr;
  Insn.Raw = &WithoutDetail;
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  EXPECT_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects), UnliftedInstruction);
  expectCleared(Effects);
  EXPECT_EQ(Effects.OpCount, 0U);
  EXPECT_TRUE(Ops.empty());
}

TEST_F(X86UndefinedEffects,
       MemoryCallProjectionClearsOldCertificateOnRejection) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x31, 0xc0}, Insn));
  LowOp Prefix;
  Prefix.Addr = 0x800;
  std::vector<LowOp> Ops{Prefix};
  auto Effects = staleEffects();
  EXPECT_FALSE(Dec.liftX64MemoryCallToLow(Insn, Ops, &Effects));
  expectCleared(Effects);
  EXPECT_EQ(Effects.OpCount, 0U);
  expectSameOps(std::vector<LowOp>{Prefix}, Ops);
}

TEST_F(X86UndefinedEffects, MemoryCallProjectionDoesNotReuseLogicalEffects) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0xff, 0x10}, Insn)); // call qword ptr [rax]
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  EXPECT_TRUE(Dec.liftX64MemoryCallToLow(Insn, Ops, &Effects));
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_NE(Effects.Diagnostic, "stale certificate");
  EXPECT_FALSE(Ops.empty());
}

TEST_F(X86UndefinedEffects, MalformedOperandCountCannotCertifyEmptyLift) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x31, 0xc0}, Insn));
  ASSERT_NE(Insn.Raw->detail, nullptr);
  Insn.Raw->detail->x86.op_count = 1;
  Dec.setStrict(false);
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  EXPECT_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
  expectCleared(Effects);
}

TEST_F(X86UndefinedEffects, MalformedWidthsAndAddressesDoNotCertifyShapes) {
  struct MutationCase {
    const char *Name;
    std::vector<uint8_t> Bytes;
    void (*Mutate)(cs_x86 &);
  };
  const MutationCase Cases[] = {
      {"byte stack push",
       {0x6a, 0x01},
       [](cs_x86 &X) { X.operands[0].size = 1; }},
      {"byte lea",
       {0x8d, 0x01},
       [](cs_x86 &X) {
         X.operands[0].reg = X86_REG_AL;
         X.operands[0].size = 1;
       }},
      {"byte near call",
       {0xff, 0xd0},
       [](cs_x86 &X) {
         X.operands[0].reg = X86_REG_AL;
         X.operands[0].size = 1;
       }},
      {"byte near jump",
       {0xff, 0xe0},
       [](cs_x86 &X) {
         X.operands[0].reg = X86_REG_AL;
         X.operands[0].size = 1;
       }},
      {"byte movzx mutated to a dword source",
       {0x0f, 0xb6, 0xc1},
       [](cs_x86 &X) {
         X.operands[1].reg = X86_REG_ECX;
         X.operands[1].size = 4;
       }},
      {"byte movsx mutated to a dword source",
       {0x0f, 0xbe, 0xc1},
       [](cs_x86 &X) {
         X.operands[1].reg = X86_REG_ECX;
         X.operands[1].size = 4;
       }},
      {"invalid scale",
       {0x8b, 0x04, 0x4b},
       [](cs_x86 &X) { X.operands[1].mem.scale = 3; }},
      {"invalid address width",
       {0x8b, 0x04, 0x4b},
       [](cs_x86 &X) { X.addr_size = 3; }},
      {"vector base",
       {0x8b, 0x04, 0x4b},
       [](cs_x86 &X) { X.operands[1].mem.base = X86_REG_XMM0; }},
      {"vector index",
       {0x8b, 0x04, 0x4b},
       [](cs_x86 &X) { X.operands[1].mem.index = X86_REG_XMM0; }},
      {"invalid segment",
       {0x8b, 0x04, 0x4b},
       [](cs_x86 &X) { X.operands[1].mem.segment = X86_REG_RAX; }},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    std::vector<LowOp> Baseline;
    LowInstructionUndefinedEffects Original;
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Baseline, {}, {}, &Original));
    ASSERT_EQ(Original.Coverage, LowUndefinedCoverage::Complete);
    Case.Mutate(Insn.Raw->detail->x86);
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
    EXPECT_FALSE(Effects.Diagnostic.empty());
  }
}

TEST_F(X86UndefinedEffects, MissingCoverageDoesNotPublishPartialEffects) {
  DecodedInsn Insn{};
  ASSERT_TRUE(decode({0x31, 0xc0}, Insn));
  // Logical AF is locally recorded even when the complete transaction cannot
  // be certified. Do not expose it as a usable partial architecture contract.
  Dec.setStrict(false);
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_FALSE(Effects.Diagnostic.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
}

TEST(X86UndefinedEffectsForms, LegacyAliasesAndAddressWidthsRemainAudited) {
  struct FormCase {
    const char *Name;
    Arch Target;
    std::vector<uint8_t> Bytes;
  };
  const FormCase Cases[] = {
      {"high byte alias", Arch::X64, {0x88, 0xe0}},
      {"REX low byte", Arch::X64, {0x40, 0x88, 0xcc}},
      {"high byte sign extension", Arch::X64, {0x0f, 0xbe, 0xc5}},
      {"32-bit address in long mode", Arch::X64, {0x67, 0x8b, 0x04, 0x4b}},
      {"16-bit address, word data", Arch::X86, {0x67, 0x66, 0x8b, 0x00}},
      {"16-bit address, dword data", Arch::X86, {0x67, 0x8b, 0x00}},
      {"MOVSXD wide", Arch::X64, {0x48, 0x63, 0xc1}},
      {"MOVSXD dword", Arch::X64, {0x63, 0xc1}},
      {"immediate byte push", Arch::X64, {0x6a, 0x01}},
      {"word push", Arch::X64, {0x66, 0x50}},
      {"immediate ALU", Arch::X64, {0x48, 0x83, 0xc0, 0x01}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Case.Target));
    DecodedInsn Insn{};
    ASSERT_EQ(Decode.decodeOneForLift(Case.Bytes.data(), Case.Bytes.size(),
                                      0x1000, Insn),
              static_cast<int>(Case.Bytes.size()));
    std::vector<LowOp> Ops;
    LowInstructionUndefinedEffects Effects;
    ASSERT_NO_THROW(Decode.liftToLow(Insn, Ops, {}, {}, &Effects));
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
        << Insn.Raw->mnemonic << " " << Insn.Raw->op_str << ": "
        << Effects.Diagnostic;
    EXPECT_TRUE(Effects.Effects.empty());
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  }
}

TEST_F(X86UndefinedEffects, WordMovsxdWithDisputedSourceWidthIsUnlifted) {
  // The pinned decoder gives 66-prefixed MOVSXD a dword source, as LLVM does,
  // while the SDM and XED read a word. The lifter refuses the form instead of
  // choosing a source width and memory access.
  const InstructionCase Cases[] = {
      {"register source", {0x66, 0x63, 0xc1}},
      {"memory source", {0x66, 0x63, 0x01}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    ASSERT_EQ(Insn.Raw->id, X86_INS_MOVSXD);
    ASSERT_EQ(Insn.Raw->detail->x86.op_count, 2U);
    EXPECT_EQ(Insn.Raw->detail->x86.operands[0].size, 2U);
    EXPECT_EQ(Insn.Raw->detail->x86.operands[1].size, 4U);
    std::vector<LowOp> Ops;
    auto Effects = staleEffects();
    EXPECT_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects),
                 UnliftedInstruction);
    expectCleared(Effects);
  }
}

TEST_F(X86UndefinedEffects, DecodedEqualWidthExtensionsRemainUnaudited) {
  // Unlike the mutated dword-source operands above, these equal-width
  // 16-bit forms really decode. They remain outside the initial flag audit.
  const InstructionCase Cases[] = {
      {"word movsx", {0x66, 0x0f, 0xbf, 0xc1}},
      {"word movzx", {0x66, 0x0f, 0xb7, 0xc1}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    DecodedInsn Insn{};
    ASSERT_TRUE(decode(Case.Bytes, Insn));
    ASSERT_EQ(Insn.Raw->detail->x86.op_count, 2U);
    ASSERT_EQ(Insn.Raw->detail->x86.operands[0].size, 2U);
    ASSERT_EQ(Insn.Raw->detail->x86.operands[1].size, 2U);
    std::vector<LowOp> PlainOps, CertifiedOps;
    ASSERT_NO_THROW(Dec.liftToLow(Insn, PlainOps));
    auto Effects = staleEffects();
    ASSERT_NO_THROW(Dec.liftToLow(Insn, CertifiedOps, {}, {}, &Effects));
    expectSameOps(PlainOps, CertifiedOps);
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
    expectCleared(Effects);
    EXPECT_FALSE(Effects.Diagnostic.empty());
    EXPECT_EQ(Effects.OpCount, CertifiedOps.size());
    EXPECT_EQ(Effects.OperationDigest,
              lowUndefinedOperationDigest(CertifiedOps));
  }
}

TEST(X86UndefinedEffectsOtherArch, AArch64DoesNotReuseAnX86Certificate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  const uint8_t Bytes[] = {0x1f, 0x20, 0x03, 0xd5}; // nop
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), 0x1000, Insn), 4);
  std::vector<LowOp> Ops;
  auto Effects = staleEffects();
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Missing);
  expectCleared(Effects);
}

TEST(X86UndefinedEffectsForms, ShiftAddressOverridesKeepCountEvidence) {
  for (const auto &[Target, Bytes] :
       std::vector<std::pair<Arch, std::vector<uint8_t>>>{
           {Arch::X86, {0x67, 0x66, 0xd3, 0x20}},
           {Arch::X86, {0x67, 0xd3, 0x20}},
           {Arch::X64, {0x67, 0xd3, 0x64, 0x8b, 4}},
           {Arch::X64, {0x64, 0x48, 0xd3, 0x20}}}) {
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Target));
    DecodedInsn Insn{};
    ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              static_cast<int>(Bytes.size()));
    std::vector<LowOp> Plain, Certified;
    LowInstructionUndefinedEffects Effects;
    Decode.liftToLow(Insn, Plain);
    Decode.liftToLow(Insn, Certified, {}, {}, &Effects);
    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
        << Effects.Diagnostic;
    expectSameOps(Plain, Certified);
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Certified));
    ASSERT_GE(Effects.Effects.size(), 2u);
    for (const auto &Effect : Effects.Effects)
      EXPECT_TRUE(Effect.When.has_value());
  }
}

} // namespace
