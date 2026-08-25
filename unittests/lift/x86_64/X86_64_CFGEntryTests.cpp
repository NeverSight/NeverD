//===- X86_64_CFGEntryTests.cpp - backward-entry CFG regressions --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "PipelineLowIRDetail.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace neverd;

class X86_64_CFGEntry : public NeverDLiftTest {};

namespace {

fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_backward_entry.o"; }

std::string functionDump(const std::string &Dump, const std::string &Name) {
  const std::string Header = "func " + Name + " @ ";
  size_t Begin = Dump.find(Header);
  if (Begin == std::string::npos)
    return {};
  size_t End = Dump.find("\nfunc ", Begin + Header.size());
  return Dump.substr(Begin, End == std::string::npos ? End : End - Begin);
}

size_t countOccurrences(const std::string &Text, const std::string &Needle) {
  size_t Count = 0;
  for (size_t Pos = 0; (Pos = Text.find(Needle, Pos)) != std::string::npos;
       Pos += Needle.size())
    ++Count;
  return Count;
}

std::string llvmFunction(const std::string &IR, const std::string &Name) {
  size_t Symbol = IR.find("@" + Name + "(");
  if (Symbol == std::string::npos)
    return {};
  size_t Begin = IR.rfind("define ", Symbol);
  size_t End = IR.find("\n}", Symbol);
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return IR.substr(Begin, End + 2 - Begin);
}

struct ParsedCfg {
  uint64_t Entry = 0;
  std::map<int, uint64_t> Starts;
  std::map<int, std::vector<int>> Succs;
};

ParsedCfg parseCfg(const std::string &Dump) {
  ParsedCfg Result;
  std::smatch Match;
  const std::regex Header(R"(^func [^ ]+ @ 0x([0-9A-Fa-f]+))");
  if (std::regex_search(Dump, Match, Header))
    Result.Entry = std::stoull(Match[1].str(), nullptr, 16);

  std::stringstream Lines(Dump);
  std::string Line;
  while (std::getline(Lines, Line)) {
    int Id = -1;
    unsigned long long Start = 0;
    if (std::sscanf(Line.c_str(), "  block %d [0x%llx", &Id, &Start) != 2)
      continue;
    Result.Starts[Id] = static_cast<uint64_t>(Start);
    const std::string Prefix = "succs=[";
    size_t SuccBegin = Line.find(Prefix);
    size_t SuccEnd = SuccBegin == std::string::npos
                         ? std::string::npos
                         : Line.find(']', SuccBegin + Prefix.size());
    if (SuccEnd == std::string::npos)
      continue;
    std::stringstream SS(Line.substr(SuccBegin + Prefix.size(),
                                     SuccEnd - SuccBegin - Prefix.size()));
    std::string Field;
    while (std::getline(SS, Field, ','))
      if (!Field.empty())
        Result.Succs[Id].push_back(std::stoi(Field));
  }
  return Result;
}

LowFunc continuationBindingLowFunc() {
  constexpr va_t EntryVA = 0x140001000;
  constexpr va_t ReturnVA = EntryVA + 0x20;
  constexpr va_t TargetVA = EntryVA + 0x100;

  LowFunc Low;
  Low.Entry = EntryVA;
  Low.Name = "continuation_binding";
  Low.CxxContinuationExitAnalysisComplete = true;
  LowCxxContinuationExitEvidence Evidence;
  Evidence.ReturnAddr = ReturnVA;
  Evidence.ReturnSeq = 9;
  Evidence.Targets = {TargetVA};
  Evidence.Complete = true;
  Low.CxxContinuationExits.push_back(std::move(Evidence));

  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = EntryVA;
  Entry.EndAddr = EntryVA + 1;
  Entry.Succs = {2};
  LowOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = EntryVA;
  Branch.Seq = 1;
  Branch.addInput(NdVar::codeAddress(ReturnVA, 8));
  Entry.Ops.push_back(std::move(Branch));

  // This empty block is deliberately retained in LowIR.  simplifyCfg removes
  // it and renumbers the RETURN block from 2 to 1 before evidence binding.
  LowBlock Removed;
  Removed.Id = 1;
  Removed.StartAddr = EntryVA + 0x10;
  Removed.EndAddr = EntryVA + 0x11;

  LowBlock Returning;
  Returning.Id = 2;
  Returning.StartAddr = ReturnVA;
  Returning.EndAddr = ReturnVA + 1;
  Returning.Preds = {0};
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = ReturnVA;
  Return.Seq = 9;
  Return.addInput(NdVar::reg(x86reg::RAX, 8));
  Returning.Ops.push_back(std::move(Return));

  Low.Blocks.push_back(std::move(Entry));
  Low.Blocks.push_back(std::move(Removed));
  Low.Blocks.push_back(std::move(Returning));
  return Low;
}

BinaryImage continuationPipelineImage(bool ReturnAddress = true) {
  constexpr va_t EntryVA = 0x140001000;
  constexpr va_t OwnerVA = EntryVA + 0x100;
  constexpr va_t TargetVA = OwnerVA + 0x20;
  constexpr va_t NativeFuncInfoVA = 0x140004000;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = EntryVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = EntryVA;
  Text.Size = 0x140;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const std::vector<uint8_t> CatchBody =
      ReturnAddress
          ? std::vector<uint8_t>{0x48, 0x8d, 0x05, 0x19, 0x01, 0x00, 0x00, 0xc3}
          // lea rax,TargetVA; ret
          : std::vector<uint8_t>{0x31, 0xc0, 0xc3}; // xor eax,eax; ret
  std::copy(CatchBody.begin(), CatchBody.end(), Text.Data.begin());
  Text.Data[OwnerVA - EntryVA] = 0x31; // xor eax,eax; ret
  Text.Data[OwnerVA - EntryVA + 1] = 0xc0;
  Text.Data[OwnerVA - EntryVA + 2] = 0xc3;
  Text.Data[TargetVA - EntryVA] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = EntryVA;
  TextSection.Size = 0x140;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(EntryVA, EntryVA + CatchBody.size());
  Img.KnownCodeRanges.emplace_back(OwnerVA, OwnerVA + 0x40);
  Img.Symbols.push_back(Symbol::makeFunc(EntryVA, CatchBody.size()));
  Img.Symbols.push_back(Symbol::makeFunc(OwnerVA, 0x40));

  ExceptionFunction Catch;
  Catch.CodeRange = {EntryVA, EntryVA + CatchBody.size()};
  Catch.Kind = RuntimeFunctionKind::Primary;
  Catch.Encoding = ExceptionEncoding::X64UnwindV1;
  Catch.ParseStatus = ExceptionParseStatus::Complete;
  Catch.Personality = ExceptionPersonality::CxxFrameHandler3;
  Catch.Cxx.emplace();
  Catch.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH3;
  Catch.Cxx->NativeFuncInfoVA = NativeFuncInfoVA;
  Catch.Cxx->IsSeparated = true;
  Catch.Cxx->IsCatchFunclet = true;
  Img.ExceptionMetadata.Functions.push_back(std::move(Catch));

  ExceptionFunction Owner;
  Owner.CodeRange = {OwnerVA, OwnerVA + 0x40};
  Owner.Kind = RuntimeFunctionKind::Primary;
  Owner.Encoding = ExceptionEncoding::X64UnwindV1;
  Owner.ParseStatus = ExceptionParseStatus::Complete;
  Owner.Personality = ExceptionPersonality::CxxFrameHandler3;
  Owner.Cxx.emplace();
  Owner.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH3;
  Owner.Cxx->NativeFuncInfoVA = NativeFuncInfoVA;
  Owner.Cxx->IsSeparated = true;
  Owner.Cxx->MaxState = 2;
  CxxUnwindAction StateZero;
  StateZero.ToState = -1;
  Owner.Cxx->UnwindMap.push_back(StateZero);
  CxxUnwindAction StateOne;
  StateOne.ToState = 0;
  Owner.Cxx->UnwindMap.push_back(StateOne);
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Handler;
  Handler.HandlerVA = EntryVA;
  Try.Handlers.push_back(std::move(Handler));
  Owner.Cxx->TryBlocks.push_back(std::move(Try));
  Img.ExceptionMetadata.Functions.push_back(std::move(Owner));
  return Img;
}

} // namespace

TEST(CFGBuilderCoverage, ReportsReachableDecodeLiftAndFailures) {
  auto Build = [](std::vector<uint8_t> Bytes) {
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Bits = Bitness::Bits64;
    Img.Format = BinaryFormat::COFF;
    Img.Base = 0x140000000;
    Segment Text;
    Text.VA = Img.Base + 0x1000;
    Text.Size = Bytes.size();
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = std::move(Bytes);
    Img.Segments.push_back(std::move(Text));

    Decoder Dec;
    EXPECT_TRUE(Dec.init(Arch::X64));
    CFGBuilder Builder;
    return Builder.build(Img, Dec, Img.Base + 0x1000, "coverage_probe");
  };

  LowFunc Complete = Build({0x90, 0xc3}); // nop; ret
  EXPECT_TRUE(Complete.hasCompleteLiftCoverage());
  EXPECT_EQ(Complete.DecodedInstructionCount, 2u);
  EXPECT_EQ(Complete.LiftedInstructionCount, 2u);

  LowFunc DecodeFailure = Build({0x0f}); // truncated two-byte opcode
  EXPECT_FALSE(DecodeFailure.hasCompleteLiftCoverage());
  EXPECT_EQ(DecodeFailure.DecodedInstructionCount, 0u);
  ASSERT_EQ(DecodeFailure.DecodeFailureAddresses.size(), 1u);
  EXPECT_EQ(DecodeFailure.DecodeFailureAddresses[0], 0x140001000u);

  LowFunc TruncatedPath = Build({0xeb, 0x7f}); // jmp outside mapped code
  EXPECT_FALSE(TruncatedPath.hasCompleteLiftCoverage());
  EXPECT_EQ(TruncatedPath.DecodedInstructionCount, 1u);
  EXPECT_EQ(TruncatedPath.LiftedInstructionCount, 1u);
  ASSERT_EQ(TruncatedPath.TruncatedPathAddresses.size(), 1u);
  EXPECT_EQ(TruncatedPath.TruncatedPathAddresses[0], 0x140001081u);
}

TEST(CFGBuilderCoverage, RejectsBranchTargetsOwnedByDataInAnRXMapping) {
  constexpr va_t EntryVA = 0x5000;
  constexpr va_t DataVA = EntryVA + 0x10;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Base = EntryVA;
  Img.Entry = EntryVA;

  Segment Mapping;
  Mapping.VA = EntryVA;
  Mapping.Size = 0x11;
  Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Mapping.Data.assign(Mapping.Size, 0x90);
  Mapping.Data[0] = 0xEB;
  Mapping.Data[1] = 0x0E;                // jmp DataVA
  Mapping.Data[DataVA - EntryVA] = 0xC3; // ret-shaped data
  Img.Segments.push_back(std::move(Mapping));

  Section Code;
  Code.Name = ".text";
  Code.VA = EntryVA;
  Code.Size = 2;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  Section Data;
  Data.Name = ".rodata";
  Data.VA = DataVA;
  Data.Size = 1;
  Data.Flags = SegmentFlags::Readable;
  Img.Sections.push_back(std::move(Data));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  LowFunc Func = Builder.build(Img, Dec, EntryVA, "section_owner_probe");

  EXPECT_EQ(Func.DecodedInstructionCount, 1u);
  EXPECT_EQ(Func.LiftedInstructionCount, 1u);
  ASSERT_EQ(Func.TruncatedPathAddresses.size(), 1u);
  EXPECT_EQ(Func.TruncatedPathAddresses[0], DataVA);
}

TEST(FuncDetectorCoverage, RejectsCandidateWithUndecodableBranchArm) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Img.Base + 0x1000;

  Segment Text;
  Text.VA = Img.Entry;
  Text.Size = 0x30;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0x90);
  Text.Data[0] = 0xc3; // The trusted image entry is a complete function.

  // Executable data can accidentally spell a direct call.  Its target has a
  // valid linear fallthrough to RET, but the other conditional branch arm
  // starts with POPA, which is invalid in 64-bit mode.  A straight-line probe
  // therefore accepts a bogus function that reachable-arm validation rejects.
  Text.Data[0x10] = 0xe8;
  Text.Data[0x11] = 0x0b;
  Text.Data[0x12] = 0x00;
  Text.Data[0x13] = 0x00;
  Text.Data[0x14] = 0x00;
  Text.Data[0x20] = 0x75;
  Text.Data[0x21] = 0x02;
  Text.Data[0x22] = 0xc3;
  Text.Data[0x24] = 0x61;
  Img.Segments.push_back(std::move(Text));
  Img.KnownCodeRanges.push_back({Img.Entry, Img.Entry + 1});

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);

  ASSERT_EQ(Functions.size(), 1u);
  EXPECT_EQ(Functions[0].first, Img.Entry);
}

TEST(FuncDetectorCoverage, DistinguishesExecutableDataExportsFromFunctions) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Img.Base + 0x1000;

  Segment Text;
  Text.VA = Img.Entry;
  Text.Size = 0xc0;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0x90);
  Text.Data[0] = 0xc3;    // ret
  Text.Data[0x10] = 0x61; // Executable-section data, invalid in x86-64.
  Text.Data[0x20] = 0x90; // nop
  Text.Data[0x21] = 0xc3; // ret
  Img.Segments.push_back(std::move(Text));

  Export DataExport;
  DataExport.Name = "ordinary_table";
  DataExport.Addr = Img.Entry + 0x10;
  Img.Exports.push_back(std::move(DataExport));
  Export FunctionExport;
  FunctionExport.Name = "ordinary_export";
  FunctionExport.Addr = Img.Entry + 0x20;
  Img.Exports.push_back(std::move(FunctionExport));
  Export LongFunctionExport;
  LongFunctionExport.Name = "ordinary_long_export";
  LongFunctionExport.Addr = Img.Entry + 0x40;
  Img.Exports.push_back(std::move(LongFunctionExport));
  Img.Segments[0].Data[0x90] = 0xc3;
  // An exported alternate entry can legitimately lie inside an unwind range
  // whose primary start is elsewhere.  It still requires decode validation,
  // but must not be discarded solely for being inside that range.
  Img.KnownCodeRanges.push_back({Img.Entry + 0x30, Img.Entry + 0xa0});

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);

  std::map<va_t, std::string> ByEntry(Functions.begin(), Functions.end());
  EXPECT_EQ(ByEntry.size(), 3u);
  EXPECT_EQ(ByEntry.count(Img.Entry), 1u);
  EXPECT_EQ(ByEntry.count(Img.Entry + 0x10), 0u);
  EXPECT_EQ(ByEntry[Img.Entry + 0x20], "ordinary_export");
  EXPECT_EQ(ByEntry[Img.Entry + 0x40], "ordinary_long_export");
}

TEST(FuncDetectorCoverage,
     AcceptsRelocationAuthenticatedLeafWithoutAConventionalPrologue) {
  constexpr va_t ImageBase = 0x140000000;
  constexpr va_t EntryVA = ImageBase + 0x1000;
  constexpr va_t LeafVA = EntryVA + 0x20;
  constexpr va_t PointerSlotVA = ImageBase + 0x2000;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = ImageBase;
  Img.Entry = EntryVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = EntryVA;
  Text.Size = 0x30;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  Text.Data[0] = 0xc3;    // ret
  Text.Data[0x20] = 0xc2; // ret 0
  Text.Data[0x21] = 0x00;
  Text.Data[0x22] = 0x00;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = EntryVA;
  TextSection.Size = 0x30;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));

  Segment ReadOnlyData;
  ReadOnlyData.Name = ".rdata";
  ReadOnlyData.VA = PointerSlotVA;
  ReadOnlyData.Size = sizeof(uint64_t);
  ReadOnlyData.Flags = SegmentFlags::Readable;
  ReadOnlyData.Data.resize(sizeof(uint64_t));
  writeLE<uint64_t>(ReadOnlyData.Data.data(), LeafVA);
  Img.Segments.push_back(std::move(ReadOnlyData));
  Img.CodePtrRelocSlots.insert(PointerSlotVA);

  // The leaf begins exactly where the preceding unwind range ends.  It has no
  // unwind row and no entry prologue, so the relocation is the only structural
  // evidence that its terminating instruction is a callable entry.
  Img.KnownCodeRanges.emplace_back(EntryVA, LeafVA);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(std::count_if(
                Functions.begin(), Functions.end(),
                [](const auto &Function) { return Function.first == LeafVA; }),
            1u);
  EXPECT_EQ(Img.VerifiedFunctionEntries.count(LeafVA), 1u);
}

TEST(CFGBuilderCoverage,
     AcceptsOnlyOwnerScopedContinuationRootsInAnAuthoritativeBody) {
  constexpr va_t EntryVA = 0x140001000;
  constexpr va_t InstructionInteriorVA = EntryVA + 2;
  constexpr va_t HiddenContinuationVA = EntryVA + 0x20;
  constexpr va_t NestedKnownEntryVA = EntryVA + 0x30;
  constexpr va_t NextEntryVA = EntryVA + 0x100;

  auto MakeImage = [&]() {
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Bits = Bitness::Bits64;
    Img.Format = BinaryFormat::COFF;
    Img.Base = 0x140000000;
    Img.Entry = EntryVA;

    Segment Text;
    Text.Name = ".text";
    Text.VA = EntryVA;
    Text.Size = NextEntryVA - EntryVA + 1;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0xcc);
    Text.Data[0] = 0xb8; // mov eax, 1; ret before the hidden block
    Text.Data[1] = 0x01;
    Text.Data[2] = 0x00;
    Text.Data[3] = 0x00;
    Text.Data[4] = 0x00;
    Text.Data[5] = 0xc3;
    Text.Data[HiddenContinuationVA - EntryVA] = 0xc3;
    Text.Data[NestedKnownEntryVA - EntryVA] = 0xc3;
    Text.Data[NextEntryVA - EntryVA] = 0xc3;
    Img.Segments.push_back(std::move(Text));

    Section TextSection;
    TextSection.Name = ".text";
    TextSection.VA = EntryVA;
    TextSection.Size = NextEntryVA - EntryVA + 1;
    TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Img.Sections.push_back(std::move(TextSection));
    return Img;
  };

  const std::set<va_t> ContinuationRoots{
      InstructionInteriorVA, HiddenContinuationVA, NestedKnownEntryVA};
  auto Build = [&](BinaryImage &Img, const std::set<va_t> *Roots) {
    Decoder Dec;
    EXPECT_TRUE(Dec.init(Arch::X64));
    CFGBuilder Builder;
    const std::set<va_t> Entries{EntryVA, NestedKnownEntryVA, NextEntryVA};
    Builder.setKnownFuncEntries(&Entries);
    Builder.setCrossFunctionContinuationRoots(Roots);
    return Builder.build(Img, Dec, EntryVA, "owner");
  };
  auto HasBlockAt = [&](const LowFunc &Function, va_t Address) {
    return std::any_of(
        Function.Blocks.begin(), Function.Blocks.end(),
        [&](const LowBlock &Block) { return Block.StartAddr == Address; });
  };

  BinaryImage Authoritative = MakeImage();
  Authoritative.KnownCodeRanges.emplace_back(EntryVA, EntryVA + 0x40);
  const LowFunc AuthoritativeFunction =
      Build(Authoritative, &ContinuationRoots);
  EXPECT_TRUE(HasBlockAt(AuthoritativeFunction, HiddenContinuationVA));
  EXPECT_FALSE(HasBlockAt(AuthoritativeFunction, InstructionInteriorVA));
  EXPECT_FALSE(HasBlockAt(AuthoritativeFunction, NestedKnownEntryVA));

  BinaryImage ImageGlobalOnly = MakeImage();
  ImageGlobalOnly.KnownCodeRanges.emplace_back(EntryVA, EntryVA + 0x40);
  ImageGlobalOnly.CodeRefTargets.insert(HiddenContinuationVA);
  EXPECT_FALSE(
      HasBlockAt(Build(ImageGlobalOnly, nullptr), HiddenContinuationVA));

  BinaryImage WeakNextEntryBoundOnly = MakeImage();
  EXPECT_FALSE(HasBlockAt(Build(WeakNextEntryBoundOnly, &ContinuationRoots),
                          HiddenContinuationVA));
}

TEST(CFGBuilderCoverage,
     ReachesAddressTakenRootsDiscoveredInsideExceptionalRoots) {
  constexpr va_t EntryVA = 0x140001000;
  constexpr va_t ExceptionalRootVA = EntryVA + 0x20;
  constexpr va_t AddressTakenRootVA = EntryVA + 0x40;
  constexpr va_t EndVA = EntryVA + 0x50;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = EntryVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = EntryVA;
  Text.Size = EndVA - EntryVA;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  Text.Data[0] = 0xc3; // ret; ordinary traversal cannot reach either root
  const size_t HandlerOffset = ExceptionalRootVA - EntryVA;
  Text.Data[HandlerOffset] = 0x48; // lea rdx,[rip+disp32]
  Text.Data[HandlerOffset + 1] = 0x8d;
  Text.Data[HandlerOffset + 2] = 0x15;
  const int32_t Displacement =
      static_cast<int32_t>(AddressTakenRootVA - (ExceptionalRootVA + 7));
  writeLE<int32_t>(Text.Data.data() + HandlerOffset + 3, Displacement);
  Text.Data[HandlerOffset + 7] = 0xc3;
  Text.Data[AddressTakenRootVA - EntryVA] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = EntryVA;
  TextSection.Size = EndVA - EntryVA;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));

  ExceptionFunction EH;
  EH.CodeRange = {EntryVA, EndVA};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {EntryVA, EntryVA + 1};
  Scope.Kind = SEHScopeKind::Finally;
  Scope.FilterOrFinallyVA = ExceptionalRootVA;
  Scope.HandlerVA = ExceptionalRootVA;
  EH.SEH->Scopes.push_back(Scope);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  const std::set<va_t> FunctionEntries{EntryVA};
  Builder.setKnownFuncEntries(&FunctionEntries);
  const LowFunc Function = Builder.build(Img, Dec, EntryVA, "owner");

  const auto HasBlockAt = [&](va_t Address) {
    return std::any_of(
        Function.Blocks.begin(), Function.Blocks.end(),
        [&](const LowBlock &Block) { return Block.StartAddr == Address; });
  };
  EXPECT_TRUE(HasBlockAt(ExceptionalRootVA));
  EXPECT_TRUE(HasBlockAt(AddressTakenRootVA));
}

TEST(CFGBuilderCoverage,
     BindsRelocationFreePCRelativeCodeAddressesToExactOutputs) {
  constexpr va_t EntryVA = 0x140001000;
  constexpr va_t HandlerVA = EntryVA + 0x20;
  constexpr va_t OwnerEntryVA = EntryVA + 0x100;
  constexpr va_t TargetVA = EntryVA + 0x120;

  auto MakeImage = [&](bool ReturnAddress) {
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Bits = Bitness::Bits64;
    Img.Format = BinaryFormat::COFF;
    Img.Base = 0x140000000;
    Img.Entry = EntryVA;

    Segment Text;
    Text.Name = ".text";
    Text.VA = EntryVA;
    Text.Size = 0x140;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0xcc);
    const std::vector<uint8_t> Prefix =
        ReturnAddress
            ? std::vector<uint8_t>{0x48, 0x8d, 0x05, 0x19,
                                   0x01, 0x00, 0x00, 0xc3}
            // lea rax,[rip+0x119]; ret
            : std::vector<uint8_t>{
                  0x48, 0x8d, 0x0d, 0x19, 0x01, 0x00,
                  0x00, 0x31, 0xc0, 0xc3}; // lea rcx,...; xor eax,eax; ret
    std::copy(Prefix.begin(), Prefix.end(), Text.Data.begin());
    Text.Data[TargetVA - EntryVA] = 0xc3;
    Img.Segments.push_back(std::move(Text));

    Section TextSection;
    TextSection.Name = ".text";
    TextSection.VA = EntryVA;
    TextSection.Size = 0x140;
    TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Img.Sections.push_back(std::move(TextSection));
    Img.KnownCodeRanges.emplace_back(EntryVA, EntryVA + 0x40);
    Img.KnownCodeRanges.emplace_back(OwnerEntryVA, EntryVA + 0x140);
    return Img;
  };

  const std::set<va_t> FunctionEntries{EntryVA, OwnerEntryVA};
  auto Build = [&](BinaryImage &Img) {
    Decoder Dec;
    EXPECT_TRUE(Dec.init(Arch::X64));
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(&FunctionEntries);
    return Builder.build(Img, Dec, EntryVA, "code_address_source");
  };
  auto FindOccurrence = [&](const LowFunc &Function) {
    return std::find_if(
        Function.RelocatedInstructionAddressOccurrences.begin(),
        Function.RelocatedInstructionAddressOccurrences.end(),
        [&](const RelocatedInstructionAddressOccurrence &Occurrence) {
          return Occurrence.InstructionAddr == EntryVA &&
                 Occurrence.TargetVA == TargetVA &&
                 Occurrence.Authority == RelocatedInstructionAddressProofKind::
                                             X86PCRelativeCodeAddress;
        });
  };
  auto CheckOccurrence = [&](const LowFunc &Function,
                             const NdVar &ExpectedOutput) {
    auto Occurrence = FindOccurrence(Function);
    ASSERT_NE(Occurrence,
              Function.RelocatedInstructionAddressOccurrences.end());
    EXPECT_EQ(
        std::count_if(Function.RelocatedInstructionAddressOccurrences.begin(),
                      Function.RelocatedInstructionAddressOccurrences.end(),
                      [&](const RelocatedInstructionAddressOccurrence &Item) {
                        return Item.InstructionAddr == EntryVA &&
                               Item.TargetVA == TargetVA;
                      }),
        1u);
    EXPECT_EQ(Occurrence->FieldVA, InvalidVA);
    EXPECT_EQ(Occurrence->TargetOwnerVA, EntryVA);
    EXPECT_EQ(Occurrence->Width, 8u);
    EXPECT_EQ(Occurrence->Provenance, ConstantAddressProvenance::CodeAddress);
    EXPECT_TRUE(Occurrence->PCRelativeFromInstructionEnd);
    EXPECT_TRUE(Occurrence->DefinesOutput);
    EXPECT_FALSE(Occurrence->OutputMayDepend);
    EXPECT_EQ(Occurrence->OutputOpcode, NdOp::COPY);
    EXPECT_EQ(Occurrence->OutputWitness, ExpectedOutput);
    EXPECT_EQ(Occurrence->InputIndex, -1);
    EXPECT_EQ(Function.CodeRefTargets, (std::vector<va_t>{TargetVA}));

    size_t MatchingOps = 0;
    for (const LowBlock &Block : Function.Blocks)
      for (const LowOp &Op : Block.Ops)
        if (Op.Addr == Occurrence->InstructionAddr &&
            Op.Seq == Occurrence->OpSeq &&
            Op.Opcode == Occurrence->OutputOpcode &&
            Op.Output == Occurrence->OutputWitness)
          ++MatchingOps;
    EXPECT_EQ(MatchingOps, 1u);
  };

  BinaryImage PositiveImage = MakeImage(true);
  const LowFunc Positive = Build(PositiveImage);
  CheckOccurrence(Positive, NdVar::reg(x86reg::RAX, 8));
  const auto PositiveEvidence =
      pipeline_detail::collectReturnedCodeEvidenceForTesting(
          PositiveImage, {Positive}, {TargetVA});
  ASSERT_TRUE(PositiveEvidence.AnalysisComplete);
  ASSERT_EQ(PositiveEvidence.CompleteByFunction.size(), 1u);
  ASSERT_EQ(PositiveEvidence.TargetsByFunction.size(), 1u);
  EXPECT_TRUE(PositiveEvidence.CompleteByFunction.front());
  EXPECT_EQ(PositiveEvidence.TargetsByFunction.front(),
            (std::set<va_t>{TargetVA}));

  // An exceptional entry has personality-defined register/frame state and is
  // not a value-preserving predecessor of the ordinary RETURN.  Model the
  // adversarial shape that originally made an unwind self-cycle grow a fresh
  // stack epoch on every data-flow round.  A deliberately small budget proves
  // returned-code evidence follows ordinary edges only.
  LowFunc ExceptionalSelfCycle = Positive;
  ASSERT_FALSE(ExceptionalSelfCycle.Blocks.empty());
  LowBlock &EntryBlock = ExceptionalSelfCycle.Blocks.front();
  ExceptionalEdge SelfEdge;
  SelfEdge.BlockId = EntryBlock.Id;
  SelfEdge.TargetVA = EntryBlock.StartAddr;
  SelfEdge.Kind = ExceptionalEdgeKind::CxxCatch;
  EntryBlock.ExceptionalPreds.push_back(SelfEdge);
  EntryBlock.ExceptionalSuccs.push_back(SelfEdge);

  LowOp StackEpochStep;
  StackEpochStep.Opcode = NdOp::INT_SUB;
  StackEpochStep.Output = NdVar::reg(x86reg::RSP, 8);
  StackEpochStep.addInput(NdVar::reg(x86reg::RSP, 8));
  StackEpochStep.addInput(NdVar::scalar(8, 8));
  StackEpochStep.Addr = EntryVA + 7;
  StackEpochStep.Seq = 1000;
  const auto ReturnOp =
      std::find_if(EntryBlock.Ops.begin(), EntryBlock.Ops.end(),
                   [](const LowOp &Op) { return Op.Opcode == NdOp::RETURN; });
  ASSERT_NE(ReturnOp, EntryBlock.Ops.end());
  EntryBlock.Ops.insert(ReturnOp, StackEpochStep);

  const auto ExceptionalEvidence =
      pipeline_detail::collectReturnedCodeEvidenceForTesting(
          PositiveImage, {ExceptionalSelfCycle}, {TargetVA}, 64);
  ASSERT_TRUE(ExceptionalEvidence.AnalysisComplete);
  ASSERT_EQ(ExceptionalEvidence.CompleteByFunction.size(), 1u);
  ASSERT_EQ(ExceptionalEvidence.TargetsByFunction.size(), 1u);
  EXPECT_TRUE(ExceptionalEvidence.CompleteByFunction.front());
  EXPECT_EQ(ExceptionalEvidence.TargetsByFunction.front(),
            (std::set<va_t>{TargetVA}));

  BinaryImage RoleSeparatedImage = MakeImage(false);
  const std::vector<uint8_t> HandlerBytes{
      0x48, 0x8d, 0x05, 0xf9,
      0x00, 0x00, 0x00, 0xc3}; // lea rax,[rip+0xf9]; ret
  std::copy(HandlerBytes.begin(), HandlerBytes.end(),
            RoleSeparatedImage.Segments.front().Data.begin() +
                (HandlerVA - EntryVA));
  ExceptionFunction Exception;
  Exception.CodeRange = {EntryVA, EntryVA + 0x40};
  Exception.Cxx.emplace();
  CxxUnwindAction Cleanup;
  Cleanup.ActionVA = HandlerVA;
  Exception.Cxx->UnwindMap.push_back(Cleanup);
  RoleSeparatedImage.ExceptionMetadata.Functions.push_back(
      std::move(Exception));

  LowFunc RoleSeparated = Build(RoleSeparatedImage);
  CheckOccurrence(RoleSeparated, NdVar::reg(x86reg::RCX, 8));
  EXPECT_EQ(RoleSeparated.ModuleAnalysisRoots,
            (std::set<va_t>{EntryVA, HandlerVA}));
  EXPECT_EQ(RoleSeparated.OrdinaryModuleAnalysisRoots,
            (std::set<va_t>{EntryVA}));
  LowBlock *HandlerBlock = RoleSeparated.blockFor(HandlerVA);
  ASSERT_NE(HandlerBlock, nullptr);
  ExceptionalEdge HandlerSelfEdge;
  HandlerSelfEdge.BlockId = HandlerBlock->Id;
  HandlerSelfEdge.TargetVA = HandlerVA;
  HandlerSelfEdge.Kind = ExceptionalEdgeKind::CxxCleanup;
  HandlerBlock->ExceptionalPreds.push_back(HandlerSelfEdge);
  HandlerBlock->ExceptionalSuccs.push_back(HandlerSelfEdge);

  const auto RoleSeparatedEvidence =
      pipeline_detail::collectReturnedCodeEvidenceForTesting(
          RoleSeparatedImage, {RoleSeparated}, {TargetVA}, 64);
  ASSERT_TRUE(RoleSeparatedEvidence.AnalysisComplete);
  ASSERT_EQ(RoleSeparatedEvidence.CompleteByFunction.size(), 1u);
  ASSERT_EQ(RoleSeparatedEvidence.TargetsByFunction.size(), 1u);
  EXPECT_TRUE(RoleSeparatedEvidence.CompleteByFunction.front());
  EXPECT_TRUE(RoleSeparatedEvidence.TargetsByFunction.front().empty());

  // A positive role set, rather than an address blacklist, preserves a VA
  // that is both an exceptional entry and a valid ordinary continuation.
  LowFunc DualRole = RoleSeparated;
  DualRole.OrdinaryModuleAnalysisRoots.insert(HandlerVA);
  const auto DualRoleEvidence =
      pipeline_detail::collectReturnedCodeEvidenceForTesting(
          RoleSeparatedImage, {DualRole}, {TargetVA}, 64);
  ASSERT_TRUE(DualRoleEvidence.AnalysisComplete);
  ASSERT_EQ(DualRoleEvidence.CompleteByFunction.size(), 1u);
  ASSERT_EQ(DualRoleEvidence.TargetsByFunction.size(), 1u);
  EXPECT_TRUE(DualRoleEvidence.CompleteByFunction.front());
  EXPECT_EQ(DualRoleEvidence.TargetsByFunction.front(),
            (std::set<va_t>{TargetVA}));

  BinaryImage NegativeImage = MakeImage(false);
  const LowFunc Negative = Build(NegativeImage);
  CheckOccurrence(Negative, NdVar::reg(x86reg::RCX, 8));
  const auto NegativeEvidence =
      pipeline_detail::collectReturnedCodeEvidenceForTesting(
          NegativeImage, {Negative}, {TargetVA});
  ASSERT_TRUE(NegativeEvidence.AnalysisComplete);
  ASSERT_EQ(NegativeEvidence.CompleteByFunction.size(), 1u);
  ASSERT_EQ(NegativeEvidence.TargetsByFunction.size(), 1u);
  EXPECT_TRUE(NegativeEvidence.CompleteByFunction.front());
  EXPECT_TRUE(NegativeEvidence.TargetsByFunction.front().empty());
  ASSERT_EQ(NegativeEvidence.OccurrencesByFunction.size(), 1u);
  ASSERT_EQ(NegativeEvidence.OccurrencesByFunction.front().size(), 1u);
  EXPECT_TRUE(NegativeEvidence.OccurrencesByFunction.front().front().Complete);
  EXPECT_TRUE(
      NegativeEvidence.OccurrencesByFunction.front().front().Targets.empty());
}

TEST(CFGBuilderCoverage,
     KeepsReturnedCodeEvidenceSeparateForIndependentReturnOccurrences) {
  constexpr va_t EntryVA = 0x140001000;
  constexpr va_t OwnerVA = EntryVA + 0x100;
  constexpr va_t FirstTargetVA = OwnerVA + 0x10;
  constexpr va_t SecondTargetVA = OwnerVA + 0x20;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = EntryVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = EntryVA;
  Text.Size = 0x140;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const std::vector<uint8_t> Body{
      0x85, 0xc9,                               // test ecx,ecx
      0x74, 0x08,                               // je second
      0x48, 0x8d, 0x05, 0x05, 0x01, 0x00, 0x00, // lea rax,FirstTargetVA
      0xc3,                                     // ret
      0x48, 0x8d, 0x05, 0x0d, 0x01, 0x00, 0x00, // lea rax,SecondTargetVA
      0xc3};                                    // ret
  std::copy(Body.begin(), Body.end(), Text.Data.begin());
  Text.Data[FirstTargetVA - EntryVA] = 0xc3;
  Text.Data[SecondTargetVA - EntryVA] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = EntryVA;
  TextSection.Size = 0x140;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(EntryVA, EntryVA + Body.size());
  Img.KnownCodeRanges.emplace_back(OwnerVA, OwnerVA + 0x40);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  const std::set<va_t> FunctionEntries{EntryVA, OwnerVA};
  Builder.setKnownFuncEntries(&FunctionEntries);
  const LowFunc Function = Builder.build(Img, Dec, EntryVA, "two_returns");

  const auto Evidence = pipeline_detail::collectReturnedCodeEvidenceForTesting(
      Img, {Function}, {FirstTargetVA, SecondTargetVA});
  ASSERT_TRUE(Evidence.AnalysisComplete);
  ASSERT_EQ(Evidence.OccurrencesByFunction.size(), 1u);
  const auto &Occurrences = Evidence.OccurrencesByFunction.front();
  ASSERT_EQ(Occurrences.size(), 2u);
  EXPECT_LT(std::tie(Occurrences[0].ReturnAddr, Occurrences[0].ReturnSeq),
            std::tie(Occurrences[1].ReturnAddr, Occurrences[1].ReturnSeq));
  EXPECT_TRUE(Occurrences[0].Complete);
  EXPECT_TRUE(Occurrences[1].Complete);
  EXPECT_EQ(Occurrences[0].Targets, (std::vector<va_t>{FirstTargetVA}));
  EXPECT_EQ(Occurrences[1].Targets, (std::vector<va_t>{SecondTargetVA}));
  EXPECT_EQ(Evidence.TargetsByFunction.front(),
            (std::set<va_t>{FirstTargetVA, SecondTargetVA}));
}

TEST(CFGBuilderCoverage,
     KeepsMergedReturnedCodeTargetsCompleteButNotUniquelyAuthorized) {
  constexpr va_t EntryVA = 0x140001000;
  constexpr va_t OwnerVA = EntryVA + 0x100;
  constexpr va_t FirstTargetVA = OwnerVA + 0x10;
  constexpr va_t SecondTargetVA = OwnerVA + 0x20;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = EntryVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = EntryVA;
  Text.Size = 0x140;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const std::vector<uint8_t> Body{
      0x85, 0xc9,                               // test ecx,ecx
      0x74, 0x09,                               // je second
      0x48, 0x8d, 0x05, 0x05, 0x01, 0x00, 0x00, // lea rax,FirstTargetVA
      0xeb, 0x07,                               // jmp joined_return
      0x48, 0x8d, 0x05, 0x0c, 0x01, 0x00, 0x00, // lea rax,SecondTargetVA
      0xc3};                                    // joined_return: ret
  std::copy(Body.begin(), Body.end(), Text.Data.begin());
  Text.Data[FirstTargetVA - EntryVA] = 0xc3;
  Text.Data[SecondTargetVA - EntryVA] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section SourceSection;
  SourceSection.Name = ".text$catch";
  SourceSection.VA = EntryVA;
  SourceSection.Size = Body.size();
  SourceSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(SourceSection));
  Section FirstTargetSection;
  FirstTargetSection.Name = ".text$first";
  FirstTargetSection.VA = OwnerVA;
  FirstTargetSection.Size = 0x20;
  FirstTargetSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(FirstTargetSection));
  Section SecondTargetSection;
  SecondTargetSection.Name = ".text$second";
  SecondTargetSection.VA = SecondTargetVA;
  SecondTargetSection.Size = 0x20;
  SecondTargetSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(SecondTargetSection));
  Img.KnownCodeRanges.emplace_back(EntryVA, EntryVA + Body.size());
  Img.KnownCodeRanges.emplace_back(OwnerVA, OwnerVA + 0x40);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  const std::set<va_t> FunctionEntries{EntryVA, OwnerVA};
  Builder.setKnownFuncEntries(&FunctionEntries);
  const LowFunc Function = Builder.build(Img, Dec, EntryVA, "merged_return");

  const auto Evidence = pipeline_detail::collectReturnedCodeEvidenceForTesting(
      Img, {Function}, {FirstTargetVA, SecondTargetVA});
  ASSERT_TRUE(Evidence.AnalysisComplete);
  ASSERT_EQ(Evidence.OccurrencesByFunction.size(), 1u);
  ASSERT_EQ(Evidence.OccurrencesByFunction.front().size(), 1u);
  const LowCxxContinuationExitEvidence &Occurrence =
      Evidence.OccurrencesByFunction.front().front();
  EXPECT_TRUE(Occurrence.Complete);
  EXPECT_EQ(Occurrence.Targets,
            (std::vector<va_t>{FirstTargetVA, SecondTargetVA}));
  EXPECT_FALSE(Occurrence.uniqueTarget().has_value());
  EXPECT_EQ(Evidence.TargetsByFunction.front(),
            (std::set<va_t>{FirstTargetVA, SecondTargetVA}));

  const auto MissingCandidate =
      pipeline_detail::collectReturnedCodeEvidenceForTesting(Img, {Function},
                                                             {FirstTargetVA});
  ASSERT_TRUE(MissingCandidate.AnalysisComplete);
  ASSERT_EQ(MissingCandidate.CompleteByFunction.size(), 1u);
  EXPECT_FALSE(MissingCandidate.CompleteByFunction.front());
  EXPECT_TRUE(MissingCandidate.TargetsByFunction.front().empty());
  ASSERT_EQ(MissingCandidate.OccurrencesByFunction.size(), 1u);
  ASSERT_EQ(MissingCandidate.OccurrencesByFunction.front().size(), 1u);
  EXPECT_FALSE(MissingCandidate.OccurrencesByFunction.front().front().Complete);
  EXPECT_TRUE(
      MissingCandidate.OccurrencesByFunction.front().front().Targets.empty());
}

TEST(CFGBuilderCoverage,
     MarksReturnedCodeOccurrenceIncompleteWhenOnePathIsUnauthenticated) {
  constexpr va_t EntryVA = 0x140001000;
  constexpr va_t OwnerVA = EntryVA + 0x100;
  constexpr va_t TargetVA = OwnerVA + 0x10;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = EntryVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = EntryVA;
  Text.Size = 0x140;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const std::vector<uint8_t> Body{0x85, 0xc9, // test ecx,ecx
                                  0x74, 0x09, // je scalar_path
                                  0x48, 0x8d, 0x05, 0x05,
                                  0x01, 0x00, 0x00, // lea rax,TargetVA
                                  0xeb, 0x02,       // jmp joined_return
                                  0x31, 0xc0,       // scalar_path: xor eax,eax
                                  0xc3};            // joined_return: ret
  std::copy(Body.begin(), Body.end(), Text.Data.begin());
  Text.Data[TargetVA - EntryVA] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = EntryVA;
  TextSection.Size = 0x140;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(EntryVA, EntryVA + Body.size());
  Img.KnownCodeRanges.emplace_back(OwnerVA, OwnerVA + 0x40);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  const std::set<va_t> FunctionEntries{EntryVA, OwnerVA};
  Builder.setKnownFuncEntries(&FunctionEntries);
  const LowFunc Function =
      Builder.build(Img, Dec, EntryVA, "incomplete_return");

  const auto Evidence = pipeline_detail::collectReturnedCodeEvidenceForTesting(
      Img, {Function}, {TargetVA});
  ASSERT_TRUE(Evidence.AnalysisComplete);
  ASSERT_EQ(Evidence.CompleteByFunction.size(), 1u);
  EXPECT_FALSE(Evidence.CompleteByFunction.front());
  EXPECT_TRUE(Evidence.TargetsByFunction.front().empty());
  ASSERT_EQ(Evidence.OccurrencesByFunction.size(), 1u);
  ASSERT_EQ(Evidence.OccurrencesByFunction.front().size(), 1u);
  const LowCxxContinuationExitEvidence &Occurrence =
      Evidence.OccurrencesByFunction.front().front();
  EXPECT_FALSE(Occurrence.Complete);
  EXPECT_TRUE(Occurrence.Targets.empty());
  EXPECT_FALSE(Occurrence.uniqueTarget().has_value());
}

TEST(CFGBuilderCoverage,
     DoesNotExposePartialReturnedCodeEvidenceAfterBudgetFailure) {
  constexpr va_t EntryVA = 0x140001000;
  constexpr va_t OwnerVA = EntryVA + 0x100;
  constexpr va_t TargetVA = OwnerVA + 0x10;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = EntryVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = EntryVA;
  Text.Size = 0x120;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const std::vector<uint8_t> Body{0x48, 0x8d, 0x05, 0x09,
                                  0x01, 0x00, 0x00, // lea rax,TargetVA
                                  0xc3};            // ret
  std::copy(Body.begin(), Body.end(), Text.Data.begin());
  Text.Data[TargetVA - EntryVA] = 0xc3;
  Img.Segments.push_back(std::move(Text));

  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = EntryVA;
  TextSection.Size = 0x120;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(EntryVA, EntryVA + Body.size());
  Img.KnownCodeRanges.emplace_back(OwnerVA, OwnerVA + 0x20);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  const std::set<va_t> FunctionEntries{EntryVA, OwnerVA};
  Builder.setKnownFuncEntries(&FunctionEntries);
  const LowFunc Function = Builder.build(Img, Dec, EntryVA, "budget_return");

  const auto Evidence = pipeline_detail::collectReturnedCodeEvidenceForTesting(
      Img, {Function}, {TargetVA}, 0);
  EXPECT_FALSE(Evidence.AnalysisComplete);
  EXPECT_TRUE(Evidence.TargetsByFunction.empty());
  EXPECT_TRUE(Evidence.CompleteByFunction.empty());
  EXPECT_TRUE(Evidence.OccurrencesByFunction.empty());
}

TEST(CFGBuilderCoverage, BindsContinuationExitToRenumberedMedReturnOccurrence) {
  const LowFunc Low = continuationBindingLowFunc();
  const MedFunc Med =
      LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);

  ASSERT_TRUE(Med.CxxContinuationExitAnalysisComplete);
  ASSERT_EQ(Med.CxxContinuationExits.size(), 1u);
  const MedCxxContinuationExitEvidence &Exit = Med.CxxContinuationExits.front();
  EXPECT_EQ(Exit.BlockId, 1);
  EXPECT_EQ(Exit.ReturnAddr, Low.CxxContinuationExits.front().ReturnAddr);
  EXPECT_EQ(Exit.ReturnSeq, Low.CxxContinuationExits.front().ReturnSeq);
  EXPECT_EQ(Exit.Targets, Low.CxxContinuationExits.front().Targets);
  EXPECT_TRUE(Exit.Complete);
  EXPECT_EQ(Exit.uniqueTarget(),
            Low.CxxContinuationExits.front().uniqueTarget());

  const MedBlock *BoundBlock = nullptr;
  const MedOp *BoundReturn = nullptr;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Block.Id == Exit.BlockId && Op.Opcode == NdOp::RETURN &&
          Op.Addr == Exit.ReturnAddr && Op.OriginSeq == Exit.ReturnSeq) {
        BoundBlock = &Block;
        BoundReturn = &Op;
      }
  ASSERT_NE(BoundBlock, nullptr);
  ASSERT_NE(BoundReturn, nullptr);
  ASSERT_EQ(BoundReturn->NumInputs, 1u);
  EXPECT_EQ(Exit.ReturnValue, BoundReturn->Inputs[0]);
}

TEST(CFGBuilderCoverage,
     RejectsEntireContinuationExitPlanOnAmbiguousOrInvalidMedBinding) {
  LowFunc Ambiguous = continuationBindingLowFunc();
  LowBlock Duplicate = Ambiguous.Blocks.back();
  Duplicate.Id = 3;
  Duplicate.StartAddr += 0x10;
  Duplicate.EndAddr += 0x10;
  Duplicate.Preds.clear();
  Ambiguous.Blocks.push_back(std::move(Duplicate));
  const MedFunc AmbiguousMed =
      LowToMedConverter().convert(Ambiguous, Arch::X64, BinaryFormat::COFF);
  EXPECT_FALSE(AmbiguousMed.CxxContinuationExitAnalysisComplete);
  EXPECT_TRUE(AmbiguousMed.CxxContinuationExits.empty());

  LowFunc InvalidWidth = continuationBindingLowFunc();
  LowBlock NarrowReturn;
  NarrowReturn.Id = 3;
  NarrowReturn.StartAddr = InvalidWidth.Entry + 0x30;
  NarrowReturn.EndAddr = NarrowReturn.StartAddr + 1;
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = NarrowReturn.StartAddr;
  Return.Seq = 10;
  Return.addInput(NdVar::reg(x86reg::RAX, 4));
  NarrowReturn.Ops.push_back(std::move(Return));
  InvalidWidth.Blocks.push_back(std::move(NarrowReturn));
  LowCxxContinuationExitEvidence NarrowEvidence;
  NarrowEvidence.ReturnAddr = InvalidWidth.Entry + 0x30;
  NarrowEvidence.ReturnSeq = 10;
  NarrowEvidence.Targets = {InvalidWidth.Entry + 0x110};
  NarrowEvidence.Complete = true;
  InvalidWidth.CxxContinuationExits.push_back(std::move(NarrowEvidence));
  const MedFunc InvalidWidthMed =
      LowToMedConverter().convert(InvalidWidth, Arch::X64, BinaryFormat::COFF);
  EXPECT_FALSE(InvalidWidthMed.CxxContinuationExitAnalysisComplete);
  EXPECT_TRUE(InvalidWidthMed.CxxContinuationExits.empty());

  LowFunc Missing = continuationBindingLowFunc();
  Missing.CxxContinuationExits.front().ReturnSeq = 999;
  const MedFunc MissingMed =
      LowToMedConverter().convert(Missing, Arch::X64, BinaryFormat::COFF);
  EXPECT_FALSE(MissingMed.CxxContinuationExitAnalysisComplete);
  EXPECT_TRUE(MissingMed.CxxContinuationExits.empty());

  LowFunc InvalidTarget = continuationBindingLowFunc();
  InvalidTarget.CxxContinuationExits.front().Targets = {InvalidVA};
  const MedFunc InvalidTargetMed =
      LowToMedConverter().convert(InvalidTarget, Arch::X64, BinaryFormat::COFF);
  EXPECT_FALSE(InvalidTargetMed.CxxContinuationExitAnalysisComplete);
  EXPECT_TRUE(InvalidTargetMed.CxxContinuationExits.empty());
}

TEST(CFGBuilderCoverage,
     PublishesContinuationExitSnapshotOnlyAfterStablePipelineClosure) {
  constexpr va_t CatchVA = 0x140001000;
  constexpr va_t OwnerVA = CatchVA + 0x100;
  constexpr va_t TargetVA = OwnerVA + 0x20;

  auto Run = [](BinaryImage Img, std::optional<size_t> Budget) {
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.DumpLow = true;
    Options.EmitDumpOutput = false;
    Options.EHContinuationEvidenceBudgetForTesting = Budget;
    return Pipeline().run(Img, Context, Options);
  };
  auto FindLow = [](const PipelineResult &Result, va_t Entry) {
    return std::find_if(
        Result.LowFuncs.begin(), Result.LowFuncs.end(),
        [&](const LowFunc &Function) { return Function.Entry == Entry; });
  };
  auto FindMed = [](const PipelineResult &Result, va_t Entry) {
    return std::find_if(
        Result.MedFuncs.begin(), Result.MedFuncs.end(),
        [&](const MedFunc &Function) { return Function.Entry == Entry; });
  };
  auto HasBlockAt = [](const LowFunc &Function, va_t Address) {
    return std::any_of(
        Function.Blocks.begin(), Function.Blocks.end(),
        [&](const LowBlock &Block) { return Block.StartAddr == Address; });
  };

  const PipelineResult Stable = Run(continuationPipelineImage(), std::nullopt);
  ASSERT_TRUE(Stable.Success) << Stable.Error;
  const auto StableCatch = FindLow(Stable, CatchVA);
  const auto StableOwner = FindLow(Stable, OwnerVA);
  ASSERT_NE(StableCatch, Stable.LowFuncs.end());
  ASSERT_NE(StableOwner, Stable.LowFuncs.end());
  ASSERT_TRUE(StableCatch->CxxContinuationExitAnalysisComplete);
  ASSERT_EQ(StableCatch->CxxContinuationExits.size(), 1u);
  EXPECT_EQ(StableCatch->CxxContinuationExits.front().uniqueTarget(),
            std::optional<va_t>(TargetVA));
  EXPECT_TRUE(HasBlockAt(*StableOwner, TargetVA));

  const auto StableMedCatch = FindMed(Stable, CatchVA);
  ASSERT_NE(StableMedCatch, Stable.MedFuncs.end());
  EXPECT_TRUE(StableMedCatch->CxxContinuationExitAnalysisComplete);
  ASSERT_EQ(StableMedCatch->CxxContinuationExits.size(), 1u);
  EXPECT_EQ(StableMedCatch->CxxContinuationExits.front().uniqueTarget(),
            std::optional<va_t>(TargetVA));

  const PipelineResult ExactEmpty =
      Run(continuationPipelineImage(false), std::nullopt);
  ASSERT_TRUE(ExactEmpty.Success) << ExactEmpty.Error;
  const auto ExactEmptyCatch = FindLow(ExactEmpty, CatchVA);
  ASSERT_NE(ExactEmptyCatch, ExactEmpty.LowFuncs.end());
  ASSERT_TRUE(ExactEmptyCatch->CxxContinuationExitAnalysisComplete);
  ASSERT_EQ(ExactEmptyCatch->CxxContinuationExits.size(), 1u);
  EXPECT_TRUE(ExactEmptyCatch->CxxContinuationExits.front().Complete);
  EXPECT_TRUE(ExactEmptyCatch->CxxContinuationExits.front().Targets.empty());
  EXPECT_FALSE(
      ExactEmptyCatch->CxxContinuationExits.front().uniqueTarget().has_value());
  const auto ExactEmptyMedCatch = FindMed(ExactEmpty, CatchVA);
  ASSERT_NE(ExactEmptyMedCatch, ExactEmpty.MedFuncs.end());
  EXPECT_TRUE(ExactEmptyMedCatch->CxxContinuationExitAnalysisComplete);
  ASSERT_EQ(ExactEmptyMedCatch->CxxContinuationExits.size(), 1u);
  EXPECT_TRUE(ExactEmptyMedCatch->CxxContinuationExits.front().Complete);
  EXPECT_TRUE(ExactEmptyMedCatch->CxxContinuationExits.front().Targets.empty());

  const PipelineResult Exhausted = Run(continuationPipelineImage(), 0);
  ASSERT_TRUE(Exhausted.Success) << Exhausted.Error;
  const auto ExhaustedCatch = FindLow(Exhausted, CatchVA);
  const auto ExhaustedOwner = FindLow(Exhausted, OwnerVA);
  ASSERT_NE(ExhaustedCatch, Exhausted.LowFuncs.end());
  ASSERT_NE(ExhaustedOwner, Exhausted.LowFuncs.end());
  EXPECT_FALSE(ExhaustedCatch->CxxContinuationExitAnalysisComplete);
  EXPECT_TRUE(ExhaustedCatch->CxxContinuationExits.empty());
  EXPECT_FALSE(HasBlockAt(*ExhaustedOwner, TargetVA));
  EXPECT_TRUE(
      std::none_of(Exhausted.LowFuncs.begin(), Exhausted.LowFuncs.end(),
                   [](const LowFunc &Function) {
                     return Function.CxxContinuationExitAnalysisComplete ||
                            !Function.CxxContinuationExits.empty();
                   }));
}

TEST(CFGBuilderCoverage,
     RoutesExactLocalUnwindTargetFromUnlistedHelperToUniqueSEHOwner) {
  constexpr va_t SourceVA = 0x140001000;
  constexpr va_t DeclaredFinallyVA = SourceVA + 0x80;
  constexpr va_t OwnerVA = SourceVA + 0x100;
  constexpr va_t TargetVA = SourceVA + 0x120;
  constexpr va_t StubVA = SourceVA + 0x180;

  auto MakeImage = [&]() {
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Bits = Bitness::Bits64;
    Img.Format = BinaryFormat::COFF;
    Img.Base = 0x140000000;
    Img.Entry = SourceVA;

    Segment Text;
    Text.Name = ".text";
    Text.VA = SourceVA;
    Text.Size = 0x200;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0xcc);
    const std::vector<uint8_t> HelperBytes{
        0x48, 0x8d, 0x15, 0x19, 0x01, 0x00, 0x00, // lea rdx, TargetVA
        0xe8, 0x74, 0x01, 0x00, 0x00,             // call StubVA
        0xc3};                                    // ret
    std::copy(HelperBytes.begin(), HelperBytes.end(), Text.Data.begin());
    Text.Data[DeclaredFinallyVA - SourceVA] = 0xc3;
    Text.Data[OwnerVA - SourceVA] = 0xc3;
    Text.Data[TargetVA - SourceVA] = 0xc3;
    Text.Data[StubVA - SourceVA] = 0xc3;
    Img.Segments.push_back(std::move(Text));

    Section TextSection;
    TextSection.Name = ".text";
    TextSection.VA = SourceVA;
    TextSection.Size = 0x200;
    TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Img.Sections.push_back(std::move(TextSection));
    Img.KnownCodeRanges.emplace_back(SourceVA, SourceVA + HelperBytes.size());
    Img.KnownCodeRanges.emplace_back(DeclaredFinallyVA, DeclaredFinallyVA + 1);
    Img.KnownCodeRanges.emplace_back(OwnerVA, OwnerVA + 0x40);
    Img.KnownCodeRanges.emplace_back(StubVA, StubVA + 1);

    Import LocalUnwind;
    LocalUnwind.Module = "VCRUNTIME140.dll";
    LocalUnwind.Name = "_local_unwind";
    Img.Imports.push_back(std::move(LocalUnwind));
    EXPECT_TRUE(Img.recordImportStub(StubVA, 0));

    ExceptionFunction Owner;
    Owner.CodeRange = {OwnerVA, OwnerVA + 0x40};
    Owner.Kind = RuntimeFunctionKind::Primary;
    Owner.Encoding = ExceptionEncoding::X64UnwindV1;
    Owner.ParseStatus = ExceptionParseStatus::Complete;
    Owner.Personality = ExceptionPersonality::CSpecificHandler;
    Owner.SEH.emplace();
    SEHScopeRecord Scope;
    Scope.GuardedRange = {OwnerVA, TargetVA};
    Scope.Kind = SEHScopeKind::Finally;
    Scope.FilterOrFinallyVA = DeclaredFinallyVA;
    Scope.HandlerVA = DeclaredFinallyVA;
    Owner.SEH->Scopes.push_back(Scope);
    Img.ExceptionMetadata.Functions.push_back(std::move(Owner));

    ExceptionFunction Helper;
    Helper.CodeRange = {SourceVA, SourceVA + HelperBytes.size()};
    Helper.Kind = RuntimeFunctionKind::Primary;
    Helper.Encoding = ExceptionEncoding::X64UnwindV1;
    Helper.ParseStatus = ExceptionParseStatus::Complete;
    Img.ExceptionMetadata.Functions.push_back(std::move(Helper));
    return Img;
  };

  auto BuildHelper = [&](BinaryImage &Img) {
    Decoder Dec;
    EXPECT_TRUE(Dec.init(Arch::X64));
    CFGBuilder Builder;
    const std::set<va_t> FunctionEntries{SourceVA, DeclaredFinallyVA, OwnerVA,
                                         StubVA};
    Builder.setKnownFuncEntries(&FunctionEntries);
    return Builder.build(Img, Dec, SourceVA, "local_unwind_helper");
  };

  const std::set<va_t> FunctionEntries{SourceVA, DeclaredFinallyVA, OwnerVA,
                                       StubVA};
  BinaryImage PositiveImage = MakeImage();
  const LowFunc Positive = BuildHelper(PositiveImage);
  ASSERT_EQ(Positive.CodeRefTargets, (std::vector<va_t>{TargetVA}));
  const auto PositiveRoots =
      pipeline_detail::collectWindowsEHContinuationRootsForTesting(
          PositiveImage, {Positive}, FunctionEntries);
  ASSERT_TRUE(PositiveRoots.AnalysisComplete);
  EXPECT_EQ(PositiveRoots.RootsByOwner,
            (std::map<va_t, std::set<va_t>>{{OwnerVA, {TargetVA}}}));

  // A coarse executable import range cannot supply the exact callee identity
  // required by the local-unwind ABI proof.
  BinaryImage RangeOnlyImage = MakeImage();
  RangeOnlyImage.ImportStubIndices.clear();
  ASSERT_TRUE(RangeOnlyImage.recordImportStubRange(StubVA, 1));
  const LowFunc RangeOnly = BuildHelper(RangeOnlyImage);
  const auto RangeOnlyRoots =
      pipeline_detail::collectWindowsEHContinuationRootsForTesting(
          RangeOnlyImage, {RangeOnly}, FunctionEntries);
  ASSERT_TRUE(RangeOnlyRoots.AnalysisComplete);
  EXPECT_TRUE(RangeOnlyRoots.RootsByOwner.empty());

  // Two complete owners covering one target are an ambiguity, never a reason
  // to choose whichever record happens to appear first.
  BinaryImage AmbiguousImage = MakeImage();
  ExceptionFunction Ambiguous =
      AmbiguousImage.ExceptionMetadata.Functions.front();
  Ambiguous.CodeRange = {OwnerVA - 0x10, OwnerVA + 0x40};
  AmbiguousImage.ExceptionMetadata.Functions.push_back(std::move(Ambiguous));
  const LowFunc AmbiguousHelper = BuildHelper(AmbiguousImage);
  const auto AmbiguousRoots =
      pipeline_detail::collectWindowsEHContinuationRootsForTesting(
          AmbiguousImage, {AmbiguousHelper}, FunctionEntries);
  ASSERT_TRUE(AmbiguousRoots.AnalysisComplete);
  EXPECT_TRUE(AmbiguousRoots.RootsByOwner.empty());
}

TEST(FuncDetectorCoverage,
     PreservesVerifiedMachODirectCallInsideBroadRangeAcrossX86) {
  constexpr va_t ImageVA = 0x1000;
  constexpr va_t HelperVA = ImageVA + 0x10;
  constexpr va_t InvalidVA = ImageVA + 0x18;

  auto CheckArchitecture = [&](Arch TargetArch, Bitness TargetBits) {
    BinaryImage Img;
    Img.Arch = TargetArch;
    Img.Bits = TargetBits;
    Img.Format = BinaryFormat::MachO;
    Img.Base = ImageVA;
    Img.Entry = ImageVA;

    Segment Text;
    Text.Name = "__TEXT";
    Text.VA = ImageVA;
    Text.Size = 0x19;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = {
        0xe8, 0x0b, 0x00, 0x00, 0x00, // call HelperVA
        0xe8, 0x0e, 0x00, 0x00, 0x00, // call InvalidVA
        0xc3,                         // ret
        0x90, 0x90, 0x90, 0x90, 0x90,
        0xb8, 0x2a, 0x00, 0x00, 0x00, // mov eax, 42
        0xc3,                         // ret
        0x90, 0x90,
        0x0f, // truncated two-byte opcode
    };
    Img.Segments.push_back(std::move(Text));

    Symbol Covering = Symbol::makeFunc(ImageVA, 0x19);
    Covering.Name = "_main";
    Img.Symbols.push_back(std::move(Covering));
    Img.Exports.push_back({"_main", 0, ImageVA});
    Img.KnownCodeRanges.emplace_back(ImageVA, ImageVA + 0x19);

    Decoder Dec;
    ASSERT_TRUE(Dec.init(TargetArch));
    FuncDetector Detector;
    auto Functions = Detector.detect(Img, Dec);

    EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                            [=](const auto &F) { return F.first == ImageVA; }),
              1u);
    EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                            [=](const auto &F) { return F.first == HelperVA; }),
              1u)
        << "arch=" << static_cast<int>(TargetArch);
    EXPECT_EQ(
        std::count_if(Functions.begin(), Functions.end(),
                      [=](const auto &F) { return F.first == InvalidVA; }),
        0u)
        << "arch=" << static_cast<int>(TargetArch);
  };

  CheckArchitecture(Arch::X64, Bitness::Bits64);
  CheckArchitecture(Arch::X86, Bitness::Bits32);
}

TEST(CFGBuilderCoverage, StopsAtTrapTerminatorBeforeEmbeddedData) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;

  Segment Text;
  Text.VA = Img.Base + 0x1000;
  Text.Size = 2;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0xcc, 0x61}; // int3; embedded POPA byte (invalid in x86-64)
  Img.Segments.push_back(std::move(Text));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  LowFunc Function =
      Builder.build(Img, Dec, Img.Base + 0x1000, "trap_terminator");

  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_EQ(Function.DecodedInstructionCount, 1u);
  EXPECT_EQ(Function.LiftedInstructionCount, 1u);
}

TEST(CFGBuilderCoverage, DecodesOperandSizePrefixedFence) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;

  Segment Text;
  Text.VA = Img.Base + 0x1000;
  Text.Size = 5;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0x66, 0x0f, 0xae, 0xf8, 0xc3}; // 66 sfence; ret
  Img.Segments.push_back(std::move(Text));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Fence{};
  ASSERT_EQ(Dec.decodeOne(Img.Segments[0].Data.data(),
                          Img.Segments[0].Data.size(), Img.Base + 0x1000,
                          Fence),
            4);
  ASSERT_NE(Fence.Raw, nullptr);
  EXPECT_EQ(Fence.Id, X86_INS_SFENCE);
  EXPECT_EQ(Fence.Raw->address, Img.Base + 0x1000);
  EXPECT_EQ(Fence.Raw->bytes[0], 0x66);

  Dec.setDetail(false);
  DecodedInsn LightFence{};
  EXPECT_EQ(Dec.decodeOneLight(Img.Segments[0].Data.data(),
                               Img.Segments[0].Data.size(), Img.Base + 0x1000,
                               LightFence),
            4);
  EXPECT_EQ(LightFence.Id, X86_INS_SFENCE);
  Dec.setDetail(true);

  CFGBuilder Builder;
  LowFunc Function =
      Builder.build(Img, Dec, Img.Base + 0x1000, "prefixed_fence");

  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_EQ(Function.DecodedInstructionCount, 2u);
  EXPECT_EQ(Function.LiftedInstructionCount, 2u);
}

TEST(MedLLVMEmitterAudit, CountsUnhandledValueIntrinsic) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "backend_audit_probe";
  Func.CC = CallingConv::SysV_AMD64;

  MedVar Output;
  Output.Kind = MedVar::Temp;
  Output.TheArch = Arch::X64;
  Output.Id = 1;
  Output.Size = 8;

  MedOp Intrinsic;
  Intrinsic.Opcode = NdOp::INTRINSIC;
  Intrinsic.Output = Output;
  Intrinsic.addInput(MedVar::makeConst(0xffff, 2));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Output);

  MedBlock Block;
  Block.Id = 0;
  Block.Ops = {Intrinsic, Return};
  Func.Blocks.push_back(std::move(Block));

  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit({Func}, Context, "backend-audit", Arch::X64);
  ASSERT_NE(Module, nullptr);
  EXPECT_EQ(Emitter.unhandledValueIntrinsicCount(), 1u);
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  auto OriginalVA = rewrite_source::getOriginalVA(*Emitted);
  ASSERT_TRUE(static_cast<bool>(OriginalVA))
      << llvm::toString(OriginalVA.takeError());
  ASSERT_TRUE(OriginalVA->has_value());
  EXPECT_EQ(**OriginalVA, Func.Entry);
}

TEST_F(X86_64_CFGEntry, TrueEntryIsBlockZeroAndBackwardEdgeSurvives) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_backward_entry.o not built";
  auto Run = liftToLowIR(testObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;
  EXPECT_EQ(countOccurrences(Run.out, "=== LowIR Dump ==="), 1U)
      << "lift dump must have one stable representation:\n"
      << Run.out;

  std::string Dump = functionDump(Run.out, "test_backward_entry");
  ASSERT_FALSE(Dump.empty()) << Run.out;
  ParsedCfg Cfg = parseCfg(Dump);
  ASSERT_NE(Cfg.Entry, 0U) << Dump;
  ASSERT_TRUE(Cfg.Starts.count(0)) << Dump;
  EXPECT_EQ(Cfg.Starts.at(0), Cfg.Entry) << Dump;

  for (int Id = 0; Id < static_cast<int>(Cfg.Starts.size()); ++Id)
    EXPECT_TRUE(Cfg.Starts.count(Id)) << "missing block id " << Id << "\n"
                                      << Dump;

  ASSERT_TRUE(Cfg.Succs.count(0)) << Dump;
  bool HasBackwardEdge = false;
  for (int Succ : Cfg.Succs.at(0)) {
    ASSERT_TRUE(Cfg.Starts.count(Succ)) << Dump;
    HasBackwardEdge |= Cfg.Starts.at(Succ) < Cfg.Entry;
  }
  EXPECT_TRUE(HasBackwardEdge) << Dump;
}

TEST_F(X86_64_CFGEntry, MedSsaHasNoCallClobberDefinitionCollision) {
  auto Run = liftToMedIR(testObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;
  EXPECT_EQ(countOccurrences(Run.out, "=== MedIR Dump ==="), 1U)
      << "lift dump must have one stable representation:\n"
      << Run.out;
  EXPECT_EQ(Run.err.find("call clobber duplicates explicit definition"),
            std::string::npos)
      << Run.err;
}

TEST_F(X86_64_CFGEntry, RemovedEmptyTargetLeavesNoOutOfRangeSuccessor) {
  auto Run = liftToMedIR(testObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;
  std::string Dump = functionDump(Run.out, "test_unmapped_branch");
  ASSERT_FALSE(Dump.empty()) << Run.out;
  EXPECT_NE(Dump.find("block 0 succs=[]"), std::string::npos) << Dump;
  EXPECT_EQ(Run.err.find("invalid successor block id"), std::string::npos)
      << Run.err;
}

TEST_F(X86_64_CFGEntry, X87StateOpsAreSideEffectsAndPreserveIntegerResult) {
  auto Run = liftToLLVMIR(testObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;
  EXPECT_EQ(Run.err.find("INTRINSIC unhandled intrinsic"), std::string::npos)
      << Run.err;

  std::string IR = llvmFunction(Run.out, "test_x87_state_ops");
  ASSERT_FALSE(IR.empty()) << Run.out;
  EXPECT_NE(IR.find("fninit"), std::string::npos) << IR;
  EXPECT_NE(IR.find("fnclex"), std::string::npos) << IR;
  EXPECT_EQ(IR.find("ret i64 0"), std::string::npos) << IR;
}

TEST_F(X86_64_CFGEntry, OptimizedAndUnoptimizedLLVMEmissionSucceed) {
  auto Optimized = liftToLLVMIR(testObj());
  ASSERT_EQ(Optimized.exitCode, 0) << Optimized.err;

  auto Unoptimized = liftToLLVMIRUnopt(testObj());
  ASSERT_EQ(Unoptimized.exitCode, 0) << Unoptimized.err;
}
