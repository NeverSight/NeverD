//===- SBFMetadataTests.cpp - Solana SBF definition-table tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/image/SBFVersion.h"
#include "neverd/sbf/runtime/SBFOpcodes.h"
#include "neverd/sbf/runtime/SBFRuntimeEnvironment.h"
#include "neverd/sbf/runtime/SBFRuntimeProfile.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/solana/SBFPubkey.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <set>
#include <type_traits>

namespace neverd::sbf {
namespace {

constexpr RuntimeFeature kUnknownRuntimeFeatureForTest =
    static_cast<RuntimeFeature>(
        RuntimeFeatureMask{1}
        << (std::numeric_limits<RuntimeFeatureMask>::digits - 1));

static_assert(kRuntimeFeatureCount > std::numeric_limits<uint32_t>::digits,
              "the runtime feature table must exercise the wide mask ABI");
static_assert((knownRuntimeFeatureMask() &
               runtimeFeatureMask(kUnknownRuntimeFeatureForTest)) == 0,
              "the fail-closed test bit must remain outside the known mask");

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

  const auto Window =
      [](llvm::StringRef Name, SyscallArgument Argument,
         SyscallMemoryAccess Access) -> const SyscallMemoryInfo * {
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
            "ef210d67f2fabeee1730498188fa78854260c679");
  EXPECT_EQ(syscallSourceRevision(SyscallSource::SBPFMain),
            "2510663bb8d894e8e3094be351e4bb4b604f1f84");
  EXPECT_EQ(syscallSourceRevision(SyscallSource::SolanaSDKMaster),
            "122f32e571ce39face4beffaccea733e37c207fd");
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
  for (const RuntimeFeatureDomainInfo &Info : runtimeFeatureDomainInfos())
    EXPECT_EQ(runtimeFeatureDomainName(Info.ID), Info.Name);
  for (const RuntimeFeatureDispositionInfo &Info :
       runtimeFeatureDispositionInfos())
    EXPECT_EQ(runtimeFeatureDispositionName(Info.ID), Info.Name);

  RuntimeFeatureMask TabulatedMask = 0;
  for (const RuntimeFeatureInfo &Info : runtimeFeatureInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(parseRuntimeFeature(Info.Name), Info.ID);
    EXPECT_EQ(parseRuntimeFeature(Info.Gate), Info.ID);
    EXPECT_FALSE(runtimeFeatureDomainName(Info.Domain).empty());
    EXPECT_FALSE(runtimeFeatureDispositionName(Info.Disposition).empty());
    TabulatedMask |= runtimeFeatureMask(Info.ID);
  }
  EXPECT_EQ(runtimeFeatureInfos().size(), kRuntimeFeatureCount);
  EXPECT_EQ(TabulatedMask, knownRuntimeFeatureMask());
  EXPECT_EQ(std::popcount(knownRuntimeFeatureMask()), kRuntimeFeatureCount);
  EXPECT_GT(runtimeFeatureMask(RuntimeFeature::DisableAllocFreeDeployment),
            std::numeric_limits<uint32_t>::max());
}

TEST(SBFSyscalls, MatchesAgaveFeatureGateTopology) {
  struct FeatureCase {
    RuntimeFeature Feature;
    llvm::StringLiteral Gate;
    llvm::StringLiteral Address;
  };
  constexpr std::array FeatureCases{
      FeatureCase{RuntimeFeature::Blake3Syscall, "blake3_syscall_enabled",
                  "HTW2pSyErTj4BV6KBM9NZ9VBUJVxt7sacNWcf76wtzb3"},
      FeatureCase{RuntimeFeature::Curve25519Syscalls,
                  "curve25519_syscall_enabled",
                  "7rcw5UtqgDTBBv2EcynNfYckgdAaH1MAsCjKgXMkN7Ri"},
      FeatureCase{RuntimeFeature::AltBn128Syscall, "enable_alt_bn128_syscall",
                  "A16q37opZdQMCbe5qJ6xpBB9usykfv8jZaMkxvZQi4GJ"},
      FeatureCase{RuntimeFeature::BigModExpSyscall,
                  "enable_big_mod_exp_syscall",
                  "expH2ppKPW2ANEdEmAjfhSEcnBQJfmoX4FjuNpe9ttg"},
      FeatureCase{RuntimeFeature::LastRestartSlotSysvar,
                  "last_restart_slot_sysvar",
                  "HooKD5NC9QNxk25QuzCssB8ecrEzGt6eXEPBUxWp1LaR"},
      FeatureCase{RuntimeFeature::PoseidonSyscall, "enable_poseidon_syscall",
                  "FL9RsQA6TVUoh5xJQ9d936RHSebA1NLQqe3Zv9sXZRpr"},
      FeatureCase{RuntimeFeature::RemainingComputeUnitsSyscall,
                  "remaining_compute_units_syscall_enabled",
                  "5TuppMutoyzhUSfuYdhgzD47F92GL1g89KpCZQKqedxP"},
      FeatureCase{RuntimeFeature::AltBn128CompressionSyscall,
                  "enable_alt_bn128_compression_syscall",
                  "EJJewYSddEEtSZHiqugnvhQHiWyZKjkFDQASd7oKSagn"},
      FeatureCase{RuntimeFeature::GetSysvarSyscall,
                  "get_sysvar_syscall_enabled",
                  "CLCoTADvV64PSrnR6QXty6Fwrt9Xc6EdxSJE4wLRePjq"},
      FeatureCase{RuntimeFeature::GetEpochStakeSyscall,
                  "enable_get_epoch_stake_syscall",
                  "FKe75t4LXxGaQnVHdUKM6DSFifVVraGZ8LyNo7oPwy1Z"},
      FeatureCase{RuntimeFeature::BLS12381Syscalls, "enable_bls12_381_syscall",
                  "b1sgUiJ3qu7hYm3tNDyyqZNQd6gLGJmJppnLNa93PCQ"},
      FeatureCase{RuntimeFeature::Sha512Syscall, "enable_sha512_syscall",
                  "s512oDwgx8hjMnaQjXfqqrZroVj4HvC6TkN3iSSWXCh"},
      FeatureCase{RuntimeFeature::FeesSysvarDisabled, "disable_fees_sysvar",
                  "JAN1trEUEtZjgXYzNBYHU9DYd7GnThhXfFP7SzPXkPsG"},
      FeatureCase{RuntimeFeature::DisableAllocFreeDeployment,
                  "disable_deploy_of_alloc_free_syscall",
                  "79HWsX9rpnnJBPcdNURVqygpMAfxdrAirzAGAVmf92im"},
  };
  for (const FeatureCase &Case : FeatureCases) {
    SCOPED_TRACE(Case.Gate.str());
    const RuntimeFeatureInfo *Info = getRuntimeFeatureInfo(Case.Feature);
    ASSERT_NE(Info, nullptr);
    EXPECT_EQ(Info->Gate, Case.Gate);
    EXPECT_EQ(Info->Address, Case.Address);
  }

  struct GateCase {
    Syscall ID;
    RuntimeFeature Feature;
    SyscallGatePolarity Polarity;
    RuntimePurposeSet Purposes;
  };
  constexpr std::array GateCases{
      GateCase{Syscall::Blake3, RuntimeFeature::Blake3Syscall,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::Sha512, RuntimeFeature::Sha512Syscall,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::Poseidon, RuntimeFeature::PoseidonSyscall,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::CurveValidatePoint, RuntimeFeature::Curve25519Syscalls,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::CurveGroupOp, RuntimeFeature::Curve25519Syscalls,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::CurveMultiscalarMul, RuntimeFeature::Curve25519Syscalls,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::CurveDecompress, RuntimeFeature::BLS12381Syscalls,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::CurvePairingMap, RuntimeFeature::BLS12381Syscalls,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::AltBn128GroupOp, RuntimeFeature::AltBn128Syscall,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::AltBn128Compression,
               RuntimeFeature::AltBn128CompressionSyscall,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::BigModExp, RuntimeFeature::BigModExpSyscall,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::GetLastRestartSlot,
               RuntimeFeature::LastRestartSlotSysvar,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::GetFeesSysvar, RuntimeFeature::FeesSysvarDisabled,
               SyscallGatePolarity::RequiresInactive, kEveryPurpose},
      GateCase{Syscall::GetSysvar, RuntimeFeature::GetSysvarSyscall,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::GetEpochStake, RuntimeFeature::GetEpochStakeSyscall,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::RemainingComputeUnits,
               RuntimeFeature::RemainingComputeUnitsSyscall,
               SyscallGatePolarity::RequiresActive, kEveryPurpose},
      GateCase{Syscall::AllocFree, RuntimeFeature::DisableAllocFreeDeployment,
               SyscallGatePolarity::RequiresInactive,
               RuntimePurposeSet::Deployment},
  };
  ASSERT_EQ(syscallGateInfos().size(), GateCases.size());
  for (const GateCase &Case : GateCases) {
    const SyscallInfo *SyscallInfo = getSyscallInfo(Case.ID);
    ASSERT_NE(SyscallInfo, nullptr);
    SCOPED_TRACE(SyscallInfo->Name.str());
    const SyscallGateInfo *Gate = getSyscallGate(Case.ID);
    ASSERT_NE(Gate, nullptr);
    EXPECT_EQ(Gate->Feature, Case.Feature);
    EXPECT_EQ(Gate->Polarity, Case.Polarity);
    EXPECT_EQ(Gate->Purposes, Case.Purposes);

    for (const RuntimePurposeInfo &Purpose : runtimePurposeInfos()) {
      RuntimeProfile FeatureOn;
      FeatureOn.Purpose = Purpose.ID;
      FeatureOn.Forced = Case.Feature;
      RuntimeProfile FeatureOff = FeatureOn;
      FeatureOff.Forced = RuntimeFeature::None;
      FeatureOff.Suppressed = Case.Feature;
      if (!contains(Case.Purposes, Purpose.ID)) {
        EXPECT_EQ(syscallRegistration(Case.ID, FeatureOn),
                  SyscallRegistration::Registered);
        EXPECT_EQ(syscallRegistration(Case.ID, FeatureOff),
                  SyscallRegistration::Registered);
        continue;
      }
      const bool RequiresActive =
          Case.Polarity == SyscallGatePolarity::RequiresActive;
      EXPECT_EQ(syscallRegistration(Case.ID, FeatureOn),
                RequiresActive ? SyscallRegistration::Registered
                               : SyscallRegistration::GateUnmet);
      EXPECT_EQ(syscallRegistration(Case.ID, FeatureOff),
                RequiresActive ? SyscallRegistration::GateUnmet
                               : SyscallRegistration::Registered);
    }
  }

  for (const SyscallInfo &Info : syscallInfos()) {
    if (Info.Lifecycle != SyscallLifecycle::FeatureGated)
      continue;
    SCOPED_TRACE(Info.Name.str());
    size_t GateCount = 0;
    for (const SyscallGateInfo &Gate : syscallGateInfos())
      GateCount += Gate.ID == Info.ID;
    EXPECT_EQ(GateCount, 1u);
  }
}

TEST(SBFSyscalls, ResolvesAnExplicitAgaveFeatureSnapshot) {
  const RuntimeFeature Active = RuntimeFeature::Sha512Syscall |
                                RuntimeFeature::FeesSysvarDisabled |
                                RuntimeFeature::DisableAllocFreeDeployment;

  EXPECT_EQ(
      syscallRegistration(Syscall::Sha512, RuntimePurpose::Execution, Active),
      SyscallRegistration::Registered);
  EXPECT_EQ(syscallRegistration(Syscall::GetFeesSysvar,
                                RuntimePurpose::Execution, Active),
            SyscallRegistration::GateUnmet);
  EXPECT_EQ(syscallRegistration(Syscall::AllocFree, RuntimePurpose::Deployment,
                                Active),
            SyscallRegistration::GateUnmet);

  const std::vector<uint32_t> Execution =
      registeredSyscallHashes(RuntimePurpose::Execution, Active);
  const std::vector<uint32_t> Deployment =
      registeredSyscallHashes(RuntimePurpose::Deployment, Active);
  EXPECT_TRUE(std::binary_search(Execution.begin(), Execution.end(),
                                 getSyscallInfo(Syscall::Sha512)->Hash));
  EXPECT_TRUE(std::binary_search(Execution.begin(), Execution.end(),
                                 getSyscallInfo(Syscall::AllocFree)->Hash));
  EXPECT_FALSE(std::binary_search(Deployment.begin(), Deployment.end(),
                                  getSyscallInfo(Syscall::AllocFree)->Hash));
  EXPECT_TRUE(std::is_sorted(Execution.begin(), Execution.end()));
  EXPECT_EQ(std::adjacent_find(Execution.begin(), Execution.end()),
            Execution.end());
}

constexpr std::array kObservedClusters{Cluster::MainnetBeta, Cluster::Testnet,
                                       Cluster::Devnet};

TEST(SBFRuntimeProfile, MatchesAgaveEnvironmentPolicyFeatures) {
  struct PolicyFeatureCase {
    RuntimeFeature Feature;
    llvm::StringLiteral Gate;
    llvm::StringLiteral Address;
    std::array<std::optional<uint64_t>, kObservedClusters.size()> Activations;
  };
  constexpr std::array Cases{
      PolicyFeatureCase{RuntimeFeature::VirtualAddressSpaceAdjustments,
                        "virtual_address_space_adjustments",
                        "7VgiehxNxu53KdxgLspGQY8myE6f7UokaWa4jsGcaSz",
                        {std::nullopt, 407900256, 463536000}},
      PolicyFeatureCase{RuntimeFeature::SyscallParameterAddressRestrictions,
                        "syscall_parameter_address_restrictions",
                        "EDGMC5kxFxGk4ixsNkGt8bW7QL5hDMXnbwaZvYMwNfzF",
                        {429840000, 407468256, 462240000}},
      PolicyFeatureCase{RuntimeFeature::AccountDataDirectMapping,
                        "account_data_direct_mapping",
                        "CR3dVN2Yoo95Y96kLSTaziWDAQT2MNEpiWh5cqVq2pNE",
                        {std::nullopt, 408332256, 463968000}},
      PolicyFeatureCase{RuntimeFeature::DisableSBPFV0Execution,
                        "disable_sbpf_v0_execution",
                        "TestFeature11111111111111111111111111111111",
                        {std::nullopt, std::nullopt, std::nullopt}},
      PolicyFeatureCase{RuntimeFeature::ReenableSBPFV0Execution,
                        "reenable_sbpf_v0_execution",
                        "TestFeature21111111111111111111111111111111",
                        {std::nullopt, std::nullopt, std::nullopt}},
      PolicyFeatureCase{RuntimeFeature::EnableSBPFV1,
                        "enable_sbpf_v1_deployment_and_execution",
                        "JE86WkYvTrzW8HgNmrHY7dFYpCmSptUpKupbo2AdQ9cG",
                        {349488000, 338780256, 389664000}},
      PolicyFeatureCase{RuntimeFeature::EnableSBPFV2,
                        "enable_sbpf_v2_deployment_and_execution",
                        "F6UVKh1ujTEFK3en2SyAL3cdVnqko1FVEXWhmdLRu6WP",
                        {356400000, 346124256, 396576000}},
      PolicyFeatureCase{RuntimeFeature::EnableSBPFV3,
                        "enable_sbpf_v3_deployment_and_execution",
                        "5cC3foj77CWun58pC51ebHFUWavHWKarWyR5UUik7dnC",
                        {428976000, 406604256, 461808000}},
      PolicyFeatureCase{RuntimeFeature::DisableLegacyDeployment,
                        "disable_sbpf_v0_v1_v2_deployment",
                        "B8JJXCy5amZyWG9r7EnUYLwzXSXTxG7GZ1qZ1qggo83g",
                        {std::nullopt, std::nullopt, std::nullopt}},
  };

  for (const PolicyFeatureCase &Case : Cases) {
    SCOPED_TRACE(Case.Gate.str());
    const RuntimeFeatureInfo *Info = getRuntimeFeatureInfo(Case.Feature);
    ASSERT_NE(Info, nullptr);
    EXPECT_EQ(Info->Gate, Case.Gate);
    EXPECT_EQ(Info->Address, Case.Address);
    for (size_t Index = 0; Index < kObservedClusters.size(); ++Index)
      EXPECT_EQ(
          runtimeFeatureActivation(Case.Feature, kObservedClusters[Index]),
          Case.Activations[Index]);
  }
}

TEST(SBFRuntimeProfile, MatchesDirectlyObservableAgaveSnapshotGates) {
  struct ObservableFeatureCase {
    RuntimeFeature Feature;
    llvm::StringLiteral Gate;
    llvm::StringLiteral Address;
    llvm::StringLiteral SIMD;
    RuntimeFeatureDomain Domain;
    RuntimeFeatureDisposition Disposition;
    std::array<std::optional<uint64_t>, kObservedClusters.size()> Activations;
  };
  constexpr std::array Cases{
      ObservableFeatureCase{RuntimeFeature::DisableAllocFreeDeployment,
                            "disable_deploy_of_alloc_free_syscall",
                            "79HWsX9rpnnJBPcdNURVqygpMAfxdrAirzAGAVmf92im",
                            "",
                            RuntimeFeatureDomain::ProgramAdmission,
                            RuntimeFeatureDisposition::FoldedBranch,
                            {209088008, 195356264, 224208000}},
      ObservableFeatureCase{RuntimeFeature::EnableLoaderSetAuthorityChecked,
                            "enable_bpf_loader_set_authority_checked_ix",
                            "5x3825XS7M2A3Ekbn5VGGkvFoAg5qrRWkTrY4bARP1GL",
                            "",
                            RuntimeFeatureDomain::LoaderManagement,
                            RuntimeFeatureDisposition::RuntimeBranch,
                            {251424000, 247628260, 255744000}},
      ObservableFeatureCase{RuntimeFeature::RemoveLoaderIncorrectProgramID,
                            "remove_bpf_loader_incorrect_program_id",
                            "2HmTkCj9tXuPE4ueHzdD7jPeMf9JGCoZh5AsyoATiWEe",
                            "",
                            RuntimeFeatureDomain::LoaderManagement,
                            RuntimeFeatureDisposition::FoldedBranch,
                            {237168000, 224300256, 247104000}},
      ObservableFeatureCase{RuntimeFeature::SimplifyAltBn128ErrorCodes,
                            "simplify_alt_bn128_syscall_error_codes",
                            "JDn5q3GBeqzvUa7z67BbmVHVdE3EbUAjvFep3weR3jxX",
                            "simd-0129",
                            RuntimeFeatureDomain::SyscallSemantics,
                            RuntimeFeatureDisposition::FoldedBranch,
                            {274320000, 278300256, 308448000}},
      ObservableFeatureCase{RuntimeFeature::AbortOnInvalidCurve,
                            "abort_on_invalid_curve",
                            "FuS3FPfJDKSNot99ECLXtp3rueq36hMNStJkPJwWodLh",
                            "simd-0137",
                            RuntimeFeatureDomain::SyscallSemantics,
                            RuntimeFeatureDisposition::RuntimeBranch,
                            {311904000, 300764256, 342576000}},
      ObservableFeatureCase{RuntimeFeature::DepleteCUMeterOnVMFailure,
                            "deplete_cu_meter_on_vm_failure",
                            "B7H2caeia4ZFcpE3QcgMqbiWiBtWrdBRBSJ1DY6Ktxbq",
                            "simd-0182",
                            RuntimeFeatureDomain::VMFaultPolicy,
                            RuntimeFeatureDisposition::RuntimeBranch,
                            {327888000, 319340257, 364176000}},
      ObservableFeatureCase{
          RuntimeFeature::FixAltBn128MultiplicationInputLength,
          "fix_alt_bn128_multiplication_input_length",
          "bn2puAyxUx6JUabAxYdKdJ5QHbNNmKw8dCGuGCyRrFN",
          "simd-0222",
          RuntimeFeatureDomain::SyscallSemantics,
          RuntimeFeatureDisposition::FoldedBranch,
          {361152000, 346988256, 397440000}},
      ObservableFeatureCase{RuntimeFeature::RaiseCPINestingLimit,
                            "raise_cpi_nesting_limit_to_8",
                            "6TkHkRmP7JZy1fdM6fg5uXn76wChQBWGokHBJzrLB3mj",
                            "simd-0268",
                            RuntimeFeatureDomain::CPIExecution,
                            RuntimeFeatureDisposition::RuntimeBranch,
                            {std::nullopt, std::nullopt, std::nullopt}},
      ObservableFeatureCase{RuntimeFeature::IncreaseCPIAccountInfoLimit,
                            "increase_cpi_account_info_limit",
                            "H6iVbVaDZgDphcPbcZwc5LoznMPWQfnJ1AM7L1xzqvt5",
                            "simd-0339",
                            RuntimeFeatureDomain::CPIExecution,
                            RuntimeFeatureDisposition::FoldedBranch,
                            {403056000, 385868256, 435456000}},
      ObservableFeatureCase{RuntimeFeature::PoseidonEnforcePadding,
                            "poseidon_enforce_padding",
                            "poUdAqRXXsNmfqAZ6UqpjbeYgwBygbfQLEvWSqVhSnb",
                            "simd-0359",
                            RuntimeFeatureDomain::SyscallSemantics,
                            RuntimeFeatureDisposition::FoldedBranch,
                            {406080000, 385868256, 438048000}},
      ObservableFeatureCase{RuntimeFeature::FixAltBn128PairingLength,
                            "fix_alt_bn128_pairing_length_check",
                            "bnYzodLwmybj7e1HAe98yZrdJTd7we69eMMLgCXqKZm",
                            "simd-0334",
                            RuntimeFeatureDomain::SyscallSemantics,
                            RuntimeFeatureDisposition::FoldedBranch,
                            {406944000, 385868256, 438480000}},
      ObservableFeatureCase{RuntimeFeature::AltBn128LittleEndian,
                            "alt_bn128_little_endian",
                            "bn2oPgpkzQPT3tohMaAsMVGjhDmmDa4jCaVPqCFmtxM",
                            "simd-0284",
                            RuntimeFeatureDomain::SyscallSemantics,
                            RuntimeFeatureDisposition::RuntimeBranch,
                            {425088000, 406604256, 456192000}},
      ObservableFeatureCase{RuntimeFeature::AltBn128G2Syscalls,
                            "enable_alt_bn128_g2_syscalls",
                            "bn1hKNURMGQaQoEVxahcEAcqiX3NwRs6hgKKNSLeKxH",
                            "simd-0302",
                            RuntimeFeatureDomain::SyscallSemantics,
                            RuntimeFeatureDisposition::RuntimeBranch,
                            {425520000, 406604256, 457056000}},
      ObservableFeatureCase{RuntimeFeature::LoaderV3MinimumExtendProgramSize,
                            "loader_v3_minimum_extend_program_size",
                            "YbbRLkvenrocjGPGyoQE4wjnvYzTgfsk38NFmcYK7a5",
                            "simd-0431",
                            RuntimeFeatureDomain::LoaderManagement,
                            RuntimeFeatureDisposition::RuntimeBranch,
                            {432864000, 416540256, 470880000}},
  };

  for (const ObservableFeatureCase &Case : Cases) {
    SCOPED_TRACE(Case.Gate.str());
    const RuntimeFeatureInfo *Info = getRuntimeFeatureInfo(Case.Feature);
    ASSERT_NE(Info, nullptr);
    EXPECT_EQ(Info->Gate, Case.Gate);
    EXPECT_EQ(Info->Address, Case.Address);
    EXPECT_EQ(Info->SIMD, Case.SIMD);
    EXPECT_EQ(Info->Domain, Case.Domain);
    EXPECT_EQ(Info->Disposition, Case.Disposition);
    for (size_t Index = 0; Index < kObservedClusters.size(); ++Index)
      EXPECT_EQ(
          runtimeFeatureActivation(Case.Feature, kObservedClusters[Index]),
          Case.Activations[Index]);
  }
}

TEST(SBFRuntimeEnvironment, ResolvesAgaveExecutionVersionTruthTable) {
  struct VersionCase {
    bool DisableV0;
    bool ReenableV0;
    Version MinimumVersion;
  };
  constexpr std::array Cases{
      VersionCase{false, false, Version::V0},
      VersionCase{false, true, Version::V0},
      VersionCase{true, false, Version::V3},
      VersionCase{true, true, Version::V0},
  };

  for (const VersionCase &Case : Cases) {
    const RuntimeFeature Snapshot =
        (Case.DisableV0 ? RuntimeFeature::DisableSBPFV0Execution
                        : RuntimeFeature::None) |
        (Case.ReenableV0 ? RuntimeFeature::ReenableSBPFV0Execution
                         : RuntimeFeature::None);
    auto SnapshotMinimum =
        minimumAgaveVersion(RuntimePurpose::Execution, Snapshot);
    ASSERT_TRUE(static_cast<bool>(SnapshotMinimum))
        << llvm::toString(SnapshotMinimum.takeError());
    EXPECT_EQ(*SnapshotMinimum, Case.MinimumVersion);

    RuntimeProfile Profile;
    Profile.Purpose = RuntimePurpose::Execution;
    Profile.Forced = Snapshot;
    Profile.Suppressed =
        (!Case.DisableV0 ? RuntimeFeature::DisableSBPFV0Execution
                         : RuntimeFeature::None) |
        (!Case.ReenableV0 ? RuntimeFeature::ReenableSBPFV0Execution
                          : RuntimeFeature::None);

    auto Environment = resolveRuntimeEnvironment(Profile);
    ASSERT_TRUE(static_cast<bool>(Environment))
        << llvm::toString(Environment.takeError());
    EXPECT_FALSE(Environment->isCustom());
    EXPECT_EQ(Environment->minimumVersion(), Case.MinimumVersion);
    EXPECT_EQ(Environment->maximumVersion(), Version::V3);
    EXPECT_EQ(Environment->supportsVersion(Version::V0),
              Case.MinimumVersion == Version::V0);
    EXPECT_TRUE(Environment->supportsVersion(Version::V3));
    EXPECT_FALSE(Environment->supportsVersion(Version::V4));
  }

  const RuntimeFeature DeploymentGate = RuntimeFeature::DisableLegacyDeployment;
  auto ExecutionMinimum =
      minimumAgaveVersion(RuntimePurpose::Execution, DeploymentGate);
  ASSERT_TRUE(static_cast<bool>(ExecutionMinimum))
      << llvm::toString(ExecutionMinimum.takeError());
  EXPECT_EQ(*ExecutionMinimum, Version::V0);
  auto DeploymentMinimum =
      minimumAgaveVersion(RuntimePurpose::Deployment, DeploymentGate);
  ASSERT_TRUE(static_cast<bool>(DeploymentMinimum))
      << llvm::toString(DeploymentMinimum.takeError());
  EXPECT_EQ(*DeploymentMinimum, Version::V3);
}

TEST(SBFRuntimeEnvironment, ResolvesHistoricalChainVersionActivations) {
  struct ActivationBoundary {
    Cluster OnCluster;
    uint64_t Slot;
    Version MaximumVersion;
  };
  constexpr std::array Cases{
      ActivationBoundary{Cluster::MainnetBeta, 349487999, Version::V0},
      ActivationBoundary{Cluster::MainnetBeta, 349488000, Version::V1},
      ActivationBoundary{Cluster::MainnetBeta, 356399999, Version::V1},
      ActivationBoundary{Cluster::MainnetBeta, 356400000, Version::V2},
      ActivationBoundary{Cluster::MainnetBeta, 428975999, Version::V2},
      ActivationBoundary{Cluster::MainnetBeta, 428976000, Version::V3},
      ActivationBoundary{Cluster::Testnet, 338780255, Version::V0},
      ActivationBoundary{Cluster::Testnet, 338780256, Version::V1},
      ActivationBoundary{Cluster::Testnet, 346124256, Version::V2},
      ActivationBoundary{Cluster::Testnet, 406604256, Version::V3},
      ActivationBoundary{Cluster::Devnet, 389663999, Version::V0},
      ActivationBoundary{Cluster::Devnet, 389664000, Version::V1},
      ActivationBoundary{Cluster::Devnet, 396576000, Version::V2},
      ActivationBoundary{Cluster::Devnet, 461808000, Version::V3},
  };

  for (const ActivationBoundary &Case : Cases) {
    RuntimeProfile Profile;
    Profile.OnCluster = Case.OnCluster;
    Profile.Slot = Case.Slot;
    auto Environment = resolveRuntimeEnvironment(Profile);
    ASSERT_TRUE(static_cast<bool>(Environment))
        << llvm::toString(Environment.takeError());
    SCOPED_TRACE(
        (clusterName(Case.OnCluster) + ":" + llvm::Twine(Case.Slot)).str());
    EXPECT_EQ(Environment->maximumVersion(), Case.MaximumVersion);
    EXPECT_TRUE(Environment->supportsVersion(Case.MaximumVersion));
    if (Case.MaximumVersion != Version::V3)
      EXPECT_FALSE(Environment->supportsVersion(
          static_cast<Version>(static_cast<uint8_t>(Case.MaximumVersion) + 1)));
  }

  RuntimeProfile NonContiguous = currentMainnetProfile();
  NonContiguous.Suppressed = RuntimeFeature::EnableSBPFV2;
  auto InvalidGates = resolveRuntimeEnvironment(NonContiguous);
  ASSERT_FALSE(static_cast<bool>(InvalidGates));
  EXPECT_NE(llvm::toString(InvalidGates.takeError()).find("not contiguous"),
            std::string::npos);

  RuntimeProfile EmptyRange;
  EmptyRange.Slot = 0;
  EmptyRange.Forced = RuntimeFeature::DisableSBPFV0Execution;
  EmptyRange.Suppressed = RuntimeFeature::ReenableSBPFV0Execution;
  auto NoEnabledVersion = resolveRuntimeEnvironment(EmptyRange);
  ASSERT_FALSE(static_cast<bool>(NoEnabledVersion));
  EXPECT_NE(llvm::toString(NoEnabledVersion.takeError()).find("disables every"),
            std::string::npos);
}

TEST(SBFRuntimeEnvironment, ResolvesAgaveAddressSpaceAndPurposeProfiles) {
  struct ProfileCase {
    Cluster OnCluster;
    uint64_t Slot;
    RuntimePurpose Purpose;
    Version MinimumVersion;
    bool HasAdjustedAddressSpace;
  };
  constexpr std::array Cases{
      ProfileCase{Cluster::MainnetBeta, kCurrentSlot, RuntimePurpose::Execution,
                  Version::V0, false},
      ProfileCase{Cluster::Testnet, 407900255, RuntimePurpose::Execution,
                  Version::V0, false},
      ProfileCase{Cluster::Testnet, 407900256, RuntimePurpose::Execution,
                  Version::V0, true},
      ProfileCase{Cluster::Testnet, kCurrentSlot, RuntimePurpose::Execution,
                  Version::V0, true},
      ProfileCase{Cluster::Devnet, 463535999, RuntimePurpose::Execution,
                  Version::V0, false},
      ProfileCase{Cluster::Devnet, kCurrentSlot, RuntimePurpose::Execution,
                  Version::V0, true},
      ProfileCase{Cluster::Localnet, 0, RuntimePurpose::Execution, Version::V0,
                  true},
      ProfileCase{Cluster::MainnetBeta, kCurrentSlot,
                  RuntimePurpose::Deployment, Version::V0, false},
      ProfileCase{Cluster::Testnet, kCurrentSlot, RuntimePurpose::Deployment,
                  Version::V0, true},
      ProfileCase{Cluster::Devnet, kCurrentSlot, RuntimePurpose::Deployment,
                  Version::V0, true},
      ProfileCase{Cluster::Localnet, 0, RuntimePurpose::Deployment, Version::V3,
                  true},
  };

  for (const ProfileCase &Case : Cases) {
    RuntimeProfile Profile;
    Profile.OnCluster = Case.OnCluster;
    Profile.Slot = Case.Slot;
    Profile.Purpose = Case.Purpose;
    auto Environment = resolveRuntimeEnvironment(Profile);
    ASSERT_TRUE(static_cast<bool>(Environment))
        << llvm::toString(Environment.takeError());
    SCOPED_TRACE(
        (clusterName(Case.OnCluster) + ":" + runtimePurposeName(Case.Purpose))
            .str());
    EXPECT_EQ(Environment->origin(), RuntimeEnvironmentOrigin::Agave);
    EXPECT_EQ(Environment->minimumVersion(), Case.MinimumVersion);
    EXPECT_EQ(Environment->maximumVersion(), Version::V3);
    EXPECT_EQ(Environment->vmConfig().EnableStackFrameGaps,
              !Case.HasAdjustedAddressSpace);
    EXPECT_EQ(Environment->vmConfig().AlignedMemoryMapping,
              !Case.HasAdjustedAddressSpace);
    EXPECT_FALSE(Environment->vmConfig().OptimizeRodata);
    EXPECT_EQ(Environment->vmConfig().RejectBrokenELFs,
              Case.Purpose == RuntimePurpose::Deployment);
  }

  auto Current = resolveRuntimeEnvironment(currentMainnetProfile());
  ASSERT_TRUE(static_cast<bool>(Current))
      << llvm::toString(Current.takeError());
  EXPECT_EQ(Current->origin(), RuntimeEnvironmentOrigin::Agave);
  EXPECT_EQ(Current->versionPolicy(), RuntimeVersionPolicy::ChainProfile);
  EXPECT_EQ(Current->minimumVersion(), Version::V0);
  EXPECT_EQ(Current->maximumVersion(), Version::V3);

  auto Upstream = resolveRuntimeEnvironment(
      currentMainnetProfile(), RuntimeVersionPolicy::UpstreamToolchain);
  ASSERT_TRUE(static_cast<bool>(Upstream))
      << llvm::toString(Upstream.takeError());
  EXPECT_EQ(Upstream->origin(), RuntimeEnvironmentOrigin::Agave);
  EXPECT_EQ(Upstream->versionPolicy(), RuntimeVersionPolicy::UpstreamToolchain);
  EXPECT_EQ(Upstream->minimumVersion(), Version::V0);
  EXPECT_EQ(Upstream->maximumVersion(), Version::V4);
  EXPECT_TRUE(Upstream->supportsVersion(Version::V4));
}

TEST(SBFRuntimeEnvironment, MaterializesTheExactLoaderFunctionRegistry) {
  const SyscallInfo *Log = getSyscallInfo(Syscall::Log);
  const SyscallInfo *AllocFree = getSyscallInfo(Syscall::AllocFree);
  const SyscallInfo *Poseidon = getSyscallInfo(Syscall::Poseidon);
  ASSERT_NE(Log, nullptr);
  ASSERT_NE(AllocFree, nullptr);
  ASSERT_NE(Poseidon, nullptr);

  RuntimeProfile Execution;
  Execution.Purpose = RuntimePurpose::Execution;
  auto ExecutionEnvironment = resolveRuntimeEnvironment(Execution);
  ASSERT_TRUE(static_cast<bool>(ExecutionEnvironment))
      << llvm::toString(ExecutionEnvironment.takeError());
  EXPECT_TRUE(ExecutionEnvironment->isSyscallRegistered(Log->Hash));
  EXPECT_TRUE(ExecutionEnvironment->isSyscallRegistered(AllocFree->Hash));

  RuntimeProfile Deployment = Execution;
  Deployment.Purpose = RuntimePurpose::Deployment;
  auto DeploymentEnvironment = resolveRuntimeEnvironment(Deployment);
  ASSERT_TRUE(static_cast<bool>(DeploymentEnvironment))
      << llvm::toString(DeploymentEnvironment.takeError());
  EXPECT_TRUE(DeploymentEnvironment->isSyscallRegistered(Log->Hash));
  EXPECT_FALSE(DeploymentEnvironment->isSyscallRegistered(AllocFree->Hash));

  RuntimeProfile BeforePoseidon = Deployment;
  BeforePoseidon.Suppressed = RuntimeFeature::PoseidonSyscall;
  auto BeforeEnvironment = resolveRuntimeEnvironment(BeforePoseidon);
  ASSERT_TRUE(static_cast<bool>(BeforeEnvironment))
      << llvm::toString(BeforeEnvironment.takeError());
  EXPECT_FALSE(BeforeEnvironment->isSyscallRegistered(Poseidon->Hash));

  RuntimeProfile AfterPoseidon = Deployment;
  AfterPoseidon.Forced = RuntimeFeature::PoseidonSyscall;
  auto AfterEnvironment = resolveRuntimeEnvironment(AfterPoseidon);
  ASSERT_TRUE(static_cast<bool>(AfterEnvironment))
      << llvm::toString(AfterEnvironment.takeError());
  EXPECT_TRUE(AfterEnvironment->isSyscallRegistered(Poseidon->Hash));

  const llvm::ArrayRef<uint32_t> Hashes =
      AfterEnvironment->registeredSyscallHashes();
  EXPECT_TRUE(std::is_sorted(Hashes.begin(), Hashes.end()));
  EXPECT_EQ(std::adjacent_find(Hashes.begin(), Hashes.end()), Hashes.end());

  RuntimeProfile UnknownOverride = Execution;
  UnknownOverride.Forced = kUnknownRuntimeFeatureForTest;
  auto InvalidOverride = resolveRuntimeEnvironment(UnknownOverride);
  ASSERT_FALSE(static_cast<bool>(InvalidOverride));
  EXPECT_NE(llvm::toString(InvalidOverride.takeError()).find("unknown bit"),
            std::string::npos);
}

TEST(SBFRuntimeEnvironment, KeepsExpertOverridesExplicitAndCustom) {
  static_assert(!std::is_default_constructible_v<ResolvedRuntimeEnvironment>);
  static_assert(!std::is_copy_assignable_v<ResolvedRuntimeEnvironment>);
  static_assert(!std::is_move_assignable_v<ResolvedRuntimeEnvironment>);

  SBFVMConfig Config;
  Config.StackFrameSize *= 2;
  Config.MaxCallDepth /= 2;
  Config.EnableStackFrameGaps = false;
  Config.OptimizeRodata = true;
  Config.AlignedMemoryMapping = true;
  Config.RejectBrokenELFs = true;
  const ExpertRuntimeEnvironmentOverride Override{
      Version::V1,
      Version::V4,
      Config,
      {UINT32_C(9), UINT32_C(3), UINT32_C(9)}};
  auto Environment = resolveExpertRuntimeEnvironment(Override);
  ASSERT_TRUE(static_cast<bool>(Environment))
      << llvm::toString(Environment.takeError());
  EXPECT_TRUE(Environment->isCustom());
  EXPECT_EQ(Environment->origin(), RuntimeEnvironmentOrigin::Custom);
  EXPECT_EQ(Environment->versionPolicy(), RuntimeVersionPolicy::ExpertOverride);
  EXPECT_FALSE(Environment->profile().has_value());
  EXPECT_EQ(Environment->activeRuntimeFeatures(), RuntimeFeature::None);
  EXPECT_EQ(Environment->accountABI(), AccountABI::V1);
  EXPECT_FALSE(Environment->supportsVersion(Version::V0));
  EXPECT_TRUE(Environment->supportsVersion(Version::V1));
  EXPECT_TRUE(Environment->supportsVersion(Version::V4));
  EXPECT_EQ(Environment->vmConfig().StackFrameSize, Config.StackFrameSize);
  EXPECT_EQ(Environment->vmConfig().MaxCallDepth, Config.MaxCallDepth);
  EXPECT_EQ(Environment->vmConfig().EnableStackFrameGaps,
            Config.EnableStackFrameGaps);
  EXPECT_EQ(Environment->vmConfig().OptimizeRodata, Config.OptimizeRodata);
  EXPECT_EQ(Environment->vmConfig().AlignedMemoryMapping,
            Config.AlignedMemoryMapping);
  EXPECT_EQ(Environment->vmConfig().RejectBrokenELFs, Config.RejectBrokenELFs);
  EXPECT_TRUE(Environment->isSyscallRegistered(UINT32_C(3)));
  EXPECT_TRUE(Environment->isSyscallRegistered(UINT32_C(9)));
  EXPECT_FALSE(Environment->isSyscallRegistered(UINT32_C(8)));
  ASSERT_EQ(Environment->registeredSyscallHashes().size(), 2u);

  ExpertRuntimeEnvironmentOverride ExplicitFacts = Override;
  ExplicitFacts.ActiveRuntimeFeatures =
      RuntimeFeature::InstructionDataPointer | RuntimeFeature::PoseidonSyscall;
  ExplicitFacts.InputABI = AccountABI::V0;
  auto Facts = resolveExpertRuntimeEnvironment(ExplicitFacts);
  ASSERT_TRUE(static_cast<bool>(Facts)) << llvm::toString(Facts.takeError());
  EXPECT_EQ(Facts->activeRuntimeFeatures(),
            ExplicitFacts.ActiveRuntimeFeatures);
  EXPECT_EQ(Facts->accountABI(), AccountABI::V0);

  ExpertRuntimeEnvironmentOverride UnknownFeature = Override;
  UnknownFeature.ActiveRuntimeFeatures = kUnknownRuntimeFeatureForTest;
  auto InvalidFeatures = resolveExpertRuntimeEnvironment(UnknownFeature);
  ASSERT_FALSE(static_cast<bool>(InvalidFeatures));
  EXPECT_NE(llvm::toString(InvalidFeatures.takeError()).find("unknown bit"),
            std::string::npos);

  ExpertRuntimeEnvironmentOverride UnknownABI = Override;
  UnknownABI.InputABI = static_cast<AccountABI>(UINT8_C(0xff));
  auto InvalidABI = resolveExpertRuntimeEnvironment(UnknownABI);
  ASSERT_FALSE(static_cast<bool>(InvalidABI));
  EXPECT_NE(llvm::toString(InvalidABI.takeError()).find("not tabulated"),
            std::string::npos);

  const ExpertRuntimeEnvironmentOverride Reversed{
      Version::V3, Version::V2, Config, {}};
  auto InvalidRange = resolveExpertRuntimeEnvironment(Reversed);
  ASSERT_FALSE(static_cast<bool>(InvalidRange));
  EXPECT_NE(llvm::toString(InvalidRange.takeError()).find("exceeds"),
            std::string::npos);

  RuntimeProfile Conflicting;
  Conflicting.Forced = RuntimeFeature::DisableSBPFV0Execution;
  Conflicting.Suppressed = RuntimeFeature::DisableSBPFV0Execution;
  auto InvalidProfile = resolveRuntimeEnvironment(Conflicting);
  ASSERT_FALSE(static_cast<bool>(InvalidProfile));
  EXPECT_NE(llvm::toString(InvalidProfile.takeError()).find("both forced"),
            std::string::npos);

  auto IncompleteExpert = resolveRuntimeEnvironment(
      currentMainnetProfile(), RuntimeVersionPolicy::ExpertOverride);
  ASSERT_FALSE(static_cast<bool>(IncompleteExpert));
  EXPECT_NE(llvm::toString(IncompleteExpert.takeError())
                .find("complete expert environment"),
            std::string::npos);
}

TEST(SBFRuntimeProfile, MatchesObservedSyscallGateActivations) {
  struct ActivationCase {
    RuntimeFeature Feature;
    Syscall Representative;
    std::array<uint64_t, kObservedClusters.size()> Slots;
  };
  constexpr std::array ActivationCases{
      ActivationCase{RuntimeFeature::Curve25519Syscalls,
                     Syscall::CurveValidatePoint,
                     {275184000, 268364256, 240192004}},
      ActivationCase{RuntimeFeature::AltBn128Syscall,
                     Syscall::AltBn128GroupOp,
                     {275616000, 247628260, 235872000}},
      ActivationCase{RuntimeFeature::AltBn128CompressionSyscall,
                     Syscall::AltBn128Compression,
                     {276912000, 279164260, 309312000}},
      ActivationCase{RuntimeFeature::LastRestartSlotSysvar,
                     Syscall::GetLastRestartSlot,
                     {282096004, 283916256, 313632000}},
      ActivationCase{RuntimeFeature::PoseidonSyscall,
                     Syscall::Poseidon,
                     {278208000, 280892257, 310608000}},
      ActivationCase{RuntimeFeature::GetSysvarSyscall,
                     Syscall::GetSysvar,
                     {321840000, 316748256, 348192000}},
      ActivationCase{RuntimeFeature::GetEpochStakeSyscall,
                     Syscall::GetEpochStake,
                     {330912000, 322796256, 368496000}},
      ActivationCase{RuntimeFeature::BLS12381Syscalls,
                     Syscall::CurveDecompress,
                     {425952004, 406604256, 457488000}},
  };
  for (const ActivationCase &Case : ActivationCases) {
    for (size_t Index = 0; Index < kObservedClusters.size(); ++Index) {
      const Cluster OnCluster = kObservedClusters[Index];
      const uint64_t ActivationSlot = Case.Slots[Index];
      SCOPED_TRACE(
          (runtimeFeatureName(Case.Feature) + " on " + clusterName(OnCluster))
              .str());
      EXPECT_EQ(runtimeFeatureActivation(Case.Feature, OnCluster),
                ActivationSlot);

      RuntimeProfile Profile;
      Profile.OnCluster = OnCluster;
      Profile.Slot = ActivationSlot - 1;
      EXPECT_FALSE(isFeatureActive(Profile, Case.Feature));
      EXPECT_EQ(syscallRegistration(Case.Representative, Profile),
                SyscallRegistration::GateUnmet);
      Profile.Slot = ActivationSlot;
      EXPECT_TRUE(isFeatureActive(Profile, Case.Feature));
      EXPECT_EQ(syscallRegistration(Case.Representative, Profile),
                SyscallRegistration::Registered);
    }
  }

  struct InactiveCase {
    RuntimeFeature Feature;
    Syscall Representative;
  };
  constexpr std::array InactiveCases{
      InactiveCase{RuntimeFeature::Blake3Syscall, Syscall::Blake3},
      InactiveCase{RuntimeFeature::BigModExpSyscall, Syscall::BigModExp},
      InactiveCase{RuntimeFeature::RemainingComputeUnitsSyscall,
                   Syscall::RemainingComputeUnits},
  };
  for (const InactiveCase &Case : InactiveCases) {
    for (Cluster OnCluster : kObservedClusters) {
      SCOPED_TRACE(
          (runtimeFeatureName(Case.Feature) + " on " + clusterName(OnCluster))
              .str());
      EXPECT_EQ(runtimeFeatureActivation(Case.Feature, OnCluster),
                std::nullopt);
      RuntimeProfile Profile;
      Profile.OnCluster = OnCluster;
      EXPECT_FALSE(isFeatureActive(Profile, Case.Feature));
      EXPECT_EQ(syscallRegistration(Case.Representative, Profile),
                SyscallRegistration::GateUnmet);
    }
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
  EXPECT_FALSE(
      isFeatureActive(Earlier, RuntimeFeature::InstructionDataPointer));
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

  // The retired allocator remains in the execution registry forever. The
  // deployment registry stopped admitting it at a recorded feature boundary.
  const SyscallGateInfo *AllocGate = getSyscallGate(Alloc->ID);
  ASSERT_NE(AllocGate, nullptr);
  EXPECT_EQ(AllocGate->Feature, RuntimeFeature::DisableAllocFreeDeployment);
  EXPECT_EQ(AllocGate->Polarity, SyscallGatePolarity::RequiresInactive);
  EXPECT_EQ(AllocGate->Purposes, RuntimePurposeSet::Deployment);
  EXPECT_EQ(syscallRegistration(Alloc->ID, Mainnet),
            SyscallRegistration::Registered);
  RuntimeProfile Deploying = Mainnet;
  Deploying.Purpose = RuntimePurpose::Deployment;
  EXPECT_EQ(syscallRegistration(Alloc->ID, Deploying),
            SyscallRegistration::GateUnmet);

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

TEST(SBFSyscalls, AllocFreeDeploymentChangesAtEachClusterActivationSlot) {
  for (Cluster OnCluster : kObservedClusters) {
    SCOPED_TRACE(clusterName(OnCluster).str());
    const std::optional<uint64_t> Activation = runtimeFeatureActivation(
        RuntimeFeature::DisableAllocFreeDeployment, OnCluster);
    ASSERT_TRUE(Activation.has_value());

    RuntimeProfile Profile;
    Profile.OnCluster = OnCluster;
    Profile.Purpose = RuntimePurpose::Deployment;
    Profile.Slot = *Activation - 1;
    EXPECT_EQ(syscallRegistration(Syscall::AllocFree, Profile),
              SyscallRegistration::Registered);
    Profile.Slot = *Activation;
    EXPECT_EQ(syscallRegistration(Syscall::AllocFree, Profile),
              SyscallRegistration::GateUnmet);

    Profile.Purpose = RuntimePurpose::Execution;
    Profile.Slot = *Activation - 1;
    EXPECT_EQ(syscallRegistration(Syscall::AllocFree, Profile),
              SyscallRegistration::Registered);
    Profile.Slot = *Activation;
    EXPECT_EQ(syscallRegistration(Syscall::AllocFree, Profile),
              SyscallRegistration::Registered);
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

TEST(SBFRuntimeProfile, RejectsForgedProfileAxesBeforeTableAccess) {
  constexpr auto InvalidCluster = static_cast<Cluster>(255);
  constexpr auto InvalidPurpose = static_cast<RuntimePurpose>(255);
  constexpr auto InvalidLoader = static_cast<Loader>(255);
  constexpr auto InvalidPolicy = static_cast<RuntimeVersionPolicy>(255);
  constexpr auto InvalidDomain = static_cast<RuntimeFeatureDomain>(255);
  constexpr auto InvalidDisposition =
      static_cast<RuntimeFeatureDisposition>(255);
  static_assert(!isValidCluster(InvalidCluster));
  static_assert(!isValidRuntimePurpose(InvalidPurpose));
  static_assert(!isValidLoader(InvalidLoader));
  static_assert(!contains(kEveryPurpose, InvalidPurpose));
  static_assert(!isValidRuntimeFeatureDomain(InvalidDomain));
  static_assert(!isValidRuntimeFeatureDisposition(InvalidDisposition));
  EXPECT_TRUE(runtimeFeatureDomainName(InvalidDomain).empty());
  EXPECT_TRUE(runtimeFeatureDispositionName(InvalidDisposition).empty());

  RuntimeProfile Profile = currentMainnetProfile();
  Profile.OnCluster = InvalidCluster;
  auto ClusterResult = resolveRuntimeEnvironment(Profile);
  ASSERT_FALSE(static_cast<bool>(ClusterResult));
  EXPECT_NE(llvm::toString(ClusterResult.takeError()).find("cluster"),
            std::string::npos);

  Profile = currentMainnetProfile();
  Profile.Purpose = InvalidPurpose;
  auto PurposeResult = resolveRuntimeEnvironment(Profile);
  ASSERT_FALSE(static_cast<bool>(PurposeResult));
  EXPECT_NE(llvm::toString(PurposeResult.takeError()).find("purpose"),
            std::string::npos);

  Profile = currentMainnetProfile();
  Profile.OwningLoader = InvalidLoader;
  auto LoaderResult = resolveRuntimeEnvironment(Profile);
  ASSERT_FALSE(static_cast<bool>(LoaderResult));
  EXPECT_NE(llvm::toString(LoaderResult.takeError()).find("loader"),
            std::string::npos);

  auto PolicyResult =
      resolveRuntimeEnvironment(currentMainnetProfile(), InvalidPolicy);
  ASSERT_FALSE(static_cast<bool>(PolicyResult));
  EXPECT_NE(llvm::toString(PolicyResult.takeError()).find("policy"),
            std::string::npos);
}

TEST(SBFRuntimeProfile, MemoryTopologyGatesDoNotInventAnAccountABI) {
  RuntimeProfile Profile = currentMainnetProfile();
  Profile.Forced = RuntimeFeature::SyscallParameterAddressRestrictions |
                   RuntimeFeature::AccountDataDirectMapping;

  for (const LoaderInfo &Loader : loaderInfos()) {
    Profile.OwningLoader = Loader.ID;
    auto Environment = resolveRuntimeEnvironment(Profile);
    ASSERT_TRUE(static_cast<bool>(Environment))
        << llvm::toString(Environment.takeError());
    EXPECT_EQ(Environment->accountABI(), Loader.ABI);
    EXPECT_TRUE(
        hasFeature(Environment->activeRuntimeFeatures(),
                   RuntimeFeature::SyscallParameterAddressRestrictions));
    EXPECT_TRUE(hasFeature(Environment->activeRuntimeFeatures(),
                           RuntimeFeature::AccountDataDirectMapping));
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
