//===- ObjCEHRuntimeCallTests.cpp - Objective-C runtime call site tests -===//
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
// Runtime call sites
//===----------------------------------------------------------------------===//

TEST(ObjCRuntimeCalls, ClassifiesEveryEntryPointAFrameCanReach) {
  struct Case {
    const char *Symbol;
    ObjCRuntimeCallKind Kind;
  } const Cases[] = {
      {"objc_exception_throw", ObjCRuntimeCallKind::Throw},
      {"objc_exception_rethrow", ObjCRuntimeCallKind::Rethrow},
      {"objc_begin_catch", ObjCRuntimeCallKind::BeginCatch},
      {"objc_end_catch", ObjCRuntimeCallKind::EndCatch},
      {"objc_sync_enter", ObjCRuntimeCallKind::SyncEnter},
      {"objc_sync_exit", ObjCRuntimeCallKind::SyncExit},
      {"objc_terminate", ObjCRuntimeCallKind::Terminate},
      {"objc_release", ObjCRuntimeCallKind::ARCCleanup},
      {"objc_storeStrong", ObjCRuntimeCallKind::ARCCleanup},
      {"objc_exception_try_enter", ObjCRuntimeCallKind::FragileTry},
      {"objc_exception_match", ObjCRuntimeCallKind::FragileTry},
  };
  for (const Case &C : Cases) {
    BinaryImage Img =
        makeObjCImage("__objc_personality_v0", {},
                      {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
    addSymbol(Img, C.Symbol, kRuntimeVA);
    writeCall(Img, kFuncVA, kRuntimeVA);

    const ExceptionFunction &F = decode(Img);
    ASSERT_TRUE(F.ObjC.has_value()) << C.Symbol;
    ASSERT_EQ(F.ObjC->RuntimeCalls.size(), 1u) << C.Symbol;
    EXPECT_EQ(F.ObjC->RuntimeCalls[0].Kind, C.Kind) << C.Symbol;
    EXPECT_EQ(F.ObjC->RuntimeCalls[0].CallVA, kFuncVA) << C.Symbol;
    EXPECT_EQ(F.ObjC->RuntimeCalls[0].TargetVA, kRuntimeVA) << C.Symbol;
  }
}

TEST(ObjCRuntimeCalls, DoesNotTakeAReturnValueOptimizationForACleanup) {
  // `objc_releaseReturnValue` optimizes a return sequence; it is not cleanup,
  // and a prefix match would sweep it in with the releases that are.
  BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  addSymbol(Img, "objc_releaseReturnValue", kRuntimeVA);
  writeCall(Img, kFuncVA, kRuntimeVA);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_TRUE(F.ObjC->RuntimeCalls.empty());
}

TEST(ObjCRuntimeCalls, MarksThePadThatClosesASynchronizedBody) {
  BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  addSymbol(Img, "objc_sync_enter", kRuntimeVA);
  addSymbol(Img, "objc_sync_exit", kRuntimeVA + 0x10);
  writeCall(Img, kFuncVA + 0x10, kRuntimeVA);
  writeCall(Img, kFuncVA + 0x40, kRuntimeVA + 0x10);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_TRUE(F.ObjC->guardsASynchronizedBody());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[0].Kind, ObjCPadKind::SynchronizedExit);
  ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value());
  EXPECT_EQ(Img.ExceptionMetadata.ObjCRuntime->SynchronizedFrames, 1u);
}

TEST(ObjCRuntimeCalls, ClaimsAFragileTryThatHasNoTableAtAll) {
  // The fragile runtime's `@try` is a setjmp buffer and a chain of matches, so
  // the frame carries no landing pad and its calls are the only evidence it
  // handles anything.  Its personality is C's, not Objective-C's.
  BinaryImage Img = makeObjCImage("__gcc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x00, /*Action=*/0}});
  addSymbol(Img, "objc_exception_try_enter", kRuntimeVA);
  addSymbol(Img, "objc_exception_extract", kRuntimeVA + 0x10);
  writeCall(Img, kFuncVA, kRuntimeVA);
  writeCall(Img, kFuncVA + 0x20, kRuntimeVA + 0x10);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_TRUE(F.ObjC->UsesFragileSetjmp);
  EXPECT_TRUE(F.ObjC->LandingPads.empty());
  ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value());
  EXPECT_EQ(Img.ExceptionMetadata.ObjCRuntime->FragileTryFrames, 1u);
}

TEST(ObjCRuntimeCalls, CountsThrowSitesButNotResumes) {
  BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  addSymbol(Img, "objc_exception_throw", kRuntimeVA);
  addSymbol(Img, "objc_exception_rethrow", kRuntimeVA + 0x10);
  writeCall(Img, kFuncVA, kRuntimeVA);
  writeCall(Img, kFuncVA + 0x20, kRuntimeVA + 0x10);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_TRUE(F.ObjC->throwsAnException());
  ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value());
  EXPECT_EQ(Img.ExceptionMetadata.ObjCRuntime->ThrowSites, 1u);
}

} // namespace
