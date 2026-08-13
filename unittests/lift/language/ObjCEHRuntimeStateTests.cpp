//===- ObjCEHRuntimeStateTests.cpp - Objective-C image-wide state tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "ObjCEHTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::objc_eh_test;

//===----------------------------------------------------------------------===//
// Image-wide state
//===----------------------------------------------------------------------===//

TEST(ObjCRuntimeState, PrefersThePersonalityToTheImagesShape) {
  BinaryImage Img =
      makeObjCImage("__gnu_objc_personality_v0", {kStringVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeNameString(Img, "MyException");
  decode(Img);

  ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value());
  EXPECT_EQ(Img.ExceptionMetadata.ObjCRuntime->Runtime, ObjCRuntimeKind::GNU);
  EXPECT_TRUE(
      Img.ExceptionMetadata.ObjCRuntime->RuntimeProvenByPersonality);
}

TEST(ObjCRuntimeState, RecognizesARC) {
  // Every marker here is an entry point only a compiler emits.  The
  // return-value handshake is the one an optimized Apple build actually
  // reaches most often, and it is spelled `objc_retainAutoreleasedReturnValue`
  // on the caller side -- one letter away from two other real symbols.
  for (const char *Marker :
       {"_objc_storeStrong", "_objc_retainAutoreleasedReturnValue",
        "_objc_autoreleaseReturnValue", "_objc_destroyWeak"}) {
    BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
    addSymbol(Img, Marker, kRuntimeVA);
    decode(Img);
    ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value()) << Marker;
    EXPECT_TRUE(Img.ExceptionMetadata.ObjCRuntime->UsesARC) << Marker;
  }
}

TEST(ObjCRuntimeState, DoesNotTakeAPlainReleaseForARC) {
  // `objc_release` and `objc_retain` are reached by hand-written code and by
  // the runtime itself, so neither says which memory model the image uses.
  for (const char *Marker : {"_objc_release", "_objc_retain"}) {
    BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
    addSymbol(Img, Marker, kRuntimeVA);
    decode(Img);
    ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value()) << Marker;
    EXPECT_FALSE(Img.ExceptionMetadata.ObjCRuntime->UsesARC) << Marker;
  }
}

TEST(ObjCRuntimeState, IgnoresAnImageWithoutTheObjectiveCRuntime) {
  BinaryImage Img = makeImage();
  addSymbol(Img, "__gxx_personality_v0", kTextVA + 0x900);
  objc_eh::parseObjCExceptions(Img);
  EXPECT_FALSE(Img.ExceptionMetadata.ObjCRuntime.has_value());
}

TEST(ObjCRuntimeState, LeavesACxxFrameAlone) {
  // A C++ frame in an image that also contains Objective-C is still a C++
  // frame: its personality said so, and nothing it calls says otherwise.
  BinaryImage Img =
      makeObjCImage("__gxx_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, "St13runtime_error");

  const ExceptionFunction &F = decode(Img);
  EXPECT_FALSE(F.ObjC.has_value());
}

//===----------------------------------------------------------------------===//
// setjmp/longjmp call-site tables
//===----------------------------------------------------------------------===//

TEST(ObjCSJLJ, RefusesToReadIndexFormCallSitesAsAddresses) {
  // Under the SJLJ form a call-site record is a pair of ULEB128 values -- a
  // dispatch selector and an action -- selected by counting rather than by
  // address.  A reader that applies the address form does not fail; it invents
  // guarded ranges and landing pads the program never named.  Every SJLJ
  // personality has to be recognized for that not to happen, not just C++'s.
  //
  // What makes this a test rather than a tautology is that the bytes below are
  // an address-form table.  Read as the personality says they must be, they do
  // yield entries -- and not one of those entries may carry an address.
  for (const char *Personality :
       {"__gxx_personality_sj0", "__gcc_personality_sj0",
        "__gnu_objc_personality_sj0"}) {
    BinaryImage Img = makeObjCImage(
        Personality, {}, {SiteSpec{0x00, 0x01, 0x02, /*Action=*/0}});
    Img.ExceptionMetadata.Runtime = detectLanguageRuntime(Img);
    dwarf_eh::parseItaniumExceptions(Img);

    ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u) << Personality;
    const ExceptionFunction &F = Img.ExceptionMetadata.Functions.front();
    ASSERT_TRUE(F.Itanium.has_value()) << Personality;
    EXPECT_FALSE(F.Itanium->IsCallSiteAddressForm) << Personality;
    for (const ItaniumCallSite &Site : F.Itanium->CallSites) {
      EXPECT_FALSE(Site.GuardedRange.isValid()) << Personality;
      EXPECT_EQ(Site.LandingPadVA, 0u) << Personality;
      // An entry that named nothing at all would be indistinguishable from a
      // decoder that skipped the table, which is what this used to do.
      EXPECT_NE(Site.CallSiteIndex, 0u) << Personality;
    }
    // Address-form columns do not divide into ULEB128 pairs, so a byte is left
    // over and the record is not fully accounted for.  That is the correct
    // reading of a file whose table and whose personality disagree.
    EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Partial) << Personality;
  }
}

TEST(ObjCSJLJ, StillReadsTheAddressFormForANonSJLJPersonality) {
  BinaryImage Img =
      makeObjCImage("__gnu_objc_personality_v0", {},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.Itanium.has_value());
  EXPECT_TRUE(F.Itanium->IsCallSiteAddressForm);
  ASSERT_EQ(F.Itanium->CallSites.size(), 1u);
  EXPECT_EQ(F.Itanium->CallSites[0].LandingPadVA, kFuncVA + 0x40);
}

} // namespace
