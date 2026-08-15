//===- MachOARM32TransactionTests.cpp - ARM32 Mach-O EH transactions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/DwarfEHFrame.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/object/MachOLayout.h"
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
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace neverd {
namespace {

using namespace llvm::MachO;

using MergeFailure = MachOCompactUnwindMergeFailure;
using MergeInputKind = MachOCompactUnwindMergeInputKind;

constexpr uint32_t kARM32ImageBase = 0x10000000;
constexpr uint32_t kARM32TextOff = 0x1000;
constexpr uint32_t kARM32TextSize = 0x40;
constexpr uint32_t kARM32EHFrameOff = 0x2000;
constexpr uint32_t kARM32EHFrameSize = 4;
constexpr uint32_t kARM32UnwindOff = 0x3000;
constexpr uint32_t kARM32TextSegmentSize = 0x8000;
constexpr uint32_t kARM32DataOff = 0x8000;
constexpr uint32_t kARM32GOTOff = 0x9000;
constexpr uint32_t kARM32DataSegmentSize = 0x4000;
constexpr uint32_t kARM32LinkeditOff = 0xc000;
constexpr uint32_t kARM32LinkeditSize = 0x100;
constexpr uint32_t kARM32SymtabOff = kARM32LinkeditOff;
constexpr uint32_t kARM32StringTableOff = kARM32LinkeditOff + 0x20;
constexpr uint32_t kARM32IndirectTableOff = kARM32LinkeditOff + 0x40;
constexpr uint32_t kARM32FunctionVA = kARM32ImageBase + kARM32TextOff;
constexpr uint32_t kARM32PersonalityVA = kARM32FunctionVA + 0x10;
constexpr uint32_t kARM32MayThrowVA = kARM32FunctionVA + 0x20;
constexpr uint32_t kARM32UnwindResumeVA = kARM32FunctionVA + 0x30;
constexpr uint32_t kARM32PersonalitySlotVA = kARM32ImageBase + kARM32GOTOff;

void setName(char (&Destination)[16], llvm::StringRef Name) {
  ASSERT_LE(Name.size(), sizeof(Destination));
  std::memset(Destination, 0, sizeof(Destination));
  std::memcpy(Destination, Name.data(), Name.size());
}

void writeU16(std::vector<uint8_t> &Bytes, size_t Offset, uint16_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write16le(Bytes.data() + Offset, Value);
}

void writeU32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write32le(Bytes.data() + Offset, Value);
}

std::vector<uint8_t> makeRegularCompactUnwind(uint32_t Encoding) {
  constexpr uint32_t HeaderSize = 28;
  constexpr uint32_t IndexOffset = HeaderSize;
  constexpr uint32_t PageOffset = IndexOffset + 2 * 12;
  constexpr uint32_t PageSize = 8 + 8;
  std::vector<uint8_t> Bytes(PageOffset + PageSize, 0);

  writeU32(Bytes, 0, macho_unwind::kUnwindSectionVersion);
  writeU32(Bytes, 4, HeaderSize);
  writeU32(Bytes, 8, 0);
  writeU32(Bytes, 12, HeaderSize);
  writeU32(Bytes, 16, 0);
  writeU32(Bytes, 20, IndexOffset);
  writeU32(Bytes, 24, 2);

  writeU32(Bytes, IndexOffset, kARM32TextOff);
  writeU32(Bytes, IndexOffset + 4, PageOffset);
  writeU32(Bytes, IndexOffset + 8, PageOffset);
  writeU32(Bytes, IndexOffset + 12, kARM32TextOff + kARM32TextSize);
  writeU32(Bytes, IndexOffset + 16, 0);
  writeU32(Bytes, IndexOffset + 20, PageOffset);

  writeU32(Bytes, PageOffset, macho_unwind::kSecondLevelRegular);
  writeU16(Bytes, PageOffset + 4, 8);
  writeU16(Bytes, PageOffset + 6, 1);
  writeU32(Bytes, PageOffset + 8, kARM32TextOff);
  writeU32(Bytes, PageOffset + 12, Encoding);
  return Bytes;
}

struct ARM32TransactionFixture {
  std::vector<uint8_t> Binary;
  BinaryImage Image;
};

ARM32TransactionFixture makeARM32TransactionFixture(uint32_t OriginalEncoding,
                                                    size_t CompactCapacity) {
  ARM32TransactionFixture Fixture;
  const std::vector<uint8_t> OriginalCompact =
      makeRegularCompactUnwind(OriginalEncoding);
  if (CompactCapacity < OriginalCompact.size() ||
      CompactCapacity > kARM32TextSegmentSize - kARM32UnwindOff) {
    ADD_FAILURE() << "invalid ARM32 compact-unwind capacity";
    return Fixture;
  }

  constexpr uint32_t TextSectionCount = 3;
  constexpr uint32_t DataSectionCount = 1;
  constexpr uint32_t TextCommandSize =
      sizeof(segment_command) + TextSectionCount * sizeof(section);
  constexpr uint32_t DataCommandSize =
      sizeof(segment_command) + DataSectionCount * sizeof(section);
  constexpr uint32_t LinkeditCommandSize = sizeof(segment_command);
  constexpr uint32_t CommandSize =
      TextCommandSize + DataCommandSize + LinkeditCommandSize +
      sizeof(symtab_command) + sizeof(dysymtab_command);
  Fixture.Binary.assign(kARM32LinkeditOff + kARM32LinkeditSize, 0);

  auto *Header = reinterpret_cast<mach_header *>(Fixture.Binary.data());
  Header->magic = MH_MAGIC;
  Header->cputype = CPU_TYPE_ARM;
  Header->cpusubtype = CPU_SUBTYPE_ARM_V7K;
  Header->filetype = MH_EXECUTE;
  Header->ncmds = 5;
  Header->sizeofcmds = CommandSize;

  uint8_t *Command = Fixture.Binary.data() + sizeof(mach_header);
  auto *TextSegment = reinterpret_cast<segment_command *>(Command);
  TextSegment->cmd = LC_SEGMENT;
  TextSegment->cmdsize = TextCommandSize;
  setName(TextSegment->segname, section_names::macho::TextSeg);
  TextSegment->vmaddr = kARM32ImageBase;
  TextSegment->vmsize = kARM32TextSegmentSize;
  TextSegment->fileoff = 0;
  TextSegment->filesize = kARM32TextSegmentSize;
  TextSegment->maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
  TextSegment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
  TextSegment->nsects = TextSectionCount;

  auto *TextSections = reinterpret_cast<section *>(TextSegment + 1);
  setName(TextSections[0].sectname, section_names::macho::Text);
  setName(TextSections[0].segname, section_names::macho::TextSeg);
  TextSections[0].addr = kARM32FunctionVA;
  TextSections[0].size = kARM32TextSize;
  TextSections[0].offset = kARM32TextOff;
  TextSections[0].align = 2;
  TextSections[0].flags = static_cast<uint32_t>(S_REGULAR) |
                          static_cast<uint32_t>(S_ATTR_PURE_INSTRUCTIONS);

  setName(TextSections[1].sectname, section_names::macho::EhFrame);
  setName(TextSections[1].segname, section_names::macho::TextSeg);
  TextSections[1].addr = kARM32ImageBase + kARM32EHFrameOff;
  TextSections[1].size = kARM32EHFrameSize;
  TextSections[1].offset = kARM32EHFrameOff;
  TextSections[1].align = 2;
  TextSections[1].flags = S_REGULAR;

  setName(TextSections[2].sectname, section_names::macho::Unwind);
  setName(TextSections[2].segname, section_names::macho::TextSeg);
  TextSections[2].addr = kARM32ImageBase + kARM32UnwindOff;
  TextSections[2].size = static_cast<uint32_t>(CompactCapacity);
  TextSections[2].offset = kARM32UnwindOff;
  TextSections[2].align = 2;
  TextSections[2].flags = S_REGULAR;

  Command += TextCommandSize;
  auto *DataSegment = reinterpret_cast<segment_command *>(Command);
  DataSegment->cmd = LC_SEGMENT;
  DataSegment->cmdsize = DataCommandSize;
  setName(DataSegment->segname, "__DATA_CONST");
  DataSegment->vmaddr = kARM32ImageBase + kARM32DataOff;
  DataSegment->vmsize = kARM32DataSegmentSize;
  DataSegment->fileoff = kARM32DataOff;
  DataSegment->filesize = kARM32DataSegmentSize;
  DataSegment->maxprot = VM_PROT_READ | VM_PROT_WRITE;
  DataSegment->initprot = VM_PROT_READ | VM_PROT_WRITE;
  DataSegment->nsects = DataSectionCount;

  auto *DataSection = reinterpret_cast<section *>(DataSegment + 1);
  setName(DataSection->sectname, "__got");
  setName(DataSection->segname, "__DATA_CONST");
  DataSection->addr = kARM32PersonalitySlotVA;
  DataSection->size = 8;
  DataSection->offset = kARM32GOTOff;
  DataSection->align = 2;
  DataSection->flags = S_NON_LAZY_SYMBOL_POINTERS;

  Command += DataCommandSize;
  auto *LinkeditSegment = reinterpret_cast<segment_command *>(Command);
  LinkeditSegment->cmd = LC_SEGMENT;
  LinkeditSegment->cmdsize = LinkeditCommandSize;
  setName(LinkeditSegment->segname, section_names::macho::LinkeditSeg);
  LinkeditSegment->vmaddr = kARM32ImageBase + kARM32LinkeditOff;
  LinkeditSegment->vmsize = 0x4000;
  LinkeditSegment->fileoff = kARM32LinkeditOff;
  LinkeditSegment->filesize = kARM32LinkeditSize;
  LinkeditSegment->maxprot = VM_PROT_READ;
  LinkeditSegment->initprot = VM_PROT_READ;

  Command += LinkeditCommandSize;
  auto *Symtab = reinterpret_cast<symtab_command *>(Command);
  Symtab->cmd = LC_SYMTAB;
  Symtab->cmdsize = sizeof(symtab_command);
  Symtab->symoff = kARM32SymtabOff;
  Symtab->nsyms = 1;
  Symtab->stroff = kARM32StringTableOff;
  constexpr char PersonalityStringTable[] = "\0_test_personality";
  Symtab->strsize = sizeof(PersonalityStringTable);

  Command += sizeof(symtab_command);
  auto *Dysymtab = reinterpret_cast<dysymtab_command *>(Command);
  Dysymtab->cmd = LC_DYSYMTAB;
  Dysymtab->cmdsize = sizeof(dysymtab_command);
  Dysymtab->indirectsymoff = kARM32IndirectTableOff;
  Dysymtab->nindirectsyms = 2;

  for (uint32_t Offset = kARM32TextOff; Offset < kARM32TextOff + kARM32TextSize;
       Offset += sizeof(uint16_t))
    writeU16(Fixture.Binary, Offset, 0xbf00);
  std::fill(Fixture.Binary.begin() + kARM32UnwindOff,
            Fixture.Binary.begin() + kARM32UnwindOff + CompactCapacity, 0xa5);
  std::copy(OriginalCompact.begin(), OriginalCompact.end(),
            Fixture.Binary.begin() + kARM32UnwindOff);
  writeU32(Fixture.Binary, kARM32GOTOff, kARM32PersonalityVA);
  std::fill(Fixture.Binary.begin() + kARM32LinkeditOff, Fixture.Binary.end(),
            0x6c);
  auto *PersonalitySymbol =
      reinterpret_cast<nlist *>(Fixture.Binary.data() + kARM32SymtabOff);
  PersonalitySymbol->n_strx = 1;
  PersonalitySymbol->n_type =
      static_cast<uint8_t>(static_cast<uint8_t>(N_UNDF) | N_EXT);
  PersonalitySymbol->n_sect = NO_SECT;
  PersonalitySymbol->n_desc = 0;
  PersonalitySymbol->n_value = 0;
  std::memcpy(Fixture.Binary.data() + kARM32StringTableOff,
              PersonalityStringTable, sizeof(PersonalityStringTable));
  writeU32(Fixture.Binary, kARM32IndirectTableOff, 0);
  writeU32(Fixture.Binary, kARM32IndirectTableOff + sizeof(uint32_t),
           INDIRECT_SYMBOL_LOCAL);

  Fixture.Image.Format = BinaryFormat::MachO;
  Fixture.Image.Arch = Arch::ARM;
  Fixture.Image.Bits = Bitness::Bits32;
  Fixture.Image.Mode = InstructionMode::Thumb;
  Fixture.Image.Base = kARM32ImageBase;
  Fixture.Image.Raw = Fixture.Binary;

  Segment TextImageSegment;
  TextImageSegment.Name = section_names::macho::TextSeg;
  TextImageSegment.VA = kARM32ImageBase;
  TextImageSegment.Size = kARM32TextSegmentSize;
  TextImageSegment.FileOff = 0;
  TextImageSegment.FileSz = kARM32TextSegmentSize;
  TextImageSegment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  TextImageSegment.Data.assign(Fixture.Binary.begin(),
                               Fixture.Binary.begin() + kARM32TextSegmentSize);
  Fixture.Image.Segments.push_back(std::move(TextImageSegment));

  Segment DataImageSegment;
  DataImageSegment.Name = "__DATA_CONST";
  DataImageSegment.VA = kARM32ImageBase + kARM32DataOff;
  DataImageSegment.Size = kARM32DataSegmentSize;
  DataImageSegment.FileOff = kARM32DataOff;
  DataImageSegment.FileSz = kARM32DataSegmentSize;
  DataImageSegment.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  DataImageSegment.Data.assign(Fixture.Binary.begin() + kARM32DataOff,
                               Fixture.Binary.begin() + kARM32LinkeditOff);
  Fixture.Image.Segments.push_back(std::move(DataImageSegment));

  Segment LinkeditImageSegment;
  LinkeditImageSegment.Name = section_names::macho::LinkeditSeg;
  LinkeditImageSegment.VA = kARM32ImageBase + kARM32LinkeditOff;
  LinkeditImageSegment.Size = 0x4000;
  LinkeditImageSegment.FileOff = kARM32LinkeditOff;
  LinkeditImageSegment.FileSz = kARM32LinkeditSize;
  LinkeditImageSegment.Flags = SegmentFlags::Readable;
  LinkeditImageSegment.Data.assign(Fixture.Binary.begin() + kARM32LinkeditOff,
                                   Fixture.Binary.end());
  Fixture.Image.Segments.push_back(std::move(LinkeditImageSegment));

  auto AddSection = [&](llvm::StringRef Name, llvm::StringRef SegmentName,
                        uint32_t VA, uint32_t Size, uint32_t FileOff,
                        SegmentFlags Flags) {
    Section ImageSection;
    ImageSection.Name = Name.str();
    ImageSection.SegmentName = SegmentName.str();
    ImageSection.VA = VA;
    ImageSection.Size = Size;
    ImageSection.FileOff = FileOff;
    ImageSection.FileSz = Size;
    ImageSection.Flags = Flags;
    ImageSection.Data.assign(Fixture.Binary.begin() + FileOff,
                             Fixture.Binary.begin() + FileOff + Size);
    Fixture.Image.Sections.push_back(std::move(ImageSection));
  };
  AddSection(section_names::macho::Text, section_names::macho::TextSeg,
             kARM32FunctionVA, kARM32TextSize, kARM32TextOff,
             SegmentFlags::Readable | SegmentFlags::Executable);
  AddSection(section_names::macho::EhFrame, section_names::macho::TextSeg,
             kARM32ImageBase + kARM32EHFrameOff, kARM32EHFrameSize,
             kARM32EHFrameOff, SegmentFlags::Readable);
  AddSection(section_names::macho::Unwind, section_names::macho::TextSeg,
             kARM32ImageBase + kARM32UnwindOff,
             static_cast<uint32_t>(CompactCapacity), kARM32UnwindOff,
             SegmentFlags::Readable);
  AddSection("__got", "__DATA_CONST", kARM32PersonalitySlotVA, 8, kARM32GOTOff,
             SegmentFlags::Readable | SegmentFlags::Writable);

  Fixture.Image.Symbols.push_back(
      Symbol::makeFunc(kARM32FunctionVA, kARM32TextSize));
  Fixture.Image.KnownCodeRanges.push_back(
      {kARM32FunctionVA, kARM32FunctionVA + kARM32TextSize});
  Fixture.Image.Exports.push_back({"test_personality", 0, kARM32PersonalityVA});
  Fixture.Image.Exports.push_back({"may_throw", 0, kARM32MayThrowVA});
  Fixture.Image.Exports.push_back({"_Unwind_Resume", 0, kARM32UnwindResumeVA});
  Fixture.Image.ImportPtrSlots[kARM32PersonalitySlotVA] = "_test_personality";
  return Fixture;
}

std::unique_ptr<llvm::Module>
makeARM32TransactionModule(llvm::LLVMContext &Context) {
  auto Module =
      std::make_unique<llvm::Module>("arm32-macho-transaction", Context);
  auto *VoidFunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  auto *Personality = llvm::Function::Create(PersonalityType,
                                             llvm::GlobalValue::ExternalLinkage,
                                             "test_personality", Module.get());
  auto *MayThrow = llvm::Function::Create(VoidFunctionType,
                                          llvm::GlobalValue::ExternalLinkage,
                                          "may_throw", Module.get());
  auto *Function = llvm::Function::Create(
      VoidFunctionType, llvm::GlobalValue::ExternalLinkage,
      "lifted_arm32_source_function", Module.get());
  rewrite_source::setOriginalVA(*Function, kARM32FunctionVA);
  Function->setPersonalityFn(Personality);
  Function->setUWTableKind(llvm::UWTableKind::Default);
  Function->addFnAttr("frame-pointer", "all");

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Return =
      llvm::BasicBlock::Create(Context, "return", Function);
  llvm::BasicBlock *Landing =
      llvm::BasicBlock::Create(Context, "landing", Function);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateInvoke(MayThrow, Return, Landing);
  llvm::IRBuilder<>(Return).CreateRetVoid();

  llvm::IRBuilder<> LandingBuilder(Landing);
  auto *LandingPadType = llvm::StructType::get(
      llvm::PointerType::get(Context, 0), llvm::Type::getInt32Ty(Context));
  llvm::LandingPadInst *LandingPad =
      LandingBuilder.CreateLandingPad(LandingPadType, 0);
  LandingPad->setCleanup(true);
  LandingBuilder.CreateResume(LandingPad);
  return Module;
}

bool writeFile(llvm::StringRef Path, llvm::ArrayRef<uint8_t> Bytes) {
  std::error_code Error;
  llvm::raw_fd_ostream Stream(Path, Error, llvm::sys::fs::OF_None);
  if (Error)
    return false;
  Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  Stream.close();
  return !Stream.has_error();
}

std::vector<uint8_t> readFile(llvm::StringRef Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path);
  if (!Buffer) {
    ADD_FAILURE() << "cannot read fixture file: "
                  << Buffer.getError().message();
    return {};
  }
  llvm::StringRef Bytes = (*Buffer)->getBuffer();
  const auto *Begin = reinterpret_cast<const uint8_t *>(Bytes.data());
  return {Begin, Begin + Bytes.size()};
}

TEST(MachOARM32Transaction,
     RejectsEveryPartialFrameDPatternWithTypedMergeError) {
  for (uint32_t Pattern = 4; Pattern != 8; ++Pattern) {
    SCOPED_TRACE(Pattern);
    const uint32_t Encoding =
        macho_unwind::kARMModeFrameD |
        (Pattern << macho_unwind::kARMFrameDRegisterCountShift);
    auto Original =
        macho_unwind::parseCompactUnwindRaw(makeRegularCompactUnwind(Encoding));
    ASSERT_TRUE(static_cast<bool>(Original))
        << llvm::toString(Original.takeError());

    MachOCompactUnwindRecords Generated;
    Generated.TargetArch = Arch::ARM;
    Generated.PointerWidth = 4;
    Generated.ByteOrder = llvm::endianness::little;
    auto Merged = mergeMachOCompactUnwind(Arch::ARM, kARM32ImageBase, *Original,
                                          Generated, {});
    ASSERT_FALSE(static_cast<bool>(Merged));
    bool SawTypedError = false;
    llvm::Error Unhandled = llvm::handleErrors(
        Merged.takeError(), [&](const MachOCompactUnwindMergeError &Error) {
          SawTypedError = true;
          EXPECT_EQ(Error.reason(), MergeFailure::UnsupportedEncoding);
          EXPECT_EQ(Error.inputKind(), MergeInputKind::OriginalRecord);
          EXPECT_EQ(Error.inputIndex(), 0u);
        });
    if (Unhandled)
      ADD_FAILURE() << llvm::toString(std::move(Unhandled));
    EXPECT_TRUE(SawTypedError);
  }
}

TEST(MachOARM32Transaction,
     SuccessCommitsCompilerAuthenticatedEHAndCompactUnwind) {
  constexpr uint32_t OriginalEncoding =
      macho_unwind::kARMModeFrame | macho_unwind::kARMFrameFirstPushR4;
  ARM32TransactionFixture Fixture = makeARM32TransactionFixture(
      OriginalEncoding, /*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  const std::optional<MachOEHFrameRegion> BeforeEH =
      findMachOEHFrameRegion(Fixture.Binary);
  ASSERT_TRUE(BeforeEH.has_value());
  ASSERT_FALSE(BeforeEH->Is64);

  llvm::SmallString<128> InputPath;
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-arm32-macho-transaction-input", "macho", InputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-arm32-macho-transaction-output", "macho", OutputPath));
  llvm::FileRemover RemoveInput(InputPath);
  llvm::FileRemover RemoveOutput(OutputPath);
  ASSERT_TRUE(writeFile(InputPath, Fixture.Binary));
  ASSERT_TRUE(writeFile(OutputPath, {0x41, 0x52, 0x4d, 0x33, 0x32}));

  MachOLoader Loader;
  auto Loaded = Loader.load(InputPath.str().str());
  ASSERT_TRUE(static_cast<bool>(Loaded))
      << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Arch, Arch::ARM);
  EXPECT_EQ(Loaded->Mode, InstructionMode::Thumb);

  llvm::LLVMContext Context;
  auto Module = makeARM32TransactionModule(Context);
  MachOPatcher Patcher;
  Patcher.setImageContext(&Fixture.Image);
  PatchResult Result = Patcher.patch(
      InputPath.str().str(), OutputPath.str().str(), *Module, Arch::ARM);

  ASSERT_TRUE(Result.Success);
  EXPECT_EQ(Result.TrampolineCount, 1u);
  EXPECT_GT(Result.CodeSize, 0u);
  EXPECT_EQ(readFile(InputPath), Fixture.Binary);
  const std::vector<uint8_t> Output = readFile(OutputPath);
  ASSERT_GT(Output.size(), Fixture.Binary.size());
  const auto *Header = reinterpret_cast<const mach_header *>(Output.data());
  EXPECT_EQ(Header->magic, MH_MAGIC);
  EXPECT_EQ(Header->cputype, CPU_TYPE_ARM);
  EXPECT_EQ(static_cast<uint32_t>(Header->cpusubtype) &
                ~static_cast<uint32_t>(CPU_SUBTYPE_MASK),
            static_cast<uint32_t>(CPU_SUBTYPE_ARM_V7K));

  unsigned InjectedSegments = 0;
  uint64_t InjectedVA = 0;
  uint64_t ShiftedLinkeditOff = 0;
  uint32_t ShiftedSymtabOff = 0;
  uint32_t ShiftedStringTableOff = 0;
  uint32_t ShiftedIndirectTableOff = 0;
  forEachMachOLoadCommand(
      Output.data(), Output.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t, bool Is64) {
        EXPECT_FALSE(Is64);
        if (ID == LC_SYMTAB) {
          const auto *Symtab =
              reinterpret_cast<const symtab_command *>(Command);
          ShiftedSymtabOff = Symtab->symoff;
          ShiftedStringTableOff = Symtab->stroff;
          return;
        }
        if (ID == LC_DYSYMTAB) {
          const auto *Dysymtab =
              reinterpret_cast<const dysymtab_command *>(Command);
          ShiftedIndirectTableOff = Dysymtab->indirectsymoff;
          return;
        }
        if (ID != LC_SEGMENT)
          return;
        const MachOSegFields Segment = readMachOSegment(Command, Is64);
        const llvm::StringRef SegmentName = readMachOName(Segment.SegName);
        if (SegmentName == section_names::macho::LinkeditSeg) {
          ShiftedLinkeditOff = Segment.FileOff;
          return;
        }
        if (SegmentName != kDefaultNdTextSegment)
          return;
        ++InjectedSegments;
        InjectedVA = Segment.VMAddr;
        EXPECT_EQ(Segment.FileOff, kARM32LinkeditOff);
        EXPECT_EQ(Segment.InitProt,
                  static_cast<uint32_t>(VM_PROT_READ | VM_PROT_EXECUTE));
      });
  EXPECT_EQ(InjectedSegments, 1u);
  EXPECT_EQ(InjectedVA, kARM32ImageBase + kARM32LinkeditOff);

  ASSERT_GT(ShiftedLinkeditOff, kARM32LinkeditOff);
  ASSERT_EQ(ShiftedSymtabOff, ShiftedLinkeditOff);
  ASSERT_EQ(ShiftedStringTableOff, ShiftedLinkeditOff + 0x20);
  ASSERT_EQ(ShiftedIndirectTableOff, ShiftedLinkeditOff + 0x40);
  ASSERT_LE(ShiftedIndirectTableOff + 2 * sizeof(uint32_t), Output.size());
  const auto *ShiftedPersonalitySymbol =
      reinterpret_cast<const nlist *>(Output.data() + ShiftedSymtabOff);
  EXPECT_EQ(ShiftedPersonalitySymbol->n_strx, 1u);
  EXPECT_EQ(ShiftedPersonalitySymbol->n_type,
            static_cast<uint8_t>(static_cast<uint8_t>(N_UNDF) | N_EXT));
  EXPECT_STREQ(
      reinterpret_cast<const char *>(Output.data() + ShiftedStringTableOff + 1),
      "_test_personality");
  EXPECT_EQ(
      llvm::support::endian::read32le(Output.data() + ShiftedIndirectTableOff),
      0u);
  EXPECT_EQ(llvm::support::endian::read32le(
                Output.data() + ShiftedIndirectTableOff + sizeof(uint32_t)),
            INDIRECT_SYMBOL_LOCAL);

  ASSERT_LE(kARM32TextOff + kARM32TextSize, Output.size());
  size_t TrampolineLength = 0;
  const std::optional<va_t> TrampolineTarget = decodeDirectBranchTarget(
      Arch::ARM, InstructionMode::Thumb, Output.data() + kARM32TextOff,
      kARM32TextSize, kARM32FunctionVA, TrampolineLength);
  ASSERT_TRUE(TrampolineTarget.has_value());
  EXPECT_EQ(*TrampolineTarget, InjectedVA);
  EXPECT_EQ(TrampolineLength, 4u);
  EXPECT_TRUE(std::equal(Fixture.Binary.begin() + kARM32TextOff +
                            TrampolineLength,
                        Fixture.Binary.begin() + kARM32TextOff +
                            kARM32TextSize,
                        Output.begin() + kARM32TextOff + TrampolineLength));

  const std::optional<MachOEHFrameRegion> AfterEH =
      findMachOEHFrameRegion(Output);
  ASSERT_TRUE(AfterEH.has_value());
  EXPECT_FALSE(AfterEH->Is64);
  EXPECT_GT(AfterEH->AppendFileOff, BeforeEH->AppendFileOff);
  const uint64_t EHBytesSize =
      AfterEH->AppendFileOff - AfterEH->SectionFileOff + sizeof(uint32_t);
  ASSERT_LE(AfterEH->SectionFileOff + EHBytesSize, Output.size());
  auto FDEs = decodeDwarfEHFrameRecords(
      llvm::ArrayRef<uint8_t>(Output).slice(
          static_cast<size_t>(AfterEH->SectionFileOff),
          static_cast<size_t>(EHBytesSize)),
      AfterEH->SectionVA, /*Is64BitAddress=*/false);
  ASSERT_TRUE(static_cast<bool>(FDEs)) << llvm::toString(FDEs.takeError());
  ASSERT_EQ(FDEs->size(), 1u);
  EXPECT_EQ(FDEs->front().BeginVA, InjectedVA);
  EXPECT_GT(FDEs->front().EndVA, FDEs->front().BeginVA);

  auto CompactRegion = findMachOCompactUnwindRegion(Output);
  ASSERT_TRUE(static_cast<bool>(CompactRegion))
      << llvm::toString(CompactRegion.takeError());
  ASSERT_TRUE(CompactRegion->has_value());
  EXPECT_FALSE((*CompactRegion)->Is64);
  auto Installed =
      macho_unwind::parseCompactUnwindRaw(llvm::ArrayRef<uint8_t>(Output).slice(
          static_cast<size_t>((*CompactRegion)->SectionFileOff),
          static_cast<size_t>((*CompactRegion)->SectionSize)));
  ASSERT_TRUE(static_cast<bool>(Installed))
      << llvm::toString(Installed.takeError());

  bool SawOriginal = false;
  bool SawGeneratedDwarf = false;
  uint32_t GeneratedDwarfOffset = 0;
  for (const macho_unwind::CompactUnwindRawPage &Page : Installed->Pages) {
    for (const macho_unwind::CompactUnwindRawRegularEntry &Entry :
         Page.RegularEntries) {
      SawOriginal |= Entry.FunctionOffset == kARM32TextOff &&
                     Entry.Encoding == OriginalEncoding;
      if (Entry.FunctionOffset == kARM32LinkeditOff &&
          (Entry.Encoding & macho_unwind::kModeMask) ==
              macho_unwind::kARMModeDwarf) {
        SawGeneratedDwarf = true;
        GeneratedDwarfOffset =
            Entry.Encoding & macho_unwind::kDwarfSectionOffsetMask;
      }
    }
  }
  EXPECT_TRUE(SawOriginal);
  EXPECT_TRUE(SawGeneratedDwarf);
  ASSERT_GE(FDEs->front().RecordVA, AfterEH->SectionVA);
  EXPECT_EQ(GeneratedDwarfOffset,
            FDEs->front().RecordVA - AfterEH->SectionVA);
}

TEST(MachOARM32Transaction,
     ARMv7kModeMismatchLeavesInputAndOutputUnchanged) {
  ARM32TransactionFixture Fixture = makeARM32TransactionFixture(
      macho_unwind::kARMModeFrame | macho_unwind::kARMFrameFirstPushR4,
      /*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  Fixture.Image.Mode = InstructionMode::ARM;

  llvm::SmallString<128> InputPath;
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-arm32-macho-mode-input", "macho", InputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-arm32-macho-mode-output", "macho", OutputPath));
  llvm::FileRemover RemoveInput(InputPath);
  llvm::FileRemover RemoveOutput(OutputPath);
  const std::vector<uint8_t> OutputSentinel = {0x4d, 0x4f, 0x44, 0x45};
  ASSERT_TRUE(writeFile(InputPath, Fixture.Binary));
  ASSERT_TRUE(writeFile(OutputPath, OutputSentinel));

  llvm::LLVMContext Context;
  auto Module = makeARM32TransactionModule(Context);
  MachOPatcher Patcher;
  Patcher.setImageContext(&Fixture.Image);
  testing::internal::CaptureStderr();
  PatchResult Result = Patcher.patch(
      InputPath.str().str(), OutputPath.str().str(), *Module, Arch::ARM);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();

  EXPECT_FALSE(Result.Success);
  EXPECT_NE(Diagnostic.find("requires Thumb code-generation mode"),
            std::string::npos)
      << Diagnostic;
  EXPECT_EQ(readFile(InputPath), Fixture.Binary);
  EXPECT_EQ(readFile(OutputPath), OutputSentinel);
}

TEST(MachOARM32Transaction, PartialFrameDFailureLeavesInputAndOutputUnchanged) {
  const uint32_t PartialEncoding =
      macho_unwind::kARMModeFrameD |
      (7u << macho_unwind::kARMFrameDRegisterCountShift);
  ARM32TransactionFixture Fixture =
      makeARM32TransactionFixture(PartialEncoding, /*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());

  llvm::SmallString<128> InputPath;
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-arm32-macho-partial-input", "macho", InputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-arm32-macho-partial-output", "macho", OutputPath));
  llvm::FileRemover RemoveInput(InputPath);
  llvm::FileRemover RemoveOutput(OutputPath);
  const std::vector<uint8_t> OutputSentinel = {0x50, 0x41, 0x52, 0x54};
  ASSERT_TRUE(writeFile(InputPath, Fixture.Binary));
  ASSERT_TRUE(writeFile(OutputPath, OutputSentinel));

  llvm::LLVMContext Context;
  auto Module = makeARM32TransactionModule(Context);
  MachOPatcher Patcher;
  Patcher.setImageContext(&Fixture.Image);
  testing::internal::CaptureStderr();
  PatchResult Result = Patcher.patch(
      InputPath.str().str(), OutputPath.str().str(), *Module, Arch::ARM);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();

  EXPECT_FALSE(Result.Success);
  EXPECT_NE(Diagnostic.find("original record 0"), std::string::npos)
      << Diagnostic;
  EXPECT_NE(Diagnostic.find("encoding is incompatible"), std::string::npos)
      << Diagnostic;
  EXPECT_EQ(readFile(InputPath), Fixture.Binary);
  EXPECT_EQ(readFile(OutputPath), OutputSentinel);
}

} // namespace
} // namespace neverd
