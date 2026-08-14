//===- COFFExceptionIRTests.cpp - Windows EH IR carriage tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Metadata.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace {

using namespace neverd;

TEST(COFFExceptionIR, EmitsLosslessNamedMetadata) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "eh_metadata_test";
  MedBlock Block;
  Block.Id = 0;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Kind = RuntimeFunctionKind::Chained;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.UnwindInfoVA = 0x140003000;
  EH.PackedUnwindData = 0x12345678;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityName = "resolved!__C_specific_handler";
  EH.PersonalityVA = 0x140001100;
  EH.HandlerDataVA = 0x140003010;
  EH.NativeUnwindBytes = {0x09, 0x04, 0x00, 0x00};
  UnwindOperation UnwindOp;
  UnwindOp.Kind = UnwindOperationKind::AllocateLarge;
  UnwindOp.SlotCount = 2;
  EH.UnwindOperations.push_back(UnwindOp);
  EH.PrimaryFunctionIndex = 7;
  EH.ChainedPrimaryRange = ExceptionAddressRange{Func.Entry, Func.Entry + 0x20};
  EH.ChainedUnwindInfoRVA = 0x3010;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = EH.CodeRange;
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = 0x140001080;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Ctx;
  auto Module = MedLLVMEmitter().emit({Func}, Ctx, "eh-metadata", Arch::X64, {},
                                      nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  llvm::NamedMDNode *Table =
      Module->getNamedMetadata("neverd.windows.eh.functions");
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->getNumOperands(), 1u);
  llvm::Function *IRFunc = Module->getFunction("eh_metadata_test");
  ASSERT_NE(IRFunc, nullptr);
  llvm::MDNode *Payload = IRFunc->getMetadata("neverd.windows.eh");
  ASSERT_NE(Payload, nullptr);
  ASSERT_EQ(Payload->getNumOperands(), windows_eh_md::OperandCount);
  auto UIntAt = [](const llvm::MDNode &Node, unsigned Index) -> uint64_t {
    const auto *Constant =
        llvm::dyn_cast<llvm::ConstantAsMetadata>(Node.getOperand(Index).get());
    EXPECT_NE(Constant, nullptr);
    const auto *Integer =
        Constant ? llvm::dyn_cast<llvm::ConstantInt>(Constant->getValue())
                 : nullptr;
    EXPECT_NE(Integer, nullptr);
    return Integer ? Integer->getZExtValue() : 0;
  };
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::Version),
            windows_eh_md::SchemaVersion);
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::RuntimeKind),
            static_cast<uint8_t>(RuntimeFunctionKind::Chained));
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::PackedUnwindData), 0x12345678u);
  const auto *ResolvedName = llvm::dyn_cast<llvm::MDString>(
      Payload->getOperand(windows_eh_md::PersonalityName).get());
  ASSERT_NE(ResolvedName, nullptr);
  EXPECT_EQ(ResolvedName->getString(), "resolved!__C_specific_handler");
  const auto *Operations = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::UnwindOperations).get());
  ASSERT_NE(Operations, nullptr);
  ASSERT_EQ(Operations->getNumOperands(), 1u);
  const auto *FirstOperation =
      llvm::dyn_cast<llvm::MDNode>(Operations->getOperand(0).get());
  ASSERT_NE(FirstOperation, nullptr);
  EXPECT_EQ(UIntAt(*FirstOperation, 3), 2u);
  const auto *PrimaryIndex = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::PrimaryFunctionIndex).get());
  ASSERT_NE(PrimaryIndex, nullptr);
  ASSERT_EQ(PrimaryIndex->getNumOperands(), 1u);
  EXPECT_EQ(UIntAt(*PrimaryIndex, 0), 7u);
  const auto *PrimaryRange = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::ChainedPrimaryRange).get());
  ASSERT_NE(PrimaryRange, nullptr);
  ASSERT_EQ(PrimaryRange->getNumOperands(), 2u);
  EXPECT_EQ(UIntAt(*PrimaryRange, 0), Func.Entry);
  EXPECT_EQ(UIntAt(*PrimaryRange, 1), Func.Entry + 0x20);
  EXPECT_EQ(UIntAt(*Payload, windows_eh_md::ChainedUnwindInfoRVA), 0x3010u);

  // This fixture carries source metadata but deliberately contains no call in
  // its protected range.  Close that zero-call contract explicitly so the
  // optimization portion isolates metadata preservation rather than native-EH
  // call-site lowering, which has dedicated tests below.
  exception_rewrite::setContract(
      *IRFunc, exception_rewrite::SourceState::Complete,
      exception_rewrite::LoweringState::Complete,
      /*RequiredCalls=*/0, /*LoweredCalls=*/0, /*SkippedPads=*/0);
  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O2;
  OptimizationResult Result = Pipeline::optimizeModule(*Module, Options);
  EXPECT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));

  Table = Module->getNamedMetadata("neverd.windows.eh.functions");
  ASSERT_NE(Table, nullptr);
  EXPECT_EQ(Table->getNumOperands(), 1u);
  IRFunc = Module->getFunction("eh_metadata_test");
  ASSERT_NE(IRFunc, nullptr);
  Payload = IRFunc->getMetadata("neverd.windows.eh");
  ASSERT_NE(Payload, nullptr);
  EXPECT_EQ(Payload->getNumOperands(), windows_eh_md::OperandCount);
  EXPECT_NE(IRFunc->getMetadata(exception_rewrite::FunctionAttachment),
            nullptr);
}
TEST(COFFExceptionIR, StructuresReducibleSEHAndCxxRegionsInHighIR) {
  auto MakeFunction = [] {
    MedFunc Func;
    Func.Entry = 0x140001000;
    Func.Name = "structured_eh";
    Func.ReturnType = NdType::makeVoid();

    MedBlock Protected;
    Protected.Id = 0;
    Protected.StartAddr = Func.Entry;
    Protected.EndAddr = Func.Entry + 0x10;
    MedOp ProtectedReturn;
    ProtectedReturn.Opcode = NdOp::RETURN;
    ProtectedReturn.Addr = Func.Entry + 8;
    Protected.Ops.push_back(ProtectedReturn);

    MedBlock Handler;
    Handler.Id = 1;
    Handler.StartAddr = Func.Entry + 0x20;
    Handler.EndAddr = Func.Entry + 0x30;
    MedOp HandlerReturn;
    HandlerReturn.Opcode = NdOp::RETURN;
    HandlerReturn.Addr = Func.Entry + 0x28;
    Handler.Ops.push_back(HandlerReturn);

    Func.Blocks.push_back(std::move(Protected));
    Func.Blocks.push_back(std::move(Handler));
    return Func;
  };

  MedFunc SEHFunc = MakeFunction();
  ExceptionFunction SEHMetadata;
  SEHMetadata.CodeRange = {SEHFunc.Entry, SEHFunc.Entry + 0x30};
  SEHMetadata.ParseStatus = ExceptionParseStatus::Complete;
  SEHMetadata.Personality = ExceptionPersonality::CSpecificHandler;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {SEHFunc.Entry, SEHFunc.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = SEHFunc.Entry + 0x20;
  SEH.Scopes.push_back(Scope);
  SEHMetadata.SEH = std::move(SEH);
  SEHFunc.ExceptionMetadata = std::move(SEHMetadata);

  HighFunc HighSEH = MedToHighConverter().convert(SEHFunc, Arch::X64);
  ASSERT_EQ(HighSEH.StructuredExceptionRegions, 1u);
  ASSERT_EQ(HighSEH.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(HighSEH.Body.empty());
  EXPECT_EQ(HighSEH.Body.front().Kind, StmtKind::SEHTry);
  ASSERT_EQ(HighSEH.Body.front().EHClauses.size(), 1u);
  EXPECT_EQ(HighSEH.Body.front().EHClauses.front().Kind,
            HighEHClauseKind::SEHExcept);

  MedFunc CxxFunc = MakeFunction();
  CxxFunc.Name = "structured_cxx";
  ExceptionFunction CxxMetadata;
  CxxMetadata.CodeRange = {CxxFunc.Entry, CxxFunc.Entry + 0x30};
  CxxMetadata.ParseStatus = ExceptionParseStatus::Complete;
  CxxMetadata.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 2;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}};
  Cxx.UnwindMap[0].ActionVA = CxxFunc.Entry + 0x100;
  Cxx.UnwindMap[0].Kind =
      CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
  Cxx.UnwindMap[0].ObjectOffset = -0x20;
  Cxx.IPMap = {{CxxFunc.Entry, 0},
               {CxxFunc.Entry + 0x10, -1},
               {CxxFunc.Entry + 0x20, 1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.HandlerVA = CxxFunc.Entry + 0x20;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  CxxMetadata.Cxx = std::move(Cxx);
  CxxFunc.ExceptionMetadata = std::move(CxxMetadata);

  HighFunc HighCxx = MedToHighConverter().convert(CxxFunc, Arch::X64);
  ASSERT_EQ(HighCxx.StructuredExceptionRegions, 1u);
  ASSERT_EQ(HighCxx.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(HighCxx.Body.empty());
  EXPECT_EQ(HighCxx.Body.front().Kind, StmtKind::CxxTry);
  ASSERT_EQ(HighCxx.Body.front().EHClauses.size(), 2u);
  EXPECT_EQ(HighCxx.Body.front().EHClauses.front().Kind,
            HighEHClauseKind::CxxCatch);
  const HighEHClause &Cleanup = HighCxx.Body.front().EHClauses.back();
  EXPECT_EQ(Cleanup.Kind, HighEHClauseKind::CxxCleanup);
  EXPECT_EQ(Cleanup.UnwindActionKind,
            CxxUnwindAction::ActionKind::DestructorWithObjectPointer);
  EXPECT_EQ(Cleanup.UnwindObjectOffset, -0x20);
}
TEST(COFFExceptionIR, SplitsProtectedRangesAndKeepsEdgesSeparate) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Segment Text;
  Text.VA = Img.Base + 0x1000;
  Text.Size = 4;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0x90, 0x90, 0x90, 0xc3}; // nop; nop; nop; ret
  Img.Segments.push_back(std::move(Text));

  ExceptionFunction EH;
  EH.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1004};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Img.Base + 0x1001, Img.Base + 0x1003};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Img.Base + 0x1003;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  LowFunc Func = Builder.build(Img, Dec, Img.Base + 0x1000, "seh_cfg");
  ASSERT_TRUE(Func.ExceptionMetadata.has_value());
  ASSERT_EQ(Func.Blocks.size(), 3u);
  EXPECT_EQ(Func.Blocks[0].StartAddr, Img.Base + 0x1000);
  EXPECT_EQ(Func.Blocks[1].StartAddr, Img.Base + 0x1001);
  EXPECT_EQ(Func.Blocks[2].StartAddr, Img.Base + 0x1003);
  ASSERT_EQ(Func.Blocks[1].ExceptionalSuccs.size(), 1u);
  EXPECT_EQ(Func.Blocks[1].ExceptionalSuccs[0].Kind,
            ExceptionalEdgeKind::SEHHandler);
  EXPECT_EQ(Func.Blocks[1].ExceptionalSuccs[0].BlockId, 2);
  EXPECT_EQ(Func.Blocks[1].Succs.size(), 1u);
  EXPECT_EQ(Func.Blocks[1].Succs[0], 2);
}

TEST(COFFExceptionIR, DecompileRetainsFaithfulCxxAnnotation) {
  HighFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "cxx_annotation_test";
  Func.ReturnType = NdType::makeInt(8);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(0, 8);
  Func.Body.push_back(std::move(Return));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 2;
  Cxx.UnwindMap.push_back({-1, Func.Entry + 0x30});
  Cxx.UnwindMap.push_back({0, Func.Entry + 0x34});
  Cxx.UnwindMap.back().Kind =
      CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
  Cxx.UnwindMap.back().ObjectOffset = -16;
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.Adjectives = 0x40;
  Catch.TypeDescriptorVA = 0x140003000;
  Catch.CatchObjectOffset = -32;
  Catch.HandlerVA = Func.Entry + 0x20;
  Catch.ParentFrameOffset = -8;
  Catch.ContinuationVAs.push_back(Func.Entry + 0x38);
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = std::move(EH);

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS));
  OS.flush();
  EXPECT_NE(Source.find("neverd.exception: encoding=x64-unwind-v1"),
            std::string::npos);
  EXPECT_NE(Source.find("personality=__CxxFrameHandler3"), std::string::npos);
  EXPECT_NE(Source.find("cxx.try[0]"), std::string::npos);
  EXPECT_NE(Source.find("handler=0x140001020"), std::string::npos);
  EXPECT_NE(Source.find("kind=destructor-object-pointer"), std::string::npos);
  EXPECT_NE(Source.find("object_offset=-16"), std::string::npos);
  EXPECT_NE(Source.find("adjectives=0x40"), std::string::npos);
  EXPECT_NE(Source.find("parent_frame_offset=-8"), std::string::npos);
  EXPECT_NE(Source.find("continuations=0x140001038"), std::string::npos);
}

} // namespace
