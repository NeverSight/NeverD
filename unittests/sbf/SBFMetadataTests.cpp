//===- SBFMetadataTests.cpp - Solana SBF definition-table tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/Opcodes.h"
#include "neverd/sbf/Relocations.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/Syscalls.h"
#include "neverd/sbf/Version.h"

#include <array>
#include <set>

namespace neverd::sbf {
namespace {

TEST(SBFVersions, RoundTripsEveryPublicSpelling) {
  EXPECT_EQ(parseVersion("auto"), Version::Auto);
  for (const VersionInfo &Info : versionInfos()) {
    EXPECT_EQ(parseVersion(Info.Name), Info.Value);
    EXPECT_EQ(versionName(Info.Value), Info.Name);
    EXPECT_EQ(versionDisplayName(Info.Value), Info.DisplayName);
    EXPECT_EQ(versionFromELFFlags(Info.ELFFlags), Info.Value);
  }
  EXPECT_FALSE(parseVersion("v5"));
  EXPECT_EQ(versionFromELFFlags(5), Version::Reserved);
}

TEST(SBFVersions, ModelsNonMonotonicFeatureSetsExplicitly) {
  EXPECT_TRUE(versionHasFeature(Version::V0, VersionFeature::StackFrameGaps));
  EXPECT_TRUE(
      versionHasFeature(Version::V1, VersionFeature::ManualStackFrames));
  EXPECT_TRUE(versionHasFeature(Version::V2, VersionFeature::PQR));
  EXPECT_TRUE(versionHasFeature(Version::V2, VersionFeature::SwapSubImmediate));
  EXPECT_FALSE(versionHasFeature(Version::V3, VersionFeature::PQR));
  EXPECT_TRUE(versionHasFeature(Version::V3, VersionFeature::StaticSyscalls));
  EXPECT_TRUE(versionHasFeature(Version::V3, VersionFeature::JMP32));
  EXPECT_FALSE(
      versionHasFeature(Version::V3, VersionFeature::AlignedMemoryMapping));
  EXPECT_TRUE(
      versionHasFeature(Version::V4, VersionFeature::AlignedMemoryMapping));
}

TEST(SBFOpcodes, HasNoEncodingCollisionWithinAnyVersion) {
  constexpr std::array Versions{Version::V0, Version::V1, Version::V2,
                                Version::V3, Version::V4};
  for (Version TheVersion : Versions) {
    SCOPED_TRACE(versionName(TheVersion).str());
    std::array<const OpcodeInfo *, 256> ByEncoding{};
    for (const OpcodeInfo &Info : opcodeInfos()) {
      EXPECT_EQ(getOpcodeInfo(Info.ID), &Info);
      if (!Info.isAvailableIn(TheVersion))
        continue;
      ASSERT_EQ(ByEncoding[Info.Encoding], nullptr)
          << "duplicate encoding 0x" << std::hex << unsigned(Info.Encoding);
      ByEncoding[Info.Encoding] = &Info;
      EXPECT_EQ(getOpcodeInfo(Info.Encoding, TheVersion), &Info);
    }
    ASSERT_NE(getOpcodeInfo(Opcode::EXIT), nullptr);
    EXPECT_EQ(getOpcodeInfo(getOpcodeInfo(Opcode::EXIT)->Encoding, TheVersion),
              getOpcodeInfo(Opcode::EXIT));
  }
  EXPECT_EQ(getOpcodeInfo(0x2c, Version::V2)->ID, Opcode::LD_1B_REG);
  EXPECT_EQ(getOpcodeInfo(0x2c, Version::V3)->ID, Opcode::MUL32_REG);
  EXPECT_EQ(getOpcodeInfo(0xf7, Version::V2)->ID, Opcode::HOR64_IMM);
  EXPECT_EQ(getOpcodeInfo(0xf7, Version::V3), nullptr);

  const OpcodeInfo *Load = getOpcodeInfo(Opcode::LD_DW_REG);
  const OpcodeInfo *Add = getOpcodeInfo(Opcode::ADD64_REG);
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Add, nullptr);
  ASSERT_NE(Call, nullptr);
  EXPECT_TRUE(Load->readsMemory());
  EXPECT_TRUE(Load->mayFault());
  EXPECT_FALSE(Add->mayFault());
  EXPECT_TRUE(Call->isCall());
  EXPECT_TRUE(Call->mayFault());
}

TEST(SBFSyscalls, HashesAndLookupsAreStableAndUnique) {
  EXPECT_EQ(hashSymbolName("log"), 0x6bf5c3feu);
  std::set<uint32_t> Hashes;
  std::set<std::string> Names;
  for (const SyscallInfo &Info : syscallInfos()) {
    EXPECT_TRUE(Hashes.insert(Info.Hash).second) << Info.Name.str();
    EXPECT_TRUE(Names.insert(Info.Name.str()).second) << Info.Name.str();
    EXPECT_EQ(Info.Hash, hashSymbolName(Info.Name));
    EXPECT_LE(Info.ArgumentCount, kArgumentRegisterCount);
    EXPECT_NE(syscallAvailabilityName(Info.Availability), "unknown");
    EXPECT_NE(syscallSourceName(Info.Source), "unknown");
    if (Info.ReturnKind == SyscallReturnKind::Never)
      EXPECT_TRUE(hasEffect(Info.Effects, SyscallEffect::Terminal));
    if (hasEffect(Info.Effects, SyscallEffect::Terminal))
      EXPECT_EQ(Info.ReturnKind, SyscallReturnKind::Never);
    if (Info.Category == SyscallCategory::CPI)
      EXPECT_TRUE(hasEffect(Info.Effects, SyscallEffect::CPI));
    EXPECT_EQ(getSyscallInfo(Info.Hash), &Info);
    EXPECT_EQ(getSyscallInfo(Info.ID), &Info);
    EXPECT_EQ(findSyscallByName(Info.Name), &Info);
  }
  EXPECT_EQ(getSyscallInfo(0), nullptr);

  std::set<std::string> SourceNames;
  for (const SyscallSourceInfo &Source : syscallSourceInfos()) {
    EXPECT_TRUE(SourceNames.insert(Source.Name.str()).second);
    EXPECT_EQ(syscallSourceName(Source.ID), Source.Name);
    EXPECT_EQ(syscallSourceRevision(Source.ID), Source.Revision);
  }
}

TEST(SBFSyscalls, MatchesCurrentStableABIAndTracksProposalsSeparately) {
  const auto ExpectArity = [](llvm::StringRef Name, uint8_t Arity) {
    const SyscallInfo *Info = findSyscallByName(Name);
    ASSERT_NE(Info, nullptr) << Name.str();
    EXPECT_EQ(Info->ArgumentCount, Arity) << Name.str();
  };
  ExpectArity("sol_panic_", 4);
  ExpectArity("sol_curve_validate_point", 2);
  ExpectArity("sol_curve_group_op", 5);
  ExpectArity("sol_poseidon", 5);
  ExpectArity("sol_alt_bn128_compression", 4);
  ExpectArity("sol_big_mod_exp", 2);
  ExpectArity("sol_get_processed_sibling_instruction", 5);

  const SyscallInfo *Sha512 = findSyscallByName("sol_sha512");
  const SyscallInfo *Decompress = findSyscallByName("sol_curve_decompress");
  const SyscallInfo *Pairing = findSyscallByName("sol_curve_pairing_map");
  ASSERT_NE(Sha512, nullptr);
  ASSERT_NE(Decompress, nullptr);
  ASSERT_NE(Pairing, nullptr);
  EXPECT_EQ(Sha512->Availability, SyscallAvailability::Proposed);
  EXPECT_EQ(Decompress->Availability, SyscallAvailability::FeatureGated);
  EXPECT_EQ(Pairing->Availability, SyscallAvailability::FeatureGated);
  EXPECT_EQ(Sha512->Source, SyscallSource::AgaveMaster);
  EXPECT_EQ(syscallSourceRevision(SyscallSource::AgaveMaster),
            "cae40aa610fdbdb313209bc1eec737079eb59688");
  EXPECT_EQ(Decompress->ArgumentCount, 3u);
  EXPECT_EQ(Pairing->ArgumentCount, 5u);

  constexpr std::array FeatureGated{
      "sol_blake3",
      "sol_poseidon",
      "sol_curve_validate_point",
      "sol_curve_group_op",
      "sol_curve_multiscalar_mul",
      "sol_alt_bn128_group_op",
      "sol_alt_bn128_compression",
      "sol_big_mod_exp",
      "sol_get_last_restart_slot",
      "sol_get_sysvar",
      "sol_get_epoch_stake",
      "sol_remaining_compute_units",
  };
  for (llvm::StringRef Name : FeatureGated) {
    const SyscallInfo *Info = findSyscallByName(Name);
    ASSERT_NE(Info, nullptr) << Name.str();
    EXPECT_EQ(Info->Availability, SyscallAvailability::FeatureGated)
        << Name.str();
  }
}

TEST(SBFRelocations, CentralTableMatchesTheELFABI) {
  ASSERT_EQ(relocationInfos().size(), 3u);
  EXPECT_EQ(getRelocationInfo(static_cast<uint32_t>(Relocation::Abs64))->ID,
            Relocation::Abs64);
  EXPECT_EQ(
      getRelocationInfo(static_cast<uint32_t>(Relocation::Relative64))->ID,
      Relocation::Relative64);
  EXPECT_EQ(getRelocationInfo(static_cast<uint32_t>(Relocation::Call32))->ID,
            Relocation::Call32);
  EXPECT_EQ(getRelocationInfo(0), nullptr);
  EXPECT_EQ(kELFMachineBPF, 247u);
  EXPECT_EQ(kELFMachineSBPF, 263u);
  EXPECT_EQ(kInstructionSize, 8u);
  EXPECT_EQ(kRegisterCount, 11u);
}

} // namespace
} // namespace neverd::sbf
