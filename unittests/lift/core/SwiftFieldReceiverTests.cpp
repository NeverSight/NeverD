#include "../../../lib/loader/MachO/ImmutableNativeFrame.h"
#include "../../../lib/loader/Swift/SwiftMangledClassMethodABI.h"
#include "../../../lib/sdk/capi/ObjCNativeSwiftReceiverSources.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Swift/SwiftMetadata.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Support/Endian.h"

using namespace neverd;
namespace {
struct FieldFixture {
  static constexpr va_t Entry = 0x1100, Call = Entry + 44, Slot = 0x5200;
  BinaryImage Image;
  llvm::LLVMContext Context;
  PipelineResult Result;
  FieldFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.MachOTwoLevelNamespace = true;
    addSegment(0x1000, SegmentFlags::Readable | SegmentFlags::Executable);
    addSegment(0x3000, SegmentFlags::Readable);
    addSegment(0x5000, SegmentFlags::Readable | SegmentFlags::Writable);
    Image.Sections[0].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    const uint32_t Words[] = {0xd10283ff, 0xa9087bf4, 0x90000028, 0xf9410108,
                              0xf8686a80, 0x6f00e400, 0xad0003e0, 0xad0103e0,
                              0xad0203e0, 0xad0303e0, 0x910003e2, 0x94000075,
                              0xa9487bf4, 0x910283ff, 0xd65f03c0};
    for (unsigned I = 0; I < std::size(Words); ++I)
      u32(Entry + 4 * I, Words[I]);
    const uint32_t Stub[] = {0xd0000001, 0xf9430021, 0xd0000010, 0xf9431210,
                             0xd61f0200};
    for (unsigned I = 0; I < std::size(Stub); ++I)
      u32(0x1300 + I * 4, Stub[I]);
    Image.Symbols.push_back(
        {"_$s4Demo7DerivedC6layoutyyF", Entry, sizeof(Words), true});
    Image.Symbols.push_back(
        {"_$s4Demo7DerivedC5layerSo7CALayerCvpWvd", Slot, 8, false});
    Image.RuntimeFunctionAddrs = {Entry, 0x1300};
    Image.DynInfo.NeededLibs = {
        "/usr/lib/libobjc.A.dylib",
        "/System/Library/Frameworks/Foundation.framework/Foundation",
        "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
        "/System/Library/Frameworks/UIKit.framework/UIKit"};
    Image.ImportPtrSlots[0x3620] = "_objc_msgSend";
    Image.DyldBindSlots[0x3620] = {"_objc_msgSend", 0,
                                   "/usr/lib/libobjc.A.dylib", false};
    text(0x3660, "removeAllAnimations");
    pointer(0x3600, 0x3660);
    Image.ObjCSourceReferences[0x3600] = {ObjCSourceReference::Kind::Selector,
                                          0x3600, 8, "removeAllAnimations"};
    Image.ObjCSourceReferences[Slot] = {ObjCSourceReference::Kind::IvarOffset,
                                        Slot, 8, "layer", "_TtC4Demo7Derived"};
    u32(0x3000, 0x50);
    relative(0x3004, 0x3080);
    relative(0x3008, 0x30c0);
    relative(0x3010, 0x3100);
    relative(0x3014, 0x3188);
    u32(0x3018, 3);
    u32(0x301c, 13);
    u32(0x3020, 1);
    u32(0x3024, 1);
    u32(0x3028, 12);
    relative(0x3088, 0x30b0);
    text(0x30b0, "Demo");
    text(0x30c0, "Derived");
    relative(0x3100, 0x3180);
    relative(0x3104, 0x3188);
    u32(0x3108, (12u << 16) | 7);
    u32(0x310c, 1);
    relative(0x3114, 0x3190);
    relative(0x3118, 0x31a0);
    *bytes(0x3180) = 1;
    relative(0x3181, 0x3000);
    *bytes(0x3188) = 1;
    relative(0x3189, 0x3800);
    text(0x3190, "So7CALayerC");
    text(0x31a0, "layer");
    pointer(0x5008, 0x5100);
    pointer(0x5020, 0x3302);
    u32(0x5038, 128);
    u32(0x503c, 24);
    pointer(0x5040, 0x3000);
    u64(0x5060, 8);
    pointer(0x5120, 0x3382);
    pointer(0x5140, 0x3800);
    pointer(0x3318, 0x3500);
    pointer(0x3330, 0x3400);
    pointer(0x3398, 0x3540);
    u32(0x3304, 8);
    u32(0x3308, 16);
    text(0x3500, "_TtC4Demo7Derived");
    text(0x3540, "_TtC4Demo4Base");
    u32(0x3400, 32);
    u32(0x3404, 1);
    pointer(0x3408, Slot);
    pointer(0x3410, 0x31a0);
    pointer(0x3418, 0x31f0);
    u32(0x3420, 3);
    u32(0x3424, 8);
    u64(Slot, 8);
    ObjCClass C;
    C.Name = "_TtC4Demo7Derived";
    C.Address = 0x5000;
    C.SuperclassAddress = 0x5100;
    C.SuperclassName = "_TtC4Demo4Base";
    C.InheritanceStatus = "resolved";
    C.IvarStatus = "recovered";
    C.InstanceStart = 8;
    C.InstanceSize = 16;
    C.Ivars.push_back({"layer", "", 0x3408, Slot, 8, 8, 8});
    Image.ObjCClasses.push_back(C);
    ObjCClass Base;
    Base.Name = C.SuperclassName;
    Base.Address = 0x5100;
    Base.SuperclassName = "CALayer";
    Base.InheritanceStatus = "resolved";
    Base.IvarStatus = "recovered";
    Image.ObjCClasses.push_back(Base);
  }
  void addSegment(va_t A, SegmentFlags Flags) {
    Segment S;
    S.VA = A;
    S.Size = S.FileSz = 0x1000;
    S.FileOff = Image.Segments.size() * 0x1000;
    S.Flags = Flags;
    S.Data.resize(0x1000);
    Image.Segments.push_back(S);
    Section R;
    R.VA = A;
    R.Size = R.FileSz = 0x1000;
    R.FileOff = S.FileOff;
    R.Flags = Flags;
    Image.Sections.push_back(R);
  }
  uint8_t *bytes(va_t A) {
    for (auto &S : Image.Segments)
      if (A >= S.VA && A < S.VA + S.Size)
        return S.Data.data() + A - S.VA;
    return nullptr;
  }
  void u32(va_t A, uint32_t V) {
    llvm::support::endian::write32le(bytes(A), V);
  }
  void u64(va_t A, uint64_t V) {
    llvm::support::endian::write64le(bytes(A), V);
  }
  void relative(va_t A, va_t B) { u32(A, uint32_t(B - A)); }
  void pointer(va_t A, va_t B) {
    u64(A, B);
    Image.DataPtrRelocSlots.insert(A);
    Image.DataPtrRelocTargetOwners[A] = Image.getSectionFor(B)->VA;
  }
  void text(va_t A, const std::string &S) {
    std::copy(S.c_str(), S.c_str() + S.size() + 1, bytes(A));
  }
  void run() {
    PipelineOptions O;
    O.EmitDumpOutput = false;
    O.OnlyFunctionEntries = {Entry};
    const auto ABI = swiftMangledZeroArgClassMethodSourceABI(Image, Entry);
    ASSERT_TRUE(ABI);
    O.SourceTypeHints.emplace(Entry, *ABI);
    Result = Pipeline().run(Image, Context, O);
    ASSERT_TRUE(Result.Success) << Result.Error;
  }
  HighFunc *high() {
    for (auto &F : Result.HighFuncs)
      if (F.Entry == Entry)
        return &F;
    return nullptr;
  }
  LowFunc *low() {
    for (auto &F : Result.LowFuncs)
      if (F.Entry == Entry)
        return &F;
    return nullptr;
  }
  static ExprPtr call(HighFunc &F) {
    ExprPtr Result;
    walkStmts(F.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        std::vector<ExprPtr> P{E};
        while (!P.empty()) {
          auto V = P.back();
          P.pop_back();
          if (!V)
            continue;
          if (V->SourceCallHint && V->SourceCallHint->NativeSwiftReceiver)
            Result = V;
          V->forEachChildExpr([&](const ExprPtr &C) { P.push_back(C); });
        }
      });
    });
    return Result;
  }
};
} // namespace

TEST(SwiftFieldReceiver, ReflectionAndIvarIdentityRemainSeparateFromLayout) {
  FieldFixture F;
  const auto C = swiftObjCClassIdentity(F.Image, 0x5000);
  ASSERT_TRUE(C);
  EXPECT_EQ(C->Module, "Demo");
  EXPECT_EQ(C->Name, "Derived");
  EXPECT_EQ(swiftObjCStoredFieldClass(F.Image, C->RuntimeName, F.Slot),
            "CALayer");
  EXPECT_TRUE(F.Image.ObjCClasses[0].Ivars[0].TypeEncoding.empty());
  EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, "setTransform:"));
  const auto Root = objcNativeSwiftSelfTypeHint(F.Image, F.Entry);
  ASSERT_TRUE(Root);
  const auto Field = objcReceiverIvarTypeHint(F.Image, *Root, F.Slot);
  ASSERT_TRUE(Field);
  const auto Decl =
      objcReceiverSourceTypeHint(F.Image, "setTransform:", *Field);
  // Reflection establishes the class, not a missing indirect-record ABI.
  EXPECT_TRUE(Decl.HasDeclaration);
  EXPECT_FALSE(Decl.Signature);
  const auto Void =
      objcReceiverSourceTypeHint(F.Image, "removeAllAnimations", *Field);
  ASSERT_TRUE(Void.Signature);
  EXPECT_EQ(Void.Signature->ReturnType->Kind, NdTypeKind::Void);
  auto Other = F.Image;
  Other.Arch = Arch::X64;
  EXPECT_EQ(swiftObjCStoredFieldClass(Other, C->RuntimeName, F.Slot),
            "CALayer");
  EXPECT_FALSE(objcNativeSwiftSelfTypeHint(Other, F.Entry));
  for (const auto Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    Other.Format = Format;
    EXPECT_FALSE(swiftObjCStoredFieldClass(Other, C->RuntimeName, F.Slot));
    EXPECT_FALSE(objcNativeSwiftSelfTypeHint(Other, F.Entry));
  }
  Other = F.Image;
  Other.Arch = Arch::ARM;
  EXPECT_FALSE(swiftObjCStoredFieldClass(Other, C->RuntimeName, F.Slot));
  EXPECT_FALSE(objcNativeSwiftSelfTypeHint(Other, F.Entry));
  auto Literal = *Field;
  Literal.Steps[0].ByteOffset = 8;
  EXPECT_FALSE(objcReceiverTypeHintValid(F.Image, Literal));
  F.u64(F.Slot, 24);
  F.u64(0x5060, 24);
  F.Image.ObjCClasses[0].InstanceStart = 24;
  F.Image.ObjCClasses[0].InstanceSize = 32;
  F.Image.ObjCClasses[0].Ivars[0].Offset = 24;
  F.u32(0x3304, 24);
  F.u32(0x3308, 32);
  EXPECT_EQ(swiftObjCStoredFieldClass(F.Image, C->RuntimeName, F.Slot),
            "CALayer");
}

TEST(SwiftFieldReceiver, MutableObjCInitializersDoNotMakeReflectionMutable) {
  FieldFixture F;
  std::copy(F.bytes(0x3300), F.bytes(0x3300) + 72, F.bytes(0x5300));
  std::copy(F.bytes(0x3400), F.bytes(0x3400) + 40, F.bytes(0x5400));
  F.pointer(0x5020, 0x5302);
  F.pointer(0x5318, 0x3500);
  F.pointer(0x5330, 0x5400);
  F.pointer(0x5408, F.Slot);
  F.pointer(0x5410, 0x31a0);
  F.pointer(0x5418, 0x31f0);
  F.Image.ObjCClasses[0].Ivars[0].MetadataAddress = 0x5408;
  EXPECT_EQ(swiftObjCStoredFieldClass(F.Image, "_TtC4Demo7Derived", F.Slot),
            "CALayer");
  EXPECT_FALSE(readImmutableImageBytes(F.Image, 0x5300, 12));
  EXPECT_TRUE(readInitialImageBytes(F.Image, 0x5300, 12));
  F.Image.DataPtrRelocSlots.insert(0x5424);
  EXPECT_FALSE(swiftObjCStoredFieldClass(F.Image, "_TtC4Demo7Derived", F.Slot));
}

TEST(SwiftFieldReceiver, NativeEntryAndDynamicFieldReachCompleteMessageABI) {
  FieldFixture F;
  F.run();
  ASSERT_NE(F.low(), nullptr);
  ASSERT_NE(F.high(), nullptr);
  auto H = buildObjCSourceCallHints(F.Image, *F.low());
  ASSERT_TRUE(H.count(F.Call));
  ASSERT_TRUE(H.at(F.Call).NativeSwiftReceiver);
  ASSERT_TRUE(H.at(F.Call).Receiver);
  EXPECT_EQ(H.at(F.Call).Receiver->Steps.size(), 1u);
  auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
  const auto Call = F.call(Bound.Function);
  ASSERT_TRUE(Call);
  EXPECT_FALSE(sdk::objcSourceCallBound(*Call, F.Image, {}, nullptr, nullptr,
                                        &Bound.Function));
  EXPECT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
      *Call, F.Image, F.Result, Bound.Function, {}));
}

TEST(SwiftFieldReceiver, ReflectionRejectsContradictoryOrIncompleteOwners) {
  for (unsigned M = 0; M < 60; ++M) {
    SCOPED_TRACE(M);
    FieldFixture F;
    switch (M) {
    case 0:
      F.u32(0x3000, 0xd0);
      break;
    case 1:
      F.u32(0x3000, 0x20000050);
      break;
    case 2:
      F.u32(0x3000, 0x10050);
      break;
    case 3:
      F.u32(0x3004, 0x7d);
      break;
    case 4:
      F.u32(0x3084, 4);
      break;
    case 5:
      F.text(0x30b0, "Fake");
      break;
    case 6:
      F.text(0x30c0, "Renamed");
      break;
    case 7:
      F.u32(0x3108, (12u << 16) | 1);
      break;
    case 8:
      F.u32(0x3108, (16u << 16) | 7);
      break;
    case 9:
      F.u32(0x310c, 2);
      break;
    case 10:
      F.u32(0x3024, 65);
      break;
    case 11:
      F.u32(0x3028, 13);
      break;
    case 12:
      F.relative(0x3181, 0x3800);
      break;
    case 13:
      F.relative(0x3189, 0x3000);
      break;
    case 14:
      F.u32(0x3110, 4);
      break;
    case 15:
      F.text(0x3190, "So7CALayerCSg");
      break;
    case 16:
      F.text(0x31a0, "other");
      break;
    case 17:
      F.Image.Symbols[1].Name = "_$s4Demo7DerivedC5layerSo6UIViewCvpWvd";
      break;
    case 18:
      F.Image.Symbols[1].Name = "_$s4Demo7DerivedC5layerSo7CALayerCvg";
      break;
    case 19:
      F.Image.Symbols[1].Name = "_$s4Demo7DerivedC5layerSo7CALayerCvpWvi";
      break;
    case 20:
      F.Image.Symbols[1].Size = 4;
      break;
    case 21:
      F.Image.Symbols.push_back(F.Image.Symbols[1]);
      break;
    case 22:
      F.Image.ObjCClasses.push_back(F.Image.ObjCClasses[0]);
      break;
    case 23:
      F.Image.ObjCClasses[0].InheritanceStatus = "unresolved";
      break;
    case 24:
      F.Image.ObjCClasses[0].SuperclassAddress += 8;
      break;
    case 25:
      F.Image.ObjCClasses[0].Name = "_TtC4Demo5Other";
      break;
    case 26:
      F.Image.ObjCClasses[0].Ivars[0].MetadataAddress += 8;
      break;
    case 27:
      F.Image.ObjCClasses[0].Ivars[0].TypeEncoding = "@";
      break;
    case 28:
      F.Image.ObjCClasses[0].Ivars[0].Size = 4;
      break;
    case 29:
      F.Image.ObjCClasses[0].Ivars[0].Alignment = 4;
      break;
    case 30:
      F.u32(0x3420, 2);
      break;
    case 31:
      F.u32(0x3424, 4);
      break;
    case 32:
      F.u64(F.Slot, 16);
      break;
    case 33:
      F.u64(0x5060, 16);
      break;
    case 34:
      F.Image.ObjCSourceReferences[F.Slot].Size = 4;
      break;
    case 35:
      F.Image.ObjCSourceReferences[F.Slot].ClassName = "Other";
      break;
    case 36:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 37:
      F.Image.MachOHasChainedFixups = true;
      break;
    case 38:
      F.Image.IsRelocatable = true;
      break;
    case 39:
      F.Image.Bits = Bitness::Bits32;
      break;
    case 40:
      F.Image.Segments[1].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Sections[1].Flags = F.Image.Segments[1].Flags;
      break;
    case 41:
      F.Image.Sections[1].FileSz = 0x110;
      break;
    case 42:
      F.Image.DyldBindSlots[0x5040] = {"_descriptor", 0, "bad", false};
      break;
    case 43:
      F.Image.DataPtrRelocSlots.insert(0x3114);
      break;
    case 44:
      F.u32(0x3400, 40);
      break;
    case 45:
      F.u32(0x3308, 32);
      break;
    case 46:
      *F.bytes(0x3185) = 1;
      break;
    case 47:
      F.u64(0x5020, 0x3301);
      break;
    case 48:
      F.Image.Symbols.push_back(
          {F.Image.Symbols[1].Name, F.Slot + 16, 8, false});
      break;
    case 49:
      F.Image.Symbols.push_back({"overlap", F.Slot - 4, 8, false});
      break;
    case 50:
      F.Image.Symbols.push_back({"interior", F.Slot + 4, 0, false});
      break;
    case 51:
      F.Image.DataPtrRelocSlots.erase(0x3418);
      break;
    case 52:
      F.Image.DataPtrRelocTargetOwners.erase(0x3410);
      break;
    case 53:
      F.Image.DataPtrRelocSlots.insert(0x3409);
      break;
    case 54:
      F.Image.DyldBindSlots[0x3418] = {"_type", 0, "bad", false};
      break;
    case 55:
      F.Image.DataPtrRelocSlots.insert(0x3424);
      break;
    case 56:
      F.Image.Sections.push_back(F.Image.Sections[2]);
      break;
    case 57:
      F.Image.DataPtrRelocTargetOwners[0x3408] = 0x1000;
      break;
    case 58:
      F.Image.DataPtrRelocSlots.erase(0x5020);
      break;
    case 59:
      F.Image.DataPtrRelocSlots.erase(0x5008);
      break;
    }
    EXPECT_FALSE(
        swiftObjCStoredFieldClass(F.Image, "_TtC4Demo7Derived", F.Slot));
  }
}

TEST(SwiftFieldReceiver, NativeReceiverRejectsStaleMachineAndPublication) {
  for (unsigned M = 0; M < 18; ++M) {
    SCOPED_TRACE(M);
    FieldFixture F;
    F.run();
    ASSERT_NE(F.high(), nullptr);
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    auto Call = F.call(Bound.Function);
    ASSERT_TRUE(Call);
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = Hint;
    switch (M) {
    case 0:
      Hint->NativeSwiftReceiver.reset();
      break;
    case 1:
      Hint->Receiver->Address += 4;
      break;
    case 2:
      Hint->Receiver->ClassName = "_TtC4Demo4Base";
      break;
    case 3:
      Hint->Receiver->Steps[0].OffsetSlot += 8;
      break;
    case 4:
      Hint->Receiver->Steps[0].ByteOffset = 8;
      break;
    case 5:
      Call->Operands[0] = HighExpr::makeConst(0, 8);
      break;
    case 6:
      Hint->NativeSwiftReceiver->Instruction += 4;
      break;
    case 7:
      F.u32(F.Entry + 16, 0xf8686a60);
      break; // receiver is x19, not swiftself
    case 8:
      F.Image.Symbols[0].Name = "_$s4Demo7DerivedC6layoutyySbF";
      break;
    case 9:
      Bound.Function.SourceTypeHint->Parameters[0].Location.RegisterOffset = 0;
      break;
    case 10:
      Bound.Function.Params[0].Type = NdType::makeInt(4);
      break;
    case 11:
      Call->Operands[0]->Type = NdType::makeFloat(8);
      break;
    case 12:
      Call->IsIndirectCall = true;
      break;
    case 13:
      Hint->Signature.Parameters[0].Type = NdType::makeInt(4);
      break;
    case 14:
      F.Result.LowFuncs[0].Blocks[0].Ops.front().Addr += 4;
      break;
    case 15:
      F.Result.FunctionAudits[0].MedIRVerified = false;
      break;
    case 16:
      F.u32(0x3108, (12u << 16) | 1);
      break;
    case 17:
      Hint->Receiver->Steps[0].OffsetWidth = 4;
      break;
    }
    EXPECT_FALSE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
  }
}

TEST(SwiftFieldReceiver, PublicationKeepsArgumentsAndUniqueEvaluation) {
  for (unsigned M = 0; M < 11; ++M) {
    SCOPED_TRACE(M);
    FieldFixture F;
    F.text(0x3660, "setMasksToBounds:");
    F.Image.ObjCSourceReferences[0x3600].Name = "setMasksToBounds:";
    F.u32(F.Entry + 40, 0x52800022); // mov w2, #1
    F.run();
    ASSERT_NE(F.high(), nullptr);
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    auto Call = F.call(Bound.Function);
    ASSERT_TRUE(Call);
    ASSERT_EQ(Call->Operands.size(), 3u);
    ASSERT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
    MedOp *Med = nullptr;
    for (auto &B : F.Result.MedFuncs.front().Blocks)
      for (auto &Op : B.Ops)
        if (Op.SourceCallHint && Op.SourceCallHint->NativeSwiftReceiver)
          Med = &Op;
    ASSERT_NE(Med, nullptr);
    switch (M) {
    case 0: {
      auto H = std::make_shared<SourceCallTypeHint>(*Med->SourceCallHint);
      H->NativeSwiftReceiver.reset();
      Med->SourceCallHint = H;
      break;
    }
    case 1:
      Med->Inputs[3].Size = 4;
      break;
    case 2:
      Med->SourceCallHint.reset();
      break;
    case 3:
      Call->Operands[2] = HighExpr::makeConst(0, 1);
      break;
    case 4:
      Call->Operands[2] = HighExpr::makeConst(0, 1);
      F.call(*F.high())->Operands[2] = HighExpr::makeConst(0, 1);
      break;
    case 5: {
      HighStmt S;
      S.Kind = StmtKind::Call;
      S.CallExpr = Call;
      Bound.Function.Body.push_back(S);
      break;
    }
    case 6:
      Bound.Function.DoesNotReturn = true;
      break;
    case 7:
      Call->Operands[1] = HighExpr::makeConst(0, 8);
      break;
    case 8:
      Med->DoesNotReturn = true;
      break;
    case 9:
      Med->PreservesCallerSaved = true;
      break;
    case 10:
      F.Image.Symbols.push_back(F.Image.Symbols[0]);
      break;
    }
    EXPECT_FALSE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
  }
}

TEST(SwiftFieldReceiver, NativeSelfRequiresEveryReachingMachinePath) {
  for (unsigned M = 0; M < 5; ++M) {
    SCOPED_TRACE(M);
    FieldFixture F;
    std::vector<uint32_t> W;
    for (unsigned I = 0; I < 15; ++I)
      W.push_back(llvm::support::endian::read32le(F.bytes(F.Entry + I * 4)));
    W.insert(W.begin() + 2,
             {0xaa1403e9, 0xb4000074, 0xaa0903f4, 0x14000002, 0xaa0903f4});
    W[16] = 0x94000070;
    if (M == 1)
      W[6] = 0xaa0003f4; // another entry argument on one predecessor
    if (M == 2)
      W[6] = 0x2a0903f4; // partial-width self on one predecessor
    for (unsigned I = 0; I < W.size(); ++I)
      F.u32(F.Entry + I * 4, W[I]);
    F.Image.Symbols[0].Size = W.size() * 4;
    F.run();
    ASSERT_NE(F.low(), nullptr);
    auto H = buildObjCSourceCallHints(F.Image, *F.low());
    auto Count = [](const auto &Hints) {
      return llvm::count_if(Hints, [](const auto &V) {
        return bool(V.second.NativeSwiftReceiver);
      });
    };
    if (M == 1 || M == 2) {
      EXPECT_EQ(Count(H), 0);
      continue;
    }
    ASSERT_EQ(Count(H), 1);
    if (M == 3) {
      std::reverse(F.low()->Blocks.begin(), F.low()->Blocks.end());
      auto Reordered = buildObjCSourceCallHints(F.Image, *F.low());
      ASSERT_EQ(Count(Reordered), 1);
      EXPECT_EQ(Reordered.at(F.Entry + 64).Receiver,
                H.at(F.Entry + 64).Receiver);
    } else if (M == 4) {
      F.low()->Blocks.front().Succs.clear();
      EXPECT_EQ(Count(buildObjCSourceCallHints(F.Image, *F.low())), 0);
    } else {
      auto B = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
      auto C = F.call(B.Function);
      ASSERT_TRUE(C);
      EXPECT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
          *C, F.Image, F.Result, B.Function, {}));
    }
  }
}

TEST(SwiftFieldReceiver, OpaqueExitsGrantNoReceiverPublication) {
  FieldFixture F;
  F.run();
  ASSERT_NE(F.high(), nullptr);
  auto B = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
  auto C = F.call(B.Function);
  ASSERT_TRUE(C);
  ASSERT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(*C, F.Image, F.Result,
                                                          B.Function, {}));
  F.u32(F.Entry + 56,
        0xd4200000); // canonical frame replay excludes opaque exits
  F.run();
  ASSERT_NE(F.high(), nullptr);
  ASSERT_TRUE(F.high()->DoesNotReturn);
  const auto Current = buildObjCSourceCallHints(F.Image, *F.low());
  EXPECT_FALSE(llvm::any_of(Current, [](const auto &H) {
    return bool(H.second.NativeSwiftReceiver);
  }));
  EXPECT_FALSE(sdk::objCNativeSwiftReceiverSourceCallBound(
      *C, F.Image, F.Result, B.Function, {}));
}
