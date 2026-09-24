#include "../../../lib/pipeline/NativeSourcePreservation.h"
#include "../../../lib/sdk/capi/ObjCSourceProjection.h"
#include "../../../lib/sdk/capi/SwiftMangledSourceABI.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedSourceParameterUses.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/BinaryFormat/MachO.h"

#include <algorithm>
#include <array>
#include <functional>

using namespace neverd;

TEST(NativeSourceHints, ExactMangledStringBundleFunctionKeepsPairResult) {
  constexpr llvm::StringLiteral Name =
      "_$s4main9localized_12languageCode6bundle5value7commentS2S_SSSgSo8"
      "NSBundleCSgS2StF";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    BinaryImage Image;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100);
    Image.Segments.push_back(std::move(Text));
    Image.Symbols.push_back({Name.str(), 0x1000, 0, true});
    const auto Hint =
        sdk::swiftMangledStringBundleSourceABI(Image, 0x1000, true);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
    EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
    EXPECT_EQ(Hint->ReturnType->Size, 16U);
    ASSERT_EQ(Hint->ReturnComponents.size(), 2U);
    ASSERT_EQ(Hint->Parameters.size(), 9U);
    EXPECT_EQ(Hint->Parameters[8].Location.Kind, SourceABICarrierKind::Stack);
    std::string Error;
    EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;
    EXPECT_FALSE(sdk::swiftMangledStringBundleSourceABI(Image, 0x1000, false));
    auto Wrong = Image;
    Wrong.Symbols[0].Name =
        "_$s4main9localized_12languageCode6bundle5value7commentS2S_SSSgSo8"
        "NSObjectCSgS2StF";
    EXPECT_FALSE(sdk::swiftMangledStringBundleSourceABI(Wrong, 0x1000, true));
    Wrong = Image;
    Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
    EXPECT_FALSE(sdk::swiftMangledStringBundleSourceABI(Wrong, 0x1000, true));
  }
}

TEST(NativeSourceHints, ObjCExtensionBoolGetterUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$sSo8NSBundleC3WMFE14isAppExtensionSbvg", 0x1000, 0, true});
  const auto Hint = sdk::swiftMangledObjCBoolMemberSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Size, 1U);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Method = Image;
  Method.Symbols[0].Name =
      "_$sSo14NSUserDefaultsC3WMFE34wmf_isTableOfContentsVisibleInlineSbyF";
  const auto MethodHint =
      sdk::swiftMangledObjCBoolMemberSourceABI(Method, 0x1000);
  ASSERT_TRUE(MethodHint);
  ASSERT_EQ(MethodHint->Parameters.size(), 1U);
  EXPECT_EQ(MethodHint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(MethodHint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  EXPECT_EQ(MethodHint->ReturnType->Size, 1U);
  EXPECT_TRUE(validateSourceABI(*MethodHint, Error)) << Error;
  Method.Symbols[0].Name =
      "_$sSo14NSUserDefaultsC3WMFE34wmf_isTableOfContentsVisibleInlineSiyF";
  EXPECT_FALSE(sdk::swiftMangledObjCBoolMemberSourceABI(Method, 0x1000));

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo8NSBundleC3WMFE14isAppExtensionSbvp";
  EXPECT_FALSE(sdk::swiftMangledObjCBoolMemberSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo8NSBundleC3WMFE14isAppExtensionSSvg";
  EXPECT_FALSE(sdk::swiftMangledObjCBoolMemberSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledObjCBoolMemberSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledObjCBoolMemberSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, SwiftObjCObjectVoidMethodUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF11ActionsViewC17willPerformActionyySo8UIButtonCF", 0x1000, 0,
       true});
  const auto Hint =
      sdk::swiftMangledObjCObjectVoidMethodSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name += "ToTm";
  EXPECT_FALSE(sdk::swiftMangledObjCObjectVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF11ActionsViewC17willPerformActionyySiF";
  EXPECT_FALSE(sdk::swiftMangledObjCObjectVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledObjCObjectVoidMethodSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, ObjCObjectPairVoidMethodUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back({"_$s3WMF18CollectionViewCellC19setBackgroundColors_"
                           "8selectedySo7UIColorC_AGtF",
                           0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 3U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X1);
  EXPECT_EQ(Hint->Parameters[2].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[2].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF18CollectionViewCellC19setBackgroundColors_8selectedySS_SStF";
  EXPECT_FALSE(
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF18CollectionViewCellC19setBackgroundColors_"
                          "8selectedSbSo7UIColorC_AGtF";
  EXPECT_FALSE(
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "Z";
  EXPECT_FALSE(
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(
      sdk::swiftMangledObjCObjectPairVoidMethodSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, UIColorIntAlphaAllocatorUsesMixedSwiftRegisters) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$sSo7UIColorC13WMFComponentsE_5alphaABSi_12CoreGraphics7CGFloatVtcfC",
       0x1000, 0, true});
  const auto Hint =
      sdk::swiftMangledUIColorIntAlphaAllocatorSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  ASSERT_EQ(Hint->Parameters.size(), 3U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::V0);
  EXPECT_EQ(Hint->Parameters[2].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[2].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo7UIColorC13WMFComponentsE_5alphaABSi_SitcfC";
  EXPECT_FALSE(
      sdk::swiftMangledUIColorIntAlphaAllocatorSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$sSo7NSColorC13WMFComponentsE_5alphaABSi_12CoreGraphics7CGFloatVtcfC";
  EXPECT_FALSE(
      sdk::swiftMangledUIColorIntAlphaAllocatorSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$sSo7UIColorC13WMFComponentsE_5alphaABSi_12CoreGraphics7CGFloatVtcfc";
  EXPECT_FALSE(
      sdk::swiftMangledUIColorIntAlphaAllocatorSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(
      sdk::swiftMangledUIColorIntAlphaAllocatorSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, ZeroArgClassVoidAndBoolMethodsUseSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF15LocationManagerC014stopMonitoringB0yyF", 0x1000, 0, true});
  const auto Hint = sdk::swiftMangledZeroArgClassMethodSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Private = Image;
  Private.Symbols[0].Name = "_$s3WMF18AlignedImageButtonC12adjustInsets33_"
                            "2AE2377B0A7FC7A2DEBED24238BE2C37LLyyF";
  const auto PrivateHint =
      sdk::swiftMangledZeroArgClassMethodSourceABI(Private, 0x1000);
  ASSERT_TRUE(PrivateHint);
  ASSERT_EQ(PrivateHint->Parameters.size(), 1U);
  EXPECT_EQ(PrivateHint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*PrivateHint, Error)) << Error;

  auto Boolean = Image;
  Boolean.Symbols[0].Name =
      "_$s7WMFData21WMFHomeDataControllerC28communityFeaturedArticleIsOnSbyF";
  const auto BooleanHint =
      sdk::swiftMangledZeroArgClassMethodSourceABI(Boolean, 0x1000);
  ASSERT_TRUE(BooleanHint);
  EXPECT_EQ(BooleanHint->ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(BooleanHint->ReturnType->Size, 1U);
  ASSERT_EQ(BooleanHint->Parameters.size(), 1U);
  EXPECT_EQ(BooleanHint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*BooleanHint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF15LocationManagerC014stopMonitoringB0SiyF";
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "Z";
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name = "_$sSo15LocationManagerC3WMFE014stopMonitoringB0yyF";
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassMethodSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassMethodSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, SwiftClassScalarSetterUsesValueAndSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF25ArticleCollectionViewCellC10isSelectedSbvs", 0x1000, 0, true});
  const auto Hint = sdk::swiftMangledClassScalarSetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[0].Type->Size, 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Parameters[1].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Integer = Image;
  Integer.Symbols[0].Name = "_$s6Lottie8LRUCacheC10countLimitSivs";
  const auto IntegerHint =
      sdk::swiftMangledClassScalarSetterSourceABI(Integer, 0x1000);
  ASSERT_TRUE(IntegerHint);
  EXPECT_EQ(IntegerHint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(IntegerHint->Parameters.size(), 2U);
  EXPECT_EQ(IntegerHint->Parameters[0].Type->Size, 8U);
  EXPECT_EQ(IntegerHint->Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(IntegerHint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*IntegerHint, Error)) << Error;

  auto CGFloat = Image;
  CGFloat.Symbols[0].Name =
      "_$s6Lottie23CompatibleAnimationViewC04loopC5Count12CoreGraphics7CGFloatVvs";
  const auto CGFloatHint =
      sdk::swiftMangledClassScalarSetterSourceABI(CGFloat, 0x1000);
  ASSERT_TRUE(CGFloatHint);
  EXPECT_EQ(CGFloatHint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(CGFloatHint->Parameters.size(), 2U);
  EXPECT_EQ(CGFloatHint->Parameters[0].Type->Kind, NdTypeKind::Float);
  EXPECT_EQ(CGFloatHint->Parameters[0].Type->Size, 8U);
  EXPECT_EQ(CGFloatHint->Parameters[0].Location.Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(CGFloatHint->Parameters[0].Location.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).FPParamRegs[0]);
  EXPECT_EQ(CGFloatHint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  EXPECT_TRUE(validateSourceABI(*CGFloatHint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name = "_$s3WMF25ArticleCollectionViewCellC10isSelectedSbvg";
  EXPECT_FALSE(sdk::swiftMangledClassScalarSetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF25ArticleCollectionViewCellC17horizontalSpacingSdvs";
  EXPECT_FALSE(sdk::swiftMangledClassScalarSetterSourceABI(Wrong, 0x1000));
  Wrong = CGFloat;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledClassScalarSetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledClassScalarSetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledClassScalarSetterSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, SwiftClassScalarGetterUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF24WMFAuthenticationManagerC20authStateIsTemporarySbvg",
       0x1000, 0, true});
  auto Hint = sdk::swiftMangledClassScalarGetterSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Hint->ReturnType->Size, 1U);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto CGFloat = Image;
  CGFloat.Symbols[0].Name =
      "_$s3WMF25ArticleCollectionViewCellC24swipeTranslationWhenOpen12CoreGraphics7CGFloatVvg";
  Hint = sdk::swiftMangledClassScalarGetterSourceABI(CGFloat, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint->ReturnType->Size, 8U);
  EXPECT_EQ(Hint->ReturnLocation.Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).FPReturnReg);
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = CGFloat;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledClassScalarGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name.back() = 's';
  EXPECT_FALSE(sdk::swiftMangledClassScalarGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledClassScalarGetterSourceABI(Wrong, 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledClassScalarGetterSourceABI(Wrong, 0x1000));
}

TEST(NativeSourceHints, ZeroArgClassInitializerUsesSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF31WMFArticlePreviewViewControllerCACycfc", 0x1000, 0,
       true});
  const auto Hint =
      sdk::swiftMangledZeroArgClassInitializerSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Ptr);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF31WMFArticlePreviewViewControllerCACycfC";
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassInitializerSourceABI(Wrong,
                                                                  0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF31WMFArticlePreviewViewControllerC5coderACSo7NSCoderC_tcfc";
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassInitializerSourceABI(Wrong,
                                                                  0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledZeroArgClassInitializerSourceABI(Wrong,
                                                                  0x1000));
}

TEST(NativeSourceHints, CGRectClassInitializerUsesFourFPLanesAndSwiftSelf) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(
      {"_$s3WMF11ActionsViewC5frameACSo6CGRectV_tcfc", 0x1000, 0,
       true});
  const auto Hint =
      sdk::swiftMangledCGRectClassInitializerSourceABI(Image, 0x1000);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::SwiftMangled);
  EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  ASSERT_EQ(Hint->Parameters[0].Components.size(), 4U);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

  auto Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF11ActionsViewC5frameACSo6CGRectV_tcfC";
  EXPECT_FALSE(sdk::swiftMangledCGRectClassInitializerSourceABI(Wrong,
                                                                 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name =
      "_$s3WMF11ActionsViewC5frameACSo6CGSizeV_tcfc";
  EXPECT_FALSE(sdk::swiftMangledCGRectClassInitializerSourceABI(Wrong,
                                                                 0x1000));
  Wrong = Image;
  Wrong.Symbols[0].Name += "To";
  EXPECT_FALSE(sdk::swiftMangledCGRectClassInitializerSourceABI(Wrong,
                                                                 0x1000));
  Wrong = Image;
  Wrong.Symbols.push_back({"_alias", 0x1000, 0, true});
  EXPECT_FALSE(sdk::swiftMangledCGRectClassInitializerSourceABI(Wrong,
                                                                 0x1000));
  Wrong = Image;
  Wrong.Arch = Arch::X64;
  EXPECT_FALSE(sdk::swiftMangledCGRectClassInitializerSourceABI(Wrong,
                                                                 0x1000));
}

namespace {
struct NativeFixture {
  BinaryImage Image;
  MedFunc Med;
  HighFunc High;
  PipelineFunctionAudit Audit;

  explicit NativeFixture(Arch Architecture = Arch::AArch64) {
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100);
    Image.Segments.push_back(std::move(Text));
    const auto &TRI = getTargetRegInfo(Architecture);
    Med.Entry = High.Entry = Audit.Entry = 0x1000;
    Med.ReturnType = High.ReturnType = NdType::makeInt(4);
    Med.Blocks.emplace_back();
    Med.Blocks[0].Id = 0;
    for (unsigned Index = 0; Index < 2; ++Index) {
      MedVar Parameter;
      Parameter.Kind = MedVar::Param;
      Parameter.Id = Index;
      Parameter.RegOff = TRI.IntParamRegs[Index];
      Parameter.Size = 4;
      Parameter.TheArch = Architecture;
      Med.Params.push_back(Parameter);
      Med.TypedParams.push_back(
          {"arg" + std::to_string(Index), NdType::makeInt(4)});
      High.Params.push_back(
          {"arg" + std::to_string(Index), NdType::makeInt(4)});
    }
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output.Kind = MedVar::Reg;
    Add.Output.Id = 10;
    Add.Output.SSAVer = 1;
    Add.Output.TheArch = Architecture;
    Add.Output.RegOff = TRI.IntReturnReg;
    Add.Output.Size = 4;
    Add.addInput(Med.Params[0]);
    Add.addInput(Med.Params[1]);
    Med.Blocks[0].Ops.push_back(Add);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Med.Blocks[0].Ops.push_back(Return);
    HighStmt Statement;
    Statement.Kind = StmtKind::Return;
    Statement.RetVal = HighExpr::makeConst(42, 4);
    High.Body.push_back(std::move(Statement));
    Audit.Disposition = PipelineFunctionDisposition::Accepted;
    Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
    Audit.DecodedInstructions = Audit.LiftedInstructions = 2;
  }

  std::optional<SourceFunctionTypeHint> infer(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error);
  }
};

NativeFixture nativePairFixture(Arch Architecture) {
  NativeFixture F(Architecture);
  const auto &TRI = getTargetRegInfo(Architecture);
  F.Med.ReturnType = F.High.ReturnType = NdType::makeInt(8, false);
  for (size_t I = 0; I < F.Med.Params.size(); ++I) {
    F.Med.Params[I].Size = 8;
    F.Med.TypedParams[I].Type = F.High.Params[I].Type = NdType::makeInt(8);
  }
  F.Med.Params[1].Id = -1;
  auto &Primary = F.Med.Blocks[0].Ops[0];
  Primary.Output.Size = 8;
  Primary.Inputs[0] = F.Med.Params[0];
  Primary.Inputs[1] = MedVar::makeConst(17, 8);
  auto Secondary = Primary;
  Secondary.Opcode = NdOp::INT_XOR;
  Secondary.Output.RegOff = TRI.IntReturnRegs[1];
  Secondary.Output.Id = 11;
  Secondary.Inputs[1] = MedVar::makeConst(UINT64_C(0xfedcba9876543210), 8);
  F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.begin() + 1, Secondary);
  return F;
}

TEST(NativeSourceHints,
     IntegerPairDemandIncludesNarrowReadsButStopsAtClobbers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      SCOPED_TRACE(Mutation);
      const auto &TRI = getTargetRegInfo(Architecture);
      LowFunc F;
      F.Blocks.emplace_back();
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(0x1080, 8));
      LowOp Read;
      Read.Opcode = NdOp::COPY;
      Read.Output = NdVar::reg(TRI.IntReturnRegs[0], 8);
      Read.addInput(NdVar::reg(TRI.IntReturnRegs[1], 8));
      if (Mutation == 1)
        Read.Inputs[0].Size = 4;
      if (Mutation == 2)
        Call.Opcode = NdOp::INDIR_CALL;
      F.Blocks[0].Ops = {Call, Read};
      if (Mutation >= 3 && Mutation <= 5) {
        LowOp Stop;
        Stop.Opcode = Mutation == 3 ? NdOp::INTRINSIC : NdOp::COPY;
        Stop.Output = NdVar::reg(TRI.IntReturnRegs[1], Mutation == 4 ? 4 : 8);
        Stop.addInput(NdVar::cst(0, 8));
        F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin() + 1, Stop);
      }
      if (Mutation == 6) {
        F.Blocks[0].Ops.back().Opcode = NdOp::INT_XOR;
        F.Blocks[0].Ops.back().addInput(Read.Inputs[0]);
      }
      if (Mutation == 7) {
        F.Blocks[0].Ops.pop_back();
        F.Blocks.emplace_back();
        F.Blocks.back().Ops.push_back(Read);
      }
      EXPECT_EQ(observedNativeIntegerPairReturns(F, Architecture),
                Mutation <= 1 ? std::set<va_t>{0x1080} : std::set<va_t>{});
    }
}

TEST(NativeSourceHints, IntegerPairsRequireBothCompleteReturnCarriers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto F = nativePairFixture(Architecture);
      auto &Ops = F.Med.Blocks[0].Ops;
      if (Mutation == 1)
        Ops[1].Output.Size = 4;
      if (Mutation == 2)
        Ops.erase(Ops.begin() + 1);
      if (Mutation == 3)
        F.Med.Blocks[0].Preds = {99};
      if (Mutation == 4)
        F.Med.Blocks[0].ExceptionalSuccs.emplace_back();
      std::string Error;
      const auto Pair = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
      if (Mutation) {
        EXPECT_TRUE(!Pair || Pair->ReturnType->Kind != NdTypeKind::Struct);
      } else {
        ASSERT_TRUE(Pair) << Error;
        EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Struct);
        EXPECT_EQ(Pair->ReturnType->Size, 16U);
        ASSERT_EQ(Pair->ReturnComponents.size(), 2U);
        for (unsigned I = 0; I < 2; ++I) {
          EXPECT_EQ(Pair->ReturnComponents[I].RegisterOffset,
                    getTargetRegInfo(Architecture).IntReturnRegs[I]);
          EXPECT_EQ(Pair->ReturnComponents[I].ValueBytes, 8U);
        }
        const auto Scalar = F.infer(Error);
        ASSERT_TRUE(Scalar) << Error;
        EXPECT_EQ(Scalar->ReturnType->Kind, NdTypeKind::Int);
      }
    }
}

TEST(NativeSourceHints,
     IntegerPairReturnsAllowScalarBitReverseAndSeparateTrapPaths) {
  auto Fixture = nativePairFixture(Arch::AArch64);
  auto &Entry = Fixture.Med.Blocks[0];
  Entry.Ops.pop_back();
  Entry.Succs = {1, 2};

  MedOp Reverse;
  Reverse.Opcode = NdOp::INTRINSIC;
  Reverse.Output.Kind = MedVar::Temp;
  Reverse.Output.Id = 300;
  Reverse.Output.Size = 8;
  Reverse.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_Rbit), 2));
  Reverse.addInput(MedVar::makeConst(1, 8));
  Entry.Ops.push_back(Reverse);

  MedBlock Returning;
  Returning.Id = 1;
  Returning.Preds = {0};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Returning.Ops.push_back(Return);
  Fixture.Med.Blocks.push_back(Returning);

  MedBlock Trapping;
  Trapping.Id = 2;
  Trapping.Preds = {0};
  MedOp Trap;
  Trap.Opcode = NdOp::INTRINSIC;
  Trap.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Brk), 2));
  Trapping.Ops.push_back(Trap);
  Fixture.Med.Blocks.push_back(Trapping);

  std::string Error;
  const auto Pair = inferNativeSourceTypeHint(
      Fixture.Image, Fixture.Med, Fixture.High, Fixture.Audit, Error, nullptr,
      true);
  ASSERT_TRUE(Pair) << Error;
  EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Struct);
  ASSERT_EQ(Pair->ReturnComponents.size(), 2U);

  Fixture.Med.Blocks[0].Ops.back().Inputs[0].ConstVal =
      static_cast<uint64_t>(Intrinsic::A64_Rev64);
  EXPECT_FALSE(inferNativeSourceTypeHint(Fixture.Image, Fixture.Med,
                                         Fixture.High, Fixture.Audit, Error,
                                         nullptr, true));
}

TEST(NativeSourceHints,
     IntegerPairRefinementRequiresTheInferredNativeIdentity) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto F = nativePairFixture(Architecture);
      std::string Error;
      const auto Scalar = F.infer(Error);
      ASSERT_TRUE(Scalar) << Error;
      F.Med.SourceTypeHint = F.High.SourceTypeHint = *Scalar;
      F.Med.SourceParametersBound = true;
      if (Mutation == 1)
        F.Med.SourceTypeHint->Origin = F.High.SourceTypeHint->Origin =
            SourceFunctionTypeHint::OriginKind::DarwinSDK;
      if (Mutation == 2)
        F.Audit.Entry += 4;
      if (Mutation == 3)
        F.Med.SourceParametersBound = false;
      if (Mutation == 4)
        F.High.SourceTypeHint->ReturnType = NdType::makeInt(4);
      if (Mutation == 5)
        F.Med.Blocks[0].Ops[1].Output.Size = 4;
      const auto Pair =
          refineNativeIntegerPairReturnHint(F.Med, F.High, F.Audit);
      EXPECT_EQ(bool(Pair), Mutation == 0);
      if (Pair) {
        F.Med.SourceTypeHint = F.High.SourceTypeHint = *Pair;
        F.Med.ReturnType = F.High.ReturnType = Pair->ReturnType;
        EXPECT_FALSE(refineNativeIntegerPairReturnHint(F.Med, F.High, F.Audit));
      }
    }
}

TEST(NativeSourceHints, RefinementNarrowsIntegerParametersWithOnlyLowWordUses) {
  const auto Local = [](int Id, uint16_t Size) {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.Id = Id;
    Value.Size = Size;
    return HighExpr::makeVar(Value, NdType::makeInt(Size, false));
  };
  const auto Assign = [](ExprPtr Destination, ExprPtr Value) {
    HighStmt Statement;
    Statement.Kind = StmtKind::Assign;
    Statement.Dst = std::move(Destination);
    Statement.Val = std::move(Value);
    return Statement;
  };
  const auto Returning = [](ExprPtr Value) {
    HighStmt Statement;
    Statement.Kind = StmtKind::Return;
    Statement.RetVal = std::move(Value);
    return Statement;
  };
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
      NativeFixture Fixture(Architecture);
      for (size_t I = 0; I < Fixture.High.Params.size(); ++I) {
        Fixture.Med.Params[I].Size = 8;
        Fixture.Med.TypedParams[I].Type = NdType::makeInt(8, false);
        Fixture.High.Params[I].Type = NdType::makeInt(8, false);
      }
      SourceFunctionTypeHint Original;
      Original.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Original.ReturnType = Fixture.High.ReturnType;
      const auto Pointer = NdType::makePtr(NdType::makeVoid());
      Fixture.High.Params[0].Type = Pointer;
      Original.Parameters = {{"native_arg0", Pointer},
                             {"native_arg1", NdType::makeInt(8, false)}};
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Original, Architecture, Error))
          << Error;
      Fixture.High.SourceTypeHint = Original;

      auto Parameter =
          HighExpr::makeVar(Fixture.Med.Params[1], NdType::makeInt(8, false));
      auto Prefix =
          HighExpr::makeBinop(NdOp::SUBBYTES, Parameter,
                              HighExpr::makeConst(Mutation == 2 ? 1 : 0, 4));
      Prefix->Type = NdType::makeInt(4, false);
      auto PointerParameter = HighExpr::makeVar(Fixture.Med.Params[0], Pointer);
      auto PointerLocal = Local(40, 8);
      PointerLocal->Type = Pointer;
      Fixture.High.Body = {Assign(PointerLocal, PointerParameter),
                           Assign(Local(41, 4), Prefix),
                           Returning(HighExpr::makeConst(0, 4))};
      if (Mutation == 1)
        Fixture.High.Body.insert(Fixture.High.Body.begin() + 2,
                                 Assign(Local(42, 8), Parameter));

      const auto Refined =
          refineNativeSourceTypeHint(Fixture.High, Fixture.Audit);
      EXPECT_EQ(bool(Refined), Mutation == 0) << Mutation;
      if (Refined) {
        ASSERT_EQ(Refined->Parameters.size(), 2U);
        EXPECT_EQ(Refined->Parameters[0].Type->Size, 8U);
        EXPECT_EQ(Refined->Parameters[1].Type->Size, 4U);
        EXPECT_EQ(Refined->Parameters[1].Location.ValueBytes, 4U);
        EXPECT_EQ(Refined->Parameters[1].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[1]);
      }
    }
}

TEST(NativeSourceHints, RefinesOnlyExplicitlyPartialIntegerReturnCandidates) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeFixture F(Architecture);
      SourceFunctionTypeHint Original;
      Original.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Original.ReturnType = NdType::makeInt(8, false);
      Original.Parameters = {{"arg0", NdType::makeInt(4)},
                             {"arg1", NdType::makeInt(4)}};
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Original, Architecture, Error));
      F.High.ReturnType = Original.ReturnType;
      F.High.SourceTypeHint = Original;
      MedVar Local;
      Local.Kind = MedVar::Temp;
      Local.Id = 40;
      Local.Size = 8;
      const auto LocalExpr = [&] {
        return HighExpr::makeVar(Local, NdType::makeInt(8, false));
      };
      auto Low = HighExpr::makeVar(F.Med.Params[0], NdType::makeInt(4));
      if (Mutation == 1)
        Low = HighExpr::makeUndef(4);
      auto Upper = HighExpr::makeUndef(4);
      if (Mutation == 2)
        Upper = HighExpr::makeConst(0, 4);
      auto Partial = HighExpr::makeBinop(NdOp::CONCAT, Upper, Low);
      Partial->Type = NdType::makeInt(8, false);
      HighStmt Define;
      Define.Kind = StmtKind::Assign;
      Define.Dst = LocalExpr();
      Define.Val = Partial;
      HighStmt Otherwise = Define;
      Otherwise.Dst = LocalExpr();
      Otherwise.Val = HighExpr::makeConst(0, 8);
      HighStmt Branch;
      Branch.Kind = StmtKind::IfElse;
      Branch.Cond = HighExpr::makeVar(F.Med.Params[1], NdType::makeInt(4));
      Branch.Body = {Define};
      Branch.ElseBody = {Otherwise};
      if (Mutation == 3)
        Branch.ElseBody.clear();
      if (Mutation == 4)
        Branch.ElseBody[0].Val = LocalExpr(); // Cyclic/missing definition.
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = LocalExpr();
      F.High.Body = {Branch, Return};
      if (Mutation == 5)
        F.High.SourceTypeHint->Origin =
            SourceFunctionTypeHint::OriginKind::ObjCRuntime;
      if (Mutation == 6)
        ++F.Audit.Entry;
      if (Mutation == 7)
        F.High.Body.back().RetVal = HighExpr::makeUndef(8);
      if (Mutation == 8)
        F.High.StructuredExceptionRegions = 1;
      if (Mutation == 9)
        Partial->Operands[1]->Type = NdType::makeInt(2);
      if (Mutation == 0) {
        const auto Flow = analyzeHighSourceFlow(F.High, true);
        for (const auto &Item : Flow.Items)
          ADD_FAILURE() << Item.Reason;
      }
      const auto Refined = refineNativeSourceTypeHint(F.High, F.Audit);
      EXPECT_EQ(bool(Refined), Mutation == 0);
      if (Refined) {
        EXPECT_EQ(Refined->ReturnType->Size, 4U);
        EXPECT_EQ(Refined->ReturnLocation.ValueBytes, 4U);
        EXPECT_EQ(Refined->Parameters.size(), Original.Parameters.size());
        EXPECT_EQ(Refined->ReturnLocation.RegisterOffset,
                  Original.ReturnLocation.RegisterOffset);
        // The candidate is not a mutation of the analyzed body or its ABI.
        EXPECT_EQ(F.High.SourceTypeHint->ReturnType->Size, 8U);
        EXPECT_EQ(F.High.Body.back().RetVal->Type->Size, 8U);
      }
    }
}

TEST(NativeSourceHints, IntegerPrefixCallersCannotObserveUnknownUpperWord) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (uint16_t Width : {4, 8}) {
      NativeFixture F(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      SourceFunctionTypeHint Callee, Entry;
      Callee.Origin = Entry.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Callee.ReturnType = NdType::makeInt(4, false);
      Entry.ReturnType = NdType::makeInt(Width, false);
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Callee, Architecture, Error));
      ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Architecture, Error));
      std::map<va_t, SourceFunctionTypeHint> Hints{{0x1000, Entry},
                                                   {0x1080, Callee}};
      LowFunc Low;
      Low.Entry = 0x1000;
      LowBlock B;
      B.Id = 0;
      B.StartAddr = 0x1000;
      B.EndAddr = 0x1008;
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.Addr = 0x1000;
      Call.Output = NdVar::reg(TRI.IntReturnReg, 8);
      Call.addInput(NdVar::cst(0x1080, 8));
      LowOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.Addr = 0x1004;
      Return.addInput(NdVar::reg(TRI.IntReturnReg, Width));
      B.Ops = {Call, Return};
      Low.Blocks = {B};
      LowToMedConverter Converter;
      Converter.setSourceCallHintsEnabled(true);
      Converter.setSourceCalleeTypeHints(&Hints);
      Converter.setSourceEntryTypeHints(&Hints);
      F.Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
      recoverCallAbi(F.Med, Architecture, {{0x1080, "native_prefix"}});
      F.Med.SourceTypeHint = Entry;
      inferMedTypes(F.Med, Architecture);
      F.High = MedToHighConverter().convert(F.Med, Architecture);
      // Isolate return publication from the separately authenticated callee
      // closure. Even an allowed call cannot define its unreturned high word.
      const auto Limitation = sdk::sourceBodyLimitation(
          F.High, Entry, &F.Audit, [](const HighExpr &) { return true; });
      EXPECT_EQ(Limitation.empty(), Width == 4) << Limitation;
      if (Width == 8)
        EXPECT_NE(Limitation.find("unresolved"), std::string::npos)
            << Limitation;
    }
}

TEST(NativeSourceHints, IntegerPairPathsMeetBothWordsAcrossJoinsAndBackedges) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Loop : {false, true})
      for (bool MissingWord : {false, true}) {
        auto F = nativePairFixture(Architecture);
        const auto Primary = F.Med.Blocks[0].Ops[0];
        const auto Secondary = F.Med.Blocks[0].Ops[1];
        const auto Return = F.Med.Blocks[0].Ops.back();
        F.Med.Blocks[0].Ops = {Primary};
        F.Med.Blocks[0].Succs = {1, 2};
        F.Med.Blocks.resize(4);
        for (int I = 1; I <= 3; ++I)
          F.Med.Blocks[I].Id = I;
        F.Med.Blocks[1].Preds =
            Loop ? std::vector<int>{0, 1} : std::vector<int>{0};
        F.Med.Blocks[1].Succs =
            Loop ? std::vector<int>{1, 3} : std::vector<int>{3};
        F.Med.Blocks[1].Ops = {Secondary};
        F.Med.Blocks[2].Preds = {0};
        F.Med.Blocks[2].Succs = {3};
        F.Med.Blocks[2].Ops =
            MissingWord ? std::vector<MedOp>{} : std::vector<MedOp>{Secondary};
        F.Med.Blocks[3].Preds = {1, 2};
        F.Med.Blocks[3].Ops = {Return};
        std::string Error;
        const auto Hint = inferNativeSourceTypeHint(
            F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnType->Kind == NdTypeKind::Struct, !MissingWord);
      }
}

TEST(NativeSourceHints, NonReturningContractsRequireTerminalFlowAndBoundCalls) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeFixture F(Architecture);
      F.Med.DoesNotReturn = F.High.DoesNotReturn = true;
      auto Binding = std::make_shared<SourceCallTypeHint>();
      Binding->CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
      Binding->TargetAddress = 0x3000;
      Binding->TargetName = "abort";
      Binding->DoesNotReturn = true;
      Binding->Signature.ReturnType = NdType::makeVoid();
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Binding->Signature, Architecture, Error));
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.DoesNotReturn = true;
      Call.SourceCallHint = Binding;
      Call.addInput(MedVar::makeConst(0x3000, 8));
      F.Med.Blocks[0].Ops = {Call};
      auto Expression = HighExpr::makeCall("abort", 0x3000, {});
      Expression->SourceCallHint = Binding;
      Expression->Type = NdType::makeVoid();
      HighStmt Statement;
      Statement.Kind = StmtKind::Call;
      Statement.CallExpr = Expression;
      F.High.Body = {Statement};
      if (Mutation == 1)
        F.Med.DoesNotReturn = false;
      if (Mutation == 2)
        F.High.DoesNotReturn = false;
      if (Mutation == 3) {
        F.Med.Blocks[0].Ops[0].DoesNotReturn = false;
        MedOp Return;
        Return.Opcode = NdOp::RETURN;
        F.Med.Blocks[0].Ops.push_back(Return);
      }
      if (Mutation == 4)
        F.Med.Blocks[0].Ops.clear();
      if (Mutation == 5)
        F.Med.Blocks[0].Ops[0].Opcode = NdOp::INTRINSIC;
      if (Mutation == 6)
        F.Med.Blocks[0].Ops[0].SourceCallHint.reset();
      if (Mutation == 7)
        Binding->DoesNotReturn = false;
      if (Mutation == 8)
        F.Med.Blocks[0].ExceptionalSuccs.emplace_back();
      if (Mutation == 9)
        F.Med.Blocks[0].Ops[0].addInput(MedVar::makeConst(0, 8));
      auto Hint = F.infer(Error);
      EXPECT_EQ(bool(Hint), Mutation == 0) << Error;
      if (Hint) {
        EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
        EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
        EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 0U);
      }
    }
}

TEST(NativeSourceHints, KeepsObservedIntegerLocationsWithoutUsingNames) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    Fixture.Med.Name = "unrelated_stripped_symbol";
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::NativeAnalysis);
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
    EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, TRI.IntParamRegs[1]);
    EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, TRI.IntReturnReg);
    EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 4U);
    EXPECT_EQ(Fixture.Med.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
  }
}

TEST(NativeSourceHints, LeafReturnsPreserveProvenFullMachineWidth) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    auto &Write = Fixture.Med.Blocks[0].Ops.front();
    Write.Opcode = NdOp::INT_ZEXT;
    Write.NumInputs = 1;
    Write.Output.Size = 8;
    Write.Inputs[0] = MedVar::makeConst(UINT32_MAX, 4);
    Fixture.High.Body.front().RetVal =
        HighExpr::makeUnary(NdOp::INT_ZEXT, HighExpr::makeConst(UINT32_MAX, 4));
    Fixture.High.Body.front().RetVal->Type = NdType::makeInt(8, false);
    std::string Error;
    const auto Full = Fixture.infer(Error);
    ASSERT_TRUE(Full) << Error;
    EXPECT_EQ(Full->ReturnType->Size, 8U);
    EXPECT_FALSE(Full->ReturnType->IsSigned);
    EXPECT_EQ(Full->ReturnLocation.ValueBytes, 8U);
    // A source expression alone cannot authorize a wider machine carrier.
    Write.Output.Size = 4;
    const auto Narrow = Fixture.infer(Error);
    ASSERT_TRUE(Narrow) << Error;
    EXPECT_EQ(Narrow->ReturnLocation.ValueBytes, 4U);
    Write.Output.Size = 8;
    Fixture.High.Body.front().RetVal = HighExpr::makeUndef(8);
    const auto Unknown = Fixture.infer(Error);
    ASSERT_TRUE(Unknown) << Error;
    EXPECT_EQ(Unknown->ReturnLocation.ValueBytes, 4U);
  }
}

TEST(NativeSourceHints, NarrowExternalResultsDoNotProveTheirUpperBits) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    auto &Write = Fixture.Med.Blocks[0].Ops.front();
    Write.Opcode = NdOp::CALL;
    Write.NumInputs = 1;
    Write.Inputs[0] = MedVar::makeConst(0x1020, 8);
    Write.Output.Size = 8;
    auto Call = std::make_shared<SourceCallTypeHint>();
    Call->Signature.ReturnType = NdType::makeInt(4, false);
    std::string Error;
    ASSERT_TRUE(
        assignDarwinScalarSourceABI(Call->Signature, Architecture, Error));
    Write.SourceCallHint = Call;
    Fixture.High.Body.front().RetVal = HighExpr::makeConst(UINT32_MAX, 8);
    const auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 4U);
  }
}

NativeFixture compilerRTPlatformVersionFixture(Arch Architecture) {
  NativeFixture Fixture(Architecture);
  const auto &TRI = getTargetRegInfo(Architecture);
  Fixture.Med.ReturnType = Fixture.High.ReturnType = NdType::makeInt(8);
  Fixture.Med.Blocks[0].Ops[0].Output.Size = 8;
  Fixture.High.Body[0].RetVal = HighExpr::makeConst(1, 8);
  for (unsigned Index = 2; Index < 4; ++Index) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = Index;
    Parameter.RegOff = TRI.IntParamRegs[Index];
    Parameter.Size = 4;
    Parameter.TheArch = Architecture;
    Fixture.Med.Params.push_back(Parameter);
    Fixture.Med.TypedParams.push_back(
        {"arg" + std::to_string(Index), NdType::makeInt(4)});
    Fixture.High.Params.push_back(
        {"arg" + std::to_string(Index), NdType::makeInt(4)});
  }
  SourceCallTypeHint Availability;
  Availability.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Availability.WeakImport = true;
  Availability.TargetAddress = 0x1080;
  Availability.TargetName = "_availability_version_check";
  Availability.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
  Availability.Signature.ReturnType = NdType::makeInt(1, false);
  Availability.Signature.Parameters = {
      {"count", NdType::makeInt(4, false)},
      {"versions", NdType::makePtr(NdType::makeVoid())},
  };
  std::string Error;
  EXPECT_TRUE(
      assignDarwinFixedSourceABI(Availability.Signature, Architecture, Error))
      << Error;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(std::move(Availability));
  Call.addInput(MedVar::makeConst(0x1080, 8));
  Call.addInput(MedVar::makeConst(1, 4));
  Call.addInput(MedVar::makeConst(0x10a0, 8));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
  Fixture.Audit.DecodedInstructions = Fixture.Audit.LiftedInstructions = 3;
  Fixture.Image.Symbols.push_back(
      {"___isPlatformVersionAtLeast", Fixture.Med.Entry, 0x80, true});
  return Fixture;
}

TEST(NativeSourceHints,
     CompilerRTPlatformVersionHelperUsesExactNarrowContract) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Fixture = compilerRTPlatformVersionFixture(Architecture);
    std::string Error;
    const auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    ASSERT_TRUE(Hint->ReturnType);
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Hint->ReturnType->Size, 4U);
    EXPECT_TRUE(Hint->ReturnType->IsSigned);
    ASSERT_EQ(Hint->Parameters.size(), 4U);
    const auto &TRI = getTargetRegInfo(Architecture);
    for (size_t I = 0; I < Hint->Parameters.size(); ++I) {
      EXPECT_EQ(Hint->Parameters[I].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Parameters[I].Type->Size, 4U);
      EXPECT_FALSE(Hint->Parameters[I].Type->IsSigned);
      EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset,
                TRI.IntParamRegs[I]);
      EXPECT_EQ(Hint->Parameters[I].Location.ValueBytes, 4U);
    }
  }
}

TEST(NativeSourceHints,
     CompilerRTPlatformVersionHelperRejectsContradictoryEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto Fixture = compilerRTPlatformVersionFixture(Architecture);
      auto &Symbol = Fixture.Image.Symbols.back();
      auto Call = std::make_shared<SourceCallTypeHint>(
          *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
      Fixture.Med.Blocks[0].Ops[0].SourceCallHint = Call;
      if (Mutation == 0)
        Symbol.IsFunc = false;
      if (Mutation == 1)
        Fixture.Image.Symbols.push_back(Symbol);
      if (Mutation == 2)
        Call->WeakImport = false;
      if (Mutation == 3)
        Call->TargetName = "_different";
      if (Mutation == 4)
        Call->CallKind = SourceCallTypeHint::Kind::Native;
      if (Mutation == 5)
        Call->Signature.ReturnType = NdType::makeInt(4, false);
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
      EXPECT_FALSE(Error.empty()) << Mutation;
    }

    for (unsigned Mutation = 0; Mutation < 2; ++Mutation) {
      auto Unrelated = compilerRTPlatformVersionFixture(Architecture);
      if (Mutation == 0)
        Unrelated.Image.Symbols.back().Name = "unrelated_local_helper";
      else
        Unrelated.Image.Symbols.back().Addr += 4;
      std::string Error;
      const auto Generic = Unrelated.infer(Error);
      ASSERT_TRUE(Generic) << Mutation << ": " << Error;
      EXPECT_EQ(Generic->ReturnType->Size, 8U) << Mutation;
    }
  }
}

struct NativeVoidFixture : NativeFixture {
  LowFunc Low;
  NativeVoidFixture(Arch Architecture, bool Indirect = false)
      : NativeFixture(Architecture) {
    Med.Params[0].Size = 8;
    Med.TypedParams[0].Type = High.Params[0].Type = NdType::makeInt(8);
    Med.Params[1].Id = -1;
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
    Hint->Signature.ReturnType = NdType::makeVoid();
    Hint->Signature.Parameters.push_back(
        {"object", NdType::makePtr(NdType::makeVoid())});
    std::string Error;
    EXPECT_TRUE(
        assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
    MedOp Call;
    Call.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
    Call.Addr = 0x1010;
    Call.OriginSeq = 7;
    Call.SourceCallHint = Hint;
    Call.addInput(MedVar::makeConst(0x1080, 8));
    Call.addInput(Med.Params[0]);
    auto Return = Med.Blocks[0].Ops.back();
    Return.Addr = Call.Addr;
    Med.Blocks[0].Ops = {Call, Return};
    Low.Entry = Med.Entry;
    Low.Blocks.emplace_back();
    Low.Blocks[0].Id = 0;
    LowOp NativeCall;
    NativeCall.Opcode = Call.Opcode;
    NativeCall.Addr = Call.Addr;
    NativeCall.Seq = Call.OriginSeq;
    NativeCall.Output =
        NdVar::reg(getTargetRegInfo(Architecture).IntReturnReg, 8);
    NativeCall.addInput(NdVar::cst(0x1080, 8));
    LowOp NativeReturn;
    NativeReturn.Opcode = NdOp::RETURN;
    NativeReturn.Addr = Call.Addr;
    Low.Blocks[0].Ops = {NativeCall, NativeReturn};
  }
  std::optional<SourceFunctionTypeHint> inferVoid(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error, &Low);
  }

  void useValueWitnessDestroy() {
    const auto &TRI = getTargetRegInfo(Image.Arch);
    auto Hint = swiftValueWitnessSourceCallHint(
        Image.Arch, SourceCallTypeHint::SwiftValueWitnessKind::Destroy);
    ASSERT_TRUE(Hint);
    auto &Call = Med.Blocks[0].Ops[0];
    ASSERT_TRUE(Call.Opcode == NdOp::CALL || Call.Opcode == NdOp::INDIR_CALL);
    MedVar Target;
    Target.Kind = MedVar::Reg;
    Target.Id = 20;
    Target.SSAVer = 1;
    Target.Size = 8;
    Target.TheArch = Image.Arch;
    Target.RegOff = TRI.IntReturnReg;
    Med.Params[1].Id = 1;
    Med.Params[1].Size = 8;
    Med.TypedParams[0].Type = High.Params[0].Type =
        NdType::makePtr(NdType::makeVoid());
    Med.TypedParams[1].Type = High.Params[1].Type =
        NdType::makePtr(NdType::makeVoid());
    Call.Opcode = NdOp::INDIR_CALL;
    Call.SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
    Call.NumInputs = 0;
    Call.addInput(Target);
    Call.addInput(Med.Params[0]);
    Call.addInput(Med.Params[1]);

    LowOp *NativeCall = nullptr;
    for (auto &Block : Low.Blocks)
      for (auto &Operation : Block.Ops)
        if (Operation.Opcode == NdOp::CALL ||
            Operation.Opcode == NdOp::INDIR_CALL) {
          ASSERT_EQ(NativeCall, nullptr);
          NativeCall = &Operation;
        }
    ASSERT_NE(NativeCall, nullptr);
    NativeCall->Opcode = NdOp::INDIR_CALL;
    NativeCall->NumInputs = 0;
    NativeCall->addInput(NdVar::reg(TRI.IntReturnReg, 8));
  }

  void useValueWitnessStoreTag() {
    useValueWitnessDestroy();
    auto Hint = swiftValueWitnessSourceCallHint(
        Image.Arch,
        SourceCallTypeHint::SwiftValueWitnessKind::StoreEnumTagSinglePayload);
    ASSERT_TRUE(Hint);
    auto &Call = Med.Blocks[0].Ops[0];
    Call.SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Hint);
    const auto Target = Call.Inputs[0];
    Call.NumInputs = 0;
    Call.addInput(Target);
    Med.Params.clear();
    Med.TypedParams.clear();
    High.Params.clear();
    for (const auto &P : Hint->Signature.Parameters) {
      MedVar Parameter;
      Parameter.Kind = MedVar::Param;
      Parameter.Id = Med.Params.size();
      Parameter.Size = P.Type->Size;
      Parameter.RegOff = P.Location.RegisterOffset;
      Parameter.TheArch = Image.Arch;
      Med.Params.push_back(Parameter);
      Med.TypedParams.push_back({P.Name, P.Type});
      High.Params.push_back({P.Name, P.Type});
      Call.addInput(Parameter);
    }
  }

  void useNativeVoidCallee() {
    auto Hint = std::make_shared<SourceCallTypeHint>(
        *Med.Blocks[0].Ops[0].SourceCallHint);
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = 0x1080;
    Hint->TargetName = "native_void_callee";
    Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Med.Blocks[0].Ops[0].SourceCallHint = std::move(Hint);
  }
};

TEST(NativeSourceHints, VoidTailForwardersNeverSupplyAnUnprovenResult) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Indirect : {false, true}) {
      NativeVoidFixture Fixture(Architecture, Indirect);
      std::string Error;
      const auto Hint = Fixture.inferVoid(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
      EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 0U);
      ASSERT_EQ(Hint->Parameters.size(), 1U);
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
                getTargetRegInfo(Architecture).IntParamRegs[0]);
      EXPECT_FALSE(Fixture.infer(Error));
      EXPECT_EQ(Fixture.Med.ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);

      // Keep the established leaf subset available when the general frame
      // proof rejects an otherwise harmless LowIR operation. The narrower
      // proof must still carry stack-address taint through that operation to
      // a declared call argument.
      NativeVoidFixture Compatibility(Architecture, Indirect);
      LowOp Intrinsic;
      Intrinsic.Opcode = NdOp::INTRINSIC;
      Intrinsic.Addr = 0x1004;
      Intrinsic.Output =
          NdVar::reg(getTargetRegInfo(Architecture).IntParamRegs[1], 8);
      Compatibility.Low.Blocks[0].Ops.insert(
          Compatibility.Low.Blocks[0].Ops.begin(), Intrinsic);
      EXPECT_TRUE(Compatibility.inferVoid(Error)) << Error;
      Compatibility.Low.Blocks[0].Ops[0].Output =
          NdVar::reg(getTargetRegInfo(Architecture).IntParamRegs[0], 8);
      Compatibility.Low.Blocks[0].Ops[0].addInput(
          NdVar::reg(getTargetRegInfo(Architecture).StackPointer, 8));
      EXPECT_FALSE(Compatibility.inferVoid(Error));
    }
}

TEST(NativeSourceHints, VoidContractsPropagateAcrossExactNativeCallees) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Fixture(Architecture);
    Fixture.useNativeVoidCallee();
    std::string Error;
    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);

    auto Forged = std::make_shared<SourceCallTypeHint>(
        *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
    Forged->TargetAddress += 4;
    Fixture.Med.Blocks[0].Ops[0].SourceCallHint = std::move(Forged);
    EXPECT_FALSE(Fixture.inferVoid(Error));
  }
}

TEST(NativeSourceHints, VoidContractsAcceptExactSwiftStringBridgeCallBindings) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (auto Kind : {SourceCallTypeHint::Kind::SwiftStringBridge,
                      SourceCallTypeHint::Kind::SwiftStringFromNSString}) {
      NativeVoidFixture Fixture(Architecture);
      auto Hint = std::make_shared<SourceCallTypeHint>(
          *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
      Hint->CallKind = Kind;
      Hint->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::SwiftStringBridge;
      Fixture.Med.Blocks[0].Ops[0].SourceCallHint = std::move(Hint);
      std::string Error;
      const auto Result = Fixture.inferVoid(Error);
      ASSERT_TRUE(Result) << Error;
      EXPECT_EQ(Result->ReturnType->Kind, NdTypeKind::Void);
    }
}

TEST(NativeSourceHints,
     VoidTailContractsRejectHiddenOutputsAndIncompleteEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
      NativeVoidFixture Fixture(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      auto &Ops = Fixture.Low.Blocks[0].Ops;
      switch (Mutation) {
      case 0:
        ++Ops[1].Addr;
        break;
      case 1:
      case 2:
      case 3: {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        const auto Register = Mutation == 1                   ? TRI.StackPointer
                              : Mutation == 2                 ? TRI.FramePointer
                              : Architecture == Arch::AArch64 ? a64reg::X19
                                                              : x86reg::RBX;
        Write.Output = NdVar::reg(Register, 8);
        Write.addInput(NdVar::cst(0, 8));
        Ops.insert(Ops.begin(), Write);
        break;
      }
      case 4:
        Ops[0].Inputs[0].Offset += 8;
        break;
      case 5:
        Ops[0].Addr += 4;
        Ops[1].Addr += 4;
        break;
      case 6:
      case 7:
      case 8: {
        auto Hint = std::make_shared<SourceCallTypeHint>(
            *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
        if (Mutation == 6) {
          // A changed return type without its matching ABI is not a valid
          // source contract, even when the helper supplies no result.
          Hint->Signature.ReturnType = NdType::makeInt(8);
        } else if (Mutation == 7) {
          Hint->CallKind = SourceCallTypeHint::Kind::Native;
        } else {
          Hint->DoesNotReturn = true;
        }
        Fixture.Med.Blocks[0].Ops[0].SourceCallHint = Hint;
        break;
      }
      case 9:
        Fixture.Low.Entry += 4;
        break;
      case 10:
        Fixture.Low.Blocks.clear();
        break;
      case 11:
        Fixture.Med.Blocks[0].Preds = {17};
        break;
      case 12:
        Ops[0].Inputs[0] = NdVar::reg(TRI.IntParamRegs[0], 8);
        break;
      case 13:
        Ops[0].NumInputs = 7;
        break;
      case 14: {
        LowOp Escape;
        Escape.Opcode = NdOp::COPY;
        Escape.Addr = Ops[0].Addr;
        Escape.Output = NdVar::reg(TRI.IntParamRegs[0], 8);
        Escape.addInput(NdVar::reg(TRI.StackPointer, 8));
        Ops.insert(Ops.begin(), Escape);
        break;
      }
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferVoid(Error)) << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints,
     CanonicalDynamicValueWitnessDestroyHasAnExactVoidCallIdentity) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Leaf(Architecture);
    Leaf.useValueWitnessDestroy();
    std::string Error;
    const auto LeafHint = Leaf.inferVoid(Error);
    ASSERT_TRUE(LeafHint) << Error;
    EXPECT_EQ(LeafHint->ReturnType->Kind, NdTypeKind::Void);
  }
}

TEST(NativeSourceHints, CanonicalSinglePayloadStorePreservesVoidCallEffects) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Fixture(Architecture);
    Fixture.useValueWitnessStoreTag();
    std::string Error;
    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Hint->Parameters.size(), 4U);
    EXPECT_EQ(Hint->Parameters[1].Type->Size, 4U);
    EXPECT_EQ(Hint->Parameters[2].Type->Size, 4U);
  }
}

TEST(NativeSourceHints,
     DynamicVoidCallIdentityRejectsForgeryMismatchAndFrameTargets) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      NativeVoidFixture Fixture(Architecture);
      Fixture.useValueWitnessDestroy();
      auto &MedCall = Fixture.Med.Blocks[0].Ops[0];
      auto &LowCall = Fixture.Low.Blocks[0].Ops[0];
      const auto &TRI = getTargetRegInfo(Architecture);
      switch (Mutation) {
      case 0: {
        auto Hint =
            std::make_shared<SourceCallTypeHint>(*MedCall.SourceCallHint);
        Hint->TargetName += "_forged";
        MedCall.SourceCallHint = std::move(Hint);
        break;
      }
      case 1: {
        auto Hint = swiftValueWitnessSourceCallHint(
            Architecture,
            SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithCopy);
        ASSERT_TRUE(Hint);
        MedCall.SourceCallHint =
            std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
        break;
      }
      case 2:
        MedCall.Opcode = NdOp::CALL;
        break;
      case 3:
        LowCall.Opcode = NdOp::CALL;
        break;
      case 4:
        LowCall.Inputs[0] = NdVar::cst(0x1080, 8);
        break;
      case 5:
        MedCall.Inputs[0] = MedVar::makeConst(0x1080, 8);
        break;
      case 6: {
        LowOp Derive;
        Derive.Opcode = NdOp::COPY;
        Derive.Addr = 0x1008;
        Derive.Output = NdVar::reg(TRI.IntReturnReg, 8);
        Derive.addInput(NdVar::reg(TRI.StackPointer, 8));
        Fixture.Low.Blocks[0].Ops.insert(Fixture.Low.Blocks[0].Ops.begin(),
                                         std::move(Derive));
        break;
      }
      case 7:
        LowCall.Inputs[0].Size = 4;
        break;
      case 8:
        ++LowCall.Seq;
        break;
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferVoid(Error))
          << unsigned(Architecture) << ": " << Mutation << ": " << Error;
    }
}

struct NativeVoidFrameFixture : NativeVoidFixture {
  Arch Architecture;
  std::vector<uint64_t> Saved;
  size_t CallIndex = 0;
  size_t RestoreIndex = 0;
  int64_t FrameBytes;

  static LowOp op(NdOp Opcode, NdVar Output,
                  std::initializer_list<NdVar> Inputs, va_t Address = 0x1004) {
    LowOp Result;
    Result.Opcode = Opcode;
    Result.Output = Output;
    Result.Addr = Address;
    for (auto Input : Inputs)
      Result.addInput(Input);
    return Result;
  }

  explicit NativeVoidFrameFixture(Arch Architecture)
      : NativeVoidFixture(Architecture), Architecture(Architecture),
        FrameBytes(Architecture == Arch::AArch64 ? 32 : 24) {
    const auto &TRI = getTargetRegInfo(Architecture);
    Saved = {Architecture == Arch::AArch64 ? a64reg::X19 : x86reg::RBX,
             TRI.FramePointer};
    if (Architecture == Arch::AArch64) {
      Saved.push_back(TRI.LinkRegister);
      Saved.push_back(TRI.VecRegBase + 8 * TRI.VecRegStride);
    }
    const auto Call = Low.Blocks[0].Ops[0];
    auto &Ops = Low.Blocks[0].Ops;
    const auto SP = NdVar::reg(TRI.StackPointer, 8);
    const auto Address = NdVar::tmp(TmpBase, 8);
    Ops = {op(NdOp::INT_SUB, SP, {SP, NdVar::cst(FrameBytes, 8)})};
    for (size_t I = 0; I < Saved.size(); ++I) {
      Ops.push_back(op(NdOp::INT_ADD, Address, {SP, NdVar::cst(I * 8, 8)}));
      Ops.push_back(op(NdOp::STORE, {}, {Address, NdVar::reg(Saved[I], 8)}));
      Ops.push_back(
          op(NdOp::COPY, NdVar::reg(Saved[I], 8), {NdVar::cst(0, 8)}));
    }
    CallIndex = Ops.size();
    Ops.push_back(Call);
    RestoreIndex = Ops.size();
    for (size_t I = 0; I < Saved.size(); ++I) {
      Ops.push_back(
          op(NdOp::INT_ADD, Address, {SP, NdVar::cst(I * 8, 8)}, 0x1020));
      Ops.push_back(op(NdOp::LOAD, NdVar::reg(Saved[I], 8), {Address}, 0x1020));
    }
    Ops.push_back(
        op(NdOp::INT_ADD, SP, {SP, NdVar::cst(FrameBytes, 8)}, 0x1020));
    Ops.push_back(op(
        NdOp::RETURN, {},
        {NdVar::reg(TRI.LinkRegister ? TRI.LinkRegister : TRI.IntReturnReg, 8)},
        0x1024));
  }

  void splitReturns(bool Loop = false) {
    auto &First = Low.Blocks[0];
    const std::vector<LowOp> Restore(First.Ops.begin() + RestoreIndex,
                                     First.Ops.end());
    First.Ops.resize(RestoreIndex);
    First.Succs = Loop ? std::vector<int>{1} : std::vector<int>{1, 2};
    LowBlock Left, Right;
    Left.Id = 1;
    Right.Id = 2;
    Left.Preds = Loop ? std::vector<int>{0, 1} : std::vector<int>{0};
    Right.Preds = Loop ? std::vector<int>{1} : std::vector<int>{0};
    Left.Ops = Loop ? std::vector<LowOp>{op(NdOp::NOP, {}, {})} : Restore;
    Right.Ops = Restore;
    if (Loop)
      Left.Succs = {1, 2};
    Low.Blocks.push_back(std::move(Left));
    Low.Blocks.push_back(std::move(Right));
  }

  size_t usePreindexedSpills() {
    auto &Ops = Low.Blocks[0].Ops;
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto SP = NdVar::reg(TRI.StackPointer, 8);
    const auto Base = NdVar::tmp(TmpBase + 64, 8);
    const auto Address = NdVar::tmp(TmpBase, 8);
    std::vector<LowOp> Prologue{
        op(NdOp::INT_SUB, Base, {SP, NdVar::cst(FrameBytes, 8)})};
    for (size_t I = 0; I < Saved.size(); ++I) {
      Prologue.push_back(
          op(NdOp::INT_ADD, Address, {Base, NdVar::cst(I * 8, 8)}));
      Prologue.push_back(
          op(NdOp::STORE, {}, {Address, NdVar::reg(Saved[I], 8)}));
      Prologue.push_back(
          op(NdOp::COPY, NdVar::reg(Saved[I], 8), {NdVar::cst(0, 8)}));
    }
    const size_t StackWrite = Prologue.size();
    Prologue.push_back(op(NdOp::COPY, SP, {Base}));
    Ops.erase(Ops.begin(), Ops.begin() + CallIndex);
    Ops.insert(Ops.begin(), Prologue.begin(), Prologue.end());
    CallIndex = Prologue.size();
    RestoreIndex++;
    return StackWrite;
  }
};

struct NativeFloatingFrameFixture : NativeVoidFrameFixture {
  static constexpr va_t ImportSlot = 0x3000;

  NativeFloatingFrameFixture() : NativeVoidFrameFixture(Arch::AArch64) {
    Med.ReturnType = High.ReturnType = NdType::makeInt(8);
    High.Body[0].RetVal = HighExpr::makeConst(42, 8);
    Med.Blocks[0].StartAddr = Low.Blocks[0].StartAddr = Med.Entry;
    // A source argument may occupy x3 without occupying earlier integer
    // argument registers. Changing the result ABI must not repack this input.
    Med.Params[0].RegOff = a64reg::X3;
    Section Text;
    Text.VA = Med.Entry;
    Text.Size = Text.FileSz = Image.Segments[0].FileSz;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(std::move(Text));
    Segment Data;
    Data.VA = ImportSlot;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Image.Segments.push_back(std::move(Data));
    constexpr auto Provider =
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
    Image.ImportPtrSlots[ImportSlot] = "_CFStringGetDoubleValue";
    Image.DyldBindSlots[ImportSlot] = {"_CFStringGetDoubleValue", 0, Provider,
                                       false};
    Image.DynInfo.NeededLibs.push_back(Provider);
    const uint32_t Words[] = {0xd0000010, 0xf9400210, 0xd61f0200};
    for (unsigned I = 0; I != 3; ++I)
      for (unsigned J = 0; J != 4; ++J)
        Image.Segments[0].Data[0x1080 - Med.Entry + I * 4 + J] =
            Words[I] >> (J * 8);
    auto &LowOps = Low.Blocks[0].Ops;
    LowOps.insert(LowOps.begin() + CallIndex,
                  op(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                     {NdVar::reg(a64reg::X3, 8)}, 0x100c));
    ++CallIndex;
    ++RestoreIndex;
    const auto Current = buildObjCSourceCallHints(Image, Low);
    const auto Binding = Current.find(0x1010);
    EXPECT_NE(Binding, Current.end());
    if (Binding == Current.end())
      return;
    auto &Call = Med.Blocks[0].Ops[0];
    Call.SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(Binding->second);
    Call.CallSiteId = 1;
    Call.Inputs[1] = Med.Params[0];
    Call.Output.Kind = MedVar::Reg;
    Call.Output.Id = 10;
    Call.Output.SSAVer = 1;
    Call.Output.TheArch = Arch::AArch64;
    Call.Output.RegOff = a64reg::V0;
    Call.Output.Size = 8;
    Med.Blocks[0].Ops.back().Addr = LowOps.back().Addr;
  }
};

TEST(NativeSourceHints, FloatingCandidateKeepsTheExistingArgumentCarrier) {
  NativeFloatingFrameFixture F;
  std::string Error;
  ASSERT_EQ(F.Med.ReturnType->Kind, NdTypeKind::Int);
  ASSERT_EQ(F.Med.ReturnType->Size, 8U);
  // Neither the provisional i64 result nor the floating value alone can
  // supply the required native frame proof.
  EXPECT_FALSE(F.infer(Error));
  const auto Hint = F.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint->ReturnType->Size, 8U);
  EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::V0);
  EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 8U);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Hint->Parameters[0].Location.Kind,
            SourceABICarrierKind::IntegerRegister);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, a64reg::X3);
  EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 8U);
  EXPECT_EQ(F.Med.ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(F.High.ReturnType->Kind, NdTypeKind::Int);
}

TEST(NativeSourceHints, FloatingCandidateStillRequiresCallAndFrameEvidence) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    NativeFloatingFrameFixture F;
    if (Mutation == 0)
      F.Med.Blocks[0].Ops[0].SourceCallHint.reset();
    else if (Mutation == 1)
      ++F.Low.Blocks[0].Ops[F.CallIndex].Seq;
    else if (Mutation == 2)
      F.Low.Blocks[0].Ops[F.CallIndex].Inputs[0].Offset += 4;
    else
      F.Low.Blocks[0].Ops[F.RestoreIndex + 1].Opcode = NdOp::NOP;
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error)) << Mutation << ": " << Error;
  }
}

TEST(NativeSourceHints, FloatingCandidateNeverReplacesADefinedIntegerResult) {
  NativeFloatingFrameFixture F;
  MedOp Integer;
  Integer.Opcode = NdOp::COPY;
  Integer.Addr = 0x1014;
  Integer.Output = F.Med.Blocks[0].Ops[0].Output;
  Integer.Output.Id = 11;
  Integer.Output.RegOff = a64reg::X0;
  Integer.addInput(MedVar::makeConst(42, 8));
  F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.end() - 1, Integer);
  F.Low.Blocks[0].Ops.insert(
      F.Low.Blocks[0].Ops.begin() + F.RestoreIndex,
      F.op(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {NdVar::cst(42, 8)}, 0x1014));
  std::string Error;
  const auto Hint = F.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Hint->ReturnType->Size, 8U);
  EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::IntegerRegister);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, a64reg::X0);
}

struct NativeMixedTerminalFixture : NativeVoidFrameFixture {
  static constexpr va_t FailureSlot = 0x3000;
  static constexpr va_t FailureStub = 0x10d0;
  static constexpr va_t FailureCall = 0x1040;

  NativeMixedTerminalFixture() : NativeVoidFrameFixture(Arch::AArch64) {
    useNativeVoidCallee();
    splitReturns();
    Low.Blocks[0].StartAddr = Med.Entry;
    Low.Blocks[1].StartAddr = 0x1020;
    Low.Blocks[2].StartAddr = FailureCall;
    auto Return = Med.Blocks[0].Ops.back();
    Return.Addr = 0x1024;
    Med.Blocks[0].StartAddr = Med.Entry;
    Med.Blocks[0].Ops.pop_back();
    Med.Blocks[0].Succs = {1, 2};
    MedBlock Normal;
    Normal.Id = 1;
    Normal.StartAddr = 0x1020;
    Normal.Preds = {0};
    Normal.Ops = {Return};
    MedBlock Failure;
    Failure.Id = 2;
    Failure.StartAddr = FailureCall;
    Failure.Preds = {0};
    Section Text;
    Text.VA = Med.Entry;
    Text.Size = Text.FileSz = Image.Segments[0].FileSz;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(std::move(Text));
    Segment Data;
    Data.VA = FailureSlot;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Image.Segments.push_back(std::move(Data));
    Image.ImportPtrSlots[FailureSlot] = "___stack_chk_fail";
    Image.DyldBindSlots[FailureSlot] = {"___stack_chk_fail", 0,
                                        "/usr/lib/libSystem.B.dylib", false};
    Image.DynInfo.NeededLibs.push_back("/usr/lib/libSystem.B.dylib");
    // ADRP x16, 0x3000; LDR x16, [x16]; BR x16. The current loader must
    // independently connect the executable veneer to this exact import slot.
    const uint32_t Words[] = {0xd0000010, 0xf9400210, 0xd61f0200};
    for (unsigned I = 0; I != 3; ++I)
      for (unsigned J = 0; J != 4; ++J)
        Image.Segments[0].Data[FailureStub - Med.Entry + I * 4 + J] =
            Words[I] >> (J * 8);
    auto Hint = darwinRuntimeSourceCallHint(Image, FailureSlot);
    EXPECT_TRUE(Hint);
    if (!Hint)
      return;
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = FailureCall;
    Call.OriginSeq = 12;
    Call.CallSiteId = 2;
    Call.DoesNotReturn = true;
    Call.SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
    Call.addInput(MedVar::makeConst(FailureStub, 8));
    Failure.Ops = {Call};
    Med.Blocks.push_back(std::move(Normal));
    Med.Blocks.push_back(std::move(Failure));
    auto NativeCall =
        op(NdOp::CALL, {}, {NdVar::cst(FailureStub, 8)}, FailureCall);
    NativeCall.Seq = Call.OriginSeq;
    Low.Blocks[2].Ops = {NativeCall};
  }

  void
  changeFailureHint(const std::function<void(SourceCallTypeHint &)> &Change) {
    auto &Call = Med.Blocks[2].Ops[0];
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Change(*Hint);
    Call.SourceCallHint = std::move(Hint);
  }

  NativeSourceCalls contracts() const {
    NativeSourceCalls Calls;
    for (const auto &B : Med.Blocks)
      for (const auto &Op : B.Ops) {
        if (Op.Opcode != NdOp::CALL || !Op.SourceCallHint)
          continue;
        NativeSourceCallContract Contract;
        Contract.Signature = &Op.SourceCallHint->Signature;
        if (Op.CallSiteId == 2)
          Contract.Termination =
              NativeSourceCallContract::TerminationKind::StackCheckFailure;
        Calls.emplace(NativeSourceCallKey{Op.Addr, Op.OriginSeq, Op.Opcode,
                                          Op.Inputs[0].ConstVal},
                      Contract);
      }
    return Calls;
  }
};

struct NativeMixedDictionaryViolationFixture : NativeMixedTerminalFixture {
  NativeMixedDictionaryViolationFixture() {
    constexpr auto Name = "$ss53KEY_TYPE_OF_DICTIONARY_VIOLATES_HASHABLE_"
                          "REQUIREMENTSys5NeverOypXpF";
    constexpr auto Module = "/usr/lib/swift/libswiftCore.dylib";
    Image.ImportPtrSlots[FailureSlot] = std::string("_") + Name;
    Image.DyldBindSlots[FailureSlot] = {std::string("_") + Name, 0, Module,
                                        false};
    Image.DynInfo.NeededLibs = {Module};
    auto Hint = swiftRuntimeSourceCallHint(Image, FailureSlot);
    EXPECT_TRUE(Hint);
    if (!Hint)
      return;
    auto &Call = Med.Blocks[2].Ops[0];
    Call.SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
    Call.addInput(MedVar::makeConst(0x4000, 8));
  }

  NativeSourceCalls contracts() const {
    auto Calls = NativeMixedTerminalFixture::contracts();
    const auto Key = nativeSourceCallKey(Low.Blocks[2].Ops[0]);
    if (Key)
      Calls.at(*Key).Termination =
          NativeSourceCallContract::TerminationKind::SwiftDictionaryViolation;
    return Calls;
  }
};

TEST(NativeSourceHints, MixedSwiftDictionaryViolationPreservesNormalReturns) {
  NativeMixedDictionaryViolationFixture F;
  SourceFunctionTypeHint Expected;
  Expected.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
  Expected.ReturnType = NdType::makeVoid();
  Expected.Parameters = {{"arg0", NdType::makePtr(NdType::makeVoid())}};
  std::string ABIError;
  EXPECT_TRUE(assignDarwinSwiftSourceABI(Expected, Arch::AArch64, ABIError))
      << ABIError;
  EXPECT_TRUE(equalSourceABIs(F.Med.Blocks[2].Ops[0].SourceCallHint->Signature,
                              Expected));
  EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.contracts()));
  std::string Error;
  const auto Hint = F.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);

  F.Low.Blocks[1].Ops[1].Output = NdVar::reg(a64reg::X20, 8);
  EXPECT_FALSE(F.inferVoid(Error));
}

TEST(NativeSourceHints, MixedSwiftDictionaryViolationAllowsImmediateTrap) {
  NativeMixedDictionaryViolationFixture F;
  auto Trap =
      F.op(NdOp::INTRINSIC, {},
           {NdVar::cst(static_cast<uint64_t>(Intrinsic::Brk), 2)}, 0x1044);
  F.Low.Blocks[2].Ops.push_back(Trap);
  EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.contracts()));
  F.Low.Blocks[2].Ops.back().Opcode = NdOp::NOP;
  EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, F.contracts()));
}

TEST(NativeSourceHints, MixedSwiftDictionaryViolationRevalidatesImport) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeMixedDictionaryViolationFixture F;
    if (Mutation == 0)
      F.Image.DyldBindSlots[F.FailureSlot].Module = "/tmp/foreign.dylib";
    else if (Mutation == 1)
      F.Image.DyldBindSlots[F.FailureSlot].Addend = 8;
    else if (Mutation == 2)
      F.Image.DyldBindSlots[F.FailureSlot].WeakImport = true;
    else if (Mutation == 3)
      F.changeFailureHint(
          [](auto &Hint) { Hint.Signature.Parameters.clear(); });
    else
      F.Med.Blocks[2].Ops[0].DoesNotReturn = false;
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error));
  }
}

TEST(NativeSourceHints, MixedStackFailureKeepsEveryNormalExitRestored) {
  NativeMixedTerminalFixture F;
  std::string Error;
  const auto Hint = F.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  EXPECT_FALSE(F.Med.DoesNotReturn);
  EXPECT_FALSE(F.High.DoesNotReturn);
  EXPECT_EQ(F.Med.ReturnType->Kind, NdTypeKind::Int);
  EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.contracts()));

  const auto Original = F.Low.Blocks[1].Ops[1];
  F.Low.Blocks[1].Ops[1].Output = NdVar::reg(a64reg::X20, 8);
  EXPECT_FALSE(F.inferVoid(Error));
  F.Low.Blocks[1].Ops[1] = Original;
  // An additional valid normal exit cannot hide another un-restored exit.
  auto Bad = F.Low.Blocks[1];
  Bad.Id = 3;
  Bad.StartAddr = 0x1050;
  Bad.Ops[1].Output = NdVar::reg(a64reg::X20, 8);
  F.Low.Blocks.push_back(std::move(Bad));
  F.Low.Blocks[0].Succs.push_back(3);
  auto MedBad = F.Med.Blocks[1];
  MedBad.Id = 3;
  MedBad.StartAddr = 0x1050;
  F.Med.Blocks.push_back(std::move(MedBad));
  F.Med.Blocks[0].Succs.push_back(3);
  EXPECT_FALSE(F.inferVoid(Error));
}

TEST(NativeSourceHints, MixedStackFailureRevalidatesExactImportAndVeneer) {
  const std::array<std::function<void(NativeMixedTerminalFixture &)>, 12>
      Mutations = {{
          [](auto &F) {
            F.Image.DyldBindSlots[F.FailureSlot].Module = "/tmp/foreign.dylib";
          },
          [](auto &F) {
            F.Image.DyldBindSlots[F.FailureSlot].WeakImport = true;
          },
          [](auto &F) { F.Image.DyldBindSlots[F.FailureSlot].Addend = 8; },
          [](auto &F) { F.Image.DyldBindSlots.erase(F.FailureSlot); },
          [](auto &F) { F.Image.ImportPtrSlots[F.FailureSlot] = "_abort"; },
          [](auto &F) {
            F.Image.ConflictingImportStorageSlots.insert(F.FailureSlot);
          },
          [](auto &F) {
            F.Image.Segments[0].Data[F.FailureStub - F.Med.Entry + 8] ^= 4;
          },
          [](auto &F) { F.Med.Blocks[2].Ops[0].Inputs[0].ConstVal += 4; },
          [](auto &F) { ++F.Med.Blocks[2].Ops[0].OriginSeq; },
          [](auto &F) { F.Image.Arch = Arch::X64; },
          [](auto &F) { F.Image.DynInfo.NeededLibs.clear(); },
          [](auto &F) {
            F.Image.DynInfo.NeededLibs = {"/tmp/libSystem.B.dylib"};
          },
      }};
  for (size_t I = 0; I != Mutations.size(); ++I) {
    SCOPED_TRACE(I);
    NativeMixedTerminalFixture F;
    Mutations[I](F);
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error));
  }
}

TEST(NativeSourceHints, MixedStackFailureRejectsSignatureAndEffectDrift) {
  const std::array<std::function<void(SourceCallTypeHint &)>, 10> Mutations = {{
      [](auto &H) { H.DoesNotReturn = false; },
      [](auto &H) { H.TargetName = "abort"; },
      [](auto &H) { H.CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall; },
      [](auto &H) { H.WeakImport = true; },
      [](auto &H) { H.Signature.ReturnType = NdType::makeInt(8); },
      [](auto &H) {
        H.Signature.Parameters.push_back({"extra", NdType::makeInt(8)});
      },
      [](auto &H) {
        H.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      },
      [](auto &H) { H.ReturnedArgument = 0; },
      [](auto &H) { H.NilTerminated.emplace(); },
      [](auto &H) { H.ByteCount = 8; },
  }};
  for (size_t I = 0; I != Mutations.size(); ++I) {
    SCOPED_TRACE(I);
    NativeMixedTerminalFixture F;
    F.changeFailureHint(Mutations[I]);
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error));
  }
  NativeMixedTerminalFixture F;
  F.Med.Blocks[2].Ops[0].DoesNotReturn = false;
  std::string Error;
  EXPECT_FALSE(F.inferVoid(Error));
  F.Med.Blocks[2].Ops[0].DoesNotReturn = true;
  F.Med.Blocks[2].Ops[0].PreservesCallerSaved = true;
  EXPECT_FALSE(F.inferVoid(Error));
}

TEST(NativeSourceHints, MixedStackFailureRequiresAnActualTerminalSink) {
  const std::array<std::function<void(NativeMixedTerminalFixture &)>, 5>
      Mutations = {{
          [](auto &F) {
            F.Low.Blocks[2].Ops.push_back(F.op(NdOp::NOP, {}, {}, 0x1044));
          },
          [](auto &F) {
            F.Low.Blocks[2].Ops.push_back(F.Low.Blocks[1].Ops.back());
          },
          [](auto &F) {
            F.Low.Blocks[2].Succs = {1};
            F.Low.Blocks[1].Preds.push_back(2);
          },
          [](auto &F) {
            F.Low.Blocks[0].Succs = {2};
            F.Low.Blocks[1].Preds.clear();
          },
          [](auto &F) {
            F.Low.Blocks[0].Succs = {2};
            F.Low.Blocks.erase(F.Low.Blocks.begin() + 1);
          },
      }};
  for (size_t I = 0; I != Mutations.size(); ++I) {
    SCOPED_TRACE(I);
    NativeMixedTerminalFixture F;
    Mutations[I](F);
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error));
    EXPECT_FALSE(
        restoresNativeSourceState(F.Low, Arch::AArch64, F.contracts()));
  }
}

TEST(NativeSourceHints,
     MixedStackFailureStillChecksFrameTaintBeforeTermination) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeMixedTerminalFixture F;
    auto &Ops = F.Low.Blocks[2].Ops;
    if (Mutation == 0)
      Ops.insert(Ops.begin(),
                 F.op(NdOp::STORE, {},
                      {NdVar::cst(0x4000, 8), NdVar::reg(a64reg::SP, 8)},
                      0x103c));
    else if (Mutation == 1)
      Ops.insert(Ops.begin(), F.op(NdOp::COPY, NdVar::reg(a64reg::SP, 8),
                                   {NdVar::cst(0, 8)}, 0x103c));
    else
      F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + F.CallIndex,
                                 F.op(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                                      {NdVar::reg(a64reg::SP, 8)}, 0x100c));
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error));
  }
}

TEST(NativeSourceHints,
     MixedStackFailureDoesNotAcceptOtherTerminationContracts) {
  NativeMixedTerminalFixture F;
  for (auto Kind :
       {NativeSourceCallContract::TerminationKind::None,
        NativeSourceCallContract::TerminationKind::RuntimeEntry,
        static_cast<NativeSourceCallContract::TerminationKind>(99)}) {
    auto Calls = F.contracts();
    const auto Key = nativeSourceCallKey(F.Low.Blocks[2].Ops[0]);
    ASSERT_TRUE(Key);
    Calls.at(*Key).Termination = Kind;
    EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, Calls));
  }
  auto Calls = F.contracts();
  const auto Key = nativeSourceCallKey(F.Low.Blocks[2].Ops[0]);
  ASSERT_TRUE(Key);
  Calls.at(*Key).ReadOnlyFrameParameters.emplace(0, 8);
  EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, Calls));
}

struct NativeIncomingStackFixture : NativeVoidFrameFixture {
  int64_t IncomingOffset;
  size_t AddressIndex;
  size_t LoadIndex;

  explicit NativeIncomingStackFixture(int64_t Offset = 0)
      : NativeVoidFrameFixture(Arch::AArch64), IncomingOffset(Offset) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id =
        getTargetRegInfo(Arch::AArch64).IntParamRegs.size() + Offset / 8;
    Parameter.RegOff = kNoParamReg;
    Parameter.Size = 8;
    Parameter.TheArch = Arch::AArch64;
    Med.Params = {Parameter};
    Med.TypedParams = {{"incoming", NdType::makeInt(8)}};
    High.Params = {{"incoming", NdType::makeInt(8)}};
    Med.Blocks[0].Ops[0].Inputs[1] = Parameter;
    useNativeVoidCallee();

    AddressIndex = CallIndex;
    LoadIndex = CallIndex + 1;
    const auto Address = NdVar::tmp(TmpBase + 128, 8);
    auto &Ops = Low.Blocks[0].Ops;
    Ops.insert(
        Ops.begin() + CallIndex,
        {op(NdOp::INT_ADD, Address,
            {NdVar::reg(a64reg::SP, 8), NdVar::cst(FrameBytes + Offset, 8)},
            0x100c),
         op(NdOp::LOAD, NdVar::reg(a64reg::X0, 8), {Address}, 0x100c)});
    CallIndex += 2;
    RestoreIndex += 2;
  }

  SourceFunctionTypeHint entrySignature() const {
    SourceFunctionTypeHint Result;
    Result.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Result.Architecture = Arch::AArch64;
    Result.HasExplicitABI = true;
    Result.ReturnType = NdType::makeVoid();
    Result.Parameters = {{"incoming",
                          NdType::makeInt(8),
                          {SourceABICarrierKind::Stack, 0, IncomingOffset, 8}}};
    return Result;
  }

  NativeSourceCalls calls() const {
    const auto Key = nativeSourceCallKey(Low.Blocks[0].Ops[CallIndex]);
    EXPECT_TRUE(Key);
    NativeSourceCallContract Contract;
    Contract.Signature = &Med.Blocks[0].Ops[0].SourceCallHint->Signature;
    return Key ? NativeSourceCalls{{*Key, Contract}} : NativeSourceCalls{};
  }
};

TEST(NativeSourceHints, IncomingStackReadsRequireCompleteObservedSlots) {
  for (int64_t Offset : {0, 16})
    for (bool Repeat : {false, true}) {
      SCOPED_TRACE(Offset);
      SCOPED_TRACE(Repeat);
      NativeIncomingStackFixture F(Offset);
      if (Repeat) {
        // Incoming values may be read again after a returning call. They are
        // external inputs, not private spill facts surviving that call.
        auto Address = F.Low.Blocks[0].Ops[F.AddressIndex];
        auto Load = F.Low.Blocks[0].Ops[F.LoadIndex];
        auto Call = F.Low.Blocks[0].Ops[F.CallIndex];
        Address.Addr = Load.Addr = Call.Addr = 0x1014;
        ++Call.Seq;
        F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + F.RestoreIndex,
                                   {Address, Load, Call});
        auto MedCall = F.Med.Blocks[0].Ops[0];
        MedCall.Addr = Call.Addr;
        MedCall.OriginSeq = Call.Seq;
        F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.begin() + 1, MedCall);
      }
      std::string Error;
      const auto Hint = F.inferVoid(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
      ASSERT_EQ(Hint->Parameters.size(), 1U);
      EXPECT_EQ(Hint->Parameters[0].Location.Kind, SourceABICarrierKind::Stack);
      EXPECT_EQ(Hint->Parameters[0].Location.EntryStackOffset, Offset);
      EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 8U);
      EXPECT_EQ(F.Med.Params[0].RegOff, kNoParamReg);
    }
}

TEST(NativeSourceHints, IncomingStackReadsRejectMissingOrPartialEvidence) {
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeIncomingStackFixture F(16);
    auto &Ops = F.Low.Blocks[0].Ops;
    switch (Mutation) {
    case 0:
      F.Med.Params.clear();
      F.Med.TypedParams.clear();
      F.High.Params.clear();
      F.Med.Blocks[0].Ops[0].Inputs[1] = MedVar::makeConst(1, 8);
      break;
    case 1:
      Ops[F.LoadIndex].Output.Size = 4;
      break;
    case 2:
      Ops[F.AddressIndex].Inputs[1].Offset += 8;
      break;
    case 3:
      Ops[F.LoadIndex] = NativeVoidFrameFixture::op(
          NdOp::STORE, {}, {Ops[F.LoadIndex].Inputs[0], NdVar::cst(42, 8)},
          0x100c);
      break;
    case 4:
      F.Med.Params.push_back(F.Med.Params[0]);
      F.Med.TypedParams.push_back(F.Med.TypedParams[0]);
      F.High.Params.push_back(F.High.Params[0]);
      break;
    case 5:
      F.Med.Params[0].Size = F.Med.Blocks[0].Ops[0].Inputs[1].Size = 4;
      F.Med.TypedParams[0].Type = F.High.Params[0].Type = NdType::makeInt(4);
      break;
    case 6:
      F.Med.TypedParams[0].Type = F.High.Params[0].Type = NdType::makeFloat(8);
      break;
    case 7:
      F.Med.Params[0].Id = F.Med.Blocks[0].Ops[0].Inputs[1].Id = 7;
      break;
    case 8:
      // A sixteen-byte read cannot borrow permission for one eight-byte slot.
      Ops[F.LoadIndex].Output.Size = 16;
      break;
    case 9:
      Ops[F.LoadIndex].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 10:
      Ops[F.LoadIndex].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 11:
      ++Ops[F.AddressIndex].Inputs[1].Offset;
      break;
    case 12:
      F.Med.TypedParams.clear();
      break;
    case 13:
      F.High.Params.clear();
      break;
    }
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error)) << Error;
    EXPECT_FALSE(Error.empty());
  }
}

TEST(NativeSourceHints, IncomingStackReadsCannotRestoreMachineState) {
  for (const auto Register :
       {a64reg::X19, a64reg::X29, a64reg::X30, a64reg::SP})
    for (unsigned UnknownAlias = 0; UnknownAlias != 3; ++UnknownAlias) {
      SCOPED_TRACE(Register);
      SCOPED_TRACE(UnknownAlias);
      NativeIncomingStackFixture F;
      auto &Ops = F.Low.Blocks[0].Ops;
      // First restore the real private spills. A later incoming load cannot
      // replace any restored identity, even when its slot and width are valid.
      const auto Address = NdVar::tmp(TmpBase + 256, 8);
      const auto Derive =
          UnknownAlias == 1
              ? NativeVoidFrameFixture::op(
                    NdOp::INT_ADD, Address,
                    {NdVar::reg(a64reg::SP, 8), NdVar::reg(a64reg::X1, 8)},
                    0x1024)
              : NativeVoidFrameFixture::op(NdOp::COPY, Address,
                                           {NdVar::reg(a64reg::SP, 8)}, 0x1024);
      Ops.insert(Ops.end() - 1,
                 {Derive, NativeVoidFrameFixture::op(NdOp::LOAD,
                                                     NdVar::reg(Register, 8),
                                                     {Address}, 0x1024)});
      if (UnknownAlias == 2)
        Ops[Ops.size() - 2].Inputs[0].Size = 4;
      const auto Signature = F.entrySignature();
      EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, F.calls(),
                                             nullptr, &Signature));
      std::string Error;
      EXPECT_FALSE(F.inferVoid(Error)) << Error;
    }
}

TEST(NativeSourceHints, UnknownIncomingAliasDoesNotAcquireStackSlotAuthority) {
  for (bool PartialAddress : {false, true}) {
    SCOPED_TRACE(PartialAddress);
    NativeIncomingStackFixture F;
    if (PartialAddress)
      F.Low.Blocks[0].Ops[F.LoadIndex].Inputs[0].Size = 4;
    else
      F.Low.Blocks[0].Ops[F.AddressIndex].Inputs[1] = NdVar::reg(a64reg::X1, 8);
    // A read through an unresolved address already produces Unknown in the
    // preservation analysis. No incoming-slot permission is needed or inferred;
    // source body validity remains a separate publication requirement.
    EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.calls()));
    const auto Signature = F.entrySignature();
    EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.calls(),
                                          nullptr, &Signature));
  }
}

TEST(NativeSourceHints, IncomingStackSignatureUsesPhysicalOffsets) {
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeIncomingStackFixture F(16);
    auto Signature = F.entrySignature();
    const SourceFunctionTypeHint *Entry = &Signature;
    switch (Mutation) {
    case 0:
      break; // The first logical parameter occupies the physical SP+16 slot.
    case 1:
      Entry = nullptr;
      break;
    case 2:
      Signature.Parameters.clear();
      break;
    case 3:
      Signature.Parameters[0].Location.EntryStackOffset = 0;
      break;
    case 4:
      Signature.Parameters[0].Location.EntryStackOffset = 17;
      break;
    case 5:
      Signature.Parameters.push_back(Signature.Parameters[0]);
      break;
    case 6:
      Signature.Parameters[0].Type = NdType::makeInt(4);
      Signature.Parameters[0].Location.ValueBytes = 4;
      break;
    case 7:
      Signature.Parameters[0].Type = NdType::makeFloat(8);
      break;
    case 8:
      Signature.HasExplicitABI = false;
      break;
    case 9:
      Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
      break;
    case 10:
      Signature.Architecture = Arch::X64;
      break;
    case 11:
      Signature.Parameters[0].Location.EntryStackOffset = INT64_MAX;
      break;
    case 12:
      Signature.Parameters[0].Location.EntryStackOffset = -8;
      break;
    case 13:
      Signature.Parameters[0].Location.RegisterOffset = a64reg::X1;
      break;
    }
    EXPECT_EQ(restoresNativeSourceState(F.Low, Arch::AArch64, F.calls(),
                                        nullptr, Entry),
              Mutation == 0);
  }
}

std::shared_ptr<SourceCallTypeHint>
useVoidRecordCallee(NativeVoidFixture &Fixture, const TypeRef &Record) {
  auto &Call = Fixture.Med.Blocks[0].Ops[0];
  auto Binding = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
  Binding->CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Binding->Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Binding->Signature.Parameters = {{"record", Record}};
  std::string Error;
  EXPECT_TRUE(
      assignDarwinFixedSourceABI(Binding->Signature, Fixture.Image.Arch, Error))
      << Error;
  Fixture.Med.Params.clear();
  Fixture.Med.TypedParams.clear();
  Fixture.High.Params.clear();
  Call.NumInputs = 1;
  for (const auto &Physical : sourceABIParameters(Binding->Signature))
    Call.addInput(MedVar::makeConst(17, Physical.Type->Size));
  Call.SourceCallHint = Binding;
  return Binding;
}

TEST(NativeSourceHints, VoidRecordsCheckEveryPhysicalRegisterMember) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Shape = 0; Shape < 4; ++Shape)
      for (unsigned Path = 0; Path < 3; ++Path) {
        SCOPED_TRACE(unsigned(Architecture));
        SCOPED_TRACE(Shape);
        SCOPED_TRACE(Path);
        // Floating records currently have an authoritative ABI only on arm64.
        if (Architecture == Arch::X64 && Shape != 2)
          continue;
        const auto Record =
            Shape == 0   ? NdType::makeStruct(
                               {NdType::makeFloat(8), NdType::makeFloat(8)})
            : Shape == 1 ? NdType::makeStruct(
                               {NdType::makeFloat(4), NdType::makeFloat(4)})
            : Shape == 2
                ? NdType::makeStruct(
                      {NdType::makeInt(8), NdType::makePtr(NdType::makeVoid())})
                : NdType::makeStruct(
                      {NdType::makeFloat(8), NdType::makeFloat(8),
                       NdType::makeFloat(8), NdType::makeFloat(8)});
        // Exercise ordinary framed calls, the general tail-call proof, and
        // the narrower leaf fallback independently. The fallback's harmless
        // intrinsic is deliberately unsupported by the general frame proof.
        NativeVoidFixture F = Path == 0 ? NativeVoidFrameFixture(Architecture)
                                        : NativeVoidFixture(Architecture);
        const auto Binding = useVoidRecordCallee(F, Record);
        if (Path == 2) {
          LowOp Intrinsic;
          Intrinsic.Opcode = NdOp::INTRINSIC;
          Intrinsic.Addr = 0x1004;
          F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin(), Intrinsic);
        }
        std::string Error;
        const auto Hint = F.inferVoid(Error);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
        EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
        EXPECT_TRUE(Hint->Parameters.empty());

        const auto Physicals = sourceABIParameters(Binding->Signature);
        ASSERT_EQ(Physicals.size(), Shape == 3 ? 4U : 2U);
        // Any byte in either member can carry a frame address, including a
        // non-leading FP lane. Clear one interior byte to ensure other-byte
        // taint cannot be erased by a partial overwrite.
        for (const auto &Physical : Physicals)
          for (bool PartialClear : {false, true}) {
            auto Escaping = F;
            auto &Ops = Escaping.Low.Blocks[0].Ops;
            const auto Call =
                std::find_if(Ops.begin(), Ops.end(),
                             [](auto &Op) { return Op.Opcode == NdOp::CALL; });
            ASSERT_NE(Call, Ops.end());
            const auto Index = Call - Ops.begin();
            const auto Register = Physical.Location.RegisterOffset;
            LowOp Derive = NativeVoidFrameFixture::op(
                NdOp::COPY, NdVar::reg(Register, 8),
                {NdVar::reg(getTargetRegInfo(Architecture).StackPointer, 8)},
                0x100c);
            Ops.insert(Ops.begin() + Index, Derive);
            if (PartialClear)
              Ops.insert(Ops.begin() + Index + 1,
                         NativeVoidFrameFixture::op(
                             NdOp::COPY, NdVar::reg(Register + 1, 1),
                             {NdVar::cst(0, 1)}, 0x100c));
            EXPECT_FALSE(Escaping.inferVoid(Error)) << Error;
          }
      }
}

TEST(NativeSourceHints, VoidRecordsRejectStaleAndBorrowedMemberContracts) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeVoidFrameFixture F(Arch::AArch64);
    auto Binding = useVoidRecordCallee(
        F, NdType::makeStruct({NdType::makeInt(8), NdType::makeInt(8)}));
    auto &Parameter = Binding->Signature.Parameters[0];
    switch (Mutation) {
    case 0:
      Parameter.Components.pop_back();
      break;
    case 1:
      Parameter.Components[1] = Parameter.Components[0];
      break;
    case 2:
      Parameter.Components[1].ValueBytes = 4;
      break;
    case 3:
      ++F.Low.Blocks[0].Ops[F.CallIndex].Seq;
      break;
    case 4:
      F.Med.Blocks[0].Ops[0].addInput(MedVar::makeConst(31, 8));
      break;
    case 5:
      --F.Med.Blocks[0].Ops[0].NumInputs;
      break;
    case 6:
    case 7:
      // The super2 certificate for source parameter zero only borrows a
      // scalar pointer. Its index must not authorize either record member.
      Binding->CallKind = SourceCallTypeHint::Kind::ObjCSuper2;
      Binding->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::ObjCRuntime;
      F.Low.Blocks[0].Ops.insert(
          F.Low.Blocks[0].Ops.begin() + F.CallIndex,
          NativeVoidFrameFixture::op(
              NdOp::COPY,
              NdVar::reg(Parameter.Components[Mutation - 6].RegisterOffset, 8),
              {NdVar::reg(a64reg::SP, 8)}, 0x100c));
      break;
    }
    std::string Error;
    EXPECT_FALSE(F.inferVoid(Error)) << Error;
    EXPECT_FALSE(Error.empty());
  }
}

struct NativeOutgoingStackFixture : NativeVoidFrameFixture {
  size_t ArgumentStore;
  std::shared_ptr<SourceCallTypeHint> Binding;

  explicit NativeOutgoingStackFixture(Arch Architecture = Arch::AArch64)
      : NativeVoidFrameFixture(Architecture) {
    const auto &TRI = getTargetRegInfo(Architecture);
    // Reserve a separate outgoing word below all saved-register identities.
    FrameBytes += 16;
    auto &Ops = Low.Blocks[0].Ops;
    for (auto &Op : Ops) {
      if ((Op.Opcode == NdOp::INT_SUB || Op.Opcode == NdOp::INT_ADD) &&
          Op.Output == NdVar::reg(TRI.StackPointer, 8))
        Op.Inputs[1] = NdVar::cst(FrameBytes, 8);
      else if (Op.Opcode == NdOp::INT_ADD &&
               Op.Output == NdVar::tmp(TmpBase, 8))
        Op.Inputs[1].Offset += 16;
    }
    ArgumentStore = CallIndex;
    Ops.insert(Ops.begin() + CallIndex,
               op(NdOp::STORE, {},
                  {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(0x7777, 8)},
                  0x100c));
    ++CallIndex;
    ++RestoreIndex;
    auto &Call = Med.Blocks[0].Ops[0];
    Binding = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Binding->Signature.Parameters.push_back(
        {"word", NdType::makeInt(8), {SourceABICarrierKind::Stack, 0, 0, 8}});
    Call.SourceCallHint = Binding;
    Call.addInput(MedVar::makeConst(0x7777, 8));
  }
};

TEST(NativeSourceHints, RecordBorrowingKeepsOriginalScalarParameterIndexes) {
  for (bool Floating : {false, true})
    for (bool Writable : {false, true})
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        SCOPED_TRACE(Floating);
        SCOPED_TRACE(Writable);
        SCOPED_TRACE(Mutation);
        NativeOutgoingStackFixture F;
        auto &Signature = F.Binding->Signature;
        const auto Member =
            Floating ? NdType::makeFloat(8) : NdType::makeInt(8);
        Signature.Parameters = {
            {"record", NdType::makeStruct({Member, Member})},
            {"scratch", NdType::makePtr(NdType::makeVoid())}};
        std::string Error;
        ASSERT_TRUE(assignDarwinFixedSourceABI(Signature, Arch::AArch64, Error))
            << Error;
        const auto Key = nativeSourceCallKey(F.Low.Blocks[0].Ops[F.CallIndex]);
        ASSERT_TRUE(Key);
        NativeSourceCallContract Contract;
        Contract.Signature = &Signature;
        auto &Borrowed = Writable ? Contract.WritableFrameParameters
                                  : Contract.ReadOnlyFrameParameters;
        // Logical parameter 1 is physical member 2 after a two-member record.
        Borrowed.emplace(Mutation == 1 ? 2 : 1, 8);
        auto &Ops = F.Low.Blocks[0].Ops;
        Ops.insert(
            Ops.begin() + F.CallIndex,
            NativeVoidFrameFixture::op(
                NdOp::COPY,
                NdVar::reg(Signature.Parameters[1].Location.RegisterOffset, 8),
                {NdVar::reg(a64reg::SP, 8)}, 0x100c));
        if (Mutation == 2 || Mutation == 3) {
          Borrowed.emplace(0, 8);
          Ops.insert(Ops.begin() + F.CallIndex,
                     NativeVoidFrameFixture::op(
                         NdOp::COPY,
                         NdVar::reg(Signature.Parameters[0]
                                        .Components[Mutation - 2]
                                        .RegisterOffset,
                                    8),
                         {NdVar::reg(a64reg::SP, 8)}, 0x100c));
        }
        if (Mutation == 4)
          Signature.Parameters[0].Components.pop_back();
        NativeSourceCalls Calls{{*Key, Contract}};
        EXPECT_EQ(restoresNativeSourceState(F.Low, Arch::AArch64, Calls),
                  Mutation == 0);
      }
}

TEST(NativeSourceHints, ReturningCallsConsumeCompletelyWrittenStackWords) {
  for (unsigned Mutation = 0; Mutation < 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeOutgoingStackFixture F(Mutation == 10 ? Arch::X64 : Arch::AArch64);
    const auto &TRI = getTargetRegInfo(F.Architecture);
    auto &Ops = F.Low.Blocks[0].Ops;
    if (Mutation == 1)
      Ops[F.ArgumentStore].Inputs[1].Size = 4;
    if (Mutation == 2)
      Ops[F.ArgumentStore] = NativeVoidFrameFixture::op(NdOp::NOP, {}, {});
    if (Mutation == 3)
      Ops[F.ArgumentStore].Inputs[1] = NdVar::reg(TRI.StackPointer, 8);
    if (Mutation == 4) {
      // The existing spill is fully written and contains a valid entry value.
      // Passing it by value lets the callee overwrite it, so that slot cannot
      // still restore the preserved register on return.
      F.Binding->Signature.Parameters.back().Location.EntryStackOffset = 16;
    }
    if (Mutation == 5)
      Ops[F.RestoreIndex + 1].Opcode = NdOp::NOP;
    if (Mutation == 6)
      F.Binding->Signature.Parameters.back().Location.EntryStackOffset =
          F.FrameBytes;
    if (Mutation == 7) {
      LowBlock CallBlock;
      CallBlock.Id = 1;
      CallBlock.Preds = {0};
      CallBlock.Ops.assign(Ops.begin() + F.CallIndex, Ops.end());
      Ops.resize(F.CallIndex);
      F.Low.Blocks[0].Succs = {1};
      F.Low.Blocks.push_back(std::move(CallBlock));
    }
    if (Mutation == 8 || Mutation == 9) {
      auto Second = F.Med.Blocks[0].Ops[0];
      Second.Addr += 4;
      ++Second.OriginSeq;
      F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.begin() + 1, Second);
      auto SecondLow = Ops[F.CallIndex];
      SecondLow.Addr = Second.Addr;
      SecondLow.Seq = Second.OriginSeq;
      Ops.insert(Ops.begin() + F.RestoreIndex, SecondLow);
      if (Mutation == 9) {
        auto Rewrite = Ops[F.ArgumentStore];
        Rewrite.Addr = Second.Addr;
        Ops.insert(Ops.begin() + F.RestoreIndex, Rewrite);
      }
    }
    if (Mutation == 10)
      // The x64 return address is below the caller's SP. This is a valid
      // x64 ABI declaration; only the ARM64 extension remains unavailable.
      F.Binding->Signature.Parameters.back().Location.EntryStackOffset = 8;
    if (Mutation == 11) {
      F.Binding->Signature.Parameters.back().Type = NdType::makeInt(4);
      F.Binding->Signature.Parameters.back().Location.ValueBytes = 4;
      F.Med.Blocks[0].Ops[0].Inputs[2].Size = 4;
    }
    if (Mutation == 12) {
      // The actual argument at SP+24 is separate from both saved values in
      // the preceding gap. The entire incoming argument area may be changed,
      // so preserving only the individual argument slot would be unsound.
      for (auto &Op : Ops)
        if (Op.Opcode == NdOp::INT_ADD && Op.Output == NdVar::tmp(TmpBase, 8) &&
            Op.Inputs[1].Offset == 24)
          Op.Inputs[1].Offset = 8;
      F.Binding->Signature.Parameters.back().Location.EntryStackOffset = 24;
      const auto Address = NdVar::tmp(TmpBase + 8, 8);
      Ops[F.ArgumentStore].Inputs[0] = Address;
      Ops.insert(Ops.begin() + F.ArgumentStore,
                 NativeVoidFrameFixture::op(
                     NdOp::INT_ADD, Address,
                     {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(24, 8)},
                     0x100c));
    }
    std::string Error;
    ASSERT_TRUE(validateSourceABI(F.Binding->Signature, Error)) << Error;
    const auto Hint = F.inferVoid(Error);
    if (Mutation == 0 || Mutation == 9) {
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
    } else {
      EXPECT_FALSE(Hint);
      EXPECT_FALSE(Error.empty());
    }
  }
}

TEST(NativeSourceHints, IndependentCallOnlyContractsRetainObservedContext) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(Mutation);
      NativeVoidFrameFixture F(Architecture);
      const auto Context =
          Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R12;
      SourceFunctionTypeHint Signature;
      Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
      Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error));
      auto Binding = std::make_shared<SourceCallTypeHint>();
      Binding->CallKind = SourceCallTypeHint::Kind::Native;
      Binding->TargetAddress = 0x1080;
      Binding->Signature = Signature;
      auto &Call = F.Med.Blocks[0].Ops[0];
      Call.SourceCallHint = Binding;
      Call.NumInputs = 1;
      // Consume an unmodified preserved context after the call, while the
      // surrounding frame saves/restores other preserved registers.
      MedVar Entry;
      Entry.Kind = MedVar::Reg;
      Entry.Id = 200;
      Entry.RegOff = Context;
      Entry.Size = 8;
      Entry.TheArch = Architecture;
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(MedVar::makeConst(0x3000, 8));
      Store.addInput(Entry);
      F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.begin() + 1, Store);
      F.Low.Blocks[0].Ops.insert(
          F.Low.Blocks[0].Ops.begin() + F.RestoreIndex,
          NativeVoidFrameFixture::op(
              NdOp::STORE, {},
              {NdVar::cst(0x3000, 8), NdVar::reg(Context, 8)}));
      NativeSourceCalleeContracts Contracts{&F.Image, {{0x1080, Signature}}};
      BinaryImage OtherImage;
      if (Mutation == 1)
        Contracts.ZeroArgumentPointerCallees.clear();
      if (Mutation == 2)
        Contracts.SourceImage = &OtherImage;
      if (Mutation == 3) {
        Contracts.ZeroArgumentPointerCallees.clear();
        Contracts.ZeroArgumentPointerCallees.emplace(0x1090, Signature);
      }
      if (Mutation == 4)
        Contracts.ZeroArgumentPointerCallees[0x1080].ReturnType =
            NdType::makeInt(8);
      if (Mutation == 5)
        Binding->TargetAddress = 0x1090;
      if (Mutation == 6)
        Binding->Signature.ReturnLocation.RegisterOffset += 8;
      if (Mutation == 7)
        ++F.Low.Blocks[0].Ops[F.CallIndex].Seq;
      if (Mutation == 8)
        F.Low.Blocks[0].Ops[F.RestoreIndex + 2].Opcode = NdOp::NOP;
      if (Mutation == 9)
        Binding->DoesNotReturn = true;
      if (Mutation == 10) {
        Binding->Signature.Parameters.push_back(
            {"hidden", NdType::makePtr(NdType::makeVoid())});
        ASSERT_TRUE(assignDarwinScalarSourceABI(Binding->Signature,
                                                Architecture, Error));
        Contracts.ZeroArgumentPointerCallees[0x1080] = Binding->Signature;
      }
      const auto Hint = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, &F.Low, false,
          Mutation == 11 ? nullptr : &Contracts);
      if (Mutation) {
        EXPECT_FALSE(Hint);
        EXPECT_FALSE(Error.empty());
      } else {
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
        ASSERT_EQ(Hint->Parameters.size(), 2U);
        EXPECT_EQ(Hint->Parameters.back().Location.RegisterOffset, Context);
        EXPECT_EQ(Hint->Parameters.back().Location.ValueBytes, 8U);
      }
    }
}

TEST(NativeSourceHints,
     CanonicalDynamicValueWitnessDestroyRestoresFramedState) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFrameFixture Fixture(Architecture);
    Fixture.useValueWitnessDestroy();
    std::string Error;
    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  }
}

TEST(NativeSourceHints, VoidFramesRestoreEntryBytesAcrossBranchesAndLoops) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Shape = 0; Shape < 4; ++Shape) {
      NativeVoidFrameFixture Fixture(Architecture);
      if (Shape == 3)
        Fixture.usePreindexedSpills();
      else if (Shape)
        Fixture.splitReturns(Shape == 2);
      std::string Error;
      const auto Hint = Fixture.inferVoid(Error);
      ASSERT_TRUE(Hint) << unsigned(Architecture) << ": " << Shape << ": "
                        << Error;
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Hint->ReturnLocation.Kind, SourceABICarrierKind::None);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
    }
}

TEST(NativeSourceHints, BoundObjCDispatchRequiresCompleteFramedCallEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (auto Kind : {SourceCallTypeHint::Kind::ObjCMessage,
                      SourceCallTypeHint::Kind::ObjCSuper2})
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        SCOPED_TRACE(Mutation);
        NativeVoidFrameFixture F(Architecture);
        auto &Call = F.Med.Blocks[0].Ops[0];
        auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
        Hint->CallKind = Kind;
        Hint->Signature.Origin =
            SourceFunctionTypeHint::OriginKind::ObjCRuntime;
        Hint->Signature.Parameters.push_back(
            {"selector", NdType::makePtr(NdType::makeVoid())});
        std::string Error;
        ASSERT_TRUE(
            assignDarwinObjCSourceABI(Hint->Signature, Architecture, Error));
        Call.addInput(MedVar::makeConst(0x1030, 8));
        Call.SourceCallHint = Hint;
        if (Mutation == 1)
          Hint->Signature.Origin =
              SourceFunctionTypeHint::OriginKind::NativeAnalysis;
        if (Mutation == 2)
          Hint->Signature.HasExplicitABI = false;
        if (Mutation == 3)
          Hint->DoesNotReturn = true;
        if (Mutation == 4)
          ++Call.OriginSeq;
        EXPECT_EQ(bool(F.inferVoid(Error)), Mutation == 0) << Error;
      }
}

TEST(NativeSourceHints, IndirectResultsNeedAnExplicitFramePreservationProof) {
  for (bool Framed : {false, true}) {
    NativeVoidFixture Fixture =
        Framed ? NativeVoidFixture(NativeVoidFrameFixture(Arch::AArch64))
               : NativeVoidFixture(Arch::AArch64);
    std::string Error;
    ASSERT_TRUE(Fixture.inferVoid(Error)) << Error;
    auto &Call = Fixture.Med.Blocks[0].Ops[0];
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = 0x1080;
    Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint->Signature.ReturnType = NdType::makeStruct(
        {NdType::makeInt(8), NdType::makeInt(8), NdType::makeInt(8)});
    ASSERT_TRUE(
        assignDarwinFixedSourceABI(Hint->Signature, Arch::AArch64, Error));
    Call.SourceCallHint = Hint;
    // The apparently unchanged explicit arguments hide a writable result
    // pointer into the frame. It may overwrite the saved register spills.
    auto &Ops = Fixture.Low.Blocks[0].Ops;
    const auto Where = std::find_if(Ops.begin(), Ops.end(), [](const auto &Op) {
      return Op.Opcode == NdOp::CALL;
    });
    ASSERT_NE(Where, Ops.end());
    const auto &TRI = getTargetRegInfo(Arch::AArch64);
    Ops.insert(Where, NativeVoidFrameFixture::op(
                          NdOp::COPY, NdVar::reg(TRI.indirectResultReg(), 8),
                          {NdVar::reg(TRI.StackPointer, 8)}, 0x100c));
    EXPECT_FALSE(Fixture.inferVoid(Error));
  }
}

TEST(NativeSourceHints,
     PreservedContextSurvivesBorrowedObjCSuperFrameArgument) {
  NativeVoidFrameFixture Fixture(Arch::AArch64);
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto Context = a64reg::X19;

  auto CallHint = std::make_shared<SourceCallTypeHint>(
      *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
  CallHint->CallKind = SourceCallTypeHint::Kind::ObjCSuper2;
  CallHint->Signature.Origin =
      SourceFunctionTypeHint::OriginKind::ObjCRuntime;
  CallHint->Signature.Parameters.push_back(
      {"selector", NdType::makePtr(NdType::makeVoid())});
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(CallHint->Signature, Arch::AArch64,
                                       Error));
  auto &MedCall = Fixture.Med.Blocks[0].Ops[0];
  MedCall.SourceCallHint = CallHint;

  MedVar ContextValue;
  ContextValue.Kind = MedVar::Reg;
  ContextValue.Id = 200;
  ContextValue.RegOff = Context;
  ContextValue.Size = 8;
  ContextValue.TheArch = Arch::AArch64;
  MedCall.addInput(ContextValue);

  auto &Ops = Fixture.Low.Blocks[0].Ops;
  // Keep the saved context live until the call instead of using it as a
  // scratch value immediately after the spill.
  ASSERT_EQ(Ops[3].Output, NdVar::reg(Context, 8));
  Ops.erase(Ops.begin() + 3);
  --Fixture.CallIndex;
  --Fixture.RestoreIndex;
  using F = NativeVoidFrameFixture;
  const auto SP = NdVar::reg(TRI.StackPointer, 8);
  Ops.insert(Ops.begin() + Fixture.CallIndex,
             {F::op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8), {SP}),
              F::op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[1], 8),
                    {NdVar::reg(Context, 8)})});

  const auto Hint = Fixture.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, Context);
  EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, 8U);

  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Changed = Fixture;
    auto &ChangedOps = Changed.Low.Blocks[0].Ops;
    auto &FrameArgument = ChangedOps[Changed.CallIndex];
    if (Mutation == 0) {
      auto Other = std::make_shared<SourceCallTypeHint>(
          *Changed.Med.Blocks[0].Ops[0].SourceCallHint);
      Other->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
      Changed.Med.Blocks[0].Ops[0].SourceCallHint = std::move(Other);
    } else if (Mutation == 1) {
      FrameArgument.Output.Size = FrameArgument.Inputs[0].Size = 4;
    } else if (Mutation < 4) {
      FrameArgument.Opcode = NdOp::INT_ADD;
      FrameArgument.addInput(NdVar::cst(Mutation == 2 ? 24 : uint64_t(-8), 8));
    } else
      ChangedOps[Changed.CallIndex + 1].Inputs[0] = NdVar::cst(0x1030, 8);
    const auto Rejected = Changed.inferVoid(Error);
    if (Rejected)
      EXPECT_EQ(Rejected->Parameters.size(), 1U) << Mutation;
    else
      EXPECT_FALSE(Error.empty()) << Mutation;
  }
}

TEST(NativeSourceHints, VoidFramesKeepBoundCallResultsInsideTheHelper) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeVoidFrameFixture Fixture(Architecture);
      const auto &TRI = getTargetRegInfo(Architecture);
      auto Hint = std::make_shared<SourceCallTypeHint>(
          *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
      Hint->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
      auto Call = Fixture.Med.Blocks[0].Ops[0];
      Call.Addr = 0x100c;
      Call.OriginSeq = 3;
      Call.SourceCallHint = Hint;
      Call.Output.Kind = MedVar::Reg;
      Call.Output.Id = 30;
      Call.Output.SSAVer = 1;
      Call.Output.TheArch = Architecture;
      Call.Output.RegOff = TRI.IntReturnReg;
      Call.Output.Size = 8;
      // The returned object is consumed by the following void release. It
      // cannot become a result of the enclosing helper after that call.
      Fixture.Med.Blocks[0].Ops[0].Inputs[1] = Call.Output;
      Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
      auto NativeCall = Fixture.Low.Blocks[0].Ops[Fixture.CallIndex];
      NativeCall.Addr = Call.Addr;
      NativeCall.Seq = Call.OriginSeq;
      Fixture.Low.Blocks[0].Ops.insert(
          Fixture.Low.Blocks[0].Ops.begin() + Fixture.CallIndex, NativeCall);
      if (Mutation == 1)
        Hint->Signature.ReturnLocation.ValueBytes = 4;
      if (Mutation == 2)
        Fixture.Med.Blocks[0].Ops[0].SourceCallHint.reset();
      if (Mutation == 3)
        ++Fixture.Low.Blocks[0].Ops[Fixture.CallIndex].Seq;
      if (Mutation == 4)
        Hint->DoesNotReturn = true;
      if (Mutation == 5) {
        // A typed result does not justify passing the private frame to a
        // callee or skipping the existing preservation/escape proof.
        Fixture.Low.Blocks[0].Ops.insert(
            Fixture.Low.Blocks[0].Ops.begin() + Fixture.CallIndex,
            NativeVoidFrameFixture::op(NdOp::COPY,
                                       NdVar::reg(TRI.IntParamRegs[0], 8),
                                       {NdVar::reg(TRI.StackPointer, 8)}));
      }
      const auto Result = Fixture.inferVoid(Error);
      if (Mutation) {
        EXPECT_FALSE(Result) << Error;
      } else {
        ASSERT_TRUE(Result) << Error;
        EXPECT_EQ(Result->ReturnType->Kind, NdTypeKind::Void);
        EXPECT_EQ(Result->ReturnLocation.Kind, SourceABICarrierKind::None);
        EXPECT_EQ(Result->ReturnLocation.ValueBytes, 0U);
      }
    }
}

TEST(NativeSourceHints, VoidFramesKeepSpillsAcrossDisjointExternalStores) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFrameFixture Fixture(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    Fixture.Low.Blocks[0].Ops.insert(
        Fixture.Low.Blocks[0].Ops.begin() + Fixture.CallIndex,
        NativeVoidFrameFixture::op(
            NdOp::STORE, {},
            {NdVar::reg(TRI.IntParamRegs[0], 8), NdVar::cst(0, 8)}));
    std::string Error;
    const auto Result = Fixture.inferVoid(Error);
    ASSERT_TRUE(Result) << unsigned(Architecture) << ": " << Error;
    EXPECT_EQ(Result->ReturnType->Kind, NdTypeKind::Void);
  }
}

TEST(NativeSourceHints,
     VoidFramesPreserveRestoredLowVectorBytesThroughNormalization) {
  NativeVoidFrameFixture Fixture(Arch::AArch64);
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const uint64_t V8 = TRI.VecRegBase + 8 * TRI.VecRegStride;
  auto &Ops = Fixture.Low.Blocks[0].Ops;
  const auto Restore = std::find_if(
      Ops.begin() + Fixture.RestoreIndex, Ops.end(), [&](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD && Op.Output == NdVar::reg(V8, 8);
      });
  ASSERT_NE(Restore, Ops.end());
  const size_t Insert = std::distance(Ops.begin(), Restore) + 1;
  Ops.insert(Ops.begin() + Insert,
             {NativeVoidFrameFixture::op(NdOp::INT_ZEXT,
                                         NdVar::reg(V8, 16),
                                         {NdVar::reg(V8, 8)}, 0x1020),
              NativeVoidFrameFixture::op(
                  NdOp::SUBBYTES, NdVar::reg(V8, 8),
                  {NdVar::reg(V8, 16), NdVar::cst(0, 8)}, 0x1020),
              NativeVoidFrameFixture::op(NdOp::INT_ZEXT,
                                         NdVar::reg(V8, 16),
                                         {NdVar::reg(V8, 8)}, 0x1020)});

  std::string Error;
  ASSERT_TRUE(Fixture.inferVoid(Error)) << Error;

  auto Shifted = Fixture;
  Shifted.Low.Blocks[0].Ops[Insert + 1].Inputs[1] = NdVar::cst(1, 8);
  EXPECT_FALSE(Shifted.inferVoid(Error));

  auto Truncated = Fixture;
  Truncated.Low.Blocks[0].Ops[Insert].Inputs[0].Size = 4;
  EXPECT_FALSE(Truncated.inferVoid(Error));
}

TEST(NativeSourceHints,
     SwiftBeginAccessUsesBoundedWritablePrivateFrameScratch) {
  NativeVoidFrameFixture Fixture(Arch::AArch64);
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  constexpr va_t ImportSlot = 0x2000;
  const std::string Import = "_swift_beginAccess";
  Segment Data;
  Data.VA = ImportSlot;
  Data.Size = Data.FileSz = 8;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(8);
  Fixture.Image.Segments.push_back(std::move(Data));
  Fixture.Image.ImportPtrSlots[ImportSlot] = Import;
  ASSERT_TRUE(Fixture.Image.recordDyldBindSlot(
      ImportSlot, Import, 0, "/usr/lib/swift/libswiftCore.dylib", false));
  const auto Runtime =
      swiftRuntimeSourceCallHint(Fixture.Image, ImportSlot);
  ASSERT_TRUE(Runtime);
  ASSERT_EQ(Runtime->Signature.Parameters.size(), 4U);

  auto &MedCall = Fixture.Med.Blocks[0].Ops[0];
  MedCall.SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(*Runtime);
  MedCall.NumInputs = 1;
  MedCall.addInput(Fixture.Med.Params[0]);
  MedCall.addInput(MedVar::makeConst(0, 8));
  MedCall.addInput(MedVar::makeConst(1, 8));
  MedCall.addInput(MedVar::makeConst(0, 8));

  auto &Ops = Fixture.Low.Blocks[0].Ops;
  const auto SP = NdVar::reg(TRI.StackPointer, 8);
  ASSERT_EQ(Ops.front().Opcode, NdOp::INT_SUB);
  Ops.front().Inputs[1] = NdVar::cst(96, 8);
  ASSERT_EQ(Ops[Fixture.RestoreIndex + Fixture.Saved.size() * 2].Opcode,
            NdOp::INT_ADD);
  Ops[Fixture.RestoreIndex + Fixture.Saved.size() * 2].Inputs[1] =
      NdVar::cst(96, 8);
  Fixture.FrameBytes = 96;
  Ops.insert(Ops.begin() + Fixture.CallIndex,
             NativeVoidFrameFixture::op(
                 NdOp::INT_ADD, NdVar::reg(TRI.IntParamRegs[1], 8),
                 {SP, NdVar::cst(32, 8)}));
  ++Fixture.CallIndex;
  ++Fixture.RestoreIndex;

  std::string Error;
  const auto Hint = Fixture.inferVoid(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);

  auto Overlap = Fixture;
  Overlap.Low.Blocks[0].Ops[Overlap.CallIndex - 1].Inputs[1] =
      NdVar::cst(0, 8);
  EXPECT_FALSE(Overlap.inferVoid(Error));

  auto Forged = Fixture;
  auto ForgedBinding = std::make_shared<SourceCallTypeHint>(
      *Forged.Med.Blocks[0].Ops[0].SourceCallHint);
  ForgedBinding->TargetName += "_forged";
  Forged.Med.Blocks[0].Ops[0].SourceCallHint = std::move(ForgedBinding);
  EXPECT_FALSE(Forged.inferVoid(Error));
}

TEST(NativeSourceHints, SwiftHasherBorrowsOnlyItsBoundedPrivateFrame) {
  for (const auto &Import :
       {"_$ss6HasherV5_seedABSi_tcfC", "_$sSS4hash4intoys6HasherVz_tF"}) {
    SCOPED_TRACE(Import);
    NativeVoidFrameFixture Fixture(Arch::AArch64);
    constexpr va_t ImportSlot = 0x2000;
    Segment Data;
    Data.VA = ImportSlot;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Fixture.Image.Segments.push_back(std::move(Data));
    Fixture.Image.ImportPtrSlots[ImportSlot] = Import;
    ASSERT_TRUE(Fixture.Image.recordDyldBindSlot(
        ImportSlot, Import, 0, "/usr/lib/swift/libswiftCore.dylib", false));
    const auto Runtime = swiftRuntimeSourceCallHint(Fixture.Image, ImportSlot);
    ASSERT_TRUE(Runtime);
    ASSERT_FALSE(Runtime->Signature.Parameters.empty());
    const auto BufferReg =
        Runtime->Signature.Parameters[0].Location.RegisterOffset;
    ASSERT_EQ(BufferReg, Import == std::string("_$ss6HasherV5_seedABSi_tcfC")
                             ? a64reg::X8
                             : a64reg::X0);

    auto &Ops = Fixture.Low.Blocks[0].Ops;
    const auto SP = NdVar::reg(a64reg::SP, 8);
    Ops.front().Inputs[1] = NdVar::cst(112, 8);
    Ops[Fixture.RestoreIndex + Fixture.Saved.size() * 2].Inputs[1] =
        NdVar::cst(112, 8);
    Ops.insert(Ops.begin() + Fixture.CallIndex,
               NativeVoidFrameFixture::op(NdOp::INT_ADD,
                                          NdVar::reg(BufferReg, 8),
                                          {SP, NdVar::cst(32, 8)}));
    ++Fixture.CallIndex;
    const auto Key = nativeSourceCallKey(Ops[Fixture.CallIndex]);
    ASSERT_TRUE(Key);
    NativeSourceCallContract Contract;
    Contract.Signature = &Runtime->Signature;
    Contract.WritableFrameParameters.emplace(0, 72);
    NativeSourceCalls Calls{{*Key, Contract}};
    EXPECT_TRUE(restoresNativeSourceState(Fixture.Low, Arch::AArch64, Calls));

    auto Overlap = Fixture;
    Overlap.Low.Blocks[0].Ops[Fixture.CallIndex - 1].Inputs[1] =
        NdVar::cst(24, 8);
    EXPECT_FALSE(restoresNativeSourceState(Overlap.Low, Arch::AArch64, Calls));

    Calls.at(*Key).WritableFrameParameters.clear();
    EXPECT_FALSE(restoresNativeSourceState(Fixture.Low, Arch::AArch64, Calls));
  }
}

TEST(NativeSourceHints, VoidFramesRejectClobbersEscapesAndStaleSpills) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 19; ++Mutation) {
      NativeVoidFrameFixture Fixture(Architecture);
      auto &Ops = Fixture.Low.Blocks[0].Ops;
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto SP = NdVar::reg(TRI.StackPointer, 8);
      const auto Word = NdVar::reg(Fixture.Saved[0], 8);
      using F = NativeVoidFrameFixture;
      switch (Mutation) {
      case 0:
        Ops[Fixture.RestoreIndex + 1].Output.Size = 4;
        break;
      case 1:
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::STORE, {}, {SP, NdVar::cst(0, 1)}));
        break;
      case 2: {
        // A partially copied stack address remains a possible private-frame
        // alias even though it is not a complete, exact frame address.
        Ops.insert(
            Ops.begin() + Fixture.CallIndex,
            {F::op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 4),
                   {NdVar::reg(TRI.StackPointer, 4)}),
             F::op(NdOp::STORE, {},
                   {NdVar::reg(TRI.IntParamRegs[0], 8), NdVar::cst(0, 8)})});
        break;
      }
      case 3:
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8), {SP}));
        break;
      case 4:
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::STORE, {}, {SP, SP}));
        break;
      case 5:
        Ops[Ops.size() - 2].Inputs[1].Offset -= 8;
        break;
      case 6:
        Ops[Fixture.RestoreIndex + 1] =
            F::op(NdOp::COPY, Word, {NdVar::reg(TRI.IntReturnReg, 8)}, 0x1020);
        break;
      case 7:
        Ops[1].Inputs[1].Offset = Fixture.FrameBytes;
        break;
      case 8:
        Ops[2].MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 9:
        // The spill address is an instruction-local temporary, not a value
        // available merely because a later instruction reuses its number.
        Ops[2].Addr += 4;
        break;
      case 10: {
        const int64_t Exposed =
            Fixture.FrameBytes - (Architecture == Arch::X64 ? 8 : 0);
        Ops.insert(Ops.begin() + Fixture.CallIndex,
                   F::op(NdOp::INT_ADD, SP, {SP, NdVar::cst(Exposed, 8)}));
        Ops.insert(
            Ops.begin() + Fixture.RestoreIndex + 1,
            F::op(NdOp::INT_SUB, SP, {SP, NdVar::cst(Exposed, 8)}, 0x1020));
        break;
      }
      case 11:
        Fixture.splitReturns();
        Fixture.Low.Blocks[2].Ops[1].Output.Size = 4;
        break;
      case 12:
        Fixture.splitReturns();
        Fixture.Low.Blocks[2].Preds.clear();
        break;
      case 13:
        Fixture.splitReturns(true);
        Fixture.Low.Blocks[1].Ops = {
            F::op(NdOp::INT_SUB, SP, {SP, NdVar::cst(8, 8)})};
        break;
      case 14:
        if (Architecture == Arch::AArch64)
          Ops.back().Inputs[0] = NdVar::reg(TRI.IntReturnReg, 8);
        else
          Ops.insert(Ops.end() - 1,
                     F::op(NdOp::STORE, {}, {SP, NdVar::cst(0, 8)}, 0x1020));
        break;
      case 15:
        // A four-byte constant is zero-extended by the eight-byte ADD;
        // interpreting its high bit as a signed frame delta invents a spill.
        Ops[0].Opcode = NdOp::INT_ADD;
        Ops[0].Inputs[1] = NdVar::cst(uint32_t(-Fixture.FrameBytes), 4);
        break;
      case 16:
        Ops.insert(
            Ops.begin() + Fixture.CallIndex,
            F::op(NdOp::INDIR_BR, {}, {NdVar::reg(TRI.IntParamRegs[0], 8)}));
        break;
      case 17:
        // A stack address below the current SP is not allocated storage.
        Ops[1].Inputs[1] = NdVar::cst(uint64_t(-8), 8);
        break;
      case 18: {
        // A later instruction cannot retroactively allocate a stack slot.
        const auto StackWrite = Fixture.usePreindexedSpills();
        Ops[StackWrite].Addr += 4;
        break;
      }
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferVoid(Error))
          << unsigned(Architecture) << ": " << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints, VoidFrameByteProofHonorsImplicitZeroExtensions) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFrameFixture Fixture(Architecture);
    auto &Ops = Fixture.Low.Blocks[0].Ops;
    // Leave the original upper bytes untouched before loading only the low
    // word. That load implicitly clears the upper word on both architectures.
    Ops[3] = NativeVoidFrameFixture::op(NdOp::NOP, {}, {});
    Ops[Fixture.RestoreIndex + 1].Output.Size = 4;
    std::string Error;
    EXPECT_FALSE(Fixture.inferVoid(Error));
  }
}

std::pair<HighFunc, PipelineFunctionAudit>
nativeVoidInputCandidate(Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Architecture = Architecture;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = NdType::makeVoid();
  const auto Auxiliary =
      Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::RAX;
  Hint.Parameters = {
      {"ordinary",
       NdType::makeInt(8),
       {SourceABICarrierKind::IntegerRegister, TRI.IntParamRegs[0], 0, 8}},
      {"auxiliary",
       NdType::makeInt(8),
       {SourceABICarrierKind::IntegerRegister, Auxiliary, 0, 8}}};
  HighFunc Function;
  Function.Entry = 0x1000;
  Function.SourceTypeHint = Hint;
  Function.ReturnType = Hint.ReturnType;
  Function.Params = {{"ordinary", NdType::makeInt(8)},
                     {"auxiliary", NdType::makeInt(8)}};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Function.Body.push_back(Return);
  PipelineFunctionAudit Audit;
  Audit.Entry = Function.Entry;
  Audit.Disposition = PipelineFunctionDisposition::Accepted;
  Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  Audit.DecodedInstructions = Audit.LiftedInstructions = 1;
  return {std::move(Function), std::move(Audit)};
}

TEST(NativeSourceHints, RefinesOnlyUnusedAuxiliaryVoidSourceInputs) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
    const auto Refined = refineNativeSourceTypeHint(Function, Audit);
    ASSERT_TRUE(Refined);
    ASSERT_EQ(Refined->Parameters.size(), 1U);
    EXPECT_EQ(Refined->Parameters[0].Name, "ordinary");
    EXPECT_EQ(Refined->Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    EXPECT_EQ(Function.SourceTypeHint->Parameters.size(), 2U);
    std::string Error;
    EXPECT_TRUE(validateSourceABI(*Refined, Error)) << Error;
  }
}

TEST(NativeSourceHints, ScalarInputRefinementPreservesReturnEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
      auto &Hint = *Function.SourceTypeHint;
      Hint.ReturnType = Function.ReturnType = NdType::makeInt(8);
      Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                             getTargetRegInfo(Architecture).IntReturnReg, 0, 8};
      auto &Value = Function.Body.front().RetVal;
      Value = HighExpr::makeConst(19, 8);
      if (Mutation == 1) {
        MedVar Input;
        Input.Kind = MedVar::Param;
        Input.Id = 1;
        Input.Size = 8;
        Value = HighExpr::makeVar(Input, NdType::makeInt(8));
      } else if (Mutation == 2) {
        Value.reset();
      } else if (Mutation == 3) {
        Value = HighExpr::makeUndef(8);
      } else if (Mutation == 4) {
        Value = HighExpr::makeConst(19, 4);
      }
      const auto Refined = refineNativeSourceTypeHint(Function, Audit);
      if (Mutation) {
        EXPECT_FALSE(Refined);
        continue;
      }
      ASSERT_TRUE(Refined);
      ASSERT_EQ(Refined->Parameters.size(), 1U);
      EXPECT_EQ(Refined->Parameters[0].Name, "ordinary");
      EXPECT_TRUE(equalSourceTypes(Refined->ReturnType, Hint.ReturnType));
      EXPECT_EQ(Refined->ReturnLocation.RegisterOffset,
                Hint.ReturnLocation.RegisterOffset);
    }
}

TEST(NativeSourceHints, DefinedLocalsDoNotRetainUnusedPhysicalInputRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
      auto &Hint = *Function.SourceTypeHint;
      Hint.ReturnType = Function.ReturnType = NdType::makeInt(8);
      Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                             getTargetRegInfo(Architecture).IntReturnReg, 0, 8};
      MedVar Local;
      Local.Kind = MedVar::Reg;
      Local.Id = 50001;
      Local.RegOff = Hint.Parameters[1].Location.RegisterOffset;
      Local.Size = 8;
      Local.TheArch = Architecture;
      Function.Body.front().RetVal =
          HighExpr::makeVar(Local, NdType::makeInt(8));
      HighStmt Assign;
      Assign.Kind = StmtKind::Assign;
      auto Dst = Local;
      if (Mutation == 4)
        Dst.Size = 4;
      Assign.Dst = HighExpr::makeVar(Dst, NdType::makeInt(Dst.Size));
      Assign.Val = HighExpr::makeConst(19, Dst.Size);
      if (Mutation == 2 || Mutation == 3) {
        HighStmt Branch;
        Branch.Kind = StmtKind::If;
        auto Input = Local;
        Input.Kind = MedVar::Param;
        Input.Id = 0;
        Input.RegOff = Hint.Parameters[0].Location.RegisterOffset;
        Branch.Cond = HighExpr::makeVar(Input, NdType::makeInt(8));
        Branch.Body = {Assign};
        if (Mutation == 3)
          Branch.ElseBody = {Assign};
        Function.Body.insert(Function.Body.begin(), Branch);
      } else if (Mutation != 1) {
        Function.Body.insert(Function.Body.begin(), Assign);
      }
      const auto Refined = refineNativeSourceTypeHint(Function, Audit);
      EXPECT_EQ(bool(Refined), Mutation == 0);
      if (Refined)
        EXPECT_EQ(Refined->Parameters.size(), 1U);
    }
}

TEST(NativeSourceHints, SourceInputRefinementRetainsUsesAndIncompleteBodies) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 23; ++Mutation) {
      auto [Function, Audit] = nativeVoidInputCandidate(Architecture);
      MedVar Input;
      Input.Kind = MedVar::Param;
      Input.Id = 1;
      Input.Size = 8;
      Input.TheArch = Architecture;
      Input.RegOff =
          Function.SourceTypeHint->Parameters[1].Location.RegisterOffset;
      auto Use = HighExpr::makeVar(Input, NdType::makeInt(8));
      HighStmt Statement;
      Statement.Kind = StmtKind::ExprStmt;
      Statement.Val = Use;
      switch (Mutation) {
      case 0:
        Function.Body.insert(Function.Body.begin(), Statement);
        break;
      case 1:
        Statement.Kind = StmtKind::Assign;
        Statement.Dst = Use;
        Statement.Val = HighExpr::makeConst(0, 8);
        Function.Body.insert(Function.Body.begin(), Statement);
        break;
      case 2:
        Function.Body[0].DefaultBody.push_back(Statement);
        break;
      case 3:
        Function.Body[0].EHClauseBodies.push_back({Statement});
        break;
      case 4:
        Function.Body[0].Cond = HighExpr::makeConst(0, 8);
        Function.Body[0].Cond->IntrinsicOutputs.push_back(Input);
        break;
      case 5:
        Input.Kind = MedVar::Reg;
        Input.SSAVer = 2;
        Input.RegOff += 4;
        Input.Size = 4;
        Function.Body[0].Cond = HighExpr::makeVar(Input, NdType::makeInt(4));
        break;
      case 6:
        Input.Id = 4;
        Function.Body[0].Cond = HighExpr::makeVar(Input, NdType::makeInt(8));
        break;
      case 7:
        Function.Params[1].Type = NdType::makeInt(4);
        break;
      case 8:
        Function.SourceTypeHint->Origin =
            SourceFunctionTypeHint::OriginKind::ObjCSDK;
        break;
      case 9:
        Function.Body.clear();
        break;
      case 10:
        Function.StructuredExceptionRegions = 1;
        break;
      case 11:
        Function.Body[0].Kind = StmtKind::Nop;
        break;
      case 12:
        Function.ReturnType = NdType::makeInt(8);
        break;
      case 13:
        Function.Body[0].Cond = HighExpr::makeConst(0, 8);
        Function.Body[0].Cond->Kind = ExprKind::Undef;
        break;
      case 14:
        Function.SourceTypeHint.reset();
        break;
      case 15: {
        auto Expression = HighExpr::makeConst(0, 8);
        for (unsigned I = 0; I < 130; ++I) {
          auto Outer = std::make_shared<HighExpr>();
          Outer->Kind = ExprKind::Cast;
          Outer->Type = NdType::makeInt(8);
          Outer->Operands = {Expression};
          Expression = Outer;
        }
        Function.Body[0].Cond = Expression;
        break;
      }
      case 16:
        Audit.Disposition = PipelineFunctionDisposition::Candidate;
        break;
      case 17:
        Audit.HasLowIR = false;
        break;
      case 18:
        Audit.DecodeFailures.push_back(0x1000);
        break;
      case 19:
        ++Audit.Entry;
        break;
      case 20:
        Audit.TruncatedPaths.push_back(0x1000);
        break;
      case 21:
        Function.Body[0].Cases.resize(65537);
        break;
      case 22:
        Function.Body[0].Cond = HighExpr::makeConst(0, 8);
        Function.Body[0].Cond->IntrinsicOutputs.resize(65537);
        break;
      }
      EXPECT_FALSE(refineNativeSourceTypeHint(Function, Audit)) << Mutation;
    }
}

struct NativeFloatingFixture : NativeFixture {
  NativeFloatingFixture(Arch Architecture, unsigned Width, bool Wide = false)
      : NativeFixture(Architecture) {
    const auto &TRI = getTargetRegInfo(Architecture);
    Med.ReturnType = High.ReturnType = NdType::makeFloat(Width);
    for (unsigned I = 0; I < 2; ++I) {
      Med.Params[I].RegOff = TRI.FPParamRegs[I];
      Med.Params[I].Size = Wide ? 16 : Width;
      Med.TypedParams[I].Type = High.Params[I].Type =
          NdType::makeInt(Med.Params[I].Size);
    }
    auto &Ops = Med.Blocks[0].Ops;
    auto Sum = Ops.front();
    auto Return = Ops.back();
    Sum.Opcode = NdOp::FLOAT_ADD;
    Sum.Output.RegOff = TRI.FPReturnReg;
    Sum.Output.Size = Width;
    Sum.Inputs[0] = Med.Params[0];
    Sum.Inputs[1] = Med.Params[1];
    Ops.clear();
    if (Wide) {
      for (unsigned I = 0; I < 2; ++I) {
        MedOp Extract;
        Extract.Opcode = NdOp::SUBBYTES;
        Extract.Output = Sum.Output;
        Extract.Output.Kind = MedVar::Temp;
        Extract.Output.Id = 20 + I;
        Extract.addInput(Med.Params[I]);
        Extract.addInput(MedVar::makeConst(0, 8));
        Ops.push_back(Extract);
        Sum.Inputs[I] = Extract.Output;
      }
      Sum.Output.Kind = MedVar::Temp;
      Ops.push_back(Sum);
      auto Upper = Ops.front();
      Upper.Output.Id = 22;
      Upper.Output.Size = 16 - Width;
      Upper.Inputs[1].ConstVal = Width;
      Ops.push_back(Upper);
      MedOp Join;
      Join.Opcode = NdOp::CONCAT;
      Join.Output = Sum.Output;
      Join.Output.Kind = MedVar::Reg;
      Join.Output.Id = 23;
      Join.Output.Size = 16;
      Join.addInput(Upper.Output);
      Join.addInput(Sum.Output);
      Ops.push_back(Join);
    } else {
      Ops.push_back(Sum);
    }
    Ops.push_back(Return);
  }
};

TEST(NativeSourceHints, FloatingLanesPreserveScalarParametersAndResults) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Width : {4U, 8U})
      for (bool Wide : {false, true}) {
        NativeFloatingFixture Fixture(Architecture, Width, Wide);
        std::string Error;
        const auto Hint = Fixture.infer(Error);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnLocation.Kind,
                  SourceABICarrierKind::FloatingRegister);
        EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
                  getTargetRegInfo(Architecture).FPReturnReg);
        EXPECT_EQ(Hint->ReturnLocation.ValueBytes, Width);
        ASSERT_EQ(Hint->Parameters.size(), 2U);
        const auto Bytes = observedMedSourceEntryBytes(Fixture.Med, *Hint);
        ASSERT_TRUE(Bytes);
        for (unsigned I = 0; I < 2; ++I) {
          EXPECT_EQ(Bytes->at(getTargetRegInfo(Architecture).FPParamRegs[I]),
                    (uint64_t(1) << Width) - 1);
          EXPECT_EQ(Hint->Parameters[I].Type->Kind, NdTypeKind::Float);
          EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset,
                    getTargetRegInfo(Architecture).FPParamRegs[I]);
          EXPECT_EQ(Hint->Parameters[I].Location.ValueBytes, Width);
          EXPECT_EQ(Fixture.Med.TypedParams[I].Type->Kind, NdTypeKind::Int);
          EXPECT_EQ(Fixture.Med.Params[I].Size, Wide ? 16 : Width);
        }
        EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                  MedReturnValueEvidence::Unknown);
      }
}

TEST(NativeSourceHints, FloatingReturnPathsRequireCompleteDefinedLanes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      NativeFloatingFixture Fixture(Architecture, 8);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      switch (Mutation) {
      case 0:
        Ops[0].Output.Size = 4;
        break;
      case 1:
        Ops[0].Output.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
        Fixture.Med.Params[0].RegOff =
            getTargetRegInfo(Architecture).FPParamRegs[2];
        Ops[0].Inputs[0] = Fixture.Med.Params[0];
        break;
      case 2:
        Ops[0].Output.RegOff = getTargetRegInfo(Architecture).FPParamRegs[1];
        Fixture.Med.Params[0].RegOff =
            getTargetRegInfo(Architecture).FPParamRegs[2];
        Ops[0].Inputs[0] = Fixture.Med.Params[0];
        break;
      case 3: {
        MedOp Call;
        Call.Opcode = NdOp::CALL;
        Call.addInput(MedVar::makeConst(0x1080, 8));
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->Signature.ReturnType = NdType::makeVoid();
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
        Call.SourceCallHint = std::move(Hint);
        Ops.insert(Ops.end() - 1, Call);
        break;
      }
      case 4: {
        const auto Return = Ops.back();
        Fixture.Med.Blocks.emplace_back();
        Fixture.Med.Blocks.back().Id = 1;
        Fixture.Med.Blocks.back().Ops = {Return};
        break;
      }
      }
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints, FloatingParametersRequireBoundedScalarEntryBytes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      NativeFloatingFixture Fixture(Architecture, 8, true);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      switch (Mutation) {
      case 0: {
        MedOp Store;
        Store.Opcode = NdOp::STORE;
        Store.addInput(MedVar::makeConst(0x1080, 8));
        Store.addInput(Fixture.Med.Params[0]);
        Ops.insert(Ops.end() - 1, Store);
        break;
      }
      case 1:
        Ops[0].Inputs[1].ConstVal = 8;
        break;
      case 2:
        Fixture.Med.Params[0].RegOff =
            getTargetRegInfo(Architecture).IntParamRegs[0];
        break;
      case 3:
        Fixture.Med.Params[0].RegOff = Fixture.Med.Params[1].RegOff;
        break;
      case 4:
        Fixture.Med.TypedParams[0].Type = NdType::makePtr(NdType::makeVoid());
        break;
      case 5:
        Ops[0].Inputs[0].Size = 4;
        break;
      }
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation << ": " << Error;
    }
}

TEST(NativeSourceHints,
     FloatingParameterEffectsSurviveAnUnprovedProvisionalReturn) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeVoidFixture Fixture(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    auto &Parameter = Fixture.Med.Params[1];
    Parameter.Id = 1;
    Parameter.RegOff = TRI.FPParamRegs[0];
    Parameter.Size = 16;
    Parameter.TheArch = Architecture;
    Fixture.Med.TypedParams[1].Type = Fixture.High.Params[1].Type =
        NdType::makeInt(16);

    auto Binding = std::make_shared<SourceCallTypeHint>(
        *Fixture.Med.Blocks[0].Ops[0].SourceCallHint);
    Binding->Signature.Parameters.push_back(
        {"value", NdType::makeFloat(8)});
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Binding->Signature, Architecture,
                                            Error));

    MedOp Extract;
    Extract.Opcode = NdOp::SUBBYTES;
    Extract.Output.Kind = MedVar::Temp;
    Extract.Output.Id = 80;
    Extract.Output.Size = 8;
    Extract.Output.TheArch = Architecture;
    Extract.addInput(Parameter);
    Extract.addInput(MedVar::makeConst(0, 8));
    auto &Call = Fixture.Med.Blocks[0].Ops[0];
    Call.SourceCallHint = std::move(Binding);
    Call.addInput(Extract.Output);
    Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(),
                                     std::move(Extract));

    const auto Hint = Fixture.inferVoid(Error);
    ASSERT_TRUE(Hint) << unsigned(Architecture) << ": " << Error;
    EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Float);
    EXPECT_EQ(Hint->Parameters[1].Type->Size, 8U);
    EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset,
              TRI.FPParamRegs[0]);
    EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, 8U);
  }
}

struct NativeRecordResultFixture : NativeFixture {
  explicit NativeRecordResultFixture(Arch Architecture, bool Swift = false)
      : NativeFixture(Architecture) {
    Med.Params.clear();
    Med.TypedParams.clear();
    High.Params.clear();
    Med.ReturnType = High.ReturnType = NdType::makeInt(8, false);
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Hint->Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
    Hint->Signature.ReturnType = NdType::makeStruct(
        {NdType::makePtr(NdType::makeVoid()), NdType::makeInt(8, false)});
    std::string Error;
    const bool Assigned =
        Swift
            ? assignDarwinSwiftSourceABI(Hint->Signature, Architecture, Error)
            : assignDarwinFixedSourceABI(Hint->Signature, Architecture, Error);
    EXPECT_TRUE(Assigned) << Error;
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Output.Kind = MedVar::Temp;
    Call.Output.Id = 100;
    Call.Output.SSAVer = 1;
    Call.Output.Size = 16;
    Call.Output.TheArch = Architecture;
    Call.SourceCallHint = Hint;
    Call.addInput(MedVar::makeConst(0x1080, 8));
    const auto Return = Med.Blocks[0].Ops.back();
    Med.Blocks[0].Ops = {Call};
    const auto &TRI = getTargetRegInfo(Architecture);
    for (unsigned I = 0; I < 2; ++I) {
      MedOp Extract;
      Extract.Opcode = NdOp::SUBBYTES;
      Extract.Output = Call.Output;
      Extract.Output.Kind = MedVar::Reg;
      Extract.Output.Id = 101 + I;
      Extract.Output.Size = 8;
      Extract.Output.RegOff = TRI.IntReturnRegs[I];
      Extract.addInput(Call.Output);
      Extract.addInput(MedVar::makeConst(I * 8, 8));
      Med.Blocks[0].Ops.push_back(Extract);
    }
    Med.Blocks[0].Ops.push_back(Return);
  }
};

TEST(NativeSourceHints, BoundRecordArgumentsUsePhysicalComponents) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool PairResult : {false, true})
      for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
        SCOPED_TRACE(Mutation);
        NativeRecordResultFixture F(Architecture);
        auto &Call = F.Med.Blocks[0].Ops[0];
        auto Binding =
            std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
        Binding->Signature.Parameters = {
            {"range", NdType::makeStruct({NdType::makeInt(8, false),
                                          NdType::makeInt(8, false)})}};
        std::string Error;
        ASSERT_TRUE(assignDarwinFixedSourceABI(Binding->Signature, Architecture,
                                               Error));
        Call.addInput(MedVar::makeConst(17, 8));
        if (Mutation != 1)
          Call.addInput(MedVar::makeConst(31, 8));
        if (Mutation == 2)
          Call.addInput(MedVar::makeConst(91, 8));
        if (Mutation == 3)
          Binding->Signature.Parameters[0].Components.pop_back();
        Call.SourceCallHint = Binding;
        const auto Hint = inferNativeSourceTypeHint(
            F.Image, F.Med, F.High, F.Audit, Error, nullptr, PairResult);
        EXPECT_EQ(bool(Hint), Mutation == 0) << Error;
        if (Hint)
          EXPECT_EQ(Hint->ReturnComponents.size(), PairResult ? 2U : 0U);
      }
}

TEST(NativeSourceHints, TypedRecordCallResultsDefineTheNativeReturnCarrier) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Swift : {false, true})
      for (bool SeparateReturn : {false, true}) {
        NativeRecordResultFixture Fixture(Architecture, Swift);
        if (SeparateReturn) {
          const auto Return = Fixture.Med.Blocks[0].Ops.back();
          Fixture.Med.Blocks[0].Ops.pop_back();
          Fixture.Med.Blocks[0].Succs = {1};
          Fixture.Med.Blocks.emplace_back();
          Fixture.Med.Blocks[1].Id = 1;
          Fixture.Med.Blocks[1].Preds = {0};
          Fixture.Med.Blocks[1].Ops = {Return};
        }
        std::string Error;
        const auto Hint = Fixture.infer(Error);
        ASSERT_TRUE(Hint) << Error;
        EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
                  getTargetRegInfo(Architecture).IntReturnReg);
        EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 8U);
        EXPECT_TRUE(Hint->Parameters.empty());
        EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                  MedReturnValueEvidence::Unknown);
      }
}

TEST(NativeSourceHints, IntegerPairReturnsPreserveBothWordsOfBoundRecordCalls) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Swift : {false, true}) {
      NativeRecordResultFixture F(Architecture, Swift);
      std::string Error;
      const auto Pair = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
      ASSERT_TRUE(Pair) << Error;
      EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Struct);
      EXPECT_EQ(Pair->ReturnComponents.size(), 2U);
      // A later scalar call clobbers the second result even when its first
      // result is a complete eight-byte value.
      auto Call = F.Med.Blocks[0].Ops[0];
      auto Binding = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
      Binding->Signature.ReturnType = NdType::makeInt(8);
      ASSERT_TRUE(
          assignDarwinFixedSourceABI(Binding->Signature, Architecture, Error));
      Call.SourceCallHint = Binding;
      Call.Output = F.Med.Blocks[0].Ops[1].Output;
      F.Med.Blocks[0].Ops.insert(F.Med.Blocks[0].Ops.end() - 1, Call);
      const auto Scalar = inferNativeSourceTypeHint(
          F.Image, F.Med, F.High, F.Audit, Error, nullptr, true);
      ASSERT_TRUE(Scalar) << Error;
      EXPECT_TRUE(Scalar->ReturnComponents.empty());
    }
}

TEST(NativeSourceHints, RecordResultDefinitionsRequireTheCompleteCallPrefix) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeRecordResultFixture Fixture(Architecture);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      switch (Mutation) {
      case 0:
        Ops.erase(Ops.begin() + 2);
        break;
      case 1:
        std::swap(Ops[1], Ops[2]);
        break;
      case 2:
        Ops[1].Inputs[1].ConstVal = 8;
        break;
      case 3:
        ++Ops[1].Inputs[0].Id;
        break;
      case 4:
        ++Ops[1].Inputs[0].SSAVer;
        break;
      case 5:
        Ops[1].Inputs[0].Size = 8;
        break;
      case 6:
        Ops[1].Output.Size = 4;
        break;
      case 7:
        Ops[2].Output.Size = 4;
        break;
      case 8:
        Ops[1].Output.RegOff = getTargetRegInfo(Architecture).IntReturnRegs[1];
        break;
      case 9:
        Ops[0].Output.Size = 8;
        break;
      case 10:
        Ops[0].SourceCallHint.reset();
        break;
      case 11: {
        auto Hint =
            std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
        Hint->Signature.ReturnComponents.pop_back();
        Ops[0].SourceCallHint = std::move(Hint);
        break;
      }
      case 12: {
        auto Copy = Ops[1];
        Copy.Opcode = NdOp::COPY;
        Copy.Output = Ops[0].Output;
        Copy.Output.Id = 200;
        Copy.NumInputs = 1;
        Ops.insert(Ops.begin() + 2, Copy);
        break;
      }
      case 13: {
        auto Clobber = Ops[0];
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->Signature.ReturnType = NdType::makeVoid();
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error));
        Clobber.SourceCallHint = std::move(Hint);
        Clobber.Output = {};
        Ops.insert(Ops.end() - 1, Clobber);
        break;
      }
      case 14:
        Ops[2].Inputs[1].ConstVal = 0;
        break;
      }
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Error;
    }
}

struct NativeTerminalContextFixture : NativeFixture {
  LowFunc Low;
  static constexpr va_t ImportSlot = 0x2000;
  static constexpr uint64_t Context = a64reg::X20;

  NativeTerminalContextFixture() : NativeFixture(Arch::AArch64) {
    const std::string Import =
        "_$ss17_assertionFailure__4file4line5flagss5NeverOs12StaticStringV_"
        "SSAHSus6UInt32VtF";
    Segment Data;
    Data.VA = ImportSlot;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Image.Segments.push_back(std::move(Data));
    Image.ImportPtrSlots[ImportSlot] = Import;
    EXPECT_TRUE(Image.recordDyldBindSlot(
        ImportSlot, Import, 0, "/usr/lib/swift/libswiftCore.dylib", false));
    const auto Runtime = swiftRuntimeSourceCallHint(Image, ImportSlot);
    EXPECT_TRUE(Runtime);
    if (!Runtime)
      return;
    Med.Params.clear();
    Med.TypedParams.clear();
    High.Params.clear();
    Med.DoesNotReturn = High.DoesNotReturn = true;
    MedVar ContextValue;
    ContextValue.Kind = MedVar::Reg;
    ContextValue.Id = 200;
    ContextValue.RegOff = Context;
    ContextValue.Size = 8;
    ContextValue.TheArch = Arch::AArch64;
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.Addr = 0x1004;
    Store.addInput(ContextValue);
    Store.addInput(MedVar::makeConst(0, 8));
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = 0x1010;
    Call.OriginSeq = 0;
    Call.DoesNotReturn = true;
    Call.SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Runtime);
    // Import metadata names the data slot; machine calls name the veneer.
    Call.addInput(MedVar::makeConst(0x1080, 8));
    std::vector<ExprPtr> Arguments;
    for (const auto &Parameter : Runtime->Signature.Parameters) {
      Call.addInput(MedVar::makeConst(0, Parameter.Type->Size));
      Arguments.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
    }
    Med.Blocks[0].Ops = {Store, Call};
    HighStmt Statement;
    Statement.Kind = StmtKind::Call;
    Statement.CallExpr = HighExpr::makeCall(Runtime->TargetName, 0x1080,
                                           std::move(Arguments));
    Statement.CallExpr->Type = NdType::makeVoid();
    Statement.CallExpr->SourceCallHint = Call.SourceCallHint;
    High.Body = {Statement};
    Low.Entry = Med.Entry;
    Low.Blocks.emplace_back();
    Low.Blocks[0].Id = 0;
    using F = NativeVoidFrameFixture;
    const auto SP = NdVar::reg(a64reg::SP, 8);
    const auto Address = NdVar::tmp(TmpBase, 8);
    Low.Blocks[0].Ops = {
        F::op(NdOp::INT_SUB, SP, {SP, NdVar::cst(32, 8)}, 0x1000),
        F::op(NdOp::STORE, {},
              {NdVar::reg(Context, 8), NdVar::cst(0, 8)}, 0x1004),
        F::op(NdOp::STORE, {}, {SP, NdVar::cst(115, 8)}, 0x1008),
        F::op(NdOp::INT_ADD, Address, {SP, NdVar::cst(8, 8)}, 0x100c),
        F::op(NdOp::STORE, {}, {Address, NdVar::cst(0, 4)}, 0x100c),
        F::op(NdOp::CALL, {}, {NdVar::cst(0x1080, 8)}, 0x1010)};
  }

  static constexpr va_t ReturningImportSlot = 0x2010;

  void addReturningRuntimeCall() {
    Segment Data;
    Data.VA = ReturningImportSlot;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Image.Segments.push_back(std::move(Data));
    const std::string Import = "_swift_unknownObjectWeakInit";
    Image.ImportPtrSlots[ReturningImportSlot] = Import;
    ASSERT_TRUE(Image.recordDyldBindSlot(
        ReturningImportSlot, Import, 0, "/usr/lib/swift/libswiftCore.dylib",
        false));
    const auto Runtime = swiftRuntimeSourceCallHint(Image, ReturningImportSlot);
    ASSERT_TRUE(Runtime);
    ASSERT_FALSE(Runtime->DoesNotReturn);
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = 0x1006;
    Call.OriginSeq = 0;
    Call.SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Runtime);
    Call.addInput(MedVar::makeConst(0x1090, 8));
    for (const auto &Parameter : Runtime->Signature.Parameters)
      Call.addInput(MedVar::makeConst(0, Parameter.Type->Size));
    Med.Blocks[0].Ops.insert(Med.Blocks[0].Ops.begin() + 1, Call);
    Low.Blocks[0].Ops.insert(
        Low.Blocks[0].Ops.begin() + 2,
        NativeVoidFrameFixture::op(NdOp::CALL, {}, {NdVar::cst(0x1090, 8)},
                                   0x1006));
  }

  std::optional<SourceFunctionTypeHint> inferContext(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error, &Low);
  }
};

TEST(NativeSourceHints, TerminalEntryBytesRequireEffectsOnlyAndProvenExit) {
  NativeTerminalContextFixture Fixture;
  SourceFunctionTypeHint Signature;
  Signature.ReturnType = NdType::makeVoid();
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error));
  EXPECT_FALSE(observedMedSourceEntryBytes(Fixture.Med, Signature));
  const auto Bytes = observedMedSourceEntryBytes(
      Fixture.Med, Signature, SourceEntryDemand::EffectsOnly);
  ASSERT_TRUE(Bytes);
  ASSERT_EQ(Bytes->size(), 1U);
  EXPECT_EQ(Bytes->at(Fixture.Context), 0xffU);
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    auto Changed = Fixture.Med;
    if (Mutation == 0)
      Changed.DoesNotReturn = false;
    else if (Mutation == 1)
      Changed.Blocks[0].Ops.back().DoesNotReturn = false;
    else {
      Changed.Blocks[0].Ops.pop_back();
      if (Mutation == 3)
        Changed.Blocks[0].Succs = {99};
    }
    EXPECT_FALSE(observedMedSourceEntryBytes(
        Changed, Signature, SourceEntryDemand::EffectsOnly)) << Mutation;
  }
}

TEST(NativeSourceHints, TerminalRuntimeRetainsAnObservedUnwrittenContext) {
  NativeTerminalContextFixture Fixture;
  std::string Error;
  const auto Hint = Fixture.inferContext(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, Fixture.Context);
  EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 8U);

  const auto WithoutLow = Fixture.infer(Error);
  ASSERT_TRUE(WithoutLow) << Error;
  EXPECT_TRUE(WithoutLow->Parameters.empty());
}

TEST(NativeSourceHints, TerminalContextSurvivesAnAuthenticatedRuntimePrefix) {
  NativeTerminalContextFixture Fixture;
  Fixture.addReturningRuntimeCall();
  std::string Error;
  const auto Hint = Fixture.inferContext(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, Fixture.Context);
  EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, 8U);
}

TEST(NativeSourceHints, TerminalRuntimePrefixRequiresIndependentCallEvidence) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeTerminalContextFixture F;
    F.addReturningRuntimeCall();
    auto &Ops = F.Low.Blocks[0].Ops;
    auto &MedOps = F.Med.Blocks[0].Ops;
    auto Binding = std::make_shared<SourceCallTypeHint>(*MedOps[1].SourceCallHint);
    MedOps[1].SourceCallHint = Binding;
    using Op = NativeVoidFrameFixture;
    const auto SP = NdVar::reg(a64reg::SP, 8);
    switch (Mutation) {
    case 0:
      F.Image.DyldBindSlots[F.ReturningImportSlot].Module = "/tmp/foreign.dylib";
      break;
    case 1:
      F.Image.DyldBindSlots.erase(F.ReturningImportSlot);
      break;
    case 2:
      Binding->TargetName += "_forged";
      break;
    case 3:
      Binding->CallKind = SourceCallTypeHint::Kind::Native;
      Binding->TargetAddress = 0x1090;
      Binding->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      break;
    case 4:
      ++Ops[2].Seq;
      break;
    case 5:
      // The unique terminating call must be last even when all occurrences
      // still match their independent runtime declarations.
      std::swap(Ops[2], Ops.back());
      std::swap(MedOps[1], MedOps.back());
      break;
    case 6:
      // Two authentic terminal declarations cannot bypass the prefix proof.
      MedOps[1].SourceCallHint = MedOps.back().SourceCallHint;
      MedOps[1].DoesNotReturn = true;
      MedOps[1].Inputs = MedOps.back().Inputs;
      MedOps[1].NumInputs = MedOps.back().NumInputs;
      Ops[2].Inputs[0] = Ops.back().Inputs[0];
      break;
    case 7:
      // Declared stack bytes must all be written after prefix effects too.
      Ops[3].Inputs[1].Size = 4;
      break;
    case 8:
      // The returning prefix cannot borrow an arbitrary private frame value.
      Ops.insert(Ops.begin() + 2,
                 Op::op(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {SP}, 0x1006));
      break;
    case 9:
      --MedOps[1].NumInputs;
      break;
    case 10:
      Binding->Signature.Parameters[0].Location.RegisterOffset = a64reg::X2;
      break;
    case 11:
      // A caller-clobbered copy cannot retain the entry identity across the
      // prefix call merely because the source graph still names X20.
      Ops.insert(Ops.begin() + 1,
                 Op::op(NdOp::COPY, NdVar::reg(a64reg::X9, 8),
                        {NdVar::reg(F.Context, 8)}, 0x1004));
      Ops[2].Inputs[0] = NdVar::cst(0x3000, 8);
      Ops.insert(Ops.begin() + 4,
                 Op::op(NdOp::STORE, {},
                        {NdVar::reg(a64reg::X9, 8), NdVar::cst(0, 8)}, 0x1008));
      break;
    }
    std::string Error;
    const auto Hint = F.inferContext(Error);
    if (Hint)
      EXPECT_TRUE(Hint->Parameters.empty()) << Error;
  }
}

TEST(NativeSourceHints, TerminalContextRequiresExactCallAndCompleteFrameProof) {
  for (unsigned Mutation = 0; Mutation < 27; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeTerminalContextFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    auto &Call = F.Med.Blocks[0].Ops.back();
    auto Binding = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Call.SourceCallHint = Binding;
    const auto SP = NdVar::reg(a64reg::SP, 8);
    using Op = NativeVoidFrameFixture;
    switch (Mutation) {
    case 0:
      F.Image.DyldBindSlots[F.ImportSlot].Module = "/tmp/foreign.dylib";
      break;
    case 1:
      F.Image.DyldBindSlots.clear();
      break;
    case 2:
      Binding->TargetName += "_forged";
      break;
    case 3:
      Binding->CallKind = SourceCallTypeHint::Kind::Native;
      Binding->TargetAddress = 0x1080;
      Binding->Signature.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      break;
    case 4:
      Call.DoesNotReturn = false;
      break;
    case 5:
      Binding->DoesNotReturn = false;
      break;
    case 6:
      ++Ops.back().Seq;
      break;
    case 7:
      Ops.back().Inputs[0].Offset += 4;
      break;
    case 8:
      Ops.back().Addr += 4;
      break;
    case 9:
      Ops.back().Opcode = NdOp::INDIR_CALL;
      break;
    case 10:
      Ops[1].Inputs[0].Size = 4;
      break;
    case 11:
    case 12:
      Ops.insert(Ops.begin() + 1,
                 Op::op(NdOp::COPY,
                        NdVar::reg(F.Context + (Mutation == 12),
                                   Mutation == 11 ? 8 : 1),
                        {NdVar::cst(0, Mutation == 11 ? 8 : 1)}, 0x1004));
      break;
    case 13:
      // A complete prologue spill is not a source-level context use.
      Ops[1] = Op::op(NdOp::STORE, {}, {SP, NdVar::reg(F.Context, 8)}, 0x1004);
      break;
    case 14:
      Ops[2].Inputs[1].Size = 4;
      break;
    case 15:
      Ops.erase(Ops.begin() + 4);
      break;
    case 16:
      Ops.insert(Ops.end() - 1,
                 Op::op(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {SP}, 0x100c));
      break;
    case 17:
      Ops[1].Inputs[1] = SP;
      break;
    case 18:
      Ops.front().Inputs[1] = NdVar::cst(8, 8);
      break;
    case 19:
      F.Low.Blocks[0].Succs = {0};
      break;
    case 20:
      F.Low.Blocks[0].ExceptionalSuccs.emplace_back();
      break;
    case 21:
      Ops.insert(Ops.begin() + 1,
                 Op::op(NdOp::BRANCH, {}, {NdVar::cst(0x1010, 8)}, 0x1004));
      break;
    case 22:
      Ops.push_back(Op::op(NdOp::RETURN, {},
                           {NdVar::reg(a64reg::X30, 8)}, 0x1014));
      break;
    case 23:
      F.Low.Blocks.emplace_back();
      F.Low.Blocks.back().Id = 1;
      break;
    case 24:
      --Call.NumInputs;
      break;
    case 25:
      Binding->Signature.Parameters.back().Location.EntryStackOffset += 4;
      break;
    case 26:
      F.Med.DoesNotReturn = F.High.DoesNotReturn = false;
      break;
    }
    std::string Error;
    const auto Hint = F.inferContext(Error);
    if (Hint)
      EXPECT_TRUE(Hint->Parameters.empty()) << Error;
  }
}

struct NativeContextFixture : NativeFixture {
  LowFunc Low;
  MedVar Context;

  explicit NativeContextFixture(Arch Architecture, uint64_t Register)
      : NativeFixture(Architecture) {
    Med.Params[1].Id = -1;
    Context.Kind = MedVar::Reg;
    Context.Id = 200;
    Context.RegOff = Register;
    Context.Size = 8;
    Context.TheArch = Architecture;
    MedOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = Context;
    Load.Output.Kind = MedVar::Temp;
    Load.Output.Id = 201;
    Load.Output.Size = 4;
    Load.addInput(Context);
    Med.Blocks[0].Ops[0].Inputs[1] = Load.Output;
    Med.Blocks[0].Ops.insert(Med.Blocks[0].Ops.begin(), Load);
    Low.Entry = Med.Entry;
    Low.Blocks.emplace_back();
    Low.Blocks[0].Id = 0;
    LowOp NativeLoad;
    NativeLoad.Opcode = NdOp::LOAD;
    NativeLoad.Output = NdVar::tmp(TmpBase, 4);
    NativeLoad.addInput(NdVar::reg(Register, 8));
    Low.Blocks[0].Ops.push_back(NativeLoad);
  }

  std::optional<SourceFunctionTypeHint> inferContext(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error, &Low);
  }
};

TEST(NativeSourceHints, BoundPreservedEntryRetainsItsPhysicalSeed) {
  NativeContextFixture Fixture(Arch::AArch64, a64reg::X20);
  MedOp Seed;
  Seed.Opcode = NdOp::COPY;
  Seed.Output = Fixture.Context;
  Seed.addInput(Fixture.Context);
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Seed);
  std::string Error;
  const auto Hint = Fixture.inferContext(Error);
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  MedVar Narrow = Fixture.Context;
  Narrow.Size = 4;
  MedOp Mask;
  Mask.Opcode = NdOp::INT_AND;
  Mask.Output.Kind = MedVar::Temp;
  Mask.Output.Id = 302;
  Mask.Output.Size = 4;
  Mask.addInput(Narrow);
  Mask.addInput(MedVar::makeConst(1, 4));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin() + 1, Mask);
  Fixture.Med.SourceTypeHint = *Hint;
  inferMedTypes(Fixture.Med, Arch::AArch64);
  ASSERT_TRUE(Fixture.Med.SourceTypeHint);
  ASSERT_EQ(Fixture.Med.Params.size(), 2U);
  EXPECT_GE(Fixture.Med.Params[1].Id, 0);
  Fixture.High = MedToHighConverter().convert(Fixture.Med, Arch::AArch64);
  EXPECT_TRUE(sdk::sourceBodyLimitation(
                  Fixture.High, *Fixture.High.SourceTypeHint, &Fixture.Audit,
                  [](const HighExpr &) { return true; })
                  .empty());
}

TEST(NativeSourceHints, DirectContextUseSurvivesUnrelatedIncompleteDemand) {
  NativeContextFixture Fixture(Arch::AArch64, a64reg::X20);
  MedVar Unrelated;
  Unrelated.Kind = MedVar::Reg;
  Unrelated.Id = 900;
  Unrelated.SSAVer = 1;
  Unrelated.RegOff = a64reg::X2;
  Unrelated.Size = 8;
  Unrelated.TheArch = Arch::AArch64;
  MedOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Unrelated);
  Store.addInput(MedVar::makeConst(0, 8));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Store);

  std::string Error;
  const auto Hint = Fixture.inferContext(Error);
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 2U);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, a64reg::X20);

  Fixture.Med.Blocks[0].Ops[1].Inputs[0].SSAVer = 1;
  const auto WithoutEntry = Fixture.inferContext(Error);
  ASSERT_TRUE(WithoutEntry) << Error;
  EXPECT_EQ(WithoutEntry->Parameters.size(), 1U);
}

TEST(NativeSourceHints, ReadOnlyContextsRetainObservedPreservedRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    unsigned Checked = 0;
    for (uint64_t Register : TRI.CalleeSaveRegs) {
      if (TRI.isFrameOrLinkReg(Register) || TRI.isVectorReg(Register))
        continue;
      NativeContextFixture Fixture(Architecture, Register);
      std::string Error;
      auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      ASSERT_EQ(Hint->Parameters.size(), 2U) << Register;
      EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
                TRI.IntParamRegs[0]);
      EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, Register);
      EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, 8U);
      EXPECT_EQ(Hint->Convention, SourceFunctionTypeHint::ConventionKind::C);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
      auto WithoutNativeProof = Fixture.infer(Error);
      ASSERT_TRUE(WithoutNativeProof) << Error;
      EXPECT_EQ(WithoutNativeProof->Parameters.size(), 1U);
      ++Checked;
    }
    EXPECT_GT(Checked, 0U);
  }
}

TEST(NativeSourceHints,
     PreservedContextsAllowASeparateArchitecturalTrapPath) {
  const auto Context = a64reg::X20;
  LowFunc Function;
  Function.Entry = 0x1000;
  Function.Blocks.resize(3);
  Function.Blocks[0].Id = 0;
  Function.Blocks[0].StartAddr = Function.Entry;
  Function.Blocks[0].Succs = {1, 2};
  Function.Blocks[1].Id = 1;
  Function.Blocks[1].Preds = {0};
  Function.Blocks[2].Id = 2;
  Function.Blocks[2].Preds = {0};

  LowOp Read;
  Read.Opcode = NdOp::LOAD;
  Read.Output = NdVar::tmp(TmpBase, 8);
  Read.addInput(NdVar::reg(Context, 8));
  Function.Blocks[0].Ops.push_back(Read);
  LowOp Reverse;
  Reverse.Opcode = NdOp::INTRINSIC;
  Reverse.Output = NdVar::tmp(TmpBase + 8, 8);
  Reverse.addInput(
      NdVar::cst(static_cast<uint64_t>(Intrinsic::A64_Rbit), 2));
  Reverse.addInput(Read.Output);
  Function.Blocks[0].Ops.push_back(Reverse);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(NdVar::reg(a64reg::X30, 8));
  Function.Blocks[1].Ops.push_back(Return);

  LowOp Clobber;
  Clobber.Opcode = NdOp::COPY;
  Clobber.Output = NdVar::reg(Context, 8);
  Clobber.addInput(NdVar::cst(0, 8));
  Function.Blocks[2].Ops.push_back(Clobber);
  LowOp Trap;
  Trap.Opcode = NdOp::INTRINSIC;
  Trap.Output = NdVar::reg(a64reg::X0, 8);
  Trap.addInput(NdVar::cst(static_cast<uint64_t>(Intrinsic::Brk), 2));
  Function.Blocks[2].Ops.push_back(Trap);

  std::set<uint64_t> Used;
  EXPECT_TRUE(restoresNativeSourceState(Function, Arch::AArch64, {}, &Used));
  EXPECT_EQ(Used, std::set<uint64_t>{Context});

  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    auto Invalid = Function;
    auto &InvalidTrap = Invalid.Blocks[2].Ops.back();
    if (Mutation == 0)
      InvalidTrap.Inputs[0].Offset = static_cast<uint64_t>(Intrinsic::Svc);
    else if (Mutation == 1)
      InvalidTrap.NumInputs = 0;
    else if (Mutation == 2)
      InvalidTrap.addInput(NdVar::cst(0, 2));
    else if (Mutation == 3)
      Invalid.Blocks[2].Succs = {1};
    else
      Invalid.Blocks[2].Ops.push_back(Clobber);
    EXPECT_FALSE(
        restoresNativeSourceState(Invalid, Arch::AArch64, {}, nullptr));
  }
  EXPECT_FALSE(restoresNativeSourceState(Function, Arch::X64, {}, nullptr));
}

TEST(NativeSourceHints, ScalarBitReverseHasExactNativeABIEvidence) {
  NativeFixture Fixture(Arch::AArch64);
  Fixture.Med.Params[0].Size = 8;
  Fixture.Med.TypedParams[0].Type = Fixture.High.Params[0].Type =
      NdType::makeInt(8, false);
  Fixture.Med.Blocks[0].Ops[0].Inputs[0] = Fixture.Med.Params[0];
  MedOp Reverse;
  Reverse.Opcode = NdOp::INTRINSIC;
  Reverse.Output.Kind = MedVar::Temp;
  Reverse.Output.Id = 300;
  Reverse.Output.Size = 8;
  Reverse.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_Rbit), 2));
  Reverse.addInput(Fixture.Med.Params[0]);
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Reverse);

  std::string Error;
  EXPECT_TRUE(Fixture.infer(Error)) << Error;
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    auto Invalid = Fixture;
    auto &Op = Invalid.Med.Blocks[0].Ops[0];
    if (Mutation == 0)
      Op.Inputs[0].ConstVal = static_cast<uint64_t>(Intrinsic::A64_Rev64);
    else if (Mutation == 1)
      Op.Inputs[0].Size = 4;
    else if (Mutation == 2)
      Op.Inputs[1].Size = 4;
    else if (Mutation == 3)
      Op.Output.Size = 16;
    else
      Op.addInput(MedVar::makeConst(0, 8));
    EXPECT_FALSE(Invalid.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, AuxiliaryInputsAllowLaterCallerSavedWrites) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Register =
        Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
    for (bool RecordedParameter : {false, true}) {
      NativeContextFixture Fixture(Architecture, Register);
      if (RecordedParameter) {
        Fixture.Context.Kind = MedVar::Param;
        Fixture.Med.Params[1] = Fixture.Context;
        Fixture.Med.TypedParams[1].Type = NdType::makeInt(8);
        Fixture.High.Params[1].Type = NdType::makeInt(8);
        Fixture.Med.Blocks[0].Ops[0].Inputs[0] = Fixture.Context;
      }
      LowOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = NdVar::reg(Register, 8);
      Write.addInput(NdVar::cst(7, 8));
      Fixture.Low.Blocks[0].Ops.push_back(Write);
      std::string Error;
      const auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      ASSERT_EQ(Hint->Parameters.size(), 2U);
      EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, Register);
      EXPECT_EQ(Hint->Parameters[1].Location.ValueBytes, 8U);
      EXPECT_FALSE(Hint->Parameters[1].Location.ExtendTo32Bits);
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
      EXPECT_EQ(bool(Fixture.infer(Error)), !RecordedParameter);
    }
  }
}

TEST(NativeSourceHints, AuxiliaryParametersRequireCompleteNativeReadEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Register =
        Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      NativeContextFixture Fixture(Architecture, Register);
      Fixture.Context.Kind = MedVar::Param;
      Fixture.Med.Params[1] = Fixture.Context;
      Fixture.Med.TypedParams[1].Type = NdType::makeInt(8);
      Fixture.High.Params[1].Type = NdType::makeInt(8);
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = Fixture.Context;
      if (Mutation == 0)
        Fixture.Low.Blocks[0].Ops[0].Inputs[0].Size = 4;
      else if (Mutation == 1)
        Fixture.Low.Blocks[0].Ops.clear();
      else if (Mutation == 2)
        ++Fixture.Low.Entry;
      else if (Mutation == 3)
        Fixture.Low.Blocks[0].Ops[0].NumInputs = 7;
      else {
        Fixture.Med.Params[1].Size = 4;
        Fixture.Med.TypedParams[1].Type = NdType::makeInt(4);
      }
      std::string Error;
      EXPECT_FALSE(Fixture.inferContext(Error)) << Mutation;
    }
  }
}

TEST(NativeSourceHints, AuxiliaryCallClobbersCannotBecomeEntryParameters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Register =
        Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
    NativeContextFixture Fixture(Architecture, Register);
    Fixture.Med.CallClobbers.push_back({Fixture.Context, 1});
    std::string Error;
    const auto Hint = Fixture.inferContext(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Parameters.size(), 1U);
  }
}

TEST(NativeSourceHints,
     ContextInputsSurviveProvenRestorationOfScratchRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    std::vector<uint64_t> Preserved;
    for (auto R : TRI.CalleeSaveRegs)
      if (!TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R))
        Preserved.push_back(R);
    ASSERT_GE(Preserved.size(), 2U);
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      SCOPED_TRACE(Mutation);
      NativeContextFixture F(Architecture, Preserved[0]);
      auto &Ops = F.Low.Blocks[0].Ops;
      LowOp Save;
      Save.Opcode = NdOp::COPY;
      Save.Output = NdVar::tmp(TmpBase + 16, 8);
      Save.addInput(NdVar::reg(Preserved[1], 8));
      LowOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = NdVar::reg(Preserved[1], 8);
      Write.addInput(NdVar::cst(19, 8));
      LowOp Restore;
      Restore.Opcode = NdOp::COPY;
      Restore.Output = Write.Output;
      Restore.addInput(Save.Output);
      LowOp Return;
      Return.Opcode = NdOp::RETURN;
      if (Architecture == Arch::AArch64)
        Return.addInput(NdVar::reg(TRI.LinkRegister, 8));
      Ops.insert(Ops.begin(), Save);
      Ops.push_back(Write);
      if (Mutation == 2)
        Restore.Output.Size = Restore.Inputs[0].Size = 4;
      if (Mutation != 1)
        Ops.push_back(Restore);
      if (Mutation == 3) {
        Write.Output = NdVar::reg(TRI.StackPointer, 8);
        Ops.push_back(Write);
      }
      Ops.push_back(Return);
      if (Mutation == 4)
        F.Low.Blocks[0].ExceptionalSuccs.emplace_back();
      if (Mutation == 5) {
        F.Low.Blocks[0].Succs = {99};
      }
      std::string Error;
      const auto Hint = F.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters.size(), Mutation == 0 ? 2U : 1U);
    }
  }
}

TEST(NativeSourceHints,
     RestoredContextRequiresANonPreservationEntryUse) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto Context = *std::find_if(
        TRI.CalleeSaveRegs.begin(), TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
          return !TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R);
        });
    NativeContextFixture Fixture(Architecture, Context);
    auto &Ops = Fixture.Low.Blocks[0].Ops;
    LowOp Save;
    Save.Opcode = NdOp::COPY;
    Save.Output = NdVar::tmp(TmpBase + 16, 8);
    Save.addInput(NdVar::reg(Context, 8));
    LowOp Write;
    Write.Opcode = NdOp::COPY;
    Write.Output = NdVar::reg(Context, 8);
    Write.addInput(NdVar::cst(19, 8));
    LowOp Restore;
    Restore.Opcode = NdOp::COPY;
    Restore.Output = Write.Output;
    Restore.addInput(Save.Output);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    if (Architecture == Arch::AArch64)
      Return.addInput(NdVar::reg(TRI.LinkRegister, 8));
    Ops.insert(Ops.begin(), Save);
    Ops.push_back(Write);
    Ops.push_back(Restore);
    Ops.push_back(Return);

    std::string Error;
    const auto Used = Fixture.inferContext(Error);
    ASSERT_TRUE(Used) << Error;
    ASSERT_EQ(Used->Parameters.size(), 2U);
    EXPECT_EQ(Used->Parameters[1].Location.RegisterOffset, Context);

    auto SpillOnly = Fixture;
    SpillOnly.Low.Blocks[0].Ops[1].Inputs[0] = NdVar::cst(0x2000, 8);
    const auto Omitted = SpillOnly.inferContext(Error);
    ASSERT_TRUE(Omitted) << Error;
    EXPECT_EQ(Omitted->Parameters.size(), 1U);
  }
}

TEST(NativeSourceHints, ContextProofRejectsClobbersAndIncompleteNativeReads) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto ContextRegister = *std::find_if(
        TRI.CalleeSaveRegs.begin(), TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
          return !TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R);
        });
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      NativeContextFixture Fixture(Architecture, ContextRegister);
      auto &Ops = Fixture.Low.Blocks[0].Ops;
      if (Mutation < 3) {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        const auto Offset =
            Mutation == 2 ? ContextRegister + 1 : ContextRegister;
        Write.Output = NdVar::reg(Offset, Mutation ? 1 : 8);
        Write.addInput(NdVar::cst(0, Write.Output.Size));
        Ops.push_back(Write);
      } else if (Mutation == 3) {
        ++Fixture.Low.Entry;
      } else if (Mutation == 4) {
        Ops[0].Inputs[0] = NdVar::cst(0, 8);
      } else if (Mutation == 5) {
        Ops[0].Inputs[0].Size = 4;
      } else if (Mutation == 6) {
        Fixture.Low.Blocks.resize(16385);
      } else if (Mutation == 7) {
        Ops[0].NumInputs = 7;
      } else {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        auto Other = std::find_if(TRI.CalleeSaveRegs.begin(),
                                  TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
                                    return R != ContextRegister &&
                                           !TRI.isFrameOrLinkReg(R) &&
                                           !TRI.isVectorReg(R);
                                  });
        ASSERT_NE(Other, TRI.CalleeSaveRegs.end());
        Write.Output = NdVar::reg(*Other, 8);
        Write.addInput(NdVar::cst(0, 8));
        Ops.push_back(Write);
      }
      std::string Error;
      auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters.size(), 1U) << Mutation;
    }
  }
}

TEST(NativeSourceHints, ContextDemandExcludesSeedsAndInternalDefinitions) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto Register = *std::find_if(
        TRI.CalleeSaveRegs.begin(), TRI.CalleeSaveRegs.end(), [&](uint64_t R) {
          return !TRI.isFrameOrLinkReg(R) && !TRI.isVectorReg(R);
        });
    for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
      NativeContextFixture Fixture(Architecture, Register);
      if (Mutation == 0) {
        auto &Load = Fixture.Med.Blocks[0].Ops[0];
        Load.Opcode = NdOp::COPY;
        Load.Output = Fixture.Context;
        Fixture.Med.Blocks[0].Ops[1].Inputs[1] = MedVar::makeConst(7, 4);
      } else if (Mutation == 1) {
        MedOp Define;
        Define.Opcode = NdOp::COPY;
        Define.Output = Fixture.Context;
        Define.addInput(MedVar::makeConst(0, 8));
        Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(),
                                         Define);
      } else {
        PhiNode Phi;
        Phi.Output = Fixture.Context;
        Phi.Args = {{0, MedVar::makeConst(0, 8)}};
        Fixture.Med.Blocks[0].Phis.push_back(Phi);
      }
      std::string Error;
      auto Hint = Fixture.inferContext(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters.size(), 1U) << Mutation;
    }
  }
}

TEST(NativeSourceHints, OmittedUnusedArgumentDoesNotShiftPhysicalRegister) {
  NativeFixture Fixture;
  Fixture.Med.Params[0].Id = -1;
  Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
  std::string Error;
  auto Hint = Fixture.infer(Error);
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).IntParamRegs[1]);
}

TEST(NativeSourceHints, ConstantFunctionNeedsNoInventedArguments) {
  NativeFixture Fixture;
  Fixture.Med.Params.clear();
  Fixture.Med.TypedParams.clear();
  Fixture.High.Params.clear();
  auto &Op = Fixture.Med.Blocks[0].Ops[0];
  Op.Opcode = NdOp::COPY;
  Op.NumInputs = 1;
  Op.Inputs[0] = MedVar::makeConst(42, 4);
  std::string Error;
  auto Hint = Fixture.infer(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_TRUE(Hint->Parameters.empty());
}

TEST(NativeSourceHints, RejectsIncompleteAuditAndMismatchedFunctionIdentity) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    NativeFixture Fixture;
    if (Mutation == 0)
      Fixture.Audit.TruncatedPaths.push_back(0x1004);
    if (Mutation == 1)
      --Fixture.Audit.LiftedInstructions;
    if (Mutation == 2)
      Fixture.Audit.MedIRVerified = false;
    if (Mutation == 3)
      Fixture.High.Entry += 4;
    if (Mutation == 4)
      Fixture.Image.IsRelocatable = true;
    if (Mutation == 5)
      Fixture.Image.Segments[0].Flags = SegmentFlags::Readable;
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
    EXPECT_FALSE(Error.empty());
  }
}

TEST(NativeSourceHints, RejectsRegisterBankTypeWidthAndDuplicateAmbiguity) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    NativeFixture Fixture;
    if (Mutation == 0)
      Fixture.Med.Params[0].RegOff =
          getTargetRegInfo(Arch::AArch64).FPParamRegs[0];
    if (Mutation == 1)
      Fixture.Med.TypedParams[0].Type = NdType::makeFloat(4);
    if (Mutation == 2)
      Fixture.Med.Params[0].Size = 8;
    if (Mutation == 3)
      Fixture.Med.Params[1].RegOff = Fixture.Med.Params[0].RegOff;
    if (Mutation == 4)
      Fixture.High.ReturnType = NdType::makeInt(8);
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, UnknownCallsCannotInventResultOrArgumentTypes) {
  NativeFixture Fixture;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(0x1020, 8));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
  std::string Error;
  EXPECT_FALSE(Fixture.infer(Error));
  EXPECT_NE(Error.find("without a source binding"), std::string::npos);
}

TEST(NativeSourceHints, EachReturnRequiresComputedCarrierNotStaleLiveIn) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    NativeFixture Fixture;
    auto &Op = Fixture.Med.Blocks[0].Ops[0];
    if (Mutation == 0) {
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] = Op.Output;
    }
    if (Mutation == 1)
      Op.Output.Size = 2;
    if (Mutation == 2) {
      MedBlock Other;
      Other.Id = 1;
      Other.Ops.push_back(Fixture.Med.Blocks[0].Ops.back());
      Fixture.Med.Blocks.push_back(Other);
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

// The source hint consumes already verified MedIR. These graphs isolate the
// all-path carrier proof; the runtime fixture exercises real lifting and SSA.
void returnDiamond(NativeFixture &Fixture) {
  const auto Compute = Fixture.Med.Blocks[0].Ops.front();
  const auto Return = Fixture.Med.Blocks[0].Ops.back();
  Fixture.Med.Blocks.resize(4);
  for (unsigned I = 0; I < 4; ++I) {
    auto &Block = Fixture.Med.Blocks[I];
    Block.Id = I;
    Block.StartAddr = Fixture.Med.Entry + I * 16;
    Block.Ops.clear();
  }
  auto &Blocks = Fixture.Med.Blocks;
  Blocks[0].Succs = {1, 2};
  for (unsigned I : {1U, 2U}) {
    Blocks[I].Preds = {0};
    Blocks[I].Succs = {3};
    Blocks[I].Ops = {Compute};
  }
  Blocks[3].Preds = {1, 2};
  Blocks[3].Ops = {Return};
}

TEST(NativeSourceHints, ComputedReturnsMergeAcrossEveryPathAndBlockOrder) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Complete : {false, true}) {
      NativeFixture Fixture(Architecture);
      // Isolate computed results from the separately proven entry carrier.
      Fixture.Med.Params[0].Id = -1;
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
      returnDiamond(Fixture);
      if (!Complete)
        Fixture.Med.Blocks[2].Ops.clear();
      const auto Blocks = Fixture.Med.Blocks;
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        for (unsigned I = 0; I < 4; ++I)
          Fixture.Med.Blocks[I] = Blocks[Order[I]];
        std::string Error;
        EXPECT_EQ(bool(Fixture.infer(Error)), Complete) << Error;
        EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                  MedReturnValueEvidence::Unknown);
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
  }
}

TEST(NativeSourceHints, ReturnLoopsRequireComputationOnTheirEntryPath) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Seeded : {false, true}) {
      NativeFixture Fixture(Architecture);
      Fixture.Med.Params[0].Id = -1;
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
      returnDiamond(Fixture);
      auto &Blocks = Fixture.Med.Blocks;
      // Entry -> header -> body -> header, or header -> return. Computing
      // only in the body cannot prove a result on the zero-trip path.
      if (Seeded)
        Blocks[0].Ops = Blocks[2].Ops;
      Blocks[0].Succs = {1};
      Blocks[1].Ops.clear();
      Blocks[1].Preds = {0, 2};
      Blocks[1].Succs = {2, 3};
      Blocks[2].Preds = {1};
      Blocks[2].Succs = {1};
      Blocks[3].Preds = {1};
      std::string Error;
      EXPECT_EQ(bool(Fixture.infer(Error)), Seeded) << Error;
      // A disconnected cycle cannot establish its own incoming fact either.
      Blocks[0].Succs.clear();
      Blocks[1].Preds = {2};
      EXPECT_FALSE(Fixture.infer(Error));
    }
  }
}

TEST(NativeSourceHints, ReturnPathsInvalidateCallsAndPartialCarrierWrites) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      NativeFixture Fixture(Architecture);
      returnDiamond(Fixture);
      auto &Ops = Fixture.Med.Blocks[2].Ops;
      MedOp Overwrite = Ops.front();
      Overwrite.Opcode = NdOp::COPY;
      Overwrite.NumInputs = 1;
      Overwrite.Inputs[0] = MedVar::makeConst(7, 4);
      if (Mutation == 0)
        Overwrite.Output.Size = 2;
      if (Mutation == 1) {
        ++Overwrite.Output.RegOff;
        Overwrite.Output.Size = 1;
      }
      if (Mutation == 2) {
        Overwrite.Opcode = NdOp::CALL;
        Overwrite.Output = {};
        Overwrite.Inputs[0] = MedVar::makeConst(0x1020, 8);
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->Signature.ReturnType = NdType::makeVoid();
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error))
            << Error;
        Overwrite.SourceCallHint = std::move(Hint);
      }
      if (Mutation == 3)
        Overwrite.Inputs[0] = Overwrite.Output; // SSA seed, not a write.
      if (Mutation == 4) {
        Overwrite.Opcode = NdOp::SUBBYTES; // Register view, not a write.
        Overwrite.Output.Size = 1;
        Overwrite.addInput(MedVar::makeConst(0, 4));
      }
      Ops.push_back(Overwrite);
      std::string Error;
      EXPECT_EQ(bool(Fixture.infer(Error)), Mutation >= 3) << Mutation << Error;
      // A subsequent complete computation reestablishes the carrier.
      Ops.push_back(Ops.front());
      EXPECT_TRUE(Fixture.infer(Error)) << Mutation << Error;
    }
  }
}

TEST(NativeSourceHints, ReturnPathsRejectUnobservedInputAndEpilogueRestores) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    returnDiamond(Fixture);
    for (unsigned I : {1U, 2U}) {
      auto &Op = Fixture.Med.Blocks[I].Ops.front();
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] = Op.Output;
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error));
  }
  NativeFixture Fixture(Arch::X64);
  returnDiamond(Fixture);
  auto &Restore = Fixture.Med.Blocks[2].Ops.front();
  Restore.Opcode = NdOp::LOAD;
  Restore.NumInputs = 1;
  Restore.Inputs[0] = Restore.Output;
  Restore.Inputs[0].RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  std::string Error;
  EXPECT_FALSE(Fixture.infer(Error));
}

TEST(NativeSourceHints, ObservedIncomingResultsKeepExactWidthsAndLocations) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (uint16_t Width : {1, 2, 4, 8}) {
      NativeFixture Fixture(Architecture);
      Fixture.Med.ReturnType = Fixture.High.ReturnType = NdType::makeInt(Width);
      for (unsigned I = 0; I < 2; ++I) {
        Fixture.Med.Params[I].Size = Width;
        Fixture.Med.TypedParams[I].Type = Fixture.High.Params[I].Type =
            NdType::makeInt(Width);
        Fixture.Med.Blocks[0].Ops[0].Inputs[I] = Fixture.Med.Params[I];
      }
      Fixture.Med.Blocks[0].Ops[0].Output.Size = Width;
      returnDiamond(Fixture);
      Fixture.Med.Blocks[2].Ops.clear();
      const auto Blocks = Fixture.Med.Blocks;
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        for (unsigned I = 0; I < 4; ++I)
          Fixture.Med.Blocks[I] = Blocks[Order[I]];
        std::string Error;
        const auto Hint = Fixture.infer(Error);
        const auto &TRI = getTargetRegInfo(Architecture);
        EXPECT_EQ(bool(Hint), TRI.IntParamRegs[0] == TRI.IntReturnReg) << Error;
        if (Hint) {
          EXPECT_EQ(Hint->ReturnLocation.ValueBytes, Width);
          EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
                    TRI.IntReturnReg);
          EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, Width);
        }
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
}

TEST(NativeSourceHints, IncomingResultsRejectUnobservedNarrowAndLaterVersions) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    NativeFixture Fixture;
    returnDiamond(Fixture);
    Fixture.Med.Blocks[2].Ops.clear();
    auto &Use = Fixture.Med.Blocks[1].Ops[0].Inputs[0];
    if (Mutation == 0)
      Use = MedVar::makeConst(3, 4);
    if (Mutation == 1)
      Use.Size = 2;
    if (Mutation == 2) {
      Use.Kind = MedVar::Reg;
      Use.SSAVer = 1;
    }
    if (Mutation == 3) {
      Fixture.Med.Params[0].Size = 2;
      Fixture.Med.TypedParams[0].Type = Fixture.High.Params[0].Type =
          NdType::makeInt(2);
      Use = Fixture.Med.Params[0];
    }
    if (Mutation == 4) {
      // A matching parameter listed only in an unreachable component does
      // not make the actual entry register an observed input.
      Fixture.Med.Blocks[0].Succs = {2};
      Fixture.Med.Blocks[1].Preds.clear();
    }
    if (Mutation == 5 || Mutation == 6) {
      Use.Kind = MedVar::Reg;
      Use.Id = 60;
      Use.SSAVer = 0;
      // An ordinary definition or a PHI may own the first SSA version.
      // Neither is evidence that the parameter's incoming bytes were used.
      if (Mutation == 5) {
        MedOp Internal;
        Internal.Opcode = NdOp::COPY;
        Internal.Output = Use;
        Internal.addInput(MedVar::makeConst(42, 4));
        auto &Ops = Fixture.Med.Blocks[1].Ops;
        Ops.insert(Ops.begin(), Internal);
      } else {
        PhiNode Phi;
        Phi.Output = Use;
        Phi.Args = {{0, MedVar::makeConst(42, 4)}};
        Fixture.Med.Blocks[1].Phis.push_back(std::move(Phi));
      }
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, IncomingResultFactsInvalidateNarrowedSelfCopies) {
  for (uint16_t WriteWidth : {2, 4}) {
    NativeFixture Fixture;
    returnDiamond(Fixture);
    auto &Ops = Fixture.Med.Blocks[2].Ops;
    MedOp Copy = Ops.front();
    Copy.Opcode = NdOp::COPY;
    Copy.NumInputs = 1;
    Copy.Inputs[0] = Copy.Output;
    Copy.Output.Size = WriteWidth;
    Ops = {Copy};
    std::string Error;
    EXPECT_EQ(bool(Fixture.infer(Error)), WriteWidth == 4) << Error;
  }
}

TEST(NativeSourceHints, IncomingResultBackedgesMeetInitialAndClobberedStates) {
  for (bool Clobber : {false, true}) {
    NativeFixture Fixture;
    const auto Return = Fixture.Med.Blocks[0].Ops.back();
    MedOp Observe;
    Observe.Opcode = NdOp::COPY;
    Observe.Output.Kind = MedVar::Temp;
    Observe.Output.Id = 50;
    Observe.Output.Size = 4;
    Observe.addInput(Fixture.Med.Params[0]);
    Fixture.Med.Blocks.resize(3);
    for (unsigned I = 0; I < 3; ++I) {
      Fixture.Med.Blocks[I].Id = I;
      Fixture.Med.Blocks[I].StartAddr = Fixture.Med.Entry + I * 16;
    }
    auto &Blocks = Fixture.Med.Blocks;
    Blocks[0].Ops = {Observe};
    Blocks[0].Preds = {1};
    Blocks[0].Succs = {1, 2};
    Blocks[1].Preds = {0};
    Blocks[1].Succs = {0};
    Blocks[2].Preds = {0};
    Blocks[2].Ops = {Return};
    if (Clobber) {
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(MedVar::makeConst(0x1020, 8));
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->Signature.ReturnType = NdType::makeVoid();
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Hint->Signature, Arch::AArch64, Error));
      Call.SourceCallHint = std::move(Hint);
      Blocks[1].Ops = {Call};
    }
    std::string Error;
    EXPECT_EQ(bool(Fixture.infer(Error)), !Clobber) << Error;
  }
}

TEST(NativeSourceHints, ReturnProofRejectsMalformedOrUnboundedControlFlow) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      NativeFixture Fixture(Architecture);
      returnDiamond(Fixture);
      auto &Blocks = Fixture.Med.Blocks;
      if (Mutation == 0)
        Blocks[0].Succs.push_back(99);
      if (Mutation == 1)
        Blocks[0].Succs.push_back(1);
      if (Mutation == 2)
        Blocks[1].Preds.push_back(0);
      if (Mutation == 3)
        Blocks[1].Preds.clear();
      if (Mutation == 4)
        Blocks[1].Id = Blocks[0].Id;
      if (Mutation == 5)
        Blocks[0].Id = -1;
      if (Mutation == 6)
        Blocks[1].StartAddr = Fixture.Med.Entry;
      if (Mutation == 7)
        Blocks[0].StartAddr += 4;
      if (Mutation == 8)
        Blocks.resize(16385);
      if (Mutation == 9)
        Blocks[1].Ops.resize(262144, Blocks[1].Ops.front());
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
      EXPECT_FALSE(Error.empty()) << Mutation;
    }
  }
}

TEST(NativeSourceHints, KeepsCompleteStackSlotAndRejectsPackedSlices) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    auto &Parameter = Fixture.Med.Params[1];
    Parameter.RegOff = kNoParamReg;
    Parameter.Id = getTargetRegInfo(Architecture).IntParamRegs.size() + 2;
    Parameter.Size = 8;
    Fixture.Med.TypedParams[1].Type = NdType::makeInt(8);
    Fixture.Med.Blocks[0].Ops[0].Inputs[1] = Parameter;
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Parameters[1].Location.EntryStackOffset,
              Architecture == Arch::X64 ? 24 : 16);
    auto &Op = Fixture.Med.Blocks[0].Ops[0];
    Op.Opcode = NdOp::SUBBYTES;
    Op.Inputs[0] = Parameter;
    Op.Inputs[1] = MedVar::makeConst(4, 4);
    EXPECT_FALSE(Fixture.infer(Error));
    EXPECT_NE(Error.find("partial"), std::string::npos);
  }
}

TEST(NativeSourceHints,
     GenericScalarAllocatorDoesNotAddObjectiveCHiddenValues) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint.ReturnType = NdType::makeFloat(8);
    Hint.Parameters = {{"integer", NdType::makeInt(4)},
                       {"floating", NdType::makeFloat(8)}};
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error))
        << Error;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint.Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
    EXPECT_EQ(Hint.Parameters[1].Location.RegisterOffset, TRI.FPParamRegs[0]);
    EXPECT_FALSE(assignDarwinObjCSourceABI(Hint, Architecture, Error));
  }
}

TEST(NativeSourceHints, InferenceSurvivesRealLowMedHighScalarPipeline) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    LowFunc Low;
    Low.Entry = Fixture.Med.Entry;
    Low.Name = "scalar_helper";
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Low.Entry;
    Block.EndAddr = Low.Entry + 8;
    LowOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Addr = Low.Entry;
    Add.Output = NdVar::reg(TRI.IntReturnReg, 4);
    Add.addInput(NdVar::reg(TRI.IntParamRegs[0], 4));
    Add.addInput(NdVar::reg(TRI.IntParamRegs[1], 4));
    Block.Ops.push_back(Add);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Low.Entry + 4;
    Return.addInput(NdVar::reg(TRI.IntReturnReg, 4));
    Block.Ops.push_back(Return);
    Low.Blocks.push_back(Block);
    Fixture.Med =
        LowToMedConverter().convert(Low, Architecture, BinaryFormat::MachO);
    inferMedTypes(Fixture.Med, Architecture);
    Fixture.High = MedToHighConverter().convert(Fixture.Med, Architecture);
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    Fixture.Med.SourceTypeHint = *Hint;
    inferMedTypes(Fixture.Med, Architecture);
    ASSERT_TRUE(Fixture.Med.SourceTypeHint);
    ASSERT_TRUE(Fixture.Med.SourceParametersBound);
    Fixture.High = MedToHighConverter().convert(Fixture.Med, Architecture);
    ASSERT_EQ(Fixture.High.Params.size(), 2U);
    EXPECT_EQ(Fixture.High.Params[0].Name, "native_arg0");
    EXPECT_EQ(Fixture.High.Params[1].Name, "native_arg1");
  }
}

void addPointerCall(NativeFixture &Fixture) {
  for (size_t Index = 0; Index < Fixture.Med.Params.size(); ++Index) {
    Fixture.Med.Params[Index].Size = 8;
    Fixture.Med.TypedParams[Index].Type = NdType::makeInt(8);
    Fixture.High.Params[Index].Type = NdType::makeInt(8);
    Fixture.Med.Blocks[0].Ops[0].Inputs[Index] = MedVar::makeConst(21, 4);
  }
  auto CallHint = std::make_shared<SourceCallTypeHint>();
  CallHint->Signature.Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  CallHint->Signature.ReturnType = NdType::makeVoid();
  CallHint->Signature.Parameters = {
      {"first", NdType::makePtr(NdType::makeVoid())},
      {"second", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(CallHint->Signature,
                                          Fixture.Image.Arch, Error))
      << Error;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint = std::move(CallHint);
  Call.addInput(MedVar::makeConst(0x1020, 8));
  Call.addInput(Fixture.Med.Params[0]);
  Call.addInput(Fixture.Med.Params[1]);
  // Generic MedIR keeps the incoming SSA registers until source parameters
  // are bound in the second pipeline run. Their IDs need not match Param IDs.
  for (unsigned Index = 1; Index <= 2; ++Index) {
    Call.Inputs[Index].Kind = MedVar::Reg;
    Call.Inputs[Index].Id += 100;
  }
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
}

TEST(NativeSourceHints,
     PointerUsesFollowWholeValuesWithoutChangingMachineTypes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Shape = 0; Shape < 4; ++Shape) {
      NativeFixture Fixture(Architecture);
      addPointerCall(Fixture);
      if (Shape == 3) {
        auto &Ops = Fixture.Med.Blocks[0].Ops;
        MedOp SelfCopy;
        SelfCopy.Opcode = NdOp::COPY;
        SelfCopy.Output = Ops[0].Inputs[1];
        SelfCopy.addInput(SelfCopy.Output);
        Ops.insert(Ops.begin(), SelfCopy);
      } else if (Shape) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 30;
        Copy.Output.SSAVer = 1;
        Copy.Output.Size = 8;
        Copy.addInput(Fixture.Med.Params[0]);
        auto &Entry = Fixture.Med.Blocks[0];
        Entry.Ops[0].Inputs[1] = Copy.Output;
        if (Shape == 1) {
          Entry.Ops.insert(Entry.Ops.begin(), Copy);
        } else {
          // A loop PHI retains the exact incoming value through its backedge.
          MedBlock Exit = std::move(Entry);
          Exit.Id = 1;
          Exit.Preds = {2};
          MedBlock Loop;
          Loop.Id = 2;
          Loop.Preds = {0, 2};
          Loop.Succs = {2, 1};
          PhiNode Phi;
          Phi.Output = Copy.Output;
          ++Phi.Output.Id;
          Phi.Args = {{0, Copy.Output}, {2, Phi.Output}};
          Loop.Phis.push_back(Phi);
          Exit.Ops[0].Inputs[1] = Phi.Output;
          Entry = {};
          Entry.Id = 0;
          Entry.Succs = {2};
          Entry.Ops.push_back(Copy);
          // Deliberately keep the consumer before the producer in block order.
          Fixture.Med.Blocks.push_back(std::move(Exit));
          Fixture.Med.Blocks.push_back(std::move(Loop));
        }
      }
      std::string Error;
      auto Hint = Fixture.infer(Error);
      ASSERT_TRUE(Hint) << Error;
      ASSERT_EQ(Hint->Parameters.size(), 2U);
      for (size_t Index = 0; Index < 2; ++Index) {
        EXPECT_EQ(Hint->Parameters[Index].Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Hint->Parameters[Index].Location.ValueBytes, 8U);
        EXPECT_EQ(Hint->Parameters[Index].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[Index]);
        EXPECT_EQ(Fixture.Med.TypedParams[Index].Type->Kind, NdTypeKind::Int);
        EXPECT_EQ(Fixture.High.Params[Index].Type->Kind, NdTypeKind::Int);
      }
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
    }
  }
}

TEST(NativeSourceHints, ConflictingOrPartialUsesCannotInventPointerParameters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      NativeFixture Fixture(Architecture);
      addPointerCall(Fixture);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      if (Mutation == 0) {
        Ops[1].Inputs[0] = Fixture.Med.Params[0];
      } else if (Mutation == 1) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 30;
        Copy.Output.Size = 4;
        Copy.addInput(Fixture.Med.Params[0]);
        Ops.insert(Ops.begin(), Copy);
      } else if (Mutation == 2) {
        auto Hint =
            std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
        Hint->Signature.Parameters[0].Type = NdType::makeInt(8);
        auto ScalarCall = Ops[0];
        ScalarCall.SourceCallHint = std::move(Hint);
        Ops.insert(Ops.begin(), ScalarCall);
      } else if (Mutation == 3) {
        // A later register version is not the incoming parameter.
        Ops[0].Inputs[1].Kind = MedVar::Reg;
        ++Ops[0].Inputs[1].SSAVer;
      } else {
        Ops[0].Inputs[1].Size = 4;
      }
      std::string Error;
      auto Hint = Fixture.infer(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Int) << Mutation;
      EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Ptr) << Mutation;
    }
  }
}

TEST(NativeSourceHints, AmbiguousValuesAndUnboundCallsDoNotSupplyPointerFacts) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    NativeFixture Fixture;
    addPointerCall(Fixture);
    auto &Ops = Fixture.Med.Blocks[0].Ops;
    if (Mutation == 0) {
      Ops[0].SourceCallHint.reset();
    } else if (Mutation == 1) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
      Hint->Signature.Parameters[0].Location.ValueBytes = 4;
      Ops[0].SourceCallHint = std::move(Hint);
    } else if (Mutation == 2) {
      const auto Duplicate = Ops[1];
      Ops.insert(Ops.begin(), Duplicate);
    } else if (Mutation == 3) {
      Ops[0].Inputs[1] = Fixture.Med.Params[0];
      Ops[0].Inputs[1].RegOff = Fixture.Med.Params[1].RegOff;
    } else {
      PhiNode Phi;
      Phi.Output = Ops[0].Inputs[1];
      Phi.Args = {{0, MedVar::makeConst(7, 8)}};
      Fixture.Med.Blocks[0].Phis.push_back(Phi);
    }
    std::string Error;
    auto Hint = Fixture.infer(Error);
    if (Mutation < 2) {
      EXPECT_FALSE(Hint);
    } else {
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Int);
    }
  }
}
} // namespace
