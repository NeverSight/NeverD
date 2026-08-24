//===- SBFAgaveConformanceTests.cpp - Agave ELF fixture gate ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ELF/SBFELFLoader.h"
#include "neverd/sbf/image/SBFProgramImage.h"
#include "neverd/sbf/runtime/SBFRuntimeEnvironment.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/solana/SBFPubkey.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

#define SBF_AGAVE_CONFORMANCE_STRING(ID, VALUE)                                \
  constexpr llvm::StringLiteral kAgave##ID(VALUE);
#define SBF_AGAVE_CONFORMANCE_LIMIT(ID, VALUE)                                 \
  constexpr size_t kAgave##ID = VALUE;
#define SBF_AGAVE_XXH64_CONSTANT(ID, VALUE)                                    \
  constexpr uint64_t kXXH64##ID = VALUE;
#include "SBFAgaveConformanceProtocol.def"

static_assert(kAgaveAcceptedFixtureCount + kAgaveRejectedFixtureCount ==
                  kAgaveFixtureCount,
              "the pinned Agave accept/reject partition must cover corpus");

#define SBF_UPSTREAM_SOURCE(ID, NAME, REVISION)                                \
  constexpr llvm::StringLiteral k##ID##Revision(REVISION);
#include "neverd/sbf/runtime/SBFUpstreamSources.def"

enum class ProtoWireType : uint8_t {
#define SBF_AGAVE_PROTO_WIRE(ID, VALUE) ID = VALUE,
#include "SBFAgaveConformanceProtocol.def"
};

enum class ProtoField : uint32_t {
#define SBF_AGAVE_PROTO_FIELD(ID, VALUE, WIRE) ID = VALUE,
#include "SBFAgaveConformanceProtocol.def"
};

struct ProtoKey {
  uint32_t Number;
  ProtoWireType Wire;
};

struct ELFLoaderInput {
  std::vector<uint8_t> ELFData;
  std::vector<uint64_t> FeatureIDs;
  bool DeployChecks = false;
};

struct ELFLoaderOutput {
  uint32_t ErrorCode = 0;
  uint64_t RodataHash = 0;
  uint64_t TextCount = 0;
  uint64_t TextOffset = 0;
  uint64_t EntryPC = 0;
  uint64_t CallDestinationsHash = 0;
};

struct ELFLoaderFixture {
  ELFLoaderInput Input;
  ELFLoaderOutput Output;
};

llvm::Error fixtureError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("sbf: Agave ELF fixture: " + Message).str(),
      llvm::inconvertibleErrorCode());
}

class ProtoReader final {
public:
  explicit ProtoReader(llvm::ArrayRef<uint8_t> Bytes) : Bytes(Bytes) {}

  [[nodiscard]] bool empty() const { return Offset == Bytes.size(); }

  llvm::Expected<uint64_t> readVarint() {
    constexpr unsigned kMaximumVarintBytes = 10;
    constexpr uint8_t kPayloadMask = 0x7f;
    constexpr uint8_t kContinuationMask = 0x80;
    constexpr uint8_t kMaximumTenthByte = 1;
    constexpr unsigned kPayloadBits = 7;

    uint64_t Value = 0;
    for (unsigned Index = 0; Index < kMaximumVarintBytes; ++Index) {
      if (empty())
        return fixtureError("truncated varint");
      const uint8_t Byte = Bytes[Offset++];
      if (Index + 1 == kMaximumVarintBytes && Byte > kMaximumTenthByte)
        return fixtureError("varint overflows 64 bits");
      Value |= static_cast<uint64_t>(Byte & kPayloadMask)
               << (Index * kPayloadBits);
      if ((Byte & kContinuationMask) == 0) {
        if (Index != 0 && Byte == 0)
          return fixtureError("non-canonical overlong varint");
        return Value;
      }
    }
    return fixtureError("varint exceeds its maximum encoded length");
  }

  llvm::Expected<ProtoKey> readKey() {
    constexpr unsigned kWireTypeBits = 3;
    constexpr uint64_t kWireTypeMask = (uint64_t{1} << kWireTypeBits) - 1;
    auto Encoded = readVarint();
    if (!Encoded)
      return Encoded.takeError();
    const uint64_t Field = *Encoded >> kWireTypeBits;
    if (Field == 0 || Field > std::numeric_limits<uint32_t>::max())
      return fixtureError("invalid protobuf field number");
    const auto Wire = static_cast<ProtoWireType>(*Encoded & kWireTypeMask);
    switch (Wire) {
    case ProtoWireType::Varint:
    case ProtoWireType::Fixed64:
    case ProtoWireType::LengthDelimited:
    case ProtoWireType::Fixed32:
      return ProtoKey{static_cast<uint32_t>(Field), Wire};
    }
    return fixtureError("unsupported protobuf wire type");
  }

  llvm::Expected<uint64_t> readFixed64() {
    constexpr size_t kFixed64Bytes = sizeof(uint64_t);
    if (remaining() < kFixed64Bytes)
      return fixtureError("truncated fixed64 field");
    const uint64_t Value =
        llvm::support::endian::read64le(Bytes.data() + Offset);
    Offset += kFixed64Bytes;
    return Value;
  }

  llvm::Expected<llvm::ArrayRef<uint8_t>> readLengthDelimited() {
    auto EncodedSize = readVarint();
    if (!EncodedSize)
      return EncodedSize.takeError();
    if (*EncodedSize > remaining())
      return fixtureError("length-delimited field exceeds its message");
    const size_t Size = static_cast<size_t>(*EncodedSize);
    const llvm::ArrayRef<uint8_t> Value = Bytes.slice(Offset, Size);
    Offset += Size;
    return Value;
  }

private:
  [[nodiscard]] size_t remaining() const { return Bytes.size() - Offset; }

  llvm::ArrayRef<uint8_t> Bytes;
  size_t Offset = 0;
};

llvm::Error requireWire(const ProtoKey &Key, ProtoWireType Expected) {
  if (Key.Wire != Expected)
    return fixtureError(llvm::Twine("field ") + llvm::Twine(Key.Number) +
                        " has the wrong wire type");
  return llvm::Error::success();
}

llvm::Error rejectDuplicate(bool &Seen, llvm::StringRef Name) {
  if (Seen)
    return fixtureError("duplicate " + Name + " field");
  Seen = true;
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint64_t>>
parseFeatureSet(llvm::ArrayRef<uint8_t> Bytes) {
  ProtoReader Reader(Bytes);
  std::vector<uint64_t> Features;
  while (!Reader.empty()) {
    auto Key = Reader.readKey();
    if (!Key)
      return Key.takeError();
    if (Key->Number != static_cast<uint32_t>(ProtoField::FeatureSetFeatures))
      return fixtureError("unknown FeatureSet field");

    if (Key->Wire == ProtoWireType::Fixed64) {
      auto Feature = Reader.readFixed64();
      if (!Feature)
        return Feature.takeError();
      Features.push_back(*Feature);
      continue;
    }
    if (Key->Wire != ProtoWireType::LengthDelimited)
      return fixtureError("FeatureSet.features has the wrong wire type");
    auto Packed = Reader.readLengthDelimited();
    if (!Packed)
      return Packed.takeError();
    if (Packed->size() % sizeof(uint64_t) != 0)
      return fixtureError("packed FeatureSet is not fixed64-aligned");
    ProtoReader PackedReader(*Packed);
    while (!PackedReader.empty()) {
      auto Feature = PackedReader.readFixed64();
      if (!Feature)
        return Feature.takeError();
      Features.push_back(*Feature);
    }
  }
  return Features;
}

llvm::Error parseFixtureMetadata(llvm::ArrayRef<uint8_t> Bytes) {
  ProtoReader Reader(Bytes);
  bool SawEntrypoint = false;
  while (!Reader.empty()) {
    auto Key = Reader.readKey();
    if (!Key)
      return Key.takeError();
    if (Key->Number != static_cast<uint32_t>(ProtoField::MetadataEntrypoint))
      return fixtureError("unknown FixtureMetadata field");
    if (llvm::Error Error = requireWire(*Key, ProtoWireType::LengthDelimited))
      return Error;
    if (llvm::Error Error = rejectDuplicate(SawEntrypoint, "entrypoint"))
      return Error;
    auto Entrypoint = Reader.readLengthDelimited();
    if (!Entrypoint)
      return Entrypoint.takeError();
    const llvm::StringRef Name(
        reinterpret_cast<const char *>(Entrypoint->data()), Entrypoint->size());
    if (Name != kAgaveFixtureEntrypoint)
      return fixtureError("metadata names an unexpected entrypoint");
  }
  if (!SawEntrypoint)
    return fixtureError("metadata is missing its entrypoint");
  return llvm::Error::success();
}

llvm::Expected<ELFLoaderInput> parseLoaderInput(llvm::ArrayRef<uint8_t> Bytes) {
  ProtoReader Reader(Bytes);
  ELFLoaderInput Input;
  bool SawELFData = false;
  bool SawFeatures = false;
  bool SawDeployChecks = false;
  while (!Reader.empty()) {
    auto Key = Reader.readKey();
    if (!Key)
      return Key.takeError();
    switch (static_cast<ProtoField>(Key->Number)) {
    case ProtoField::InputELFData: {
      if (llvm::Error Error = requireWire(*Key, ProtoWireType::LengthDelimited))
        return std::move(Error);
      if (llvm::Error Error = rejectDuplicate(SawELFData, "ELF data"))
        return std::move(Error);
      auto ELFData = Reader.readLengthDelimited();
      if (!ELFData)
        return ELFData.takeError();
      Input.ELFData.assign(ELFData->begin(), ELFData->end());
      break;
    }
    case ProtoField::InputFeatures: {
      if (llvm::Error Error = requireWire(*Key, ProtoWireType::LengthDelimited))
        return std::move(Error);
      if (llvm::Error Error = rejectDuplicate(SawFeatures, "feature set"))
        return std::move(Error);
      auto FeatureBytes = Reader.readLengthDelimited();
      if (!FeatureBytes)
        return FeatureBytes.takeError();
      auto Features = parseFeatureSet(*FeatureBytes);
      if (!Features)
        return Features.takeError();
      Input.FeatureIDs = std::move(*Features);
      break;
    }
    case ProtoField::InputDeployChecks: {
      if (llvm::Error Error = requireWire(*Key, ProtoWireType::Varint))
        return std::move(Error);
      if (llvm::Error Error = rejectDuplicate(SawDeployChecks, "deploy-checks"))
        return std::move(Error);
      auto DeployChecks = Reader.readVarint();
      if (!DeployChecks)
        return DeployChecks.takeError();
      if (*DeployChecks > 1)
        return fixtureError("deploy-checks is not a canonical boolean");
      Input.DeployChecks = *DeployChecks != 0;
      break;
    }
    default:
      return fixtureError("unknown ELFLoaderCtx field");
    }
  }
  if (!SawELFData || !SawFeatures)
    return fixtureError("ELFLoaderCtx is missing ELF data or its feature set");
  return Input;
}

llvm::Expected<ELFLoaderOutput>
parseLoaderOutput(llvm::ArrayRef<uint8_t> Bytes) {
  ProtoReader Reader(Bytes);
  ELFLoaderOutput Output;
  bool SawErrorCode = false;
  bool SawRodataHash = false;
  bool SawTextCount = false;
  bool SawTextOffset = false;
  bool SawEntryPC = false;
  bool SawCallDestinationsHash = false;
  while (!Reader.empty()) {
    auto Key = Reader.readKey();
    if (!Key)
      return Key.takeError();
    switch (static_cast<ProtoField>(Key->Number)) {
    case ProtoField::OutputErrorCode: {
      if (llvm::Error Error = requireWire(*Key, ProtoWireType::Varint))
        return std::move(Error);
      if (llvm::Error Error = rejectDuplicate(SawErrorCode, "error-code"))
        return std::move(Error);
      auto Value = Reader.readVarint();
      if (!Value)
        return Value.takeError();
      if (*Value > std::numeric_limits<uint32_t>::max())
        return fixtureError("loader error code overflows uint32");
      Output.ErrorCode = static_cast<uint32_t>(*Value);
      break;
    }
    case ProtoField::OutputRodataHash: {
      if (llvm::Error Error = requireWire(*Key, ProtoWireType::Fixed64))
        return std::move(Error);
      if (llvm::Error Error = rejectDuplicate(SawRodataHash, "rodata-hash"))
        return std::move(Error);
      auto Value = Reader.readFixed64();
      if (!Value)
        return Value.takeError();
      Output.RodataHash = *Value;
      break;
    }
    case ProtoField::OutputTextCount: {
      if (llvm::Error Error = requireWire(*Key, ProtoWireType::Varint))
        return std::move(Error);
      if (llvm::Error Error = rejectDuplicate(SawTextCount, "text-count"))
        return std::move(Error);
      auto Value = Reader.readVarint();
      if (!Value)
        return Value.takeError();
      Output.TextCount = *Value;
      break;
    }
    case ProtoField::OutputTextOffset: {
      if (llvm::Error Error = requireWire(*Key, ProtoWireType::Varint))
        return std::move(Error);
      if (llvm::Error Error = rejectDuplicate(SawTextOffset, "text-offset"))
        return std::move(Error);
      auto Value = Reader.readVarint();
      if (!Value)
        return Value.takeError();
      Output.TextOffset = *Value;
      break;
    }
    case ProtoField::OutputEntryPC: {
      if (llvm::Error Error = requireWire(*Key, ProtoWireType::Varint))
        return std::move(Error);
      if (llvm::Error Error = rejectDuplicate(SawEntryPC, "entry-pc"))
        return std::move(Error);
      auto Value = Reader.readVarint();
      if (!Value)
        return Value.takeError();
      Output.EntryPC = *Value;
      break;
    }
    case ProtoField::OutputCallDestinationsHash: {
      if (llvm::Error Error = requireWire(*Key, ProtoWireType::Fixed64))
        return std::move(Error);
      if (llvm::Error Error = rejectDuplicate(SawCallDestinationsHash,
                                              "call-destinations-hash"))
        return std::move(Error);
      auto Value = Reader.readFixed64();
      if (!Value)
        return Value.takeError();
      Output.CallDestinationsHash = *Value;
      break;
    }
    default:
      return fixtureError("unknown ELFLoaderEffects field");
    }
  }
  // Proto3 canonically omits scalar zero values, so a successful fixture need
  // not carry error_code, text_cnt, text_off, or entry_pc. The two XXH64
  // effects are non-zero even for empty inputs and are therefore the mandatory
  // evidence that this is a complete success record rather than an empty or
  // truncated submessage.
  if (Output.ErrorCode == 0 && (!SawRodataHash || !SawCallDestinationsHash))
    return fixtureError(
        "successful ELFLoaderEffects is missing its XXH64 effects");
  return Output;
}

llvm::Expected<ELFLoaderFixture> parseFixture(llvm::ArrayRef<uint8_t> Bytes) {
  ProtoReader Reader(Bytes);
  ELFLoaderFixture Fixture;
  bool SawMetadata = false;
  bool SawInput = false;
  bool SawOutput = false;
  while (!Reader.empty()) {
    auto Key = Reader.readKey();
    if (!Key)
      return Key.takeError();
    if (llvm::Error Error = requireWire(*Key, ProtoWireType::LengthDelimited))
      return std::move(Error);
    auto Message = Reader.readLengthDelimited();
    if (!Message)
      return Message.takeError();
    switch (static_cast<ProtoField>(Key->Number)) {
    case ProtoField::FixtureMetadata:
      if (llvm::Error Error = rejectDuplicate(SawMetadata, "metadata"))
        return std::move(Error);
      if (llvm::Error Error = parseFixtureMetadata(*Message))
        return std::move(Error);
      break;
    case ProtoField::FixtureInput: {
      if (llvm::Error Error = rejectDuplicate(SawInput, "input"))
        return std::move(Error);
      auto Input = parseLoaderInput(*Message);
      if (!Input)
        return Input.takeError();
      Fixture.Input = std::move(*Input);
      break;
    }
    case ProtoField::FixtureOutput: {
      if (llvm::Error Error = rejectDuplicate(SawOutput, "output"))
        return std::move(Error);
      auto Output = parseLoaderOutput(*Message);
      if (!Output)
        return Output.takeError();
      Fixture.Output = *Output;
      break;
    }
    default:
      return fixtureError("unknown ELFLoaderFixture field");
    }
  }
  if (!SawMetadata || !SawInput || !SawOutput)
    return fixtureError("fixture is missing metadata, input, or output");
  return Fixture;
}

struct FeatureFingerprint {
  RuntimeFeature Feature;
  uint64_t WireID;
};

llvm::Expected<std::vector<FeatureFingerprint>>
validateFeatureFingerprints(std::vector<FeatureFingerprint> Fingerprints) {
  llvm::sort(Fingerprints, [](const FeatureFingerprint &Left,
                              const FeatureFingerprint &Right) {
    if (Left.WireID != Right.WireID)
      return Left.WireID < Right.WireID;
    return runtimeFeatureMask(Left.Feature) < runtimeFeatureMask(Right.Feature);
  });
  if (std::adjacent_find(
          Fingerprints.begin(), Fingerprints.end(),
          [](const FeatureFingerprint &Left, const FeatureFingerprint &Right) {
            return Left.WireID == Right.WireID;
          }) != Fingerprints.end())
    return fixtureError("runtime feature wire fingerprints collide");
  return Fingerprints;
}

bool isAgaveWireFeature(RuntimeFeature Feature) {
  switch (Feature) {
#define SBF_AGAVE_NON_WIRE_RUNTIME_FEATURE(ID)                                 \
  case RuntimeFeature::ID:                                                     \
    return false;
#include "SBFAgaveConformanceProtocol.def"
  default:
    return true;
  }
}

llvm::Expected<std::vector<FeatureFingerprint>> featureFingerprints() {
  std::vector<FeatureFingerprint> Fingerprints;
  Fingerprints.reserve(runtimeFeatureInfos().size());
  for (const RuntimeFeatureInfo &Info : runtimeFeatureInfos()) {
    if (!isAgaveWireFeature(Info.ID))
      continue;
    auto Key = parsePubkey(Info.Address);
    if (!Key)
      return Key.takeError();
    Fingerprints.push_back(
        {Info.ID, llvm::support::endian::read64le(Key->Bytes.data())});
  }
  return validateFeatureFingerprints(std::move(Fingerprints));
}

RuntimeFeature
activeRuntimeFeatures(llvm::ArrayRef<uint64_t> FeatureIDs,
                      llvm::ArrayRef<FeatureFingerprint> Fingerprints) {
  RuntimeFeature Active = RuntimeFeature::None;
  for (const FeatureFingerprint &Fingerprint : Fingerprints)
    if (llvm::is_contained(FeatureIDs, Fingerprint.WireID))
      Active = Active | Fingerprint.Feature;
  return Active;
}

llvm::Expected<ExpertRuntimeEnvironmentOverride>
makeEnvironment(const ELFLoaderInput &Input, RuntimeFeature ActiveFeatures) {
  const bool AdjustedAddressSpace = hasFeature(
      ActiveFeatures, RuntimeFeature::VirtualAddressSpaceAdjustments);

  SBFVMConfig Config;
  Config.EnableStackFrameGaps = !AdjustedAddressSpace;
  Config.OptimizeRodata = false;
  Config.AlignedMemoryMapping = !AdjustedAddressSpace;
  Config.RejectBrokenELFs = Input.DeployChecks;

  const RuntimePurpose Purpose = Input.DeployChecks ? RuntimePurpose::Deployment
                                                    : RuntimePurpose::Execution;
  auto MinimumVersion = minimumAgaveVersion(Purpose, ActiveFeatures);
  if (!MinimumVersion)
    return MinimumVersion.takeError();
  ExpertRuntimeEnvironmentOverride Environment;
  Environment.MinimumVersion = *MinimumVersion;
  Environment.MaximumVersion = Version::V3;
  Environment.VMConfig = Config;
  Environment.RegisteredSyscallHashes =
      registeredSyscallHashes(Purpose, ActiveFeatures);
  Environment.ActiveRuntimeFeatures = ActiveFeatures;
  Environment.InputABI = AccountABI::V1;
  return Environment;
}

struct ObservedLoaderOutput {
  bool Accepted = false;
  std::string Diagnostic;
  uint64_t RodataHash = 0;
  uint64_t TextCount = 0;
  uint64_t TextOffset = 0;
  uint64_t EntryPC = 0;
  uint64_t CallDestinationsHash = 0;
};

uint64_t xxh64Round(uint64_t Accumulator, uint64_t Input) {
  constexpr unsigned kAccumulatorRotation = 31;
  Accumulator += Input * kXXH64Prime2;
  Accumulator = std::rotl(Accumulator, kAccumulatorRotation);
  return Accumulator * kXXH64Prime1;
}

uint64_t xxh64MergeRound(uint64_t Accumulator, uint64_t Value) {
  Accumulator ^= xxh64Round(0, Value);
  return Accumulator * kXXH64Prime1 + kXXH64Prime4;
}

uint64_t xxh64(llvm::ArrayRef<uint8_t> Bytes, uint64_t Seed = 0) {
  constexpr size_t kBulkLaneCount = 4;
  constexpr unsigned kLane1Rotation = 1;
  constexpr unsigned kLane2Rotation = 7;
  constexpr unsigned kLane3Rotation = 12;
  constexpr unsigned kLane4Rotation = 18;
  constexpr unsigned kWordRotation = 27;
  constexpr unsigned kDoubleWordRotation = 23;
  constexpr unsigned kByteRotation = 11;
  constexpr unsigned kAvalancheShift1 = 33;
  constexpr unsigned kAvalancheShift2 = 29;
  constexpr unsigned kAvalancheShift3 = 32;
  constexpr uint8_t kEmptyStorage = 0;
  const uint8_t *Cursor = Bytes.empty() ? &kEmptyStorage : Bytes.data();
  const uint8_t *const End = Cursor + Bytes.size();
  uint64_t Hash = 0;
  if (Bytes.size() >= kBulkLaneCount * sizeof(uint64_t)) {
    const uint8_t *const BulkEnd = End - kBulkLaneCount * sizeof(uint64_t);
    uint64_t Lane1 = Seed + kXXH64Prime1 + kXXH64Prime2;
    uint64_t Lane2 = Seed + kXXH64Prime2;
    uint64_t Lane3 = Seed;
    uint64_t Lane4 = Seed - kXXH64Prime1;
    do {
      Lane1 = xxh64Round(Lane1, llvm::support::endian::read64le(Cursor));
      Cursor += sizeof(uint64_t);
      Lane2 = xxh64Round(Lane2, llvm::support::endian::read64le(Cursor));
      Cursor += sizeof(uint64_t);
      Lane3 = xxh64Round(Lane3, llvm::support::endian::read64le(Cursor));
      Cursor += sizeof(uint64_t);
      Lane4 = xxh64Round(Lane4, llvm::support::endian::read64le(Cursor));
      Cursor += sizeof(uint64_t);
    } while (Cursor <= BulkEnd);
    Hash = std::rotl(Lane1, kLane1Rotation) + std::rotl(Lane2, kLane2Rotation) +
           std::rotl(Lane3, kLane3Rotation) + std::rotl(Lane4, kLane4Rotation);
    Hash = xxh64MergeRound(Hash, Lane1);
    Hash = xxh64MergeRound(Hash, Lane2);
    Hash = xxh64MergeRound(Hash, Lane3);
    Hash = xxh64MergeRound(Hash, Lane4);
  } else {
    Hash = Seed + kXXH64Prime5;
  }

  Hash += Bytes.size();
  while (static_cast<size_t>(End - Cursor) >= sizeof(uint64_t)) {
    const uint64_t Lane =
        xxh64Round(0, llvm::support::endian::read64le(Cursor));
    Hash ^= Lane;
    Hash = std::rotl(Hash, kWordRotation) * kXXH64Prime1 + kXXH64Prime4;
    Cursor += sizeof(uint64_t);
  }
  if (static_cast<size_t>(End - Cursor) >= sizeof(uint32_t)) {
    Hash ^= static_cast<uint64_t>(llvm::support::endian::read32le(Cursor)) *
            kXXH64Prime1;
    Hash = std::rotl(Hash, kDoubleWordRotation) * kXXH64Prime2 + kXXH64Prime3;
    Cursor += sizeof(uint32_t);
  }
  while (Cursor != End) {
    Hash ^= static_cast<uint64_t>(*Cursor++) * kXXH64Prime5;
    Hash = std::rotl(Hash, kByteRotation) * kXXH64Prime1;
  }

  Hash ^= Hash >> kAvalancheShift1;
  Hash *= kXXH64Prime2;
  Hash ^= Hash >> kAvalancheShift2;
  Hash *= kXXH64Prime3;
  return Hash ^ (Hash >> kAvalancheShift3);
}

llvm::Expected<llvm::ArrayRef<uint8_t>>
readOnlyImage(const ProgramImage &Program) {
  const ProgramRegion *ReadOnly = nullptr;
  for (const ProgramRegion &Region : Program.regions()) {
    if (Region.Kind != ProgramRegionKind::LegacyReadOnly &&
        Region.Kind != ProgramRegionKind::ReadOnly)
      continue;
    if (ReadOnly)
      return fixtureError("accepted image has multiple read-only images");
    ReadOnly = &Region;
  }
  return ReadOnly ? llvm::ArrayRef<uint8_t>(ReadOnly->Bytes)
                  : llvm::ArrayRef<uint8_t>();
}

llvm::Expected<uint64_t> callDestinationsHash(const ProgramImage &Program) {
  std::vector<uint64_t> Destinations;
  Destinations.reserve(Program.functions().size());
  for (const ProgramFunctionEntry &Function : Program.functions())
    Destinations.push_back(Function.TargetSlot);
  llvm::sort(Destinations);
  Destinations.erase(std::unique(Destinations.begin(), Destinations.end()),
                     Destinations.end());
  if (Destinations.size() >
      std::numeric_limits<size_t>::max() / sizeof(uint64_t))
    return fixtureError("call-destination byte size overflows");
  std::vector<uint8_t> Bytes(Destinations.size() * sizeof(uint64_t));
  for (size_t Index = 0; Index < Destinations.size(); ++Index)
    llvm::support::endian::write64le(Bytes.data() + Index * sizeof(uint64_t),
                                     Destinations[Index]);
  return xxh64(Bytes);
}

llvm::Expected<ObservedLoaderOutput>
observeNeverD(const ELFLoaderFixture &Fixture, RuntimeFeature ActiveFeatures) {
  BinaryImage Image;
  Image.Raw = Fixture.Input.ELFData;
  auto Loaded = loadSBFELF(Image);
  if (!Loaded)
    return ObservedLoaderOutput{false, llvm::toString(Loaded.takeError())};
  if (!*Loaded)
    return ObservedLoaderOutput{false, "input is not an SBF ELF"};

  auto Override = makeEnvironment(Fixture.Input, ActiveFeatures);
  if (!Override)
    return Override.takeError();
  auto Environment = resolveExpertRuntimeEnvironment(*Override);
  if (!Environment)
    return Environment.takeError();
  auto Program = buildProgramImage(Image, *Image.SBF, *Environment);
  if (!Program)
    return ObservedLoaderOutput{false, llvm::toString(Program.takeError())};
  if (Program->textAddress() < kBytecodeStart)
    return ObservedLoaderOutput{
        false, "accepted image maps text below the bytecode region"};

  auto Rodata = readOnlyImage(*Program);
  if (!Rodata)
    return Rodata.takeError();
  auto CallHash = callDestinationsHash(*Program);
  if (!CallHash)
    return CallHash.takeError();

  return ObservedLoaderOutput{
      true,
      {},
      xxh64(*Rodata),
      static_cast<uint64_t>(Program->text().size() / kInstructionSize),
      Program->textAddress() - kBytecodeStart,
      static_cast<uint64_t>(Program->entrySlot()),
      *CallHash};
}

std::optional<std::filesystem::path> configuredRoot() {
  if (const char *Root = std::getenv(kAgaveRootEnvironment.data()))
    if (*Root != '\0')
      return std::filesystem::path(Root);
  return std::nullopt;
}

bool conformanceRequired() {
  const char *Required = std::getenv(kAgaveRequiredEnvironment.data());
  return Required && llvm::StringRef(Required) == kAgaveRequiredValue;
}

class TemporaryGitOutput final {
public:
  TemporaryGitOutput() {
    Error = llvm::sys::fs::createTemporaryFile("neverd-agave-git", "stdout",
                                               StandardOutput);
    if (!Error)
      Error = llvm::sys::fs::createTemporaryFile("neverd-agave-git", "stderr",
                                                 StandardError);
  }

  ~TemporaryGitOutput() {
    if (!StandardOutput.empty())
      llvm::sys::fs::remove(StandardOutput);
    if (!StandardError.empty())
      llvm::sys::fs::remove(StandardError);
  }

  std::error_code error() const { return Error; }
  llvm::StringRef standardOutput() const { return StandardOutput; }
  llvm::StringRef standardError() const { return StandardError; }

private:
  llvm::SmallString<128> StandardOutput;
  llvm::SmallString<128> StandardError;
  std::error_code Error;
};

llvm::Expected<std::string> runGit(const std::filesystem::path &Root,
                                   llvm::ArrayRef<llvm::StringRef> Arguments) {
  auto Git = llvm::sys::findProgramByName(kAgaveGitProgram);
  if (!Git)
    return fixtureError("cannot find git to authenticate the fixture corpus");
  TemporaryGitOutput Output;
  if (Output.error())
    return fixtureError("cannot create git authentication output files");

  const std::string RootString = Root.string();
  llvm::SmallVector<llvm::StringRef, 12> Command{*Git, "-C", RootString,
                                                 "--literal-pathspecs"};
  Command.append(Arguments.begin(), Arguments.end());
  std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, Output.standardOutput(), Output.standardError()};
  std::string ExecutionError;
  const int ExitCode = llvm::sys::ExecuteAndWait(
      *Git, Command, std::nullopt, Redirects, kAgaveGitTimeoutSeconds,
      /*MemoryLimit=*/0, &ExecutionError);
  if (ExitCode != 0 || !ExecutionError.empty())
    return fixtureError("git could not authenticate the fixture corpus");
  auto Buffer = llvm::MemoryBuffer::getFile(Output.standardOutput());
  if (!Buffer)
    return fixtureError("cannot read git authentication output");
  return (*Buffer)->getBuffer().str();
}

llvm::Error verifyCorpusIdentity(const std::filesystem::path &Root) {
  constexpr llvm::StringLiteral kRevParse("rev-parse");
  constexpr llvm::StringLiteral kVerify("--verify");
  const std::array<llvm::StringRef, 3> RevisionArguments{kRevParse, kVerify,
                                                         kAgaveGitHeadCommit};
  auto Revision = runGit(Root, RevisionArguments);
  if (!Revision)
    return Revision.takeError();
  if (llvm::StringRef(*Revision).trim() != kFiredancerTestVectorsRevision)
    return fixtureError("fixture checkout HEAD is not the pinned revision");

  constexpr llvm::StringLiteral kStatus("status");
  constexpr llvm::StringLiteral kPorcelain("--porcelain=v1");
  constexpr llvm::StringLiteral kUntracked("--untracked-files=all");
  constexpr llvm::StringLiteral kPathSeparator("--");
  const std::array<llvm::StringRef, 5> StatusArguments{
      kStatus, kPorcelain, kUntracked, kPathSeparator, kAgaveFixtureDirectory};
  auto Status = runGit(Root, StatusArguments);
  if (!Status)
    return Status.takeError();
  if (!llvm::StringRef(*Status).trim().empty())
    return fixtureError("fixture directory differs from its pinned git tree");
  return llvm::Error::success();
}

llvm::Expected<std::vector<std::filesystem::path>>
fixturePaths(const std::filesystem::path &Root) {
  const std::filesystem::path Directory = Root / kAgaveFixtureDirectory.str();
  std::error_code Error;
  if (!std::filesystem::is_directory(Directory, Error) || Error)
    return fixtureError("fixture directory is missing");

  std::vector<std::filesystem::path> Paths;
  for (std::filesystem::directory_iterator It(Directory, Error), End;
       !Error && It != End; It.increment(Error)) {
    if (!It->is_regular_file(Error) || Error)
      return fixtureError("fixture directory contains a non-regular entry");
    if (It->path().extension() !=
        std::filesystem::path(kAgaveFixtureExtension.str()))
      return fixtureError("fixture directory contains an unexpected file");
    Paths.push_back(It->path());
  }
  if (Error)
    return fixtureError("fixture directory cannot be enumerated");
  llvm::sort(Paths);
  if (Paths.size() != kAgaveFixtureCount)
    return fixtureError(llvm::Twine("expected ") +
                        llvm::Twine(kAgaveFixtureCount) + " fixtures, found " +
                        llvm::Twine(Paths.size()));
  return Paths;
}

llvm::Expected<ELFLoaderFixture>
readFixture(const std::filesystem::path &Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string(), false, false);
  if (!Buffer)
    return fixtureError("cannot read " + Path.string());
  const llvm::StringRef Contents = (*Buffer)->getBuffer();
  return parseFixture(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Contents.data()), Contents.size()));
}

void appendVarint(std::vector<uint8_t> &Bytes, uint64_t Value) {
  constexpr uint8_t kPayloadMask = 0x7f;
  constexpr uint8_t kContinuationMask = 0x80;
  constexpr unsigned kPayloadBits = 7;
  do {
    uint8_t Byte = static_cast<uint8_t>(Value & kPayloadMask);
    Value >>= kPayloadBits;
    if (Value != 0)
      Byte |= kContinuationMask;
    Bytes.push_back(Byte);
  } while (Value != 0);
}

void appendMessage(std::vector<uint8_t> &Bytes, ProtoField Field,
                   llvm::ArrayRef<uint8_t> Message) {
  constexpr unsigned kWireTypeBits = 3;
  appendVarint(Bytes, static_cast<uint64_t>(Field) << kWireTypeBits |
                          static_cast<uint8_t>(ProtoWireType::LengthDelimited));
  appendVarint(Bytes, Message.size());
  Bytes.insert(Bytes.end(), Message.begin(), Message.end());
}

void appendFixed64(std::vector<uint8_t> &Bytes, ProtoField Field,
                   uint64_t Value) {
  constexpr unsigned kWireTypeBits = 3;
  appendVarint(Bytes, static_cast<uint64_t>(Field) << kWireTypeBits |
                          static_cast<uint8_t>(ProtoWireType::Fixed64));
  const size_t Offset = Bytes.size();
  Bytes.resize(Offset + sizeof(uint64_t));
  llvm::support::endian::write64le(Bytes.data() + Offset, Value);
}

std::vector<uint8_t>
minimalFixture(llvm::StringRef Entrypoint = kAgaveFixtureEntrypoint,
               bool IncludeSuccessEvidence = true) {
  std::vector<uint8_t> Metadata;
  appendMessage(Metadata, ProtoField::MetadataEntrypoint,
                llvm::ArrayRef<uint8_t>(
                    reinterpret_cast<const uint8_t *>(Entrypoint.data()),
                    Entrypoint.size()));

  std::vector<uint8_t> Input;
  constexpr std::array<uint8_t, 1> ELFPrefix{0x7f};
  appendMessage(Input, ProtoField::InputELFData, ELFPrefix);
  appendMessage(Input, ProtoField::InputFeatures, {});

  std::vector<uint8_t> Output;
  if (IncludeSuccessEvidence) {
    appendFixed64(Output, ProtoField::OutputRodataHash, kXXH64EmptyHash);
    appendFixed64(Output, ProtoField::OutputCallDestinationsHash,
                  kXXH64EmptyHash);
  }

  std::vector<uint8_t> Fixture;
  appendMessage(Fixture, ProtoField::FixtureMetadata, Metadata);
  appendMessage(Fixture, ProtoField::FixtureInput, Input);
  appendMessage(Fixture, ProtoField::FixtureOutput, Output);
  return Fixture;
}

TEST(SBFAgaveConformanceProtocol, ParsesTheTypedMinimalFixture) {
  const std::vector<uint8_t> Bytes = minimalFixture();
  auto Fixture = parseFixture(Bytes);
  ASSERT_TRUE(static_cast<bool>(Fixture))
      << (Fixture ? std::string() : llvm::toString(Fixture.takeError()));
  ASSERT_EQ(Fixture->Input.ELFData.size(), 1u);
  EXPECT_EQ(Fixture->Input.ELFData.front(), 0x7f);
  EXPECT_TRUE(Fixture->Input.FeatureIDs.empty());
  EXPECT_FALSE(Fixture->Input.DeployChecks);
  EXPECT_EQ(Fixture->Output.ErrorCode, 0u);
  EXPECT_EQ(Fixture->Output.RodataHash, kXXH64EmptyHash);
  EXPECT_EQ(Fixture->Output.TextCount, 0u)
      << "omitted proto3 zero values must retain their typed defaults";
  EXPECT_EQ(Fixture->Output.CallDestinationsHash, kXXH64EmptyHash);
}

TEST(SBFAgaveConformanceProtocol, RejectsAnEmptySuccessfulOutput) {
  const std::vector<uint8_t> Bytes =
      minimalFixture(kAgaveFixtureEntrypoint,
                     /*IncludeSuccessEvidence=*/false);
  auto Fixture = parseFixture(Bytes);
  ASSERT_FALSE(static_cast<bool>(Fixture));
  EXPECT_NE(llvm::toString(Fixture.takeError()).find("XXH64"),
            std::string::npos);
}

TEST(SBFAgaveConformanceProtocol, RejectsUnknownAndTruncatedMessages) {
  constexpr unsigned kWireTypeBits = 3;
  std::vector<uint8_t> Unknown = minimalFixture();
  const uint32_t UnknownField =
      static_cast<uint32_t>(ProtoField::FixtureOutput) + 1;
  appendVarint(Unknown,
               static_cast<uint64_t>(UnknownField) << kWireTypeBits |
                   static_cast<uint8_t>(ProtoWireType::LengthDelimited));
  appendVarint(Unknown, 0);
  auto UnknownResult = parseFixture(Unknown);
  ASSERT_FALSE(static_cast<bool>(UnknownResult));
  EXPECT_NE(llvm::toString(UnknownResult.takeError()).find("unknown"),
            std::string::npos);

  std::vector<uint8_t> Truncated;
  appendVarint(Truncated,
               static_cast<uint64_t>(ProtoField::FixtureInput)
                       << kWireTypeBits |
                   static_cast<uint8_t>(ProtoWireType::LengthDelimited));
  appendVarint(Truncated, 1);
  auto TruncatedResult = parseFixture(Truncated);
  ASSERT_FALSE(static_cast<bool>(TruncatedResult));
  EXPECT_NE(llvm::toString(TruncatedResult.takeError()).find("exceeds"),
            std::string::npos);
}

TEST(SBFAgaveConformanceProtocol, RejectsDuplicateFeatureSetFields) {
  std::vector<uint8_t> Input;
  constexpr std::array<uint8_t, 1> ELFPrefix{0x7f};
  appendMessage(Input, ProtoField::InputELFData, ELFPrefix);
  appendMessage(Input, ProtoField::InputFeatures, {});
  appendMessage(Input, ProtoField::InputFeatures, {});

  auto Result = parseLoaderInput(Input);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("duplicate feature set"),
            std::string::npos);
}

TEST(SBFAgaveConformanceProtocol, RejectsAnotherFixtureEntrypoint) {
  auto Result =
      parseFixture(minimalFixture(kAgaveFixtureEntrypoint.drop_back()));
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("unexpected entrypoint"),
            std::string::npos);
}

TEST(SBFAgaveConformanceProtocol,
     MapsObservableRuntimeGatesFromTheAgaveWireFingerprint) {
  auto Fingerprints = featureFingerprints();
  ASSERT_TRUE(static_cast<bool>(Fingerprints))
      << (Fingerprints ? std::string()
                       : llvm::toString(Fingerprints.takeError()));

  constexpr std::array Features{
      RuntimeFeature::SyscallParameterAddressRestrictions,
      RuntimeFeature::AccountDataDirectMapping,
      RuntimeFeature::DisableAllocFreeDeployment,
      RuntimeFeature::EnableLoaderSetAuthorityChecked,
      RuntimeFeature::RemoveLoaderIncorrectProgramID,
      RuntimeFeature::SimplifyAltBn128ErrorCodes,
      RuntimeFeature::AbortOnInvalidCurve,
      RuntimeFeature::DepleteCUMeterOnVMFailure,
      RuntimeFeature::FixAltBn128MultiplicationInputLength,
      RuntimeFeature::RaiseCPINestingLimit,
      RuntimeFeature::IncreaseCPIAccountInfoLimit,
      RuntimeFeature::PoseidonEnforcePadding,
      RuntimeFeature::FixAltBn128PairingLength,
      RuntimeFeature::AltBn128LittleEndian,
      RuntimeFeature::AltBn128G2Syscalls,
      RuntimeFeature::LoaderV3MinimumExtendProgramSize,
  };
  for (RuntimeFeature Feature : Features) {
    const RuntimeFeatureInfo *Info = getRuntimeFeatureInfo(Feature);
    ASSERT_NE(Info, nullptr);
    auto Key = parsePubkey(Info->Address);
    ASSERT_TRUE(static_cast<bool>(Key))
        << (Key ? std::string() : llvm::toString(Key.takeError()));
    const uint64_t WireID = llvm::support::endian::read64le(Key->Bytes.data());
    EXPECT_TRUE(
        llvm::any_of(*Fingerprints, [&](const FeatureFingerprint &Fingerprint) {
          return Fingerprint.Feature == Feature && Fingerprint.WireID == WireID;
        }));
    EXPECT_TRUE(
        hasFeature(activeRuntimeFeatures({WireID}, *Fingerprints), Feature));
  }
}

TEST(SBFAgaveConformanceProtocol, RejectsCollidingWireFeatureFingerprints) {
  constexpr uint64_t kCollidingWireID = kXXH64EmptyHash;
  auto Fingerprints = validateFeatureFingerprints(
      {{RuntimeFeature::InstructionDataPointer, kCollidingWireID},
       {RuntimeFeature::DirectAccountPointers, kCollidingWireID}});
  ASSERT_FALSE(static_cast<bool>(Fingerprints));
  EXPECT_NE(llvm::toString(Fingerprints.takeError()).find("collide"),
            std::string::npos);
}

TEST(SBFAgaveConformanceProtocol, MatchesCanonicalXXH64Vectors) {
  EXPECT_EQ(xxh64({}), kXXH64EmptyHash);
  const llvm::ArrayRef<uint8_t> HelloBytes(
      reinterpret_cast<const uint8_t *>(kAgaveXXH64HelloInput.data()),
      kAgaveXXH64HelloInput.size());
  EXPECT_EQ(xxh64(HelloBytes), kXXH64HelloWorldHash);
  EXPECT_EQ(xxh64(HelloBytes, kAgaveXXH64TestSeed), kXXH64SeededHelloWorldHash);

  std::vector<uint8_t> ByteRange(kAgaveXXH64TestRangeSize);
  for (size_t Index = 0; Index < ByteRange.size(); ++Index)
    ByteRange[Index] = static_cast<uint8_t>(Index);
  EXPECT_EQ(xxh64(ByteRange), kXXH64ByteRangeHash);
}

TEST(SBFAgaveConformance, MatchesPinnedSolCompatELFLoaderV1Corpus) {
  const std::optional<std::filesystem::path> Root = configuredRoot();
  if (!Root) {
    if (conformanceRequired()) {
      ADD_FAILURE() << "required Agave conformance root is missing: set "
                    << kAgaveRootEnvironment.str();
      return;
    }
    GTEST_SKIP() << "set " << kAgaveRootEnvironment.str()
                 << " to run the pinned sol_compat_elf_loader_v1 corpus";
  }

  const char *Revision = std::getenv(kAgaveRevisionEnvironment.data());
  ASSERT_NE(Revision, nullptr)
      << "configured corpus must set " << kAgaveRevisionEnvironment.str();
  ASSERT_EQ(llvm::StringRef(Revision), kFiredancerTestVectorsRevision)
      << "fixture corpus is not the audited Firedancer test-vectors revision";
  if (llvm::Error Error = verifyCorpusIdentity(*Root)) {
    ADD_FAILURE() << llvm::toString(std::move(Error));
    return;
  }

  auto Paths = fixturePaths(*Root);
  ASSERT_TRUE(static_cast<bool>(Paths))
      << (Paths ? std::string() : llvm::toString(Paths.takeError()));
  auto Fingerprints = featureFingerprints();
  ASSERT_TRUE(static_cast<bool>(Fingerprints))
      << (Fingerprints ? std::string()
                       : llvm::toString(Fingerprints.takeError()));

  size_t AcceptedFixtureCount = 0;
  size_t RejectedFixtureCount = 0;
  llvm::DenseSet<uint32_t> OutputCodes;
  for (const std::filesystem::path &Path : *Paths) {
    SCOPED_TRACE(Path.filename().string());
    auto Fixture = readFixture(Path);
    ASSERT_TRUE(static_cast<bool>(Fixture))
        << (Fixture ? std::string() : llvm::toString(Fixture.takeError()));
    const RuntimeFeature Active =
        activeRuntimeFeatures(Fixture->Input.FeatureIDs, *Fingerprints);
    auto Observed = observeNeverD(*Fixture, Active);
    ASSERT_TRUE(static_cast<bool>(Observed))
        << (Observed ? std::string() : llvm::toString(Observed.takeError()));
    const bool ExpectedAccepted = Fixture->Output.ErrorCode == 0;
    OutputCodes.insert(Fixture->Output.ErrorCode);
    if (ExpectedAccepted)
      ++AcceptedFixtureCount;
    else
      ++RejectedFixtureCount;
    EXPECT_EQ(Observed->Accepted, ExpectedAccepted) << Observed->Diagnostic;
    if (!ExpectedAccepted || !Observed->Accepted)
      continue;
    EXPECT_EQ(Observed->EntryPC, Fixture->Output.EntryPC);
    EXPECT_EQ(Observed->TextOffset, Fixture->Output.TextOffset);
    EXPECT_EQ(Observed->TextCount, Fixture->Output.TextCount);
    EXPECT_EQ(Observed->RodataHash, Fixture->Output.RodataHash);
    EXPECT_EQ(Observed->CallDestinationsHash,
              Fixture->Output.CallDestinationsHash);
  }
  EXPECT_EQ(AcceptedFixtureCount, kAgaveAcceptedFixtureCount);
  EXPECT_EQ(RejectedFixtureCount, kAgaveRejectedFixtureCount);
  EXPECT_EQ(AcceptedFixtureCount + RejectedFixtureCount, kAgaveFixtureCount);
  EXPECT_EQ(OutputCodes.size(), kAgaveDistinctOutputCodeCount);
}

} // namespace
} // namespace neverd::sbf
