//===- SBFISAConformanceTests.cpp - Anza ISA/verifier conformance -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/runtime/SBFOpcodes.h"
#include "neverd/sbf/SBFConstants.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace neverd::sbf {
namespace {

struct UpstreamOpcode {
  Opcode ID;
  uint8_t Encoding;
  llvm::StringLiteral Mnemonic;
  OperandForm Form;
  uint8_t Width;
  VersionMask Versions;
};

constexpr std::array UpstreamOpcodes = {
#define SBF_UPSTREAM_OPCODE(ID, ENCODING, MNEMONIC, FORM, WIDTH, VERSION_MASK) \
  UpstreamOpcode{Opcode::ID,        ENCODING, MNEMONIC,                        \
                 OperandForm::FORM, WIDTH,    VersionMask::VERSION_MASK},
#include "SBFUpstreamOpcodes.def"
};

constexpr std::array Versions = {Version::V0, Version::V1, Version::V2,
                                 Version::V3, Version::V4};

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

std::vector<uint8_t>
bytes(std::initializer_list<EncodedInstruction> Instructions) {
  std::vector<uint8_t> Result;
  Result.reserve(Instructions.size() * kInstructionSize);
  for (const EncodedInstruction &Instruction : Instructions)
    Result.insert(Result.end(), Instruction.begin(), Instruction.end());
  return Result;
}

BinaryImage makeImage(Version TheVersion, llvm::ArrayRef<uint8_t> TextBytes) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart;
  Image.Raw.assign(TextBytes.begin(), TextBytes.end());

  Section Text;
  Text.Name = kTextSectionName.str();
  Text.VA = kBytecodeStart;
  Text.Size = TextBytes.size();
  Text.FileSz = TextBytes.size();
  Text.Flags = SegmentFlags::Executable;
  Text.Alignment = kInstructionSize;
  Text.Data = Image.Raw;
  Image.Sections.push_back(std::move(Text));

  Metadata Meta;
  Meta.Machine = kELFMachineBPF;
  Meta.ELFFlags = static_cast<uint32_t>(TheVersion);
  Meta.Version = TheVersion;
  Meta.StrictLayout = versionHasFeature(TheVersion, VersionFeature::StrictELF);
  Meta.TextFile = {0, TextBytes.size()};
  Meta.TextVM = {kBytecodeStart, TextBytes.size()};
  Image.SBF = Meta;
  return Image;
}

llvm::Expected<SBFProgram> analyzeBytes(Version TheVersion,
                                        llvm::ArrayRef<uint8_t> TextBytes) {
  AnalyzeOptions Options;
  if (TheVersion == Version::V4)
    Options.ExpertEnvironment = ExpertRuntimeEnvironmentOverride{
        Version::V0, Version::V4, SBFVMConfig{}};
  return analyze(makeImage(TheVersion, TextBytes), Options);
}

bool accepts(Version TheVersion, llvm::ArrayRef<uint8_t> TextBytes,
             std::string *Error = nullptr) {
  auto Program = analyzeBytes(TheVersion, TextBytes);
  if (Program)
    return true;
  std::string Message = llvm::toString(Program.takeError());
  if (Error)
    *Error = std::move(Message);
  return false;
}

TEST(SBFISAConformance, ExhaustsEveryEncodingForEveryVersion) {
  for (Version TheVersion : Versions) {
    SCOPED_TRACE(versionName(TheVersion).str());
    for (unsigned Encoding = 0; Encoding <= UINT8_MAX; ++Encoding) {
      const UpstreamOpcode *Expected = nullptr;
      for (const UpstreamOpcode &Candidate : UpstreamOpcodes) {
        if (Candidate.Encoding != Encoding ||
            !versionInMask(TheVersion, Candidate.Versions))
          continue;
        ASSERT_EQ(Expected, nullptr)
            << "upstream manifest collision at opcode " << Encoding;
        Expected = &Candidate;
      }

      const OpcodeInfo *Actual =
          getOpcodeInfo(static_cast<uint8_t>(Encoding), TheVersion);
      ASSERT_EQ(Actual != nullptr, Expected != nullptr)
          << "encoding 0x" << std::hex << Encoding;
      if (!Expected)
        continue;
      EXPECT_EQ(Actual->ID, Expected->ID);
      EXPECT_EQ(Actual->Encoding, Expected->Encoding);
      EXPECT_EQ(Actual->Mnemonic, Expected->Mnemonic);
      EXPECT_EQ(Actual->Form, Expected->Form);
      EXPECT_EQ(Actual->Width, Expected->Width);
    }
  }
}

TEST(SBFISAConformance, ProductionAndUpstreamTablesAreBijections) {
  ASSERT_EQ(opcodeInfos().size(), UpstreamOpcodes.size());
  for (const OpcodeInfo &Actual : opcodeInfos()) {
    size_t Matches = 0;
    for (const UpstreamOpcode &Expected : UpstreamOpcodes)
      Matches += Actual.ID == Expected.ID;
    EXPECT_EQ(Matches, 1u) << opcodeName(Actual.ID).str();
  }
}

TEST(SBFVerifierConformance, ChecksBothRegisterNibbles) {
  for (Version TheVersion : Versions) {
    for (uint8_t Dst = 0; Dst <= kRegisterEncodingMask; ++Dst) {
      for (uint8_t Src = 0; Src <= kRegisterEncodingMask; ++Src) {
        SCOPED_TRACE(::testing::Message()
                     << versionName(TheVersion).str()
                     << " dst=" << unsigned(Dst) << " src=" << unsigned(Src));
        const auto Program =
            bytes({encode(Opcode::MOV64_REG, Dst, Src), encode(Opcode::EXIT)});
        EXPECT_EQ(accepts(TheVersion, Program),
                  Dst < kFramePointerRegister && Src < kRegisterCount);
      }
    }
  }
}

TEST(SBFVerifierConformance, AppliesFramePointerAndAlignmentRules) {
  for (Version TheVersion : Versions) {
    const Opcode Store =
        TheVersion == Version::V2 ? Opcode::ST_8B_IMM : Opcode::ST_DW_IMM;
    EXPECT_TRUE(accepts(TheVersion, bytes({encode(Store, kFramePointerRegister),
                                           encode(Opcode::EXIT)})));

    const bool Manual =
        versionHasFeature(TheVersion, VersionFeature::ManualStackFrames);
    EXPECT_EQ(accepts(TheVersion,
                      bytes({encode(Opcode::ADD64_IMM, kFramePointerRegister, 0,
                                    0, kDynamicStackFrameAlignment),
                             encode(Opcode::EXIT)})),
              Manual);
    EXPECT_FALSE(accepts(
        TheVersion,
        bytes({encode(Opcode::ADD64_IMM, kFramePointerRegister, 0, 0, 1),
               encode(Opcode::EXIT)})));
  }
}

TEST(SBFVerifierConformance, ChecksLDDWAndControlFlowBoundaries) {
  EXPECT_FALSE(accepts(Version::V0, bytes({encode(Opcode::LDDW)})));
  EXPECT_FALSE(accepts(Version::V0,
                       bytes({encode(Opcode::LDDW), encode(Opcode::EXIT)})));

  EncodedInstruction Continuation{};
  EXPECT_FALSE(accepts(Version::V0, bytes({encode(Opcode::LDDW), Continuation,
                                           encode(Opcode::JA, 0, 0, -2),
                                           encode(Opcode::EXIT)})));
  EXPECT_FALSE(accepts(
      Version::V3, bytes({encode(Opcode::JA, 0, 0, 2), encode(Opcode::EXIT)})));
  EXPECT_FALSE(accepts(Version::V3, bytes({encode(Opcode::JA, 0, 0, -2),
                                           encode(Opcode::EXIT)})));
}

TEST(SBFVerifierConformance, ChecksImmediateArithmeticDomains) {
  for (Version TheVersion : Versions) {
    for (const OpcodeInfo &Info : opcodeInfos()) {
      if (!Info.isAvailableIn(TheVersion) || Info.Form != OperandForm::DstImm)
        continue;
      if (Info.Op == Operation::UDiv || Info.Op == Operation::URem ||
          Info.Op == Operation::SDiv || Info.Op == Operation::SRem) {
        EXPECT_FALSE(accepts(TheVersion, bytes({encode(Info.ID, 0, 0, 0, 0),
                                                encode(Opcode::EXIT)})))
            << opcodeName(Info.ID).str();
        EXPECT_TRUE(accepts(TheVersion, bytes({encode(Info.ID, 0, 0, 0, 1),
                                               encode(Opcode::EXIT)})))
            << opcodeName(Info.ID).str();
      }
      if (Info.Op == Operation::LSh || Info.Op == Operation::RSh ||
          Info.Op == Operation::ARSh) {
        EXPECT_TRUE(
            accepts(TheVersion, bytes({encode(Info.ID, 0, 0, 0, Info.Width - 1),
                                       encode(Opcode::EXIT)})))
            << opcodeName(Info.ID).str();
        EXPECT_FALSE(
            accepts(TheVersion, bytes({encode(Info.ID, 0, 0, 0, Info.Width),
                                       encode(Opcode::EXIT)})))
            << opcodeName(Info.ID).str();
        EXPECT_FALSE(accepts(TheVersion, bytes({encode(Info.ID, 0, 0, 0, -1),
                                                encode(Opcode::EXIT)})))
            << opcodeName(Info.ID).str();
      }
    }
  }
}

TEST(SBFVerifierConformance, ChecksEndianWidths) {
  for (Version TheVersion : Versions) {
    for (Opcode ID : {Opcode::LE, Opcode::BE}) {
      const OpcodeInfo *Info = getOpcodeInfo(ID);
      if (!Info->isAvailableIn(TheVersion))
        continue;
      for (int Width : {16, 32, 64})
        EXPECT_TRUE(accepts(TheVersion, bytes({encode(ID, 0, 0, 0, Width),
                                               encode(Opcode::EXIT)})));
      for (int Width : {0, 8, 63, 65})
        EXPECT_FALSE(accepts(TheVersion, bytes({encode(ID, 0, 0, 0, Width),
                                                encode(Opcode::EXIT)})));
    }
  }
}

TEST(SBFVerifierConformance, SelectsTheVersionedCALLXRegisterField) {
  for (Version TheVersion : Versions) {
    EncodedInstruction Call = encode(Opcode::CALL_REG, 3, 4, 0, 5);
    auto Program =
        analyzeBytes(TheVersion, bytes({Call, encode(Opcode::EXIT)}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << (Program ? std::string() : llvm::toString(Program.takeError()));
    const uint8_t Expected =
        TheVersion == Version::V2 ? 4 : (TheVersion >= Version::V3 ? 3 : 5);
    EXPECT_EQ(Program->Low.Instructions[0].CallRegister, Expected);

    Call = encode(Opcode::CALL_REG, kFramePointerRegister,
                  kFramePointerRegister, 0, kFramePointerRegister);
    EXPECT_FALSE(accepts(TheVersion, bytes({Call, encode(Opcode::EXIT)})));
  }
}

TEST(SBFVerifierConformance, RejectsProgramsPastTheInstructionLimit) {
  const EncodedInstruction Move = encode(Opcode::MOV64_IMM);
  std::vector<uint8_t> Text((kMaxInstructions + 1) * kInstructionSize);
  for (size_t Slot = 0; Slot <= kMaxInstructions; ++Slot)
    std::copy(Move.begin(), Move.end(),
              Text.begin() + static_cast<ptrdiff_t>(Slot * kInstructionSize));
  std::string Error;
  EXPECT_FALSE(accepts(Version::V3, Text, &Error));
  EXPECT_NE(Error.find("instruction limit"), std::string::npos) << Error;
}

TEST(SBFVerifierConformance, AcceptsProgramsPastTheLegacyInstructionLimit) {
  const EncodedInstruction Move = encode(Opcode::MOV64_IMM);
  const EncodedInstruction Exit = encode(Opcode::EXIT);
  const size_t InstructionCount = kLegacyProgramInstructionCount + 1;
  std::vector<uint8_t> Text(InstructionCount * kInstructionSize);
  for (size_t Slot = 0; Slot + 1 < InstructionCount; ++Slot)
    std::copy(Move.begin(), Move.end(),
              Text.begin() + static_cast<ptrdiff_t>(Slot * kInstructionSize));
  std::copy(Exit.begin(), Exit.end(),
            Text.begin() + static_cast<ptrdiff_t>((InstructionCount - 1) *
                                                  kInstructionSize));
  EXPECT_TRUE(accepts(Version::V3, Text));
}

} // namespace
} // namespace neverd::sbf
