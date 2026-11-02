//===- MachONativeInplaceTests.cpp - Native ABI admission ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Public in-place rewriting must authenticate the ABI of the actual input
/// bytes even when neither caller metadata nor LLVM carries that information.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/object/SectionNames.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace neverd {
namespace {

#define NEVERD_INPLACE_ABI_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define NEVERD_INPLACE_ABI_TEXT(Name, Value) constexpr char Name[] = Value;
#include "MachONativeInplaceFixtures.def"

struct SubtypeCase {
  const char *Name;
  uint32_t Subtype;
};

constexpr SubtypeCase kSubtypeCases[] = {
#define NEVERD_INPLACE_ABI_CASE(Name, Subtype) {#Name, Subtype},
#include "MachONativeInplaceFixtures.def"
};

enum class CallerMetadata { Complete, ClearedSubtype, Empty };

std::vector<uint8_t> makeInput(uint32_t Subtype) {
  using namespace llvm::MachO;
  std::vector<uint8_t> Bytes(kFileSize);
  mach_header_64 Header{};
  Header.magic = MH_MAGIC_64;
  Header.cputype = CPU_TYPE_ARM64;
  Header.cpusubtype = Subtype;
  Header.filetype = MH_EXECUTE;
  Header.ncmds = kCommandCount;
  Header.sizeofcmds =
      sizeof(segment_command_64) + sizeof(section_64) + sizeof(symtab_command);
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  segment_command_64 Segment{};
  Segment.cmd = LC_SEGMENT_64;
  Segment.cmdsize = sizeof(Segment) + sizeof(section_64);
  std::memcpy(Segment.segname, section_names::macho::TextSeg,
              std::strlen(section_names::macho::TextSeg));
  Segment.vmaddr = kBase;
  Segment.vmsize = kVMSize;
  Segment.filesize = Bytes.size();
  Segment.maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
  Segment.initprot = Segment.maxprot;
  Segment.nsects = kSectionCount;
  std::memcpy(Bytes.data() + sizeof(Header), &Segment, sizeof(Segment));

  section_64 Text{};
  std::memcpy(Text.sectname, section_names::macho::Text,
              std::strlen(section_names::macho::Text));
  std::memcpy(Text.segname, Segment.segname, sizeof(Text.segname));
  Text.addr = kBase + kTextOffset;
  Text.size = kTextSize;
  Text.offset = kTextOffset;
  Text.align = kInstructionAlignment;
  Text.flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
  std::memcpy(Bytes.data() + sizeof(Header) + sizeof(Segment), &Text,
              sizeof(Text));

  symtab_command Symtab{};
  Symtab.cmd = LC_SYMTAB;
  Symtab.cmdsize = sizeof(Symtab);
  Symtab.symoff = kSymbolOffset;
  Symtab.nsyms = kSymbolCount;
  Symtab.stroff = kStringOffset;
  Symtab.strsize = sizeof(kSymbolStrings);
  std::memcpy(Bytes.data() + sizeof(Header) + Segment.cmdsize, &Symtab,
              sizeof(Symtab));
  nlist_64 Symbol{};
  Symbol.n_strx = kFunctionNameIndex;
  Symbol.n_type = N_SECT | N_EXT;
  Symbol.n_sect = kTextSectionIndex;
  Symbol.n_value = Text.addr;
  std::memcpy(Bytes.data() + kSymbolOffset, &Symbol, sizeof(Symbol));
  std::memcpy(Bytes.data() + kStringOffset, kSymbolStrings,
              sizeof(kSymbolStrings));
  for (uint32_t Offset = kTextOffset; Offset < kTextOffset + kTextSize;
       Offset += kInstructionBytes)
    llvm::support::endian::write32le(Bytes.data() + Offset, kNOP);
  llvm::support::endian::write32le(
      Bytes.data() + kTextOffset + kTextSize - kInstructionBytes, kRET);
  return Bytes;
}

bool writeInput(llvm::StringRef Path, llvm::ArrayRef<uint8_t> Bytes) {
  std::error_code Error;
  llvm::raw_fd_ostream Stream(Path, Error, llvm::sys::fs::OF_None);
  if (Error)
    return false;
  Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  Stream.close();
  return !Stream.has_error();
}

std::vector<uint8_t> readOutput(llvm::StringRef Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path);
  if (!Buffer) {
    ADD_FAILURE() << Buffer.getError().message();
    return {};
  }
  llvm::StringRef Bytes = (*Buffer)->getBuffer();
  return {Bytes.bytes_begin(), Bytes.bytes_end()};
}

std::unique_ptr<llvm::Module> makeModule(llvm::LLVMContext &Context) {
  auto Module = std::make_unique<llvm::Module>(kModule, Context);
  auto *Type = llvm::FunctionType::get(llvm::Type::getInt64Ty(Context), false);
  auto *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, kFunction, Module.get());
  rewrite_source::setOriginalVA(*Function, kBase + kTextOffset);
  Function->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, kEntry, Function));
  Builder.CreateRet(Builder.getInt64(kReturnValue));
  return Module;
}

void checkRewrite(uint32_t Subtype, CallerMetadata Metadata,
                  bool ExistingOutput, bool ExpectSuccess) {
  const std::vector<uint8_t> Input = makeInput(Subtype);
  llvm::SmallString<kPathBytes> InputPath;
  llvm::SmallString<kPathBytes> OutputPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile(kInputPrefix, kExtension, InputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kOutputPrefix, kExtension,
                                                  OutputPath));
  llvm::FileRemover RemoveInput(InputPath);
  llvm::FileRemover RemoveOutput(OutputPath);
  ASSERT_TRUE(writeInput(InputPath, Input));
  llvm::StringRef Sentinel(kOutputSentinel);
  const std::vector<uint8_t> OutputSentinel(Sentinel.bytes_begin(),
                                            Sentinel.bytes_end());
  if (ExistingOutput)
    ASSERT_TRUE(writeInput(OutputPath, OutputSentinel));
  else
    ASSERT_FALSE(llvm::sys::fs::remove(OutputPath));

  auto Loaded = MachOLoader().load(InputPath.str().str());
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  ASSERT_EQ(Loaded->isArm64e(), !ExpectSuccess);
  BinaryImage Image = std::move(*Loaded);
  if (Metadata == CallerMetadata::ClearedSubtype)
    Image.MachOCPUSubtype = kOrdinarySubtype;
  else if (Metadata == CallerMetadata::Empty)
    Image = BinaryImage{};
  if (Metadata != CallerMetadata::Complete)
    ASSERT_FALSE(Image.isArm64e());

  llvm::LLVMContext Context;
  auto Module = makeModule(Context);
  ASSERT_EQ(Module->getModuleFlag(kArm64eAnalysisModuleFlag), nullptr);
  auto Rewriter = InplaceRewriter::create(BinaryFormat::MachO);
  ASSERT_NE(Rewriter, nullptr);
  testing::internal::CaptureStderr();
  const PatchResult Result =
      Rewriter->rewrite(InputPath.str().str(), OutputPath.str().str(), *Module,
                        Image, Arch::AArch64);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();

  EXPECT_EQ(readOutput(InputPath), Input);
  if (!ExpectSuccess) {
    EXPECT_FALSE(Result.Success);
    EXPECT_NE(Diagnostic.find(kArm64eRebuildUnsupported), std::string::npos)
        << Diagnostic;
    EXPECT_EQ(Result.CodeSize, 0u);
    EXPECT_EQ(Result.TrampolineCount, 0u);
    EXPECT_TRUE(Result.OutputPath.empty());
    EXPECT_TRUE(Result.PatchedOriginalEntries.empty());
    EXPECT_TRUE(Result.PatchedFunctions.empty());
    EXPECT_TRUE(Result.OriginalCallbacks.empty());
    if (ExistingOutput)
      EXPECT_EQ(readOutput(OutputPath), OutputSentinel);
    else
      EXPECT_FALSE(llvm::sys::fs::exists(OutputPath));
    return;
  }

  ASSERT_TRUE(Result.Success) << Diagnostic;
  EXPECT_EQ(Result.TrampolineCount, 0u);
  EXPECT_EQ(Result.PatchedOriginalEntries,
            (std::vector<va_t>{kBase + kTextOffset}));
  const std::vector<uint8_t> Output = readOutput(OutputPath);
  ASSERT_EQ(Output.size(), Input.size());
  EXPECT_FALSE(std::equal(Input.begin() + kTextOffset,
                          Input.begin() + kTextOffset + kTextSize,
                          Output.begin() + kTextOffset));
  EXPECT_TRUE(
      std::equal(Input.begin(), Input.begin() + kTextOffset, Output.begin()));
  EXPECT_TRUE(std::equal(Input.begin() + kTextOffset + kTextSize, Input.end(),
                         Output.begin() + kTextOffset + kTextSize));
}

TEST(MachONativeInplace, RejectsArm64eIndependentlyOfCallerMetadata) {
  for (const SubtypeCase &Case : kSubtypeCases) {
    SCOPED_TRACE(Case.Name);
    for (CallerMetadata Metadata :
         {CallerMetadata::Complete, CallerMetadata::ClearedSubtype,
          CallerMetadata::Empty}) {
      SCOPED_TRACE(static_cast<unsigned>(Metadata));
      for (bool ExistingOutput : {false, true}) {
        SCOPED_TRACE(ExistingOutput);
        ASSERT_NO_FATAL_FAILURE(
            checkRewrite(Case.Subtype, Metadata, ExistingOutput, false));
      }
    }
  }
}

TEST(MachONativeInplace, OrdinaryARM64StillRewritesTheFunctionInPlace) {
  ASSERT_NO_FATAL_FAILURE(
      checkRewrite(kOrdinarySubtype, CallerMetadata::Complete, true, true));
}

} // namespace
} // namespace neverd
