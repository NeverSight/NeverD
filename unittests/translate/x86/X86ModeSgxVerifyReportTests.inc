//===- X86ModeSgxVerifyReportTests.inc - Software report verification -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify the complete read-only report MAC structure and its retirement.
//===----------------------------------------------------------------------===//

namespace {
// Python hmac.digest with the public test key 0..31 and the report below.
void makeVerifyReport(X86SgxPage &Page) {
  Page.Permissions = MemoryPermission::Read;
  Page.Bytes[0] = 0x81;
  const auto CPUSVN = llvm::fromHex("4820f3376ae6b2f2034d3b7a4b48a778");
  std::copy(CPUSVN.begin(), CPUSVN.end(), Page.Bytes.begin() + 16);
  for (unsigned I = 0; I != 160; ++I)
    Page.Bytes[32 + I] = I;
  const auto MAC = llvm::fromHex(
      "bc48a6dbe537729315c084f3c6be9568ed941b59137f03ae90f80276ecce3d43");
  std::copy(MAC.begin(), MAC.end(), Page.Bytes.begin() + 224);
}

class SgxVerifyReportPlatform final : public SgxReferencePlatform {
  static std::array<uint8_t, 32> publicMAC(std::span<const uint8_t> Body) {
    std::array<uint8_t, 64> InnerKey, OuterKey;
    for (unsigned I = 0; I != InnerKey.size(); ++I) {
      const uint8_t KeyByte = I < 32 ? I : 0;
      InnerKey[I] = KeyByte ^ 0x36;
      OuterKey[I] = KeyByte ^ 0x5c;
    }
    llvm::SHA256 Inner, Outer;
    Inner.update(InnerKey);
    Inner.update({Body.data(), Body.size()});
    Outer.update(OuterKey);
    Outer.update(Inner.final());
    return Outer.final();
  }

public:
  mutable unsigned Authentications = 0;
  llvm::Expected<std::array<uint8_t, 32>>
  report2MAC(const X86SgxPage &,
             std::span<const uint8_t, 192> Body) const override {
    return publicMAC(Body);
  }
  llvm::Expected<std::array<uint8_t, 32>>
  report2MAC224(std::span<const uint8_t, 224> Body) const override {
    return publicMAC(Body);
  }
  llvm::Expected<bool>
  verifyReport2MAC(std::span<const uint8_t, 224> Body,
                   std::span<const uint8_t, 32> MAC) const override {
    ++Authentications;
    const auto Tag = publicMAC(Body);
    return std::equal(Tag.begin(), Tag.end(), MAC.begin());
  }
};
} // namespace

TEST_F(X86ModeSgxThreadTest, VerifyReportConsumesAnIssuedEnclaveReport) {
  using Coverage = X86SgxReport2MACCoverage;
  for (auto C :
       {std::optional<Coverage>{}, std::optional{Coverage::Operation192},
        std::optional{Coverage::Verifiable224}}) {
    SCOPED_TRACE(C ? int(*C) : -1);
    startThread();
    privateInstruction(0, Enclu);
    privateInstruction(3, Enclu);
    for (unsigned I = 0; I != 64; ++I)
      Code.Bytes[64 + I] = I;
    std::fill_n(Next.Bytes.begin(), 1024, 0xac);
    bindThread({.SGX256 = true,
                .Report2Write = X86SgxReport2Write::Payload768,
                .Report2MACCoverage = C,
                .VerifyReport2 = X86SgxVerifyReport2Profile::SDM093,
                .VerifyReport2Flags = X86SgxVerifyReport2Flags::SetStatus});
    auto Platform = std::make_shared<SgxVerifyReportPlatform>();
    Memory->sgxMemory()->bindKeyPlatform(Platform);
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = 10;
    R.GPR[3] = Code.Linear + 64;
    R.GPR[1] = Next.Linear;
    R.GPR[2] = 0;
    Budget = 1;
    auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
    std::array<uint8_t, 1024> Report;
    ASSERT_EQ(
        llvm::cantFail(
            Memory->load({Next.Linear, MemoryAccessKind::Read, 3}, Report))
            .Status,
        X86PageWalkStatus::Completed);
    if (!C) {
      EXPECT_EQ(Step.Status, X86LegacyExecutionStatus::ModeUnavailable);
      EXPECT_NE(Step.OwnerError.find("MAC prefix size"), std::string::npos);
      EXPECT_EQ(Step.Completions, 0u);
      EXPECT_EQ(R.PC, Code.Linear);
      EXPECT_EQ(R.GPR[0], 10u);
      EXPECT_TRUE(std::equal(Report.begin(), Report.end(), Next.Bytes.begin()));
      continue;
    }
    ASSERT_EQ(Step.Completions, 1u) << Step.OwnerError;
    ASSERT_EQ(R.PC, Code.Linear + 3);
    ASSERT_EQ(R.GPR[0], 0u);
    const bool Verifiable = *C == Coverage::Verifiable224;
    EXPECT_EQ(
        llvm::toHex({Report.data() + 224, 32}, true),
        Verifiable
            ? "cd391871843ae85acd66e2aa071689fed3972faf7f5d1172552abe1fcff61fed"
            : "f320853d2e63ce78bb2cb03167da1260ebb7974fc82d3f181ca6b03f7505bbe"
              "1");
    R.GPR[0] = 8;
    R.GPR[3] = Next.Linear;
    Budget = 1;
    Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Step.Completions, 1u) << Step.OwnerError;
    EXPECT_EQ(R.PC, Code.Linear + 6);
    EXPECT_EQ(R.GPR[0], Verifiable ? 0u : 28u);
    EXPECT_EQ(bool(R.RFlags & 0x40), !Verifiable);
    EXPECT_EQ(Platform->Authentications, 1u);
  }
}

TEST_F(X86ModeSgxThreadTest,
       VerifyReportAuthenticatesTheFullPrefixAfterCPUSVN) {
  using Flags = X86SgxVerifyReport2Flags;
  unsigned Completed = 0;
  // Each offset identifies one changed byte in the public report.
  for (int Changed : {-1, 31, 191, 223, 255})
    for (auto Policy : {Flags::SetStatus, Flags::Preserve}) {
      SCOPED_TRACE(testing::Message() << Changed << ' ' << int(Policy));
      startThread();
      privateInstruction(0, {0x67, 0x0f, 1, 0xd7});
      makeVerifyReport(Next);
      if (Changed >= 0)
        Next.Bytes[Changed] ^= 1;
      bindThread({.VerifyReport2 = X86SgxVerifyReport2Profile::SDM093,
                  .VerifyReport2Flags = Policy});
      auto Platform = std::make_shared<SgxVerifyReportPlatform>();
      Memory->sgxMemory()->bindKeyPlatform(Platform);
      X86ModeExecution Execution(*Memory, Options);
      uint64_t Budget = 1;
      ASSERT_EQ(
          llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
          1u);
      auto &R = Machine.Core.Core.Core.Core.Core.Core;
      R.GPR[0] = 0xfedcba9800000008ull;
      R.GPR[3] = Next.Linear;
      R.GPR[1] = R.GPR[2] = UINT64_MAX;
      R.RFlags |= 0x108d5;
      const uint64_t Before = R.RFlags;
      Budget = 1;
      auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
      ASSERT_EQ(Step.Completions, 1u) << Step.OwnerError;
      const unsigned Status = Changed < 0 ? 0 : (Changed == 31 ? 32 : 28);
      const uint64_t Expected =
          Policy == Flags::Preserve
              ? Before
              : (Before & ~uint64_t{0x8d5}) | (Status ? 0x40 : 0);
      EXPECT_EQ(R.PC, Code.Linear + 4);
      EXPECT_EQ(R.GPR[0], Status);
      EXPECT_EQ(R.GPR[3], Next.Linear);
      EXPECT_EQ(R.GPR[1], UINT64_MAX);
      EXPECT_EQ(R.GPR[2], UINT64_MAX);
      EXPECT_EQ(R.RFlags, Expected & ~uint64_t{0x10000});
      EXPECT_EQ(Platform->Authentications, unsigned(Changed != 31));
      EXPECT_EQ(Platform->VersionChecks, 0u);
      std::array<uint8_t, 256> After;
      ASSERT_EQ(
          llvm::cantFail(
              Memory->load({Next.Linear, MemoryAccessKind::Read, 3}, After))
              .Status,
          X86PageWalkStatus::Completed);
      EXPECT_TRUE(std::equal(After.begin(), After.end(), Next.Bytes.begin()));
      ++Completed;
    }
  RecordProperty("verification_cases", Completed);
}

TEST_F(X86ModeSgxThreadTest,
       VerifyReportChecksTheLegacyHeaderBeforeVersionAndMAC) {
  unsigned Completed = 0;
  for (int Changed : {-1, 0, 1, 2, 3, 15, 16, 192, 223, 255}) {
    SCOPED_TRACE(Changed);
    startThread();
    privateInstruction(0, Enclu);
    makeVerifyReport(Next);
    auto *Report = Next.Bytes.data() + 256;
    std::copy_n(Next.Bytes.begin(), 256, Report);
    Report[16] = 0x44; // Accepted by the platform, distinct from current SVN.
    const auto MAC = llvm::fromHex(
        "32c49700d17f541b039fbd5532576964ff9f77c62a96d0edeb059c6541c7cb27");
    std::copy(MAC.begin(), MAC.end(), Report + 224);
    const bool InvalidHeader =
        Changed >= 0 && (Changed < 16 || Changed == 192 || Changed == 223);
    if (Changed >= 0)
      Report[Changed] ^= 1;
    if (InvalidHeader)
      Report[16] = 0;
    bindThread({.VerifyReport2 = X86SgxVerifyReport2Profile::TDX002,
                .VerifyReport2Flags = X86SgxVerifyReport2Flags::SetStatus});
    auto Platform = std::make_shared<SgxVerifyReportPlatform>();
    Memory->sgxMemory()->bindKeyPlatform(Platform);
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = 8;
    R.GPR[3] = Next.Linear + 256;
    Budget = 1;
    auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Step.Completions, 1u) << Step.OwnerError;
    EXPECT_EQ(R.PC, Code.Linear + 3);
    EXPECT_EQ(R.GPR[0], Changed < 0 ? 0u : (Changed == 16 ? 32u : 28u));
    EXPECT_EQ(bool(R.RFlags & 0x40), Changed >= 0);
    EXPECT_EQ(Platform->VersionChecks, unsigned(!InvalidHeader));
    EXPECT_EQ(Platform->Authentications,
              unsigned(!InvalidHeader && Changed != 16));
    ++Completed;
  }
  RecordProperty("legacy_verification_cases", Completed);
}

TEST_F(X86ModeSgxThreadTest,
       VerifyReportPreservesTheInstructionWithAnUnboundProfile) {
  for (unsigned Offset : {0u, 256u}) {
    SCOPED_TRACE(Offset);
    startThread();
    privateInstruction(0, Enclu);
    makeVerifyReport(Next);
    bindThread({.VerifyReport2 = X86SgxVerifyReport2Profile::Unbound});
    auto Platform = std::make_shared<SgxVerifyReportPlatform>();
    Memory->sgxMemory()->bindKeyPlatform(Platform);
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = 8;
    R.GPR[3] = Next.Linear + Offset;
    const uint64_t Before = R.RFlags;
    Budget = 1;
    auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
    EXPECT_EQ(Step.Completions, 0u);
    EXPECT_EQ(Step.Status, X86LegacyExecutionStatus::ModeUnavailable);
    EXPECT_NE(Step.OwnerError.find(Offset ? "operand alignment"
                                          : "verification profile"),
              std::string::npos);
    EXPECT_EQ(R.PC, Code.Linear);
    EXPECT_EQ(R.GPR[0], 8u);
    EXPECT_EQ(R.RFlags, Before);
    EXPECT_EQ(Platform->Authentications, 0u);
  }
}

TEST_F(X86ModeSgxThreadTest, VerifyReportRetiresOnlyWithDeterminateResults) {
  enum class Binding { Absent, Legacy, AmbiguousFlags, EqualFlags };
  for (Binding B : {Binding::Absent, Binding::Legacy, Binding::AmbiguousFlags,
                    Binding::EqualFlags}) {
    SCOPED_TRACE(int(B));
    startThread();
    privateInstruction(0, Enclu);
    makeVerifyReport(Next);
    bindThread({.VerifyReport2 = X86SgxVerifyReport2Profile::SDM093});
    if (B == Binding::Legacy)
      Memory->sgxMemory()->bindKeyPlatform(
          std::make_shared<SgxReferencePlatform>());
    else if (B != Binding::Absent)
      Memory->sgxMemory()->bindKeyPlatform(
          std::make_shared<SgxVerifyReportPlatform>());
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = 0xfedcba9800000008ull;
    R.GPR[3] = Next.Linear;
    R.RFlags &= ~uint64_t{0x8d5};
    if (B != Binding::EqualFlags)
      R.RFlags |= 1;
    const uint64_t Before = R.RFlags;
    Budget = 1;
    auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
    if (B == Binding::EqualFlags) {
      ASSERT_EQ(Step.Completions, 1u) << Step.OwnerError;
      EXPECT_EQ(R.PC, Code.Linear + 3);
      EXPECT_EQ(R.GPR[0], 0u);
    } else {
      EXPECT_EQ(Step.Completions, 0u);
      EXPECT_EQ(Step.Status, X86LegacyExecutionStatus::ModeUnavailable);
      const char *Reason = B == Binding::Absent   ? "platform identity"
                           : B == Binding::Legacy ? "authenticator"
                                                  : "status-flag";
      EXPECT_NE(Step.OwnerError.find(Reason), std::string::npos);
      EXPECT_EQ(R.PC, Code.Linear);
      EXPECT_EQ(R.GPR[0], 0xfedcba9800000008ull);
    }
    EXPECT_EQ(R.RFlags, Before);
  }
}
