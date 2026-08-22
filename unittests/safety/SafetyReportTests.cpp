//===- SafetyReportTests.cpp - Stable aggregate report contract ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/safety/Safety.h"

#include "llvm/Support/JSON.h"

#include <limits>

using namespace neverd::safety;

TEST(SafetyReport, PipelineCoverageRejectsPartialFunctionLifts) {
  neverd::PipelineResult Result;

  neverd::PipelineFunctionAudit Accepted;
  Accepted.Entry = 0x1000;
  Accepted.Disposition = neverd::PipelineFunctionDisposition::Accepted;
  Accepted.HasLowIR = true;
  Accepted.HasMedIR = true;
  Accepted.MedIRVerified = true;
  Result.FunctionAudits.push_back(Accepted);
  Result.LowFuncs.resize(1);
  Result.LowFuncs.front().Entry = Accepted.Entry;
  Result.MedFuncs.resize(1);
  Result.MedFuncs.front().Entry = Accepted.Entry;
  EXPECT_FALSE(validatePipelineCoverage(Result).has_value());

  neverd::PipelineFunctionAudit Import;
  Import.Entry = 0x2000;
  Import.Disposition = neverd::PipelineFunctionDisposition::SkippedImportStub;
  Result.FunctionAudits.push_back(Import);
  EXPECT_FALSE(validatePipelineCoverage(Result).has_value());

  Result.FunctionAudits.front().TruncatedPaths.push_back(0x1010);
  EXPECT_TRUE(validatePipelineCoverage(Result).has_value());
  Result.FunctionAudits.front().TruncatedPaths.clear();

  Result.FunctionAudits.front().DecodedInstructions = 1;
  Result.FunctionAudits.front().LiftedInstructions = 2;
  EXPECT_TRUE(validatePipelineCoverage(Result).has_value());
  Result.FunctionAudits.front().LiftedInstructions = 1;
  EXPECT_FALSE(validatePipelineCoverage(Result).has_value());

  neverd::PipelineFunctionAudit Rejected;
  Rejected.Entry = 0x3000;
  Rejected.Disposition =
      neverd::PipelineFunctionDisposition::RejectedIncomplete;
  Result.FunctionAudits.push_back(Rejected);
  const std::optional<std::string> Error = validatePipelineCoverage(Result);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("0x3000"), std::string::npos);
  EXPECT_NE(Error->find("rejected-incomplete"), std::string::npos);
}

TEST(SafetyReport, PipelineCoverageMatchesAcceptedFunctionIdentities) {
  auto accepted = [](neverd::va_t Entry) {
    neverd::PipelineFunctionAudit Audit;
    Audit.Entry = Entry;
    Audit.Disposition = neverd::PipelineFunctionDisposition::Accepted;
    Audit.HasLowIR = true;
    Audit.HasMedIR = true;
    Audit.MedIRVerified = true;
    return Audit;
  };

  neverd::PipelineResult DuplicateAudit;
  DuplicateAudit.FunctionAudits = {accepted(0x1000), accepted(0x1000)};
  DuplicateAudit.LowFuncs.resize(2);
  DuplicateAudit.LowFuncs[0].Entry = 0x1000;
  DuplicateAudit.LowFuncs[1].Entry = 0x2000;
  DuplicateAudit.MedFuncs.resize(2);
  DuplicateAudit.MedFuncs[0].Entry = 0x1000;
  DuplicateAudit.MedFuncs[1].Entry = 0x2000;
  EXPECT_TRUE(validatePipelineCoverage(DuplicateAudit).has_value());

  neverd::PipelineResult CrossedInventories;
  CrossedInventories.FunctionAudits = {accepted(0x1000)};
  CrossedInventories.LowFuncs.resize(1);
  CrossedInventories.LowFuncs[0].Entry = 0x1000;
  CrossedInventories.MedFuncs.resize(1);
  CrossedInventories.MedFuncs[0].Entry = 0x2000;
  EXPECT_TRUE(validatePipelineCoverage(CrossedInventories).has_value());
}

TEST(SafetyReport, PipelineCoverageAuthenticatesRemovedJumpTableTargets) {
  neverd::PipelineResult Result;

  neverd::PipelineFunctionAudit Accepted;
  Accepted.Entry = 0x1000;
  Accepted.Disposition = neverd::PipelineFunctionDisposition::Accepted;
  Accepted.HasLowIR = true;
  Accepted.HasMedIR = true;
  Accepted.MedIRVerified = true;
  Result.FunctionAudits.push_back(Accepted);

  neverd::PipelineFunctionAudit Removed;
  Removed.Entry = 0x1010;
  Removed.Disposition =
      neverd::PipelineFunctionDisposition::RemovedJumpTableTarget;
  Result.FunctionAudits.push_back(Removed);

  Result.LowFuncs.resize(1);
  Result.LowFuncs.front().Entry = Accepted.Entry;
  Result.MedFuncs.resize(1);
  Result.MedFuncs.front().Entry = Accepted.Entry;

  // A disposition label alone is not proof that the removed candidate was an
  // interior case.  The final published function must retain both the table
  // target and its decoded instruction boundary.
  EXPECT_TRUE(validatePipelineCoverage(Result).has_value());

  neverd::JumpTable Table;
  Table.InsnAddr = 0x1008;
  Table.Targets = {Removed.Entry};
  Result.LowFuncs.front().JumpTables.push_back(std::move(Table));
  neverd::LowBlock Block;
  Block.Id = 0;
  Block.InstructionBoundaries.push_back({Removed.Entry, 4});
  Result.LowFuncs.front().Blocks.push_back(std::move(Block));
  EXPECT_FALSE(validatePipelineCoverage(Result).has_value());

  // A target that the loader still authenticates as its own function entry is
  // not an interior-label exemption, even if an erroneous table absorbed it.
  neverd::BinaryImage Img;
  Img.Arch = neverd::Arch::X64;
  Img.Format = neverd::BinaryFormat::ELF;
  neverd::Segment Text;
  Text.Name = "text";
  Text.VA = 0x1000;
  Text.Size = 0x40;
  Text.FileSz = 0x40;
  Text.Data.resize(0x40);
  Text.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  Img.Segments.push_back(std::move(Text));
  Img.Symbols.push_back(neverd::Symbol::makeFunc(Removed.Entry, 4));
  EXPECT_TRUE(validatePipelineCoverage(Result, &Img).has_value());
}

TEST(SafetyReport, PipelineCoverageRejectsUnresolvedIndirectBranches) {
  neverd::PipelineResult Result;
  neverd::PipelineFunctionAudit Accepted;
  Accepted.Entry = 0x1000;
  Accepted.Disposition = neverd::PipelineFunctionDisposition::Accepted;
  Accepted.HasLowIR = true;
  Accepted.HasMedIR = true;
  Accepted.MedIRVerified = true;
  Result.FunctionAudits.push_back(Accepted);

  neverd::LowFunc Low;
  Low.Entry = Accepted.Entry;
  neverd::LowBlock Block;
  Block.Id = 0;
  neverd::LowOp Indirect;
  Indirect.Opcode = neverd::NdOp::INDIR_BR;
  Indirect.Addr = 0x1010;
  Block.Ops.push_back(Indirect);
  Low.Blocks.push_back(std::move(Block));
  Result.LowFuncs.push_back(std::move(Low));
  Result.MedFuncs.resize(1);
  Result.MedFuncs.front().Entry = Accepted.Entry;

  std::optional<std::string> Error = validatePipelineCoverage(Result);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("0x1010"), std::string::npos);
  EXPECT_NE(Error->find("unresolved-indirect-branch"), std::string::npos);

  neverd::JumpTable Table;
  Table.InsnAddr = 0x1010;
  Table.Targets = {0x1020, 0x1030};
  Result.LowFuncs.front().JumpTables.push_back(Table);
  EXPECT_FALSE(validatePipelineCoverage(Result).has_value());

  Result.LowFuncs.front().JumpTables.front().MutatedUnsafe = true;
  Error = validatePipelineCoverage(Result);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("unresolved-indirect-branch"), std::string::npos);

  Result.LowFuncs.front().JumpTables.front().MutatedUnsafe = false;
  neverd::MedBlock MedBlock;
  MedBlock.Id = 0;
  neverd::MedOp IndirectCall;
  IndirectCall.Opcode = neverd::NdOp::INDIR_CALL;
  IndirectCall.Addr = 0x1018;
  MedBlock.Ops.push_back(IndirectCall);
  Result.MedFuncs.front().Blocks.push_back(std::move(MedBlock));
  neverd::MedCallInfo Call;
  Call.BlockId = 0;
  Call.OpIdx = 0;
  Call.IsIndirect = true;
  Result.MedFuncs.front().CallInfos.push_back(Call);
  neverd::LowOp IndirectCallLow;
  IndirectCallLow.Opcode = neverd::NdOp::INDIR_CALL;
  IndirectCallLow.Addr = 0x1018;
  Result.LowFuncs.front().Blocks.front().Ops.push_back(IndirectCallLow);

  Error = validatePipelineCoverage(Result);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("0x1018"), std::string::npos);
  EXPECT_NE(Error->find("unresolved-indirect-call"), std::string::npos);

  Result.MedFuncs.front().CallInfos.front().TargetAddr = 0x9000;
  EXPECT_FALSE(validatePipelineCoverage(Result).has_value());
}

TEST(SafetyReport, PipelineCoverageRejectsUnliftedResolvedIndirectCallee) {
  neverd::PipelineResult Result;
  neverd::PipelineFunctionAudit Accepted;
  Accepted.Entry = 0x1000;
  Accepted.Disposition = neverd::PipelineFunctionDisposition::Accepted;
  Accepted.HasLowIR = true;
  Accepted.HasMedIR = true;
  Accepted.MedIRVerified = true;
  Result.FunctionAudits.push_back(Accepted);

  neverd::LowFunc Low;
  Low.Entry = Accepted.Entry;
  neverd::LowBlock LowBlock;
  LowBlock.Id = 0;
  neverd::LowOp LowCall;
  LowCall.Opcode = neverd::NdOp::INDIR_CALL;
  LowCall.Addr = 0x1010;
  LowBlock.Ops.push_back(LowCall);
  Low.Blocks.push_back(std::move(LowBlock));
  Result.LowFuncs.push_back(std::move(Low));

  neverd::MedFunc Med;
  Med.Entry = Accepted.Entry;
  neverd::MedBlock Block;
  Block.Id = 0;
  neverd::MedOp CallOp;
  CallOp.Opcode = neverd::NdOp::INDIR_CALL;
  CallOp.Addr = 0x1010;
  Block.Ops.push_back(CallOp);
  Med.Blocks.push_back(std::move(Block));
  neverd::MedCallInfo Call;
  Call.BlockId = 0;
  Call.OpIdx = 0;
  Call.TargetAddr = 0x2000;
  Call.IsIndirect = true;
  Med.CallInfos.push_back(Call);
  Result.MedFuncs.push_back(std::move(Med));

  neverd::BinaryImage Img;
  neverd::Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x2000;
  Text.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  Img.Segments.push_back(std::move(Text));

  std::optional<std::string> Error = validatePipelineCoverage(Result, &Img);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("0x2000"), std::string::npos);
  EXPECT_NE(Error->find("unresolved-internal-call"), std::string::npos);

  neverd::PipelineFunctionAudit Callee = Accepted;
  Callee.Entry = 0x2000;
  Result.FunctionAudits.push_back(Callee);
  Result.LowFuncs.emplace_back();
  Result.LowFuncs.back().Entry = Callee.Entry;
  Result.MedFuncs.emplace_back();
  Result.MedFuncs.back().Entry = Callee.Entry;
  EXPECT_FALSE(validatePipelineCoverage(Result, &Img).has_value());

  Img.Arch = neverd::Arch::ARM;
  Img.Mode = neverd::InstructionMode::Thumb;
  Result.MedFuncs.front().CallInfos.front().TargetAddr = 0x2001;
  const std::optional<std::string> ThumbError =
      validatePipelineCoverage(Result, &Img);
  EXPECT_FALSE(ThumbError.has_value()) << ThumbError.value_or("");
}

TEST(SafetyReport, PipelineCoverageRejectsUnliftedResolvedDirectCallee) {
  neverd::PipelineResult Result;
  neverd::PipelineFunctionAudit Accepted;
  Accepted.Entry = 0x1000;
  Accepted.Disposition = neverd::PipelineFunctionDisposition::Accepted;
  Accepted.HasLowIR = true;
  Accepted.HasMedIR = true;
  Accepted.MedIRVerified = true;
  Result.FunctionAudits.push_back(Accepted);

  constexpr neverd::va_t kTarget = 0x2000;
  neverd::LowFunc Low;
  Low.Entry = Accepted.Entry;
  neverd::LowBlock LowBlock;
  LowBlock.Id = 0;
  neverd::LowOp LowCall;
  LowCall.Opcode = neverd::NdOp::CALL;
  LowCall.Addr = 0x1010;
  LowCall.addInput(neverd::NdVar::cst(kTarget, 8));
  LowBlock.Ops.push_back(LowCall);
  Low.Blocks.push_back(std::move(LowBlock));
  Result.LowFuncs.push_back(std::move(Low));

  neverd::MedFunc Med;
  Med.Entry = Accepted.Entry;
  neverd::MedBlock Block;
  Block.Id = 0;
  neverd::MedOp CallOp;
  CallOp.Opcode = neverd::NdOp::CALL;
  CallOp.Addr = 0x1010;
  CallOp.addInput(neverd::MedVar::makeConst(kTarget, 8));
  Block.Ops.push_back(CallOp);
  Med.Blocks.push_back(std::move(Block));
  neverd::MedCallInfo Call;
  Call.BlockId = 0;
  Call.OpIdx = 0;
  Call.TargetAddr = kTarget;
  Med.CallInfos.push_back(Call);
  Result.MedFuncs.push_back(std::move(Med));

  neverd::BinaryImage Img;
  neverd::Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x2000;
  Text.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  Img.Segments.push_back(std::move(Text));

  const std::optional<std::string> Error =
      validatePipelineCoverage(Result, &Img);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("0x2000"), std::string::npos);
  EXPECT_NE(Error->find("unresolved-internal-call"), std::string::npos);
}

TEST(SafetyReport, PipelineCoverageRequiresExactCallInventory) {
  neverd::PipelineResult Result;
  neverd::PipelineFunctionAudit Accepted;
  Accepted.Entry = 0x1000;
  Accepted.Disposition = neverd::PipelineFunctionDisposition::Accepted;
  Accepted.HasLowIR = true;
  Accepted.HasMedIR = true;
  Accepted.MedIRVerified = true;
  Result.FunctionAudits.push_back(Accepted);

  neverd::LowFunc Low;
  Low.Entry = Accepted.Entry;
  neverd::LowBlock LowBlock;
  LowBlock.Id = 0;
  neverd::LowOp LowCall;
  LowCall.Opcode = neverd::NdOp::CALL;
  LowCall.Addr = 0x1010;
  LowBlock.Ops.push_back(LowCall);
  Low.Blocks.push_back(std::move(LowBlock));
  Result.LowFuncs.push_back(std::move(Low));

  neverd::MedFunc Med;
  Med.Entry = Accepted.Entry;
  neverd::MedBlock MedBlock;
  MedBlock.Id = 0;
  neverd::MedOp MedCall;
  MedCall.Opcode = neverd::NdOp::CALL;
  MedCall.Addr = 0x1010;
  MedBlock.Ops.push_back(MedCall);
  Med.Blocks.push_back(std::move(MedBlock));
  Result.MedFuncs.push_back(std::move(Med));

  std::optional<std::string> Error = validatePipelineCoverage(Result);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("0x1010"), std::string::npos);
  EXPECT_NE(Error->find("incomplete-call-inventory"), std::string::npos);

  neverd::MedCallInfo Call;
  Call.BlockId = 0;
  Call.OpIdx = 0;
  Result.MedFuncs.front().CallInfos.push_back(Call);
  Error = validatePipelineCoverage(Result);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("incomplete-call-inventory"), std::string::npos);

  constexpr neverd::va_t kTarget = 0x2000;
  Result.LowFuncs.front().Blocks.front().Ops.front().addInput(
      neverd::NdVar::cst(kTarget, 8));
  Result.MedFuncs.front().Blocks.front().Ops.front().addInput(
      neverd::MedVar::makeConst(kTarget, 8));
  Result.MedFuncs.front().CallInfos.front().TargetAddr = kTarget;
  EXPECT_FALSE(validatePipelineCoverage(Result).has_value());

  Result.MedFuncs.front().CallInfos.front().TargetAddr = kTarget + 0x10;
  Error = validatePipelineCoverage(Result);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("incomplete-call-inventory"), std::string::npos);
  Result.MedFuncs.front().CallInfos.front().TargetAddr = kTarget;

  Result.LowFuncs.front().Blocks.front().Ops.front().Inputs[0] =
      neverd::NdVar::cst(kTarget + 0x20, 8);
  Error = validatePipelineCoverage(Result);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("incomplete-call-inventory"), std::string::npos);
  Result.LowFuncs.front().Blocks.front().Ops.front().Inputs[0] =
      neverd::NdVar::cst(kTarget, 8);

  Result.MedFuncs.front().CallInfos.front().OpIdx = 1;
  Error = validatePipelineCoverage(Result);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("incomplete-call-inventory"), std::string::npos);

  Result.MedFuncs.front().CallInfos.clear();
  Result.MedFuncs.front().Blocks.front().Ops.clear();
  Error = validatePipelineCoverage(Result);
  ASSERT_TRUE(Error.has_value());
  EXPECT_NE(Error->find("0x1010"), std::string::npos);
  EXPECT_NE(Error->find("incomplete-call-inventory"), std::string::npos);
}

TEST(SafetyReport, JsonCarriesSchemaAndAggregateVerdict) {
  SafetyReport Report;
  Report.Origin = Track::Hunt;
  Report.Format = "ELF";
  Report.Arch = "x86_64";

  Finding Unknown;
  Unknown.TheVerdict = Verdict::Unknown;
  Unknown.TheConfidence = Confidence::Low;
  Report.Findings.push_back(Unknown);

  Finding Unsafe;
  Unsafe.TheVerdict = Verdict::Unsafe;
  Unsafe.TheConfidence = Confidence::High;
  Report.Findings.push_back(Unsafe);

  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(toJson(Report));
  ASSERT_TRUE(static_cast<bool>(Parsed));
  const llvm::json::Object *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getInteger("schema_version"), 1);
  EXPECT_EQ(Root->getString("verdict"), "UNSAFE");
  EXPECT_EQ(Root->getString("confidence"), "HIGH");
}

TEST(SafetyReport, UnknownDominatesSafeAtTheRoot) {
  SafetyReport Report;
  Finding Safe;
  Safe.TheVerdict = Verdict::Safe;
  Safe.TheConfidence = Confidence::High;
  Report.Findings.push_back(Safe);
  Finding Unknown;
  Unknown.TheVerdict = Verdict::Unknown;
  Unknown.TheConfidence = Confidence::Low;
  Report.Findings.push_back(Unknown);

  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(toJson(Report));
  ASSERT_TRUE(static_cast<bool>(Parsed));
  const llvm::json::Object *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getString("verdict"), "UNKNOWN");
  EXPECT_EQ(Root->getString("confidence"), "LOW");
}

TEST(SafetyReport, PreservesUnsignedCapacityRange) {
  SafetyReport Report;
  Finding F;
  F.TheVerdict = Verdict::Unknown;
  F.Capacity = std::numeric_limits<uint64_t>::max();
  Report.Findings.push_back(std::move(F));

  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(toJson(Report));
  ASSERT_TRUE(static_cast<bool>(Parsed));
  const llvm::json::Object *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  const llvm::json::Array *Findings = Root->getArray("findings");
  ASSERT_NE(Findings, nullptr);
  ASSERT_EQ(Findings->size(), 1u);
  const llvm::json::Object *FindingObject = Findings->front().getAsObject();
  ASSERT_NE(FindingObject, nullptr);
  const llvm::json::Value *Capacity = FindingObject->get("capacity");
  ASSERT_NE(Capacity, nullptr);
  EXPECT_EQ(Capacity->getAsUINT64(), std::numeric_limits<uint64_t>::max());
}

TEST(SafetyReport, AuditScannedCountsAllocationSitesNotOnlyFindings) {
  neverd::BinaryImage Img;
  neverd::MedFunc F;
  F.Entry = 0x100;
  F.Name = "f";
  neverd::MedBlock B;
  B.Id = 0;

  auto addCall = [&](llvm::StringRef Name, int TempId, neverd::va_t Addr) {
    neverd::MedOp Op;
    Op.Opcode = neverd::NdOp::CALL;
    if (TempId >= 0) {
      Op.Output.Kind = neverd::MedVar::Temp;
      Op.Output.Id = TempId;
      Op.Output.Size = 8;
    }
    Op.Addr = Addr;
    Op.addInput(neverd::MedVar::makeConst(0x9000 + Addr, 8));
    B.Ops.push_back(Op);
    neverd::MedCallInfo CI;
    CI.BlockId = 0;
    CI.OpIdx = static_cast<int>(B.Ops.size() - 1);
    CI.TargetName = Name.str();
    CI.TargetAddr = 0x9000 + Addr;
    CI.Args = {neverd::MedVar::makeConst(16, 8)};
    F.CallInfos.push_back(std::move(CI));
  };
  addCall("malloc", 1, 0x400);
  addCall("malloc", 2, 0x408);
  neverd::MedOp Free;
  Free.Opcode = neverd::NdOp::CALL;
  Free.Addr = 0x410;
  Free.addInput(neverd::MedVar::makeConst(0xA000, 8));
  B.Ops.push_back(Free);
  neverd::MedCallInfo FreeCI;
  FreeCI.BlockId = 0;
  FreeCI.OpIdx = static_cast<int>(B.Ops.size() - 1);
  FreeCI.TargetName = "free";
  FreeCI.TargetAddr = 0xA000;
  neverd::MedVar Freed;
  Freed.Kind = neverd::MedVar::Temp;
  Freed.Id = 2;
  Freed.Size = 8;
  FreeCI.Args = {Freed};
  F.CallInfos.push_back(std::move(FreeCI));
  F.Blocks.push_back(std::move(B));

  std::vector<neverd::MedFunc> Funcs{std::move(F)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;
  SafetyReport Report = runAudit(In, SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_EQ(Report.Scanned, 2u);
  ASSERT_EQ(Report.Findings.size(), 1u);
  EXPECT_EQ(Report.Findings[0].Class, VulnClass::HeapLeak);
  EXPECT_EQ(Report.Findings[0].TheVerdict, Verdict::Unknown);
}

TEST(SafetyReport, ReachableAuditCandidateIsSymbolicallyCorroborated) {
  neverd::BinaryImage Img;
  neverd::MedFunc F;
  F.Entry = 0x400;
  F.Name = "leak";
  neverd::MedBlock MB;
  MB.Id = 0;
  neverd::MedOp Alloc;
  Alloc.Opcode = neverd::NdOp::CALL;
  Alloc.Output.Kind = neverd::MedVar::Temp;
  Alloc.Output.Id = 1;
  Alloc.Output.Size = 8;
  Alloc.Addr = 0x400;
  Alloc.addInput(neverd::MedVar::makeConst(0x9000, 8));
  MB.Ops.push_back(Alloc);
  neverd::MedCallInfo CI;
  CI.BlockId = 0;
  CI.OpIdx = 0;
  CI.TargetAddr = 0x9000;
  CI.TargetName = "malloc";
  CI.Args = {neverd::MedVar::makeConst(16, 8)};
  F.CallInfos.push_back(CI);
  neverd::MedOp Ret;
  Ret.Opcode = neverd::NdOp::RETURN;
  Ret.Addr = 0x408;
  MB.Ops.push_back(Ret);
  F.Blocks.push_back(std::move(MB));

  neverd::LowFunc LF;
  LF.Entry = F.Entry;
  LF.DecodedInstructionCount = 2;
  LF.LiftedInstructionCount = 2;
  neverd::LowBlock LB;
  LB.Id = 0;
  LB.StartAddr = 0x400;
  LB.EndAddr = 0x410;
  neverd::LowOp FrameStore;
  FrameStore.Opcode = neverd::NdOp::STORE;
  FrameStore.Addr = 0x400;
  FrameStore.addInput(neverd::NdVar::reg(0x20, 8));
  FrameStore.addInput(neverd::NdVar::reg(0x28, 8));
  LB.Ops.push_back(FrameStore);
  neverd::LowOp LCall;
  LCall.Opcode = neverd::NdOp::CALL;
  LCall.Addr = 0x400;
  LCall.addInput(neverd::NdVar::cst(0x9000, 8));
  LB.Ops.push_back(LCall);
  neverd::LowOp LRet;
  LRet.Opcode = neverd::NdOp::RETURN;
  LRet.Addr = 0x408;
  LB.Ops.push_back(LRet);
  LF.Blocks.push_back(std::move(LB));

  std::vector<neverd::MedFunc> Med{std::move(F)};
  std::vector<neverd::LowFunc> Low{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Med;
  In.LowFuncs = &Low;
  SafetyReport Report = runAudit(In, SinkCatalog::defaults(), SafetyBudgets{});
  ASSERT_EQ(Report.Findings.size(), 1u);
  EXPECT_EQ(Report.Findings[0].TheVerdict, Verdict::Unsafe)
      << Report.Findings[0].Corroboration << ": " << Report.Findings[0].Detail;
  EXPECT_FALSE(Report.Findings[0].Corroboration.empty());
}

TEST(SafetyReport, UnmodelledAuditPathFailsClosed) {
  neverd::BinaryImage Img;
  neverd::MedFunc F;
  F.Entry = 0x500;
  F.Name = "leak";
  neverd::MedBlock MB;
  MB.Id = 0;
  neverd::MedOp Alloc;
  Alloc.Opcode = neverd::NdOp::CALL;
  Alloc.Output.Kind = neverd::MedVar::Temp;
  Alloc.Output.Id = 1;
  Alloc.Output.Size = 8;
  Alloc.Addr = 0x504;
  Alloc.addInput(neverd::MedVar::makeConst(0x9000, 8));
  MB.Ops.push_back(Alloc);
  neverd::MedCallInfo CI;
  CI.BlockId = 0;
  CI.OpIdx = 0;
  CI.TargetAddr = 0x9000;
  CI.TargetName = "malloc";
  CI.Args = {neverd::MedVar::makeConst(16, 8)};
  F.CallInfos.push_back(CI);
  neverd::MedOp Ret;
  Ret.Opcode = neverd::NdOp::RETURN;
  Ret.Addr = 0x508;
  MB.Ops.push_back(Ret);
  F.Blocks.push_back(std::move(MB));

  neverd::LowFunc LF;
  LF.Entry = F.Entry;
  LF.DecodedInstructionCount = 3;
  LF.LiftedInstructionCount = 3;
  neverd::LowBlock LB;
  LB.Id = 0;
  LB.StartAddr = 0x500;
  LB.EndAddr = 0x510;
  neverd::LowOp Unknown;
  Unknown.Opcode = neverd::NdOp::INTRINSIC;
  Unknown.Addr = 0x500;
  LB.Ops.push_back(Unknown);
  neverd::LowOp LCall;
  LCall.Opcode = neverd::NdOp::CALL;
  LCall.Addr = 0x504;
  LCall.addInput(neverd::NdVar::cst(0x9000, 8));
  LB.Ops.push_back(LCall);
  neverd::LowOp LRet;
  LRet.Opcode = neverd::NdOp::RETURN;
  LRet.Addr = 0x508;
  LB.Ops.push_back(LRet);
  LF.Blocks.push_back(std::move(LB));

  std::vector<neverd::MedFunc> Med{std::move(F)};
  std::vector<neverd::LowFunc> Low{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Med;
  In.LowFuncs = &Low;
  SafetyReport Report = runAudit(In, SinkCatalog::defaults(), SafetyBudgets{});
  ASSERT_EQ(Report.Findings.size(), 1u);
  EXPECT_EQ(Report.Findings[0].TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Report.Findings[0].TheConfidence, Confidence::Low);
}

TEST(SafetyReport, NonNullLeakPathSurvivesANullComparison) {
  neverd::BinaryImage Img;
  Img.Arch = neverd::Arch::X64;
  neverd::MedFunc F;
  F.Entry = 0x400;
  F.Name = "conditional_leak";

  neverd::MedVar Handle;
  Handle.Kind = neverd::MedVar::Temp;
  Handle.Id = 1;
  Handle.Size = 8;
  neverd::MedVar Flag;
  Flag.Kind = neverd::MedVar::Temp;
  Flag.Id = 2;
  Flag.Size = 1;

  neverd::MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = 0x400;
  Entry.EndAddr = 0x410;
  Entry.Succs = {1, 2};
  neverd::MedOp Alloc;
  Alloc.Opcode = neverd::NdOp::CALL;
  Alloc.Output = Handle;
  Alloc.Addr = 0x400;
  Alloc.addInput(neverd::MedVar::makeConst(0x9000, 8));
  Entry.Ops.push_back(Alloc);
  neverd::MedCallInfo AllocCI;
  AllocCI.BlockId = 0;
  AllocCI.OpIdx = 0;
  AllocCI.TargetAddr = 0x9000;
  AllocCI.TargetName = "malloc";
  AllocCI.Args = {neverd::MedVar::makeConst(16, 8)};
  F.CallInfos.push_back(AllocCI);
  neverd::MedOp Compare;
  Compare.Opcode = neverd::NdOp::INT_NOTEQUAL;
  Compare.Output = Flag;
  Compare.addInput(Handle);
  Compare.addInput(neverd::MedVar::makeConst(0, 8));
  Entry.Ops.push_back(Compare);

  neverd::MedBlock Leak;
  Leak.Id = 1;
  Leak.StartAddr = 0x410;
  Leak.EndAddr = 0x420;
  Leak.Preds = {0};
  neverd::MedOp LeakRet;
  LeakRet.Opcode = neverd::NdOp::RETURN;
  Leak.Ops.push_back(LeakRet);

  neverd::MedBlock Release;
  Release.Id = 2;
  Release.StartAddr = 0x420;
  Release.EndAddr = 0x430;
  Release.Preds = {0};
  neverd::MedOp Free;
  Free.Opcode = neverd::NdOp::CALL;
  Free.Addr = 0x420;
  Free.addInput(neverd::MedVar::makeConst(0x9100, 8));
  Release.Ops.push_back(Free);
  neverd::MedCallInfo FreeCI;
  FreeCI.BlockId = 2;
  FreeCI.OpIdx = 0;
  FreeCI.TargetAddr = 0x9100;
  FreeCI.TargetName = "free";
  FreeCI.Args = {Handle};
  F.CallInfos.push_back(FreeCI);
  neverd::MedOp ReleaseRet;
  ReleaseRet.Opcode = neverd::NdOp::RETURN;
  Release.Ops.push_back(ReleaseRet);
  F.Blocks = {Entry, Leak, Release};

  neverd::LowFunc LF;
  LF.Entry = F.Entry;
  LF.DecodedInstructionCount = 5;
  LF.LiftedInstructionCount = 5;
  neverd::LowBlock LEntry;
  LEntry.Id = 0;
  LEntry.StartAddr = 0x400;
  LEntry.EndAddr = 0x410;
  LEntry.Succs = {1, 2};
  neverd::LowOp LAlloc;
  LAlloc.Opcode = neverd::NdOp::CALL;
  LAlloc.Addr = 0x400;
  LAlloc.addInput(neverd::NdVar::cst(0x9000, 8));
  LEntry.Ops.push_back(LAlloc);
  neverd::LowOp LCompare;
  LCompare.Opcode = neverd::NdOp::INT_NOTEQUAL;
  LCompare.Output = neverd::NdVar::tmp(8, 1);
  LCompare.addInput(neverd::NdVar::reg(0, 8));
  LCompare.addInput(neverd::NdVar::cst(0, 8));
  LEntry.Ops.push_back(LCompare);
  neverd::LowOp Branch;
  Branch.Opcode = neverd::NdOp::COND_BR;
  Branch.Addr = 0x408;
  Branch.addInput(neverd::NdVar::cst(0x410, 8));
  Branch.addInput(neverd::NdVar::tmp(8, 1));
  LEntry.Ops.push_back(Branch);

  neverd::LowBlock LLeak;
  LLeak.Id = 1;
  LLeak.StartAddr = 0x410;
  LLeak.EndAddr = 0x420;
  LLeak.Preds = {0};
  neverd::LowOp LLeakRet;
  LLeakRet.Opcode = neverd::NdOp::RETURN;
  LLeak.Ops.push_back(LLeakRet);

  neverd::LowBlock LRelease;
  LRelease.Id = 2;
  LRelease.StartAddr = 0x420;
  LRelease.EndAddr = 0x430;
  LRelease.Preds = {0};
  neverd::LowOp LFree;
  LFree.Opcode = neverd::NdOp::CALL;
  LFree.Addr = 0x420;
  LFree.addInput(neverd::NdVar::cst(0x9100, 8));
  LRelease.Ops.push_back(LFree);
  neverd::LowOp LReleaseRet;
  LReleaseRet.Opcode = neverd::NdOp::RETURN;
  LRelease.Ops.push_back(LReleaseRet);
  LF.Blocks = {LEntry, LLeak, LRelease};

  std::vector<neverd::MedFunc> Med{std::move(F)};
  std::vector<neverd::LowFunc> Low{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Med;
  In.LowFuncs = &Low;
  SafetyReport Report = runAudit(In, SinkCatalog::defaults(), SafetyBudgets{});
  ASSERT_EQ(Report.Findings.size(), 1u);
  EXPECT_EQ(Report.Findings[0].Class, VulnClass::HeapLeak);
  EXPECT_EQ(Report.Findings[0].TheVerdict, Verdict::Unsafe);
}

TEST(SafetyReport, NullOnlyDoubleFreeIsInfeasibleForAnAllocation) {
  neverd::BinaryImage Img;
  Img.Arch = neverd::Arch::X64;
  neverd::MedFunc F;
  F.Entry = 0x600;
  F.Name = "null_only_release";

  neverd::MedVar Handle;
  Handle.Kind = neverd::MedVar::Temp;
  Handle.Id = 1;
  Handle.Size = 8;
  neverd::MedVar Flag;
  Flag.Kind = neverd::MedVar::Temp;
  Flag.Id = 2;
  Flag.Size = 1;

  auto AddMedCall = [&](neverd::MedBlock &Block, llvm::StringRef Name,
                        neverd::va_t Target, neverd::va_t Address,
                        const neverd::MedVar &Output,
                        std::vector<neverd::MedVar> Args) {
    neverd::MedOp Op;
    Op.Opcode = neverd::NdOp::CALL;
    Op.Output = Output;
    Op.Addr = Address;
    Op.addInput(neverd::MedVar::makeConst(Target, 8));
    Block.Ops.push_back(Op);
    neverd::MedCallInfo CI;
    CI.BlockId = Block.Id;
    CI.OpIdx = static_cast<int>(Block.Ops.size() - 1);
    CI.TargetAddr = Target;
    CI.TargetName = Name.str();
    CI.Args = std::move(Args);
    F.CallInfos.push_back(std::move(CI));
  };

  neverd::MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = 0x600;
  Entry.EndAddr = 0x610;
  Entry.Succs = {1, 2};
  AddMedCall(Entry, "malloc", 0x9000, 0x600, Handle,
             {neverd::MedVar::makeConst(16, 8)});
  neverd::MedOp Compare;
  Compare.Opcode = neverd::NdOp::INT_EQUAL;
  Compare.Output = Flag;
  Compare.addInput(Handle);
  Compare.addInput(neverd::MedVar::makeConst(0, 8));
  Entry.Ops.push_back(Compare);

  neverd::MedBlock NullPath;
  NullPath.Id = 1;
  NullPath.StartAddr = 0x610;
  NullPath.EndAddr = 0x620;
  NullPath.Preds = {0};
  AddMedCall(NullPath, "free", 0x9100, 0x610, neverd::MedVar{}, {Handle});
  AddMedCall(NullPath, "free", 0x9100, 0x618, neverd::MedVar{}, {Handle});
  neverd::MedOp NullRet;
  NullRet.Opcode = neverd::NdOp::RETURN;
  NullPath.Ops.push_back(NullRet);

  neverd::MedBlock NonNullPath;
  NonNullPath.Id = 2;
  NonNullPath.StartAddr = 0x620;
  NonNullPath.EndAddr = 0x630;
  NonNullPath.Preds = {0};
  AddMedCall(NonNullPath, "free", 0x9100, 0x620, neverd::MedVar{}, {Handle});
  neverd::MedOp NonNullRet;
  NonNullRet.Opcode = neverd::NdOp::RETURN;
  NonNullPath.Ops.push_back(NonNullRet);
  F.Blocks = {Entry, NullPath, NonNullPath};

  auto LowCall = [](neverd::va_t Address, neverd::va_t Target) {
    neverd::LowOp Op;
    Op.Opcode = neverd::NdOp::CALL;
    Op.Addr = Address;
    Op.addInput(neverd::NdVar::cst(Target, 8));
    return Op;
  };
  auto LowReturn = [] {
    neverd::LowOp Op;
    Op.Opcode = neverd::NdOp::RETURN;
    return Op;
  };

  neverd::LowFunc LF;
  LF.Entry = F.Entry;
  LF.DecodedInstructionCount = 8;
  LF.LiftedInstructionCount = 8;
  neverd::LowBlock LEntry;
  LEntry.Id = 0;
  LEntry.StartAddr = 0x600;
  LEntry.EndAddr = 0x610;
  LEntry.Succs = {1, 2};
  LEntry.Ops.push_back(LowCall(0x600, 0x9000));
  neverd::LowOp LCompare;
  LCompare.Opcode = neverd::NdOp::INT_EQUAL;
  LCompare.Output = neverd::NdVar::tmp(8, 1);
  LCompare.addInput(neverd::NdVar::reg(0, 8));
  LCompare.addInput(neverd::NdVar::cst(0, 8));
  LEntry.Ops.push_back(LCompare);
  neverd::LowOp Branch;
  Branch.Opcode = neverd::NdOp::COND_BR;
  Branch.Addr = 0x608;
  Branch.addInput(neverd::NdVar::cst(0x610, 8));
  Branch.addInput(neverd::NdVar::tmp(8, 1));
  LEntry.Ops.push_back(Branch);

  neverd::LowBlock LNullPath;
  LNullPath.Id = 1;
  LNullPath.StartAddr = 0x610;
  LNullPath.EndAddr = 0x620;
  LNullPath.Preds = {0};
  LNullPath.Ops.push_back(LowCall(0x610, 0x9100));
  LNullPath.Ops.push_back(LowCall(0x618, 0x9100));
  LNullPath.Ops.push_back(LowReturn());

  neverd::LowBlock LNonNullPath;
  LNonNullPath.Id = 2;
  LNonNullPath.StartAddr = 0x620;
  LNonNullPath.EndAddr = 0x630;
  LNonNullPath.Preds = {0};
  LNonNullPath.Ops.push_back(LowCall(0x620, 0x9100));
  LNonNullPath.Ops.push_back(LowReturn());
  LF.Blocks = {LEntry, LNullPath, LNonNullPath};

  std::vector<neverd::MedFunc> Med{std::move(F)};
  std::vector<neverd::LowFunc> Low{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Med;
  In.LowFuncs = &Low;
  SafetyReport Report = runAudit(In, SinkCatalog::defaults(), SafetyBudgets{});
  EXPECT_TRUE(Report.Findings.empty());
}
