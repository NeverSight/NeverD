//===- NativePointerRelocationBoundaryTests.cpp --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "gtest/gtest.h"

#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

namespace {

using namespace neverd;

class NativePointerRelocationBoundary : public NeverDLiftTest {
protected:
  static fs::path fixture(const char *Name) {
    return fs::path(TEST_OBJ_DIR) / Name;
  }

  static bool hasBaseRelocation(const BinaryImage &Img, va_t Address) {
    return std::any_of(
        Img.BaseRelocations.begin(), Img.BaseRelocations.end(),
        [&](const BaseRelocation &R) { return R.Address == Address; });
  }
};

TEST_F(NativePointerRelocationBoundary,
       SharedClassifierSeparatesDataSlotsFromCodeImmediates) {
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;

  auto AddSegment = [&](const char *Name, va_t VA, size_t Size,
                        SegmentFlags Flags, bool FileBacked = true) {
    Segment S;
    S.Name = Name;
    S.VA = VA;
    S.Size = Size;
    S.FileSz = FileBacked ? Size : 0;
    S.Flags = Flags;
    if (FileBacked)
      S.Data.resize(Size);
    Img.Segments.push_back(std::move(S));
  };
  AddSegment(".text", 0x1000, 0x40,
             SegmentFlags::Readable | SegmentFlags::Executable);
  AddSegment(".rodata", 0x2000, 0x80, SegmentFlags::Readable);
  AddSegment(".data", 0x3000, 0x40,
             SegmentFlags::Readable | SegmentFlags::Writable);
  AddSegment(".bss", 0x4000, 0x40,
             SegmentFlags::Readable | SegmentFlags::Writable,
             /*FileBacked=*/false);

  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x2000, 0x2020));
  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x2008, 0x3000));
  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x2010, 0x4000));
  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x2018, 0x1000));
  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x1008, 0x2028));
  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x1010, 0x1000));

  EXPECT_EQ(Img.DataPtrRelocSlots, (std::set<va_t>{0x2000, 0x2008, 0x2010}));
  EXPECT_EQ(Img.CodePtrRelocSlots, (std::set<va_t>{0x2018}));
  EXPECT_EQ(Img.RelocDataAddrs, (std::set<va_t>{0x2020, 0x2028}));
  EXPECT_EQ(Img.WritableRelocDataAddrs, (std::set<va_t>{0x3000, 0x4000}));
  EXPECT_EQ(Img.CodeRefTargets, (std::set<va_t>{0x1000}));
}

TEST_F(NativePointerRelocationBoundary,
       PEBaseRelocationsClassifyLocalDataPointerSlots) {
  struct Case {
    const char *Name;
    Arch TargetArch;
    uint32_t PointerSize;
  };
  for (const Case &C : {Case{"test_pe_pointer_reloc_x64.exe", Arch::X64, 8},
                        Case{"test_pe_pointer_reloc_x86.exe", Arch::X86, 4}}) {
    SCOPED_TRACE(C.Name);
    const fs::path Path = fixture(C.Name);
    if (!fs::exists(Path))
      GTEST_SKIP() << C.Name << " not built";

    auto ImgOrErr = loadBinary(Path);
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    ASSERT_EQ(Img.Format, BinaryFormat::COFF);
    ASSERT_EQ(Img.Arch, C.TargetArch);
    ASSERT_EQ(Img.getPointerSize(), C.PointerSize);

    const Section *Table = Img.getSectionByName(".rdata");
    ASSERT_NE(Table, nullptr);
    const va_t Slot0 = Table->VA;
    const va_t Slot1 = Slot0 + C.PointerSize;
    const va_t Target0 = Slot0 + 2 * C.PointerSize;
    const va_t Target1 = Target0 + sizeof(uint32_t);

    ASSERT_NE(Img.readVA(Slot0, 2 * C.PointerSize), nullptr);
    EXPECT_EQ(readPtr(Img.readVA(Slot0, C.PointerSize), C.PointerSize == 8),
              Target0);
    EXPECT_EQ(readPtr(Img.readVA(Slot1, C.PointerSize), C.PointerSize == 8),
              Target1);
    EXPECT_TRUE(hasBaseRelocation(Img, Slot0));
    EXPECT_TRUE(hasBaseRelocation(Img, Slot1));
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot0), 1u);
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot1), 1u);
    EXPECT_EQ(Img.RelocDataAddrs.count(Target0), 1u);
    EXPECT_EQ(Img.RelocDataAddrs.count(Target1), 1u);
  }
}

TEST_F(NativePointerRelocationBoundary,
       ELFDynamicRelativeRelocationsAreAppliedAndClassified) {
  for (const char *Name :
       {"test_elf_pointer_reloc_x64", "test_elf_pointer_relr_x64",
        "test_elf_pointer_android_relr_x64",
        "test_elf_pointer_android_packed_x64"}) {
    SCOPED_TRACE(Name);
    const fs::path Path = fixture(Name);
    if (!fs::exists(Path))
      GTEST_SKIP() << Name << " not built";

    auto ImgOrErr = loadBinary(Path);
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    ASSERT_EQ(Img.Format, BinaryFormat::ELF);
    ASSERT_EQ(Img.Arch, Arch::X64);
    ASSERT_FALSE(Img.IsRelocatable);

    const Section *Table = Img.getSectionByName(".data.rel.ro");
    const Section *Values = Img.getSectionByName(".rodata");
    ASSERT_NE(Table, nullptr);
    ASSERT_NE(Values, nullptr);
    const va_t Slot0 = Table->VA;
    const va_t Slot1 = Slot0 + sizeof(uint64_t);
    const va_t Target0 = Values->VA;
    const va_t Target1 = Target0 + sizeof(uint32_t);

    ASSERT_NE(Img.readVA(Slot0, 2 * sizeof(uint64_t)), nullptr);
    EXPECT_EQ(readLE<uint64_t>(Img.readVA(Slot0, sizeof(uint64_t))), Target0);
    EXPECT_EQ(readLE<uint64_t>(Img.readVA(Slot1, sizeof(uint64_t))), Target1);
    EXPECT_TRUE(hasBaseRelocation(Img, Slot0));
    EXPECT_TRUE(hasBaseRelocation(Img, Slot1));
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot0), 1u);
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot1), 1u);
    EXPECT_EQ(Img.RelocDataAddrs.count(Target0), 1u);
    EXPECT_EQ(Img.RelocDataAddrs.count(Target1), 1u);

    if (std::string(Name) == "test_elf_pointer_reloc_x64" ||
        std::string(Name) == "test_elf_pointer_android_packed_x64") {
      for (const auto &[Slot, Target] :
           {std::pair{Slot0, Target0}, std::pair{Slot1, Target1}}) {
        auto It = std::find_if(
            Img.Relocations.begin(), Img.Relocations.end(),
            [&](const RelocationEntry &R) { return R.Address == Slot; });
        ASSERT_NE(It, Img.Relocations.end());
        EXPECT_TRUE(It->HasExplicitAddend);
        EXPECT_EQ(static_cast<uint64_t>(It->Addend), Target);
      }
    }
  }
}

TEST_F(NativePointerRelocationBoundary,
       LiftedPEAndELFUseRelocatablePointerMirrors) {
  for (const char *Name :
       {"test_pe_pointer_reloc_x64.exe", "test_elf_pointer_reloc_x64",
        "test_pe_pointer_reloc_x86.exe", "test_elf_pointer_relr_x64",
        "test_elf_pointer_android_relr_x64",
        "test_elf_pointer_android_packed_x64"}) {
    const fs::path Path = fixture(Name);
    if (!fs::exists(Path))
      GTEST_SKIP() << Name << " not built";
    RunResult R = liftToLLVMIR(Path);
    ASSERT_EQ(R.exitCode, 0) << Name << ": " << R.err;
    EXPECT_NE(R.out.find("@__nd_codeptr_"), std::string::npos)
        << Name << " retained a raw pointer-table byte array:\n"
        << R.out;
    EXPECT_NE(R.out.find("ptrtoint"), std::string::npos) << Name;
  }
}

} // namespace
