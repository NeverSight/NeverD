//===- SBFAnalyzerTests.cpp - Solana SBF staged analysis tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFAnalyzerDetail.h"
#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/analysis/SBFDataflow.h"
#include "neverd/sbf/analysis/SBFFunctionBody.h"
#include "neverd/sbf/analysis/SBFStructuredCFG.h"
#include "neverd/sbf/emit/SBFCEmitter.h"
#include "neverd/sbf/emit/SBFRustEmitter.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/solana/SBFSolanaRecovery.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace neverd::sbf {
namespace {

using EncodedInstruction = std::array<uint8_t, kInstructionSize>;

inline constexpr llvm::StringLiteral kProtocolCeilingScaleEnvironment =
    "NEVERD_RUN_SBF_PROTOCOL_CEILING_SCALE";

EncodedInstruction encode(Opcode ID, uint8_t Dst = 0, uint8_t Src = 0,
                          int16_t Offset = 0, int32_t Immediate = 0) {
  EncodedInstruction Bytes{};
  const OpcodeInfo *Info = getOpcodeInfo(ID);
  EXPECT_NE(Info, nullptr);
  if (!Info)
    return Bytes;
  Bytes[kOpcodeOffset] = Info->Encoding;
  Bytes[kRegisterByteOffset] =
      static_cast<uint8_t>((Src << kRegisterEncodingBits) | Dst);
  llvm::support::endian::write16le(Bytes.data() + kBranchOffsetOffset,
                                   static_cast<uint16_t>(Offset));
  llvm::support::endian::write32le(Bytes.data() + kImmediateOffset,
                                   static_cast<uint32_t>(Immediate));
  return Bytes;
}

BinaryImage makeImage(Version TheVersion,
                      llvm::ArrayRef<EncodedInstruction> Instructions,
                      size_t EntrySlot = 0) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart + EntrySlot * kInstructionSize;
  for (const EncodedInstruction &Instruction : Instructions)
    Image.Raw.insert(Image.Raw.end(), Instruction.begin(), Instruction.end());

  Section Text;
  Text.Name = kTextSectionName.str();
  Text.VA = kBytecodeStart;
  Text.Size = Image.Raw.size();
  Text.FileSz = Image.Raw.size();
  Text.Flags = SegmentFlags::Executable;
  Text.Alignment = kInstructionSize;
  Text.Data = Image.Raw;
  Image.Sections.push_back(std::move(Text));

  Metadata Meta;
  Meta.Machine = kELFMachineBPF;
  Meta.ELFFlags = static_cast<uint32_t>(TheVersion);
  Meta.Version = TheVersion;
  Meta.StrictLayout = versionHasFeature(TheVersion, VersionFeature::StrictELF);
  Meta.TextFile = {0, Image.Raw.size()};
  Meta.TextVM = {kBytecodeStart, Image.Raw.size()};
  Image.SBF = Meta;
  return Image;
}

BinaryImage makeImage(Version TheVersion,
                      std::initializer_list<EncodedInstruction> Instructions,
                      size_t EntrySlot = 0) {
  return makeImage(TheVersion,
                   llvm::ArrayRef<EncodedInstruction>(Instructions.begin(),
                                                      Instructions.size()),
                   EntrySlot);
}

ELFRelocationProvenance
exactELFSymbol(uint64_t Value, uint16_t SectionIndex, uint8_t Type,
               std::optional<llvm::StringRef> Name = std::nullopt) {
  ELFRelocationProvenance Result;
  Result.Source = ELFRelocationSource::ProgramDynamicTable;
  ELFRelocationSymbol Symbol;
  Symbol.SectionIndex = SectionIndex;
  Symbol.Binding = llvm::ELF::STB_GLOBAL;
  Symbol.Type = Type;
  Symbol.Value = Value;
  if (Name)
    Symbol.Name = Name->str();
  Result.Symbol = std::move(Symbol);
  return Result;
}

TEST(SBFAnalyzer, ResolvesTheVMConfigurationFromTheRuntimeProfile) {
  auto Mainnet = analyze(makeImage(Version::V3, {encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Mainnet))
      << llvm::toString(Mainnet.takeError());
  EXPECT_EQ(Mainnet->EnvironmentOrigin, RuntimeEnvironmentOrigin::Agave);
  EXPECT_EQ(Mainnet->VersionPolicy, RuntimeVersionPolicy::ChainProfile);
  EXPECT_EQ(Mainnet->MinimumRuntimeVersion, Version::V0);
  EXPECT_EQ(Mainnet->MaximumRuntimeVersion, Version::V3);
  ASSERT_TRUE(Mainnet->Profile.has_value());
  EXPECT_EQ(Mainnet->RuntimeAccountABI, AccountABI::V1);
  EXPECT_TRUE(hasFeature(Mainnet->ActiveRuntimeFeatures,
                         RuntimeFeature::InstructionDataPointer));
  EXPECT_TRUE(hasFeature(Mainnet->ActiveRuntimeFeatures,
                         RuntimeFeature::SyscallParameterAddressRestrictions));
  EXPECT_FALSE(hasFeature(Mainnet->ActiveRuntimeFeatures,
                          RuntimeFeature::AccountDataDirectMapping));
  EXPECT_TRUE(Mainnet->Config.EnableStackFrameGaps);
  EXPECT_TRUE(Mainnet->Config.AlignedMemoryMapping);
  EXPECT_FALSE(Mainnet->Config.OptimizeRodata);
  EXPECT_FALSE(Mainnet->Config.RejectBrokenELFs);

  AnalyzeOptions Deployment;
  Deployment.Profile.OnCluster = Cluster::Testnet;
  Deployment.Profile.Purpose = RuntimePurpose::Deployment;
  auto TestnetDeployment =
      analyze(makeImage(Version::V3, {encode(Opcode::EXIT)}), Deployment);
  ASSERT_TRUE(static_cast<bool>(TestnetDeployment))
      << llvm::toString(TestnetDeployment.takeError());
  EXPECT_EQ(TestnetDeployment->EnvironmentOrigin,
            RuntimeEnvironmentOrigin::Agave);
  EXPECT_FALSE(TestnetDeployment->Config.EnableStackFrameGaps);
  EXPECT_FALSE(TestnetDeployment->Config.AlignedMemoryMapping);
  EXPECT_FALSE(TestnetDeployment->Config.OptimizeRodata);
  EXPECT_TRUE(TestnetDeployment->Config.RejectBrokenELFs);
  EXPECT_TRUE(hasFeature(TestnetDeployment->ActiveRuntimeFeatures,
                         RuntimeFeature::SyscallParameterAddressRestrictions));
  EXPECT_TRUE(hasFeature(TestnetDeployment->ActiveRuntimeFeatures,
                         RuntimeFeature::AccountDataDirectMapping));
}

TEST(SBFAnalyzer, SeparatesChainAndOfflineVersionPolicies) {
  AnalyzeOptions DisabledV0;
  DisabledV0.Profile.Forced = RuntimeFeature::DisableSBPFV0Execution;
  DisabledV0.Profile.Suppressed = RuntimeFeature::ReenableSBPFV0Execution;
  auto Legacy =
      analyze(makeImage(Version::V2, {encode(Opcode::EXIT)}), DisabledV0);
  ASSERT_FALSE(static_cast<bool>(Legacy));
  EXPECT_NE(llvm::toString(Legacy.takeError()).find("enabled version range"),
            std::string::npos);

  auto Upstream = analyze(makeImage(Version::V4, {encode(Opcode::EXIT)}));
  ASSERT_FALSE(static_cast<bool>(Upstream));
  llvm::consumeError(Upstream.takeError());

  AnalyzeOptions ExplicitV4;
  ExplicitV4.VersionOverride = Version::V4;
  auto Offline =
      analyze(makeImage(Version::V4, {encode(Opcode::EXIT)}), ExplicitV4);
  ASSERT_TRUE(static_cast<bool>(Offline))
      << llvm::toString(Offline.takeError());
  EXPECT_EQ(Offline->EnvironmentOrigin, RuntimeEnvironmentOrigin::Agave);
  EXPECT_EQ(Offline->VersionPolicy, RuntimeVersionPolicy::UpstreamToolchain);
  EXPECT_EQ(Offline->MinimumRuntimeVersion, Version::V0);
  EXPECT_EQ(Offline->MaximumRuntimeVersion, Version::V4);
  EXPECT_EQ(Offline->Low.TheVersion, Version::V4);

  ExplicitV4.RuntimeVersions = RuntimeVersionPolicy::ChainProfile;
  auto ForcedChain =
      analyze(makeImage(Version::V4, {encode(Opcode::EXIT)}), ExplicitV4);
  ASSERT_FALSE(static_cast<bool>(ForcedChain));
  EXPECT_NE(
      llvm::toString(ForcedChain.takeError()).find("enabled version range"),
      std::string::npos);

  AnalyzeOptions Expert;
  SBFVMConfig ExpertConfig;
  ExpertConfig.EnableStackFrameGaps = false;
  ExpertConfig.OptimizeRodata = true;
  ExpertConfig.AlignedMemoryMapping = true;
  ExpertConfig.RejectBrokenELFs = true;
  Expert.ExpertEnvironment =
      ExpertRuntimeEnvironmentOverride{Version::V0, Version::V4, ExpertConfig};
  auto Accepted =
      analyze(makeImage(Version::V4, {encode(Opcode::EXIT)}), Expert);
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_EQ(Accepted->EnvironmentOrigin, RuntimeEnvironmentOrigin::Custom);
  EXPECT_EQ(Accepted->VersionPolicy, RuntimeVersionPolicy::ExpertOverride);
  EXPECT_FALSE(Accepted->Profile.has_value());
  EXPECT_EQ(Accepted->ActiveRuntimeFeatures, RuntimeFeature::None);
  EXPECT_EQ(Accepted->RuntimeAccountABI, AccountABI::V1);
  EXPECT_EQ(Accepted->Config.EnableStackFrameGaps,
            ExpertConfig.EnableStackFrameGaps);
  EXPECT_EQ(Accepted->Config.OptimizeRodata, ExpertConfig.OptimizeRodata);
  EXPECT_EQ(Accepted->Config.AlignedMemoryMapping,
            ExpertConfig.AlignedMemoryMapping);
  EXPECT_EQ(Accepted->Config.RejectBrokenELFs, ExpertConfig.RejectBrokenELFs);

  Expert.RuntimeVersions = RuntimeVersionPolicy::ChainProfile;
  auto ConflictingPolicies =
      analyze(makeImage(Version::V4, {encode(Opcode::EXIT)}), Expert);
  ASSERT_FALSE(static_cast<bool>(ConflictingPolicies));
  EXPECT_NE(llvm::toString(ConflictingPolicies.takeError())
                .find("complete expert environment"),
            std::string::npos);
}

TEST(SBFAnalyzer, UsesOneResolvedAuthorityForEveryExpertRuntimeFact) {
  AnalyzeOptions Options;
  // These named-profile facts conflict deliberately with the complete custom
  // environment below. They must not leak into any downstream consumer.
  Options.Profile.Forced = RuntimeFeature::InstructionDataPointer;
  Options.Profile.OwningLoader = Loader::V3;
  Options.Verification = VerificationPolicy::RequisiteAndLocalPreflight;

  ExpertRuntimeEnvironmentOverride Override{
      Version::V0, Version::V4, SBFVMConfig{}, {0}};
  Override.ActiveRuntimeFeatures = RuntimeFeature::None;
  Override.InputABI = AccountABI::V0;
  Options.ExpertEnvironment = Override;

  auto Program = analyze(
      makeImage(Version::V3,
                {encode(Opcode::MOV64_REG, 0, kInstructionDataRegister),
                 encode(Opcode::CALL_IMM, 0, 0, 0, 0), encode(Opcode::EXIT)}),
      Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_FALSE(Program->Profile.has_value());
  EXPECT_EQ(Program->EnvironmentOrigin, RuntimeEnvironmentOrigin::Custom);
  EXPECT_EQ(Program->VersionPolicy, RuntimeVersionPolicy::ExpertOverride);
  EXPECT_EQ(Program->RuntimeAccountABI, AccountABI::V0);
  EXPECT_EQ(Program->ActiveRuntimeFeatures, RuntimeFeature::None);
  EXPECT_TRUE(Program->isSyscallRegistered(0));
  EXPECT_TRUE(Program->Verification.accepted())
      << "the verifier must use the same exact custom registry as the loader";
  ASSERT_FALSE(Program->Med.Blocks.empty());
  EXPECT_EQ(
      Program->Med.Blocks.front().Inputs[kInstructionDataRegister].ValueKind,
      RegisterValue::Kind::Constant);
  EXPECT_EQ(Program->Med.Blocks.front().Inputs[kInstructionDataRegister].Value,
            0u);
}

TEST(SBFAnalyzer, CombinesLDDWAndRetainsTheContinuationSlot) {
  EncodedInstruction Low = encode(Opcode::LDDW, 3, 0, 0, 0x55667788);
  EncodedInstruction High{};
  llvm::support::endian::write32le(High.data() + kImmediateOffset, 0x11223344);
  auto Program =
      analyze(makeImage(Version::V0, {Low, High, encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.Instructions.size(), 3u);
  EXPECT_EQ(Program->Low.Instructions[0].Immediate, 0x1122334455667788ULL);
  EXPECT_EQ(Program->Low.Instructions[0].SlotWidth, kLDDWSlotCount);
  EXPECT_TRUE(Program->Low.Instructions[1].IsContinuation);
  ASSERT_EQ(Program->Med.Instructions.size(), 2u);
  EXPECT_EQ(Program->Med.Instructions[0].SlotWidth, kLDDWSlotCount);
  EXPECT_EQ(Program->Med.Instructions[0].Semantics.Immediate,
            ImmediateExtension::Full64);
}

TEST(SBFAnalyzer, EntrypointInsideLDDWFollowsTheOfficialRuntimeFaultPath) {
  auto Program = analyze(makeImage(
      Version::V3,
      {encode(Opcode::LDDW), EncodedInstruction{}, encode(Opcode::EXIT)}, 1));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.EntrySlot, 1u);
  ASSERT_TRUE(Program->Low.Instructions[1].IsContinuation);

  const auto EntryBlock = std::find_if(
      Program->Low.Blocks.begin(), Program->Low.Blocks.end(),
      [](const BasicBlock &Block) { return Block.StartSlot == 1; });
  ASSERT_NE(EntryBlock, Program->Low.Blocks.end());
  EXPECT_EQ(EntryBlock->EndSlot, 2u);
  EXPECT_TRUE(EntryBlock->Reachable);
  const auto FaultEdge = std::find_if(
      Program->Low.Edges.begin(), Program->Low.Edges.end(),
      [&](const CFGEdge &Edge) { return Edge.From == EntryBlock->ID; });
  ASSERT_NE(FaultEdge, Program->Low.Edges.end());
  EXPECT_EQ(FaultEdge->Kind, EdgeKind::Fault);
  EXPECT_FALSE(FaultEdge->To.has_value());

  const Function *Entrypoint = findFunction(*Program);
  ASSERT_NE(Entrypoint, nullptr);
  EXPECT_EQ(Entrypoint->EntrySlot, 1u);
}

TEST(SBFAnalyzer, RelaxedUnknownOpcodeRemainsAnExplicitTerminalFault) {
  EncodedInstruction Unknown{};
  Unknown[kOpcodeOffset] = 0xff;
  AnalyzeOptions Options;
  Options.Strict = false;

  auto Program = analyze(
      makeImage(Version::V3, {Unknown, encode(Opcode::MOV64_IMM, 0, 0, 0, 7),
                              encode(Opcode::EXIT)}),
      Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->Low.Instructions.size(), 3u);
  EXPECT_EQ(Program->Low.Instructions[0].InvalidReason,
            ValidationRule::UnknownOpcode);
  ASSERT_EQ(Program->Med.Instructions.size(), 3u);
  EXPECT_EQ(Program->Med.Instructions[0].Slot, 0u);
  EXPECT_EQ(Program->Med.Instructions[0].Op, Operation::Invalid);
  EXPECT_EQ(Program->Med.Instructions[0].InvalidReason,
            ValidationRule::UnknownOpcode);
  ASSERT_FALSE(Program->Low.Diagnostics.empty());
  EXPECT_EQ(Program->Low.Diagnostics.front().Rule,
            ValidationRule::UnknownOpcode);

  ASSERT_EQ(Program->Low.Blocks.size(), 2u);
  EXPECT_EQ(Program->Low.Blocks[0].StartSlot, 0u);
  EXPECT_EQ(Program->Low.Blocks[0].EndSlot, 1u);
  EXPECT_TRUE(Program->Low.Blocks[0].Successors.empty());
  EXPECT_FALSE(Program->Low.Blocks[1].Reachable);
  const auto InvalidEdge =
      std::find_if(Program->Low.Edges.begin(), Program->Low.Edges.end(),
                   [](const CFGEdge &Edge) { return Edge.From == 0; });
  ASSERT_NE(InvalidEdge, Program->Low.Edges.end());
  EXPECT_EQ(InvalidEdge->Kind, EdgeKind::Invalid);
  EXPECT_FALSE(InvalidEdge->To.has_value());
}

TEST(SBFAnalyzer, RelaxedMalformedLDDWDoesNotConsumeTheFollowingSlot) {
  AnalyzeOptions Options;
  Options.Strict = false;
  auto Program = analyze(
      makeImage(Version::V0, {encode(Opcode::LDDW, 0), encode(Opcode::EXIT),
                              encode(Opcode::EXIT)}),
      Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->Low.Instructions.size(), 3u);
  EXPECT_EQ(Program->Low.Instructions[0].InvalidReason,
            ValidationRule::NonZeroLDDWContinuation);
  EXPECT_EQ(Program->Low.Instructions[0].SlotWidth, 1u);
  EXPECT_FALSE(Program->Low.Instructions[1].IsContinuation);
  ASSERT_NE(Program->Low.Instructions[1].Info, nullptr);
  EXPECT_EQ(Program->Low.Instructions[1].Info->ID, Opcode::EXIT);

  ASSERT_EQ(Program->Med.Instructions.size(), 3u);
  EXPECT_EQ(Program->Med.Instructions[0].Op, Operation::Invalid);
  EXPECT_EQ(Program->Med.Instructions[1].SourceOpcode, Opcode::EXIT);
  ASSERT_EQ(Program->Low.Blocks.size(), 3u);
  EXPECT_TRUE(Program->Low.Blocks[0].Successors.empty());
  EXPECT_FALSE(Program->Low.Blocks[1].Reachable);
  EXPECT_FALSE(Program->Low.Blocks[2].Reachable);
}

TEST(SBFAnalyzer, RelaxedInvalidLDDWPreservesItsRawContinuation) {
  EncodedInstruction Continuation{};
  const BinaryImage Image =
      makeImage(Version::V0,
                {encode(Opcode::LDDW, 15), Continuation, encode(Opcode::EXIT)});
  auto Strict = analyze(Image);
  ASSERT_FALSE(static_cast<bool>(Strict));
  llvm::consumeError(Strict.takeError());

  AnalyzeOptions Options;
  Options.Strict = false;
  auto Program = analyze(Image, Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->Low.Instructions.size(), 3u);
  EXPECT_EQ(Program->Low.Instructions[0].InvalidReason,
            ValidationRule::InvalidDestinationRegister);
  EXPECT_EQ(Program->Low.Instructions[0].SlotWidth, kLDDWSlotCount);
  EXPECT_TRUE(Program->Low.Instructions[1].IsContinuation);
  EXPECT_EQ(Program->Low.Instructions[1].RawOpcode, 0u);
  EXPECT_EQ(Program->Low.Instructions[1].InvalidReason, ValidationRule::None);
  ASSERT_EQ(Program->Low.Diagnostics.size(), 1u);
  ASSERT_EQ(Program->Med.Instructions.size(), 2u);
  EXPECT_EQ(Program->Med.Instructions[0].Op, Operation::Invalid);
  EXPECT_EQ(Program->Med.Instructions[0].SlotWidth, kLDDWSlotCount);
  EXPECT_EQ(Program->Med.Instructions[1].SourceOpcode, Opcode::EXIT);
}

TEST(SBFAnalyzer, RelaxedInvalidSourceRegisterIsQuarantinedBeforeMedIR) {
  AnalyzeOptions Options;
  Options.Strict = false;
  auto Program =
      analyze(makeImage(Version::V3, {encode(Opcode::MOV64_REG, 0, 15),
                                      encode(Opcode::EXIT)}),
              Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->Low.Instructions.size(), 2u);
  EXPECT_EQ(Program->Low.Instructions[0].Src, 15u);
  EXPECT_EQ(Program->Low.Instructions[0].InvalidReason,
            ValidationRule::InvalidSourceRegister);
  EXPECT_EQ(
      getValidationRuleInfo(Program->Low.Instructions[0].InvalidReason).Fault,
      FaultCode::InvalidRegister);
  ASSERT_EQ(Program->Med.Instructions.size(), 2u);
  EXPECT_EQ(Program->Med.Instructions[0].Op, Operation::Invalid);
  EXPECT_EQ(Program->Med.Instructions[0].Src, 0u);
  EXPECT_EQ(Program->Med.Instructions[0].Dst, 0u);
  EXPECT_TRUE(Program->Low.Blocks[0].Successors.empty());
}

TEST(SBFAnalyzer, InvalidCallXDoesNotLeakIntoTheRecoveredCallGraph) {
  AnalyzeOptions Options;
  Options.Strict = false;
  auto Program =
      analyze(makeImage(Version::V0, {encode(Opcode::CALL_REG, 0, 15, 0, 0),
                                      encode(Opcode::EXIT)}),
              Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->Low.Instructions.size(), 2u);
  EXPECT_EQ(Program->Low.Instructions[0].InvalidReason,
            ValidationRule::InvalidSourceRegister);
  EXPECT_EQ(Program->Low.Instructions[0].Call, CallKind::None);
  EXPECT_TRUE(Program->High.Calls.empty());
  EXPECT_TRUE(Program->High.Syscalls.empty());
}

TEST(SBFAnalyzer, StaticCallMayTargetAnLDDWContinuation) {
  EncodedInstruction Continuation{};
  auto Program =
      analyze(makeImage(Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0, 1),
                                      encode(Opcode::LDDW, 0), Continuation,
                                      encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->Low.Instructions.size(), 4u);
  EXPECT_TRUE(Program->Low.Instructions[2].IsContinuation);
  EXPECT_EQ(Program->Low.Instructions[0].InvalidReason, ValidationRule::None);
  EXPECT_EQ(Program->Low.Instructions[0].Call, CallKind::Internal);
  EXPECT_EQ(Program->Low.Instructions[0].CallTarget, 2u);
  ASSERT_EQ(Program->High.Calls.size(), 1u);
  EXPECT_EQ(Program->High.Calls[0].TargetSlot, 2u);
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_EQ(Program->High.Functions[0].EntrySlot, 0u);

  ASSERT_EQ(Program->Low.Blocks.size(), 4u);
  for (size_t ID = 0; ID < Program->Low.Blocks.size(); ++ID) {
    EXPECT_EQ(Program->Low.Blocks[ID].ID, ID);
    EXPECT_EQ(Program->Low.Blocks[ID].StartSlot, ID);
    EXPECT_EQ(Program->Low.Blocks[ID].EndSlot, ID + 1);
  }
  auto HasEdge = [&](size_t From, std::optional<size_t> To, EdgeKind Kind) {
    return std::any_of(Program->Low.Edges.begin(), Program->Low.Edges.end(),
                       [&](const CFGEdge &Edge) {
                         return Edge.From == From && Edge.To == To &&
                                Edge.Kind == Kind;
                       });
  };
  EXPECT_TRUE(HasEdge(0, 2u, EdgeKind::Call));
  EXPECT_TRUE(HasEdge(0, 1u, EdgeKind::Fallthrough));
  EXPECT_TRUE(HasEdge(1, 3u, EdgeKind::Fallthrough));
  EXPECT_TRUE(HasEdge(2, std::nullopt, EdgeKind::Fault));
}

TEST(SBFAnalyzer, UnsupportedStaticCallDispatchRemainsAValidInstruction) {
  auto Program = analyze(makeImage(
      Version::V3, {encode(Opcode::CALL_IMM, 0, 2), encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->Low.Instructions.size(), 2u);
  EXPECT_EQ(Program->Low.Instructions[0].InvalidReason, ValidationRule::None);
  EXPECT_EQ(Program->Low.Instructions[0].Call, CallKind::Unsupported);
  ASSERT_EQ(Program->Low.Diagnostics.size(), 1u);
  EXPECT_EQ(Program->Low.Diagnostics[0].Severity, DiagnosticSeverity::Warning);
  ASSERT_FALSE(Program->Low.Edges.empty());
  EXPECT_EQ(Program->Low.Edges[0].Kind, EdgeKind::Fault);
  EXPECT_FALSE(Program->Low.Edges[0].To.has_value());
}

TEST(SBFAnalyzer, RelaxedInvalidDestinationsRetainTheirVerifierReason) {
  struct Case {
    uint8_t Dst;
    ValidationRule Rule;
  };
  const Case Cases[] = {
      {kFramePointerRegister, ValidationRule::FramePointerWrite},
      {15, ValidationRule::InvalidDestinationRegister},
  };

  for (const Case &Test : Cases) {
    SCOPED_TRACE(unsigned(Test.Dst));
    AnalyzeOptions Options;
    Options.Strict = false;
    auto Program =
        analyze(makeImage(Version::V3, {encode(Opcode::MOV64_IMM, Test.Dst),
                                        encode(Opcode::EXIT)}),
                Options);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    ASSERT_EQ(Program->Low.Instructions.size(), 2u);
    EXPECT_EQ(Program->Low.Instructions[0].InvalidReason, Test.Rule);
    EXPECT_EQ(getValidationRuleInfo(Test.Rule).Fault,
              FaultCode::InvalidRegister);
    ASSERT_EQ(Program->Med.Instructions.size(), 2u);
    EXPECT_EQ(Program->Med.Instructions[0].Op, Operation::Invalid);
    EXPECT_EQ(Program->Med.Instructions[0].Dst, 0u);
  }
}

TEST(SBFAnalyzer, RelaxedValidationUsesOfficialPerInstructionPrecedence) {
  struct Case {
    Version TheVersion;
    EncodedInstruction Instruction;
    ValidationRule Rule;
    FaultCode Fault;
  };
  const Case Cases[] = {
      {Version::V2, encode(Opcode::UDIV64_IMM, 15, 15, 0, 0),
       ValidationRule::ImmediateDivisionByZero, FaultCode::DivideByZero},
      {Version::V3, encode(Opcode::LSH64_IMM, 15, 15, 0, 64),
       ValidationRule::ImmediateShiftOutOfRange, FaultCode::InvalidInstruction},
      {Version::V3, encode(Opcode::BE, 15, 15, 0, 24),
       ValidationRule::InvalidEndianImmediate, FaultCode::InvalidInstruction},
      {Version::V2, encode(Opcode::ADD64_IMM, kFramePointerRegister, 15, 0, 1),
       ValidationRule::MisalignedFrameAdjustment,
       FaultCode::InvalidInstruction},
      {Version::V3,
       encode(Opcode::JA, 15, 15, std::numeric_limits<int16_t>::max()),
       ValidationRule::BranchOutOfRange, FaultCode::InvalidBranch},
      {Version::V0, encode(Opcode::CALL_REG, 15, 15, 0, -1),
       ValidationRule::InvalidCallXRegister, FaultCode::InvalidRegister},
  };

  for (const Case &Test : Cases) {
    SCOPED_TRACE(getValidationRuleInfo(Test.Rule).StableID.str());
    const BinaryImage Image =
        makeImage(Test.TheVersion, {Test.Instruction, encode(Opcode::EXIT)});
    auto Strict = analyze(Image);
    ASSERT_FALSE(static_cast<bool>(Strict));
    llvm::consumeError(Strict.takeError());

    AnalyzeOptions Options;
    Options.Strict = false;
    auto Relaxed = analyze(Image, Options);
    ASSERT_TRUE(static_cast<bool>(Relaxed))
        << llvm::toString(Relaxed.takeError());
    ASSERT_EQ(Relaxed->Low.Instructions.size(), 2u);
    EXPECT_EQ(Relaxed->Low.Instructions[0].InvalidReason, Test.Rule);
    EXPECT_EQ(getValidationRuleInfo(Test.Rule).Fault, Test.Fault);
    ASSERT_EQ(Relaxed->Med.Instructions.size(), 2u);
    EXPECT_EQ(Relaxed->Med.Instructions[0].Op, Operation::Invalid);
    EXPECT_EQ(Relaxed->Med.Instructions[0].InvalidReason, Test.Rule);
    EXPECT_EQ(Relaxed->Med.Instructions[0].Dst, 0u);
    EXPECT_EQ(Relaxed->Med.Instructions[0].Src, 0u);
  }
}

TEST(SBFAnalyzer, LDDWRegisterDiagnosticsUseTheOfficialContinuationSlot) {
  EncodedInstruction Load = encode(Opcode::LDDW, 15, 15);
  const BinaryImage Image = makeImage(
      Version::V0, {Load, EncodedInstruction{}, encode(Opcode::EXIT)});
  auto Strict = analyze(Image);
  ASSERT_FALSE(static_cast<bool>(Strict));
  EXPECT_NE(llvm::toString(Strict.takeError()).find("instruction 1"),
            std::string::npos);

  AnalyzeOptions Options;
  Options.Strict = false;
  auto Relaxed = analyze(Image, Options);
  ASSERT_TRUE(static_cast<bool>(Relaxed))
      << llvm::toString(Relaxed.takeError());
  EXPECT_EQ(Relaxed->Low.Instructions[0].InvalidReason,
            ValidationRule::InvalidSourceRegister);
  ASSERT_FALSE(Relaxed->Low.Diagnostics.empty());
  EXPECT_EQ(Relaxed->Low.Diagnostics.front().Slot, 1u);
  EXPECT_EQ(Relaxed->Low.Diagnostics.front().Address,
            kBytecodeStart + kInstructionSize);
}

TEST(SBFAnalyzer, RelaxedBranchToLDDWContinuationHasNoTargetEdge) {
  EncodedInstruction Continuation{};
  AnalyzeOptions Options;
  Options.Strict = false;
  auto Program =
      analyze(makeImage(Version::V0,
                        {encode(Opcode::LDDW, 0), Continuation,
                         encode(Opcode::JA, 0, 0, -2), encode(Opcode::EXIT)}),
              Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.Instructions.size(), 4u);
  EXPECT_TRUE(Program->Low.Instructions[1].IsContinuation);
  EXPECT_EQ(Program->Low.Instructions[2].InvalidReason,
            ValidationRule::BranchToLDDWContinuation);
  EXPECT_FALSE(Program->Low.Instructions[2].BranchTarget.has_value());

  const auto Block =
      std::find_if(Program->Low.Blocks.begin(), Program->Low.Blocks.end(),
                   [](const BasicBlock &Candidate) {
                     return Candidate.StartSlot <= 2 && 2 < Candidate.EndSlot;
                   });
  ASSERT_NE(Block, Program->Low.Blocks.end());
  EXPECT_TRUE(Block->Successors.empty());
  const auto Edge = std::find_if(
      Program->Low.Edges.begin(), Program->Low.Edges.end(),
      [&](const CFGEdge &Candidate) { return Candidate.From == Block->ID; });
  ASSERT_NE(Edge, Program->Low.Edges.end());
  EXPECT_EQ(Edge->Kind, EdgeKind::Invalid);
  EXPECT_FALSE(Edge->To.has_value());
}

TEST(SBFAnalyzer, DoesNotReadAnInternalCallAsAChoiceBetweenTwoPaths) {
  // A call has two successors, the callee and the instruction after it, and
  // neither is an alternative to the other: both run. Counting successors
  // reads this as a two-armed region and swallows the whole callee into one
  // arm.
  const auto Instructions = {
      encode(Opcode::CALL_IMM, 0, 1, 0, 2), // call the block two slots ahead
      encode(Opcode::EXIT),
      encode(Opcode::MOV64_IMM, 0, 0, 0, 7),
      encode(Opcode::EXIT),
  };
  auto Program = analyze(makeImage(Version::V3, Instructions));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  const bool CallsInternally = std::any_of(
      Program->Low.Edges.begin(), Program->Low.Edges.end(),
      [](const CFGEdge &Edge) { return Edge.Kind == EdgeKind::Call; });
  ASSERT_TRUE(CallsInternally) << "the fixture must exercise an internal call";
  for (const Region &Region : Program->High.Regions)
    EXPECT_NE(Region.Kind, RegionKind::If) << "block_" << Region.HeaderBlock;
}

TEST(SBFAnalyzer, DoesNotReadRecursiveCallEdgesAsNaturalLoops) {
  auto Program =
      analyze(makeImage(Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0, -1),
                                      encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_TRUE(std::any_of(
      Program->Low.Edges.begin(), Program->Low.Edges.end(),
      [](const CFGEdge &Edge) { return Edge.Kind == EdgeKind::Call; }));
  EXPECT_TRUE(std::none_of(
      Program->High.Regions.begin(), Program->High.Regions.end(),
      [](const Region &Region) { return Region.Kind == RegionKind::Loop; }));
}

TEST(SBFAnalyzer, RecoversNaturalLoopsInsideNonEntryFunctions) {
  auto Program = analyze(makeImage(
      Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
                    encode(Opcode::JA, 0, 0, -1, 0)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 2u);

  const size_t CalleeIndex = 1;
  const auto Loop =
      std::find_if(Program->High.Regions.begin(), Program->High.Regions.end(),
                   [&](const Region &Candidate) {
                     return Candidate.Kind == RegionKind::Loop &&
                            Candidate.FunctionIndex == CalleeIndex;
                   });
  ASSERT_NE(Loop, Program->High.Regions.end());
  EXPECT_EQ(Program->High.functionForBlock(Loop->HeaderBlock),
            &Program->High.Functions[CalleeIndex]);
}

TEST(SBFAnalyzer, MergesMultipleBackedgesIntoOneNaturalLoop) {
  auto Program =
      analyze(makeImage(Version::V3, {encode(Opcode::JEQ64_IMM, 0, 0, 1, 0),
                                      encode(Opcode::JA, 0, 0, -2, 0),
                                      encode(Opcode::JA, 0, 0, -3, 0)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  const size_t LoopCount =
      std::count_if(Program->High.Regions.begin(), Program->High.Regions.end(),
                    [](const Region &Candidate) {
                      return Candidate.Kind == RegionKind::Loop;
                    });
  ASSERT_EQ(LoopCount, 1u);
  const Region &Loop =
      *std::find_if(Program->High.Regions.begin(), Program->High.Regions.end(),
                    [](const Region &Candidate) {
                      return Candidate.Kind == RegionKind::Loop;
                    });
  EXPECT_EQ(Loop.BlockCount, 3u);
  EXPECT_TRUE(Loop.Blocks.empty());
  EXPECT_EQ(Program->High.latches(Loop).size(), 2u);
  for (const BasicBlock &Block : Program->Low.Blocks)
    EXPECT_TRUE(Program->High.loopContains(Loop, Block.ID));
}

TEST(SBFAnalyzer, StillRecognizesARealConditionalBranchAsAChoice) {
  const auto Instructions = {
      encode(Opcode::JEQ64_IMM, 1, 0, 1, 0),
      encode(Opcode::MOV64_IMM, 0, 0, 0, 3),
      encode(Opcode::EXIT),
  };
  auto Program = analyze(makeImage(Version::V3, Instructions));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const bool HasIf = std::any_of(
      Program->High.Regions.begin(), Program->High.Regions.end(),
      [](const Region &Region) { return Region.Kind == RegionKind::If; });
  EXPECT_TRUE(HasIf);
}

TEST(SBFAnalyzer, RefusesAFunctionSymbolPointingIntoAWideLoad) {
  EncodedInstruction Low = encode(Opcode::LDDW, 3, 0, 0, 0x55667788);
  EncodedInstruction High{};
  llvm::support::endian::write32le(High.data() + kImmediateOffset, 0x11223344);
  BinaryImage Image = makeImage(Version::V3, {Low, High, encode(Opcode::EXIT)});
  // Nothing rejects this at link time, but a function starting here begins in
  // the middle of an instruction: everything decoded after it is four bytes
  // out of phase, and the recovered body is fiction.
  Image.addSymbol("halfway", kBytecodeStart + kInstructionSize,
                  kInstructionSize, true);

  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  for (const Function &Function : Program->High.Functions)
    EXPECT_NE(Function.Name, "halfway");

  const bool Reported =
      std::any_of(Program->Low.Diagnostics.begin(),
                  Program->Low.Diagnostics.end(), [](const Diagnostic &Note) {
                    return Note.Message.find("halfway") != std::string::npos &&
                           Note.Message.find("wide load") != std::string::npos;
                  });
  EXPECT_TRUE(Reported) << "dropping the symbol silently hides a bad input";
}

TEST(SBFAnalyzer, FunctionSymbolsSplitCFGAndStopThePrecedingFunction) {
  constexpr size_t InnerEntrySlot = 2;
  BinaryImage Image =
      makeImage(Version::V3,
                {encode(Opcode::MOV64_IMM, 0, 0, 0, 1),
                 encode(Opcode::MOV64_IMM, 0, 0, 0, 2),
                 encode(Opcode::MOV64_IMM, 0, 0, 0, 3), encode(Opcode::EXIT)});
  Image.addSymbol("inner", kBytecodeStart + InnerEntrySlot * kInstructionSize,
                  2 * kInstructionSize, true);

  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  const auto InnerBlock =
      std::find_if(Program->Low.Blocks.begin(), Program->Low.Blocks.end(),
                   [](const BasicBlock &Block) {
                     return Block.StartSlot == InnerEntrySlot;
                   });
  ASSERT_NE(InnerBlock, Program->Low.Blocks.end())
      << "every trusted function entry must begin a basic block";

  const Function *Entry = findFunction(*Program);
  const Function *Inner = findFunction(*Program, "inner");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Inner, nullptr);
  EXPECT_EQ(Inner->EntrySlot, InnerEntrySlot);
  const llvm::ArrayRef<size_t> InnerBlocks = Program->High.ownedBlocks(*Inner);
  const llvm::ArrayRef<size_t> EntryBlocks = Program->High.ownedBlocks(*Entry);
  EXPECT_NE(std::find(InnerBlocks.begin(), InnerBlocks.end(), InnerBlock->ID),
            InnerBlocks.end());
  EXPECT_EQ(std::find(EntryBlocks.begin(), EntryBlocks.end(), InnerBlock->ID),
            EntryBlocks.end())
      << "function traversal must stop at another function entry";
}

TEST(SBFAnalyzer, StoresTenThousandFunctionBodiesInLinearSpace) {
  constexpr size_t FunctionCount = 10'000;
  std::vector<EncodedInstruction> Instructions(
      FunctionCount, encode(Opcode::MOV64_IMM, 0, 0, 0, 1));
  Instructions.back() = encode(Opcode::EXIT);
  BinaryImage Image = makeImage(Version::V3, Instructions);
  for (size_t Slot = 0; Slot < FunctionCount; ++Slot)
    Image.addSymbol("function_" + std::to_string(Slot),
                    kBytecodeStart + Slot * kInstructionSize, kInstructionSize,
                    true);

  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), FunctionCount);
  ASSERT_EQ(Program->Low.Blocks.size(), FunctionCount);
  ASSERT_EQ(Program->High.BlockOwners.size(), Program->Low.Blocks.size());
  ASSERT_EQ(Program->High.FunctionBlocks.size(), Program->Low.Blocks.size())
      << "flattened function membership must be O(B), not O(F * B)";

  llvm::BitVector SeenBlocks(Program->Low.Blocks.size());
  for (size_t FunctionID = 0; FunctionID < Program->High.Functions.size();
       ++FunctionID) {
    const Function &Function = Program->High.Functions[FunctionID];
    const llvm::ArrayRef<size_t> Blocks = Program->High.ownedBlocks(Function);
    ASSERT_EQ(Blocks.size(), 1u);
    ASSERT_LT(Blocks.front(), Program->Low.Blocks.size());
    EXPECT_EQ(Program->Low.Blocks[Blocks.front()].StartSlot,
              Function.EntrySlot);
    EXPECT_FALSE(SeenBlocks.test(Blocks.front()));
    SeenBlocks.set(Blocks.front());
    EXPECT_EQ(Program->High.BlockOwners[Blocks.front()], FunctionID);
    EXPECT_EQ(Program->High.functionForBlock(Blocks.front()), &Function);
  }
  EXPECT_EQ(SeenBlocks.count(), Program->Low.Blocks.size());
}

TEST(SBFAnalyzer,
     IndexesTenThousandFunctionsSharingTenThousandBlocksByProvenanceClass) {
  constexpr size_t FunctionCount = 10'000;
  constexpr size_t SharedBlockCount = 10'000;
  constexpr size_t BlockCount = FunctionCount + SharedBlockCount;
  constexpr size_t LinearStorageAllowancePerEntity = 4;

  SBFProgram Program;
  Program.Low.Blocks.resize(BlockCount);
  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    BasicBlock &Block = Program.Low.Blocks[BlockID];
    Block.ID = BlockID;
    Block.StartSlot = BlockID;
    Block.EndSlot = BlockID + 1;
  }

  Program.High.Functions.reserve(FunctionCount);
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    Function Recovered;
    Recovered.EntrySlot = FunctionID;
    Recovered.Address = kBytecodeStart + FunctionID * kInstructionSize;
    Recovered.Name = "function_" + std::to_string(FunctionID);
    Program.High.Functions.push_back(std::move(Recovered));
    Program.Low.Edges.push_back({FunctionID, FunctionCount, EdgeKind::Branch});
  }
  for (size_t BlockID = FunctionCount; BlockID + 1 < BlockCount; ++BlockID)
    Program.Low.Edges.push_back({BlockID, BlockID + 1, EdgeKind::Fallthrough});
  ASSERT_TRUE(Program.High.Calls.empty());

  const FunctionBodyIndex Bodies(Program);
  const FunctionBodyIndex::Statistics &Stats = Bodies.statistics();
  EXPECT_EQ(Stats.IndexedBlockCount, BlockCount);
  EXPECT_EQ(Stats.IndexedEdgeCount, Program.Low.Edges.size());
  EXPECT_EQ(Stats.IndexedFunctionCount, FunctionCount);
  EXPECT_LE(Stats.ResidentIndexEntryCount,
            LinearStorageAllowancePerEntity *
                    (BlockCount + Program.Low.Edges.size() + FunctionCount) +
                LinearStorageAllowancePerEntity);

  const uint64_t ExpectedSize = (SharedBlockCount + 1) * kInstructionSize;
  for (size_t FunctionID : {size_t{0}, FunctionCount / 2, FunctionCount - 1})
    EXPECT_EQ(Bodies.byteSize(Program.High.Functions[FunctionID]),
              ExpectedSize);

  const std::vector<size_t> TailFunctions =
      Bodies.functionsForBlock(FunctionCount);
  EXPECT_EQ(TailFunctions.size(), FunctionCount);
  const std::vector<size_t> FirstBody =
      Bodies.blocks(Program.High.Functions.front());
  ASSERT_EQ(FirstBody.size(), SharedBlockCount + 1);
  EXPECT_EQ(FirstBody.front(), 0u);
  EXPECT_EQ(FirstBody[1], FunctionCount);
  EXPECT_EQ(FirstBody.back(), BlockCount - 1);
}

TEST(SBFAnalyzer, IndexesRepeatedRemoteJoinsInLinearResidentSpace) {
  constexpr size_t FunctionCount = 2'000;
  constexpr size_t JoinCount = 2'000;
  constexpr size_t FunctionsPerSide = FunctionCount / 2;
  constexpr size_t LeftChain = FunctionCount;
  constexpr size_t RightChain = LeftChain + JoinCount;
  constexpr size_t FirstJoin = RightChain + JoinCount;
  constexpr size_t BlockCount = FirstJoin + JoinCount;
  constexpr size_t LinearStorageAllowancePerEntity = 4;

  SBFProgram Program;
  Program.Low.Blocks.resize(BlockCount);
  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    BasicBlock &Block = Program.Low.Blocks[BlockID];
    Block.ID = BlockID;
    Block.StartSlot = BlockID;
    Block.EndSlot = BlockID + 1;
  }
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    Function Recovered;
    Recovered.EntrySlot = FunctionID;
    Recovered.Address = kBytecodeStart + FunctionID * kInstructionSize;
    Program.High.Functions.push_back(std::move(Recovered));
    Program.Low.Edges.push_back(
        {FunctionID, FunctionID < FunctionsPerSide ? LeftChain : RightChain,
         EdgeKind::Branch});
  }
  for (size_t Join = 0; Join < JoinCount; ++Join) {
    const size_t Left = LeftChain + Join;
    const size_t Right = RightChain + Join;
    const size_t JoinBlock = FirstJoin + Join;
    Program.Low.Edges.push_back({Left, JoinBlock, EdgeKind::BranchTaken});
    Program.Low.Edges.push_back({Right, JoinBlock, EdgeKind::BranchTaken});
    if (Join + 1 < JoinCount) {
      Program.Low.Edges.push_back({Left, Left + 1, EdgeKind::Fallthrough});
      Program.Low.Edges.push_back({Right, Right + 1, EdgeKind::Fallthrough});
    }
  }

  const FunctionBodyIndex Bodies(Program);
  const FunctionBodyIndex::Statistics &Stats = Bodies.statistics();
  EXPECT_EQ(Stats.IndexedBlockCount, BlockCount);
  EXPECT_EQ(Stats.IndexedEdgeCount, Program.Low.Edges.size());
  EXPECT_EQ(Stats.IndexedFunctionCount, FunctionCount);
  EXPECT_LE(Stats.ResidentIndexEntryCount,
            LinearStorageAllowancePerEntity *
                    (BlockCount + Program.Low.Edges.size() + FunctionCount) +
                LinearStorageAllowancePerEntity);
  EXPECT_EQ(Bodies.functionsForBlock(FirstJoin).size(), FunctionCount);
}

TEST(SBFAnalyzer, KeepsStaircaseFunctionProvenanceLinearAndQueryLocal) {
  constexpr size_t FunctionCount = 10'000;
  constexpr size_t JoinCount = FunctionCount - 1;
  constexpr size_t FirstJoin = FunctionCount;
  constexpr size_t BlockCount = FunctionCount + JoinCount;
  constexpr size_t LinearStorageAllowancePerEntity = 4;

  SBFProgram Program;
  Program.Low.Blocks.resize(BlockCount);
  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    BasicBlock &Block = Program.Low.Blocks[BlockID];
    Block.ID = BlockID;
    Block.StartSlot = BlockID;
    Block.EndSlot = BlockID + 1;
  }
  Program.High.Functions.reserve(FunctionCount);
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    Function Recovered;
    Recovered.EntrySlot = FunctionID;
    Recovered.Address = kBytecodeStart + FunctionID * kInstructionSize;
    Program.High.Functions.push_back(std::move(Recovered));
  }

  Program.Low.Edges.push_back({0, FirstJoin, EdgeKind::BranchTaken});
  Program.Low.Edges.push_back({1, FirstJoin, EdgeKind::BranchTaken});
  for (size_t FunctionID = 2; FunctionID < FunctionCount; ++FunctionID) {
    const size_t PreviousJoin = FirstJoin + FunctionID - 2;
    const size_t CurrentJoin = FirstJoin + FunctionID - 1;
    Program.Low.Edges.push_back(
        {PreviousJoin, CurrentJoin, EdgeKind::Fallthrough});
    Program.Low.Edges.push_back(
        {FunctionID, CurrentJoin, EdgeKind::BranchTaken});
  }

  const FunctionBodyIndex Bodies(Program);
  const FunctionBodyIndex::Statistics InitialStats = Bodies.statistics();
  EXPECT_EQ(InitialStats.IndexedBlockCount, BlockCount);
  EXPECT_EQ(InitialStats.IndexedEdgeCount, Program.Low.Edges.size());
  EXPECT_EQ(InitialStats.IndexedFunctionCount, FunctionCount);
  EXPECT_LE(InitialStats.ResidentIndexEntryCount,
            LinearStorageAllowancePerEntity *
                    (BlockCount + Program.Low.Edges.size() + FunctionCount) +
                LinearStorageAllowancePerEntity);

  const auto CheckFunction = [&](size_t FunctionID, size_t ExpectedBlockCount) {
    const Function &Recovered = Program.High.Functions[FunctionID];
    EXPECT_EQ(Bodies.blocks(Recovered).size(), ExpectedBlockCount);
    EXPECT_EQ(Bodies.byteSize(Recovered),
              ExpectedBlockCount * kInstructionSize);
  };
  CheckFunction(0, FunctionCount);
  CheckFunction(FunctionCount / 2, FunctionCount - FunctionCount / 2 + 1);
  CheckFunction(FunctionCount - 1, 2);

  EXPECT_EQ(Bodies.functionsForBlock(FirstJoin).size(), 2u);
  EXPECT_EQ(Bodies.functionsForBlock(FirstJoin + FunctionCount / 2 - 1).size(),
            FunctionCount / 2 + 1);
  const std::vector<size_t> FinalOwners =
      Bodies.functionsForBlock(BlockCount - 1);
  ASSERT_EQ(FinalOwners.size(), FunctionCount);
  EXPECT_EQ(FinalOwners.front(), 0u);
  EXPECT_EQ(FinalOwners.back(), FunctionCount - 1);
  EXPECT_EQ(Bodies.statistics().ResidentIndexEntryCount,
            InitialStats.ResidentIndexEntryCount)
      << "queries must not memoize the quadratic transitive closure";
}

TEST(SBFAnalyzer, BatchesSharedCallSitesForOneTargetIntoOneReverseWalk) {
  constexpr size_t RepetitionCount = 10'000;
  constexpr size_t SharedBlock = 2;

  SBFProgram Program;
  Program.Low.Blocks.resize(4);
  for (size_t BlockID = 0; BlockID < Program.Low.Blocks.size(); ++BlockID) {
    BasicBlock &Block = Program.Low.Blocks[BlockID];
    Block.ID = BlockID;
    Block.StartSlot = BlockID;
    Block.EndSlot = BlockID + 1;
  }
  Program.Low.Edges = {{0, SharedBlock, EdgeKind::BranchTaken},
                       {1, SharedBlock, EdgeKind::BranchTaken},
                       {SharedBlock, SharedBlock + 1, EdgeKind::Fallthrough}};
  for (size_t FunctionID = 0; FunctionID < 2; ++FunctionID) {
    Function Recovered;
    Recovered.EntrySlot = FunctionID;
    Recovered.Address = kBytecodeStart + FunctionID * kInstructionSize;
    Program.High.Functions.push_back(std::move(Recovered));
  }
  Program.High.BlockOwners = {0, 1, HighIR::AmbiguousFunction,
                              HighIR::AmbiguousFunction};

  const FunctionBodyIndex Bodies(Program);
  std::vector<size_t> UniqueCallSites(RepetitionCount, 0);
  EXPECT_EQ(Bodies.functionsForAnyBlock(UniqueCallSites),
            std::vector<size_t>({0}));
  EXPECT_EQ(Bodies.statistics().BlockFunctionBatchQueryCount, 1u);
  EXPECT_EQ(Bodies.statistics().ReverseReachabilityQueryCount, 0u)
      << "unique ownership must use the O(1) fast path";
  EXPECT_EQ(Bodies.statistics().ReverseWorkspaceInitializationCount, 0u)
      << "the O(1) path must not zero a whole-program visited set";

  std::vector<size_t> CallSiteBlocks;
  CallSiteBlocks.reserve(RepetitionCount * 2);
  for (size_t I = 0; I < RepetitionCount; ++I) {
    CallSiteBlocks.push_back(SharedBlock);
    CallSiteBlocks.push_back(SharedBlock + 1);
  }

  EXPECT_EQ(Bodies.functionsForAnyBlock(CallSiteBlocks),
            std::vector<size_t>({0, 1}));
  EXPECT_EQ(Bodies.statistics().BlockFunctionBatchQueryCount, 2u);
  EXPECT_EQ(Bodies.statistics().ReverseReachabilityQueryCount, 1u)
      << "all ambiguous call blocks for one target need one reverse walk";
  EXPECT_EQ(Bodies.statistics().ReverseWorkspaceInitializationCount, 1u);
}

TEST(SBFAnalyzer, ResolvesTenThousandDistinctTargetsInTwoBodyTraversals) {
  constexpr size_t FunctionCount = 2;
  constexpr size_t TargetCount = 10'000;
  constexpr size_t FirstSharedBlock = FunctionCount;
  constexpr size_t BlockCount = FunctionCount + TargetCount;

  SBFProgram Program;
  Program.Low.Blocks.resize(BlockCount);
  Program.High.BlockOwners.assign(BlockCount, HighIR::AmbiguousFunction);
  Program.Low.Edges.reserve(FunctionCount + TargetCount - 1);
  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    BasicBlock &Block = Program.Low.Blocks[BlockID];
    Block.ID = BlockID;
    Block.StartSlot = BlockID;
    Block.EndSlot = BlockID + 1;
  }
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    Function Recovered;
    Recovered.EntrySlot = FunctionID;
    Recovered.Address = kBytecodeStart + FunctionID * kInstructionSize;
    Program.High.Functions.push_back(std::move(Recovered));
    Program.High.BlockOwners[FunctionID] = FunctionID;
    Program.Low.Edges.push_back(
        {FunctionID, FirstSharedBlock, EdgeKind::BranchTaken});
  }
  for (size_t BlockID = FirstSharedBlock; BlockID + 1 < BlockCount; ++BlockID)
    Program.Low.Edges.push_back({BlockID, BlockID + 1, EdgeKind::Fallthrough});

  std::vector<size_t> SourceBlocks(TargetCount);
  std::vector<FunctionBodyIndex::BlockGroupQuery> Groups;
  Groups.reserve(TargetCount);
  for (size_t TargetID = 0; TargetID < TargetCount; ++TargetID) {
    SourceBlocks[TargetID] = FirstSharedBlock + TargetID;
    Groups.push_back({llvm::ArrayRef<size_t>(&SourceBlocks[TargetID], 1)});
  }

  const FunctionBodyIndex Bodies(Program);
  const FunctionBodyIndex::BlockGroupFunctionBatch Batch =
      Bodies.functionsForBlockGroups(Groups,
                                     kCallGraphProvenanceBlockVisitBudget,
                                     kCallGraphOutputEdgeBudget);
  ASSERT_TRUE(Batch.complete());
  EXPECT_EQ(Batch.FunctionTraversalCount, FunctionCount);
  EXPECT_EQ(Batch.WorkspaceInitializationCount, 1u);
  EXPECT_EQ(Batch.BlockVisitCount, FunctionCount * (TargetCount + 1));
  EXPECT_EQ(Batch.ChargedBlockWork,
            BlockCount + FunctionCount * (TargetCount + 1));
  EXPECT_EQ(Batch.OutputRelationCount, FunctionCount * TargetCount);
  ASSERT_EQ(Batch.FunctionOffsets.size(), TargetCount + 1);
  const std::vector<size_t> ExpectedFunctions = {0, 1};
  EXPECT_EQ(Batch.functionsForGroup(0),
            llvm::ArrayRef<size_t>(ExpectedFunctions));
  EXPECT_EQ(Batch.functionsForGroup(TargetCount - 1),
            llvm::ArrayRef<size_t>(ExpectedFunctions));
}

TEST(SBFAnalyzer, ResolvesOneSharedTailByOneReverseTraversal) {
  constexpr size_t FunctionCount = 10'000;
  constexpr size_t SharedBlockCount = 10'000;
  constexpr size_t FirstSharedBlock = FunctionCount;
  constexpr size_t BlockCount = FunctionCount + SharedBlockCount;

  SBFProgram Program;
  Program.Low.Blocks.resize(BlockCount);
  Program.High.BlockOwners.assign(BlockCount, HighIR::AmbiguousFunction);
  Program.High.Functions.reserve(FunctionCount);
  Program.Low.Edges.reserve(FunctionCount + SharedBlockCount - 1);
  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    BasicBlock &Block = Program.Low.Blocks[BlockID];
    Block.ID = BlockID;
    Block.StartSlot = BlockID;
    Block.EndSlot = BlockID + 1;
  }
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    Function Recovered;
    Recovered.EntrySlot = FunctionID;
    Recovered.Address = kBytecodeStart + FunctionID * kInstructionSize;
    Program.High.Functions.push_back(std::move(Recovered));
    Program.High.BlockOwners[FunctionID] = FunctionID;
    Program.Low.Edges.push_back(
        {FunctionID, FirstSharedBlock, EdgeKind::BranchTaken});
  }
  for (size_t BlockID = FirstSharedBlock; BlockID + 1 < BlockCount; ++BlockID)
    Program.Low.Edges.push_back({BlockID, BlockID + 1, EdgeKind::Fallthrough});

  const size_t SourceBlock = BlockCount - 1;
  const FunctionBodyIndex::BlockGroupQuery Group = {
      llvm::ArrayRef<size_t>(&SourceBlock, 1)};
  const FunctionBodyIndex Bodies(Program);
  const FunctionBodyIndex::BlockGroupFunctionBatch Batch =
      Bodies.functionsForBlockGroups(
          llvm::ArrayRef<FunctionBodyIndex::BlockGroupQuery>(&Group, 1),
          kCallGraphProvenanceBlockVisitBudget, kCallGraphOutputEdgeBudget);

  ASSERT_TRUE(Batch.complete());
  EXPECT_EQ(Batch.FunctionTraversalCount, 0u);
  EXPECT_EQ(Batch.ReverseGroupTraversalCount, 1u);
  EXPECT_EQ(Batch.WorkspaceInitializationCount, 1u);
  EXPECT_EQ(Batch.BlockVisitCount, BlockCount);
  EXPECT_EQ(Batch.ChargedBlockWork, 2 * BlockCount);
  EXPECT_EQ(Batch.OutputRelationCount, FunctionCount);
  ASSERT_EQ(Batch.functionsForGroup(0).size(), FunctionCount);
  EXPECT_EQ(Batch.functionsForGroup(0).front(), 0u);
  EXPECT_EQ(Batch.functionsForGroup(0).back(), FunctionCount - 1);
}

TEST(SBFAnalyzer, BlockGroupProvenanceBudgetsAreExactOrFail) {
  constexpr size_t BlockCount = 3;
  SBFProgram Program;
  Program.Low.Blocks.resize(BlockCount);
  Program.High.BlockOwners = {0, 1, HighIR::NoFunction};
  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    Program.Low.Blocks[BlockID].ID = BlockID;
    Program.Low.Blocks[BlockID].StartSlot = BlockID;
    Program.Low.Blocks[BlockID].EndSlot = BlockID + 1;
  }
  for (size_t FunctionID = 0; FunctionID < 2; ++FunctionID) {
    Function Recovered;
    Recovered.EntrySlot = FunctionID;
    Recovered.Address = kBytecodeStart + FunctionID * kInstructionSize;
    Program.High.Functions.push_back(std::move(Recovered));
  }

  const size_t InvalidBlock = BlockCount + 7;
  const std::array<size_t, 4> FirstBlocks = {0, 0, 2, InvalidBlock};
  const size_t SecondBlock = 1;
  const std::array<FunctionBodyIndex::BlockGroupQuery, 2> Groups = {
      FunctionBodyIndex::BlockGroupQuery{FirstBlocks},
      FunctionBodyIndex::BlockGroupQuery{
          llvm::ArrayRef<size_t>(&SecondBlock, 1)}};
  const FunctionBodyIndex Bodies(Program);

  const auto Exact = Bodies.functionsForBlockGroups(
      Groups, kCallGraphProvenanceBlockVisitBudget, kCallGraphOutputEdgeBudget);
  ASSERT_TRUE(Exact.complete());
  const std::vector<size_t> FirstExpected = {0};
  const std::vector<size_t> SecondExpected = {1};
  EXPECT_EQ(Exact.functionsForGroup(0), llvm::ArrayRef<size_t>(FirstExpected));
  EXPECT_EQ(Exact.functionsForGroup(1), llvm::ArrayRef<size_t>(SecondExpected));
  EXPECT_EQ(Exact.OutputRelationCount, 2u)
      << "unique-owner relations participate in the output budget";
  EXPECT_EQ(Exact.WorkspaceInitializationCount, 0u);

  const auto Exhausted = Bodies.functionsForBlockGroups(
      Groups, kCallGraphProvenanceBlockVisitBudget,
      /*OutputRelationBudget=*/1);
  EXPECT_FALSE(Exhausted.complete());
  EXPECT_TRUE(Exhausted.OutputBudgetExhausted);
  EXPECT_TRUE(Exhausted.FunctionOffsets.empty());
  EXPECT_TRUE(Exhausted.FunctionIDs.empty());
  EXPECT_TRUE(Exhausted.functionsForGroup(0).empty());
  EXPECT_EQ(Exhausted.WorkspaceInitializationCount, 0u);
}

TEST(SBFAnalyzer, ZeroBlockWorkBudgetAllocatesNoProvenanceWorkspace) {
  SBFProgram Program;
  Program.Low.Blocks.resize(2);
  Program.High.BlockOwners = {0, HighIR::AmbiguousFunction};
  for (size_t BlockID = 0; BlockID < Program.Low.Blocks.size(); ++BlockID) {
    Program.Low.Blocks[BlockID].ID = BlockID;
    Program.Low.Blocks[BlockID].StartSlot = BlockID;
    Program.Low.Blocks[BlockID].EndSlot = BlockID + 1;
  }
  Program.Low.Edges.push_back({0, 1, EdgeKind::Fallthrough});
  Function Recovered;
  Recovered.EntrySlot = 0;
  Recovered.Address = kBytecodeStart;
  Program.High.Functions.push_back(std::move(Recovered));

  const size_t SourceBlock = 1;
  const FunctionBodyIndex::BlockGroupQuery Group = {
      llvm::ArrayRef<size_t>(&SourceBlock, 1)};
  const FunctionBodyIndex Bodies(Program);
  const auto Batch = Bodies.functionsForBlockGroups(
      llvm::ArrayRef<FunctionBodyIndex::BlockGroupQuery>(&Group, 1),
      /*BlockVisitBudget=*/0, kCallGraphOutputEdgeBudget);
  EXPECT_FALSE(Batch.complete());
  EXPECT_TRUE(Batch.VisitBudgetExhausted);
  EXPECT_EQ(Batch.ChargedBlockWork, 0u);
  EXPECT_EQ(Batch.BlockVisitCount, 0u);
  EXPECT_EQ(Batch.WorkspaceInitializationCount, 0u);
  EXPECT_TRUE(Batch.FunctionOffsets.empty());
  EXPECT_TRUE(Batch.FunctionIDs.empty());

  const auto PartiallyVisited = Bodies.functionsForBlockGroups(
      llvm::ArrayRef<FunctionBodyIndex::BlockGroupQuery>(&Group, 1),
      /*BlockVisitBudget=*/Program.Low.Blocks.size() + 1,
      kCallGraphOutputEdgeBudget);
  EXPECT_FALSE(PartiallyVisited.complete());
  EXPECT_TRUE(PartiallyVisited.VisitBudgetExhausted);
  EXPECT_EQ(PartiallyVisited.BlockVisitCount, 1u);
  EXPECT_TRUE(PartiallyVisited.FunctionOffsets.empty());
  EXPECT_TRUE(PartiallyVisited.FunctionIDs.empty())
      << "mid-traversal exhaustion must not retain a CSR prefix";
}

TEST(SBFAnalyzer, CallGraphOutputCapacityRejectsNodesBeforeMaterialization) {
  const std::optional<size_t> EmptyCapacity = callGraphEdgeCapacity(0);
  ASSERT_TRUE(EmptyCapacity.has_value());
  EXPECT_EQ(*EmptyCapacity, kCallGraphOutputEdgeBudget);
  EXPECT_TRUE(callGraphEdgeCapacity(kCallGraphOutputNodeBudget).has_value());
  EXPECT_FALSE(
      callGraphEdgeCapacity(kCallGraphOutputNodeBudget + 1).has_value());
  EXPECT_FALSE(
      callGraphEdgeCapacity(kCallGraphOutputElementBudget + 1).has_value());
}

TEST(SBFAnalyzer, OutputByteBudgetHandlesExactAndOverflowBoundaries) {
  AnalysisOutputByteBudget Budget(/*Limit=*/10);
  EXPECT_TRUE(Budget.consume(4));
  EXPECT_EQ(Budget.consumed(), 4u);
  EXPECT_TRUE(Budget.consume(6));
  EXPECT_EQ(Budget.consumed(), 10u);
  EXPECT_FALSE(Budget.exceeded());
  EXPECT_FALSE(Budget.consume(1));
  EXPECT_TRUE(Budget.exceeded());
  EXPECT_EQ(Budget.consumed(), 10u)
      << "a rejected increment must not wrap or retain a partial byte";

  AnalysisOutputByteBudget FullWidth(std::numeric_limits<size_t>::max());
  EXPECT_TRUE(FullWidth.consume(std::numeric_limits<size_t>::max()));
  EXPECT_FALSE(FullWidth.consume(1));
  EXPECT_TRUE(FullWidth.exceeded());
}

TEST(SBFAnalyzer, EdgeKindTableOwnsConditionalAPISpellings) {
  EXPECT_EQ(getEdgeKindInfo(EdgeKind::Fallthrough).APIName, "fallthrough");
  EXPECT_EQ(getEdgeKindInfo(EdgeKind::Fallthrough).ConditionalAPIName, "false");
  EXPECT_EQ(getEdgeKindInfo(EdgeKind::BranchTaken).APIName, "true");
  EXPECT_EQ(getEdgeKindInfo(EdgeKind::BranchTaken).ConditionalAPIName, "true");
}

TEST(SBFAnalyzer, BatchesOneLargeSharedTailForEveryFunctionSize) {
  constexpr size_t FunctionCount = 10'000;
  constexpr size_t SharedBlockCount = 10'000;
  constexpr size_t FirstSharedBlock = FunctionCount;
  constexpr size_t BlockCount = FunctionCount + SharedBlockCount;

  SBFProgram Program;
  Program.Low.Blocks.resize(BlockCount);
  Program.High.BlockOwners.assign(BlockCount, HighIR::AmbiguousFunction);
  Program.High.Functions.reserve(FunctionCount);
  Program.Low.Edges.reserve(FunctionCount + SharedBlockCount - 1);
  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    BasicBlock &Block = Program.Low.Blocks[BlockID];
    Block.ID = BlockID;
    Block.StartSlot = BlockID;
    Block.EndSlot = BlockID + 1;
  }
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    Function Recovered;
    Recovered.EntrySlot = FunctionID;
    Recovered.Address = kBytecodeStart + FunctionID * kInstructionSize;
    Program.High.Functions.push_back(std::move(Recovered));
    Program.High.BlockOwners[FunctionID] = FunctionID;
    Program.Low.Edges.push_back(
        {FunctionID, FirstSharedBlock, EdgeKind::BranchTaken});
  }
  for (size_t BlockID = FirstSharedBlock; BlockID + 1 < BlockCount; ++BlockID)
    Program.Low.Edges.push_back({BlockID, BlockID + 1, EdgeKind::Fallthrough});

  const FunctionBodyIndex Bodies(Program);
  const FunctionBodyIndex::ByteSizeBatch Sizes =
      Bodies.byteSizes(kFunctionBodyBatchBlockVisitBudget);
  ASSERT_EQ(Sizes.Bytes.size(), FunctionCount);
  EXPECT_TRUE(Sizes.Exact.all());
  EXPECT_FALSE(Sizes.BudgetExhausted);
  EXPECT_EQ(Sizes.BlockVisitCount, SharedBlockCount)
      << "one shared frontier must be traversed once, not once per function";
  EXPECT_EQ(Sizes.ReachabilityQueryCount, 1u);
  EXPECT_EQ(Sizes.WorkspaceInitializationCount, 1u);
  const uint64_t Expected = (SharedBlockCount + 1) * kInstructionSize;
  EXPECT_EQ(Sizes.Bytes.front(), Expected);
  EXPECT_EQ(Sizes.Bytes.back(), Expected);
}

TEST(SBFAnalyzer, ReportsUnknownInsteadOfAPartialSizeAtTheQueryBudget) {
  constexpr size_t FunctionCount = 64;
  constexpr size_t FirstJoin = FunctionCount;
  constexpr size_t BlockCount = 2 * FunctionCount - 1;
  constexpr size_t BlockVisitBudget = 16;

  SBFProgram Program;
  Program.Low.Blocks.resize(BlockCount);
  Program.High.BlockOwners.assign(BlockCount, HighIR::AmbiguousFunction);
  Program.High.Functions.reserve(FunctionCount);
  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    BasicBlock &Block = Program.Low.Blocks[BlockID];
    Block.ID = BlockID;
    Block.StartSlot = BlockID;
    Block.EndSlot = BlockID + 1;
  }
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    Function Recovered;
    Recovered.EntrySlot = FunctionID;
    Recovered.Address = kBytecodeStart + FunctionID * kInstructionSize;
    Program.High.Functions.push_back(std::move(Recovered));
    Program.High.BlockOwners[FunctionID] = FunctionID;
  }
  Program.Low.Edges.push_back({0, FirstJoin, EdgeKind::BranchTaken});
  Program.Low.Edges.push_back({1, FirstJoin, EdgeKind::BranchTaken});
  for (size_t FunctionID = 2; FunctionID < FunctionCount; ++FunctionID) {
    const size_t PreviousJoin = FirstJoin + FunctionID - 2;
    const size_t CurrentJoin = FirstJoin + FunctionID - 1;
    Program.Low.Edges.push_back(
        {PreviousJoin, CurrentJoin, EdgeKind::Fallthrough});
    Program.Low.Edges.push_back(
        {FunctionID, CurrentJoin, EdgeKind::BranchTaken});
  }

  const FunctionBodyIndex Bodies(Program);
  ASSERT_GT(Bodies.byteSize(Program.High.Functions.front()), 0u);
  const FunctionBodyIndex::ByteSizeBatch Sizes =
      Bodies.byteSizes(BlockVisitBudget);
  EXPECT_TRUE(Sizes.BudgetExhausted);
  EXPECT_EQ(Sizes.BlockVisitCount, BlockVisitBudget);
  EXPECT_EQ(Sizes.ReachabilityQueryCount, 1u);
  EXPECT_EQ(Sizes.WorkspaceInitializationCount, 1u);
  EXPECT_FALSE(Sizes.Exact.any());
  EXPECT_TRUE(std::all_of(Sizes.Bytes.begin(), Sizes.Bytes.end(),
                          [](uint64_t Size) { return Size == 0; }))
      << "the C API's unknown sentinel must never expose a partial traversal";
}

TEST(SBFAnalyzer, ZeroSizeBudgetAllocatesNoReachabilityWorkspace) {
  constexpr size_t FunctionCount = 1'024;
  constexpr size_t BlockCount = 2 * FunctionCount;

  SBFProgram Program;
  Program.Low.Blocks.resize(BlockCount);
  Program.High.BlockOwners.assign(BlockCount, HighIR::AmbiguousFunction);
  Program.High.Functions.reserve(FunctionCount);
  Program.Low.Edges.reserve(FunctionCount);
  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    BasicBlock &Block = Program.Low.Blocks[BlockID];
    Block.ID = BlockID;
    Block.StartSlot = BlockID;
    Block.EndSlot = BlockID + 1;
  }
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    Function Recovered;
    Recovered.EntrySlot = FunctionID;
    Recovered.Address = kBytecodeStart + FunctionID * kInstructionSize;
    Program.High.Functions.push_back(std::move(Recovered));
    Program.High.BlockOwners[FunctionID] = FunctionID;
    Program.Low.Edges.push_back(
        {FunctionID, FunctionCount + FunctionID, EdgeKind::BranchTaken});
  }

  const auto CheckNoWorkspace = [](const FunctionBodyIndex &Bodies) {
    const FunctionBodyIndex::ByteSizeBatch Sizes = Bodies.byteSizes(0);
    EXPECT_TRUE(Sizes.BudgetExhausted);
    EXPECT_EQ(Sizes.BlockVisitCount, 0u);
    EXPECT_EQ(Sizes.ReachabilityQueryCount, 0u);
    EXPECT_EQ(Sizes.WorkspaceInitializationCount, 0u);
    EXPECT_FALSE(Sizes.Exact.any());
  };
  CheckNoWorkspace(FunctionBodyIndex(Program));

  // An invalid owner authority selects the fallback path. It must apply the
  // same pre-allocation guard rather than zeroing a B-bit workspace once per
  // remaining function after the budget is already exhausted.
  Program.High.BlockOwners.front() = HighIR::AmbiguousFunction;
  CheckNoWorkspace(FunctionBodyIndex(Program));
}

TEST(SBFAnalyzer, PropagatesFactsAcrossTenThousandReverseOrderedBlocks) {
  constexpr size_t BlockCount = 10'000;
  std::vector<EncodedInstruction> Instructions(BlockCount,
                                               encode(Opcode::JA, 0, 0, -2));
  Instructions.front() = encode(Opcode::EXIT);

  // Execution starts at the highest-numbered block and walks toward block 0.
  // A whole-CFG scan in block-number order therefore advances the entry fact
  // by only one block per pass and performs O(B^2) block evaluations.  A
  // dependency worklist reaches every block once the predecessor changes.
  auto Program =
      analyze(makeImage(Version::V3, Instructions, Instructions.size() - 1));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.Blocks.size(), BlockCount);
  ASSERT_EQ(Program->Med.Blocks.size(), BlockCount);
  ASSERT_EQ(Program->Low.Edges.size(), BlockCount);
  EXPECT_EQ(Program->Low.Instructions.capacity(),
            Program->Low.Instructions.size());
  EXPECT_EQ(Program->Low.Blocks.capacity(), Program->Low.Blocks.size());
  EXPECT_EQ(Program->Low.Edges.capacity(), Program->Low.Edges.size());
  EXPECT_EQ(Program->Med.Instructions.capacity(),
            Program->Med.Instructions.size());
  EXPECT_EQ(Program->Med.Blocks.capacity(), Program->Med.Blocks.size());
  EXPECT_EQ(Program->Med.Blocks.front().Inputs[kFramePointerRegister].ValueKind,
            RegisterValue::Kind::StackAddress);
  EXPECT_EQ(Program->High.FunctionBlocks.size(), BlockCount);
}

TEST(SBFAnalyzerScale, AnalyzesTheDeployableProtocolCeiling) {
  if (!std::getenv(kProtocolCeilingScaleEnvironment.data()))
    GTEST_SKIP() << "set " << kProtocolCeilingScaleEnvironment.data()
                 << " to run the 10 MiB end-to-end scale benchmark";

  std::vector<EncodedInstruction> Instructions(kMaxInstructions,
                                               encode(Opcode::JA, 0, 0, -2));
  const SyscallInfo *Invocation = getSyscallInfo(Syscall::InvokeSignedRust);
  ASSERT_NE(Invocation, nullptr);
  Instructions[0] =
      encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Invocation->Hash));
  Instructions[1] = encode(Opcode::EXIT);
  Instructions[2] = encode(Opcode::JA, 0, 0, -3);
  auto Program =
      analyze(makeImage(Version::V3, Instructions, kMaxInstructions - 1));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_EQ(Program->Low.Instructions.size(), kMaxInstructions);
  EXPECT_EQ(Program->Low.Blocks.size(), kMaxInstructions);
  EXPECT_EQ(Program->Med.Blocks.size(), kMaxInstructions);
  EXPECT_EQ(Program->High.FunctionBlocks.size(), kMaxInstructions);
  EXPECT_EQ(Program->Low.Instructions.capacity(), kMaxInstructions);
  EXPECT_EQ(Program->Low.Blocks.capacity(), kMaxInstructions);
  EXPECT_EQ(Program->Low.Edges.capacity(), kMaxInstructions);
  EXPECT_EQ(Program->Med.Instructions.capacity(), kMaxInstructions);
  EXPECT_EQ(Program->Med.Blocks.capacity(), kMaxInstructions);
  EXPECT_TRUE(std::any_of(Program->Med.Instructions.begin(),
                          Program->Med.Instructions.end(),
                          consumesScratchFacts))
      << "the ceiling gate must exercise demand-driven ScratchFlow";
  EXPECT_EQ(Program->High.Solana.ScratchPrecision,
            ScratchRecoveryPrecision::Exact);
}

TEST(SBFAnalyzer, FoldsTenThousandConditionalLatchesIntoOneLoopRegion) {
  constexpr size_t BlockCount = 10'000;
  std::vector<EncodedInstruction> Instructions;
  Instructions.reserve(BlockCount);
  for (size_t Slot = 0; Slot + 1 < BlockCount; ++Slot) {
    const int16_t BackToEntry =
        static_cast<int16_t>(-static_cast<int32_t>(Slot + 1));
    Instructions.push_back(encode(Opcode::JEQ64_IMM, 0, 0, BackToEntry));
  }
  Instructions.push_back(encode(Opcode::EXIT));

  // Every conditional block is a possible latch of the same natural loop.
  // Treating each latch as an independent If region materializes the nested
  // prefixes and makes both dominance queries and region storage quadratic.
  auto Program = analyze(makeImage(Version::V3, Instructions));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.Blocks.size(), BlockCount);
  ASSERT_EQ(Program->High.Regions.size(), 1u);
  EXPECT_EQ(Program->High.Regions.front().Kind, RegionKind::Loop);
  const Region &Loop = Program->High.Regions.front();
  EXPECT_TRUE(Loop.Blocks.empty());
  EXPECT_EQ(Loop.BlockCount, BlockCount - 1);
  EXPECT_EQ(Program->High.latches(Loop).size(), BlockCount - 1);
  EXPECT_EQ(Loop.ExitBlock, std::optional<size_t>(BlockCount - 1));
}

TEST(SBFAnalyzer, RepresentsAndEmitsTenThousandRemoteJoinConditionals) {
  constexpr size_t BlockCount = 10'000;
  constexpr size_t JoinSlot = BlockCount - 1;
  std::vector<EncodedInstruction> Instructions;
  Instructions.reserve(BlockCount);
  for (size_t Slot = 0; Slot + 2 < BlockCount; ++Slot) {
    const int16_t ToJoin = static_cast<int16_t>(JoinSlot - Slot - 1);
    Instructions.push_back(encode(Opcode::JEQ64_IMM, 0, 0, ToJoin));
  }
  Instructions.push_back(encode(Opcode::JA, 0, 0, 0));
  Instructions.push_back(encode(Opcode::EXIT));

  // Every conditional shares one remote join. Explicitly materializing each
  // nested If closure stores a triangular number of block IDs. If regions
  // are instead defined by their typed CFG boundary, total metadata is O(B).
  auto Program = analyze(makeImage(Version::V3, Instructions));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.Blocks.size(), BlockCount);
  ASSERT_EQ(Program->High.Regions.size(), BlockCount - 2);
  for (const Region &Recovered : Program->High.Regions) {
    ASSERT_EQ(Recovered.Kind, RegionKind::If);
    EXPECT_EQ(Recovered.ExitBlock, std::optional<size_t>(JoinSlot));
    EXPECT_TRUE(Recovered.Blocks.empty())
        << "If membership is implicit in its CFG boundary";
  }

  const std::optional<StructuredControlFlow> Structured =
      buildStructuredControlFlow(*Program);
  ASSERT_TRUE(Structured);
  EXPECT_EQ(Structured->Nodes.size(), BlockCount);
  EXPECT_NE(Structured->Entry, StructuredNode::NoNode);
  {
    auto Source = emitC(*Program);
    ASSERT_TRUE(static_cast<bool>(Source))
        << llvm::toString(Source.takeError());
    EXPECT_NE(Source->find("switch (pc)"), std::string::npos)
        << "hostile nesting must use the exact portable C dispatcher";
  }
  {
    auto Source = emitRust(*Program);
    ASSERT_TRUE(static_cast<bool>(Source))
        << llvm::toString(Source.takeError());
    EXPECT_NE(Source->find("match pc"), std::string::npos)
        << "hostile nesting must use the exact portable Rust dispatcher";
  }
}

TEST(SBFAnalyzerDominators, MatchesSixtyThousandSeededRandomCFGs) {
  constexpr uint64_t RandomSeed = UINT64_C(0x4e65766572445342);
  constexpr size_t GraphCount = 60'000;
  constexpr size_t MaximumBlockCount = 24;
  constexpr size_t MaximumSuccessorCount = 3;
  constexpr size_t CommonDominatorQueriesPerGraph = 3;
  constexpr size_t Root = 0;
  constexpr size_t NoBlock = std::numeric_limits<size_t>::max();
  std::mt19937_64 Random(RandomSeed);

  for (size_t GraphIndex = 0; GraphIndex < GraphCount; ++GraphIndex) {
    const size_t BlockCount = 1 + Random() % MaximumBlockCount;
    std::vector<std::vector<size_t>> Successors(BlockCount);
    std::vector<std::vector<size_t>> Predecessors(BlockCount);
    for (size_t From = 0; From < BlockCount; ++From) {
      const size_t SuccessorCount = Random() % (MaximumSuccessorCount + 1);
      for (size_t Edge = 0; Edge < SuccessorCount; ++Edge) {
        const size_t To = Random() % BlockCount;
        if (llvm::is_contained(Successors[From], To))
          continue;
        Successors[From].push_back(To);
        Predecessors[To].push_back(From);
      }
    }

    llvm::BitVector Reachable(BlockCount);
    std::vector<size_t> ReachabilityWork{Root};
    Reachable.set(Root);
    for (size_t Next = 0; Next < ReachabilityWork.size(); ++Next)
      for (size_t To : Successors[ReachabilityWork[Next]])
        if (!Reachable.test(To)) {
          Reachable.set(To);
          ReachabilityWork.push_back(To);
        }

    std::vector<llvm::BitVector> Expected(BlockCount,
                                          llvm::BitVector(BlockCount));
    Expected[Root].set(Root);
    for (size_t Block = 0; Block < BlockCount; ++Block)
      if (Block != Root && Reachable.test(Block))
        Expected[Block] = Reachable;

    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (size_t Block = 0; Block < BlockCount; ++Block) {
        if (Block == Root || !Reachable.test(Block))
          continue;
        llvm::BitVector Meet = Reachable;
        for (size_t Pred : Predecessors[Block])
          if (Reachable.test(Pred))
            Meet &= Expected[Pred];
        Meet.set(Block);
        if (Meet != Expected[Block]) {
          Expected[Block] = std::move(Meet);
          Changed = true;
        }
      }
    }

    const analyzer_detail::DominatorTree Actual =
        analyzer_detail::buildDominatorTree(Successors, Predecessors, Root);
    for (size_t Block = 0; Block < BlockCount; ++Block) {
      size_t ExpectedIDom = NoBlock;
      if (Block == Root) {
        ExpectedIDom = Root;
      } else if (Reachable.test(Block)) {
        for (size_t Candidate = 0; Candidate < BlockCount; ++Candidate) {
          if (Candidate == Block || !Expected[Block].test(Candidate))
            continue;
          bool IsNearest = true;
          for (size_t Other = 0; Other < BlockCount; ++Other)
            if (Other != Block && Other != Candidate &&
                Expected[Block].test(Other) &&
                !Expected[Candidate].test(Other)) {
              IsNearest = false;
              break;
            }
          if (IsNearest) {
            ExpectedIDom = Candidate;
            break;
          }
        }
      }
      if (Actual.IDom[Block] != ExpectedIDom) {
        ADD_FAILURE() << "seeded CFG " << GraphIndex << ", block " << Block
                      << ": expected idom " << ExpectedIDom << ", got "
                      << Actual.IDom[Block];
        return;
      }
    }

    for (size_t A = 0; A < BlockCount; ++A)
      for (size_t B = 0; B < BlockCount; ++B) {
        const bool ExpectedDominance = Reachable.test(B) && Expected[B].test(A);
        if (analyzer_detail::dominates(Actual, A, B) != ExpectedDominance) {
          ADD_FAILURE() << "seeded CFG " << GraphIndex << ": dominance " << A
                        << " -> " << B << " disagrees";
          return;
        }
      }

    for (size_t Query = 0; Query < CommonDominatorQueriesPerGraph; ++Query) {
      const size_t A = Random() % BlockCount;
      const size_t B = Random() % BlockCount;
      std::optional<size_t> ExpectedCommon;
      if (!Reachable.test(A) && !Reachable.test(B)) {
        ExpectedCommon = std::nullopt;
      } else if (!Reachable.test(A)) {
        ExpectedCommon = B;
      } else if (!Reachable.test(B)) {
        ExpectedCommon = A;
      } else {
        size_t GreatestDepth = 0;
        for (size_t Candidate = 0; Candidate < BlockCount; ++Candidate) {
          if (!Expected[A].test(Candidate) || !Expected[B].test(Candidate))
            continue;
          const size_t CandidateDepth = Expected[Candidate].count();
          if (!ExpectedCommon || CandidateDepth > GreatestDepth) {
            ExpectedCommon = Candidate;
            GreatestDepth = CandidateDepth;
          }
        }
      }
      const std::optional<size_t> ActualCommon =
          analyzer_detail::nearestCommonDominator(Actual, A, B);
      if (ActualCommon != ExpectedCommon) {
        ADD_FAILURE() << "seeded CFG " << GraphIndex
                      << ": nearest common dominator of " << A << " and " << B
                      << " disagrees";
        return;
      }
    }
  }
}

TEST(SBFAnalyzer, RepresentsFiveThousandNestedNaturalLoopsCompactly) {
  constexpr size_t LoopDepth = 5'000;
  constexpr size_t BlockCount = LoopDepth * 2 + 1;
  std::vector<EncodedInstruction> Instructions;
  Instructions.reserve(BlockCount);
  for (size_t Header = 0; Header < LoopDepth; ++Header) {
    const size_t AfterLatch = LoopDepth * 2 - Header;
    const int16_t ExitOffset = static_cast<int16_t>(AfterLatch - Header - 1);
    Instructions.push_back(encode(Opcode::JEQ64_IMM, 0, 0, ExitOffset));
  }
  for (size_t Reverse = LoopDepth; Reverse > 0; --Reverse) {
    const size_t Header = Reverse - 1;
    const size_t Latch = Instructions.size();
    const int16_t Backedge = static_cast<int16_t>(
        static_cast<int32_t>(Header) - static_cast<int32_t>(Latch) - 1);
    Instructions.push_back(encode(Opcode::JA, 0, 0, Backedge));
  }
  Instructions.push_back(encode(Opcode::EXIT));

  auto Program = analyze(makeImage(Version::V3, Instructions));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.Blocks.size(), BlockCount);
  size_t RecoveredLoopCount = 0;
  size_t ExplicitMemberCount = 0;
  const Region *Outermost = nullptr;
  const Region *Innermost = nullptr;
  for (const Region &Recovered : Program->High.Regions)
    if (Recovered.Kind == RegionKind::Loop) {
      ++RecoveredLoopCount;
      ExplicitMemberCount += Recovered.Blocks.size();
      EXPECT_TRUE(Recovered.Blocks.empty());
      EXPECT_EQ(Program->High.latches(Recovered).size(), 1u);
      EXPECT_TRUE(Program->High.loopContains(Recovered, Recovered.HeaderBlock));
      if (!Recovered.ParentRegion)
        Outermost = &Recovered;
      if (Recovered.BlockCount == 2)
        Innermost = &Recovered;
    }
  ASSERT_EQ(RecoveredLoopCount, LoopDepth);
  EXPECT_LE(ExplicitMemberCount, Program->Low.Blocks.size())
      << "the loop forest must not materialize every nested membership";
  ASSERT_NE(Outermost, nullptr);
  ASSERT_NE(Innermost, nullptr);
  EXPECT_EQ(Outermost->BlockCount, BlockCount - 1);
  EXPECT_EQ(Outermost->LoopSubtreeEnd - Outermost->LoopPreorder, LoopDepth);
  EXPECT_EQ(Innermost->LoopSubtreeEnd - Innermost->LoopPreorder, 1u);
  for (size_t Block = 0; Block + 1 < BlockCount; ++Block)
    EXPECT_TRUE(Program->High.loopContains(*Outermost, Block));
  EXPECT_FALSE(Program->High.loopContains(*Outermost, BlockCount - 1));

  const std::optional<StructuredControlFlow> Structured =
      buildStructuredControlFlow(*Program);
  ASSERT_TRUE(Structured);
  EXPECT_EQ(Structured->Nodes.size(), BlockCount);
  {
    auto Source = emitC(*Program);
    ASSERT_TRUE(static_cast<bool>(Source))
        << llvm::toString(Source.takeError());
    EXPECT_NE(Source->find("switch (pc)"), std::string::npos)
        << "deep loop syntax must use the exact portable C dispatcher";
  }
  {
    auto Source = emitRust(*Program);
    ASSERT_TRUE(static_cast<bool>(Source))
        << llvm::toString(Source.takeError());
    EXPECT_NE(Source->find("match pc"), std::string::npos)
        << "deep loop syntax must use the exact portable Rust dispatcher";
  }
}

TEST(SBFAnalyzer, GivesEveryFunctionEntryACanonicalStackFrame) {
  constexpr size_t CalleeEntrySlot = 1;
  BinaryImage Image = makeImage(
      Version::V3, {encode(Opcode::EXIT),
                    encode(Opcode::MOV64_REG, 0, kFramePointerRegister),
                    encode(Opcode::EXIT)});
  Image.addSymbol("callee", kBytecodeStart + kInstructionSize,
                  2 * kInstructionSize, true);

  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const auto Callee = std::find_if(
      Program->Med.Blocks.begin(), Program->Med.Blocks.end(),
      [](const MedBlock &Block) { return Block.StartSlot == CalleeEntrySlot; });
  ASSERT_NE(Callee, Program->Med.Blocks.end());
  EXPECT_EQ(Callee->Inputs[kFramePointerRegister].ValueKind,
            RegisterValue::Kind::StackAddress);
  EXPECT_EQ(Callee->Inputs[kFirstArgumentRegister].ValueKind,
            RegisterValue::Kind::Unknown)
      << "only the loader entry owns the serialized-input ABI fact";
}

TEST(SBFAnalyzer, MeetsEntryABIFactsWithRealLoopBackedges) {
  auto Program = analyze(makeImage(
      Version::V3, {encode(Opcode::JEQ64_IMM, 0, 0, 2),
                    encode(Opcode::MOV64_IMM, kFirstArgumentRegister, 0, 0, 42),
                    encode(Opcode::JA, 0, 0, -3), encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_FALSE(Program->Med.Blocks.empty());
  EXPECT_EQ(
      Program->Med.Blocks.front().Inputs[kFirstArgumentRegister].ValueKind,
      RegisterValue::Kind::Unknown)
      << "the virtual loader predecessor must not hide a clobbering backedge";
}

TEST(SBFAnalyzer, RegisterBoundariesUseRootScopedSeedStorage) {
  constexpr size_t BlockCount = 10'000;
  SBFProgram Program;
  Program.Low.TheVersion = Version::V3;
  Program.Low.EntrySlot = 0;
  Program.Low.Instructions.resize(BlockCount);
  Program.Low.Blocks.resize(BlockCount);
  Program.Med.Blocks.resize(BlockCount);
  Program.Low.Edges.reserve(BlockCount - 1);
  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    BasicBlock &Low = Program.Low.Blocks[BlockID];
    Low.ID = BlockID;
    Low.StartSlot = BlockID;
    Low.EndSlot = BlockID + 1;
    MedBlock &Med = Program.Med.Blocks[BlockID];
    Med.ID = BlockID;
    Med.StartSlot = BlockID;
    Med.EndSlot = BlockID + 1;
    if (BlockID + 1 < BlockCount)
      Program.Low.Edges.push_back(
          {BlockID, BlockID + 1, EdgeKind::Fallthrough});
  }
  llvm::BitVector FunctionEntries(BlockCount);
  FunctionEntries.set(0);

  const analyzer_detail::RegisterDataflowStatistics Stats =
      analyzer_detail::runRegisterDataflow(Program, FunctionEntries);
  EXPECT_EQ(Stats.BlockCount, BlockCount);
  EXPECT_EQ(Stats.RootCount, 1u);
  EXPECT_EQ(Stats.FunctionRootCount, 1u);
  EXPECT_EQ(Stats.BoundarySeedCount, 1u);
  EXPECT_EQ(Stats.PeakBoundaryWorkspaceEntryCount, 1u)
      << "boundary state storage must scale with roots, not all blocks";
  EXPECT_EQ(Program.Med.Blocks.back().Inputs[kFramePointerRegister].ValueKind,
            RegisterValue::Kind::StackAddress);
}

TEST(SBFAnalyzer, ScratchFlowRootsTheLoaderEntryWithoutHighIR) {
  constexpr int16_t StackByteOffset = -1;
  constexpr int32_t StoredByte = 42;
  AnalyzeOptions Options;
  Options.RecoverHighIR = false;
  auto Program = analyze(
      makeImage(Version::V3, {encode(Opcode::ST_B_IMM, kFramePointerRegister, 0,
                                     StackByteOffset, StoredByte),
                              encode(Opcode::JA), encode(Opcode::EXIT)}),
      Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_TRUE(Program->High.Functions.empty());
  const auto ExitBlock =
      std::find_if(Program->Med.Blocks.begin(), Program->Med.Blocks.end(),
                   [](const MedBlock &Block) { return Block.StartSlot == 2; });
  ASSERT_NE(ExitBlock, Program->Med.Blocks.end());

  const MedInstructionIndex Index(Program->Med);
  const ScratchFlow Flow(*Program, Index);
  const ScratchFlow::Statistics &Stats = Flow.statistics();
  const size_t IndexedEdgeCount = static_cast<size_t>(
      std::count_if(Program->Low.Edges.begin(), Program->Low.Edges.end(),
                    [&](const CFGEdge &Edge) {
                      return Edge.To && *Edge.To < Program->Low.Blocks.size() &&
                             Edge.From < Program->Low.Blocks.size() &&
                             getEdgeKindInfo(Edge.Kind).IsIntraprocedural;
                    }));
  EXPECT_EQ(Stats.IndexedBlockCount, Program->Low.Blocks.size());
  EXPECT_EQ(Stats.IndexedEdgeCount, IndexedEdgeCount);
  EXPECT_EQ(Stats.SuccessorIndexEntryCount,
            Program->Low.Blocks.size() + 1 + IndexedEdgeCount)
      << "scratch CFG storage must be flat O(B + E), not B SmallVectors";
  const va_t StoredAddress =
      initialFramePointer(Program->Low.TheVersion, Program->Config) +
      StackByteOffset;
  const llvm::ArrayRef<uint8_t> Bytes =
      Flow.entryState(ExitBlock->ID).Memory.read(StoredAddress, 1);
  ASSERT_EQ(Bytes.size(), 1u);
  EXPECT_EQ(Bytes.front(), static_cast<uint8_t>(StoredByte));
}

TEST(SBFAnalyzer, PropagatesScratchFactsAcrossTwentyThousandBlocks) {
  constexpr size_t ChainBlockCount = 20'000;
  constexpr int16_t StackByteOffset = -1;
  constexpr int32_t StoredByte = 42;
  std::vector<EncodedInstruction> Instructions;
  Instructions.reserve(ChainBlockCount + 2);
  Instructions.push_back(encode(Opcode::EXIT));
  for (size_t Slot = 1; Slot < ChainBlockCount; ++Slot)
    Instructions.push_back(encode(Opcode::JA, 0, 0, -2));
  const size_t EntrySlot = Instructions.size();
  Instructions.push_back(encode(Opcode::ST_B_IMM, kFramePointerRegister, 0,
                                StackByteOffset, StoredByte));
  Instructions.push_back(encode(Opcode::JA, 0, 0, -3));

  auto Program = analyze(makeImage(Version::V3, Instructions, EntrySlot));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_GT(Program->Low.Blocks.size(), ChainBlockCount);

  const MedInstructionIndex Index(Program->Med);
  const ScratchFlow Flow(*Program, Index);
  const va_t StoredAddress =
      initialFramePointer(Program->Low.TheVersion, Program->Config) +
      StackByteOffset;
  const llvm::ArrayRef<uint8_t> Bytes =
      Flow.entryState(Program->Med.Blocks.front().ID)
          .Memory.read(StoredAddress, 1);
  ASSERT_EQ(Bytes.size(), 1u);
  EXPECT_EQ(Bytes.front(), static_cast<uint8_t>(StoredByte));
}

TEST(SBFAnalyzer, ScratchFactDemandIsLimitedToRuntimeCPIAndPDAConsumers) {
  MedInstruction MemorySyscall;
  MemorySyscall.Call = CallKind::Syscall;
  MemorySyscall.Syscall = getSyscallInfo(Syscall::Memset);
  ASSERT_NE(MemorySyscall.Syscall, nullptr);
  EXPECT_FALSE(consumesScratchFacts(MemorySyscall));

  MedInstruction Invocation;
  Invocation.Call = CallKind::Syscall;
  Invocation.Syscall = getSyscallInfo(Syscall::InvokeSignedRust);
  ASSERT_NE(Invocation.Syscall, nullptr);
  EXPECT_TRUE(consumesScratchFacts(Invocation));

  MedInstruction Derivation;
  Derivation.Call = CallKind::Syscall;
  Derivation.Syscall = getSyscallInfo(Syscall::CreateProgramAddress);
  ASSERT_NE(Derivation.Syscall, nullptr);
  EXPECT_TRUE(consumesScratchFacts(Derivation));

  Invocation.Call = CallKind::Internal;
  Invocation.Dispatch = CallDispatchPolicy::LegacyRuntimeThenFunction;
  EXPECT_TRUE(consumesScratchFacts(Invocation))
      << "legacy collision dispatch still executes the runtime syscall";
}

TEST(SBFAnalyzer, ScratchFlowWidensHostileRetainedStateToSoundUnknown) {
  constexpr size_t SeedStoreCount = kMaxModeledScratchBytes / sizeof(uint64_t);
  constexpr size_t BlockCount =
      kScratchFlowRetainedByteBudget / kMaxModeledScratchBytes +
      SeedStoreCount + 16;
  constexpr va_t ScratchBase = kStackStart + kDefaultStackFrameSize;

  SBFProgram Program;
  Program.Low.TheVersion = Version::V3;
  Program.Low.EntrySlot = 0;
  Program.Low.Blocks.resize(BlockCount);
  Program.Med.Blocks.resize(BlockCount);
  Program.Med.Instructions.reserve(BlockCount);
  Program.Low.Edges.reserve(BlockCount - 1);

  for (size_t BlockID = 0; BlockID < BlockCount; ++BlockID) {
    BasicBlock &Low = Program.Low.Blocks[BlockID];
    Low.ID = BlockID;
    Low.StartSlot = BlockID;
    Low.EndSlot = BlockID + 1;

    MedBlock &Med = Program.Med.Blocks[BlockID];
    Med.ID = BlockID;
    Med.StartSlot = BlockID;
    Med.EndSlot = BlockID + 1;
    Med.Inputs[kFirstArgumentRegister] = {RegisterValue::Kind::Constant,
                                          ScratchBase, 0};

    MedInstruction Instruction;
    Instruction.Slot = BlockID;
    if (BlockID + 1 == BlockCount) {
      Instruction.Op = Operation::Call;
      Instruction.Form = OperandForm::CallImm;
      Instruction.Call = CallKind::Syscall;
      Instruction.Syscall = getSyscallInfo(Syscall::InvokeSignedRust);
      ASSERT_NE(Instruction.Syscall, nullptr);
    } else {
      Instruction.Op = Operation::Store;
      Instruction.Form = OperandForm::StoreImm;
      Instruction.Width = kDoubleWordBitWidth;
      Instruction.Dst = kFirstArgumentRegister;
      if (BlockID < SeedStoreCount) {
        Instruction.Offset = static_cast<int16_t>(BlockID * sizeof(uint64_t));
        Instruction.Immediate = BlockID;
      } else {
        // Once all 1 KiB are known, alternate one byte forever. The semantic
        // state changes at every block while retaining the maximum payload.
        Instruction.Width = kBitsPerByte;
        Instruction.Immediate = BlockID & 1u;
      }
    }
    Program.Med.Instructions.push_back(Instruction);

    if (BlockID + 1 < BlockCount)
      Program.Low.Edges.push_back(
          {BlockID, BlockID + 1, EdgeKind::Fallthrough});
  }

  const MedInstructionIndex Index(Program.Med);
  const ScratchFlow Flow(Program, Index);
  const ScratchFlow::Statistics &Stats = Flow.statistics();
  EXPECT_EQ(Stats.Precision, ScratchFlowPrecision::WidenedToUnknown);
  EXPECT_LE(Stats.PeakRetainedByteEstimate, kScratchFlowRetainedByteBudget);
  EXPECT_EQ(Stats.RetainedByteEstimate, 0u);
  EXPECT_TRUE(Flow.entryState(BlockCount - 1).Memory.empty())
      << "widening may lose precision but must never retain a must-fact";

  const SolanaModel Model = recoverSolanaModel(Program);
  EXPECT_EQ(Model.ScratchPrecision, ScratchRecoveryPrecision::BlockLocal);
  EXPECT_FALSE(Model.empty());
  EXPECT_NE(dumpSolanaModel(Model).find("scratch-precision=block-local"),
            std::string::npos);
}

TEST(SBFAnalyzer, ScratchResidentEstimateChargesSparseRunNodes) {
  constexpr size_t FactCount = 64;
  constexpr va_t DenseBase = kStackStart + kDefaultStackFrameSize;
  constexpr va_t SparseBase = DenseBase + kDefaultStackFrameSize;
  constexpr std::array<uint8_t, FactCount> Bytes{};

  MemoryModel Dense;
  Dense.write(DenseBase, Bytes);
  MemoryModel Sparse;
  for (size_t Index = 0; Index < FactCount; ++Index)
    Sparse.write(SparseBase + 2 * Index,
                 llvm::ArrayRef<uint8_t>(Bytes).slice(Index, 1));

  ASSERT_EQ(Dense.trackedBytes(), Sparse.trackedBytes());
  EXPECT_GT(Dense.retainedByteEstimate(), Dense.trackedBytes());
  EXPECT_GT(Sparse.retainedByteEstimate(), Dense.retainedByteEstimate())
      << "the budget must charge map/vector overhead, not payload alone";
}

TEST(SBFAnalyzer, ScratchMemmoveSnapshotsAnOverlappingSourceBeforeWrites) {
  constexpr va_t Source = kStackStart + 64;
  constexpr va_t Destination = Source + 1;
  constexpr uint64_t Length = 4;
  constexpr std::array<uint8_t, 5> Initial = {1, 2, 3, 4, 5};
  constexpr std::array<uint8_t, Length> Expected = {1, 2, 3, 4};

  MachineState State;
  State.Registers[argumentRegister(SyscallArgument::Arg1)] = {
      RegisterValue::Kind::Constant, Destination, 0};
  State.Registers[argumentRegister(SyscallArgument::Arg2)] = {
      RegisterValue::Kind::Constant, Source, 0};
  State.Registers[argumentRegister(SyscallArgument::Arg3)] = {
      RegisterValue::Kind::Constant, Length, 0};
  State.Scratch.Memory.write(Source, Initial);

  MedInstruction Move;
  Move.Call = CallKind::Syscall;
  Move.Syscall = getSyscallInfo(Syscall::Memmove);
  ASSERT_NE(Move.Syscall, nullptr);
  applyTransfer(Move, State, ProgramImage{});

  EXPECT_EQ(State.Scratch.Memory.read(Destination, Length),
            llvm::ArrayRef<uint8_t>(Expected));
}

TEST(SBFAnalyzer, ScratchStoreCrossingAVMRegionDoesNotCreateFacts) {
  constexpr va_t LastStackByte = kStackStart + kMemoryRegionSize - 1;

  MachineState State;
  State.Registers[1] = {RegisterValue::Kind::Constant, LastStackByte, 0};
  State.Registers[2] = {RegisterValue::Kind::Constant, 42, 0};

  MedInstruction Store;
  Store.Op = Operation::Store;
  Store.Form = OperandForm::StoreReg;
  Store.Width = kDoubleWordBitWidth;
  Store.Dst = 1;
  Store.Src = 2;
  applyTransfer(Store, State, ProgramImage{});

  EXPECT_TRUE(State.Scratch.Memory.empty())
      << "a runtime-faulting store cannot prove bytes across VM regions";
}

TEST(SBFAnalyzer, ScratchMemsetCrossingAVMRegionDoesNotCreateFacts) {
  constexpr va_t LastStackByte = kStackStart + kMemoryRegionSize - 1;
  constexpr va_t ExistingFact = kHeapStart + kInstructionSize;
  constexpr std::array<uint8_t, 1> Initial = {7};

  MachineState State;
  State.Registers[argumentRegister(SyscallArgument::Arg1)] = {
      RegisterValue::Kind::Constant, LastStackByte, 0};
  State.Registers[argumentRegister(SyscallArgument::Arg2)] = {
      RegisterValue::Kind::Constant, 42, 0};
  State.Registers[argumentRegister(SyscallArgument::Arg3)] = {
      RegisterValue::Kind::Constant, 2, 0};
  State.Scratch.Memory.write(ExistingFact, Initial);

  MedInstruction Set;
  Set.Call = CallKind::Syscall;
  Set.Syscall = getSyscallInfo(Syscall::Memset);
  ASSERT_NE(Set.Syscall, nullptr);
  applyTransfer(Set, State, ProgramImage{});

  EXPECT_TRUE(State.Scratch.Memory.empty())
      << "a guaranteed fault has no reachable successor scratch facts";
}

TEST(SBFAnalyzer, CountedSyscallWriteOverflowInvalidatesScratchFacts) {
  constexpr va_t Destination = kStackStart + kInstructionSize;
  constexpr std::array<uint8_t, 1> Initial = {42};

  MachineState State;
  State.Registers[argumentRegister(SyscallArgument::Arg1)] = {
      RegisterValue::Kind::Constant, Destination, 0};
  State.Registers[argumentRegister(SyscallArgument::Arg2)] = {
      RegisterValue::Kind::Constant, 0, 0};
  State.Registers[argumentRegister(SyscallArgument::Arg3)] = {
      RegisterValue::Kind::Constant, std::numeric_limits<uint64_t>::max(), 0};
  State.Scratch.Memory.write(Destination, Initial);

  MedInstruction Set;
  Set.Call = CallKind::Syscall;
  Set.Syscall = getSyscallInfo(Syscall::Memset);
  ASSERT_NE(Set.Syscall, nullptr);
  applyTransfer(Set, State, ProgramImage{});

  EXPECT_TRUE(State.Scratch.Memory.empty());
}

TEST(SBFAnalyzer, InternalCallInvalidatesScratchWithoutPointerArguments) {
  constexpr va_t TrackedAddress = kHeapStart + kInstructionSize;
  constexpr std::array<uint8_t, 1> Initial = {42};

  MachineState State;
  for (unsigned Ordinal = 0; Ordinal < kArgumentRegisterCount; ++Ordinal)
    State.Registers[kFirstArgumentRegister + Ordinal] = {
        RegisterValue::Kind::Constant, 0, 0};
  State.Scratch.Memory.write(TrackedAddress, Initial);

  MedInstruction Call;
  Call.Call = CallKind::Internal;
  applyTransfer(Call, State, ProgramImage{});

  EXPECT_TRUE(State.Scratch.Memory.empty());
}

TEST(SBFAnalyzer, SharedFunctionTailsHaveNoOrderDependentOwner) {
  constexpr size_t OtherEntrySlot = 2;
  constexpr size_t SharedTailSlot = 4;
  BinaryImage Image =
      makeImage(Version::V3,
                {encode(Opcode::JA, 0, 0, 3), encode(Opcode::EXIT),
                 encode(Opcode::JA, 0, 0, 1), encode(Opcode::EXIT),
                 encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)});
  Image.addSymbol("other", kBytecodeStart + OtherEntrySlot * kInstructionSize,
                  2 * kInstructionSize, true);

  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const auto Tail =
      std::find_if(Program->Low.Blocks.begin(), Program->Low.Blocks.end(),
                   [](const BasicBlock &Block) {
                     return Block.StartSlot == SharedTailSlot;
                   });
  ASSERT_NE(Tail, Program->Low.Blocks.end());
  ASSERT_LT(Tail->ID, Program->High.BlockOwners.size());
  EXPECT_EQ(Program->High.BlockOwners[Tail->ID], HighIR::AmbiguousFunction);
  EXPECT_EQ(Program->High.functionForBlock(Tail->ID), nullptr);
  for (const Function &Function : Program->High.Functions) {
    const llvm::ArrayRef<size_t> Blocks = Program->High.ownedBlocks(Function);
    EXPECT_EQ(std::find(Blocks.begin(), Blocks.end(), Tail->ID), Blocks.end());
  }
  EXPECT_TRUE(std::any_of(
      Program->Low.Diagnostics.begin(), Program->Low.Diagnostics.end(),
      [](const Diagnostic &Diagnostic) {
        return Diagnostic.Slot == SharedTailSlot &&
               Diagnostic.Message.find("multiple function entries") !=
                   std::string::npos;
      }));
}

TEST(SBFAnalyzer, TheEntryReceivesInstructionDataOnlyWhereItIsActivated) {
  const auto Instructions = {encode(Opcode::MOV64_REG, 0, 2),
                             encode(Opcode::EXIT)};

  AnalyzeOptions Activated;
  Activated.Profile = currentMainnetProfile();
  auto WithPointer = analyze(makeImage(Version::V3, Instructions), Activated);
  ASSERT_TRUE(static_cast<bool>(WithPointer))
      << llvm::toString(WithPointer.takeError());
  const RegisterValue &Live =
      WithPointer->Med.Blocks.front().Inputs[kInstructionDataRegister];
  // Where the instruction data lands depends on the accounts, so it is its own
  // kind of value. Calling it a constant would let a load through it be
  // reported as a named account field.
  EXPECT_EQ(Live.ValueKind, RegisterValue::Kind::InstructionDataAddress);

  AnalyzeOptions Earlier = Activated;
  Earlier.Profile.Suppressed = RuntimeFeature::InstructionDataPointer;
  auto WithoutPointer = analyze(makeImage(Version::V3, Instructions), Earlier);
  ASSERT_TRUE(static_cast<bool>(WithoutPointer))
      << llvm::toString(WithoutPointer.takeError());
  const RegisterValue &Zero =
      WithoutPointer->Med.Blocks.front().Inputs[kInstructionDataRegister];
  EXPECT_EQ(Zero.ValueKind, RegisterValue::Kind::Constant);
  EXPECT_EQ(Zero.Value, 0u);
}

TEST(SBFAnalyzer, NormalizesTheNonMonotonicV2Semantics) {
  const auto Instructions = {
      encode(Opcode::MOV32_REG, 1, 2), encode(Opcode::SUB64_IMM, 1, 0, 0, 7),
      encode(Opcode::CALL_REG, 0, 7, 0, 9), encode(Opcode::EXIT)};
  auto V2 = analyze(makeImage(Version::V2, Instructions));
  ASSERT_TRUE(static_cast<bool>(V2)) << llvm::toString(V2.takeError());
  EXPECT_EQ(V2->Med.Instructions[0].Semantics.Result, ResultExtension::Sign32);
  EXPECT_TRUE(V2->Med.Instructions[1].Semantics.SwapOperands);
  EXPECT_EQ(V2->Low.Instructions[2].CallRegister, 7u);

  const auto V3Instructions = {
      encode(Opcode::MOV32_REG, 1, 2), encode(Opcode::SUB64_IMM, 1, 0, 0, 7),
      encode(Opcode::CALL_REG, 8, 0, 0, 9), encode(Opcode::EXIT)};
  auto V3 = analyze(makeImage(Version::V3, V3Instructions));
  ASSERT_TRUE(static_cast<bool>(V3)) << llvm::toString(V3.takeError());
  EXPECT_EQ(V3->Med.Instructions[0].Semantics.Result, ResultExtension::Zero32);
  EXPECT_FALSE(V3->Med.Instructions[1].Semantics.SwapOperands);
  EXPECT_EQ(V3->Low.Instructions[2].CallRegister, 8u);
}

TEST(SBFAnalyzer, RefinesAProvenConstantCallXIntoAnExactCallEdge) {
  constexpr size_t TargetSlot = 5;
  constexpr uint64_t CallAddress =
      kBytecodeStart + TargetSlot * kInstructionSize + (kInstructionSize - 1);
  static_assert((CallAddress >> kWordBitWidth) == 1);

  // V2 obtains CALLX's target from src and constructs 64-bit constants with
  // MOV64_IMM + HOR64_IMM.  Slot 5 initially lies in the middle of the block
  // beginning at slot 4, so target recovery must repartition the CFG as well
  // as annotating the instruction.
  auto Program = analyze(makeImage(
      Version::V2,
      {encode(Opcode::MOV64_IMM, 8, 0, 0, static_cast<int32_t>(CallAddress)),
       encode(Opcode::HOR64_IMM, 8, 0, 0,
              static_cast<int32_t>(CallAddress >> kWordBitWidth)),
       encode(Opcode::CALL_REG, 0, 8), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 1),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->Low.Instructions[2].Call, CallKind::Internal);
  ASSERT_EQ(Program->Low.Instructions[2].CallTarget,
            std::optional<size_t>(TargetSlot));
  const auto TargetBlock = std::find_if(
      Program->Low.Blocks.begin(), Program->Low.Blocks.end(),
      [](const BasicBlock &Block) { return Block.StartSlot == TargetSlot; });
  ASSERT_NE(TargetBlock, Program->Low.Blocks.end());
  EXPECT_TRUE(std::any_of(Program->Low.Edges.begin(), Program->Low.Edges.end(),
                          [&](const CFGEdge &Edge) {
                            return Edge.Kind == EdgeKind::Call &&
                                   Edge.To == TargetBlock->ID;
                          }));
  const Function *Target = findFunction(*Program, "0x100000028");
  ASSERT_NE(Target, nullptr);
  EXPECT_EQ(Target->EntrySlot, TargetSlot);
}

TEST(SBFAnalyzer, RevokesACallXProofInvalidatedByItsFunctionBoundary) {
  constexpr uint8_t TargetRegister = 8;
  constexpr size_t BranchSlot = 2;
  constexpr size_t TargetSlot = 4;
  constexpr size_t CallSlot = TargetSlot;
  constexpr int16_t BranchOffset =
      static_cast<int16_t>(TargetSlot - BranchSlot - 1);
  constexpr uint64_t CallAddress =
      kBytecodeStart + TargetSlot * kInstructionSize;
  static_assert((CallAddress >> kWordBitWidth) == 1);

  // The ordinary branch initially carries r8's exact value into slot 4, so a
  // first intraprocedural pass can mistake the self-call for a proven target.
  // Once that candidate makes slot 4 a function boundary, its virtual ABI
  // predecessor contributes Unknown for r8 and invalidates the proof.
  auto Program = analyze(makeImage(
      Version::V2,
      {encode(Opcode::MOV64_IMM, TargetRegister, 0, 0,
              static_cast<int32_t>(CallAddress)),
       encode(Opcode::HOR64_IMM, TargetRegister, 0, 0,
              static_cast<int32_t>(CallAddress >> kWordBitWidth)),
       encode(Opcode::JA, 0, 0, BranchOffset), encode(Opcode::EXIT),
       encode(Opcode::CALL_REG, 0, TargetRegister), encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  const LowInstruction &Call = Program->Low.Instructions[CallSlot];
  EXPECT_EQ(Call.Call, CallKind::Indirect);
  EXPECT_FALSE(Call.CallTarget.has_value());

  const auto CallBlock = std::find_if(
      Program->Med.Blocks.begin(), Program->Med.Blocks.end(),
      [](const MedBlock &Block) { return Block.StartSlot == CallSlot; });
  ASSERT_NE(CallBlock, Program->Med.Blocks.end());
  EXPECT_EQ(CallBlock->Inputs[TargetRegister].ValueKind,
            RegisterValue::Kind::Unknown);
  EXPECT_TRUE(std::any_of(Program->Low.Edges.begin(), Program->Low.Edges.end(),
                          [&](const CFGEdge &Edge) {
                            return Edge.To == CallBlock->ID &&
                                   Edge.Kind == EdgeKind::Branch;
                          }));

  EXPECT_TRUE(std::any_of(Program->Low.Edges.begin(), Program->Low.Edges.end(),
                          [&](const CFGEdge &Edge) {
                            return Edge.From == CallBlock->ID &&
                                   Edge.Kind == EdgeKind::IndirectCall;
                          }));
  EXPECT_FALSE(std::any_of(Program->Low.Edges.begin(), Program->Low.Edges.end(),
                           [&](const CFGEdge &Edge) {
                             return Edge.From == CallBlock->ID &&
                                    Edge.Kind == EdgeKind::Call;
                           }));
}

TEST(SBFAnalyzer, RejectsVerifierFailuresWithSlotAndAddress) {
  auto BadFrame = analyze(makeImage(
      Version::V3, {encode(Opcode::MOV64_IMM, kFramePointerRegister, 0, 0, 1),
                    encode(Opcode::EXIT)}));
  ASSERT_FALSE(static_cast<bool>(BadFrame));
  std::string Error = llvm::toString(BadFrame.takeError());
  EXPECT_NE(Error.find("instruction 0"), std::string::npos);
  EXPECT_NE(Error.find("0x100000000"), std::string::npos);
  EXPECT_NE(Error.find("frame pointer"), std::string::npos);

  auto DivideByZero =
      analyze(makeImage(Version::V2, {encode(Opcode::UDIV64_IMM, 0, 0, 0, 0),
                                      encode(Opcode::EXIT)}));
  ASSERT_FALSE(static_cast<bool>(DivideByZero));
  EXPECT_NE(llvm::toString(DivideByZero.takeError()).find("division"),
            std::string::npos);
}

TEST(SBFAnalyzer, UsesTheRealEntryBlockForDataflowAndStructuring) {
  auto Program = analyze(
      makeImage(Version::V3,
                {encode(Opcode::EXIT), encode(Opcode::MOV64_IMM, 0, 0, 0, 0),
                 encode(Opcode::JEQ64_IMM, 0, 0, 1, 0),
                 encode(Opcode::MOV64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT)},
                1));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_GE(Program->Med.Blocks.size(), 4u);
  EXPECT_EQ(Program->Med.Blocks[1].Inputs[kFramePointerRegister].ValueKind,
            RegisterValue::Kind::StackAddress);
  ASSERT_FALSE(Program->High.Regions.empty());
  const Region &If = Program->High.Regions.back();
  EXPECT_EQ(If.Kind, RegionKind::If);
  EXPECT_EQ(If.HeaderBlock, 1u);
  ASSERT_TRUE(If.ExitBlock.has_value());
  EXPECT_EQ(*If.ExitBlock, 3u);
}

TEST(SBFAnalyzer, LeavesDisjointBranchExitsUnjoined) {
  auto Program = analyze(
      makeImage(Version::V3, {encode(Opcode::JEQ64_IMM, 0, 0, 1, 0),
                              encode(Opcode::EXIT), encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Regions.size(), 1u);
  const Region &If = Program->High.Regions.front();
  EXPECT_EQ(If.Kind, RegionKind::If);
  EXPECT_FALSE(If.ExitBlock.has_value());
}

TEST(SBFAnalyzer, KeepsADeadPredecessorOutOfASelfLoop) {
  auto Program = analyze(makeImage(
      Version::V3,
      {encode(Opcode::JA, 0, 0, 0), encode(Opcode::MOV64_IMM, 0, 0, 0, 0),
       encode(Opcode::JLT64_IMM, 0, 0, -2, 1), encode(Opcode::EXIT)},
      1));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const auto Loop =
      std::find_if(Program->High.Regions.begin(), Program->High.Regions.end(),
                   [](const Region &Candidate) {
                     return Candidate.Kind == RegionKind::Loop;
                   });
  ASSERT_NE(Loop, Program->High.Regions.end());
  EXPECT_TRUE(Loop->Blocks.empty());
  EXPECT_EQ(Loop->BlockCount, 1u);
  EXPECT_TRUE(Program->High.loopContains(*Loop, Loop->HeaderBlock));
  EXPECT_FALSE(Program->Low.Blocks.front().Reachable);
  EXPECT_NE(Loop->HeaderBlock, Program->Low.Blocks.front().ID);
}

TEST(SBFAnalyzer, HighOrUpdatesThePreviousDataflowValue) {
  auto Program =
      analyze(makeImage(Version::V2, {encode(Opcode::MOV64_IMM, 1, 0, 0, 7),
                                      encode(Opcode::HOR64_IMM, 1, 0, 0, 1),
                                      encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Med.Blocks.size(), 1u);
  EXPECT_EQ(Program->Med.Blocks.front().Outputs[1].ValueKind,
            RegisterValue::Kind::Constant);
  EXPECT_EQ(Program->Med.Blocks.front().Outputs[1].Value,
            UINT64_C(0x100000007));
}

TEST(SBFAnalyzer, DataflowUsesNormalizedWidthAndVersionSemantics) {
  auto V3 =
      analyze(makeImage(Version::V3, {encode(Opcode::MOV32_IMM, 1, 0, 0, -1),
                                      encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(V3)) << llvm::toString(V3.takeError());
  ASSERT_EQ(V3->Med.Blocks.size(), 1u);
  EXPECT_EQ(V3->Med.Blocks.front().Outputs[1].ValueKind,
            RegisterValue::Kind::Constant);
  EXPECT_EQ(V3->Med.Blocks.front().Outputs[1].Value, UINT64_C(0xffffffff));

  auto V2 =
      analyze(makeImage(Version::V2, {encode(Opcode::MOV64_IMM, 1, 0, 0, 5),
                                      encode(Opcode::SUB64_IMM, 1, 0, 0, 9),
                                      encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(V2)) << llvm::toString(V2.takeError());
  ASSERT_EQ(V2->Med.Blocks.size(), 1u);
  EXPECT_EQ(V2->Med.Blocks.front().Outputs[1].ValueKind,
            RegisterValue::Kind::Constant);
  EXPECT_EQ(V2->Med.Blocks.front().Outputs[1].Value, 4u);
}

TEST(SBFAnalyzer, AppliesLegacyTextRelocationsBeforeDecoding) {
  EncodedInstruction Continuation{};
  BinaryImage AddressImage =
      makeImage(Version::V0, {encode(Opcode::LDDW, 1, 0, 0, 0x20), Continuation,
                              encode(Opcode::EXIT)});
  Symbol Target;
  Target.Name = "target_data";
  Target.Addr = kBytecodeStart + 0x200;
  AddressImage.Symbols.push_back(Target);
  RelocationEntry AddressRelocation;
  AddressRelocation.Address = kBytecodeStart;
  AddressRelocation.Type = static_cast<uint32_t>(Relocation::Abs64);
  AddressRelocation.SymbolName = Target.Name;
  AddressRelocation.ELF =
      exactELFSymbol(Target.Addr - kBytecodeStart, 1, llvm::ELF::STT_NOTYPE);
  AddressImage.Relocations.push_back(AddressRelocation);

  auto AddressProgram = analyze(AddressImage);
  ASSERT_TRUE(static_cast<bool>(AddressProgram))
      << llvm::toString(AddressProgram.takeError());
  const uint64_t ExpectedAddress = Target.Addr + 0x20;
  EXPECT_EQ(AddressProgram->Low.Instructions[0].Immediate, ExpectedAddress);
  EXPECT_EQ(llvm::support::endian::read32le(AddressProgram->text().data() +
                                            kImmediateOffset),
            static_cast<uint32_t>(ExpectedAddress));
  EXPECT_EQ(llvm::support::endian::read32le(AddressProgram->text().data() +
                                            kInstructionSize +
                                            kImmediateOffset),
            static_cast<uint32_t>(ExpectedAddress >> 32));

  BinaryImage RelativeImage =
      makeImage(Version::V0, {encode(Opcode::LDDW, 1, 0, 0, 0x300),
                              Continuation, encode(Opcode::EXIT)});
  RelocationEntry RelativeRelocation;
  RelativeRelocation.Address = kBytecodeStart;
  RelativeRelocation.Type = static_cast<uint32_t>(Relocation::Relative64);
  RelativeImage.Relocations.push_back(RelativeRelocation);
  auto RelativeProgram = analyze(RelativeImage);
  ASSERT_TRUE(static_cast<bool>(RelativeProgram))
      << llvm::toString(RelativeProgram.takeError());
  EXPECT_EQ(RelativeProgram->Low.Instructions[0].Immediate,
            kBytecodeStart + 0x300);

  BinaryImage CallImage =
      makeImage(Version::V0,
                {encode(Opcode::CALL_IMM, 0, 0, 0, -1), encode(Opcode::EXIT)});
  RelocationEntry CallRelocation;
  CallRelocation.Address = kBytecodeStart;
  CallRelocation.Type = static_cast<uint32_t>(Relocation::Call32);
  CallRelocation.SymbolName = "sol_log_64_";
  CallRelocation.ELF =
      exactELFSymbol(0, llvm::ELF::SHN_UNDEF, llvm::ELF::STT_NOTYPE,
                     CallRelocation.SymbolName);
  CallImage.Relocations.push_back(CallRelocation);

  auto CallProgram = analyze(CallImage);
  ASSERT_TRUE(static_cast<bool>(CallProgram))
      << llvm::toString(CallProgram.takeError());
  ASSERT_EQ(CallProgram->Low.Instructions[0].Call, CallKind::Syscall);
  EXPECT_EQ(CallProgram->Low.Instructions[0].SyscallHash,
            hashSymbolName(CallRelocation.SymbolName));
  EXPECT_EQ(llvm::support::endian::read32le(CallProgram->text().data() +
                                            kImmediateOffset),
            hashSymbolName(CallRelocation.SymbolName));

  BinaryImage InternalImage =
      makeImage(Version::V0,
                {encode(Opcode::CALL_IMM, 0, 0, 0, -1), encode(Opcode::EXIT)});
  Symbol Function;
  Function.Name = "local_function";
  Function.Addr = kBytecodeStart + kInstructionSize;
  Function.IsFunc = true;
  InternalImage.Symbols.push_back(Function);
  RelocationEntry InternalRelocation;
  InternalRelocation.Address = kBytecodeStart;
  InternalRelocation.Type = static_cast<uint32_t>(Relocation::Call32);
  InternalRelocation.SymbolName = Function.Name;
  InternalRelocation.ELF =
      exactELFSymbol(Function.Addr - kBytecodeStart, 1, llvm::ELF::STT_FUNC,
                     InternalRelocation.SymbolName);
  InternalImage.Relocations.push_back(InternalRelocation);
  auto InternalProgram = analyze(InternalImage);
  ASSERT_TRUE(static_cast<bool>(InternalProgram))
      << llvm::toString(InternalProgram.takeError());
  ASSERT_EQ(InternalProgram->Low.Instructions[0].Call, CallKind::Internal);
  ASSERT_EQ(InternalProgram->Low.Instructions[0].CallTarget, 1u);
  std::array<uint8_t, sizeof(uint64_t)> TargetBytes{};
  llvm::support::endian::write64le(TargetBytes.data(), uint64_t{1});
  const uint32_t InternalHash = hashSymbolName(llvm::StringRef(
      reinterpret_cast<const char *>(TargetBytes.data()), TargetBytes.size()));
  EXPECT_EQ(llvm::support::endian::read32le(InternalProgram->text().data() +
                                            kImmediateOffset),
            InternalHash);

  ASSERT_NE(findFunction(*InternalProgram), nullptr);
  EXPECT_EQ(findFunction(*InternalProgram)->Address, kBytecodeStart);
  ASSERT_NE(findFunction(*InternalProgram, Function.Name), nullptr);
  EXPECT_EQ(findFunction(*InternalProgram, Function.Name)->Address,
            Function.Addr);
  ASSERT_NE(findFunction(*InternalProgram, "0x100000008"), nullptr);
  EXPECT_EQ(findFunction(*InternalProgram, "0x100000008")->Name, Function.Name);
  EXPECT_EQ(findFunction(*InternalProgram, "-1"), nullptr);
}

TEST(SBFAnalyzer, RejectsUnknownLegacyRelocationTypes) {
  BinaryImage Image =
      makeImage(Version::V0,
                {encode(Opcode::MOV64_IMM, 0, 0, 0, 0), encode(Opcode::EXIT)});
  RelocationEntry Relocation;
  Relocation.Address = kBytecodeStart;
  Relocation.Type = 0xffff;
  Image.Relocations.push_back(Relocation);

  auto Program = analyze(Image);
  ASSERT_FALSE(static_cast<bool>(Program));
  EXPECT_NE(llvm::toString(Program.takeError()).find("relocation"),
            std::string::npos);
}

TEST(SBFAnalyzer, ResolvesLoaderRelocatedLegacyInternalCalls) {
  constexpr size_t TargetSlot = 2;
  BinaryImage Image =
      makeImage(Version::V0,
                {encode(Opcode::CALL_IMM, 0, 0, 0, 1), encode(Opcode::EXIT),
                 encode(Opcode::MOV64_IMM, 0, 0, 0, 7), encode(Opcode::EXIT)});
  Symbol Function;
  Function.Name = "relative_target";
  Function.Addr = kBytecodeStart + TargetSlot * kInstructionSize;
  Function.IsFunc = true;
  Image.Symbols.push_back(Function);

  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_EQ(Program->Low.Instructions.front().Call, CallKind::Internal);
  EXPECT_EQ(Program->Low.Instructions.front().CallTarget, TargetSlot);
  EXPECT_EQ(Program->Low.Instructions.front().ResolvedName, Function.Name);
}

TEST(SBFAnalyzer, ExactRelocationSymbolCannotBeShadowedByDebugName) {
  BinaryImage Image =
      makeImage(Version::V0,
                {encode(Opcode::CALL_IMM, 0, 0, 0, -1), encode(Opcode::EXIT)});
  Symbol DebugImpostor;
  DebugImpostor.Name = "same_name";
  DebugImpostor.Addr = kBytecodeStart;
  DebugImpostor.IsFunc = true;
  Image.Symbols.push_back(DebugImpostor);

  RelocationEntry Relocation;
  Relocation.Address = kBytecodeStart;
  Relocation.Type = static_cast<uint32_t>(Relocation::Call32);
  Relocation.SymbolName = DebugImpostor.Name;
  Relocation.ELF = exactELFSymbol(kInstructionSize, 1, llvm::ELF::STT_FUNC,
                                  DebugImpostor.Name);
  Image.Relocations.push_back(std::move(Relocation));

  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const LowInstruction &Call = Program->Low.Instructions.front();
  EXPECT_EQ(Call.Call, CallKind::Internal);
  ASSERT_TRUE(Call.CallTarget.has_value());
  EXPECT_EQ(*Call.CallTarget, 1u);
  EXPECT_EQ(Call.ResolvedName, DebugImpostor.Name);
}

TEST(SBFAnalyzer, DiagnosesSyscallsOutsideTheAuditedRuntimeABI) {
  ASSERT_EQ(getSyscallInfo(0), nullptr);
  auto Static =
      analyze(makeImage(Version::V3, {encode(Opcode::CALL_IMM, 0, 0, 0, 0),
                                      encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Static)) << llvm::toString(Static.takeError());
  ASSERT_EQ(Static->Low.Instructions.front().Call, CallKind::Syscall);
  ASSERT_EQ(Static->Low.Diagnostics.size(), 1u);
  EXPECT_EQ(Static->Low.Diagnostics.front().Severity,
            DiagnosticSeverity::Warning);
  EXPECT_NE(Static->Low.Diagnostics.front().Message.find("audited runtime ABI"),
            std::string::npos);

  BinaryImage Legacy =
      makeImage(Version::V0,
                {encode(Opcode::CALL_IMM, 0, 0, 0, -1), encode(Opcode::EXIT)});
  RelocationEntry UnknownRelocation;
  UnknownRelocation.Address = kBytecodeStart;
  UnknownRelocation.Type = static_cast<uint32_t>(Relocation::Call32);
  UnknownRelocation.SymbolName = "custom_runtime_syscall";
  UnknownRelocation.ELF =
      exactELFSymbol(0, llvm::ELF::SHN_UNDEF, llvm::ELF::STT_NOTYPE,
                     UnknownRelocation.SymbolName);
  ASSERT_EQ(findSyscallByName(UnknownRelocation.SymbolName), nullptr);
  Legacy.Relocations.push_back(UnknownRelocation);

  auto LegacyProgram = analyze(Legacy);
  ASSERT_TRUE(static_cast<bool>(LegacyProgram))
      << llvm::toString(LegacyProgram.takeError());
  ASSERT_EQ(LegacyProgram->Low.Instructions.front().Call, CallKind::Syscall);
  ASSERT_EQ(LegacyProgram->Low.Diagnostics.size(), 1u);
  EXPECT_EQ(LegacyProgram->Low.Diagnostics.front().Severity,
            DiagnosticSeverity::Warning);
  EXPECT_NE(LegacyProgram->Low.Diagnostics.front().Message.find("unaudited"),
            std::string::npos);
}

} // namespace
} // namespace neverd::sbf
