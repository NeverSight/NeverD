#include "../../lib/loader/MachO/ImmutableNativeFrame.h"
#include "../../lib/loader/Swift/SwiftBooleanProjection.h"
#include "../../lib/loader/Swift/SwiftVirtualSlot.h"
#include "../../lib/sdk/capi/ObjCSourceBindings.h"
#include "../../lib/sdk/capi/ObjCSwiftBooleanSources.h"
#include "../../lib/sdk/capi/ObjCSwiftVirtualSources.h"
#include "../../lib/sdk/capi/SwiftMangledSourceABI.h"
#include "../lift/core/MappedSectionFlags.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftVirtualCalls.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>

using namespace neverd;

namespace {
struct Fixture {
  enum class GetterKind { CGFloat, Double, Bool };
  static constexpr va_t Entry = 0x1100;
  static constexpr va_t CallSite = 0x1170;
  static constexpr va_t IvarSlot = 0x2100;
  static constexpr va_t MaskSlot = 0x2180;

  BinaryImage Image;
  LowFunc Function;

  explicit Fixture(GetterKind Kind = GetterKind::CGFloat, bool Setter = false) {
    const bool Bool = Kind == GetterKind::Bool;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x1000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x1000);
    Text.Data[CallSite - Text.VA + 0] = Bool && Setter ? 0xc0 : 0xa0;
    Text.Data[CallSite - Text.VA + 1] = 0x02;
    Text.Data[CallSite - Text.VA + 2] = 0x3f;
    Text.Data[CallSite - Text.VA + 3] = 0xd6;
    Image.Segments.push_back(std::move(Text));
    Segment Data;
    Data.VA = 0x2000;
    Data.Size = Data.FileSz = 0x1000;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(0x1000);
    Image.Segments.push_back(std::move(Data));
    Image.ImportPtrSlots[MaskSlot] = "_swift_isaMask";
    Image.DyldBindSlots[MaskSlot] = {
        "_swift_isaMask", 0, "/usr/lib/swift/libswiftCore.dylib", false};
    Image.ObjCSourceReferences[IvarSlot] = {
        ObjCSourceReference::Kind::IvarOffset, IvarSlot, 8, "_animationView",
        "_TtC6Lottie23CompatibleAnimationView"};
    ObjCMethod Method;
    Method.Implementation = Entry;
    Method.ClassName = "_TtC6Lottie23CompatibleAnimationView";
    Method.Selector =
        Bool ? (Setter ? "setShouldRasterizeWhenIdle:"
                       : "shouldRasterizeWhenIdle")
        : Kind == GetterKind::Double
            ? (Setter ? "setCurrentTime:" : "currentTime")
            : (Setter ? "setCurrentProgress:" : "currentProgress");
    Method.TypeEncoding = Setter ? (Bool ? "v20@0:8B16" : "v24@0:8d16")
                                 : (Bool ? "B16@0:8" : "d16@0:8");
    SourceFunctionTypeHint Signature;
    Signature.ReturnType =
        Setter ? NdType::makeVoid()
               : (Bool ? NdType::makeInt(1, false) : NdType::makeFloat(8));
    Signature.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())},
                            {"_cmd", NdType::makePtr(NdType::makeVoid())}};
    if (Setter)
      Signature.Parameters.push_back(
          {"value", Bool ? NdType::makeInt(1, false) : NdType::makeFloat(8)});
    std::string Diagnostic;
    EXPECT_TRUE(assignDarwinObjCSourceABI(Signature, Arch::AArch64, Diagnostic))
        << Diagnostic;
    Method.TypeHint = Signature;
    Image.ObjCMethods.push_back(Method);
    const std::string SymbolPrefix =
        Bool ? "_$s6Lottie23CompatibleAnimationViewC23shouldRasterizeWhenIdle"
        : Kind == GetterKind::Double
            ? "_$s6Lottie23CompatibleAnimationViewC11currentTime"
            : "_$s6Lottie23CompatibleAnimationViewC15currentProgress";
    const std::string SymbolSuffix =
        Bool                         ? (Setter ? "SbvsTo" : "SbvgTo")
        : Kind == GetterKind::Double ? (Setter ? "SdvsTo" : "SdvgTo")
                                     : (Setter ? "12CoreGraphics7CGFloatVvsTo"
                                               : "12CoreGraphics7CGFloatVvgTo");
    Image.Symbols.push_back({SymbolPrefix + SymbolSuffix, Entry, 0x80, true});

    Function.Entry = Entry;
    Function.Blocks.resize(1);
    Function.Blocks[0].StartAddr = Entry;
    auto Add = [&](va_t Address, NdOp Code, NdVar Output,
                   std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Addr = Address;
      Op.Opcode = Code;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Function.Blocks[0].Ops.push_back(Op);
    };
    const auto X0 = NdVar::reg(a64reg::X0, 8);
    const auto X8 = NdVar::reg(a64reg::X8, 8);
    const auto X9 = NdVar::reg(a64reg::X9, 8);
    const auto X20 = NdVar::reg(a64reg::X20, 8);
    const auto X21 = NdVar::reg(a64reg::X21, 8);
    const auto X22 = NdVar::reg(a64reg::X22, 8);
    const auto Target = Bool && Setter ? X22 : X21;
    Add(0x1110, NdOp::COPY, X8, {NdVar::dataAddress(IvarSlot, 8)});
    Add(0x1114, NdOp::LOAD, X8, {X8});
    Add(0x111c, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8), {X0, X8});
    Add(0x111c, NdOp::LOAD, X20, {NdVar::tmp(TmpBase, 8)});
    Add(0x1120, NdOp::LOAD, X8, {X20});
    Add(0x1124, NdOp::COPY, X9, {NdVar::dataAddress(MaskSlot, 8)});
    Add(0x1128, NdOp::LOAD, X9, {X9});
    Add(0x112c, NdOp::LOAD, X9, {X9});
    Add(0x1130, NdOp::INT_AND, X8, {X8, X9});
    Add(0x1138, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
        {X8, NdVar::scalar((Bool                         ? 600
                            : Kind == GetterKind::Double ? 648
                                                         : 624) +
                               (Setter ? 8 : 0),
                           8)});
    Add(0x1138, NdOp::LOAD, Target, {NdVar::tmp(TmpBase, 8)});
    Add(0x1140, NdOp::CALL, X0, {NdVar::codeAddress(0x1300, 8)});
    Add(CallSite, NdOp::INDIR_CALL, X0, {Target});
  }
};

void addVoidVirtualMetadata(Fixture &F) {
  constexpr va_t Metadata = 0x2400, Descriptor = 0x4000;
  auto &Method = F.Image.ObjCMethods[0];
  Method.ClassAddress = Metadata;
  Section Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 0x1000;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections.push_back(Data);
  Segment ReadOnly;
  ReadOnly.VA = Descriptor;
  ReadOnly.Size = ReadOnly.FileSz = 0x200;
  ReadOnly.Flags = SegmentFlags::Readable;
  ReadOnly.Data.resize(0x200);
  F.Image.Segments.push_back(ReadOnly);
  Section Constants;
  Constants.VA = Descriptor;
  Constants.Size = Constants.FileSz = 0x200;
  Constants.Flags = SegmentFlags::Readable;
  F.Image.Sections.push_back(Constants);
  auto &Mutable = F.Image.Segments[1].Data;
  const auto Put64 = [&](va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(Mutable.data() + Address - 0x2000, Value);
  };
  Put64(Metadata + 32, 0x2502);
  Put64(0x2500 + 24, 0x2580);
  std::copy(Method.ClassName.begin(), Method.ClassName.end(),
            Mutable.begin() + 0x580);
  Put64(Metadata + 56, uint64_t(24) << 32 | 136);
  Put64(Metadata + 64, Descriptor);
  Put64(Metadata + 96, 0x1400);
  F.Image.DataPtrRelocSlots.insert(Metadata + 64);
  F.Image.DataPtrRelocTargetOwners[Metadata + 64] = Descriptor;
  F.Image.CodePtrRelocSlots.insert(Metadata + 96);
  auto &Bytes = F.Image.Segments[2].Data;
  const auto Put32 = [&](unsigned Offset, uint32_t Value) {
    llvm::support::endian::write32le(Bytes.data() + Offset, Value);
  };
  Put32(0, 0x80000050);
  Put32(4, 0xc0 - 4);
  Put32(8, 0x80 - 8);
  Put32(24, 3);
  Put32(28, 14);
  Put32(32, 1);
  Put32(44, 12);
  Put32(48, 1);
  Put32(52, 0x10);
  Put32(56, uint32_t(0x1400 - (Descriptor + 56)));
  Put32(0xc0, 0);
  Put32(0xc8, 0xe0 - 0xc8);
  const std::string Name = "AnimationViewBase", Module = "Lottie";
  std::copy(Name.begin(), Name.end(), Bytes.begin() + 0x80);
  std::copy(Module.begin(), Module.end(), Bytes.begin() + 0xe0);
  F.Image.Symbols.push_back(
      {"_$s6Lottie17AnimationViewBaseC6layoutyyF", 0x1400, 4, true});
}

struct NativeSelfFixture : Fixture {
  static constexpr va_t NativeCall = Entry + 60;
  llvm::LLVMContext Context;
  PipelineResult Result;
  SourceFunctionTypeHint EntryABI;

  void word(unsigned Index, uint32_t Value) {
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + Entry - 0x1000 + Index * 4, Value);
  }

  explicit NativeSelfFixture(bool Comparison = false) {
    Section Text;
    Text.Name = "__text";
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x1000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Text);
    Image.ObjCMethods[0].ClassName = "_TtC6Lottie17AnimationViewBase";
    Image.ObjCMethods[0].Implementation = 0x1500;
    addVoidVirtualMetadata(*this);
    Image.Symbols[0].Name =
        "_$s6Lottie17AnimationViewBaseC7setPair_6secondySo8NSObjectC_AGtF";
    Image.Symbols[0].Size = 76;
    // Both paths preserve entry swiftself in x21. Dispatch reads the live
    // object's masked isa and its own no-argument void vtable slot.
    const uint32_t Words[] = {0xa9be53f5, 0xa9017bfd, 0x910043fd, 0xaa1403f5,
                              0xb4000060, 0xaa0003e9, 0x14000002, 0xaa0103e9,
                              0xf94002a8, 0xb0000009, 0xf940c129, 0xf9400129,
                              0x8a080128, 0xf9403108, 0xaa1503f4, 0xd63f0100,
                              0xa9417bfd, 0xa8c253f5, 0xd65f03c0};
    for (unsigned I = 0; I < std::size(Words); ++I)
      word(I, Words[I]);
    if (Comparison) {
      word(4, 0x9400007c); // BL 0x1300, exact NSObject equality veneer.
      word(5, 0x36000040); // TBZ w0,#0,0x111c observes only bit zero.
      const uint32_t Veneer[] = {0xb0000010, 0xf9410210, 0xd61f0200};
      for (unsigned I = 0; I < 3; ++I)
        llvm::support::endian::write32le(
            Image.Segments[0].Data.data() + 0x300 + I * 4, Veneer[I]);
      constexpr va_t EqualitySlot = 0x2200;
      Image.ImportPtrSlots[EqualitySlot] =
          SwiftBooleanObjectEqualityImport.str();
      Image.DyldBindSlots[EqualitySlot] = {
          SwiftBooleanObjectEqualityImport.str(), 0,
          SwiftBooleanObjectEqualityProvider.str(), false};
      Image.DynInfo.NeededLibs.push_back(
          SwiftBooleanObjectEqualityProvider.str());
    }
    const auto ABI =
        sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Image, Entry);
    EXPECT_TRUE(ABI);
    if (ABI)
      EntryABI = *ABI;
    run();
  }

  void run() {
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry};
    Options.SourceTypeHints.emplace(Entry, EntryABI);
    Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
    const auto Found =
        std::find_if(Result.LowFuncs.begin(), Result.LowFuncs.end(),
                     [](const auto &F) { return F.Entry == Entry; });
    EXPECT_NE(Found, Result.LowFuncs.end());
    if (Found != Result.LowFuncs.end())
      Function = *Found;
  }

  HighFunc *high() {
    for (auto &F : Result.HighFuncs)
      if (F.Entry == Entry)
        return &F;
    return nullptr;
  }
  MedFunc *med() {
    for (auto &F : Result.MedFuncs)
      if (F.Entry == Entry)
        return &F;
    return nullptr;
  }
};

ExprPtr nativeVirtualExpression(const HighFunc &Function) {
  ExprPtr Found;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty()) {
        auto Expression = Pending.back();
        Pending.pop_back();
        if (!Expression)
          continue;
        if (Expression->SourceCallHint && Expression->SourceCallHint->Virtual &&
            Expression->SourceCallHint->Virtual->NativeSelfClass)
          Found = Expression;
        Expression->forEachChildExpr(
            [&](const ExprPtr &Child) { Pending.push_back(Child); });
      }
    });
  });
  return Found;
}
} // namespace

TEST(SwiftVirtualCalls, NativeClassSelfSurvivesEveryIncomingPath) {
  NativeSelfFixture F;
  ASSERT_GT(F.Function.Blocks.size(), 1U);
  ASSERT_TRUE(swift_virtual_detail::isVoidClassVirtualSlot(
      F.Image, F.Image.ObjCMethods[0], 96, "Lottie", "AnimationViewBase"));
  size_t Budget = 1U << 18;
  ASSERT_TRUE(immutableNativeFrameMachineMatches(F.Image, F.Function, Budget));
  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(NativeSelfFixture::NativeCall);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint.Signature.Parameters.size(), 1U);
  EXPECT_EQ(Hint.Signature.Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  std::reverse(F.Function.Blocks.begin(), F.Function.Blocks.end());
  EXPECT_EQ(buildSwiftVirtualCallHints(F.Image, F.Function).size(), 1U);
  unsigned Bound = 0;
  for (const auto &Function : F.Result.MedFuncs)
    if (Function.Entry == Fixture::Entry)
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::INDIR_CALL &&
              Op.Addr == NativeSelfFixture::NativeCall) {
            ASSERT_TRUE(Op.SourceCallHint);
            EXPECT_EQ(Op.SourceCallHint->CallKind,
                      SourceCallTypeHint::Kind::SwiftVirtual);
            EXPECT_EQ(Op.NumInputs, 2U);
            ++Bound;
          }
  EXPECT_EQ(Bound, 1U);
}

TEST(SwiftVirtualCalls, NativeClassSelfRejectsChangedPathOrMachine) {
  for (unsigned Case = 0; Case < 22; ++Case) {
    SCOPED_TRACE(Case);
    NativeSelfFixture F;
    if (Case == 0 || Case == 1) {
      // One predecessor substitutes an argument; or entry self is truncated.
      F.word(Case == 0 ? 7 : 3, Case == 0 ? 0xaa0003f5 : 0x2a1403f5);
      F.run();
    } else if (Case == 2) {
      F.Function.Blocks.front().Preds.push_back(999);
    } else if (Case == 3) {
      F.Function.Blocks.push_back(F.Function.Blocks.front());
    } else if (Case == 4) {
      F.Function.Blocks.front().InstructionBoundaries.clear();
    } else if (Case == 5) {
      // The stale LowIR still claims x20 = x21, unlike the current image.
      F.word(14, 0xaa0003f4);
    } else if (Case == 6) {
      F.Image.Symbols.push_back(F.Image.Symbols.front());
    } else if (Case == 7) {
      F.Image.ObjCMethods.front().ClassName = "DifferentClass";
    } else if (Case == 8) {
      F.word(5, 0xb5000001); // A reaching predecessor has a self-loop.
      F.run();
    } else if (Case == 9) {
      F.word(5, 0xaa0003f4);  // Change swiftself on only one path.
      F.word(14, 0xd503201f); // No restoring copy at the call.
      F.run();
    } else if (Case == 10) {
      F.word(3, 0xaa0003e9); // Self saved in volatile x9 instead of x21.
      F.word(5, 0x9400007b); // BL 0x1300 clobbers that saved self.
      F.word(7, 0xaa0103ea);
      F.word(8, 0xf9400128);
      F.word(14, 0xaa0903f4);
      F.run();
    } else if (Case == 11) {
      F.Function.ModuleAnalysisRoots.insert(0x1400);
    } else if (Case == 12) {
      ++F.Function.DecodedInstructionCount;
    } else if (Case == 13) {
      F.Function.Blocks.back().ExceptionalPreds.emplace_back();
    } else if (Case == 14) {
      F.Image.Symbols[0].Name =
          "_$s6Lottie17AnimationViewBaseC7setPair_6secondySo8NSObjectC_AGtYaF";
    } else if (Case == 15) {
      F.Image.Symbols[0].Name =
          "_$s6Lottie17AnimationViewBaseC7setPair_6secondSiSo8NSObjectC_AGtF";
    } else if (Case == 16) {
      F.Image.DyldBindSlots[Fixture::MaskSlot].WeakImport = true;
    } else if (Case == 17) {
      F.Image.DyldBindSlots[Fixture::MaskSlot].Module =
          "/tmp/libswiftCore.dylib";
    } else if (Case == 18) {
      F.Image.ObjCMethods[0].Implementation = Fixture::Entry;
    } else if (Case == 19) {
      setMappedSectionFlags(F.Image, F.Image.Sections[0],
                            SegmentFlags::Readable | SegmentFlags::Writable |
                                SegmentFlags::Executable);
    } else if (Case == 20) {
      F.Image.Symbols.back().Name =
          "_$s6Lottie17AnimationViewBaseC6layoutyySbF";
    } else {
      F.Image.ObjCMethods.push_back(F.Image.ObjCMethods[0]);
      F.Image.ObjCMethods.back().ClassName = "ConflictingClass";
    }
    EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  }
}

TEST(SwiftVirtualCalls, NativePublicationRechecksTheOriginalDynamicCall) {
  NativeSelfFixture F;
  ASSERT_TRUE(F.high());
  auto Expression = nativeVirtualExpression(*F.high());
  ASSERT_TRUE(Expression);
  EXPECT_TRUE(Expression->IsIndirectCall);
  EXPECT_EQ(Expression->CallAddr, 0U);
  EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, F.Image, {}));
  EXPECT_TRUE(sdk::objCSwiftVirtualSourceCallBound(*Expression, F.Image,
                                                   F.Result, *F.high()));
}

TEST(SwiftVirtualCalls,
     DeclaredVoidEntryNormalizesBooleanBeforeVirtualDispatch) {
  NativeSelfFixture F(true);
  ASSERT_TRUE(F.high());
  ASSERT_TRUE(F.med());
  const auto Qualified =
      qualifySwiftBooleanProjections(F.Image, F.Function, F.EntryABI);
  ASSERT_EQ(Qualified.size(), 1U);
  EXPECT_EQ(Qualified[0].Normalization.Site.Instruction, Fixture::Entry + 16);
  EXPECT_EQ(Qualified[0].Runtime.RawContract.DefinedResultBits, 1U);
  unsigned Comparisons = 0;
  for (const auto &Block : F.med()->Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Addr == Fixture::Entry + 16 && Op.Opcode == NdOp::CALL) {
        ASSERT_TRUE(Op.SourceCallHint);
        EXPECT_TRUE(Op.SourceCallHint->BooleanResult);
        EXPECT_EQ(Op.NumInputs, 4U); // target, lhs, rhs, metadata in swiftself
        ++Comparisons;
      }
  EXPECT_EQ(Comparisons, 1U);
  auto Signature = F.EntryABI;
  Signature.Parameters.pop_back();
  EXPECT_TRUE(
      qualifySwiftBooleanProjections(F.Image, F.Function, Signature).empty());
  F.word(5, 0x34000040); // CBZ w0 would observe undefined bits 31:1.
  F.run();
  EXPECT_TRUE(
      qualifySwiftBooleanProjections(F.Image, F.Function, F.EntryABI).empty());
}

TEST(SwiftVirtualCalls, NativePublicationRejectsStaleAndDuplicatedEvidence) {
  for (unsigned Case = 0; Case < 27; ++Case) {
    SCOPED_TRACE(Case);
    NativeSelfFixture F;
    ASSERT_TRUE(F.high());
    ASSERT_TRUE(F.med());
    auto Expression = nativeVirtualExpression(*F.high());
    ASSERT_TRUE(Expression);
    auto Hint = *Expression->SourceCallHint;
    MedVar WrongParameter;
    WrongParameter.Kind = MedVar::Param;
    WrongParameter.TheArch = Arch::AArch64;
    WrongParameter.Size = 8;
    switch (Case) {
    case 0:
      F.Result.SourceImage = nullptr;
      break;
    case 1:
      F.Result.Success = false;
      break;
    case 2:
      F.Result.LowFuncs.push_back(F.Function);
      break;
    case 3:
      F.med()->SourceParametersBound = false;
      break;
    case 4:
      for (auto &Audit : F.Result.FunctionAudits)
        if (Audit.Entry == Fixture::Entry)
          Audit.MedIRVerified = false;
      break;
    case 5:
      F.high()->SourceTypeHint->Parameters.pop_back();
      break;
    case 6:
      F.word(7, 0xaa0003f5);
      break;
    case 7:
      F.Image.CodePtrRelocSlots.clear();
      break;
    case 8:
      Hint.Virtual->CallSite += 4;
      break;
    case 9:
      Hint.Virtual->MethodEntry += 4;
      break;
    case 10:
      Hint.Virtual->NativeSelfClass += 8;
      break;
    case 11:
      Hint.Virtual->VtableByteOffset += 8;
      break;
    case 12:
      Hint.Virtual->DirectSelf = true;
      break;
    case 13:
      Hint.Signature.Parameters[0].Location.ValueBytes = 4;
      break;
    case 14:
      Expression->IsIndirectCall = false;
      break;
    case 15:
      Expression->CallAddr = 0x1400;
      break;
    case 16:
      Expression->IndirectTarget = HighExpr::makeConst(0x1400, 8);
      break;
    case 17:
      Expression->Operands.clear();
      break;
    case 18:
      Expression->Type = NdType::makeInt(8);
      break;
    case 19: {
      HighStmt Statement;
      Statement.Kind = StmtKind::Call;
      Statement.CallExpr = Expression;
      F.high()->Body.push_back(Statement);
      break;
    }
    case 20:
    case 21:
    case 22:
      for (auto &Block : F.med()->Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::INDIR_CALL) {
            if (Case == 20)
              ++Op.OriginSeq;
            if (Case == 21)
              Op.SourceCallHint.reset();
            if (Case == 22)
              Op.PreservesCallerSaved = true;
          }
      break;
    case 23:
      for (auto &Low : F.Result.LowFuncs)
        if (Low.Entry == Fixture::Entry)
          for (auto &Block : Low.Blocks)
            for (auto &Op : Block.Ops)
              if (Op.Opcode == NdOp::INDIR_CALL)
                Op.Inputs[0] = NdVar::reg(a64reg::X9, 8);
      break;
    case 24:
      Expression->Operands[0] = HighExpr::makeVar(
          WrongParameter, NdType::makePtr(NdType::makeVoid()));
      break;
    case 25:
      Expression->IndirectTarget =
          HighExpr::makeVar(WrongParameter, NdType::makeInt(8));
      break;
    case 26:
      F.high()->Params.pop_back();
      break;
    }
    Expression->SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(Hint);
    EXPECT_FALSE(sdk::objCSwiftVirtualSourceCallBound(*Expression, F.Image,
                                                      F.Result, *F.high()));
  }
}

TEST(SwiftVirtualCalls, NativeDynamicDispatchMatchesOriginalARM64AtO0AndO2) {
#if defined(NEVERD_TEST_CLANG) && defined(__APPLE__) && defined(__aarch64__)
  for (bool Comparison : {false, true}) {
    SCOPED_TRACE(Comparison);
    NativeSelfFixture F(Comparison);
    ASSERT_TRUE(F.high());
    const std::map<va_t, const HighFunc *> Functions{
        {Fixture::Entry, F.high()}};
    auto Bound =
        sdk::bindObjCSourceReferences(*F.high(), F.Image, nullptr, &Functions);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto Expression = nativeVirtualExpression(Bound.Function);
    ASSERT_TRUE(Expression);
    ASSERT_TRUE(sdk::objCSwiftVirtualSourceCallBound(*Expression, F.Image,
                                                     F.Result, Bound.Function));
    Bound.Function.Name = "native_class_virtual";
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-class-virtual",
                                                      Directory));
    const std::filesystem::path Work(Directory.str().str());
    struct Cleanup {
      std::filesystem::path Work;
      ~Cleanup() {
        std::error_code Error;
        std::filesystem::remove_all(Work, Error);
      }
    } Cleanup{Work};
    const auto Path = (Work / "virtual.c").string();
    std::ofstream Out(Path);
    Out << (Comparison ? "#define HAVE_COMPARISON 1\n"
                       : "#define HAVE_COMPARISON 0\n")
        << Source << R"(
#include <stddef.h>
uint64_t runtime_isa_mask __asm__("_swift_isaMask") = ~(uint64_t)7;
uint64_t raw_bool_bits;
uintptr_t raw_bool_inputs[3];
unsigned raw_bool_calls;
static unsigned calls, target;
static void *observed;
typedef void __attribute__((swiftcall)) (*method)(void * __attribute__((swift_context)));
static void __attribute__((swiftcall)) first(void * __attribute__((swift_context)) self) {
  ++calls; target = 1; observed = self;
}
static void __attribute__((swiftcall)) second(void * __attribute__((swift_context)) self) {
  ++calls; target = 2; observed = self;
}
extern void __attribute__((swiftcall)) original_class_virtual(
    void *, void *, void * __attribute__((swift_context)));
__asm__(".text\n.p2align 2\n.globl _original_class_virtual\n_original_class_virtual:\n"
)";
    // Every non-relocated instruction is copied byte-for-byte from the same
    // fixture that the current LowIR and virtual-call proof authenticate.
    for (unsigned I = 0; I < 19; ++I) {
      if (I == 4 && Comparison)
        Out << "\"bl _$sSo8NSObjectC10ObjectiveCE2eeoiySbAB_ABtFZ\\n\"\n";
      else if (I == 9)
        Out << "\"adrp x9,Lnative_mask_got@PAGE\\n\"\n";
      else if (I == 10)
        Out << "\"ldr x9,[x9,Lnative_mask_got@PAGEOFF]\\n\"\n";
      else
        Out << "\".long "
            << llvm::support::endian::read32le(F.Image.Segments[0].Data.data() +
                                               0x100 + I * 4)
            << "\\n\"\n";
    }
    Out << R"(
".p2align 2\n.globl _$sSo8NSObjectC10ObjectiveCE2eeoiySbAB_ABtFZ\n"
"_$sSo8NSObjectC10ObjectiveCE2eeoiySbAB_ABtFZ:\n"
"adrp x8,_raw_bool_inputs@PAGE\nadd x8,x8,_raw_bool_inputs@PAGEOFF\n"
"stp x0,x1,[x8]\nstr x20,[x8,#16]\n"
"adrp x8,_raw_bool_calls@PAGE\nldr w9,[x8,_raw_bool_calls@PAGEOFF]\n"
"add w9,w9,#1\nstr w9,[x8,_raw_bool_calls@PAGEOFF]\n"
"adrp x8,_raw_bool_bits@PAGE\nldr x0,[x8,_raw_bool_bits@PAGEOFF]\nret\n"
".section __DATA_CONST,__const\n.p2align 3\nLnative_mask_got:\n"
".quad _swift_isaMask\n.text\n");
int main(void) {
  struct { uint64_t before; uintptr_t isa; uint64_t after; } object;
  uintptr_t table[16] __attribute__((aligned(16))) = {0};
  object.before = 0xfeed1234; object.after = 0xcafe5678;
  for (unsigned i = 0; i != 1024; ++i) {
    method current = i & 1 ? first : second;
    table[12] = (uintptr_t)current;
    object.isa = (uintptr_t)table | (i & 7);
    void *a = i & 2 ? &object.before : NULL;
    void *b = i & 4 ? &object.after : NULL;
    raw_bool_bits = (0xfeeddeaddead0000ULL ^ ((uint64_t)i << 24)) | (i & 1);
    raw_bool_calls = 0;
    calls = target = 0; observed = NULL;
    original_class_virtual(a, b, &object.isa);
    if (calls != 1 || target != (i & 1 ? 1 : 2) || observed != &object.isa) return 1;
    if (raw_bool_calls != HAVE_COMPARISON || (HAVE_COMPARISON &&
        (raw_bool_inputs[0] != (uintptr_t)a || raw_bool_inputs[1] != (uintptr_t)b ||
         raw_bool_inputs[2] != (uintptr_t)&object.isa))) return 4;
    raw_bool_calls = 0;
    calls = target = 0; observed = NULL;
    native_class_virtual(a, b, &object.isa);
    if (calls != 1 || target != (i & 1 ? 1 : 2) || observed != &object.isa) return 2;
    if (raw_bool_calls != HAVE_COMPARISON || (HAVE_COMPARISON &&
        (raw_bool_inputs[0] != (uintptr_t)a || raw_bool_inputs[1] != (uintptr_t)b ||
         raw_bool_inputs[2] != (uintptr_t)&object.isa))) return 5;
    if (object.before != 0xfeed1234 || object.after != 0xcafe5678 ||
        object.isa != ((uintptr_t)table | (i & 7)) || table[12] != (uintptr_t)current) return 3;
  }
  return 0;
}
)";
    Out.close();
    for (const auto *Level : {"-O0", "-O2"}) {
      SCOPED_TRACE(Level);
      const auto Output = (Work / (std::string("virtual") + Level)).string();
      ASSERT_EQ(llvm::sys::ExecuteAndWait(
                    NEVERD_TEST_CLANG,
                    {NEVERD_TEST_CLANG, Level, Path, "-o", Output}),
                0)
          << Source;
      EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}), 0);
    }
  }
#else
  GTEST_SKIP() << "Original ARM64 comparison requires Apple ARM64 and Clang";
#endif
}

TEST(SwiftVirtualCalls, ExactMaskedIsaGetterBindsSwiftContext) {
  Fixture F;
  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->MethodEntry, Fixture::Entry);
  EXPECT_EQ(Hint.Virtual->IsaMaskImport, Fixture::MaskSlot);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 624U);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));
  EXPECT_EQ(Hint.Signature.Parameters.at(0).TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  auto Global = darwinRuntimeGlobalAddressHint(F.Image, Fixture::MaskSlot);
  ASSERT_TRUE(Global);
  EXPECT_EQ(Global->TargetName, "swift_isaMask");
}

TEST(SwiftVirtualCalls, ExactMaskedIsaBoolGetterBindsSwiftContext) {
  Fixture F(Fixture::GetterKind::Bool);
  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 600U);
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 1U);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));

  F.Image.ObjCMethods[0].TypeEncoding = "d16@0:8";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, ExactMaskedIsaDoubleGetterBindsSwiftContext) {
  Fixture F(Fixture::GetterKind::Double);
  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 648U);
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 8U);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));

  F.Image.Symbols[0].Name += "Other";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, ExactMaskedIsaSettersBindValueAndSwiftContext) {
  const std::pair<Fixture::GetterKind, uint32_t> Cases[] = {
      {Fixture::GetterKind::CGFloat, 632},
      {Fixture::GetterKind::Double, 656},
      {Fixture::GetterKind::Bool, 608},
  };
  for (const auto &[Kind, Slot] : Cases) {
    Fixture F(Kind, true);
    const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Hint = Hints.at(Fixture::CallSite);
    ASSERT_TRUE(Hint.Virtual);
    EXPECT_EQ(Hint.Virtual->VtableByteOffset, Slot);
    ASSERT_TRUE(Hint.Signature.ReturnType);
    EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Hint.Signature.Parameters.size(), 2U);
    EXPECT_EQ(Hint.Signature.Parameters[0].Type->Kind,
              Kind == Fixture::GetterKind::Bool ? NdTypeKind::Int
                                                : NdTypeKind::Float);
    EXPECT_EQ(Hint.Signature.Parameters[1].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));
  }

  Fixture F(Fixture::GetterKind::Bool, true);
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000] = 0xa0;
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, ExactVoidMethodWindowRejectsOrdinaryArguments) {
  Fixture F;
  auto &Method = F.Image.ObjCMethods[0];
  Method.Selector = "stop";
  Method.TypeEncoding = "v16@0:8";
  SourceFunctionTypeHint Signature;
  Signature.ReturnType = NdType::makeVoid();
  Signature.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())},
                          {"_cmd", NdType::makePtr(NdType::makeVoid())}};
  std::string Diagnostic;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Signature, Arch::AArch64, Diagnostic))
      << Diagnostic;
  Method.TypeHint = Signature;
  F.Image.Symbols[0].Name = "_$s6Lottie23CompatibleAnimationViewC4stopyyFTo";
  F.Image.Imports.push_back({"/usr/lib/libobjc.A.dylib", "_objc_retain", 0, 0});
  F.Image.ImportStubIndices[0x1300] = 0;
  auto &Ops = F.Function.Blocks[0].Ops;
  for (auto &Op : Ops)
    if (Op.Opcode == NdOp::INT_ADD && Op.Addr == 0x1138)
      Op.Inputs[1] = NdVar::scalar(280, 8);
  LowOp Saved;
  Saved.Addr = 0x1144;
  Saved.Opcode = NdOp::COPY;
  Saved.Output = NdVar::reg(a64reg::X19, 8);
  Saved.addInput(NdVar::reg(a64reg::X0, 8));
  LowOp Link;
  Link.Addr = Fixture::CallSite;
  Link.Opcode = NdOp::COPY;
  Link.Output = NdVar::reg(a64reg::X30, 8);
  Link.addInput(NdVar::scalar(Fixture::CallSite + 4, 8));
  Ops.insert(Ops.end() - 1, Saved);
  Ops.insert(Ops.end() - 1, Link);

  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 280U);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint.Signature.Parameters.size(), 1U);
  EXPECT_EQ(Hint.Signature.Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));

  LowOp ExtraArgument;
  ExtraArgument.Addr = 0x1148;
  ExtraArgument.Opcode = NdOp::COPY;
  ExtraArgument.Output = NdVar::reg(a64reg::X0, 8);
  ExtraArgument.addInput(NdVar::scalar(0, 8));
  ExtraArgument.Inputs[0].Provenance = ConstantAddressProvenance::Unknown;
  Ops.insert(Ops.end() - 2, ExtraArgument);
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());

  LowOp SecondZero = ExtraArgument;
  SecondZero.Addr = 0x114c;
  SecondZero.Output = NdVar::reg(a64reg::X1, 8);
  Ops.insert(Ops.end() - 2, SecondZero);
  const auto NilArgumentHints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(NilArgumentHints.size(), 1U);
  const auto &NilHint = NilArgumentHints.at(Fixture::CallSite);
  ASSERT_TRUE(NilHint.Virtual);
  EXPECT_EQ(NilHint.Virtual->ZeroArgumentWords, 2U);
  ASSERT_EQ(NilHint.Signature.Parameters.size(), 3U);
  EXPECT_EQ(NilHint.Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(NilHint.Signature.Parameters[1].Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(NilHint.Signature.Parameters[2].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, NilHint));

  Ops[Ops.size() - 3].Inputs[0] = NdVar::scalar(1, 8);
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, RetainedSelfVoidMethodUsesSwiftContext) {
  Fixture F;
  auto &Method = F.Image.ObjCMethods[0];
  Method.ClassName = "_TtC6Lottie17AnimationViewBase";
  Method.Selector = "layoutSubviews";
  Method.TypeEncoding = "v16@0:8";
  Method.TypeHint->ReturnType = NdType::makeVoid();
  F.Image.Symbols[0].Name =
      "_$s6Lottie17AnimationViewBaseC14layoutSubviewsyyFTo";
  addVoidVirtualMetadata(F);
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 + 0] = 0x00;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 + 1] = 0x01;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4 + 0] = 0xf4;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4 + 1] = 0x03;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4 + 2] = 0x13;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4 + 3] = 0xaa;
  F.Image.Imports.push_back({"/usr/lib/libobjc.A.dylib", "_objc_retain", 0, 0});
  F.Image.ImportStubIndices[0x1300] = 0;

  auto &Ops = F.Function.Blocks[0].Ops;
  Ops.clear();
  const auto X0 = NdVar::reg(a64reg::X0, 8);
  const auto X8 = NdVar::reg(a64reg::X8, 8);
  const auto X9 = NdVar::reg(a64reg::X9, 8);
  const auto X19 = NdVar::reg(a64reg::X19, 8);
  const auto X20 = NdVar::reg(a64reg::X20, 8);
  auto Add = [&](va_t Address, NdOp Code, NdVar Output,
                 std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Addr = Address;
    Op.Opcode = Code;
    Op.Output = Output;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Ops.push_back(Op);
  };
  Add(0x1110, NdOp::COPY, X19, {X0});
  Add(0x1114, NdOp::COPY, X0, {X19});
  Add(0x1118, NdOp::CALL, X0, {NdVar::codeAddress(0x1300, 8)});
  Add(0x111c, NdOp::COPY, X19, {X0});
  Add(0x1120, NdOp::LOAD, X8, {X19});
  Add(0x1124, NdOp::COPY, X9, {NdVar::dataAddress(Fixture::MaskSlot, 8)});
  Add(0x1128, NdOp::LOAD, X9, {X9});
  Add(0x112c, NdOp::LOAD, X9, {X9});
  Add(0x1130, NdOp::INT_AND, X8, {X8, X9});
  Add(0x1138, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
      {X8, NdVar::scalar(96, 8)});
  Add(0x1138, NdOp::LOAD, X8, {NdVar::tmp(TmpBase, 8)});
  Add(0x1168, NdOp::COPY, X20, {X19});
  Add(Fixture::CallSite, NdOp::COPY, NdVar::reg(a64reg::X30, 8),
      {NdVar::scalar(Fixture::CallSite + 4, 8)});
  Add(Fixture::CallSite, NdOp::INDIR_CALL, X0, {X8});

  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_TRUE(Hint.Virtual->DirectSelf);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 96U);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint.Signature.Parameters.size(), 1U);
  EXPECT_EQ(Hint.Signature.Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));

  const auto OriginalImage = F.Image;
  for (unsigned Mutation = 0; Mutation < 28; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto Put32 = [&](unsigned Offset, uint32_t Value) {
      llvm::support::endian::write32le(F.Image.Segments[2].Data.data() + Offset,
                                       Value);
    };
    switch (Mutation) {
    case 0:
      Put32(0, 0x800000d0);
      break; // Generic context.
    case 1:
      Put32(0, 0xa0000050);
      break; // Resilient superclass.
    case 2:
      Put32(0, 0x80010050);
      break; // Extra initialization prefix.
    case 3:
      Put32(44, 13);
      break; // Slot is outside this vtable.
    case 4:
      Put32(48, 0);
      break;
    case 5:
      Put32(52, 0x11);
      break; // Constructor, not an ordinary method.
    case 6:
      Put32(56, uint32_t(0x1404 - (0x4000 + 56)));
      break;
    case 7:
      F.Image.Symbols.back().Name = "_$s6Lottie5OtherC6layoutyyF";
      break;
    case 8:
      // The enclosing ObjC method remains void; its callee takes a Bool.
      F.Image.Symbols.back().Name =
          "_$s6Lottie17AnimationViewBaseC6layoutyySbF";
      break;
    case 9:
      F.Image.Symbols.push_back(F.Image.Symbols.back());
      break;
    case 10:
      F.Image.DataPtrRelocSlots.erase(0x2440);
      break;
    case 11:
      F.Image.ImportPtrSlots[0x2460] = "_external";
      break;
    case 12:
      F.Image.DataPtrRelocSlots.insert(0x2460);
      break;
    case 13:
      F.Image.MachOHasChainedFixups = true;
      break;
    case 14:
      F.Image.ConflictingImportStorageSlots.insert(0x2440);
      break;
    case 15:
      F.Image.Segments[2].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Sections[1].Flags = F.Image.Segments[2].Flags;
      break;
    case 16:
      Put32(48, 512);
      break;
    case 17:
      F.Image.Segments[2].Data[0x80] = 'X';
      break;
    case 18:
      Put32(4, 0xbd);
      break; // Indirect parent context.
    case 19:
      Put32(28, UINT32_MAX);
      break;
    case 20:
      F.Image.ObjCMethods[0].ClassAddress = 0;
      break;
    case 21:
      F.Image.Segments[1].Data[0x580] = 'X';
      break;
    case 22:
      F.Image.Symbols.back().Name = "_$s6Lottie17AnimationViewBaseC6layoutSiyF";
      break;
    case 23:
      F.Image.Symbols.back().Name =
          "_$s6Lottie17AnimationViewBaseC6layoutyyYaF";
      break;
    case 24:
      Put32(52, 0);
      break; // Static method has no swiftself.
    case 25:
      // Same carrier shape as WMF's sizeThatFits(_:apply:) call inside
      // layoutSubviews: two floating arguments, a Bool and a CGSize result.
      F.Image.Symbols.back().Name =
          "_$s6Lottie17AnimationViewBaseC6layout_5applySo6CGSizeVAG_SbtF";
      break;
    case 26:
      Put32(0xc4, 4); // A module context cannot have a parent.
      break;
    case 27:
      Put32(48, 2);
      F.Image.Sections[1].Size = F.Image.Sections[1].FileSz = 60;
      break; // The selected record exists but the declared vtable is truncated.
    }
    EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
    EXPECT_FALSE(isSwiftVirtualSourceCallHint(F.Image, Hint));
    F.Image = OriginalImage;
  }

  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4] = 0x00;
  EXPECT_FALSE(isSwiftVirtualSourceCallHint(F.Image, Hint));
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4] = 0xf4;

  F.Image.DyldBindSlots[Fixture::MaskSlot].Module =
      "/usr/lib/libSystem.B.dylib";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F.Image.DyldBindSlots[Fixture::MaskSlot].Module =
      "/usr/lib/swift/libswiftCore.dylib";
  F.Image.Imports[0].Name = "_objc_release";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F.Image.Imports[0].Name = "_objc_retain";
  Ops[Ops.size() - 3].Inputs[0] = X0;
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  Ops[Ops.size() - 3].Inputs[0] = X19;
  Ops[0].Inputs[0] = NdVar::reg(a64reg::X1, 8);
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  Ops[0].Inputs[0] = X0;
  F.Image.Imports[0].Name = "_objc_retain_x19";
  Ops[1].Inputs[0] = NdVar::reg(a64reg::X1, 8);
  EXPECT_EQ(buildSwiftVirtualCallHints(F.Image, F.Function).size(), 1U);
  F.Image.Imports[0].Name = "_objc_retain";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, RejectsAlteredCodeImportAndReceiverEvidence) {
  Fixture F;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000] = 0x00;
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F = Fixture();
  F.Image.DyldBindSlots[Fixture::MaskSlot].Module =
      "/usr/lib/libSystem.B.dylib";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F = Fixture();
  F.Image.ObjCSourceReferences[Fixture::IvarSlot].ClassName = "Other";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F = Fixture();
  F.Image.ObjCMethods[0].TypeEncoding = "q16@0:8";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}
