//===- ObjCEHGnuTests.cpp - GNU Objective-C runtime EH tests ----------===//
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
// The GNU runtime
//===----------------------------------------------------------------------===//

TEST(ObjCGnuEH, ReadsTheClassNameOutOfTheSlotItself) {
  // The GNU runtime puts the class name string in the type-table slot rather
  // than the address of a descriptor.  Reading it the Itanium way -- as an
  // address whose `+ sizeof(void *)` field is a name pointer -- would follow a
  // pointer assembled out of the string's own characters.
  BinaryImage Img =
      makeObjCImage("__gnu_objc_personality_v0", {kStringVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeNameString(Img, "MyException");

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_EQ(F.ObjC->Runtime, ObjCRuntimeKind::GNU);
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);

  const ObjCCatchClause &Clause = F.ObjC->LandingPads[0].Catches[0];
  EXPECT_EQ(Clause.Kind, ObjCCatchKind::Class);
  EXPECT_EQ(Clause.ClassName, "MyException");
  // Nothing is dereferenced, so there is no class object to name.
  EXPECT_EQ(Clause.ClassVA, 0u);
}

TEST(ObjCGnuEH, ReadsTheNonFragileCatchIdMarker) {
  // The non-fragile GNU ABI needs `@catch(id)` to be distinct from a real
  // catch-all so that a foreign exception is not swallowed, and spells the
  // difference with the string `@id`.
  BinaryImage Img =
      makeObjCImage("__gnu_objc_personality_v0", {kStringVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeNameString(Img, "@id");

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[0].Catches[0].Kind, ObjCCatchKind::AnyObject);
}

TEST(ObjCGnuEH, ReadsTheFragileCatchAll) {
  // The fragile ABI had only one kind of catch-all and spelled it with a null
  // slot, which is what breaks foreign exceptions there.
  BinaryImage Img = makeObjCImage("__gnu_objc_personality_v0", {0},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[0].Catches[0].Kind, ObjCCatchKind::CatchAll);
}

TEST(ObjCGnustepEH, ReadsAnObjectiveCxxTypeInfo) {
  // GNUstep's Objective-C++ routine puts a real `std::type_info` subclass in
  // the slot so that C++ and Objective-C types can share a table, which means
  // the slot is read exactly the way an Itanium one is.
  BinaryImage Img =
      makeObjCImage("__gnustep_objcxx_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, "MyException");

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_EQ(F.ObjC->Runtime, ObjCRuntimeKind::GNUstepObjCXX);
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);

  const ObjCCatchClause &Clause = F.ObjC->LandingPads[0].Catches[0];
  EXPECT_EQ(Clause.Kind, ObjCCatchKind::Class);
  EXPECT_EQ(Clause.ClassName, "MyException");
  // GNUstep names a class by string and leaves the runtime to look it up, so
  // there is no class field to read even though the record has three words.
  EXPECT_EQ(Clause.ClassVA, 0u);
}

} // namespace
