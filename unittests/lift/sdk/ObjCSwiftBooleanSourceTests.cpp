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
  BooleanFixture(bool Source = true, bool Patch = false, bool Twice = false,
                 bool Prefix = false, bool OpaquePrefix = false,
                 bool ObjectEquality = false, bool Suffix = false,
                 bool Native = false, bool Pair = false,
                 bool NativeCallee = false) {
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Image.Entry = 0x1000;
    const auto Provider = ObjectEquality ? SwiftBooleanObjectEqualityProvider
                                         : SwiftBooleanComparisonProvider;
    Image.DynInfo.NeededLibs = {Provider.str()};
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
    if (ObjectEquality) {
      // Preserve the incoming callee-saved x20 and provide a distinct metadata
      // value in the ABI's swiftself register before the comparison.
      Body[0] = 0xa9be7bfd;                    // STP FP,LR,[SP,#-32]!
      Body[2] = 0xf9000bf4;                    // STR X20,[SP,#16]
      Body[6] = 0xd2800134;                    // MOV X20,#9
      Body.insert(Body.end() - 2, 0xf9400bf4); // LDR X20,[SP,#16]
      Body[Body.size() - 2] = 0xa8c27bfd;      // LDP FP,LR,[SP],#32
    }
    if (Twice)
      Body.insert(Body.end() - 2, {0x94000037, 0x12000000});
    if (Pair)
      Body.insert(Body.end() - 2, 0xd2800021); // MOV X1,#1 before return.
    if (OpaquePrefix)
      Body[2] = 0x9400005e; // BL 0x1180 before any selected result exists.
    if (NativeCallee)
      Body = {0xa9be7bfd, 0x910003fd, 0xf9000bf3, 0xd28000a0,
              0xd2800001, 0xd28000e2, 0xd2800003, 0x52800004,
              0x94000038, // BL 0x1100: raw i1 Boolean.
              0xaa0003e8, // Keep its undefined high bits in x8.
              0x12000013, // AND W19,W0,#1: only bit 0 is observable.
              0x94000075, // BL 0x1200: no-argument native result.
              0xaa1303e0, 0xf9400bf3, 0xa8c27bfd, 0xd65f03c0};
    for (unsigned I = 0; I != std::size(Body); ++I)
      word(0x1000 + 4 * I, Body[I]);
    if (NativeCallee) {
      word(0x1200, 0xd2800000);
      word(0x1204, 0xd65f03c0);
      Image.Symbols.push_back({"native_noarg", 0x1200, 8, true});
    }
    word(0x1100, 0xb0000010);
    word(0x1104, 0xf9404210);
    word(0x1108, 0xd61f0200);
    const auto Import = ObjectEquality ? SwiftBooleanObjectEqualityImport
                        : Suffix       ? SwiftBooleanSuffixImport
                        : Prefix       ? SwiftBooleanPrefixImport
                                       : SwiftBooleanComparisonImport;
    Image.ImportPtrSlots[0x2080] = Import.str();
    EXPECT_TRUE(Image.recordDyldBindSlot(0x2080, Import, 0, Provider, false));
    ObjCMethod Method;
    Method.Implementation = 0x1000;
    Method.ClassName = "BooleanFixture";
    Method.Selector = "value";
    Method.TypeEncoding = "B16@0:8";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    Method.Status = "supported";
    if (!Native)
      Image.ObjCMethods.push_back(Method);
    Image.Symbols.push_back({"bool_method", 0x1000, Body.size() * 4, true});
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {0x1000};
    if (NativeCallee) {
      SourceFunctionTypeHint Callee;
      Callee.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Callee.ReturnType = NdType::makeInt(8, false);
      std::string Error;
      EXPECT_TRUE(assignDarwinScalarSourceABI(Callee, Image.Arch, Error))
          << Error;
      Options.OnlyFunctionEntries.insert(0x1200);
      Options.SourceTypeHints.emplace(0x1200, std::move(Callee));
    }
    if (Source) {
      auto Entry = Native ? *provisionalNativeSwiftBooleanEntry(Image, 0x1000)
                          : *objcMethodSourceTypeHint(Image, 0x1000);
      if (Pair) {
        Entry.ReturnType = NdType::makeStruct(
            {NdType::makeInt(8, false), NdType::makeInt(8, false)});
        std::string Error;
        EXPECT_TRUE(assignDarwinFixedSourceABI(Entry, Arch::AArch64, Error))
            << Error;
      }
      Options.SourceTypeHints.emplace(0x1000, std::move(Entry));
    } else {
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

TEST(ObjCSwiftBooleanSources,
     NativeBooleanProofUsesOnlyCurrentCompleteCalleeContracts) {
  BooleanFixture F;
  constexpr va_t Target = 0x1200;
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Signature.ReturnType = NdType::makeInt(8, false);
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error))
      << Error;

  MedFunc Caller;
  MedBlock Block;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(Target, 8));
  auto Binding = std::make_shared<SourceCallTypeHint>();
  Binding->CallKind = SourceCallTypeHint::Kind::Native;
  Binding->TargetAddress = Target;
  Binding->Signature = Signature;
  Call.SourceCallHint = Binding;
  Block.Ops.push_back(Call);
  Caller.Blocks.push_back(Block);

  HighFunc Callee;
  Callee.Entry = Target;
  Callee.SourceTypeHint = Signature;
  F.Result.HighFuncs.push_back(Callee);
  PipelineFunctionAudit Audit;
  Audit.Entry = Target;
  Audit.Disposition = PipelineFunctionDisposition::Accepted;
  Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  Audit.DecodedInstructions = Audit.LiftedInstructions = 2;
  F.Result.FunctionAudits.push_back(Audit);

  EXPECT_EQ(nativeBooleanPublicationCallees(F.Result, Caller).count(Target),
            1U);
  F.Result.FunctionAudits.back().MedIRVerified = false;
  EXPECT_TRUE(nativeBooleanPublicationCallees(F.Result, Caller).empty());
  F.Result.FunctionAudits.back().MedIRVerified = true;
  F.Result.HighFuncs.back().SourceTypeHint->ReturnType = NdType::makeVoid();
  EXPECT_TRUE(nativeBooleanPublicationCallees(F.Result, Caller).empty());
  F.Result.HighFuncs.back().SourceTypeHint = Signature;
  Caller.Blocks.front().Ops.push_back(Call);
  auto Conflicting = std::make_shared<SourceCallTypeHint>(*Binding);
  Conflicting->Signature.ReturnType = NdType::makeVoid();
  Caller.Blocks.front().Ops.back().SourceCallHint = Conflicting;
  EXPECT_TRUE(nativeBooleanPublicationCallees(F.Result, Caller).empty());
}

TEST(ObjCSwiftBooleanSources,
     PublicationRechecksNativeCallAcrossObservedBooleanPadding) {
  BooleanFixture F(true, false, false, false, false, false, false, false, false,
                   true);
  const auto E = F.expression();
  ASSERT_TRUE(E);
  const auto Bound = [&] {
    return objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high());
  };
  ASSERT_TRUE(Bound());
  auto Audit = std::find_if(F.Result.FunctionAudits.begin(),
                            F.Result.FunctionAudits.end(),
                            [](const PipelineFunctionAudit &Candidate) {
                              return Candidate.Entry == 0x1200;
                            });
  ASSERT_NE(Audit, F.Result.FunctionAudits.end());
  Audit->MedIRVerified = false;
  EXPECT_FALSE(Bound());
  Audit->MedIRVerified = true;
  auto Callee = std::find_if(
      F.Result.HighFuncs.begin(), F.Result.HighFuncs.end(),
      [](const HighFunc &Candidate) { return Candidate.Entry == 0x1200; });
  ASSERT_NE(Callee, F.Result.HighFuncs.end());
  ASSERT_TRUE(Callee->SourceTypeHint);
  Callee->SourceTypeHint->ReturnType = NdType::makeVoid();
  EXPECT_FALSE(Bound());
}

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

TEST(ObjCSwiftBooleanSources, NativePublicationReinfersCompleteEntryABI) {
  BooleanFixture F(true, false, false, false, false, false, false, true);
  const auto E = F.expression();
  ASSERT_TRUE(E);
  EXPECT_TRUE(objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  auto Forged = *F.high().SourceTypeHint;
  Forged.Parameters.push_back({"extra", NdType::makeInt(8)});
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Forged, Arch::AArch64, Error));
  F.high().SourceTypeHint = Forged;
  EXPECT_FALSE(
      objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
}

TEST(ObjCSwiftBooleanSources, NativePairReturnRechecksBothCarriers) {
  BooleanFixture F(true, false, false, false, false, false, false, true, true);
  const auto E = F.expression();
  ASSERT_TRUE(E);
  ASSERT_EQ(F.high().SourceTypeHint->ReturnComponents.size(), 2U);
  EXPECT_TRUE(objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  auto Forged = *F.high().SourceTypeHint;
  Forged.ReturnType = NdType::makeStruct(
      {NdType::makeInt(8, false), NdType::makePtr(NdType::makeVoid())});
  F.high().SourceTypeHint = std::move(Forged);
  EXPECT_FALSE(
      objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
}

TEST(ObjCSwiftBooleanSources,
     ObjectEqualityRechecksContextProviderAndConsumer) {
  BooleanFixture F(true, false, false, false, false, true);
  const auto E = F.expression();
  ASSERT_TRUE(E);
  ASSERT_EQ(E->SourceCallHint->Signature.Parameters.size(), 3U);
  EXPECT_EQ(
      E->SourceCallHint->Signature.Parameters.back().Location.RegisterOffset,
      160U);
  EXPECT_TRUE(objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  const auto Provider = F.Image.DyldBindSlots[0x2080].Module;
  F.Image.DyldBindSlots[0x2080].Module = SwiftBooleanComparisonProvider.str();
  EXPECT_FALSE(
      objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  F.Image.DyldBindSlots[0x2080].Module = Provider;
  // The publication proof consumes the current pipeline LowIR.
  for (auto &Low : F.Result.LowFuncs)
    for (auto &Block : Low.Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::INT_AND)
          for (auto &Input : Op.Inputs)
            if (Input.isConst() && Input.Offset == 1)
              Input.Offset = 3;
  EXPECT_FALSE(
      objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
}

TEST(ObjCSwiftBooleanSources, OpaquePrefixDoesNotAcquireSourceBinding) {
  BooleanFixture F(true, false, false, false, true);
  const auto E = F.expression();
  ASSERT_TRUE(E);
  EXPECT_TRUE(objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  unsigned Unbound = 0;
  walkStmts(F.high().Body, [&](HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty()) {
        const auto Current = Pending.back();
        Pending.pop_back();
        if (!Current)
          continue;
        if (Current->Kind == ExprKind::Call && Current->CallAddr == 0x1180) {
          ++Unbound;
          EXPECT_FALSE(Current->SourceCallHint);
          EXPECT_FALSE(objcSourceCallBound(*Current, F.Image, {}));
        }
        Pending.insert(Pending.end(), Current->Operands.begin(),
                       Current->Operands.end());
      }
    });
  });
  EXPECT_GT(Unbound, 0U);
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

TEST(ObjCSwiftBooleanSources,
     PrefixUsesFourInputsAndRejectsComparisonEvidence) {
  BooleanFixture F(true, false, true, true);
  auto E = F.expression();
  ASSERT_TRUE(E);
  ASSERT_EQ(E->Operands.size(), 4U);
  EXPECT_EQ(E->SourceCallHint->TargetName,
            SwiftBooleanPrefixImport.drop_front());
  ASSERT_TRUE(objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  auto Forged = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
  Forged->TargetName = SwiftBooleanComparisonImport.drop_front().str();
  Forged->Signature = *swiftBooleanNormalizedSignature();
  E->Operands.push_back(HighExpr::makeConst(0, 1));
  E->SourceCallHint = Forged;
  ASSERT_TRUE(isSwiftBooleanSourceBinding(*Forged));
  EXPECT_FALSE(
      objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
}

TEST(ObjCSwiftBooleanSources, SuffixRequiresCurrentOneBitConsumerProof) {
  BooleanFixture F(true, false, false, false, false, false, true);
  const auto E = F.expression();
  ASSERT_TRUE(E);
  ASSERT_EQ(E->Operands.size(), 4U);
  EXPECT_EQ(E->SourceCallHint->TargetName,
            SwiftBooleanSuffixImport.drop_front());
  EXPECT_TRUE(objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
  for (auto &Low : F.Result.LowFuncs)
    for (auto &Block : Low.Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::INT_AND)
          for (auto &Input : Op.Inputs)
            if (Input.isConst() && Input.Offset == 1)
              Input.Offset = 3;
  EXPECT_FALSE(
      objCSwiftBooleanSourceCallBound(*E, F.Image, F.Result, F.high()));
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
