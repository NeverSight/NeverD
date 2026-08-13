//===- ObjCEHTests.cpp - Apple Objective-C runtime EH tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "ObjCEHTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::objc_eh_test;

TEST(ObjCPersonality, NamesEveryRuntimesRoutine) {
  struct Case {
    const char *Symbol;
    ExceptionPersonality Personality;
    ObjCRuntimeKind Runtime;
  } const Cases[] = {
      {"__objc_personality_v0", ExceptionPersonality::ObjCPersonalityV0,
       ObjCRuntimeKind::AppleNonFragile},
      {"__gnu_objc_personality_v0",
       ExceptionPersonality::GnuObjCPersonalityV0, ObjCRuntimeKind::GNU},
      {"__gnu_objc_personality_seh0",
       ExceptionPersonality::GnuObjCPersonalitySEH0, ObjCRuntimeKind::GNU},
      {"__gnu_objc_personality_sj0",
       ExceptionPersonality::GnuObjCPersonalitySJ0, ObjCRuntimeKind::GNU},
      {"__gnustep_objc_personality_v0",
       ExceptionPersonality::GNUstepObjCPersonalityV0, ObjCRuntimeKind::GNU},
      {"__gnustep_objcxx_personality_v0",
       ExceptionPersonality::GNUstepObjCXXPersonalityV0,
       ObjCRuntimeKind::GNUstepObjCXX},
  };
  for (const Case &C : Cases) {
    EXPECT_EQ(classifyPersonalityName(C.Symbol), C.Personality) << C.Symbol;
    EXPECT_EQ(getPersonalityRuntime(C.Personality),
              SourceLanguageRuntime::ObjectiveC)
        << C.Symbol;
    ASSERT_TRUE(getObjCRuntimeForPersonality(C.Personality).has_value())
        << C.Symbol;
    EXPECT_EQ(*getObjCRuntimeForPersonality(C.Personality), C.Runtime)
        << C.Symbol;
    EXPECT_TRUE(isItaniumPersonality(C.Personality)) << C.Symbol;
    EXPECT_STREQ(getExceptionPersonalityName(C.Personality), C.Symbol);
  }
}

TEST(ObjCPersonality, DetectsAGnuRuntimeImageWithNoAppleSections) {
  BinaryImage Img = makeImage();
  addSymbol(Img, "__gnu_objc_personality_v0", kTextVA + 0x900);
  const LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_TRUE(Info.is(SourceLanguageRuntime::ObjectiveC));
}

TEST(ObjCPersonality, DetectsAGnuRuntimeImageFromItsMessageSend) {
  BinaryImage Img = makeImage();
  addSymbol(Img, "objc_msg_lookup", kTextVA + 0x900);
  const LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_TRUE(Info.is(SourceLanguageRuntime::ObjectiveC));
}

//===----------------------------------------------------------------------===//
// Apple's non-fragile runtime
//===----------------------------------------------------------------------===//

TEST(ObjCAppleEH, ReadsAClassClauseThroughItsDescriptor) {
  BinaryImage Img =
      makeObjCImage("__objc_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, "NSException");

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_EQ(F.ObjC->Runtime, ObjCRuntimeKind::AppleNonFragile);
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);

  const ObjCLandingPad &Pad = F.ObjC->LandingPads.front();
  EXPECT_EQ(Pad.Kind, ObjCPadKind::Catch);
  EXPECT_EQ(Pad.PadVA, kFuncVA + 0x40);
  ASSERT_EQ(Pad.Catches.size(), 1u);

  const ObjCCatchClause &Clause = Pad.Catches.front();
  EXPECT_EQ(Clause.Kind, ObjCCatchKind::Class);
  EXPECT_EQ(Clause.ClassName, "NSException");
  EXPECT_EQ(Clause.TypeInfoVA, kDescriptorVA);
  // The third field is Objective-C's own, and reading it is what turns a name
  // into the class object the image carries.
  EXPECT_EQ(Clause.ClassVA, kClassVA);
  EXPECT_FALSE(Clause.IsCxxType);
}

TEST(ObjCAppleEH, TellsCatchIdApartFromCatchAll) {
  // `@catch(id)` names `OBJC_EHTYPE_id`, a real symbol, and `@catch(...)` a
  // null slot.  The two are not the same handler: an `id` clause takes any
  // Objective-C object and lets a foreign exception continue past it.
  BinaryImage Img = makeObjCImage(
      "__objc_personality_v0", {kDescriptorVA, 0},
      {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1},
       SiteSpec{0x20, 0x10, 0x50, /*Action=*/3}});
  // A descriptor imported from libobjc holds no name in this image; the symbol
  // that names it is all there is, which is exactly the real case.
  writeAppleDescriptor(Img, /*ClassName=*/nullptr);
  addSymbol(Img, "_OBJC_EHTYPE_id", kDescriptorVA, /*IsFunc=*/false);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 2u);

  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[0].Catches[0].Kind, ObjCCatchKind::AnyObject);
  EXPECT_TRUE(F.ObjC->LandingPads[0].Catches[0].ClassName.empty());

  ASSERT_EQ(F.ObjC->LandingPads[1].Catches.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[1].Catches[0].Kind, ObjCCatchKind::CatchAll);
}

TEST(ObjCAppleEH, KeepsAnImportedCxxTypeApartByItsSymbol) {
  // One table holds both, because Apple's descriptor is `std::type_info`
  // shaped precisely so that it can.  A `catch (std::runtime_error &)` there
  // is not a clause any Objective-C object can satisfy.  The standard
  // exceptions come from libc++abi, so the descriptor holds nothing in this
  // image and the symbol naming it is the proof.
  BinaryImage Img =
      makeObjCImage("__objc_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, /*ClassName=*/nullptr);
  addSymbol(Img, "_ZTISt13runtime_error", kDescriptorVA, /*IsFunc=*/false);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  EXPECT_TRUE(F.ObjC->LandingPads[0].Catches[0].IsCxxType);
}

TEST(ObjCAppleEH, KeepsALocalCxxTypeApartByItsVTable) {
  // A type defined in this translation unit carries its own descriptor, whose
  // name field reads as a plain string and says nothing about which kind of
  // descriptor it is.  The vtable in the first field does: a `type_info`
  // subclass points into `__cxxabiv1`'s and an `objc_typeinfo` never can.
  BinaryImage Img =
      makeObjCImage("__objc_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, "9MyCxxType");
  // A vtable pointer addresses the second slot past the table's start, which
  // is where `writeAppleDescriptor` puts kDataVA + 0x7f0.
  addSymbol(Img, "_ZTVN10__cxxabiv117__class_type_infoE",
            kDataVA + 0x7f0 - 16, /*IsFunc=*/false);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  EXPECT_TRUE(F.ObjC->LandingPads[0].Catches[0].IsCxxType);
  EXPECT_EQ(F.ObjC->LandingPads[0].Catches[0].ClassName, "9MyCxxType");
}

TEST(ObjCAppleEH, DoesNotClaimAnObjectiveCClassIsACxxType) {
  BinaryImage Img =
      makeObjCImage("__objc_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, "NSException");
  addSymbol(Img, "objc_ehtype_vtable", kDataVA + 0x7f0 - 16,
            /*IsFunc=*/false);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  EXPECT_FALSE(F.ObjC->LandingPads[0].Catches[0].IsCxxType);
  EXPECT_EQ(F.ObjC->LandingPads[0].Catches[0].ClassName, "NSException");
}

TEST(ObjCAppleEH, UnwrapsAnImportedDescriptorsSymbolIntoAClassName) {
  BinaryImage Img =
      makeObjCImage("__objc_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, /*ClassName=*/nullptr);
  addSymbol(Img, "_OBJC_EHTYPE_$_NSFileHandleOperationException",
            kDescriptorVA, /*IsFunc=*/false);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  const ObjCCatchClause &Clause = F.ObjC->LandingPads[0].Catches[0];
  EXPECT_EQ(Clause.Kind, ObjCCatchKind::Class);
  EXPECT_EQ(Clause.ClassName, "NSFileHandleOperationException");
}

TEST(ObjCAppleEH, LeavesACleanupPadWithoutClauses) {
  BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[0].Kind, ObjCPadKind::Cleanup);
  EXPECT_TRUE(F.ObjC->LandingPads[0].Catches.empty());
  EXPECT_FALSE(F.ObjC->catchesAnything());
}

} // namespace
