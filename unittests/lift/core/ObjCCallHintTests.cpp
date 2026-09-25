#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

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

TEST(ObjCCallHints, FrameworkDeclarationsSupplyAbsentScalarCallSignatures) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/Foundation.framework/Foundation");
  for (llvm::StringRef Selector : {"objectForKeyedSubscript:", "copy", "length",
                                   "addObject:", "doubleValue"}) {
    SCOPED_TRACE(Selector.str());
    Image.ObjCSourceReferences.at(0x2100).Name = Selector.str();
    const auto Hints = buildObjCSourceCallHints(Image, caller());
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Hint = Hints.at(0x1200);
    EXPECT_EQ(Hint.Selector, Selector);
    EXPECT_EQ(Hint.Signature.Parameters.size(),
              Selector.contains(':') ? 3U : 2U);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Hint.Signature, Diagnostic)) << Diagnostic;
  }
  Image.ObjCSourceReferences.at(0x2100).Name = "length";
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    auto Invalid = Image;
    Invalid.DyldBindSlots[0x2180] = {"_objc_msgSend", 0};
    if (Mutation == 0)
      Invalid.DyldBindSlots[0x2180].Addend = 4;
    if (Mutation == 1)
      Invalid.DyldBindSlots[0x2180].WeakImport = true;
    if (Mutation == 2)
      Invalid.DyldBindSlots[0x2180].Name = "_other";
    if (Mutation == 3)
      Invalid.ImportStorageSlots[0x2180] = {"_objc_msgSend", 8};
    EXPECT_TRUE(buildObjCSourceCallHints(Invalid, caller()).empty())
        << Mutation;
  }
}

TEST(ObjCCallHints, FrameworkDeclarationsKeepMissingAndConflictingEvidence) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "length";
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.DynInfo.NeededLibs = {"/tmp/Foundation.framework/Foundation"};
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"};
  ASSERT_EQ(buildObjCSourceCallHints(Image, caller()).size(), 1U);
  ObjCMethod Conflict;
  Conflict.Selector = "length";
  Conflict.TypeHint = signature(Image.Arch, 0); // SDK result is 64 bits.
  Image.ObjCMethods.push_back(Conflict);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.ObjCMethods.clear();
  ObjCProtocol Protocol;
  ObjCProtocolMethod Unsupported;
  Unsupported.Selector = "length";
  Protocol.Methods.push_back(Unsupported);
  Image.ObjCProtocols.push_back(Protocol);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  // A declaration absent from the SDK's common platform scope cannot veto
  // the independently observed contract of an application's own selector.
  Image.ObjCProtocols.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "sharedInstance";
  Conflict.Selector = "sharedInstance";
  Image.ObjCMethods.push_back(Conflict);
  EXPECT_EQ(buildObjCSourceCallHints(Image, caller()).size(), 1U);
}

TEST(ObjCCallHints, ExactMethodForwardingSuppliesMissingSelectorDeclaration) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "ddLogLevel";
  ObjCMethod Wrapper;
  Wrapper.ClassName = "DDLog";
  Wrapper.IsClassMethod = true;
  Wrapper.Selector = "levelForClass:";
  Wrapper.Implementation = 0x1200;
  Wrapper.TypeHint = parseObjCMethodEncoding(Wrapper.Selector, "q24@0:8#16");
  ASSERT_TRUE(Wrapper.TypeHint);
  Image.ObjCMethods.push_back(Wrapper);

  const auto &TRI = getTargetRegInfo(Image.Arch);
  auto Function = caller();
  Function.Blocks[0].Ops.back().Inputs[0] = NdVar::reg(TRI.IntReturnReg, 8);
  Function.Blocks[0].Ops.insert(
      Function.Blocks[0].Ops.begin(),
      operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                {NdVar::reg(TRI.IntParamRegs[2], 8)}, 0x11fc));
  auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_TRUE(Hints.count(0x1200));
  const auto &Getter = Hints.at(0x1200);
  ASSERT_TRUE(Getter.SelectorForwardingUse);
  EXPECT_EQ(Getter.SelectorForwardingUse->MethodEntry, 0x1200U);
  EXPECT_EQ(Getter.SelectorForwardingUse->ReceiverSourceParameter, 2U);
  EXPECT_TRUE(Getter.SelectorForwardingUse->ArgumentSourceParameters.empty());
  ASSERT_TRUE(Getter.Signature.ReturnType);
  EXPECT_EQ(Getter.Signature.ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Getter.Signature.ReturnType->Size, 8U);

  // The setter forwards its declared level and Class parameters into the
  // receiver and sole selector argument before returning void.
  Image.ObjCSourceReferences.at(0x2100).Name = "ddSetLogLevel:";
  Image.ObjCMethods[0].Selector = "setLevel:forClass:";
  Image.ObjCMethods[0].TypeHint =
      parseObjCMethodEncoding(Image.ObjCMethods[0].Selector, "v32@0:8q16#24");
  ASSERT_TRUE(Image.ObjCMethods[0].TypeHint);
  Function.Blocks[0].Ops = {
      operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                {NdVar::reg(TRI.IntParamRegs[3], 8)}, 0x11f8),
      operation(NdOp::CALL, NdVar::reg(TRI.IntReturnReg, 8),
                {NdVar::cst(0x1100, 8)}, 0x1200),
      operation(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 8)}, 0x1204)};
  Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_TRUE(Hints.count(0x1200));
  const auto &Setter = Hints.at(0x1200);
  ASSERT_TRUE(Setter.SelectorForwardingUse);
  EXPECT_EQ(Setter.SelectorForwardingUse->ReceiverSourceParameter, 3U);
  EXPECT_EQ(Setter.SelectorForwardingUse->ArgumentSourceParameters,
            std::vector<unsigned>{2});
  ASSERT_EQ(Setter.Signature.Parameters.size(), 3U);
  EXPECT_EQ(Setter.Signature.Parameters[2].Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(Setter.Signature.Parameters[2].Type->Size, 8U);
  EXPECT_EQ(Setter.Signature.ReturnType->Kind, NdTypeKind::Void);

  // A rewritten forwarded value, a non-tail call, or an incomplete selector
  // declaration removes the proof instead of guessing the ABI.
  auto Invalid = Function;
  Invalid.Blocks[0].Ops.insert(
      Invalid.Blocks[0].Ops.begin() + 1,
      operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[2], 8),
                {NdVar::cst(7, 8)}, 0x11fc));
  EXPECT_FALSE(buildObjCSourceCallHints(Image, Invalid).count(0x1200));
  Invalid = Function;
  Invalid.Blocks[0].Ops.insert(
      Invalid.Blocks[0].Ops.end() - 1,
      operation(NdOp::COPY, NdVar::reg(a64reg::X19, 8),
                {NdVar::cst(0, 8)}, 0x1202));
  EXPECT_FALSE(buildObjCSourceCallHints(Image, Invalid).count(0x1200));
  ObjCMethod Unknown;
  Unknown.ClassName = "UnknownLogger";
  Unknown.Selector = "ddSetLogLevel:";
  Image.ObjCMethods.push_back(std::move(Unknown));
  EXPECT_FALSE(buildObjCSourceCallHints(Image, Function).count(0x1200));
  Image.ObjCMethods.back().TypeHint =
      parseObjCMethodEncoding("ddSetLogLevel:", "v24@0:8@16");
  ASSERT_TRUE(Image.ObjCMethods.back().TypeHint);
  EXPECT_FALSE(buildObjCSourceCallHints(Image, Function).count(0x1200));
}

TEST(ObjCCallHints, FrameworkProvidersRequireExactActivationAndAgreement) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.ObjCMethods.clear();
    const std::string Foundation =
        "/System/Library/Frameworks/Foundation.framework/Foundation";
    const std::string CoreData =
        "/System/Library/Frameworks/CoreData.framework/CoreData";
    Image.DynInfo.NeededLibs = {Foundation};
    auto Options = objcSelectorSourceTypeHint(Image, "options");
    ASSERT_TRUE(Options);
    EXPECT_EQ(Options->ReturnType->Kind, NdTypeKind::Int);
    Image.DynInfo.NeededLibs = {CoreData};
    Options = objcSelectorSourceTypeHint(Image, "options");
    ASSERT_TRUE(Options);
    EXPECT_EQ(Options->ReturnType->Kind, NdTypeKind::Ptr);
    for (const bool Reverse : {false, true}) {
      Image.DynInfo.NeededLibs = Reverse ? std::vector{CoreData, Foundation}
                                         : std::vector{Foundation, CoreData};
      EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "options"));
    }
    constexpr auto Selector = "executeFetchRequest:error:";
    for (const auto *Module :
         {"/tmp/CoreData.framework/CoreData",
          "/System/Library/Frameworks/CoreData.framework/Versions/C/CoreData",
          "/System/Library/Frameworks/Foundation.framework/Foundation"}) {
      Image.DynInfo.NeededLibs = {Module};
      EXPECT_FALSE(objcSelectorSourceTypeHint(Image, Selector)) << Module;
    }
    for (const auto *Module :
         {"/System/Library/Frameworks/CoreData.framework/CoreData",
          "/System/Library/Frameworks/CoreData.framework/Versions/A/"
          "CoreData"}) {
      Image.DynInfo.NeededLibs = {Module};
      auto Hint = objcSelectorSourceTypeHint(Image, Selector);
      ASSERT_TRUE(Hint) << Module;
      EXPECT_EQ(Hint->Architecture, Architecture);
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
      ASSERT_EQ(Hint->Parameters.size(), 4U);
      EXPECT_EQ(Hint->Parameters[3].Type->Kind, NdTypeKind::Ptr);
      EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "doubleValue"));
      Image.DynInfo.NeededLibs.push_back(
          "/System/Library/Frameworks/Foundation.framework/Foundation");
      EXPECT_TRUE(objcSelectorSourceTypeHint(Image, "doubleValue"));
      ObjCMethod Conflict;
      Conflict.Selector = Selector;
      Conflict.TypeHint = signature(Architecture, 0);
      Image.ObjCMethods.push_back(Conflict);
      EXPECT_FALSE(objcSelectorSourceTypeHint(Image, Selector));
      Image.ObjCMethods.clear();
      EXPECT_EQ(bool(objcSelectorSourceTypeHint(Image, "save:")),
                Architecture == Arch::AArch64);
      Image.DynInfo.NeededLibs.clear();
      EXPECT_FALSE(objcSelectorSourceTypeHint(Image, Selector));
    }
  }
}

TEST(ObjCCallHints, ResultUseDisambiguatesConflictingSelectorDeclarations) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "save:";
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/CoreData.framework/CoreData"};
  ObjCMethod VoidSave;
  VoidSave.ClassName = "ApplicationController";
  VoidSave.Selector = "save:";
  VoidSave.TypeHint = parseObjCMethodEncoding("save:", "v24@0:8@16");
  ASSERT_TRUE(VoidSave.TypeHint);
  Image.ObjCMethods.push_back(VoidSave);

  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "save:"));
  auto Function = caller();
  auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x1200);
  EXPECT_EQ(Hint.Selector, "save:");
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 1U);
  EXPECT_TRUE(Hint.Signature.ReturnLocation.ExtendTo32Bits);
  ASSERT_TRUE(Hint.SelectorResultUse);
  EXPECT_EQ(Hint.SelectorResultUse->RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint.SelectorResultUse->ValueBytes, 4U);

  // A full-width save can transport the narrow result so long as its later
  // use proves that only the declared W0 bytes are consumed.
  Function = caller();
  Function.Blocks[0].Ops.insert(Function.Blocks[0].Ops.begin() + 1,
                                operation(NdOp::COPY,
                                          NdVar::reg(a64reg::X20, 8),
                                          {NdVar::reg(a64reg::X0, 8)}, 0x1202));
  Function.Blocks[0].Ops.back().Inputs[0] = NdVar::reg(a64reg::X20, 4);
  Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_TRUE(Hints.count(0x1200));
  ASSERT_TRUE(Hints.at(0x1200).SelectorResultUse);
  EXPECT_EQ(Hints.at(0x1200).SelectorResultUse->ValueBytes, 4U);

  // An intervening call with no authenticated ABI revokes the saved alias.
  Function.Blocks[0].Ops.insert(Function.Blocks[0].Ops.end() - 1,
                                operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                                          {NdVar::cst(0x1500, 8)}, 0x1203));
  EXPECT_FALSE(buildObjCSourceCallHints(Image, Function).count(0x1200));

  // No observed result read leaves both declarations possible.
  Function.Blocks[0].Ops[1].NumInputs = 0;
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());

  // A new definition before the read severs the call-result provenance.
  Function = caller();
  Function.Blocks[0].Ops.insert(Function.Blocks[0].Ops.begin() + 1,
                                operation(NdOp::COPY, NdVar::reg(a64reg::X0, 4),
                                          {NdVar::cst(1, 4)}, 0x1202));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());

  // BOOL defines W0 on arm64, not the full X0 read requested here.
  Function = caller();
  Function.Blocks[0].Ops[1].Inputs[0] = NdVar::reg(a64reg::X0, 8);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());

  // Any incomplete declaration remains a veto even when another candidate
  // happens to fit the observed carrier.
  ObjCMethod Unknown = VoidSave;
  Unknown.ClassName = "UnknownController";
  Unknown.TypeHint.reset();
  Image.ObjCMethods.push_back(std::move(Unknown));
  Function = caller();
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
}

TEST(ObjCCallHints,
     ResultConsumerTypeDisambiguatesEqualWidthSelectorDeclarations) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "code";
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  ObjCMethod ObjectCode;
  ObjectCode.ClassName = "ApplicationValue";
  ObjectCode.Selector = "code";
  ObjectCode.TypeHint = parseObjCMethodEncoding("code", "@16@0:8");
  ASSERT_TRUE(ObjectCode.TypeHint);
  Image.ObjCMethods.push_back(ObjectCode);
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "code"));

  // A catalogued retain consumes its first parameter as an object pointer.
  // The call boundary is an implicit register use in LowIR, so it supplies
  // type evidence before the ordinary caller-saved clobber kills X0.
  Image.ImportPtrSlots[0x2190] = "_objc_retainAutoreleasedReturnValue";
  const uint32_t RetainStub[] = {0xb0000010, 0xf940ca10, 0xd61f0200};
  for (size_t I = 0; I < 3; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x140 + I * 4, RetainStub[I]);
  auto Function = caller();
  Function.Blocks[0].EndAddr = 0x120c;
  Function.Blocks[0].Ops = {
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                {NdVar::cst(0x1100, 8)}, 0x1200),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                {NdVar::cst(0x1140, 8)}, 0x1204),
      operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x1208)};
  auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_TRUE(Hints.count(0x1200));
  EXPECT_EQ(Hints.at(0x1200).Signature.ReturnType->Kind, NdTypeKind::Ptr);
  ASSERT_TRUE(Hints.at(0x1200).SelectorResultTypeUse);
  EXPECT_EQ(*Hints.at(0x1200).SelectorResultTypeUse, NdTypeKind::Ptr);

  // An untyped integer operation is not a declared source consumer: a casted
  // object pointer has the same machine operation. Equal width remains
  // ambiguous rather than guessing the dynamic receiver's implementation.
  Function.Blocks[0].Ops = {
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                {NdVar::cst(0x1100, 8)}, 0x1200),
      operation(NdOp::INT_SUB, NdVar::reg(a64reg::X8, 8),
                {NdVar::reg(a64reg::X0, 8), NdVar::cst(4, 8)}, 0x1204),
      operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X8, 8)}, 0x1208)};
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());

  Function.Blocks[0].Ops[1] =
      operation(NdOp::COPY, NdVar::reg(a64reg::X8, 8),
                {NdVar::reg(a64reg::X0, 8)}, 0x1204);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
}

TEST(ObjCCallHints,
     RetainConsumerSelectsUIKitObjectFromConflictingSystemVersionDeclarations) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "systemVersion";
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation",
      "/System/Library/Frameworks/UIKit.framework/UIKit"};
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "systemVersion"));

  Image.ImportPtrSlots[0x2190] = "_objc_retainAutoreleasedReturnValue";
  const uint32_t RetainStub[] = {0xb0000010, 0xf940ca10, 0xd61f0200};
  for (size_t I = 0; I < 3; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x140 + I * 4, RetainStub[I]);
  auto Function = caller();
  Function.Blocks[0].EndAddr = 0x120c;
  Function.Blocks[0].Ops = {
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                {NdVar::cst(0x1100, 8)}, 0x1200),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                {NdVar::cst(0x1140, 8)}, 0x1204),
      operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x1208)};

  const auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_TRUE(Hints.count(0x1200));
  const auto &Hint = Hints.at(0x1200);
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Ptr);
  ASSERT_TRUE(Hint.SelectorResultTypeUse);
  EXPECT_EQ(*Hint.SelectorResultTypeUse, NdTypeKind::Ptr);
}

TEST(ObjCCallHints,
     DeclaredEntryArgumentTypeDisambiguatesConflictingSelectorDeclarations) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "save:";
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/CoreData.framework/CoreData"};

  ObjCMethod VoidSave;
  VoidSave.ClassName = "ApplicationController";
  VoidSave.Selector = "save:";
  VoidSave.TypeHint = parseObjCMethodEncoding("save:", "v24@0:8@16");
  ASSERT_TRUE(VoidSave.TypeHint);
  Image.ObjCMethods.push_back(VoidSave);

  ObjCMethod Caller;
  Caller.ClassName = "Migrator";
  Caller.Selector = "runWithObject:error:";
  Caller.Implementation = 0x1200;
  Caller.TypeHint = parseObjCMethodEncoding(Caller.Selector, "v32@0:8@16^@24");
  ASSERT_TRUE(Caller.TypeHint);
  Image.ObjCMethods.push_back(Caller);
  const auto CallerSignature =
      objcMethodSourceTypeHint(Image, Caller.Implementation);
  ASSERT_TRUE(CallerSignature);
  ASSERT_EQ(CallerSignature->Parameters.size(), 4U);
  EXPECT_EQ(CallerSignature->Parameters[3].Location.RegisterOffset, a64reg::X3);
  SourceCallTypeHint::SelectorArgumentTypeEvidence DirectEvidence;
  DirectEvidence.Parameter = 2;
  DirectEvidence.MethodEntry = Caller.Implementation;
  DirectEvidence.Source = CallerSignature->Parameters[3].Location;
  EXPECT_TRUE(objcSelectorSourceTypeHintForArgumentTypeUse(
      Image, "save:", DirectEvidence));

  LowFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "typed_argument_caller";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x1200;
  Block.EndAddr = 0x120c;
  Block.Ops = {operation(NdOp::COPY, NdVar::reg(a64reg::X2, 8),
                         {NdVar::reg(a64reg::X3, 8)}, 0x1200),
               operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                         {NdVar::cst(0x1100, 8)}, 0x1204),
               operation(NdOp::RETURN, {}, {}, 0x1208)};
  Function.Blocks.push_back(Block);

  auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x1204);
  ASSERT_TRUE(Hint.SelectorArgumentTypeUse);
  EXPECT_EQ(Hint.SelectorArgumentTypeUse->Parameter, 2U);
  EXPECT_EQ(Hint.SelectorArgumentTypeUse->MethodEntry, 0x1200U);
  EXPECT_EQ(Hint.SelectorArgumentTypeUse->Source.Kind,
            SourceABICarrierKind::IntegerRegister);
  EXPECT_EQ(Hint.SelectorArgumentTypeUse->Source.RegisterOffset, a64reg::X3);
  ASSERT_EQ(Hint.Signature.Parameters.size(), 3U);
  ASSERT_TRUE(Hint.Signature.Parameters[2].Type->Pointee);
  EXPECT_EQ(Hint.Signature.Parameters[2].Type->Pointee->Kind, NdTypeKind::Ptr);

  // A typed spill below an escaped higher-addressed frame object remains
  // private. The same spill above the escaped base cannot supply evidence.
  Block.Ops = {
      operation(NdOp::INT_SUB, NdVar::reg(a64reg::SP, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(64, 4)}, 0x1200),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X20, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(32, 4)}, 0x1204),
      operation(NdOp::STORE, {},
                {NdVar::reg(a64reg::X20, 8), NdVar::reg(a64reg::X3, 8)},
                0x1208),
      operation(NdOp::COPY, NdVar::reg(a64reg::X20, 8), {NdVar::cst(0, 8)},
                0x120c),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0x1300, 8)},
                0x1210),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X20, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(32, 4)}, 0x1214),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X21, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(48, 4)}, 0x1218),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X22, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(56, 4)}, 0x121c),
      operation(NdOp::STORE, {},
                {NdVar::reg(a64reg::X22, 8), NdVar::reg(a64reg::X21, 8)},
                0x1220),
      operation(NdOp::LOAD, NdVar::reg(a64reg::X23, 8),
                {NdVar::reg(a64reg::X20, 8)}, 0x1224),
      operation(NdOp::COPY, NdVar::reg(a64reg::X2, 8),
                {NdVar::reg(a64reg::X23, 8)}, 0x1228),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0x1100, 8)},
                0x122c),
      operation(NdOp::RETURN, {}, {}, 0x1230)};
  Block.EndAddr = 0x1234;
  Function.Blocks[0] = Block;
  Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_TRUE(Hints.at(0x122c).SelectorArgumentTypeUse);

  Block.Ops[6] =
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X21, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(16, 4)}, 0x1218);
  Function.Blocks[0] = Block;
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());

  // Bare object parameters have the same machine width but do not prove an
  // NSError ** contract.
  Image.ObjCMethods.back().TypeHint =
      parseObjCMethodEncoding(Caller.Selector, "v32@0:8@16@24");
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());

  // Every method record sharing the entry must agree on the source type.
  Image.ObjCMethods.back().TypeHint = Caller.TypeHint;
  auto Alias = Image.ObjCMethods.back();
  Alias.Selector = "aliasWithObject:error:";
  Alias.TypeHint = parseObjCMethodEncoding(Alias.Selector, "v32@0:8@16@24");
  Image.ObjCMethods.push_back(std::move(Alias));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
}

TEST(ObjCCallHints,
     DeclaredObjectArgumentDisambiguatesPointerAndFloatingDeclarations) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "setProgress:";
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/UIKit.framework/UIKit"};

  ObjCMethod ObjectSetter;
  ObjectSetter.ClassName = "LoadState";
  ObjectSetter.Selector = "setProgress:";
  ObjectSetter.TypeHint =
      parseObjCMethodEncoding(ObjectSetter.Selector, "v24@0:8@16");
  ASSERT_TRUE(ObjectSetter.TypeHint);
  Image.ObjCMethods.push_back(ObjectSetter);
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, ObjectSetter.Selector));

  ObjCMethod Caller;
  Caller.ClassName = "ImageView";
  Caller.Selector = "forwardProgress:";
  Caller.Implementation = 0x1200;
  Caller.TypeHint = parseObjCMethodEncoding(Caller.Selector, "v24@0:8@16");
  ASSERT_TRUE(Caller.TypeHint);
  Image.ObjCMethods.push_back(Caller);

  const auto CallerSignature =
      objcMethodSourceTypeHint(Image, Caller.Implementation);
  ASSERT_TRUE(CallerSignature);
  SourceCallTypeHint::SelectorArgumentTypeEvidence Evidence;
  Evidence.Parameter = 2;
  Evidence.MethodEntry = Caller.Implementation;
  Evidence.Source = CallerSignature->Parameters[2].Location;
  EXPECT_FALSE(objcSelectorSourceTypeHintForArgumentTypeUse(
      Image, ObjectSetter.Selector, Evidence));
  Evidence.ConsumedAsObject = true;
  auto Direct = objcSelectorSourceTypeHintForArgumentTypeUse(
      Image, ObjectSetter.Selector, Evidence);
  ASSERT_TRUE(Direct);
  ASSERT_EQ(Direct->Parameters.size(), 3U);
  ASSERT_TRUE(Direct->Parameters[2].Type);
  EXPECT_EQ(Direct->Parameters[2].Type->Kind, NdTypeKind::Ptr);

  LowFunc Function;
  Function.Entry = Caller.Implementation;
  Function.Name = "object_argument_caller";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  Block.EndAddr = 0x1214;
  Image.ImportPtrSlots[0x2190] = "_objc_retain";
  const uint32_t RetainStub[] = {0xb0000010, 0xf940ca10, 0xd61f0200};
  for (size_t I = 0; I < 3; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x140 + I * 4, RetainStub[I]);
  Block.Ops = {
      operation(NdOp::COPY, NdVar::reg(a64reg::X19, 8),
                {NdVar::reg(a64reg::X2, 8)}, 0x1200),
      operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                {NdVar::reg(a64reg::X19, 8)}, 0x1204),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                {NdVar::cst(0x1140, 8)}, 0x1208),
      operation(NdOp::COPY, NdVar::reg(a64reg::X2, 8),
                {NdVar::reg(a64reg::X19, 8)}, 0x120c),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                {NdVar::cst(0x1100, 8)}, 0x1210)};
  Function.Blocks.push_back(Block);

  const auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 2U);
  const auto &Hint = Hints.at(0x1210);
  ASSERT_TRUE(Hint.SelectorArgumentTypeUse);
  EXPECT_EQ(Hint.SelectorArgumentTypeUse->Parameter, 2U);
  EXPECT_EQ(Hint.SelectorArgumentTypeUse->MethodEntry, Caller.Implementation);
  EXPECT_TRUE(Hint.SelectorArgumentTypeUse->ConsumedAsObject);
  EXPECT_EQ(Hint.Signature.Parameters[2].Type->Kind, NdTypeKind::Ptr);

  // The fact is exact source provenance, not a width guess. An overwrite of
  // the saved carrier removes the evidence even though the value stays eight
  // bytes wide.
  Function.Blocks[0].Ops.insert(
      Function.Blocks[0].Ops.begin() + 1,
      operation(NdOp::COPY, NdVar::reg(a64reg::X19, 8),
                {NdVar::cst(1, 8)}, 0x1202));
  EXPECT_FALSE(buildObjCSourceCallHints(Image, Function).count(0x1210));
}

TEST(ObjCCallHints,
     PrivateFrameStorageDisambiguatesPointerToPointerSelectorArguments) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "save:";
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/CoreData.framework/CoreData"};
  ObjCMethod VoidSave;
  VoidSave.ClassName = "ApplicationController";
  VoidSave.Selector = "save:";
  VoidSave.TypeHint = parseObjCMethodEncoding("save:", "v24@0:8@16");
  ASSERT_TRUE(VoidSave.TypeHint);
  Image.ObjCMethods.push_back(VoidSave);

  SourceCallTypeHint::SelectorArgumentStorageEvidence DirectEvidence;
  DirectEvidence.Parameter = 2;
  DirectEvidence.FrameOffset = -24;
  auto Direct = objcSelectorSourceTypeHintForArgumentStorageUse(
      Image, "save:", DirectEvidence);
  ASSERT_TRUE(Direct);
  ASSERT_EQ(Direct->Parameters.size(), 3U);
  ASSERT_TRUE(Direct->Parameters[2].Type->Pointee);
  EXPECT_EQ(Direct->Parameters[2].Type->Pointee->Kind, NdTypeKind::Ptr);
  DirectEvidence.FrameOffset = 8;
  EXPECT_FALSE(objcSelectorSourceTypeHintForArgumentStorageUse(
      Image, "save:", DirectEvidence));
  DirectEvidence.FrameOffset = -24;
  auto AmbiguousImage = Image;
  ObjCMethod OtherPointerSave;
  OtherPointerSave.ClassName = "OtherController";
  OtherPointerSave.Selector = "save:";
  OtherPointerSave.TypeHint =
      parseObjCMethodEncoding("save:", "B24@0:8^^i16");
  ASSERT_TRUE(OtherPointerSave.TypeHint);
  AmbiguousImage.ObjCMethods.push_back(std::move(OtherPointerSave));
  EXPECT_FALSE(objcSelectorSourceTypeHintForArgumentStorageUse(
      AmbiguousImage, "save:", DirectEvidence));

  LowFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "frame_storage_caller";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x1200;
  Block.EndAddr = 0x1210;
  Block.Ops = {
      operation(NdOp::INT_SUB, NdVar::reg(a64reg::X20, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(64, 4)}, 0x1200),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X2, 8),
                {NdVar::reg(a64reg::X20, 8), NdVar::cst(40, 4)}, 0x1204),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                {NdVar::cst(0x1100, 8)}, 0x1208),
      operation(NdOp::RETURN, {}, {}, 0x120c)};
  Function.Blocks.push_back(Block);

  auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x1208);
  ASSERT_TRUE(Hint.SelectorArgumentStorageUse);
  EXPECT_EQ(Hint.SelectorArgumentStorageUse->Parameter, 2U);
  EXPECT_EQ(Hint.SelectorArgumentStorageUse->FrameOffset, -24);
  EXPECT_FALSE(Hint.SelectorArgumentTypeUse);
  EXPECT_FALSE(Hint.SelectorResultUse);

  // An incoming/caller-owned address is not private frame storage.
  Block.Ops[0] = operation(NdOp::INT_ADD, NdVar::reg(a64reg::X20, 8),
                           {NdVar::reg(a64reg::SP, 8), NdVar::cst(8, 4)},
                           0x1200);
  Block.Ops[1] = operation(NdOp::COPY, NdVar::reg(a64reg::X2, 8),
                           {NdVar::reg(a64reg::X20, 8)}, 0x1204);
  Function.Blocks[0] = Block;
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
}

TEST(ObjCCallHints,
     SDKObjectPointerOutParameterQualifiesTheLoadedReceiverClass) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  Image.ObjCSourceReferences.at(0x2100).Name = "writeToFile:options:error:";

  const auto Class =
      objcSelectorOutParameterClass(Image, "writeToFile:options:error:", 4);
  ASSERT_TRUE(Class);
  EXPECT_EQ(*Class, "NSError");
  EXPECT_FALSE(
      objcSelectorOutParameterClass(Image, "writeToFile:options:error:", 3));

  ObjCReceiverTypeHint Receiver;
  Receiver.Origin = ObjCReceiverTypeHint::OriginKind::OutParameter;
  Receiver.Address = 0x2100;
  Receiver.ClassName = *Class;
  Receiver.OutParameters.push_back({0x2100, "writeToFile:options:error:", 4});
  EXPECT_TRUE(objcReceiverTypeHintValid(Image, Receiver));
  const auto Code = objcReceiverSourceTypeHint(Image, "code", Receiver);
  ASSERT_TRUE(Code.Signature);
  EXPECT_EQ(Code.Signature->ReturnType->Kind, NdTypeKind::Int);
  ObjCMethod ConflictingCode;
  ConflictingCode.ClassName = "ApplicationEvent";
  ConflictingCode.Selector = "code";
  ConflictingCode.TypeHint =
      parseObjCMethodEncoding(ConflictingCode.Selector, "@16@0:8");
  ASSERT_TRUE(ConflictingCode.TypeHint);
  Image.ObjCMethods.push_back(std::move(ConflictingCode));

  ObjCSourceReference CodeReference;
  CodeReference.Address = 0x2108;
  CodeReference.Name = "code";
  Image.ObjCSourceReferences.emplace(CodeReference.Address, CodeReference);
  // ADRP x1,0x2000; LDR x1,[x1,#0x108]; ADRP x16,0x2000;
  // LDR x16,[x16,#0x180]; BR x16.
  const uint32_t CodeStub[] = {0xb0000001, 0xf9408421, 0xb0000010, 0xf940c210,
                               0xd61f0200};
  for (size_t I = 0; I < 5; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x120 + I * 4, CodeStub[I]);

  LowFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "error_out_parameter_caller";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  Block.EndAddr = 0x1224;
  Block.Ops = {
      operation(NdOp::INT_SUB, NdVar::reg(a64reg::SP, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(64, 4)}, 0x1200),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X19, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(40, 4)}, 0x1204),
      operation(NdOp::STORE, {}, {NdVar::reg(a64reg::X19, 8), NdVar::cst(0, 8)},
                0x1208),
      operation(NdOp::COPY, NdVar::reg(a64reg::X4, 8),
                {NdVar::reg(a64reg::X19, 8)}, 0x120c),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0x1100, 8)},
                0x1210),
      operation(NdOp::LOAD, NdVar::reg(a64reg::X0, 8),
                {NdVar::reg(a64reg::X19, 8)}, 0x1214),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0x1120, 8)},
                0x1218),
      operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x121c)};
  Function.Blocks.push_back(Block);
  const auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_TRUE(Hints.count(0x1210));
  ASSERT_TRUE(Hints.count(0x1218));
  ASSERT_TRUE(Hints.at(0x1218).Receiver);
  EXPECT_EQ(Hints.at(0x1218).Receiver->Origin,
            ObjCReceiverTypeHint::OriginKind::OutParameter);
  EXPECT_EQ(Hints.at(0x1218).Signature.ReturnType->Kind, NdTypeKind::Int);
  auto Overwritten = Function;
  Overwritten.Blocks[0].Ops.insert(
      Overwritten.Blocks[0].Ops.begin() + 5,
      operation(NdOp::STORE, {}, {NdVar::reg(a64reg::X19, 8), NdVar::cst(1, 8)},
                0x1212));
  const auto OverwrittenHints = buildObjCSourceCallHints(Image, Overwritten);
  EXPECT_TRUE(OverwrittenHints.count(0x1210));
  EXPECT_FALSE(OverwrittenHints.count(0x1218));
  auto Cleared = Function;
  Cleared.Blocks[0].Ops.insert(
      Cleared.Blocks[0].Ops.begin() + 5,
      operation(NdOp::STORE, {}, {NdVar::reg(a64reg::X19, 8), NdVar::cst(0, 8)},
                0x1212));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Cleared).count(0x1218));
  auto Escaped = Function;
  Escaped.Blocks[0].Ops.insert(
      Escaped.Blocks[0].Ops.begin() + 2,
      operation(NdOp::STORE, {},
                {NdVar::reg(a64reg::X10, 8), NdVar::reg(a64reg::X19, 8)},
                0x1206));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Escaped).count(0x1218));

  ObjCSourceReference AttributesReference;
  AttributesReference.Address = 0x2110;
  AttributesReference.Name = "setAttributes:ofItemAtPath:error:";
  Image.ObjCSourceReferences.emplace(AttributesReference.Address,
                                     AttributesReference);
  const uint32_t AttributesStub[] = {0xb0000001, 0xf9408821, 0xb0000010,
                                     0xf940c210, 0xd61f0200};
  for (size_t I = 0; I < 5; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x140 + I * 4, AttributesStub[I]);
  LowFunc Joined;
  Joined.Entry = 0x1200;
  Joined.Name = "joined_error_out_parameter_caller";
  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = 0x1200;
  Entry.EndAddr = 0x120c;
  Entry.Succs = {1, 2};
  Entry.Ops = {
      operation(NdOp::INT_SUB, NdVar::reg(a64reg::SP, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(64, 4)}, 0x1200),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X19, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(40, 4)}, 0x1204),
      operation(NdOp::STORE, {}, {NdVar::reg(a64reg::X19, 8), NdVar::cst(0, 8)},
                0x1208)};
  LowBlock Write;
  Write.Id = 1;
  Write.StartAddr = 0x1210;
  Write.EndAddr = 0x1218;
  Write.Preds = {0};
  Write.Succs = {3};
  Write.Ops = {operation(NdOp::COPY, NdVar::reg(a64reg::X4, 8),
                         {NdVar::reg(a64reg::X19, 8)}, 0x1210),
               operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                         {NdVar::cst(0x1100, 8)}, 0x1214)};
  LowBlock Attributes = Write;
  Attributes.Id = 2;
  Attributes.StartAddr = 0x1220;
  Attributes.EndAddr = 0x1228;
  Attributes.Ops[0].Addr = 0x1220;
  Attributes.Ops[1] = operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                                {NdVar::cst(0x1140, 8)}, 0x1224);
  LowBlock Join;
  Join.Id = 3;
  Join.StartAddr = 0x1230;
  Join.EndAddr = 0x123c;
  Join.Preds = {1, 2};
  Join.Ops = {operation(NdOp::LOAD, NdVar::reg(a64reg::X0, 8),
                        {NdVar::reg(a64reg::X19, 8)}, 0x1230),
              operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                        {NdVar::cst(0x1120, 8)}, 0x1234),
              operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x1238)};
  Joined.Blocks = {Entry, Write, Attributes, Join};
  const auto JoinedHints = buildObjCSourceCallHints(Image, Joined);
  ASSERT_TRUE(JoinedHints.count(0x1234));
  ASSERT_TRUE(JoinedHints.at(0x1234).Receiver);
  EXPECT_EQ(JoinedHints.at(0x1234).Receiver->OutParameters.size(), 2U);
  EXPECT_TRUE(
      objcReceiverTypeHintValid(Image, *JoinedHints.at(0x1234).Receiver));

  // Runtime metadata cannot preserve the NSError pointee spelling in ^@.
  // A local declaration therefore vetoes the SDK-only source class fact.
  ObjCMethod Local;
  Local.ClassName = "LocalData";
  Local.Selector = "writeToFile:options:error:";
  Local.TypeHint = parseObjCMethodEncoding(Local.Selector, "B40@0:8@16Q24^@32");
  ASSERT_TRUE(Local.TypeHint);
  Image.ObjCMethods.push_back(std::move(Local));
  EXPECT_FALSE(
      objcSelectorOutParameterClass(Image, "writeToFile:options:error:", 4));
  EXPECT_FALSE(objcReceiverTypeHintValid(Image, Receiver));
}

TEST(ObjCCallHints, ReceiverIdentitySurvivesOnlyLowerPrivateFrameSpills) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "save:";
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/CoreData.framework/CoreData"};

  ObjCMethod LocalSave;
  LocalSave.ClassName = "ExploreController";
  LocalSave.Selector = "save:";
  LocalSave.Implementation = 0x1400;
  LocalSave.TypeHint = parseObjCMethodEncoding("save:", "v24@0:8@16");
  ASSERT_TRUE(LocalSave.TypeHint);
  Image.ObjCMethods.push_back(LocalSave);
  ObjCMethod Caller;
  Caller.ClassName = LocalSave.ClassName;
  Caller.Selector = "migrate:context:";
  Caller.Implementation = 0x1200;
  Caller.TypeHint = parseObjCMethodEncoding(Caller.Selector, "v32@0:8@16@24");
  ASSERT_TRUE(Caller.TypeHint);
  Image.ObjCMethods.push_back(Caller);
  ObjCClass Class;
  Class.Name = LocalSave.ClassName;
  Class.RootClass = true;
  Class.InheritanceStatus = "root";
  Image.ObjCClasses.push_back(std::move(Class));

  LowFunc Function;
  Function.Entry = Caller.Implementation;
  Function.Name = "receiver_spill_caller";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  Block.EndAddr = 0x1234;
  Block.Ops = {
      operation(NdOp::INT_SUB, NdVar::reg(a64reg::SP, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(64, 4)}, 0x1200),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X20, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(32, 4)}, 0x1204),
      operation(NdOp::STORE, {},
                {NdVar::reg(a64reg::X20, 8), NdVar::reg(a64reg::X0, 8)},
                0x1208),
      operation(NdOp::COPY, NdVar::reg(a64reg::X20, 8), {NdVar::cst(0, 8)},
                0x120c),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0x1300, 8)},
                0x1210),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X20, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(32, 4)}, 0x1214),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X21, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(48, 4)}, 0x1218),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X22, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(56, 4)}, 0x121c),
      operation(NdOp::STORE, {},
                {NdVar::reg(a64reg::X22, 8), NdVar::reg(a64reg::X21, 8)},
                0x1220),
      operation(NdOp::LOAD, NdVar::reg(a64reg::X0, 8),
                {NdVar::reg(a64reg::X20, 8)}, 0x1224),
      operation(NdOp::COPY, NdVar::reg(a64reg::X2, 8),
                {NdVar::reg(a64reg::X3, 8)}, 0x1228),
      operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0x1100, 8)},
                0x122c),
      operation(NdOp::RETURN, {}, {}, 0x1230)};
  Function.Blocks.push_back(Block);

  auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x122c);
  ASSERT_TRUE(Hint.Receiver);
  EXPECT_EQ(Hint.Receiver->ClassName, LocalSave.ClassName);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);

  Block.Ops[6] =
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X21, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(16, 4)}, 0x1218);
  Function.Blocks[0] = Block;
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
}

TEST(ObjCCallHints,
     FloatingResultUseSurvivesThePreservedHalfOfAnAArch64Vector) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "duration";
  ObjCSourceReference TouchReference;
  TouchReference.Address = 0x2110;
  TouchReference.Name = "touch";
  Image.ObjCSourceReferences[TouchReference.Address] = TouchReference;
  ObjCMethod Touch;
  Touch.ClassName = "Worker";
  Touch.Selector = TouchReference.Name;
  Touch.TypeHint = parseObjCMethodEncoding(Touch.Selector, "v16@0:8");
  ASSERT_TRUE(Touch.TypeHint);
  Image.ObjCMethods.push_back(Touch);
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/CoreSpotlight.framework/CoreSpotlight",
      "/System/Library/Frameworks/QuartzCore.framework/QuartzCore"};
  ASSERT_FALSE(objcSelectorSourceTypeHint(Image, "duration"));

  Image.ImportPtrSlots[0x2190] = "_objc_release";
  const uint32_t ReleaseStub[] = {0xb0000010, 0xf940ca10, 0xd61f0200};
  for (size_t I = 0; I < 3; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x140 + I * 4, ReleaseStub[I]);
  // ADRP x1,0x2000; LDR x1,[x1,#0x110]; ADRP x16,0x2000;
  // LDR x16,[x16,#0x180]; BR x16.
  const uint32_t TouchStub[] = {
      0xb0000001, 0xf9408821, 0xb0000010, 0xf940c210, 0xd61f0200};
  for (size_t I = 0; I < 5; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x160 + I * 4, TouchStub[I]);

  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  auto Function = caller();
  Function.Blocks[0].EndAddr = 0x1218;
  Function.Blocks[0].Ops = {
      operation(NdOp::CALL, NdVar::reg(TRI.IntReturnReg, 8),
                {NdVar::cst(0x1100, 8)}, 0x1200),
      operation(NdOp::COPY, NdVar::reg(a64reg::V(8), 16),
                {NdVar::reg(TRI.FPReturnReg, 16)}, 0x1204),
      operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                {NdVar::cst(0, 8)}, 0x1208),
      operation(NdOp::CALL, NdVar::reg(TRI.IntReturnReg, 8),
                {NdVar::cst(0x1140, 8)}, 0x120c),
      operation(NdOp::COPY, NdVar::reg(TRI.FPReturnReg, 16),
                {NdVar::reg(a64reg::V(8), 16)}, 0x1210),
      operation(NdOp::RETURN, {}, {NdVar::reg(TRI.FPReturnReg, 8)}, 0x1214)};

  auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_TRUE(Hints.count(0x1200));
  const auto &Hint = Hints.at(0x1200);
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 8U);
  ASSERT_TRUE(Hint.SelectorResultUse);
  EXPECT_EQ(Hint.SelectorResultUse->Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint.SelectorResultUse->RegisterOffset, TRI.FPReturnReg);
  EXPECT_EQ(Hint.SelectorResultUse->ValueBytes, 8U);

  const auto Med = convert(Image, Function);
  const MedOp *BoundCall = nullptr;
  const MedOp *WideResult = nullptr;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.Addr == 0x1200 && Op.SourceCallHint)
        BoundCall = &Op;
      if (Op.Addr == 0x1200 && Op.Opcode == NdOp::CONCAT &&
          Op.Output.Kind == MedVar::Reg &&
          Op.Output.RegOff == TRI.FPReturnReg && Op.Output.Size == 16)
        WideResult = &Op;
    }
  ASSERT_NE(BoundCall, nullptr);
  ASSERT_EQ(BoundCall->Output.Kind, MedVar::Reg);
  EXPECT_EQ(BoundCall->Output.RegOff, TRI.FPReturnReg);
  EXPECT_EQ(BoundCall->Output.Size, 8U);
  ASSERT_NE(WideResult, nullptr);
  ASSERT_EQ(WideResult->NumInputs, 2U);
  EXPECT_EQ(WideResult->Inputs[1].Kind, BoundCall->Output.Kind);
  EXPECT_EQ(WideResult->Inputs[1].Id, BoundCall->Output.Id);
  EXPECT_EQ(WideResult->Inputs[1].RegOff, BoundCall->Output.RegOff);
  EXPECT_EQ(WideResult->Inputs[1].Size, BoundCall->Output.Size);
  EXPECT_GT(WideResult->Inputs[1].SSAVer, 0);

  // A fixed Objective-C declaration authenticates the intervening call's
  // ordinary ABI just like a catalogued runtime call. The low half of v8 is
  // call-preserved, so the later d0 use still selects the floating result.
  Function.Blocks[0].Ops[3].Inputs[0] = NdVar::cst(0x1160, 8);
  Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_TRUE(Hints.count(0x1200));
  ASSERT_TRUE(Hints.at(0x1200).SelectorResultUse);
  EXPECT_EQ(Hints.at(0x1200).SelectorResultUse->Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hints.at(0x1200).SelectorResultUse->ValueBytes, 8U);

  ObjCMethod ConflictingTouch = Touch;
  ConflictingTouch.ClassName = "OtherWorker";
  ConflictingTouch.TypeHint =
      parseObjCMethodEncoding(ConflictingTouch.Selector, "q16@0:8");
  ASSERT_TRUE(ConflictingTouch.TypeHint);
  Image.ObjCMethods.push_back(ConflictingTouch);
  EXPECT_FALSE(buildObjCSourceCallHints(Image, Function).count(0x1200));
  Image.ObjCMethods.pop_back();

  Function.Blocks[0].Ops[1].Output = NdVar::reg(a64reg::V(8), 4);
  Function.Blocks[0].Ops[1].Inputs[0] = NdVar::reg(TRI.FPReturnReg, 4);
  Function.Blocks[0].Ops[4].Output = NdVar::reg(TRI.FPReturnReg, 4);
  Function.Blocks[0].Ops[4].Inputs[0] = NdVar::reg(a64reg::V(8), 4);
  Function.Blocks[0].Ops.back().Inputs[0] = NdVar::reg(TRI.FPReturnReg, 4);
  EXPECT_FALSE(buildObjCSourceCallHints(Image, Function).count(0x1200));
}

TEST(ObjCCallHints, FloatingResultUseFollowsSavedVectorAcrossCFGEdge) {
  auto Image = image();
  Image.ObjCSourceReferences.at(0x2100).Name = "duration";
  Image.ObjCMethods[0].Selector = "duration";
  Image.ObjCMethods[0].TypeHint =
      parseObjCMethodEncoding("duration", "d16@0:8");
  ASSERT_TRUE(Image.ObjCMethods[0].TypeHint);
  ObjCMethod IntegerDuration = Image.ObjCMethods[0];
  IntegerDuration.ClassName = "Other";
  IntegerDuration.TypeHint =
      parseObjCMethodEncoding("duration", "q16@0:8");
  ASSERT_TRUE(IntegerDuration.TypeHint);
  Image.ObjCMethods.push_back(std::move(IntegerDuration));
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "duration"));

  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  LowFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "cross_block_floating_result";
  LowBlock Call;
  Call.Id = 0;
  Call.StartAddr = 0x1200;
  Call.EndAddr = 0x1208;
  Call.Succs = {1};
  Call.Ops = {
      operation(NdOp::CALL, NdVar::reg(TRI.IntReturnReg, 8),
                {NdVar::cst(0x1100, 8)}, 0x1200),
      operation(NdOp::COPY, NdVar::reg(a64reg::V(8), 16),
                {NdVar::reg(TRI.FPReturnReg, 16)}, 0x1204)};
  LowBlock Return;
  Return.Id = 1;
  Return.StartAddr = 0x1208;
  Return.EndAddr = 0x1210;
  Return.Preds = {0};
  Return.Ops = {
      operation(NdOp::COPY, NdVar::reg(TRI.FPReturnReg, 16),
                {NdVar::reg(a64reg::V(8), 16)}, 0x1208),
      operation(NdOp::RETURN, {}, {NdVar::reg(TRI.FPReturnReg, 8)}, 0x120c)};
  Function.Blocks = {std::move(Call), std::move(Return)};

  const auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x1200);
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 8U);
  ASSERT_TRUE(Hint.SelectorResultUse);
  EXPECT_EQ(Hint.SelectorResultUse->Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint.SelectorResultUse->ValueBytes, 8U);

  // Conflicting reachable uses in the integer and floating return banks do
  // not select either of the ambiguous declarations.
  Function.Blocks[0].Succs.push_back(2);
  Function.Blocks[0].Ops.push_back(
      operation(NdOp::COPY, NdVar::reg(a64reg::X19, 8),
                {NdVar::reg(TRI.IntReturnReg, 8)}, 0x1206));
  LowBlock IntegerReturn;
  IntegerReturn.Id = 2;
  IntegerReturn.StartAddr = 0x1210;
  IntegerReturn.EndAddr = 0x1214;
  IntegerReturn.Preds = {0};
  IntegerReturn.Ops = {operation(
      NdOp::RETURN, {}, {NdVar::reg(a64reg::X19, 8)}, 0x1210)};
  Function.Blocks.push_back(std::move(IntegerReturn));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
}

TEST(ObjCCallHints, FrameworkVariadicAndUnsupportedRecordsRemainUnbound) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  for (llvm::StringRef Selector :
       {"stringWithFormat:", "decimalValue", "neverdUnknownSelector:"}) {
    SCOPED_TRACE(Selector.str());
    Image.ObjCSourceReferences.at(0x2100).Name = Selector.str();
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  }
  // Runtime encodings omit an ellipsis. A matching fixed prefix cannot
  // override the compiler's explicit variadic declaration.
  Image.ObjCSourceReferences.at(0x2100).Name = "stringWithFormat:";
  ObjCMethod Prefix;
  Prefix.Selector = "stringWithFormat:";
  Prefix.TypeHint = signature(Image.Arch);
  Image.ObjCMethods.push_back(Prefix);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    Image.Arch = A;
    const auto Range = objcSelectorSourceTypeHint(Image, "rangeOfString:");
    ASSERT_TRUE(Range);
    ASSERT_EQ(Range->ReturnType->Kind, NdTypeKind::Struct);
    ASSERT_EQ(Range->ReturnComponents.size(), 2U);
    const auto &TRI = getTargetRegInfo(A);
    for (unsigned I = 0; I < 2; ++I) {
      EXPECT_EQ(Range->ReturnComponents[I].Kind,
                SourceABICarrierKind::IntegerRegister);
      EXPECT_EQ(Range->ReturnComponents[I].RegisterOffset,
                TRI.IntReturnRegs[I]);
    }
  }
}

TEST(ObjCCallHints, FrameworkABIIsArchitectureSpecificAndRevalidated) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.ObjCMethods.clear();
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    const auto Hint = objcSelectorSourceTypeHint(Image, "length");
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::ObjCSDK);
    EXPECT_EQ(Hint->Architecture, Architecture);
    ASSERT_EQ(Hint->ReturnType->Size, 8U);
    EXPECT_FALSE(Hint->ReturnType->IsSigned);
    const auto Boolean = objcSelectorSourceTypeHint(Image, "isEqualToString:");
    if (Architecture == Arch::AArch64) {
      ASSERT_TRUE(Boolean);
      EXPECT_EQ(Boolean->ReturnType->Size, 1U);
      EXPECT_FALSE(Boolean->ReturnType->IsSigned);
      EXPECT_TRUE(Boolean->ReturnLocation.ExtendTo32Bits);
    } else {
      // macOS x86-64 uses signed char, while iOS uses bool. No platform
      // identity is asserted by this catalog's common Darwin ABI facts.
      EXPECT_FALSE(Boolean);
    }
    auto Call = HighExpr::makeCall("objc_msgSend", 0, {});
    SourceCallTypeHint Binding;
    Binding.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Binding.TargetName = "objc_msgSend";
    Binding.Selector = "length";
    Binding.Signature = *Hint;
    auto Evidence = std::make_shared<SourceCallTypeHint>(Binding);
    Call->SourceCallHint = Evidence;
    Call->Type = Hint->ReturnType;
    for (const auto &Parameter : Hint->Parameters)
      Call->Operands.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    Evidence->Signature.ReturnType = NdType::makeInt(8);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
    *Evidence = Binding;
    Image.DynInfo.NeededLibs.clear();
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
    Image.Format = BinaryFormat::ELF;
    EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "length"));
  }
}

TEST(ObjCCallHints, BoundCallsPreserveOnlyABIProvenRuntimeReferenceRegisters) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      auto Image = image(Architecture);
      Image.ImportPtrSlots[0x2180] = "_objc_retain";
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto Register =
          Mutation == 1 ? TRI.IntParamRegs.front() : TRI.CalleeSaveRegs.front();
      LowFunc F;
      F.Entry = 0x1200;
      LowBlock B;
      B.StartAddr = 0x1200;
      B.Ops = {
          operation(NdOp::LOAD, NdVar::reg(Register, 8),
                    {NdVar::cst(0x2180, 8)}, 0x1200),
          operation(NdOp::INDIR_CALL, {}, {NdVar::reg(Register, 8)}, 0x1204)};
      if (Mutation == 2)
        B.Ops.push_back(operation(NdOp::COPY, NdVar::reg(Register, 4),
                                  {NdVar::cst(0, 4)}, 0x1208));
      if (Mutation == 3)
        B.Ops.push_back(
            operation(NdOp::CALL, {}, {NdVar::cst(0x1900, 8)}, 0x1208));
      B.Ops.push_back(
          operation(NdOp::INDIR_CALL, {}, {NdVar::reg(Register, 8)}, 0x120c));
      F.Blocks.push_back(std::move(B));
      const auto Hints = buildObjCSourceCallHints(Image, F);
      EXPECT_TRUE(Hints.count(0x1204));
      EXPECT_EQ(Hints.count(0x120c), Mutation == 0)
          << static_cast<int>(Architecture) << ':' << Mutation;
    }
  }
}

namespace {
BinaryImage runtimeImage(llvm::StringRef Name,
                         Arch Architecture = Arch::AArch64) {
  auto Image = image(Architecture);
  Image.ImportPtrSlots[0x2180] = Name.str();
  auto *Bytes = Image.Segments[0].Data.data() + 0x100;
  if (Architecture == Arch::AArch64) {
    const uint32_t Stub[] = {0xb0000010, 0xf940c210, 0xd61f0200};
    for (size_t I = 0; I < 3; ++I)
      llvm::support::endian::write32le(Bytes + I * 4, Stub[I]);
  } else {
    Bytes[0] = 0xff;
    Bytes[1] = 0x25;
    llvm::support::endian::write32le(Bytes + 2, 0x2180 - 0x1106);
  }
  return Image;
}
} // namespace

TEST(ObjCCallHints, FoundationNSNotFoundGetterRequiresExactStrongImport) {
  constexpr llvm::StringLiteral Name = "$s10Foundation10NSNotFoundSivg";
  constexpr llvm::StringLiteral Provider =
      "/System/Library/Frameworks/Foundation.framework/Foundation";
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("_" + Name.str(), Architecture);
    Image.DyldBindSlots[0x2180] = {"_" + Name.str(), 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->TargetName, Name.str());
    EXPECT_EQ(Hint->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::SwiftSDK);
    ASSERT_TRUE(Hint->Signature.ReturnType);
    EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Hint->Signature.ReturnType->Size, 8U);
    EXPECT_TRUE(Hint->Signature.Parameters.empty());
    auto Call = HighExpr::makeCall(Name.str(), 0x2180, {});
    Call->Type = Hint->Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    Image.DyldBindSlots[0x2180].Module = "/tmp/foreign.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
    Image.DyldBindSlots[0x2180].Module = Provider.str();
    Image.DyldBindSlots[0x2180].WeakImport = true;
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
  }
}

TEST(ObjCCallHints, SwiftStringRangeSubscriptKeepsFourWordResultABI) {
  constexpr llvm::StringLiteral Name = "$sSSySsSnySS5IndexVGcig";
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  auto Image = runtimeImage("_" + Name.str());
  Image.DyldBindSlots[0x2180] = {"_" + Name.str(), 0, Provider.str(), false};
  const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
  EXPECT_EQ(Hint->TargetName, Name);
  EXPECT_EQ(Hint->SwiftStringInputs,
            (std::vector<std::pair<unsigned, unsigned>>{{2, 3}}));
  const auto &ABI = Hint->Signature;
  EXPECT_EQ(ABI.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
  EXPECT_EQ(ABI.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  ASSERT_TRUE(ABI.ReturnType);
  EXPECT_EQ(ABI.ReturnType->Kind, NdTypeKind::Struct);
  EXPECT_EQ(ABI.ReturnType->Size, 32U);
  ASSERT_EQ(ABI.ReturnType->Fields.size(), 4U);
  ASSERT_EQ(ABI.ReturnComponents.size(), 4U);
  ASSERT_EQ(ABI.Parameters.size(), 4U);
  for (size_t I = 0; I < 4; ++I) {
    EXPECT_EQ(ABI.ReturnComponents[I].Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(ABI.ReturnComponents[I].RegisterOffset, I * 8);
    EXPECT_EQ(ABI.Parameters[I].Location.Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(ABI.Parameters[I].Location.RegisterOffset,
              getTargetRegInfo(Arch::AArch64).IntParamRegs[I]);
  }
  EXPECT_EQ(ABI.ReturnType->Fields[3]->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(ABI.Parameters[3].Type->Kind, NdTypeKind::Ptr);
  std::string Diagnostic;
  EXPECT_TRUE(validateSourceABI(ABI, Diagnostic)) << Diagnostic;
  std::vector<ExprPtr> Arguments;
  for (const auto &Parameter : ABI.Parameters) {
    auto Argument = HighExpr::makeConst(0, Parameter.Type->Size);
    Argument->Type = Parameter.Type;
    Arguments.push_back(std::move(Argument));
  }
  auto Call = HighExpr::makeCall(Name.str(), 0x2180, std::move(Arguments));
  Call->Type = ABI.ReturnType;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
  HighFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "slice_string";
  Function.ReturnType = ABI.ReturnType;
  Function.SourceTypeHint = ABI;
  for (size_t I = 0; I < ABI.Parameters.size(); ++I)
    Function.Params.push_back({ABI.Parameters[I].Name,
                               ABI.Parameters[I].Type});
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = Call;
  Function.Body = {Return};
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  EXPECT_TRUE(HighCEmitter().emit({Function}, OS, Options));
  EXPECT_NE(Source.find("slice_string"), std::string::npos);
  EXPECT_NE(Source.find("swiftcall"), std::string::npos);
  Image.DyldBindSlots[0x2180].Module = "/tmp/foreign.dylib";
  EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
  EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
  Image.DyldBindSlots[0x2180].Module = Provider.str();
  Image.DyldBindSlots[0x2180].WeakImport = true;
  EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
  auto X64 = runtimeImage("_" + Name.str(), Arch::X64);
  X64.DyldBindSlots[0x2180] = {"_" + Name.str(), 0, Provider.str(), false};
  EXPECT_FALSE(swiftRuntimeSourceCallHint(X64, 0x2180));
}

TEST(ObjCCallHints, SwiftAllocErrorKeepsObjectAndPayloadResults) {
  auto Image = runtimeImage("_swift_allocError");
  Image.DyldBindSlots[0x2180] = {
      "_swift_allocError", 0, "/usr/lib/swift/libswiftCore.dylib", false};
  const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
  const auto &ABI = Hint->Signature;
  EXPECT_EQ(ABI.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
  EXPECT_EQ(ABI.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  ASSERT_TRUE(ABI.ReturnType);
  EXPECT_EQ(ABI.ReturnType->Kind, NdTypeKind::Struct);
  ASSERT_EQ(ABI.ReturnType->Fields.size(), 2U);
  ASSERT_EQ(ABI.ReturnComponents.size(), 2U);
  ASSERT_EQ(ABI.Parameters.size(), 4U);
  for (size_t I = 0; I < 2; ++I) {
    EXPECT_EQ(ABI.ReturnType->Fields[I]->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(ABI.ReturnComponents[I].Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(ABI.ReturnComponents[I].RegisterOffset,
              getTargetRegInfo(Arch::AArch64).IntReturnReg + I * 8);
  }
  for (size_t I = 0; I < 4; ++I) {
    EXPECT_EQ(ABI.Parameters[I].Location.Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(ABI.Parameters[I].Location.RegisterOffset,
              getTargetRegInfo(Arch::AArch64).IntParamRegs[I]);
  }
  EXPECT_EQ(ABI.Parameters[3].Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(ABI.Parameters[3].Type->Size, 1U);
  std::string Diagnostic;
  EXPECT_TRUE(validateSourceABI(ABI, Diagnostic)) << Diagnostic;
  Image.DyldBindSlots[0x2180].Module = "/tmp/foreign.dylib";
  EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
  Image.DyldBindSlots[0x2180].Module = "/usr/lib/swift/libswiftCore.dylib";
  Image.DyldBindSlots[0x2180].Name += "_suffix";
  EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
}

TEST(ObjCCallHints, SwiftGenericSinglePayloadEnumKeepsCallbackABI) {
  constexpr llvm::StringLiteral Provider =
      "/usr/lib/swift/libswiftCore.dylib";
  for (const char *Name : {"swift_getEnumTagSinglePayloadGeneric",
                           "swift_storeEnumTagSinglePayloadGeneric"}) {
    const bool IsGet = llvm::StringRef(Name).starts_with("swift_get");
    const std::string Import = std::string("_") + Name;
    auto Image = runtimeImage(Import);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint) << Name;
    const auto &ABI = Hint->Signature;
    EXPECT_EQ(ABI.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
    EXPECT_EQ(ABI.ReturnType->Kind,
              IsGet ? NdTypeKind::Int : NdTypeKind::Void);
    EXPECT_EQ(ABI.ReturnLocation.Kind,
              IsGet ? SourceABICarrierKind::IntegerRegister
                    : SourceABICarrierKind::None);
    if (IsGet)
      EXPECT_EQ(ABI.ReturnType->Size, 4U);
    ASSERT_EQ(ABI.Parameters.size(), IsGet ? 4U : 5U);
    for (size_t I = 0; I < ABI.Parameters.size(); ++I) {
      const bool IsCase = I == 1 || (!IsGet && I == 2);
      EXPECT_EQ(ABI.Parameters[I].Type->Kind,
                IsCase ? NdTypeKind::Int : NdTypeKind::Ptr);
      EXPECT_EQ(ABI.Parameters[I].Type->Size, IsCase ? 4U : 8U);
      EXPECT_EQ(ABI.Parameters[I].Location.Kind,
                SourceABICarrierKind::IntegerRegister);
      EXPECT_EQ(ABI.Parameters[I].Location.RegisterOffset,
                getTargetRegInfo(Arch::AArch64).IntParamRegs[I]);
    }
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(ABI, Diagnostic)) << Diagnostic;
    Image.DyldBindSlots[0x2180].Module = "/tmp/foreign.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    Image.DyldBindSlots[0x2180].Module = Provider.str();
    Image.DyldBindSlots[0x2180].WeakImport = true;
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    auto X64 = runtimeImage(Import, Arch::X64);
    X64.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto X64Hint = swiftRuntimeSourceCallHint(X64, 0x2180);
    ASSERT_TRUE(X64Hint);
    EXPECT_TRUE(validateSourceABI(X64Hint->Signature, Diagnostic))
        << Diagnostic;
  }
}

TEST(ObjCCallHints, SwiftClassMetadataDependencyKeepsTwoWordSwiftResult) {
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  for (const char *Name :
       {"swift_initClassMetadata2", "swift_updateClassMetadata2"}) {
    const std::string Import = std::string("_") + Name;
    auto Image = runtimeImage(Import);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint) << Name;
    const auto &ABI = Hint->Signature;
    EXPECT_EQ(ABI.Origin, SourceFunctionTypeHint::OriginKind::SwiftRuntime);
    EXPECT_EQ(ABI.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_EQ(ABI.ReturnComponents.size(), 2U);
    ASSERT_EQ(ABI.ReturnType->Fields.size(), 2U);
    EXPECT_EQ(ABI.ReturnType->Fields[0]->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(ABI.ReturnType->Fields[1]->Kind, NdTypeKind::Int);
    EXPECT_EQ(ABI.ReturnType->Fields[1]->Size, 8U);
    for (size_t I = 0; I < 2; ++I) {
      EXPECT_EQ(ABI.ReturnComponents[I].Kind,
                SourceABICarrierKind::IntegerRegister);
      EXPECT_EQ(ABI.ReturnComponents[I].RegisterOffset,
                getTargetRegInfo(Arch::AArch64).IntReturnReg + I * 8);
    }
    ASSERT_EQ(ABI.Parameters.size(), 5U);
    for (size_t I = 0; I < 5; ++I) {
      EXPECT_EQ(ABI.Parameters[I].Type->Kind,
                I == 1 || I == 2 ? NdTypeKind::Int : NdTypeKind::Ptr);
      EXPECT_EQ(ABI.Parameters[I].Type->Size, 8U);
      EXPECT_EQ(ABI.Parameters[I].Location.Kind,
                SourceABICarrierKind::IntegerRegister);
      EXPECT_EQ(ABI.Parameters[I].Location.RegisterOffset,
                getTargetRegInfo(Arch::AArch64).IntParamRegs[I]);
    }
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(ABI, Diagnostic)) << Diagnostic;
    Image.DyldBindSlots[0x2180].Module = "/tmp/foreign.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    Image.DyldBindSlots[0x2180].Module = Provider.str();
    Image.DyldBindSlots[0x2180].WeakImport = true;
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
  }
}

TEST(ObjCCallHints, SwiftOpaqueConformance2KeepsSignedDescriptorABI) {
  auto Image = runtimeImage("_swift_getOpaqueTypeConformance2");
  Image.DyldBindSlots[0x2180] = {"_swift_getOpaqueTypeConformance2", 0,
                                 "/usr/lib/swift/libswiftCore.dylib", false};
  const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(Hint);
  const auto &ABI = Hint->Signature;
  EXPECT_EQ(ABI.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
  EXPECT_EQ(ABI.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  ASSERT_TRUE(ABI.ReturnType);
  EXPECT_EQ(ABI.ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(ABI.ReturnLocation.Kind,
            SourceABICarrierKind::IntegerRegister);
  EXPECT_EQ(ABI.ReturnLocation.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).IntReturnReg);
  ASSERT_EQ(ABI.Parameters.size(), 3U);
  for (size_t I = 0; I < 3; ++I) {
    EXPECT_EQ(ABI.Parameters[I].Location.Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(ABI.Parameters[I].Location.RegisterOffset,
              getTargetRegInfo(Arch::AArch64).IntParamRegs[I]);
  }
  EXPECT_EQ(ABI.Parameters[0].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(ABI.Parameters[1].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(ABI.Parameters[2].Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(ABI.Parameters[2].Type->Size, 8U);
  std::string Diagnostic;
  EXPECT_TRUE(validateSourceABI(ABI, Diagnostic)) << Diagnostic;
  Image.DyldBindSlots[0x2180].Module = "/tmp/foreign.dylib";
  EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
  Image.DyldBindSlots[0x2180].Module = "/usr/lib/swift/libswiftCore.dylib";
  Image.DyldBindSlots[0x2180].Name += "_suffix";
  EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
  auto X64 = runtimeImage("_swift_getOpaqueTypeConformance2", Arch::X64);
  X64.DyldBindSlots[0x2180] = {"_swift_getOpaqueTypeConformance2", 0,
                               "/usr/lib/swift/libswiftCore.dylib", false};
  EXPECT_FALSE(swiftRuntimeSourceCallHint(X64, 0x2180));
}

TEST(ObjCCallHints, SwiftDefaultActorLifecycleKeepsSwiftConvention) {
  constexpr llvm::StringLiteral Provider =
      "/usr/lib/swift/libswift_Concurrency.dylib";
  for (const char *Name : {"swift_defaultActor_initialize",
                           "swift_defaultActor_destroy"}) {
    const std::string Import = std::string("_") + Name;
    auto Image = runtimeImage(Import);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint) << Name;
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
    EXPECT_EQ(Hint->TargetName, Name);
    const auto &ABI = Hint->Signature;
    EXPECT_EQ(ABI.Origin, SourceFunctionTypeHint::OriginKind::SwiftRuntime);
    EXPECT_EQ(ABI.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(ABI.ReturnType);
    EXPECT_EQ(ABI.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(ABI.Parameters.size(), 1U);
    EXPECT_EQ(ABI.Parameters[0].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(ABI.Parameters[0].Location.Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(ABI.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Arch::AArch64).IntParamRegs[0]);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(ABI, Diagnostic)) << Diagnostic;
    Image.DyldBindSlots[0x2180].Module = "/usr/lib/swift/libswiftCore.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    Image.DyldBindSlots[0x2180].Module = Provider.str();
    Image.DyldBindSlots[0x2180].WeakImport = true;
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    auto X64 = runtimeImage(Import, Arch::X64);
    X64.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    EXPECT_FALSE(swiftRuntimeSourceCallHint(X64, 0x2180));
  }
}

TEST(ObjCCallHints, SwiftTaskFrameAllocationKeepsSwiftConvention) {
  constexpr llvm::StringLiteral Provider =
      "/usr/lib/swift/libswift_Concurrency.dylib";
  for (const char *Name : {"swift_task_alloc", "swift_task_dealloc"}) {
    const bool Alloc = llvm::StringRef(Name) == "swift_task_alloc";
    const std::string Import = std::string("_") + Name;
    auto Image = runtimeImage(Import);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint) << Name;
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
    const auto &ABI = Hint->Signature;
    EXPECT_EQ(ABI.Origin, SourceFunctionTypeHint::OriginKind::SwiftRuntime);
    EXPECT_EQ(ABI.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(ABI.ReturnType);
    EXPECT_EQ(ABI.ReturnType->Kind,
              Alloc ? NdTypeKind::Ptr : NdTypeKind::Void);
    EXPECT_EQ(ABI.ReturnLocation.Kind,
              Alloc ? SourceABICarrierKind::IntegerRegister
                    : SourceABICarrierKind::None);
    ASSERT_EQ(ABI.Parameters.size(), 1U);
    EXPECT_EQ(ABI.Parameters[0].Type->Kind,
              Alloc ? NdTypeKind::Int : NdTypeKind::Ptr);
    EXPECT_EQ(ABI.Parameters[0].Type->Size, 8U);
    EXPECT_EQ(ABI.Parameters[0].Location.Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(ABI.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Arch::AArch64).IntParamRegs[0]);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(ABI, Diagnostic)) << Diagnostic;
    Image.DyldBindSlots[0x2180].Module = "/usr/lib/swift/libswiftCore.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    Image.DyldBindSlots[0x2180].Module = Provider.str();
    Image.DyldBindSlots[0x2180].WeakImport = true;
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    auto X64 = runtimeImage(Import, Arch::X64);
    X64.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    EXPECT_FALSE(swiftRuntimeSourceCallHint(X64, 0x2180));
  }
}

TEST(ObjCCallHints, FoundationValueBridgesKeepCompilerObservedSwiftABI) {
  enum class Shape {
    IndirectFromObjC,
    ContextToObjC,
    DataFromObjC,
    DataToObjC,
    ObjectToObject,
    GenericArray,
    GenericDictionary
  };
  struct Bridge {
    const char *Name;
    Shape TheShape;
  };
  const Bridge Bridges[] = {
      {"$s10Foundation10URLRequestV19_bridgeToObjectiveCSo12NSURLRequestCyF",
       Shape::ContextToObjC},
      {"$s10Foundation10URLRequestV36_"
       "unconditionallyBridgeFromObjectiveCyACSo12"
       "NSURLRequestCSgFZ",
       Shape::IndirectFromObjC},
      {"$s10Foundation12NotificationV19_"
       "bridgeToObjectiveCSo14NSNotificationCyF",
       Shape::ContextToObjC},
      {"$s10Foundation12NotificationV36_"
       "unconditionallyBridgeFromObjectiveCyACSo"
       "14NSNotificationCSgFZ",
       Shape::IndirectFromObjC},
      {"$s10Foundation13URLComponentsV19_bridgeToObjectiveCSo15NSURLComponentsC"
       "yF",
       Shape::ContextToObjC},
      {"$s10Foundation14DateComponentsV36_"
       "unconditionallyBridgeFromObjectiveCyACSo06NSDateC0CSgFZ",
       Shape::IndirectFromObjC},
      {"$s10Foundation22_convertErrorToNSErrorySo0E0Cs0C0_pF",
       Shape::ObjectToObject},
      {"$s10Foundation3URLV19_bridgeToObjectiveCSo5NSURLCyF",
       Shape::ContextToObjC},
      {"$s10Foundation3URLV36_"
       "unconditionallyBridgeFromObjectiveCyACSo5NSURLCSgFZ",
       Shape::IndirectFromObjC},
      {"$s10Foundation4DataV19_bridgeToObjectiveCSo6NSDataCyF",
       Shape::DataToObjC},
      {"$s10Foundation4DataV36_"
       "unconditionallyBridgeFromObjectiveCyACSo6NSDataCSgFZ",
       Shape::DataFromObjC},
      {"$s10Foundation4DateV19_bridgeToObjectiveCSo6NSDateCyF",
       Shape::ContextToObjC},
      {"$s10Foundation4DateV36_"
       "unconditionallyBridgeFromObjectiveCyACSo6NSDateCSgFZ",
       Shape::IndirectFromObjC},
      {"$s10Foundation6LocaleV19_bridgeToObjectiveCSo8NSLocaleCyF",
       Shape::ContextToObjC},
      {"$s10Foundation6LocaleV36_"
       "unconditionallyBridgeFromObjectiveCyACSo8NSLocaleCSgFZ",
       Shape::IndirectFromObjC},
      {"$s10Foundation9IndexPathV19_bridgeToObjectiveCSo07NSIndexC0CyF",
       Shape::ContextToObjC},
      {"$s10Foundation9IndexPathV36_"
       "unconditionallyBridgeFromObjectiveCyACSo07NS"
       "IndexC0CSgFZ",
       Shape::IndirectFromObjC},
      {"$sSD10FoundationE19_bridgeToObjectiveCSo12NSDictionaryCyF",
       Shape::GenericDictionary},
      {"$sSD10FoundationE36_unconditionallyBridgeFromObjectiveCySDyxq_"
       "GSo12NSDictionaryCSgFZ",
       Shape::GenericDictionary},
      {"$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF",
       Shape::GenericArray},
      {"$sSa10FoundationE36_unconditionallyBridgeFromObjectiveCySayxGSo7NSArray"
       "CSgFZ",
       Shape::GenericArray},
  };
  constexpr llvm::StringLiteral Provider =
      "/System/Library/Frameworks/Foundation.framework/Foundation";
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &Bridge : Bridges) {
      SCOPED_TRACE(std::string(Bridge.Name) + ":" +
                   std::to_string(static_cast<int>(Architecture)));
      const std::string Import = "_" + std::string(Bridge.Name);
      auto Image = runtimeImage(Import, Architecture);
      Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
      EXPECT_EQ(Hint->TargetName, Bridge.Name);
      const auto &Signature = Hint->Signature;
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
      const auto &TRI = getTargetRegInfo(Architecture);
      switch (Bridge.TheShape) {
      case Shape::IndirectFromObjC:
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
        ASSERT_EQ(Signature.Parameters.size(), 2U);
        EXPECT_EQ(Signature.Parameters[0].TheRole,
                  SourceParameterTypeHint::Role::SwiftIndirectResult);
        EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                  Architecture == Arch::AArch64 ? TRI.indirectResultReg()
                                                : TRI.IntReturnReg);
        EXPECT_EQ(Signature.Parameters[1].TheRole,
                  SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
                  TRI.IntParamRegs[0]);
        break;
      case Shape::ContextToObjC:
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
        ASSERT_EQ(Signature.Parameters.size(), 1U);
        EXPECT_EQ(Signature.Parameters[0].TheRole,
                  SourceParameterTypeHint::Role::SwiftContext);
        EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                  Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
        break;
      case Shape::DataFromObjC:
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Int);
        EXPECT_EQ(Signature.ReturnType->Size, 16U);
        ASSERT_EQ(Signature.ReturnComponents.size(), 2U);
        ASSERT_EQ(Signature.Parameters.size(), 1U);
        EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                  TRI.IntParamRegs[0]);
        break;
      case Shape::DataToObjC:
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
        ASSERT_EQ(Signature.Parameters.size(), 2U);
        EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                  TRI.IntParamRegs[0]);
        EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
                  TRI.IntParamRegs[1]);
        break;
      case Shape::ObjectToObject:
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
        ASSERT_EQ(Signature.Parameters.size(), 1U);
        EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Signature.Parameters[0].TheRole,
                  SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                  TRI.IntParamRegs[0]);
        break;
      case Shape::GenericArray:
      case Shape::GenericDictionary: {
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
        const size_t Count = Bridge.TheShape == Shape::GenericArray ? 2 : 4;
        ASSERT_EQ(Signature.Parameters.size(), Count);
        for (size_t I = 0; I < Count; ++I) {
          EXPECT_EQ(Signature.Parameters[I].Type->Kind, NdTypeKind::Ptr);
          EXPECT_EQ(Signature.Parameters[I].TheRole,
                    SourceParameterTypeHint::Role::Ordinary);
          EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                    TRI.IntParamRegs[I]);
        }
        break;
      }
      }

      std::vector<ExprPtr> Arguments;
      for (const auto &Parameter : Signature.Parameters)
        Arguments.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
      auto Call = HighExpr::makeCall("untrusted_name", 0x2180, Arguments);
      Call->Type = Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
      Changed->Signature.Parameters[0].TheRole =
          Changed->Signature.Parameters[0].TheRole ==
                  SourceParameterTypeHint::Role::Ordinary
              ? SourceParameterTypeHint::Role::SwiftContext
              : SourceParameterTypeHint::Role::Ordinary;
      Call->SourceCallHint = std::move(Changed);
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);

      HighFunc Function;
      Function.Name = "bridge_wrapper";
      Function.ReturnType = Signature.ReturnType;
      HighStmt Statement;
      if (Signature.ReturnType->Kind == NdTypeKind::Void) {
        Statement.Kind = StmtKind::Call;
        Statement.CallExpr = Call;
      } else {
        Statement.Kind = StmtKind::Return;
        Statement.RetVal = Call;
      }
      Function.Body = {Statement};
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
      EXPECT_NE(Source.find("__attribute__((swiftcall))"), std::string::npos)
          << Source;
      EXPECT_NE(Source.find("__asm__(\"" + Import + "\")"), std::string::npos)
          << Source;
      EXPECT_EQ(Source.find("__attribute__((swift_indirect_result))") !=
                    std::string::npos,
                Bridge.TheShape == Shape::IndirectFromObjC)
          << Source;
      EXPECT_EQ(Source.find("__attribute__((swift_context))") !=
                    std::string::npos,
                Bridge.TheShape == Shape::ContextToObjC)
          << Source;

      for (llvm::StringRef Alias :
           {"/System/Library/Frameworks/Foundation.framework/Versions/C/"
            "Foundation",
            "/usr/lib/swift/libswiftFoundation.dylib"}) {
        auto Aliased = Image;
        Aliased.DyldBindSlots[0x2180].Module = Alias.str();
        EXPECT_TRUE(swiftRuntimeSourceCallHint(Aliased, 0x2180)) << Alias.str();
      }
      for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
        auto Wrong = Image;
        if (Mutation == 0)
          Wrong.DyldBindSlots[0x2180].Module =
              "/tmp/Foundation.framework/Foundation";
        else if (Mutation == 1)
          Wrong.DyldBindSlots[0x2180].Addend = 1;
        else if (Mutation == 2)
          Wrong.DyldBindSlots[0x2180].WeakImport = true;
        else
          Wrong.DyldBindSlots.erase(0x2180);
        EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180)) << Mutation;
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
      }
    }
  }
}

TEST(ObjCCallHints, SwiftCocoaArrayEndIndexKeepsItsWordABI) {
  constexpr llvm::StringLiteral Name = "$ss18_CocoaArrayWrapperV8endIndexSivg";
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage(("_" + Name).str(), Architecture);
    Image.DyldBindSlots[0x2180] = {("_" + Name).str(), 0, Provider.str(),
                                   false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
    EXPECT_EQ(Hint->TargetName, Name);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.ReturnType->Size, 8U);
    ASSERT_EQ(Signature.Parameters.size(), 1U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.Parameters[0].Type->Size, 8U);
    EXPECT_EQ(Signature.Parameters[0].TheRole,
              SourceParameterTypeHint::Role::Ordinary);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);

    auto Call = HighExpr::makeCall("untrusted_name", 0x2180,
                                   {HighExpr::makeConst(1, 8)});
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    Image.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
  }
}

TEST(ObjCCallHints, FoundationNSKeyValueObservationInvalidateUsesSwiftSelf) {
  constexpr llvm::StringLiteral Name =
      "$s10Foundation21NSKeyValueObservationC10invalidateyyFTj";
  constexpr llvm::StringLiteral Provider =
      "/System/Library/Frameworks/Foundation.framework/Foundation";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const std::string Import = "_" + Name.str();
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
    EXPECT_EQ(Hint->TargetName, Name);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Signature.Parameters.size(), 1U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[0].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;

    auto Call =
        HighExpr::makeCall("untrusted", 0x2180, {HighExpr::makeConst(0, 8)});
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    for (llvm::StringRef Alias :
         {"/System/Library/Frameworks/Foundation.framework/Versions/C/"
          "Foundation",
          "/usr/lib/swift/libswiftFoundation.dylib"}) {
      auto Aliased = Image;
      Aliased.DyldBindSlots[0x2180].Module = Alias.str();
      EXPECT_TRUE(swiftRuntimeSourceCallHint(Aliased, 0x2180)) << Alias.str();
    }
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      auto Wrong = Image;
      if (Mutation == 0)
        Wrong.DyldBindSlots[0x2180].Module = "/tmp/Foundation";
      else if (Mutation == 1)
        Wrong.DyldBindSlots[0x2180].Addend = 1;
      else if (Mutation == 2)
        Wrong.DyldBindSlots[0x2180].WeakImport = true;
      else
        Wrong.DyldBindSlots.erase(0x2180);
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180)) << Mutation;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
    }
  }
}

TEST(ObjCCallHints, SwiftUIBindingWrappedValueSetterKeepsGenericABI) {
  constexpr llvm::StringLiteral Name = "$s7SwiftUI7BindingV12wrappedValuexvs";
  constexpr llvm::StringLiteral Provider =
      "/System/Library/Frameworks/SwiftUI.framework/SwiftUI";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const std::string Import = "_" + Name.str();
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
    EXPECT_EQ(Hint->TargetName, Name);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Signature.Parameters.size(), 3U);
    const auto &TRI = getTargetRegInfo(Architecture);
    for (size_t I = 0; I < 2; ++I) {
      EXPECT_EQ(Signature.Parameters[I].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Signature.Parameters[I].TheRole,
                SourceParameterTypeHint::Role::Ordinary);
      EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                TRI.IntParamRegs[I]);
    }
    EXPECT_EQ(Signature.Parameters[2].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[2].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_EQ(Signature.Parameters[2].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;

    auto Call = HighExpr::makeCall("untrusted", 0x2180,
                                   {HighExpr::makeConst(0, 8),
                                    HighExpr::makeConst(0, 8),
                                    HighExpr::makeConst(0, 8)});
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      auto Wrong = Image;
      if (Mutation == 0)
        Wrong.DyldBindSlots[0x2180].Module = "/tmp/SwiftUI.framework/SwiftUI";
      else if (Mutation == 1)
        Wrong.DyldBindSlots[0x2180].Addend = 1;
      else if (Mutation == 2)
        Wrong.DyldBindSlots[0x2180].WeakImport = true;
      else
        Wrong.DyldBindSlots.erase(0x2180);
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180)) << Mutation;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
    }
  }
}

TEST(ObjCCallHints, SwiftStringAppendKeepsTwoWordPayloadAndSwiftSelf) {
  constexpr llvm::StringLiteral Name = "$sSS6appendyySSF";
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const std::string Import = "_" + Name.str();
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
    EXPECT_EQ(Hint->TargetName, Name);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Signature.Parameters.size(), 3U);
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.Parameters[0].Type->Size, 8U);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
              TRI.IntParamRegs[0]);
    EXPECT_EQ(Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
              TRI.IntParamRegs[1]);
    EXPECT_EQ(Signature.Parameters[2].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[2].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_EQ(Signature.Parameters[2].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;

    auto Call = HighExpr::makeCall("untrusted", 0x2180,
                                   {HighExpr::makeConst(0, 8),
                                    HighExpr::makeConst(0, 8),
                                    HighExpr::makeConst(0, 8)});
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    auto Wrong = Image;
    Wrong.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {}));
    Wrong = Image;
    Wrong.DyldBindSlots[0x2180].WeakImport = true;
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {}));
  }
}

TEST(ObjCCallHints, SwiftLocalizationSDKCallsUseCompilerObservedABI) {
  struct Case {
    const char *Name;
    const char *Provider;
    NdTypeKind ReturnKind;
    unsigned ReturnSize;
    std::vector<NdTypeKind> ParameterKinds;
  };
  const Case Cases[] = {
      {"$s10Foundation6LocaleV18preferredLanguagesSaySSGvgZ",
       "/System/Library/Frameworks/Foundation.framework/Foundation",
       NdTypeKind::Ptr, 8, {}},
      {"$sSS10lowercasedSSyF", "/usr/lib/swift/libswiftCore.dylib",
       NdTypeKind::Int, 16, {NdTypeKind::Int, NdTypeKind::Ptr}},
      {"$sSS5index5afterSS5IndexVAD_tF",
       "/usr/lib/swift/libswiftCore.dylib", NdTypeKind::Int, 8,
       {NdTypeKind::Int, NdTypeKind::Int, NdTypeKind::Ptr}},
      {"$sSSySJSS5IndexVcig", "/usr/lib/swift/libswiftCore.dylib",
       NdTypeKind::Int, 16,
       {NdTypeKind::Int, NdTypeKind::Int, NdTypeKind::Ptr}},
      {"$sSo7UIImageC5UIKitE24imageLiteralResourceNameABSS_tcfC",
       "/System/Library/Frameworks/UIKit.framework/UIKit", NdTypeKind::Ptr,
       8, {NdTypeKind::Int, NdTypeKind::Ptr}},
  };
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &Case : Cases) {
      const std::string Import = std::string("_") + Case.Name;
      auto Image = runtimeImage(Import, Architecture);
      Image.DyldBindSlots[0x2180] = {Import, 0, Case.Provider, false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint) << Case.Name;
      const auto &Signature = Hint->Signature;
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      ASSERT_TRUE(Signature.ReturnType);
      EXPECT_EQ(Signature.ReturnType->Kind, Case.ReturnKind);
      EXPECT_EQ(Signature.ReturnType->Size, Case.ReturnSize);
      ASSERT_EQ(Signature.Parameters.size(), Case.ParameterKinds.size());
      for (size_t I = 0; I < Case.ParameterKinds.size(); ++I) {
        EXPECT_EQ(Signature.Parameters[I].Type->Kind, Case.ParameterKinds[I]);
        EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[I]);
      }
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
      std::vector<ExprPtr> Args;
      for (size_t I = 0; I < Case.ParameterKinds.size(); ++I)
        Args.push_back(HighExpr::makeConst(0, 8));
      auto Call = HighExpr::makeCall("untrusted", 0x2180, std::move(Args));
      Call->Type = Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      auto Wrong = Image;
      Wrong.DyldBindSlots[0x2180].Module = "/tmp/untrusted.dylib";
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {}));
    }
  }
}

TEST(ObjCCallHints, SwiftStringGutsGrowKeepsCapacityAndSwiftSelfABI) {
  constexpr llvm::StringLiteral Name = "$ss11_StringGutsV4growyySiF";
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const std::string Import = "_" + Name.str();
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
    EXPECT_EQ(Hint->TargetName, Name);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Signature.Parameters.size(), 2U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.Parameters[0].Type->Size, 8U);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    EXPECT_EQ(Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[1].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;

    auto Call = HighExpr::makeCall(
        "untrusted", 0x2180,
        {HighExpr::makeConst(30, 8), HighExpr::makeConst(0, 8)});
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    auto Wrong = Image;
    Wrong.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {}));
    Wrong = Image;
    Wrong.DyldBindSlots[0x2180].WeakImport = true;
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {}));
  }
}

TEST(ObjCCallHints, FoundationNSNumberIntegerLiteralKeepsMetatypeABI) {
  constexpr llvm::StringLiteral Name =
      "$sSo8NSNumberC10FoundationE14integerLiteralABSi_tcfC";
  constexpr llvm::StringLiteral Provider =
      "/System/Library/Frameworks/Foundation.framework/Foundation";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const std::string Import = "_" + Name.str();
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
    EXPECT_EQ(Hint->TargetName, Name);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
    ASSERT_EQ(Signature.Parameters.size(), 2U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.Parameters[0].Type->Size, 8U);
    EXPECT_EQ(Signature.Parameters[0].TheRole,
              SourceParameterTypeHint::Role::Ordinary);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    EXPECT_EQ(Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[1].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;

    auto Call = HighExpr::makeCall(
        "untrusted", 0x2180,
        {HighExpr::makeConst(5000, 8), HighExpr::makeConst(0, 8)});
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    for (llvm::StringRef Alias :
         {"/System/Library/Frameworks/Foundation.framework/Versions/C/"
          "Foundation",
          "/usr/lib/swift/libswiftFoundation.dylib"}) {
      auto Aliased = Image;
      Aliased.DyldBindSlots[0x2180].Module = Alias.str();
      EXPECT_TRUE(swiftRuntimeSourceCallHint(Aliased, 0x2180)) << Alias.str();
    }
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      auto Wrong = Image;
      if (Mutation == 0)
        Wrong.DyldBindSlots[0x2180].Module = "/tmp/Foundation";
      else if (Mutation == 1)
        Wrong.DyldBindSlots[0x2180].Addend = 1;
      else if (Mutation == 2)
        Wrong.DyldBindSlots[0x2180].WeakImport = true;
      else
        Wrong.DyldBindSlots.erase(0x2180);
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180)) << Mutation;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
    }
  }
}

TEST(ObjCCallHints,
     FoundationStringContainsKeepsGenericWitnessAndSwiftSelfABI) {
  constexpr llvm::StringLiteral Name =
      "$sSy10FoundationE8containsySbqd__SyRd__lF";
  constexpr llvm::StringLiteral Provider =
      "/System/Library/Frameworks/Foundation.framework/Foundation";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const std::string Import = "_" + Name.str();
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
    EXPECT_EQ(Hint->TargetName, Name);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.ReturnType->Size, 1U);
    ASSERT_EQ(Signature.Parameters.size(), 6U);
    const auto &TRI = getTargetRegInfo(Architecture);
    for (size_t I = 0; I < 5; ++I) {
      EXPECT_EQ(Signature.Parameters[I].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Signature.Parameters[I].TheRole,
                SourceParameterTypeHint::Role::Ordinary);
      EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                TRI.IntParamRegs[I]);
    }
    EXPECT_EQ(Signature.Parameters[5].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[5].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_EQ(Signature.Parameters[5].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;

    std::vector<ExprPtr> Arguments;
    for (const auto &Parameter : Signature.Parameters)
      Arguments.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
    auto Call = HighExpr::makeCall("untrusted", 0x2180, Arguments);
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    for (llvm::StringRef Alias :
         {"/System/Library/Frameworks/Foundation.framework/Versions/C/"
          "Foundation",
          "/usr/lib/swift/libswiftFoundation.dylib"}) {
      auto Aliased = Image;
      Aliased.DyldBindSlots[0x2180].Module = Alias.str();
      EXPECT_TRUE(swiftRuntimeSourceCallHint(Aliased, 0x2180)) << Alias.str();
    }
    auto Wrong = Image;
    Wrong.DyldBindSlots[0x2180].Module = "/tmp/Foundation";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
  }
}

TEST(ObjCCallHints, FoundationURLPathAndStringComparisonKeepSwiftABI) {
  constexpr llvm::StringLiteral Provider =
      "/System/Library/Frameworks/Foundation.framework/Foundation";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    for (const auto &[Name, OrdinaryCount, ReturnBytes] :
         {std::tuple{"$s10Foundation3URLV13pathExtensionSSvg", 0U, 16U},
          std::tuple{"$sSy10FoundationE22caseInsensitiveCompareySo18NS"
                     "ComparisonResultVqd__SyRd__lF",
                     5U, 8U}}) {
      SCOPED_TRACE(Name);
      const std::string Import = "_" + std::string(Name);
      auto Image = runtimeImage(Import, Architecture);
      Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      const auto &Signature = Hint->Signature;
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      ASSERT_TRUE(Signature.ReturnType);
      EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(Signature.ReturnType->Size, ReturnBytes);
      ASSERT_EQ(Signature.Parameters.size(), OrdinaryCount + 1);
      for (unsigned I = 0; I < OrdinaryCount; ++I) {
        EXPECT_EQ(Signature.Parameters[I].Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Signature.Parameters[I].TheRole,
                  SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                  TRI.IntParamRegs[I]);
      }
      const auto &Context = Signature.Parameters.back();
      EXPECT_EQ(Context.Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Context.TheRole, SourceParameterTypeHint::Role::SwiftContext);
      EXPECT_EQ(Context.Location.RegisterOffset,
                Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
      if (ReturnBytes == 16) {
        ASSERT_EQ(Signature.ReturnComponents.size(), 2U);
        for (unsigned I = 0; I < 2; ++I)
          EXPECT_EQ(Signature.ReturnComponents[I].RegisterOffset,
                    TRI.IntReturnRegs[I]);
      } else {
        EXPECT_TRUE(Signature.ReturnComponents.empty());
        EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, TRI.IntReturnReg);
      }
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
      auto Wrong = Image;
      Wrong.DyldBindSlots[0x2180].Module = "/tmp/Foundation";
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
    }
  }
}

TEST(ObjCCallHints, SwiftStringHashIntoKeepsInoutAndStringCarriers) {
  constexpr llvm::StringLiteral Name = "$sSS4hash4intoys6HasherVz_tF";
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const std::string Import = "_" + Name.str();
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->TargetName, Name);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Signature.Parameters.size(), 3U);
    const auto &TRI = getTargetRegInfo(Architecture);
    for (unsigned I = 0; I < 3; ++I) {
      EXPECT_EQ(Signature.Parameters[I].Type->Kind,
                I == 1 ? NdTypeKind::Int : NdTypeKind::Ptr);
      EXPECT_EQ(Signature.Parameters[I].TheRole,
                SourceParameterTypeHint::Role::Ordinary);
      EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                TRI.IntParamRegs[I]);
    }
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
    auto Wrong = Image;
    Wrong.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
    Wrong = Image;
    Wrong.DyldBindSlots[0x2180].Addend = 1;
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
  }
}

TEST(ObjCCallHints, SwiftHasherSeedAndFinalizeKeepSpecialCarriers) {
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    for (const auto &Name : {"$ss6HasherV5_seedABSi_tcfC",
                             "$ss6HasherV9_finalizeSiyF"}) {
      SCOPED_TRACE(Name);
      const std::string Import = "_" + std::string(Name);
      auto Image = runtimeImage(Import, Architecture);
      Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      const auto &Signature = Hint->Signature;
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      const bool Seed = llvm::StringRef(Name).contains("_seed");
      EXPECT_EQ(Signature.ReturnType->Kind,
                Seed ? NdTypeKind::Void : NdTypeKind::Int);
      ASSERT_EQ(Signature.Parameters.size(), Seed ? 2U : 1U);
      if (Seed) {
        EXPECT_EQ(Signature.Parameters[0].TheRole,
                  SourceParameterTypeHint::Role::SwiftIndirectResult);
        EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                  Architecture == Arch::AArch64 ? TRI.indirectResultReg()
                                              : TRI.IntReturnReg);
        EXPECT_EQ(Signature.Parameters[1].TheRole,
                  SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Signature.Parameters[1].Type->Kind, NdTypeKind::Int);
        EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
                  TRI.IntParamRegs[0]);
      } else {
        EXPECT_EQ(Signature.ReturnType->Size, 8U);
        EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, TRI.IntReturnReg);
        EXPECT_EQ(Signature.Parameters[0].TheRole,
                  SourceParameterTypeHint::Role::SwiftContext);
        EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                  Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
      }
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
      auto Wrong = Image;
      Wrong.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
    }
  }
}

TEST(ObjCCallHints, SwiftDictionaryStorageAllocationKeepsMetadataContext) {
  constexpr llvm::StringLiteral Name =
      "$ss18_DictionaryStorageC8allocate8capacityAByxq_GSi_tFZ";
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const std::string Import = "_" + Name.str();
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.ReturnLocation.RegisterOffset,
              getTargetRegInfo(Architecture).IntReturnReg);
    ASSERT_EQ(Signature.Parameters.size(), 2U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.Parameters[0].TheRole,
              SourceParameterTypeHint::Role::Ordinary);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    EXPECT_EQ(Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[1].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
    auto Wrong = Image;
    Wrong.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
    Wrong = Image;
    Wrong.DyldBindSlots[0x2180].Addend = 1;
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
  }
}

TEST(ObjCCallHints, SwiftDictionaryHashableViolationNeverReturns) {
  constexpr llvm::StringLiteral Name = "$ss53KEY_TYPE_OF_DICTIONARY_VIOLATES_"
                                       "HASHABLE_REQUIREMENTSys5NeverOypXpF";
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const std::string Import = "_" + Name.str();
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_TRUE(Hint->DoesNotReturn);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Signature.Parameters.size(), 1U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[0].TheRole,
              SourceParameterTypeHint::Role::Ordinary);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
    auto Wrong = Image;
    Wrong.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
    Wrong = Image;
    Wrong.DyldBindSlots[0x2180].Addend = 1;
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
  }
}

TEST(ObjCCallHints, SwiftDictionaryStorageCopyAndResizeKeepScalarABI) {
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const auto &Name :
         {"$ss18_DictionaryStorageC4copy8originalAByxq_Gs05__RawaB0C_tFZ",
          "$ss18_DictionaryStorageC6resize8original8capacity4moveAByxq_Gs05__"
          "RawaB0C_SiSbtFZ"}) {
      SCOPED_TRACE(Name);
      const bool Resize = llvm::StringRef(Name).contains("resize");
      const std::string Import = "_" + std::string(Name);
      auto Image = runtimeImage(Import, Architecture);
      Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      EXPECT_FALSE(Hint->DoesNotReturn);
      const auto &Signature = Hint->Signature;
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      ASSERT_TRUE(Signature.ReturnType);
      EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Signature.ReturnLocation.RegisterOffset,
                getTargetRegInfo(Architecture).IntReturnReg);
      ASSERT_EQ(Signature.Parameters.size(), Resize ? 4U : 2U);
      EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                getTargetRegInfo(Architecture).IntParamRegs[0]);
      if (Resize) {
        EXPECT_EQ(Signature.Parameters[1].Type->Size, 8U);
        EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[1]);
        EXPECT_EQ(Signature.Parameters[2].Type->Size, 1U);
        EXPECT_EQ(Signature.Parameters[2].Location.ValueBytes, 1U);
        EXPECT_EQ(Signature.Parameters[2].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[2]);
      }
      const auto &Context = Signature.Parameters.back();
      EXPECT_EQ(Context.Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Context.TheRole, SourceParameterTypeHint::Role::SwiftContext);
      EXPECT_EQ(Context.Location.RegisterOffset,
                Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
      auto Wrong = Image;
      Wrong.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
      Wrong = Image;
      Wrong.DyldBindSlots[0x2180].Addend = 1;
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
    }
}

TEST(ObjCCallHints, FoundationURLAppendingPathKeepsIndirectResultABI) {
  constexpr llvm::StringLiteral Name =
      "$s10Foundation3URLV22appendingPathComponentyACSSF";
  constexpr llvm::StringLiteral Provider =
      "/System/Library/Frameworks/Foundation.framework/Foundation";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const std::string Import = "_" + Name.str();
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Signature.Parameters.size(), 4U);
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Signature.Parameters[0].TheRole,
              SourceParameterTypeHint::Role::SwiftIndirectResult);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? TRI.indirectResultReg()
                                            : TRI.IntReturnReg);
    EXPECT_EQ(Signature.Parameters[1].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
              TRI.IntParamRegs[0]);
    EXPECT_EQ(Signature.Parameters[2].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[2].Location.RegisterOffset,
              TRI.IntParamRegs[1]);
    EXPECT_EQ(Signature.Parameters[3].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_EQ(Signature.Parameters[3].Location.RegisterOffset,
              Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
    auto Wrong = Image;
    Wrong.DyldBindSlots[0x2180].Module = "/tmp/Foundation";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180));
  }
}

TEST(ObjCCallHints, StdlibAnyBridgesKeepCompilerObservedSwiftABI) {
  enum class Shape { IndirectAnyResult, GenericToObject };
  struct Bridge {
    const char *Name;
    Shape TheShape;
  };
  const Bridge Bridges[] = {
      {"$ss018_bridgeAnyObjectToB0yypyXlSgF", Shape::IndirectAnyResult},
      {"$ss27_bridgeAnythingToObjectiveCyyXlxlF", Shape::GenericToObject},
  };
  constexpr llvm::StringLiteral Provider =
      "/usr/lib/swift/libswiftCore.dylib";
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (const auto &Bridge : Bridges) {
      SCOPED_TRACE(std::string(Bridge.Name) + ":" +
                   std::to_string(static_cast<int>(Architecture)));
      const std::string Import = "_" + std::string(Bridge.Name);
      auto Image = runtimeImage(Import, Architecture);
      Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
      EXPECT_EQ(Hint->TargetName, Bridge.Name);
      const auto &Signature = Hint->Signature;
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
      const auto &TRI = getTargetRegInfo(Architecture);
      if (Bridge.TheShape == Shape::IndirectAnyResult) {
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
        ASSERT_EQ(Signature.Parameters.size(), 2U);
        EXPECT_EQ(Signature.Parameters[0].TheRole,
                  SourceParameterTypeHint::Role::SwiftIndirectResult);
        EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                  Architecture == Arch::AArch64 ? TRI.indirectResultReg()
                                                : TRI.IntReturnReg);
        EXPECT_EQ(Signature.Parameters[1].TheRole,
                  SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
                  TRI.IntParamRegs[0]);
      } else {
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
        ASSERT_EQ(Signature.Parameters.size(), 2U);
        EXPECT_EQ(Signature.Parameters[0].TheRole,
                  SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                  TRI.IntParamRegs[0]);
        EXPECT_EQ(Signature.Parameters[1].TheRole,
                  SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset,
                  TRI.IntParamRegs[1]);
      }

      for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
        auto Wrong = Image;
        if (Mutation == 0)
          Wrong.DyldBindSlots[0x2180].Module =
              "/tmp/libswiftCore.dylib";
        else if (Mutation == 1)
          Wrong.DyldBindSlots[0x2180].Addend = 1;
        else if (Mutation == 2)
          Wrong.DyldBindSlots[0x2180].WeakImport = true;
        else
          Wrong.DyldBindSlots.erase(0x2180);
        EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180)) << Mutation;
      }
    }
}

TEST(ObjCCallHints, DispatchSemaphoreMethodsKeepCompilerObservedSwiftABI) {
  struct Method {
    const char *Name;
    bool ReturnsWord;
  };
  const Method Methods[] = {
      {"$sSo21OS_dispatch_semaphoreC8DispatchE4waityyF", false},
      {"$sSo21OS_dispatch_semaphoreC8DispatchE6signalSiyF", true},
  };
  constexpr llvm::StringLiteral Provider =
      "/usr/lib/swift/libswiftDispatch.dylib";
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (const auto &Method : Methods) {
      SCOPED_TRACE(std::string(Method.Name) + ":" +
                   std::to_string(static_cast<int>(Architecture)));
      const std::string Import = "_" + std::string(Method.Name);
      auto Image = runtimeImage(Import, Architecture);
      Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
      EXPECT_EQ(Hint->TargetName, Method.Name);
      const auto &Signature = Hint->Signature;
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      EXPECT_EQ(Signature.ReturnType->Kind,
                Method.ReturnsWord ? NdTypeKind::Int : NdTypeKind::Void);
      if (Method.ReturnsWord)
        EXPECT_EQ(Signature.ReturnType->Size, 8U);
      ASSERT_EQ(Signature.Parameters.size(), 1U);
      EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Signature.Parameters[0].TheRole,
                SourceParameterTypeHint::Role::SwiftContext);
      EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;

      for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
        auto Wrong = Image;
        if (Mutation == 0)
          Wrong.DyldBindSlots[0x2180].Module =
              "/tmp/libswiftDispatch.dylib";
        else if (Mutation == 1)
          Wrong.DyldBindSlots[0x2180].Addend = 1;
        else if (Mutation == 2)
          Wrong.DyldBindSlots[0x2180].WeakImport = true;
        else
          Wrong.DyldBindSlots.erase(0x2180);
        EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180)) << Mutation;
      }
    }
}

TEST(ObjCCallHints, RuntimeImportsBindArgumentsBeforeSSAOnBothDarwinTargets) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image =
        runtimeImage("_objc_retainAutoreleasedReturnValue", Architecture);
    auto Low = caller(Architecture);
    const auto Med = convert(Image, Low);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    ASSERT_TRUE(Call.SourceCallHint);
    EXPECT_EQ(Call.SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::ObjCRuntimeCall);
    EXPECT_EQ(Call.SourceCallHint->TargetAddress, 0x2180U);
    EXPECT_EQ(Call.TargetName, "objc_retainAutoreleasedReturnValue");
    ASSERT_EQ(Call.Args.size(), 1U);
    EXPECT_EQ(Call.Args[0].RegOff,
              getTargetRegInfo(Architecture).IntParamRegs.front());
    EXPECT_EQ(Call.Args[0].Size, 8U);
    const auto &Op = Med.Blocks[Call.BlockId].Ops[Call.OpIdx];
    EXPECT_EQ(Op.Output.RegOff, getTargetRegInfo(Architecture).IntReturnReg);
    EXPECT_EQ(Op.Output.Size, 8U);
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Architecture);
    const auto *Expression = sourceCall(High);
    ASSERT_NE(Expression, nullptr);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
  }
}

TEST(ObjCCallHints, SwiftCRuntimeCallsKeepExactCarriersAndImportIdentity) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (const auto &[Name, Count, ReturnsPointer] :
         {std::tuple{"swift_unknownObjectWeakLoadStrong", 1U, true},
          std::tuple{"swift_unknownObjectWeakAssign", 2U, true},
          std::tuple{"swift_unknownObjectWeakCopyAssign", 2U, true},
          std::tuple{"swift_unknownObjectWeakDestroy", 1U, false},
          std::tuple{"swift_unknownObjectUnownedInit", 2U, true},
          std::tuple{"swift_unknownObjectUnownedLoadStrong", 1U, true},
          std::tuple{"swift_unknownObjectUnownedDestroy", 1U, false},
          std::tuple{"swift_getObjectType", 1U, true},
          std::tuple{"swift_bridgeObjectRetain", 1U, true},
          std::tuple{"swift_bridgeObjectRelease", 1U, false},
          std::tuple{"swift_beginAccess", 4U, false},
          std::tuple{"swift_endAccess", 1U, false}}) {
      SCOPED_TRACE(Name);
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      const auto &Hint = *Call.SourceCallHint;
      EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
      EXPECT_EQ(Hint.TargetAddress, 0x2180U);
      EXPECT_EQ(Hint.TargetName, Name);
      EXPECT_EQ(Hint.Signature.Origin,
                SourceFunctionTypeHint::OriginKind::SwiftRuntime);
      EXPECT_EQ(Hint.Signature.ReturnType->Kind,
                ReturnsPointer ? NdTypeKind::Ptr : NdTypeKind::Void);
      ASSERT_EQ(Call.Args.size(), Count);
      for (size_t I = 0; I < Count; ++I) {
        EXPECT_EQ(Call.Args[I].RegOff,
                  getTargetRegInfo(Architecture).IntParamRegs[I]);
        EXPECT_EQ(Call.Args[I].Size, 8U);
        EXPECT_EQ(Hint.Signature.Parameters[I].Type->Kind,
                  std::string(Name) == "swift_beginAccess" && I == 2
                      ? NdTypeKind::Int
                      : NdTypeKind::Ptr);
      }
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      auto Changed = *Expression;
      auto WrongHint = std::make_shared<SourceCallTypeHint>(Hint);
      WrongHint->TargetName = "swift_release";
      Changed.SourceCallHint = std::move(WrongHint);
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {}));
      Image.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
      EXPECT_TRUE(
          buildObjCSourceCallHints(Image, caller(Architecture)).empty());
    }
  }
}

TEST(ObjCCallHints,
     SwiftDeclaredAllocationABIsPreservePointersSizesAndEffects) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &[Name, Encoding] :
         {std::pair{"swift_allocObject", "ppzz"},
          std::pair{"swift_deallocUninitializedObject", "vpzz"},
          std::pair{"swift_slowAlloc", "pzz"},
          std::pair{"swift_slowDealloc", "vpzz"},
          std::pair{"swift_projectBox", "pp"},
          std::pair{"swift_arrayDestroy", "vpzp"}}) {
      SCOPED_TRACE(Name);
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      const auto &Hint = *Call.SourceCallHint;
      EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
      EXPECT_EQ(Hint.TargetName, Name);
      EXPECT_FALSE(Hint.DoesNotReturn);
      EXPECT_TRUE(Hint.BorrowedByteInputs.empty());
      EXPECT_EQ(Hint.Signature.ReturnType->Kind,
                Encoding[0] == 'v' ? NdTypeKind::Void : NdTypeKind::Ptr);
      const llvm::StringRef Arguments(Encoding + 1);
      ASSERT_EQ(Call.Args.size(), Arguments.size());
      for (size_t I = 0; I < Arguments.size(); ++I) {
        EXPECT_EQ(Call.Args[I].Size, 8U);
        EXPECT_EQ(Call.Args[I].RegOff,
                  getTargetRegInfo(Architecture).IntParamRegs[I]);
        const auto Type = Hint.Signature.Parameters[I].Type;
        EXPECT_EQ(Type->Kind,
                  Arguments[I] == 'p' ? NdTypeKind::Ptr : NdTypeKind::Int);
        if (Arguments[I] == 'z')
          EXPECT_FALSE(Type->IsSigned);
      }
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
  }
}

TEST(ObjCCallHints, SwiftDeclaredABIsRejectOtherConventionsAndUnprovedImports) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Name :
         {"_swift_allocBox", "_swift_allocObject_suffix", "swift_allocObject",
          "_swift_retainDirect", "_swift_getEnumTagSinglePayloadGeneric",
          "_malloc", "_objc_msgSend"}) {
      auto Image = runtimeImage(Name, Architecture);
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180)) << Name;
    }
    for (unsigned Case = 0; Case < 6; ++Case) {
      auto Image = runtimeImage("_swift_allocObject", Architecture);
      switch (Case) {
      case 0:
        Image.ImportPtrSlots.clear();
        break;
      case 1:
        Image.ConflictingImportStorageSlots.insert(0x2180);
        break;
      case 2:
        Image.DyldBindSlots[0x2180].Addend = 8;
        break;
      case 3:
        Image.DyldBindSlots[0x2180].WeakImport = true;
        break;
      case 4:
        Image.IsRelocatable = true;
        break;
      case 5:
        Image.Format = BinaryFormat::ELF;
        break;
      }
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180)) << Case;
    }
  }
}

TEST(ObjCCallHints,
     SwiftFixedRuntimeDeclarationsPreserveConventionAndArguments) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &[Name, Encoding] :
         std::vector<std::pair<const char *, const char *>>{
             {"_swift_allocateMetadataPack", "ppz"},
             {"_swift_allocateWitnessTablePack", "ppz"},
             {"_swift_errorInMain", "vp"},
             {"_swift_getAssociatedConformanceWitness", "pppppp"},
             {"_swift_getAssociatedConformanceWitnessRelative", "pppppp"},
             {"_swift_getTupleTypeLayout", "vppzp"},
             {"_swift_getTupleTypeLayout2", "zppp"},
             {"_swift_getTypeByMangledNameInContext", "ppzpp"},
             {"_swift_getTypeByMangledNameInContext2", "ppzpp"},
             {"_swift_getTypeByMangledNameInContextInMetadataState", "pzpzpp"},
             {"_swift_getTypeByMangledNameInContextInMetadataState2", "pzpzpp"},
             {"_swift_unexpectedError", "vp"},
             {"_swift_updatePureObjCClassMetadata", "ppzzp"}}) {
      SCOPED_TRACE(Name);
      auto Image = runtimeImage(Name, Architecture);
      auto &Bind = Image.DyldBindSlots[0x2180];
      Bind.Name = Name;
      Bind.Module = "/usr/lib/swift/libswiftCore.dylib";
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      const auto &Hint = *Call.SourceCallHint;
      EXPECT_EQ(Hint.Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      EXPECT_EQ(Hint.Signature.Origin,
                SourceFunctionTypeHint::OriginKind::SwiftRuntime);
      EXPECT_EQ(Hint.DoesNotReturn,
                llvm::StringRef(Name) == "_swift_unexpectedError");
      EXPECT_TRUE(Hint.BorrowedByteInputs.empty());
      EXPECT_EQ(Hint.Signature.ReturnType->Kind,
                Encoding[0] == 'v'   ? NdTypeKind::Void
                : Encoding[0] == 'z' ? NdTypeKind::Int
                                     : NdTypeKind::Ptr);
      const llvm::StringRef Arguments(Encoding + 1);
      ASSERT_EQ(Call.Args.size(), Arguments.size());
      for (size_t I = 0; I < Arguments.size(); ++I) {
        EXPECT_EQ(Call.Args[I].Size, 8U);
        EXPECT_EQ(Call.Args[I].RegOff,
                  getTargetRegInfo(Architecture).IntParamRegs[I]);
        EXPECT_EQ(Hint.Signature.Parameters[I].Type->Kind,
                  Arguments[I] == 'p' ? NdTypeKind::Ptr : NdTypeKind::Int);
      }
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      auto Forged = *Expression;
      auto Changed =
          std::make_shared<SourceCallTypeHint>(*Expression->SourceCallHint);
      Changed->Signature.Convention = SourceFunctionTypeHint::ConventionKind::C;
      Forged.SourceCallHint = std::move(Changed);
      EXPECT_FALSE(sdk::objcSourceCallBound(Forged, Image, {}));
    }
  }
}

TEST(ObjCCallHints,
     SwiftAssertionFailureKeepsExactTerminalABIAndImportIdentity) {
  const std::string Import =
      "_$ss17_assertionFailure__4file4line5flagss5NeverOs12StaticStringV_"
      "SSAHSus6UInt32VtF";
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {Import, 0,
                                   "/usr/lib/swift/libswiftCore.dylib", false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
    EXPECT_EQ(Hint->TargetName, Import.substr(1));
    EXPECT_TRUE(Hint->DoesNotReturn);
    EXPECT_EQ(Hint->BorrowedByteInputs,
              (std::vector<std::pair<unsigned, unsigned>>{{0, 1}, {5, 6}}));
    EXPECT_EQ(Hint->SwiftStringInputs,
              (std::vector<std::pair<unsigned, unsigned>>{{3, 4}}));
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Signature.Parameters.size(), 10U);
    EXPECT_EQ(Signature.Parameters[2].Type->Size, 1U);
    EXPECT_EQ(Signature.Parameters[4].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[7].Type->Size, 1U);
    EXPECT_EQ(Signature.Parameters[9].Type->Size, 4U);
    const auto &TRI = getTargetRegInfo(Architecture);
    int64_t StackOffset = Architecture == Arch::X64 ? 8 : 0;
    for (size_t I = 0; I < Signature.Parameters.size(); ++I) {
      const auto &Parameter = Signature.Parameters[I];
      const auto &Location = Parameter.Location;
      EXPECT_EQ(Location.ValueBytes, Parameter.Type->Size) << I;
      if (I < TRI.IntParamRegs.size()) {
        EXPECT_EQ(Location.Kind, SourceABICarrierKind::IntegerRegister) << I;
        EXPECT_EQ(Location.RegisterOffset, TRI.IntParamRegs[I]) << I;
      } else {
        const int64_t Alignment =
            Architecture == Arch::AArch64 ? Parameter.Type->Size : 8;
        StackOffset = (StackOffset + Alignment - 1) & -Alignment;
        EXPECT_EQ(Location.Kind, SourceABICarrierKind::Stack) << I;
        EXPECT_EQ(Location.EntryStackOffset, StackOffset) << I;
        StackOffset += Architecture == Arch::AArch64 ? Parameter.Type->Size : 8;
      }
    }

    std::vector<ExprPtr> Arguments;
    for (const auto &Parameter : Signature.Parameters)
      Arguments.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
    auto Call = HighExpr::makeCall("untrusted_name", 0x1100, Arguments);
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    HighStmt Statement;
    Statement.Kind = StmtKind::Call;
    Statement.CallExpr = Call;
    HighFunc Function;
    Function.Entry = 0x1200;
    Function.Name = "assertion_wrapper";
    Function.ReturnType = NdType::makeVoid();
    Function.Body = {Statement};
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
    EXPECT_NE(Source.find("__attribute__((swiftcall))"), std::string::npos)
        << Source;
    EXPECT_NE(Source.find("__attribute__((noreturn))"), std::string::npos)
        << Source;
    EXPECT_NE(Source.find("__asm__(\"" + Import + "\")"), std::string::npos)
        << Source;
    EXPECT_NE(Source.find("uint8_t"), std::string::npos) << Source;

    const std::string Fatal = "Fatal error";
    const std::string Detail = "not implemented";
    const std::string File = "Example.swift";
    const auto Store = [&](va_t Address, llvm::StringRef Bytes) {
      auto At = Image.Segments[0].Data.begin() + (Address - 0x1000);
      std::copy(Bytes.begin(), Bytes.end(), At);
      At[Bytes.size()] = 0;
    };
    Store(0x2200, Fatal);
    Store(0x2280, Detail);
    Store(0x22c0, File);
    const auto WidenedCount = [](uint32_t Value) -> ExprPtr {
      auto Signed =
          HighExpr::makeUnary(NdOp::INT_SEXT, HighExpr::makeConst(Value, 4));
      Signed->Type = NdType::makeInt(8, true);
      auto Unsigned = std::make_shared<HighExpr>();
      Unsigned->Kind = ExprKind::Cast;
      Unsigned->Type = Unsigned->CastTo = NdType::makeInt(8, false);
      Unsigned->Operands = {std::move(Signed)};
      return Unsigned;
    };
    std::vector<ExprPtr> LiteralArguments{
        HighExpr::makeConst(0x2200, 8, ConstantAddressProvenance::DataAddress),
        WidenedCount(Fatal.size()),
        HighExpr::makeConst(2, 1),
        HighExpr::makeConst(UINT64_C(0xd000000000000000) | Detail.size(), 8),
        HighExpr::makeConst(UINT64_C(0x8000000000000000) | (0x2280 - 32), 8,
                            ConstantAddressProvenance::DataAddress),
        HighExpr::makeConst(0x22c0, 8, ConstantAddressProvenance::DataAddress),
        WidenedCount(File.size()),
        HighExpr::makeConst(2, 1),
        HighExpr::makeConst(7, 8),
        HighExpr::makeConst(0, 4)};
    auto LiteralCall =
        HighExpr::makeCall("untrusted_name", 0x1100, LiteralArguments);
    LiteralCall->Type = NdType::makeVoid();
    LiteralCall->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    HighStmt LiteralStatement;
    LiteralStatement.Kind = StmtKind::Call;
    LiteralStatement.CallExpr = LiteralCall;
    HighFunc LiteralFunction;
    LiteralFunction.Entry = 0x1300;
    LiteralFunction.Name = "literal_assertion_wrapper";
    LiteralFunction.ReturnType = NdType::makeVoid();
    LiteralFunction.Body = {LiteralStatement};
    auto Bound = sdk::bindObjCSourceReferences(LiteralFunction, Image);
    EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_EQ(Bound.BorrowedBytes.size(), 3U);
    ASSERT_TRUE(Bound.Function.Body[0].CallExpr);
    EXPECT_TRUE(
        sdk::objcSourceCallBound(*Bound.Function.Body[0].CallExpr, Image, {}));

    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      auto WrongCall = *Call;
      auto Wrong = std::make_shared<SourceCallTypeHint>(*Hint);
      if (Mutation == 0)
        Wrong->DoesNotReturn = false;
      else if (Mutation == 1)
        Wrong->TargetName += "_suffix";
      else if (Mutation == 2)
        Wrong->Signature.Parameters[2].Type = NdType::makeInt(8, false);
      else if (Mutation == 3)
        ++Wrong->Signature.Parameters.back().Location.EntryStackOffset;
      else if (Mutation == 4)
        Wrong->Signature.Origin =
            SourceFunctionTypeHint::OriginKind::SwiftRuntime;
      else if (Mutation == 5)
        Wrong->BorrowedByteInputs.pop_back();
      else
        Wrong->SwiftStringInputs.clear();
      WrongCall.SourceCallHint = std::move(Wrong);
      EXPECT_FALSE(sdk::objcSourceCallBound(WrongCall, Image, {})) << Mutation;
    }
    Image.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
  }
}

TEST(ObjCCallHints, SwiftAssertionFailureTerminatesTheBoundMachineCall) {
  const std::string Import =
      "_$ss17_assertionFailure__4file4line5flagss5NeverOs12StaticStringV_"
      "SSAHSus6UInt32VtF";
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Image = runtimeImage(Import, Architecture);
    Image.DyldBindSlots[0x2180] = {
        Import, 0, "/usr/lib/swift/libswiftCore.dylib", false};
    // The x64 stack arguments need the call's return-address bias.
    if (Architecture == Arch::X64)
      Image.Segments[0].Data[0x200] = 0xe8;
    auto Med = convert(Image, caller(Architecture));
    unsigned BoundCalls = 0;
    for (const auto &Block : Med.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL) {
          ASSERT_TRUE(Op.SourceCallHint);
          EXPECT_TRUE(Op.DoesNotReturn);
          EXPECT_EQ(Op.NumInputs, 11U);
          EXPECT_EQ(Op.Inputs[0].ConstVal, 0x1100U);
          EXPECT_EQ(Op.SourceCallHint->TargetAddress, 0x2180U);
          ++BoundCalls;
        }
    ASSERT_EQ(BoundCalls, 1U);
    std::vector<MedFunc> Functions{std::move(Med)};
    propagateInternalNoReturn(Functions, Architecture);
    EXPECT_TRUE(Functions[0].DoesNotReturn);
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Functions[0], Architecture);
    EXPECT_TRUE(High.DoesNotReturn);
    unsigned SourceReturns = 0;
    walkStmts(High.Body, [&](const HighStmt &Statement) {
      SourceReturns += Statement.Kind == StmtKind::Return;
    });
    EXPECT_EQ(SourceReturns, 0U);

    // Neither disabled source binding nor a same-named foreign import grants
    // the machine termination effect from the SDK declaration.
    for (bool Enabled : {false, true}) {
      if (Enabled)
        Image.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
      const auto Unbound = convert(Image, caller(Architecture), Enabled);
      for (const auto &Block : Unbound.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL) {
            EXPECT_FALSE(Op.SourceCallHint);
            EXPECT_FALSE(Op.DoesNotReturn);
          }
    }
  }
}

TEST(ObjCCallHints, SwiftRuntimeRecordResultsKeepBothDeclaredCarriers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (const auto &[Name, Arguments] :
         {std::pair{"_swift_allocBox", "p"},
          std::pair{"_swift_makeBoxUnique", "ppz"},
          std::pair{"_swift_checkMetadataState", "zp"},
          std::pair{"_swift_getAssociatedTypeWitness", "zpppp"},
          std::pair{"_swift_getAssociatedTypeWitnessRelative", "zpppp"},
          std::pair{"_swift_getBorrowTypeMetadata", "zp"},
          std::pair{"_swift_getForeignTypeMetadata", "zp"},
          std::pair{"_swift_getGenericMetadata", "zpp"},
          std::pair{"_swift_getSingletonMetadata", "zp"},
          std::pair{"_swift_getTupleTypeMetadata", "zzppp"},
          std::pair{"_swift_getTupleTypeMetadata2", "zpppp"},
          std::pair{"_swift_getTupleTypeMetadata3", "zppppp"}}) {
      SCOPED_TRACE(Name);
      auto Image = runtimeImage(Name, Architecture);
      Image.DyldBindSlots[0x2180].Name = Name;
      Image.DyldBindSlots[0x2180].Module = "/usr/lib/swift/libswiftCore.dylib";
      const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      EXPECT_FALSE(Hint->DoesNotReturn);
      EXPECT_TRUE(Hint->BorrowedByteInputs.empty());
      const auto &Signature = Hint->Signature;
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      const auto Members = sourceAggregateMembers(Signature.ReturnType);
      ASSERT_EQ(Members.size(), 2U);
      ASSERT_EQ(Signature.ReturnComponents.size(), 2U);
      const bool Box = llvm::StringRef(Name).contains("Box");
      const auto &TRI = getTargetRegInfo(Architecture);
      for (unsigned I = 0; I < 2; ++I) {
        EXPECT_EQ(Members[I].ByteOffset, I * 8U);
        EXPECT_EQ(Members[I].Type->Size, 8U);
        EXPECT_EQ(Members[I].Type->Kind,
                  !I || Box ? NdTypeKind::Ptr : NdTypeKind::Int);
        EXPECT_EQ(Signature.ReturnComponents[I].RegisterOffset,
                  TRI.IntReturnRegs[I]);
        EXPECT_EQ(Signature.ReturnComponents[I].ValueBytes, 8U);
      }
      ASSERT_EQ(Signature.Parameters.size(), llvm::StringRef(Arguments).size());
      for (size_t I = 0; I < Signature.Parameters.size(); ++I) {
        EXPECT_EQ(Signature.Parameters[I].Type->Kind,
                  Arguments[I] == 'p' ? NdTypeKind::Ptr : NdTypeKind::Int);
        EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                  TRI.IntParamRegs[I]);
      }
    }
}

TEST(ObjCCallHints, SwiftRuntimeRecordResultsRevalidateShapeAndImport) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (const char *Name : {"_swift_allocBox", "_swift_checkMetadataState"}) {
      auto Image = runtimeImage(Name, Architecture);
      Image.DyldBindSlots[0x2180].Name = Name;
      Image.DyldBindSlots[0x2180].Module = "/usr/lib/swift/libswiftCore.dylib";
      const auto &TRI = getTargetRegInfo(Architecture);
      auto Low = caller(Architecture);
      Low.Blocks[0].Ops.insert(
          Low.Blocks[0].Ops.begin() + 1,
          operation(NdOp::COPY, NdVar::reg(TRI.IntReturnReg, 8),
                    {NdVar::reg(TRI.IntReturnRegs[1], 8)}, 0x1204));
      const auto Med = convert(Image, Low);
      const auto High = MedToHighConverter().convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        auto Changed = *Expression;
        auto Wrong =
            std::make_shared<SourceCallTypeHint>(*Changed.SourceCallHint);
        if (Mutation == 0)
          Wrong->Signature.ReturnComponents.pop_back();
        else if (Mutation == 1)
          std::swap(Wrong->Signature.ReturnComponents[0],
                    Wrong->Signature.ReturnComponents[1]);
        else if (Mutation == 2)
          Wrong->Signature.ReturnType = NdType::makeInt(16, false);
        else if (Mutation == 3)
          Wrong->Signature.Convention =
              SourceFunctionTypeHint::ConventionKind::C;
        else
          Wrong->TargetName += "_suffix";
        Changed.SourceCallHint = std::move(Wrong);
        EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {})) << Mutation;
      }
      Image.DyldBindSlots[0x2180].WeakImport = true;
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
}

TEST(ObjCCallHints, SwiftCIntegerDeclarationsPreserveWordAndBooleanCarriers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (const auto &[Name, Encoding] :
         {std::pair{"_swift_unknownObjectRetain_n", "ppu"},
          std::pair{"_swift_unknownObjectRelease_n", "vpu"},
          std::pair{"_swift_getEnumCaseMultiPayload", "upp"},
          std::pair{"_swift_dynamicCast", "bppppz"},
          std::pair{"_swift_isUniquelyReferenced_nonNull_native", "bp"}}) {
      SCOPED_TRACE(Name);
      auto Image = runtimeImage(Name, Architecture);
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      const auto &Hint = *Call.SourceCallHint;
      const auto &Signature = Hint.Signature;
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::C);
      EXPECT_FALSE(Hint.DoesNotReturn);
      EXPECT_TRUE(Hint.BorrowedByteInputs.empty());
      const auto Bytes = [](char Code) -> unsigned {
        return Code == 'v' ? 0 : Code == 'b' ? 1 : Code == 'u' ? 4 : 8;
      };
      EXPECT_EQ(Signature.ReturnType->Size, Bytes(Encoding[0]));
      EXPECT_EQ(Signature.ReturnLocation.ExtendTo32Bits,
                Architecture == Arch::AArch64 && Encoding[0] == 'b');
      if (Encoding[0] == 'b' || Encoding[0] == 'u')
        EXPECT_FALSE(Signature.ReturnType->IsSigned);
      const llvm::StringRef Arguments(Encoding + 1);
      ASSERT_EQ(Call.Args.size(), Arguments.size());
      for (size_t I = 0; I < Arguments.size(); ++I) {
        EXPECT_EQ(Call.Args[I].Size, Bytes(Arguments[I]));
        EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[I]);
        EXPECT_EQ(Signature.Parameters[I].Location.ValueBytes,
                  Bytes(Arguments[I]));
        EXPECT_FALSE(Signature.Parameters[I].Location.ExtendTo32Bits);
      }
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
}

TEST(ObjCCallHints, SwiftCIntegerBindingsRevalidateWidthsAndImports) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (const char *Name :
         {"_swift_unknownObjectRetain_n", "_swift_dynamicCast"}) {
      auto Image = runtimeImage(Name, Architecture);
      const auto Med = convert(Image, caller(Architecture));
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      auto Forged = *Expression;
      auto Changed =
          std::make_shared<SourceCallTypeHint>(*Expression->SourceCallHint);
      if (llvm::StringRef(Name) == "_swift_dynamicCast") {
        Changed->Signature.ReturnType = NdType::makeInt(4, false);
        Changed->Signature.ReturnLocation.ValueBytes = 4;
        Changed->Signature.ReturnLocation.ExtendTo32Bits = false;
      } else {
        Changed->Signature.Parameters[1].Type = NdType::makeInt(8, false);
        Changed->Signature.Parameters[1].Location.ValueBytes = 8;
      }
      Forged.SourceCallHint = Changed;
      EXPECT_FALSE(sdk::objcSourceCallBound(Forged, Image, {}));
      Image.DyldBindSlots[0x2180].WeakImport = true;
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
}

TEST(ObjCCallHints, SwiftFixedRuntimeImportsRequireExactStrongProvider) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      auto Image =
          runtimeImage("_swift_getTypeByMangledNameInContext2", Architecture);
      auto &Bind = Image.DyldBindSlots[0x2180];
      Bind.Name = Image.ImportPtrSlots.at(0x2180);
      Bind.Module = "/usr/lib/swift/libswiftCore.dylib";
      if (Mutation == 0)
        Image.DyldBindSlots.clear();
      if (Mutation == 1)
        Bind.Module = "libswiftCore.dylib";
      if (Mutation == 2)
        Bind.Module = "/tmp/libswiftCore.dylib";
      if (Mutation == 3)
        Bind.WeakImport = true;
      if (Mutation == 4)
        Bind.Addend = 8;
      if (Mutation == 5)
        Bind.Name += "_suffix";
      if (Mutation == 6)
        Image.ConflictingImportStorageSlots.insert(0x2180);
      if (Mutation == 7)
        Image.IsRelocatable = true;
      if (Mutation == 8)
        Image.Format = BinaryFormat::ELF;
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180)) << Mutation;
    }
  }
}

TEST(ObjCCallHints, SwiftFixedRuntimeDeclarationsExcludeCustomParameterABIs) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (const char *Name :
         {"_swift_willThrow", "_swift_allocBoxTyped",
          "_swift_retainDirect", "_swift_task_getCurrent",
          "_swift_getTypeByMangledNameInContext2_suffix"}) {
      auto Image = runtimeImage(Name, Architecture);
      Image.DyldBindSlots[0x2180].Name = Name;
      Image.DyldBindSlots[0x2180].Module = "/usr/lib/swift/libswiftCore.dylib";
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180)) << Name;
    }
  auto X64 = runtimeImage("_swift_allocError", Arch::X64);
  X64.DyldBindSlots[0x2180] = {
      "_swift_allocError", 0, "/usr/lib/swift/libswiftCore.dylib", false};
  EXPECT_FALSE(swiftRuntimeSourceCallHint(X64, 0x2180));
}

TEST(ObjCCallHints, SwiftOnceKeepsCallbackContextAndVoidResult) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("_swift_once", Architecture);
    const auto Med = convert(Image, caller(Architecture));
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    ASSERT_TRUE(Call.SourceCallHint);
    const auto &Hint = *Call.SourceCallHint;
    ASSERT_EQ(Hint.Signature.Parameters.size(), 3U);
    ASSERT_EQ(Call.Args.size(), 3U);
    EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
    EXPECT_EQ(Hint.Signature.ReturnLocation.Kind, SourceABICarrierKind::None);
    const auto Callback = Hint.Signature.Parameters[1].Type;
    ASSERT_EQ(Callback->Kind, NdTypeKind::Ptr);
    ASSERT_TRUE(Callback->Pointee);
    EXPECT_EQ(Callback->Pointee->Kind, NdTypeKind::Func);
    EXPECT_EQ(Callback->Pointee->RetType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Callback->Pointee->ParamTypes.size(), 1U);
    EXPECT_TRUE(equalSourceTypes(Callback->Pointee->ParamTypes[0],
                                 Hint.Signature.Parameters[2].Type));
    for (size_t I = 0; I < 3; ++I) {
      EXPECT_EQ(Call.Args[I].RegOff,
                getTargetRegInfo(Architecture).IntParamRegs[I]);
      EXPECT_EQ(Call.Args[I].Size, 8U);
    }
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Architecture);
    const auto *Expression = sourceCall(High);
    ASSERT_NE(Expression, nullptr);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    for (const auto &WrongType :
         {NdType::makePtr(NdType::makeVoid()),
          NdType::makePtr(NdType::makeFunc(NdType::makeInt(8))),
          NdType::makePtr(NdType::makeFunc(NdType::makeVoid()))}) {
      auto Changed = *Expression;
      auto Wrong = std::make_shared<SourceCallTypeHint>(Hint);
      Wrong->Signature.Parameters[1].Type = WrongType;
      Changed.SourceCallHint = Wrong;
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {}));
    }
    Image.ConflictingImportStorageSlots.insert(0x2180);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
  }
}

TEST(ObjCCallHints, DiagnosticRuntimeUsesCABIAndKeepsItsLinkerSpelling) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &[Name, Count] :
         {std::pair{"_swift_stdlib_reportUnimplementedInitializer", 5U},
          std::pair{"_swift_stdlib_reportFatalError", 5U},
          std::pair{"_swift_stdlib_reportUnimplementedInitializerInFile", 9U},
          std::pair{"_swift_stdlib_reportFatalErrorInFile", 8U}}) {
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      ASSERT_EQ(Hint->Signature.Parameters.size(), Count);
      EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Hint->BorrowedByteInputs.size(), Count == 5 ? 2U : 3U);
      EXPECT_EQ(Hint->Signature.Parameters[1].Type->Size, 4U);
      EXPECT_TRUE(Hint->Signature.Parameters[1].Type->IsSigned);
      EXPECT_EQ(Hint->Signature.Parameters.back().Type->Size, 4U);
      EXPECT_FALSE(Hint->Signature.Parameters.back().Type->IsSigned);
      std::string Error;
      EXPECT_TRUE(validateSourceABI(Hint->Signature, Error)) << Error;
      std::vector<ExprPtr> Args;
      for (const auto &Parameter : Hint->Signature.Parameters)
        Args.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
      auto Call = HighExpr::makeCall("unrelated_veneer", 0x1100, Args);
      Call->Type = NdType::makeVoid();
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      HighStmt Statement;
      Statement.Kind = StmtKind::Call;
      Statement.CallExpr = Call;
      HighFunc Function;
      Function.Entry = 0x1200;
      Function.Name = "diagnostic_wrapper";
      Function.ReturnType = NdType::makeVoid();
      Function.Body = {Statement};
      std::string C;
      llvm::raw_string_ostream OS(C);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      Options.Format = BinaryFormat::MachO;
      ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
      EXPECT_NE(C.find("__asm__(\"_" + std::string(Name) + "\")"),
                std::string::npos)
          << C;
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      auto Forged = std::make_shared<SourceCallTypeHint>(*Hint);
      Forged->BorrowedByteInputs = {{1, 0}};
      Call->SourceCallHint = Forged;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
    }
  }
}

TEST(ObjCCallHints, ImmutableBytesRejectAliasingWritableAndRelocatedStorage) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    auto Image = runtimeImage("__swift_stdlib_reportUnimplementedInitializer");
    auto &Data = Image.Sections[1];
    // Linked Mach-O data inherits RX segment permissions; instruction
    // attributes, rather than those coarse permissions, identify code.
    Image.Segments[0].Flags =
        Image.Segments[0].Flags | SegmentFlags::Executable;
    Data.Flags = Data.Flags | SegmentFlags::Executable;
    ASSERT_TRUE(readImmutableImageBytes(Image, 0x2200, 5));
    if (Mutation == 0)
      Data.Flags = Data.Flags | SegmentFlags::Writable;
    if (Mutation == 1)
      Image.Segments[0].Flags =
          Image.Segments[0].Flags | SegmentFlags::Writable;
    if (Mutation == 2)
      Image.Sections.push_back(Data);
    if (Mutation == 3)
      Image.Segments.push_back(Image.Segments[0]);
    if (Mutation == 4)
      Image.DataPtrRelocSlots.insert(0x21ff);
    if (Mutation == 5)
      Image.CodePtrRelocSlots.insert(0x2204);
    if (Mutation == 6)
      Image.DyldBindSlots[0x2201] = {};
    if (Mutation == 7)
      Image.ConflictingImportStorageSlots.insert(0x21fa);
    if (Mutation == 8)
      Data.FileSz = 0x201;
    if (Mutation == 9)
      Image.MachOChainedFixupsAmbiguous = true;
    if (Mutation == 10)
      Data.Type |= llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    if (Mutation == 11)
      Image.ImportPtrSlots[0x2200] = "_other";
    EXPECT_FALSE(readImmutableImageBytes(Image, 0x2200, 5)) << Mutation;
  }
  const auto Image =
      runtimeImage("__swift_stdlib_reportUnimplementedInitializer");
  EXPECT_FALSE(readImmutableImageBytes(Image, UINT64_MAX - 2, 5));
  EXPECT_FALSE(readImmutableImageBytes(Image, 0x2200, 1024 * 1024 + 1));
}

TEST(ObjCCallHints, BorrowedBytesRequireTheBoundedConsumerOccurrence) {
  auto Image = runtimeImage("__swift_stdlib_reportUnimplementedInitializer");
  const uint8_t Bytes[] = {65, 0, 255, 34, 92};
  std::copy(std::begin(Bytes), std::end(Bytes),
            Image.Segments[0].Data.begin() + 0x1200);
  const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(Hint);
  auto Address = HighExpr::makeConst(0x2200, 8);
  auto Count = HighExpr::makeConst(5, 4);
  auto Call = HighExpr::makeCall(
      {}, 0x1100, {Address, Count, Address, Count, HighExpr::makeConst(0, 4)});
  Call->Type = NdType::makeVoid();
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = Call;
  HighFunc Function;
  Function.ReturnType = NdType::makeVoid();
  Function.Body = {Statement};
  auto Result = sdk::bindObjCSourceReferences(Function, Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.BorrowedBytes.size(), 1U);
  std::set<std::string> Helpers;
  auto Source =
      sdk::renderBorrowedByteHelpers(Image, Result.BorrowedBytes, Helpers);
  EXPECT_NE(Source.find("65, 0, 255, 34, 92, 0"), std::string::npos);
  EXPECT_EQ(Call->Operands[0], Address)
      << "Binding must not mutate the original DAG";
  auto BoundCall = Result.Function.Body[0].CallExpr;
  EXPECT_TRUE(sdk::objcSourceCallBound(*BoundCall, Image, {}));
  EXPECT_TRUE(sdk::objcSourceCallBound(*BoundCall->Operands[0], Image, {}));
  Count->ConstVal =
      UINT32_MAX; // Negative precision would make printf unbounded.
  EXPECT_FALSE(
      sdk::bindObjCSourceReferences(Function, Image).Limitation.empty());
  Count->ConstVal = 5;
  auto ZeroExtended =
      HighExpr::makeUnary(NdOp::INT_ZEXT, HighExpr::makeConst(5, 2));
  ZeroExtended->Type = NdType::makeInt(8, false);
  Call->Operands[1] = ZeroExtended;
  EXPECT_TRUE(
      sdk::bindObjCSourceReferences(Function, Image).Limitation.empty());
  auto Negative =
      HighExpr::makeUnary(NdOp::INT_SEXT, HighExpr::makeConst(UINT32_MAX, 4));
  Negative->Type = NdType::makeInt(8, true);
  Call->Operands[1] = Negative;
  EXPECT_FALSE(
      sdk::bindObjCSourceReferences(Function, Image).Limitation.empty());
  Call->Operands[1] = HighExpr::makeConst(1024 * 1024 + 1, 8);
  EXPECT_FALSE(
      sdk::bindObjCSourceReferences(Function, Image).Limitation.empty());
  Call->Operands[1] = Count;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = Address;
  Function.ReturnType = NdType::makePtr(NdType::makeVoid());
  Function.Body.push_back(Return);
  Result = sdk::bindObjCSourceReferences(Function, Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_EQ(Result.Function.Body.back().RetVal->Kind, ExprKind::Const);
}

TEST(ObjCCallHints, BorrowedByteArgumentsRetainDistinctRangesAcrossCopies) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("__swift_stdlib_reportUnimplementedInitializer",
                              Architecture);
    const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    HighFunc Function;
    Function.ReturnType = NdType::makeVoid();
    for (unsigned I = 0; I < 64; ++I) {
      auto FirstCount = HighExpr::makeConst(1 + I % 7, 4);
      HighStmt SharedCount;
      SharedCount.Kind = StmtKind::ExprStmt;
      SharedCount.Val = FirstCount;
      Function.Body.push_back(std::move(SharedCount));
      HighStmt Statement;
      Statement.Kind = StmtKind::Call;
      Statement.CallExpr = HighExpr::makeCall(
          {}, 0x1100,
          {HighExpr::makeConst(0x2200 + 8 * I, 8), FirstCount,
           HighExpr::makeConst(0x2600 + 8 * I, 8),
           HighExpr::makeConst(7 - I % 7, 4), HighExpr::makeConst(0, 4)});
      Statement.CallExpr->Type = Function.ReturnType;
      Statement.CallExpr->SourceCallHint =
          std::make_shared<SourceCallTypeHint>(*Hint);
      Function.Body.push_back(std::move(Statement));
    }
    for (unsigned Round = 0; Round < 8; ++Round) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(Architecture) << ':' << Round);
      const auto Bound = sdk::bindObjCSourceReferences(Function, Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_EQ(Bound.BorrowedBytes.size(), 128U);
      for (unsigned I = 0; I < 64; ++I) {
        const auto &Operands =
            Bound.Function.Body[2 * I + 1].CallExpr->Operands;
        for (unsigned Argument : {0U, 2U}) {
          ASSERT_TRUE(Operands[Argument]->SourceCallHint);
          const auto &Bytes = *Operands[Argument]->SourceCallHint;
          EXPECT_EQ(Bytes.TargetAddress, (Argument ? 0x2600 : 0x2200) + 8 * I);
          EXPECT_EQ(Bytes.ByteCount, Argument ? 7 - I % 7 : 1 + I % 7);
          EXPECT_EQ(Function.Body[2 * I + 1].CallExpr->Operands[Argument]->Kind,
                    ExprKind::Const);
        }
      }
    }
  }
}

TEST(ObjCCallHints, SwiftRuntimeRejectsSpecialConventionsAndUnprovenTargets) {
  for (const char *Name :
       {"_swift_retainDirect", "_swift_releaseDirect", "_swift_retain_x20",
        "_swift_getTypeByMangledName",
        "_swift_unknownObjectWeakLoadStrong_suffix", "swift_retain"}) {
    auto Image = runtimeImage(Name);
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty()) << Name;
  }
  auto Image = runtimeImage("_swift_retain");
  Image.ImportPtrSlots.clear();
  Image.Symbols.push_back({"_swift_retain", 0x1100});
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image = runtimeImage("_swift_retain");
  Image.Segments[0].Data[0x108] ^= 1;
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
}

TEST(ObjCCallHints,
     SwiftStringBridgePreservesSwiftConventionAndScalarCarriers) {
  const std::string Name =
      "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF";
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Image = runtimeImage(Name, Architecture);
    Image.DyldBindSlots[0x2180] = {
        Name, 0, "/System/Library/Frameworks/Foundation.framework/Foundation",
        false};
    const auto Med = convert(Image, caller(Architecture));
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    ASSERT_TRUE(Call.SourceCallHint);
    const auto &Hint = *Call.SourceCallHint;
    EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::SwiftStringBridge);
    EXPECT_EQ(Hint.Signature.Origin,
              SourceFunctionTypeHint::OriginKind::SwiftStringBridge);
    EXPECT_EQ(Hint.TargetName, Name);
    EXPECT_EQ(Hint.TargetAddress, 0x2180U);
    EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Ptr);
    ASSERT_EQ(Call.Args.size(), 2U);
    for (unsigned I = 0; I < 2; ++I) {
      EXPECT_EQ(Call.Args[I].RegOff,
                getTargetRegInfo(Architecture).IntParamRegs[I]);
      EXPECT_EQ(Call.Args[I].Size, 8U);
      EXPECT_EQ(Hint.Signature.Parameters[I].Type->Kind,
                I ? NdTypeKind::Ptr : NdTypeKind::Int);
    }
    EXPECT_FALSE(Hint.Signature.Parameters[0].Type->IsSigned);
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Architecture);
    const auto *Expression = sourceCall(High);
    ASSERT_NE(Expression, nullptr);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    std::string C;
    llvm::raw_string_ostream OS(C);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    EXPECT_NE(
        C.find("extern void *neverd_swift_string_to_nsstring(uint64_t, void *) "
               "__asm__(\"" +
               Name + "\") __attribute__((swiftcall));"),
        std::string::npos)
        << C;
    EXPECT_NE(C.find("neverd_swift_string_to_nsstring((uint64_t)"),
              std::string::npos)
        << C;
    EXPECT_EQ(C.find("extern int _sSS"), std::string::npos) << C;
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto Changed = *Expression;
      auto Wrong = std::make_shared<SourceCallTypeHint>(Hint);
      if (Mutation == 0)
        Wrong->CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
      else if (Mutation == 1)
        Wrong->Signature.Origin =
            SourceFunctionTypeHint::OriginKind::SwiftRuntime;
      else if (Mutation == 2)
        Wrong->Signature.Parameters[0].Type =
            NdType::makePtr(NdType::makeVoid());
      else if (Mutation == 3)
        Wrong->Signature.Parameters[1].Location.RegisterOffset =
            getTargetRegInfo(Architecture).IntParamRegs[2];
      else
        Wrong->TargetName += "_suffix";
      Changed.SourceCallHint = std::move(Wrong);
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {})) << Mutation;
    }
    Image.ConflictingImportStorageSlots.insert(0x2180);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
  }
}

TEST(ObjCCallHints, NSStringBridgePreservesBothResultWordsAndExactImport) {
  const std::string Name =
      "_$sSS10FoundationE36_"
      "unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ";
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    auto Image = runtimeImage(Name, Architecture);
    Image.DyldBindSlots[0x2180] = {
        Name, 0, "/System/Library/Frameworks/Foundation.framework/Foundation",
        false};
    auto Low = caller(Architecture);
    Low.Blocks[0].Ops.insert(
        Low.Blocks[0].Ops.begin() + 1,
        operation(NdOp::COPY, NdVar::reg(TRI.IntReturnReg, 8),
                  {NdVar::reg(TRI.IntReturnRegs[1], 8)}, 0x1204));
    auto Med = convert(Image, Low);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    ASSERT_TRUE(Med.CallInfos[0].SourceCallHint);
    const auto &Hint = *Med.CallInfos[0].SourceCallHint;
    EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::SwiftStringFromNSString);
    ASSERT_EQ(Hint.Signature.ReturnComponents.size(), 2U);
    ASSERT_EQ(Hint.Signature.Parameters.size(), 1U);
    EXPECT_EQ(Hint.Signature.ReturnType->Size, 16U);
    EXPECT_EQ(Hint.Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
    const auto High = MedToHighConverter().convert(Med, Architecture);
    const auto *Expression = sourceCall(High);
    ASSERT_NE(Expression, nullptr);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    EXPECT_NE(Source.find("extern unsigned __int128 "
                          "neverd_nsstring_to_swift_string(void *) __asm__(\"" +
                          Name + "\") __attribute__((swiftcall));"),
              std::string::npos)
        << Source;
    EXPECT_EQ(Source.find("caller-saved register clobbered"), std::string::npos)
        << Source;
    EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      auto Changed = *Expression;
      auto Wrong = std::make_shared<SourceCallTypeHint>(Hint);
      if (Mutation == 0)
        Wrong->Signature.ReturnComponents.pop_back();
      else if (Mutation == 1)
        std::swap(Wrong->Signature.ReturnComponents[0],
                  Wrong->Signature.ReturnComponents[1]);
      else if (Mutation == 2)
        Wrong->CallKind = SourceCallTypeHint::Kind::SwiftStringBridge;
      else
        Wrong->TargetName += "_suffix";
      Changed.SourceCallHint = std::move(Wrong);
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {})) << Mutation;
    }
    Image.DyldBindSlots[0x2180].Name = Name;
    Image.DyldBindSlots[0x2180].WeakImport = true;
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
  }
}

TEST(ObjCCallHints, SwiftStringBridgeRejectsOtherABIsAndUnprovenImports) {
  const std::string Name =
      "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF";
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &Other :
         {Name.substr(1), Name + "_suffix",
          std::string("_$sSS10FoundationE36_"
                      "unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgF"),
          std::string("_swift_retainDirect")}) {
      const auto Image = runtimeImage(Other, Architecture);
      EXPECT_TRUE(buildObjCSourceCallHints(Image, caller(Architecture)).empty())
          << Other;
    }
    for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
      auto Image = runtimeImage(Name, Architecture);
      Image.DyldBindSlots[0x2180] = {
          Name, 0, "/System/Library/Frameworks/Foundation.framework/Foundation",
          false};
      if (Mutation == 0) {
        Image.ImportPtrSlots.clear();
        Image.Symbols.push_back({Name, 0x1100});
      } else if (Mutation == 1)
        Image.IsRelocatable = true;
      else if (Mutation == 2)
        Image.Format = BinaryFormat::ELF;
      else if (Mutation == 3)
        Image.Bits = Bitness::Bits32;
      else if (Mutation == 4)
        Image.Arch = Arch::ARM;
      else if (Mutation == 5)
        Image.ConflictingImportStorageSlots.insert(0x2180);
      else if (Mutation == 6 || Mutation == 7) {
        auto &Binding = Image.ImportStorageSlots[0x2180];
        Binding.Name = Mutation == 6 ? "_different" : Name;
        Binding.Addend = Mutation == 7;
      } else {
        auto &Binding = Image.DyldBindSlots[0x2180];
        Binding.Name = Mutation == 8 ? "_different" : Name;
        Binding.Addend = Mutation == 9;
        Binding.WeakImport = Mutation == 10;
        if (Mutation == 11)
          Binding.Module = "/System/Library/Frameworks/AppKit.framework/AppKit";
      }
      EXPECT_TRUE(buildObjCSourceCallHints(Image, caller(Architecture)).empty())
          << Mutation;
    }
  }
}

TEST(ObjCCallHints,
     DarwinLocksKeepPointerAndBooleanCarriersWithSDKDeclarations) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Name :
         {"os_unfair_lock_lock", "os_unfair_lock_unlock",
          "os_unfair_lock_trylock", "os_unfair_lock_assert_owner",
          "os_unfair_lock_assert_not_owner"}) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(static_cast<int>(Architecture));
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      const auto &Hint = *Call.SourceCallHint;
      EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
      EXPECT_EQ(Hint.TargetAddress, 0x2180U);
      EXPECT_EQ(Hint.TargetName, Name);
      EXPECT_EQ(Hint.Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinRuntime);
      const bool TryLock = std::string(Name) == "os_unfair_lock_trylock";
      EXPECT_EQ(Hint.Signature.ReturnType->Kind,
                TryLock ? NdTypeKind::Int : NdTypeKind::Void);
      ASSERT_EQ(Call.Args.size(), 1U);
      EXPECT_EQ(Call.Args[0].RegOff,
                getTargetRegInfo(Architecture).IntParamRegs.front());
      EXPECT_EQ(Call.Args[0].Size, 8U);
      EXPECT_EQ(Hint.Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
      const auto &Op = Med.Blocks[Call.BlockId].Ops[Call.OpIdx];
      EXPECT_EQ(Op.Output.Size, TryLock ? 1U : 0U);
      EXPECT_EQ(Hint.Signature.ReturnLocation.ExtendTo32Bits,
                TryLock && Architecture == Arch::AArch64);
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      std::string C;
      llvm::raw_string_ostream OS(C);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      EXPECT_NE(C.find("#include <os/lock.h>"), std::string::npos) << C;
      EXPECT_NE(C.find(std::string(Name) + "("), std::string::npos) << C;
      EXPECT_EQ(C.find("extern void os_unfair_lock"), std::string::npos) << C;
      EXPECT_EQ(C.find("extern int os_unfair_lock"), std::string::npos) << C;
      auto Changed = *Expression;
      auto WrongHint = std::make_shared<SourceCallTypeHint>(Hint);
      if (TryLock && Architecture == Arch::AArch64) {
        WrongHint->Signature.ReturnLocation.ExtendTo32Bits = false;
        Changed.SourceCallHint = WrongHint;
        EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {}));
        WrongHint = std::make_shared<SourceCallTypeHint>(Hint);
      }
      WrongHint->TargetName = std::string(Name) + "_unproved";
      Changed.SourceCallHint = std::move(WrongHint);
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {}));
      Image.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
  }
}

TEST(ObjCCallHints, DarwinLocksRejectUnprovenImportsAndNonzeroAddends) {
  for (const char *Name :
       {"os_unfair_lock_lock", "_os_unfair_lock_lock_suffix",
        "_os_unfair_lock_lock_with_options", "_os_unfair_lock_lock_with_flags",
        "_OSSpinLockLock"}) {
    auto Image = runtimeImage(Name);
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty()) << Name;
  }
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    auto Image = runtimeImage("_os_unfair_lock_lock");
    if (Mutation == 0) {
      Image.ImportPtrSlots.clear();
      Image.Symbols.push_back({"_os_unfair_lock_lock", 0x1100});
    } else if (Mutation == 1)
      Image.Segments[0].Data[0x108] ^= 1;
    else if (Mutation == 2)
      Image.IsRelocatable = true;
    else if (Mutation == 3)
      Image.Format = BinaryFormat::ELF;
    else if (Mutation == 4)
      Image.Bits = Bitness::Bits32;
    else if (Mutation == 5)
      Image.Arch = Arch::ARM;
    else if (Mutation == 6 || Mutation == 7) {
      auto &Binding = Image.ImportStorageSlots[0x2180];
      Binding.Name = Mutation == 6 ? "_different" : "_os_unfair_lock_lock";
      Binding.Addend = Mutation == 7;
    } else {
      auto &Binding = Image.DyldBindSlots[0x2180];
      Binding.Name = "_os_unfair_lock_lock";
      Binding.Addend = Mutation == 8;
      Binding.WeakImport = Mutation == 9;
    }
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty()) << Mutation;
  }
}

TEST(ObjCCallHints,
     BlockRuntimeKeepsExactCNamesPointerCarriersAndOwnershipFlags) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (llvm::StringRef Name :
         {"_Block_copy", "_Block_release", "_Block_object_assign",
          "_Block_object_dispose"}) {
      SCOPED_TRACE(Name.str());
      SCOPED_TRACE(static_cast<int>(Architecture));
      auto Image = runtimeImage(("_" + Name).str(), Architecture);
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      const auto &Hint = *Call.SourceCallHint;
      EXPECT_EQ(Hint.TargetName, Name.str());
      EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
      EXPECT_FALSE(Hint.DoesNotReturn);
      const bool Assign = Name == "_Block_object_assign";
      const bool Flags = Assign || Name == "_Block_object_dispose";
      ASSERT_EQ(Hint.Signature.Parameters.size(), Assign  ? 3U
                                                  : Flags ? 2U
                                                          : 1U);
      EXPECT_EQ(Hint.Signature.ReturnType->Kind,
                Name == "_Block_copy" ? NdTypeKind::Ptr : NdTypeKind::Void);
      for (size_t I = 0; I < Hint.Signature.Parameters.size(); ++I) {
        const auto &P = Hint.Signature.Parameters[I];
        const bool IsFlags = Flags && I + 1 == Hint.Signature.Parameters.size();
        EXPECT_EQ(P.Type->Size, IsFlags ? 4U : 8U);
        EXPECT_EQ(P.Type->Kind, IsFlags ? NdTypeKind::Int : NdTypeKind::Ptr);
        EXPECT_EQ(P.Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[I]);
        if (IsFlags)
          EXPECT_TRUE(P.Type->IsSigned);
      }
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      std::string C;
      llvm::raw_string_ostream OS(C);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      EXPECT_NE(C.find("#include <Block.h>"), std::string::npos) << C;
      EXPECT_EQ(C.find("#include <os/lock.h>"), std::string::npos) << C;
      EXPECT_NE(C.find(Name.str() + "("), std::string::npos) << C;
      EXPECT_EQ(C.find("nd__Block_"), std::string::npos) << C;
      EXPECT_EQ(C.find("extern int Block_"), std::string::npos) << C;
      auto Wrong = *Expression;
      auto Binding = std::make_shared<SourceCallTypeHint>(Hint);
      Binding->TargetName = Name.drop_front().str();
      Wrong.SourceCallHint = Binding;
      EXPECT_FALSE(sdk::objcSourceCallBound(Wrong, Image, {}));
      Image.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
  }
}

TEST(ObjCCallHints, StackFailureRetainsItsTerminalRuntimeCall) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool ThroughSlot : {false, true}) {
      auto Image = runtimeImage("___stack_chk_fail", Architecture);
      auto Low = caller(Architecture);
      if (ThroughSlot) {
        Low.Blocks[0].Ops.front().Opcode = NdOp::INDIR_CALL;
        Low.Blocks[0].Ops.front().Inputs[0] = NdVar::cst(0x2180, 8);
      }
      const auto Med = convert(Image, Low);
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      ASSERT_TRUE(Med.CallInfos.front().SourceCallHint);
      const auto &Hint = *Med.CallInfos.front().SourceCallHint;
      EXPECT_EQ(Hint.TargetName, "__stack_chk_fail");
      EXPECT_TRUE(Hint.DoesNotReturn);
      EXPECT_TRUE(Hint.Signature.Parameters.empty());
      EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      auto High = Converter.convert(Med, Architecture);
      const auto *Call = sourceCall(High);
      ASSERT_NE(Call, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      EXPECT_NE(Source.find("__stack_chk_fail();"), std::string::npos)
          << Source;
      EXPECT_NE(Source.find("__attribute__((noreturn))"), std::string::npos);
      EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
      auto Forged = *Call;
      auto ChangedHint = std::make_shared<SourceCallTypeHint>(Hint);
      ChangedHint->DoesNotReturn = false;
      Forged.SourceCallHint = ChangedHint;
      EXPECT_FALSE(sdk::objcSourceCallBound(Forged, Image, {}));
      Image.DyldBindSlots[0x2180].Name = "___stack_chk_fail";
      Image.DyldBindSlots[0x2180].WeakImport = true;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
    }
  }
}

TEST(ObjCCallHints, StackGuardAddressRequiresExactRuntimeIdentity) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("___stack_chk_guard", Architecture);
    HighFunc Function;
    Function.Name = "guard_address";
    Function.ReturnType = NdType::makePtr(NdType::makeVoid());
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.Addr = 0x1200;
    Return.RetVal =
        HighExpr::makeLoad(HighExpr::makeConst(0x2180, 8), NdType::makeInt(8));
    Function.Body = {Return};
    auto Bound = sdk::bindObjCSourceReferences(Function, Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto *Address = sourceCall(Bound.Function);
    ASSERT_NE(Address, nullptr);
    EXPECT_EQ(Address->SourceCallHint->TargetName, "__stack_chk_guard");
    EXPECT_TRUE(sdk::objcSourceCallBound(*Address, Image, {}));
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
    EXPECT_NE(Source.find("extern long __stack_chk_guard[8];"),
              std::string::npos)
        << Source;
    EXPECT_EQ(Source.find("0x2180"), std::string::npos);
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.ImportPtrSlots[0x2180] = "___stack_chk_guard_suffix";
      else if (Mutation == 1)
        Changed.ImportPtrSlots.clear();
      else if (Mutation == 2)
        Changed.IsRelocatable = true;
      else if (Mutation == 3)
        Changed.ConflictingImportStorageSlots.insert(0x2180);
      else {
        auto &Binding = Changed.DyldBindSlots[0x2180];
        Binding.Name = Mutation == 4 ? "_different" : "___stack_chk_guard";
        Binding.Addend = Mutation == 5;
        Binding.WeakImport = Mutation == 6;
        if (Mutation == 7)
          Changed.Format = BinaryFormat::ELF;
      }
      EXPECT_FALSE(sdk::objcSourceCallBound(*Address, Changed, {}));
      EXPECT_FALSE(
          sdk::bindObjCSourceReferences(Function, Changed).Limitation.empty());
    }
    for (bool Ordered : {false, true}) {
      auto Copy = Function;
      Copy.Body.front().RetVal = HighExpr::makeLoad(
          HighExpr::makeConst(0x2180, 8), NdType::makeInt(Ordered ? 8 : 4),
          Ordered ? NdMemoryOrdering::Acquire : NdMemoryOrdering::None);
      EXPECT_FALSE(
          sdk::bindObjCSourceReferences(Copy, Image).Limitation.empty());
    }
  }
}

TEST(ObjCCallHints, AssociatedObjectImportsPreserveKeyValueAndPolicyCarriers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (const auto &[Name, Count] :
         {std::pair{"objc_getAssociatedObject", 2U},
          std::pair{"objc_setAssociatedObject", 4U},
          std::pair{"objc_removeAssociatedObjects", 1U}}) {
      SCOPED_TRACE(Name);
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      ASSERT_EQ(Call.Args.size(), Count);
      const auto &Hint = Call.SourceCallHint->Signature;
      const auto &TRI = getTargetRegInfo(Architecture);
      for (size_t I = 0; I < Count; ++I) {
        EXPECT_EQ(Call.Args[I].RegOff, TRI.IntParamRegs[I]);
        EXPECT_EQ(Call.Args[I].Size, 8U);
        EXPECT_EQ(Hint.Parameters[I].Type->Kind,
                  I == 3 ? NdTypeKind::Int : NdTypeKind::Ptr);
      }
      if (Count == 4)
        EXPECT_FALSE(Hint.Parameters[3].Type->IsSigned);
      EXPECT_EQ(Hint.ReturnType->Kind,
                Count == 2 ? NdTypeKind::Ptr : NdTypeKind::Void);
      const auto &Op = Med.Blocks[Call.BlockId].Ops[Call.OpIdx];
      EXPECT_EQ(Op.Output.Size, Count == 2 ? 8U : 0U);
      Image.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(objcRuntimeSourceCallHint(Image, 0x2180));
    }
  }
}

TEST(ObjCCallHints, RegisterSpecificRuntimeCallsReadTheNamedRegister) {
  for (unsigned Register : {0, 1, 8, 15, 19, 20, 28}) {
    for (llvm::StringRef Operation : {"retain", "release"}) {
      const auto Name =
          "_objc_" + Operation.str() + "_x" + std::to_string(Register);
      auto Image = runtimeImage(Name);
      const auto Med = convert(Image, caller());
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      ASSERT_EQ(Call.Args.size(), 1U);
      EXPECT_EQ(Call.Args[0].RegOff, Register * 8U);
      EXPECT_EQ(Call.TargetName, "objc_" + Operation.str());
      const auto &Op = Med.Blocks[Call.BlockId].Ops[Call.OpIdx];
      EXPECT_EQ(Op.Output.Size, Operation == "retain" ? 8 : 0);
    }
  }
}

TEST(ObjCCallHints, RuntimeBindingsRequireExactImportedIdentityAndABI) {
  for (llvm::StringRef Name :
       {"_objc_retain_x16", "_objc_retain_x18", "_objc_retain_x29",
        "_objc_retain_x01", "_objc_release_x20_extra", "_objc_retainFake"}) {
    auto Image = runtimeImage(Name);
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty())
        << Name.str();
  }
  auto Image = runtimeImage("_objc_retain_x19", Arch::X64);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller(Arch::X64)).empty());
  Image = runtimeImage("_objc_retain");
  Image.ConflictingImportStorageSlots.insert(0x2180);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.ConflictingImportStorageSlots.clear();
  Image.ImportPtrSlots.clear();
  Symbol Symbol;
  Symbol.Name = "_objc_retain";
  Symbol.Addr = 0x1100;
  Symbol.IsFunc = true;
  Image.Symbols.push_back(Symbol);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
}

TEST(ObjCCallHints, RuntimeIndirectCallUsesTheLoadedImportSlot) {
  auto Image = runtimeImage("_objc_retain_x19");
  auto Low = caller();
  auto &Ops = Low.Blocks[0].Ops;
  Ops[0].Opcode = NdOp::INDIR_CALL;
  Ops[0].Inputs[0] = NdVar::reg(16 * 8, 8);
  Ops.insert(Ops.begin(), operation(NdOp::LOAD, NdVar::reg(16 * 8, 8),
                                    {NdVar::cst(0x2180, 8)}, 0x11fc));
  const auto Hints = buildObjCSourceCallHints(Image, Low);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.begin()->second.TargetAddress, 0x2180U);
  EXPECT_EQ(
      Hints.begin()->second.Signature.Parameters[0].Location.RegisterOffset,
      19 * 8U);
  Ops.insert(Ops.begin() + 1, operation(NdOp::COPY, NdVar::reg(16 * 8, 4),
                                        {NdVar::cst(0, 4)}, 0x11fe));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Low).empty());
}

TEST(ObjCCallHints, InlinedColdArgumentsKeepTheirBranchEntryAndRuntimeCall) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    auto Image = runtimeImage("_objc_release", Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto First = NdVar::reg(TRI.IntParamRegs[0], 8);
    const auto Second = NdVar::reg(TRI.IntParamRegs[1], 8);
    const auto Result = NdVar::reg(TRI.IntReturnReg, 8);
    LowFunc Low;
    Low.Entry = 0x1200;
    Low.Name = "cold_release";
    Low.Blocks.resize(3);
    auto &Entry = Low.Blocks[0];
    Entry.Id = 0;
    Entry.StartAddr = 0x1200;
    Entry.EndAddr = 0x1204;
    Entry.Succs = {1, 2};
    Entry.Ops = {
        operation(NdOp::COND_BR, {}, {NdVar::cst(0x1220, 8), First}, 0x1200)};
    auto &Join = Low.Blocks[1];
    Join.Id = 1;
    Join.StartAddr = 0x1204;
    Join.EndAddr = 0x120c;
    Join.Preds = {0, 2};
    Join.Ops = {operation(NdOp::COPY, Result, {NdVar::cst(42, 8)}, 0x1204),
                operation(NdOp::RETURN, {}, {Result}, 0x1208)};
    auto &Cold = Low.Blocks[2];
    Cold.Id = 2;
    Cold.StartAddr = 0x1220;
    Cold.EndAddr = 0x122c;
    Cold.Preds = {0};
    Cold.Succs = {1};
    Cold.Ops = {operation(NdOp::COPY, First, {Second}, 0x1220),
                operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1224),
                operation(NdOp::BRANCH, {}, {NdVar::cst(0x1204, 8)}, 0x1228)};
    const auto Med = convert(Image, Low);
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Image.Arch);
    const auto *Call = sourceCall(High);
    ASSERT_NE(Call, nullptr) << "The reachable release must survive DCE";
    EXPECT_EQ(Call->SourceCallHint->TargetName, "objc_release");
    std::set<va_t> Targets;
    std::map<va_t, unsigned> Entries;
    walkStmts(High.Body, [&](const HighStmt &Statement) {
      if (Statement.Kind == StmtKind::Goto)
        Targets.insert(Statement.GotoTarget);
      if (Statement.Addr)
        ++Entries[Statement.Addr];
    });
    for (va_t Target : Targets)
      EXPECT_EQ(Entries[Target], 1U)
          << "Missing or duplicated source branch entry";
  }
}

namespace {
uint64_t preservedFactRegister(Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  for (auto Register : TRI.CalleeSaveRegs)
    if (!TRI.isFrameOrLinkReg(Register) && TRI.isCallPreserved(Register, 8))
      return Register;
  ADD_FAILURE() << "The Darwin ABI requires a preserved integer register";
  return 0;
}

LowFunc callFactDiamond(Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  const auto Carrier = NdVar::reg(preservedFactRegister(Architecture), 8);
  LowFunc Function;
  Function.Entry = 0x1200;
  Function.Blocks.resize(4);
  for (unsigned I = 0; I < 4; ++I) {
    auto &Block = Function.Blocks[I];
    Block.Id = I;
    Block.StartAddr = 0x1200 + I * 16;
    Block.EndAddr = Block.StartAddr + 12;
  }
  Function.Blocks[0].Succs = {1, 2};
  Function.Blocks[0].Ops = {
      operation(NdOp::COPY, Carrier, {NdVar::cst(0x2100, 8)})};
  for (unsigned I : {1U, 2U}) {
    Function.Blocks[I].Preds = {0};
    Function.Blocks[I].Succs = {3};
  }
  auto &Join = Function.Blocks[3];
  Join.Preds = {1, 2};
  Join.Ops = {
      operation(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8), {Carrier},
                0x1230),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1234),
      operation(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 4)}, 0x1238)};
  return Function;
}
} // namespace

TEST(ObjCCallHints, EqualPredecessorFactsBindRegardlessOfBlockOrder) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Image = image(Architecture);
    auto Base = callFactDiamond(Architecture);
    // One path recomputes the same address instead of forwarding its identity.
    const auto Carrier = NdVar::reg(preservedFactRegister(Architecture), 8);
    Base.Blocks[1].Ops = {
        operation(NdOp::INT_ADD, Carrier,
                  {NdVar::cst(0x2000, 8), NdVar::cst(0x100, 8)}, 0x1210)};
    std::vector<unsigned> Order{0, 1, 2, 3};
    do {
      auto Function = Base;
      for (unsigned I = 0; I < 4; ++I)
        Function.Blocks[I] = Base.Blocks[Order[I]];
      const auto Hints = buildObjCSourceCallHints(Image, Function);
      ASSERT_EQ(Hints.size(), 1U);
      EXPECT_EQ(Hints.at(0x1234).Selector, "scale:");
      EXPECT_EQ(Hints.at(0x1234).Signature.Parameters.size(), 3U);
    } while (std::next_permutation(Order.begin(), Order.end()));
    const auto Med = convert(Image, Base);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    ASSERT_TRUE(Med.CallInfos[0].SourceCallHint);
    EXPECT_EQ(Med.CallInfos[0].Args.size(), 3U);
  }
}

TEST(ObjCCallHints, ConflictingMissingAndOverlappingIncomingFactsStayUnknown) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case < 3; ++Case) {
      auto Function = callFactDiamond(Architecture);
      const auto Register = preservedFactRegister(Architecture);
      Function.Blocks[2].Ops = {operation(
          NdOp::COPY, NdVar::reg(Register, Case == 2 ? 4 : 8),
          {Case == 1
               ? NdVar::reg(getTargetRegInfo(Architecture).IntParamRegs[2], 8)
               : NdVar::cst(0x2110, Case == 2 ? 4 : 8)},
          0x1220)};
      EXPECT_TRUE(
          buildObjCSourceCallHints(image(Architecture), Function).empty());
    }
}

TEST(ObjCCallHints, JoinedSelectorsRetainNamesAndImportsRetainSlots) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (bool Import : {false, true})
      for (bool Conflict : {false, true}) {
        auto Image = image(Architecture);
        Image.ObjCSourceReferences[0x2110] = Image.ObjCSourceReferences[0x2100];
        Image.ObjCSourceReferences[0x2110].Name =
            Conflict ? "different:" : "scale:";
        Image.ImportPtrSlots[0x2200] = "_objc_msgSend";
        auto Function = callFactDiamond(Architecture);
        const auto Carrier = NdVar::reg(preservedFactRegister(Architecture), 8);
        Function.Blocks[0].Ops[0] = operation(
            NdOp::LOAD, Carrier, {NdVar::cst(Import ? 0x2180 : 0x2100, 8)});
        Function.Blocks[2].Ops = {operation(
            NdOp::LOAD, Carrier,
            {NdVar::cst(Import ? (Conflict ? 0x2200 : 0x2180) : 0x2110, 8)},
            0x1220)};
        auto &Ops = Function.Blocks[3].Ops;
        if (Import) {
          Ops[0].Inputs[0] = NdVar::cst(0x2100, 8);
          Ops[1].Inputs[0] = Carrier;
        } else {
          Ops[0].Opcode = NdOp::COPY;
        }
        const auto Hints = buildObjCSourceCallHints(Image, Function);
        EXPECT_EQ(Hints.count(0x1234), Conflict ? 0U : 1U);
      }
}

TEST(ObjCCallHints, LoopBackedgesRevokeProvisionalBindingsBeforePublication) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (bool Change : {false, true}) {
      auto Function = callFactDiamond(Architecture);
      // Entry -> header -> body -> header. The first header visit sees only
      // the entry fact; the body can invalidate it on a later iteration.
      Function.Blocks[0].Succs = {3};
      Function.Blocks[3].Preds = {0, 1};
      Function.Blocks[3].Succs = {1};
      Function.Blocks[3].Ops.pop_back();
      Function.Blocks[1].Preds = {3};
      Function.Blocks[1].Succs = {3};
      Function.Blocks[2].Preds.clear();
      Function.Blocks[2].Succs.clear();
      if (Change)
        Function.Blocks[1].Ops = {operation(
            NdOp::COPY, NdVar::reg(preservedFactRegister(Architecture), 8),
            {NdVar::cst(0x2110, 8)}, 0x1210)};
      const auto Hints =
          buildObjCSourceCallHints(image(Architecture), Function);
      EXPECT_EQ(Hints.size(), Change ? 0U : 1U);
    }
}

TEST(ObjCCallHints, IndependentAndExceptionalEntriesEraseInheritedFacts) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case < 5; ++Case) {
      auto Function = callFactDiamond(Architecture);
      if (Case == 0)
        Function.ModuleAnalysisRoots.insert(0x1230);
      else if (Case == 1)
        Function.Entry = 0x1230;
      else if (Case == 2)
        Function.Blocks[3].ExceptionalPreds.emplace_back();
      else if (Case == 3)
        Function.OrdinaryModuleAnalysisRoots.insert(0x1230);
      else {
        auto &Edge = Function.Blocks[1].ExceptionalSuccs.emplace_back();
        Edge.BlockId = 3;
      }
      EXPECT_TRUE(
          buildObjCSourceCallHints(image(Architecture), Function).empty());
    }
}

TEST(ObjCCallHints, CallsPreserveOnlyProvenABIViewsAcrossJoins) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case < 3; ++Case) {
      const bool Known = Case == 1;
      auto Image = image(Architecture);
      Image.ImportPtrSlots[0x2200] =
          Known ? "_objc_release" : "_unmodeled_call";
      auto Function = callFactDiamond(Architecture);
      Function.Blocks[1].Ops = {
          operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2200, 8)}, 0x1210)};
      if (Case == 2)
        Function.Blocks[1].Ops[0].Opcode = NdOp::INTRINSIC;
      const auto Hints = buildObjCSourceCallHints(Image, Function);
      EXPECT_EQ(Hints.count(0x1234), Known ? 1U : 0U);
    }
}

TEST(ObjCCallHints, InstructionTemporariesNeverBecomeCrossBlockFacts) {
  auto Function = callFactDiamond(Arch::AArch64);
  const auto Temporary = NdVar::tmp(77, 8);
  Function.Blocks[0].Ops[0].Output = Temporary;
  Function.Blocks[3].Ops[0].Inputs[0] = Temporary;
  EXPECT_TRUE(buildObjCSourceCallHints(image(), Function).empty());
}

TEST(ObjCCallHints, MalformedEdgesAndDuplicateCallSitesDoNotSupplyBindings) {
  for (unsigned Case = 0; Case < 5; ++Case) {
    auto Function = callFactDiamond(Arch::AArch64);
    if (Case == 0)
      Function.Blocks[3].Preds.push_back(77);
    else if (Case == 1)
      Function.Blocks[0].Succs.pop_back();
    else if (Case == 2)
      Function.Blocks[0].Succs.push_back(77);
    else if (Case == 3)
      Function.Blocks[2].Id = 1;
    else
      Function.Blocks[3].Ops.insert(Function.Blocks[3].Ops.begin() + 2,
                                    Function.Blocks[3].Ops[1]);
    EXPECT_TRUE(buildObjCSourceCallHints(image(), Function).empty());
  }
}

TEST(ObjCCallHints, LongReorderedChainsUseAnIterativeProof) {
  auto Function = callFactDiamond(Arch::AArch64);
  auto Entry = Function.Blocks.front();
  auto Exit = Function.Blocks.back();
  Function.Blocks.clear();
  Entry.Succs = {1};
  Function.Blocks.push_back(Entry);
  for (unsigned I = 1; I < 512; ++I) {
    LowBlock Block;
    Block.Id = I;
    Block.StartAddr = 0x2000 + I * 4;
    Block.Preds = {int(I - 1)};
    Block.Succs = {int(I + 1)};
    Function.Blocks.push_back(Block);
  }
  Exit.Id = 512;
  Exit.Preds = {511};
  Function.Blocks.push_back(Exit);
  std::reverse(Function.Blocks.begin(), Function.Blocks.end());
  const auto Hints = buildObjCSourceCallHints(image(), Function);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.at(0x1234).Selector, "scale:");
  Function.Blocks.resize(16385);
  for (size_t I = 0; I < Function.Blocks.size(); ++I)
    Function.Blocks[I].Id = I;
  EXPECT_TRUE(buildObjCSourceCallHints(image(), Function).empty());
}

TEST(ObjCCallHints, FactBudgetNeverPublishesAnEarlierPartialBinding) {
  auto Function = caller();
  // A binding discovered before an oversized instruction must not escape a
  // failed proof. The temporary facts share one instruction's lifetime.
  auto &Ops = Function.Blocks.front().Ops;
  Ops.pop_back();
  for (unsigned I = 0; I < 4097; ++I)
    Ops.push_back(operation(NdOp::COPY, NdVar::tmp(I * 8, 8),
                            {NdVar::cst(I, 8)}, 0x1300));
  Ops.push_back(operation(NdOp::RETURN, {}, {}, 0x1304));
  EXPECT_TRUE(buildObjCSourceCallHints(image(), Function).empty());
}

TEST(ObjCCallHints, UnresolvedFormatImportsDoNotInheritMessageDispatch) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    Image.ImportPtrSlots[0x2180] = "_NSLog";
    ASSERT_TRUE(Image.recordDyldBindSlot(
        0x2180, "_NSLog", 0,
        "/System/Library/Frameworks/Foundation.framework/Foundation", false));
    ASSERT_TRUE(darwinRuntimeFormatDeclaration(Image, 0x2180));
    auto Function = callFactDiamond(Architecture);
    // The format argument is unknown. A selector-shaped value reaching the
    // second register does not change the identity of the imported callee.
    EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
  }
}

TEST(ObjCCallHints, SnprintfBindsOnlyFromItsProvenCFormatRegister) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.ImportPtrSlots[0x2180] = "_snprintf";
    Image.DyldBindSlots[0x2180] = {"_snprintf", 0,
                                   "/usr/lib/system/libsystem_c.dylib", false};
    const std::string Format = "%02x";
    std::copy(Format.begin(), Format.end(),
              Image.Segments[0].Data.begin() + 0x1200);
    Image.Segments[0].Data[0x1200 + Format.size()] = 0;
    const auto &TRI = getTargetRegInfo(Architecture);
    LowFunc Function;
    Function.Entry = 0x1200;
    Function.Blocks.resize(1);
    auto &Block = Function.Blocks.front();
    Block.StartAddr = 0x1200;
    Block.Ops = {
        operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[2], 8),
                  {NdVar::cst(0x2200, 8)}, 0x1200),
        operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1204),
        operation(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 4)}, 0x1208)};
    const auto Hints = buildObjCSourceCallHints(Image, Function);
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Hint = Hints.at(0x1204);
    EXPECT_EQ(Hint.TargetName, "snprintf");
    ASSERT_TRUE(Hint.Format);
    EXPECT_EQ(Hint.Format->FormatAddress, 0x2200U);
    EXPECT_EQ(Hint.Format->Syntax, SourceCallTypeHint::FormatSyntax::Printf);
    ASSERT_EQ(Hint.Signature.Parameters.size(), 4U);

    Function.Blocks[0].Ops[0].Inputs[0] = NdVar::reg(TRI.IntParamRegs[3], 8);
    EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
  }
}

TEST(ObjCCallHints, RuntimeVoidAndWeakSignaturesDoNotInventResults) {
  auto Image = runtimeImage("_objc_storeStrong");
  const auto Hints = buildObjCSourceCallHints(Image, caller());
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Signature = Hints.begin()->second.Signature;
  EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Signature.Parameters.size(), 2U);
  EXPECT_EQ(Signature.Parameters[0].Type->Pointee->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Signature.Parameters[1].Type->Pointee->Kind, NdTypeKind::Void);
  Image.ImportPtrSlots[0x2180] = "_objc_autoreleasePoolPush";
  const auto Pool = buildObjCSourceCallHints(Image, caller());
  ASSERT_EQ(Pool.size(), 1U);
  EXPECT_TRUE(Pool.begin()->second.Signature.Parameters.empty());
  EXPECT_EQ(Pool.begin()->second.Signature.ReturnType->Kind, NdTypeKind::Ptr);
}

TEST(ObjCCallHints, EnumerationMutationKeepsItsObjectAndReturningContinuation) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("_objc_enumerationMutation", Architecture);
    auto Low = caller(Architecture);
    auto &Block = Low.Blocks[0];
    Block.Ops.insert(
        Block.Ops.end() - 1,
        operation(NdOp::COPY,
                  NdVar::reg(getTargetRegInfo(Architecture).IntReturnReg, 4),
                  {NdVar::cst(73, 4)}, 0x1204));
    Block.Ops.back().Addr = 0x1208;
    Block.EndAddr = 0x120c;
    const auto Hints = buildObjCSourceCallHints(Image, Low);
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Signature = Hints.begin()->second.Signature;
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Signature.Parameters.size(), 1U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    const auto Med = convert(Image, Low);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    EXPECT_EQ(Med.Blocks[Call.BlockId].Ops[Call.OpIdx].Output.Size, 0U);
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Architecture);
    ASSERT_NE(sourceCall(High), nullptr);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    EXPECT_NE(Source.find("#include <objc/runtime.h>"), std::string::npos);
    EXPECT_NE(Source.find("objc_enumerationMutation("), std::string::npos);
    bool ReturnsMarker = false;
    walkStmts(High.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Return)
        return;
      std::vector<ExprPtr> Work{S.RetVal};
      while (!Work.empty()) {
        auto E = Work.back();
        Work.pop_back();
        if (!E)
          continue;
        ReturnsMarker |= E->Kind == ExprKind::Const && E->ConstVal == 73;
        Work.insert(Work.end(), E->Operands.begin(), E->Operands.end());
      }
    });
    EXPECT_TRUE(ReturnsMarker);
  }
}

TEST(ObjCCallHints,
     RuntimeImportsRejectContradictoryBindingsAndWeakIdentities) {
  for (const auto *Name : {"_objc_enumerationMutation", "_objc_retain"})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      auto Image = runtimeImage(Name);
      if (Mutation < 2) {
        auto &Binding = Image.ImportStorageSlots[0x2180];
        Binding.Name = Mutation == 0 ? "_different" : Name;
        Binding.Addend = Mutation == 1;
      } else {
        auto &Binding = Image.DyldBindSlots[0x2180];
        Binding.Name = Mutation == 2 ? "_different" : Name;
        Binding.Addend = Mutation == 3;
        Binding.WeakImport = Mutation == 4;
      }
      EXPECT_FALSE(objcRuntimeSourceCallHint(Image, 0x2180))
          << Name << ':' << Mutation;
    }
}

TEST(ObjCCallHints, PropertyGetterKeepsSignedOffsetAndPlatformBoolABI) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("_objc_getProperty", Architecture);
    EXPECT_FALSE(objcRuntimeSourceCallHint(Image, 0x2180));
    auto &Binding = Image.DyldBindSlots[0x2180];
    Binding.Name = "_objc_getProperty";
    Binding.Module = "/usr/lib/libobjc.A.dylib";
    const auto Hint = objcRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    const auto &Signature = Hint->Signature;
    ASSERT_EQ(Signature.Parameters.size(), 4U);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[2].Type->Size, 8U);
    EXPECT_TRUE(Signature.Parameters[2].Type->IsSigned);
    EXPECT_EQ(Signature.Parameters[3].Type->Size, 1U);
    EXPECT_EQ(Signature.Parameters[3].Type->IsSigned,
              Architecture == Arch::X64);
    EXPECT_EQ(Signature.Parameters[3].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[3]);
    Binding.Module = "/tmp/libobjc.A.dylib";
    EXPECT_FALSE(objcRuntimeSourceCallHint(Image, 0x2180));
  }
}

TEST(ObjCCallHints, PropertyRuntimeKeepsValueBeforeSignedOffset) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("_objc_setProperty_nonatomic_copy", Architecture);
    const auto Hints = buildObjCSourceCallHints(Image, caller(Architecture));
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Signature = Hints.begin()->second.Signature;
    ASSERT_EQ(Signature.Parameters.size(), 4U);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    EXPECT_EQ(Signature.Parameters[2].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[3].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.Parameters[3].Type->Size, 8U);
    EXPECT_TRUE(Signature.Parameters[3].Type->IsSigned);
    EXPECT_EQ(Signature.Parameters[3].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[3]);
  }
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
  Image.Sections[0].Name = "__objc_stubs";
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
  MedToHighConverter HighConverter;
  HighConverter.setBinaryImage(&Image);
  const auto High = HighConverter.convert(Med, Image.Arch);
  const auto *SourceCall = sourceCall(High);
  ASSERT_NE(SourceCall, nullptr);
  ASSERT_EQ(SourceCall->Operands.size(), 3U);
  EXPECT_EQ(SourceCall->Operands[0]->Kind, ExprKind::Var);
  EXPECT_EQ(SourceCall->Operands[0]->Var.Kind, MedVar::Param);
  EXPECT_EQ(SourceCall->Operands[0]->Var.RegOff, 0U);
  EXPECT_FALSE(SourceCall->Operands[1]->Kind == ExprKind::Const &&
               SourceCall->Operands[1]->ConstVal == 0);
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
  Block.StartAddr = Function.Entry;
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
  Block.EndAddr = Block.Ops.back().Addr + 4;
  return Function;
}

void expectNativeSelectorCommand(const MedFunc &Function,
                                 const BinaryImage *Image, uint64_t Command) {
  MedToHighConverter Converter;
  Converter.setBinaryImage(Image);
  const auto High = Converter.convert(Function, Arch::AArch64);
  std::vector<const HighExpr *> Work;
  walkStmts(High.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Expression) {
      Work.push_back(Expression.get());
    });
  });
  std::vector<const HighExpr *> Calls;
  while (!Work.empty()) {
    const auto *Expression = Work.back();
    Work.pop_back();
    if (Expression->Kind == ExprKind::Call && Expression->CallAddr == 0x1100)
      Calls.push_back(Expression);
    for (const auto &Operand : Expression->Operands)
      if (Operand)
        Work.push_back(Operand.get());
  }
  ASSERT_EQ(Calls.size(), 1U);
  const auto &Call = *Calls.front();
  EXPECT_FALSE(Call.SourceCallHint);
  EXPECT_FALSE(Call.IsIndirectCall);
  ASSERT_EQ(Call.Operands.size(), 3U);
  const uint64_t Expected[] = {0x2222, Command, 0x123456};
  for (size_t I = 0; I < 3; ++I) {
    ASSERT_NE(Call.Operands[I], nullptr);
    ASSERT_EQ(Call.Operands[I]->Kind, ExprKind::Const);
    EXPECT_EQ(Call.Operands[I]->ConstVal, Expected[I]);
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.EmitComments = false;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  const std::string ExpectedCall = Command == 0
                                       ? "sub_1100(0x2222, 0, 0x123456)"
                                       : "sub_1100(0x2222, 0x777777, 0x123456)";
  EXPECT_NE(Source.find(ExpectedCall), std::string::npos) << Source;
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
    expectNativeSelectorCommand(callerWithStaleCommand(false), &Image, 0);
  }
}

TEST(ObjCCallHints, DynamicFormatStubBindsEmptyAndProvenPointerTails) {
  auto Image = selectorStubImage();
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/Foundation.framework/Foundation");
  Image.ObjCSourceReferences.at(0x2100).Name = "localizedStringWithFormat:";
  const auto Declared =
      objcSelectorStubDynamicFormatSourceCallHint(Image, 0x1100);
  ASSERT_TRUE(Declared);
  ASSERT_TRUE(Declared->Format);
  EXPECT_TRUE(Declared->Format->DynamicWithoutArguments);
  EXPECT_EQ(Declared->Format->FixedCount, 3U);
  EXPECT_EQ(Declared->Format->FormatParameter, 2U);
  EXPECT_EQ(Declared->Format->FormatAddress, 0U);
  EXPECT_TRUE(Declared->Format->AlternativeFormatAddresses.empty());
  EXPECT_EQ(Declared->SelectorReferenceAddress, 0x2100U);

  auto Native = std::make_shared<SourceCallTypeHint>(*Declared);
  Native->CallKind = SourceCallTypeHint::Kind::Native;
  Native->TargetName = "sub_1100";
  Native->Selector.clear();
  Native->SelectorReferenceAddress = 0;
  Native->Format.reset();
  Native->Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Native->Signature.ReturnType = NdType::makeInt(8, false);
  for (auto &Parameter : Native->Signature.Parameters)
    Parameter.Type = NdType::makeInt(8, false);

  MedVar DynamicFormat;
  DynamicFormat.Kind = MedVar::Param;
  DynamicFormat.Id = 0;
  DynamicFormat.Size = 8;
  DynamicFormat.TheArch = Arch::AArch64;
  auto MakeCall = [&] {
    auto Call = HighExpr::makeCall(
        Native->TargetName, Native->TargetAddress,
        {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
         HighExpr::makeVar(DynamicFormat, NdType::makeInt(8, false))});
    Call->Type = Native->Signature.ReturnType;
    Call->SourceCallHint = Native;
    HighFunc Function;
    Function.ReturnType = NdType::makePtr(NdType::makeVoid());
    Function.Params = {{"format", NdType::makePtr(NdType::makeVoid())}};
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body = {Return};
    return Function;
  };

  auto Function = MakeCall();
  auto Bound = sdk::bindObjCSourceReferences(Function, Image);
  EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  const auto &Call = Bound.Function.Body.front().RetVal;
  ASSERT_TRUE(Call && Call->SourceCallHint);
  EXPECT_EQ(Call->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::ObjCMessage);
  EXPECT_TRUE(Call->SourceCallHint->Format->DynamicWithoutArguments);
  EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}, nullptr, nullptr,
                                       &Bound.Function));

  // A raw direct call may not have a native source hint: selector stubs are
  // not ordinary function entries. The exact stub plus every fixed scalar
  // carrier is sufficient physical proof for the zero-tail call.
  Function = MakeCall();
  Function.Body.front().RetVal->SourceCallHint.reset();
  Function.Body.front().RetVal->Type.reset();
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  const auto &RawCall = Bound.Function.Body.front().RetVal;
  ASSERT_TRUE(RawCall && RawCall->SourceCallHint);
  EXPECT_EQ(RawCall->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::ObjCMessage);
  EXPECT_TRUE(RawCall->SourceCallHint->Format->DynamicWithoutArguments);

  // A dynamic format can also preserve a nonempty variadic tail when every
  // actual value already has a source pointer type. No format-text type is
  // inferred; the pointer type itself proves the promoted physical carrier.
  const auto PointerTail = objcDynamicFormatPointerArgumentsSourceCallHint(
      Image, "localizedStringWithFormat:", 2);
  ASSERT_TRUE(PointerTail);
  ASSERT_TRUE(PointerTail->Format);
  EXPECT_FALSE(PointerTail->Format->DynamicWithoutArguments);
  EXPECT_TRUE(PointerTail->Format->DynamicPointerArguments);
  ASSERT_EQ(PointerTail->Signature.Parameters.size(), 5U);
  EXPECT_EQ(PointerTail->Signature.Parameters[3].Location.Kind,
            SourceABICarrierKind::Stack);
  EXPECT_EQ(PointerTail->Signature.Parameters[3].Location.EntryStackOffset, 0);
  EXPECT_EQ(PointerTail->Signature.Parameters[4].Location.EntryStackOffset, 8);

  Function = MakeCall();
  auto &PointerCall = Function.Body.front().RetVal;
  auto PointerNative = std::make_shared<SourceCallTypeHint>(*Native);
  PointerNative->Signature = PointerTail->Signature;
  PointerNative->Signature.Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  for (auto &Parameter : PointerNative->Signature.Parameters)
    Parameter.Type = NdType::makeInt(8, false);
  PointerCall->SourceCallHint = PointerNative;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  MedVar FirstPointer = DynamicFormat;
  FirstPointer.Id = 1;
  MedVar SecondPointer = DynamicFormat;
  SecondPointer.Id = 2;
  PointerCall->Operands.push_back(HighExpr::makeVar(FirstPointer, Pointer));
  PointerCall->Operands.push_back(HighExpr::makeVar(SecondPointer, Pointer));
  Function.Params.push_back({"first", Pointer});
  Function.Params.push_back({"second", Pointer});
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  const auto &BoundPointerCall = Bound.Function.Body.front().RetVal;
  ASSERT_TRUE(BoundPointerCall && BoundPointerCall->SourceCallHint);
  EXPECT_EQ(BoundPointerCall->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::ObjCMessage);
  ASSERT_TRUE(BoundPointerCall->SourceCallHint->Format);
  EXPECT_TRUE(
      BoundPointerCall->SourceCallHint->Format->DynamicPointerArguments);
  EXPECT_TRUE(sdk::objcSourceCallBound(*BoundPointerCall, Image, {}, nullptr,
                                       nullptr, &Bound.Function));

  PointerCall->SourceCallHint.reset();
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  ASSERT_TRUE(Bound.Function.Body.front().RetVal->SourceCallHint);
  EXPECT_EQ(Bound.Function.Body.front().RetVal->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::ObjCMessage);

  // Native SSA can retain a scalar spelling after an authenticated ARC call
  // has proved that the value is an Objective-C pointer. Follow the complete
  // definition family instead of treating every 64-bit scalar as a pointer.
  constexpr va_t RetainSlot = 0x2188;
  Image.ImportPtrSlots[RetainSlot] = "_objc_retainAutoreleasedReturnValue";
  Image.DyldBindSlots[RetainSlot] = {"_objc_retainAutoreleasedReturnValue", 0,
                                     "/usr/lib/libobjc.A.dylib", false};
  const auto RetainHint = objcRuntimeSourceCallHint(Image, RetainSlot);
  ASSERT_TRUE(RetainHint);
  Function = MakeCall();
  auto &ProvenCall = Function.Body.front().RetVal;
  const auto OnePointerTail = objcDynamicFormatPointerArgumentsSourceCallHint(
      Image, "localizedStringWithFormat:", 1);
  ASSERT_TRUE(OnePointerTail);
  auto ProvenNative = std::make_shared<SourceCallTypeHint>(*Native);
  ProvenNative->Signature = OnePointerTail->Signature;
  ProvenNative->Signature.Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  for (auto &Parameter : ProvenNative->Signature.Parameters)
    Parameter.Type = NdType::makeInt(8, false);
  ProvenCall->SourceCallHint = ProvenNative;
  MedVar Retained = DynamicFormat;
  Retained.Kind = MedVar::Temp;
  Retained.Id = 3;
  auto RetainedValue = HighExpr::makeVar(Retained, NdType::makeInt(8, false));
  ProvenCall->Operands.push_back(RetainedValue);
  auto Retain = HighExpr::makeCall("objc_retainAutoreleasedReturnValue",
                                   RetainSlot, {HighExpr::makeConst(0, 8)});
  Retain->Type = NdType::makeInt(8, false);
  Retain->SourceCallHint = std::make_shared<SourceCallTypeHint>(*RetainHint);
  HighStmt DefineRetained;
  DefineRetained.Kind = StmtKind::Assign;
  DefineRetained.Dst = RetainedValue;
  DefineRetained.Val = Retain;
  Function.Body.insert(Function.Body.begin(), DefineRetained);
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  const auto &ProvenPointerCall = Bound.Function.Body.back().RetVal;
  ASSERT_TRUE(ProvenPointerCall && ProvenPointerCall->SourceCallHint);
  EXPECT_EQ(ProvenPointerCall->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::ObjCMessage);
  EXPECT_TRUE(sdk::objcSourceCallBound(*ProvenPointerCall, Image, {}, nullptr,
                                       nullptr, &Bound.Function));

  HighStmt ConflictingDefinition = DefineRetained;
  ConflictingDefinition.Val = HighExpr::makeConst(7, 8);
  Function.Body.insert(Function.Body.begin(), ConflictingDefinition);
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  ASSERT_TRUE(Bound.Function.Body.back().RetVal->SourceCallHint);
  EXPECT_EQ(Bound.Function.Body.back().RetVal->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::Native);

  Function = MakeCall();
  auto &IntegerTailCall = Function.Body.front().RetVal;
  IntegerTailCall->SourceCallHint = PointerNative;
  IntegerTailCall->Operands.push_back(HighExpr::makeVar(FirstPointer, Pointer));
  IntegerTailCall->Operands.push_back(HighExpr::makeConst(7, 8));
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  ASSERT_TRUE(Bound.Function.Body.front().RetVal->SourceCallHint);
  EXPECT_EQ(Bound.Function.Body.front().RetVal->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::Native);
  EXPECT_FALSE(objcDynamicFormatPointerArgumentsSourceCallHint(
      Image, "localizedStringWithFormat:", 62));

  Function = MakeCall();
  Function.Body.front().RetVal->SourceCallHint.reset();
  Function.Body.front().RetVal->Type = NdType::makeFloat(8);
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  EXPECT_FALSE(Bound.Function.Body.front().RetVal->SourceCallHint);

  Function = MakeCall();
  Function.Body.front().RetVal->SourceCallHint.reset();
  Function.Body.front().RetVal->Operands[2]->Type = NdType::makeFloat(8);
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  EXPECT_FALSE(Bound.Function.Body.front().RetVal->SourceCallHint);

  Function = MakeCall();
  Function.Body.front().RetVal->SourceCallHint.reset();
  Function.Body.front().RetVal->Operands.push_back(HighExpr::makeConst(7, 8));
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  EXPECT_FALSE(Bound.Function.Body.front().RetVal->SourceCallHint);

  // One untyped tail operand is still too much: without the format object's
  // exact contents there is no promoted source type for an integer value.
  Function = MakeCall();
  Function.Body.front().RetVal->Operands.push_back(HighExpr::makeConst(7, 8));
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  ASSERT_TRUE(Bound.Function.Body.front().RetVal->SourceCallHint);
  EXPECT_EQ(Bound.Function.Body.front().RetVal->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::Native);

  // The selector stub and native carrier proof are both authoritative.
  Function = MakeCall();
  auto WrongABI = std::make_shared<SourceCallTypeHint>(
      *Function.Body.front().RetVal->SourceCallHint);
  WrongABI->Signature.Parameters[2].Location.RegisterOffset += 8;
  Function.Body.front().RetVal->SourceCallHint = WrongABI;
  Bound = sdk::bindObjCSourceReferences(Function, Image);
  ASSERT_TRUE(Bound.Function.Body.front().RetVal->SourceCallHint);
  EXPECT_EQ(Bound.Function.Body.front().RetVal->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::Native);
  Image.ObjCSourceReferences.at(0x2100).Name = "stringWithFormat:";
  EXPECT_FALSE(objcSelectorStubDynamicFormatSourceCallHint(Image, 0x1104));
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
      llvm::support::endian::write32le(Image.Segments[0].Data.data() + 0x104,
                                       0xf9408000); // LDR x0
      break;
    case 15:
      llvm::support::endian::write32le(Image.Segments[0].Data.data() + 0x110,
                                       0xd61f0220); // BR x17
      break;
    }
    EXPECT_FALSE(objcSelectorStubOverwritesCommand(Image, 0x1100));
    expectNativeSelectorCommand(callerWithStaleCommand(false), &Image,
                                0x777777);
  }
}

TEST(ObjCCallHints, VerifiedSelectorStubDiscardsStaleCallerCommand) {
  for (bool Clobbered : {false, true}) {
    SCOPED_TRACE(Clobbered);
    auto Image = selectorStubImage();
    Image.ObjCMethods.clear();
    auto Function = callerWithStaleCommand(Clobbered);
    // The ordinary source pipeline does not run recoverCallAbi. Exercise
    // that route before also checking the independently recovered ABI record.
    expectNativeSelectorCommand(Function, &Image, 0);
    if (!Clobbered)
      expectNativeSelectorCommand(Function, nullptr, 0x777777);
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
    expectNativeSelectorCommand(Function, &Image, 0);
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
    expectNativeSelectorCommand(Function, &Image, 0x777777);
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

TEST(ObjCCallHints, CallOnlyHintDoesNotReplaceCurrentFunctionEntryABI) {
  auto EntryHint = signature(Arch::AArch64, 1);
  std::string Diagnostic;

  SourceFunctionTypeHint CallOnlyHint;
  CallOnlyHint.ReturnType = NdType::makePtr(NdType::makeVoid());
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(CallOnlyHint, Arch::AArch64, Diagnostic))
      << Diagnostic;
  std::map<va_t, SourceFunctionTypeHint> EntryHints{{0x1200, EntryHint}};
  std::map<va_t, SourceFunctionTypeHint> CalleeHints{{0x1200, CallOnlyHint},
                                                     {0x1600, CallOnlyHint}};

  auto Low = caller();
  Low.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(0x1600, 8);
  Low.Blocks[0].Ops.back().Inputs[0] = NdVar::cst(0x2200, 8);
  LowToMedConverter Converter;
  Converter.setSourceEntryTypeHints(&EntryHints);
  Converter.setSourceCalleeTypeHints(&CalleeHints);
  Converter.setSourceCallHintsEnabled(true);
  auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  recoverCallAbi(Med, Arch::AArch64, {});

  ASSERT_EQ(Med.CallInfos.size(), 1U);
  ASSERT_TRUE(Med.CallInfos[0].SourceCallHint);
  EXPECT_TRUE(Med.CallInfos[0].Args.empty());
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  bool FoundEntryReturn = false;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        ASSERT_EQ(Op.NumInputs, 1U);
        EXPECT_EQ(Op.Inputs[0].Kind, MedVar::Reg);
        EXPECT_EQ(Op.Inputs[0].RegOff, TRI.IntReturnReg);
        EXPECT_EQ(Op.Inputs[0].Size, 4U);
        FoundEntryReturn = true;
      }
  EXPECT_TRUE(FoundEntryReturn);
}

TEST(ObjCCallHints, ExplicitVoidEntryDropsGenericMachineReturnCarrier) {
  SourceFunctionTypeHint EntryHint;
  EntryHint.ReturnType = NdType::makeVoid();
  EntryHint.Parameters = {
      {"objc_self", NdType::makePtr(NdType::makeVoid())},
      {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
  std::string Diagnostic;
  ASSERT_TRUE(
      assignDarwinObjCSourceABI(EntryHint, Arch::AArch64, Diagnostic))
      << Diagnostic;
  std::map<va_t, SourceFunctionTypeHint> EntryHints{{0x1200, EntryHint}};

  auto Low = caller();
  Low.Blocks[0].Ops.back().Inputs[0] = NdVar::reg(a64reg::X0, 8);
  LowToMedConverter Converter;
  Converter.setSourceEntryTypeHints(&EntryHints);
  Converter.setSourceCallHintsEnabled(true);
  const auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);

  bool FoundReturn = false;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        EXPECT_EQ(Op.NumInputs, 0U);
        FoundReturn = true;
      }
  EXPECT_TRUE(FoundReturn);

  MedFunc Branching;
  Branching.Entry = 0x1200;
  Branching.Name = "void_fallthrough";
  Branching.ReturnType = NdType::makeVoid();
  Branching.SourceTypeHint = EntryHint;
  Branching.SourceParametersBound = true;
  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = 0x1200;
  Entry.EndAddr = 0x1204;
  Entry.Succs = {1, 2};
  MedOp Cond;
  Cond.Opcode = NdOp::COND_BR;
  Cond.Addr = 0x1200;
  Cond.addInput(MedVar::makeConst(0x1220, 8));
  Cond.addInput(MedVar::makeConst(1, 1));
  Entry.Ops.push_back(Cond);
  MedBlock Returned;
  Returned.Id = 1;
  Returned.StartAddr = 0x1210;
  Returned.EndAddr = 0x1214;
  Returned.Preds = {0};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1210;
  Returned.Ops.push_back(Return);
  MedBlock Fallthrough;
  Fallthrough.Id = 2;
  Fallthrough.StartAddr = 0x1220;
  Fallthrough.EndAddr = 0x1224;
  Fallthrough.Preds = {0};
  MedOp MachineResult;
  MachineResult.Opcode = NdOp::COPY;
  MachineResult.Addr = 0x1220;
  MachineResult.Output.Kind = MedVar::Reg;
  MachineResult.Output.Id = 7;
  MachineResult.Output.SSAVer = 1;
  MachineResult.Output.RegOff = a64reg::X0;
  MachineResult.Output.Size = 8;
  MachineResult.Output.TheArch = Arch::AArch64;
  MachineResult.addInput(MedVar::makeConst(42, 8));
  Fallthrough.Ops.push_back(MachineResult);
  Branching.Blocks = {Entry, Returned, Fallthrough};

  const auto High = MedToHighConverter().convert(Branching, Arch::AArch64);
  ASSERT_FALSE(High.Body.empty());
  ASSERT_EQ(High.Body.back().Kind, StmtKind::Return);
  EXPECT_FALSE(High.Body.back().RetVal);
}

TEST(ObjCCallHints, AArch64NarrowArgumentsUseDefinedWRegisterAcrossJoin) {
  auto Hint = signature(Arch::AArch64, 6);
  Hint.Parameters.back().Type = NdType::makeInt(1, false);
  std::string Diagnostic;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Diagnostic))
      << Diagnostic;
  ASSERT_EQ(Hint.Parameters.back().Location.RegisterOffset, a64reg::X7);
  ASSERT_EQ(Hint.Parameters.back().Location.ValueBytes, 1U);
  std::map<va_t, SourceFunctionTypeHint> Hints{{0x1600, Hint}};

  LowFunc Low;
  Low.Entry = 0x1200;
  Low.Name = "narrow_join_caller";
  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = 0x1200;
  Entry.Succs = {1, 2};
  LowBlock Left;
  Left.Id = 1;
  Left.StartAddr = 0x1210;
  Left.Preds = {0};
  Left.Succs = {3};
  Left.Ops = {operation(NdOp::INT_ZEXT, NdVar::reg(a64reg::X7, 4),
                        {NdVar::cst(0x7f, 1)}, 0x1210)};
  LowBlock Right;
  Right.Id = 2;
  Right.StartAddr = 0x1220;
  Right.Preds = {0};
  Right.Succs = {3};
  Right.Ops = {operation(NdOp::COPY, NdVar::reg(a64reg::X7, 4),
                         {NdVar::cst(0, 4)}, 0x1220)};
  LowBlock Join;
  Join.Id = 3;
  Join.StartAddr = 0x1230;
  Join.Preds = {1, 2};
  Join.Ops = {operation(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
                        {NdVar::cst(0x1600, 8)}, 0x1230),
              operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 4)}, 0x1234)};
  Low.Blocks = {Entry, Left, Right, Join};

  LowToMedConverter Converter;
  Converter.setSourceCalleeTypeHints(&Hints);
  Converter.setSourceCallHintsEnabled(true);
  auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  recoverCallAbi(Med, Arch::AArch64, {});
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  const auto &Call = Med.CallInfos.front();
  ASSERT_TRUE(Call.SourceCallHint);
  ASSERT_EQ(Call.Args.size(), 8U);
  const auto Narrow = Call.Args.back();
  ASSERT_EQ(Narrow.Kind, MedVar::Temp);
  ASSERT_EQ(Narrow.Size, 1U);

  const MedOp *Extract = nullptr;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Output.Kind == Narrow.Kind && Op.Output.Id == Narrow.Id &&
          Op.Output.SSAVer == Narrow.SSAVer)
        Extract = &Op;
  ASSERT_NE(Extract, nullptr);
  EXPECT_EQ(Extract->Opcode, NdOp::SUBBYTES);
  ASSERT_EQ(Extract->NumInputs, 2U);
  EXPECT_EQ(Extract->Inputs[0].Kind, MedVar::Reg);
  EXPECT_EQ(Extract->Inputs[0].RegOff, a64reg::X7);
  EXPECT_EQ(Extract->Inputs[0].Size, 4U);
  EXPECT_TRUE(Extract->Inputs[1].isConst());
  EXPECT_EQ(Extract->Inputs[1].ConstVal, 0U);
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

TEST(ObjCCallHints, SuperInitResultKeepsTheStoredDynamicReceiver) {
  auto Image = image();
  Image.ObjCMethods.front().Implementation = 0x1200;
  ObjCClass First;
  First.Name = "First";
  First.SuperclassName = "NSObject";
  First.InheritanceStatus = "resolved";
  Image.ObjCClasses.push_back(First);
  ObjCClass NSObject;
  NSObject.Name = "NSObject";
  NSObject.RootClass = true;
  NSObject.InheritanceStatus = "root";
  Image.ObjCClasses.push_back(NSObject);
  Image.ObjCClasses.front().RootClass = false;
  Image.ObjCClasses.front().SuperclassName = "NSObject";
  Image.ObjCClasses.front().InheritanceStatus = "resolved";
  Image.ImportPtrSlots[0x2180] = "_objc_msgSendSuper2";
  Image.ImportPtrSlots[0x2188] = "_objc_msgSend";
  Image.ObjCSourceReferences[0x2100] = {
      ObjCSourceReference::Kind::Selector, 0x2100, 8, "init"};
  Image.ObjCSourceReferences[0x2110] = {
      ObjCSourceReference::Kind::Selector, 0x2110, 8, "setIndex:"};

  auto Init = Image.ObjCMethods.front();
  Init.ClassName = "NSObject";
  Init.Selector = "init";
  Init.Implementation = 0x1500;
  Init.TypeHint = signature(Arch::AArch64, 0);
  Init.TypeHint->ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Diagnostic;
  ASSERT_TRUE(assignDarwinObjCSourceABI(*Init.TypeHint, Arch::AArch64,
                                       Diagnostic));
  Image.ObjCMethods.push_back(Init);
  auto Setter = Image.ObjCMethods.front();
  Setter.ClassName = "First";
  Setter.Selector = "setIndex:";
  Setter.Implementation = 0x1510;
  Image.ObjCMethods.push_back(Setter);
  auto Conflicting = Setter;
  Conflicting.ClassName = "Other";
  Conflicting.Implementation = 0x1520;
  Conflicting.TypeHint->Parameters.back().Type =
      NdType::makePtr(NdType::makeVoid());
  Diagnostic.clear();
  ASSERT_TRUE(assignDarwinObjCSourceABI(*Conflicting.TypeHint, Arch::AArch64,
                                       Diagnostic));
  Image.ObjCMethods.push_back(Conflicting);
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "setIndex:"));

  auto Function = caller();
  auto &Block = Function.Blocks.front();
  Block.Ops = {
      operation(NdOp::INT_SUB, NdVar::reg(a64reg::SP, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(32, 4)}, 0x1200),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X8, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(16, 4)}, 0x1204),
      operation(NdOp::STORE, {},
                {NdVar::reg(a64reg::X8, 8), NdVar::reg(a64reg::X0, 8)},
                0x1208),
      operation(NdOp::LOAD, NdVar::reg(a64reg::X1, 8),
                {NdVar::cst(0x2100, 8)}, 0x120c),
      operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                {NdVar::reg(a64reg::X8, 8)}, 0x1210),
      operation(NdOp::INDIR_CALL, NdVar::reg(a64reg::X0, 8),
                {NdVar::cst(0x2180, 8)}, 0x1214),
      operation(NdOp::COPY, NdVar::reg(a64reg::X19, 8),
                {NdVar::reg(a64reg::X0, 8)}, 0x1218),
      operation(NdOp::LOAD, NdVar::reg(a64reg::X1, 8),
                {NdVar::cst(0x2110, 8)}, 0x121c),
      operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                {NdVar::reg(a64reg::X19, 8)}, 0x1220),
      operation(NdOp::COPY, NdVar::reg(a64reg::X2, 8),
                {NdVar::cst(7, 8)}, 0x1224),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2188, 8)}, 0x1228),
      operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X19, 8)}, 0x122c)};
  Block.EndAddr = 0x1230;

  const auto Hints = buildObjCSourceCallHints(Image, Function);
  EXPECT_EQ(Hints.count(0x1214), 1U);
  EXPECT_EQ(Hints.count(0x1228), 1U);
  ASSERT_EQ(Hints.size(), 2U);
  ASSERT_TRUE(Hints.at(0x1228).Receiver);
  EXPECT_EQ(Hints.at(0x1228).Receiver->ClassName, "First");

  Block.Ops[3] = operation(NdOp::LOAD, NdVar::reg(a64reg::X1, 8),
                           {NdVar::cst(0x2110, 8)}, 0x120c);
  EXPECT_EQ(buildObjCSourceCallHints(Image, Function).count(0x1228), 0U);
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

TEST(ObjCCallHints, SDKCDeclarationsPreservePointerIntegerAndFloatCarriers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &[Name, Module, Count, ResultKind, ResultBytes] :
         {std::tuple{
              "NSStringFromClass",
              "/System/Library/Frameworks/Foundation.framework/Foundation", 1U,
              NdTypeKind::Ptr, 8U},
          std::tuple{"CFStringCompare",
                     "/System/Library/Frameworks/CoreFoundation.framework/"
                     "CoreFoundation",
                     3U, NdTypeKind::Int, 8U},
          std::tuple{"pthread_mutex_lock", "/usr/lib/libSystem.B.dylib", 1U,
                     NdTypeKind::Int, 4U},
          std::tuple{"dispatch_time", "/usr/lib/system/libdispatch.dylib", 2U,
                     NdTypeKind::Int, 8U},
          std::tuple{"fmod", "/usr/lib/libSystem.B.dylib", 2U,
                     NdTypeKind::Float, 8U},
          std::tuple{
              "CACurrentMediaTime",
              "/System/Library/Frameworks/QuartzCore.framework/QuartzCore", 0U,
              NdTypeKind::Float, 8U},
          std::tuple{"UTTypeIsDynamic",
                     "/System/Library/Frameworks/CoreServices.framework/"
                     "CoreServices",
                     1U, NdTypeKind::Int, 1U},
          std::tuple{"__error", "/usr/lib/libSystem.B.dylib", 0U,
                     NdTypeKind::Ptr, 8U}}) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(static_cast<int>(Architecture));
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      Image.DyldBindSlots[0x2180] = {"_" + std::string(Name), 0, Module, false};
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      ASSERT_TRUE(Med.CallInfos.front().SourceCallHint);
      const auto &Hint = *Med.CallInfos.front().SourceCallHint;
      EXPECT_EQ(Hint.Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinSDK);
      EXPECT_EQ(Hint.TargetName, Name);
      EXPECT_EQ(Hint.Signature.Parameters.size(), Count);
      EXPECT_EQ(Hint.Signature.ReturnType->Kind, ResultKind);
      EXPECT_EQ(Hint.Signature.ReturnType->Size, ResultBytes);
      if (ResultKind == NdTypeKind::Float) {
        EXPECT_EQ(Hint.Signature.ReturnLocation.Kind,
                  SourceABICarrierKind::FloatingRegister);
        for (const auto &Parameter : Hint.Signature.Parameters)
          EXPECT_EQ(Parameter.Location.Kind,
                    SourceABICarrierKind::FloatingRegister);
      }
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      std::string C;
      llvm::raw_string_ostream OS(C);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      EXPECT_NE(C.find("neverd_darwin_" + std::string(Name) + "("),
                std::string::npos)
          << C;
      EXPECT_NE(C.find("__asm__(\"_" + std::string(Name) + "\")"),
                std::string::npos)
          << C;
      EXPECT_EQ(C.find("#include <os/lock.h>"), std::string::npos) << C;
      Image.DyldBindSlots[0x2180].Module = "/tmp/impostor.dylib";
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
  }
}

TEST(ObjCCallHints, UIKitCGSizeStringKeepsExactProviderAndRecordABI) {
  auto Image = runtimeImage("_NSStringFromCGSize", Arch::AArch64);
  Image.DyldBindSlots[0x2180] = {
      "_NSStringFromCGSize", 0,
      "/System/Library/Frameworks/UIKit.framework/UIKit", false};
  const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
  EXPECT_EQ(Hint->TargetName, "NSStringFromCGSize");
  EXPECT_EQ(Hint->Signature.Origin,
            SourceFunctionTypeHint::OriginKind::DarwinSDK);
  ASSERT_TRUE(Hint->Signature.ReturnType);
  EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Ptr);
  ASSERT_EQ(Hint->Signature.Parameters.size(), 1U);
  const auto &Size = Hint->Signature.Parameters.front();
  ASSERT_TRUE(Size.Type);
  EXPECT_EQ(Size.Type->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Size.Type->Size, 16U);
  ASSERT_EQ(Size.Type->Fields.size(), 2U);
  ASSERT_EQ(Size.Components.size(), 2U);
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (unsigned I = 0; I != 2; ++I) {
    EXPECT_EQ(Size.Type->Fields[I]->Kind, NdTypeKind::Float);
    EXPECT_EQ(Size.Type->Fields[I]->Size, 8U);
    EXPECT_EQ(Size.Components[I].Kind,
              SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Size.Components[I].RegisterOffset, TRI.FPParamRegs[I]);
    EXPECT_EQ(Size.Components[I].ValueBytes, 8U);
  }
  std::string Diagnostic;
  EXPECT_TRUE(validateSourceABI(Hint->Signature, Diagnostic)) << Diagnostic;

  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    auto Changed = Image;
    if (Mutation == 0)
      Changed.Arch = Arch::X64;
    if (Mutation == 1)
      Changed.DyldBindSlots.clear();
    if (Mutation == 2)
      Changed.DyldBindSlots[0x2180].Module =
          "/System/Library/Frameworks/Foundation.framework/Foundation";
    if (Mutation == 3)
      Changed.DyldBindSlots[0x2180].Module =
          "/tmp/UIKit.framework/UIKit";
    if (Mutation == 4)
      Changed.DyldBindSlots[0x2180].Name = "_other";
    if (Mutation == 5)
      Changed.DyldBindSlots[0x2180].Addend = 8;
    if (Mutation == 6)
      Changed.DyldBindSlots[0x2180].WeakImport = true;
    if (Mutation == 7)
      Changed.ConflictingImportStorageSlots.insert(0x2180);
    EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
  }
}

TEST(ObjCCallHints, UIKitImageContextKeepsExactProviderAndPublicABIs) {
  constexpr auto UIKit =
      "/System/Library/Frameworks/UIKit.framework/UIKit";
  for (const auto &[Name, ParameterCount, ReturnKind] :
       {std::tuple{"UIGraphicsBeginImageContext", 1U, NdTypeKind::Void},
        std::tuple{"UIGraphicsGetCurrentContext", 0U, NdTypeKind::Ptr},
        std::tuple{"UIGraphicsGetImageFromCurrentImageContext", 0U,
                   NdTypeKind::Ptr},
        std::tuple{"UIGraphicsEndImageContext", 0U, NdTypeKind::Void}}) {
    SCOPED_TRACE(Name);
    auto Image = runtimeImage("_" + std::string(Name), Arch::AArch64);
    Image.DyldBindSlots[0x2180] = {
        "_" + std::string(Name), 0, UIKit, false};
    const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
    EXPECT_EQ(Hint->TargetName, Name);
    EXPECT_EQ(Hint->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::DarwinSDK);
    ASSERT_TRUE(Hint->Signature.ReturnType);
    EXPECT_EQ(Hint->Signature.ReturnType->Kind, ReturnKind);
    EXPECT_EQ(Hint->Signature.Parameters.size(), ParameterCount);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Hint->Signature, Diagnostic)) << Diagnostic;

    if (ParameterCount) {
      const auto &Size = Hint->Signature.Parameters.front();
      ASSERT_TRUE(Size.Type);
      EXPECT_EQ(Size.Type->Kind, NdTypeKind::Struct);
      EXPECT_EQ(Size.Type->Size, 16U);
      ASSERT_EQ(Size.Components.size(), 2U);
      const auto &TRI = getTargetRegInfo(Arch::AArch64);
      for (unsigned I = 0; I != 2; ++I) {
        EXPECT_EQ(Size.Components[I].Kind,
                  SourceABICarrierKind::FloatingRegister);
        EXPECT_EQ(Size.Components[I].RegisterOffset, TRI.FPParamRegs[I]);
        EXPECT_EQ(Size.Components[I].ValueBytes, 8U);
      }
    }

    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.Arch = Arch::X64;
      if (Mutation == 1)
        Changed.DyldBindSlots.clear();
      if (Mutation == 2)
        Changed.DyldBindSlots[0x2180].Module =
            "/System/Library/Frameworks/Foundation.framework/Foundation";
      if (Mutation == 3)
        Changed.DyldBindSlots[0x2180].Module =
            "/tmp/UIKit.framework/UIKit";
      if (Mutation == 4)
        Changed.DyldBindSlots[0x2180].Name = "_other";
      if (Mutation == 5)
        Changed.DyldBindSlots[0x2180].Addend = 8;
      if (Mutation == 6)
        Changed.DyldBindSlots[0x2180].WeakImport = true;
      if (Mutation == 7)
        Changed.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
    }
  }
}

TEST(ObjCCallHints, DarwinNotifyCancelKeepsGeneratedIntegerABIAndProviders) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Module : {"/usr/lib/libSystem.B.dylib",
                               "/usr/lib/system/libsystem_notify.dylib"}) {
      auto Image = runtimeImage("_notify_cancel", Architecture);
      Image.DyldBindSlots[0x2180] = {"_notify_cancel", 0, Module, false};
      const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint) << Module;
      EXPECT_EQ(Hint->TargetName, "notify_cancel");
      EXPECT_EQ(Hint->Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinSDK);
      ASSERT_TRUE(Hint->Signature.ReturnType);
      EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Signature.ReturnType->Size, 4U);
      EXPECT_FALSE(Hint->Signature.ReturnType->IsSigned);
      ASSERT_EQ(Hint->Signature.Parameters.size(), 1U);
      EXPECT_EQ(Hint->Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Signature.Parameters[0].Type->Size, 4U);
      EXPECT_TRUE(Hint->Signature.Parameters[0].Type->IsSigned);
      EXPECT_EQ(Hint->Signature.Parameters[0].Location.RegisterOffset,
                getTargetRegInfo(Architecture).IntParamRegs.front());
    }
    auto Wrong = runtimeImage("_notify_cancel", Architecture);
    Wrong.DyldBindSlots[0x2180] = {
        "_notify_cancel", 0, "/tmp/libsystem_notify.dylib", false};
    EXPECT_FALSE(darwinRuntimeSourceCallHint(Wrong, 0x2180));
  }
}

TEST(ObjCCallHints, DarwinMallocSizePreservesPointerAndSizeTABI) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Module : {"/usr/lib/libSystem.B.dylib",
                               "/usr/lib/system/libsystem_malloc.dylib"}) {
      auto Image = runtimeImage("_malloc_size", Architecture);
      Image.DyldBindSlots[0x2180] = {"_malloc_size", 0, Module, false};
      const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint) << Module;
      EXPECT_EQ(Hint->TargetName, "malloc_size");
      EXPECT_EQ(Hint->Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinSDK);
      ASSERT_TRUE(Hint->Signature.ReturnType);
      EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Signature.ReturnType->Size, 8U);
      EXPECT_FALSE(Hint->Signature.ReturnType->IsSigned);
      ASSERT_EQ(Hint->Signature.Parameters.size(), 1U);
      const auto &Pointer = Hint->Signature.Parameters.front();
      ASSERT_TRUE(Pointer.Type);
      EXPECT_EQ(Pointer.Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Pointer.Location.Kind, SourceABICarrierKind::IntegerRegister);
      EXPECT_EQ(Pointer.Location.RegisterOffset,
                getTargetRegInfo(Architecture).IntParamRegs.front());
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Hint->Signature, Diagnostic)) << Diagnostic;
    }

    auto Image = runtimeImage("_malloc_size", Architecture);
    Image.DyldBindSlots[0x2180] = {"_malloc_size", 0,
                                   "/usr/lib/libSystem.B.dylib", false};
    auto Weak = Image;
    Weak.DyldBindSlots[0x2180].WeakImport = true;
    const auto WeakHint = darwinRuntimeSourceCallHint(Weak, 0x2180);
    ASSERT_TRUE(WeakHint);
    EXPECT_TRUE(WeakHint->WeakImport);

    for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.DyldBindSlots[0x2180].Module = "/tmp/libSystem.B.dylib";
      if (Mutation == 1)
        Changed.DyldBindSlots[0x2180].Addend = 1;
      if (Mutation == 2)
        Changed.ImportPtrSlots[0x2180] = "_malloc";
      if (Mutation == 3)
        Changed.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
    }
  }
}

TEST(ObjCCallHints, SDKCDeclarationsRequireExactExportsAndFixedPrototypes) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      auto Image = runtimeImage("_NSStringFromClass", Architecture);
      const std::string Module = "/System/Library/Frameworks/"
                                 "Foundation.framework/Versions/C/Foundation";
      Image.DyldBindSlots[0x2180] = {"_NSStringFromClass", 0, Module, false};
      if (Mutation == 1)
        Image.DyldBindSlots.clear();
      if (Mutation == 2)
        Image.DyldBindSlots[0x2180].Module.clear();
      if (Mutation == 3)
        Image.DyldBindSlots[0x2180].Module = "/usr/lib/libSystem.B.dylib";
      if (Mutation == 4)
        Image.DyldBindSlots[0x2180].Module =
            "/tmp/Foundation.framework/Foundation";
      if (Mutation == 5)
        Image.DyldBindSlots[0x2180].WeakImport = true;
      if (Mutation == 6)
        Image.DyldBindSlots[0x2180].Addend = 4;
      if (Mutation == 7)
        Image.ConflictingImportStorageSlots.insert(0x2180);
      const auto Binding = darwinRuntimeSourceCallHint(Image, 0x2180);
      EXPECT_EQ(bool(Binding), Mutation == 0 || Mutation == 5) << Mutation;
      if (Mutation == 5 && Binding)
        EXPECT_TRUE(Binding->WeakImport);
    }
    // Exported names cannot turn a variadic prefix, a by-value aggregate or
    // an unknown callback prototype into a complete scalar declaration.
    for (const char *Name : {"NSLog", "CFStringCreateWithFormat", "sigsetjmp",
                             "vfork", "dispatch_async_f"}) {
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      Image.DyldBindSlots[0x2180] = {
          "_" + std::string(Name), 0,
          std::string(Name) == "NSLog"
              ? "/System/Library/Frameworks/Foundation.framework/Foundation"
          : std::string(Name) == "CFStringCreateWithFormat"
              ? "/System/Library/Frameworks/CoreFoundation.framework/"
                "CoreFoundation"
              : "/usr/lib/libSystem.B.dylib",
          false};
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180)) << Name;
    }
  }
}

TEST(ObjCCallHints, WeakSDKFunctionKeepsOptionalExternalCallIdentity) {
  constexpr llvm::StringLiteral Name = "CGColorSpaceUsesITUR_2100TF";
  constexpr llvm::StringLiteral Symbol = "_CGColorSpaceUsesITUR_2100TF";
  constexpr llvm::StringLiteral Module =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage(Symbol, Architecture);
    Image.DyldBindSlots[0x2180] = {Symbol.str(), 0, Module.str(), true};
    const auto Med = convert(Image, caller(Architecture));
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    ASSERT_TRUE(Med.CallInfos[0].SourceCallHint);
    const auto &Binding = *Med.CallInfos[0].SourceCallHint;
    EXPECT_EQ(Binding.TargetName, Name);
    EXPECT_EQ(Binding.CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
    EXPECT_EQ(Binding.Signature.Origin,
              SourceFunctionTypeHint::OriginKind::DarwinSDK);
    EXPECT_TRUE(Binding.WeakImport);
    EXPECT_EQ(Binding.Signature.ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Binding.Signature.ReturnType->Size, 1U);
    ASSERT_EQ(Binding.Signature.Parameters.size(), 1U);
    EXPECT_EQ(Binding.Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);

    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Architecture);
    const auto *Expression = sourceCall(High);
    ASSERT_NE(Expression, nullptr);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));

    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    EXPECT_NE(Source.find(
                  "extern __attribute__((weak_import)) uint8_t "
                  "neverd_darwin_CGColorSpaceUsesITUR_2100TF(void*) "
                  "__asm__(\"_CGColorSpaceUsesITUR_2100TF\");"),
              std::string::npos)
        << Source;

    auto Forged = *Expression;
    auto ForgedHint =
        std::make_shared<SourceCallTypeHint>(*Expression->SourceCallHint);
    ForgedHint->WeakImport = false;
    Forged.SourceCallHint = std::move(ForgedHint);
    EXPECT_FALSE(sdk::objcSourceCallBound(Forged, Image, {}));

    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.DyldBindSlots[0x2180].WeakImport = false;
      if (Mutation == 1)
        Changed.DyldBindSlots[0x2180].Module += ".impostor";
      if (Mutation == 2)
        Changed.DyldBindSlots[0x2180].Addend = 8;
      if (Mutation == 3)
        Changed.DyldBindSlots.clear();
      if (Mutation == 4) {
        Changed.ImportPtrSlots[0x2180] += "Suffix";
        Changed.DyldBindSlots[0x2180].Name =
            Changed.ImportPtrSlots[0x2180];
      }
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Changed, {}))
          << Mutation;
    }
  }
}

TEST(ObjCCallHints, DispatchOnceFPreservesExactCallbackPrototypeAndExport) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Module : {"/usr/lib/libSystem.B.dylib",
                               "/usr/lib/system/libdispatch.dylib"}) {
      auto Image = runtimeImage("_dispatch_once_f", Architecture);
      Image.DyldBindSlots[0x2180] = {"_dispatch_once_f", 0, Module, false};
      const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      EXPECT_EQ(Hint->TargetName, "dispatch_once_f");
      EXPECT_EQ(Hint->Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinSDK);
      EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Void);
      ASSERT_EQ(Hint->Signature.Parameters.size(), 3U);
      EXPECT_EQ(Hint->Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Hint->Signature.Parameters[0].Type->Pointee->Kind,
                NdTypeKind::Int);
      EXPECT_EQ(Hint->Signature.Parameters[0].Type->Pointee->Size, 8U);
      EXPECT_EQ(Hint->Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);
      const auto Callback = Hint->Signature.Parameters[2].Type;
      ASSERT_EQ(Callback->Kind, NdTypeKind::Ptr);
      ASSERT_TRUE(Callback->Pointee);
      EXPECT_EQ(Callback->Pointee->Kind, NdTypeKind::Func);
      EXPECT_EQ(Callback->Pointee->RetType->Kind, NdTypeKind::Void);
      ASSERT_EQ(Callback->Pointee->ParamTypes.size(), 1U);
      EXPECT_EQ(Callback->Pointee->ParamTypes[0]->Kind, NdTypeKind::Ptr);
      for (const auto &Parameter : Hint->Signature.Parameters)
        EXPECT_EQ(Parameter.Location.Kind,
                  SourceABICarrierKind::IntegerRegister);
    }

    auto Image = runtimeImage("_dispatch_once_f", Architecture);
    Image.DyldBindSlots[0x2180] = {
        "_dispatch_once_f", 0, "/usr/lib/libSystem.B.dylib", false};
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.DyldBindSlots[0x2180].Module = "/tmp/libSystem.B.dylib";
      if (Mutation == 1)
        Changed.DyldBindSlots[0x2180].Addend = 1;
      if (Mutation == 2)
        Changed.DyldBindSlots[0x2180].WeakImport = true;
      if (Mutation == 3)
        Changed.DyldBindSlots.clear();
      if (Mutation == 4)
        Changed.ImportPtrSlots[0x2180] = "_dispatch_async_f";
      if (Mutation == 5)
        Changed.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
    }
  }
}

TEST(ObjCCallHints, ReachabilityCallsPreserveCallbackAndBooleanABI) {
  constexpr llvm::StringLiteral Provider =
      "/System/Library/Frameworks/SystemConfiguration.framework/"
      "SystemConfiguration";
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Name : {"SCNetworkReachabilitySetCallback",
                             "SCNetworkReachabilitySetDispatchQueue"}) {
      const std::string Import = std::string("_") + Name;
      auto Image = runtimeImage(Import, Architecture);
      Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
      const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint) << Name;
      EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
      EXPECT_EQ(Hint->TargetName, Name);
      const auto &ABI = Hint->Signature;
      EXPECT_EQ(ABI.Origin, SourceFunctionTypeHint::OriginKind::DarwinSDK);
      ASSERT_TRUE(ABI.ReturnType);
      EXPECT_EQ(ABI.ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(ABI.ReturnType->Size, 1U);
      ASSERT_EQ(ABI.Parameters.size(),
                std::string_view(Name) == "SCNetworkReachabilitySetCallback"
                    ? 3U
                    : 2U);
      for (const auto &Parameter : ABI.Parameters)
        EXPECT_EQ(Parameter.Location.Kind,
                  SourceABICarrierKind::IntegerRegister);
      EXPECT_EQ(ABI.Parameters[0].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(ABI.Parameters.back().Type->Kind, NdTypeKind::Ptr);
      if (ABI.Parameters.size() == 3) {
        const auto Callback = ABI.Parameters[1].Type;
        ASSERT_EQ(Callback->Kind, NdTypeKind::Ptr);
        ASSERT_TRUE(Callback->Pointee);
        EXPECT_EQ(Callback->Pointee->Kind, NdTypeKind::Func);
        EXPECT_EQ(Callback->Pointee->RetType->Kind, NdTypeKind::Void);
        ASSERT_EQ(Callback->Pointee->ParamTypes.size(), 3U);
        EXPECT_EQ(Callback->Pointee->ParamTypes[0]->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Callback->Pointee->ParamTypes[1]->Kind, NdTypeKind::Int);
        EXPECT_EQ(Callback->Pointee->ParamTypes[1]->Size, 4U);
        EXPECT_EQ(Callback->Pointee->ParamTypes[2]->Kind, NdTypeKind::Ptr);
      }
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(ABI, Diagnostic)) << Diagnostic;
      Image.DyldBindSlots[0x2180].Module = "/tmp/impostor.dylib";
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180));
      Image.DyldBindSlots[0x2180].Module = Provider.str();
      Image.DyldBindSlots[0x2180].WeakImport = true;
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180));
    }
  }
}

TEST(ObjCCallHints, ReachabilityCreateWithNameKeepsAllocatorAndName) {
  constexpr llvm::StringLiteral Name = "_SCNetworkReachabilityCreateWithName";
  constexpr llvm::StringLiteral Provider =
      "/System/Library/Frameworks/SystemConfiguration.framework/"
      "SystemConfiguration";
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage(Name, Architecture);
    Image.DyldBindSlots[0x2180] = {Name.str(), 0, Provider.str(), false};
    const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->TargetName, "SCNetworkReachabilityCreateWithName");
    EXPECT_EQ(Hint->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::DarwinSDK);
    ASSERT_TRUE(Hint->Signature.ReturnType);
    EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Ptr);
    ASSERT_EQ(Hint->Signature.Parameters.size(), 2U);
    EXPECT_EQ(Hint->Signature.Parameters[0].Name, "allocator");
    EXPECT_EQ(Hint->Signature.Parameters[1].Name, "nodename");
    for (const auto &Parameter : Hint->Signature.Parameters) {
      EXPECT_EQ(Parameter.Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Parameter.Location.Kind,
                SourceABICarrierKind::IntegerRegister);
    }
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Hint->Signature, Diagnostic)) << Diagnostic;
    Image.DyldBindSlots[0x2180].Module = "/tmp/impostor.dylib";
    EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180));
  }
}

TEST(ObjCCallHints,
     DispatchQueueSetSpecificPreservesDestructorPrototypeAndExport) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Module : {"/usr/lib/libSystem.B.dylib",
                               "/usr/lib/system/libdispatch.dylib"}) {
      auto Image = runtimeImage("_dispatch_queue_set_specific", Architecture);
      Image.DyldBindSlots[0x2180] = {
          "_dispatch_queue_set_specific", 0, Module, false};
      const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      EXPECT_EQ(Hint->TargetName, "dispatch_queue_set_specific");
      EXPECT_EQ(Hint->Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinSDK);
      EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Void);
      ASSERT_EQ(Hint->Signature.Parameters.size(), 4U);
      for (unsigned I = 0; I != 3; ++I) {
        EXPECT_EQ(Hint->Signature.Parameters[I].Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Hint->Signature.Parameters[I].Location.Kind,
                  SourceABICarrierKind::IntegerRegister);
      }
      const auto Destructor = Hint->Signature.Parameters[3].Type;
      ASSERT_EQ(Destructor->Kind, NdTypeKind::Ptr);
      ASSERT_TRUE(Destructor->Pointee);
      EXPECT_EQ(Destructor->Pointee->Kind, NdTypeKind::Func);
      EXPECT_EQ(Destructor->Pointee->RetType->Kind, NdTypeKind::Void);
      ASSERT_EQ(Destructor->Pointee->ParamTypes.size(), 1U);
      EXPECT_EQ(Destructor->Pointee->ParamTypes[0]->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Hint->Signature.Parameters[3].Location.Kind,
                SourceABICarrierKind::IntegerRegister);
    }

    auto Image = runtimeImage("_dispatch_queue_set_specific", Architecture);
    Image.DyldBindSlots[0x2180] = {
        "_dispatch_queue_set_specific", 0, "/usr/lib/libSystem.B.dylib",
        false};
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.DyldBindSlots[0x2180].Module = "/tmp/libSystem.B.dylib";
      if (Mutation == 1)
        Changed.DyldBindSlots[0x2180].Addend = 1;
      if (Mutation == 2)
        Changed.DyldBindSlots[0x2180].WeakImport = true;
      if (Mutation == 3)
        Changed.DyldBindSlots.clear();
      if (Mutation == 4)
        Changed.ImportPtrSlots[0x2180] = "_dispatch_queue_get_specific";
      if (Mutation == 5)
        Changed.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
    }
  }
}

TEST(ObjCCallHints, AvailabilityCheckPreservesExactWeakImportAndABI) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("__availability_version_check", Architecture);
    Image.DyldBindSlots[0x2180] = {"__availability_version_check", 0,
                                   "/usr/lib/libSystem.B.dylib", true};
    const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_TRUE(Hint->WeakImport);
    EXPECT_EQ(Hint->TargetName, "_availability_version_check");
    EXPECT_EQ(Hint->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::DarwinSDK);
    EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Hint->Signature.ReturnType->Size, 1U);
    EXPECT_FALSE(Hint->Signature.ReturnType->IsSigned);
    ASSERT_EQ(Hint->Signature.Parameters.size(), 2U);
    EXPECT_EQ(Hint->Signature.Parameters[0].Type->Size, 4U);
    EXPECT_FALSE(Hint->Signature.Parameters[0].Type->IsSigned);
    EXPECT_EQ(Hint->Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);
    auto Call = HighExpr::makeCall(
        Hint->TargetName, Hint->TargetAddress,
        {HighExpr::makeConst(1, 4), HighExpr::makeConst(0x2000, 8)});
    Call->Type = NdType::makeInt(1, false);
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    auto Forged = std::make_shared<SourceCallTypeHint>(*Hint);
    Forged->WeakImport = false;
    Call->SourceCallHint = Forged;
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));

    const auto Med = convert(Image, caller(Architecture));
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    ASSERT_TRUE(Med.CallInfos[0].SourceCallHint);
    const auto &Veneer = *Med.CallInfos[0].SourceCallHint;
    EXPECT_TRUE(Veneer.WeakImport);
    EXPECT_EQ(Veneer.TargetAddress, 0x2180U);
    EXPECT_EQ(Veneer.TargetName, "_availability_version_check");

    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.DyldBindSlots[0x2180].WeakImport = false;
      if (Mutation == 1)
        Changed.DyldBindSlots[0x2180].Module = "/tmp/libSystem.B.dylib";
      if (Mutation == 2)
        Changed.DyldBindSlots[0x2180].Addend = 1;
      if (Mutation == 3)
        Changed.DyldBindSlots.clear();
      if (Mutation == 4)
        Changed.ImportPtrSlots[0x2180] = "__other_version_check";
      if (Mutation == 5)
        Changed.ConflictingImportStorageSlots.insert(0x2180);
      if (Mutation == 6)
        Changed.ImportStorageSlots[0x2180] = {"__availability_version_check",
                                              1};
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
    }
  }
}

TEST(ObjCCallHints, CompilerRTPlatformCheckUsesItsLinkedPublicContract) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.Symbols.push_back(
        {"___isPlatformVersionAtLeast", 0x1500, 0x100, true});
    const auto Hint = darwinCompilerRTSourceCallHint(Image, 0x1500);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
    EXPECT_EQ(Hint->TargetName, "__isPlatformVersionAtLeast");
    EXPECT_EQ(Hint->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::DarwinRuntime);
    ASSERT_TRUE(Hint->Signature.ReturnType);
    EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Hint->Signature.ReturnType->Size, 4U);
    EXPECT_TRUE(Hint->Signature.ReturnType->IsSigned);
    ASSERT_EQ(Hint->Signature.Parameters.size(), 4U);
    for (const auto &Parameter : Hint->Signature.Parameters) {
      ASSERT_TRUE(Parameter.Type);
      EXPECT_EQ(Parameter.Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Parameter.Type->Size, 4U);
      EXPECT_FALSE(Parameter.Type->IsSigned);
    }

    auto Function = caller(Architecture);
    Function.Blocks.front().Ops.front().Inputs[0] = NdVar::cst(0x1500, 8);
    const auto Calls = buildObjCSourceCallHints(Image, Function);
    const auto Bound = Calls.find(0x1200);
    ASSERT_NE(Bound, Calls.end());
    EXPECT_EQ(Bound->second.CallKind,
              SourceCallTypeHint::Kind::DarwinRuntimeCall);
    EXPECT_EQ(Bound->second.TargetAddress, 0x1500U);

    auto Call = HighExpr::makeCall(
        Hint->TargetName, Hint->TargetAddress,
        {HighExpr::makeConst(1, 4), HighExpr::makeConst(17, 4),
         HighExpr::makeConst(6, 4), HighExpr::makeConst(0, 4)});
    Call->Type = Hint->Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));

    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.Symbols.front().Name = "___isOSVersionAtLeast";
      if (Mutation == 1)
        Changed.Symbols.front().Addr = 0x1510;
      if (Mutation == 2)
        Changed.Symbols.front().IsFunc = false;
      if (Mutation == 3)
        Changed.Symbols.push_back(Changed.Symbols.front());
      if (Mutation == 4)
        Changed.IsRelocatable = true;
      if (Mutation == 5) {
        Changed.Sections.front().Flags = SegmentFlags::Readable;
        Changed.Sections.front().Type = 0;
        Changed.Segments.front().Flags = SegmentFlags::Readable;
      }
      EXPECT_FALSE(darwinCompilerRTSourceCallHint(Changed, 0x1500))
          << Mutation;
    }
  }
}

TEST(ObjCCallHints, SystemDeclarationsPreserveWidthsOpaquePointersAndExports) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &[Name, Widths] :
         std::vector<std::pair<std::string, std::vector<unsigned>>>{
             {"getxattr", {8, 8, 8, 8, 4, 4}},
             {"setxattr", {8, 8, 8, 8, 4, 4}},
             {"listxattr", {8, 8, 8, 4}},
             {"removexattr", {8, 8, 4}},
             {"os_log_type_enabled", {8, 1}},
             {"asl_get", {8, 8}},
             {"asl_set", {8, 8, 8}},
             {"CC_SHA256", {8, 4, 8}},
             {"CC_SHA256_Init", {8}},
             {"CC_SHA256_Update", {8, 8, 4}},
             {"CC_SHA256_Final", {8, 8}}}) {
      SCOPED_TRACE(Name);
      auto Image = runtimeImage("_" + Name, Architecture);
      Image.DyldBindSlots[0x2180] = {"_" + Name, 0,
                                     "/usr/lib/libSystem.B.dylib", false};
      auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      EXPECT_EQ(Hint->Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinSDK);
      ASSERT_EQ(Hint->Signature.Parameters.size(), Widths.size());
      for (size_t I = 0; I < Widths.size(); ++I) {
        EXPECT_EQ(Hint->Signature.Parameters[I].Type->Size, Widths[I]);
        EXPECT_EQ(Hint->Signature.Parameters[I].Location.ValueBytes, Widths[I]);
      }
      EXPECT_EQ(Hint->Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
      if (Name == "getxattr" || Name == "listxattr") {
        EXPECT_EQ(Hint->Signature.ReturnType->Size, 8);
        EXPECT_TRUE(Hint->Signature.ReturnType->IsSigned);
      }
      if (Name == "os_log_type_enabled") {
        EXPECT_EQ(Hint->Signature.ReturnType->Size, 1);
        EXPECT_FALSE(Hint->Signature.ReturnType->IsSigned);
        EXPECT_FALSE(Hint->Signature.Parameters[1].Type->IsSigned);
      }
      if (Name == "CC_SHA256_Init")
        EXPECT_EQ(Hint->Signature.Parameters[0].Type->Pointee->Kind,
                  NdTypeKind::Void);
      if (Name == "asl_get" || Name == "CC_SHA256")
        EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Ptr);
      Image.DyldBindSlots[0x2180].Module = "/tmp/libSystem.B.dylib";
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180));
      Image.DyldBindSlots[0x2180].Module = "/usr/lib/libSystem.B.dylib";
      Image.DyldBindSlots[0x2180].Addend = 1;
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180));
      Image.DyldBindSlots[0x2180].Addend = 0;
      Image.DyldBindSlots[0x2180].WeakImport = true;
      Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      EXPECT_TRUE(Hint->WeakImport);
    }
    auto Variadic = runtimeImage("_asl_log", Architecture);
    Variadic.DyldBindSlots[0x2180] = {"_asl_log", 0,
                                      "/usr/lib/libSystem.B.dylib", false};
    EXPECT_FALSE(darwinRuntimeSourceCallHint(Variadic, 0x2180));
  }
}

TEST(ObjCCallHints, VectorIOAndUTTypeDeclarationsKeepExactProvidersAndABI) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto VectorIO = runtimeImage("_writev", Architecture);
    VectorIO.DyldBindSlots[0x2180] = {
        "_writev", 0, "/usr/lib/libSystem.B.dylib", false};
    auto Hint = darwinRuntimeSourceCallHint(VectorIO, 0x2180);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->TargetName, "writev");
    EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Hint->Signature.ReturnType->Size, 8U);
    EXPECT_TRUE(Hint->Signature.ReturnType->IsSigned);
    ASSERT_EQ(Hint->Signature.Parameters.size(), 3U);
    EXPECT_EQ(Hint->Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Hint->Signature.Parameters[0].Type->Size, 4U);
    EXPECT_EQ(Hint->Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Hint->Signature.Parameters[1].Type->Pointee->Kind,
              NdTypeKind::Void);
    EXPECT_EQ(Hint->Signature.Parameters[2].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Hint->Signature.Parameters[2].Type->Size, 4U);
    VectorIO.DyldBindSlots[0x2180].Module = "/tmp/libSystem.B.dylib";
    EXPECT_FALSE(darwinRuntimeSourceCallHint(VectorIO, 0x2180));

    for (const char *Module : {
             "/System/Library/Frameworks/CoreServices.framework/CoreServices",
             "/System/Library/Frameworks/CoreServices.framework/Versions/A/"
             "CoreServices",
             "/System/Library/Frameworks/CoreServices.framework/Versions/A/"
             "Frameworks/LaunchServices.framework/Versions/A/LaunchServices"}) {
      auto UTType = runtimeImage("_UTTypeConformsTo", Architecture);
      UTType.DyldBindSlots[0x2180] = {
          "_UTTypeConformsTo", 0, Module, false};
      Hint = darwinRuntimeSourceCallHint(UTType, 0x2180);
      ASSERT_TRUE(Hint) << Module;
      EXPECT_EQ(Hint->TargetName, "UTTypeConformsTo");
      EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Signature.ReturnType->Size, 1U);
      EXPECT_FALSE(Hint->Signature.ReturnType->IsSigned);
      ASSERT_EQ(Hint->Signature.Parameters.size(), 2U);
      for (const auto &Parameter : Hint->Signature.Parameters) {
        EXPECT_EQ(Parameter.Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Parameter.Type->Pointee->Kind, NdTypeKind::Void);
      }
    }
    auto UTType = runtimeImage("_UTTypeConformsTo", Architecture);
    UTType.DyldBindSlots[0x2180] = {
        "_UTTypeConformsTo", 0, "/tmp/CoreServices.framework/CoreServices",
        false};
    EXPECT_FALSE(darwinRuntimeSourceCallHint(UTType, 0x2180));
  }
}

TEST(ObjCCallHints, MountEnumerationKeepsArchitectureSpecificLinkerIdentity) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Name : {"getmntinfo", "getmntinfo$INODE64"}) {
      SCOPED_TRACE(Name);
      const auto Symbol = "_" + std::string(Name);
      auto Image = runtimeImage(Symbol, Architecture);
      Image.DyldBindSlots[0x2180] = {Symbol, 0, "/usr/lib/libSystem.B.dylib",
                                     false};
      const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
      const bool Supported = (Architecture == Arch::AArch64) ==
                             (std::string(Name) == "getmntinfo");
      ASSERT_EQ(bool(Hint), Supported);
      if (!Hint)
        continue;
      EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Signature.ReturnType->Size, 4U);
      EXPECT_TRUE(Hint->Signature.ReturnType->IsSigned);
      ASSERT_EQ(Hint->Signature.Parameters.size(), 2U);
      const auto &Output = Hint->Signature.Parameters[0];
      ASSERT_EQ(Output.Type->Kind, NdTypeKind::Ptr);
      ASSERT_EQ(Output.Type->Pointee->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Output.Type->Pointee->Pointee->Kind, NdTypeKind::Void);
      EXPECT_EQ(Output.Location.ValueBytes, 8U);
      EXPECT_EQ(Hint->Signature.Parameters[1].Location.ValueBytes, 4U);
      EXPECT_TRUE(Hint->Signature.Parameters[1].Type->IsSigned);
      for (const char *Provider : {"/usr/lib/libSystem.B.dylib",
                                   "/usr/lib/system/libsystem_c.dylib"}) {
        Image.DyldBindSlots[0x2180].Module = Provider;
        EXPECT_TRUE(darwinRuntimeSourceCallHint(Image, 0x2180));
      }
      Image.DyldBindSlots[0x2180].Module =
          "/usr/lib/system/libsystem_kernel.dylib";
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180));
      Image.DyldBindSlots[0x2180].Module = "/usr/lib/libSystem.B.dylib";
      Image.DyldBindSlots[0x2180].Addend = 1;
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180));
      Image.DyldBindSlots[0x2180].Addend = 0;
      Image.DyldBindSlots[0x2180].WeakImport = true;
      const auto Weak = darwinRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Weak);
      EXPECT_TRUE(Weak->WeakImport);
    }
  }
}

TEST(ObjCCallHints, GraphicsDeclarationsPreserveOpaquePointersAndExactExports) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &[Name, Framework, Kind] :
         {std::tuple{"CGImageGetWidth", "CoreGraphics", NdTypeKind::Int},
          std::tuple{"CGImageGetHeight", "CoreGraphics", NdTypeKind::Int},
          std::tuple{"CGImageRetain", "CoreGraphics", NdTypeKind::Ptr},
          std::tuple{"CGColorGetAlpha", "CoreGraphics", NdTypeKind::Float},
          std::tuple{"CGColorGetNumberOfComponents", "CoreGraphics",
                     NdTypeKind::Int},
          std::tuple{"CGImageSourceGetCount", "ImageIO", NdTypeKind::Int}}) {
      SCOPED_TRACE(Name);
      const std::string Root = "/System/Library/Frameworks/" +
                               std::string(Framework) + ".framework/";
      for (const std::string &Version :
           {std::string(), std::string("Versions/A/")}) {
        auto Image = runtimeImage("_" + std::string(Name), Architecture);
        Image.DyldBindSlots[0x2180] = {"_" + std::string(Name), 0,
                                       Root + Version + Framework, false};
        const auto Med = convert(Image, caller(Architecture));
        ASSERT_EQ(Med.CallInfos.size(), 1U);
        ASSERT_TRUE(Med.CallInfos[0].SourceCallHint);
        const auto &Hint = Med.CallInfos[0].SourceCallHint->Signature;
        EXPECT_EQ(Hint.Origin, SourceFunctionTypeHint::OriginKind::DarwinSDK);
        EXPECT_EQ(Hint.ReturnType->Kind, Kind);
        EXPECT_EQ(Hint.ReturnType->Size, 8U);
        EXPECT_EQ(Hint.ReturnLocation.Kind,
                  Kind == NdTypeKind::Float
                      ? SourceABICarrierKind::FloatingRegister
                      : SourceABICarrierKind::IntegerRegister);
        ASSERT_EQ(Hint.Parameters.size(), 1U);
        EXPECT_EQ(Hint.Parameters[0].Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Hint.Parameters[0].Location.Kind,
                  SourceABICarrierKind::IntegerRegister);
        MedToHighConverter Converter;
        Converter.setBinaryImage(&Image);
        const auto High = Converter.convert(Med, Architecture);
        const auto *Expression = sourceCall(High);
        ASSERT_NE(Expression, nullptr);
        EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
        for (const auto &Wrong :
             {"/tmp/" + std::string(Framework) + ".framework/" + Framework,
              Root + "Versions/B/" + Framework, Root + "Other",
              std::string("/usr/lib/libSystem.B.dylib")}) {
          Image.DyldBindSlots[0x2180].Module = Wrong;
          EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180));
          EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
        }
      }
    }
    for (const char *Name : {"CGContextGetCTM", "CGPathApply"}) {
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      Image.DyldBindSlots[0x2180] = {
          "_" + std::string(Name), 0,
          "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
          false};
      // Aggregate results and unknown callback prototypes still need their
      // own complete ABI proof even when the exported symbol is known.
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180));
    }
  }
}

TEST(ObjCCallHints, SDKDataBindingsPreserveStorageAddressesAndSubsequentLoads) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &[Name, Module] :
         {std::pair{"NSDefaultRunLoopMode", "/System/Library/Frameworks/"
                                            "Foundation.framework/Foundation"},
          std::pair{"_dispatch_main_q", "/usr/lib/libSystem.B.dylib"},
          std::pair{"_os_log_default", "/usr/lib/libSystem.B.dylib"},
          std::pair{"_os_log_disabled",
                    "/usr/lib/system/libsystem_trace.dylib"},
          std::pair{"kCAGravityResize", "/System/Library/Frameworks/"
                                        "QuartzCore.framework/QuartzCore"},
          std::pair{"_dispatch_source_type_timer",
                    "/usr/lib/system/libdispatch.dylib"}}) {
      SCOPED_TRACE(Name);
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      Image.DyldBindSlots[0x2180] = {"_" + std::string(Name), 0, Module, false};
      HighFunc Function;
      Function.Name = "data_address";
      Function.ReturnType = NdType::makeInt(8);
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x2180, 8),
                                         NdType::makeInt(8));
      Function.Body = {Return};
      for (bool Dereference : {false, true}) {
        auto Input = Function;
        if (Dereference)
          Input.Body.front().RetVal =
              HighExpr::makeLoad(Return.RetVal, NdType::makeInt(8));
        auto Bound = sdk::bindObjCSourceReferences(Input, Image);
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        const auto *Address = sourceCall(Bound.Function);
        ASSERT_NE(Address, nullptr);
        EXPECT_EQ(Address->SourceCallHint->TargetName, Name);
        EXPECT_EQ(Address->SourceCallHint->Signature.Origin,
                  SourceFunctionTypeHint::OriginKind::DarwinSDK);
        EXPECT_TRUE(sdk::objcSourceCallBound(*Address, Image, {}));
        const auto &Value = Bound.Function.Body.front().RetVal;
        EXPECT_EQ(Value->Kind, Dereference ? ExprKind::Load : ExprKind::Call);
        if (Dereference)
          EXPECT_EQ(Value->Operands.front().get(), Address);
        EXPECT_EQ(Return.RetVal->Kind, ExprKind::Load);
        std::string Source;
        llvm::raw_string_ostream OS(Source);
        CEmitterOptions Options;
        Options.TheArch = Architecture;
        ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
        EXPECT_NE(Source.find("extern unsigned char neverd_darwin_data_" +
                              std::string(Name) + "[] __asm__(\"_" + Name +
                              "\");"),
                  std::string::npos)
            << Source;
        EXPECT_EQ(Source.find("0x2180"), std::string::npos);
        auto Changed = Image;
        Changed.DyldBindSlots[0x2180].Module = "/tmp/impostor.dylib";
        EXPECT_FALSE(sdk::objcSourceCallBound(*Address, Changed, {}));
      }
    }
  }
}

TEST(ObjCCallHints, FrameworkAndCompilerDataKeepExactExportIdentities) {
  const std::pair<const char *, const char *> Declarations[] = {
      {"__NSArray0__", "CoreFoundation"},
      {"__NSArray0__struct", "CoreFoundation"},
      {"__NSDictionary0__", "CoreFoundation"},
      {"__NSDictionary0__struct", "CoreFoundation"},
      {"__kCFBooleanTrue", "CoreFoundation"},
      {"__kCFBooleanFalse", "CoreFoundation"},
      {"NSManagedObjectContextDidSaveNotification", "CoreData"},
      {"kCGImagePropertyGIFDictionary", "ImageIO"},
      {"CSSearchableItemActivityIdentifier", "CoreSpotlight"},
      {"kCGColorSpaceSRGB", "CoreGraphics"},
      {"kUTTagClassFilenameExtension", "CoreServices"},
      {"kUTTypeImage", "CoreServices"},
      {"UTTypeGIF", "UniformTypeIdentifiers"},
      {"WKWebsiteDataTypeDiskCache", "WebKit"},
      {"WKWebsiteDataTypeMemoryCache", "WebKit"},
      {"CIDetectorAccuracy", "CoreImage"},
      {"CIDetectorAccuracyLow", "CoreImage"},
      {"CIDetectorTypeFace", "CoreImage"},
      {"kCIInputImageKey", "CoreImage"},
      {"CIDetectorAccuracy", "QuartzCore"},
      {"kCIInputImageKey", "QuartzCore"}};
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (auto [Name, Framework] : Declarations) {
      SCOPED_TRACE(Name);
      for (bool Versioned : {false, true}) {
        const auto Symbol = "_" + std::string(Name);
        auto Image = runtimeImage(Symbol, Architecture);
        Image.DyldBindSlots[0x2180] = {
            Symbol, 0,
            "/System/Library/Frameworks/" + std::string(Framework) +
                ".framework/" + (Versioned ? "Versions/A/" : "") + Framework,
            false};
        const auto Binding = darwinRuntimeGlobalAddressHint(Image, 0x2180);
        ASSERT_TRUE(Binding);
        EXPECT_EQ(Binding->TargetName, Name);
        EXPECT_EQ(Binding->CallKind,
                  SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress);
        Image.DyldBindSlots[0x2180].Module += ".impostor";
        EXPECT_FALSE(darwinRuntimeGlobalAddressHint(Image, 0x2180));
        Image.DyldBindSlots[0x2180].Module =
            "/System/Library/Frameworks/UIKit.framework/UIKit";
        EXPECT_FALSE(darwinRuntimeGlobalAddressHint(Image, 0x2180));
      }
    }
  }
}

TEST(ObjCCallHints, WeakSDKDataKeepsOptionalExternalStorageIdentity) {
  constexpr llvm::StringLiteral Name = "kCGImageDestinationEncodeRequest";
  constexpr llvm::StringLiteral Symbol = "_kCGImageDestinationEncodeRequest";
  constexpr llvm::StringLiteral Module =
      "/System/Library/Frameworks/ImageIO.framework/ImageIO";
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage(Symbol, Architecture);
    Image.DyldBindSlots[0x2180] = {Symbol.str(), 0, Module.str(), true};
    const auto Binding = darwinRuntimeGlobalAddressHint(Image, 0x2180);
    ASSERT_TRUE(Binding);
    EXPECT_EQ(Binding->TargetName, Name);
    EXPECT_EQ(Binding->CallKind,
              SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress);
    EXPECT_EQ(Binding->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::DarwinSDK);
    EXPECT_TRUE(Binding->WeakImport);

    HighFunc Function;
    Function.Name = "weak_framework_external_storage";
    Function.ReturnType = NdType::makeInt(8);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x2180, 8),
                                       NdType::makeInt(8));
    Function.Body = {Return};
    const auto Bound = sdk::bindObjCSourceReferences(Function, Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto *Address = sourceCall(Bound.Function);
    ASSERT_NE(Address, nullptr);
    EXPECT_TRUE(Address->SourceCallHint->WeakImport);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Address, Image, {}));

    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
    EXPECT_NE(Source.find(
                  "extern __attribute__((weak_import)) unsigned char "
                  "neverd_darwin_data_kCGImageDestinationEncodeRequest[] "
                  "__asm__(\"_kCGImageDestinationEncodeRequest\");"),
              std::string::npos)
        << Source;
    EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("0x2180"), std::string::npos) << Source;

    auto Forged = *Address;
    auto ForgedHint =
        std::make_shared<SourceCallTypeHint>(*Address->SourceCallHint);
    ForgedHint->WeakImport = false;
    Forged.SourceCallHint = std::move(ForgedHint);
    EXPECT_FALSE(sdk::objcSourceCallBound(Forged, Image, {}));

    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.DyldBindSlots[0x2180].WeakImport = false;
      if (Mutation == 1)
        Changed.DyldBindSlots[0x2180].Module += ".impostor";
      if (Mutation == 2)
        Changed.DyldBindSlots[0x2180].Addend = 8;
      if (Mutation == 3)
        Changed.DyldBindSlots.clear();
      if (Mutation == 4) {
        Changed.ImportPtrSlots[0x2180] += "Suffix";
        Changed.DyldBindSlots[0x2180].Name =
            Changed.ImportPtrSlots[0x2180];
      }
      EXPECT_FALSE(sdk::objcSourceCallBound(*Address, Changed, {}))
          << Mutation;
    }
  }
}

TEST(ObjCCallHints, MobileSDKDataKeepsExactFrameworkStorageIdentities) {
  const std::pair<llvm::StringRef, llvm::StringRef> Declarations[] = {
      {"UIApplicationDidReceiveMemoryWarningNotification", "UIKit"},
      {"UIApplicationWillTerminateNotification", "UIKit"},
      {"UIApplicationDidEnterBackgroundNotification", "UIKit"},
      {"UIBackgroundTaskInvalid", "UIKit"},
      {"UIAccessibilityTraitButton", "UIKit"},
      {"UIEdgeInsetsZero", "UIKit"},
      {"UIViewNoIntrinsicMetric", "UIKit"},
      {"kCIContextPriorityRequestLow", "CoreImage"},
      {"kCIContextUseSoftwareRenderer", "CoreImage"}};
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (auto [Name, Framework] : Declarations) {
      SCOPED_TRACE(Name.str());
      auto Image = runtimeImage(("_" + Name).str(), Architecture);
      const auto Module = ("/System/Library/Frameworks/" + Framework +
                           ".framework/" + Framework)
                              .str();
      Image.DyldBindSlots[0x2180] = {("_" + Name).str(), 0, Module, false};
      const auto Binding = darwinRuntimeGlobalAddressHint(Image, 0x2180);
      // These supplemental declarations cover ARM64 device and simulator.
      if ((Framework == "CoreImage" ||
           Name == "UIApplicationDidEnterBackgroundNotification") &&
          Architecture == Arch::X64) {
        EXPECT_FALSE(Binding);
        continue;
      }
      ASSERT_TRUE(Binding);
      auto Versioned = Image;
      Versioned.DyldBindSlots[0x2180].Module =
          ("/System/Library/Frameworks/" + Framework +
           ".framework/Versions/A/" + Framework)
              .str();
      EXPECT_EQ(darwinRuntimeGlobalAddressHint(Versioned, 0x2180).has_value(),
                Framework == "CoreImage");
      EXPECT_EQ(Binding->TargetName, Name);
      EXPECT_EQ(Binding->CallKind,
                SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress);
      EXPECT_EQ(Binding->Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinSDK);

      HighFunc Function;
      Function.Name = "framework_external_storage";
      Function.ReturnType = NdType::makeInt(8);
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x2180, 8),
                                         NdType::makeInt(8));
      Function.Body = {Return};
      const auto Bound = sdk::bindObjCSourceReferences(Function, Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      const auto *Address = sourceCall(Bound.Function);
      ASSERT_NE(Address, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Address, Image, {}));
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
      EXPECT_NE(Source.find("extern unsigned char neverd_darwin_data_" +
                            Name.str() + "[] __asm__(\"_" + Name.str() +
                            "\");"),
                std::string::npos)
          << Source;
      EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
      EXPECT_EQ(Source.find("0x2180"), std::string::npos) << Source;

      for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
        auto Changed = Image;
        if (Mutation == 0)
          Changed.DyldBindSlots[0x2180].Module += ".impostor";
        if (Mutation == 1)
          Changed.DyldBindSlots[0x2180].WeakImport = true;
        if (Mutation == 2)
          Changed.DyldBindSlots[0x2180].Addend = 8;
        if (Mutation == 3)
          Changed.DyldBindSlots.clear();
        if (Mutation == 4) {
          Changed.ImportPtrSlots[0x2180] += "Suffix";
          Changed.DyldBindSlots[0x2180].Name = Changed.ImportPtrSlots[0x2180];
        }
        if (Mutation == 5)
          Changed.DyldBindSlots[0x2180].Module =
              "/System/Library/Frameworks/Foundation.framework/Foundation";
        EXPECT_FALSE(darwinRuntimeGlobalAddressHint(Changed, 0x2180))
            << Mutation;
        EXPECT_FALSE(sdk::objcSourceCallBound(*Address, Changed, {}))
            << Mutation;
      }
    }
  }
}

TEST(ObjCCallHints, SwiftRuntimeDataKeepsExactExternalStorageIdentity) {
  constexpr llvm::StringLiteral Module = "/usr/lib/swift/libswiftCore.dylib";
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Import : {"__swiftEmptyArrayStorage",
                               "__swiftEmptyDictionarySingleton",
                               "__swiftEmptySetSingleton",
                               "_$sSSN",
                               "_$sSsN",
                               "_$ss11AnyHashableVN",
                               "_$sSbN",
                               "_$sSiN",
                               "_$sSuN",
                               "_$sSfN",
                               "_$sSdN",
                               "_$sypN",
                               "_$ss4Int8VN",
                               "_$ss5Int16VN",
                               "_$ss5Int32VN",
                               "_$ss5Int64VN",
                               "_$ss5UInt8VN",
                               "_$ss6UInt16VN",
                               "_$ss6UInt32VN",
                               "_$ss6UInt64VN",
                               "_$sSSSHsWP",
                               "_$ss11AnyHashableVSHsWP",
                               "_$sSiSHsWP",
                               "_$sSbSHsWP",
                               "_$sSuSHsWP",
                               "_$sSfSHsWP",
                               "_$sSdSHsWP"}) {
      auto Image = runtimeImage(Import, Architecture);
      Image.DyldBindSlots[0x2180] = {Import, 0, Module.str(), false};
      const auto Binding = darwinRuntimeGlobalAddressHint(Image, 0x2180);
      ASSERT_TRUE(Binding) << Import;
      EXPECT_EQ(Binding->TargetName, llvm::StringRef(Import).drop_front())
          << Import;
      EXPECT_EQ(Binding->CallKind,
                SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress);
      EXPECT_EQ(Binding->Signature.Origin,
                SourceFunctionTypeHint::OriginKind::SwiftRuntime);

      HighFunc Function;
      Function.Name = "empty_collection_storage";
      Function.ReturnType = NdType::makeInt(8);
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x2180, 8),
                                         NdType::makeInt(8));
      Function.Body = {Return};
      const auto Bound = sdk::bindObjCSourceReferences(Function, Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      const auto *Address = sourceCall(Bound.Function);
      ASSERT_NE(Address, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Address, Image, {}));
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
      const auto SourceName = llvm::StringRef(Import).drop_front().str();
      EXPECT_NE(Source.find("[] __asm__(\"_" + SourceName + "\");"),
                std::string::npos)
          << Source;
      EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
      EXPECT_EQ(Source.find("0x2180"), std::string::npos) << Source;

      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        auto Changed = Image;
        if (Mutation == 0)
          Changed.DyldBindSlots[0x2180].Module += ".impostor";
        if (Mutation == 1)
          Changed.DyldBindSlots[0x2180].WeakImport = true;
        if (Mutation == 2)
          Changed.DyldBindSlots[0x2180].Addend = 8;
        if (Mutation == 3)
          Changed.DyldBindSlots.clear();
        if (Mutation == 4) {
          Changed.ImportPtrSlots[0x2180] += "Suffix";
          Changed.DyldBindSlots[0x2180].Name =
              Changed.ImportPtrSlots[0x2180];
        }
        EXPECT_FALSE(darwinRuntimeGlobalAddressHint(Changed, 0x2180))
            << Import << " " << Mutation;
      }
    }
  }
}

TEST(ObjCCallHints, SwiftMetadataAccessorsAndUnknownNominalsAreNotData) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (const char *Name :
         {"_$sSSMa", "_$sSSMn", "_$s4Test6StringVN", "_$sSSNsuffix",
          "_$sSsNsuffix",
          "_$sypNsuffix", "_$sSSSHsWPsuffix", "_$s4Test6StringVSHsWP"}) {
      auto Image = runtimeImage(Name, Architecture);
      Image.DyldBindSlots[0x2180] = {
          Name, 0, "/usr/lib/swift/libswiftCore.dylib", false};
      EXPECT_FALSE(darwinRuntimeGlobalAddressHint(Image, 0x2180)) << Name;
    }
}

TEST(ObjCCallHints, SDKDataBindingsRequireExactExportsAndDataDeclarations) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      auto Image = runtimeImage("_NSDefaultRunLoopMode", Architecture);
      Image.DyldBindSlots[0x2180] = {
          "_NSDefaultRunLoopMode", 0,
          "/System/Library/Frameworks/Foundation.framework/Versions/C/"
          "Foundation",
          false};
      if (Mutation == 1)
        Image.DyldBindSlots.clear();
      if (Mutation == 2)
        Image.DyldBindSlots[0x2180].Module.clear();
      if (Mutation == 3)
        Image.DyldBindSlots[0x2180].Module = "/usr/lib/libSystem.B.dylib";
      if (Mutation == 4)
        Image.DyldBindSlots[0x2180].WeakImport = true;
      if (Mutation == 5)
        Image.DyldBindSlots[0x2180].Addend = 4;
      if (Mutation == 6)
        Image.ConflictingImportStorageSlots.insert(0x2180);
      if (Mutation == 7)
        Image.IsRelocatable = true;
      if (Mutation == 8)
        Image.Format = BinaryFormat::ELF;
      if (Mutation == 9)
        Image.ImportPtrSlots[0x2180] = "_different";
      const auto Binding = darwinRuntimeGlobalAddressHint(Image, 0x2180);
      EXPECT_EQ(bool(Binding), Mutation == 0 || Mutation == 4) << Mutation;
      if (Mutation == 4 && Binding)
        EXPECT_TRUE(Binding->WeakImport);
    }
    for (const char *Name :
         {"NSStringFromClass", "NSDefaultRunLoopMode_suffix"}) {
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      Image.DyldBindSlots[0x2180] = {
          "_" + std::string(Name), 0,
          "/System/Library/Frameworks/Foundation.framework/Foundation", false};
      EXPECT_FALSE(darwinRuntimeGlobalAddressHint(Image, 0x2180)) << Name;
    }
  }
}

TEST(ObjCCallHints, OptimizedRuntimeQueriesRetainByteResultsAndExactImports) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Name :
         {"objc_opt_isKindOfClass", "objc_opt_respondsToSelector"}) {
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      Image.DyldBindSlots[0x2180] = {"_" + std::string(Name), 0,
                                     "/usr/lib/libobjc.A.dylib", false};
      auto Hint = objcRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      EXPECT_EQ(Hint->TargetName, Name);
      EXPECT_EQ(Hint->Signature.ReturnType->Size, 1U);
      EXPECT_FALSE(Hint->Signature.ReturnType->IsSigned);
      EXPECT_EQ(Hint->Signature.ReturnLocation.ValueBytes, 1U);
      EXPECT_EQ(Hint->Signature.ReturnLocation.ExtendTo32Bits,
                Architecture == Arch::AArch64);
      ASSERT_EQ(Hint->Signature.Parameters.size(), 2U);
      for (const auto &Parameter : Hint->Signature.Parameters) {
        EXPECT_EQ(Parameter.Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Parameter.Location.ValueBytes, 8U);
      }
      auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      ASSERT_TRUE(Med.CallInfos.front().SourceCallHint);
      EXPECT_EQ(Med.CallInfos.front().Args.size(), 2U);
      for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
        auto Changed = Image;
        if (Mutation == 0)
          Changed.DyldBindSlots[0x2180].Module = "/tmp/libobjc.A.dylib";
        if (Mutation == 1)
          Changed.DyldBindSlots.clear();
        if (Mutation == 2)
          Changed.DyldBindSlots[0x2180].WeakImport = true;
        if (Mutation == 3)
          Changed.DyldBindSlots[0x2180].Addend = 8;
        if (Mutation == 4)
          Changed.ConflictingImportStorageSlots.insert(0x2180);
        if (Mutation == 5) {
          Changed.ImportPtrSlots[0x2180] += "_suffix";
          Changed.DyldBindSlots[0x2180].Name = Changed.ImportPtrSlots[0x2180];
        }
        if (Mutation == 6) {
          Changed.ImportPtrSlots[0x2180] = Name;
          Changed.DyldBindSlots[0x2180].Name = Name;
        }
        EXPECT_FALSE(objcRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
      }
    }
  }
}

namespace {
BinaryImage receiverImage(Arch Architecture, bool ClassMethod = false) {
  auto Image = image(Architecture);
  Image.ObjCMethods.front().Implementation = 0x1200;
  Image.ObjCMethods.front().IsClassMethod = ClassMethod;
  ObjCClass Class;
  Class.Name = "First";
  Class.RootClass = true;
  Class.InheritanceStatus = "root";
  Image.ObjCClasses.push_back(Class);
  auto Other = Image.ObjCMethods.front();
  Other.ClassName = "Other";
  Other.Implementation = 0x1400;
  Other.TypeHint->ReturnType = NdType::makePtr(NdType::makeVoid());
  Image.ObjCMethods.push_back(Other);
  return Image;
}

LowFunc receiverCaller(Arch Architecture) {
  auto Function = caller(Architecture);
  auto &Ops = Function.Blocks.front().Ops;
  const auto &TRI = getTargetRegInfo(Architecture);
  Ops.front() =
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1204);
  Ops.insert(Ops.begin(),
             operation(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8),
                       {NdVar::cst(0x2100, 8)}, 0x1200));
  Ops.back().Addr = 0x1208;
  return Function;
}

ExprPtr receiverCallExpression(const SourceCallTypeHint &Binding) {
  auto Call = HighExpr::makeCall("objc_msgSend", 0, {});
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Binding);
  Call->Type = Binding.Signature.ReturnType;
  for (const auto &Parameter : Binding.Signature.Parameters)
    Call->Operands.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
  return Call;
}
} // namespace

TEST(ObjCCallHints,
     FoundationNonescapingBlocksRequireExactParentAndCallbackDeclarations) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.ObjCMethods.clear();
    Image.ObjCProtocols.clear();
    Image.ObjCProperties.clear();
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    SourceCallTypeHint Call;
    Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Call.Selector = "indexesOfObjectsPassingTest:";
    const auto Parent = objcSelectorSourceTypeHint(Image, Call.Selector);
    ASSERT_TRUE(Parent);
    Call.Signature = *Parent;
    const auto Callback = objcNonEscapingBlockSignature(Image, Call, 2);
    ASSERT_TRUE(Callback);
    EXPECT_EQ(Callback->ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Callback->ReturnType->Size, 1U);
    EXPECT_EQ(Callback->Parameters.size(), 4U);

    auto DictionaryCall = Call;
    DictionaryCall.Selector = "enumerateKeysAndObjectsUsingBlock:";
    const auto DictionaryParent =
        objcSelectorSourceTypeHint(Image, DictionaryCall.Selector);
    ASSERT_TRUE(DictionaryParent);
    DictionaryCall.Signature = *DictionaryParent;
    const auto DictionaryCallback =
        objcNonEscapingBlockSignature(Image, DictionaryCall, 2);
    EXPECT_EQ(DictionaryCallback.has_value(), Architecture == Arch::AArch64);
    if (DictionaryCallback)
      EXPECT_EQ(DictionaryCallback->Parameters.size(), 4U);

    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto ChangedImage = Image;
      auto ChangedCall = Call;
      unsigned Parameter = 2;
      if (Mutation == 0)
        ChangedImage.DynInfo.NeededLibs.front() =
            "/tmp/Foundation.framework/Foundation";
      if (Mutation == 1)
        ChangedCall.Signature.Parameters.pop_back();
      if (Mutation == 2)
        ChangedCall.Signature.Origin =
            SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      if (Mutation == 3)
        ChangedCall.Selector = "enumerateObjectsUsingBlock:";
      if (Mutation == 4)
        Parameter = 1;
      EXPECT_FALSE(
          objcNonEscapingBlockSignature(ChangedImage, ChangedCall, Parameter))
          << Mutation;
    }
  }
}

TEST(ObjCCallHints,
     FoundationNonescapingBlocksFollowRevalidatedReceiverHierarchy) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverImage(Architecture);
    auto &Class = Image.ObjCClasses.front();
    Class.RootClass = false;
    Class.InheritanceStatus = "resolved";
    Class.SuperclassName = "NSArray";
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Receiver);
    SourceCallTypeHint Call;
    Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Call.Selector = "enumerateObjectsUsingBlock:";
    Call.Receiver = *Receiver;
    const auto Parent =
        objcReceiverSourceTypeHint(Image, Call.Selector, *Receiver);
    ASSERT_TRUE(Parent.Signature);
    Call.Signature = *Parent.Signature;
    const auto ArrayCallback = objcNonEscapingBlockSignature(Image, Call, 2);
    ASSERT_TRUE(ArrayCallback);
    EXPECT_EQ(ArrayCallback->Parameters.size(), 4U);

    auto OptionsCall = Call;
    OptionsCall.Selector = "enumerateObjectsWithOptions:usingBlock:";
    const auto OptionsParent =
        objcReceiverSourceTypeHint(Image, OptionsCall.Selector, *Receiver);
    ASSERT_TRUE(OptionsParent.Signature);
    OptionsCall.Signature = *OptionsParent.Signature;
    const auto ArrayOptionsCallback =
        objcNonEscapingBlockSignature(Image, OptionsCall, 3);
    EXPECT_EQ(ArrayOptionsCallback.has_value(), Architecture == Arch::AArch64);
    if (ArrayOptionsCallback)
      EXPECT_EQ(ArrayOptionsCallback->Parameters.size(), 4U);

    auto SortedCall = Call;
    SortedCall.Selector = "sortedArrayUsingComparator:";
    const auto SortedParent =
        objcReceiverSourceTypeHint(Image, SortedCall.Selector, *Receiver);
    ASSERT_TRUE(SortedParent.Signature);
    SortedCall.Signature = *SortedParent.Signature;
    const auto SortedCallback =
        objcNonEscapingBlockSignature(Image, SortedCall, 2);
    ASSERT_TRUE(SortedCallback);
    EXPECT_EQ(SortedCallback->ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(SortedCallback->ReturnType->Size, 8U);
    EXPECT_EQ(SortedCallback->Parameters.size(), 3U);

    auto SetImage = Image;
    SetImage.ObjCClasses.front().SuperclassName = "NSSet";
    const auto SetParent =
        objcReceiverSourceTypeHint(SetImage, Call.Selector, *Receiver);
    ASSERT_TRUE(SetParent.Signature);
    auto SetCall = Call;
    SetCall.Signature = *SetParent.Signature;
    const auto SetCallback =
        objcNonEscapingBlockSignature(SetImage, SetCall, 2);
    ASSERT_TRUE(SetCallback);
    EXPECT_EQ(SetCallback->Parameters.size(), 3U);

    const auto SetOptionsParent =
        objcReceiverSourceTypeHint(SetImage, OptionsCall.Selector, *Receiver);
    ASSERT_TRUE(SetOptionsParent.Signature);
    auto SetOptionsCall = OptionsCall;
    SetOptionsCall.Signature = *SetOptionsParent.Signature;
    const auto SetOptionsCallback =
        objcNonEscapingBlockSignature(SetImage, SetOptionsCall, 3);
    EXPECT_EQ(SetOptionsCallback.has_value(), Architecture == Arch::AArch64);
    if (SetOptionsCallback)
      EXPECT_EQ(SetOptionsCallback->Parameters.size(), 3U);

    auto Unqualified = Call;
    Unqualified.Receiver.reset();
    EXPECT_FALSE(objcNonEscapingBlockSignature(Image, Unqualified, 2));
    auto Unknown = Image;
    Unknown.ObjCClasses.front().SuperclassName = "UnknownCollection";
    EXPECT_FALSE(objcNonEscapingBlockSignature(Unknown, Call, 2));

    auto DictionaryImage = Image;
    DictionaryImage.ObjCClasses.front().SuperclassName = "NSDictionary";
    auto DictionaryCall = Call;
    DictionaryCall.Selector =
        "keysSortedByValueWithOptions:usingComparator:";
    const auto DictionaryParent = objcReceiverSourceTypeHint(
        DictionaryImage, DictionaryCall.Selector, *Receiver);
    ASSERT_TRUE(DictionaryParent.Signature);
    DictionaryCall.Signature = *DictionaryParent.Signature;
    const auto DictionaryCallback = objcNonEscapingBlockSignature(
        DictionaryImage, DictionaryCall, 3);
    ASSERT_TRUE(DictionaryCallback);
    EXPECT_EQ(DictionaryCallback->ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(DictionaryCallback->ReturnType->Size, 8U);
    EXPECT_EQ(DictionaryCallback->Parameters.size(), 3U);

    auto RegularExpressionImage = Image;
    RegularExpressionImage.ObjCClasses.front().SuperclassName =
        "NSRegularExpression";
    auto RegularExpressionCall = Call;
    RegularExpressionCall.Selector =
        "enumerateMatchesInString:options:range:usingBlock:";
    const auto RegularExpressionParent = objcReceiverSourceTypeHint(
        RegularExpressionImage, RegularExpressionCall.Selector, *Receiver);
    ASSERT_TRUE(RegularExpressionParent.Signature);
    RegularExpressionCall.Signature = *RegularExpressionParent.Signature;
    const auto RegularExpressionCallback = objcNonEscapingBlockSignature(
        RegularExpressionImage, RegularExpressionCall, 5);
    EXPECT_EQ(RegularExpressionCallback.has_value(),
              Architecture == Arch::AArch64);
    if (RegularExpressionCallback)
      EXPECT_EQ(RegularExpressionCallback->Parameters.size(), 4U);
  }
}

TEST(ObjCCallHints,
     CoreDataNonescapingBlocksFollowRevalidatedReceiverHierarchy) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (llvm::StringRef Owner : {"NSManagedObjectContext",
                                  "NSPersistentStoreCoordinator"}) {
      auto Image = receiverImage(Architecture);
      auto &Class = Image.ObjCClasses.front();
      Class.RootClass = false;
      Class.InheritanceStatus = "resolved";
      Class.SuperclassName = Owner.str();
      Image.DynInfo.NeededLibs = {
          "/System/Library/Frameworks/CoreData.framework/CoreData",
          "/System/Library/Frameworks/Foundation.framework/Foundation"};
      const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
      ASSERT_TRUE(Receiver);
      SourceCallTypeHint Call;
      Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
      Call.Selector = "performBlockAndWait:";
      Call.Receiver = *Receiver;
      const auto Parent =
          objcReceiverSourceTypeHint(Image, Call.Selector, *Receiver);
      ASSERT_TRUE(Parent.Signature);
      Call.Signature = *Parent.Signature;
      const auto Callback = objcNonEscapingBlockSignature(Image, Call, 2);
      ASSERT_TRUE(Callback) << Owner.str();
      EXPECT_EQ(Callback->ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Callback->Parameters.size(), 1U);

      auto Unknown = Image;
      Unknown.ObjCClasses.front().SuperclassName = "NSPersistentContainer";
      EXPECT_FALSE(objcNonEscapingBlockSignature(Unknown, Call, 2));
      EXPECT_FALSE(objcNonEscapingBlockSignature(Image, Call, 1));
    }
  }
}

TEST(ObjCCallHints,
     UIKitNonescapingBlocksFollowRevalidatedReceiverHierarchy) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverImage(Architecture);
    auto &Class = Image.ObjCClasses.front();
    Class.RootClass = false;
    Class.InheritanceStatus = "resolved";
    Class.SuperclassName = "UIGraphicsImageRenderer";
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/UIKit.framework/UIKit",
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Receiver);
    SourceCallTypeHint Call;
    Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Call.Selector = "imageWithActions:";
    Call.Receiver = *Receiver;
    const auto Parent =
        objcReceiverSourceTypeHint(Image, Call.Selector, *Receiver);
    ASSERT_TRUE(Parent.Signature);
    Call.Signature = *Parent.Signature;
    const auto Callback = objcNonEscapingBlockSignature(Image, Call, 2);
    ASSERT_TRUE(Callback);
    EXPECT_EQ(Callback->ReturnType->Kind, NdTypeKind::Void);
    EXPECT_EQ(Callback->Parameters.size(), 2U);

    auto Unknown = Image;
    Unknown.ObjCClasses.front().SuperclassName = "UIGraphicsPDFRenderer";
    EXPECT_FALSE(objcNonEscapingBlockSignature(Unknown, Call, 2));
    EXPECT_FALSE(objcNonEscapingBlockSignature(Image, Call, 1));
  }
}

TEST(ObjCCallHints, SuperDispatchUsesExactCurrentClassSuperclassDeclaration) {
  auto Image = receiverImage(Arch::AArch64);
  auto &Class = Image.ObjCClasses.front();
  Class.RootClass = false;
  Class.SuperclassName = "UIImage";
  Class.InheritanceStatus = "resolved";
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/UIKit.framework/UIKit",
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  Image.ImportPtrSlots[0x2180] = "_objc_msgSendSuper2";
  Image.ObjCSourceReferences[0x2100] = {
      ObjCSourceReference::Kind::Selector, 0x2100, 8,
      "initWithCGImage:scale:orientation:"};
  Image.ObjCSourceReferences[0x2120] = {
      ObjCSourceReference::Kind::Class, 0x2120, 8, "First"};

  auto Function = caller();
  auto &Block = Function.Blocks.front();
  Block.Ops = {
      operation(NdOp::INT_SUB, NdVar::reg(a64reg::SP, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(32, 4)}, 0x1200),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X8, 8),
                {NdVar::reg(a64reg::SP, 8), NdVar::cst(16, 4)}, 0x1204),
      operation(NdOp::STORE, {},
                {NdVar::reg(a64reg::X8, 8), NdVar::reg(a64reg::X0, 8)},
                0x1208),
      operation(NdOp::INT_ADD, NdVar::reg(a64reg::X9, 8),
                {NdVar::reg(a64reg::X8, 8), NdVar::cst(8, 4)}, 0x120c),
      operation(NdOp::LOAD, NdVar::reg(a64reg::X10, 8),
                {NdVar::cst(0x2120, 8)}, 0x1210),
      operation(NdOp::STORE, {},
                {NdVar::reg(a64reg::X9, 8), NdVar::reg(a64reg::X10, 8)},
                0x1214),
      operation(NdOp::LOAD, NdVar::reg(a64reg::X1, 8),
                {NdVar::cst(0x2100, 8)}, 0x1218),
      operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                {NdVar::reg(a64reg::X8, 8)}, 0x121c),
      operation(NdOp::INDIR_CALL, NdVar::reg(a64reg::X0, 8),
                {NdVar::cst(0x2180, 8)}, 0x1220),
      operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x1224)};
  Block.EndAddr = 0x1228;

  const auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x1220);
  EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::ObjCSuper2);
  ASSERT_TRUE(Hint.Receiver);
  EXPECT_EQ(Hint.Receiver->Origin,
            ObjCReceiverTypeHint::OriginKind::ClassReference);
  EXPECT_EQ(Hint.Receiver->ClassName, "First");
  EXPECT_EQ(Hint.Signature.Parameters.size(), 5U);
  auto Expression = receiverCallExpression(Hint);
  EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));

  auto UntypedReceiver = Function;
  UntypedReceiver.Blocks.front().Ops[2].Inputs[1] =
      NdVar::reg(a64reg::X11, 8);
  EXPECT_EQ(buildObjCSourceCallHints(Image, UntypedReceiver).size(), 1U);

  auto EscapedHigherFrame = Function;
  auto &EscapedOps = EscapedHigherFrame.Blocks.front().Ops;
  for (size_t I = 1; I < EscapedOps.size(); ++I)
    EscapedOps[I].Addr += 0x10;
  EscapedOps.insert(
      EscapedOps.begin() + 1,
      {operation(NdOp::COPY, NdVar::reg(a64reg::X19, 8),
                 {NdVar::reg(a64reg::X0, 8)}, 0x1204),
       operation(NdOp::INT_ADD, NdVar::reg(a64reg::X0, 8),
                 {NdVar::reg(a64reg::SP, 8), NdVar::cst(48, 4)}, 0x1208),
       operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2190, 8)}, 0x120c)});
  EscapedOps[5].Inputs[1] = NdVar::reg(a64reg::X19, 8);
  const auto EscapedHints =
      buildObjCSourceCallHints(Image, EscapedHigherFrame);
  EXPECT_EQ(EscapedHints.size(), 1U);
  EXPECT_EQ(EscapedHints.count(0x1230), 1U);

  auto UnprovedFrame = Function;
  UnprovedFrame.Blocks.front().Ops[4].Inputs[0] = NdVar::cst(0x2130, 8);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, UnprovedFrame).empty());
  auto MismatchedClass = Image;
  MismatchedClass.ObjCSourceReferences.at(0x2120).Name = "Other";
  EXPECT_TRUE(buildObjCSourceCallHints(MismatchedClass, Function).empty());

  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Changed = Image;
    if (Mutation == 0)
      Changed.ObjCSourceReferences.erase(0x2120);
    else if (Mutation == 1)
      Changed.ObjCSourceReferences.at(0x2120).Name = "Other";
    else if (Mutation == 2)
      Changed.ObjCClasses.front().InheritanceStatus = "unresolved";
    else if (Mutation == 3)
      Changed.ObjCClasses.front().SuperclassName.clear();
    else
      Changed.DynInfo.NeededLibs.front() = "/tmp/UIKit.framework/UIKit";
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Changed, {}))
        << Mutation;
  }
}

TEST(ObjCCallHints,
     AuthenticatedUnknownMessagesPreserveCalleeSavedReceiverIdentity) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const bool Restore : {false, true}) {
      const auto Image = receiverImage(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto Saved = NdVar::reg(preservedFactRegister(Architecture), 8);
      auto Function = receiverCaller(Architecture);
      auto &Ops = Function.Blocks.front().Ops;
      Ops = {
          operation(NdOp::COPY, Saved,
                    {NdVar::reg(TRI.IntParamRegs[0], 8)}, 0x1200),
          operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1204),
          operation(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8),
                    {NdVar::cst(0x2100, 8)}, 0x120c),
          operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1210),
          operation(NdOp::RETURN, {}, {}, 0x1214),
      };
      if (Restore)
        Ops.insert(Ops.begin() + 2,
                   operation(NdOp::COPY,
                             NdVar::reg(TRI.IntParamRegs[0], 8), {Saved},
                             0x1208));
      Function.Blocks.front().EndAddr = 0x1218;
      const auto Hints = buildObjCSourceCallHints(Image, Function);
      EXPECT_EQ(Hints.count(0x1204), 0U);
      EXPECT_EQ(Hints.count(0x1210), Restore ? 1U : 0U);
      if (Restore) {
        ASSERT_TRUE(Hints.at(0x1210).Receiver);
        EXPECT_EQ(Hints.at(0x1210).Receiver->ClassName, "First");
      }
    }
}

TEST(ObjCCallHints, ReceiverDeclarationsSeparateOwnersAndDispatchRoles) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const bool ClassMethod : {false, true}) {
      auto Image = receiverImage(Architecture, ClassMethod);
      auto Opposite = Image.ObjCMethods.front();
      Opposite.IsClassMethod = !ClassMethod;
      Opposite.Implementation = 0x1500;
      Opposite.TypeHint->ReturnType = NdType::makeFloat(8);
      Image.ObjCMethods.push_back(Opposite);
      EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "scale:"));
      const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
      ASSERT_TRUE(Receiver);
      for (const bool Reverse : {false, true}) {
        if (Reverse)
          std::reverse(Image.ObjCMethods.begin(), Image.ObjCMethods.end());
        const auto Hint =
            objcReceiverSourceTypeHint(Image, "scale:", *Receiver);
        ASSERT_TRUE(Hint.HasDeclaration);
        ASSERT_TRUE(Hint.Signature);
        EXPECT_EQ(Hint.Signature->ReturnType->Kind, NdTypeKind::Int);
        EXPECT_EQ(Hint.Signature->ReturnType->Size, 4U);
        const auto Hints =
            buildObjCSourceCallHints(Image, receiverCaller(Architecture));
        ASSERT_EQ(Hints.size(), 1U);
        ASSERT_TRUE(Hints.at(0x1204).Receiver);
        EXPECT_EQ(Hints.at(0x1204).Receiver->IsClassMethod, ClassMethod);
      }
    }
  }
}

TEST(ObjCCallHints, ReceiverSDKInheritanceRequiresCurrentFrameworkIdentity) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverImage(Architecture);
    for (auto &Method : Image.ObjCMethods)
      Method.Selector = "code";
    Image.ObjCMethods.front().Selector = "readCode";
    auto &Class = Image.ObjCClasses.front();
    Class.RootClass = false;
    Class.SuperclassName = "NSError";
    Class.InheritanceStatus = "resolved";
    const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Receiver);
    for (const auto *Module :
         {"/System/Library/Frameworks/Foundation.framework/Foundation",
          "/System/Library/Frameworks/Foundation.framework/Versions/C/"
          "Foundation"}) {
      Image.DynInfo.NeededLibs = {Module};
      EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "code"));
      const auto Hint = objcReceiverSourceTypeHint(Image, "code", *Receiver);
      ASSERT_TRUE(Hint.Signature);
      EXPECT_EQ(Hint.Signature->ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint.Signature->ReturnType->Size, 8U);
      EXPECT_TRUE(Hint.Signature->ReturnType->IsSigned);
      EXPECT_EQ(Hint.Signature->Origin,
                SourceFunctionTypeHint::OriginKind::ObjCSDK);
    }
    for (const auto *Module : {"", "/tmp/Foundation.framework/Foundation"}) {
      Image.DynInfo.NeededLibs = {Module};
      EXPECT_FALSE(
          objcReceiverSourceTypeHint(Image, "code", *Receiver).Signature);
    }
  }
}

TEST(ObjCCallHints,
     ReceiverCategoriesPropertiesAndSubclassesKeepNegativeEvidence) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      auto Image = receiverImage(Architecture);
      const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
      ASSERT_TRUE(Receiver);
      if (Mutation == 0 || Mutation == 1) {
        auto Conflict = Image.ObjCMethods.back();
        Conflict.ClassName = "First";
        Conflict.CategoryName = "Extension";
        if (Mutation == 1)
          Conflict.TypeHint.reset();
        Image.ObjCMethods.push_back(Conflict);
      } else if (Mutation == 2) {
        ObjCProperty Property;
        Property.Owner = ObjCProperty::OwnerKind::Category;
        Property.ClassName = "First";
        Property.Getter = "scale:";
        Image.ObjCProperties.push_back(Property);
      } else {
        ObjCClass Child;
        Child.Name = "Other";
        Child.SuperclassName = "First";
        Child.InheritanceStatus = "resolved";
        Image.ObjCClasses.push_back(Child);
      }
      for (const bool Reverse : {false, true}) {
        if (Reverse)
          std::reverse(Image.ObjCMethods.begin(), Image.ObjCMethods.end());
        const auto Hint =
            objcReceiverSourceTypeHint(Image, "scale:", *Receiver);
        EXPECT_TRUE(Hint.HasDeclaration);
        EXPECT_FALSE(Hint.Signature) << Mutation;
        EXPECT_TRUE(
            buildObjCSourceCallHints(Image, receiverCaller(Architecture))
                .empty());
      }
    }
  }
}

TEST(ObjCCallHints, ReceiverHierarchyRejectsCyclesMissingParentsAndBounds) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto Image = receiverImage(Architecture);
      const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
      ASSERT_TRUE(Receiver);
      auto &Class = Image.ObjCClasses.front();
      if (Mutation == 0) {
        Class.InheritanceStatus = "unresolved";
      } else if (Mutation == 1 || Mutation == 2) {
        Class.RootClass = false;
        Class.InheritanceStatus = "resolved";
        Class.SuperclassName = Mutation == 1 ? "Missing" : "First";
      } else if (Mutation == 3) {
        auto Conflict = Class;
        Conflict.RootClass = false;
        Conflict.InheritanceStatus = "resolved";
        Conflict.SuperclassName = "Other";
        Image.ObjCClasses.push_back(Conflict);
      } else {
        for (unsigned I = 0; I != 256; ++I) {
          ObjCClass Child;
          Child.Name = "Child" + std::to_string(I);
          Child.SuperclassName = "First";
          Child.InheritanceStatus = "resolved";
          Image.ObjCClasses.push_back(Child);
        }
      }
      const auto Hint = objcReceiverSourceTypeHint(Image, "scale:", *Receiver);
      EXPECT_TRUE(Hint.HasDeclaration);
      EXPECT_FALSE(Hint.Signature) << Mutation;
    }
  }
}

TEST(ObjCCallHints, ReceiverProtocolsUseRecordedAdoptionAndSeparateNamespaces) {
  auto Image = receiverImage(Arch::AArch64);
  auto &Class = Image.ObjCClasses.front();
  Class.RootClass = false;
  Class.InheritanceStatus = "resolved";
  Class.SuperclassName = "NSObject";
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
  ASSERT_TRUE(Receiver);
  ASSERT_TRUE(objcReceiverSourceTypeHint(Image, "scale:", *Receiver).Signature);
  ObjCProtocol Protocol;
  Protocol.Name = "Unrelated";
  Protocol.Address = 0x2500;
  Protocol.Status = "recovered";
  ObjCProtocolMethod Method;
  Method.Selector = "scale:";
  Protocol.Methods.push_back(Method);
  Image.ObjCProtocols.push_back(Protocol);
  EXPECT_TRUE(objcReceiverSourceTypeHint(Image, "scale:", *Receiver).Signature);
  ObjCProtocol Root;
  Root.Name = "NSObject";
  Root.Address = 0x2600;
  Root.Status = "recovered";
  Root.AdoptedProtocols = {Protocol.Address};
  Image.ObjCProtocols.push_back(Root);
  EXPECT_FALSE(
      objcReceiverSourceTypeHint(Image, "scale:", *Receiver).Signature);
  Image.ObjCProtocols.front().Methods.clear();
  EXPECT_TRUE(objcReceiverSourceTypeHint(Image, "scale:", *Receiver).Signature);
  Image.ObjCProtocols.front().AdoptedProtocols = {Root.Address};
  EXPECT_FALSE(
      objcReceiverSourceTypeHint(Image, "scale:", *Receiver).Signature);
}

TEST(ObjCCallHints, ReceiverEntryRequiresEveryAliasedMethodToAgree) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto Image = receiverImage(Architecture);
      auto Alias = Image.ObjCMethods.front();
      Alias.Selector = "alias";
      if (Mutation == 1)
        Alias.ClassName = "Other";
      else if (Mutation == 2)
        Alias.IsClassMethod = true;
      else if (Mutation == 3)
        Alias.TypeHint.reset();
      else if (Mutation == 4)
        Alias.ClassName.clear();
      Image.ObjCMethods.push_back(Alias);
      EXPECT_EQ(bool(objcMethodReceiverTypeHint(Image, 0x1200)), Mutation == 0);
      EXPECT_EQ(
          buildObjCSourceCallHints(Image, receiverCaller(Architecture)).size(),
          Mutation == 0 ? 1U : 0U);
    }
  }
}

TEST(ObjCCallHints, ReceiverClassReferencesRequireExactSlotsAndFullWidth) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto Image = receiverImage(Architecture, true);
      Image.ObjCMethods.front().Implementation = 0x1300;
      const auto &TRI = getTargetRegInfo(Architecture);
      ObjCSourceReference Ref;
      Ref.TheKind = ObjCSourceReference::Kind::Class;
      Ref.Address = 0x2200;
      Ref.Name = "First";
      if (Mutation == 1)
        Ref.TheKind = ObjCSourceReference::Kind::Metaclass;
      else if (Mutation == 2)
        Ref.Address += 8;
      else if (Mutation == 3)
        Ref.Size = 4;
      Image.ObjCSourceReferences[0x2200] = Ref;
      auto Function = receiverCaller(Architecture);
      Function.Blocks.front().Ops.insert(
          Function.Blocks.front().Ops.begin(),
          operation(NdOp::LOAD,
                    NdVar::reg(TRI.IntParamRegs[0], Mutation == 4 ? 4 : 8),
                    {NdVar::cst(0x2200, 8)}, 0x11fc));
      const auto Hints = buildObjCSourceCallHints(Image, Function);
      ASSERT_EQ(Hints.size(), Mutation == 0 ? 1U : 0U);
      if (Mutation == 0) {
        const auto &Hint = Hints.at(0x1204);
        ASSERT_TRUE(Hint.Receiver);
        EXPECT_EQ(Hint.Receiver->Origin,
                  ObjCReceiverTypeHint::OriginKind::ClassReference);
        auto Expression = receiverCallExpression(Hint);
        EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
        Image.ObjCSourceReferences.clear();
        EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
      }
    }
  }
}

TEST(ObjCCallHints, ReceiverEntryBackedgesAndIndependentEntriesEraseSelfFacts) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto Image = receiverImage(Architecture);
      auto Function = receiverCaller(Architecture);
      auto &Entry = Function.Blocks.front();
      Entry.Succs = {1};
      LowBlock Back;
      Back.Id = 1;
      Back.StartAddr = 0x1300;
      Back.Preds = {Entry.Id};
      if (Mutation <= 1) {
        Back.Succs = {Entry.Id};
        Entry.Preds = {Back.Id};
      }
      const auto &TRI = getTargetRegInfo(Architecture);
      if (Mutation == 1)
        Back.Ops.push_back(operation(NdOp::COPY,
                                     NdVar::reg(TRI.IntParamRegs[0], 4),
                                     {NdVar::cst(0, 4)}, 0x1300));
      // The message call itself invalidates caller-saved self on the backedge.
      if (Mutation <= 1)
        EXPECT_TRUE(buildObjCSourceCallHints(Image,
                                             [&] {
                                               auto F = Function;
                                               F.Blocks.push_back(Back);
                                               return F;
                                             }())
                        .empty());
      else {
        Back.Ops = Entry.Ops;
        for (auto &Op : Back.Ops)
          Op.Addr += 0x100;
        if (Mutation == 2)
          Function.ModuleAnalysisRoots.insert(Back.StartAddr);
        else if (Mutation == 3)
          Function.OrdinaryModuleAnalysisRoots.insert(Back.StartAddr);
        else
          Back.ExceptionalPreds.emplace_back();
        Function.Blocks.push_back(Back);
        const auto Hints = buildObjCSourceCallHints(Image, Function);
        EXPECT_TRUE(Hints.count(0x1204));
        EXPECT_FALSE(Hints.count(0x1304));
      }
    }
  }
}

TEST(ObjCCallHints, ReceiverSourceBindingsRevalidateProvenanceAndDeclarations) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Image = receiverImage(Architecture);
    const auto Hints =
        buildObjCSourceCallHints(Image, receiverCaller(Architecture));
    ASSERT_EQ(Hints.size(), 1U);
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      auto Changed = Image;
      auto Expression = receiverCallExpression(Hints.at(0x1204));
      auto Binding =
          std::make_shared<SourceCallTypeHint>(*Expression->SourceCallHint);
      Expression->SourceCallHint = Binding;
      ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Changed, {}));
      if (Mutation == 0)
        Binding->Receiver->ClassName = "Other";
      else if (Mutation == 1)
        Binding->Receiver->Address += 4;
      else if (Mutation == 2)
        Binding->Receiver->IsClassMethod = true;
      else if (Mutation == 3)
        Binding->Receiver->Origin =
            ObjCReceiverTypeHint::OriginKind::ClassReference;
      else if (Mutation == 4)
        Binding->CallKind = SourceCallTypeHint::Kind::ObjCSuper2;
      else if (Mutation == 5)
        Binding->Format = SourceCallTypeHint::FormatArguments{};
      else if (Mutation == 6)
        Changed.ObjCMethods.front().TypeHint.reset();
      else
        Changed.ObjCMethods.back().ClassName = "First";
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Changed, {}))
          << Mutation;
    }
  }
}

TEST(ObjCCallHints, ReceiverFactsSurviveOnlyAgreedCopiesAndPreservedViews) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
      auto Image = receiverImage(Architecture);
      Image.ImportPtrSlots[0x2188] = "_objc_retain";
      const auto Self = NdVar::reg(TRI.IntParamRegs[0], 8);
      const auto Saved = NdVar::reg(TRI.CalleeSaveRegs.front(), 8);
      LowFunc Function;
      Function.Entry = 0x1200;
      LowBlock Entry;
      Entry.Id = 0;
      Entry.StartAddr = 0x1200;
      Entry.Succs = {1, 2};
      Entry.Ops = {operation(NdOp::COPY, Saved, {Self})};
      LowBlock Left;
      Left.Id = 1;
      Left.StartAddr = 0x1300;
      Left.Preds = {0};
      Left.Succs = {3};
      Left.Ops = {
          operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2188, 8)}, 0x1300)};
      LowBlock Right = Left;
      Right.Id = 2;
      Right.StartAddr = 0x1400;
      Right.Ops.front().Addr = 0x1400;
      if (Mutation == 1)
        Right.Ops.push_back(operation(NdOp::COPY, NdVar::reg(Saved.Offset, 4),
                                      {NdVar::cst(0, 4)}, 0x1404));
      else if (Mutation == 2)
        Right.Ops.front().Inputs[0] = NdVar::cst(0x2190, 8);
      else if (Mutation == 3)
        Function.ModuleAnalysisRoots.insert(Right.StartAddr);
      else if (Mutation == 4)
        Function.OrdinaryModuleAnalysisRoots.insert(Right.StartAddr);
      else if (Mutation == 5)
        Right.ExceptionalPreds.emplace_back();
      auto Join = receiverCaller(Architecture).Blocks.front();
      Join.Id = 3;
      Join.StartAddr = 0x1500;
      Join.Preds = {1, 2};
      for (auto &Op : Join.Ops)
        Op.Addr += 0x300;
      Join.Ops.insert(Join.Ops.begin(),
                      operation(NdOp::COPY, Self, {Saved}, 0x14fc));
      std::vector<LowBlock> Blocks{Entry, Left, Right, Join};
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        Function.Blocks.clear();
        for (auto Index : Order)
          Function.Blocks.push_back(Blocks[Index]);
        const auto Hints = buildObjCSourceCallHints(Image, Function);
        EXPECT_EQ(Hints.count(0x1504), Mutation == 0) << Mutation;
        if (Mutation == 0) {
          ASSERT_TRUE(Hints.at(0x1504).Receiver);
          EXPECT_EQ(Hints.at(0x1504).Receiver->Address, 0x1200U);
        }
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
  }
}

TEST(ObjCCallHints, ReceiverExactClassDispatchExcludesDerivedOverrides) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverImage(Architecture, true);
    ObjCClass Child;
    Child.Name = "Other";
    Child.SuperclassName = "First";
    Child.InheritanceStatus = "resolved";
    Image.ObjCClasses.push_back(Child);
    const auto Self = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Self);
    EXPECT_FALSE(objcReceiverSourceTypeHint(Image, "scale:", *Self).Signature);
    ObjCSourceReference Ref;
    Ref.TheKind = ObjCSourceReference::Kind::Class;
    Ref.Address = 0x2200;
    Ref.Name = "First";
    Image.ObjCSourceReferences[Ref.Address] = Ref;
    const ObjCReceiverTypeHint Exact{
        ObjCReceiverTypeHint::OriginKind::ClassReference, Ref.Address, Ref.Name,
        true};
    ASSERT_TRUE(objcReceiverSourceTypeHint(Image, "scale:", Exact).Signature);
  }
}

TEST(ObjCCallHints, ReceiverFrameworkVariadicsRetainTheirFormatRequirement) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverImage(Architecture, true);
    Image.ObjCMethods.clear();
    Image.ObjCClasses.clear();
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    ObjCSourceReference Ref;
    Ref.TheKind = ObjCSourceReference::Kind::Class;
    Ref.Address = 0x2200;
    Ref.Name = "NSString";
    Image.ObjCSourceReferences[Ref.Address] = Ref;
    const ObjCReceiverTypeHint Exact{
        ObjCReceiverTypeHint::OriginKind::ClassReference, Ref.Address, Ref.Name,
        true};
    const auto Decl =
        objcReceiverSourceTypeHint(Image, "stringWithFormat:", Exact);
    EXPECT_TRUE(Decl.HasDeclaration);
    EXPECT_FALSE(Decl.Signature);
    auto Function = receiverCaller(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    Image.ObjCSourceReferences.at(0x2100).Name = "stringWithFormat:";
    Function.Blocks.front().Ops.insert(
        Function.Blocks.front().Ops.begin(),
        operation(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[0], 8),
                  {NdVar::cst(Ref.Address, 8)}, 0x11fc));
    EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
  }
}

TEST(ObjCCallHints,
     MissingExternalHierarchyRequiresGlobalDeclarationAgreement) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      auto Image = receiverImage(Architecture);
      auto &Class = Image.ObjCClasses.front();
      Class.RootClass = false;
      Class.InheritanceStatus = "resolved";
      Class.SuperclassName = "ExternalBase";
      if (Mutation == 0)
        Image.ObjCMethods.back().TypeHint = Image.ObjCMethods.front().TypeHint;
      else if (Mutation == 2)
        Image.ObjCMethods.front().TypeHint->ReturnType = NdType::makeFloat(16);
      const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
      if (Mutation == 2) {
        EXPECT_FALSE(Receiver);
        continue;
      }
      ASSERT_TRUE(Receiver);
      const auto Decl = objcReceiverSourceTypeHint(Image, "scale:", *Receiver);
      EXPECT_TRUE(Decl.HasDeclaration);
      EXPECT_TRUE(Decl.RequiresGlobalAgreement);
      EXPECT_FALSE(Decl.Signature);
      const auto Hints =
          buildObjCSourceCallHints(Image, receiverCaller(Architecture));
      ASSERT_EQ(Hints.size(), Mutation == 0 ? 1U : 0U);
      if (Mutation == 0)
        EXPECT_FALSE(Hints.at(0x1204).Receiver);
      // Missing hierarchy never suppresses an explicitly unsupported member.
      ObjCProperty Property;
      Property.ClassName = "First";
      Property.Getter = "scale:";
      Image.ObjCProperties.push_back(Property);
      const auto Rejected =
          objcReceiverSourceTypeHint(Image, "scale:", *Receiver);
      EXPECT_TRUE(Rejected.HasDeclaration);
      EXPECT_FALSE(Rejected.RequiresGlobalAgreement);
      EXPECT_FALSE(Rejected.Signature);
      EXPECT_TRUE(buildObjCSourceCallHints(Image, receiverCaller(Architecture))
                      .empty());
    }
  }
}

namespace {
BinaryImage receiverFieldImage(Arch Architecture) {
  auto Image = receiverImage(Architecture);
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  Image.ObjCMethods.front().Selector = "readCode";
  Image.ObjCMethods.front().TypeHint = signature(Architecture, 0);
  Image.ObjCMethods.back().Selector = "code";
  Image.ObjCMethods.back().TypeHint->Parameters.resize(2);
  Image.ObjCSourceReferences.at(0x2100).Name = "code";
  auto &Class = Image.ObjCClasses.front();
  Class.IvarStatus = "recovered";
  Class.InstanceStart = 8;
  Class.InstanceSize = 32;
  Class.Ivars = {{"_error", "@\"NSError\"", 0x2500, 0x2300, 8, 8, 8},
                 {"_next", "@\"First\"", 0x2520, 0x2308, 16, 8, 8},
                 {"_other", "@\"NSError\"", 0x2540, 0x2310, 24, 8, 8}};
  for (const auto &Ivar : Class.Ivars)
    Image.ObjCSourceReferences[Ivar.OffsetAddress] = {
        ObjCSourceReference::Kind::IvarOffset, Ivar.OffsetAddress,
        static_cast<uint16_t>(Architecture == Arch::AArch64 ? 4 : 8), Ivar.Name,
        Class.Name};
  return Image;
}

LowFunc receiverFieldCaller(Arch Architecture, bool Dynamic,
                            bool Nested = false) {
  auto Function = receiverCaller(Architecture);
  auto &Ops = Function.Blocks.front().Ops;
  Ops.clear();
  const auto &TRI = getTargetRegInfo(Architecture);
  const auto Self = NdVar::reg(TRI.IntParamRegs[0], 8);
  const auto Address = NdVar::reg(TRI.IntParamRegs[2], 8);
  const auto Offset = NdVar::reg(TRI.IntParamRegs[3], 8);
  auto Field = [&](uint64_t ByteOffset, va_t Slot, va_t PC) {
    if (Dynamic) {
      const auto Width = Architecture == Arch::AArch64 ? 4 : 8;
      Ops.push_back(operation(NdOp::LOAD, NdVar::reg(Offset.Offset, Width),
                              {NdVar::cst(Slot, 8)}, PC));
      if (Width == 4)
        Ops.push_back(operation(NdOp::INT_SEXT, Offset,
                                {NdVar::reg(Offset.Offset, 4)}, PC));
    }
    Ops.push_back(operation(
        NdOp::INT_ADD, Address,
        {Self, Dynamic ? Offset : NdVar::cst(ByteOffset, 8)}, PC + 4));
    Ops.push_back(operation(NdOp::LOAD, Self, {Address}, PC + 4));
  };
  if (Nested)
    Field(16, 0x2308, 0x1200);
  Field(8, 0x2300, 0x1210);
  Ops.push_back(operation(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8),
                          {NdVar::cst(0x2100, 8)}, 0x122c));
  Ops.push_back(
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1230));
  return Function;
}
} // namespace

TEST(ObjCCallHints, ReceiverFieldsPreserveExactObjectTypesAcrossNestedLoads) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const bool Dynamic : {false, true}) {
      for (const bool Nested : {false, true}) {
        auto Image = receiverFieldImage(Architecture);
        EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "code"));
        const auto Hints = buildObjCSourceCallHints(
            Image, receiverFieldCaller(Architecture, Dynamic, Nested));
        ASSERT_EQ(Hints.size(), 1U)
            << static_cast<int>(Architecture) << Dynamic << Nested;
        const auto &Hint = Hints.at(0x1230);
        ASSERT_TRUE(Hint.Receiver);
        EXPECT_EQ(Hint.Receiver->ClassName, "First");
        const std::vector<va_t> Expected =
            Nested ? std::vector<va_t>{0x2308, 0x2300}
                   : std::vector<va_t>{0x2300};
        std::vector<va_t> Actual;
        for (const auto &Access : Hint.Receiver->Steps) {
          Actual.push_back(Access.OffsetSlot);
          EXPECT_EQ(Access.ByteOffset.has_value(), !Dynamic);
          EXPECT_EQ(Access.OffsetWidth,
                    Architecture == Arch::AArch64 ? 4U : 8U);
        }
        EXPECT_EQ(Actual, Expected);
        EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Int);
        EXPECT_EQ(Hint.Signature.ReturnType->Size, 8U);
        EXPECT_TRUE(Hint.Signature.ReturnType->IsSigned);
        EXPECT_TRUE(
            sdk::objcSourceCallBound(*receiverCallExpression(Hint), Image, {}));
      }
    }
  }
}

TEST(ObjCCallHints, ReceiverFieldsFollowRecordedSuperclassStorage) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverFieldImage(Architecture);
    auto Base = Image.ObjCClasses.front();
    Base.Name = "Base";
    for (auto &[Address, Ref] : Image.ObjCSourceReferences)
      if (Ref.TheKind == ObjCSourceReference::Kind::IvarOffset)
        Ref.ClassName = Base.Name;
    auto &Child = Image.ObjCClasses.front();
    Child.RootClass = false;
    Child.InheritanceStatus = "resolved";
    Child.SuperclassName = Base.Name;
    Child.InstanceStart = 32;
    Child.InstanceSize = 40;
    Child.Ivars.clear();
    Image.ObjCClasses.push_back(Base);
    const auto Root = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Root);
    const auto Field = objcReceiverFieldTypeHint(Image, *Root, 8);
    ASSERT_TRUE(Field);
    EXPECT_TRUE(objcReceiverSourceTypeHint(Image, "code", *Field).Signature);
    Image.ObjCClasses.back().RootClass = false;
    Image.ObjCClasses.back().InheritanceStatus = "resolved";
    Image.ObjCClasses.back().SuperclassName = "First";
    EXPECT_FALSE(objcReceiverFieldTypeHint(Image, *Root, 8));
  }
}

TEST(ObjCCallHints, ReceiverFieldsRejectPartialUnknownAndNonObjectAccesses) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      auto Image = receiverFieldImage(Architecture);
      auto Function = receiverFieldCaller(Architecture, false);
      const auto &TRI = getTargetRegInfo(Architecture);
      if (Mutation <= 2)
        Function.Blocks.front().Ops.front().Inputs[1] =
            NdVar::cst(Mutation == 0   ? 7
                       : Mutation == 1 ? 9
                                       : UINT64_MAX,
                       8);
      else if (Mutation == 3)
        Function.Blocks.front().Ops[1].Output.Size = 4;
      else if (Mutation == 4)
        Function.Blocks.front().Ops.front().Inputs[0] =
            NdVar::reg(TRI.IntParamRegs[4], 8);
      else if (Mutation == 5)
        Image.ObjCClasses.front().Ivars.front().TypeEncoding = "@";
      else if (Mutation == 6)
        Image.ObjCClasses.front().Ivars.front().TypeEncoding = "@?";
      else
        Image.ObjCClasses.front().Ivars.front().TypeEncoding = "^@\"NSError\"";
      EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty())
          << Mutation;
    }
  }
}

TEST(ObjCCallHints, ReceiverFieldsRequireMatchingMetadataAndReferenceSlots) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
      auto Image = receiverFieldImage(Architecture);
      auto &Class = Image.ObjCClasses.front();
      auto &Ivar = Class.Ivars.front();
      auto &Ref = Image.ObjCSourceReferences.at(0x2300);
      if (Mutation == 0)
        Ref.Address += 1;
      else if (Mutation == 1)
        Ref.ClassName = "Other";
      else if (Mutation == 2)
        Ref.Name = "_next";
      else if (Mutation == 3)
        Ref.Size = 2;
      else if (Mutation == 4)
        Ref.TheKind = ObjCSourceReference::Kind::Class;
      else if (Mutation == 5)
        Ivar.MetadataAddress = 0;
      else if (Mutation == 6)
        Ivar.Size = 4;
      else if (Mutation == 7)
        Class.InstanceSize = 12;
      else if (Mutation == 8)
        Class.IvarStatus = "unresolved";
      else if (Mutation == 9)
        Class.Ivars.push_back(Ivar);
      else
        Image.ObjCClasses.push_back(Class);
      const auto Root = objcMethodReceiverTypeHint(Image, 0x1200);
      ASSERT_TRUE(Root);
      EXPECT_FALSE(objcReceiverIvarTypeHint(Image, *Root, 0x2300)) << Mutation;
      EXPECT_FALSE(objcReceiverFieldTypeHint(Image, *Root, 8)) << Mutation;
      EXPECT_TRUE(buildObjCSourceCallHints(
                      Image, receiverFieldCaller(Architecture, true))
                      .empty())
          << Mutation;
    }
  }
}

TEST(ObjCCallHints,
     RuntimeIvarTypesRequireSlotsWithoutInventingLiteralOffsets) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverFieldImage(Architecture);
    auto &Class = Image.ObjCClasses.front();
    Class.IvarStatus = "runtime";
    Class.Ivars.front().Offset.reset();
    const auto Root = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Root);
    EXPECT_TRUE(objcReceiverIvarTypeHint(Image, *Root, 0x2300));
    EXPECT_FALSE(objcReceiverFieldTypeHint(Image, *Root, 8));
    const auto Hints = buildObjCSourceCallHints(
        Image, receiverFieldCaller(Architecture, true));
    ASSERT_EQ(Hints.size(), 1U);
    auto Expression = receiverCallExpression(Hints.begin()->second);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    auto Forged =
        std::make_shared<SourceCallTypeHint>(*Expression->SourceCallHint);
    Forged->Receiver->Steps.front().ByteOffset = 8;
    Expression->SourceCallHint = Forged;
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    Class.Ivars.front().Size = 0;
    EXPECT_FALSE(objcReceiverIvarTypeHint(Image, *Root, 0x2300));
  }
}

TEST(ObjCCallHints, ReceiverFieldProofsRevalidateEveryStepAfterReload) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Image = receiverFieldImage(Architecture);
    const auto Hints = buildObjCSourceCallHints(
        Image, receiverFieldCaller(Architecture, true, true));
    ASSERT_EQ(Hints.size(), 1U);
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      auto Changed = Image;
      auto Expression = receiverCallExpression(Hints.at(0x1230));
      auto Binding =
          std::make_shared<SourceCallTypeHint>(*Expression->SourceCallHint);
      Expression->SourceCallHint = Binding;
      ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Changed, {}));
      if (Mutation == 0)
        Binding->Receiver->Steps.front().OffsetSlot += 1;
      else if (Mutation == 1)
        Binding->Receiver->Steps.back().OffsetSlot = 0x2308;
      else if (Mutation == 2)
        Binding->Receiver->Steps.resize(9, {0x2308, std::nullopt, 4});
      else if (Mutation == 3)
        Changed.ObjCClasses.front().Ivars[1].TypeEncoding = "@\"Other\"";
      else if (Mutation == 4)
        Changed.ObjCClasses.front().Ivars.front().TypeEncoding = "q";
      else if (Mutation == 5)
        Changed.ObjCClasses.front().IvarStatus = "unresolved";
      else if (Mutation == 6)
        Changed.ObjCSourceReferences.erase(0x2300);
      else
        Changed.DynInfo.NeededLibs.clear();
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Changed, {}))
          << Mutation;
    }
  }
}

TEST(ObjCCallHints, ReceiverFieldPathsHaveABoundedDepth) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverFieldImage(Architecture);
    auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Receiver);
    for (unsigned Depth = 0; Depth != 8; ++Depth) {
      Receiver = objcReceiverIvarTypeHint(Image, *Receiver, 0x2308);
      ASSERT_TRUE(Receiver);
      EXPECT_EQ(Receiver->Steps.size(), Depth + 1U);
      EXPECT_TRUE(objcReceiverTypeHintValid(Image, *Receiver));
    }
    EXPECT_FALSE(objcReceiverIvarTypeHint(Image, *Receiver, 0x2300));
    Receiver->Steps.push_back({0x2300, std::nullopt, 4});
    EXPECT_FALSE(objcReceiverTypeHintValid(Image, *Receiver));
  }
}

TEST(ObjCCallHints, ReceiverObjectEncodingsRequireAnExplicitCompleteClass) {
  for (const auto *Encoding :
       {"@\"NSError\"", "r@\"NSError\"", "n@\"NSError\""})
    EXPECT_EQ(objcEncodedObjectClass(Encoding),
              std::optional<std::string>("NSError"));
  EXPECT_EQ(objcEncodedObjectClass("@\"_TtC4Test4Node\""),
            std::optional<std::string>("_TtC4Test4Node"));
  for (const auto *Encoding :
       {"@", "@?", "^@\"NSError\"", "@\"\"", "@\"<NSCopying>\"",
        "@\"NSError<NSCopying>\"", "@\"1Invalid\"", "@\"NSError\"0",
        "@\"NSError", "@\"NS Error\""})
    EXPECT_FALSE(objcEncodedObjectClass(Encoding)) << Encoding;
  EXPECT_FALSE(objcEncodedObjectClass("@\"" + std::string(4096, 'A') + "\""));
}

TEST(ObjCCallHints, ReceiverProtocolEncodingsRequireOneCompleteProtocol) {
  for (const auto *Encoding :
       {"@\"<SDImageLoader>\"", "r@\"<SDImageLoader>\"",
        "n@\"<SDImageLoader>\""})
    EXPECT_EQ(objcEncodedObjectProtocol(Encoding),
              std::optional<std::string>("SDImageLoader"));
  for (const auto *Encoding :
       {"@", "@?", "^@\"<SDImageLoader>\"", "@\"<>\"",
        "@\"SDImageLoader\"", "@\"NSObject<SDImageLoader>\"",
        "@\"<First><Second>\"", "@\"<1Invalid>\"",
        "@\"<SDImageLoader>\"0", "@\"<SDImageLoader\"",
        "@\"<SD ImageLoader>\""})
    EXPECT_FALSE(objcEncodedObjectProtocol(Encoding)) << Encoding;
  EXPECT_FALSE(objcEncodedObjectProtocol("@\"<" + std::string(4096, 'A') +
                                         ">\""));
}

TEST(ObjCCallHints,
     ReceiverFieldsKeepConstantOffsetsDistinctFromRuntimeReferences) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const bool Dynamic : {false, true}) {
      auto Image = receiverFieldImage(Architecture);
      const auto Hints = buildObjCSourceCallHints(
          Image, receiverFieldCaller(Architecture, Dynamic));
      ASSERT_EQ(Hints.size(), 1U);
      const auto Expression = receiverCallExpression(Hints.at(0x1230));
      ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      auto &Fields = Image.ObjCClasses.front().Ivars;
      // The offset slot still names _error, while byte offset 8 now contains
      // a different receiver type. Only the runtime slot follows this move.
      std::swap(Fields[0].Offset, Fields[2].Offset);
      Fields[2].TypeEncoding = "@\"Other\"";
      EXPECT_EQ(sdk::objcSourceCallBound(*Expression, Image, {}), Dynamic);
      Image.ObjCSourceReferences.at(0x2300).Size =
          Architecture == Arch::AArch64 ? 8 : 4;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
  }
}

namespace {
void receiverRuntimeImport(BinaryImage &Image, llvm::StringRef Name) {
  Image.ImportPtrSlots[0x2188] = "_" + Name.str();
  Image.DyldBindSlots[0x2188] = {Image.ImportPtrSlots.at(0x2188), 0,
                                 "/usr/lib/libobjc.A.dylib", false};
}

LowFunc receiverRuntimeCaller(Arch Architecture) {
  auto Function = receiverCaller(Architecture);
  const auto &TRI = getTargetRegInfo(Architecture);
  auto &Ops = Function.Blocks.front().Ops;
  for (auto &Op : Ops)
    Op.Addr += 0x40;
  Ops.insert(Ops.begin(),
             operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                       {NdVar::reg(TRI.IntReturnReg, 8)}, 0x123c));
  Ops.insert(Ops.begin(),
             operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2188, 8)}, 0x1200));
  return Function;
}
} // namespace

namespace {
BinaryImage receiverResultImage(Arch Architecture) {
  auto Image = receiverFieldImage(Architecture);
  auto &Class = Image.ObjCClasses.front();
  Class.RootClass = false;
  Class.SuperclassName = "NSObject";
  Class.InheritanceStatus = "resolved";
  ObjCProperty Property;
  Property.ClassName = "First";
  Property.Getter = "error";
  Property.TypeEncoding = "@\"NSError\"";
  Property.GetterTypeHint = signature(Architecture, 0);
  Property.GetterTypeHint->ReturnType = NdType::makePtr(NdType::makeVoid());
  Image.ObjCProperties.push_back(Property);
  auto Getter = Image.ObjCMethods.front();
  Getter.Selector = "error";
  Getter.Implementation = 0x1600;
  Getter.TypeEncoding = "@16@0:8";
  Getter.TypeHint = Property.GetterTypeHint;
  Image.ObjCMethods.push_back(Getter);
  Image.ObjCSourceReferences[0x2110] = {ObjCSourceReference::Kind::Selector,
                                        0x2110, 8, "error"};
  return Image;
}
} // namespace

TEST(ObjCCallHints, ReceiverResultsRevalidatePropertyAndSDKDeclarations) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverResultImage(Architecture);
    const auto Root = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Root);
    const auto Result = objcReceiverCallResultTypeHint(Image, *Root, "error");
    ASSERT_TRUE(Result);
    EXPECT_EQ(Result->Steps.size(), 1U);
    EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "code"));
    const auto Declaration = objcReceiverSourceTypeHint(Image, "code", *Result);
    ASSERT_TRUE(Declaration.Signature);
    EXPECT_EQ(Declaration.Signature->ReturnType->Kind, NdTypeKind::Int);
    auto Changed = Image;
    Changed.ObjCProperties.front().TypeEncoding = "@";
    EXPECT_FALSE(objcReceiverTypeHintValid(Changed, *Result));
    Changed = Image;
    Changed.DynInfo.NeededLibs = {"/tmp/Foundation.framework/Foundation"};
    EXPECT_FALSE(objcReceiverTypeHintValid(Changed, *Result));

    // The SDK names this factory's result and the related alloc/init result.
    Image.ObjCMethods.front().IsClassMethod = true;
    Image.ObjCMethods.front().ClassName = "NSError";
    const auto ErrorClass = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(ErrorClass);
    const auto Error = objcReceiverCallResultTypeHint(
        Image, *ErrorClass, "errorWithDomain:code:userInfo:");
    ASSERT_TRUE(Error);
    EXPECT_TRUE(objcReceiverSourceTypeHint(Image, "code", *Error).Signature);
    const auto Alloc =
        objcReceiverCallResultTypeHint(Image, *ErrorClass, "alloc");
    ASSERT_TRUE(Alloc);
    const auto Init = objcReceiverCallResultTypeHint(Image, *Alloc, "init");
    ASSERT_TRUE(Init);
    EXPECT_EQ(Init->Steps.size(), 2U);
    EXPECT_TRUE(objcReceiverSourceTypeHint(Image, "code", *Init).Signature);
  }
}

TEST(ObjCCallHints, ReceiverResultsPreserveLocalProtocolContracts) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverResultImage(Architecture);
    Image.ObjCProperties.front().TypeEncoding = "@\"<SDImageLoader>\"";
    ObjCProtocol Protocol;
    Protocol.Address = 0x2400;
    Protocol.Name = "SDImageLoader";
    Protocol.Status = "recovered";
    ObjCProtocolMethod Method;
    Method.MetadataAddress = 0x2410;
    Method.Selector = "shouldBlockFailedURLWithURL:error:options:context:";
    Method.TypeEncoding = "B48@0:8@16@24Q32@40";
    Method.Status = "supported";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    ASSERT_TRUE(Method.TypeHint);
    Protocol.Methods.push_back(std::move(Method));
    Image.ObjCProtocols.push_back(std::move(Protocol));

    const auto Root = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Root);
    const auto Loader = objcReceiverCallResultTypeHint(Image, *Root, "error");
    ASSERT_TRUE(Loader);
    const auto Declaration = objcReceiverSourceTypeHint(
        Image, "shouldBlockFailedURLWithURL:error:options:context:", *Loader);
    ASSERT_TRUE(Declaration.Signature);
    EXPECT_EQ(Declaration.Signature->ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Declaration.Signature->ReturnType->Size, 1U);

    auto Changed = Image;
    Changed.ObjCProperties.front().TypeEncoding =
        "@\"<SDImageLoader><Other>\"";
    EXPECT_FALSE(objcReceiverTypeHintValid(Changed, *Loader));
    Changed = Image;
    Changed.ObjCProtocols.front().Status = "unresolved";
    EXPECT_FALSE(objcReceiverSourceTypeHint(
                     Changed,
                     "shouldBlockFailedURLWithURL:error:options:context:",
                     *Loader)
                     .Signature);
  }
}

TEST(ObjCCallHints, ReceiverResultsRejectUnknownConflictingAndMalformedPaths) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverResultImage(Architecture);
    const auto Root = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Root);
    const auto Result = objcReceiverCallResultTypeHint(Image, *Root, "error");
    ASSERT_TRUE(Result);
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.ObjCProperties.front().TypeEncoding = "@";
      if (Mutation == 1)
        Changed.ObjCMethods.back().TypeEncoding = "@\"Other\"16@0:8";
      if (Mutation == 2)
        Changed.ObjCMethods.back().TypeHint.reset();
      if (Mutation == 3)
        Changed.ObjCMethods.back().TypeHint->ReturnType = NdType::makeFloat(8);
      if (Mutation == 4)
        Changed.ObjCClasses.front().SuperclassName = "Missing";
      if (Mutation == 5)
        Changed.ObjCProperties.front().TypeEncoding = "@?";
      if (Mutation == 6)
        Changed.ObjCProperties.front().TypeEncoding = "@\"<MissingProtocol>\"";
      for (const bool Reverse : {false, true}) {
        if (Reverse)
          std::reverse(Changed.ObjCMethods.begin(), Changed.ObjCMethods.end());
        EXPECT_FALSE(objcReceiverTypeHintValid(Changed, *Result)) << Mutation;
      }
    }
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto Changed = *Result;
      auto &Step = Changed.Steps.front();
      if (Mutation == 0)
        Step.OffsetSlot = 0x2300;
      if (Mutation == 1)
        Step.ByteOffset = 0;
      if (Mutation == 2)
        Step.OffsetWidth = 8;
      if (Mutation == 3)
        Step.Selector.clear();
      if (Mutation == 4)
        Step.TheKind = static_cast<ObjCReceiverTypeHint::TypeStep::Kind>(99);
      EXPECT_FALSE(objcReceiverTypeHintValid(Image, Changed)) << Mutation;
    }
  }
}

TEST(ObjCCallHints, ReceiverResultsShareOneBoundWithFieldProvenance) {
  auto Image = receiverResultImage(Arch::AArch64);
  auto Current = objcMethodReceiverTypeHint(Image, 0x1200);
  ASSERT_TRUE(Current);
  for (unsigned I = 0; I != 7; ++I) {
    Current = objcReceiverIvarTypeHint(Image, *Current, 0x2308);
    ASSERT_TRUE(Current);
  }
  const auto Result = objcReceiverCallResultTypeHint(Image, *Current, "error");
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->Steps.size(), 8U);
  EXPECT_TRUE(objcReceiverTypeHintValid(Image, *Result));
  EXPECT_FALSE(objcReceiverCallResultTypeHint(Image, *Result, "self"));
  auto TooLong = *Result;
  TooLong.Steps.push_back(Result->Steps.back());
  EXPECT_FALSE(objcReceiverTypeHintValid(Image, TooLong));
  auto Reordered = *Result;
  std::swap(Reordered.Steps.front(), Reordered.Steps.back());
  EXPECT_FALSE(objcReceiverTypeHintValid(Image, Reordered));
}

TEST(ObjCCallHints, ReceiverResultsFlowThroughMessageReturnRegisters) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverResultImage(Architecture);
    auto Function = receiverCaller(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    auto &Ops = Function.Blocks.front().Ops;
    const std::vector<LowOp> Prefix{
        operation(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8),
                  {NdVar::cst(0x2110, 8)}, 0x11e0),
        operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x11e4),
        operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                  {NdVar::reg(TRI.IntReturnReg, 8)}, 0x11e8)};
    Ops.insert(Ops.begin(), Prefix.begin(), Prefix.end());
    const auto Hints = buildObjCSourceCallHints(Image, Function);
    ASSERT_EQ(Hints.size(), 2U);
    ASSERT_TRUE(Hints.at(0x1204).Receiver);
    EXPECT_EQ(Hints.at(0x1204).Receiver->Steps.back().Selector, "error");
    const auto Call = receiverCallExpression(Hints.at(0x1204));
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    Image.ObjCProperties.front().TypeEncoding = "@";
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
    EXPECT_EQ(buildObjCSourceCallHints(Image, Function).count(0x1204), 0U);
  }
}

TEST(ObjCCallHints, RuntimeResultTypesPreserveFactoryCallsAndValidateEffects) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Name : {"objc_alloc", "objc_allocWithZone",
                             "objc_alloc_init", "objc_opt_new"}) {
      auto Image = receiverResultImage(Architecture);
      Image.ObjCMethods.front().IsClassMethod = true;
      Image.ObjCSourceReferences.at(0x2100).Name = "error";
      receiverRuntimeImport(Image, Name);
      auto Function = receiverRuntimeCaller(Architecture);
      const auto Hints = buildObjCSourceCallHints(Image, Function);
      ASSERT_EQ(Hints.size(), 2U) << Name;
      const auto &Runtime = Hints.at(0x1200);
      EXPECT_FALSE(Runtime.ReturnedArgument);
      ASSERT_TRUE(Runtime.RuntimeObjCResultType);
      EXPECT_EQ(Runtime.RuntimeObjCResultType->ReceiverArgument, 0U);
      ASSERT_TRUE(Hints.at(0x1244).Receiver);
      EXPECT_EQ(Hints.at(0x1244).Receiver->Steps.size(),
                llvm::StringRef(Name) == "objc_alloc_init" ? 2U : 1U);
      auto Call = HighExpr::makeCall(Name, 0, {HighExpr::makeConst(0, 8)});
      Call->Type = Runtime.Signature.ReturnType;
      auto Binding = std::make_shared<SourceCallTypeHint>(Runtime);
      Call->SourceCallHint = Binding;
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      Binding->RuntimeObjCResultType->Selectors = {"self"};
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      *Binding = Runtime;
      Binding->RuntimeObjCResultType->ReceiverArgument = 1;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      *Binding = Runtime;
      Image.DyldBindSlots[0x2188].Module = "/tmp/libobjc.A.dylib";
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      EXPECT_FALSE(
          objcRuntimeSourceCallHint(Image, 0x2188)->RuntimeObjCResultType);
      receiverRuntimeImport(Image, Name);
      const auto &TRI = getTargetRegInfo(Architecture);
      Function.Blocks.front().Ops.insert(
          Function.Blocks.front().Ops.begin(),
          operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 4),
                    {NdVar::cst(0, 4)}, 0x11f0));
      const auto Partial = buildObjCSourceCallHints(Image, Function);
      ASSERT_EQ(Partial.count(0x1200), 1U);
      if (Partial.count(0x1244))
        EXPECT_FALSE(Partial.at(0x1244).Receiver);
    }
  }
}

TEST(ObjCCallHints, RuntimeReturnIdentityRequiresExactCatalogAndImport) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Name :
         {"objc_retain", "objc_autorelease", "objc_autoreleaseReturnValue",
          "objc_retainAutorelease", "objc_retainAutoreleaseReturnValue",
          "objc_retainAutoreleasedReturnValue",
          "objc_unsafeClaimAutoreleasedReturnValue"}) {
      auto Image = receiverImage(Architecture);
      receiverRuntimeImport(Image, Name);
      const auto Hint = objcRuntimeSourceCallHint(Image, 0x2188);
      ASSERT_TRUE(Hint);
      EXPECT_EQ(Hint->ReturnedArgument, 0U);
      auto Call = HighExpr::makeCall(Name, 0, {HighExpr::makeConst(0, 8)});
      Call->Type = Hint->Signature.ReturnType;
      const auto Binding = std::make_shared<SourceCallTypeHint>(*Hint);
      Call->SourceCallHint = Binding;
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      Binding->ReturnedArgument = 1;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      Binding->ReturnedArgument.reset();
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      Binding->ReturnedArgument = 0;
      for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
        auto Changed = Image;
        if (Mutation == 0)
          Changed.DyldBindSlots[0x2188].Module = "/tmp/libobjc.A.dylib";
        if (Mutation == 1)
          Changed.DyldBindSlots.clear();
        if (Mutation == 2)
          Changed.DyldBindSlots[0x2188].WeakImport = true;
        if (Mutation == 3)
          Changed.DyldBindSlots[0x2188].Addend = 8;
        if (Mutation == 4)
          Changed.DyldBindSlots[0x2188].Name = "_objc_retainBlock";
        if (Mutation == 5)
          Changed.ConflictingImportStorageSlots.insert(0x2188);
        const auto Other = objcRuntimeSourceCallHint(Changed, 0x2188);
        EXPECT_TRUE(!Other || !Other->ReturnedArgument) << Mutation;
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Changed, {})) << Mutation;
        EXPECT_EQ(buildObjCSourceCallHints(Changed,
                                           receiverRuntimeCaller(Architecture))
                      .count(0x1244),
                  0U)
            << Mutation;
      }
    }
  }
}

TEST(ObjCCallHints, RuntimeReturnIdentityPreservesReceiverTypeAndCallEffects) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Name :
         {"objc_retain", "objc_autorelease", "objc_autoreleaseReturnValue",
          "objc_retainAutorelease", "objc_retainAutoreleaseReturnValue",
          "objc_retainAutoreleasedReturnValue",
          "objc_unsafeClaimAutoreleasedReturnValue"}) {
      auto Image = receiverImage(Architecture);
      receiverRuntimeImport(Image, Name);
      const auto Hints =
          buildObjCSourceCallHints(Image, receiverRuntimeCaller(Architecture));
      ASSERT_EQ(Hints.size(), 2U) << Name;
      EXPECT_EQ(Hints.at(0x1200).TargetName, Name);
      EXPECT_EQ(Hints.at(0x1200).ReturnedArgument, 0U);
      ASSERT_TRUE(Hints.at(0x1244).Receiver);
      EXPECT_EQ(*Hints.at(0x1244).Receiver,
                *objcMethodReceiverTypeHint(Image, 0x1200));
      EXPECT_TRUE(sdk::objcSourceCallBound(
          *receiverCallExpression(Hints.at(0x1244)), Image, {}));
    }
  }
}

TEST(ObjCCallHints, RuntimeReturnIdentityUsesTheDeclaredArgumentRegister) {
  for (unsigned Register : {0U, 1U, 15U, 19U, 28U}) {
    auto Image = receiverImage(Arch::AArch64);
    receiverRuntimeImport(Image, "objc_retain_x" + std::to_string(Register));
    const auto &TRI = getTargetRegInfo(Image.Arch);
    const auto Self = NdVar::reg(TRI.IntParamRegs[0], 8);
    const auto Argument = NdVar::reg(Self.Offset + Register * 8, 8);
    for (bool Partial : {false, true}) {
      auto Function = receiverRuntimeCaller(Image.Arch);
      auto &Ops = Function.Blocks.front().Ops;
      Ops.insert(Ops.begin(), operation(NdOp::COPY, Argument, {Self}, 0x1200));
      if (Partial)
        Ops.insert(Ops.begin() + 1,
                   operation(NdOp::COPY, NdVar::reg(Argument.Offset, 4),
                             {NdVar::cst(0, 4)}, 0x1200));
      const auto Hints = buildObjCSourceCallHints(Image, Function);
      ASSERT_EQ(Hints.count(0x1200), 1U);
      EXPECT_EQ(
          Hints.at(0x1200).Signature.Parameters[0].Location.RegisterOffset,
          Argument.Offset);
      EXPECT_EQ(Hints.count(0x1244), !Partial) << Register;
    }
  }
}

TEST(ObjCCallHints, RuntimeReturnIdentityRejectsUnknownAndNonIdentityValues) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Name :
         {"objc_retainBlock", "objc_alloc", "objc_allocWithZone",
          "objc_alloc_init", "objc_opt_new", "objc_opt_self", "objc_opt_class",
          "objc_loadWeak", "objc_loadWeakRetained", "objc_storeWeak",
          "objc_initWeak", "objc_release"}) {
      auto Image = receiverImage(Architecture);
      receiverRuntimeImport(Image, Name);
      const auto Hint = objcRuntimeSourceCallHint(Image, 0x2188);
      ASSERT_TRUE(Hint) << Name;
      EXPECT_FALSE(Hint->ReturnedArgument);
      EXPECT_EQ(
          buildObjCSourceCallHints(Image, receiverRuntimeCaller(Architecture))
              .count(0x1244),
          0U)
          << Name;
    }
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      auto Image = receiverImage(Architecture);
      receiverRuntimeImport(Image, "objc_retain");
      auto Function = receiverRuntimeCaller(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      auto &Ops = Function.Blocks.front().Ops;
      if (Mutation == 0)
        Image.ObjCMethods.front().Implementation = 0x1600;
      if (Mutation == 1)
        Ops.insert(Ops.begin(),
                   operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 4),
                             {NdVar::cst(0, 4)}));
      if (Mutation == 2)
        Ops.insert(Ops.begin(),
                   operation(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[0], 8),
                             {NdVar::cst(0x2100, 8)}));
      if (Mutation == 3)
        Ops.insert(Ops.begin(),
                   operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                             {NdVar::cst(0x2100, 8)}));
      if (Mutation == 4)
        Ops.insert(Ops.begin() + 1,
                   operation(NdOp::COPY, NdVar::reg(TRI.IntReturnReg, 4),
                             {NdVar::cst(0, 4)}, 0x1230));
      EXPECT_EQ(buildObjCSourceCallHints(Image, Function).count(0x1244), 0U)
          << Mutation;
    }
  }
}

TEST(ObjCCallHints, RuntimeReturnIdentityRetainsCompleteFieldProvenance) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Dynamic : {false, true}) {
      auto Image = receiverFieldImage(Architecture);
      receiverRuntimeImport(Image, "objc_retain");
      auto Function = receiverFieldCaller(Architecture, Dynamic, true);
      auto &Ops = Function.Blocks.front().Ops;
      const auto &TRI = getTargetRegInfo(Architecture);
      Ops.insert(Ops.end() - 2, operation(NdOp::INDIR_CALL, {},
                                          {NdVar::cst(0x2188, 8)}, 0x1220));
      Ops.insert(Ops.end() - 2,
                 operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                           {NdVar::reg(TRI.IntReturnReg, 8)}, 0x1224));
      const auto Hints = buildObjCSourceCallHints(Image, Function);
      ASSERT_EQ(Hints.size(), 2U);
      ASSERT_TRUE(Hints.at(0x1230).Receiver);
      EXPECT_EQ(Hints.at(0x1230).Receiver->Steps.size(), 2U);
      EXPECT_TRUE(sdk::objcSourceCallBound(
          *receiverCallExpression(Hints.at(0x1230)), Image, {}));
    }
  }
}

TEST(ObjCCallHints, RuntimeReturnIdentityConvergesAcrossJoinsAndBackedges) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto Self = NdVar::reg(TRI.IntParamRegs[0], 8);
    const auto Saved = NdVar::reg(TRI.CalleeSaveRegs.front(), 8);
    for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
      auto Image = receiverImage(Architecture);
      receiverRuntimeImport(Image, "objc_retain");
      LowFunc Function;
      Function.Entry = 0x1200;
      LowBlock Entry;
      Entry.Id = 0;
      Entry.StartAddr = 0x1200;
      Entry.Succs = {1, 2};
      Entry.Ops = {operation(NdOp::COPY, Saved, {Self})};
      LowBlock Left;
      Left.Id = 1;
      Left.StartAddr = 0x1300;
      Left.Preds = {0};
      Left.Succs = {3};
      Left.Ops = {
          operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2188, 8)}, 0x1300),
          operation(NdOp::COPY, Self, {NdVar::reg(TRI.IntReturnReg, 8)},
                    0x1304)};
      LowBlock Right = Left;
      Right.Id = 2;
      Right.StartAddr = 0x1400;
      for (auto &Op : Right.Ops)
        Op.Addr += 0x100;
      // A full copy and a runtime identity result must meet as the same fact.
      Right.Ops = {operation(NdOp::COPY, Self, {Saved}, 0x1400)};
      if (Mutation == 1)
        Right.Ops.front() =
            operation(NdOp::COPY, Self, {NdVar::cst(0, 8)}, 0x1400);
      if (Mutation == 2)
        Function.ModuleAnalysisRoots.insert(Right.StartAddr);
      auto Join = receiverCaller(Architecture).Blocks.front();
      Join.Id = 3;
      Join.StartAddr = 0x1500;
      Join.Preds = {1, 2};
      for (auto &Op : Join.Ops)
        Op.Addr += 0x300;
      if (Mutation == 3) {
        Join.Succs = {1};
        Left.Preds.push_back(3);
        // The message result on the backedge has no receiver provenance.
        // It must revoke the provisional first-iteration runtime identity.
        Right.Succs = {1};
        Left.Preds.push_back(2);
        Join.Preds = {1};
      }
      const std::vector<LowBlock> Blocks{Entry, Left, Right, Join};
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        Function.Blocks.clear();
        for (auto Index : Order)
          Function.Blocks.push_back(Blocks[Index]);
        const auto Hints = buildObjCSourceCallHints(Image, Function);
        EXPECT_EQ(Hints.count(0x1504), Mutation == 0) << Mutation;
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
  }
}

TEST(ObjCCallHints, ReceiverResultsKeepMessagePathsDistinctAtJoins) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      auto Image = receiverResultImage(Architecture);
      auto Property = Image.ObjCProperties.front();
      Property.Getter = "otherError";
      Image.ObjCProperties.push_back(Property);
      Image.ObjCSourceReferences[0x2118] = {
          ObjCSourceReference::Kind::Selector, 0x2118, 8,
          Mutation == 1 ? "otherError" : "error"};
      const auto &TRI = getTargetRegInfo(Architecture);
      LowFunc Function;
      Function.Entry = 0x1200;
      LowBlock Entry;
      Entry.Id = 0;
      Entry.StartAddr = 0x1200;
      Entry.Succs = {1, 2};
      LowBlock Left;
      Left.Id = 1;
      Left.StartAddr = 0x1300;
      Left.Preds = {0};
      Left.Succs = {3};
      Left.Ops = {
          operation(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8),
                    {NdVar::cst(0x2110, 8)}, 0x1300),
          operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1304),
          operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                    {NdVar::reg(TRI.IntReturnReg, 8)}, 0x1308)};
      auto Right = Left;
      Right.Id = 2;
      Right.StartAddr = 0x1400;
      Right.Ops.front().Inputs[0] = NdVar::cst(0x2118, 8);
      for (auto &Op : Right.Ops)
        Op.Addr += 0x100;
      if (Mutation == 2)
        Function.ModuleAnalysisRoots.insert(0x1400);
      auto Join = receiverCaller(Architecture).Blocks.front();
      Join.Id = 3;
      Join.StartAddr = 0x1500;
      Join.Preds = {1, 2};
      for (auto &Op : Join.Ops)
        Op.Addr += 0x300;
      const std::vector<LowBlock> Blocks{Entry, Left, Right, Join};
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        Function.Blocks.clear();
        for (auto Index : Order)
          Function.Blocks.push_back(Blocks[Index]);
        const auto Hints = buildObjCSourceCallHints(Image, Function);
        EXPECT_EQ(Hints.count(0x1504), Mutation == 0) << Mutation;
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
  }
}

TEST(ObjCCallHints, SharedFrameworkDeclarationsKeepExactProviderAndScalarABI) {
  const struct {
    const char *Framework;
    const char *Selector;
    NdTypeKind Kind;
    unsigned Bytes;
    unsigned Parameters;
  } Cases[] = {
      {"QuartzCore", "opacity", NdTypeKind::Float, 4, 2},
      {"CoreLocation", "distanceFromLocation:", NdTypeKind::Float, 8, 3},
      {"CoreSpotlight", "contentDescription", NdTypeKind::Ptr, 8, 2},
      {"UserNotifications",
       "requestWithIdentifier:content:trigger:", NdTypeKind::Ptr, 8, 5},
      {"UniformTypeIdentifiers", "typeWithIdentifier:", NdTypeKind::Ptr, 8, 3}};
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &Case : Cases) {
      SCOPED_TRACE(Case.Framework);
      auto Image = image(Architecture);
      Image.ObjCMethods.clear();
      Image.ObjCSourceReferences.at(0x2100).Name = Case.Selector;
      const auto Root = std::string("/System/Library/Frameworks/") +
                        Case.Framework + ".framework/";
      for (const auto &Module :
           {Root + Case.Framework, Root + "Versions/A/" + Case.Framework}) {
        Image.DynInfo.NeededLibs = {Module};
        const auto Hints =
            buildObjCSourceCallHints(Image, receiverCaller(Architecture));
        ASSERT_EQ(Hints.size(), 1U);
        const auto &Hint = Hints.at(0x1204);
        EXPECT_EQ(Hint.Signature.ReturnType->Kind, Case.Kind);
        EXPECT_EQ(Hint.Signature.ReturnType->Size, Case.Bytes);
        EXPECT_EQ(Hint.Signature.Parameters.size(), Case.Parameters);
        EXPECT_TRUE(
            sdk::objcSourceCallBound(*receiverCallExpression(Hint), Image, {}));
        auto Changed = Image;
        Changed.DynInfo.NeededLibs = {std::string("/tmp/") + Case.Framework +
                                      ".framework/" + Case.Framework};
        EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, Case.Selector));
        EXPECT_FALSE(sdk::objcSourceCallBound(*receiverCallExpression(Hint),
                                              Changed, {}));
      }
    }
  }
}

TEST(ObjCCallHints, IOSFrameworkDeclarationsRequireExactDeviceEvidence) {
  constexpr auto Module = "/System/Library/Frameworks/UIKit.framework/UIKit";
  const struct {
    const char *Selector;
    NdTypeKind ReturnKind;
    unsigned Parameters;
  } Cases[] = {
      {"CGImage", NdTypeKind::Ptr, 2},
      {"addSubview:", NdTypeKind::Void, 3},
      {"CGColor", NdTypeKind::Ptr, 2},
      {"CIImage", NdTypeKind::Ptr, 2},
      {"CGSizeValue", NdTypeKind::Struct, 2},
      {"alpha", NdTypeKind::Float, 2},
      {"blackColor", NdTypeKind::Ptr, 2},
      {"colorWithAlphaComponent:", NdTypeKind::Ptr, 3},
      {"colorWithRed:green:blue:alpha:", NdTypeKind::Ptr, 6},
      {"getRed:green:blue:alpha:", NdTypeKind::Int, 6},
      {"imageByPreparingForDisplay", NdTypeKind::Ptr, 2},
      {"imageFlippedForRightToLeftLayoutDirection", NdTypeKind::Ptr, 2},
      {"imageForState:", NdTypeKind::Ptr, 3},
      {"imageOrientation", NdTypeKind::Int, 2},
      {"imageWithCIImage:scale:orientation:", NdTypeKind::Ptr, 5},
      {"initWithCGImage:", NdTypeKind::Ptr, 3},
      {"initWithProgressViewStyle:", NdTypeKind::Ptr, 3},
      {"initWithActivityIndicatorStyle:", NdTypeKind::Ptr, 3},
      {"isHighDynamicRange", NdTypeKind::Int, 2},
      {"interactivePopGestureRecognizer", NdTypeKind::Ptr, 2},
      {"parentViewController", NdTypeKind::Ptr, 2},
      {"transitionCoordinator", NdTypeKind::Ptr, 2},
      {"flashScrollIndicators", NdTypeKind::Void, 2},
      {"endBackgroundTask:", NdTypeKind::Void, 3},
      {"initWithRed:green:blue:alpha:", NdTypeKind::Ptr, 6},
      {"insertSubview:belowSubview:", NdTypeKind::Void, 4},
      {"instantiateWithOwner:options:", NdTypeKind::Ptr, 4},
      {"nibWithNibName:bundle:", NdTypeKind::Ptr, 4},
      {"resignFirstResponder", NdTypeKind::Int, 2},
      {"sendActionsForControlEvents:", NdTypeKind::Void, 3},
      {"dismissViewControllerAnimated:completion:", NdTypeKind::Void, 4},
      {"setAccessibilityIgnoresInvertColors:", NdTypeKind::Void, 3},
      {"setActivityIndicatorViewStyle:", NdTypeKind::Void, 3},
      {"setAdjustsFontForContentSizeCategory:", NdTypeKind::Void, 3},
      {"setCenter:", NdTypeKind::Void, 3},
      {"setLayoutMargins:", NdTypeKind::Void, 3},
      {"setProgressViewStyle:", NdTypeKind::Void, 3},
      {"setShowsCancelButton:animated:", NdTypeKind::Void, 4},
      {"setText:", NdTypeKind::Void, 3},
      {"setTranslatesAutoresizingMaskIntoConstraints:", NdTypeKind::Void, 3},
      {"setView:", NdTypeKind::Void, 3},
      {"sizeToFit", NdTypeKind::Void, 2},
      {"superview", NdTypeKind::Ptr, 2},
      {"topViewController", NdTypeKind::Ptr, 2},
      {"window", NdTypeKind::Ptr, 2},
      {"whiteColor", NdTypeKind::Ptr, 2},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Selector);
    auto Image = image(Arch::AArch64);
    Image.ObjCMethods.clear();
    Image.DynInfo.NeededLibs = {Module};
    const auto Hint = objcSelectorSourceTypeHint(Image, Case.Selector);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::ObjCSDK);
    EXPECT_EQ(Hint->ReturnType->Kind, Case.ReturnKind);
    ASSERT_EQ(Hint->Parameters.size(), Case.Parameters);
    if (llvm::StringRef(Case.Selector) == "endBackgroundTask:") {
      EXPECT_EQ(Hint->Parameters[2].Type->Size, 8U);
      EXPECT_FALSE(Hint->Parameters[2].Type->IsSigned);
      EXPECT_EQ(Hint->Parameters[2].Location.RegisterOffset, 2U * 8);
    }
    if (llvm::StringRef(Case.Selector) == "isHighDynamicRange") {
      EXPECT_EQ(Hint->ReturnType->Size, 1U);
      EXPECT_FALSE(Hint->ReturnType->IsSigned);
    }
    if (llvm::StringRef(Case.Selector) == "initWithActivityIndicatorStyle:") {
      EXPECT_EQ(Hint->Parameters[2].Type->Size, 8U);
      EXPECT_TRUE(Hint->Parameters[2].Type->IsSigned);
      EXPECT_EQ(Hint->Parameters[2].Location.RegisterOffset, 2U * 8);
    }
    if (llvm::StringRef(Case.Selector) == "setShowsCancelButton:animated:") {
      for (size_t I : {2U, 3U}) {
        EXPECT_EQ(Hint->Parameters[I].Type->Kind, NdTypeKind::Int);
        EXPECT_EQ(Hint->Parameters[I].Type->Size, 1U);
        EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset, I * 8);
      }
    }
    if (llvm::StringRef(Case.Selector) == "setView:") {
      EXPECT_EQ(Hint->Parameters[2].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Hint->Parameters[2].Location.RegisterOffset, 16U);
    }

    auto Changed = Image;
    Changed.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/UIKit.framework/Versions/A/UIKit"};
    EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, Case.Selector));
    Changed.DynInfo.NeededLibs = {"/tmp/UIKit.framework/UIKit"};
    EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, Case.Selector));

    auto Unsupported = image(Arch::X64);
    Unsupported.ObjCMethods.clear();
    Unsupported.DynInfo.NeededLibs = {Module};
    EXPECT_FALSE(objcSelectorSourceTypeHint(Unsupported, Case.Selector));
  }
}

TEST(ObjCCallHints, IOSProgressKeepsFloatAndBooleanInSeparateABIRegisters) {
  constexpr auto Module = "/System/Library/Frameworks/UIKit.framework/UIKit";
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (const char *Selector : {"observedProgress", "setProgress:animated:"}) {
    SCOPED_TRACE(Selector);
    const bool Setter = llvm::StringRef(Selector) == "setProgress:animated:";
    auto Image = image(Arch::AArch64);
    Image.ObjCMethods.clear();
    Image.DynInfo.NeededLibs = {Module};
    Image.ObjCSourceReferences.at(0x2100).Name = Selector;
    auto Low = caller();
    if (Setter)
      Low.Blocks.front().Ops.back().NumInputs = 0;
    const auto Med = convert(Image, Low);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    ASSERT_TRUE(Call.SourceCallHint);
    const auto &Hint = *Call.SourceCallHint;
    EXPECT_EQ(Hint.Signature.Origin,
              SourceFunctionTypeHint::OriginKind::ObjCSDK);
    EXPECT_EQ(Hint.Signature.ReturnType->Kind,
              Setter ? NdTypeKind::Void : NdTypeKind::Ptr);
    ASSERT_EQ(Call.Args.size(), Setter ? 4U : 2U);
    EXPECT_EQ(Call.Args[0].RegOff, TRI.IntParamRegs[0]);
    EXPECT_EQ(Hint.Signature.Parameters[1].Location.RegisterOffset,
              TRI.IntParamRegs[1]);
    if (Setter) {
      // A float in s0 must not consume x2: the following BOOL still uses w2.
      EXPECT_EQ(Call.Args[2].RegOff, TRI.FPParamRegs[0]);
      EXPECT_EQ(Call.Args[2].Size, 4U);
      EXPECT_EQ(Hint.Signature.Parameters[2].Type->Kind, NdTypeKind::Float);
      EXPECT_EQ(Hint.Signature.Parameters[3].Location.RegisterOffset,
                TRI.IntParamRegs[2]);
      EXPECT_EQ(Call.Args[3].Size, 1U);
      EXPECT_EQ(Hint.Signature.Parameters[3].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Med.Blocks[Call.BlockId].Ops[Call.OpIdx].Output.Size, 0U);
    } else {
      EXPECT_EQ(Hint.Signature.ReturnLocation.RegisterOffset, TRI.IntReturnReg);
      EXPECT_EQ(Hint.Signature.ReturnLocation.ValueBytes, 8U);
    }
    auto Expression = receiverCallExpression(Hint);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    if (Setter) {
      auto ChangedHint = std::make_shared<SourceCallTypeHint>(Hint);
      ChangedHint->Signature.Parameters[2].Location.ValueBytes = 8;
      Expression->SourceCallHint = ChangedHint;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
      Expression->SourceCallHint = std::make_shared<SourceCallTypeHint>(Hint);
    }
    for (const char *OtherModule :
         {"/tmp/UIKit.framework/UIKit",
          "/System/Library/Frameworks/UIKit.framework/Versions/A/UIKit",
          "/System/Library/Frameworks/Foundation.framework/Foundation"}) {
      auto Changed = Image;
      Changed.DynInfo.NeededLibs = {OtherModule};
      EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, Selector));
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Changed, {}));
    }
    auto Unsupported = image(Arch::X64);
    Unsupported.ObjCMethods.clear();
    Unsupported.DynInfo.NeededLibs = {Module};
    EXPECT_FALSE(objcSelectorSourceTypeHint(Unsupported, Selector));
  }
}

TEST(ObjCCallHints, IOSProgressSetterRequiresPropertyReceiverAcrossARC) {
  const auto Architecture = Arch::AArch64;
  const auto &TRI = getTargetRegInfo(Architecture);
  auto Image = receiverResultImage(Architecture);
  Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/UIKit.framework/UIKit");
  Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/QuartzCore.framework/QuartzCore");
  Image.ObjCProperties.front().TypeEncoding = "@\"UIProgressView\"";
  Image.ObjCSourceReferences.at(0x2100).Name = "setProgress:";
  auto ObjectSetter = Image.ObjCMethods.back();
  ObjectSetter.ClassName = "Other";
  ObjectSetter.Selector = "setProgress:";
  ObjectSetter.TypeEncoding = "v24@0:8@16";
  ObjectSetter.TypeHint =
      parseObjCMethodEncoding(ObjectSetter.Selector, ObjectSetter.TypeEncoding);
  Image.ObjCMethods.push_back(ObjectSetter);
  receiverRuntimeImport(Image, "objc_retainAutoreleasedReturnValue");
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "setProgress:"));

  auto Function = receiverCaller(Architecture);
  auto &Ops = Function.Blocks.front().Ops;
  Ops.back().NumInputs = 0;
  const std::vector<LowOp> Prefix{
      operation(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8),
                {NdVar::cst(0x2110, 8)}, 0x11e0),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x11e4),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2188, 8)}, 0x11e8),
      operation(NdOp::COPY, NdVar::reg(TRI.FPParamRegs[0], 4),
                {NdVar::cst(0x3f800000, 4)}, 0x11ec)};
  Ops.insert(Ops.begin(), Prefix.begin(), Prefix.end());
  const auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 3U);
  const auto &Hint = Hints.at(0x1204);
  ASSERT_TRUE(Hint.Receiver);
  ASSERT_EQ(Hint.Receiver->Steps.size(), 1U);
  EXPECT_EQ(Hint.Receiver->Steps.front().Selector, "error");
  ASSERT_EQ(Hint.Signature.Parameters.size(), 3U);
  EXPECT_EQ(Hint.Signature.Parameters[2].Type->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint.Signature.Parameters[2].Type->Size, 4U);
  EXPECT_EQ(Hint.Signature.Parameters[2].Location.RegisterOffset,
            TRI.FPParamRegs[0]);
  auto Expression = receiverCallExpression(Hint);
  EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));

  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Image;
    if (Mutation == 0)
      Changed.ObjCProperties.front().TypeEncoding = "@";
    else if (Mutation == 1)
      Changed.ObjCProperties.front().TypeEncoding = "@\"UIPageControl\"";
    else if (Mutation == 2)
      Changed.DyldBindSlots.at(0x2188).Module = "/tmp/libobjc.A.dylib";
    else if (Mutation == 3) {
      ObjCClass Child;
      Child.Name = "Other";
      Child.SuperclassName = "UIProgressView";
      Child.InheritanceStatus = "resolved";
      Changed.ObjCClasses.push_back(Child);
    } else
      Changed.DynInfo.NeededLibs.pop_back(); // Missing QuartzCore parent proof.
    // The float constant is not receiver evidence. Neither the SDK object
    // alternative nor the unrelated local object setter can win by order.
    EXPECT_EQ(buildObjCSourceCallHints(Changed, Function).count(0x1204), 0U);
    if (Mutation != 2)
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Changed, {}));
  }
}

TEST(ObjCCallHints, UnknownDictionaryResultCannotInheritAnotherPathsReceiver) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation",
      "/System/Library/Frameworks/UIKit.framework/UIKit"};
  ObjCMethod EntryMethod;
  EntryMethod.ClassName = "NSDictionary";
  EntryMethod.Selector = "consumeProgress:";
  EntryMethod.Implementation = 0x1200;
  EntryMethod.TypeEncoding = "v24@0:8@16";
  EntryMethod.TypeHint = parseObjCMethodEncoding(EntryMethod.Selector,
                                                EntryMethod.TypeEncoding);
  Image.ObjCMethods.push_back(EntryMethod);
  auto Setter = EntryMethod;
  Setter.ClassName = "Result";
  Setter.Selector = "setProgress:";
  Setter.Implementation = 0x1600;
  Image.ObjCMethods.push_back(Setter);
  ObjCClass ResultClass;
  ResultClass.Name = "Result";
  ResultClass.SuperclassName = "NSObject";
  ResultClass.InheritanceStatus = "resolved";
  Image.ObjCClasses.push_back(ResultClass);
  Image.ObjCSourceReferences[0x2100].Name = "setProgress:";
  Image.ObjCSourceReferences[0x2110] = {
      ObjCSourceReference::Kind::Selector, 0x2110, 8, "objectForKey:"};
  Image.ObjCSourceReferences[0x2118] = {
      ObjCSourceReference::Kind::Selector, 0x2118, 8, "new"};
  Image.ObjCSourceReferences[0x2200] = {
      ObjCSourceReference::Kind::Class, 0x2200, 8, "Result"};
  receiverRuntimeImport(Image, "objc_retainAutoreleasedReturnValue");
  ASSERT_FALSE(objcSelectorSourceTypeHint(Image, "setProgress:"));

  const auto Self = NdVar::reg(TRI.IntParamRegs[0], 8);
  const auto Command = NdVar::reg(TRI.IntParamRegs[1], 8);
  const auto Argument = NdVar::reg(TRI.IntParamRegs[2], 8);
  const auto SavedArgument = NdVar::reg(a64reg::X20, 8);
  LowFunc Function;
  Function.Entry = 0x1200;
  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = 0x1200;
  Entry.Succs = {1, 2};
  Entry.Ops = {operation(NdOp::COPY, SavedArgument, {Argument}, 0x1200)};
  LowBlock Dictionary;
  Dictionary.Id = 1;
  Dictionary.StartAddr = 0x1300;
  Dictionary.Preds = {0};
  Dictionary.Succs = {3};
  Dictionary.Ops = {
      operation(NdOp::LOAD, Command, {NdVar::cst(0x2110, 8)}, 0x1300),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1304),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2188, 8)}, 0x1308)};
  LowBlock Created;
  Created.Id = 2;
  Created.StartAddr = 0x1400;
  Created.Preds = {0};
  Created.Succs = {3};
  Created.Ops = {
      operation(NdOp::LOAD, Self, {NdVar::cst(0x2200, 8)}, 0x1400),
      operation(NdOp::LOAD, Command, {NdVar::cst(0x2118, 8)}, 0x1404),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1408)};
  LowBlock Join;
  Join.Id = 3;
  Join.StartAddr = 0x1500;
  Join.Preds = {1, 2};
  Join.Ops = {
      operation(NdOp::COPY, Argument, {SavedArgument}, 0x1500),
      operation(NdOp::LOAD, Command, {NdVar::cst(0x2100, 8)}, 0x1504),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1508),
      operation(NdOp::RETURN, {}, {}, 0x150c)};
  const std::vector<LowBlock> Blocks{Entry, Dictionary, Created, Join};
  std::array<unsigned, 4> Order{0, 1, 2, 3};
  do {
    Function.Blocks.clear();
    for (auto Index : Order)
      Function.Blocks.push_back(Blocks[Index]);
    const auto Hints = buildObjCSourceCallHints(Image, Function);
    ASSERT_TRUE(Hints.count(0x1304));
    ASSERT_TRUE(Hints.count(0x1408));
    // A typed pointer in x2 does not prove which ABI the unknown receiver
    // consumes. The new result on the other edge cannot supply that proof.
    EXPECT_FALSE(Hints.count(0x1508));
  } while (std::next_permutation(Order.begin(), Order.end()));

  Entry.Succs = {2};
  Join.Preds = {2};
  Function.Blocks = {Entry, Created, Join};
  const auto Known = buildObjCSourceCallHints(Image, Function);
  ASSERT_TRUE(Known.count(0x1508));
  ASSERT_TRUE(Known.at(0x1508).Receiver);
  EXPECT_EQ(Known.at(0x1508).Signature.Parameters[2].Type->Kind, NdTypeKind::Ptr);
}

TEST(ObjCCallHints, IOSViewDeclarationsKeepVoidAndUnsignedControlState) {
  constexpr auto Module = "/System/Library/Frameworks/UIKit.framework/UIKit";
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (const char *Selector :
       {"invalidateIntrinsicContentSize", "setTitleColor:forState:"}) {
    SCOPED_TRACE(Selector);
    const bool Setter = llvm::StringRef(Selector) == "setTitleColor:forState:";
    auto Image = image(Arch::AArch64);
    Image.ObjCMethods.clear();
    Image.DynInfo.NeededLibs = {Module};
    Image.ObjCSourceReferences.at(0x2100).Name = Selector;
    auto Low = caller();
    Low.Blocks.front().Ops.back().NumInputs = 0;
    const auto Med = convert(Image, Low);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    ASSERT_TRUE(Call.SourceCallHint);
    const auto &Hint = *Call.SourceCallHint;
    EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
    EXPECT_EQ(Med.Blocks[Call.BlockId].Ops[Call.OpIdx].Output.Size, 0U);
    ASSERT_EQ(Call.Args.size(), Setter ? 4U : 2U);
    if (Setter) {
      EXPECT_EQ(Hint.Signature.Parameters[2].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Call.Args[2].RegOff, TRI.IntParamRegs[2]);
      const auto &State = Hint.Signature.Parameters[3];
      EXPECT_EQ(State.Type->Kind, NdTypeKind::Int);
      EXPECT_FALSE(State.Type->IsSigned);
      EXPECT_EQ(State.Type->Size, 8U);
      EXPECT_EQ(Call.Args[3].RegOff, TRI.IntParamRegs[3]);
      EXPECT_EQ(Call.Args[3].Size, 8U);
    }
    auto Expression = receiverCallExpression(Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    if (Setter) {
      auto Forged = std::make_shared<SourceCallTypeHint>(Hint);
      Forged->Signature.Parameters[3].Type = NdType::makeInt(8, true);
      Expression->SourceCallHint = Forged;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
      Expression->SourceCallHint = std::make_shared<SourceCallTypeHint>(Hint);
    }
    for (const char *Other :
         {"/tmp/UIKit.framework/UIKit",
          "/System/Library/Frameworks/UIKit.framework/Versions/A/UIKit"}) {
      auto Changed = Image;
      Changed.DynInfo.NeededLibs = {Other};
      EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, Selector));
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Changed, {}));
    }
    auto Unsupported = image(Arch::X64);
    Unsupported.ObjCMethods.clear();
    Unsupported.DynInfo.NeededLibs = {Module};
    EXPECT_FALSE(objcSelectorSourceTypeHint(Unsupported, Selector));
    auto Conflicting = Image;
    ObjCMethod OtherMethod;
    OtherMethod.ClassName = "Unrelated";
    OtherMethod.Selector = Selector;
    OtherMethod.TypeEncoding = Setter ? "v32@0:8@16q24" : "@16@0:8";
    OtherMethod.TypeHint =
        parseObjCMethodEncoding(Selector, OtherMethod.TypeEncoding);
    Conflicting.ObjCMethods.push_back(OtherMethod);
    EXPECT_FALSE(objcSelectorSourceTypeHint(Conflicting, Selector));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Conflicting, {}));
  }
}

TEST(ObjCCallHints, IOSImageViewSuperclassDisambiguatesLocalObjectSetter) {
  auto Image = receiverImage(Arch::AArch64);
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation",
      "/System/Library/Frameworks/UIKit.framework/UIKit",
      "/System/Library/Frameworks/QuartzCore.framework/QuartzCore"};
  auto &Class = Image.ObjCClasses.front();
  Class.RootClass = false;
  Class.SuperclassName = "UIImageView";
  Class.InheritanceStatus = "resolved";
  Image.ObjCSourceReferences.at(0x2100).Name = "setCurrentFrame:";
  for (auto &Method : Image.ObjCMethods) {
    Method.Selector = "setCurrentFrame:";
    Method.TypeEncoding =
        Method.ClassName == "First" ? "v24@0:8@16" : "v24@0:8d16";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  }
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "setCurrentFrame:"));
  auto Low = receiverCaller(Arch::AArch64);
  Low.Blocks.front().Ops.back().NumInputs = 0;
  const auto Hints = buildObjCSourceCallHints(Image, Low);
  ASSERT_EQ(Hints.count(0x1204), 1U);
  const auto &Hint = Hints.at(0x1204);
  ASSERT_TRUE(Hint.Receiver);
  EXPECT_EQ(Hint.Receiver->ClassName, "First");
  ASSERT_EQ(Hint.Signature.Parameters.size(), 3U);
  EXPECT_EQ(Hint.Signature.Parameters[2].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint.Signature.Parameters[2].Location.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).IntParamRegs[2]);
  auto Expression = receiverCallExpression(Hint);
  EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Image;
    if (Mutation == 0)
      Changed.ObjCClasses.front().SuperclassName = "UnknownImageView";
    else if (Mutation == 1)
      Changed.DynInfo.NeededLibs.pop_back(); // Unproved CALayerDelegate parent.
    else if (Mutation == 2)
      Changed.DynInfo.NeededLibs[1] = "/tmp/UIKit.framework/UIKit";
    else if (Mutation == 3) {
      ObjCClass Child;
      Child.Name = "Other";
      Child.SuperclassName = "First";
      Child.InheritanceStatus = "resolved";
      Changed.ObjCClasses.push_back(Child);
    } else
      Changed.ObjCMethods.front().Implementation = 0x1300;
    EXPECT_EQ(buildObjCSourceCallHints(Changed, Low).count(0x1204), 0U);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Changed, {}));
  }
  auto Unsupported = Image;
  Unsupported.Arch = Arch::X64;
  auto X64Low = receiverCaller(Arch::X64);
  X64Low.Blocks.front().Ops.back().NumInputs = 0;
  EXPECT_EQ(buildObjCSourceCallHints(Unsupported, X64Low).count(0x1204), 0U);
  EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Unsupported, {}));
}

TEST(ObjCCallHints, IOSCoreImageCropKeepsExactProviderAndRecordABI) {
  constexpr auto Module =
      "/System/Library/Frameworks/CoreImage.framework/CoreImage";
  auto Image = image(Arch::AArch64);
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs = {Module};
  for (const auto &[Selector, ReturnKind, Parameters] :
       {std::tuple{"contextWithOptions:", NdTypeKind::Ptr, 3U},
        std::tuple{"createCGImage:fromRect:", NdTypeKind::Ptr, 4U},
        std::tuple{"detectorOfType:context:options:", NdTypeKind::Ptr, 5U},
        std::tuple{"extent", NdTypeKind::Struct, 2U},
        std::tuple{"filterWithName:", NdTypeKind::Ptr, 3U},
        std::tuple{"outputImage", NdTypeKind::Ptr, 2U}}) {
    SCOPED_TRACE(Selector);
    const auto Declaration = objcSelectorSourceTypeHint(Image, Selector);
    ASSERT_TRUE(Declaration);
    EXPECT_EQ(Declaration->Origin,
              SourceFunctionTypeHint::OriginKind::ObjCSDK);
    ASSERT_TRUE(Declaration->ReturnType);
    EXPECT_EQ(Declaration->ReturnType->Kind, ReturnKind);
    EXPECT_EQ(Declaration->Parameters.size(), Parameters);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(*Declaration, Diagnostic)) << Diagnostic;

    auto Changed = Image;
    Changed.DynInfo.NeededLibs = {"/tmp/CoreImage.framework/CoreImage"};
    EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, Selector));
    Changed = image(Arch::X64);
    Changed.ObjCMethods.clear();
    Changed.DynInfo.NeededLibs = {Module};
    EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, Selector));
  }
  const auto Hint =
      objcSelectorSourceTypeHint(Image, "imageByCroppingToRect:");
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::ObjCSDK);
  ASSERT_TRUE(Hint->ReturnType);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  ASSERT_EQ(Hint->Parameters.size(), 3U);
  const auto &Rect = Hint->Parameters.back();
  ASSERT_TRUE(Rect.Type);
  EXPECT_EQ(Rect.Type->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Rect.Type->Size, 32U);
  EXPECT_EQ(Rect.Components.size(), 4U);
  for (const auto &Component : Rect.Components) {
    EXPECT_EQ(Component.Kind, SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Component.ValueBytes, 8U);
  }
  std::string Diagnostic;
  EXPECT_TRUE(validateSourceABI(*Hint, Diagnostic)) << Diagnostic;

  auto Changed = Image;
  Changed.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/CoreImage.framework/Versions/A/CoreImage"};
  EXPECT_FALSE(
      objcSelectorSourceTypeHint(Changed, "imageByCroppingToRect:"));
  Changed.DynInfo.NeededLibs = {"/tmp/CoreImage.framework/CoreImage"};
  EXPECT_FALSE(
      objcSelectorSourceTypeHint(Changed, "imageByCroppingToRect:"));
  Changed = image(Arch::X64);
  Changed.ObjCMethods.clear();
  Changed.DynInfo.NeededLibs = {Module};
  EXPECT_FALSE(
      objcSelectorSourceTypeHint(Changed, "imageByCroppingToRect:"));
}

TEST(ObjCCallHints, IOSScaleUsesObservedFloatingResultToRejectConflicts) {
  auto Image = image(Arch::AArch64);
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation",
      "/System/Library/Frameworks/UIKit.framework/UIKit"};
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "scale"));

  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto Hint = objcSelectorSourceTypeHintForResultUse(
      Image, "scale",
      {SourceABICarrierKind::FloatingRegister, TRI.FPReturnReg, 0, 8});
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::ObjCSDK);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint->ReturnType->Size, 8U);
  EXPECT_EQ(Hint->ReturnLocation.Kind,
            SourceABICarrierKind::FloatingRegister);

  // QuartzCore contains both float and double declarations for this selector.
  // Their complete alternatives must remain available for an exact carrier
  // use to select the double result; the unqualified selector stays ambiguous.
  Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/QuartzCore.framework/QuartzCore");
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "scale"));
  const auto WithQuartzCore = objcSelectorSourceTypeHintForResultUse(
      Image, "scale",
      {SourceABICarrierKind::FloatingRegister, TRI.FPReturnReg, 0, 8});
  ASSERT_TRUE(WithQuartzCore);
  EXPECT_EQ(WithQuartzCore->ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(WithQuartzCore->ReturnType->Size, 8U);

  auto Changed = image(Arch::AArch64);
  Changed.ObjCMethods.clear();
  Changed.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation",
      "/tmp/UIKit.framework/UIKit"};
  EXPECT_FALSE(objcSelectorSourceTypeHintForResultUse(
      Changed, "scale",
      {SourceABICarrierKind::FloatingRegister, TRI.FPReturnReg, 0, 8}));
}

TEST(ObjCCallHints, SharedFrameworkRecordsUseTheSupportedArchitectureLayout) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.ObjCMethods.clear();
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/QuartzCore.framework/QuartzCore"};
    const auto Position = objcSelectorSourceTypeHint(Image, "position");
    const auto SetPosition = objcSelectorSourceTypeHint(Image, "setPosition:");
    if (Architecture == Arch::AArch64) {
      ASSERT_TRUE(Position);
      ASSERT_TRUE(SetPosition);
      EXPECT_EQ(Position->ReturnType->Size, 16U);
      EXPECT_EQ(Position->ReturnComponents.size(), 2U);
      EXPECT_EQ(SetPosition->Parameters.size(), 3U);
      EXPECT_EQ(SetPosition->Parameters.back().Components.size(), 2U);
      EXPECT_TRUE(equalSourceTypes(Position->ReturnType,
                                   SetPosition->Parameters.back().Type));
    } else {
      EXPECT_FALSE(Position);
      EXPECT_FALSE(SetPosition);
    }
    EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "transform"));
  }
}

TEST(ObjCCallHints, UIKitReceiverDeclarationsRequireProvenOwnerAndProvider) {
  for (const auto *Selector : {"images", "CIImage", "scale", "duration"}) {
    for (const bool Category : {false, true}) {
      SCOPED_TRACE(Selector);
      SCOPED_TRACE(Category);
      auto Image = receiverImage(Arch::AArch64);
      Image.DynInfo.NeededLibs = {
          "/System/Library/Frameworks/UIKit.framework/UIKit",
          "/System/Library/Frameworks/Foundation.framework/Foundation"};
      Image.ObjCMethods.front().Selector = "readImageProperty";
      Image.ObjCMethods.back().Selector = Selector;
      auto Other = Image.ObjCMethods.front();
      Other.Selector = Selector;
      Other.ClassName = "Third";
      Other.Implementation = 0x1600;
      Image.ObjCMethods.push_back(Other);
      Image.ObjCSourceReferences.at(0x2100).Name = Selector;
      if (Category) {
        Image.ObjCClasses.clear();
        Image.ObjCMethods.front().ClassName = "UIImage";
        Image.ObjCMethods.front().CategoryName = "ImageMetadata";
      } else {
        auto &Class = Image.ObjCClasses.front();
        Class.RootClass = false;
        Class.InheritanceStatus = "resolved";
        Class.SuperclassName = "UIImage";
      }
      // An unrelated owner's incompatible declaration must not replace the
      // receiver's method ABI or make the selector globally unambiguous.
      EXPECT_FALSE(objcSelectorSourceTypeHint(Image, Selector));
      const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
      ASSERT_TRUE(Receiver);
      const auto Hint = objcReceiverSourceTypeHint(Image, Selector, *Receiver);
      ASSERT_TRUE(Hint.Signature);
      EXPECT_EQ(Hint.Signature->Parameters.size(), 2U);
      EXPECT_EQ(Hint.Signature->ReturnType->Size, 8U);
      EXPECT_EQ(Hint.Signature->ReturnType->Kind,
                llvm::StringRef(Selector) == "images" ||
                        llvm::StringRef(Selector) == "CIImage"
                    ? NdTypeKind::Ptr
                    : NdTypeKind::Float);
      const auto Hints =
          buildObjCSourceCallHints(Image, receiverCaller(Arch::AArch64));
      ASSERT_EQ(Hints.size(), 1U);
      auto Call = receiverCallExpression(Hints.at(0x1204));
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
        auto Changed = Image;
        if (Mutation == 0)
          Changed.DynInfo.NeededLibs.front() = "/tmp/UIKit.framework/UIKit";
        if (Mutation == 1)
          Changed.Arch = Arch::X64;
        if (Mutation == 2) {
          auto Conflict = Changed.ObjCMethods.at(1);
          Conflict.ClassName = Changed.ObjCMethods.front().ClassName;
          Changed.ObjCMethods.push_back(Conflict);
        }
        auto ChangedReceiver = *Receiver;
        if (Mutation == 3)
          ChangedReceiver.IsClassMethod = true;
        if (Mutation == 4)
          ChangedReceiver.ClassName = "UnknownImage";
        EXPECT_FALSE(
            objcReceiverSourceTypeHint(Changed, Selector, ChangedReceiver)
                .Signature)
            << Mutation;
        if (Mutation < 3)
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Changed, {}))
              << Mutation;
      }
    }
  }
}

TEST(ObjCCallHints, UIKitImageConstructionKeepsScalarRecordAndResultTypes) {
  const struct {
    const char *Owner, *Selector;
    bool ClassMethod;
    NdTypeKind ReturnKind;
    unsigned Parameters;
    const char *ReturnClass;
  } Cases[] = {
      {"UIImage", "imageOrientation", false, NdTypeKind::Int, 2, ""},
      {"UIImage", "isHighDynamicRange", false, NdTypeKind::Int, 2, ""},
      {"UINavigationController", "interactivePopGestureRecognizer", false,
       NdTypeKind::Ptr, 2, "UIGestureRecognizer"},
      {"UINavigationController", "viewControllers", false, NdTypeKind::Ptr, 2,
       "NSArray"},
      {"UIViewController", "parentViewController", false, NdTypeKind::Ptr, 2,
       "UIViewController"},
      {"UIViewController", "transitionCoordinator", false, NdTypeKind::Ptr, 2,
       ""},
      {"UIScrollView", "flashScrollIndicators", false, NdTypeKind::Void, 2, ""},
      {"UIApplication", "endBackgroundTask:", false, NdTypeKind::Void, 3, ""},
      {"UIActivityIndicatorView", "initWithActivityIndicatorStyle:", false,
       NdTypeKind::Ptr, 3, "First"},
      {"UIImage", "imageFlippedForRightToLeftLayoutDirection", false,
       NdTypeKind::Ptr, 2, "UIImage"},
      {"UIImage", "imageWithCGImage:", true, NdTypeKind::Ptr, 3, "UIImage"},
      {"UIImage", "imageWithCGImage:scale:orientation:", true, NdTypeKind::Ptr,
       5, "UIImage"},
      {"UIImage", "initWithCGImage:scale:orientation:", false, NdTypeKind::Ptr,
       5, "First"},
      {"UIImage", "drawInRect:", false, NdTypeKind::Void, 3, ""},
      {"UIScreen", "mainScreen", true, NdTypeKind::Ptr, 2, "UIScreen"},
      {"UIScreen", "scale", false, NdTypeKind::Float, 2, ""},
      {"UIGraphicsImageRendererFormat", "defaultFormat", true, NdTypeKind::Ptr,
       2, "First"},
      {"UIGraphicsImageRendererFormat", "scale", false, NdTypeKind::Float, 2,
       ""},
      {"UIGraphicsImageRendererFormat", "setScale:", false, NdTypeKind::Void, 3,
       ""},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Selector);
    auto Image = receiverImage(Arch::AArch64, Case.ClassMethod);
    Image.ObjCMethods.front().Selector = "useImageAPI";
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/UIKit.framework/UIKit",
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    if (llvm::StringRef(Case.Owner) == "UIActivityIndicatorView" ||
        llvm::StringRef(Case.Owner) == "UIScrollView")
      Image.DynInfo.NeededLibs.push_back(
          "/System/Library/Frameworks/QuartzCore.framework/QuartzCore");
    auto &Class = Image.ObjCClasses.front();
    Class.RootClass = false;
    Class.InheritanceStatus = "resolved";
    Class.SuperclassName = Case.Owner;
    const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
    ASSERT_TRUE(Receiver);
    const auto Hint =
        objcReceiverSourceTypeHint(Image, Case.Selector, *Receiver);
    ASSERT_TRUE(Hint.Signature);
    EXPECT_EQ(Hint.Signature->ReturnType->Kind, Case.ReturnKind);
    ASSERT_EQ(Hint.Signature->Parameters.size(), Case.Parameters);
    EXPECT_EQ(Hint.ReturnClass.value_or(""), Case.ReturnClass);
    const auto &TRI = getTargetRegInfo(Arch::AArch64);
    if (llvm::StringRef(Case.Selector).contains("scale:orientation:")) {
      const auto &Scale = Hint.Signature->Parameters[3];
      EXPECT_EQ(Scale.Type->Kind, NdTypeKind::Float);
      EXPECT_EQ(Scale.Type->Size, 8U);
      EXPECT_EQ(Scale.Location.RegisterOffset, TRI.FPParamRegs[0]);
      const auto &Orientation = Hint.Signature->Parameters[4];
      EXPECT_EQ(Orientation.Type->Size, 8U);
      EXPECT_TRUE(Orientation.Type->IsSigned);
      EXPECT_EQ(Orientation.Location.RegisterOffset, TRI.IntParamRegs[3]);
    }
    if (llvm::StringRef(Case.Selector) == "drawInRect:") {
      const auto &Rect = Hint.Signature->Parameters[2];
      EXPECT_EQ(Rect.Type->Size, 32U);
      ASSERT_EQ(Rect.Components.size(), 4U);
      for (unsigned I = 0; I < 4; ++I) {
        EXPECT_EQ(Rect.Components[I].Kind,
                  SourceABICarrierKind::FloatingRegister);
        EXPECT_EQ(Rect.Components[I].RegisterOffset, TRI.FPParamRegs[I]);
      }
    }
    for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
      auto Changed = Image;
      auto ChangedReceiver = *Receiver;
      if (Mutation == 0)
        Changed.Arch = Arch::X64;
      if (Mutation == 1)
        Changed.DynInfo.NeededLibs.erase(Changed.DynInfo.NeededLibs.begin());
      if (Mutation == 2)
        ChangedReceiver.IsClassMethod = !Case.ClassMethod;
      EXPECT_FALSE(
          objcReceiverSourceTypeHint(Changed, Case.Selector, ChangedReceiver)
              .Signature)
          << Mutation;
    }
  }
}

TEST(ObjCCallHints, UIKitScreenResultSurvivesExactArcReturnIdentity) {
  auto Image = image(Arch::AArch64);
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/UIKit.framework/UIKit",
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  Image.ObjCSourceReferences[0x2100] = {
      ObjCSourceReference::Kind::Selector, 0x2100, 8, "mainScreen"};
  Image.ObjCSourceReferences[0x2110] = {
      ObjCSourceReference::Kind::Selector, 0x2110, 8, "scale"};
  Image.ObjCSourceReferences[0x2200] = {
      ObjCSourceReference::Kind::Class, 0x2200, 8, "UIScreen"};
  receiverRuntimeImport(Image, "objc_retainAutoreleasedReturnValue");

  LowFunc Function;
  Function.Entry = 0x1200;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const auto X0 = NdVar::reg(TRI.IntParamRegs[0], 8);
  const auto X1 = NdVar::reg(TRI.IntParamRegs[1], 8);
  Block.Ops = {
      operation(NdOp::LOAD, X0, {NdVar::cst(0x2200, 8)}, 0x1200),
      operation(NdOp::LOAD, X1, {NdVar::cst(0x2100, 8)}, 0x1204),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1208),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2188, 8)}, 0x120c),
      operation(NdOp::LOAD, X1, {NdVar::cst(0x2110, 8)}, 0x1210),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1214),
      operation(NdOp::RETURN, {}, {}, 0x1218)};
  Function.Blocks.push_back(Block);

  const auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 3U);
  ASSERT_TRUE(Hints.at(0x1208).Receiver);
  EXPECT_EQ(Hints.at(0x1208).Receiver->ClassName, "UIScreen");
  EXPECT_EQ(Hints.at(0x120c).ReturnedArgument, 0U);
  const auto &Scale = Hints.at(0x1214);
  ASSERT_TRUE(Scale.Receiver);
  ASSERT_EQ(Scale.Receiver->Steps.size(), 1U);
  EXPECT_EQ(Scale.Receiver->Steps.front().Selector, "mainScreen");
  EXPECT_EQ(Scale.Signature.ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Scale.Signature.ReturnType->Size, 8U);
  EXPECT_EQ(Scale.Signature.ReturnLocation.Kind,
            SourceABICarrierKind::FloatingRegister);

  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    auto Changed = Image;
    if (Mutation == 0)
      Changed.DynInfo.NeededLibs.front() = "/tmp/UIKit.framework/UIKit";
    if (Mutation == 1)
      Changed.ObjCSourceReferences.at(0x2200).Size = 4;
    if (Mutation == 2)
      Changed.DyldBindSlots.at(0x2188).Module = "/tmp/libobjc.A.dylib";
    const auto ChangedHints = buildObjCSourceCallHints(Changed, Function);
    const auto It = ChangedHints.find(0x1214);
    EXPECT_TRUE(It == ChangedHints.end() || !It->second.Receiver) << Mutation;
  }
}

TEST(ObjCCallHints, SharedFrameworkReceiverHierarchyKeepsConflictingEvidence) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = receiverImage(Architecture);
    Image.ObjCMethods.front().Selector = "readOpacity";
    Image.ObjCMethods.back().Selector = "opacity";
    auto &Class = Image.ObjCClasses.front();
    Class.RootClass = false;
    Class.InheritanceStatus = "resolved";
    Class.SuperclassName = "CALayer";
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    Image.ObjCSourceReferences.at(0x2100).Name = "opacity";
    EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "opacity"));
    const auto Hints =
        buildObjCSourceCallHints(Image, receiverCaller(Architecture));
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Hint = Hints.at(0x1204);
    ASSERT_TRUE(Hint.Receiver);
    EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Float);
    EXPECT_EQ(Hint.Signature.ReturnType->Size, 4U);
    auto Call = receiverCallExpression(Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    auto Conflicting = Image.ObjCMethods.back();
    Conflicting.ClassName = "First";
    Image.ObjCMethods.push_back(Conflicting);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
  }
}

namespace {
LowFunc frameSelectorCaller(Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  const auto Stack = NdVar::reg(TRI.StackPointer, 8);
  const auto Frame = NdVar::reg(TRI.FramePointer, 8);
  const auto Selector = NdVar::reg(TRI.IntParamRegs[1], 8);
  LowFunc Function;
  Function.Entry = 0x1200;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  Block.EndAddr = 0x1260;
  Block.Ops = {
      operation(NdOp::INT_SUB, Stack, {Stack, NdVar::cst(64, 8)}, 0x1200),
      operation(NdOp::COPY, Frame, {Stack}, 0x1204),
      operation(NdOp::LOAD, Selector, {NdVar::cst(0x2100, 8)}, 0x1208),
      operation(NdOp::INT_ADD, NdVar::tmp(0, 8), {Frame, NdVar::cst(16, 8)},
                0x120c),
      operation(NdOp::STORE, {}, {NdVar::tmp(0, 8), Selector}, 0x120c),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2200, 8)}, 0x1240),
      operation(NdOp::INT_ADD, NdVar::tmp(0, 8), {Frame, NdVar::cst(16, 8)},
                0x1248),
      operation(NdOp::LOAD, Selector, {NdVar::tmp(0, 8)}, 0x1248),
      operation(NdOp::INDIR_CALL, {}, {NdVar::cst(0x2180, 8)}, 0x1250),
      operation(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 4)}, 0x1258)};
  Function.Blocks.push_back(std::move(Block));
  return Function;
}
} // namespace

TEST(ObjCCallHints, PrivateFrameSelectorSurvivesADeclaredCall) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.ImportPtrSlots[0x2200] = "_objc_retain";
    const auto Function = frameSelectorCaller(Architecture);
    const auto Hints = buildObjCSourceCallHints(Image, Function);
    ASSERT_EQ(Hints.size(), 2U);
    EXPECT_EQ(Hints.at(0x1250).Selector, "scale:");
    const auto Med = convert(Image, Function);
    ASSERT_EQ(Med.CallInfos.size(), 2U);
    EXPECT_TRUE(Med.CallInfos.back().SourceCallHint);
  }
}

TEST(ObjCCallHints, IndirectMessageResultsRequireNilStorageModeling) {
  auto Image = image(Arch::AArch64);
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  Image.ObjCSourceReferences.at(0x2100).Name = "operatingSystemVersion";
  // The compiler-backed Foundation declaration has three signed words, but
  // nil objc_msgSend preserves the caller's old result buffer. A fixed C
  // record result does not supply that dispatch-specific storage behavior.
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "operatingSystemVersion"));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
}

namespace {
LowFunc nonNullSelfIndirectResultCaller() {
  LowFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "-[NSProcessInfo neverd_versionAtLeast:]";
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto Stack = NdVar::reg(TRI.StackPointer, 8);
  const auto Buffer = NdVar::reg(TRI.indirectResultReg(), 8);
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  Block.EndAddr = 0x1220;
  Block.Ops = {
      operation(NdOp::INT_SUB, Stack, {Stack, NdVar::cst(64, 8)}, 0x1200),
      operation(NdOp::INT_ADD, Buffer, {Stack, NdVar::cst(8, 8)}, 0x1204),
      operation(NdOp::CALL, NdVar::reg(TRI.IntReturnReg, 8),
                {NdVar::cst(0x1100, 8)}, 0x1208),
      operation(NdOp::INT_ADD, NdVar::tmp(0, 8), {Stack, NdVar::cst(8, 8)},
                0x120c),
      operation(NdOp::LOAD, NdVar::tmp(1, 8), {NdVar::tmp(0, 8)}, 0x1210),
      operation(NdOp::INT_EQUAL, NdVar::reg(TRI.IntReturnReg, 4),
                {NdVar::tmp(1, 8), NdVar::cst(1, 8)}, 0x1214),
      operation(NdOp::INT_ADD, Stack, {Stack, NdVar::cst(64, 8)}, 0x1218),
      operation(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 4)}, 0x121c)};
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

BinaryImage nonNullSelfIndirectResultImage() {
  auto Image = image(Arch::AArch64);
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  Image.ObjCSourceReferences.at(0x2100).Name = "operatingSystemVersion";
  Image.ObjCMethods.clear();
  ObjCMethod Method;
  Method.Status = "supported";
  Method.ClassName = "NSProcessInfo";
  Method.Selector = "neverd_hasVersion";
  Method.Implementation = 0x1200;
  Method.TypeHint = parseObjCMethodEncoding(Method.Selector, "B16@0:8");
  Image.ObjCMethods.push_back(std::move(Method));
  return Image;
}
} // namespace

TEST(ObjCCallHints,
     NonNullMethodSelfAndPrivateFrameAdmitIndirectMessageResult) {
  auto Image = nonNullSelfIndirectResultImage();
  const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1200);
  ASSERT_TRUE(Receiver);
  const auto Ordinary =
      objcReceiverSourceTypeHint(Image, "operatingSystemVersion", *Receiver);
  EXPECT_TRUE(Ordinary.HasDeclaration);
  EXPECT_FALSE(Ordinary.Signature);
  const auto NonNull =
      objcNonNilSelfSourceTypeHint(Image, "operatingSystemVersion", *Receiver);
  ASSERT_TRUE(NonNull.Signature);
  EXPECT_EQ(NonNull.Signature->ReturnLocation.Kind,
            SourceABICarrierKind::IndirectResultPointer);

  const auto Low = nonNullSelfIndirectResultCaller();
  const auto Hints = buildObjCSourceCallHints(Image, Low);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x1208);
  ASSERT_TRUE(Hint.Receiver);
  ASSERT_TRUE(Hint.ObjCIndirectResultStorage);
  EXPECT_EQ(Hint.ObjCIndirectResultStorage->MethodEntry, 0x1200U);
  EXPECT_EQ(Hint.ObjCIndirectResultStorage->FrameOffset, -56);
  EXPECT_EQ(Hint.ObjCIndirectResultStorage->ByteCount, 24U);
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 24U);

  const auto EntryHint = objcMethodSourceTypeHint(Image, Low.Entry);
  ASSERT_TRUE(EntryHint);
  std::map<va_t, SourceFunctionTypeHint> EntryHints{{Low.Entry, *EntryHint}};
  LowToMedConverter LowConverter;
  LowConverter.setBinaryImage(&Image);
  LowConverter.setSourceEntryTypeHints(&EntryHints);
  LowConverter.setSourceCallHintsEnabled(true);
  auto Med = LowConverter.convert(Low, Image.Arch, BinaryFormat::MachO);
  Med.SourceTypeHint = *EntryHint;
  inferMedTypes(Med, Image.Arch);
  ASSERT_TRUE(Med.SourceTypeHint);
  recoverCallAbi(Med, Image.Arch, {}, &Image);
  ASSERT_TRUE(Med.SourceTypeHint);
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  ASSERT_TRUE(Med.CallInfos.front().SourceCallHint);
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Image);
  const auto High = Converter.convert(Med, Image.Arch);
  const auto *Expression = sourceCall(High);
  ASSERT_NE(Expression, nullptr);
  ASSERT_TRUE(High.SourceTypeHint);
  ASSERT_EQ(High.Params.size(), High.SourceTypeHint->Parameters.size());
  ASSERT_EQ(Expression->Operands.size(), 2U);
  EXPECT_TRUE(
      sdk::objc_binding_detail::exactParameterValue(Expression->Operands[0], 0))
      << Expression->Operands[0]->str();
  EXPECT_GT(High.FrameSize, 0);
  EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}, nullptr, nullptr,
                                       &High));

  auto BadFunction = High;
  BadFunction.FrameSize = 16;
  EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}, nullptr,
                                        nullptr, &BadFunction));
  auto BadExpression = *Expression;
  BadExpression.Operands[0] = HighExpr::makeConst(1, 8);
  EXPECT_FALSE(sdk::objcSourceCallBound(BadExpression, Image, {}, nullptr,
                                        nullptr, &High));
}

TEST(ObjCCallHints, IndirectMessageResultRejectsMissingCallSiteProofs) {
  const auto BaseImage = nonNullSelfIndirectResultImage();
  const auto Base = nonNullSelfIndirectResultCaller();
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = BaseImage;
    auto Low = Base;
    auto &Ops = Low.Blocks.front().Ops;
    if (Mutation == 0)
      Ops.insert(Ops.begin() + 1,
                 operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                           {NdVar::cst(0, 8)}, 0x1202));
    if (Mutation == 1)
      Ops[1] = operation(NdOp::COPY, NdVar::reg(TRI.indirectResultReg(), 8),
                         {NdVar::cst(0x2000, 8)}, 0x1204);
    if (Mutation == 2) {
      Ops[0].Inputs[1] = NdVar::cst(16, 8);
      Ops[6].Inputs[1] = NdVar::cst(16, 8);
    }
    if (Mutation == 3)
      Ops.insert(Ops.begin() + 2,
                 operation(NdOp::STORE, {},
                           {NdVar::reg(TRI.IntParamRegs[3], 8),
                            NdVar::reg(TRI.indirectResultReg(), 8)},
                           0x1206));
    if (Mutation == 4)
      Image.ObjCMethods.front().ClassName = "NSString";
    EXPECT_TRUE(buildObjCSourceCallHints(Image, Low).empty());
  }
}

TEST(ObjCCallHints, FrameSelectorsRejectEscapesOverwritesAndUnknownCalls) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case < 8; ++Case) {
      auto Image = image(Architecture);
      Image.ImportPtrSlots[0x2200] = "_objc_retain";
      auto Function = frameSelectorCaller(Architecture);
      auto &Ops = Function.Blocks.front().Ops;
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto Frame = NdVar::reg(TRI.FramePointer, 8);
      if (Case == 0) {
        Ops[5].Inputs[0] = NdVar::cst(0x2210, 8);
      } else if (Case == 1) {
        Ops.insert(Ops.begin() + 5,
                   operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                             {Frame}, 0x1230));
      } else if (Case == 2) {
        Ops.insert(Ops.begin() + 5,
                   {operation(NdOp::INT_ADD, NdVar::tmp(0, 8),
                              {Frame, NdVar::cst(19, 8)}, 0x1230),
                    operation(NdOp::STORE, {},
                              {NdVar::tmp(0, 8), NdVar::cst(0, 1)}, 0x1230)});
      } else if (Case == 3) {
        Ops[4].MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      } else if (Case == 4) {
        Ops.insert(
            Ops.begin() + 5,
            operation(NdOp::STORE, {},
                      {NdVar::reg(TRI.IntParamRegs[0], 8), NdVar::cst(0, 8)},
                      0x1230));
      } else {
        const auto Opcode = Case == 5   ? NdOp::ATOMIC_XCHG
                            : Case == 6 ? NdOp::ATOMIC_ADD
                                        : NdOp::ATOMIC_CMPXCHG;
        auto Atomic = operation(Opcode, NdVar::tmp(1, 8),
                                {NdVar::tmp(0, 8), NdVar::cst(0, 8)}, 0x1230);
        Atomic.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
        if (Case == 7) {
          Atomic.NumInputs = 3;
          Atomic.Inputs[2] = NdVar::cst(1, 8);
        }
        Ops.insert(Ops.begin() + 5,
                   {operation(NdOp::INT_ADD, NdVar::tmp(0, 8),
                              {Frame, NdVar::cst(16, 8)}, 0x1230),
                    Atomic});
      }
      EXPECT_FALSE(buildObjCSourceCallHints(Image, Function).count(0x1250))
          << "case " << Case;
    }
}

TEST(ObjCCallHints, FrameSelectorJoinsRequireEveryIncomingStore) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case < 3; ++Case) {
      auto Image = image(Architecture);
      Image.ImportPtrSlots[0x2200] = "_objc_retain";
      const auto Base = frameSelectorCaller(Architecture);
      const auto &Ops = Base.Blocks.front().Ops;
      std::vector<LowBlock> Blocks(4);
      for (unsigned I = 0; I < 4; ++I) {
        Blocks[I].Id = I;
        Blocks[I].StartAddr = 0x1200 + I * 16;
        Blocks[I].EndAddr = Blocks[I].StartAddr + 16;
      }
      Blocks[0].Ops.assign(Ops.begin(), Ops.begin() + 3);
      Blocks[0].Succs = {1, 2};
      for (unsigned I : {1U, 2U}) {
        Blocks[I].Preds = {0};
        Blocks[I].Succs = {3};
        Blocks[I].Ops.assign(Ops.begin() + 3, Ops.begin() + 5);
        for (auto &Op : Blocks[I].Ops)
          Op.Addr = Blocks[I].StartAddr;
      }
      if (Case == 1)
        Blocks[2].Ops.clear();
      else if (Case == 2)
        Blocks[2].Ops.back().Inputs[1] = NdVar::cst(7, 8);
      Blocks[3].Preds = {1, 2};
      Blocks[3].Ops.assign(Ops.begin() + 5, Ops.end());
      std::vector<unsigned> Order{0, 1, 2, 3};
      do {
        auto Function = Base;
        Function.Blocks.clear();
        for (auto Index : Order)
          Function.Blocks.push_back(Blocks[Index]);
        const auto Hints = buildObjCSourceCallHints(Image, Function);
        EXPECT_EQ(Hints.count(0x1250), Case == 0 ? 1U : 0U) << Case;
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
}

TEST(ObjCCallHints, PossibleFrameEscapesSurviveJoinsAndPartialAliases) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool FullOverwrite : {false, true}) {
      auto Image = image(Architecture);
      Image.ImportPtrSlots[0x2200] = "_objc_retain";
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto Carrier = NdVar::reg(preservedFactRegister(Architecture), 8);
      auto Function = frameSelectorCaller(Architecture);
      const auto Ops = Function.Blocks.front().Ops;
      Function.Blocks.resize(4);
      for (unsigned I = 0; I < 4; ++I) {
        Function.Blocks[I].Id = I;
        Function.Blocks[I].StartAddr = 0x1200 + I * 16;
        Function.Blocks[I].EndAddr = Function.Blocks[I].StartAddr + 16;
      }
      auto &Entry = Function.Blocks[0];
      Entry.Ops.assign(Ops.begin(), Ops.begin() + 5);
      Entry.Succs = {1, 2};
      auto &Left = Function.Blocks[1];
      Left.Preds = {0};
      Left.Succs = {3};
      Left.Ops = {operation(NdOp::COPY, Carrier,
                            {NdVar::reg(TRI.FramePointer, 8)}, 0x1210),
                  operation(NdOp::INT_XOR, Carrier, {Carrier, NdVar::cst(1, 8)},
                            0x1214),
                  operation(NdOp::COPY,
                            NdVar::reg(Carrier.Offset, FullOverwrite ? 8 : 1),
                            {NdVar::cst(0, FullOverwrite ? 8 : 1)}, 0x1218)};
      auto &Right = Function.Blocks[2];
      Right.Preds = {0};
      Right.Succs = {3};
      Right.Ops = {operation(NdOp::COPY, Carrier, {NdVar::cst(0, 8)}, 0x1220)};
      auto &Join = Function.Blocks[3];
      Join.Preds = {1, 2};
      Join.Ops.assign(Ops.begin() + 5, Ops.end());
      Join.Ops.insert(Join.Ops.begin(),
                      operation(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
                                {Carrier}, 0x1230));
      const auto Blocks = Function.Blocks;
      std::vector<unsigned> Order{0, 1, 2, 3};
      do {
        Function.Blocks.clear();
        for (auto Index : Order)
          Function.Blocks.push_back(Blocks[Index]);
        const auto Hints = buildObjCSourceCallHints(Image, Function);
        EXPECT_EQ(Hints.count(0x1250), FullOverwrite ? 1U : 0U);
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
}

TEST(ObjCCallHints, FrameSlotsExcludeOutgoingArgumentsAndDeallocatedStorage) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case < 3; ++Case) {
      auto Image = image(Architecture);
      Image.ImportPtrSlots[0x2200] = "_objc_retain";
      auto Function = frameSelectorCaller(Architecture);
      auto &Ops = Function.Blocks.front().Ops;
      const auto Stack =
          NdVar::reg(getTargetRegInfo(Architecture).StackPointer, 8);
      if (Case == 1) {
        Image.ObjCMethods.front().TypeHint = signature(Architecture, 16);
        Ops[5].Inputs[0] = NdVar::cst(0x2180, 8);
      } else if (Case == 2) {
        Ops.insert(Ops.begin() + 5,
                   {operation(NdOp::INT_ADD, Stack, {Stack, NdVar::cst(64, 8)},
                              0x1230),
                    operation(NdOp::INT_SUB, Stack, {Stack, NdVar::cst(64, 8)},
                              0x1234)});
      }
      EXPECT_EQ(buildObjCSourceCallHints(Image, Function).count(0x1250),
                Case == 0 ? 1U : 0U)
          << Case;
    }
}

TEST(ObjCCallHints, FrameBackedgesRevokeProvisionalSpillBindings) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Overwrite : {false, true}) {
      auto Image = image(Architecture);
      Image.ImportPtrSlots[0x2200] = "_objc_retain";
      auto Function = frameSelectorCaller(Architecture);
      const auto Ops = Function.Blocks.front().Ops;
      Function.Blocks.front().Ops.assign(Ops.begin(), Ops.begin() + 5);
      Function.Blocks.front().Succs = {1};
      LowBlock Loop;
      Loop.Id = 1;
      Loop.StartAddr = 0x1240;
      Loop.EndAddr = 0x1260;
      Loop.Preds = {0, 1};
      Loop.Succs = {1};
      Loop.Ops.assign(Ops.begin() + 5, Ops.end() - 1);
      if (Overwrite) {
        const auto Frame =
            NdVar::reg(getTargetRegInfo(Architecture).FramePointer, 8);
        Loop.Ops.push_back(operation(NdOp::INT_ADD, NdVar::tmp(0, 8),
                                     {Frame, NdVar::cst(16, 8)}, 0x1258));
        Loop.Ops.push_back(operation(
            NdOp::STORE, {}, {NdVar::tmp(0, 8), NdVar::cst(0, 8)}, 0x1258));
      }
      Function.Blocks.push_back(Loop);
      EXPECT_EQ(buildObjCSourceCallHints(Image, Function).count(0x1250),
                Overwrite ? 0U : 1U);
      std::reverse(Function.Blocks.begin(), Function.Blocks.end());
      EXPECT_EQ(buildObjCSourceCallHints(Image, Function).count(0x1250),
                Overwrite ? 0U : 1U);
    }
}

TEST(ObjCCallHints, DynamicFormatInteger64TailRequiresDeclaredCompleteValues) {
  auto Image = selectorStubImage();
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/Foundation.framework/Foundation");
  Image.ObjCSourceReferences.at(0x2100).Name = "localizedStringWithFormat:";
  const auto Day = objcSelectorSourceTypeHint(Image, "day");
  ASSERT_TRUE(Day);
  ASSERT_TRUE(Day->ReturnType);
  ASSERT_EQ(Day->ReturnType->Kind, NdTypeKind::Int);
  ASSERT_EQ(Day->ReturnType->Size, 8U);
  const auto Integer = Day->ReturnType;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Tail = objcDynamicFormatInteger64ArgumentsSourceCallHint(
      Image, "localizedStringWithFormat:", {Integer});
  ASSERT_TRUE(Tail);
  ASSERT_TRUE(Tail->Format);
  EXPECT_TRUE(Tail->Format->DynamicInteger64Arguments);
  EXPECT_FALSE(Tail->Format->DynamicWithoutArguments);
  EXPECT_FALSE(Tail->Format->DynamicPointerArguments);
  ASSERT_EQ(Tail->Signature.Parameters.size(), 4U);
  EXPECT_EQ(Tail->Signature.Parameters[3].Location.Kind,
            SourceABICarrierKind::Stack);
  EXPECT_EQ(Tail->Signature.Parameters[3].Location.EntryStackOffset, 0);
  EXPECT_EQ(Tail->Signature.Parameters[3].Location.ValueBytes, 8U);

  auto Cast = [](const ExprPtr &Value, const TypeRef &Type) {
    auto E = std::make_shared<HighExpr>();
    E->Kind = ExprKind::Cast;
    E->Type = E->CastTo = Type;
    E->Operands = {Value};
    return E;
  };
  auto Leaf = [&] {
    auto E = HighExpr::makeCall(
        "objc_msgSend", 0,
        {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8)});
    E->Type = Integer;
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Hint->TargetName = "objc_msgSend";
    Hint->Selector = "day";
    Hint->Signature = *Day;
    E->SourceCallHint = std::move(Hint);
    return E;
  };
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = 42;
  V.Size = 8;
  V.TheArch = Arch::AArch64;
  auto Value = [&] { return HighExpr::makeVar(V, Integer); };
  auto Make = [&] {
    HighFunc F;
    F.ReturnType = Pointer;
    HighStmt Assign;
    Assign.Kind = StmtKind::Assign;
    Assign.Dst = Value();
    Assign.Val = Leaf();
    F.Body.push_back(Assign);
    auto Native = std::make_shared<SourceCallTypeHint>(*Tail);
    Native->CallKind = SourceCallTypeHint::Kind::Native;
    Native->TargetAddress = 0x1100;
    Native->TargetName = "sub_1100";
    Native->Selector.clear();
    Native->Format.reset();
    Native->Signature.Origin =
        SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Native->Signature.ReturnType = NdType::makeInt(8, false);
    for (auto &P : Native->Signature.Parameters)
      P.Type = NdType::makeInt(8, false);
    auto Call = HighExpr::makeCall("sub_1100", 0x1100,
                                   {HighExpr::makeConst(0, 8),
                                    HighExpr::makeConst(0, 8),
                                    HighExpr::makeConst(0, 8), Value()});
    Call->Type = Native->Signature.ReturnType;
    Call->SourceCallHint = std::move(Native);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    F.Body.push_back(Return);
    return F;
  };
  auto Check = [&](const HighFunc &F, bool Expected) {
    const auto Bound = sdk::bindObjCSourceReferences(F, Image);
    const auto &Call = Bound.Function.Body.back().RetVal;
    ASSERT_TRUE(Call && Call->SourceCallHint);
    const bool Recovered =
        Call->SourceCallHint->CallKind == SourceCallTypeHint::Kind::ObjCMessage;
    EXPECT_EQ(Recovered, Expected) << Bound.Limitation;
    if (Recovered) {
      ASSERT_TRUE(Call->SourceCallHint->Format);
      EXPECT_TRUE(Call->SourceCallHint->Format->DynamicInteger64Arguments);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}, nullptr, nullptr,
                                           &Bound.Function));
    }
  };
  Check(Make(), true);
  auto Casted = Make();
  Casted.Body.front().Val =
      Cast(Casted.Body.front().Val, NdType::makeInt(8, !Integer->IsSigned));
  Check(Casted, true);
  auto Joined = Make();
  Joined.Body.insert(Joined.Body.begin(), Joined.Body.front());
  Check(Joined, true);
  auto Selected = Make();
  Selected.Body.front().Val =
      HighExpr::makeBinop(NdOp::SELECT, HighExpr::makeConst(1, 1), Leaf());
  Selected.Body.front().Val->Operands.push_back(Leaf());
  Selected.Body.front().Val->Type = Integer;
  Check(Selected, true);

  for (unsigned Mutation = 0; Mutation < 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = Make();
    auto &Call = F.Body.back().RetVal;
    if (Mutation == 0)
      F.Body.front().Val = HighExpr::makeConst(7, 8);
    if (Mutation == 1)
      F.Body.front().Val =
          HighExpr::makeLoad(HighExpr::makeConst(0, 8), Integer);
    if (Mutation == 2) {
      auto P = V;
      P.Kind = MedVar::Param;
      P.Id = 0;
      F.Params.push_back({"unproved_integer", Integer});
      F.Body.front().Val = HighExpr::makeVar(P, Integer);
    }
    if (Mutation == 3)
      F.Body.front().Val->Type = NdType::makeInt(4, true);
    if (Mutation == 4) {
      F.Body.front().Val->Type = NdType::makeFloat(8);
      F.Body.front().Val = Cast(F.Body.front().Val, Integer);
    }
    if (Mutation == 5) {
      F.Body.front().Val = Cast(F.Body.front().Val, Integer);
      F.Body.front().Val->CastTo = NdType::makeInt(4, true);
    }
    if (Mutation == 6) {
      auto Bad = std::make_shared<SourceCallTypeHint>(
          *F.Body.front().Val->SourceCallHint);
      Bad->Signature.ReturnType = NdType::makeInt(8, !Integer->IsSigned);
      F.Body.front().Val->SourceCallHint = Bad;
    }
    if (Mutation == 7)
      F.Body.front().Val = Value();
    if (Mutation == 8) {
      auto Unknown = F.Body.front();
      Unknown.Val = HighExpr::makeConst(0, 8);
      F.Body.insert(F.Body.begin(), Unknown);
    }
    if (Mutation == 9 || Mutation == 10) {
      auto Bad = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
      if (Mutation == 9)
        Bad->Signature.Parameters[3].Location.EntryStackOffset = 8;
      else
        Bad->Signature.Parameters[3].Location.ValueBytes = 4;
      Call->SourceCallHint = Bad;
    }
    // The exact selector stub, complete operand carriers, and the declared
    // integer producer together remain sufficient without a duplicate native
    // callee hint.
    if (Mutation == 11)
      Call->SourceCallHint.reset();
    if (Mutation == 12) {
      auto Other = Leaf();
      auto OtherHint =
          std::make_shared<SourceCallTypeHint>(*Other->SourceCallHint);
      OtherHint->Signature = *objcSelectorSourceTypeHint(Image, "length");
      OtherHint->Selector = "length";
      Other->SourceCallHint = OtherHint;
      Other->Type = Other->SourceCallHint->Signature.ReturnType;
      auto Define = F.Body.front();
      Define.Val = Other;
      F.Body.insert(F.Body.begin(), Define);
    }
    Check(F, Mutation == 11);
  }

  const auto Bound = sdk::bindObjCSourceReferences(Make(), Image);
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    auto F = Bound.Function;
    auto Call = std::make_shared<HighExpr>(*F.Body.back().RetVal);
    auto MutableHint =
        std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = MutableHint;
    F.Body.back().RetVal = Call;
    auto &Format = *MutableHint->Format;
    if (Mutation == 0)
      Format.DynamicWithoutArguments = true;
    if (Mutation == 1)
      Format.DynamicPointerArguments = true;
    if (Mutation == 2)
      Format.FormatAddress = 0x4000;
    if (Mutation == 3)
      Format.AlternativeFormatAddresses = {0x4000};
    if (Mutation == 4)
      MutableHint->Signature.Parameters[3].Type = Pointer;
    if (Mutation == 5)
      F.Body.front().Val = HighExpr::makeConst(1, 8);
    EXPECT_FALSE(
        sdk::objcSourceCallBound(*Call, Image, {}, nullptr, nullptr, &F));
  }
  for (const auto &Bad :
       {NdType::makeInt(4, true), NdType::makeFloat(8), Pointer})
    EXPECT_FALSE(objcDynamicFormatInteger64ArgumentsSourceCallHint(
        Image, "localizedStringWithFormat:", {Bad}));
  EXPECT_FALSE(objcDynamicFormatInteger64ArgumentsSourceCallHint(
      Image, "localizedStringWithFormat:", {}));
  Image.Arch = Arch::X64;
  EXPECT_FALSE(objcDynamicFormatInteger64ArgumentsSourceCallHint(
      Image, "localizedStringWithFormat:", {Integer}));
}
