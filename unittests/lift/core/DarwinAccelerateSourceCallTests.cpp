#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"

using namespace neverd;

namespace {
constexpr va_t ImportSlot = 0x2000;
constexpr const char *ImportName = "_vImageBoxConvolve_ARGB8888";
constexpr const char *Accelerate =
    "/System/Library/Frameworks/Accelerate.framework/Accelerate";

BinaryImage image(Arch Architecture, const char *Name = ImportName) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.Arch = Architecture;
  Image.ImportPtrSlots[ImportSlot] = Name;
  Image.DyldBindSlots[ImportSlot] = {Name, 0, Accelerate, false};
  return Image;
}
} // namespace

TEST(DarwinAccelerateSourceCalls, BufferCreationPreservesFixedScalarABI) {
  struct Expected {
    const char *Name;
    unsigned ParameterCount;
    NdTypeKind ReturnKind;
  };
  for (const auto &Case : {
           Expected{"_vImageBuffer_Init", 5, NdTypeKind::Int},
           Expected{"_vImageBuffer_InitWithCGImage", 5, NdTypeKind::Int},
           Expected{"_vImageCreateCGImageFromBuffer", 6, NdTypeKind::Ptr},
       }) {
    for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(Case.Name);
      const auto Image = image(Architecture, Case.Name);
      const auto Hint = darwinRuntimeSourceCallHint(Image, ImportSlot);
      ASSERT_TRUE(Hint);
      ASSERT_TRUE(Hint->Signature.ReturnType);
      EXPECT_EQ(Hint->Signature.ReturnType->Kind, Case.ReturnKind);
      ASSERT_EQ(Hint->Signature.Parameters.size(), Case.ParameterCount);
      EXPECT_EQ(Hint->Signature.Parameters.front().Type->Kind, NdTypeKind::Ptr);
      std::string Error;
      EXPECT_TRUE(validateSourceABI(Hint->Signature, Error)) << Error;
    }
  }
}

TEST(DarwinAccelerateSourceCalls, BoxConvolvePreservesFixedScalarABI) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Image = image(Architecture);
    const auto Hint = darwinRuntimeSourceCallHint(Image, ImportSlot);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
    EXPECT_EQ(Hint->TargetName, "vImageBoxConvolve_ARGB8888");
    EXPECT_EQ(Hint->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::DarwinSDK);
    ASSERT_TRUE(Hint->Signature.ReturnType);
    EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Int);
    EXPECT_EQ(Hint->Signature.ReturnType->Size, 8U);
    EXPECT_TRUE(Hint->Signature.ReturnType->IsSigned);
    const auto &Parameters = Hint->Signature.Parameters;
    ASSERT_EQ(Parameters.size(), 9U);
    for (const size_t I : {0U, 1U, 2U, 7U}) {
      EXPECT_EQ(Parameters[I].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Parameters[I].Type->Size, 8U);
    }
    for (const size_t I : {3U, 4U}) {
      EXPECT_EQ(Parameters[I].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Parameters[I].Type->Size, 8U);
      EXPECT_FALSE(Parameters[I].Type->IsSigned);
    }
    for (const size_t I : {5U, 6U, 8U}) {
      EXPECT_EQ(Parameters[I].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Parameters[I].Type->Size, 4U);
      EXPECT_FALSE(Parameters[I].Type->IsSigned);
    }
    EXPECT_EQ(Parameters[8].Location.Kind, SourceABICarrierKind::Stack);
    std::string Error;
    EXPECT_TRUE(validateSourceABI(Hint->Signature, Error)) << Error;
  }
}

TEST(DarwinAccelerateSourceCalls, BoxConvolveRequiresExactImportProvider) {
  auto Image = image(Arch::AArch64);
  Image.DyldBindSlots[ImportSlot].Module =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, ImportSlot));
  Image = image(Arch::AArch64);
  Image.DyldBindSlots[ImportSlot].Name = "_other";
  EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, ImportSlot));
  Image = image(Arch::AArch64);
  Image.DyldBindSlots[ImportSlot].Addend = 1;
  EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, ImportSlot));
}
