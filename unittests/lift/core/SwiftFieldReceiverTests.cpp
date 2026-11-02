#include "../../../lib/loader/MachO/ImmutableNativeFrame.h"
#include "../../../lib/loader/Swift/SwiftMangledClassMethodABI.h"
#include "../../../lib/loader/Swift/SwiftMangledValueConstructorABI.h"
#include "../../../lib/sdk/capi/ObjCBlockSources.h"
#include "../../../lib/sdk/capi/ObjCByValueCopySources.h"
#include "../../../lib/sdk/capi/ObjCNativeSwiftReceiverSources.h"
#include "../../../lib/sdk/capi/SourceSwiftValueConstructorProjection.h"
#include "MappedSectionFlags.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
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
    const auto ABI = swiftMangledReceiverClassMethodSourceABI(Image, Entry);
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

struct ObjCImageCopyFixture : FieldFixture {
  static constexpr va_t CopyCall = Entry + 28;
  ObjCImageCopyFixture() {
    Image.Symbols.clear();
    Image.ObjCClasses.resize(1);
    auto &Owner = Image.ObjCClasses.front();
    Owner.Name = "ImageOwner";
    Owner.SuperclassAddress = 0;
    Owner.SuperclassName = "CIImage";
    Owner.Ivars.clear();
    Image.DynInfo.NeededLibs.push_back(
        "/System/Library/Frameworks/CoreImage.framework/CoreImage");
    text(0x3660, "imageByApplyingTransform:");
    Image.ObjCSourceReferences[0x3600].Name = "imageByApplyingTransform:";
    const uint32_t Words[] = {
        0xd10143ff, 0xa9047bfd, 0x910103fd, // 80-byte frame, save FP/LR.
        0x6d0007e0, 0x6d010fe2, 0x6d0217e4, // Six incoming doubles, 48 bytes.
        0x910003e2, 0x94000079,             // Pass the private copy in X2.
        0xa9447bfd, 0x910143ff, 0xd65f03c0};
    for (unsigned I = 0; I < std::size(Words); ++I)
      u32(Entry + I * 4, Words[I]);
    Image.Symbols.push_back({"image_copy", Entry, sizeof(Words), true});
    ObjCMethod Method;
    Method.ClassName = Owner.Name;
    Method.ClassAddress = Owner.Address;
    Method.MetadataAddress = 0x3800;
    Method.Implementation = Entry;
    Method.Selector = "apply:a:b:c:d:e:";
    Method.TypeEncoding = "@64@0:8d16d24d32d40d48d56";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    Image.ObjCMethods = {Method};
  }
  void run() {
    PipelineOptions O;
    O.EmitDumpOutput = false;
    O.OnlyFunctionEntries = {Entry};
    const auto ABI = objcMethodSourceTypeHint(Image, Entry);
    ASSERT_TRUE(ABI);
    O.SourceTypeHints.emplace(Entry, *ABI);
    Result = Pipeline().run(Image, Context, O);
    ASSERT_TRUE(Result.Success) << Result.Error;
  }
  static ExprPtr copyCall(const HighFunc &F) {
    ExprPtr Result;
    walkStmts(F.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        if (E && E->SourceCallHint && E->SourceCallHint->ByValueCopy)
          Result = E;
      });
    });
    return Result;
  }
};

struct ObjCAffineImageCopyFixture : ObjCImageCopyFixture {
  static constexpr va_t ProducerSlot = 0x3680, RectSlot = 0x36a0;
  bool Concat;
  va_t ProducerCall, ImageCall;
  va_t FirstProducerCall = InvalidVA, RectCall = InvalidVA;
  std::string ProducerName, FunctionName;
  std::vector<uint32_t> Words;
  explicit ObjCAffineImageCopyFixture(bool Concat, bool RectBorrow = false)
      : Concat(Concat) {
    ProducerName =
        Concat ? "CGAffineTransformConcat" : "CGAffineTransformMakeRotation";
    FunctionName = Concat ? "affine_concat" : "affine_rotation";
    const auto Provider =
        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
    Image.DynInfo.NeededLibs.push_back(Provider);
    Image.ImportPtrSlots[ProducerSlot] = "_" + ProducerName;
    Image.DyldBindSlots[ProducerSlot] = {"_" + ProducerName, 0, Provider,
                                         false};
    const uint32_t Stub[] = {0xd0000010, 0xf9434210, 0xd61f0200};
    for (unsigned I = 0; I < std::size(Stub); ++I)
      u32(0x1400 + I * 4, Stub[I]);
    Image.RuntimeFunctionAddrs.insert(0x1400);
    if (RectBorrow) {
      EXPECT_FALSE(Concat);
      FunctionName = "affine_rect_rotation";
      Image.ImportPtrSlots[RectSlot] = "_CGRectApplyAffineTransform";
      Image.DyldBindSlots[RectSlot] = {"_CGRectApplyAffineTransform", 0,
                                       Provider, false};
      const uint32_t RectStub[] = {0xd0000010, 0xf9435210, 0xd61f0200};
      for (unsigned I = 0; I < std::size(RectStub); ++I)
        u32(0x1450 + I * 4, RectStub[I]);
      Image.RuntimeFunctionAddrs.insert(0x1450);
      // CGRect consumes the first complete result through x0 and returns
      // d0-d3. The second rotation must reinitialize the permitted input
      // writes before CoreImage consumes it. Saved d8 retains the angle.
      Words = {0xd101c3ff, 0xa9067bfd, 0x910183fd, 0xa90453f3, 0xfd001be8,
               0xaa0003f3, 0x1e604008, 0x910003e8, 0x940000b8, 0x910003e0,
               0x6f00e400, 0x6f00e401, 0x1e6e1002, 0x1e6e1003, 0x940000c6,
               0x910003e8, 0x1e604100, 0x940000af, 0xaa1303e0, 0x910003e2,
               0x9400006c, 0xfd401be8, 0xa94453f3, 0xa9467bfd, 0x9101c3ff,
               0xd65f03c0};
      FirstProducerCall = Entry + 32;
      RectCall = Entry + 56;
      ProducerCall = Entry + 68;
      ImageCall = Entry + 80;
    } else if (Concat) {
      // Two initialized 48-byte inputs, a distinct 48-byte result, then
      // saved x19/x20 and FP/LR in the final 32 bytes of the 176-byte frame.
      Words = {0xd102c3ff, 0xa90a7bfd, 0x910283fd, 0xa90953f3, 0xaa0003f3,
               0x6d400440, 0x6d410c42, 0x6d421444, 0x6d0007e0, 0x6d010fe2,
               0x6d0217e4, 0x6d400460, 0x6d410c62, 0x6d421464, 0x6d0307e0,
               0x6d040fe2, 0x6d0517e4, 0x910003e0, 0x9100c3e1, 0x910183e8,
               0x940000ac, 0xaa1303e0, 0x910183e2, 0x94000069, 0xa94953f3,
               0xa94a7bfd, 0x9102c3ff, 0xd65f03c0};
      ProducerCall = Entry + 80;
      ImageCall = Entry + 92;
    } else {
      // The SDK writes the complete 48-byte result through x8; no prior
      // store can authenticate the subsequent CoreImage copy instead.
      Words = {0xd10143ff, 0xa9047bfd, 0x910103fd, 0xa90353f3, 0xaa0003f3,
               0x910003e8, 0x940000ba, 0xaa1303e0, 0x910003e2, 0x94000077,
               0xa94353f3, 0xa9447bfd, 0x910143ff, 0xd65f03c0};
      ProducerCall = Entry + 24;
      ImageCall = Entry + 36;
    }
    for (unsigned I = 0; I < Words.size(); ++I)
      u32(Entry + I * 4, Words[I]);
    Image.Symbols = {{FunctionName, Entry, Words.size() * 4, true}};
    auto &Method = Image.ObjCMethods.front();
    Method.Selector = Concat ? "concat:with:" : "rotate:";
    Method.TypeEncoding = Concat ? "@32@0:8^v16^v24" : "@24@0:8d16";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  }
};

struct ObjCFrameBlockFixture : ObjCImageCopyFixture {
  static constexpr va_t BlockCall = Entry + 68;
  static constexpr va_t IsaSlot = 0x36c0, Descriptor = 0x3b20;
  static constexpr va_t Signature = 0x3b60;
  std::vector<uint32_t> Words{
      0xd10103ff, 0xa9037bfd, 0xfd0013e0, 0xd0000008, 0xf9436108, 0xf90003e8,
      0xd0000008, 0xfd458100, 0xfd0007e0, 0x90000008, 0x91140108, 0xd0000009,
      0x912c8129, 0xa90127e8, 0x52800028, 0x3900a3e8, 0x910003e2, 0x9400006f,
      0xa9437bfd, 0x910103ff, 0xd65f03c0};
  ObjCFrameBlockFixture() {
    Image.ObjCClasses.front().SuperclassName = "UIGraphicsImageRenderer";
    text(0x3660, "imageWithActions:");
    Image.ObjCSourceReferences[0x3600].Name = "imageWithActions:";
    Image.ImportPtrSlots[IsaSlot] = "__NSConcreteStackBlock";
    Image.DyldBindSlots[IsaSlot] = {"__NSConcreteStackBlock", 0,
                                    "/usr/lib/libSystem.B.dylib", false};
    u64(0x3b00, 0xc0000000); // Signature and extended nonobject layout.
    u64(Descriptor, 0);
    u64(Descriptor + 8, 48);
    pointer(Descriptor + 16, Signature);
    u64(Descriptor + 24, 0);
    text(Signature, "v16@?0@8");
    u32(0x1500, 0xd65f03c0);
    Image.RuntimeFunctionAddrs.insert(0x1500);
    Image.Symbols = {{"frame_block", Entry, Words.size() * 4, true},
                     {"frame_block_invoke", 0x1500, 4, true}};
    auto &Method = Image.ObjCMethods.front();
    Method.Selector = "render:";
    Method.TypeEncoding = "@24@0:8d16";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    for (unsigned I = 0; I < Words.size(); ++I)
      u32(Entry + I * 4, Words[I]);
  }
  SourceCallOccurrenceKey site() const {
    for (const auto &Function : Result.LowFuncs)
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Addr == BlockCall && Op.Opcode == NdOp::CALL)
            return *sourceCallOccurrenceKey(Op);
    throw std::runtime_error("Missing original block call");
  }
};

struct ObjCFrameBlockCopyFixture : ObjCFrameBlockFixture {
  static constexpr va_t CopySelector = 0x3640, ProducerSlot = 0x3680;
  static constexpr va_t CopyTarget = 0x1380, ProducerTarget = 0x1400;
  va_t ImageCopyCall;
  explicit ObjCFrameBlockCopyFixture(bool WithConstant = false) {
    text(0x3a00, "imageByApplyingTransform:");
    pointer(CopySelector, 0x3a00);
    Image.ObjCSourceReferences[CopySelector] = {
        ObjCSourceReference::Kind::Selector, CopySelector, 8,
        "imageByApplyingTransform:"};
    const uint32_t CopyStub[] = {0xd0000001, 0xf9432021, 0xd0000010, 0xf9431210,
                                 0xd61f0200};
    for (unsigned I = 0; I != std::size(CopyStub); ++I)
      u32(CopyTarget + 4 * I, CopyStub[I]);
    const std::string Provider =
        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
    Image.DynInfo.NeededLibs.push_back(Provider);
    Image.ImportPtrSlots[ProducerSlot] = "_CGAffineTransformMakeRotation";
    Image.DyldBindSlots[ProducerSlot] = {"_CGAffineTransformMakeRotation", 0,
                                         Provider, false};
    const uint32_t ProducerStub[] = {0xd0000010, 0xf9434210, 0xd61f0200};
    for (unsigned I = 0; I != std::size(ProducerStub); ++I)
      u32(ProducerTarget + 4 * I, ProducerStub[I]);
    Image.RuntimeFunctionAddrs.insert(CopyTarget);
    Image.RuntimeFunctionAddrs.insert(ProducerTarget);
    // The live 48-byte literal and the SDK-initialized 48-byte affine copy
    // occupy separate ranges. Neither includes the saved registers.
    Words = {0xd10203ff, 0xa9077bfd, 0x9101c3fd, 0xa90653f3, 0xaa0003f3,
             0xaa0203f4, 0xfd0013e0, 0xd0000008, 0xf9436108, 0xf90003e8,
             0xd0000008, 0xfd458100, 0xfd0007e0, 0x90000008, 0x91140108,
             0xd0000009, 0x912c8129, 0xa90127e8, 0x52800028, 0x3900a3e8,
             0x9100c3e8, 0xfd4013e0, 0,          0xaa1403e0, 0x9100c3e2,
             0,          0xaa1303e0, 0x910003e2, 0,          0xa94653f3,
             0xa9477bfd, 0x910203ff, 0xd65f03c0};
    for (auto [Index, Target] : {std::pair{22u, ProducerTarget},
                                 {25u, CopyTarget},
                                 {28u, va_t{0x1300}}})
      Words[Index] = 0x94000000 | uint32_t((Target - (Entry + 4 * Index)) / 4);
    ImageCopyCall = Entry + 25 * 4;
    Image.Symbols.front() = {"frame_block_copy", Entry, Words.size() * 4, true};
    auto &Method = Image.ObjCMethods.front();
    Method.Selector = "renderImage:angle:";
    Method.TypeEncoding = "@32@0:8@\"CIImage\"16d24";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    if (WithConstant) {
      addSegment(0x7000, SegmentFlags::Readable);
      Image.Segments.back().Name = "__DATA_CONST";
      auto &Record = Image.Sections.back();
      Record.Name = "__cfstring";
      Record.SegmentName = "__DATA_CONST";
      Record.Size = Record.FileSz = 32;
      Section Characters = Record;
      Characters.VA = 0x7100;
      Characters.FileOff += 0x100;
      Characters.Size = Characters.FileSz = 16;
      Characters.Name = "__cstring";
      Characters.Type = llvm::MachO::S_CSTRING_LITERALS;
      Image.Sections.push_back(Characters);
      const std::string Foundation =
          "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
      Image.DynInfo.NeededLibs.push_back(Foundation);
      Image.recordDyldBindSlot(0x7000, "___CFConstantStringClassReference", 0,
                               Foundation, false);
      u64(0x7008, 0x7c8);
      pointer(0x7010, 0x7100);
      u64(0x7018, 4);
      text(0x7100, "copy");
      Image.ImportPtrSlots[0x36e0] = "_objc_release";
      Image.DyldBindSlots[0x36e0] = {"_objc_release", 0,
                                     "/usr/lib/libobjc.A.dylib", false};
      const uint32_t ReleaseStub[] = {0xd0000010, 0xf9437210, 0xd61f0200};
      for (unsigned I = 0; I != std::size(ReleaseStub); ++I)
        u32(0x1580 + 4 * I, ReleaseStub[I]);
      Image.RuntimeFunctionAddrs.insert(0x1580);
      // Preserve the image result while the runtime consumes a separately
      // authenticated constant object. Its source binding is not block data.
      Words.insert(Words.begin() + 29,
                   {0xaa0003f4, 0xd0000020, 0x91000000,
                    0x94000000 | uint32_t((0x1580 - (Entry + 32 * 4)) / 4),
                    0xaa1403e0});
      Image.Symbols.front().Size = Words.size() * 4;
    }
    for (unsigned I = 0; I != Words.size(); ++I)
      u32(Entry + I * 4, Words[I]);
  }
  void runWithInvoke() {
    run();
    const auto Plan = sdk::discoverObjCBlockSources(Image, Result);
    PipelineOptions O;
    O.EmitDumpOutput = false;
    O.OnlyFunctionEntries = {Entry, 0x1500};
    O.SourceTypeHints.emplace(Entry, *objcMethodSourceTypeHint(Image, Entry));
    sdk::applyObjCBlockInvokeHints(Plan, O);
    Result = Pipeline().run(Image, Context, O);
    ASSERT_TRUE(Result.Success) << Result.Error;
  }
  std::map<va_t, const HighFunc *> functions() const {
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &F : Result.HighFuncs)
      Functions.emplace(F.Entry, &F);
    return Functions;
  }
};

TEST(ObjCFrameBlockBorrows, CurrentCopyCanBorrowDisjointLiveLiteralStorage) {
  ObjCFrameBlockCopyFixture F;
  F.run();
  const auto Copy = ObjCImageCopyFixture::copyCall(*F.high());
  ASSERT_TRUE(Copy);
  ASSERT_TRUE(sdk::objCByValueCopyFrameCallBound(*Copy, F.Image, F.Result,
                                                 *F.high(), {}));
  const auto Plan = sdk::discoverObjCBlockSources(F.Image, F.Result);
  ASSERT_EQ(Plan.StackBlocks.count(F.Entry), 1u)
      << (Plan.Rejections.count(F.Entry) ? Plan.Rejections.at(F.Entry) : "");
  EXPECT_EQ(Plan.StackBlocks.at(F.Entry).front().FrameOffset, -128);
}

TEST(ObjCFrameBlockBorrows, PublicationRepeatsBothOwnersInProjectionOrder) {
  ObjCFrameBlockCopyFixture F;
  F.runWithInvoke();
  const sdk::ObjCBlockSourceContext Source(F.Image);
  const auto Plan = sdk::discoverObjCBlockSources(Source, F.Result);
  const auto Functions = F.functions();
  const auto Blocks =
      sdk::bindObjCBlockSourceReferences(*F.high(), Source, Plan, Functions);
  ASSERT_TRUE(Blocks.Limitation.empty()) << Blocks.Limitation;
  auto Bound = sdk::bindObjCSourceReferences(Blocks.Function, F.Image, nullptr,
                                             &Functions);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  const auto Copy = ObjCImageCopyFixture::copyCall(Bound.Function);
  ASSERT_TRUE(Copy);
  sdk::ObjCBlockByValueCopySourceValidator Validator(Source, F.Result, Plan);
  EXPECT_TRUE(Validator.validate(*Copy, Bound.Function, Functions));
}

TEST(ObjCFrameBlockBorrows, ConstantsKeepTheirIndependentPublicationOwner) {
  for (bool Changed : {false, true}) {
    ObjCFrameBlockCopyFixture F(true);
    F.runWithInvoke();
    const sdk::ObjCBlockSourceContext Source(F.Image);
    const auto Plan = sdk::discoverObjCBlockSources(Source, F.Result);
    const auto Functions = F.functions();
    const auto Blocks =
        sdk::bindObjCBlockSourceReferences(*F.high(), Source, Plan, Functions);
    ASSERT_TRUE(Blocks.Limitation.empty()) << Blocks.Limitation;
    auto Bound = sdk::bindObjCSourceReferences(Blocks.Function, F.Image,
                                               nullptr, &Functions);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    ASSERT_EQ(Bound.ConstantStrings, std::set<va_t>{0x7000});
    const auto Copy = ObjCImageCopyFixture::copyCall(Bound.Function);
    ASSERT_TRUE(Copy);
    if (Changed)
      F.Image.DyldBindSlots.at(0x7000).WeakImport = true;
    sdk::ObjCBlockByValueCopySourceValidator Validator(Source, F.Result, Plan);
    EXPECT_EQ(Validator.validate(*Copy, Bound.Function, Functions), !Changed);
  }
}

TEST(ObjCFrameBlockBorrows, PublicationRejectsChangedBlockCopyAndCallback) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ObjCFrameBlockCopyFixture F;
    F.runWithInvoke();
    const sdk::ObjCBlockSourceContext Source(F.Image);
    auto Plan = sdk::discoverObjCBlockSources(Source, F.Result);
    const auto Functions = F.functions();
    const auto Blocks =
        sdk::bindObjCBlockSourceReferences(*F.high(), Source, Plan, Functions);
    ASSERT_TRUE(Blocks.Limitation.empty()) << Blocks.Limitation;
    auto Bound = sdk::bindObjCSourceReferences(Blocks.Function, F.Image,
                                               nullptr, &Functions);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    auto Copy = ObjCImageCopyFixture::copyCall(Bound.Function);
    ASSERT_TRUE(Copy);
    if (Mutation == 0)
      Plan.SourceResult = nullptr;
    else if (Mutation == 1)
      F.u32(F.ImageCopyCall, 0xd63f0100);
    else if (Mutation == 2)
      F.u64(F.Descriptor + 8, 64);
    else if (Mutation == 3)
      F.text(F.Signature, "i16@?0@8");
    else if (Mutation == 4) {
      bool Changed = false;
      walkStmts(Bound.Function.Body, [&](HighStmt &S) {
        forEachExpr(S, [&](ExprPtr &E) {
          if (E && E->SourceCallHint &&
              E->SourceCallHint->CallKind ==
                  SourceCallTypeHint::Kind::NativeAddress) {
            auto Hint =
                std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
            Hint->TargetAddress += 4;
            E->SourceCallHint = Hint;
            Changed = true;
          }
        });
      });
      ASSERT_TRUE(Changed);
    } else if (Mutation == 5) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Copy->SourceCallHint);
      Hint->ByValueCopy->FrameOffset += 8;
      Copy->SourceCallHint = Hint;
    } else if (Mutation == 6) {
      for (auto &Function : F.Result.HighFuncs)
        if (Function.Entry == 0x1500) {
          MedVar P;
          P.Kind = MedVar::Param;
          P.Id = 0;
          P.Size = 8;
          Function.Body.front().RetVal =
              HighExpr::makeVar(P, Function.Params.front().Type);
        }
    } else
      Copy = std::make_shared<HighExpr>(*Copy);
    sdk::ObjCBlockByValueCopySourceValidator Validator(Source, F.Result, Plan);
    EXPECT_FALSE(Validator.validate(*Copy, Bound.Function, Functions));
  }
}

TEST(ObjCImageValueCopy, FrameBoundaryRepeatsCurrentBodyAndOccurrenceProof) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ObjCImageCopyFixture F;
    F.run();
    auto Copy = ObjCImageCopyFixture::copyCall(*F.high());
    ASSERT_TRUE(Copy);
    ASSERT_TRUE(sdk::objCByValueCopyFrameCallBound(*Copy, F.Image, F.Result,
                                                   *F.high(), {}));
    if (Mutation == 0)
      F.u32(F.CopyCall, 0xd63f0100);
    else if (Mutation == 1) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Copy->SourceCallHint);
      ++Hint->ByValueCopy->Site.Sequence;
      Copy->SourceCallHint = Hint;
    } else if (Mutation == 2)
      Copy->Operands[2] = HighExpr::makeConst(0, 8);
    else if (Mutation == 3)
      F.high()->FrameSize += 16;
    else if (Mutation == 4)
      F.Result.SourceImage = nullptr;
    else if (Mutation == 5)
      F.Image.ObjCMethods.clear();
    else if (Mutation == 6)
      F.Image.DynInfo.NeededLibs.pop_back();
    else if (Mutation == 7)
      Copy = std::make_shared<HighExpr>(*Copy);
    else
      F.Result.MedFuncs.clear();
    EXPECT_FALSE(sdk::objCByValueCopyFrameCallBound(*Copy, F.Image, F.Result,
                                                    *F.high(), {}));
  }
}

TEST(ObjCFrameBlockBorrows, ReplaysInitializedHeaderWithoutInventingPadding) {
  ObjCFrameBlockFixture F;
  F.run();
  ASSERT_TRUE(F.low());
  const auto Hints = buildObjCSourceCallHints(F.Image, *F.low());
  ASSERT_TRUE(Hints.count(F.BlockCall));
  const auto &Binding = Hints.at(F.BlockCall);
  ASSERT_TRUE(Binding.Receiver);
  const auto Effect = objcNonEscapingBlockSourceFrameEffects(F.Image, *F.low(),
                                                             F.site(), Binding);
  ASSERT_TRUE(Effect);
  EXPECT_EQ(Effect->WritableFrameParameters,
            (std::map<size_t, size_t>{{2, 48}}));
  EXPECT_TRUE(Effect->ReadOnlyFrameParameters.empty());
  EXPECT_TRUE(Effect->InitializedFrameParameters.empty());
  EXPECT_FALSE(Effect->InitializesIndirectResult);
  NativeSourceCallContract Contract;
  Contract.Signature = &Binding.Signature;
  const NativeSourceCalls Calls{{F.site(), Contract}};
  const auto Storage = sourceFrameCallArgumentStorage(*F.low(), Arch::AArch64,
                                                      Calls, F.site(), 2, 48);
  ASSERT_TRUE(Storage);
  EXPECT_EQ(Storage->FrameOffset, -64);
  for (unsigned I = 0; I < 41; ++I)
    EXPECT_TRUE(Storage->Bytes[I].Initialized) << I;
  for (unsigned I = 41; I < 48; ++I)
    EXPECT_FALSE(Storage->Bytes[I].Initialized) << I;
}

TEST(ObjCFrameBlockBorrows, RejectsChangedImportsHeadersABIAndMachine) {
  for (unsigned Mutation = 0; Mutation < 19; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ObjCFrameBlockFixture F;
    F.run();
    ASSERT_TRUE(F.low());
    const auto Hints = buildObjCSourceCallHints(F.Image, *F.low());
    ASSERT_TRUE(Hints.count(F.BlockCall));
    auto Binding = Hints.at(F.BlockCall);
    const auto Site = F.site();
    ASSERT_TRUE(objcNonEscapingBlockSourceFrameEffects(F.Image, *F.low(), Site,
                                                       Binding));
    switch (Mutation) {
    case 0:
      F.Image.DyldBindSlots.at(F.IsaSlot).WeakImport = true;
      break;
    case 1:
      F.Image.DyldBindSlots.at(F.IsaSlot).Module = "/tmp/blocks";
      break;
    case 2:
      F.Image.DyldBindSlots.at(F.IsaSlot).Name = "__NSConcreteGlobalBlock";
      break;
    case 3:
      F.u64(0x3b00, 0xc4000000);
      break;
    case 4:
      F.u64(F.Descriptor + 8, 80);
      break;
    case 5:
      F.u32(F.Entry + 40, 0x91141108);
      F.run();
      break;
    case 6:
      F.u32(F.Entry + 32, 0xbd0007e0);
      F.run();
      break;
    case 7:
      F.u32(F.Entry + 56, 0x910003e8);
      F.u32(F.Entry + 60, 0xf90017e8);
      F.run();
      break;
    case 8:
      F.text(F.Signature, "i16@?0@8");
      break;
    case 9:
      Binding.Signature.Parameters[2].Location.ValueBytes = 4;
      break;
    case 10:
      F.u32(F.BlockCall, 0xd63f0100);
      break;
    case 11:
      F.u32(F.Entry + 64, 0x9100c3e2);
      F.run();
      break;
    case 12:
      F.Image.ImportPtrSlots[F.IsaSlot] = "__NSConcreteGlobalBlock";
      F.Image.DyldBindSlots.at(F.IsaSlot).Name = "__NSConcreteGlobalBlock";
      break;
    case 13:
      F.u64(F.Descriptor + 24, 0x200);
      break;
    case 14:
      Binding.Receiver.reset();
      break;
    case 15:
      Binding.SelectorReferenceAddress += 8;
      break;
    case 16:
      // A scalar equal to a preferred code address is not a relocated PC
      // recipe for that function in a linked image.
      F.u32(F.Entry + 40, 0xd282a008); // mov x8, #0x1500
      F.run();
      break;
    case 17:
      Binding.TargetName = "unknown_dispatch";
      break;
    case 18:
      Binding.OwnerClass = "unknown_owner";
      break;
    }
    EXPECT_FALSE(objcNonEscapingBlockSourceFrameEffects(F.Image, *F.low(), Site,
                                                        Binding));
  }
}

TEST(ObjCAffineImageValueCopy, CGRectInputUsesTheSameCurrentFrameOwner) {
  ObjCAffineImageCopyFixture F(false, true);
  F.run();
  const auto Hints = buildObjCSourceCallHints(F.Image, *F.low());
  ASSERT_TRUE(Hints.count(F.RectCall));
  const auto Effects =
      darwinMatrixSourceFrameEffects(F.Image, Hints.at(F.RectCall));
  ASSERT_TRUE(Effects);
  EXPECT_FALSE(Effects->InitializesIndirectResult);
  EXPECT_EQ(Effects->WritableFrameParameters,
            (std::map<size_t, size_t>{{1, 48}}));
  EXPECT_EQ(Effects->InitializedFrameParameters, (std::set<size_t>{1}));
  EXPECT_TRUE(Effects->ReadOnlyFrameParameters.empty());
  ASSERT_TRUE(Hints.count(F.ImageCall));
  ASSERT_TRUE(Hints.at(F.ImageCall).ByValueCopy);
  EXPECT_EQ(Hints.at(F.ImageCall).ByValueCopy->FrameOffset, -112);
  const auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
  const auto Call = F.copyCall(Bound.Function);
  ASSERT_TRUE(Call);
  EXPECT_TRUE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                  Bound.Function, {}));
}

TEST(ObjCAffineImageValueCopy, CGRectBorrowRejectsExpiredInputsAndChangedABI) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ObjCAffineImageCopyFixture F(false, true);
    F.run();
    const auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    const auto Call = F.copyCall(Bound.Function);
    ASSERT_TRUE(Call);
    ASSERT_TRUE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                    Bound.Function, {}));
    switch (Mutation) {
    case 0:
      F.Image.DyldBindSlots.at(F.RectSlot).WeakImport = true;
      break;
    case 1:
      F.Image.DyldBindSlots.at(F.RectSlot).Module =
          "/System/Library/Frameworks/UIKit.framework/UIKit";
      break;
    case 2:
      F.Image.ConflictingImportStorageSlots.insert(F.RectSlot);
      break;
    case 3:
      F.u32(F.ProducerCall, 0xd503201f); // Consumed bytes need a new producer.
      break;
    case 4:
      F.u32(F.FirstProducerCall,
            0xd503201f); // Borrow itself needs initialized input.
      break;
    case 5:
      F.u32(F.Entry + 36, 0x910143e0); // Input extends outside the live frame.
      break;
    case 6:
    case 7:
      for (auto &M : F.Result.MedFuncs)
        for (auto &B : M.Blocks)
          for (auto &Op : B.Ops)
            if (Op.Opcode == NdOp::CALL && Op.Addr == F.RectCall) {
              ASSERT_TRUE(Op.SourceCallHint);
              auto H = std::make_shared<SourceCallTypeHint>(*Op.SourceCallHint);
              if (Mutation == 6)
                H->Signature.Parameters[1].Location.RegisterOffset = a64reg::X1;
              else
                H->Signature.ReturnComponents[0].Kind =
                    SourceABICarrierKind::IntegerRegister;
              Op.SourceCallHint = H;
            }
      break;
    }
    EXPECT_FALSE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                     Bound.Function, {}));
    if (Mutation < 6) {
      F.run();
      const auto Hints = buildObjCSourceCallHints(F.Image, *F.low());
      EXPECT_FALSE(Hints.count(F.ImageCall));
    }
  }
}

TEST(ObjCAffineImageValueCopy, CurrentProducerInitializesThePublishedCopy) {
  for (bool Concat : {false, true}) {
    SCOPED_TRACE(Concat);
    ObjCAffineImageCopyFixture F(Concat);
    F.run();
    ASSERT_TRUE(F.low());
    const auto Hints = buildObjCSourceCallHints(F.Image, *F.low());
    ASSERT_TRUE(Hints.count(F.ProducerCall));
    const auto Effects =
        darwinMatrixSourceFrameEffects(F.Image, Hints.at(F.ProducerCall));
    ASSERT_TRUE(Effects);
    EXPECT_TRUE(Effects->InitializesIndirectResult);
    EXPECT_EQ(Effects->WritableFrameParameters.size(), Concat ? 2U : 0U);
    ASSERT_TRUE(Hints.count(F.ImageCall));
    ASSERT_TRUE(Hints.at(F.ImageCall).ByValueCopy);
    EXPECT_EQ(Hints.at(F.ImageCall).ByValueCopy->Bytes, 48U);
    EXPECT_EQ(Hints.at(F.ImageCall).ByValueCopy->FrameOffset, -80);
    const auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    const auto Call = F.copyCall(Bound.Function);
    ASSERT_TRUE(Call);
    EXPECT_TRUE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                    Bound.Function, {}));
  }
}

TEST(ObjCAffineImageValueCopy, RejectsWrongProducerFrameAndSavedIR) {
  for (bool Concat : {false, true}) {
    SCOPED_TRACE(Concat);
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(Mutation);
      ObjCAffineImageCopyFixture F(Concat);
      F.run();
      auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
      auto Call = F.copyCall(Bound.Function);
      ASSERT_TRUE(Call);
      ASSERT_TRUE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                      Bound.Function, {}));
      switch (Mutation) {
      case 0:
        F.Image.DyldBindSlots.at(F.ProducerSlot).WeakImport = true;
        break;
      case 1:
        F.Image.DyldBindSlots.at(F.ProducerSlot).Module =
            "/System/Library/Frameworks/QuartzCore.framework/QuartzCore";
        break;
      case 2:
        F.Image.DynInfo.NeededLibs.pop_back();
        break;
      case 3:
        F.Image.ConflictingImportStorageSlots.insert(F.ProducerSlot);
        break;
      case 4:
        // The result would extend outside the frame and over saved registers.
        F.u32(F.ProducerCall - 4, Concat ? 0x910283e8 : 0x910103e8);
        break;
      case 5:
        if (Concat)
          F.u32(F.Entry + 40,
                0xd503201f); // Sixteen input bytes remain unwritten.
        else
          F.u32(F.ProducerCall, 0xd503201f); // No result producer remains.
        break;
      case 6:
        if (Concat)
          F.u32(F.Entry + 64,
                0xd503201f); // Second input also needs all 48 bytes.
        else
          F.u32(F.ProducerCall - 4, 0x910003e7); // Wrong hidden carrier.
        break;
      case 7:
        if (Concat)
          F.u32(F.ImageCall - 4, 0x910003e2); // Consumed input is invalidated.
        else
          F.u32(F.ImageCall - 4,
                0x910023e2); // Partial/out-of-range result copy.
        break;
      case 8:
        F.Image.IsRelocatable = true;
        break;
      case 9:
      case 10:
      case 11:
        for (auto &M : F.Result.MedFuncs)
          for (auto &B : M.Blocks)
            for (auto &Op : B.Ops)
              if (Op.Opcode == NdOp::CALL && Op.Addr == F.ProducerCall) {
                ASSERT_TRUE(Op.SourceCallHint);
                auto Hint =
                    std::make_shared<SourceCallTypeHint>(*Op.SourceCallHint);
                if (Mutation == 9)
                  ++Hint->ByteCount;
                if (Mutation == 10)
                  Hint->Signature.ReturnLocation.RegisterOffset = a64reg::X0;
                if (Mutation == 11)
                  Hint->Signature.ReturnType = NdType::makeStruct(
                      std::vector<TypeRef>(5, NdType::makeFloat(8)));
                Op.SourceCallHint = Hint;
              }
        break;
      }
      EXPECT_FALSE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                       Bound.Function, {}));
      if (Mutation < 9) {
        // Rebuild after an image edit, rather than rejecting only stale saved
        // IR. The changed current machine must not acquire a copy certificate.
        F.run();
        const auto Hints = buildObjCSourceCallHints(F.Image, *F.low());
        EXPECT_FALSE(Hints.count(F.ImageCall));
      }
    }
  }
}

TEST(ObjCAffineImageValueCopy, GeneratedCMatchesOriginalMachineAndSDKResults) {
#if !defined(__APPLE__) || !defined(__aarch64__)
  GTEST_SKIP() << "Native CoreGraphics SDK execution requires Apple arm64";
#else
  for (unsigned Variant = 0; Variant < 3; ++Variant) {
    SCOPED_TRACE(Variant);
    const bool Concat = Variant == 1;
    ObjCAffineImageCopyFixture F(Concat, Variant == 2);
    F.run();
    const auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    const auto Call = F.copyCall(Bound.Function);
    ASSERT_TRUE(Call);
    ASSERT_TRUE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                    Bound.Function, {}));
    std::string Program;
    llvm::raw_string_ostream OS(Program);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.EmitComments = false;
    ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
    // Independently named SDK declarations retain the observed six-double
    // logical ABI. The original body below executes the exact fixture words,
    // replacing only BL relocations with their linked platform callees.
    OS << R"(
struct OracleTransform { double a,b,c,d,tx,ty; };
extern struct OracleTransform sdk_rotation(double)
  __asm__("_CGAffineTransformMakeRotation");
extern struct OracleTransform sdk_concat(struct OracleTransform,
                                         struct OracleTransform)
  __asm__("_CGAffineTransformConcat");
struct CopyState { unsigned calls; uint64_t bits[6]; uint64_t result; };
static unsigned selectors;
static uint64_t selector_token;
static SEL volatile raw_selector;
SEL sel_registerName(const char *name) {
  if(__builtin_strcmp(name,"imageByApplyingTransform:"))__builtin_trap();
  ++selectors;return (SEL)&selector_token;
}
void *copy_oracle(void *,void *,unsigned char *) __asm__("_objc_msgSend");
void *copy_oracle(void *receiver,void *selector,unsigned char *copy) {
  struct CopyState *s=receiver;
  if(selector!=&selector_token||!copy)__builtin_trap();
  ++s->calls;__builtin_memcpy(s->bits,copy,48);
  __builtin_memset(copy,0xa5,48);s->result=0x93ace0142785bf62ULL;
  return &s->result;
}
__attribute__((used,naked,noinline)) static void original_message(void) {
  __asm__("adrp x1, _raw_selector@PAGE\n"
          "ldr x1, [x1, _raw_selector@PAGEOFF]\n"
          "b _objc_msgSend\n");
}
__attribute__((naked,noinline)) void *original_affine(void *receiver,void *cmd,
)";
    OS << (Concat ? "void *first,void *second" : "double angle") << ") {\n"
       << "  __asm__(\n";
    for (unsigned I = 0; I < F.Words.size(); ++I) {
      const auto Address = F.Entry + 4 * I;
      if (Address == F.ProducerCall || Address == F.FirstProducerCall)
        OS << "    \"bl _" << F.ProducerName << "\\n\"\n";
      else if (Address == F.RectCall)
        OS << "    \"bl _CGRectApplyAffineTransform\\n\"\n";
      else if (Address == F.ImageCall)
        OS << "    \"bl _original_message\\n\"\n";
      else
        OS << "    \".inst " << F.Words[I] << "\\n\"\n";
    }
    OS << R"(  );
}
int main(void) {
  const uint64_t patterns[]={0,0x8000000000000000ULL,1,0x8000000000000001ULL,
    0x3ff0000000000000ULL,0xbff012345678abcdULL,0x7ff0000000000000ULL,
    0xfff0000000000000ULL,0x7ff812345678abcdULL,0x7ff012345678abcdULL};
  raw_selector=(SEL)&selector_token;
  for(unsigned i=0;i<1000;++i) {
    struct {uint64_t guard;struct OracleTransform t[2];uint64_t tail;} input={0};
    struct {uint64_t guard;struct CopyState s;uint64_t tail;} generated={0},original={0};
    uint64_t bits[12];
    input.guard=generated.guard=original.guard=0x123456789abcdef0ULL;
    input.tail=generated.tail=original.tail=0xfedcba9876543210ULL;
    for(unsigned j=0;j<12;++j)bits[j]=patterns[(i/(j+1)+j*3)%10];
    __builtin_memcpy(input.t,bits,96);
    double angle=((int)i-500)*0.01;
    struct OracleTransform expected;
    void *a,*b;
)";
    if (Concat)
      OS << "    expected=sdk_concat(input.t[0],input.t[1]);\n"
         << "    a=" << F.FunctionName
         << "(&generated.s,(void *)(uintptr_t)0x777,input.t,input.t+1);\n"
         << "    b=original_affine(&original.s,(void "
            "*)(uintptr_t)0x777,input.t,input.t+1);\n";
    else
      OS << "    expected=sdk_rotation(angle);\n"
         << "    a=" << F.FunctionName
         << "(&generated.s,(void *)(uintptr_t)0x777,angle);\n"
         << "    b=original_affine(&original.s,(void "
            "*)(uintptr_t)0x777,angle);\n";
    OS << R"(
    if(a!=&generated.s.result||b!=&original.s.result||
       generated.s.result!=original.s.result||generated.s.calls!=1||
       original.s.calls!=1||selectors!=i+1)return 1;
    if(__builtin_memcmp(generated.s.bits,&expected,48)||
       __builtin_memcmp(original.s.bits,&expected,48))return 2;
    if(__builtin_memcmp(input.t,bits,96))return 3;
    if(input.guard!=0x123456789abcdef0ULL||generated.guard!=input.guard||
       original.guard!=input.guard||input.tail!=0xfedcba9876543210ULL||
       generated.tail!=input.tail||original.tail!=input.tail)return 4;
  }
  return 0;
}
)";
    for (const auto Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      source_call_execution_test::compileAndRun(
          Program, {Optimization, "-framework", "CoreGraphics"});
    }
  }
#endif
}

TEST(ObjCImageValueCopy, OriginalFrameAndCompleteBodyAuthorizePublication) {
  ObjCImageCopyFixture F;
  F.run();
  ASSERT_TRUE(F.low());
  const auto Hints = buildObjCSourceCallHints(F.Image, *F.low());
  ASSERT_TRUE(Hints.count(F.CopyCall));
  ASSERT_TRUE(Hints.at(F.CopyCall).ByValueCopy);
  EXPECT_FALSE(Hints.at(F.CopyCall).NativeSwiftReceiver);
  EXPECT_EQ(Hints.at(F.CopyCall).ByValueCopy->Bytes, 48U);
  auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
  const auto Call = F.copyCall(Bound.Function);
  ASSERT_TRUE(Call);
  const auto Current =
      sdk::native_source_detail::currentFunction(F.Result, F.Entry);
  ASSERT_TRUE(Current);
  EXPECT_TRUE(validateObjCByValueCopyBindings(F.Image, F.low(), *Current->Med));
  unsigned Calls = 0, ResultAssignments = 0;
  for (const auto &B : Current->Med->Blocks)
    for (const auto &Op : B.Ops)
      if (Op.Addr == F.CopyCall) {
        Calls += Op.Opcode == NdOp::CALL;
        ResultAssignments += Op.Opcode == NdOp::COPY;
      }
  EXPECT_EQ(Calls, 1U);
  EXPECT_NE(ResultAssignments, 0U);
  EXPECT_FALSE(sdk::objcSourceCallBound(*Call, F.Image, {}, nullptr, nullptr,
                                        &Bound.Function));
  EXPECT_TRUE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                  Bound.Function, {}));
}

TEST(ObjCImageValueCopy, GeneratedCExecutesAgainstIndependentPhysicalCopyABI) {
#if !defined(__aarch64__) && !defined(_M_ARM64)
  GTEST_SKIP() << "The six-double indirect CoreImage ABI requires arm64";
#else
  ObjCImageCopyFixture F;
  F.run();
  ASSERT_TRUE(F.high());
  const auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
  const auto Call = F.copyCall(Bound.Function);
  ASSERT_TRUE(Call);
  ASSERT_TRUE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                  Bound.Function, {}));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.EmitComments = false;
  ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
  ASSERT_NE(Source.find("__builtin_memcpy"), std::string::npos);
  // The callee accepts the compiler-observed physical X2 pointer. It does
  // not repeat the emitter's logical CGAffineTransform declaration.
  const auto Program = Source + R"(
struct CopyState { unsigned calls; uint64_t bits[6]; uint64_t result; };
static unsigned selectors;
static uint64_t selector_token;
SEL sel_registerName(const char *name) {
  if (__builtin_strcmp(name,"imageByApplyingTransform:")) __builtin_trap();
  ++selectors; return (SEL)&selector_token;
}
#ifdef __APPLE__
#define COPY_DISPATCH_SYMBOL "_objc_msgSend"
#else
#define COPY_DISPATCH_SYMBOL "objc_msgSend"
#endif
void *copy_oracle(void *,void *,unsigned char *) __asm__(COPY_DISPATCH_SYMBOL);
void *copy_oracle(void *receiver,void *selector,unsigned char *copy) {
  struct CopyState *s=receiver;
  if(selector!=&selector_token||!copy) __builtin_trap();
  ++s->calls;__builtin_memcpy(s->bits,copy,48);
  // A legal write to disposable by-value storage cannot change caller inputs.
  __builtin_memset(copy,0xa5,48);
  s->result=0x93ace0142785bf62ULL;return &s->result;
}
int main(void) {
  const uint64_t patterns[]={0,0x8000000000000000ULL,1,0x8000000000000001ULL,
    0x3ff0000000000000ULL,0xbff012345678abcdULL,0x7ff0000000000000ULL,
    0xfff0000000000000ULL,0x7ff812345678abcdULL,0x7ff012345678abcdULL};
  for(unsigned i=0;i<1000;++i) {
    struct {uint64_t guard;double values[6];uint64_t tail;} input={0};
    struct {uint64_t guard;struct CopyState s;uint64_t tail;} output={0};
    uint64_t expected[6];
    input.guard=output.guard=0x123456789abcdef0ULL;
    input.tail=output.tail=0xfedcba9876543210ULL;
    for(unsigned j=0;j<6;++j) {
      expected[j]=patterns[(i/(j+1)+j*3)%10];
      __builtin_memcpy(input.values+j,expected+j,8);
    }
    void *result=image_copy(&output.s,(void *)(uintptr_t)0x777,
      input.values[0],input.values[1],input.values[2],input.values[3],
      input.values[4],input.values[5]);
    if(result!=&output.s.result||output.s.result!=0x93ace0142785bf62ULL||
       output.s.calls!=1||selectors!=i+1)return 1;
    if(__builtin_memcmp(output.s.bits,expected,48)||
       __builtin_memcmp(input.values,expected,48))return 2;
    if(input.guard!=0x123456789abcdef0ULL||output.guard!=input.guard||
       input.tail!=0xfedcba9876543210ULL||output.tail!=input.tail)return 3;
  }
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    source_call_execution_test::compileAndRun(Program, {Optimization});
  }
#endif
}

TEST(ObjCImageValueCopy, RejectsChangedCopyCallBodyAndCurrentImage) {
  for (unsigned Mutation = 0; Mutation < 24; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ObjCImageCopyFixture F;
    F.run();
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    const auto Call = F.copyCall(Bound.Function);
    ASSERT_TRUE(Call);
    ASSERT_TRUE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                    Bound.Function, {}));
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = Hint;
    if (Mutation == 0)
      Hint->ByValueCopy.reset();
    if (Mutation == 1)
      Hint->ByValueCopy->FrameOffset += 8;
    if (Mutation == 2)
      Hint->ByValueCopy->Bytes = 40;
    if (Mutation == 3)
      ++Hint->ByValueCopy->Site.Sequence;
    if (Mutation == 4)
      Call->Operands[2] = Call->Operands[0];
    if (Mutation == 5)
      Call->Operands[0] = HighExpr::makeConst(0, 8);
    if (Mutation == 6)
      Bound.Function.FrameSize += 16;
    if (Mutation == 7)
      Hint->WeakImport = true;
    if (Mutation == 8) {
      for (auto &S : Bound.Function.Body)
        if (S.Kind == StmtKind::Store) {
          S.StoreVal = HighExpr::makeConst(0, S.StoreVal->Type->Size);
          break;
        }
    }
    if (Mutation == 9) {
      auto Store =
          std::find_if(Bound.Function.Body.begin(), Bound.Function.Body.end(),
                       [](const auto &S) { return S.Kind == StmtKind::Store; });
      ASSERT_NE(Store, Bound.Function.Body.end());
      Bound.Function.Body.erase(Store);
    }
    if (Mutation == 10)
      F.u32(F.Entry + 20, 0xd503201f);
    if (Mutation == 11)
      F.Image.DynInfo.NeededLibs.pop_back();
    if (Mutation == 12)
      F.Image.ObjCMethods[0].TypeEncoding = "@16@0:8";
    if (Mutation == 13) {
      for (auto &M : F.Result.MedFuncs)
        for (auto &B : M.Blocks)
          for (auto &Op : B.Ops)
            if (Op.SourceCallHint && Op.SourceCallHint->ByValueCopy) {
              auto H = std::make_shared<SourceCallTypeHint>(*Op.SourceCallHint);
              H->ByValueCopy->FrameOffset += 8;
              Op.SourceCallHint = H;
            }
    }
    if (Mutation == 14 || Mutation == 15 || Mutation == 16) {
      for (auto &M : F.Result.MedFuncs)
        for (auto &B : M.Blocks)
          for (auto &Op : B.Ops) {
            if (Mutation == 14 && Op.Addr == F.CopyCall &&
                Op.Opcode == NdOp::COPY)
              Op.SourceCallHint = Hint;
            if (Op.SourceCallHint && Op.SourceCallHint->ByValueCopy &&
                Op.Opcode == NdOp::CALL) {
              if (Mutation == 15)
                Op.SourceCallHint.reset();
              if (Mutation == 16)
                Op.Inputs[3] = MedVar::makeConst(0x7000, 8);
            }
          }
    }
    if (Mutation == 17)
      Hint->TargetAddress += 4;
    if (Mutation == 18)
      Hint->ByValueCopy->Parameter = 1;
    if (Mutation == 19)
      Bound.Function.DoesNotReturn = true;
    if (Mutation == 20)
      Bound.Function.Params.back().Type = NdType::makeInt(8);
    if (Mutation == 21) {
      auto Copy =
          std::find_if(Bound.Function.Body.begin(), Bound.Function.Body.end(),
                       [&](const auto &S) { return S.Addr == F.CopyCall; });
      ASSERT_NE(Copy, Bound.Function.Body.end());
      Bound.Function.Body.push_back(*Copy);
    }
    if (Mutation == 22 || Mutation == 23) {
      // Edit both saved representations consistently: only independent current
      // machine replay can reject this, rather than replaying the edit itself.
      for (auto &M : F.Result.MedFuncs) {
        if (M.Entry != F.Entry)
          continue;
        bool Changed = false;
        for (auto &B : M.Blocks)
          for (auto &Op : B.Ops) {
            if (Mutation == 22 && Op.SourceCallHint &&
                Op.SourceCallHint->ByValueCopy) {
              Op.Inputs[3] = MedVar::makeConst(0x7000, 8);
              Changed = true;
            }
            if (Mutation == 23 && Op.Opcode == NdOp::STORE &&
                Op.NumInputs == 2) {
              Op.Inputs[1] = MedVar::makeConst(0, Op.Inputs[1].Size);
              Changed = true;
              break;
            }
          }
        ASSERT_TRUE(Changed);
        MedToHighConverter Converter;
        Converter.setBinaryImage(&F.Image);
        for (auto &H : F.Result.HighFuncs)
          if (H.Entry == F.Entry)
            H = Converter.convert(M, F.Image.Arch);
      }
      Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
      auto Edited = F.copyCall(Bound.Function);
      ASSERT_TRUE(Edited);
      EXPECT_FALSE(sdk::objCByValueCopySourceCallBound(
          *Edited, F.Image, F.Result, Bound.Function, {}));
      continue;
    }
    EXPECT_FALSE(sdk::objCByValueCopySourceCallBound(*Call, F.Image, F.Result,
                                                     Bound.Function, {}));
  }
}

struct SwiftRuntimeRootFixture : FieldFixture {
  static constexpr const char *Provider = "/usr/lib/swift/libswiftCore.dylib";
  static constexpr const char *Superclass = "_OBJC_CLASS_$__TtCs12_SwiftObject";
  SwiftRuntimeRootFixture() {
    Image.ObjCClasses.resize(1);
    auto &C = Image.ObjCClasses.front();
    C.SuperclassAddress = 0;
    C.SuperclassName = "_TtCs12_SwiftObject";
    C.InstanceStart = 16;
    C.InstanceSize = 24;
    C.Ivars[0].Offset = 16;
    u32(0x3014, 0);
    u32(0x3104, 0);
    u32(0x3108, (12u << 16) | 1);
    u32(0x3304, 16);
    u32(0x3308, 24);
    u32(0x5028, 2);
    u32(0x5030, 24);
    u32(0x5034, 7);
    u64(0x5060, 16);
    u64(Slot, 16);
    u64(0x5008, 0);
    Image.DataPtrRelocSlots.erase(0x5008);
    Image.DataPtrRelocTargetOwners.erase(0x5008);
    Image.DynInfo.NeededLibs.push_back(Provider);
    Image.ImportPtrSlots[0x5008] = Superclass;
    Image.DyldBindSlots[0x5008] = {Superclass, 0, Provider, false};
  }
};
struct FixedRootFixture : SwiftRuntimeRootFixture {
  static constexpr va_t FirstSlot = 0x3580, SecondSlot = 0x3588;
  FixedRootFixture() {
    auto &C = Image.ObjCClasses.front();
    C.InstanceSize = 33;
    C.Ivars = {{"model", "", 0x3408, FirstSlot, 16, 16, 8},
               {"older", "", 0x3428, SecondSlot, 32, 1, 1}};
    u32(0x301c, 13);
    u32(0x3020, 3);
    u32(0x3024, 2);
    u32(0x3028, 10);
    u32(0x310c, 2);
    u32(0x3110, 2);
    u32(0x311c, 2);
    relative(0x3120, 0x31d0);
    relative(0x3124, 0x31b0);
    text(0x3190, "SSSgSg");
    text(0x31a0, "model");
    text(0x31b0, "older");
    text(0x31d0, "SbSg");
    u32(0x3308, 33);
    u32(0x3404, 2);
    pointer(0x3408, FirstSlot);
    u32(0x3424, 16);
    pointer(0x3428, SecondSlot);
    pointer(0x3430, 0x31b0);
    pointer(0x3438, 0x31f0);
    u32(0x3440, 0);
    u32(0x3444, 1);
    u64(FirstSlot, 16);
    u64(SecondSlot, 32);
    u32(0x5030, 33);
    u32(0x5038, 128);
    u64(0x5050, 16);
    u64(0x5058, 32);
    Image.ObjCSourceReferences.erase(Slot);
    Image.ObjCSourceReferences[FirstSlot] = {
        ObjCSourceReference::Kind::IvarOffset, FirstSlot, 8, "model", C.Name};
    Image.ObjCSourceReferences[SecondSlot] = {
        ObjCSourceReference::Kind::IvarOffset, SecondSlot, 8, "older", C.Name};
    Image.Symbols[1] = {"_$s4Demo7DerivedC5modelSSSgSgvpWvd", FirstSlot, 8,
                        false};
    Image.Symbols.push_back(
        {"_$s4Demo7DerivedC5olderSbSgvpWvd", SecondSlot, 8, false});
    Image.Symbols.push_back({"_$s4Demo7DerivedCMn", 0x3000, 44, false});
    addSegment(0x7000, SegmentFlags::Readable);
    auto &Registration = Image.Sections.back();
    Registration.Name = "__swift5_types";
    Registration.Size = Registration.FileSz = 4;
    relative(0x7000, 0x3000);
  }
};
struct StaticRootFixture : FixedRootFixture {
  static constexpr va_t Base = 0x5400, Accessor = 0x1200;
  static constexpr va_t SelfStub = 0x1320, InitStub = 0x1340, InitSlot = 0x3628;
  HighFunc Function, MetadataFunction;
  ExprPtr Init;
  StaticRootFixture() {
    Image.Symbols[0] = {"_$s4Demo7DerivedC6shared_WZ", Entry, 32, true};
    Image.Symbols.push_back({"_$s4Demo7DerivedCMa", Accessor, 32, true});
    Image.Symbols.push_back({"_$s4Demo7DerivedC6shared_WZTv_", Base, 0, false});
    Image.Symbols.push_back(
        {"_$s4Demo7DerivedC6shared_Wz", Base + 48, 8, false});
    Image.RuntimeFunctionAddrs.insert(Accessor);
    relative(0x300c, Accessor);
    u64(Base + 32, 1);
    u64(Base + 40, 2);
    Image.ImportPtrSlots[0x3620] = "_objc_opt_self";
    Image.DyldBindSlots[0x3620] = {"_objc_opt_self", 0,
                                   "/usr/lib/libobjc.A.dylib", false};
    Image.ImportPtrSlots[InitSlot] = "_swift_initStaticObject";
    Image.DyldBindSlots[InitSlot] = {"_swift_initStaticObject", 0, Provider,
                                     false};
    const uint32_t Caller[] = {0xa9bf7bfd, 0x910003fd, 0x9400003e, 0x90000021,
                               0x91102021, 0x9400008b, 0xa8c17bfd, 0xd65f03c0};
    const uint32_t Metadata[] = {0xa9bf7bfd, 0x910003fd, 0x90000020,
                                 0x91000000, 0x94000044, 0xd2800001,
                                 0xa8c17bfd, 0xd65f03c0};
    const uint32_t Self[] = {0xd0000010, 0xf9431210, 0xd61f0200};
    const uint32_t Initialize[] = {0xd0000010, 0xf9431610, 0xd61f0200};
    for (unsigned I = 0; I < 8; ++I) {
      u32(Entry + I * 4, Caller[I]);
      u32(Accessor + I * 4, Metadata[I]);
    }
    for (unsigned I = 0; I < 3; ++I) {
      u32(SelfStub + I * 4, Self[I]);
      u32(InitStub + I * 4, Initialize[I]);
    }
    auto Local = [](int Id) {
      MedVar V;
      V.Kind = MedVar::Temp;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, NdType::makePtr(NdType::makeVoid()));
    };
    Function.Entry = Entry;
    Function.Name = Image.Symbols[0].Name;
    Function.ReturnType = NdType::makePtr(NdType::makeVoid());
    HighStmt Query;
    Query.Kind = StmtKind::Assign;
    Query.Addr = Entry + 8;
    Query.Dst = Local(0);
    Query.Val = HighExpr::makeCall("metadata", Accessor, {});
    Query.Val->Type = Function.ReturnType;
    const auto Runtime = swiftRuntimeSourceCallHint(Image, InitSlot);
    EXPECT_TRUE(Runtime);
    Init = HighExpr::makeCall(
        "swift_initStaticObject", InitStub,
        {Local(0), HighExpr::makeConst(
                       Base + 8, 8, ConstantAddressProvenance::DataAddress)});
    Init->Type = Function.ReturnType;
    if (Runtime)
      Init->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
    HighStmt InitializeObject;
    InitializeObject.Kind = StmtKind::Assign;
    InitializeObject.Addr = Entry + 20;
    InitializeObject.Dst = Local(1);
    InitializeObject.Val = Init;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.Addr = Entry + 28;
    Return.RetVal = Local(1);
    Function.Body = {Query, InitializeObject, Return};
    MetadataFunction.Entry = Accessor;
    MetadataFunction.Name = "_$s4Demo7DerivedCMa";
    MetadataFunction.ReturnType = Function.ReturnType;
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Function.ReturnType;
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Image.Arch, Error));
    MetadataFunction.SourceTypeHint = Signature;
  }
  std::map<va_t, const HighFunc *> functions() const {
    return {{Accessor, &MetadataFunction}};
  }
};
} // namespace

TEST(SwiftFieldReceiver, AuthenticatesImportedSwiftRuntimeRootIdentity) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftRuntimeRootFixture F;
    F.Image.Arch = Architecture;
    const auto Identity = swiftObjCClassIdentity(F.Image, 0x5000);
    ASSERT_TRUE(Identity);
    EXPECT_EQ(Identity->Module, "Demo");
    EXPECT_EQ(Identity->Name, "Derived");
    EXPECT_EQ(Identity->Metadata, 0x5000U);
    EXPECT_EQ(Identity->Descriptor, 0x3000U);
    EXPECT_TRUE(isInitialImageImportSlot(F.Image, 0x5008));
    EXPECT_FALSE(isImmutableImageImportSlot(F.Image, 0x5008));
    // This class identity cannot turn native Swift field records into the
    // separate Objective-C reflection/field-layout proof.
    EXPECT_FALSE(
        swiftObjCStoredFieldClass(F.Image, Identity->RuntimeName, F.Slot));
  }
}

TEST(SwiftFieldReceiver, FixedNativeRootLayoutUsesImmutableDeclaredOffsets) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    FixedRootFixture F;
    F.Image.Arch = Architecture;
    EXPECT_FALSE(readImmutableImageBytes(F.Image, F.FirstSlot, 8));
    EXPECT_EQ(readImmutableImageIvarOffset(F.Image, F.FirstSlot), 16U);
    const auto Layout = swiftFixedRootClassStorage(F.Image, 0x5000);
    ASSERT_TRUE(Layout);
    EXPECT_EQ(Layout->Size, 33U);
    EXPECT_EQ(Layout->Alignment, 8U);
    ASSERT_EQ(Layout->Fields.size(), 2U);
    EXPECT_EQ(Layout->Fields[0].MangledType, "SSSgSg");
    EXPECT_EQ(Layout->Fields[0].Offset, 16U);
    EXPECT_EQ(Layout->Fields[0].ByteCount, 16U);
    EXPECT_EQ(Layout->Fields[1].MangledType, "SbSg");
    EXPECT_EQ(Layout->Fields[1].Offset, 32U);
    EXPECT_EQ(Layout->Fields[1].ByteCount, 1U);
  }
}

TEST(SwiftFieldReceiver,
     FixedNativeRootLayoutRejectsStaleOrIncompleteEvidence) {
  for (unsigned Variant = 0; Variant != 20; ++Variant) {
    SCOPED_TRACE(Variant);
    FixedRootFixture F;
    switch (Variant) {
    case 0:
      F.Image.ObjCSourceReferences.erase(F.FirstSlot);
      break;
    case 1:
      F.Image.ObjCSourceReferences.at(F.FirstSlot).Name = "other";
      break;
    case 2:
      F.Image.ObjCSourceReferences.at(F.FirstSlot).ClassName = "other";
      break;
    case 3:
      F.Image.ObjCSourceReferences.at(F.FirstSlot).Size = 4;
      break;
    case 4:
      F.Image.DataPtrRelocSlots.insert(F.FirstSlot);
      break;
    case 5:
      F.Image.RelDataPtrRelocSlots.insert(F.FirstSlot + 4);
      break;
    case 6:
      setMappedSectionFlags(F.Image, F.Image.Sections[1],
                            SegmentFlags::Readable | SegmentFlags::Writable);
      break;
    case 7:
      F.Image.Sections[1].Type = llvm::MachO::S_THREAD_LOCAL_REGULAR;
      break;
    case 8:
      F.u64(F.FirstSlot, 24);
      break;
    case 9:
      F.u64(0x5050, 24);
      break;
    case 10:
      F.text(0x3190, "SS");
      break;
    case 11:
      F.text(0x31a0, "other");
      break;
    case 12:
      F.u32(0x3108, (12u << 16) | 7);
      break;
    case 13:
      F.u32(0x3404, 1);
      break;
    case 14:
      F.u32(0x5034, 15);
      break;
    case 15:
      F.u32(0x5028, 0);
      break;
    case 16:
      F.Image.Symbols[1].Name = "_$s4Demo7DerivedC5modelSSSgvpWvd";
      break;
    case 17:
      F.u32(0x7000, 0);
      break;
    case 18:
      F.Image.ObjCClasses[0].Ivars[1].Alignment = 8;
      break;
    case 19:
      F.u32(0x3110, 3);
      break;
    }
    EXPECT_FALSE(swiftFixedRootClassStorage(F.Image, 0x5000));
  }
}

TEST(SwiftFieldReceiver,
     StaticRootStoragePreservesTokenHeaderAndOptionalValues) {
  StaticRootFixture F;
  const auto Proof =
      sdk::objc_binding_detail::swiftStaticRootObjectStorage(F.Image, F.Base);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->Hint.ByteCount, 48U);
  EXPECT_TRUE(sdk::objc_binding_detail::swiftStaticRootObjectUse(
      F.Image, F.Function, *F.Init, F.Entry + 20, *Proof));
  auto Functions = F.functions();
  const auto Bound =
      sdk::bindObjCSourceReferences(F.Function, F.Image, nullptr, &Functions);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  ASSERT_EQ(Bound.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{F.Base, 48}}));
  const auto Call = Bound.Function.Body[1].Val;
  ASSERT_EQ(Call->Operands[1]->Kind, ExprKind::BinOp);
  const auto Storage = Call->Operands[1]->Operands[0];
  EXPECT_TRUE(sdk::objcSourceCallBound(*Storage, F.Image, Functions));
  EXPECT_TRUE(sdk::objcSourceCallBound(*Call, F.Image, Functions, nullptr,
                                       nullptr, &Bound.Function));
  EXPECT_EQ(Bound.Function.Body[0].Val->CallAddr, F.Accessor);
  EXPECT_EQ(Call->CallAddr, F.InitStub);
  std::set<std::string> Helpers;
  const auto C = sdk::renderObjCLocalStorageHelpers(
      F.Image, Bound.LocalStorageExtents, Helpers);
  EXPECT_NE(C.find("storage[48]"), std::string::npos);
  EXPECT_NE(C.find("[32] = 1"), std::string::npos);
  EXPECT_NE(C.find("[40] = 2"), std::string::npos);
  F.u64(F.Base + 32, 0);
  EXPECT_FALSE(sdk::objcSourceCallBound(*Storage, F.Image, Functions));
  EXPECT_FALSE(sdk::objcSourceCallBound(*Call, F.Image, Functions, nullptr,
                                        nullptr, &Bound.Function));
}

TEST(SwiftFieldReceiver,
     StaticRootStoragePreservesOptionalDepthAndArchitecture) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const bool Nested : {false, true}) {
      StaticRootFixture F;
      F.Image.Arch = Architecture;
      if (!Nested) {
        F.text(0x3190, "SSSg");
        F.Image.Symbols[1].Name = "_$s4Demo7DerivedC5modelSSSgvpWvd";
      }
      const uint64_t Nil = Nested ? (Architecture == Arch::AArch64 ? 1 : 2) : 0;
      F.u64(F.Base + 32, Nil);
      const auto Proof = sdk::objc_binding_detail::swiftStaticRootObjectStorage(
          F.Image, F.Base);
      ASSERT_TRUE(Proof);
      EXPECT_EQ(Proof->Hint.ByteCount, 48U);
      F.u64(F.Base + 32, Nil + 1);
      EXPECT_FALSE(sdk::objc_binding_detail::swiftStaticRootObjectStorage(
          F.Image, F.Base));
    }
  }
}

TEST(SwiftFieldReceiver, StaticRootStorageRejectsChangedPublishedArguments) {
  for (unsigned Variant = 0; Variant != 14; ++Variant) {
    SCOPED_TRACE(Variant);
    StaticRootFixture F;
    auto Functions = F.functions();
    auto Bound =
        sdk::bindObjCSourceReferences(F.Function, F.Image, nullptr, &Functions);
    ASSERT_TRUE(Bound.LocalStorageExtents.count(F.Base));
    const auto Call = Bound.Function.Body[1].Val;
    const auto Object = Call->Operands[1];
    const auto Storage = Object->Operands[0];
    const auto Offset = Object->Operands[1];
    switch (Variant) {
    case 0:
      Call->Operands[1] = Storage;
      break;
    case 1:
      std::swap(Object->Operands[0], Object->Operands[1]);
      break;
    case 2:
      Offset->ConstVal = 16;
      break;
    case 3:
      Offset->Type = NdType::makeInt(1);
      break;
    case 4:
      Offset->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 5:
      Offset->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 6:
      Offset->IndirectTarget = HighExpr::makeUndef(8);
      break;
    case 7:
      Offset->Operands.push_back(HighExpr::makeUndef(8));
      break;
    case 8:
      Object->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 9:
      Object->Op = NdOp::INT_SUB;
      break;
    case 10:
      Bound.Function.Body[0].Val->CallAddr += 4;
      break;
    case 11:
      Bound.Function.Body[1].Addr += 4;
      break;
    case 12: {
      auto Changed =
          std::make_shared<SourceCallTypeHint>(*Storage->SourceCallHint);
      Changed->ByteCount = 40;
      Storage->SourceCallHint = Changed;
      break;
    }
    case 13:
      Bound.Function.Body.insert(Bound.Function.Body.begin(),
                                 Bound.Function.Body[0]);
      break;
    }
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, F.Image, Functions, nullptr,
                                          nullptr, &Bound.Function));
  }
}

TEST(SwiftFieldReceiver, StaticRootStorageRejectsMalformedInitializersAndUses) {
  for (unsigned Variant = 0; Variant != 18; ++Variant) {
    SCOPED_TRACE(Variant);
    StaticRootFixture F;
    switch (Variant) {
    case 0:
      F.u64(F.Base, 1);
      break;
    case 1:
      F.u64(F.Base + 8, 0x5000);
      break;
    case 2:
      F.u64(F.Base + 32, 2);
      break;
    case 3:
      *F.bytes(F.Base + 47) = 1;
      break;
    case 4:
      F.Image.DataPtrRelocSlots.insert(F.Base + 16);
      break;
    case 5:
      F.Image.Symbols.push_back({"overlap", F.Base - 8, 16, false});
      break;
    case 6:
      F.Image.Symbols.push_back(
          {"_$s4Demo7DerivedC6shared_WZTv_", F.Base + 128, 0, false});
      break;
    case 7:
      F.Image.DyldBindSlots.at(F.InitSlot).WeakImport = true;
      break;
    case 8:
      F.relative(0x300c, F.Accessor + 4);
      break;
    case 9:
      F.u32(F.Entry + 12, 0x90000020);
      break;
    case 10:
      F.Init->Operands[0] = HighExpr::makeUndef(8);
      break;
    case 11:
      F.Function.Body[0].Val->CallAddr += 4;
      break;
    case 12:
      F.Function.Name = "other";
      break;
    case 13:
      F.Init->IndirectTarget = HighExpr::makeUndef(8);
      break;
    case 14:
      F.Init->Operands[1]->IndirectTarget = HighExpr::makeUndef(8);
      break;
    case 15:
      F.Function.Body[0].Val->Operands.push_back(HighExpr::makeUndef(8));
      break;
    case 16:
      F.Image.Symbols[0].Name = "other";
      break;
    case 17:
      F.Image.Arch = Arch::X64;
      F.u64(F.Base + 32, 2);
      break;
    }
    auto Functions = F.functions();
    const auto Bound =
        sdk::bindObjCSourceReferences(F.Function, F.Image, nullptr, &Functions);
    EXPECT_FALSE(Bound.LocalStorageExtents.count(F.Base));
  }
}

TEST(SwiftFieldReceiver, ImportedSwiftRootRequiresCurrentExactDeclaration) {
  for (unsigned Variant = 0; Variant != 25; ++Variant) {
    SCOPED_TRACE(Variant);
    SwiftRuntimeRootFixture F;
    auto &C = F.Image.ObjCClasses.front();
    switch (Variant) {
    case 0:
      F.Image.DyldBindSlots.at(0x5008).WeakImport = true;
      break;
    case 1:
      F.Image.DyldBindSlots.at(0x5008).Module = "/usr/lib/libobjc.A.dylib";
      break;
    case 2:
      F.Image.DynInfo.NeededLibs.pop_back();
      break;
    case 3:
      F.Image.DyldBindSlots.at(0x5008).Addend = 8;
      break;
    case 4:
      F.Image.ImportPtrSlots.at(0x5008) = "_other_class";
      break;
    case 5:
      F.Image.DyldBindSlots.erase(0x5008);
      break;
    case 6:
      F.Image.MachOTwoLevelNamespace = false;
      break;
    case 7:
      F.Image.ConflictingImportStorageSlots.insert(0x5008);
      break;
    case 8:
      C.SuperclassName = "NSObject";
      break;
    case 9:
      C.InheritanceStatus = "unresolved";
      break;
    case 10:
      C.SuperclassAddress = 0x5100;
      break;
    case 11:
      F.relative(0x3014, 0x3188);
      break;
    case 12:
      C.RootClass = true;
      break;
    case 13:
      C.InstanceStart = 8;
      F.u32(0x3304, 8);
      break;
    case 14:
      F.u32(0x5030, 32);
      break;
    case 15:
      F.u32(0x3308, 32);
      break;
    case 16:
      F.u32(0x502c, 8);
      break;
    case 17:
      F.Image.DataPtrRelocSlots.insert(0x5008);
      break;
    case 18:
      F.Image.RelDataPtrRelocSlots.insert(0x500c);
      break;
    case 19:
      F.Image.ImportPtrSlots[0x500c] = "_overlapping_import";
      break;
    case 20:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 21:
      F.Image.Format = BinaryFormat::ELF;
      break;
    case 22:
      F.Image.DataPtrRelocSlots.insert(0x5030);
      break;
    case 23:
      F.Image.ImportStorageSlots[0x5008] = {F.Superclass, 8,
                                            ImportStorageEvidence::LoaderBind};
      break;
    case 24:
      F.Image.Sections.push_back(F.Image.Sections.back());
      break;
    }
    EXPECT_FALSE(swiftObjCClassIdentity(F.Image, 0x5000));
  }
}

TEST(SwiftFieldReceiver, ReflectionAndIvarIdentityRemainSeparateFromLayout) {
  FieldFixture F;
  const auto C = swiftObjCClassIdentity(F.Image, 0x5000);
  ASSERT_TRUE(C);
  EXPECT_EQ(C->Module, "Demo");
  EXPECT_EQ(C->Name, "Derived");
  EXPECT_EQ(swiftObjCStoredFieldClass(F.Image, C->RuntimeName, F.Slot),
            "CALayer");
  EXPECT_TRUE(F.Image.ObjCClasses[0].Ivars[0].TypeEncoding.empty());
  const auto Unambiguous = objcSelectorSourceTypeHint(F.Image, "setTransform:");
  ASSERT_TRUE(Unambiguous);
  EXPECT_EQ(Unambiguous->Parameters[2].Type->Size, 128U);
  auto Conflict = F.Image;
  ObjCMethod Affine;
  Affine.ClassName = "OtherView";
  Affine.Selector = "setTransform:";
  Affine.TypeEncoding = "v64@0:8{CGAffineTransform=dddddd}16";
  Affine.TypeHint =
      parseObjCMethodEncoding(Affine.Selector, Affine.TypeEncoding);
  ASSERT_TRUE(Affine.TypeHint);
  Conflict.ObjCMethods.push_back(Affine);
  EXPECT_FALSE(objcSelectorSourceTypeHint(Conflict, "setTransform:"));
  const auto Root = objcNativeSwiftSelfTypeHint(F.Image, F.Entry);
  ASSERT_TRUE(Root);
  const auto Field = objcReceiverIvarTypeHint(F.Image, *Root, F.Slot);
  ASSERT_TRUE(Field);
  const auto Decl =
      objcReceiverSourceTypeHint(F.Image, "setTransform:", *Field);
  // The complete class declaration now has a logical record and a physical
  // copy pointer. Neither reflection nor the ABI proves the call's storage.
  EXPECT_TRUE(Decl.HasDeclaration);
  ASSERT_TRUE(Decl.Signature);
  ASSERT_EQ(Decl.Signature->Parameters.size(), 3U);
  EXPECT_EQ(Decl.Signature->Parameters[2].Type->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Decl.Signature->Parameters[2].Type->Size, 128U);
  EXPECT_TRUE(Decl.Signature->Parameters[2].IndirectByValue);
  EXPECT_EQ(Decl.Signature->Parameters[2].Location.ValueBytes, 8U);
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

TEST(SwiftFieldReceiver, IncompleteRecordDeclarationDoesNotIssueCopyReceipt) {
  FieldFixture F;
  F.text(0x3660, "setTransform:");
  F.Image.ObjCSourceReferences[0x3600].Name = "setTransform:";
  const auto Root = objcNativeSwiftSelfTypeHint(F.Image, F.Entry);
  ASSERT_TRUE(Root);
  const auto Field = objcReceiverIvarTypeHint(F.Image, *Root, F.Slot);
  ASSERT_TRUE(Field);
  const auto Declaration =
      objcReceiverSourceTypeHint(F.Image, "setTransform:", *Field);
  ASSERT_TRUE(Declaration.Signature);
  ASSERT_TRUE(Declaration.Signature->Parameters[2].IndirectByValue);
  F.u32(F.Entry + 36, 0x6d0603e0); // only 16 of the last 32 bytes initialized
  F.run();
  ASSERT_TRUE(F.low());
  EXPECT_FALSE(buildObjCSourceCallHints(F.Image, *F.low()).count(F.Call));
  size_t Calls = 0;
  for (const auto &M : F.Result.MedFuncs)
    for (const auto &B : M.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Addr == F.Call && Op.Opcode == NdOp::CALL) {
          ++Calls;
          EXPECT_FALSE(Op.SourceCallHint);
        }
  EXPECT_EQ(Calls, 1U);
}

TEST(SwiftFieldReceiver, CGRectMethodSelfKeepsItsLogicalParameterIdentity) {
  FieldFixture F;
  F.Image.Symbols[0].Name =
      "_$s4Demo7DerivedC4draw2in4rectySo12CGContextRefa_So6CGRectVtF";
  F.run();
  ASSERT_TRUE(F.high());
  ASSERT_EQ(F.high()->Params.size(), 3U);
  const auto Receiver = objcNativeSwiftSelfTypeHint(F.Image, F.Entry);
  ASSERT_TRUE(Receiver);
  EXPECT_EQ(Receiver->SourceParameter, 2U);
  EXPECT_TRUE(objcReceiverTypeHintValid(F.Image, *Receiver));
  for (unsigned Other : {0U, 1U, 3U}) {
    auto Wrong = *Receiver;
    Wrong.SourceParameter = Other;
    EXPECT_FALSE(objcReceiverTypeHintValid(F.Image, Wrong)) << Other;
  }
  auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
  const auto Call = F.call(Bound.Function);
  ASSERT_TRUE(Call);
  ASSERT_TRUE(Call->SourceCallHint->Receiver);
  EXPECT_EQ(Call->SourceCallHint->Receiver->SourceParameter, 2U);
  EXPECT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
      *Call, F.Image, F.Result, Bound.Function, {}));
}

TEST(SwiftFieldReceiver, CGRectMethodRejectsChangedEntryAndReceiverParameter) {
  for (unsigned Case = 0; Case != 5; ++Case) {
    SCOPED_TRACE(Case);
    FieldFixture F;
    F.Image.Symbols[0].Name =
        "_$s4Demo7DerivedC4draw2in4rectySo12CGContextRefa_So6CGRectVtF";
    F.run();
    ASSERT_TRUE(F.high());
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    auto Call = F.call(Bound.Function);
    ASSERT_TRUE(Call);
    ASSERT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
    if (Case == 0) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
      Hint->Receiver->SourceParameter = 0;
      Call->SourceCallHint = Hint;
    }
    if (Case == 1) {
      std::vector<ExprPtr> Pending;
      walkStmts(Bound.Function.Body, [&](const HighStmt &S) {
        forEachExpr(S, [&](const auto &E) { Pending.push_back(E); });
      });
      unsigned Changed = 0;
      while (!Pending.empty()) {
        auto E = Pending.back();
        Pending.pop_back();
        if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Param &&
            E->Var.Id == 2) {
          E->Var.Id = 0;
          ++Changed;
        }
        E->forEachChildExpr(
            [&](const auto &Child) { Pending.push_back(Child); });
      }
      ASSERT_NE(Changed, 0U);
    }
    if (Case == 2)
      Bound.Function.SourceTypeHint->Parameters[2].Location.RegisterOffset =
          a64reg::X0;
    if (Case == 3)
      F.Image.Symbols[0].Name += "To";
    if (Case == 4)
      Bound.Function.Params[1].Type = NdType::makeInt(8);
    EXPECT_FALSE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
  }
}

TEST(SwiftFieldReceiver, InitializedRecordCopyKeepsLogicalMessageAndPublishes) {
  FieldFixture F;
  F.text(0x3660, "setTransform:");
  F.Image.ObjCSourceReferences[0x3600].Name = "setTransform:";
  F.run();
  ASSERT_TRUE(F.high());
  auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
  const auto Call = F.call(Bound.Function);
  ASSERT_TRUE(Call);
  ASSERT_EQ(Call->Operands.size(), 3U);
  EXPECT_EQ(Call->Operands[2]->Type->Size, 8U);
  EXPECT_EQ(Call->SourceCallHint->Signature.Parameters[2].Type->Size, 128U);
  EXPECT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
      *Call, F.Image, F.Result, Bound.Function, {}));
}

TEST(SwiftFieldReceiver, RecordCopyRejectsStaleStorageAndPublication) {
  for (unsigned Case = 0; Case < 30; ++Case) {
    SCOPED_TRACE(Case);
    FieldFixture F;
    F.text(0x3660, "setTransform:");
    F.Image.ObjCSourceReferences[0x3600].Name = "setTransform:";
    F.run();
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    auto Call = F.call(Bound.Function);
    ASSERT_TRUE(Call);
    ASSERT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = Hint;
    MedOp *Med = nullptr;
    for (auto &M : F.Result.MedFuncs)
      for (auto &B : M.Blocks)
        for (auto &Op : B.Ops)
          if (Op.Addr == F.Call && Op.Opcode == NdOp::CALL)
            Med = &Op;
    ASSERT_NE(Med, nullptr);
    auto Store =
        std::find_if(Bound.Function.Body.begin(), Bound.Function.Body.end(),
                     [](const auto &S) { return S.Kind == StmtKind::Store; });
    ASSERT_NE(Store, Bound.Function.Body.end());
    switch (Case) {
    case 0:
      Hint->ByValueCopy.reset();
      break;
    case 1:
      Hint->ByValueCopy->FrameOffset += 8;
      break;
    case 2:
      Hint->ByValueCopy->Bytes = 64;
      break;
    case 3:
      Hint->ByValueCopy->Parameter = 1;
      break;
    case 4:
      ++Hint->ByValueCopy->FunctionEntry;
      break;
    case 5:
      ++Hint->ByValueCopy->Site.Sequence;
      break;
    case 6:
      Hint->Signature.Parameters[2].Type = NdType::makePtr(NdType::makeVoid());
      break;
    case 7:
      Hint->Signature.Parameters[2].Location.ValueBytes = 4;
      break;
    case 8:
      Hint->Signature.Parameters[2].IndirectByValue = false;
      break;
    case 9:
      Hint->NativeSwiftReceiver.reset();
      break;
    case 10:
      Call->Operands[2] = Call->Operands[0];
      break;
    case 11: {
      HighStmt S;
      S.Kind = StmtKind::Call;
      S.CallExpr = Call;
      Bound.Function.Body.push_back(S);
      break;
    }
    case 12:
      Store->StoreVal = HighExpr::makeConst(1, Store->StoreVal->Type->Size);
      break;
    case 13:
      Bound.Function.Body.erase(Store);
      break;
    case 14:
      std::swap(Bound.Function.Body[0], Bound.Function.Body[1]);
      break;
    case 15:
      Bound.Function.FrameSize += 16;
      break;
    case 16: {
      auto H = std::make_shared<SourceCallTypeHint>(*Med->SourceCallHint);
      H->ByValueCopy.reset();
      Med->SourceCallHint = H;
      break;
    }
    case 17:
      for (auto &B : F.low()->Blocks)
        for (auto &Op : B.Ops)
          if (Op.Opcode == NdOp::STORE)
            Op.Inputs[1] = NdVar::scalar(1, Op.Inputs[1].Size);
      break;
    case 18:
      F.u32(F.Entry + 36, 0xd503201f);
      break;
    case 19:
      Bound.Function.SourceTypeHint->Parameters[0].Location.RegisterOffset =
          a64reg::X0;
      break;
    case 20:
      Hint->WeakImport = true;
      break;
    case 21:
      Call->Operands[2] = HighExpr::makeConst(0, 4);
      break;
    case 22: {
      HighStmt S = *Store;
      Bound.Function.Body.push_back(S);
      break;
    }
    case 23:
      Call->Type = NdType::makeInt(8, false);
      break;
    case 24:
      for (auto &M : F.Result.MedFuncs)
        for (auto &B : M.Blocks)
          for (auto &Op : B.Ops)
            if (Op.Opcode == NdOp::STORE)
              Op.Inputs[1] = MedVar::makeConst(1, Op.Inputs[1].Size);
      break;
    case 25:
      Hint->Receiver->Steps[0].OffsetSlot += 8;
      break;
    case 26:
      Call->Operands[0] = HighExpr::makeConst(0, 8);
      break;
    case 27:
      Call->CallAddr += 4;
      break;
    case 28: {
      Hint->ByValueCopy.reset();
      auto H = std::make_shared<SourceCallTypeHint>(*Med->SourceCallHint);
      H->ByValueCopy.reset();
      Med->SourceCallHint = H;
      break;
    }
    case 29:
      Hint->Signature.Parameters[2].Type =
          NdType::makeStruct(std::vector<TypeRef>(6, NdType::makeFloat(8)));
      break;
    }
    EXPECT_FALSE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
  }
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

namespace {
struct FixedRecordFixture {
  BinaryImage Image;
  static constexpr va_t Descriptor = 0x3000, Class = 0x3080;
  static constexpr va_t Metadata = 0x3600, Witnesses = 0x3700;
  FixedRecordFixture(Arch Architecture) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.MachOTwoLevelNamespace = true;
    Image.MachOHasChainedFixups = true;
    segment(0x1000, 0x100, "__text", true);
    segment(0x3000, 0x1000, "__const");
    segment(0x5000, 8, "__swift5_types");
    segment(0x6000, 0x100, "__got");
    Image.Sections.back().Type = llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS;
    for (unsigned I = 0; I != 9; ++I) {
      u32(0x1000 + I * 4, Architecture == Arch::AArch64 ? 0xd65f03c0 : 0xc3);
      Image.RuntimeFunctionAddrs.insert(0x1000 + I * 4);
    }
    u32(Descriptor, 0x51);
    relative(Descriptor + 4, 0x3140);
    relative(Descriptor + 8, 0x3100);
    relative(Descriptor + 12, 0x1020);
    relative(Descriptor + 16, 0x3200);
    u32(Descriptor + 20, 4);
    u32(Descriptor + 24, 2);
    text(0x3100, "Value");
    relative(0x3148, 0x3150);
    text(0x3150, "Demo");
    u32(Class, 0x50);
    relative(Class + 4, 0x3140);
    relative(Class + 8, 0x3160);
    text(0x3160, "Object");
    relative(0x5000, Descriptor);
    relative(0x5004, Class);
    Image.Symbols.push_back({"_$s4Demo5ValueVMn", Descriptor, 0, false});
    Image.Symbols.push_back({"_$s4Demo6ObjectCMn", Class, 0, false});
    Image.Symbols.push_back({"_$s4Demo5ValueVN", Metadata, 0, false});
    u64(Metadata, 0x200);
    pointer(Metadata + 8, Descriptor);
    pointer(Metadata - 8, Witnesses);
    relative(0x3200, 0x3260);
    u32(0x3208, 12u << 16);
    u32(0x320c, 4);
    symbolic(0x3260, Descriptor, 1);
    symbolic(0x3270, 0x6000, 2);
    text(0x3280, "Sd");
    symbolic(0x3290, Class, 1);
    const va_t Types[] = {0x3270, 0x3270, 0x3280, 0x3290};
    for (unsigned I = 0; I != 4; ++I) {
      u32(0x3210 + I * 12, 2);
      relative(0x3214 + I * 12, Types[I]);
      relative(0x3218 + I * 12, 0x3420 + I * 32);
      text(0x3420 + I * 32, "field" + std::to_string(I));
      u32(Metadata + 16 + I * 4, I * 8);
    }
    for (unsigned I = 0; I != 8; ++I)
      pointer(Witnesses + I * 8, 0x1000 + I * 4, true);
    u64(Witnesses + 64, 32);
    u64(Witnesses + 72, 32);
    u32(Witnesses + 80, 0x30007);
    u32(Witnesses + 84, 0x7fffffff);
    EXPECT_TRUE(Image.recordDyldBindSlot(
        0x6000, "_$s12CoreGraphics7CGFloatVMn", 0,
        "/usr/lib/swift/libswiftCoreFoundation.dylib", false));
  }
  void segment(va_t Address, uint64_t Size, const char *Name,
               bool Code = false) {
    Segment S;
    S.VA = S.FileOff = Address;
    S.Size = S.FileSz = Size;
    S.Flags = SegmentFlags::Readable;
    if (Code)
      S.Flags = S.Flags | SegmentFlags::Executable;
    S.Data.resize(Size);
    Image.Segments.push_back(S);
    Section R;
    R.VA = R.FileOff = Address;
    R.Size = R.FileSz = Size;
    R.Flags = S.Flags;
    R.Name = Name;
    R.Type = Code ? llvm::MachO::S_ATTR_PURE_INSTRUCTIONS : 0;
    Image.Sections.push_back(R);
  }
  uint8_t *bytes(va_t Address) {
    for (auto &S : Image.Segments)
      if (Address >= S.VA && Address - S.VA < S.Size)
        return S.Data.data() + Address - S.VA;
    return nullptr;
  }
  void u32(va_t Address, uint32_t Value) {
    llvm::support::endian::write32le(bytes(Address), Value);
  }
  void u64(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(bytes(Address), Value);
  }
  void relative(va_t Address, va_t Target) {
    u32(Address, uint32_t(Target - Address));
  }
  void text(va_t Address, const std::string &Text) {
    std::memcpy(bytes(Address), Text.c_str(), Text.size() + 1);
  }
  void symbolic(va_t Address, va_t Target, uint8_t Kind) {
    *bytes(Address) = Kind;
    relative(Address + 1, Target);
    *bytes(Address + 5) = 0;
  }
  void pointer(va_t Address, va_t Target, bool Code = false) {
    u64(Address, Target);
    Image.MachOResolvedChainedPointerSlots.insert(Address);
    (Code ? Image.CodePtrRelocSlots : Image.DataPtrRelocSlots).insert(Address);
    if (!Code)
      Image.DataPtrRelocTargetOwners[Address] = Image.getSectionFor(Target)->VA;
  }
  std::optional<SwiftFixedRecordStorage> prove() {
    return swiftFixedRecordStorage(Image, Descriptor);
  }
};
} // namespace

TEST(SwiftFixedRecordStorage, IndependentNominalFieldsAndWitnessesAgree) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    FixedRecordFixture F(A);
    ASSERT_EQ(swiftLocalRegisteredNominalType(F.Image, F.Descriptor),
              "4Demo5ValueV");
    ASSERT_EQ(swiftLocalRegisteredNominalType(F.Image, F.Class),
              "4Demo6ObjectC");
    const auto R = F.prove();
    ASSERT_TRUE(R);
    EXPECT_EQ(R->MangledType, "4Demo5ValueV");
    EXPECT_EQ(R->Metadata, F.Metadata);
    EXPECT_EQ(R->Size, 32u);
    EXPECT_EQ(R->Alignment, 8u);
    for (unsigned I = 0; I != 4; ++I) {
      EXPECT_EQ(R->Fields[I].Offset, I * 8);
      EXPECT_EQ(R->Fields[I].Name, "field" + std::to_string(I));
      EXPECT_TRUE(R->Fields[I].IsMutable);
      EXPECT_EQ(R->Fields[I].Storage,
                I == 3 ? SwiftFixedRecordField::Kind::StrongReference
                       : SwiftFixedRecordField::Kind::Float64);
    }
    EXPECT_EQ(R->Fields[0].MangledType, "12CoreGraphics7CGFloatV");
    EXPECT_EQ(R->Fields[2].MangledType, "Sd");
    EXPECT_EQ(R->Fields[3].MangledType, "4Demo6ObjectC");
    // This physical storage certificate cannot widen Swift source declarations.
    const auto Source = recoverSwiftTypes(F.Image);
    ASSERT_FALSE(Source.empty());
    EXPECT_NE(Source[0].Status, "recovered");
    // Every ordinary Double field and immutable stored property is also valid.
    for (unsigned I = 0; I != 3; ++I)
      F.relative(0x3214 + I * 12, 0x3280);
    for (unsigned I = 0; I != 4; ++I)
      F.u32(0x3210 + I * 12, 0);
    ASSERT_TRUE(F.prove());
  }
}

TEST(SwiftFixedRecordStorage, RejectsStaleOrIncompleteStorageEvidence) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 47; ++Mutation) {
      SCOPED_TRACE(Mutation);
      FixedRecordFixture F(A);
      ASSERT_TRUE(F.prove());
      switch (Mutation) {
      case 0:
        F.u32(F.Descriptor, 0xd1);
        break; // Generic instantiation required.
      case 1:
        F.u32(F.Descriptor, 0x151);
        break; // Unrecognized descriptor version.
      case 2:
        F.u32(F.Descriptor, 0x10051);
        break; // Dynamic metadata initialization.
      case 3:
        F.u32(F.Descriptor + 20, 3);
        break;
      case 4:
        F.u32(F.Descriptor + 24, 3);
        break;
      case 5:
        F.relative(F.Descriptor + 12, 0x1040);
        break; // No authenticated accessor entry.
      case 6:
        F.Image.Symbols[0].Name = "_$s4Demo5OtherVMn";
        break;
      case 7:
        F.Image.Symbols[1].Name = "_$s4Demo6ObjectVMn";
        break;
      case 8:
        F.relative(0x5004, F.Descriptor);
        break; // Reference class unregistered.
      case 9:
        F.u32(0x5000, 1);
        break;
      case 10:
        F.Image.Symbols.push_back(F.Image.Symbols[2]);
        break;
      case 11:
        F.Image.Exports.push_back({F.Image.Symbols[0].Name, 0, F.Descriptor});
        break;
      case 12:
        F.Image.Symbols[2].Name = "_$s4Demo5OtherVN";
        break;
      case 13:
        F.u64(F.Metadata, 0x201);
        break;
      case 14:
        F.pointer(F.Metadata + 8, F.Class);
        break;
      case 15:
        F.u32(F.Metadata + 24, 24);
        break; // Overlapping stored fields.
      case 16:
        F.u32(F.Metadata + 28, 32);
        break; // A gap cannot be hidden by stride.
      case 17:
        F.u64(F.Witnesses + 64, 24);
        break;
      case 18:
        F.u64(F.Witnesses + 72, 40);
        break;
      case 19:
        F.u32(F.Witnesses + 80, 0x400000 | 0x30007);
        break; // Incomplete layout.
      case 20:
        F.u32(F.Witnesses + 80, 0x20007);
        break; // POD contradicts strong reference.
      case 21:
        F.u32(F.Witnesses + 80, 0x130007);
        break; // Other lifetime representation.
      case 22:
        F.Image.CodePtrRelocSlots.erase(F.Witnesses + 16);
        break;
      case 23:
        F.Image.MachOResolvedChainedPointerSlots.erase(F.Metadata + 8);
        break;
      case 24:
        F.Image.DataPtrRelocSlots.insert(F.Metadata + 17);
        break;
      case 25:
        F.u32(0x3208, (16u << 16));
        break;
      case 26:
        F.u32(0x320c, 5);
        break;
      case 27:
        F.relative(0x3200, 0x3290);
        break; // Field descriptor belongs to other type.
      case 28:
        F.u32(0x3204, 4);
        break; // Struct cannot declare a superclass.
      case 29:
        F.u32(0x3210, 4);
        break;
      case 30:
        F.relative(0x323c, 0x3420);
        break; // Duplicate field name.
      case 31:
        F.text(0x3280, "Sf");
        break; // Float is not Float64.
      case 32:
        F.text(0x3280, "SdSg");
        break; // Optional has a distinct representation.
      case 33:
        F.text(0x3295, "Xw");
        break; // Weak reference storage is not a strong pointer.
      case 34:
        F.text(0x3295, "Xo");
        break; // Unowned reference.
      case 35:
        F.text(0x3295, "Sg");
        break;
      case 36:
        F.symbolic(0x3290, F.Descriptor, 1);
        break;
      case 37:
        F.u32(F.Class, 0xd0);
        break; // Generic class type lacks arguments.
      case 38:
        F.Image.DyldBindSlots[0x6000].Module =
            "/usr/lib/swift/libswiftCore.dylib";
        break;
      case 39:
        F.Image.DyldBindSlots[0x6000].WeakImport = true;
        break;
      case 40:
        F.Image.DyldBindSlots[0x6000].Addend = 8;
        break;
      case 41:
        F.Image.Sections.back().Type =
            llvm::MachO::S_THREAD_LOCAL_VARIABLE_POINTERS;
        break;
      case 42:
        F.Image.Sections.back().Type = llvm::MachO::S_LAZY_SYMBOL_POINTERS;
        break;
      case 43:
        F.Image.ConflictingImportStorageSlots.insert(0x6000);
        break;
      case 44:
        F.Image.Segments[1].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 45:
        F.Image.RuntimeFunctionAddrs.insert(F.Metadata + 24);
        break;
      case 46:
        F.u64(0x3900, 0x200);
        F.pointer(0x3908, F.Descriptor);
        F.Image.Symbols.push_back({"different_metadata", 0x3900, 0, false});
        break;
      }
      EXPECT_FALSE(F.prove());
    }
}

TEST(SwiftFixedRecordStorage, CurrentPointersBoundsAndIndependentFieldTypes) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    FixedRecordFixture F(A);
    ASSERT_TRUE(F.prove());
    for (unsigned I = 0; I != 8; ++I) {
      auto C = F;
      C.pointer(C.Witnesses + I * 8, 0x1040, true);
      EXPECT_FALSE(C.prove());
    }
    for (unsigned I = 0; I != 4; ++I) {
      auto C = F;
      C.relative(0x3214 + I * 12, I == 3 ? 0x3280 : 0x3290);
      EXPECT_FALSE(C.prove());
    }
    for (unsigned I = 0; I != 3; ++I) {
      auto C = F;
      C.relative(0x3214 + I * 12, 0x3280);
      EXPECT_TRUE(C.prove());
    }
    for (uint32_t Kind : {llvm::MachO::S_THREAD_LOCAL_REGULAR,
                          llvm::MachO::S_THREAD_LOCAL_ZEROFILL,
                          llvm::MachO::S_THREAD_LOCAL_VARIABLES,
                          llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS}) {
      auto C = F;
      C.Image.Sections.back().Type = Kind;
      EXPECT_FALSE(C.prove());
    }
    auto Truncated = F;
    Truncated.Image.Sections[1].FileSz = F.Witnesses + 87 - 0x3000;
    EXPECT_FALSE(Truncated.prove());
    auto Alias = F;
    Alias.Image.Sections.push_back(Alias.Image.Sections[1]);
    EXPECT_FALSE(Alias.prove());
  }
}

TEST(SwiftFixedRecordStorage, RegistrationAndStorageCannotBeThreadLocal) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Section : {1u, 2u})
      for (uint32_t Kind :
           {llvm::MachO::S_THREAD_LOCAL_REGULAR,
            llvm::MachO::S_THREAD_LOCAL_ZEROFILL,
            llvm::MachO::S_THREAD_LOCAL_VARIABLES,
            llvm::MachO::S_THREAD_LOCAL_VARIABLE_POINTERS,
            llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS}) {
        SCOPED_TRACE(Section);
        SCOPED_TRACE(Kind);
        FixedRecordFixture F(A);
        ASSERT_TRUE(F.prove());
        F.Image.Sections[Section].Type = Kind;
        EXPECT_FALSE(swiftLocalRegisteredNominalType(F.Image, F.Descriptor));
        EXPECT_FALSE(F.prove());
      }
}

TEST(SwiftFixedRecordStorage, PointerRecordsCannotHideInThreadLocalSlices) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (va_t Slot :
         {FixedRecordFixture::Metadata - 8, FixedRecordFixture::Metadata + 8,
          FixedRecordFixture::Witnesses, FixedRecordFixture::Witnesses + 56}) {
      FixedRecordFixture F(A);
      auto Left = F.Image.Sections[1];
      auto Middle = Left, Right = Left;
      Left.Size = Left.FileSz = Slot - Left.VA;
      Middle.VA = Middle.FileOff = Slot;
      Middle.Size = Middle.FileSz = 8;
      Right.VA = Right.FileOff = Slot + 8;
      Right.Size = Right.FileSz = 0x4000 - Right.VA;
      F.Image.Sections[1] = Left;
      F.Image.Sections.push_back(Middle);
      F.Image.Sections.push_back(Right);
      for (auto &[Address, Owner] : F.Image.DataPtrRelocTargetOwners) {
        const auto Target = llvm::support::endian::read64le(F.bytes(Address));
        Owner = F.Image.getSectionFor(Target)->VA;
      }
      ASSERT_TRUE(F.prove());
      F.Image.Sections[F.Image.Sections.size() - 2].Type =
          llvm::MachO::S_THREAD_LOCAL_REGULAR;
      EXPECT_FALSE(F.prove()) << llvm::utohexstr(Slot);
    }
}

namespace {
struct FixedConstructorFixture : FixedRecordFixture {
  static constexpr va_t Entry = 0x1080;
  static constexpr const char *Name =
      "_$s4Demo5ValueV6field06field16field26field34Demo5ValueV"
      "12CoreGraphics7CGFloatV_12CoreGraphics7CGFloatVSdySbcSgtcfC";
  FixedConstructorFixture(Arch A) : FixedRecordFixture(A) {
    u32(Entry, A == Arch::AArch64 ? 0xd65f03c0 : 0x000000c3);
    Image.Symbols.push_back({Name, Entry, A == Arch::AArch64 ? 4u : 1u, true});
    Image.RuntimeFunctionAddrs.insert(Entry);
  }
  auto declaration() {
    return swiftMangledFixedRecordConstructorDeclaration(Image, Entry);
  }
};
} // namespace

TEST(SwiftFixedRecordConstructor, CompleteDeclarationKeepsAllCarriers) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    FixedConstructorFixture F(A);
    const auto D = F.declaration();
    ASSERT_TRUE(D);
    EXPECT_EQ(D->Storage.MangledType, "4Demo5ValueV");
    ASSERT_EQ(D->Signature.Parameters.size(), 5u);
    ASSERT_EQ(sourceABIParameters(D->Signature).size(), 5u);
    ASSERT_EQ(D->Signature.ReturnComponents.size(), 4u);
    EXPECT_EQ(D->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::SwiftMangled);
    std::string Error;
    EXPECT_TRUE(validateSourceABI(D->Signature, Error)) << Error;
    for (unsigned I = 0; I != 3; ++I) {
      EXPECT_EQ(D->Signature.Parameters[I].Type->Kind, NdTypeKind::Float);
      EXPECT_EQ(D->Signature.Parameters[I].Type->Size, 8u);
      const auto FP = A == Arch::AArch64 ? a64reg::V(I) : x86reg::vectorReg(I);
      EXPECT_EQ(D->Signature.Parameters[I].Location.Kind,
                SourceABICarrierKind::FloatingRegister);
      EXPECT_EQ(D->Signature.Parameters[I].Location.RegisterOffset, FP);
      EXPECT_EQ(D->Signature.ReturnComponents[I].RegisterOffset, FP);
      EXPECT_EQ(D->Signature.ReturnComponents[I].ValueBytes, 8u);
    }
    EXPECT_EQ(D->Signature.Parameters[3].Location.RegisterOffset,
              A == Arch::AArch64 ? a64reg::X0 : x86reg::RDI);
    EXPECT_EQ(D->Signature.Parameters[4].Location.RegisterOffset,
              A == Arch::AArch64 ? a64reg::X1 : x86reg::RSI);
    EXPECT_EQ(D->Signature.ReturnComponents[3].RegisterOffset,
              A == Arch::AArch64 ? a64reg::X0 : x86reg::RAX);
    for (unsigned I = 3; I != 5; ++I)
      EXPECT_EQ(D->Signature.Parameters[I].TheRole,
                SourceParameterTypeHint::Role::Ordinary);
  }
}

TEST(SwiftFixedRecordConstructor,
     RejectsDifferentDeclarationsAndCurrentIdentity) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 43; ++Mutation) {
      SCOPED_TRACE(Mutation);
      FixedConstructorFixture F(A);
      ASSERT_TRUE(F.declaration());
      auto &Name = F.Image.Symbols.back().Name;
      auto Replace = [&](const char *From, const char *To) {
        const auto P = Name.find(From);
        ASSERT_NE(P, std::string::npos);
        Name.replace(P, std::strlen(From), To);
      };
      switch (Mutation) {
      case 0:
        F.Image.Format = BinaryFormat::ELF;
        break;
      case 1:
        F.Image.IsRelocatable = true;
        break;
      case 2:
        F.Image.Bits = Bitness::Bits32;
        break;
      case 3:
        F.Image.Arch = Arch::Unknown;
        break;
      case 4:
        F.Image.MachOTwoLevelNamespace = false;
        break;
      case 5:
        F.Image.Symbols.push_back(F.Image.Symbols.back());
        break;
      case 6: {
        auto Other = F.Image.Symbols.back();
        Other.Addr += 8;
        F.Image.Symbols.push_back(Other);
        break;
      }
      case 7:
        F.Image.Symbols.push_back({"data_alias", F.Entry, 8, false});
        break;
      case 8:
        F.Image.Symbols.back().IsFunc = false;
        break;
      case 9:
        F.Image.Segments[0].Flags =
            F.Image.Segments[0].Flags | SegmentFlags::Writable;
        break;
      case 10:
        F.Image.DataPtrRelocSlots.insert(F.Entry);
        break;
      case 11:
        Name.insert(0, "_");
        break;
      case 12:
        Name += "unknown";
        break;
      case 13:
        Replace("4Demo5ValueV", "4Demo5ValueC");
        break;
      case 14:
        Name += "Tg5";
        break;
      case 15:
        Replace("cfC", "cfc");
        break;
      case 16:
        Replace("tcfC", "tKcfC");
        break;
      case 17:
        Replace("tcfC", "tYacfC");
        break;
      case 18:
        Replace("ySbcSg", "SiSbcSg");
        break;
      case 19:
        Replace("ySbcSg", "ySucSg");
        break;
      case 20:
        Replace("ySbcSg", "ySbc");
        break;
      case 21:
        Replace("ySbcSg", "ySbXCSg");
        break;
      case 22:
        Replace("ySbcSg", "ySbKcSg");
        break;
      case 23:
        Replace("ySbcSg", "ySbYacSg");
        break;
      case 24:
        Replace("ySbcSg", "ySbcSgSg");
        break;
      case 25:
        Replace("VSdy", "VSfy");
        break;
      case 26:
        Replace("12CoreGraphics7CGFloatV", "Sd");
        break;
      case 27:
        Replace("_12CoreGraphics7CGFloatV", "_Sd");
        break;
      case 28:
        Replace("field34Demo5ValueV", "field34Demo5OtherV");
        break;
      case 29:
        Replace("6field1", "6other1");
        break;
      case 30:
        Replace("6field3", "6field4");
        break;
      case 31:
        F.Image.Symbols[2].Name = "other_metadata";
        break;
      case 32:
        F.u64(F.Witnesses + 64, 24);
        break;
      case 33:
        F.text(0x3295, "Sg");
        break;
      case 34:
        F.Image.Sections[2].Type = llvm::MachO::S_THREAD_LOCAL_REGULAR;
        break;
      case 35:
        F.Image.DyldBindSlots[0x6000].WeakImport = true;
        break;
      case 36:
        F.Image.DyldBindSlots[0x6000].Module = "/usr/lib/libUnknown.dylib";
        break;
      case 37:
        F.u32(0x3210, 4);
        break;
      case 38:
        F.Image.Symbols.push_back(F.Image.Symbols[2]);
        break;
      case 39: {
        auto Other = F.Image.Symbols[0];
        Other.Addr += 8;
        F.Image.Symbols.push_back(Other);
        break;
      }
      case 40:
        F.Image.Exports.push_back({"different_function", 0, F.Entry});
        break;
      case 41:
        F.Image.Exports.push_back({Name, 0, F.Entry + 8});
        break;
      case 42:
        Replace("ySbcSg", "ySbXBSg");
        break;
      }
      EXPECT_FALSE(F.declaration());
    }
}

TEST(SwiftFixedRecordConstructor,
     IndependentFieldTypesAndLiveMetadataStayRequired) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    FixedConstructorFixture F(A);
    ASSERT_TRUE(F.declaration());
    auto &Name = F.Image.Symbols.back().Name;
    for (unsigned I = 0; I != 2; ++I) {
      const auto P = Name.find("12CoreGraphics7CGFloatV");
      ASSERT_NE(P, std::string::npos);
      Name.replace(P, std::strlen("12CoreGraphics7CGFloatV"), "Sd");
      EXPECT_FALSE(F.declaration());
      F.relative(0x3214 + I * 12, 0x3280);
      ASSERT_TRUE(F.declaration());
    }
    const auto Original = F.declaration();
    ASSERT_TRUE(Original);
    F.u32(F.Metadata + 28, 32);
    EXPECT_FALSE(F.declaration());
    F.u32(F.Metadata + 28, 24);
    ASSERT_TRUE(F.declaration());
    F.Image.Symbols.back().Name += "TA";
    EXPECT_FALSE(F.declaration());
    EXPECT_EQ(Original->Signature.Parameters.size(), 5u);
  }
  FixedConstructorFixture F(Arch::AArch64);
  F.Image.Symbols.back().Addr = F.Entry + 1;
  F.Image.RuntimeFunctionAddrs.insert(F.Entry + 1);
  EXPECT_FALSE(
      swiftMangledFixedRecordConstructorDeclaration(F.Image, F.Entry + 1));
}

// A linked, independently named constructor whose machine body forwards all
// three FP lanes and the first opaque input. It exercises current re-lifting;
// its declaration alone cannot certify either a changed body or caller.
namespace {
PipelineResult constructorPipeline(FixedConstructorFixture &F,
                                   llvm::LLVMContext &Context,
                                   bool WithCaller = false) {
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Entry};
  if (WithCaller) {
    constexpr va_t Caller = 0x1100;
    F.segment(Caller, 0x100, "__text_caller", true);
    // An observable external store keeps this ordinary call in the pipeline;
    // the empty identity leaf is intentionally eligible for leaf elimination.
    F.u32(F.Entry, 0xf9000020); // str x0, [x1]
    F.u32(F.Entry + 4, 0xd65f03c0);
    for (auto &S : F.Image.Symbols)
      if (S.Addr == F.Entry)
        S.Size = 8;
    const uint32_t Words[] = {0xa9bf7bfd, 0x910003fd, 0x97ffffde, 0xa8c17bfd,
                              0xd65f03c0};
    for (unsigned I = 0; I != std::size(Words); ++I)
      F.u32(Caller + 4 * I, Words[I]);
    F.Image.Symbols.push_back({"caller", Caller, sizeof(Words), true});
    F.Image.RuntimeFunctionAddrs.insert(Caller);
    Options.OnlyFunctionEntries.insert(Caller);
  }
  auto R = Pipeline().run(F.Image, Context, Options);
  std::map<va_t, std::string> Diagnostics;
  neverd::sdk::inferObjCNativeDependencies(F.Image, R, Options, Diagnostics,
                                           {F.Entry});
  return Pipeline().run(F.Image, Context, Options);
}
} // namespace

TEST(SwiftFixedRecordConstructor, CurrentEntryAndSourceReplayKeepFourResults) {
  FixedConstructorFixture F(Arch::AArch64);
  llvm::LLVMContext Context;
  auto R = constructorPipeline(F, Context);
  ASSERT_TRUE(R.Success);
  const auto Current =
      neverd::sdk::native_source_detail::currentFunction(R, F.Entry);
  ASSERT_TRUE(Current);
  ASSERT_TRUE(swiftFixedRecordConstructorEntryABI(F.Image, *Current->Low));
  ASSERT_EQ(Current->Med->SourceTypeHint->Parameters.size(), 5u);
  ASSERT_EQ(Current->Med->SourceTypeHint->ReturnComponents.size(), 4u);
  const auto B = neverd::sdk::bindObjCSourceReferences(*Current->High, F.Image);
  ASSERT_TRUE(B.Limitation.empty()) << B.Limitation;
  EXPECT_TRUE(
      neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
          .valid(B.Function));
}

TEST(SwiftFixedRecordConstructor, OriginalCallRequiresCurrentCompleteCallee) {
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FixedConstructorFixture F(Arch::AArch64);
    llvm::LLVMContext Context;
    auto R = constructorPipeline(F, Context, true);
    ASSERT_TRUE(R.Success);
    MedFunc *Caller = nullptr;
    LowFunc *Low = nullptr;
    for (auto &M : R.MedFuncs)
      if (M.Entry == 0x1100)
        Caller = &M;
    for (auto &L : R.LowFuncs)
      if (L.Entry == 0x1100)
        Low = &L;
    ASSERT_TRUE(Caller && Low);
    MedOp *Call = nullptr;
    for (auto &B : Caller->Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::CALL && O.Addr == 0x1108)
          Call = &O;
    ASSERT_TRUE(Call && Call->SourceCallHint);
    ASSERT_TRUE(Call->SourceCallHint->SwiftValueConstructor);
    EXPECT_TRUE(Call->SourceCallHint->requiresUniqueSourceOccurrence());
    auto Contracts = nativeSourceCalleeContracts(F.Image, R);
    ASSERT_TRUE(validateSwiftValueConstructorBindings(F.Image, Low, *Caller,
                                                      &Contracts));
    auto ChangedHint =
        std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = ChangedHint;
    switch (Mutation) {
    case 0:
      ChangedHint->SwiftValueConstructor.reset();
      break;
    case 1:
      Call->Output.Size = 8;
      break;
    case 2:
      Call->NumInputs = 3;
      break;
    case 3:
      Call->Inputs[1].Size = 4;
      break;
    case 4:
      Call->OriginSeq += 1;
      break;
    case 5:
      ChangedHint->Signature.ReturnComponents.pop_back();
      break;
    case 6:
      Contracts.CurrentCallees.erase(F.Entry);
      break;
    case 7:
      Contracts.CurrentCallees.at(F.Entry).Audit = nullptr;
      break;
    case 8:
      F.u32(F.Entry, 0xaa0103e0);
      break;
    case 9:
      F.Image.Symbols.back().Size = 4;
      F.u32(0x1108, 0x94000001);
      break;
    case 10:
      Call->PreservesCallerSaved = true;
      break;
    case 11:
      for (auto &L : R.LowFuncs)
        if (L.Entry == F.Entry)
          L.Blocks[0].Ops[0].Addr += 4;
      break;
    case 12:
      Caller->Blocks[0].Ops.push_back(*Call);
      break;
    case 13:
      ChangedHint->SwiftValueConstructor.reset();
      F.u32(F.Metadata + 16, 8);
      break;
    }
    EXPECT_FALSE(validateSwiftValueConstructorBindings(F.Image, Low, *Caller,
                                                       &Contracts));
  }
}

TEST(SwiftFixedRecordConstructor, PublicationRebuildsCurrentEntryAndWholeBody) {
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FixedConstructorFixture F(Arch::AArch64);
    llvm::LLVMContext Context;
    auto R = constructorPipeline(F, Context);
    const auto C =
        neverd::sdk::native_source_detail::currentFunction(R, F.Entry);
    ASSERT_TRUE(C);
    auto B = neverd::sdk::bindObjCSourceReferences(*C->High, F.Image);
    ASSERT_TRUE(
        neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
            .valid(B.Function));
    switch (Mutation) {
    case 0:
      B.Function.SourceTypeHint->ReturnComponents.pop_back();
      break;
    case 1:
      B.Function.Params.pop_back();
      break;
    case 2:
      B.Function.Params[0].Type = NdType::makeFloat(4);
      break;
    case 3:
      B.Function.Body.clear();
      break;
    case 4:
      B.Function.FrameSize += 16;
      break;
    case 5:
      B.Function.Body.push_back(B.Function.Body.back());
      break;
    case 6:
      R.MedFuncs[0].Blocks[0].Ops.clear();
      break;
    case 7:
      R.FunctionAudits[0].MedIRVerified = false;
      break;
    case 8:
      F.u32(F.Entry, 0xaa0103e0);
      break;
    case 9:
      F.u32(F.Metadata + 16, 8);
      break;
    case 10:
      B.Function.SourceTypeHint->Parameters[3].Location.RegisterOffset += 8;
      break;
    case 11:
      B.Function.Body.back().RetVal = HighExpr::makeConst(0, 8);
      break;
    }
    EXPECT_FALSE(
        neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
            .valid(B.Function));
  }
}

TEST(SwiftFixedRecordConstructor,
     X64DeclarationDoesNotBypassARM64MachineReplay) {
  FixedConstructorFixture F(Arch::X64);
  F.u32(F.Entry, 0xc3f88948); // mov rdi, rax; ret
  for (auto &S : F.Image.Symbols)
    if (S.Addr == F.Entry)
      S.Size = 4;
  auto D = F.declaration();
  ASSERT_TRUE(D);
  ASSERT_EQ(D->Signature.ReturnComponents.size(), 4u);
  EXPECT_EQ(D->Signature.ReturnComponents[3].RegisterOffset, x86reg::RAX);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Entry};
  Options.SourceTypeHints.emplace(F.Entry, D->Signature);
  auto R = Pipeline().run(F.Image, Context, Options);
  const auto C = neverd::sdk::native_source_detail::currentFunction(R, F.Entry);
  ASSERT_TRUE(C);
  EXPECT_FALSE(swiftFixedRecordConstructorEntryABI(F.Image, *C->Low));
  const auto B = neverd::sdk::bindObjCSourceReferences(*C->High, F.Image);
  ASSERT_TRUE(B.Limitation.empty()) << B.Limitation;
  EXPECT_FALSE(
      neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
          .valid(B.Function));
}

TEST(SwiftFixedRecordConstructor,
     CallerPublicationReplaysArgumentsAndOccurrence) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FixedConstructorFixture F(Arch::AArch64);
    F.segment(0x1100, 0x100, "__text_caller", true);
    F.u32(F.Entry, 0xf9000020);
    F.u32(F.Entry + 4, 0xd65f03c0);
    for (auto &S : F.Image.Symbols)
      if (S.Addr == F.Entry)
        S.Size = 8;
    const uint32_t Words[] = {0xa9bf7bfd, 0x910003fd, 0xaa0003e1, 0xaa1f03e0,
                              0x9e6703e0, 0x9e6703e1, 0x9e6703e2, 0x97ffffd9,
                              0xaa1f03e0, 0xa8c17bfd, 0xd65f03c0};
    for (unsigned I = 0; I != std::size(Words); ++I)
      F.u32(0x1100 + I * 4, Words[I]);
    F.Image.Symbols.push_back({"caller", 0x1100, sizeof(Words), true});
    F.Image.RuntimeFunctionAddrs.insert(0x1100);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {F.Entry, 0x1100};
    PipelineResult R;
    for (unsigned Round = 0; Round != 4; ++Round) {
      R = Pipeline().run(F.Image, Context, Options);
      std::map<va_t, std::string> D;
      if (!neverd::sdk::inferObjCNativeDependencies(
              F.Image, R, Options, D, Options.OnlyFunctionEntries))
        break;
    }
    const auto C =
        neverd::sdk::native_source_detail::currentFunction(R, 0x1100);
    ASSERT_TRUE(C);
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &H : R.HighFuncs)
      Functions.emplace(H.Entry, &H);
    auto B = neverd::sdk::bindObjCSourceReferences(*C->High, F.Image, nullptr,
                                                   &Functions);
    ASSERT_TRUE(B.Limitation.empty()) << B.Limitation;
    ASSERT_TRUE(
        neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
            .valid(B.Function));
    HighExpr *Call = nullptr;
    walkStmts(B.Function.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        std::vector<ExprPtr> Pending{E};
        while (!Pending.empty()) {
          auto V = Pending.back();
          Pending.pop_back();
          if (!V)
            continue;
          if (V->SourceCallHint && V->SourceCallHint->SwiftValueConstructor)
            Call = V.get();
          V->forEachChildExpr([&](const ExprPtr &C) { Pending.push_back(C); });
        }
      });
    });
    ASSERT_TRUE(Call);
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = Hint;
    switch (Mutation) {
    case 0:
      Hint->SwiftValueConstructor.reset();
      break;
    case 1:
      Call->Operands[0] = HighExpr::makeConst(7, 8);
      break;
    case 2:
      Call->Operands[4] = HighExpr::makeConst(0, 8);
      break;
    case 3:
      Call->Operands.pop_back();
      break;
    case 4:
      Hint->Signature.ReturnComponents.pop_back();
      break;
    case 5:
      Call->Type = NdType::makeInt(8);
      break;
    case 6:
      Hint->SwiftValueConstructor->Site.Sequence += 1;
      break;
    case 8:
      Hint->SwiftValueConstructor.reset();
      F.u32(F.Metadata + 16, 8);
      for (auto &M : R.MedFuncs)
        for (auto &Block : M.Blocks)
          for (auto &O : Block.Ops)
            if (O.SourceCallHint && O.SourceCallHint->SwiftValueConstructor) {
              auto H = std::make_shared<SourceCallTypeHint>(*O.SourceCallHint);
              H->SwiftValueConstructor.reset();
              O.SourceCallHint = H;
            }
      break;
    case 7: {
      HighStmt Extra;
      Extra.Kind = StmtKind::Call;
      Extra.CallExpr = std::make_shared<HighExpr>(*Call);
      B.Function.Body.insert(B.Function.Body.begin(), Extra);
      break;
    }
    }
    EXPECT_FALSE(
        neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
            .valid(B.Function));
  }
}

namespace {
struct ConstructorMessageFixture : FixedConstructorFixture {
  static constexpr va_t Caller = 0x1100, Stub = 0x1300;
  static constexpr va_t Message = Caller + 44, Selector = 0x3c00;
  ConstructorMessageFixture() : FixedConstructorFixture(Arch::AArch64) {
    segment(Caller, 0x100, "__text_caller", true);
    segment(Stub, 0x20, "__objc_stubs", true);
    u32(Entry, 0xf9000020); // str x0, [x1]
    u32(Entry + 4, 0xd65f03c0);
    for (auto &S : Image.Symbols)
      if (S.Addr == Entry)
        S.Size = 8;
    const uint32_t Words[] = {
        0xa9be7bfd,
        0xa90153f3,
        0x910003fd,
        0xaa0103f4,
        0xaa0003e1,
        0xaa1f03e0,
        0x9e6703e0,
        0x9e6703e1,
        0x9e6703e2,
        0x94000000 | uint32_t(((Entry - (Caller + 36)) / 4) & 0x03ffffff),
        0xaa1403e0,
        0x94000000 | uint32_t(((Stub - Message) / 4) & 0x03ffffff),
        0xaa1f03e0,
        0xa94153f3,
        0xa8c27bfd,
        0xd65f03c0};
    for (unsigned I = 0; I != std::size(Words); ++I)
      u32(Caller + 4 * I, Words[I]);
    // ADRP/LDR selector, ADRP/LDR strong objc_msgSend slot, BR x16.
    const uint32_t StubWords[] = {0xd0000001, 0xf9460021, 0xb0000030,
                                  0xf9400610, 0xd61f0200};
    for (unsigned I = 0; I != std::size(StubWords); ++I)
      u32(Stub + 4 * I, StubWords[I]);
    Image.Symbols.push_back({"caller", Caller, sizeof(Words), true});
    Image.RuntimeFunctionAddrs.insert(Caller);
    Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
    Image.DynInfo.NeededLibs.push_back(
        "/System/Library/Frameworks/QuartzCore.framework/QuartzCore");
    EXPECT_TRUE(Image.recordDyldBindSlot(0x6008, "_objc_msgSend", 0,
                                         "/usr/lib/libobjc.A.dylib", false));
    Image.ImportPtrSlots[0x6008] = "_objc_msgSend";
    text(0x3c60, "setNeedsDisplay");
    pointer(Selector, 0x3c60);
    Image.ObjCSourceReferences[Selector] = {ObjCSourceReference::Kind::Selector,
                                            Selector, 8, "setNeedsDisplay"};
  }
  PipelineResult run(llvm::LLVMContext &Context) {
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry, Caller};
    PipelineResult R;
    for (unsigned Round = 0; Round != 4; ++Round) {
      R = Pipeline().run(Image, Context, Options);
      std::map<va_t, std::string> D;
      if (!sdk::inferObjCNativeDependencies(Image, R, Options, D,
                                            Options.OnlyFunctionEntries))
        break;
    }
    return R;
  }
};
} // namespace

TEST(SwiftFixedRecordConstructor,
     MessageReplayUsesCurrentDispatchAndArguments) {
  ConstructorMessageFixture F;
  const auto Stub = objcSelectorStubSourceCallHint(F.Image, F.Stub);
  ASSERT_TRUE(Stub);
  ASSERT_EQ(Stub->Selector, "setNeedsDisplay");
  llvm::LLVMContext Context;
  auto R = F.run(Context);
  const auto C = sdk::native_source_detail::currentFunction(R, F.Caller);
  ASSERT_TRUE(C);
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &H : R.HighFuncs)
    Functions.emplace(H.Entry, &H);
  auto B =
      sdk::bindObjCSourceReferences(*C->High, F.Image, nullptr, &Functions);
  ASSERT_TRUE(B.Limitation.empty()) << B.Limitation;
  unsigned Messages = 0;
  walkStmts(B.Function.Body, [&](const HighStmt &S) {
    if (S.CallExpr && S.CallExpr->SourceCallHint &&
        S.CallExpr->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::ObjCMessage) {
      ++Messages;
      EXPECT_EQ(S.Addr, F.Message);
      EXPECT_TRUE(sdk::objcSourceCallBound(*S.CallExpr, F.Image, Functions,
                                           nullptr, nullptr, &B.Function));
    }
  });
  ASSERT_EQ(Messages, 1u);
  EXPECT_TRUE(sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
                  .valid(B.Function));
}

TEST(SwiftFixedRecordConstructor, MessageReplayRejectsChangedCurrentEvidence) {
  for (unsigned Mutation = 0; Mutation != 36; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ConstructorMessageFixture F;
    llvm::LLVMContext Context;
    auto R = F.run(Context);
    const auto C = sdk::native_source_detail::currentFunction(R, F.Caller);
    ASSERT_TRUE(C);
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &H : R.HighFuncs)
      Functions.emplace(H.Entry, &H);
    auto B =
        sdk::bindObjCSourceReferences(*C->High, F.Image, nullptr, &Functions);
    ASSERT_TRUE(B.Limitation.empty()) << B.Limitation;
    ASSERT_TRUE(sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
                    .valid(B.Function));
    HighStmt *Statement = nullptr;
    walkStmts(B.Function.Body, [&](HighStmt &S) {
      if (S.Addr == F.Message && S.CallExpr)
        Statement = &S;
    });
    ASSERT_TRUE(Statement);
    Statement->CallExpr = std::make_shared<HighExpr>(*Statement->CallExpr);
    auto &Call = *Statement->CallExpr;
    auto H = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Call.SourceCallHint = H;
    MedOp *Med = nullptr;
    LowOp *Low = nullptr;
    for (auto &M : R.MedFuncs)
      if (M.Entry == F.Caller)
        for (auto &Block : M.Blocks)
          for (auto &O : Block.Ops)
            if (O.Addr == F.Message && O.Opcode == NdOp::CALL)
              Med = &O;
    for (auto &L : R.LowFuncs)
      if (L.Entry == F.Caller)
        for (auto &Block : L.Blocks)
          for (auto &O : Block.Ops)
            if (O.Addr == F.Message && O.Opcode == NdOp::CALL)
              Low = &O;
    ASSERT_TRUE(Med && Low);
    switch (Mutation) {
    case 0:
      Call.Operands[0] = HighExpr::makeConst(0, 8);
      break;
    case 1:
      Call.Operands[1] = HighExpr::makeConst(0, 8);
      break;
    case 2:
      Call.CallAddr += 4;
      break;
    case 3:
      H->Selector = "removeAllAnimations";
      break;
    case 4:
      H->SelectorReferenceAddress += 8;
      break;
    case 5:
      H->Signature.Parameters[0].Location.RegisterOffset += 8;
      break;
    case 6:
      Call.SourceCallHint.reset();
      break;
    case 7:
      B.Function.Body.push_back(*Statement);
      break;
    case 8:
      Statement->CallExpr.reset();
      break;
    case 9:
      Statement->Addr += 4;
      break;
    case 10:
      H->CallKind = SourceCallTypeHint::Kind::Native;
      break;
    case 11:
      Call.IsIndirectCall = true;
      break;
    case 12:
      Call.Type = NdType::makeInt(8);
      break;
    case 13:
      Call.Operands.pop_back();
      break;
    case 14:
      Call.Operands[0] = HighExpr::makeConst(0, 4);
      break;
    case 15:
      H->WeakImport = true;
      break;
    case 16:
      H->DoesNotReturn = true;
      break;
    case 17:
      H->ReturnedArgument = 0;
      break;
    case 18:
      Med->Inputs[1].Size = 4;
      break;
    case 19:
      Med->NumInputs = 2;
      break;
    case 20:
      ++Med->OriginSeq;
      break;
    case 21:
      Med->Addr += 4;
      break;
    case 22:
      Med->SourceCallHint.reset();
      break;
    case 23:
      Med->PreservesCallerSaved = true;
      break;
    case 24:
      Low->Addr += 4;
      break;
    case 25:
      F.u32(F.Stub + 16, 0xd503201f);
      break;
    case 26:
      F.Image.DyldBindSlots[0x6008].Module = "wrong";
      break;
    case 27:
      F.Image.DyldBindSlots[0x6008].WeakImport = true;
      break;
    case 28:
      F.Image.ObjCSourceReferences[F.Selector].Name = "removeAllAnimations";
      break;
    case 29:
      F.u32(F.Message - 4, 0xaa1503e0);
      break;
    case 30:
      Med->SourceCallHint.reset();
      Call.SourceCallHint.reset();
      break;
    case 31:
      F.Image.Sections[3].Type = llvm::MachO::S_THREAD_LOCAL_VARIABLES;
      break;
    case 32:
      setMappedSectionFlags(F.Image, F.Image.Sections.back(),
                            F.Image.Sections.back().Flags |
                                SegmentFlags::Writable);
      break;
    case 33:
      F.text(0x3c60, "removeAllAnimations");
      break;
    case 34:
      F.Image.DataPtrRelocSlots.erase(F.Selector);
      break;
    case 35:
      F.Image.Sections.push_back(F.Image.Sections.back());
      break;
    }
    EXPECT_FALSE(sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
                     .valid(B.Function));
  }
}

TEST(SwiftFixedRecordConstructor,
     MessageIdentityKeepsExactSelectorPointerOwner) {
  for (unsigned Mutation = 0; Mutation != 22; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ConstructorMessageFixture F;
    ASSERT_TRUE(objcImmutableSelectorStubSourceCallHint(F.Image, F.Stub));
    ASSERT_TRUE(readInitialImageSelectorPointer(F.Image, F.Selector));
    // Ordinary pointers still reject a selector metadata record; admitting
    // this one exact record must not suppress other conflicting fixups.
    EXPECT_FALSE(readInitialImagePointer(F.Image, F.Selector));
    const uint32_t TLS[] = {llvm::MachO::S_THREAD_LOCAL_REGULAR,
                            llvm::MachO::S_THREAD_LOCAL_ZEROFILL,
                            llvm::MachO::S_THREAD_LOCAL_VARIABLES,
                            llvm::MachO::S_THREAD_LOCAL_VARIABLE_POINTERS,
                            llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS};
    if (Mutation < 5)
      F.Image.Sections[3].Type = TLS[Mutation];
    else if (Mutation < 10)
      F.Image.Sections[1].Type = TLS[Mutation - 5];
    else
      switch (Mutation) {
      case 10:
        F.Image.DataPtrRelocTargetOwners[F.Selector] += 1;
        break;
      case 11:
        F.Image.MachOResolvedChainedPointerSlots.erase(F.Selector);
        break;
      case 12:
        F.Image.ObjCSourceReferences[F.Selector].Address += 8;
        break;
      case 13:
        F.Image.ObjCSourceReferences[F.Selector].Size = 4;
        break;
      case 14:
        F.Image.ObjCSourceReferences[F.Selector].TheKind =
            ObjCSourceReference::Kind::Class;
        break;
      case 15:
        F.Image.DataPtrRelocSlots.insert(F.Selector + 4);
        break;
      case 16:
        F.Image.CodePtrRelocSlots.insert(F.Selector);
        break;
      case 17:
        F.Image.DyldBindSlots[F.Selector] = {"_alias", 0, "bad", false};
        break;
      case 18:
        F.Image.ObjCSourceReferences[F.Selector + 4] =
            F.Image.ObjCSourceReferences[F.Selector];
        break;
      case 19:
        setMappedSectionFlags(F.Image, F.Image.Sections[3],
                              F.Image.Sections[3].Flags |
                                  SegmentFlags::Writable);
        break;
      case 20:
        F.Image.Sections.push_back(F.Image.Sections[1]);
        break;
      case 21:
        F.Image.Sections[3].Type = llvm::MachO::S_LAZY_SYMBOL_POINTERS;
        break;
      }
    EXPECT_FALSE(objcImmutableSelectorStubSourceCallHint(F.Image, F.Stub));
  }
}

TEST(SwiftFixedRecordConstructor,
     MessageIdentityRejectsTLSCodeAndSelectorText) {
  const uint32_t TLS[] = {llvm::MachO::S_THREAD_LOCAL_REGULAR,
                          llvm::MachO::S_THREAD_LOCAL_ZEROFILL,
                          llvm::MachO::S_THREAD_LOCAL_VARIABLES,
                          llvm::MachO::S_THREAD_LOCAL_VARIABLE_POINTERS,
                          llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS};
  for (bool Code : {false, true})
    for (const auto Type : TLS) {
      SCOPED_TRACE(Code);
      SCOPED_TRACE(Type);
      ConstructorMessageFixture F;
      size_t Index = F.Image.Sections.size() - 1;
      if (!Code) {
        auto Text = F.Image.Sections[1];
        Text.VA = Text.FileOff = 0x3c60;
        Text.Size = Text.FileSz = 0x20;
        Text.Type = llvm::MachO::S_CSTRING_LITERALS;
        F.Image.Sections[1].Size = F.Image.Sections[1].FileSz = 0xc60;
        Index = F.Image.Sections.size();
        F.Image.Sections.push_back(Text);
        F.Image.DataPtrRelocTargetOwners[F.Selector] = Text.VA;
      }
      ASSERT_TRUE(objcImmutableSelectorStubSourceCallHint(F.Image, F.Stub));
      F.Image.Sections[Index].Type = Type;
      EXPECT_FALSE(objcImmutableSelectorStubSourceCallHint(F.Image, F.Stub));
    }
}
