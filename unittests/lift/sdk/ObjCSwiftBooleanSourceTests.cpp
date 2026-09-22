#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "../../../lib/sdk/capi/ObjCSwiftBooleanSources.h"
#include "gtest/gtest.h"

#include "neverd/loader/ObjC/ObjCEncoding.h"

#include "llvm/Support/Endian.h"

using namespace neverd;
using namespace neverd::sdk;
namespace {
struct BooleanFixture {
  BinaryImage Image;
  llvm::LLVMContext Context;
  PipelineResult Result;
  BooleanFixture(bool Source = true, bool Patch = false, bool Twice = false) {
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Image.Entry = 0x1000;
    Image.DynInfo.NeededLibs = {SwiftBooleanComparisonProvider.str()};
    for (unsigned I = 0; I != 2; ++I) {
      Segment Segment;
      Segment.VA = 0x1000 + I * 0x1000;
      Segment.FileOff = I * 0x1000;
      Segment.Size = Segment.FileSz = 0x1000;
      Segment.Data.resize(0x1000);
      Segment.Flags = SegmentFlags::Readable;
      if (!I)
        Segment.Flags = Segment.Flags | SegmentFlags::Executable;
      Image.Segments.push_back(Segment);
      Section Section;
      Section.VA = Segment.VA;
      Section.FileOff = Segment.FileOff;
      Section.Size = Section.FileSz = Segment.Size;
      Section.Flags = Segment.Flags;
      Section.Type = I ? 0 : llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      Image.Sections.push_back(Section);
    }
    std::vector<uint32_t> Body = {0xa9bf7bfd, 0x910003fd, // save FP/LR
                                  0xd28000a0, 0xd2800001, 0xd28000e2,
                                  0xd2800003, 0x52800024,
                                  0x94000039, // BL 0x1100 from 0x101c
                                  0x12000000, // AND W0,W0,#1
                                  0xa8c17bfd, 0xd65f03c0};
    if (Twice)
      Body.insert(Body.end() - 2, {0x94000037, 0x12000000});
    for (unsigned I = 0; I != std::size(Body); ++I)
      word(0x1000 + 4 * I, Body[I]);
    word(0x1100, 0xb0000010);
    word(0x1104, 0xf9404210);
    word(0x1108, 0xd61f0200);
    Image.ImportPtrSlots[0x2080] = SwiftBooleanComparisonImport.str();
    EXPECT_TRUE(Image.recordDyldBindSlot(0x2080, SwiftBooleanComparisonImport,
                                         0, SwiftBooleanComparisonProvider,
                                         false));
    ObjCMethod Method;
    Method.Implementation = 0x1000;
    Method.ClassName = "BooleanFixture";
    Method.Selector = "value";
    Method.TypeEncoding = "B16@0:8";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    Method.Status = "supported";
    Image.ObjCMethods.push_back(Method);
    Image.Symbols.push_back({"bool_method", 0x1000, Body.size() * 4, true});
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {0x1000};
    if (Source)
      Options.SourceTypeHints.emplace(0x1000,
                                      *objcMethodSourceTypeHint(Image, 0x1000));
    else {
      Options.LiftMode = !Patch;
      Options.PatchMode = Patch;
    }
    Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
  }
  void word(va_t Address, uint32_t Value) {
    uint8_t Bytes[4];
    llvm::support::endian::write32le(Bytes, Value);
    ASSERT_TRUE(Image.writeVA(Address, Bytes, 4));
  }
  HighFunc &high() {
    for (auto &Function : Result.HighFuncs)
      if (Function.Entry == 0x1000)
        return Function;
    throw std::runtime_error("missing Boolean high function");
  }
  ExprPtr expression() {
    ExprPtr Found;
    walkStmts(high().Body, [&](HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &Root) {
        std::vector<ExprPtr> Pending{Root};
        while (!Pending.empty()) {
          const auto E = Pending.back();
          Pending.pop_back();
          if (!E)
            continue;
          if (E->SourceCallHint && E->SourceCallHint->BooleanResult)
            Found = E;
          Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
        }
      });
    });
    return Found;
  }
};
} // namespace

TEST(ObjCSwiftBooleanSources, RealLoweringAndPublicationRepeatCallerProof) {
  BooleanFixture F;
  const auto E = F.expression();
  ASSERT_TRUE(E);
  EXPECT_TRUE(objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  EXPECT_FALSE(objcSourceCallBound(*E, F.Image, {}));
  EXPECT_EQ(E->SourceCallHint->BooleanResult->Site.Instruction, 0x101cU);
  EXPECT_EQ(E->CallAddr, 0x1100U);
  EXPECT_EQ(E->Type->Size, 1U);
  unsigned Normalizations = 0;
  for (const auto &Low : F.Result.LowFuncs)
    for (const auto &B : Low.Blocks)
      for (const auto &Op : B.Ops)
        Normalizations += Op.Opcode == NdOp::INT_AND;
  EXPECT_EQ(Normalizations, 1U);
}

TEST(ObjCSwiftBooleanSources,
     DistinctOccurrencesRequireDistinctCurrentEvidence) {
  BooleanFixture F(true, false, true);
  std::vector<ExprPtr> Calls;
  walkStmts(F.high().Body, [&](HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &Root) {
      if (Root && Root->SourceCallHint && Root->SourceCallHint->BooleanResult)
        Calls.push_back(Root);
    });
  });
  ASSERT_EQ(Calls.size(), 2U);
  for (const auto &E : Calls)
    EXPECT_TRUE(
        objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  Calls[1]->CallAddr += 4;
  for (const auto &E : Calls)
    EXPECT_FALSE(
        objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  Calls[1]->CallAddr -= 4;
  auto Duplicate =
      std::make_shared<SourceCallTypeHint>(*Calls[1]->SourceCallHint);
  Duplicate->BooleanResult = Calls[0]->SourceCallHint->BooleanResult;
  Calls[1]->SourceCallHint = Duplicate;
  for (const auto &E : Calls)
    EXPECT_FALSE(
        objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
}

TEST(ObjCSwiftBooleanSources, RejectsStaleCurrentProofAndDuplicateEvaluations) {
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BooleanFixture F;
    auto E = F.expression();
    ASSERT_TRUE(E);
    auto Binding = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
    E->SourceCallHint = Binding;
    switch (Mutation) {
    case 0:
      Binding->BooleanResult->FunctionEntry += 4;
      break;
    case 1:
      Binding->BooleanResult->Site.Sequence += 1;
      break;
    case 2:
      Binding->BooleanResult->Site.Instruction += 4;
      break;
    case 3:
      Binding->TargetAddress += 8;
      break;
    case 4:
      F.Result.SourceImage = nullptr;
      break;
    case 5:
      F.Result.FunctionAudits[0].MedIRVerified = false;
      break;
    case 6:
      F.Result.FunctionAudits.push_back(F.Result.FunctionAudits[0]);
      break;
    case 7:
      F.Result.LowFuncs.push_back(F.Result.LowFuncs[0]);
      break;
    case 8:
      F.Image.DyldBindSlots[0x2080].Module = "/tmp/libswiftCore.dylib";
      break;
    case 9:
      F.word(0x101c, 0x94000038);
      break;
    case 10:
      for (auto &Low : F.Result.LowFuncs)
        for (auto &B : Low.Blocks)
          for (auto &Op : B.Ops)
            if (Op.Opcode == NdOp::INT_AND)
              for (auto &Input : Op.Inputs)
                if (Input.isConst() && Input.Offset == 1)
                  Input.Offset = 3;
      break;
    case 11:
      E->CallAddr += 4;
      break;
    case 12:
      E->Operands.pop_back();
      break;
    case 13: {
      HighStmt Duplicate;
      Duplicate.Kind = StmtKind::Call;
      Duplicate.CallExpr = E;
      F.high().Body.push_back(Duplicate);
      break;
    }
    case 14:
      Binding->ReturnedArgument = 0;
      break;
    case 15:
      E->Operands[0].reset();
      break;
    case 16:
      F.Result.Success = false;
      break;
    case 17: {
      HighStmt Duplicate;
      Duplicate.Kind = StmtKind::Assign;
      Duplicate.Dst = HighExpr::makeLoad(E, NdType::makeInt(1, false));
      Duplicate.Val = HighExpr::makeConst(0, 1);
      F.high().Body.push_back(Duplicate);
      break;
    }
    }
    EXPECT_FALSE(
        objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  }
}

TEST(ObjCSwiftBooleanSources, MachineModesDoNotNormalizeTheRuntimeResult) {
  for (bool Patch : {false, true}) {
    BooleanFixture F(false, Patch);
    for (const auto &Function : F.Result.MedFuncs)
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          EXPECT_FALSE(Op.SourceCallHint && Op.SourceCallHint->BooleanResult);
  }
}
