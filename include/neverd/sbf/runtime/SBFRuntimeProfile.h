//===- SBFRuntimeProfile.h - Which runtime the answer is about --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the runtime a recovered program is being described against.
///
/// Almost every question about a Solana program has more than one true answer,
/// and which one is wanted depends on facts that are not in the program file.
/// Whether a syscall resolves depends on the chain and the slot. Which bytes an
/// account field sits at depends on the loader that owns the program. Whether
/// the entrypoint receives a second argument depends on a switch the chain
/// throws. Whether a program can be deployed is a different question from
/// whether it runs.
///
/// A single "version" switch cannot express that, and trying makes it express
/// something false. These are separate axes, so they are separate fields.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_RUNTIME_SBFRUNTIMEPROFILE_H
#define NEVERD_SBF_RUNTIME_SBFRUNTIMEPROFILE_H

#include "neverd/sbf/solana/SBFAccountLayout.h"
#include "neverd/sbf/solana/SBFKnownAddresses.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace neverd::sbf {

//===----------------------------------------------------------------------===//
// The chain
//===----------------------------------------------------------------------===//

enum class Cluster : uint8_t {
#define SBF_CLUSTER(ID, NAME, ACTIVATES_EVERYTHING, SUMMARY) ID,
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  Count,
};

inline constexpr size_t kClusterCount = static_cast<size_t>(Cluster::Count);

constexpr bool isValidCluster(Cluster ID) {
  return static_cast<size_t>(ID) < kClusterCount;
}

struct ClusterInfo {
  Cluster ID;
  llvm::StringLiteral Name;
  /// True for a chain started from a fresh genesis, which activates every gate
  /// its validator knows rather than the ones a real chain has reached.
  bool ActivatesEverything;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<ClusterInfo> clusterInfos();
const ClusterInfo &getClusterInfo(Cluster ID);
llvm::StringRef clusterName(Cluster ID);
std::optional<Cluster> parseCluster(llvm::StringRef Name);

//===----------------------------------------------------------------------===//
// What is being asked
//===----------------------------------------------------------------------===//

enum class RuntimePurpose : uint8_t {
#define SBF_RUNTIME_PURPOSE(ID, NAME, SUMMARY) ID,
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  Count,
};

inline constexpr size_t kRuntimePurposeCount =
    static_cast<size_t>(RuntimePurpose::Count);

constexpr bool isValidRuntimePurpose(RuntimePurpose Purpose) {
  return static_cast<size_t>(Purpose) < kRuntimePurposeCount;
}

struct RuntimePurposeInfo {
  RuntimePurpose ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<RuntimePurposeInfo> runtimePurposeInfos();
llvm::StringRef runtimePurposeName(RuntimePurpose ID);
std::optional<RuntimePurpose> parseRuntimePurpose(llvm::StringRef Name);

/// A set of purposes, for saying that something belongs to one registry and
/// not the other.
enum class RuntimePurposeSet : uint8_t {
  None = 0,
#define SBF_RUNTIME_PURPOSE(ID, NAME, SUMMARY)                                 \
  ID = uint8_t{1} << static_cast<uint8_t>(RuntimePurpose::ID),
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
};

static_assert(kRuntimePurposeCount <= std::numeric_limits<uint8_t>::digits,
              "the purpose set must hold every tabulated runtime purpose");

constexpr RuntimePurposeSet operator|(RuntimePurposeSet L,
                                      RuntimePurposeSet R) {
  return static_cast<RuntimePurposeSet>(static_cast<uint8_t>(L) |
                                        static_cast<uint8_t>(R));
}

constexpr bool contains(RuntimePurposeSet Set, RuntimePurpose Purpose) {
  return isValidRuntimePurpose(Purpose) &&
         (static_cast<uint8_t>(Set) &
          (uint8_t{1} << static_cast<uint8_t>(Purpose))) != 0;
}

/// Every purpose, which is what applies to anything with no exception
/// recorded against it.
inline constexpr RuntimePurposeSet kEveryPurpose = [] {
  auto Every = RuntimePurposeSet::None;
#define SBF_RUNTIME_PURPOSE(ID, NAME, SUMMARY)                                 \
  Every = Every | RuntimePurposeSet::ID;
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  return Every;
}();

//===----------------------------------------------------------------------===//
// The loader
//===----------------------------------------------------------------------===//

enum class Loader : uint8_t {
#define SBF_LOADER(ID, NAME, KNOWN_ADDRESS, ACCOUNT_ABI, DEPLOYS, EXECUTES,    \
                   SUMMARY)                                                    \
  ID,
#include "neverd/sbf/runtime/SBFLoaders.def"
  Count,
};

inline constexpr size_t kLoaderCount = static_cast<size_t>(Loader::Count);

constexpr bool isValidLoader(Loader ID) {
  return static_cast<size_t>(ID) < kLoaderCount;
}

struct LoaderInfo {
  Loader ID;
  llvm::StringLiteral Name;
  KnownAddress Address;
  /// The shape of the buffer this loader hands the programs it owns.
  AccountABI ABI;
  bool Deploys;
  bool Executes;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<LoaderInfo> loaderInfos();
const LoaderInfo &getLoaderInfo(Loader ID);
llvm::StringRef loaderName(Loader ID);
std::optional<Loader> parseLoader(llvm::StringRef Name);

/// The loader that owns programs at \p Address, or nothing when the address is
/// not a loader.
std::optional<Loader> loaderForAddress(KnownAddress Address);

//===----------------------------------------------------------------------===//
// The gates
//===----------------------------------------------------------------------===//

/// The runtime subsystem whose externally observable behaviour a gate changes.
/// This is generated from the same definition file as the feature identities,
/// so consumers never have to classify gates by matching their spelling.
enum class RuntimeFeatureDomain : uint8_t {
#define SBF_RUNTIME_FEATURE_DOMAIN(ID, NAME, SUMMARY) ID,
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  Count,
};

inline constexpr size_t kRuntimeFeatureDomainCount =
    static_cast<size_t>(RuntimeFeatureDomain::Count);

constexpr bool isValidRuntimeFeatureDomain(RuntimeFeatureDomain Domain) {
  return static_cast<size_t>(Domain) < kRuntimeFeatureDomainCount;
}

struct RuntimeFeatureDomainInfo {
  RuntimeFeatureDomain ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<RuntimeFeatureDomainInfo> runtimeFeatureDomainInfos();
llvm::StringRef runtimeFeatureDomainName(RuntimeFeatureDomain Domain);

/// Whether the pinned Agave revision still branches on a feature at runtime.
/// Folded and historical rows remain valuable to a slot-qualified analysis,
/// but must not be presented as a current dynamic branch in upstream code.
enum class RuntimeFeatureDisposition : uint8_t {
#define SBF_RUNTIME_FEATURE_DISPOSITION(ID, NAME, SUMMARY) ID,
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  Count,
};

inline constexpr size_t kRuntimeFeatureDispositionCount =
    static_cast<size_t>(RuntimeFeatureDisposition::Count);

constexpr bool
isValidRuntimeFeatureDisposition(RuntimeFeatureDisposition Disposition) {
  return static_cast<size_t>(Disposition) < kRuntimeFeatureDispositionCount;
}

struct RuntimeFeatureDispositionInfo {
  RuntimeFeatureDisposition ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<RuntimeFeatureDispositionInfo> runtimeFeatureDispositionInfos();
llvm::StringRef
runtimeFeatureDispositionName(RuntimeFeatureDisposition Disposition);

enum class RuntimeFeatureBit : uint8_t {
#define SBF_RUNTIME_FEATURE(ID, NAME, GATE, ADDRESS, SIMD, DOMAIN,             \
                            DISPOSITION, SUMMARY)                              \
  ID,
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  Count,
};

inline constexpr size_t kRuntimeFeatureCount =
    static_cast<size_t>(RuntimeFeatureBit::Count);

/// Frozen storage and host-ABI width of one version-2 runtime-feature snapshot.
/// Feature identities are append-only, but this public width is not: if the
/// table ever outgrows this word, introduce a version-3 multiword snapshot
/// instead of changing this alias and silently breaking existing hosts.
using RuntimeFeatureMask = uint64_t;

enum class RuntimeFeature : RuntimeFeatureMask {
  None = 0,
#define SBF_RUNTIME_FEATURE(ID, NAME, GATE, ADDRESS, SIMD, DOMAIN,             \
                            DISPOSITION, SUMMARY)                              \
  ID = RuntimeFeatureMask{1}                                                   \
       << static_cast<RuntimeFeatureMask>(RuntimeFeatureBit::ID),
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
};

static_assert(kRuntimeFeatureCount <=
                  std::numeric_limits<RuntimeFeatureMask>::digits,
              "a runtime feature set must hold every tabulated gate");
static_assert(std::numeric_limits<RuntimeFeatureMask>::digits >
                  std::numeric_limits<uint32_t>::digits,
              "the host ABI must not truncate append-only feature bits");

constexpr RuntimeFeatureMask runtimeFeatureMask(RuntimeFeature Feature) {
  return static_cast<RuntimeFeatureMask>(Feature);
}

/// Every bit assigned by the append-only runtime-feature table.
constexpr RuntimeFeatureMask knownRuntimeFeatureMask() {
  RuntimeFeatureMask Known = 0;
#define SBF_RUNTIME_FEATURE(ID, NAME, GATE, ADDRESS, SIMD, DOMAIN,             \
                            DISPOSITION, SUMMARY)                              \
  Known |= runtimeFeatureMask(RuntimeFeature::ID);
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  return Known;
}

constexpr RuntimeFeature operator|(RuntimeFeature L, RuntimeFeature R) {
  return static_cast<RuntimeFeature>(runtimeFeatureMask(L) |
                                     runtimeFeatureMask(R));
}

constexpr bool hasFeature(RuntimeFeature Set, RuntimeFeature Feature) {
  return (runtimeFeatureMask(Set) & runtimeFeatureMask(Feature)) != 0;
}

struct RuntimeFeatureInfo {
  RuntimeFeature ID;
  llvm::StringLiteral Name;
  /// The identifier the runtime gives the switch.
  llvm::StringLiteral Gate;
  /// The feature account whose state records its activation slot. A pending
  /// account can exist without activating the gate.
  llvm::StringLiteral Address;
  /// The proposal that specified the behaviour, empty for switches that
  /// predate the process.
  llvm::StringLiteral SIMD;
  RuntimeFeatureDomain Domain;
  RuntimeFeatureDisposition Disposition;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<RuntimeFeatureInfo> runtimeFeatureInfos();
const RuntimeFeatureInfo &getRuntimeFeatureInfo(RuntimeFeatureBit Bit);
const RuntimeFeatureInfo *getRuntimeFeatureInfo(RuntimeFeature Feature);
llvm::StringRef runtimeFeatureName(RuntimeFeature Feature);
std::optional<RuntimeFeature> parseRuntimeFeature(llvm::StringRef Name);

/// The slot \p Feature was activated at on \p Cluster, and nothing when that
/// cluster has not activated it.
std::optional<uint64_t> runtimeFeatureActivation(RuntimeFeature Feature,
                                                 Cluster OnCluster);

//===----------------------------------------------------------------------===//
// The profile
//===----------------------------------------------------------------------===//

/// A slot far enough ahead to mean "everything the table records has already
/// happened", which is what an analysis with no particular slot in mind wants.
inline constexpr uint64_t kCurrentSlot = std::numeric_limits<uint64_t>::max();

/// Which runtime a recovered program is being described against.
struct RuntimeProfile {
  Cluster OnCluster = Cluster::MainnetBeta;
  /// The slot the description is about. A gate counts as on when its cluster
  /// activated it at or before this slot, which is what lets a program that
  /// was correct in its own era be read in that era rather than in this one.
  uint64_t Slot = kCurrentSlot;
  RuntimePurpose Purpose = RuntimePurpose::Execution;
  Loader OwningLoader = Loader::V3;
  /// Gates forced on and off regardless of what the cluster did. A validator
  /// can be run with either, and an analysis of a proposal has to be able to
  /// ask what would happen, so an override is a first-class input rather than
  /// a way of lying about a cluster.
  RuntimeFeature Forced = RuntimeFeature::None;
  RuntimeFeature Suppressed = RuntimeFeature::None;

  /// The loader-selected logical serialization. Runtime memory-topology gates
  /// can change how account data is backed or which addresses syscalls accept,
  /// but do not create another ABIv0/ABIv1 field layout.
  [[nodiscard]] AccountABI accountABI() const;
};

/// The mainnet profile as it stands, which is what a caller that says nothing
/// gets.
RuntimeProfile currentMainnetProfile();

bool isFeatureActive(const RuntimeProfile &Profile, RuntimeFeature Feature);

/// Every gate \p Profile has on, as one set.
RuntimeFeature activeFeatures(const RuntimeProfile &Profile);

/// Report a loader whose address is not a loader address, an activation naming
/// a feature or cluster the tables do not declare, or a profile forcing and
/// suppressing the same gate.
llvm::Error validateRuntimeProfileTables();

} // namespace neverd::sbf

#endif // NEVERD_SBF_RUNTIME_SBFRUNTIMEPROFILE_H
