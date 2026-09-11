#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"

#include "llvm/Support/Endian.h"

using namespace neverd;

namespace {
SourceFunctionTypeHint signature(Arch Architecture, unsigned IntegerCount = 1) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(4);
  Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                     {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
  for (unsigned I = 0; I < IntegerCount; ++I)
    Hint.Parameters.push_back({"arg" + std::to_string(I), NdType::makeInt(4)});
  std::string Diagnostic;
  EXPECT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Diagnostic))
      << Diagnostic;
  return Hint;
}

BinaryImage image(Arch Architecture = Arch::AArch64) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Segment;
  Segment.VA = 0x1000;
  Segment.Size = Segment.FileSz = 0x2000;
  Segment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Segment.Data.resize(0x2000);
  Image.Segments.push_back(Segment);
  Section Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.push_back(Text);
  Section Data;
  Data.VA = 0x2000;
  Data.FileOff = 0x1000;
  Data.Size = Data.FileSz = 0x1000;
  Data.Flags = SegmentFlags::Readable;
  Image.Sections.push_back(Data);
  Image.ImportPtrSlots[0x2180] = "_objc_msgSend";
  ObjCSourceReference Reference;
  Reference.Address = 0x2100;
  Reference.Name = "scale:";
  Image.ObjCSourceReferences[0x2100] = Reference;
  ObjCMethod Method;
  Method.ClassName = "First";
  Method.Selector = Reference.Name;
  Method.Implementation = 0x1400;
  Method.TypeHint = signature(Architecture);
  Image.ObjCMethods.push_back(Method);
  // ADRP x1,0x2000; LDR x1,[x1,#0x100]; ADRP x16,0x2000;
  // LDR x16,[x16,#0x180]; BR x16. No symbol table is supplied.
  const uint32_t Stub[] = {0xb0000001, 0xf9408021, 0xb0000010, 0xf940c210,
                           0xd61f0200};
  for (size_t I = 0; I < 5; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x100 + I * 4, Stub[I]);
  return Image;
}

LowOp operation(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
                va_t Address = 0x1200) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Address;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

LowFunc caller(Arch Architecture = Arch::AArch64) {
  LowFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "caller";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x1200;
  Block.EndAddr = 0x1208;
  const auto &TRI = getTargetRegInfo(Architecture);
  Block.Ops.push_back(operation(NdOp::CALL, NdVar::reg(TRI.IntReturnReg, 8),
                                {NdVar::cst(0x1100, 8)}));
  Block.Ops.push_back(
      operation(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 4)}, 0x1204));
  Function.Blocks.push_back(Block);
  return Function;
}

MedFunc convert(const BinaryImage &Image, const LowFunc &Low,
                bool Enable = true) {
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  Converter.setSourceCallHintsEnabled(Enable);
  auto Result = Converter.convert(Low, Image.Arch, BinaryFormat::MachO);
  recoverCallAbi(Result, Image.Arch, {}, &Image);
  return Result;
}

const HighExpr *sourceCall(const HighFunc &High) {
  std::vector<const HighExpr *> Work;
  walkStmts(High.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Expression) {
      Work.push_back(Expression.get());
    });
  });
  const HighExpr *SourceCall = nullptr;
  while (!Work.empty()) {
    const auto *Expression = Work.back();
    Work.pop_back();
    if (Expression->SourceCallHint) {
      SourceCall = Expression;
      break;
    }
    for (const auto &Operand : Expression->Operands)
      if (Operand)
        Work.push_back(Operand.get());
  }
  return SourceCall;
}

TEST(ObjCCallHints, RecognizesStrippedSelectorStubFromInstructionsAndSlots) {
  auto Image = image();
  auto Hints = buildObjCSourceCallHints(Image, caller());
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x1200);
  EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::ObjCMessage);
  EXPECT_EQ(Hint.TargetName, "objc_msgSend");
  EXPECT_EQ(Hint.Selector, "scale:");
  EXPECT_EQ(Hint.SelectorReferenceAddress, 0x2100U);
  ASSERT_EQ(Hint.Signature.Parameters.size(), 3U);
  EXPECT_EQ(Hint.Signature.Parameters[2].Location.RegisterOffset, 16U);
}

TEST(ObjCCallHints, RejectsAmbiguousAndUnsupportedSelectorSignatures) {
  auto Image = image();
  auto Other = Image.ObjCMethods[0];
  Other.ClassName = "Other";
  Other.TypeHint->ReturnType = NdType::makeInt(8);
  Image.ObjCMethods.push_back(Other);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.ObjCMethods.back().TypeHint.reset();
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.ObjCMethods.back().TypeHint = Image.ObjCMethods[0].TypeHint;
  EXPECT_EQ(buildObjCSourceCallHints(Image, caller()).size(), 1U);
}

TEST(ObjCCallHints, RejectsSymbolOnlyWrongBranchAndWrongImport) {
  auto Image = image();
  Symbol S;
  S.Name = "_objc_msgSend$scale:";
  S.Addr = 0x1100;
  S.IsFunc = true;
  Image.Symbols.push_back(S);
  Image.ImportPtrSlots[0x2180] = "_unrelated_runtime";
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.ImportPtrSlots[0x2180] = "_objc_msgSend";
  llvm::support::endian::write32le(Image.Segments[0].Data.data() + 0x110,
                                   0xd61f0220); // BR x17, not loaded x16
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
}

TEST(ObjCCallHints, PreservesReceiverArgumentAndReturnBeforeSSA) {
  auto Image = image();
  auto Low = caller();
  auto &Ops = Low.Blocks[0].Ops;
  Ops.insert(Ops.begin(), operation(NdOp::COPY, NdVar::reg(16, 4),
                                    {NdVar::cst(0x80000001, 4)}, 0x11fc));
  const auto Med = convert(Image, Low);
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  const auto &Call = Med.CallInfos[0];
  ASSERT_TRUE(Call.SourceCallHint);
  ASSERT_EQ(Call.Args.size(), 3U);
  EXPECT_EQ(Call.Args[0].Kind, MedVar::Reg);
  EXPECT_EQ(Call.Args[0].RegOff, 0U);
  EXPECT_EQ(Call.Args[2].Kind, MedVar::Reg);
  EXPECT_EQ(Call.Args[2].RegOff, 16U);
  EXPECT_EQ(Call.Args[2].Size, 4U);
  const auto &Op = Med.Blocks[Call.BlockId].Ops[Call.OpIdx];
  EXPECT_EQ(Op.Output.RegOff, 0U);
  EXPECT_EQ(Op.Output.Size, 4U);
  EXPECT_EQ(Med.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
  const auto High = MedToHighConverter().convert(Med, Image.Arch);
  const auto *SourceCall = sourceCall(High);
  ASSERT_NE(SourceCall, nullptr);
  ASSERT_EQ(SourceCall->Operands.size(), 3U);
  EXPECT_EQ(SourceCall->Operands[0]->Kind, ExprKind::Var);
  EXPECT_EQ(SourceCall->Operands[0]->Var.Kind, MedVar::Param);
  EXPECT_EQ(SourceCall->Operands[0]->Var.RegOff, 0U);
  EXPECT_EQ(SourceCall->Operands[2]->Kind, ExprKind::Const);
  EXPECT_EQ(SourceCall->Operands[2]->ConstVal, 0x80000001U);
  const auto Disabled = convert(Image, Low, false);
  ASSERT_EQ(Disabled.CallInfos.size(), 1U);
  EXPECT_FALSE(Disabled.CallInfos[0].SourceCallHint);
}

BinaryImage selectorStubImage() {
  auto Image = image();
  Image.Sections[0].Name = "__objc_stubs";
  return Image;
}

MedFunc callerWithStaleCommand(bool Clobbered) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  auto Register = [&](int Id, int Version, unsigned Index) {
    MedVar Value;
    Value.Kind = MedVar::Reg;
    Value.Id = Id;
    Value.SSAVer = Version;
    Value.Size = 8;
    Value.RegOff = TRI.IntParamRegs[Index];
    Value.TheArch = Arch::AArch64;
    return Value;
  };
  MedFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "stale_selector_caller";
  Function.Blocks.resize(1);
  auto &Block = Function.Blocks[0];
  Block.Id = 0;
  auto Append = [&](NdOp Opcode, MedVar Output, MedVar Input,
                    unsigned CallSite = 0) {
    MedOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = 0x1200 + Block.Ops.size() * 4;
    Op.CallSiteId = CallSite;
    Op.addInput(Input);
    Block.Ops.push_back(Op);
  };
  MedVar Command = MedVar::makeConst(0x777777, 8);
  if (Clobbered) {
    Append(NdOp::CALL, Register(10, 1, 0), MedVar::makeConst(0x1500, 8), 1);
    Command = Register(20, 1, 1);
    Function.CallClobbers.push_back({Command, 1});
  }
  Append(NdOp::COPY, Register(11, 2, 0), MedVar::makeConst(0x2222, 8));
  Append(NdOp::COPY, Register(21, 2, 1), Command);
  Append(NdOp::COPY, Register(30, 1, 2), MedVar::makeConst(0x123456, 8));
  Append(NdOp::CALL, Register(12, 3, 0), MedVar::makeConst(0x1100, 8), 2);
  Append(NdOp::RETURN, {}, Register(12, 3, 0));
  return Function;
}

TEST(ObjCCallHints, SelectorStubCommandProofDoesNotRequireMethodSignature) {
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Image = selectorStubImage();
    if (Mode == 0)
      Image.ObjCMethods.clear();
    else if (Mode == 1)
      Image.ObjCMethods[0].TypeHint.reset();
    else {
      auto Conflicting = Image.ObjCMethods[0];
      Conflicting.TypeHint->ReturnType = NdType::makeInt(8);
      Image.ObjCMethods.push_back(std::move(Conflicting));
    }
    EXPECT_TRUE(objcSelectorStubOverwritesCommand(Image, 0x1100));
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  }
}

TEST(ObjCCallHints, SelectorStubCommandProofRejectsUnverifiedCodeAndSlots) {
  for (unsigned Mutation = 0; Mutation < 16; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = selectorStubImage();
    switch (Mutation) {
    case 0:
      Image.Format = BinaryFormat::ELF;
      break;
    case 1:
      Image.IsRelocatable = true;
      break;
    case 2:
      Image.Arch = Arch::X64;
      break;
    case 3:
      Image.Bits = Bitness::Bits32;
      break;
    case 4:
      Image.Sections[0].Name = "__text";
      break;
    case 5:
      Image.Sections[0].Flags = SegmentFlags::Readable;
      break;
    case 6:
      Image.Segments[0].Flags = SegmentFlags::Readable;
      break;
    case 7:
      Image.Sections[0].FileSz = 0x110;
      break;
    case 8:
      Image.Segments[0].FileSz = 0x110;
      break;
    case 9:
      Image.ObjCSourceReferences.clear();
      break;
    case 10:
      Image.ObjCSourceReferences[0x2100].TheKind =
          ObjCSourceReference::Kind::Class;
      break;
    case 11:
      Image.ObjCSourceReferences[0x2100].Size = 4;
      break;
    case 12:
      Image.ObjCSourceReferences[0x2100].Name.clear();
      break;
    case 13:
      Image.ImportPtrSlots[0x2180] = "_unrelated_runtime";
      break;
    case 14:
      llvm::support::endian::write32le(
          Image.Segments[0].Data.data() + 0x104, 0xf9408000); // LDR x0
      break;
    case 15:
      llvm::support::endian::write32le(
          Image.Segments[0].Data.data() + 0x110, 0xd61f0220); // BR x17
      break;
    }
    EXPECT_FALSE(objcSelectorStubOverwritesCommand(Image, 0x1100));
  }
}

TEST(ObjCCallHints, VerifiedSelectorStubDiscardsStaleCallerCommand) {
  for (bool Clobbered : {false, true}) {
    SCOPED_TRACE(Clobbered);
    auto Image = selectorStubImage();
    Image.ObjCMethods.clear();
    auto Function = callerWithStaleCommand(Clobbered);
    ASSERT_TRUE(verifyMedFunc(Function, "before-selector-stub-abi"));
    const auto ClobberCount = Function.CallClobbers.size();
    recoverCallAbi(Function, Arch::AArch64, {}, &Image);
    ASSERT_EQ(Function.CallInfos.size(), Clobbered ? 2U : 1U);
    const auto &Call = Function.CallInfos.back();
    EXPECT_EQ(Call.TargetAddr, 0x1100U);
    EXPECT_FALSE(Call.SourceCallHint);
    ASSERT_EQ(Call.Args.size(), 3U);
    ASSERT_TRUE(Call.Args[0].isConst());
    EXPECT_EQ(Call.Args[0].ConstVal, 0x2222U);
    ASSERT_TRUE(Call.Args[1].isConst());
    EXPECT_EQ(Call.Args[1].ConstVal, 0U);
    EXPECT_EQ(Call.Args[1].Size, 8U);
    ASSERT_TRUE(Call.Args[2].isConst());
    EXPECT_EQ(Call.Args[2].ConstVal, 0x123456U);
    EXPECT_EQ(Function.CallClobbers.size(), ClobberCount);
    EXPECT_TRUE(verifyMedFunc(Function, "after-selector-stub-abi"));
  }
}

TEST(ObjCCallHints, UnverifiedSelectorStubKeepsObservedCallerCommand) {
  for (bool NamedSection : {false, true}) {
    SCOPED_TRACE(NamedSection);
    auto Image = selectorStubImage();
    if (NamedSection)
      Image.ImportPtrSlots[0x2180] = "_unrelated_runtime";
    else
      Image.Sections[0].Name = "__text";
    auto Function = callerWithStaleCommand(false);
    recoverCallAbi(Function, Arch::AArch64, {}, &Image);
    ASSERT_EQ(Function.CallInfos.size(), 1U);
    const auto &Call = Function.CallInfos.front();
    EXPECT_FALSE(Call.SourceCallHint);
    ASSERT_EQ(Call.Args.size(), 3U);
    ASSERT_TRUE(Call.Args[1].isConst());
    EXPECT_EQ(Call.Args[1].ConstVal, 0x777777U);
    EXPECT_TRUE(verifyMedFunc(Function, "unverified-selector-stub-abi"));
  }
}

TEST(ObjCCallHints, ResolvesX64CallsiteSelectorAndRejectsStaleAlias) {
  auto Image = image(Arch::X64);
  auto Low = caller(Arch::X64);
  auto &Ops = Low.Blocks[0].Ops;
  Ops[0].Opcode = NdOp::INDIR_CALL;
  Ops[0].Inputs[0] = NdVar::cst(0x2180, 8);
  const auto SelectorReg = getTargetRegInfo(Arch::X64).IntParamRegs[1];
  Ops.insert(Ops.begin(), operation(NdOp::LOAD, NdVar::reg(SelectorReg, 8),
                                    {NdVar::cst(0x2100, 8)}, 0x11f0));
  EXPECT_EQ(buildObjCSourceCallHints(Image, Low).size(), 1U);
  Ops.insert(Ops.begin() + 1, operation(NdOp::COPY, NdVar::reg(SelectorReg, 1),
                                        {NdVar::cst(7, 1)}, 0x11f8));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Low).empty());
}

TEST(ObjCCallHints, KeepsFullRegisterArgumentListBeyondFormerInputCapacity) {
  auto Image = image();
  Image.ObjCMethods[0].TypeHint = signature(Arch::AArch64, 6);
  Image.ObjCMethods[0].Selector = "a:b:c:d:e:f:";
  Image.ObjCSourceReferences[0x2100].Name = Image.ObjCMethods[0].Selector;
  auto Low = caller();
  const auto Med = convert(Image, Low);
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  const auto &Call = Med.CallInfos[0];
  ASSERT_TRUE(Call.SourceCallHint);
  ASSERT_EQ(Call.Args.size(), 8U);
  for (size_t I = 2; I < 8; ++I) {
    EXPECT_EQ(Call.Args[I].Kind, MedVar::Reg) << I;
    EXPECT_EQ(Call.Args[I].RegOff, I * 8) << I;
  }
  const auto High = MedToHighConverter().convert(Med, Image.Arch);
  const auto *Expression = sourceCall(High);
  ASSERT_NE(Expression, nullptr);
  ASSERT_EQ(Expression->Operands.size(), 8U);
  for (size_t I = 2; I < 8; ++I) {
    ASSERT_TRUE(Expression->Operands[I]);
    EXPECT_EQ(Expression->Operands[I]->Kind, ExprKind::Var) << I;
    EXPECT_EQ(Expression->Operands[I]->Var.Kind, MedVar::Param) << I;
    EXPECT_EQ(Expression->Operands[I]->Var.RegOff, I * 8) << I;
  }
}

TEST(ObjCCallHints, NativeHintsPreserveIndependentFloatingRegisterBank) {
  auto Image = image();
  auto Hint = signature(Arch::AArch64, 0);
  Hint.ReturnType = NdType::makeFloat(8);
  Hint.Parameters.push_back({"floating", NdType::makeFloat(8)});
  Hint.Parameters.push_back({"integer", NdType::makeInt(4)});
  std::string Diagnostic;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Diagnostic));
  std::map<va_t, SourceFunctionTypeHint> Hints{{0x1600, Hint}};
  auto Low = caller();
  Low.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(0x1600, 8);
  Low.Blocks[0].Ops.back().Inputs[0] =
      NdVar::reg(getTargetRegInfo(Arch::AArch64).FPReturnReg, 8);
  LowToMedConverter Converter;
  Converter.setSourceCalleeTypeHints(&Hints);
  Converter.setSourceCallHintsEnabled(true);
  auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  recoverCallAbi(Med, Arch::AArch64, {});
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  const auto &Call = Med.CallInfos[0];
  ASSERT_TRUE(Call.SourceCallHint);
  EXPECT_EQ(Call.SourceCallHint->CallKind, SourceCallTypeHint::Kind::Native);
  ASSERT_EQ(Call.Args.size(), 4U);
  EXPECT_EQ(Call.Args[2].RegOff,
            getTargetRegInfo(Arch::AArch64).FPParamRegs[0]);
  EXPECT_EQ(Call.Args[3].RegOff,
            getTargetRegInfo(Arch::AArch64).IntParamRegs[2]);
  EXPECT_EQ(Med.Blocks[Call.BlockId].Ops[Call.OpIdx].Output.RegOff,
            getTargetRegInfo(Arch::AArch64).FPReturnReg);
}

TEST(ObjCCallHints, SuperDispatchUsesVerifiedRuntimeVeneerAndLoadedSelector) {
  auto Image = image();
  Image.ImportPtrSlots[0x2180] = "_objc_msgSendSuper2";
  auto Low = caller();
  Low.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(0x1108, 8);
  Low.Blocks[0].Ops.insert(
      Low.Blocks[0].Ops.begin(),
      operation(NdOp::LOAD, NdVar::reg(8, 8), {NdVar::cst(0x2100, 8)}, 0x11fc));
  auto Hints = buildObjCSourceCallHints(Image, Low);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.at(0x1200).CallKind, SourceCallTypeHint::Kind::ObjCSuper2);
  EXPECT_EQ(Hints.at(0x1200).SelectorReferenceAddress, 0U);
  Low.Blocks[0].Ops[1].Inputs[0] = NdVar::cst(0x2180, 8);
  // The address of a bound pointer slot is not the function stored in it.
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Low).empty());
}

TEST(ObjCCallHints, ExplicitNativeStackHintsRequireKnownX64CallInstruction) {
  auto Image = image(Arch::X64);
  auto Hint = signature(Arch::X64, 5); // seven values, one stack argument
  ASSERT_EQ(Hint.Parameters.back().Location.Kind, SourceABICarrierKind::Stack);
  std::map<va_t, SourceFunctionTypeHint> Hints{{0x1600, Hint}};
  auto Low = caller(Arch::X64);
  Low.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(0x1600, 8);
  auto Recover = [&] {
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    Converter.setSourceCalleeTypeHints(&Hints);
    Converter.setSourceCallHintsEnabled(true);
    auto Med = Converter.convert(Low, Arch::X64, BinaryFormat::MachO);
    recoverCallAbi(Med, Arch::X64, {});
    return Med;
  };
  auto Unknown = Recover();
  ASSERT_EQ(Unknown.CallInfos.size(), 1U);
  EXPECT_FALSE(Unknown.CallInfos[0].SourceCallHint);
  Image.Segments[0].Data[0x200] =
      0xe8; // actual near CALL pushes return address
  auto Bound = Recover();
  ASSERT_EQ(Bound.CallInfos.size(), 1U);
  ASSERT_TRUE(Bound.CallInfos[0].SourceCallHint);
  EXPECT_EQ(Bound.CallInfos[0].Args.size(), 7U);
  Image.IsRelocatable = true;
  auto Relocatable = Recover();
  ASSERT_EQ(Relocatable.CallInfos.size(), 1U);
  EXPECT_FALSE(Relocatable.CallInfos[0].SourceCallHint);
}

TEST(ObjCCallHints,
     OperandStorageRetainsSixtyFourArgumentsWithoutSilentTruncation) {
  MedOp Op;
  Op.Opcode = NdOp::CALL;
  Op.addInput(MedVar::makeConst(0x1400, 8));
  for (unsigned I = 0; I < 64; ++I)
    Op.addInput(MedVar::makeConst(I + 100, 8));
  ASSERT_EQ(Op.NumInputs, 65U);
  auto Copy = Op;
  ASSERT_EQ(Copy.Inputs.size(), 65U);
  for (unsigned I = 0; I < 64; ++I)
    EXPECT_EQ(Copy.Inputs[I + 1].ConstVal, I + 100);
}
} // namespace
