//===- SBFAnalyzerTests.cpp - Solana SBF staged analysis tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/image/SBFRelocations.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

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
                      std::initializer_list<EncodedInstruction> Instructions,
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

  const bool CallsInternally =
      std::any_of(Program->Low.Edges.begin(), Program->Low.Edges.end(),
                  [](const CFGEdge &Edge) {
                    return Edge.Kind == EdgeKind::Call;
                  });
  ASSERT_TRUE(CallsInternally) << "the fixture must exercise an internal call";
  for (const Region &Region : Program->High.Regions)
    EXPECT_NE(Region.Kind, RegionKind::If)
        << "block_" << Region.HeaderBlock;
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
  const bool HasIf =
      std::any_of(Program->High.Regions.begin(), Program->High.Regions.end(),
                  [](const Region &Region) {
                    return Region.Kind == RegionKind::If;
                  });
  EXPECT_TRUE(HasIf);
}

TEST(SBFAnalyzer, RefusesAFunctionSymbolPointingIntoAWideLoad) {
  EncodedInstruction Low = encode(Opcode::LDDW, 3, 0, 0, 0x55667788);
  EncodedInstruction High{};
  llvm::support::endian::write32le(High.data() + kImmediateOffset, 0x11223344);
  BinaryImage Image =
      makeImage(Version::V3, {Low, High, encode(Opcode::EXIT)});
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

  const bool Reported = std::any_of(
      Program->Low.Diagnostics.begin(), Program->Low.Diagnostics.end(),
      [](const Diagnostic &Note) {
        return Note.Message.find("halfway") != std::string::npos &&
               Note.Message.find("wide load") != std::string::npos;
      });
  EXPECT_TRUE(Reported) << "dropping the symbol silently hides a bad input";
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
  ASSERT_EQ(Loop->Blocks.size(), 1u);
  EXPECT_EQ(Loop->Blocks.front(), Loop->HeaderBlock);
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

TEST(SBFAnalyzer, ResolvesAlreadyRelocatedLegacyInternalCalls) {
  constexpr size_t TargetSlot = 2;
  std::array<uint8_t, sizeof(uint64_t)> TargetBytes{};
  llvm::support::endian::write64le(TargetBytes.data(), TargetSlot);
  const uint32_t CallKey = hashSymbolName(llvm::StringRef(
      reinterpret_cast<const char *>(TargetBytes.data()), TargetBytes.size()));

  BinaryImage Image = makeImage(
      Version::V0,
      {encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(CallKey)),
       encode(Opcode::EXIT), encode(Opcode::MOV64_IMM, 0, 0, 0, 7),
       encode(Opcode::EXIT)});
  Symbol Function;
  Function.Name = "already_relocated";
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
