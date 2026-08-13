//===- SBFMetadataTests.cpp - Solana SBF definition-table tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/runtime/SBFOpcodes.h"
#include "neverd/sbf/solana/SBFPubkey.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/runtime/SBFRuntimeProfile.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/image/SBFVersion.h"

#include "llvm/Support/Error.h"

#include <array>
#include <optional>
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

TEST(SBFVersions, AttributesEveryFeatureToOneDistinctBitAndProposal) {
  ASSERT_EQ(versionFeatureInfos().size(), kVersionFeatureCount);

  uint32_t Seen = 0;
  for (size_t Index = 0; Index < versionFeatureInfos().size(); ++Index) {
    const VersionFeatureInfo &Info = versionFeatureInfos()[Index];
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(&getVersionFeatureInfo(static_cast<VersionFeatureBit>(Index)),
              &Info);
    EXPECT_EQ(versionFeatureName(Info.ID), Info.Name);
    EXPECT_FALSE(Info.Summary.empty());
    EXPECT_FALSE(Info.Upstream.empty());
    // A version number is not a specification; the proposal that decided the
    // change is what a recovered version claim traces back to.
    EXPECT_TRUE(Info.SIMD.starts_with("simd-")) << Info.SIMD.str();

    const auto Bit = static_cast<uint32_t>(Info.ID);
    EXPECT_EQ(Bit & (Bit - 1), 0u) << "a feature must occupy one bit";
    EXPECT_EQ(Seen & Bit, 0u) << "two features share a bit";
    Seen |= Bit;
  }

  // Every bit the version table composes has to be a tabulated feature, so a
  // version cannot claim a capability nothing describes.
  for (const VersionInfo &Info : versionInfos())
    EXPECT_EQ(static_cast<uint32_t>(Info.Features) & ~Seen, 0u)
        << Info.Name.str();
}

TEST(SBFVersions, RecordsTheProposalsUpstreamAttributesEachChangeTo) {
  // These are the attributions anza-xyz/sbpf documents on the predicates in
  // src/program.rs. Reading them off the version number instead would lose
  // that SIMD-0173 and SIMD-0174 both land in v2 and that neither carries
  // into v3.
  const auto Proposal = [](VersionFeature Feature) {
    for (const VersionFeatureInfo &Info : versionFeatureInfos())
      if (Info.ID == Feature)
        return Info.SIMD;
    return llvm::StringLiteral("");
  };
  EXPECT_EQ(Proposal(VersionFeature::StackFrameGaps), "simd-0166");
  EXPECT_EQ(Proposal(VersionFeature::ManualStackFrames), "simd-0166");
  EXPECT_EQ(Proposal(VersionFeature::PQR), "simd-0174");
  EXPECT_EQ(Proposal(VersionFeature::SwapSubImmediate), "simd-0174");
  EXPECT_EQ(Proposal(VersionFeature::MovedMemory), "simd-0173");
  EXPECT_EQ(Proposal(VersionFeature::DisableLDDW), "simd-0173");
  EXPECT_EQ(Proposal(VersionFeature::StaticSyscalls), "simd-0178");
  EXPECT_EQ(Proposal(VersionFeature::StrictELF), "simd-0189");
  EXPECT_EQ(Proposal(VersionFeature::LowerRodata), "simd-0189");
  EXPECT_EQ(Proposal(VersionFeature::JMP32), "simd-0377");
  EXPECT_EQ(Proposal(VersionFeature::CallXDestination), "simd-0377");
  EXPECT_EQ(Proposal(VersionFeature::AlignedMemoryMapping), "simd-0177");

  // The two callx rules come from different proposals and pick opposite
  // registers, which is why they are separate features rather than one.
  EXPECT_NE(Proposal(VersionFeature::CallXSource),
            Proposal(VersionFeature::CallXDestination));
  EXPECT_TRUE(versionHasFeature(Version::V2, VersionFeature::CallXSource));
  EXPECT_FALSE(versionHasFeature(Version::V3, VersionFeature::CallXSource));
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
    EXPECT_FALSE(syscallLifecycleName(Info.Lifecycle).empty());
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

TEST(SBFSyscalls, PointerArgumentsAgreeWithArityAndMemoryEffects) {
  for (const SyscallInfo &Info : syscallInfos()) {
    SCOPED_TRACE(Info.Name.str());
    for (unsigned Ordinal = 0; Ordinal < kArgumentRegisterCount; ++Ordinal)
      if (isPointerArgument(Info.PointerArguments, Ordinal))
        // A syscall cannot pass an address in a register it never reads.
        EXPECT_LT(Ordinal, Info.ArgumentCount);

    // Touching caller memory requires an address to touch. The converse does
    // not hold: the retired allocator takes an address and returns one without
    // reading or writing through it.
    if (hasEffect(Info.Effects, SyscallEffect::ReadsMemory) ||
        hasEffect(Info.Effects, SyscallEffect::WritesMemory))
      EXPECT_NE(Info.PointerArguments, SyscallPointerArguments::None);
  }

  const SyscallInfo *Memcmp = findSyscallByName("sol_memcmp_");
  ASSERT_NE(Memcmp, nullptr);
  EXPECT_TRUE(isPointerArgument(Memcmp->PointerArguments, 0));
  EXPECT_TRUE(isPointerArgument(Memcmp->PointerArguments, 1));
  // The third argument is a length, not an address.
  EXPECT_FALSE(isPointerArgument(Memcmp->PointerArguments, 2));
  EXPECT_TRUE(isPointerArgument(Memcmp->PointerArguments, 3));

  const SyscallInfo *Invoke = findSyscallByName("sol_invoke_signed_rust");
  ASSERT_NE(Invoke, nullptr);
  EXPECT_TRUE(isPointerArgument(Invoke->PointerArguments, 0));
  EXPECT_FALSE(isPointerArgument(Invoke->PointerArguments, 2));
  EXPECT_FALSE(isPointerArgument(Invoke->PointerArguments, 4));

  const SyscallInfo *Log64 = findSyscallByName("sol_log_64_");
  ASSERT_NE(Log64, nullptr);
  EXPECT_EQ(Log64->PointerArguments, SyscallPointerArguments::None);
}

TEST(SBFSyscalls, MemoryWindowsDescribeTheCallerBytesEachSyscallTouches) {
  llvm::Error TableError = validateSyscallMemoryTable();
  ASSERT_FALSE(static_cast<bool>(TableError))
      << llvm::toString(std::move(TableError));

  const auto Window = [](llvm::StringRef Name, SyscallArgument Argument,
                         SyscallMemoryAccess Access)
      -> const SyscallMemoryInfo * {
    const SyscallInfo *Info = findSyscallByName(Name);
    if (!Info)
      return nullptr;
    for (const SyscallMemoryInfo &Row : getSyscallMemory(Info->ID))
      if (Row.Argument == Argument && Row.Access == Access)
        return &Row;
    return nullptr;
  };

  // A copy is bounded by its own length argument, which is what lets recovery
  // keep the bytes a program wrote outside the copied range.
  const SyscallMemoryInfo *CopyOut =
      Window("sol_memcpy_", SyscallArgument::Arg1, SyscallMemoryAccess::Write);
  ASSERT_NE(CopyOut, nullptr);
  EXPECT_EQ(CopyOut->Extent, SyscallExtent::Counted);
  ASSERT_TRUE(CopyOut->lengthArgument().has_value());
  EXPECT_EQ(*CopyOut->lengthArgument(), SyscallArgument::Arg3);
  EXPECT_FALSE(CopyOut->fixedBytes().has_value());

  // A derived address is exactly one key wide wherever it is written.
  const SyscallMemoryInfo *Derived =
      Window("sol_try_find_program_address", SyscallArgument::Arg4,
             SyscallMemoryAccess::Write);
  ASSERT_NE(Derived, nullptr);
  ASSERT_TRUE(Derived->fixedBytes().has_value());
  EXPECT_EQ(*Derived->fixedBytes(), kPubkeyByteCount);
  EXPECT_FALSE(Derived->lengthArgument().has_value());

  // An invocation writes account data, which is not the caller's own memory,
  // so an instruction assembled before it is still that instruction after it.
  const SyscallInfo *Invoke = findSyscallByName("sol_invoke_signed_rust");
  ASSERT_NE(Invoke, nullptr);
  EXPECT_TRUE(preservesCallerMemory(Invoke->ID));
  EXPECT_TRUE(preservesCallerMemory(Syscall::Log));
  EXPECT_FALSE(preservesCallerMemory(Syscall::Memcmp));
  EXPECT_FALSE(preservesCallerMemory(Syscall::Unknown));

  for (const SyscallMemoryInfo &Row : syscallMemoryInfos()) {
    SCOPED_TRACE(getSyscallInfo(Row.ID)->Name.str());
    EXPECT_NE(syscallMemoryAccessName(Row.Access), "unknown");
    EXPECT_NE(syscallExtentName(Row.Extent), "unknown");
    EXPECT_LT(argumentOrdinal(Row.Argument), kArgumentRegisterCount);
    EXPECT_EQ(argumentRegister(Row.Argument),
              kFirstArgumentRegister + argumentOrdinal(Row.Argument));
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
  EXPECT_EQ(Sha512->Lifecycle, SyscallLifecycle::FeatureGated);
  EXPECT_EQ(Decompress->Lifecycle, SyscallLifecycle::FeatureGated);
  EXPECT_EQ(Pairing->Lifecycle, SyscallLifecycle::FeatureGated);
  EXPECT_EQ(Sha512->Source, SyscallSource::AgaveMaster);
  EXPECT_EQ(syscallSourceRevision(SyscallSource::AgaveMaster),
            "cae40aa610fdbdb313209bc1eec737079eb59688");
  EXPECT_EQ(syscallSourceRevision(SyscallSource::SBPFMain),
            "71425d0de59e0bff048c6be8f4a8a9bc655916e2");
  EXPECT_EQ(syscallSourceRevision(SyscallSource::SolanaSDKMaster),
            "d045b94c18f8cae8bc30eec310984030ead8d4f4");
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
    EXPECT_EQ(Info->Lifecycle, SyscallLifecycle::FeatureGated) << Name.str();
  }
}

//===----------------------------------------------------------------------===//
// Whether a call resolves, which is three questions
//===----------------------------------------------------------------------===//

TEST(SBFRuntimeProfile, TablesAgreeWithEachOther) {
  llvm::Error ProfileError = validateRuntimeProfileTables();
  ASSERT_FALSE(static_cast<bool>(ProfileError))
      << llvm::toString(std::move(ProfileError));
  llvm::Error RegistrationError = validateSyscallRegistrationTable();
  ASSERT_FALSE(static_cast<bool>(RegistrationError))
      << llvm::toString(std::move(RegistrationError));

  for (const ClusterInfo &Info : clusterInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(&getClusterInfo(Info.ID), &Info);
    EXPECT_EQ(parseCluster(Info.Name), Info.ID);
  }
  for (const LoaderInfo &Info : loaderInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(parseLoader(Info.Name), Info.ID);
    EXPECT_EQ(loaderForAddress(Info.Address), Info.ID);
    // Something that cannot execute cannot deploy either, or a program could
    // be accepted onto a chain that would then refuse to run it.
    if (Info.Deploys)
      EXPECT_TRUE(Info.Executes);
  }
  for (const RuntimeFeatureInfo &Info : runtimeFeatureInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(parseRuntimeFeature(Info.Name), Info.ID);
    EXPECT_EQ(parseRuntimeFeature(Info.Gate), Info.ID);
  }
}

TEST(SBFRuntimeProfile, AGateIsOnForOneChainAndOffForAnother) {
  RuntimeProfile Mainnet = currentMainnetProfile();
  EXPECT_EQ(Mainnet.OnCluster, Cluster::MainnetBeta);
  EXPECT_TRUE(isFeatureActive(Mainnet, RuntimeFeature::InstructionDataPointer));
  EXPECT_FALSE(isFeatureActive(Mainnet, RuntimeFeature::Sha512Syscall));

  RuntimeProfile Devnet = Mainnet;
  Devnet.OnCluster = Cluster::Devnet;
  EXPECT_TRUE(isFeatureActive(Devnet, RuntimeFeature::Sha512Syscall));

  // A slot before the activation is a different chain state, and the whole
  // point of recording the slot is that a program from that era is read in it.
  const std::optional<uint64_t> Activated = runtimeFeatureActivation(
      RuntimeFeature::InstructionDataPointer, Cluster::MainnetBeta);
  ASSERT_TRUE(Activated.has_value());
  RuntimeProfile Earlier = Mainnet;
  Earlier.Slot = *Activated - 1;
  EXPECT_FALSE(isFeatureActive(Earlier, RuntimeFeature::InstructionDataPointer));
  Earlier.Slot = *Activated;
  EXPECT_TRUE(isFeatureActive(Earlier, RuntimeFeature::InstructionDataPointer));

  // A validator started from genesis is neither of those chains.
  RuntimeProfile Local = Mainnet;
  Local.OnCluster = Cluster::Localnet;
  EXPECT_TRUE(isFeatureActive(Local, RuntimeFeature::Sha512Syscall));

  // An override describes a validator somebody configured, and outranks what
  // the chain did in both directions.
  RuntimeProfile Forced = Mainnet;
  Forced.Forced = RuntimeFeature::Sha512Syscall;
  EXPECT_TRUE(isFeatureActive(Forced, RuntimeFeature::Sha512Syscall));
  Forced.Suppressed = RuntimeFeature::Sha512Syscall;
  EXPECT_FALSE(isFeatureActive(Forced, RuntimeFeature::Sha512Syscall));
}

TEST(SBFSyscalls, RegistrationSeparatesTheGateFromTheRegistry) {
  const SyscallInfo *Sha512 = findSyscallByName("sol_sha512");
  const SyscallInfo *Fees = findSyscallByName("sol_get_fees_sysvar");
  const SyscallInfo *Alloc = findSyscallByName("sol_alloc_free_");
  ASSERT_NE(Sha512, nullptr);
  ASSERT_NE(Fees, nullptr);
  ASSERT_NE(Alloc, nullptr);

  RuntimeProfile Mainnet = currentMainnetProfile();
  RuntimeProfile Devnet = Mainnet;
  Devnet.OnCluster = Cluster::Devnet;

  // An adding gate: absent on one chain, present on another.
  EXPECT_EQ(syscallRegistration(Sha512->ID, Mainnet),
            SyscallRegistration::GateUnmet);
  EXPECT_EQ(syscallRegistration(Sha512->ID, Devnet),
            SyscallRegistration::Registered);

  // A removing gate reads exactly like an adding one unless the direction is
  // recorded. Activating this one is what took the syscall away, so it is
  // unavailable everywhere it has been activated.
  const SyscallGateInfo *FeesGate = getSyscallGate(Fees->ID);
  ASSERT_NE(FeesGate, nullptr);
  EXPECT_EQ(FeesGate->Polarity, SyscallGatePolarity::RequiresInactive);
  EXPECT_EQ(syscallRegistration(Fees->ID, Mainnet),
            SyscallRegistration::GateUnmet);
  RuntimeProfile Before = Mainnet;
  Before.Suppressed = RuntimeFeature::FeesSysvarDisabled;
  EXPECT_EQ(syscallRegistration(Fees->ID, Before),
            SyscallRegistration::Registered);

  // The retired allocator is a registry difference and nothing else: no gate,
  // no cluster, no slot. A program that calls it runs and cannot be deployed.
  EXPECT_EQ(getSyscallGate(Alloc->ID), nullptr);
  EXPECT_EQ(syscallRegistration(Alloc->ID, Mainnet),
            SyscallRegistration::Registered);
  RuntimeProfile Deploying = Mainnet;
  Deploying.Purpose = RuntimePurpose::Deployment;
  EXPECT_EQ(syscallRegistration(Alloc->ID, Deploying),
            SyscallRegistration::EnvironmentExcluded);

  // A syscall governed by nothing resolves in both registries on every chain.
  const SyscallInfo *Log = findSyscallByName("sol_log_");
  ASSERT_NE(Log, nullptr);
  for (const RuntimePurposeInfo &Purpose : runtimePurposeInfos()) {
    RuntimeProfile Profile = Mainnet;
    Profile.Purpose = Purpose.ID;
    EXPECT_EQ(syscallRegistration(Log->ID, Profile),
              SyscallRegistration::Registered)
        << Purpose.Name.str();
  }
}

TEST(SBFRuntimeProfile, TheLoaderDecidesTheSerialization) {
  RuntimeProfile Profile = currentMainnetProfile();
  EXPECT_EQ(Profile.accountABI(), AccountABI::V1);

  // The first loader cannot deploy anything and still runs what it owns, which
  // is exactly why its serialization has to stay readable.
  Profile.OwningLoader = Loader::V1;
  EXPECT_EQ(Profile.accountABI(), AccountABI::V0);
  EXPECT_FALSE(getLoaderInfo(Loader::V1).Deploys);
  EXPECT_TRUE(getLoaderInfo(Loader::V1).Executes);
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
