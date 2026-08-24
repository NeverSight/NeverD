//===- SBFVerifierTests.cpp - Layered SBF verifier tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/analysis/SBFVerifier.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <tuple>

namespace neverd::sbf {
namespace {

using EncodedInstruction = std::array<uint8_t, kInstructionSize>;

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
                      std::initializer_list<EncodedInstruction> Instructions) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart;
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

llvm::Expected<SBFProgram> analyzeCall(Version TheVersion, uint8_t Source,
                                       int32_t Immediate,
                                       const AnalyzeOptions &Options = {}) {
  return analyze(
      makeImage(TheVersion, {encode(Opcode::CALL_IMM, 0, Source, 0, Immediate),
                             encode(Opcode::EXIT)}),
      Options);
}

TEST(SBFVerifier, LatestLocalPreflightUsesTheResolvedSyscallRegistry) {
  auto Unknown = analyzeCall(Version::V3, 0, 0);
  ASSERT_TRUE(static_cast<bool>(Unknown))
      << llvm::toString(Unknown.takeError());
  auto UnknownReport =
      verifyLocalPreflight(Unknown->Low, Unknown->RegisteredSyscallHashes);
  ASSERT_TRUE(static_cast<bool>(UnknownReport))
      << llvm::toString(UnknownReport.takeError());
  ASSERT_EQ(UnknownReport->Issues.size(), 1u);
  EXPECT_EQ(UnknownReport->Issues.front().Rule, VerifierRule::InvalidSyscall);
  EXPECT_EQ(UnknownReport->Issues.front().Payload, 0u);

  const SyscallInfo *Log = findSyscallByName("sol_log_");
  ASSERT_NE(Log, nullptr);
  auto Registered = analyzeCall(Version::V3, 0, Log->Hash);
  ASSERT_TRUE(static_cast<bool>(Registered))
      << llvm::toString(Registered.takeError());
  auto RegisteredReport = verifyLocalPreflight(
      Registered->Low, Registered->RegisteredSyscallHashes);
  ASSERT_TRUE(static_cast<bool>(RegisteredReport))
      << llvm::toString(RegisteredReport.takeError());
  EXPECT_TRUE(RegisteredReport->accepted());
}

TEST(SBFVerifier, LatestLocalPreflightTreatsTheSyscallRegistryAsASet) {
  constexpr uint32_t RegisteredHash = std::numeric_limits<uint32_t>::max();
  auto Program = analyzeCall(Version::V3, 0, -1);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  constexpr std::array<uint32_t, 5> UnsortedRegistry = {
      RegisteredHash, RegisteredHash, 0, 1, 2};
  auto Report = verifyLocalPreflight(Program->Low, UnsortedRegistry);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  EXPECT_TRUE(Report->accepted());
}

TEST(SBFVerifier, LatestLocalPreflightPreservesOfficialCallRules) {
  auto Unresolved = analyzeCall(Version::V3, 1, -1);
  ASSERT_TRUE(static_cast<bool>(Unresolved))
      << llvm::toString(Unresolved.takeError());
  auto UnresolvedReport = verifyLocalPreflight(
      Unresolved->Low, Unresolved->RegisteredSyscallHashes);
  ASSERT_TRUE(static_cast<bool>(UnresolvedReport))
      << llvm::toString(UnresolvedReport.takeError());
  ASSERT_EQ(UnresolvedReport->Issues.size(), 1u);
  EXPECT_EQ(UnresolvedReport->Issues.front().Rule,
            VerifierRule::InvalidFunction);
  EXPECT_EQ(UnresolvedReport->Issues.front().Payload, 0u);

  for (const auto [Version, Source, Immediate] :
       {std::tuple{Version::V2, uint8_t{0}, int32_t{0}},
        std::tuple{Version::V3, uint8_t{1}, int32_t{99}},
        std::tuple{Version::V3, uint8_t{2}, int32_t{-1}}}) {
    auto Program = analyzeCall(Version, Source, Immediate);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report =
        verifyLocalPreflight(Program->Low, Program->RegisteredSyscallHashes);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    EXPECT_TRUE(Report->accepted());
  }
}

TEST(SBFVerifier, LatestLocalPreflightReturnsTheFirstIssue) {
  auto Program =
      analyze(makeImage(Version::V3, {encode(Opcode::CALL_IMM, 0, 0, 0, 0),
                                      encode(Opcode::CALL_IMM, 0, 1, 0, -1),
                                      encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report =
      verifyLocalPreflight(Program->Low, Program->RegisteredSyscallHashes);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  ASSERT_EQ(Report->Issues.size(), 1u);
  EXPECT_EQ(Report->Issues.front().Rule, VerifierRule::InvalidSyscall);
  EXPECT_EQ(Report->Issues.front().Slot, 0u);
}

TEST(SBFVerifier, CompositePipelineRunsRequisiteBeforeLocalPreflight) {
  AnalyzeOptions Options;
  Options.Strict = false;
  EncodedInstruction Invalid{};
  Invalid[kOpcodeOffset] = 0xff;
  auto Program = analyze(makeImage(Version::V3, {Invalid}), Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report =
      verifyLocalPreflight(Program->Low, Program->RegisteredSyscallHashes);
  ASSERT_FALSE(static_cast<bool>(Report));
  EXPECT_NE(llvm::toString(Report.takeError()).find("requisite"),
            std::string::npos);
}

TEST(SBFVerifier, AnalyzerDoesNotReportSkippedLocalPreflightAsAccepted) {
  AnalyzeOptions Options;
  Options.Strict = false;
  Options.Verification = VerificationPolicy::RequisiteAndLocalPreflight;
  EncodedInstruction Invalid{};
  Invalid[kOpcodeOffset] = 0xff;

  auto Program = analyze(makeImage(Version::V3, {Invalid}), Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_EQ(Program->Verification.Stage, VerificationStage::LocalPreflight);
  EXPECT_EQ(Program->Verification.State, VerificationState::BlockedByRequisite);
  EXPECT_FALSE(Program->Verification.accepted())
      << "requisite-invalid LowIR blocks local preflight; it was not accepted";
}

TEST(SBFVerifier, AnalyzerLocalPreflightIsExplicitAndStructured) {
  auto CurrentAgave = analyzeCall(Version::V3, 0, 0);
  ASSERT_TRUE(static_cast<bool>(CurrentAgave))
      << llvm::toString(CurrentAgave.takeError());
  EXPECT_EQ(CurrentAgave->Verification.State, VerificationState::NotRequested);
  EXPECT_FALSE(CurrentAgave->Verification.accepted());

  AnalyzeOptions Strict;
  Strict.Verification = VerificationPolicy::RequisiteAndLocalPreflight;
  auto Rejected = analyzeCall(Version::V3, 0, 0, Strict);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError()).find("local-preflight"),
            std::string::npos);

  AnalyzeOptions Relaxed = Strict;
  Relaxed.Strict = false;
  auto Recovered = analyzeCall(Version::V3, 0, 0, Relaxed);
  ASSERT_TRUE(static_cast<bool>(Recovered))
      << llvm::toString(Recovered.takeError());
  ASSERT_EQ(Recovered->Verification.Issues.size(), 1u);
  EXPECT_EQ(Recovered->Verification.State, VerificationState::Rejected);
  EXPECT_FALSE(Recovered->Verification.accepted());
  EXPECT_EQ(Recovered->Verification.Issues.front().Rule,
            VerifierRule::InvalidSyscall);
}

} // namespace
} // namespace neverd::sbf
