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

} // namespace
