//===- X86ModeSgxWideKeyReportTests.inc - SGX 256-bit leaves --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Software EGETKEY256 and EREPORT2 on initialized x64 enclaves.
//===----------------------------------------------------------------------===//

class SgxWideKeyPlatform final : public SgxReferencePlatform {
public:
  mutable unsigned WideDerivations = 0;

  llvm::Expected<std::array<uint8_t, 32>>
  deriveKey256(const X86SgxPage &SECS,
               std::span<const uint8_t, 512> Request) const override {
    ++WideDerivations;
    const auto Attributes =
        llvm::support::endian::read64le(SECS.Bytes.data() + 48);
    const uint8_t Salt = Attributes & X86SgxFeatures::SHA384 ? 0xa3 : 0x5c;
    std::array<uint8_t, 32> Bytes;
    for (unsigned I = 0; I != Bytes.size(); ++I)
      Bytes[I] = Request[71 - I] ^ Salt;
    return Bytes;
  }
};

TEST_F(X86ModeSgxThreadTest, WideKeyWritesExactly32BytesWithAnAliasedRequest) {
  for (bool SHA384 : {false, true})
    for (uint16_t Name : {0, 1, 2, 3, 4, 65535}) {
      SCOPED_TRACE(testing::Message() << Name << " SHA384=" << SHA384);
      startThread();
      privateInstruction(0, {0x67, 0x0f, 1, 0xd7});
      auto *Request = Previous.Bytes.data() + 512;
      llvm::support::endian::write16le(Request, Name);
      Request[8] = 0x44;
      for (unsigned I = 0; I != 32; ++I)
        Request[40 + I] = 1 + 3 * I;
      const uint64_t Attributes = 5 | X86SgxFeatures::InitToken |
                                  X86SgxFeatures::Provision |
                                  (SHA384 ? X86SgxFeatures::SHA384 : 0);
      llvm::support::endian::write64le(Secs.Bytes.data() + 48, Attributes);
      bindThread({.ProvisionKey = true,
                  .InitTokenKey = true,
                  .SGX256 = true,
                  .SHA384Measurements = SHA384});
      auto Platform = std::make_shared<SgxWideKeyPlatform>();
      Memory->sgxMemory()->bindKeyPlatform(Platform);
      X86ModeExecution Execution(*Memory, Options);
      uint64_t Budget = 1;
      ASSERT_EQ(
          llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
          1u);
      auto &R = Machine.Core.Core.Core.Core.Core.Core;
      R.GPR[0] = 0xfedcba980000000bull;
      R.GPR[3] = Previous.Linear + 512;
      R.GPR[1] = R.GPR[3] + 32;
      R.RFlags |= 0x108d5;
      const auto Flags = R.RFlags;
      Budget = 1;
      auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
      const bool Allowed = Name == 0 || Name == 2 || Name == 4;
      ASSERT_EQ(Step.Completions, 1u) << Step.OwnerError;
      EXPECT_EQ(R.PC, Code.Linear + 4);
      EXPECT_EQ(R.GPR[0], Allowed ? 0u : 256u);
      EXPECT_EQ(R.RFlags, (Flags & ~uint64_t{0x108d5}) | (Allowed ? 0 : 0x40));
      EXPECT_EQ(Platform->VersionChecks, unsigned(Allowed));
      EXPECT_EQ(Platform->WideDerivations, unsigned(Allowed));
      EXPECT_EQ(Platform->Derivations, 0u);
      std::array<uint8_t, 512> Actual, Expected;
      std::copy_n(Request, Expected.size(), Expected.begin());
      if (Allowed)
        for (unsigned I = 0; I != 32; ++I)
          Expected[32 + I] = (1 + 3 * (31 - I)) ^ (SHA384 ? 0xa3 : 0x5c);
      ASSERT_EQ(llvm::cantFail(
                    Memory->load({R.GPR[3], MemoryAccessKind::Read, 3}, Actual))
                    .Status,
                X86PageWalkStatus::Completed);
      EXPECT_EQ(Actual, Expected);
    }
}

TEST_F(X86ModeSgxThreadTest, WideKeyDoesNotReuseThe128BitProvider) {
  startThread();
  privateInstruction(0, Enclu);
  auto *Request = Code.Bytes.data() + 512;
  Request[0] = 4;
  Request[8] = 0x44;
  std::fill_n(Previous.Bytes.begin() + 512, 32, 0xc1);
  bindThread({.SGX256 = true});
  auto Platform = std::make_shared<SgxReferencePlatform>();
  Memory->sgxMemory()->bindKeyPlatform(Platform);
  X86ModeExecution Execution(*Memory, Options);
  uint64_t Budget = 1;
  ASSERT_EQ(llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
            1u);
  auto &R = Machine.Core.Core.Core.Core.Core.Core;
  R.GPR[0] = 11;
  R.GPR[3] = Code.Linear + 512;
  R.GPR[1] = Previous.Linear + 512;
  const auto Flags = R.RFlags;
  Budget = 1;
  auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
  EXPECT_EQ(Step.Status, X86LegacyExecutionStatus::ModeUnavailable);
  EXPECT_NE(Step.OwnerError.find("256-bit key provider"), std::string::npos);
  EXPECT_EQ(Step.Completions, 0u);
  EXPECT_EQ(Platform->Derivations, 0u);
  EXPECT_EQ(R.PC, Code.Linear);
  EXPECT_EQ(R.GPR[0], 11u);
  EXPECT_EQ(R.RFlags, Flags);
  std::array<uint8_t, 32> Output;
  ASSERT_EQ(llvm::cantFail(
                Memory->load({R.GPR[1], MemoryAccessKind::Read, 3}, Output))
                .Status,
            X86PageWalkStatus::Completed);
  EXPECT_TRUE(std::all_of(Output.begin(), Output.end(),
                          [](uint8_t B) { return B == 0xc1; }));
}

TEST_F(X86ModeSgxThreadTest,
       Report2RejectsTheFullSubtypeAfterCheckingOperands) {
  for (uint64_t Subtype : {uint64_t{0}, uint64_t{1}, uint64_t{1} << 32}) {
    SCOPED_TRACE(Subtype);
    startThread();
    privateInstruction(0, Enclu);
    std::fill_n(Next.Bytes.begin(), 1024, 0xc3);
    bindThread({.SGX256 = true});
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = 10;
    R.GPR[3] = Code.Linear + 64;
    R.GPR[1] = Next.Linear;
    R.GPR[2] = Subtype;
    R.RFlags |= 0x108d5;
    const auto Flags = R.RFlags;
    Budget = 1;
    auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
    if (Subtype) {
      ASSERT_EQ(Step.Completions, 1u) << Step.OwnerError;
      EXPECT_EQ(R.PC, Code.Linear + 3);
      EXPECT_EQ(R.GPR[0], 33u);
      EXPECT_EQ(R.RFlags, (Flags & ~uint64_t{0x108d5}) | 0x40);
    } else {
      EXPECT_EQ(Step.Status, X86LegacyExecutionStatus::ModeUnavailable);
      EXPECT_EQ(Step.Completions, 0u);
      EXPECT_EQ(R.PC, Code.Linear);
      EXPECT_EQ(R.GPR[0], 10u);
      EXPECT_EQ(R.RFlags, Flags);
    }
    std::array<uint8_t, 1024> Output;
    ASSERT_EQ(llvm::cantFail(
                  Memory->load({R.GPR[1], MemoryAccessKind::Read, 3}, Output))
                  .Status,
              X86PageWalkStatus::Completed);
    EXPECT_TRUE(std::all_of(Output.begin(), Output.end(),
                            [](uint8_t B) { return B == 0xc3; }));
  }
}

namespace {
// These vectors use Python hashlib and hmac with the public test key 0..31.
struct Report2Vector {
  const char *PrefixSHA256, *MAC, *PayloadSHA256;
};
constexpr Report2Vector Report2Vectors[] = {
    {"1ae20152cdfe04519d2aa8e5a5024f5f3a648b0fa7349a15a9b3708a4e252713",
     "a002a23b03b4f6747783f1e0901cfad8ac363229d0de73e6cd01edeb998250e3",
     "0af0006833bc15df0c94ccedf5294bf9d9d4cbefc05c7c9c73ab060b247dbc9a"},
    {"a574979c93fa083697f7d7edf7bec1949557d196c2115f5a672a523f4fd98248",
     "2fe6b834348d86a40222fdcf9796eea1be62174b2e3c83931e3a1faa06393082",
     "39219c67d3df2dc4ec51255d46924a17c838ff8b3f32285f18d300bcbb073bad"},
};

class SgxReport2Platform final : public SgxReferencePlatform {
public:
  mutable unsigned Measurements = 0, Authentications = 0;
  llvm::Expected<X86SgxReportIdentity>
  reportIdentity(const X86SgxPage &) const override {
    X86SgxReportIdentity Identity{};
    for (unsigned I = 0; I != 16; ++I) {
      Identity.CPUSVN[I] = 0x21 + I;
      Identity.ISVExtProdID[I] = 0x31 + I;
      Identity.ISVFamilyID[I] = 0x41 + I;
    }
    return Identity;
  }
  llvm::Expected<X86SgxMeasurements384>
  measurements384(const X86SgxPage &) const override {
    ++Measurements;
    X86SgxMeasurements384 M;
    for (unsigned I = 0; I != M.MREnclave.size(); ++I) {
      M.MREnclave[I] = 0x80 + I;
      M.MRSigner[I] = 0xb0 + I;
    }
    return M;
  }
  llvm::Expected<std::array<uint8_t, 32>>
  report2MAC(const X86SgxPage &Secs,
             std::span<const uint8_t, 192> Prefix) const override {
    ++Authentications;
    const auto Attributes =
        llvm::support::endian::read64le(Secs.Bytes.data() + 48);
    const auto &V = Report2Vectors[bool(Attributes & X86SgxFeatures::SHA384)];
    if (llvm::toHex(llvm::SHA256::hash({Prefix.data(), Prefix.size()}), true) !=
        V.PrefixSHA256)
      return llvm::createStringError(
          "REPORT2 authenticator input differs from its independent vector");
    const auto Bytes = llvm::fromHex(V.MAC);
    std::array<uint8_t, 32> MAC;
    std::copy_n(Bytes.begin(), MAC.size(), MAC.begin());
    return MAC;
  }
};
} // namespace

TEST_F(X86ModeSgxThreadTest, Report2AuthenticatesAllFieldsBeforePublishing) {
  for (bool SHA384 : {false, true})
    for (auto Footprint :
         {X86SgxReport2Write::Payload768, X86SgxReport2Write::ZeroReservedTail})
      for (bool AlterData : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << SHA384 << " footprint=" << int(Footprint)
                     << " altered=" << AlterData);
        startThread();
        privateInstruction(0, {0x67, 0x0f, 1, 0xd7});
        namespace le = llvm::support::endian;
        le::write32le(Secs.Bytes.data() + 20, 1);
        le::write64le(Secs.Bytes.data() + 48,
                      5 | X86SgxFeatures::KSS |
                          (SHA384 ? X86SgxFeatures::SHA384 : 0));
        for (unsigned I = 0; I != 6; ++I)
          Secs.Bytes[256 + I] = 0x11 * (I + 1);
        for (unsigned I = 0; I != 32; ++I) {
          Secs.Bytes[64 + I] = 0x10 + I;
          Secs.Bytes[128 + I] = 0x40 + I;
        }
        std::fill_n(Next.Bytes.begin(), 1024, 0xcc);
        for (unsigned I = 0; I != 64; ++I) {
          Secs.Bytes[192 + I] = 0x60 + I;
          Next.Bytes[128 + I] = 5 + 3 * I;
        }
        if (AlterData)
          Next.Bytes[191] ^= 1;
        bindThread(
            {.KeySeparationAndSharing = true,
             .SGX256 = true,
             .SHA384Measurements = SHA384,
             .Report2Write = Footprint,
             .Report2MACCoverage = X86SgxReport2MACCoverage::Operation192});
        auto Platform = std::make_shared<SgxReport2Platform>();
        Memory->sgxMemory()->bindKeyPlatform(Platform);
        X86ModeExecution Execution(*Memory, Options);
        uint64_t Budget = 1;
        ASSERT_EQ(
            llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
            1u);
        auto &R = Machine.Core.Core.Core.Core.Core.Core;
        R.GPR[0] = 0xfedcba980000000aull;
        R.GPR[3] = Next.Linear + 128;
        R.GPR[1] = Next.Linear;
        R.GPR[2] = 0;
        R.RFlags |= 0x108d5;
        const auto Flags = R.RFlags;
        Budget = 1;
        auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
        EXPECT_EQ(Platform->Measurements, unsigned(SHA384));
        EXPECT_EQ(Platform->Authentications, 1u);
        std::array<uint8_t, 1024> Output;
        ASSERT_EQ(
            llvm::cantFail(
                Memory->load({R.GPR[1], MemoryAccessKind::Read, 3}, Output))
                .Status,
            X86PageWalkStatus::Completed);
        if (AlterData) {
          EXPECT_EQ(Step.Status, X86LegacyExecutionStatus::ModeUnavailable);
          EXPECT_EQ(Step.Completions, 0u);
          EXPECT_EQ(R.PC, Code.Linear);
          EXPECT_EQ(R.GPR[0], 0xfedcba980000000aull);
          EXPECT_EQ(R.RFlags, Flags);
          EXPECT_TRUE(
              std::equal(Output.begin(), Output.end(), Next.Bytes.begin()));
        } else {
          ASSERT_EQ(Step.Completions, 1u) << Step.OwnerError;
          EXPECT_EQ(R.PC, Code.Linear + 4);
          EXPECT_EQ(R.GPR[0], 0u);
          EXPECT_EQ(R.RFlags, Flags & ~uint64_t{0x108d5});
          EXPECT_EQ(llvm::toHex(llvm::SHA256::hash({Output.data(), 768}), true),
                    Report2Vectors[SHA384].PayloadSHA256);
          const auto Tail =
              Footprint == X86SgxReport2Write::Payload768 ? 0xcc : 0;
          EXPECT_TRUE(
              std::all_of(Output.begin() + 768, Output.end(),
                          [Tail](uint8_t Byte) { return Byte == Tail; }));
        }
      }
}

TEST_F(X86ModeSgxThreadTest, CompiledBodiesReadTheUpperHalfOfTheWideKey) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  unsigned Completed = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const auto *Level : {"-O0", "-O2"}) {
        SCOPED_TRACE(testing::Message()
                     << Compiler << " llvm=" << LLVM << ' ' << Level);
        startThread();
        privateInstruction(0, {0x67, 0x0f, 1, 0xd7});
        auto Transfers = std::move(Program);
        Program.clear();
        privateInstruction(4, {0x48, 0x8b, 0x41, 24}); // MOV RAX,[RCX+24]
        ASSERT_NO_FATAL_FAILURE(
            compileLong(LLVM, Level, Compiler, false, Code.Linear + 4));
        Program.insert(Program.end(), Transfers.begin(), Transfers.end());
        auto *Request = Code.Bytes.data() + 512;
        Request[0] = 4;
        Request[8] = 0x44;
        for (unsigned I = 0; I != 32; ++I)
          Request[40 + I] = I;
        bindThread({.SGX256 = true});
        Memory->sgxMemory()->bindKeyPlatform(
            std::make_shared<SgxWideKeyPlatform>());
        X86ModeExecution Execution(*Memory, Options);
        uint64_t Budget = 1;
        ASSERT_EQ(
            llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
            1u);
        auto &R = Machine.Core.Core.Core.Core.Core.Core;
        R.GPR[0] = 11;
        R.GPR[3] = Code.Linear + 512;
        R.GPR[1] = Next.Linear;
        Budget = 2;
        auto Done = llvm::cantFail(Execution.run(Machine, Program, Budget));
        ASSERT_EQ(Done.Completions, 2u) << Done.OwnerError;
        EXPECT_EQ(R.PC, Code.Linear + 8);
        EXPECT_EQ(R.GPR[0], 0x5c5d5e5f58595a5bull);
        ++Completed;
      }
  RecordProperty("compiled_executions", Completed);
  EXPECT_EQ(Completed, Compilers.size() * 4);
}
