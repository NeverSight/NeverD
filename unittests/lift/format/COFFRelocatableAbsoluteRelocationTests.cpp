//===- COFFRelocatableAbsoluteRelocationTests.cpp ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "gtest/gtest.h"

#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

namespace {

using namespace neverd;

class COFFRelocatableAbsoluteRelocation : public NeverDLiftTest {
protected:
  fs::path compileCOFF(std::string_view Name, std::string_view Triple,
                       std::string_view Assembly) {
    const fs::path Source = tmpFile(std::string(Name) + ".s");
    const fs::path Object = tmpFile(std::string(Name) + ".obj");
    {
      std::ofstream OS(Source);
      EXPECT_TRUE(OS) << Source;
      OS << Assembly;
    }
    RunResult Compile =
        exec(NEVERD_TEST_CLANG, {"-target", std::string(Triple), "-c",
                                 Source.string(), "-o", Object.string()});
    if (Compile.exitCode != 0) {
      ADD_FAILURE() << "clang could not emit the COFF fixture: " << Compile.err;
      return {};
    }
    return Object;
  }

  static const Symbol *findSymbol(const BinaryImage &Img,
                                  std::string_view Name) {
    auto It = std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                           [&](const Symbol &Sym) { return Sym.Name == Name; });
    return It == Img.Symbols.end() ? nullptr : &*It;
  }
};

TEST_F(COFFRelocatableAbsoluteRelocation,
       AMD64REL32AppliesSignedInPlaceAddends) {
  const fs::path Object =
      compileCOFF("coff_rel32_addends_x64", "x86_64-pc-windows-msvc", R"(
.text
.globl rel32_positive
rel32_positive:
  leaq data_target+8(%rip), %rax
  retq

.globl rel32_negative
rel32_negative:
  leaq data_target-4(%rip), %rcx
  retq

.section .rdata,"dr"
.p2align 3
.globl data_target
data_target:
  .quad 0x1122334455667788
  .quad 0x99aabbccddeeff00
)");
  ASSERT_FALSE(Object.empty());

  auto ImgOrErr = loadBinary(Object);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  ASSERT_EQ(Img.Format, BinaryFormat::COFF);
  ASSERT_EQ(Img.Arch, Arch::X64);
  ASSERT_TRUE(Img.IsRelocatable);

  const Symbol *Positive = findSymbol(Img, "rel32_positive");
  const Symbol *Negative = findSymbol(Img, "rel32_negative");
  const Symbol *Target = findSymbol(Img, "data_target");
  ASSERT_NE(Positive, nullptr);
  ASSERT_NE(Negative, nullptr);
  ASSERT_NE(Target, nullptr);

  const va_t PositiveField = Positive->Addr + 3;
  const va_t NegativeField = Negative->Addr + 3;
  const uint8_t *PositiveBytes = Img.readVA(PositiveField, sizeof(uint32_t));
  const uint8_t *NegativeBytes = Img.readVA(NegativeField, sizeof(uint32_t));
  ASSERT_NE(PositiveBytes, nullptr);
  ASSERT_NE(NegativeBytes, nullptr);
  EXPECT_EQ(readLE<uint32_t>(PositiveBytes),
            static_cast<uint32_t>(Target->Addr + 8 - (PositiveField + 4)));
  EXPECT_EQ(readLE<uint32_t>(NegativeBytes),
            static_cast<uint32_t>(Target->Addr - 4 - (NegativeField + 4)));
}

TEST_F(COFFRelocatableAbsoluteRelocation, I386REL32AppliesSignedInPlaceAddend) {
  const fs::path Object =
      compileCOFF("coff_rel32_addend_x86", "i686-pc-windows-msvc", R"(
.text
.globl caller
caller:
  calll callee+4
  retl

.section .text$callee,"xr"
.globl callee
callee:
  retl
  retl
  retl
  retl
  retl
)");
  ASSERT_FALSE(Object.empty());

  auto ImgOrErr = loadBinary(Object);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  ASSERT_EQ(Img.Format, BinaryFormat::COFF);
  ASSERT_EQ(Img.Arch, Arch::X86);
  ASSERT_TRUE(Img.IsRelocatable);

  const Symbol *Caller = findSymbol(Img, "caller");
  const Symbol *Callee = findSymbol(Img, "callee");
  ASSERT_NE(Caller, nullptr);
  ASSERT_NE(Callee, nullptr);
  const va_t FieldVA = Caller->Addr + 1;
  const uint8_t *Bytes = Img.readVA(FieldVA, sizeof(uint32_t));
  ASSERT_NE(Bytes, nullptr);
  EXPECT_EQ(readLE<uint32_t>(Bytes),
            static_cast<uint32_t>(Callee->Addr + 4 - (FieldVA + 4)));
}

TEST_F(COFFRelocatableAbsoluteRelocation,
       AMD64SeparatesFullDataSlotsFromExactInstructionFields) {
  const fs::path Object =
      compileCOFF("coff_absolute_occurrences_x64", "x86_64-pc-windows-msvc", R"(
.text
.globl addr64_immediate
addr64_immediate:
  movabsq $data_end, %rax
  retq

.globl addr64_negative_immediate
addr64_negative_immediate:
  movabsq $data_end-4, %rcx
  retq

.globl addr64_underflow_immediate
addr64_underflow_immediate:
  movabsq $data_target-0x2000, %rdx
  retq

.globl addr32_immediate
addr32_immediate:
  movl $data_target+4, %eax
  retq

.globl addr32_displacement
addr32_displacement:
  movl data_target, %eax
  retq

.globl addr32_negative_displacement
addr32_negative_displacement:
  movl data_end-4, %eax
  retq

.globl addr32_underflow_immediate
addr32_underflow_immediate:
  movl $data_target-0x2000, %edx
  retq

.globl addr32_zext_overflow_immediate
addr32_zext_overflow_immediate:
  movl $high_target+0x7fffffff, %edi
  retq

.globl addr32nb_immediate
addr32nb_immediate:
  .byte 0xb8
  .long data_end@IMGREL-4
  retq

.section .rdata,"dr"
.p2align 3
.globl full_data_slot
full_data_slot:
  .quad data_end
.globl narrow_data_slot
narrow_data_slot:
  .long data_target
.globl data_target
data_target:
  .long 7
.globl data_end
data_end:

.section .after,"dw"
  .long 13

.section .huge,"bw"
.space 0x80000000

.section .high,"bw"
.globl high_target
high_target:
  .space 0x80000000
)");
  ASSERT_FALSE(Object.empty());

  auto ImgOrErr = loadBinary(Object);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  ASSERT_EQ(Img.Format, BinaryFormat::COFF);
  ASSERT_EQ(Img.Arch, Arch::X64);
  ASSERT_TRUE(Img.IsRelocatable);

  const Symbol *Target = findSymbol(Img, "data_target");
  ASSERT_NE(Target, nullptr);
  ASSERT_LT(Target->Addr, 0x2000ULL);
  const Section *SemanticOwner = Img.getSectionFor(Target->Addr);
  ASSERT_NE(SemanticOwner, nullptr);
  ASSERT_FALSE(Img.hasExecutableCodeOwnerAt(Target->Addr));
  const va_t OnePastTarget = Target->Addr + sizeof(uint32_t);
  const Symbol *DataEnd = findSymbol(Img, "data_end");
  ASSERT_NE(DataEnd, nullptr);
  EXPECT_EQ(DataEnd->Addr, OnePastTarget);
  const Section *NumericOwner = Img.getSectionFor(OnePastTarget);
  ASSERT_NE(NumericOwner, nullptr);
  EXPECT_EQ(NumericOwner->Name, ".after");
  EXPECT_EQ(NumericOwner->VA, OnePastTarget);
  EXPECT_TRUE(NumericOwner->isWritable());

  const Symbol *Addr64Underflow = findSymbol(Img, "addr64_underflow_immediate");
  const Symbol *Addr32Underflow = findSymbol(Img, "addr32_underflow_immediate");
  const Symbol *Addr32ZExtOverflow =
      findSymbol(Img, "addr32_zext_overflow_immediate");
  const Symbol *HighTarget = findSymbol(Img, "high_target");
  ASSERT_NE(Addr64Underflow, nullptr);
  ASSERT_NE(Addr32Underflow, nullptr);
  ASSERT_NE(Addr32ZExtOverflow, nullptr);
  ASSERT_NE(HighTarget, nullptr);
  const va_t Addr64UnderflowField = Addr64Underflow->Addr + 2;
  const va_t Addr32UnderflowField = Addr32Underflow->Addr + 1;
  const va_t Addr32ZExtOverflowField = Addr32ZExtOverflow->Addr + 1;
  const va_t Addr32UnencodableTarget = HighTarget->Addr + 0x7fffffffULL;
  ASSERT_GT(Addr32UnencodableTarget, UINT32_MAX);
  const Section *HighOwner = Img.getSectionFor(HighTarget->Addr);
  ASSERT_NE(HighOwner, nullptr);
  ASSERT_TRUE(HighOwner->isReadable());
  ASSERT_TRUE(HighOwner->contains(Addr32UnencodableTarget));

  unsigned Addr64Instructions = 0;
  unsigned Addr32Instructions = 0;
  unsigned Addr32NBInstructions = 0;
  unsigned UnderflowFields = 0;
  unsigned ZExtOverflowFields = 0;
  unsigned FullDataSlots = 0;
  unsigned NarrowDataSlots = 0;
  std::set<va_t> Addr64Targets;
  std::set<va_t> Addr32Targets;
  for (const RelocationEntry &Reloc : Img.Relocations) {
    if (Reloc.SymbolName != "data_target" && Reloc.SymbolName != "data_end" &&
        Reloc.SymbolName != "high_target")
      continue;
    const Section *Owner = Img.getSectionByName(Reloc.SectionName);
    ASSERT_NE(Owner, nullptr);
    const va_t FieldVA = Reloc.Address;
    ASSERT_TRUE(Owner->contains(FieldVA));

    if (FieldVA == Addr64UnderflowField || FieldVA == Addr32UnderflowField) {
      ++UnderflowFields;
      EXPECT_EQ(Img.DataAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodeAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
      if (FieldVA == Addr64UnderflowField) {
        EXPECT_EQ(Reloc.Type, llvm::COFF::IMAGE_REL_AMD64_ADDR64);
        const uint8_t *Bytes = Img.readVA(FieldVA, sizeof(uint64_t));
        ASSERT_NE(Bytes, nullptr);
        EXPECT_EQ(readLE<uint64_t>(Bytes), Target->Addr - 0x2000ULL);
      } else {
        EXPECT_EQ(Reloc.Type, llvm::COFF::IMAGE_REL_AMD64_ADDR32);
        const uint8_t *Bytes = Img.readVA(FieldVA, sizeof(uint32_t));
        ASSERT_NE(Bytes, nullptr);
        EXPECT_EQ(readLE<uint32_t>(Bytes),
                  static_cast<uint32_t>(Target->Addr - 0x2000ULL));
      }
      continue;
    }

    if (FieldVA == Addr32ZExtOverflowField) {
      ++ZExtOverflowFields;
      EXPECT_EQ(Reloc.Type, llvm::COFF::IMAGE_REL_AMD64_ADDR32);
      EXPECT_EQ(Img.DataAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodeAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
      const uint8_t *Bytes = Img.readVA(FieldVA, sizeof(uint32_t));
      ASSERT_NE(Bytes, nullptr);
      EXPECT_EQ(readLE<uint32_t>(Bytes),
                static_cast<uint32_t>(Addr32UnencodableTarget));
      continue;
    }

    if (Reloc.SectionName == ".text" &&
        Reloc.Type == llvm::COFF::IMAGE_REL_AMD64_ADDR64) {
      ++Addr64Instructions;
      auto It = Img.DataAddressRelocOperands.find(FieldVA);
      ASSERT_NE(It, Img.DataAddressRelocOperands.end());
      EXPECT_EQ(It->second.Width, 8u);
      EXPECT_EQ(It->second.EncodedValue, It->second.TargetVA);
      EXPECT_EQ(It->second.TargetOwnerVA, SemanticOwner->VA);
      Addr64Targets.insert(It->second.TargetVA);
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
    } else if (Reloc.SectionName == ".text" &&
               Reloc.Type == llvm::COFF::IMAGE_REL_AMD64_ADDR32) {
      ++Addr32Instructions;
      auto It = Img.DataAddressRelocOperands.find(FieldVA);
      ASSERT_NE(It, Img.DataAddressRelocOperands.end());
      EXPECT_EQ(It->second.Width, 4u);
      EXPECT_EQ(It->second.EncodedValue,
                static_cast<uint32_t>(It->second.TargetVA));
      EXPECT_EQ(It->second.TargetOwnerVA, SemanticOwner->VA);
      Addr32Targets.insert(It->second.TargetVA);
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
    } else if (Reloc.SectionName == ".text" &&
               Reloc.Type == llvm::COFF::IMAGE_REL_AMD64_ADDR32NB) {
      ++Addr32NBInstructions;
      EXPECT_EQ(Img.DataAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodeAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
      const uint8_t *Bytes = Img.readVA(FieldVA, sizeof(uint32_t));
      ASSERT_NE(Bytes, nullptr);
      EXPECT_EQ(readLE<uint32_t>(Bytes),
                static_cast<uint32_t>(Target->Addr - Img.Base));
    } else if (Reloc.SectionName == ".rdata" &&
               Reloc.Type == llvm::COFF::IMAGE_REL_AMD64_ADDR64) {
      ++FullDataSlots;
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 1u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.DataAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodeAddressRelocOperands.count(FieldVA), 0u);
      auto OwnerIt = Img.DataPtrRelocTargetOwners.find(FieldVA);
      ASSERT_NE(OwnerIt, Img.DataPtrRelocTargetOwners.end());
      EXPECT_EQ(OwnerIt->second, SemanticOwner->VA);
      const uint8_t *Bytes = Img.readVA(FieldVA, sizeof(uint64_t));
      ASSERT_NE(Bytes, nullptr);
      EXPECT_EQ(readLE<uint64_t>(Bytes), OnePastTarget);
    } else if (Reloc.SectionName == ".rdata" &&
               Reloc.Type == llvm::COFF::IMAGE_REL_AMD64_ADDR32) {
      ++NarrowDataSlots;
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.DataAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodeAddressRelocOperands.count(FieldVA), 0u);
    }
  }

  EXPECT_EQ(Addr64Instructions, 2u);
  EXPECT_EQ(Addr32Instructions, 3u);
  EXPECT_EQ(Addr32NBInstructions, 1u);
  EXPECT_EQ(UnderflowFields, 2u);
  EXPECT_EQ(ZExtOverflowFields, 1u);
  EXPECT_EQ(FullDataSlots, 1u);
  EXPECT_EQ(NarrowDataSlots, 1u);
  EXPECT_EQ(Addr64Targets, (std::set<va_t>{Target->Addr, OnePastTarget}));
  EXPECT_EQ(Addr32Targets, (std::set<va_t>{Target->Addr, OnePastTarget}));
  EXPECT_EQ(Img.RelocDataAddrs.count(Target->Addr), 1u);
  EXPECT_EQ(Img.RelocDataAddrs.count(OnePastTarget), 1u);
  EXPECT_EQ(Img.WritableRelocDataAddrs.count(OnePastTarget), 0u);
}

TEST_F(COFFRelocatableAbsoluteRelocation,
       I386DIR32SeparatesInstructionFieldsFromPointerSlots) {
  const fs::path Object =
      compileCOFF("coff_absolute_occurrences_x86", "i686-pc-windows-msvc", R"(
.text
.globl dir32_immediate
dir32_immediate:
  movl $data_target+4, %eax
  retl

.globl dir32_displacement
dir32_displacement:
  movl data_target, %eax
  retl

.globl dir32_negative_displacement
dir32_negative_displacement:
  movl data_end-4, %eax
  retl

.globl dir32_underflow_immediate
dir32_underflow_immediate:
  movl $data_target-0x2000, %edx
  retl

.globl dir32_zext_overflow_immediate
dir32_zext_overflow_immediate:
  movl $high_target+0x7fffffff, %edi
  retl

.section .rdata,"dr"
.p2align 2
.globl data_slot
data_slot:
  .long data_end
.globl data_target
data_target:
  .long 9
.globl data_end
data_end:

.section .after,"dw"
  .long 17

.section .huge,"bw"
.space 0x80000000

.section .high,"bw"
.globl high_target
high_target:
  .space 0x80000000
)");
  ASSERT_FALSE(Object.empty());

  auto ImgOrErr = loadBinary(Object);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  ASSERT_EQ(Img.Format, BinaryFormat::COFF);
  ASSERT_EQ(Img.Arch, Arch::X86);
  ASSERT_TRUE(Img.IsRelocatable);

  const Symbol *Target = findSymbol(Img, "data_target");
  ASSERT_NE(Target, nullptr);
  ASSERT_LT(Target->Addr, 0x2000ULL);
  const Section *SemanticOwner = Img.getSectionFor(Target->Addr);
  ASSERT_NE(SemanticOwner, nullptr);
  ASSERT_FALSE(Img.hasExecutableCodeOwnerAt(Target->Addr));
  const va_t OnePastTarget = Target->Addr + sizeof(uint32_t);
  const Symbol *DataEnd = findSymbol(Img, "data_end");
  ASSERT_NE(DataEnd, nullptr);
  EXPECT_EQ(DataEnd->Addr, OnePastTarget);
  const Section *NumericOwner = Img.getSectionFor(OnePastTarget);
  ASSERT_NE(NumericOwner, nullptr);
  EXPECT_EQ(NumericOwner->Name, ".after");
  EXPECT_EQ(NumericOwner->VA, OnePastTarget);
  EXPECT_TRUE(NumericOwner->isWritable());

  const Symbol *Underflow = findSymbol(Img, "dir32_underflow_immediate");
  const Symbol *ZExtOverflow = findSymbol(Img, "dir32_zext_overflow_immediate");
  const Symbol *HighTarget = findSymbol(Img, "high_target");
  ASSERT_NE(Underflow, nullptr);
  ASSERT_NE(ZExtOverflow, nullptr);
  ASSERT_NE(HighTarget, nullptr);
  const va_t UnderflowField = Underflow->Addr + 1;
  const va_t ZExtOverflowField = ZExtOverflow->Addr + 1;
  const va_t UnencodableTarget = HighTarget->Addr + 0x7fffffffULL;
  ASSERT_GT(UnencodableTarget, UINT32_MAX);
  const Section *HighOwner = Img.getSectionFor(HighTarget->Addr);
  ASSERT_NE(HighOwner, nullptr);
  ASSERT_TRUE(HighOwner->isReadable());
  ASSERT_TRUE(HighOwner->contains(UnencodableTarget));

  unsigned InstructionFields = 0;
  unsigned UnderflowFields = 0;
  unsigned ZExtOverflowFields = 0;
  unsigned PointerSlots = 0;
  std::set<va_t> InstructionTargets;
  for (const RelocationEntry &Reloc : Img.Relocations) {
    if ((Reloc.SymbolName != "data_target" && Reloc.SymbolName != "data_end" &&
         Reloc.SymbolName != "high_target") ||
        Reloc.Type != llvm::COFF::IMAGE_REL_I386_DIR32)
      continue;
    const Section *Owner = Img.getSectionByName(Reloc.SectionName);
    ASSERT_NE(Owner, nullptr);
    const va_t FieldVA = Reloc.Address;
    ASSERT_TRUE(Owner->contains(FieldVA));
    if (FieldVA == UnderflowField) {
      ++UnderflowFields;
      EXPECT_EQ(Img.DataAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodeAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
      const uint8_t *Bytes = Img.readVA(FieldVA, sizeof(uint32_t));
      ASSERT_NE(Bytes, nullptr);
      EXPECT_EQ(readLE<uint32_t>(Bytes),
                static_cast<uint32_t>(Target->Addr - 0x2000ULL));
      continue;
    }
    if (FieldVA == ZExtOverflowField) {
      ++ZExtOverflowFields;
      EXPECT_EQ(Img.DataAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodeAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
      const uint8_t *Bytes = Img.readVA(FieldVA, sizeof(uint32_t));
      ASSERT_NE(Bytes, nullptr);
      EXPECT_EQ(readLE<uint32_t>(Bytes),
                static_cast<uint32_t>(UnencodableTarget));
      continue;
    }
    if (Reloc.SectionName == ".text") {
      ++InstructionFields;
      auto It = Img.DataAddressRelocOperands.find(FieldVA);
      ASSERT_NE(It, Img.DataAddressRelocOperands.end());
      EXPECT_EQ(It->second.Width, 4u);
      EXPECT_EQ(It->second.EncodedValue, It->second.TargetVA);
      EXPECT_EQ(It->second.TargetOwnerVA, SemanticOwner->VA);
      InstructionTargets.insert(It->second.TargetVA);
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
    } else if (Reloc.SectionName == ".rdata") {
      ++PointerSlots;
      EXPECT_EQ(Img.DataPtrRelocSlots.count(FieldVA), 1u);
      EXPECT_EQ(Img.CodePtrRelocSlots.count(FieldVA), 0u);
      EXPECT_EQ(Img.DataAddressRelocOperands.count(FieldVA), 0u);
      EXPECT_EQ(Img.CodeAddressRelocOperands.count(FieldVA), 0u);
      auto OwnerIt = Img.DataPtrRelocTargetOwners.find(FieldVA);
      ASSERT_NE(OwnerIt, Img.DataPtrRelocTargetOwners.end());
      EXPECT_EQ(OwnerIt->second, SemanticOwner->VA);
      const uint8_t *Bytes = Img.readVA(FieldVA, sizeof(uint32_t));
      ASSERT_NE(Bytes, nullptr);
      EXPECT_EQ(readLE<uint32_t>(Bytes), OnePastTarget);
    }
  }

  EXPECT_EQ(InstructionFields, 3u);
  EXPECT_EQ(UnderflowFields, 1u);
  EXPECT_EQ(ZExtOverflowFields, 1u);
  EXPECT_EQ(PointerSlots, 1u);
  EXPECT_EQ(InstructionTargets, (std::set<va_t>{Target->Addr, OnePastTarget}));
  EXPECT_EQ(Img.RelocDataAddrs.count(Target->Addr), 1u);
  EXPECT_EQ(Img.RelocDataAddrs.count(OnePastTarget), 1u);
  EXPECT_EQ(Img.WritableRelocDataAddrs.count(OnePastTarget), 0u);
}

TEST_F(COFFRelocatableAbsoluteRelocation,
       ARM64AppliesPageOffsetAddendsAndAbsoluteSlots) {
  const fs::path Object =
      compileCOFF("coff_arm64_address_addends", "aarch64-pc-windows-msvc",
                  R"(
.text
.globl materialize_data
materialize_data:
  adrp x0, data_target+8
  add x0, x0, :lo12:data_target+8
  adrp x1, data_target+8
  ldr x1, [x1, :lo12:data_target+8]
  adrp x2, data_target+16
  ldr q0, [x2, :lo12:data_target+16]
  ret

.section .rdata,"dr"
.p2align 3
.globl pointer_slot
pointer_slot:
  .quad data_target+8
.globl narrow_slot
narrow_slot:
  .long data_target+8

.data
.p2align 4
.globl data_target
data_target:
  .quad 0x1122334455667788
  .quad 0x99aabbccddeeff00
)");
  ASSERT_FALSE(Object.empty());

  auto ImgOrErr = loadBinary(Object);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  ASSERT_EQ(Img.Format, BinaryFormat::COFF);
  ASSERT_EQ(Img.Arch, Arch::AArch64);
  ASSERT_TRUE(Img.IsRelocatable);

  const Symbol *Function = findSymbol(Img, "materialize_data");
  const Symbol *Slot = findSymbol(Img, "pointer_slot");
  const Symbol *NarrowSlot = findSymbol(Img, "narrow_slot");
  const Symbol *Target = findSymbol(Img, "data_target");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Slot, nullptr);
  ASSERT_NE(NarrowSlot, nullptr);
  ASSERT_NE(Target, nullptr);
  const Section *TargetOwner = Img.getSectionFor(Target->Addr);
  ASSERT_NE(TargetOwner, nullptr);

  const uint8_t *AddBytes = Img.readVA(Function->Addr + 4, sizeof(uint32_t));
  const uint8_t *LoadBytes = Img.readVA(Function->Addr + 12, sizeof(uint32_t));
  const uint8_t *VectorLoadBytes =
      Img.readVA(Function->Addr + 20, sizeof(uint32_t));
  const uint8_t *SlotBytes = Img.readVA(Slot->Addr, sizeof(uint64_t));
  const uint8_t *NarrowSlotBytes =
      Img.readVA(NarrowSlot->Addr, sizeof(uint32_t));
  ASSERT_NE(AddBytes, nullptr);
  ASSERT_NE(LoadBytes, nullptr);
  ASSERT_NE(VectorLoadBytes, nullptr);
  ASSERT_NE(SlotBytes, nullptr);
  ASSERT_NE(NarrowSlotBytes, nullptr);
  const uint64_t ExpectedTarget = Target->Addr + 8;
  const uint32_t AddInsn = readLE<uint32_t>(AddBytes);
  const uint32_t LoadInsn = readLE<uint32_t>(LoadBytes);
  const uint32_t VectorLoadInsn = readLE<uint32_t>(VectorLoadBytes);
  EXPECT_EQ((AddInsn >> 10) & 0xfffu, ExpectedTarget & 0xfffu);
  EXPECT_EQ((LoadInsn >> 10) & 0xfffu, (ExpectedTarget & 0xfffu) >> 3);
  EXPECT_EQ((VectorLoadInsn >> 10) & 0xfffu,
            ((Target->Addr + 16) & 0xfffu) >> 4);
  EXPECT_EQ(readLE<uint64_t>(SlotBytes), ExpectedTarget);
  EXPECT_EQ(readLE<uint32_t>(NarrowSlotBytes),
            static_cast<uint32_t>(ExpectedTarget));

  EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot->Addr), 1u);
  auto OwnerIt = Img.DataPtrRelocTargetOwners.find(Slot->Addr);
  ASSERT_NE(OwnerIt, Img.DataPtrRelocTargetOwners.end());
  EXPECT_EQ(OwnerIt->second, TargetOwner->VA);
}

TEST_F(COFFRelocatableAbsoluteRelocation,
       ARM32AppliesAbsoluteAddendsAndRecordsPointerSlots) {
  const fs::path Object =
      compileCOFF("coff_arm32_address_addends", "armv7-pc-windows-msvc",
                  R"(
.syntax unified
.thumb
.text
.globl materialize_data
.thumb_func
materialize_data:
  movw r0, :lower16:data_target+4
  movt r0, :upper16:data_target+4
  bx lr

.globl materialize_negative
.thumb_func
materialize_negative:
  movw r2, :lower16:data_target-4
  movt r2, :upper16:data_target-4
  bx lr

.section .rdata,"dr"
.p2align 2
.globl pointer_slot
pointer_slot:
  .long data_target+4

.data
.p2align 2
.globl data_target
data_target:
  .long 0x11223344
  .long 0x55667788
)");
  ASSERT_FALSE(Object.empty());

  auto ImgOrErr = loadBinary(Object);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  ASSERT_EQ(Img.Format, BinaryFormat::COFF);
  ASSERT_EQ(Img.Arch, Arch::ARM);
  ASSERT_TRUE(Img.IsRelocatable);

  const Symbol *Function = findSymbol(Img, "materialize_data");
  const Symbol *NegativeFunction = findSymbol(Img, "materialize_negative");
  const Symbol *Slot = findSymbol(Img, "pointer_slot");
  const Symbol *Target = findSymbol(Img, "data_target");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(NegativeFunction, nullptr);
  ASSERT_NE(Slot, nullptr);
  ASSERT_NE(Target, nullptr);
  const Section *TargetOwner = Img.getSectionFor(Target->Addr);
  ASSERT_NE(TargetOwner, nullptr);

  const uint8_t *SlotBytes = Img.readVA(Slot->Addr, sizeof(uint32_t));
  ASSERT_NE(SlotBytes, nullptr);
  EXPECT_EQ(readLE<uint32_t>(SlotBytes),
            static_cast<uint32_t>(Target->Addr + 4));
  EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot->Addr), 1u);
  auto OwnerIt = Img.DataPtrRelocTargetOwners.find(Slot->Addr);
  ASSERT_NE(OwnerIt, Img.DataPtrRelocTargetOwners.end());
  EXPECT_EQ(OwnerIt->second, TargetOwner->VA);

  const uint8_t *MovBytes = Img.readVA(Function->Addr, 8);
  ASSERT_NE(MovBytes, nullptr);
  auto DecodeThumbImm16 = [](const uint8_t *Bytes) {
    const uint16_t Hi = readLE<uint16_t>(Bytes);
    const uint16_t Lo = readLE<uint16_t>(Bytes + 2);
    return static_cast<uint32_t>(((Hi & 0x000fu) << 12) |
                                 ((Hi & 0x0400u) << 1) | ((Lo & 0x7000u) >> 4) |
                                 (Lo & 0x00ffu));
  };
  const uint32_t Low = DecodeThumbImm16(MovBytes);
  const uint32_t High = DecodeThumbImm16(MovBytes + 4);
  EXPECT_EQ(Low | (High << 16), static_cast<uint32_t>(Target->Addr + 4));

  const uint8_t *NegativeBytes = Img.readVA(NegativeFunction->Addr, 8);
  ASSERT_NE(NegativeBytes, nullptr);
  const uint32_t NegativeAddress = DecodeThumbImm16(NegativeBytes) |
                                   (DecodeThumbImm16(NegativeBytes + 4) << 16);
  EXPECT_EQ(NegativeAddress, static_cast<uint32_t>(Target->Addr - 4));
}

TEST_F(COFFRelocatableAbsoluteRelocation,
       ELFARM32AppliesMOVWMovTAddressRelocations) {
  const fs::path Object =
      compileCOFF("elf_arm32_mov_pair", "armv7-none-linux-gnueabihf", R"(
.syntax unified
.arm
.text
.globl materialize_data
.type materialize_data,%function
materialize_data:
  movw r0, #:lower16:data_target+4
  movt r0, #:upper16:data_target+4
  bx lr

.globl materialize_negative
.type materialize_negative,%function
materialize_negative:
  movw r2, #:lower16:data_target-4
  movt r2, #:upper16:data_target-4
  bx lr

.thumb
.globl materialize_thumb
.thumb_func
.type materialize_thumb,%function
materialize_thumb:
  movw r1, #:lower16:data_target+4
  movt r1, #:upper16:data_target+4
  bx lr


.globl materialize_thumb_negative
.thumb_func
.type materialize_thumb_negative,%function
materialize_thumb_negative:
  movw r3, #:lower16:data_target-4
  movt r3, #:upper16:data_target-4
  bx lr

.section .rodata,"a",%progbits
.p2align 2
.globl pointer_slot
pointer_slot:
  .word data_target+4

.data
.p2align 2
.globl data_target
data_target:
  .word 0x11223344
  .word 0x55667788
)");
  ASSERT_FALSE(Object.empty());

  auto ImgOrErr = loadBinary(Object);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  ASSERT_EQ(Img.Format, BinaryFormat::ELF);
  ASSERT_EQ(Img.Arch, Arch::ARM);
  ASSERT_TRUE(Img.IsRelocatable);

  const Symbol *Function = findSymbol(Img, "materialize_data");
  const Symbol *NegativeFunction = findSymbol(Img, "materialize_negative");
  const Symbol *ThumbFunction = findSymbol(Img, "materialize_thumb");
  const Symbol *ThumbNegativeFunction =
      findSymbol(Img, "materialize_thumb_negative");
  const Symbol *Slot = findSymbol(Img, "pointer_slot");
  const Symbol *Target = findSymbol(Img, "data_target");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(NegativeFunction, nullptr);
  ASSERT_NE(ThumbFunction, nullptr);
  ASSERT_NE(ThumbNegativeFunction, nullptr);
  ASSERT_NE(Slot, nullptr);
  ASSERT_NE(Target, nullptr);

  const uint8_t *MovBytes = Img.readVA(Function->Addr, 8);
  const uint8_t *SlotBytes = Img.readVA(Slot->Addr, sizeof(uint32_t));
  ASSERT_NE(MovBytes, nullptr);
  ASSERT_NE(SlotBytes, nullptr);
  auto DecodeARMImm16 = [](const uint8_t *Bytes) {
    const uint32_t Insn = readLE<uint32_t>(Bytes);
    return ((Insn >> 4) & 0xf000u) | (Insn & 0x0fffu);
  };
  const uint32_t Address =
      DecodeARMImm16(MovBytes) | (DecodeARMImm16(MovBytes + 4) << 16);
  EXPECT_EQ(Address, static_cast<uint32_t>(Target->Addr + 4));
  const uint8_t *NegativeBytes = Img.readVA(NegativeFunction->Addr, 8);
  ASSERT_NE(NegativeBytes, nullptr);
  const uint32_t NegativeAddress =
      DecodeARMImm16(NegativeBytes) | (DecodeARMImm16(NegativeBytes + 4) << 16);
  EXPECT_EQ(NegativeAddress, static_cast<uint32_t>(Target->Addr - 4));

  const uint8_t *ThumbBytes = Img.readVA(ThumbFunction->Addr, 8);
  ASSERT_NE(ThumbBytes, nullptr);
  auto DecodeThumbImm16 = [](const uint8_t *Bytes) {
    const uint16_t Hi = readLE<uint16_t>(Bytes);
    const uint16_t Lo = readLE<uint16_t>(Bytes + 2);
    return static_cast<uint32_t>(((Hi & 0x000fu) << 12) |
                                 ((Hi & 0x0400u) << 1) | ((Lo & 0x7000u) >> 4) |
                                 (Lo & 0x00ffu));
  };
  const uint32_t ThumbAddress =
      DecodeThumbImm16(ThumbBytes) | (DecodeThumbImm16(ThumbBytes + 4) << 16);
  EXPECT_EQ(ThumbAddress, static_cast<uint32_t>(Target->Addr + 4));
  const uint8_t *ThumbNegativeBytes =
      Img.readVA(ThumbNegativeFunction->Addr, 8);
  ASSERT_NE(ThumbNegativeBytes, nullptr);
  const uint32_t ThumbNegativeAddress =
      DecodeThumbImm16(ThumbNegativeBytes) |
      (DecodeThumbImm16(ThumbNegativeBytes + 4) << 16);
  EXPECT_EQ(ThumbNegativeAddress, static_cast<uint32_t>(Target->Addr - 4));
  EXPECT_EQ(readLE<uint32_t>(SlotBytes),
            static_cast<uint32_t>(Target->Addr + 4));
  EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot->Addr), 1u);
}

TEST_F(COFFRelocatableAbsoluteRelocation,
       MachOARM32AppliesBranchAndHalfPairRelocations) {
  const fs::path Object =
      compileCOFF("macho_arm32_branch_half", "armv7-apple-darwin", R"(
.syntax unified
.arm
.section __TEXT,__text,regular,pure_instructions
.globl _caller
_caller:
  nop
  bl _callee
  bl _thumb_callee
  movw r0, :lower16:_data_target+4
  movt r0, :upper16:_data_target+4
  bx lr

.section __TEXT,__callee,regular,pure_instructions
.globl _callee
_callee:
  bx lr

.section __TEXT,__thumb_callee,regular,pure_instructions
.thumb
.thumb_func _thumb_callee
.globl _thumb_callee
_thumb_callee:
  bx lr

.section __DATA,__const
.p2align 2
.globl _pointer_slot
_pointer_slot:
  .long _data_target+4

.section __DATA,__data
.p2align 2
.globl _data_target
_data_target:
  .long 0x11223344
  .long 0x55667788
)");
  ASSERT_FALSE(Object.empty());

  auto ImgOrErr = loadBinary(Object);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  ASSERT_EQ(Img.Format, BinaryFormat::MachO);
  ASSERT_EQ(Img.Arch, Arch::ARM);
  ASSERT_TRUE(Img.IsRelocatable);

  const Symbol *Caller = findSymbol(Img, "_caller");
  const Symbol *Callee = findSymbol(Img, "_callee");
  const Symbol *ThumbCallee = findSymbol(Img, "_thumb_callee");
  const Symbol *Slot = findSymbol(Img, "_pointer_slot");
  const Symbol *Target = findSymbol(Img, "_data_target");
  ASSERT_NE(Caller, nullptr);
  ASSERT_NE(Callee, nullptr);
  ASSERT_NE(ThumbCallee, nullptr);
  ASSERT_NE(Slot, nullptr);
  ASSERT_NE(Target, nullptr);

  const uint8_t *Instructions = Img.readVA(Caller->Addr, 20);
  const uint8_t *SlotBytes = Img.readVA(Slot->Addr, sizeof(uint32_t));
  ASSERT_NE(Instructions, nullptr);
  ASSERT_NE(SlotBytes, nullptr);
  const uint32_t Branch = readLE<uint32_t>(Instructions + 4);
  int32_t BranchWords = static_cast<int32_t>(Branch & 0x00ffffffu);
  if ((BranchWords & 0x00800000) != 0)
    BranchWords |= ~0x00ffffff;
  EXPECT_EQ(static_cast<uint64_t>(static_cast<int64_t>(Caller->Addr) + 12 +
                                  static_cast<int64_t>(BranchWords) * 4),
            Callee->Addr);

  const uint32_t ThumbBranch = readLE<uint32_t>(Instructions + 8);
  EXPECT_EQ(ThumbBranch & 0xfe000000u, 0xfa000000u);
  uint32_t ThumbDisplacement =
      ((ThumbBranch & 0x00ffffffu) << 2) | ((ThumbBranch >> 23) & 0x2u);
  if ((ThumbDisplacement & 0x02000000u) != 0)
    ThumbDisplacement |= 0xfc000000u;
  EXPECT_EQ(static_cast<uint64_t>(static_cast<int64_t>(Caller->Addr) + 16 +
                                  static_cast<int32_t>(ThumbDisplacement)),
            ThumbCallee->Addr);

  auto DecodeARMImm16 = [](const uint8_t *Bytes) {
    const uint32_t Insn = readLE<uint32_t>(Bytes);
    return ((Insn >> 4) & 0xf000u) | (Insn & 0x0fffu);
  };
  const uint32_t Materialized = DecodeARMImm16(Instructions + 12) |
                                (DecodeARMImm16(Instructions + 16) << 16);
  EXPECT_EQ(Materialized, static_cast<uint32_t>(Target->Addr + 4));
  EXPECT_EQ(readLE<uint32_t>(SlotBytes),
            static_cast<uint32_t>(Target->Addr + 4));
  EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot->Addr), 1u);
  EXPECT_EQ(Img.WritableRelocDataAddrs.count(Target->Addr + 4), 1u);
}

} // namespace
