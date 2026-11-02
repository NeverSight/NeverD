//===- PEImageBaseTests.cpp - PE headers as the linker's __ImageBase ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A linked PE image's code reaches its own DOS and NT headers through the
/// linker's `__ImageBase`.  The loader places the headers whether or not it
/// maps them; HighC, LLVM C and LLVM IR spell every header address from that
/// symbol and never copy or fold a header byte; patching resolves it.
//===----------------------------------------------------------------------===//

#include "../../loader/PEHeaderFixture.h"
#include "NeverDLiftFixture.h"

#include "neverd/backend/codegen/COFF/COFFReloc.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace {
using namespace neverd;

/// One walker: its machine, whether its headers are unmapped and, on x86,
/// whether it runs at its link address without base relocations.
struct Case {
  uint16_t Machine;
  bool Unmapped;
  bool Fixed = false;
};

const std::vector<Case> &cases() {
  using namespace llvm::COFF;
  static const std::vector<Case> All = {{IMAGE_FILE_MACHINE_I386, false},
                                        {IMAGE_FILE_MACHINE_I386, true},
                                        {IMAGE_FILE_MACHINE_I386, false, true},
                                        {IMAGE_FILE_MACHINE_I386, true, true},
                                        {IMAGE_FILE_MACHINE_AMD64, false},
                                        {IMAGE_FILE_MACHINE_AMD64, true},
                                        {IMAGE_FILE_MACHINE_ARM64, false},
                                        {IMAGE_FILE_MACHINE_ARM64, true}};
  return All;
}

std::string label(const Case &C) {
  return (C.Machine == llvm::COFF::IMAGE_FILE_MACHINE_I386 ? std::string("x86")
          : C.Machine == llvm::COFF::IMAGE_FILE_MACHINE_AMD64
              ? std::string("x64")
              : std::string("aarch64")) +
         (C.Unmapped ? "-unmapped" : "-mapped") + (C.Fixed ? "-fixed" : "");
}

test::PEImageBaseWalker walker(const Case &C) {
  return test::peImageBaseWalker(C.Machine, C.Unmapped, C.Fixed);
}

std::string hexVA(uint64_t VA, bool Lower = false) {
  return "0x" + llvm::utohexstr(VA, Lower);
}

class PEImageBase : public NeverDLiftTest {
protected:
  fs::path write(const test::PEImageBaseWalker &W, const Case &C) {
    const auto Path = tmpFile("walker-" + label(C) + ".exe");
    std::ofstream(Path, std::ios::binary) << W.Bytes;
    return Path;
  }

  static std::string read(const fs::path &Path) {
    std::ifstream Input(Path, std::ios::binary);
    return {std::istreambuf_iterator<char>(Input),
            std::istreambuf_iterator<char>()};
  }

  /// The C names the headers through one `__ImageBase` declaration and keeps
  /// no stale header address.  The header reads survive whether or not the
  /// loader maps the headers: e_magic is still compared with "MZ" at run
  /// time, and no header byte is folded, quoted or copied (e_lfanew is 0x80).
  static void expectImageBaseC(const std::string &C,
                               const test::PEImageBaseWalker &W) {
    const std::string Declaration = "extern unsigned char __ImageBase[];";
    const size_t First = C.find(Declaration);
    EXPECT_NE(First, std::string::npos) << C;
    EXPECT_EQ(First, C.rfind(Declaration)) << C;
    for (const uint64_t Stale : {W.Base, W.Base + 0x3c, W.Base + 0x80})
      for (bool Lower : {false, true})
        EXPECT_EQ(C.find(hexVA(Stale, Lower)), std::string::npos)
            << hexVA(Stale) << "\n"
            << C;
    EXPECT_EQ(C.find(std::to_string(W.Base)), std::string::npos) << C;
    EXPECT_NE(C.find("0x5A4D"), std::string::npos) << C;
    EXPECT_FALSE(std::regex_search(C, std::regex("\\b0[xX]80\\b"))) << C;
    for (const char *Copied : {"\"MZ", "\"PE", "NATIVE_TEXT_CANARY"})
      EXPECT_EQ(C.find(Copied), std::string::npos) << Copied << "\n" << C;
  }

  /// \p Source compiles for the image's target to an object that imports the
  /// linker's symbol.
  void expectImportsImageBase(const fs::path &Source,
                              const test::PEImageBaseWalker &W) {
    const auto Object = tmpFile(Source.stem().string() + ".obj");
    const auto Built =
        runHighCClang(Source,
                      {std::string("--target=") + W.Triple, "-ffreestanding",
                       "-std=gnu11", "-fno-strict-aliasing"},
                      {"-O1", "-c", "-o", Object.string()});
    ASSERT_EQ(Built.exitCode, 0) << Built.err << "\n" << read(Source);
    auto Buffer = llvm::MemoryBuffer::getFile(Object.string());
    ASSERT_TRUE(bool(Buffer));
    auto File = llvm::object::ObjectFile::createObjectFile(**Buffer);
    ASSERT_TRUE(bool(File)) << llvm::toString(File.takeError());
    bool Imports = false;
    for (const auto &Symbol : (*File)->symbols()) {
      auto Name = Symbol.getName();
      auto Flags = Symbol.getFlags();
      if (Name && Flags)
        Imports |= *Name == W.Symbol &&
                   (*Flags & llvm::object::SymbolRef::SF_Undefined);
      if (!Name)
        llvm::consumeError(Name.takeError());
      if (!Flags)
        llvm::consumeError(Flags.takeError());
    }
    EXPECT_TRUE(Imports) << W.Symbol << "\n" << read(Source);
  }

  /// On a Linux x86-64 host, an x64 walker's C runs against a harness that
  /// defines `__ImageBase` with the file's headers: it returns the NT
  /// headers' address, and it reads a broken DOS header at run time.
  void expectRunsAgainstHostImageBase(const fs::path &Source,
                                      const test::PEImageBaseWalker &W,
                                      const Case &C) {
#if defined(__linux__) && defined(__x86_64__)
    if (C.Machine != llvm::COFF::IMAGE_FILE_MACHINE_AMD64)
      return;
    const auto Harness = tmpFile(Source.stem().string() + "-harness.c");
    const auto Program = tmpFile(Source.stem().string() + "-host");
    std::ofstream H(Harness);
    H << "#include <stdint.h>\nunsigned char __ImageBase[0x200] = {";
    for (size_t I = 0; I != 0x200; ++I)
      H << unsigned(static_cast<uint8_t>(W.Bytes[I])) << ',';
    H << "};\nextern uint64_t sub_" << llvm::utohexstr(W.Entry)
      << "();\nint main(void) {\n"
         "  if (sub_"
      << llvm::utohexstr(W.Entry)
      << "() != (uint64_t)(uintptr_t)__ImageBase + 0x80) return 1;\n"
         "  __ImageBase[0] = 'X';\n"
         "  return sub_"
      << llvm::utohexstr(W.Entry) << "() == 0 ? 0 : 2;\n}\n";
    H.close();
    const auto Built =
        runHighCClang(Source, {"-std=gnu11", "-fno-strict-aliasing"},
                      {"-O2", Harness.string(), "-o", Program.string()});
    ASSERT_EQ(Built.exitCode, 0) << Built.err << "\n" << read(Source);
    EXPECT_EQ(exec(Program.string(), {}).exitCode, 0) << read(Source);
#else
    (void)Source;
    (void)W;
    (void)C;
#endif
  }
};

TEST_F(PEImageBase, LoaderPlacesTheHeadersWhetherOrNotItMapsThem) {
  for (const Case &C : cases()) {
    SCOPED_TRACE(label(C));
    const auto W = walker(C);
    auto Image =
        COFFLoader().loadBuffer(llvm::MemoryBufferRef(W.Bytes, "walker.exe"));
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->readVA(W.Base, 2) == nullptr, C.Unmapped);
    EXPECT_EQ(llvm::any_of(Image->LoadDiagnostics,
                           [](const LoadDiagnostic &D) {
                             return D.Code == "pe.headers_unmapped";
                           }),
              C.Unmapped);
    EXPECT_EQ(Image->PEHeaderBytes, 0x200u);
    EXPECT_EQ(Image->imageBaseOffset(W.Base), std::optional<uint64_t>(0));
    EXPECT_EQ(Image->imageBaseOffset(W.Base + 0x1ff),
              std::optional<uint64_t>(0x1ff));
    EXPECT_FALSE(Image->imageBaseOffset(W.Base + 0x200));
    EXPECT_FALSE(Image->imageBaseOffset(W.Base - 1));
    EXPECT_TRUE(Image->isPotentiallyRelocatableAddress(W.Base + 0x3c));
    if (C.Machine != llvm::COFF::IMAGE_FILE_MACHINE_I386 || C.Fixed)
      continue;
    // A relocated operand names the headers whether or not they are mapped:
    // the base, e_lfanew and the base the NT headers' offset is added to.
    for (const auto &[Field, Address] :
         {std::pair{uint64_t{0x401003}, W.Base},
          std::pair{uint64_t{0x40100c}, W.Base + 0x3c},
          std::pair{uint64_t{0x401012}, W.Base}}) {
      const auto Operand = Image->DataAddressRelocOperands.find(Field);
      ASSERT_NE(Operand, Image->DataAddressRelocOperands.end()) << hexVA(Field);
      EXPECT_EQ(Operand->second.TargetVA, Address);
      EXPECT_EQ(Operand->second.TargetOwnerVA, W.Base);
      EXPECT_TRUE(Image->relocatedTargetBelongsToOwner(Address, W.Base));
    }
  }
  // An image linked at zero cannot tell a header address from a small
  // number, so it places no headers.
  auto Bytes = test::peHeaderImage(true, llvm::COFF::IMAGE_FILE_MACHINE_AMD64);
  auto PE =
      locatePEHeaders(reinterpret_cast<uint8_t *>(Bytes.data()), Bytes.size());
  reinterpret_cast<llvm::object::pe32plus_header *>(PE.OptionalHeader)
      ->ImageBase = 0;
  auto Zero =
      COFFLoader().loadBuffer(llvm::MemoryBufferRef(Bytes, "zero-base.exe"));
  ASSERT_TRUE(bool(Zero)) << llvm::toString(Zero.takeError());
  EXPECT_EQ(Zero->PEHeaderBytes, 0u);
  EXPECT_FALSE(Zero->imageBaseOffset(0x3c));
}

TEST_F(PEImageBase, HighCNamesTheHeadersThroughImageBase) {
  for (const Case &C : cases()) {
    SCOPED_TRACE(label(C));
    const auto W = walker(C);
    const auto Image = write(W, C);
    const auto Source = tmpFile("highc-" + label(C) + ".c");
    const auto R = exec(ndBin(), {"decompile", "--language", "c", "-o",
                                  Source.string(), Image.string()});
    ASSERT_EQ(R.exitCode, 0) << R.err;
    const std::string Text = read(Source);
    expectImageBaseC(Text, W);
    // Both header reads stay reads of the running image.
    EXPECT_NE(Text.find("&__ImageBase[0]"), std::string::npos) << Text;
    EXPECT_NE(Text.find("&__ImageBase[60]"), std::string::npos) << Text;
    expectImportsImageBase(Source, W);
    expectRunsAgainstHostImageBase(Source, W, C);
  }
}

TEST_F(PEImageBase, LLVMCNamesTheHeadersThroughImageBase) {
  for (const Case &C : cases()) {
    SCOPED_TRACE(label(C));
    const auto W = walker(C);
    const auto Image = write(W, C);
    const auto Source = tmpFile("llvmc-" + label(C) + ".c");
    const auto R = exec(ndBin(), {"decompile", "--llvm", "-o", Source.string(),
                                  Image.string()});
    ASSERT_EQ(R.exitCode, 0) << R.err;
    expectImageBaseC(read(Source), W);
    expectImportsImageBase(Source, W);
    expectRunsAgainstHostImageBase(Source, W, C);
  }
}

TEST_F(PEImageBase, LLVMIRDeclaresImageBaseWithoutAHeaderCopy) {
  for (const Case &C : cases()) {
    SCOPED_TRACE(label(C));
    const auto W = walker(C);
    const auto Image = write(W, C);
    for (bool Optimized : {false, true}) {
      const auto R = Optimized ? liftToLLVMIR(Image) : liftToLLVMIRUnopt(Image);
      ASSERT_EQ(R.exitCode, 0) << R.err;
      EXPECT_NE(R.out.find("@__ImageBase = external global [0 x i8]"),
                std::string::npos)
          << R.out;
      EXPECT_EQ(R.out.find(std::to_string(W.Base)), std::string::npos) << R.out;
      // No module global holds the DOS header's bytes.
      EXPECT_EQ(R.out.find("c\"MZ"), std::string::npos) << R.out;
    }
  }
}

TEST_F(PEImageBase, BranchTargetFromAHeaderReadStaysIndirect) {
  // mov eax, [rip -> e_lfanew]; lea rcx, [rip -> 0x140000f94];
  // add rcx, rax; jmp rcx; int3; int3; 0x140001014: mov eax, 7; ret
  // The jump reaches 0x140001014 only through the file's e_lfanew (0x80).
  const std::string Code("\x8b\x05\x36\xf0\xff\xff\x48\x8d\x0d\x87\xff\xff\xff"
                         "\x48\x01\xc1\xff\xe1\xcc\xcc\xb8\x07\x00\x00\x00\xc3",
                         26);
  for (bool Unmapped : {false, true}) {
    SCOPED_TRACE(Unmapped);
    auto Bytes =
        test::peHeaderImage(true, llvm::COFF::IMAGE_FILE_MACHINE_AMD64);
    auto PE = locatePEHeaders(reinterpret_cast<uint8_t *>(Bytes.data()),
                              Bytes.size());
    auto *Text =
        reinterpret_cast<llvm::object::coff_section *>(PE.SectionTable);
    Text->VirtualSize = 0x40;
    Bytes.replace(Text->PointerToRawData, Code.size(), Code);
    if (Unmapped)
      setPEDataDirectory(PE, llvm::COFF::GLOBAL_PTR, 0x1000, 4);
    const auto Image =
        tmpFile(Unmapped ? "branch-unmapped.exe" : "branch-mapped.exe");
    std::ofstream(Image, std::ios::binary) << Bytes;
    const auto R = liftToLowIR(Image);
    ASSERT_EQ(R.exitCode, 0) << R.err;
    // The scalar-branch proof reads no header byte: the jump stays indirect
    // whether or not the headers are mapped.
    EXPECT_EQ(R.out.find("cst:0x140001014"), std::string::npos) << R.out;
    EXPECT_NE(R.out.find("[0x140001010.0] INDIR_"), std::string::npos) << R.out;
  }
}

TEST_F(PEImageBase, PatchResolvesImageBaseToTheImageBase) {
  for (const Case &C : cases()) {
    SCOPED_TRACE(label(C));
    const auto W = walker(C);
    auto Image =
        COFFLoader().loadBuffer(llvm::MemoryBufferRef(W.Bytes, "walker.exe"));
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    COFFRelocResolver FromImage;
    FromImage.populateFromImage(*Image, Image->Arch);
    COFFRelocResolver FromFile;
    FromFile.parse(std::vector<uint8_t>(W.Bytes.begin(), W.Bytes.end()),
                   Image->Arch);
    // The object symbol C's `__ImageBase` links as: x86 decorates it.
    for (const RelocResolver *Resolver : {&FromImage, &FromFile}) {
      const auto *Entry = Resolver->findEntry(W.Symbol);
      ASSERT_NE(Entry, nullptr);
      EXPECT_EQ(Entry->Addr, W.Base);
      EXPECT_FALSE(Entry->IsCode);
    }
  }
}

} // namespace
