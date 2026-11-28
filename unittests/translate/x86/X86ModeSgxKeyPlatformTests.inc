//===- X86ModeSgxKeyPlatformTests.inc - Bound SGX key platform
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check key version priority and the complete public Intel SDK report image.
//===----------------------------------------------------------------------===//

// Intel's x64 SDK simulation verified this public report for its self target.
// https://github.com/gmh5225/test_mac_intel/actions/runs/46139290670
constexpr llvm::StringLiteral PublicTargetHex =
    "50269c895112290d0e39fc575d69d65dfd6c339c72c10fb87fc21e93ed529700"
    "0700000000000000070000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr llvm::StringLiteral PublicReportHex =
    "4820f3376ae6b2f2034d3b7a4b48a77800000000000000000000000000000000"
    "0000000000000000000000000000000007000000000000000700000000000000"
    "50269c895112290d0e39fc575d69d65dfd6c339c72c10fb87fc21e93ed529700"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "d876dcd61820de4a91e846fc4301d9425a98d7827db83dc4f2e90f35c28423f0"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0104070a0d101316191c1f2225282b2e3134373a3d404346494c4f5255585b5e"
    "6164676a6d707376797c7f8285888b8e9194979a9da0a3a6a9acafb2b5b8bbbe"
    "ff00ff00ff00ff00ff00ff00ff00ff0000000000000000000000000000000000"
    "6f994ab4dd7e59998c507d3909bb87c2";

class SgxReferencePlatform final : public X86SgxKeyPlatform {
public:
  std::string Target = llvm::fromHex(PublicTargetHex);
  std::string Report = llvm::fromHex(PublicReportHex);
  mutable unsigned VersionChecks = 0, Derivations = 0, MACs = 0;

  bool acceptsCPUSVN(std::span<const uint8_t, 16> Requested) const override {
    ++VersionChecks;
    return Requested.front() == 0x44;
  }

  llvm::Expected<std::array<uint8_t, 16>>
  deriveKey(const X86SgxPage &,
            std::span<const uint8_t, 512> Request) const override {
    ++Derivations;
    // A synthetic result makes the successful key store observable.
    std::array<uint8_t, 16> Key;
    std::copy_n(Request.begin() + 40, Key.size(), Key.begin());
    return Key;
  }

  llvm::Expected<X86SgxReportIdentity>
  reportIdentity(const X86SgxPage &) const override {
    X86SgxReportIdentity Identity{};
    std::copy_n(Report.begin(), 16, Identity.CPUSVN.begin());
    std::copy_n(Report.begin() + 32, 16, Identity.ISVExtProdID.begin());
    std::copy_n(Report.begin() + 304, 16, Identity.ISVFamilyID.begin());
    std::copy_n(Report.begin() + 384, 32, Identity.ReportKeyID.begin());
    return Identity;
  }

  llvm::Expected<std::array<uint8_t, 16>>
  reportMAC(const X86SgxPage &, std::span<const uint8_t, 512> TargetInfo,
            std::span<const uint8_t, 32> KeyID,
            std::span<const uint8_t, 384> Body) const override {
    ++MACs;
    if (std::memcmp(TargetInfo.data(), Target.data(), TargetInfo.size()) ||
        std::memcmp(KeyID.data(), Report.data() + 384, KeyID.size()) ||
        std::memcmp(Body.data(), Report.data(), Body.size()))
      return llvm::createStringError(
          "report inputs differ from the signed SDK simulation");
    std::array<uint8_t, 16> MAC;
    std::copy_n(Report.begin() + 416, 16, MAC.begin());
    return MAC;
  }
};

TEST_F(X86ModeSgxThreadTest, BoundKeyPlatformChecksVersionsBeforeWriting) {
  struct Case {
    uint16_t Name, ISV, Config;
    bool ValidCPU;
    unsigned Error;
  };
  for (Case C : {Case{4, 11, 12, false, 32},
                 {4, 11, 12, true, 64},
                 {4, 2, 12, true, 64},
                 {4, 3, 5, true, 0},
                 {3, 65535, 65535, false, 0},
                 {0, 3, 50, true, 0},
                 {1, 3, 50, false, 32},
                 {1, 4, 50, true, 64},
                 {2, 3, 50, true, 0},
                 {0xffff, 3, 0, false, 256}}) {
    SCOPED_TRACE(testing::Message() << C.Name << " " << C.ISV << " " << C.Config
                                    << " " << C.ValidCPU);
    startThread();
    privateInstruction(0, {0x67, 0x0f, 1, 0xd7});
    auto *Request = Code.Bytes.data() + 512;
    namespace le = llvm::support::endian;
    le::write16le(Request, C.Name);
    le::write16le(Request + 4, C.ISV);
    le::write16le(Request + 76, C.Config);
    Request[8] = C.ValidCPU ? 0x44 : 0x03;
    for (unsigned I = 0; I != 16; ++I)
      Request[40 + I] = 0x80 + I;
    le::write64le(Secs.Bytes.data() + 48, 5 | X86SgxFeatures::InitToken |
                                              X86SgxFeatures::Provision |
                                              X86SgxFeatures::KSS);
    le::write16le(Secs.Bytes.data() + 258, 3);
    le::write16le(Secs.Bytes.data() + 260, 5);
    std::fill_n(Previous.Bytes.begin() + 512, 16, 0x5a);
    bindThread({.ProvisionKey = true,
                .InitTokenKey = true,
                .KeySeparationAndSharing = true});
    auto Platform = std::make_shared<SgxReferencePlatform>();
    Memory->sgxMemory()->bindKeyPlatform(Platform);
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Enter = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Enter.Completions, 1u) << Enter.OwnerError;
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = 0xf00d000000000001ull;
    R.GPR[3] = Code.Linear + 512;
    R.GPR[1] = Previous.Linear + 512;
    R.RFlags |= 0x108d5;
    const auto Flags = R.RFlags;
    Budget = 1;
    auto Done = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Done.Completions, 1u) << Done.OwnerError;
    EXPECT_EQ(Done.Attempts, 1u);
    EXPECT_EQ(R.PC, Code.Linear + 4);
    EXPECT_EQ(R.GPR[0], C.Error);
    EXPECT_EQ(R.RFlags, (Flags & ~uint64_t{0x108d5}) | (C.Error ? 0x40 : 0));
    EXPECT_EQ(Platform->VersionChecks, unsigned(C.Name <= 4 && C.Name != 3));
    EXPECT_EQ(Platform->Derivations, unsigned(!C.Error));
    std::array<uint8_t, 16> Output;
    ASSERT_EQ(llvm::cantFail(
                  Memory->load({R.GPR[1], MemoryAccessKind::Read, 3}, Output))
                  .Status,
              X86PageWalkStatus::Completed);
    for (unsigned I = 0; I != Output.size(); ++I)
      EXPECT_EQ(Output[I], C.Error ? 0x5a : Request[40 + I]) << I;
  }
}

TEST_F(X86ModeSgxThreadTest,
       LocalReportMatchesIntelSDKWithAliasedDataAndOutput) {
  for (bool Altered : {false, true}) {
    SCOPED_TRACE(Altered);
    startThread();
    privateInstruction(0, {0x67, 0x0f, 1, 0xd7});
    auto Platform = std::make_shared<SgxReferencePlatform>();
    ASSERT_EQ(Platform->Target.size(), 512u);
    ASSERT_EQ(Platform->Report.size(), 432u);
    const auto *Target = Platform->Target.data();
    std::copy_n(Target, 512, Code.Bytes.begin() + 512);
    // The input enclave identity comes from the independent self-target.
    std::copy_n(Target, 32, Secs.Bytes.begin() + 64);
    std::copy_n(Target + 32, 16, Secs.Bytes.begin() + 48);
    std::copy_n(Platform->Report.begin() + 128, 32, Secs.Bytes.begin() + 128);
    std::fill_n(Previous.Bytes.begin() + 512, 512, 0xa5);
    for (unsigned I = 0; I != 64; ++I)
      Previous.Bytes[512 + I] = 3 * I + 1;
    if (Altered)
      Previous.Bytes[575] ^= 1;
    const auto Original = Previous.Bytes;
    Controls.CR4 |= uint64_t{1} << 18;
    Options.Legacy.ControlRegisters->OtherCR4Features |= 1u << 18;
    bindThread();
    auto &EPC = *Memory->sgxMemory();
    llvm::cantFail(EPC.bindX87(*EPC.x87(), 7));
    EPC.bindKeyPlatform(Platform);
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Enter = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Enter.Completions, 1u) << Enter.OwnerError;
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = 0xf00d000000000000ull;
    R.GPR[3] = Code.Linear + 512;
    R.GPR[1] = R.GPR[2] = Previous.Linear + 512;
    R.RFlags |= 0x108d5;
    const auto Flags = R.RFlags;
    Budget = 1;
    const auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
    EXPECT_EQ(Step.Attempts, 1u);
    EXPECT_EQ(Platform->MACs, 1u);
    std::array<uint8_t, 512> Output;
    ASSERT_EQ(llvm::cantFail(
                  Memory->load({R.GPR[2], MemoryAccessKind::Read, 3}, Output))
                  .Status,
              X86PageWalkStatus::Completed);
    EXPECT_EQ(R.GPR[0], 0xf00d000000000000ull);
    if (Altered) {
      EXPECT_EQ(Step.Completions, 0u);
      EXPECT_EQ(Step.Status, X86LegacyExecutionStatus::ModeUnavailable);
      EXPECT_EQ(R.PC, Code.Linear);
      EXPECT_EQ(R.RFlags, Flags);
      EXPECT_EQ(std::memcmp(Output.data(), Original.data() + 512, 512), 0);
    } else {
      ASSERT_EQ(Step.Completions, 1u) << Step.OwnerError;
      EXPECT_EQ(R.PC, Code.Linear + 4);
      EXPECT_EQ(R.RFlags, Flags & ~uint64_t{0x10000});
      EXPECT_EQ(std::memcmp(Output.data(), Platform->Report.data(), 432), 0);
      EXPECT_EQ(std::memcmp(Output.data() + 432, Original.data() + 944, 80), 0);
    }
  }
}

TEST_F(X86ModeSgxThreadTest,
       ReportSerializesKssIdentityAndZeroesReservedBytes) {
  startThread();
  privateInstruction(0, Enclu);
  auto Platform = std::make_shared<SgxReferencePlatform>();
  Platform->Target.assign(512, 0);
  Platform->Report.assign(432, 0);
  auto &Expected = Platform->Report;
  namespace le = llvm::support::endian;
  le::write32le(Secs.Bytes.data() + 20, 1);
  le::write64le(Secs.Bytes.data() + 48, 5 | X86SgxFeatures::KSS);
  Expected[16] = 1;
  Expected[48] = static_cast<char>(5 | X86SgxFeatures::KSS);
  Expected[56] = 3;
  for (unsigned I = 0; I != 32; ++I) {
    Secs.Bytes[64 + I] = Expected[64 + I] = 0x21 + I;
    Secs.Bytes[128 + I] = Expected[128 + I] = 0x41 + I;
    Expected[384 + I] = 0x71 + I;
  }
  for (unsigned I = 0; I != 70; ++I)
    Secs.Bytes[192 + I] = Expected[192 + I] = 0x10 + I;
  for (unsigned I = 0; I != 16; ++I) {
    Expected[I] = 0xa0 + I;
    Expected[32 + I] = 0xb0 + I;
    Expected[304 + I] = 0xc0 + I;
    Expected[416 + I] = 0xd0 + I;
  }
  for (unsigned I = 0; I != 64; ++I)
    Previous.Bytes[I] = Expected[320 + I] = 0xe0 ^ I;
  std::fill_n(Previous.Bytes.begin() + 512, 512, 0xff);
  bindThread({.KeySeparationAndSharing = true});
  Memory->sgxMemory()->bindKeyPlatform(Platform);
  X86ModeExecution Execution(*Memory, Options);
  uint64_t Budget = 1;
  auto Enter = llvm::cantFail(Execution.run(Machine, Program, Budget));
  ASSERT_EQ(Enter.Completions, 1u) << Enter.OwnerError;
  auto &R = Machine.Core.Core.Core.Core.Core.Core;
  R.GPR[0] = 0;
  R.GPR[3] = Code.Linear + 512;
  R.GPR[1] = Previous.Linear;
  R.GPR[2] = Previous.Linear + 512;
  Budget = 1;
  auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
  ASSERT_EQ(Step.Completions, 1u) << Step.OwnerError;
  ASSERT_EQ(Platform->MACs, 1u);
  std::array<uint8_t, 432> Actual;
  ASSERT_EQ(llvm::cantFail(
                Memory->load({R.GPR[2], MemoryAccessKind::Read, 3}, Actual))
                .Status,
            X86PageWalkStatus::Completed);
  EXPECT_EQ(std::memcmp(Actual.data(), Expected.data(), Actual.size()), 0);
}
