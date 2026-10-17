//===- DarwinCallbackTests.cpp - Original callback admission facts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent original-byte startup forms, target bounds and refusals.
#include "DarwinTestImage.h"
#include "gtest/gtest.h"
#include "os/darwin/process/DarwinProcess.h"

#include "neverd/loader/MachO/MachOExecutionImage.h"

namespace neverd::emulation {
namespace {
using namespace llvm::MachO;
using darwin_test::TemporaryImage;
struct CallbackImage : darwin_test::Image {
  static constexpr uint64_t Base = 0x100000000ULL;
  size_t Section = 0, Routine = 0, Data = 0;
  CallbackImage(bool X64, unsigned Form) : darwin_test::Image(X64) {
    u32(Thread, LC_MAIN);
    u32(Thread + 4, 24);
    u64(Thread + 8, 4096);
    u64(Thread + 16, 0);
    CommandsEnd = Thread + 24;
    u32(20, CommandsEnd - 32);
    u32(24, MH_DYLDLINK | MH_TWOLEVEL | MH_NOUNDEFS | MH_PIE);
    if (Form == 2) {
      Routine = command(LC_ROUTINES_64, 72);
      std::fill(Bytes.begin() + Routine + 8, Bytes.begin() + Routine + 72, 0);
      u64(Routine + 8, Base + 4100);
      return;
    }
    Bytes.resize(32768, 0);
    Data = command(LC_SEGMENT_64, 152);
    std::copy_n("__CONST", 7, Bytes.data() + Data + 8);
    u64(Data + 24, Base + 16384);
    u64(Data + 32, 16384);
    u64(Data + 40, 16384);
    u64(Data + 48, 16384);
    u32(Data + 56, 1);
    u32(Data + 60, 1);
    u32(Data + 64, 1);
    Section = Data + 72;
    std::copy_n("__init", 6, Bytes.data() + Section);
    std::copy_n("__CONST", 7, Bytes.data() + Section + 16);
    u64(Section + 32, Base + 16384);
    u64(Section + 40, Form ? 4 : 8);
    u32(Section + 48, 16384);
    u32(Section + 52, Form ? 2 : 3);
    u32(Section + 64, Form ? S_INIT_FUNC_OFFSETS : S_MOD_INIT_FUNC_POINTERS);
    u64(16384, Form ? 4100 : Base + 4100);
  }
};
auto load(const CallbackImage &I) {
  TemporaryImage File(I);
  return darwin_model::loadImage(File.path(), darwin_model::macOSProfile(), {});
}
void rejects(const CallbackImage &I) {
  auto R = load(I);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
}
TEST(DarwinCallbacks, AllThreeOriginalFormsRequireStrongMainAdmission) {
  for (bool X64 : {false, true})
    for (unsigned Form = 0; Form != 3; ++Form) {
      SCOPED_TRACE(Form);
      CallbackImage I(X64, Form);
      auto R = load(I);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Initializers.size(), 1u);
      EXPECT_EQ(R->Initializers[0].Target, I.Base + 4100);
      EXPECT_EQ(R->Initializers[0].Width, Form == 1 ? 4u : 8u);
      EXPECT_EQ(R->Initializers[0].Slot,
                I.Base + (Form == 2 ? I.Routine + 8 : 16384));
      EXPECT_EQ(R->ExecutableSpans.size(), 1u);
      I.u32(24, MH_NOUNDEFS);
      rejects(I);
    }
}
TEST(DarwinCallbacks, ConstructorOnlyImagesKeepTheirExistingExactBudget) {
  for (bool X64 : {false, true})
    for (unsigned Form = 0; Form != 3; ++Form) {
      CallbackImage I(X64, Form);
      TemporaryImage File(I);
      ProcessOptions Options;
      Options.StackSize = 16384;
      auto Full = darwin_model::loadImage(
          File.path(), darwin_model::macOSProfile(), Options);
      ASSERT_TRUE(bool(Full)) << llvm::toString(Full.takeError());
      EXPECT_FALSE(Full->HasTLSCallbacks);
      EXPECT_FALSE(Full->HasGlobalCallbacks);
      Options.MemoryLimit = Full->MetadataBytes + Full->Plan.MappedBytes +
                            Options.StackSize + Full->Memory.PageSize;
      auto Exact = darwin_model::loadImage(
          File.path(), darwin_model::macOSProfile(), Options);
      ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
      --Options.MemoryLimit;
      auto Under = darwin_model::loadImage(
          File.path(), darwin_model::macOSProfile(), Options);
      ASSERT_FALSE(bool(Under));
      llvm::consumeError(Under.takeError());
    }
}
TEST(DarwinCallbacks,
     InitializerStorageRejectsPartialMisalignedOrWritableOffsets) {
  for (unsigned Form = 0; Form != 2; ++Form)
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      CallbackImage I(false, Form);
      if (Mutation == 0)
        I.u64(I.Section + 40, Form ? 3 : 7);
      else if (Mutation == 1)
        I.u64(I.Section + 32, I.Base + 16385);
      else if (Mutation == 2)
        I.u32(I.Section + 48, 16392);
      else if (Mutation == 3)
        I.u64(I.Section + 40, 16392);
      else
        I.u64(I.Section + 32, UINT64_MAX - 3);
      rejects(I);
    }
  for (bool ReadOnlyAfterFixups : {false, true}) {
    CallbackImage I(false, 1);
    I.u32(I.Data + 56, 3);
    I.u32(I.Data + 60, 3);
    I.u32(I.Data + 68, ReadOnlyAfterFixups ? SG_READ_ONLY : 0);
    rejects(I);
  }
}
TEST(DarwinCallbacks, RoutinesRejectDuplicatesModuleStateAndUnknownWidth) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    CallbackImage I(false, 2);
    if (Mutation == 0) {
      const auto Added = I.command(LC_ROUTINES_64, 72);
      std::copy_n(I.Bytes.data() + I.Routine + 8, 64,
                  I.Bytes.data() + Added + 8);
    } else if (Mutation == 1)
      I.u64(I.Routine + 16, 1);
    else if (Mutation == 2)
      I.u64(I.Routine + 64, 1);
    else if (Mutation == 3)
      I.u32(I.Routine, LC_ROUTINES);
    else
      I.u64(I.Routine + 8, 0);
    rejects(I);
  }
}
TEST(DarwinCallbacks, TargetsRequireCompleteOriginalExecutableEntryBytes) {
  for (bool X64 : {false, true})
    for (unsigned Form = 0; Form != 3; ++Form)
      for (uint64_t Target : {uint64_t(0), uint64_t(16384),
                              uint64_t(0x7fffffc00000ULL), UINT64_MAX}) {
        if (Form == 1 && Target == 0)
          continue; // A compact zero is an offset to the executable header.
        CallbackImage I(X64, Form);
        const uint64_t Word = Form == 1                  ? Target
                              : Target && Target < 32768 ? I.Base + Target
                                                         : Target;
        I.u64(Form == 2 ? I.Routine + 8 : 16384, Word);
        if (Form == 1 && Target == UINT64_MAX)
          I.u32(16384, UINT32_MAX);
        rejects(I);
      }
  for (bool X64 : {false, true}) {
    CallbackImage I(X64, 2);
    I.u64(I.Text + 32, 5002);
    I.u64(I.Text + 48, 5002);
    I.u64(I.Routine + 8, I.Base + 5000);
    auto R = load(I);
    EXPECT_EQ(bool(R), X64);
    if (!R)
      llvm::consumeError(R.takeError());
    I.u64(I.Routine + 8, I.Base + 5004);
    rejects(I); // Page rounding cannot grant callback target ownership.
  }
}
TEST(DarwinCallbacks, GlobalTerminatorsAndLegacyTLSCallbacksRemainRefused) {
  for (uint32_t Kind :
       {S_MOD_TERM_FUNC_POINTERS, S_THREAD_LOCAL_INIT_FUNCTION_POINTERS}) {
    CallbackImage I(false, 0);
    I.u32(I.Section + 64, Kind);
    rejects(I);
  }
}
} // namespace
} // namespace neverd::emulation
