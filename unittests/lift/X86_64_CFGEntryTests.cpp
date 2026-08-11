//===- X86_64_CFGEntryTests.cpp - backward-entry CFG regressions --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <regex>
#include <sstream>
#include <string>
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
