//===- ELFARM32ModeTests.cpp - ELF ARM instruction-mode evidence ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"

#include <cstring>

namespace {

using namespace neverd;

#include "../fixtures/ARMUnmarkedModeFixtures.inc"

class ELFARM32ModeTest : public NeverDLiftTest {
protected:
  llvm::Expected<BinaryImage> loadAssembly(const std::string &Source,
                                           uint32_t StrippedEntry = 0) {
    const auto Assembly = tmpFile("mode.s");
    std::ofstream(Assembly) << Source;
    const auto Object = tmpFile("mode.o");
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", "armv7-linux-gnueabi", "-c",
                                 Assembly.string(), "-o", Object.string()});
    if (!Compiled.ok())
      return llvm::make_error<llvm::StringError>(
          Compiled.err, llvm::inconvertibleErrorCode());
    if (StrippedEntry != 0) {
      // Model a linked, stripped image: retain the actual instruction bytes,
      // assign the allocated code its linked VA, and remove symbol evidence.
      using namespace llvm::ELF;
      using ELFT = llvm::object::ELF32LE;
      std::ifstream Input(Object, std::ios::binary);
      std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
      Input.close();
      ELFT::Ehdr Header;
      std::memcpy(&Header, Bytes.data(), sizeof(Header));
      Header.e_type = ET_EXEC;
      Header.e_entry = StrippedEntry;
      std::memcpy(Bytes.data(), &Header, sizeof(Header));
      for (unsigned Index = 0; Index != Header.e_shnum; ++Index) {
        const auto Offset = Header.e_shoff + Index * Header.e_shentsize;
        ELFT::Shdr Section;
        std::memcpy(&Section, Bytes.data() + Offset, sizeof(Section));
        if (Section.sh_flags & SHF_ALLOC)
          Section.sh_addr = StrippedEntry & ~1u;
        if (Section.sh_type == SHT_SYMTAB || Section.sh_type == SHT_DYNSYM)
          Section.sh_type = SHT_NULL;
        std::memcpy(Bytes.data() + Offset, &Section, sizeof(Section));
      }
      std::ofstream Output(Object, std::ios::binary | std::ios::trunc);
      Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    }
    return ELFLoader().load(Object);
  }

  /// Links \p Source for ARM32 and removes its section table, the way a
  /// stripped image reaches the loader: only the entry and the code remain.
  llvm::Expected<BinaryImage>
  loadStrippedLinkedAssembly(const std::string &Name,
                             const std::string &Source) {
    const auto Assembly = tmpFile(Name + ".s");
    std::ofstream(Assembly) << Source;
    const auto Object = tmpFile(Name + ".o");
    const auto Linked = tmpFile(Name + "-linked.elf");
    const auto Stripped = tmpFile(Name + "-stripped.elf");
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", "armv7-linux-gnueabi", "-c",
                                 Assembly.string(), "-o", Object.string()});
    if (!Compiled.ok())
      return llvm::make_error<llvm::StringError>(
          Compiled.err, llvm::inconvertibleErrorCode());
    const auto LinkedOk =
        exec("ld.lld", {"-m", "armelf_linux_eabi", "-e", "_start",
                        Object.string(), "-o", Linked.string()});
    if (!LinkedOk.ok())
      return llvm::make_error<llvm::StringError>(
          LinkedOk.err, llvm::inconvertibleErrorCode());
    std::ifstream Input(Linked, std::ios::binary);
    std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
    llvm::object::ELF32LE::Ehdr Header;
    std::memcpy(&Header, Bytes.data(), sizeof(Header));
    Header.e_shoff = 0;
    Header.e_shnum = 0;
    Header.e_shentsize = 0;
    Header.e_shstrndx = llvm::ELF::SHN_UNDEF;
    std::memcpy(Bytes.data(), &Header, sizeof(Header));
    std::ofstream Output(Stripped, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Output.close();
    return ELFLoader().load(Stripped);
  }

  /// Links \p Source as an ARM32 shared library and strips its symbol table,
  /// as an Android JNI library ships: only the dynamic symbols, the PLT, and
  /// the imports remain.
  llvm::Expected<BinaryImage>
  loadStrippedSharedAssembly(const std::string &Name, const std::string &Source,
                             bool StripSections = false) {
    const auto Assembly = tmpFile(Name + ".s");
    std::ofstream(Assembly) << Source;
    const auto Object = tmpFile(Name + ".o");
    const auto Library = tmpFile(Name + ".so");
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", "armv7-linux-gnueabi", "-c",
                                 Assembly.string(), "-o", Object.string()});
    if (!Compiled.ok())
      return llvm::make_error<llvm::StringError>(
          Compiled.err, llvm::inconvertibleErrorCode());
    const auto LinkedOk =
        exec("ld.lld", {"-m", "armelf_linux_eabi", "-shared", "--strip-all",
                        Object.string(), "-o", Library.string()});
    if (!LinkedOk.ok())
      return llvm::make_error<llvm::StringError>(
          LinkedOk.err, llvm::inconvertibleErrorCode());
    if (StripSections) {
      std::ifstream Input(Library, std::ios::binary);
      std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
      Input.close();
      llvm::object::ELF32LE::Ehdr Header;
      std::memcpy(&Header, Bytes.data(), sizeof(Header));
      Header.e_shoff = 0;
      Header.e_shnum = 0;
      Header.e_shstrndx = 0;
      std::memcpy(Bytes.data(), &Header, sizeof(Header));
      std::ofstream Output(Library, std::ios::binary | std::ios::trunc);
      Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    }
    return ELFLoader().load(Library);
  }
};

TEST_F(ELFARM32ModeTest, SectionlessDynamicSymbolsRetainEachInstructionMode) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  const std::string Source = R"(
.syntax unified
.text
.arm
.p2align 2
.globl arm_add
.type arm_add,%function
arm_add:
  add r0,r0,#7
  bx lr
.size arm_add, .-arm_add
.thumb
.p2align 1
.globl thumb_add
.type thumb_add,%function
.thumb_func
thumb_add:
  adds r0,#9
  bx lr
.size thumb_add, .-thumb_add
)";
  auto Full = loadStrippedSharedAssembly("dynamic-full", Source);
  ASSERT_TRUE(bool(Full)) << llvm::toString(Full.takeError());
  auto Stripped =
      loadStrippedSharedAssembly("dynamic-sectionless", Source, true);
  ASSERT_TRUE(bool(Stripped)) << llvm::toString(Stripped.takeError());
  EXPECT_TRUE(Stripped->Sections.empty());
  EXPECT_EQ(Stripped->Mode, InstructionMode::MixedARMThumb);
  for (const auto &[Name, Mode] :
       {std::pair{"arm_add", InstructionMode::ARM},
        std::pair{"thumb_add", InstructionMode::Thumb}}) {
    SCOPED_TRACE(Name);
    const auto *Symbol = Stripped->findSymbol(Name);
    const auto *Original = Full->findSymbol(Name);
    ASSERT_NE(Symbol, nullptr);
    ASSERT_NE(Original, nullptr);
    EXPECT_EQ(Symbol->Addr, Original->Addr);
    EXPECT_EQ(Stripped->instructionModeAt(Symbol->Addr), Mode);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Symbol->Addr};
    const auto Result = Pipeline().run(*Stripped, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.HighFuncs.size(), 1u);
  }
}

TEST_F(ELFARM32ModeTest, PreservesThumbModeBeforeNormalizingFunctionAddresses) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.thumb
.globl add32
.type add32,%function
.thumb_func
add32:
  adds r0, r0, r1
  bx lr
.size add32, .-add32
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Mode, InstructionMode::Thumb);
  const auto Symbols = Image->getFunctionSymbols();
  ASSERT_EQ(Symbols.size(), 1u);
  EXPECT_EQ(Symbols.front()->Addr, 0u);
  EXPECT_EQ(Symbols.front()->Size, 4u);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image->Arch, Image->Mode));
  const auto *Segment = Image->getSegmentFor(Symbols.front()->Addr);
  ASSERT_NE(Segment, nullptr);
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOne(Segment->Data.data(), Segment->Data.size(), 0, Insn),
            2);
  EXPECT_STREQ(Insn.Raw->mnemonic, "adds");
}

TEST_F(ELFARM32ModeTest, ARMCodeDoesNotInheritModeFromOddDataOrArbitraryNames) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.globl add32
.type add32,%function
add32:
  add r0, r0, r1
  bx lr
.size add32, .-add32
.local $thing
$thing:
  nop
.data
.byte 0
.type odd_data,%object
odd_data:
  .byte 1
.local $t.data
$t.data:
  .byte 2
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Mode, InstructionMode::ARM);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image->Arch, Image->Mode));
  const auto *Segment = Image->getSegmentFor(0);
  ASSERT_NE(Segment, nullptr);
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOne(Segment->Data.data(), Segment->Data.size(), 0, Insn),
            4);
  EXPECT_STREQ(Insn.Raw->mnemonic, "add");
}

TEST_F(ELFARM32ModeTest, MappingSymbolRetainsThumbModeWithoutAFunctionSymbol) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.thumb
  adds r0, r0, r1
  bx lr
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Mode, InstructionMode::Thumb);
  EXPECT_TRUE(Image->getFunctionSymbols().empty());
}

TEST_F(ELFARM32ModeTest, EmbeddedDataHasNoInstructionMode) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.globl arm_before
.type arm_before,%function
arm_before:
  add r0, r0, r1
  bx lr
.size arm_before, .-arm_before
.word 0xe0800001
.thumb
.globl thumb_after
.type thumb_after,%function
.thumb_func
thumb_after:
  adds r0, r0, r1
  bx lr
.size thumb_after, .-thumb_after
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->instructionModeAt(0), InstructionMode::ARM);
  EXPECT_FALSE(Image->instructionModeAt(8));
  EXPECT_EQ(Image->instructionModeAt(12), InstructionMode::Thumb);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  EXPECT_FALSE(Dec.selectMode(*Image, 8, InstructionMode::ARM));
  EXPECT_TRUE(Dec.selectMode(*Image, 12));
  EXPECT_EQ(Dec.currentMode(), InstructionMode::Thumb);
}

TEST_F(ELFARM32ModeTest, LinkedEntryRetainsModeWithoutSymbolEvidence) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  for (bool Thumb : {false, true}) {
    SCOPED_TRACE(Thumb);
    auto Image = loadAssembly(std::string(".syntax unified\n.text\n") +
                                  (Thumb ? ".thumb\nadds r0, r0, r1\nbx lr\n"
                                         : ".arm\nadd r0, r0, r1\nbx lr\n"),
                              Thumb ? 0x1001 : 0x1000);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Mode,
              Thumb ? InstructionMode::Thumb : InstructionMode::ARM);
    EXPECT_EQ(Image->Entry, 0x1000u);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image->Arch, Image->Mode));
    const auto *Segment = Image->getSegmentFor(Image->Entry);
    ASSERT_NE(Segment, nullptr);
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOne(Segment->Data.data(), Segment->Data.size(),
                            Image->Entry, Insn),
              Thumb ? 2 : 4);
    EXPECT_STREQ(Insn.Raw->mnemonic, Thumb ? "adds" : "add");
  }
}

TEST_F(ELFARM32ModeTest, DirectEntryModeDoesNotCoverUnmarkedCode) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
  blx thumb_target
  bx lr
.thumb
.thumb_func
thumb_target:
  adds r0, r0, r1
  bx lr
)",
                            0x1000);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Mode, InstructionMode::ARM);
  EXPECT_EQ(Image->instructionModeAt(0x1000), InstructionMode::ARM);
  EXPECT_FALSE(Image->instructionModeAt(0x1000, InstructionMode::Thumb));
  EXPECT_FALSE(Image->instructionModeAt(0x1008));
  EXPECT_EQ(Image->instructionModeAt(0x1008, InstructionMode::Thumb),
            InstructionMode::Thumb);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  ASSERT_TRUE(Dec.selectMode(*Image, 0x1008, InstructionMode::Thumb));
  EXPECT_EQ(Dec.currentMode(), InstructionMode::Thumb);
}

TEST_F(ELFARM32ModeTest, UnmarkedObjectRequiresAnExactFunctionMode) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";

  for (const UnmarkedModeCase &Case : UnmarkedModeCases) {
    SCOPED_TRACE(Case.Name);
    const auto Assembly = tmpFile("unmarked-mode.s");
    std::ofstream(Assembly) << Case.Assembly;
    const auto Object = tmpFile("unmarked-mode.o");
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", "armv7-linux-gnueabi", "-c",
                                 Assembly.string(), "-o", Object.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err;

    // Remove the symbol table, including the assembler's ARM/Thumb mapping
    // symbols. The code and its executable section remain intact.
    std::ifstream Input(Object, std::ios::binary);
    std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
    using ELFT = llvm::object::ELF32LE;
    ELFT::Ehdr Header;
    ASSERT_GE(Bytes.size(), sizeof(Header));
    std::memcpy(&Header, Bytes.data(), sizeof(Header));
    bool Removed = false;
    for (unsigned Index = 0; Index != Header.e_shnum; ++Index) {
      const auto Offset = Header.e_shoff + Index * Header.e_shentsize;
      ASSERT_LE(Offset + Header.e_shentsize, Bytes.size());
      ASSERT_GE(Header.e_shentsize, sizeof(ELFT::Shdr));
      ELFT::Shdr Section;
      std::memcpy(&Section, Bytes.data() + Offset, sizeof(Section));
      if (Section.sh_type != llvm::ELF::SHT_SYMTAB)
        continue;
      std::memset(Bytes.data() + Offset, 0, Header.e_shentsize);
      Removed = true;
    }
    ASSERT_TRUE(Removed);
    std::ofstream Output(Object, std::ios::binary | std::ios::trunc);
    Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Output.close();

    auto Image = ELFLoader().load(Object);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_FALSE(Image->instructionModeAt(0));
    BinaryLoadOptions Options;
    Options.ARMFunctionModes[0] = Case.Mode;
    auto Asserted = loadBinary(Object, Options);
    ASSERT_TRUE(static_cast<bool>(Asserted))
        << llvm::toString(Asserted.takeError());
    EXPECT_EQ(Asserted->instructionModeAt(0), Options.ARMFunctionModes[0]);

    for (bool Symbolic : {false, true})
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(Symbolic ? "enhanced" : "ordinary");
        SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
        const auto CFile = tmpFile("unmarked-mode.c");
        std::vector<std::string> Args{"decompile", "--func=0"};
        if (LLVM)
          Args.push_back("--llvm");
        if (Symbolic)
          Args.push_back("--symbolic-simplify");
        Args.insert(Args.end(), {"-o", CFile.string(), Object.string()});
        const auto Unknown = exec(ndBin(), Args);
        EXPECT_FALSE(Unknown.ok());
        EXPECT_NE(Unknown.err.find("--arm-function-mode"), std::string::npos)
            << Unknown.err;

        Args.insert(Args.begin() + 2, Case.Hint);
        const auto Decompiled = exec(ndBin(), Args);
        ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
        std::ifstream SourceInput(CFile);
        const std::string Source(std::istreambuf_iterator<char>(SourceInput),
                                 {});
        ASSERT_NE(Source.find("sub_0("), std::string::npos) << Source;
        EXPECT_EQ(Source.find(" ^ ") == std::string::npos, Symbolic) << Source;
        EXPECT_EQ(Source.find(" & ") == std::string::npos, Symbolic) << Source;
        const auto Harness = tmpFile("unmarked-mode-execute.c");
        std::ofstream(Harness) << Source << UnmarkedModeExecutionHarness;
        for (const char *Optimization : {"-O0", "-O2"}) {
          SCOPED_TRACE(Optimization);
          const auto Executable = tmpFile("unmarked-mode-execute");
          const auto Recompiled =
              exec(NEVERD_TEST_CLANG,
                   {"-std=c11", Optimization, "-Werror=return-type",
                    "-Werror=implicit-function-declaration",
                    "-fsanitize=undefined", "-fsanitize-trap=undefined",
                    Harness.string(), "-o", Executable.string()});
          ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
          const auto Ran = exec(Executable.string(), {});
          EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
        }
      }
  }
}

TEST_F(ELFARM32ModeTest, RejectsConditionalCallRelocation) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM relocation fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.section .text.entry,"ax",%progbits
.arm
.globl cond_entry
.type cond_entry,%function
cond_entry:
  cmp r0, #0
call_site:
  .inst 0x1b000000
  bx lr
.reloc call_site, R_ARM_CALL, arm_leaf
.size cond_entry, .-cond_entry
.section .text.leaf,"ax",%progbits
.arm
.globl arm_leaf
.type arm_leaf,%function
arm_leaf:
  add r0, r0, #1
  bx lr
.size arm_leaf, .-arm_leaf
)");
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(
      llvm::toString(Image.takeError()).find("malformed ARM call relocation"),
      std::string::npos);
}

TEST_F(ELFARM32ModeTest, PreservesConditionalBranchRelocation) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM relocation fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.section .text.entry,"ax",%progbits
.arm
.globl cond_entry
.type cond_entry,%function
cond_entry:
  cmp r0, #0
  blne arm_leaf
  bx lr
.size cond_entry, .-cond_entry
.section .text.leaf,"ax",%progbits
.arm
.globl arm_leaf
.type arm_leaf,%function
arm_leaf:
  add r0, r0, #1
  bx lr
.size arm_leaf, .-arm_leaf
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const auto *Call = Image->readVA(4, sizeof(uint32_t));
  ASSERT_NE(Call, nullptr);
  uint32_t Encoding = 0;
  std::memcpy(&Encoding, Call, sizeof(Encoding));
  EXPECT_EQ(Encoding & 0xff000000u, 0x1b000000u);
}

TEST_F(ELFARM32ModeTest, FindsDirectInterworkingCallsWithoutSectionMetadata) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  const auto Assembly = tmpFile("stripped-interworking.s");
  std::ofstream(Assembly) << R"(
.syntax unified
.text
.arm
.globl arm_wrapper
.type arm_wrapper,%function
arm_wrapper:
  push {lr}
  blx thumb_leaf
  pop {pc}
.size arm_wrapper, .-arm_wrapper
.word 0xe0800001
.thumb
.globl thumb_leaf
.type thumb_leaf,%function
.thumb_func
thumb_leaf:
  push {lr}
  cbz r0, thumb_call
  adds r0, r0, #0
thumb_call:
  blx arm_leaf
  pop {pc}
.size thumb_leaf, .-thumb_leaf
.arm
.p2align 2
.globl arm_leaf
.type arm_leaf,%function
arm_leaf:
  eor r2, r0, r1
  and r3, r0, r1
  add r0, r2, r3, lsl #1
  bx lr
.size arm_leaf, .-arm_leaf
)";
  const auto Object = tmpFile("stripped-interworking.o");
  const auto Linked = tmpFile("linked-interworking.elf");
  const auto Stripped = tmpFile("stripped-interworking.elf");
  ASSERT_TRUE(
      exec(NEVERD_TEST_CLANG, {"-target", "armv7-linux-gnueabi", "-c",
                               Assembly.string(), "-o", Object.string()})
          .ok());
  ASSERT_TRUE(exec("ld.lld", {"-m", "armelf_linux_eabi", "-e", "arm_wrapper",
                              Object.string(), "-o", Linked.string()})
                  .ok());

  auto LinkedImage = ELFLoader().load(Linked);
  ASSERT_TRUE(static_cast<bool>(LinkedImage))
      << llvm::toString(LinkedImage.takeError());
  const auto Functions = LinkedImage->getFunctionSymbols();
  const auto Leaf = std::find_if(
      Functions.begin(), Functions.end(),
      [](const Symbol *Function) { return Function->Name == "thumb_leaf"; });
  ASSERT_NE(Leaf, Functions.end());
  const va_t LeafVA = (*Leaf)->Addr;
  const auto ARMLeaf = std::find_if(
      Functions.begin(), Functions.end(),
      [](const Symbol *Function) { return Function->Name == "arm_leaf"; });
  ASSERT_NE(ARMLeaf, Functions.end());
  const va_t ARMLeafVA = (*ARMLeaf)->Addr;
  const auto Wrapper = std::find_if(
      Functions.begin(), Functions.end(),
      [](const Symbol *Function) { return Function->Name == "arm_wrapper"; });
  ASSERT_NE(Wrapper, Functions.end());

  std::ifstream Input(Linked, std::ios::binary);
  std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
  llvm::object::ELF32LE::Ehdr Header;
  ASSERT_GE(Bytes.size(), sizeof(Header));
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  Header.e_shoff = 0;
  Header.e_shnum = 0;
  Header.e_shentsize = 0;
  Header.e_shstrndx = llvm::ELF::SHN_UNDEF;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::ofstream Output(Stripped, std::ios::binary);
  Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  Output.close();

  auto Image = ELFLoader().load(Stripped);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->Sections.empty());
  EXPECT_EQ(Image->Mode, InstructionMode::MixedARMThumb);
  EXPECT_EQ(Image->instructionModeAt(LeafVA), InstructionMode::Thumb);
  EXPECT_EQ(Image->instructionModeAt(LeafVA + 4), InstructionMode::Thumb);
  EXPECT_EQ(Image->instructionModeAt(ARMLeafVA), InstructionMode::ARM);
  EXPECT_FALSE(Image->instructionModeAt((*Wrapper)->Addr + 12));
  const auto Lifted = exec(ndBin(), {"lift", "--dump-low", Stripped.string()});
  ASSERT_TRUE(Lifted.ok()) << Lifted.err;
  EXPECT_NE(Lifted.out.find(" @ 0x" + llvm::utohexstr(LeafVA)),
            std::string::npos)
      << Lifted.out;
  EXPECT_NE(Lifted.out.find(" @ 0x" + llvm::utohexstr(ARMLeafVA)),
            std::string::npos)
      << Lifted.out;
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
    const auto CFile = tmpFile(LLVM ? "stripped-llvm.c" : "stripped-high.c");
    std::vector<std::string> Arguments{"decompile"};
    if (LLVM)
      Arguments.push_back("--llvm");
    Arguments.insert(Arguments.end(),
                     {"-o", CFile.string(), Stripped.string()});
    const auto Decompiled = exec(ndBin(), Arguments);
    ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
    std::ifstream Input(CFile);
    const std::string Source(std::istreambuf_iterator<char>(Input), {});
    for (va_t FunctionVA : {(*Wrapper)->Addr, LeafVA, ARMLeafVA})
      EXPECT_NE(Source.find("sub_" + llvm::utohexstr(FunctionVA) + "("),
                std::string::npos)
          << Source;
    EXPECT_EQ(Source.find("/* unknown"), std::string::npos) << Source;
    const auto LeafDecl =
        Source.rfind("sub_" + llvm::utohexstr(ARMLeafVA) + "(");
    ASSERT_NE(LeafDecl, std::string::npos) << Source;
    const auto LeafOpen = Source.find('{', LeafDecl);
    ASSERT_NE(LeafOpen, std::string::npos) << Source;
    const auto LeafClose = Source.find('}', LeafOpen);
    ASSERT_NE(LeafClose, std::string::npos) << Source;
    const auto LeafBody = Source.substr(LeafOpen, LeafClose - LeafOpen);
    EXPECT_NE(LeafBody.find('+'), std::string::npos) << LeafBody;
    EXPECT_NE(LeafBody.find('^'), std::string::npos) << LeafBody;
    EXPECT_NE(LeafBody.find('&'), std::string::npos) << LeafBody;
    {
      std::ofstream Append(CFile, std::ios::app);
      Append << "\nint main(void) { return sub_"
             << llvm::utohexstr((*Wrapper)->Addr) << "(7, 5) == 12 && sub_"
             << llvm::utohexstr((*Wrapper)->Addr)
             << "(0xffffffffu, 2) == 1 ? 0 : 1; }\n";
    }
    const auto Executable = tmpFile(LLVM ? "stripped-llvm" : "stripped-high");
    const auto Compiled = exec(
        NEVERD_TEST_CLANG, {"-std=c11", "-Werror=implicit-function-declaration",
                            "-O0", CFile.string(), "-o", Executable.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
    EXPECT_TRUE(exec(Executable.string(), {}).ok());

    // Explicit enhancement must retain the same ARM/Thumb mode recovery.
    Arguments.insert(
        Arguments.begin() + 1,
        {"--func=0x" + llvm::utohexstr(ARMLeafVA), "--symbolic-simplify"});
    const auto Enhanced = exec(ndBin(), Arguments);
    ASSERT_TRUE(Enhanced.ok()) << Enhanced.err;
    std::ifstream EnhancedInput(CFile);
    const std::string EnhancedSource(
        std::istreambuf_iterator<char>(EnhancedInput), {});
    EXPECT_EQ(EnhancedSource.find('^'), std::string::npos) << EnhancedSource;
    EXPECT_EQ(EnhancedSource.find('&'), std::string::npos) << EnhancedSource;
    {
      std::ofstream Append(CFile, std::ios::app);
      Append << "\nint main(void) { return sub_" << llvm::utohexstr(ARMLeafVA)
             << "(7, 5) == 12 && sub_" << llvm::utohexstr(ARMLeafVA)
             << "(0xffffffffu, 2) == 1 ? 0 : 1; }\n";
    }
    const auto EnhancedCompiled = exec(
        NEVERD_TEST_CLANG, {"-std=c11", "-Werror=implicit-function-declaration",
                            "-O0", CFile.string(), "-o", Executable.string()});
    ASSERT_TRUE(EnhancedCompiled.ok()) << EnhancedCompiled.err;
    EXPECT_TRUE(exec(Executable.string(), {}).ok());
  }

  const auto Patched = tmpFile("stripped-patched.elf");
  const auto Patch = exec(ndBin(), {"patch", "--mode=section", "-o",
                                    Patched.string(), Stripped.string()});
  ASSERT_TRUE(Patch.ok()) << Patch.err;
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "patched LLVMC" : "patched HighC");
    const auto CFile =
        tmpFile(LLVM ? "patched-stripped-llvm.c" : "patched-stripped-high.c");
    std::vector<std::string> Arguments{"decompile"};
    if (LLVM)
      Arguments.push_back("--llvm");
    Arguments.insert(Arguments.end(), {"-o", CFile.string(), Patched.string()});
    const auto Decompiled = exec(ndBin(), Arguments);
    ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
    std::ifstream Input(CFile);
    const std::string Source(std::istreambuf_iterator<char>(Input), {});
    EXPECT_EQ(Source.find("/* unknown"), std::string::npos) << Source;
    {
      std::ofstream Append(CFile, std::ios::app);
      Append << "\nint main(void) { return sub_"
             << llvm::utohexstr((*Wrapper)->Addr) << "(7, 5) == 12 && sub_"
             << llvm::utohexstr((*Wrapper)->Addr)
             << "(0xffffffffu, 2) == 1 ? 0 : 1; }\n";
    }
    const auto Executable =
        tmpFile(LLVM ? "patched-stripped-llvm" : "patched-stripped-high");
    const auto Compiled = exec(
        NEVERD_TEST_CLANG, {"-std=c11", "-Werror=implicit-function-declaration",
                            "-O0", CFile.string(), "-o", Executable.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
    EXPECT_TRUE(exec(Executable.string(), {}).ok());
  }

  // A mapped original can still gain generated executable bytes beyond every
  // section. Re-load those bytes and compile both C views of the full image.
  const auto MappedPatched = tmpFile("mapped-patched.elf");
  const auto MappedPatch =
      exec(ndBin(), {"patch", "--mode=section", "-o", MappedPatched.string(),
                     Linked.string()});
  ASSERT_TRUE(MappedPatch.ok()) << MappedPatch.err;
  auto MappedImage = ELFLoader().load(MappedPatched);
  ASSERT_TRUE(static_cast<bool>(MappedImage))
      << llvm::toString(MappedImage.takeError());
  EXPECT_EQ(MappedImage->Mode, InstructionMode::MixedARMThumb);
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "mapped LLVMC" : "mapped HighC");
    const auto CFile =
        tmpFile(LLVM ? "mapped-patched-llvm.c" : "mapped-patched-high.c");
    std::vector<std::string> Arguments{"decompile"};
    if (LLVM)
      Arguments.push_back("--llvm");
    Arguments.insert(Arguments.end(),
                     {"-o", CFile.string(), MappedPatched.string()});
    const auto Decompiled = exec(ndBin(), Arguments);
    ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
    std::ifstream Input(CFile);
    const std::string Source(std::istreambuf_iterator<char>(Input), {});
    EXPECT_EQ(Source.find("/* unknown"), std::string::npos) << Source;
    {
      std::ofstream Append(CFile, std::ios::app);
      Append << "\nint main(void) { return arm_wrapper(7, 5) == 12 && "
                "thumb_leaf(7, 5) == 12 && arm_leaf(7, 5) == 12 && "
                "arm_wrapper(0xffffffffu, 2) == 1 "
                "? 0 : 1; }\n";
    }
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Executable = tmpFile(
          std::string(LLVM ? "mapped-llvm" : "mapped-high") + Optimization);
      const auto Compiled =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", "-Werror=implicit-function-declaration",
                Optimization, CFile.string(), "-o", Executable.string()});
      ASSERT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
      EXPECT_TRUE(exec(Executable.string(), {}).ok());
    }
  }
}

TEST_F(ELFARM32ModeTest, AlignedThumbFrameEscapesToARMCall) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  const auto Assembly = tmpFile("aligned-frame.s");
  std::ofstream(Assembly) << R"(
.syntax unified
.text
.arm
.p2align 2
.globl arm_load
.type arm_load,%function
arm_load:
  ldr r0, [r0]
  bx lr
.size arm_load, .-arm_load
.thumb
.p2align 1
.globl align_wrapper
.type align_wrapper,%function
.thumb_func
align_wrapper:
  push {r4,lr}
  sub sp, sp, #32
  mov r4, sp
  bfc r4, #0, #4
  str r0, [r4, #12]
  add r0, r4, #12
  blx arm_load
  add sp, sp, #32
  pop {r4,pc}
.size align_wrapper, .-align_wrapper
)";
  const auto Object = tmpFile("aligned-frame.o");
  const auto Linked = tmpFile("aligned-frame.elf");
  ASSERT_TRUE(
      exec(NEVERD_TEST_CLANG, {"-target", "armv7-linux-gnueabi", "-c",
                               Assembly.string(), "-o", Object.string()})
          .ok());
  ASSERT_TRUE(exec("ld.lld", {"-m", "armelf_linux_eabi", "-e", "align_wrapper",
                              Object.string(), "-o", Linked.string()})
                  .ok());

  const auto Patched = tmpFile("aligned-frame-patched.elf");
  const auto Patch = exec(ndBin(), {"patch", "--mode=section", "-o",
                                    Patched.string(), Linked.string()});
  ASSERT_TRUE(Patch.ok()) << Patch.err;
  for (const auto &Image : {Linked, Patched}) {
    SCOPED_TRACE(Image.string());
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      const auto CFile =
          tmpFile(LLVM ? "aligned-frame-llvm.c" : "aligned-frame-high.c");
      std::vector<std::string> Arguments{"decompile"};
      if (LLVM)
        Arguments.push_back("--llvm");
      Arguments.insert(Arguments.end(), {"-o", CFile.string(), Image.string()});
      const auto Decompiled = exec(ndBin(), Arguments);
      ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
      std::ifstream Input(CFile);
      const std::string Source(std::istreambuf_iterator<char>(Input), {});
      EXPECT_EQ(Source.find("/* unknown"), std::string::npos) << Source;
      {
        std::ofstream Append(CFile, std::ios::app);
        Append << "\nint main(void) { return align_wrapper(17) == 17 && "
                  "align_wrapper(0x12345678) == 0x12345678 ? 0 : 1; }\n";
      }
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        const auto Executable = tmpFile(
            std::string(LLVM ? "aligned-llvm" : "aligned-high") + Optimization);
        const auto Compiled =
            exec(NEVERD_TEST_CLANG,
                 {"-std=c11", "-Werror=implicit-function-declaration",
                  Optimization, CFile.string(), "-o", Executable.string()});
        ASSERT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
        EXPECT_TRUE(exec(Executable.string(), {}).ok());
      }
    }
  }
}

TEST_F(ELFARM32ModeTest, PointerRoleFollowsOnlyExactEntryForwarding) {
  const uint64_t R0 =
      getTargetRegInfo(Arch::ARM).integerArgumentLayout(false).Registers[0];
  MedVar Entry;
  Entry.Kind = MedVar::Param;
  Entry.Id = -1;
  Entry.RegOff = R0;
  Entry.Size = 4;
  auto MakeFunction = [&](va_t Address) {
    MedFunc Func;
    Func.Entry = Address;
    Func.Params.push_back(Entry);
    return Func;
  };
  auto AddForward = [&](MedFunc &Func, va_t Target, MedVar Arg) {
    MedCallInfo Call;
    Call.TargetAddr = Target;
    Call.Args.push_back(Arg);
    Func.CallInfos.push_back(std::move(Call));
  };

  MedFunc Outer = MakeFunction(0x1000);
  AddForward(Outer, 0x2000, Entry);
  MedFunc Middle = MakeFunction(0x2000);
  AddForward(Middle, 0x3000, Entry);
  MedFunc Leaf = MakeFunction(0x3000);
  MedBlock Body;
  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.NumInputs = 1;
  Load.Inputs[0] = Entry;
  Body.Ops.push_back(Load);
  Leaf.Blocks.push_back(std::move(Body));
  MedFunc Redefined = MakeFunction(0x4000);
  MedVar Later = Entry;
  Later.Kind = MedVar::Reg;
  Later.SSAVer = 1;
  AddForward(Redefined, 0x3000, Later);

  std::vector<MedFunc> Functions{Outer, Middle, Leaf, Redefined};
  propagateForwardedPointerParams(Functions, Arch::ARM);
  for (size_t I : {0u, 1u, 2u}) {
    ASSERT_EQ(Functions[I].TypedParams.size(), 1u);
    ASSERT_TRUE(Functions[I].TypedParams[0].Type);
    EXPECT_EQ(Functions[I].TypedParams[0].Type->Kind, NdTypeKind::Ptr);
  }
  ASSERT_EQ(Functions[3].TypedParams.size(), 1u);
  ASSERT_TRUE(Functions[3].TypedParams[0].Type);
  EXPECT_EQ(Functions[3].TypedParams[0].Type->Kind, NdTypeKind::Int);
}

TEST_F(ELFARM32ModeTest, DoesNotAssignModeToUnreachedBytesInStrippedELF) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  const auto Assembly = tmpFile("unreached.s");
  std::ofstream(Assembly) << R"(
.syntax unified
.text
.arm
.globl arm_entry
.type arm_entry,%function
arm_entry:
  add r0, r0, r1
  bx lr
.size arm_entry, .-arm_entry
.word 0xe0800001
)";
  const auto Object = tmpFile("unreached.o");
  const auto Linked = tmpFile("unreached-linked.elf");
  const auto Stripped = tmpFile("unreached-stripped.elf");
  ASSERT_TRUE(
      exec(NEVERD_TEST_CLANG, {"-target", "armv7-linux-gnueabi", "-c",
                               Assembly.string(), "-o", Object.string()})
          .ok());
  ASSERT_TRUE(exec("ld.lld", {"-m", "armelf_linux_eabi", "-e", "arm_entry",
                              Object.string(), "-o", Linked.string()})
                  .ok());
  std::ifstream Input(Linked, std::ios::binary);
  std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
  llvm::object::ELF32LE::Ehdr Header;
  ASSERT_GE(Bytes.size(), sizeof(Header));
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  Header.e_shoff = 0;
  Header.e_shnum = 0;
  Header.e_shentsize = 0;
  Header.e_shstrndx = llvm::ELF::SHN_UNDEF;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::ofstream Output(Stripped, std::ios::binary);
  Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  Output.close();

  auto Image = ELFLoader().load(Stripped);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Mode, InstructionMode::ARM);
  EXPECT_EQ(Image->instructionModeAt(Image->Entry), InstructionMode::ARM);
  EXPECT_FALSE(Image->instructionModeAt(Image->Entry + 8));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  EXPECT_FALSE(Dec.selectMode(*Image, Image->Entry + 8));
}

// After a call that does not return comes the caller's literal pool, which
// its own PC-relative load proves is data. Decoded as Thumb, this pool word
// opens a 32-bit instruction across the next function's first one.
TEST_F(ELFARM32ModeTest, StopsAtTheLiteralPoolAfterACallThatDoesNotReturn) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedLinkedAssembly("pool", R"(
.syntax unified
.text
.thumb
.globl _start
.type _start,%function
.thumb_func
_start:
  push {r4, lr}
  bl next
  ldr r0, pool
  bl stop
  nop
.p2align 2
pool:
  .word 0xfffb0000
.thumb_func
next:
  adds r0, r0, #1
  bx lr
.thumb_func
stop:
  b stop
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = Image->Entry & ~va_t(1);
  // push, bl next, ldr, bl stop, nop: the pool starts 14 bytes in, aligned.
  const va_t Pool = Entry + 16;
  EXPECT_FALSE(Image->instructionModeAt(Pool));
  EXPECT_EQ(Image->instructionModeAt(Pool + 4), InstructionMode::Thumb);
}

TEST_F(ELFARM32ModeTest, StillRejectsACallIntoTheMiddleOfAnInstruction) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedLinkedAssembly("middle", R"(
.syntax unified
.text
.thumb
.globl _start
.type _start,%function
.thumb_func
_start:
  push {r4, lr}
  bl inner
  bl whole
  pop {r4, pc}
.thumb_func
whole:
  ldr.w r0, [r1, #4]
  bx lr
.set inner, whole + 2
)");
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Message = llvm::toString(Image.takeError());
  EXPECT_NE(Message.find("overlaps the instruction at"), std::string::npos)
      << Message;
}

// A routine that ends in exit_group never returns, so what follows a call
// to it is not the caller's: here, a Thumb routine the caller also calls.
TEST_F(ELFARM32ModeTest, StopsAfterACallToARoutineThatExits) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedLinkedAssembly("exits", R"(
.syntax unified
.text
.arm
.globl _start
.type _start,%function
_start:
  blx helper
  bl die
.thumb
.thumb_func
helper:
  adds r0, r0, #1
  bx lr
.arm
die:
  ldr r7, =248
  svc #0
  mov r0, r0
.ltorg
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = Image->Entry & ~va_t(1);
  const va_t Helper = Entry + 8, Die = Entry + 12;
  // `helper` returns, so the caller goes on to call `die`.
  EXPECT_EQ(Image->instructionModeAt(Entry + 4), InstructionMode::ARM);
  EXPECT_EQ(Image->instructionModeAt(Helper), InstructionMode::Thumb);
  EXPECT_EQ(Image->instructionModeAt(Helper + 2), InstructionMode::Thumb);
  EXPECT_EQ(Image->instructionModeAt(Die), InstructionMode::ARM);
  EXPECT_EQ(Image->instructionModeAt(Die + 4), InstructionMode::ARM);
  // The instruction after the exiting system call is never reached.
  EXPECT_FALSE(Image->instructionModeAt(Die + 8));
}

// Nor does a system call the path cannot prove is exit end it: a routine
// that may take either branch returns.
TEST_F(ELFARM32ModeTest, FollowsACallToARoutineThatMayReturn) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedLinkedAssembly("returns", R"(
.syntax unified
.text
.arm
.globl _start
.type _start,%function
_start:
  bl maybe
  mov r0, #1
  bl other
  b _start
maybe:
  cmp r0, #0
  bxeq lr
  mov r7, #248
  svc #0
other:
  mov r7, #20
  svc #0
  bx lr
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = Image->Entry & ~va_t(1);
  EXPECT_EQ(Image->instructionModeAt(Entry + 4), InstructionMode::ARM);
  EXPECT_EQ(Image->instructionModeAt(Entry + 12), InstructionMode::ARM);
  // getpid returns, and so does the routine that makes it.
  EXPECT_EQ(Image->instructionModeAt(Entry + 40), InstructionMode::ARM);
}

// A trap does not go on either, and a call that runs only under a condition
// always may.
TEST_F(ELFARM32ModeTest, StopsAtATrapButNotAfterAConditionalCall) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedLinkedAssembly("trap", R"(
.syntax unified
.text
.thumb
.globl _start
.type _start,%function
.thumb_func
_start:
  cmp r0, #0
  it eq
  bleq die
  blx arm_part
  udf #254
.arm
.p2align 2
arm_part:
  add r0, r0, #1
  bx lr
die:
  mov r7, #1
  svc #0
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const va_t Entry = Image->Entry & ~va_t(1);
  // cmp, it, bleq (4), blx (4), udf: the call to `die` falls through, the
  // trap does not, and the padding after it is nobody's.
  EXPECT_EQ(Image->instructionModeAt(Entry + 8), InstructionMode::Thumb);
  EXPECT_EQ(Image->instructionModeAt(Entry + 12), InstructionMode::Thumb);
  EXPECT_FALSE(Image->instructionModeAt(Entry + 14));
  EXPECT_EQ(Image->instructionModeAt(Entry + 16), InstructionMode::ARM);
}

// An Android library calls abort through a PLT veneer in ARM code, and its
// Thumb caller is followed by an ARM routine.  The image has both modes, so
// its PLT is not paired with the imports; the veneer's own instructions
// still say which import it forwards to.
TEST_F(ELFARM32ModeTest, StopsAfterAVeneerToAnImportThatDoesNotReturn) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedSharedAssembly("import", R"(
.syntax unified
.text
.thumb
.globl caller
.type caller,%function
.thumb_func
caller:
  push {r4, lr}
  blx callee
  bl abort
.arm
.p2align 2
.globl callee
.type callee,%function
callee:
  add r0, r0, #1
  bx lr
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Mode, InstructionMode::MixedARMThumb);
}

// A mixed ARM/Thumb image pairs its ARM PLT with the imports as a
// single-mode one does: a Thumb caller's `blx` reaches the same stub, and
// without the pairing each of its import calls read as an unnamed routine.
TEST_F(ELFARM32ModeTest, MixedImagePairsItsPLTWithTheImports) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedSharedAssembly("mixed_plt", R"(
.syntax unified
.text
.thumb
.globl caller
.type caller,%function
.thumb_func
caller:
  push {r4, lr}
  bl puts
  pop {r4, pc}
.arm
.p2align 2
.globl arm_side
.type arm_side,%function
arm_side:
  bx lr
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Mode, InstructionMode::MixedARMThumb);
  std::vector<va_t> Stubs;
  for (const auto &[Stub, Import] : Image->ImportStubIndices)
    if (Image->Imports[Import].Name == "puts")
      Stubs.push_back(Stub);
  ASSERT_EQ(Stubs.size(), 1u);
  const Section *Plt = Image->getSectionFor(Stubs.front());
  ASSERT_NE(Plt, nullptr);
  EXPECT_EQ(Plt->Name, ".plt");
}

// A Thumb tail call to an import enters the ARM PLT through the thunk lld
// writes for it (`__ThumbV7PILongThunk_puts`, a symbol the stripped library
// no longer has), which stands for the import as the PLT entry does.
// A stripped library keeps no symbol for its own routines, but the code
// pointers it stores still carry bit 0: the dynamic linker calls the init
// array's entries as `blx` does, and a relocated slot with bit 0 set holds
// a Thumb code address.  What they address is decoded in that mode; what an
// untagged slot addresses may be data and stays without one.
TEST_F(ELFARM32ModeTest, StoredCodePointersStateTheModeOfStrippedRoutines) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedSharedAssembly("code_pointers", R"(
.syntax unified
.text
.thumb
.type through_slot,%function
.thumb_func
through_slot:
  adds r0, r0, #1
  adds r0, r0, #2
  bx lr
.thumb
.type thumb_constructor,%function
.thumb_func
thumb_constructor:
  movs r0, #1
  bx lr
.arm
.p2align 2
.type arm_constructor,%function
arm_constructor:
  mov r0, #0
  add r0, r0, #1
  bx lr
.arm
.p2align 2
.type untagged,%function
untagged:
  mov r0, #2
  bx lr
.section .init_array,"aw",%init_array
.p2align 2
  .word arm_constructor
  .word thumb_constructor
.data
.p2align 2
  .word through_slot
  .word untagged
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_TRUE(Image->LoadDiagnostics.empty());
  ASSERT_EQ(Image->DynInfo.InitArray.size(), 2u);
  const va_t ARMConstructor = Image->DynInfo.InitArray[0];
  const va_t ThumbConstructor = Image->DynInfo.InitArray[1];
  const Section *Data = nullptr;
  for (const Section &Sec : Image->Sections)
    if (Sec.Name == ".data")
      Data = &Sec;
  ASSERT_NE(Data, nullptr);
  const uint8_t *Slots = Image->readVA(Data->VA, 8);
  ASSERT_NE(Slots, nullptr);
  const uint32_t TaggedSlot = readLE<uint32_t>(Slots);
  const uint32_t UntaggedSlot = readLE<uint32_t>(Slots + 4);
  ASSERT_EQ(TaggedSlot & 1u, 1u);
  ASSERT_EQ(UntaggedSlot & 1u, 0u);
  const va_t ThroughSlot = TaggedSlot & ~1u;

  EXPECT_EQ(Image->instructionModeAt(ThroughSlot), InstructionMode::Thumb);
  EXPECT_EQ(Image->instructionModeAt(ThroughSlot + 2), InstructionMode::Thumb);
  EXPECT_EQ(Image->instructionModeAt(ThumbConstructor), InstructionMode::Thumb);
  EXPECT_EQ(Image->instructionModeAt(ARMConstructor), InstructionMode::ARM);
  EXPECT_EQ(Image->instructionModeAt(ARMConstructor + 4), InstructionMode::ARM);
  EXPECT_FALSE(Image->instructionModeAt(UntaggedSlot));
}

// The exception index names where each function starts, and a function's
// address has bit 0 set exactly when it is Thumb code: a stored value with
// bit 0 clear at an indexed start addresses ARM code.  One that names code
// inside an index range but no start of it still states nothing, as one
// does where no index exists.
TEST_F(ELFARM32ModeTest, StoredPointersAtIndexedStartsStateARMCode) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedSharedAssembly("indexed_starts", R"(
.syntax unified
.text
.arm
.p2align 2
.type indexed,%function
indexed:
  .fnstart
  mov r0, #3
  add r0, r0, #4
  bx lr
  .cantunwind
  .fnend
.p2align 2
.type unindexed,%function
unindexed:
  mov r0, #5
  add r0, r0, #6
  bx lr
.data
.p2align 2
  .word indexed
  .word unindexed
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_TRUE(Image->LoadDiagnostics.empty());
  const Section *Data = nullptr;
  for (const Section &Sec : Image->Sections)
    if (Sec.Name == ".data")
      Data = &Sec;
  ASSERT_NE(Data, nullptr);
  const uint8_t *Slots = Image->readVA(Data->VA, 8);
  ASSERT_NE(Slots, nullptr);
  const va_t Indexed = readLE<uint32_t>(Slots);
  const va_t Unindexed = readLE<uint32_t>(Slots + 4);
  ASSERT_EQ(Indexed & 1u, 0u);
  ASSERT_EQ(Unindexed & 1u, 0u);
  const auto StartsAt = [&](va_t Address) {
    return llvm::any_of(Image->ExceptionMetadata.Functions,
                        [&](const ExceptionFunction &Function) {
                          return Function.Kind ==
                                     RuntimeFunctionKind::Primary &&
                                 Function.CodeRange.Begin == Address;
                        });
  };
  ASSERT_TRUE(StartsAt(Indexed));
  ASSERT_FALSE(StartsAt(Unindexed));

  EXPECT_EQ(Image->instructionModeAt(Indexed), InstructionMode::ARM);
  EXPECT_EQ(Image->instructionModeAt(Indexed + 4), InstructionMode::ARM);
  EXPECT_FALSE(Image->instructionModeAt(Unindexed));
}

// A function no evidence gives a mode stays out of a whole unit, named once
// at its head with why; asking for that function alone still fails.
TEST_F(ELFARM32ModeTest, AWholeUnitNamesAFunctionWithoutAMode) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedSharedAssembly("unknown_mode_unit", R"(
.syntax unified
.text
.thumb
.globl exported
.type exported,%function
.thumb_func
exported:
  movs r0, #1
  bx lr
.arm
.p2align 2
.type lone,%function
lone:
  .fnstart
  mov r0, #2
  bx lr
  .cantunwind
  .fnend
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  // lld closes the index with an empty entry at the end of the code.
  std::optional<va_t> Lone;
  for (const ExceptionFunction &Function : Image->ExceptionMetadata.Functions)
    if (!Lone && Function.Kind == RuntimeFunctionKind::Primary &&
        Function.ParseStatus != ExceptionParseStatus::Malformed &&
        !Image->instructionModeAt(Function.CodeRange.Begin))
      Lone = Function.CodeRange.Begin;
  ASSERT_TRUE(Lone.has_value());
  const std::string Library = tmpFile("unknown_mode_unit.so").string();
  const std::string Named = " at 0x" + llvm::utohexstr(*Lone) +
                            ": ARM/Thumb mode cannot be established";

  const auto Unit = exec(ndBin(), {"decompile", Library});
  ASSERT_TRUE(Unit.ok()) << Unit.err;
  const size_t Note = Unit.out.find(Named);
  ASSERT_NE(Note, std::string::npos) << Unit.out;
  EXPECT_EQ(Unit.out.find(" at 0x" + llvm::utohexstr(*Lone) + ":", Note + 1),
            std::string::npos)
      << Unit.out;
  EXPECT_NE(Unit.out.find("exported("), std::string::npos) << Unit.out;

  const auto Alone = exec(
      ndBin(), {"decompile", Library, "--func=0x" + llvm::utohexstr(*Lone)});
  EXPECT_FALSE(Alone.ok());
  EXPECT_NE(Alone.err.find("ARM/Thumb mode cannot be established"),
            std::string::npos)
      << Alone.err;
}

// Position-independent Thumb code takes a routine's address as
// `ldr rN, [pc, #k]; add rN, pc`, the literal being the routine's address
// with bit 0 set less the PC: what it gets is a Thumb code address, as a
// stored one is.
TEST_F(ELFARM32ModeTest, AddressTheCodeTakesStatesTheModeOfItsRoutine) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedSharedAssembly("taken_address", R"(
.syntax unified
.text
.thumb
.globl taker
.type taker,%function
.thumb_func
taker:
  ldr.n r0, .Lliteral
.Lpc:
  add r0, pc
  bx lr
.p2align 2
.Lliteral:
  .word callback + 1 - (.Lpc + 4)
callback:
  movs r0, #7
  adds r0, r0, #1
  bx lr
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  va_t Taker = 0;
  for (const Export &Exp : Image->Exports)
    if (Exp.Name == "taker")
      Taker = clearThumbBit(Exp.Addr);
  ASSERT_NE(Taker, 0u);
  const uint8_t *Literal = Image->readVA(Taker + 8, 4);
  ASSERT_NE(Literal, nullptr);
  const uint32_t Address =
      readLE<uint32_t>(Literal) + static_cast<uint32_t>(Taker + 2 + 4);
  ASSERT_EQ(Address & 1u, 1u);
  const va_t Callback = Address & ~1u;

  EXPECT_EQ(Image->instructionModeAt(Callback), InstructionMode::Thumb);
  EXPECT_EQ(Image->instructionModeAt(Callback + 2), InstructionMode::Thumb);
  // The literal is data, whatever the code around it.
  EXPECT_FALSE(Image->instructionModeAt(Taker + 8));
}

// The unwinder resumes a frame in the instruction set of its PC
// (`_Unwind_SetIP` keeps its Thumb bit), so a landing pad no branch reaches
// is decoded in the mode of the call site that throws into it.
TEST_F(ELFARM32ModeTest, LandingPadsTakeTheModeOfTheirCallSites) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  const auto Source = tmpFile("landing_pads.cpp");
  std::ofstream(Source) << R"(
struct Failure {};
void may_throw(int);
extern "C" int catcher(int Value) {
  try {
    may_throw(Value);
  } catch (const Failure &) {
    return -1;
  } catch (...) {
    return -2;
  }
  return 0;
}
)";
  const auto Object = tmpFile("landing_pads.o");
  const auto Library = tmpFile("landing_pads.so");
  ASSERT_TRUE(exec(NEVERD_TEST_CLANG,
                   {"-target", "armv7-linux-gnueabihf", "-mthumb", "-O2",
                    "-fPIC", "-c", Source.string(), "-o", Object.string()})
                  .ok());
  ASSERT_TRUE(
      exec("ld.lld", {"-m", "armelf_linux_eabi", "-shared", "--strip-all",
                      Object.string(), "-o", Library.string()})
          .ok());
  auto Image = ELFLoader().load(Library);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());

  std::vector<va_t> Pads;
  for (const ExceptionFunction &Function : Image->ExceptionMetadata.Functions)
    if (Function.Itanium)
      for (const ItaniumCallSite &Site : Function.Itanium->CallSites)
        if (Site.LandingPadVA)
          Pads.push_back(Site.LandingPadVA);
  ASSERT_FALSE(Pads.empty());
  for (const va_t Pad : Pads) {
    SCOPED_TRACE(Pad);
    EXPECT_EQ(Image->instructionModeAt(Pad), InstructionMode::Thumb);
  }
  const auto C =
      exec(ndBin(), {"decompile", Library.string(), "--func=catcher"});
  EXPECT_TRUE(C.ok()) << C.err;
}

TEST_F(ELFARM32ModeTest, LinkerThunkToAnImportStandsForTheImport) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedSharedAssembly("plt_thunk", R"(
.syntax unified
.text
.thumb
.globl tail_caller
.type tail_caller,%function
.thumb_func
tail_caller:
  ldr r0, .Lmessage
.Lpc:
  add r0, pc
  b.w puts
.p2align 2
.Lmessage:
  .word message - (.Lpc + 4)
.section .rodata
message:
  .asciz "through the thunk"
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  std::vector<va_t> Stubs;
  for (const auto &[Stub, Import] : Image->ImportStubIndices)
    if (Image->Imports[Import].Name == "puts")
      Stubs.push_back(Stub);
  ASSERT_EQ(Stubs.size(), 2u) << "the PLT entry and lld's thunk";
  std::vector<std::string> Owners;
  for (va_t Stub : Stubs)
    if (const Section *Owner = Image->getSectionFor(Stub))
      Owners.push_back(Owner->Name);
  EXPECT_EQ(Owners, (std::vector<std::string>{".text", ".plt"}));

  // The caller tail-calls the import with its argument.
  const auto C = exec(ndBin(), {"decompile", tmpFile("plt_thunk.so").string(),
                                "--func=tail_caller"});
  ASSERT_TRUE(C.ok()) << C.err;
  EXPECT_NE(C.out.find("puts(\"through the thunk\")"), std::string::npos)
      << C.out;
}

// A library's veneer for a routine the library defines goes to that
// definition, whose code says it never returns: a C++ runtime's throw
// helpers end in __cxa_throw without being on any list.
TEST_F(ELFARM32ModeTest, StopsAfterAVeneerToTheLibrarysOwnThrowHelper) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM fixture requires cross-target clang";
  if (!exec("ld.lld", {"--version"}).ok())
    GTEST_SKIP() << "ARM ELF linker is unavailable";

  auto Image = loadStrippedSharedAssembly("helper", R"(
.syntax unified
.text
.thumb
.globl caller
.type caller,%function
.thumb_func
caller:
  push {r4, lr}
  blx callee
  bl throw_helper
.arm
.p2align 2
.globl callee
.type callee,%function
callee:
  add r0, r0, #1
  bx lr
.thumb
.globl throw_helper
.type throw_helper,%function
.thumb_func
throw_helper:
  push {r4, lr}
  bl __cxa_throw
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Mode, InstructionMode::MixedARMThumb);
}

TEST_F(ELFARM32ModeTest, UsesAddressSpecificModesInMixedImages) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  const std::string ARM = R"(
.arm
.p2align 2
.globl arm32_add
.type arm32_add,%function
arm32_add:
  add r0, r0, r1
  bx lr
.size arm32_add, .-arm32_add
)";
  const std::string Thumb = R"(
.thumb
.p2align 1
.globl thumb32_add
.type thumb32_add,%function
.thumb_func
thumb32_add:
  adds r0, r0, r1
  bx lr
.size thumb32_add, .-thumb32_add
)";
  for (bool ThumbFirst : {false, true}) {
    SCOPED_TRACE(ThumbFirst);
    auto Image = loadAssembly(".syntax unified\n.text\n" +
                              (ThumbFirst ? Thumb + ARM : ARM + Thumb));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Mode, InstructionMode::MixedARMThumb);
    EXPECT_EQ(Image->getFunctionSymbols().size(), 2u);
    const auto Functions = Image->getFunctionSymbols();
    ASSERT_EQ(Functions.size(), 2u);
    for (const Symbol *Function : Functions)
      EXPECT_EQ(Image->instructionModeAt(Function->Addr),
                Function->Name == "thumb32_add" ? InstructionMode::Thumb
                                                : InstructionMode::ARM);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(*Image));
    for (const Symbol *Function : Functions) {
      ASSERT_TRUE(Dec.selectMode(*Image, Function->Addr));
      EXPECT_EQ(Dec.currentMode(), Image->instructionModeAt(Function->Addr));
    }
    const auto Object = tmpFile("mode.o").string();
    const auto Headers = exec(ndBin(), {"headers", "--json", Object});
    ASSERT_TRUE(Headers.ok()) << Headers.err;
    EXPECT_NE(Headers.out.find("mixed_arm_thumb"), std::string::npos)
        << Headers.out;
    const auto Symbols = exec(ndBin(), {"symbols", "--json", Object});
    ASSERT_TRUE(Symbols.ok()) << Symbols.err;
    EXPECT_NE(Symbols.out.find("arm32_add"), std::string::npos);
    EXPECT_NE(Symbols.out.find("thumb32_add"), std::string::npos);
    const auto Lifted = exec(ndBin(), {"lift", "--dump-low", Object});
    EXPECT_TRUE(Lifted.ok()) << Lifted.err;
    EXPECT_NE(Lifted.out.find("arm32_add"), std::string::npos);
    EXPECT_NE(Lifted.out.find("thumb32_add"), std::string::npos);
  }
}

TEST_F(ELFARM32ModeTest, FunctionModeHintsRespectMappingEvidence) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.thumb
.globl mapped_thumb
.type mapped_thumb,%function
.thumb_func
mapped_thumb:
  adds r0, r0, r1
  bx lr
.size mapped_thumb, .-mapped_thumb
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const auto Functions = Image->getFunctionSymbols();
  ASSERT_EQ(Functions.size(), 1u);
  const va_t Entry = Functions.front()->Addr;
  BinaryLoadOptions Options;
  Options.ARMFunctionModes.emplace(Entry, InstructionMode::Thumb);
  const auto Object = tmpFile("mode.o");
  auto Allowed = loadBinary(Object, Options);
  ASSERT_TRUE(static_cast<bool>(Allowed))
      << llvm::toString(Allowed.takeError());
  EXPECT_EQ(Allowed->instructionModeAt(Entry), InstructionMode::Thumb);

  Options.ARMFunctionModes[Entry] = InstructionMode::ARM;
  auto Rejected = loadBinary(Object, Options);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError()).find("conflicts"),
            std::string::npos);
}

TEST_F(ELFARM32ModeTest, RejectsAThumbFunctionAliasOverARMMappingEvidence) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.globl arm32_add
.type arm32_add,%function
arm32_add:
  add r0, r0, r1
  bx lr
.size arm32_add, .-arm32_add
.globl conflicting_alias
.type conflicting_alias,%function
.set conflicting_alias, arm32_add + 1
)");
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError())
                .find("conflicting ARM/Thumb instruction modes"),
            std::string::npos);
}

TEST_F(ELFARM32ModeTest, DecompilesCallsAcrossARMAndThumbRegions) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.p2align 2
.globl arm_leaf
.type arm_leaf,%function
arm_leaf:
  add r0, r0, r1
  bx lr
.size arm_leaf, .-arm_leaf
.globl arm_call_thumb
.type arm_call_thumb,%function
arm_call_thumb:
  push {lr}
  blx thumb_leaf
  pop {pc}
.size arm_call_thumb, .-arm_call_thumb
.thumb
.p2align 1
.globl thumb_leaf
.type thumb_leaf,%function
.thumb_func
thumb_leaf:
  adds r0, r0, r1
  bx lr
.size thumb_leaf, .-thumb_leaf
.globl thumb_call_arm
.type thumb_call_arm,%function
.thumb_func
thumb_call_arm:
  push {lr}
  blx arm_leaf
  pop {pc}
.size thumb_call_arm, .-thumb_call_arm
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Mode, InstructionMode::MixedARMThumb);
  ASSERT_EQ(Image->getFunctionSymbols().size(), 4u);
  const uint8_t *ARMCall = Image->readVA(12, 4);
  const uint8_t *ThumbCall = Image->readVA(26, 4);
  ASSERT_NE(ARMCall, nullptr);
  ASSERT_NE(ThumbCall, nullptr);
  uint32_t ARMEncoding;
  uint16_t ThumbEncoding[2];
  std::memcpy(&ARMEncoding, ARMCall, sizeof(ARMEncoding));
  std::memcpy(ThumbEncoding, ThumbCall, sizeof(ThumbEncoding));
  EXPECT_EQ(ARMEncoding, 0xfa000000u);
  EXPECT_EQ(ThumbEncoding[0], 0xf7ffu);
  EXPECT_EQ(ThumbEncoding[1], 0xeff2u);
  const auto Object = tmpFile("mode.o").string();
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
    const auto Output = tmpFile(LLVM ? "mixed-llvm.c" : "mixed-high.c");
    std::vector<std::string> Arguments{"decompile"};
    if (LLVM)
      Arguments.push_back("--llvm");
    Arguments.insert(Arguments.end(), {"-o", Output.string(), Object});
    const auto Decompiled = exec(ndBin(), Arguments);
    ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
    std::ifstream Input(Output);
    const std::string Source(std::istreambuf_iterator<char>(Input), {});
    EXPECT_NE(Source.find("arm_call_thumb"), std::string::npos) << Source;
    EXPECT_NE(Source.find("thumb_call_arm"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("return (int32_t)thumb_call_arm()"),
              std::string::npos)
        << Source;
    if (!LLVM) {
      EXPECT_NE(Source.find("arm_call_thumb(int32_t arg0, int32_t arg1)"),
                std::string::npos)
          << Source;
      EXPECT_NE(Source.find("thumb_call_arm(int32_t arg0, int32_t arg1)"),
                std::string::npos)
          << Source;
      EXPECT_NE(Source.find("thumb_leaf(arg0, arg1)"), std::string::npos)
          << Source;
      EXPECT_NE(Source.find("arm_leaf(arg0, arg1)"), std::string::npos)
          << Source;
    }
    EXPECT_EQ(Source.find("/* unknown"), std::string::npos) << Source;
    {
      std::ofstream CFile(Output, std::ios::app);
      CFile << "\nint main(void) {\n"
               "  return arm_call_thumb(7, 5) == 12 && "
               "thumb_call_arm(7, 5) == 12 && "
               "arm_call_thumb(0xffffffffu, 2) == 1 && "
               "thumb_call_arm(0xffffffffu, 2) == 1 ? 0 : 1;\n"
               "}\n";
    }
    const auto Executable = tmpFile(LLVM ? "mixed-llvm" : "mixed-high");
    const auto Compiled = exec(
        NEVERD_TEST_CLANG, {"-std=c11", "-Werror=implicit-function-declaration",
                            "-O0", Output.string(), "-o", Executable.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err;
  }
}

TEST_F(ELFARM32ModeTest, RecoversForwardedArgumentsAcrossModeChains) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.p2align 2
.globl arm_outer
.type arm_outer,%function
arm_outer:
  push {lr}
  blx thumb_middle
  pop {pc}
.size arm_outer, .-arm_outer
.thumb
.p2align 1
.globl thumb_middle
.type thumb_middle,%function
.thumb_func
thumb_middle:
  push {lr}
  blx arm_leaf
  pop {pc}
.size thumb_middle, .-thumb_middle
.arm
.p2align 2
.globl arm_leaf
.type arm_leaf,%function
arm_leaf:
  add r0, r0, r1
  bx lr
.size arm_leaf, .-arm_leaf
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const auto Output = tmpFile("chain.c");
  const auto Decompiled = exec(ndBin(), {"decompile", "-o", Output.string(),
                                         tmpFile("mode.o").string()});
  ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
  std::ifstream Input(Output);
  const std::string Source(std::istreambuf_iterator<char>(Input), {});
  EXPECT_NE(Source.find("arm_outer(int32_t arg0, int32_t arg1)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("thumb_middle(int32_t arg0, int32_t arg1)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("thumb_middle(arg0, arg1)"), std::string::npos)
      << Source;
  {
    std::ofstream CFile(Output, std::ios::app);
    CFile << "\nint main(void) { return arm_outer(7, 5) == 12 && "
             "arm_outer(0xffffffffu, 2) == 1 ? 0 : 1; }\n";
  }
  const auto Executable = tmpFile("chain");
  const auto Compiled = exec(
      NEVERD_TEST_CLANG, {"-std=c11", "-Werror=implicit-function-declaration",
                          "-O0", Output.string(), "-o", Executable.string()});
  ASSERT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
  EXPECT_TRUE(exec(Executable.string(), {}).ok());
}

TEST_F(ELFARM32ModeTest, AppliesThumbWideBranchRelocations) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM branch fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.thumb
.p2align 1
.globl thumb_branch
.type thumb_branch,%function
.thumb_func
thumb_branch:
  cmp r0, #0
  beq.w thumb_target
  b.w thumb_target
  bx lr
.size thumb_branch, .-thumb_branch
.globl thumb_target
.type thumb_target,%function
.thumb_func
thumb_target:
  adds r0, r0, #1
  bx lr
.size thumb_target, .-thumb_target
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const uint8_t *Conditional = Image->readVA(2, 4);
  const uint8_t *Unconditional = Image->readVA(6, 4);
  ASSERT_NE(Conditional, nullptr);
  ASSERT_NE(Unconditional, nullptr);
  uint16_t ConditionalHalfwords[2], UnconditionalHalfwords[2];
  std::memcpy(ConditionalHalfwords, Conditional, 4);
  std::memcpy(UnconditionalHalfwords, Unconditional, 4);
  EXPECT_EQ(ConditionalHalfwords[0], 0xf000u);
  EXPECT_EQ(ConditionalHalfwords[1], 0x8003u);
  EXPECT_EQ(UnconditionalHalfwords[0], 0xf000u);
  EXPECT_EQ(UnconditionalHalfwords[1], 0xb801u);
}

TEST_F(ELFARM32ModeTest, AppliesARMCallRelocationToHalfwordThumbEntry) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM branch fixture requires cross-target clang";
  auto Image = loadAssembly(R"(
.syntax unified
.text
.arm
.globl arm_to_thumb_half
.type arm_to_thumb_half,%function
arm_to_thumb_half:
  blx thumb_half
  bx lr
.size arm_to_thumb_half, .-arm_to_thumb_half
.thumb
  nop
.globl thumb_half
.type thumb_half,%function
.thumb_func
thumb_half:
  adds r0, r0, r1
  bx lr
.size thumb_half, .-thumb_half
)");
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->instructionModeAt(10), InstructionMode::Thumb);
  const uint8_t *Call = Image->readVA(0, 4);
  ASSERT_NE(Call, nullptr);
  uint32_t Encoding = 0;
  std::memcpy(&Encoding, Call, sizeof(Encoding));
  EXPECT_EQ(Encoding, 0xfb000000u);
}

TEST_F(ELFARM32ModeTest, RetainsHalfwordBitInGeneratedARMToThumbCall) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  auto Source = loadAssembly(R"(
.syntax unified
.text
.arm
.globl arm_caller
.type arm_caller,%function
arm_caller:
  bx lr
.size arm_caller, .-arm_caller
.thumb
.globl thumb_callee
.type thumb_callee,%function
.thumb_func
thumb_callee:
  bx lr
.size thumb_callee, .-thumb_callee
)");
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_EQ(Source->Mode, InstructionMode::MixedARMThumb);

  CompiledImage Compiled;
  Compiled.Bytes.resize(14);
  const uint32_t PackedBL = 0xeb000000u;
  std::memcpy(Compiled.Bytes.data(), &PackedBL, sizeof(PackedBL));
  CompiledSection Code;
  Code.Name = ".text";
  Code.VA = 0x2000;
  Code.Size = Compiled.Bytes.size();
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  CompiledFixupReference Call;
  Call.Offset = 0;
  Call.Symbol = "thumb_callee";
  Call.IsPCRel = true;
  Call.IsResolved = true;
  Call.BitWidth = 24;
  Call.ResolvedValue = 10; // Exact target 0x200a; BL packed it to 0x2008.
  Code.FixupReferences.push_back(Call);
  Compiled.Sections.push_back(Code);
  Compiled.SourceFunctionOwners.push_back({"arm_caller", "arm_caller", 0x2000});
  Compiled.SourceFunctionOwners.push_back(
      {"thumb_callee", "thumb_callee", 0x200a});
  Compiled.SourceFunctionOriginalVAs["arm_caller"] = 0;
  Compiled.SourceFunctionOriginalVAs["thumb_callee"] = 4;

  std::string Detail;
  ASSERT_TRUE(repairMixedARMInterworkingCalls(Compiled, *Source, Detail))
      << Detail;
  uint32_t BLX = 0;
  std::memcpy(&BLX, Compiled.Bytes.data(), sizeof(BLX));
  EXPECT_EQ(BLX, 0xfb000000u);

  const uint32_t PackedBranch = 0xea000000u;
  std::memcpy(Compiled.Bytes.data(), &PackedBranch, sizeof(PackedBranch));
  Detail.clear();
  EXPECT_FALSE(repairMixedARMInterworkingCalls(Compiled, *Source, Detail));
  EXPECT_NE(Detail.find("interworking veneer"), std::string::npos);
  uint32_t Unchanged = 0;
  std::memcpy(&Unchanged, Compiled.Bytes.data(), sizeof(Unchanged));
  EXPECT_EQ(Unchanged, PackedBranch);
}

TEST_F(ELFARM32ModeTest, RepairsHalfwordAlignedGeneratedThumbToARMCall) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM interworking fixture requires cross-target clang";
  auto Source = loadAssembly(R"(
.syntax unified
.text
.thumb
.globl thumb_caller
.type thumb_caller,%function
.thumb_func
thumb_caller:
  bx lr
  nop
.size thumb_caller, .-thumb_caller
.arm
.globl arm_callee
.type arm_callee,%function
arm_callee:
  bx lr
.size arm_callee, .-arm_callee
)");
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_EQ(Source->Mode, InstructionMode::MixedARMThumb);

  CompiledImage Compiled;
  Compiled.Bytes.resize(10);
  const uint32_t PackedBL = 0xf801f000u;
  std::memcpy(Compiled.Bytes.data(), &PackedBL, sizeof(PackedBL));
  CompiledSection Code;
  Code.Name = ".text";
  Code.VA = 0x2002;
  Code.Size = Compiled.Bytes.size();
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  CompiledFixupReference Call;
  Call.Offset = 0;
  Call.Symbol = "arm_callee";
  Call.IsPCRel = true;
  Call.IsResolved = true;
  Call.BitWidth = 32;
  Call.ResolvedValue = 6; // BL reaches 0x2008 without changing mode.
  Code.FixupReferences.push_back(Call);
  Compiled.Sections.push_back(Code);
  Compiled.SourceFunctionOwners.push_back(
      {"thumb_caller", "thumb_caller", 0x2002});
  Compiled.SourceFunctionOwners.push_back({"arm_callee", "arm_callee", 0x2008});
  Compiled.SourceFunctionOriginalVAs["thumb_caller"] = 0;
  Compiled.SourceFunctionOriginalVAs["arm_callee"] = 4;

  std::string Detail;
  ASSERT_TRUE(repairMixedARMInterworkingCalls(Compiled, *Source, Detail))
      << Detail;
  uint32_t BLX = 0;
  std::memcpy(&BLX, Compiled.Bytes.data(), sizeof(BLX));
  EXPECT_EQ(BLX, 0xe802f000u);

  const uint32_t PackedBranch = 0xb801f000u;
  std::memcpy(Compiled.Bytes.data(), &PackedBranch, sizeof(PackedBranch));
  Detail.clear();
  EXPECT_FALSE(repairMixedARMInterworkingCalls(Compiled, *Source, Detail));
  EXPECT_NE(Detail.find("interworking veneer"), std::string::npos);
  uint32_t Unchanged = 0;
  std::memcpy(&Unchanged, Compiled.Bytes.data(), sizeof(Unchanged));
  EXPECT_EQ(Unchanged, PackedBranch);
}

} // namespace
