#include "../../../lib/loader/MachO/MachOLocalFunction.h"
#include "../../../lib/pipeline/NativeSourcePreservation.h"
#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "../../../lib/sdk/capi/ObjCSourceProjection.h"
#include "../../../lib/sdk/capi/SourceRegisterCopyProjection.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/MachO/SourceRegisterCopy.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCClassGetterCalls.h"
#include "neverd/loader/ObjC/ObjCConstantStrings.h"
#include "neverd/pipeline/NativeSourceHints.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace neverd;
namespace {
struct CopyFixture {
  static constexpr va_t Root = 0x1000, Leaf = 0x1100;
  BinaryImage Image;
  llvm::LLVMContext Context;
  SourceFunctionTypeHint Signature;
  PipelineResult Result;

  void word(va_t Address, uint32_t Word) {
    llvm::support::endian::write32le(Image.Segments[0].Data.data() + Address,
                                     Word);
  }
  static uint32_t branch(va_t Site) {
    return 0x94000000 |
           (uint32_t((int64_t(Leaf) - int64_t(Site)) / 4) & 0x03ffffff);
  }
  CopyFixture(bool Swap = false, bool Twice = false) {
    using namespace llvm::MachO;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Entry = Root;
    Segment Text;
    Text.Size = Text.FileSz = 0x2000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    Image.Segments.push_back(std::move(Text));
    Section Code;
    Code.Name = "__text";
    Code.VA = Code.FileOff = Root;
    Code.Size = Code.FileSz = 0x300;
    Code.Flags = Image.Segments[0].Flags;
    Code.Type = S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Code);
    auto *Header =
        reinterpret_cast<mach_header_64 *>(Image.Segments[0].Data.data());
    Header->magic = MH_MAGIC_64;
    Header->cputype = CPU_TYPE_ARM64;
    Header->filetype = MH_EXECUTE;
    Header->ncmds = 2;
    Header->sizeofcmds = sizeof(segment_command_64) + sizeof(section_64) +
                         sizeof(symtab_command);
    auto *Segment = reinterpret_cast<segment_command_64 *>(Header + 1);
    Segment->cmd = LC_SEGMENT_64;
    Segment->cmdsize = sizeof(*Segment) + sizeof(section_64);
    std::strcpy(Segment->segname, "__TEXT");
    Segment->vmsize = Segment->filesize = 0x2000;
    Segment->maxprot = Segment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
    Segment->nsects = 1;
    auto *Section = reinterpret_cast<section_64 *>(Segment + 1);
    std::strcpy(Section->segname, "__TEXT");
    std::strcpy(Section->sectname, "__text");
    Section->addr = Section->offset = Root;
    Section->size = 0x300;
    Section->align = 2;
    Section->flags = S_ATTR_PURE_INSTRUCTIONS;
    auto *Symbols = reinterpret_cast<symtab_command *>(Section + 1);
    Symbols->cmd = LC_SYMTAB;
    Symbols->cmdsize = sizeof(*Symbols);
    Symbols->symoff = 0x1800;
    Symbols->nsyms = 1;
    Symbols->stroff = 0x1880;
    Symbols->strsize = 32;
    auto *Symbol =
        reinterpret_cast<nlist_64 *>(Image.Segments[0].Data.data() + 0x1800);
    Symbol->n_strx = 1;
    Symbol->n_type = N_SECT;
    Symbol->n_sect = 1;
    Symbol->n_value = Leaf;
    std::strcpy(
        reinterpret_cast<char *>(Image.Segments[0].Data.data() + 0x1881),
        "_copy_leaf");
    Image.Symbols = {{"copy_root", Root, 0, true},
                     {"copy_leaf", Leaf, 0, true}};
    Image.Exports = {{"copy_root", 0, Root}, {"copy_leaf", 0, Leaf}};
    std::vector<uint32_t> Body{0xa9be7bfd, 0xa90153f3, branch(Root + 8)};
    if (Twice)
      Body.push_back(branch(Root + 12));
    Body.insert(Body.end(),
                {0xcb140260, 0x8b020000, 0xa94153f3, 0xa8c27bfd, 0xd65f03c0});
    for (unsigned I = 0; I < Body.size(); ++I)
      word(Root + 4 * I, Body[I]);
    std::vector<uint32_t> Copies;
    if (Swap)
      Copies = {0xaa0003e9, 0xaa0103e0, 0xaa0903e1};
    Copies.insert(Copies.end(), {0xaa0103f3, 0xaa0003f4, 0xd65f03c0});
    for (unsigned I = 0; I < Copies.size(); ++I)
      word(Leaf + 4 * I, Copies[I]);
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = NdType::makeInt(8, false);
    for (unsigned I = 0; I < 3; ++I)
      Signature.Parameters.push_back(
          {"arg" + std::to_string(I), NdType::makeInt(8, false)});
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Image.Arch, Error));
    run();
  }
  void run(bool Typed = true) {
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Root};
    if (Typed)
      Options.SourceTypeHints.emplace(Root, Signature);
    Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
  }
  const LowFunc &low() const { return Result.LowFuncs.front(); }
  const MedFunc &med() const { return Result.MedFuncs.front(); }
  const HighFunc &high() const { return Result.HighFuncs.front(); }
  NativeSourceCalls calls() const {
    NativeSourceCalls Calls;
    for (const auto &[Site, Proof] : med().RegisterCopyProjections) {
      NativeSourceCallContract Contract;
      Contract.RegisterCopy = &Proof;
      Calls.emplace(Site, Contract);
    }
    return Calls;
  }
};

uint32_t pageAddress(va_t PC, va_t Address, unsigned Register) {
  const auto Delta =
      (int64_t(Address & ~va_t(0xfff)) - int64_t(PC & ~va_t(0xfff))) / 4096;
  const uint32_t Imm = uint32_t(Delta) & 0x1fffff;
  return 0x90000000 | ((Imm & 3) << 29) | ((Imm >> 2) << 5) | Register;
}
uint32_t completeAddress(unsigned Destination, unsigned Source, va_t Address) {
  return 0x91000000 | ((Address & 0xfff) << 10) | (Source << 5) | Destination;
}
void constantStrings(CopyFixture &F) {
  using namespace llvm::MachO;
  for (unsigned I = 0; I < 2; ++I) {
    Segment Data;
    Data.VA = Data.FileOff = 0x2000 + I * 0x1000;
    Data.Size = Data.FileSz = 0x1000;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.resize(Data.Size);
    F.Image.Segments.push_back(Data);
    Section Sec;
    Sec.VA = Sec.FileOff = Data.VA;
    Sec.Size = Sec.FileSz = Data.Size;
    Sec.Flags = Data.Flags;
    Sec.Name = I ? "__cstring" : "__cfstring";
    Sec.SegmentName = I ? "__TEXT" : "__DATA_CONST";
    Sec.Type = I ? S_CSTRING_LITERALS : S_REGULAR;
    F.Image.Sections.push_back(Sec);
    auto *Header =
        reinterpret_cast<mach_header_64 *>(F.Image.Segments[0].Data.data());
    auto *Command = reinterpret_cast<segment_command_64 *>(
        reinterpret_cast<uint8_t *>(Header + 1) + Header->sizeofcmds);
    ++Header->ncmds;
    Header->sizeofcmds += sizeof(*Command) + sizeof(section_64);
    Command->cmd = LC_SEGMENT_64;
    Command->cmdsize = sizeof(*Command) + sizeof(section_64);
    std::strcpy(Command->segname, Sec.SegmentName.c_str());
    Command->vmaddr = Command->fileoff = Data.VA;
    Command->vmsize = Command->filesize = Data.Size;
    Command->maxprot = Command->initprot = VM_PROT_READ;
    Command->nsects = 1;
    auto *Raw = reinterpret_cast<section_64 *>(Command + 1);
    std::strcpy(Raw->segname, Sec.SegmentName.c_str());
    std::strcpy(Raw->sectname, Sec.Name.c_str());
    Raw->addr = Raw->offset = Data.VA;
    Raw->size = Data.Size;
    Raw->flags = Sec.Type;
  }
  for (unsigned I = 0; I < 2; ++I) {
    const va_t Address = 0x2040 + I * 32;
    ASSERT_TRUE(F.Image.recordDyldBindSlot(
        Address, "___CFConstantStringClassReference", 0,
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
        false));
    auto *Record = F.Image.Segments[1].Data.data() + Address - 0x2000;
    llvm::support::endian::write64le(Record + 8, 0x7c8);
    llvm::support::endian::write64le(Record + 16, 0x3000 + I * 16);
    llvm::support::endian::write64le(Record + 24, 4);
    std::memcpy(F.Image.Segments[2].Data.data() + I * 16, I ? "file" : "test",
                5);
    ASSERT_TRUE(readObjCConstantString(F.Image, Address));
  }
}
void addressLeaf(CopyFixture &F) {
  const uint32_t Body[] = {0xaa0103f3,
                           0xaa0003f4,
                           pageAddress(F.Leaf + 8, 0x2040, 4),
                           completeAddress(4, 4, 0x2040),
                           pageAddress(F.Leaf + 16, 0x2060, 6),
                           completeAddress(6, 6, 0x2060),
                           0xd65f03c0};
  for (unsigned I = 0; I < std::size(Body); ++I)
    F.word(F.Leaf + I * 4, Body[I]);
}

TEST(SourceRegisterCopy, ExactConstantStringAddressesShareFreshMachineProof) {
  CopyFixture F(false, true);
  constantStrings(F);
  addressLeaf(F);
  F.run();
  ASSERT_EQ(F.med().RegisterCopyProjections.size(), 2U);
  for (const auto &[Site, Proof] : F.med().RegisterCopyProjections) {
    ASSERT_EQ(Proof.Registers.size(), 4U);
    const auto &Address =
        std::get<SourceConstantStringAddress>(Proof.Registers.at(4 * 8));
    EXPECT_EQ(Address.Address, 0x2040U);
    EXPECT_EQ(Address.ContentsAddress, 0x3000U);
    EXPECT_EQ(Address.Units, (std::vector<uint16_t>{'t', 'e', 's', 't'}));
  }
  EXPECT_TRUE(restoresNativeSourceState(F.low(), Arch::AArch64, F.calls()));
  EXPECT_TRUE(
      sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, F.Result));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&F.Image);
  Converter.setSourceCallHintsEnabled(true);
  const auto Med =
      Converter.convert(F.low(), Arch::AArch64, BinaryFormat::MachO);
  unsigned Addresses = 0;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        const auto &Input = Op.Inputs[I];
        if (Input.Kind != MedVar::Const ||
            (Input.ConstVal != 0x2040 && Input.ConstVal != 0x2060))
          continue;
        ++Addresses;
        EXPECT_EQ(Input.Provenance, ConstantAddressProvenance::DataAddress);
        EXPECT_EQ(Input.AddressOwnerVA, Input.ConstVal);
        EXPECT_EQ(Op.OriginSeq, -1);
        EXPECT_EQ(Op.CallSiteId, 0U);
      }
  EXPECT_EQ(Addresses, 4U);
}

TEST(SourceRegisterCopy, RejectsInvalidAddressChainsAndMutatedObjects) {
  CopyFixture F;
  constantStrings(F);
  addressLeaf(F);
  F.run();
  const auto Original = F.Image;
  for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    F.Image = Original;
    switch (Mutation) {
    case 0:
      F.word(F.Leaf + 12, completeAddress(4, 4, 0x2040) | (1U << 22));
      break;
    case 1:
      F.word(F.Leaf + 12, completeAddress(4, 4, 0x2040) & ~0x80000000U);
      break;
    case 2:
      F.word(F.Leaf + 12, completeAddress(4, 4, 0x2040) | (1U << 29));
      break;
    case 3:
      F.word(F.Leaf + 12, completeAddress(4, 0, 0x2040));
      break;
    case 4:
      F.word(F.Leaf + 12, completeAddress(5, 4, 0x2040));
      break; // Surviving page.
    case 5:
      F.word(F.Leaf + 8, pageAddress(F.Leaf + 8, F.Root, 4));
      break;
    case 6:
      F.word(F.Leaf + 12, completeAddress(4, 4, 0x2041));
      break;
    case 7:
      F.word(F.Leaf + 8, pageAddress(F.Leaf + 8, 0x9000, 4));
      break;
    case 8:
      F.word(F.Leaf + 8, pageAddress(F.Leaf + 8, 0x2040, 18));
      break;
    case 9:
      F.word(F.Leaf + 12, completeAddress(31, 4, 0x2040));
      break;
    case 10:
      F.word(F.Leaf + 12, completeAddress(4, 31, 0x2040));
      break;
    case 11:
      F.word(F.Leaf + 8, 0xd0ffffe4);
      break; // ADRP underflow.
    case 12:
      F.Image.DyldBindSlots.erase(0x2040);
      break;
    case 13:
      F.Image.Segments[1].Data[0x48] = 0;
      break;
    case 14:
      F.Image.CodePtrRelocSlots.insert(0x3000);
      break;
    case 15:
      F.Image.Segments[2].Data[0] = 'b';
      break;
    case 16:
      F.Image.Segments[2].Data[4] = 'b';
      break;
    case 17:
      F.Image.ConflictingImportStorageSlots.insert(0x2040);
      break;
    }
    EXPECT_FALSE(validateSourceRegisterCopies(F.Image, F.low(),
                                              F.med().RegisterCopyProjections));
    EXPECT_FALSE(
        sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, F.Result));
    if (Mutation != 15)
      EXPECT_TRUE(sourceRegisterCopies(F.Image, F.low()).empty());
  }
}

TEST(SourceRegisterCopy, PageCalculationUsesTheCalleePCAndRejectsOverflow) {
  using namespace llvm::MachO;
  for (va_t Base : {va_t(0), va_t(0x4000), va_t(0xffffffffffffc000)}) {
    SCOPED_TRACE(Base);
    CopyFixture F;
    constantStrings(F);
    // Move the text mapping independently from the object and file offsets.
    // The negative case is outside the caller page; the high case overflows.
    F.Image.Segments[0].VA = Base;
    F.Image.Sections[0].VA += Base;
    for (auto &Symbol : F.Image.Symbols)
      Symbol.Addr += Base;
    auto *Header =
        reinterpret_cast<mach_header_64 *>(F.Image.Segments[0].Data.data());
    auto *Segment = reinterpret_cast<segment_command_64 *>(Header + 1);
    Segment->vmaddr = Base;
    auto *Section = reinterpret_cast<section_64 *>(Segment + 1);
    Section->addr += Base;
    auto *Symbol =
        reinterpret_cast<nlist_64 *>(F.Image.Segments[0].Data.data() + 0x1800);
    Symbol->n_value += Base;
    const auto Entry = F.Leaf + Base;
    const uint32_t Page =
        Base > INT64_MAX ? 0xf0000024 : pageAddress(Entry, 0x2040, 4);
    F.word(F.Leaf, Page);
    F.word(F.Leaf + 4, completeAddress(4, 4, 0x2040));
    F.word(F.Leaf + 8, 0xd65f03c0);
    const auto Proof = sourceRegisterCopyLeafRegisters(F.Image, Entry);
    if (Base > INT64_MAX) {
      EXPECT_FALSE(Proof);
    } else {
      ASSERT_TRUE(Proof);
      EXPECT_EQ(std::get<SourceConstantStringAddress>(Proof->at(4 * 8)).Address,
                0x2040U);
    }
  }
}

TEST(SourceRegisterCopy, PageAndCompleteAddressCopiesRetainTheirKind) {
  CopyFixture F;
  constantStrings(F);
  const uint32_t Body[] = {pageAddress(F.Leaf, 0x2040, 4),
                           0xaa0403e5,
                           completeAddress(4, 4, 0x2040),
                           completeAddress(5, 5, 0x2060),
                           0xaa0403e6,
                           0xaa0003f5,
                           0xd65f03c0};
  for (unsigned I = 0; I < std::size(Body); ++I)
    F.word(F.Leaf + I * 4, Body[I]);
  const auto Proof = sourceRegisterCopyLeafRegisters(F.Image, F.Leaf);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->at(4 * 8), Proof->at(6 * 8));
  EXPECT_EQ(std::get<SourceConstantStringAddress>(Proof->at(5 * 8)).Address,
            0x2060U);
  EXPECT_EQ(std::get<SourceEntryRegister>(Proof->at(21 * 8)).Offset, 0U);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Leaf};
  auto Result = Pipeline().run(F.Image, F.Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Error;
  EXPECT_FALSE(inferNativeSourceTypeHint(
      F.Image, Result.MedFuncs.front(), Result.HighFuncs.front(),
      Result.FunctionAudits.front(), Error, &Result.LowFuncs.front()));
  EXPECT_NE(Error.find("private register outputs"), std::string::npos);
  F.word(F.Leaf + 12,
         completeAddress(5, 4, 0x2060)); // Completed base, not page.
  EXPECT_FALSE(sourceRegisterCopyLeafRegisters(F.Image, F.Leaf));
}

TEST(SourceRegisterCopy, ConstantWritesCannotRestoreAnEntryRegisterOrFrame) {
  CopyFixture F;
  constantStrings(F);
  F.word(F.Leaf, pageAddress(F.Leaf, 0x2040, 19));
  F.word(F.Leaf + 4, completeAddress(19, 19, 0x2040));
  F.word(F.Leaf + 8, 0xd65f03c0);
  F.run();
  ASSERT_EQ(F.med().RegisterCopyProjections.size(), 1U);
  EXPECT_TRUE(restoresNativeSourceState(F.low(), Arch::AArch64, F.calls()));
  F.word(F.Root + 20, 0xd503201f);
  F.run();
  EXPECT_FALSE(restoresNativeSourceState(F.low(), Arch::AArch64, F.calls()));
  auto Med = F.med();
  auto &Value =
      Med.RegisterCopyProjections.begin()->second.Registers.at(19 * 8);
  std::get<SourceConstantStringAddress>(Value).Address = 19 * 8;
  EXPECT_FALSE(validateSourceRegisterCopies(F.Image, F.low(),
                                            Med.RegisterCopyProjections));
}

TEST(SourceRegisterCopy, GeneratedAddressProjectionsExecuteAgainstFoundation) {
#if defined(__APPLE__) && defined(__aarch64__) && defined(NEVERD_TEST_CLANG)
  CopyFixture F(false, true);
  constantStrings(F);
  addressLeaf(F);
  std::vector<HighFunc> Functions;
  std::set<va_t> Strings;
  F.Signature.Parameters.resize(2);
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(F.Signature, Arch::AArch64, Error));
  for (unsigned Register : {4U, 6U}) {
    F.word(F.Root + 16, 0xaa0003e0 | (Register << 16)); // MOV x0, string.
    F.word(F.Root + 20, 0xd503201f);
    F.run();
    ASSERT_EQ(F.med().RegisterCopyProjections.size(), 2U);
    ASSERT_TRUE(
        sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, F.Result));
    auto Bound = sdk::bindObjCSourceReferences(F.high(), F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_EQ(Bound.ConstantStrings.size(), 1U);
    Bound.Function.Name = "string_" + std::to_string(Register);
    Strings.insert(Bound.ConstantStrings.begin(), Bound.ConstantStrings.end());
    Functions.push_back(std::move(Bound.Function));
  }
  std::string Source = "#include <CoreFoundation/CoreFoundation.h>\n";
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.EmitComments = false;
  ASSERT_TRUE(HighCEmitter().emit(Functions, Stream, Options));
  std::set<std::string> Shared;
  Source += sdk::renderObjCConstantStringHelpers(F.Image, Strings, Shared);
  Source += R"C(
int main(void) {
  CFStringRef first = (CFStringRef)string_4(11, 22);
  CFStringRef second = (CFStringRef)string_6(33, 44);
  if (first != (CFStringRef)string_4(55, 66) || second != (CFStringRef)string_6(77, 88)) return 1;
  if (first == second) return 2;
  if (CFStringCompare(first, CFSTR("test"), 0) != kCFCompareEqualTo) return 3;
  if (CFStringCompare(second, CFSTR("file"), 0) != kCFCompareEqualTo) return 4;
  return 0;
}
)C";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-address-leaf", Directory));
  const std::filesystem::path Work(Directory.c_str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  const auto Path = (Work / "source.c").string();
  const auto Executable = (Work / "source").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::ofstream(Path) << Source;
  for (const auto *Level : {"-O0", "-O2"}) {
    const std::string Compiler = NEVERD_TEST_CLANG;
    const std::vector<std::string> Arguments{
        Compiler,     "-std=gnu11", Level,        "-Werror",
        "-framework", "Foundation", "-framework", "CoreFoundation",
        Path,         "-o",         Executable};
    const std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, ErrorPath};
    const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                  Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "")
                         << Source;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                        Redirects, 30, 0, &Error),
              0)
        << Error << Source;
  }
#else
  GTEST_SKIP()
      << "Foundation runtime verification requires macOS arm64 and clang";
#endif
}

void classGetter(CopyFixture &F) {
  constantStrings(F);
  constexpr auto Foundation =
      "/System/Library/Frameworks/Foundation.framework/Foundation";
  F.Image.DynInfo.NeededLibs = {Foundation, "/usr/lib/libobjc.A.dylib"};
  F.Image.ObjCSourceReferences[0x2100] = {
      ObjCSourceReference::Kind::Class, 0x2100, 8, "NSSet", {}};
  F.Image.ImportPtrSlots[0x2100] = "_OBJC_CLASS_$_NSSet";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(0x2100, "_OBJC_CLASS_$_NSSet", 0,
                                         Foundation, false));
  F.word(F.Leaf, pageAddress(F.Leaf, 0x2100, 8));
  F.word(F.Leaf + 4, 0xf9408100); // LDR x0,[x8,#0x100].
  F.word(F.Leaf + 8, 0xd65f03c0);
  F.run();
}

TEST(SourceClassGetter, KeepsOrdinaryCallAndIndependentNativeBinding) {
  CopyFixture F(false, true);
  classGetter(F);
  const auto Facts = sourceClassGetterCalls(F.Image, F.low());
  ASSERT_EQ(Facts.size(), 2U);
  EXPECT_EQ(F.med().ClassGetterCallFacts, Facts);
  EXPECT_EQ(F.high().ClassGetterCallFacts, Facts);
  EXPECT_TRUE(F.med().RegisterCopyProjections.empty());
  EXPECT_TRUE(isImmutableImageClassImportSlot(F.Image, 0x2100));
  EXPECT_FALSE(isImmutableImageImportSlot(F.Image, 0x2100));
  EXPECT_FALSE(readImmutableImageBytes(F.Image, 0x2100, 8));
  unsigned Ordinary = 0;
  for (const auto &Block : F.med().Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL) {
        ++Ordinary;
        EXPECT_FALSE(Op.SourceCallHint);
      }
  EXPECT_EQ(Ordinary, 2U);
  EXPECT_TRUE(
      sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, F.Result));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&F.Image);
  EXPECT_TRUE(Converter.convert(F.low(), Arch::AArch64, BinaryFormat::MachO)
                  .ClassGetterCallFacts.empty());
}

TEST(SourceClassGetter, RejectsStaleMachineImportAndClassOwnership) {
  CopyFixture F;
  classGetter(F);
  ASSERT_EQ(F.med().ClassGetterCallFacts.size(), 1U);
  const auto Original = F.Image;
  for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    F.Image = Original;
    switch (Mutation) {
    case 0:
      F.word(F.Root + 8, F.branch(F.Root + 12));
      break;
    case 1:
      F.word(F.Leaf, pageAddress(F.Leaf, 0x2100, 9));
      break;
    case 2:
      F.word(F.Leaf + 4, 0xb9408100);
      break; // W0 load.
    case 3:
      F.word(F.Leaf + 4, 0xf9408101);
      break; // Wrong output.
    case 4:
      F.word(F.Leaf + 4, 0xf9408500);
      break; // Wrong slot.
    case 5:
      F.word(F.Leaf + 8, 0xd65f0100);
      break; // RET x8.
    case 6:
      F.Image.ObjCSourceReferences.at(0x2100).Name = "NSAssertionHandler";
      break;
    case 7:
      F.Image.DyldBindSlots.at(0x2100).Module = "/usr/lib/libobjc.A.dylib";
      break;
    case 8:
      F.Image.DyldBindSlots.at(0x2100).WeakImport = true;
      break;
    case 9:
      F.Image.DyldBindSlots.at(0x2100).Addend = 8;
      break;
    case 10:
      F.Image.ObjCSourceReferences.at(0x2100).TheKind =
          ObjCSourceReference::Kind::Metaclass;
      break;
    case 11:
      F.Image.Segments[1].Flags =
          F.Image.Segments[1].Flags | SegmentFlags::Writable;
      break;
    case 12:
      F.Image.CodePtrRelocSlots.insert(0x2100);
      break;
    case 13:
      F.Image.ConflictingImportStorageSlots.insert(0x2100);
      break;
    case 14:
      F.Image.ObjCSourceReferences[0x2104] =
          F.Image.ObjCSourceReferences.at(0x2100);
      break;
    case 15:
      F.Image.DyldBindSlots.erase(0x2100);
      break;
    case 16:
      F.Image.Segments[0].Data[0x1804] |= llvm::MachO::N_EXT;
      break;
    case 17:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    }
    EXPECT_TRUE(sourceClassGetterCalls(F.Image, F.low()).empty());
    EXPECT_FALSE(
        sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, F.Result));
  }
}

TEST(SourceClassGetter, PublicationRequiresFreshCallFactsAndOneOrdinaryCall) {
  CopyFixture F;
  classGetter(F);
  ASSERT_EQ(F.med().ClassGetterCallFacts.size(), 1U);
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    PipelineResult Changed;
    Changed.SourceImage = &F.Image;
    Changed.LowFuncs = F.Result.LowFuncs;
    Changed.MedFuncs = F.Result.MedFuncs;
    auto High = F.high();
    if (Mutation == 0)
      High.ClassGetterCallFacts.clear();
    if (Mutation == 1)
      Changed.MedFuncs.front().ClassGetterCallFacts.clear();
    if (Mutation == 2)
      ++High.ClassGetterCallFacts.begin()->second.LeafWords[1];
    if (Mutation == 3)
      Changed.LowFuncs.front().Blocks.front().InstructionBoundaries.clear();
    if (Mutation == 4)
      Changed.MedFuncs.push_back(Changed.MedFuncs.front());
    if (Mutation >= 5) {
      auto &Ops = Changed.MedFuncs.front().Blocks.front().Ops;
      auto Call = std::find_if(Ops.begin(), Ops.end(), [](const auto &Op) {
        return Op.Opcode == NdOp::CALL;
      });
      ASSERT_NE(Call, Ops.end());
      if (Mutation == 5)
        Ops.erase(Call);
      else {
        const auto Copy = *Call;
        Ops.push_back(Copy);
      }
    }
    EXPECT_FALSE(
        sdk::sourceRegisterCopyProjectionValid(High, F.Image, Changed));
  }
}

TEST(SourceClassGetter, PreservesFreshFrameFactsButCannotUndoAnEarlierEscape) {
  CopyFixture F;
  classGetter(F);
  F.Image.ObjCSourceReferences[0x2110] = {
      ObjCSourceReference::Kind::Selector, 0x2110, 8, "setWithObjects:", {}};
  F.Image.ImportPtrSlots[0x2180] = "_objc_msgSend";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(0x2180, "_objc_msgSend", 0,
                                         "/usr/lib/libobjc.A.dylib", false));
  const uint32_t Stub[] = {0xb0000001, 0xf9408821, 0xb0000010, 0xf940c210,
                           0xd61f0200};
  for (unsigned I = 0; I < std::size(Stub); ++I)
    F.word(0x1280 + I * 4, Stub[I]);
  F.word(0x1260, 0xd65f03c0);
  std::vector<uint32_t> Body = {0xd100c3ff,
                                0xa9027bfd,
                                0xa90153f3,
                                0xd503201f,
                                0xd503201f,
                                F.branch(F.Root + 20),
                                0xaa0003f3,
                                0xaa1303e0,
                                pageAddress(F.Root + 32, 0x2040, 2),
                                completeAddress(2, 2, 0x2040),
                                pageAddress(F.Root + 40, 0x2060, 8),
                                completeAddress(8, 8, 0x2060),
                                0xf90003e8,
                                0xf90007ff,
                                0x94000000u | ((0x1280 - (F.Root + 56)) / 4),
                                0xa94153f3,
                                0xa9427bfd,
                                0x9100c3ff,
                                0xd65f03c0};
  for (unsigned I = 0; I < Body.size(); ++I)
    F.word(F.Root + I * 4, Body[I]);
  F.run();
  const auto Hints = buildObjCSourceCallHints(F.Image, F.low());
  const auto Found = Hints.find(F.Root + 56);
  ASSERT_NE(Found, Hints.end());
  ASSERT_TRUE(Found->second.Receiver);
  EXPECT_EQ(Found->second.Receiver->ClassName, "NSSet");
  ASSERT_TRUE(Found->second.NilTerminated);
  EXPECT_EQ(Found->second.NilTerminated->Objects,
            (std::vector<va_t>{0x2040, 0x2060}));
  EXPECT_TRUE(
      sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, F.Result));
  // Exposing this frame to an earlier unknown call remains an escape even
  // though the later getter itself cannot consume a frame pointer.
  F.word(F.Root + 12, 0x910003e0); // MOV x0,sp.
  F.word(F.Root + 16, 0x94000000u | ((0x1260 - (F.Root + 16)) / 4));
  F.run();
  const auto Escaped = buildObjCSourceCallHints(F.Image, F.low());
  const auto Later = Escaped.find(F.Root + 56);
  EXPECT_TRUE(Later == Escaped.end() || !Later->second.NilTerminated);
}

TEST(SourceRegisterCopy, ExactLocalLeafKeepsPhysicalEffectsAndOriginalCalls) {
  CopyFixture F;
  ASSERT_EQ(F.Result.LowFuncs.size(), 1U);
  const auto Copies = sourceRegisterCopies(F.Image, F.low());
  ASSERT_EQ(Copies.size(), 1U);
  EXPECT_EQ(Copies.begin()->second.Registers,
            (SourceRegisterValues{{19 * 8, SourceEntryRegister{8}},
                                  {20 * 8, SourceEntryRegister{0}}}));
  EXPECT_EQ(F.med().RegisterCopyProjections, Copies);
  EXPECT_EQ(F.high().RegisterCopyProjections, Copies);
  EXPECT_TRUE(
      sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, F.Result));
  EXPECT_TRUE(restoresNativeSourceState(F.low(), F.Image.Arch, F.calls()));
  unsigned OriginalCalls = 0, ProjectedCalls = 0;
  for (const auto &Block : F.low().Blocks)
    for (const auto &Op : Block.Ops)
      OriginalCalls += Op.Opcode == NdOp::CALL;
  for (const auto &Block : F.med().Blocks)
    for (const auto &Op : Block.Ops)
      ProjectedCalls += Op.Opcode == NdOp::CALL;
  EXPECT_EQ(OriginalCalls, 1U);
  EXPECT_EQ(ProjectedCalls, 0U);
  EXPECT_TRUE(sdk::sourceBodyLimitation(F.high(), F.Signature,
                                        &F.Result.FunctionAudits.front())
                  .empty());
  LowToMedConverter Generic;
  Generic.setBinaryImage(&F.Image);
  auto Unprojected =
      Generic.convert(F.low(), Arch::AArch64, BinaryFormat::MachO);
  EXPECT_TRUE(Unprojected.RegisterCopyProjections.empty());
  unsigned GenericCalls = 0;
  for (const auto &Block : Unprojected.Blocks)
    for (const auto &Op : Block.Ops)
      GenericCalls += Op.Opcode == NdOp::CALL;
  EXPECT_EQ(GenericCalls, 1U);
}

TEST(SourceRegisterCopy, SequentialAliasesNormalizeToEntryAndBindEveryCall) {
  CopyFixture F(true, true);
  const auto Copies = sourceRegisterCopies(F.Image, F.low());
  ASSERT_EQ(Copies.size(), 2U);
  for (const auto &[Site, Proof] : Copies) {
    EXPECT_EQ(Proof.Registers,
              (SourceRegisterValues{{0, SourceEntryRegister{8}},
                                    {8, SourceEntryRegister{0}},
                                    {9 * 8, SourceEntryRegister{0}},
                                    {19 * 8, SourceEntryRegister{0}},
                                    {20 * 8, SourceEntryRegister{8}}}));
    EXPECT_EQ(Proof.Site, Site);
    EXPECT_EQ(Proof.Caller, F.Root);
  }
  EXPECT_NE(Copies.begin()->first, Copies.rbegin()->first);
  EXPECT_TRUE(restoresNativeSourceState(F.low(), Arch::AArch64, F.calls()));
}

TEST(SourceRegisterCopy, RejectsChangedMachineLinkageAndUnsupportedEffects) {
  CopyFixture F;
  const auto Original = F.Image;
  for (unsigned Mutation = 0; Mutation < 23; ++Mutation) {
    SCOPED_TRACE(Mutation);
    F.Image = Original;
    switch (Mutation) {
    case 0:
      F.word(F.Leaf, 0x2a0103f3);
      break; // W-register copy.
    case 1:
      F.word(F.Leaf, 0xaa1e03f3);
      break; // Read LR.
    case 2:
      F.word(F.Leaf, 0xaa0103fe);
      break; // Write LR.
    case 3:
      F.word(F.Leaf, 0xaa1d03f3);
      break; // Read FP.
    case 4:
      F.word(F.Leaf, 0xaa1203f3);
      break; // Read platform register.
    case 5:
      F.word(F.Leaf, 0xaa0103ff);
      break; // Zero register.
    case 6:
      F.word(F.Leaf, 0x910003f3);
      break; // Copy SP alias is ADD.
    case 7:
      F.word(F.Leaf, 0xf9400013);
      break; // Load.
    case 8:
      F.word(F.Leaf, 0x94000000);
      break; // Nested call.
    case 9:
      F.word(F.Leaf + 8, 0xd65f0260);
      break; // RET x19.
    case 10:
      F.word(F.Leaf + 8, 0xd503201f);
      break; // Incomplete leaf.
    case 11:
      F.word(F.Root + 8, 0x1400003e);
      break; // B, not BL.
    case 12:
      F.word(F.Root + 8, CopyFixture::branch(F.Root + 12));
      break;
    case 13:
      F.Image.Segments[0].Flags =
          F.Image.Segments[0].Flags | SegmentFlags::Writable;
      break;
    case 14:
      F.Image.Segments.push_back(F.Image.Segments.front());
      break;
    case 15:
      F.Image.Sections.push_back(F.Image.Sections.front());
      break;
    case 16:
      F.Image.CodePtrRelocSlots.insert(F.Leaf + 4);
      break;
    case 17:
      F.Image.CodePtrRelocSlots.insert(F.Root + 8);
      break;
    case 18:
      F.Image.Segments[0].Data[0x1804] |= llvm::MachO::N_EXT;
      break;
    case 19:
      F.Image.Segments[0].Data[0x1808] += 4;
      break;
    case 20:
      F.Image.Segments[0].Data.resize(F.Leaf + 8);
      break;
    case 21:
      F.Image.IsRelocatable = true;
      break;
    case 22:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    }
    EXPECT_TRUE(sourceRegisterCopies(F.Image, F.low()).empty());
    EXPECT_FALSE(
        sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, F.Result));
  }
}

TEST(SourceRegisterCopy, RejectsStaleCallerBoundariesAndProjectionReceipts) {
  CopyFixture F;
  for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Low = F.low();
    auto Copies = F.med().RegisterCopyProjections;
    if (Mutation == 0)
      Low.TruncatedPathAddresses.push_back(F.Root + 8);
    if (Mutation == 1)
      ++Low.LiftedInstructionCount;
    if (Mutation == 2)
      Low.Blocks.front().InstructionBoundaries.clear();
    if (Mutation == 3)
      ++Low.Entry;
    if (Mutation == 4)
      Copies.begin()->second.Registers[19 * 8] = SourceEntryRegister{0};
    if (Mutation == 5)
      ++Copies.begin()->second.Site.Sequence;
    if (Mutation == 6)
      ++Copies.begin()->second.CallWord;
    if (Mutation == 7)
      ++Copies.begin()->second.LeafWords.front();
    if (Mutation == 8)
      for (auto &Block : Low.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL)
            Op.Output = NdVar::reg(8, 8);
    if (Mutation == 9)
      for (auto &Block : Low.Blocks)
        for (auto &Boundary : Block.InstructionBoundaries)
          if (Boundary.Control == LowInstructionControl::Call)
            Boundary.ControlFlags |= LowInstructionControlFlag::NoReturn;
    EXPECT_FALSE(validateSourceRegisterCopies(F.Image, Low, Copies));
  }
  auto Changed = F.high();
  Changed.RegisterCopyProjections.clear();
  EXPECT_FALSE(
      sdk::sourceRegisterCopyProjectionValid(Changed, F.Image, F.Result));
  auto Calls = F.calls();
  Calls.begin()->second.Signature = &F.Signature;
  EXPECT_FALSE(restoresNativeSourceState(F.low(), Arch::AArch64, Calls));
  auto Low = F.low();
  for (auto &Block : Low.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Output == NdVar::reg(30 * 8, 8) && Op.Opcode == NdOp::COPY &&
          Op.NumInputs == 1 && !Op.Inputs[0].isConst())
        Op.Inputs[0] = NdVar::cst(0, 8);
  EXPECT_FALSE(restoresNativeSourceState(Low, Arch::AArch64, F.calls()));
}

TEST(SourceRegisterCopy, CoalescedAliasesMustAllBeLocalAndAtTheSameEntry) {
  CopyFixture F;
  using namespace llvm::MachO;
  auto &Bytes = F.Image.Segments[0].Data;
  auto *Symbols = reinterpret_cast<symtab_command *>(
      Bytes.data() + sizeof(mach_header_64) + sizeof(segment_command_64) +
      sizeof(section_64));
  auto *Names = reinterpret_cast<nlist_64 *>(Bytes.data() + Symbols->symoff);
  Symbols->nsyms = 2;
  Names[1] = Names[0];
  EXPECT_FALSE(isMachOLocalFunctionRange(F.Image, F.Leaf, 12));
  EXPECT_EQ(sourceRegisterCopies(F.Image, F.low()).size(), 1U);
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Names[1] = Names[0];
    if (Mutation == 0)
      Names[1].n_type |= N_EXT;
    if (Mutation == 1)
      Names[1].n_value += 4;
    if (Mutation == 2)
      Names[1].n_sect = 0;
    if (Mutation == 3)
      Names[1].n_strx = 0;
    EXPECT_TRUE(sourceRegisterCopies(F.Image, F.low()).empty());
  }
}

TEST(SourceRegisterCopy, PublicationRejectsDuplicateAndMissingMachineOwners) {
  CopyFixture F;
  auto Snapshot = [&] {
    PipelineResult Result;
    Result.SourceImage = &F.Image;
    Result.LowFuncs = F.Result.LowFuncs;
    Result.MedFuncs = F.Result.MedFuncs;
    return Result;
  };
  auto Changed = Snapshot();
  Changed.LowFuncs.push_back(F.low());
  EXPECT_FALSE(
      sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, Changed));
  Changed = Snapshot();
  Changed.MedFuncs.push_back(F.med());
  EXPECT_FALSE(
      sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, Changed));
  Changed = Snapshot();
  Changed.LowFuncs.clear();
  EXPECT_FALSE(
      sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, Changed));
  Changed = Snapshot();
  const auto Site = F.med().RegisterCopyProjections.begin()->first;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Site.Instruction;
  Call.OriginSeq = Site.Sequence;
  Call.addInput(MedVar::makeConst(*Site.StaticTarget, 8));
  Changed.MedFuncs.front().Blocks.front().Ops.push_back(Call);
  EXPECT_FALSE(
      sdk::sourceRegisterCopyProjectionValid(F.high(), F.Image, Changed));
  auto Low = F.low();
  Low.Blocks.push_back(Low.Blocks.front());
  EXPECT_FALSE(validateSourceRegisterCopies(F.Image, Low,
                                            F.med().RegisterCopyProjections));
}

TEST(SourceRegisterCopy, NativeInferenceObservesInputsThroughProjectedCalls) {
  CopyFixture F;
  F.run(false);
  std::string Error;
  const auto Hint = inferNativeSourceTypeHint(F.Image, F.med(), F.high(),
                                              F.Result.FunctionAudits.front(),
                                              Error, &F.low());
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 3U);
  for (unsigned I = 0; I < 3; ++I)
    EXPECT_EQ(Hint->Parameters[I].Location.RegisterOffset, 8 * I);
  auto Med = F.med();
  Med.RegisterCopyProjections.begin()->second.Registers[19 * 8] =
      SourceEntryRegister{0};
  EXPECT_FALSE(inferNativeSourceTypeHint(F.Image, Med, F.high(),
                                         F.Result.FunctionAudits.front(), Error,
                                         &F.low()));
}

TEST(SourceRegisterCopy, PrivateLeafCannotDeclareAnOrdinaryCABI) {
  CopyFixture F;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Leaf};
  auto Result = Pipeline().run(F.Image, F.Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.LowFuncs.size(), 1U);
  ASSERT_EQ(Result.MedFuncs.size(), 1U);
  ASSERT_EQ(Result.HighFuncs.size(), 1U);
  std::string Error;
  EXPECT_FALSE(inferNativeSourceTypeHint(
      F.Image, Result.MedFuncs.front(), Result.HighFuncs.front(),
      Result.FunctionAudits.front(), Error, &Result.LowFuncs.front()));
  EXPECT_NE(Error.find("private register outputs"), std::string::npos);
}

TEST(SourceRegisterCopy, NativeInferenceRejectsUnrestoredPrivateOutputs) {
  CopyFixture F;
  F.word(F.Root + 20, 0xd503201f); // Remove x19/x20 restoration.
  F.run(false);
  ASSERT_EQ(F.med().RegisterCopyProjections.size(), 1U);
  std::string Error;
  EXPECT_FALSE(inferNativeSourceTypeHint(F.Image, F.med(), F.high(),
                                         F.Result.FunctionAudits.front(), Error,
                                         &F.low()));
  EXPECT_NE(Error.find("restore native call state"), std::string::npos);
}

TEST(SourceRegisterCopy, ReceiverIdentitySurvivesPrivateRegisterConvention) {
  CopyFixture F;
  using namespace llvm::MachO;
  Segment Data;
  Data.VA = Data.FileOff = 0x2000;
  Data.Size = Data.FileSz = 0x1000;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  F.Image.Segments.push_back(Data);
  auto &Bytes = F.Image.Segments[0].Data;
  auto *Header = reinterpret_cast<mach_header_64 *>(Bytes.data());
  auto *Command = reinterpret_cast<segment_command_64 *>(
      Bytes.data() + sizeof(*Header) + Header->sizeofcmds);
  ++Header->ncmds;
  Header->sizeofcmds += sizeof(*Command);
  Command->cmd = LC_SEGMENT_64;
  Command->cmdsize = sizeof(*Command);
  std::strcpy(Command->segname, "__DATA");
  Command->vmaddr = Command->fileoff = 0x2000;
  Command->vmsize = Command->filesize = 0x1000;
  Command->maxprot = Command->initprot = VM_PROT_READ | VM_PROT_WRITE;
  Section Section;
  Section.VA = Section.FileOff = 0x2000;
  Section.Size = Section.FileSz = 0x1000;
  Section.Flags = Data.Flags;
  // The local-linkage parser owns section inventory, so encode this section
  // in the added command as well as in the image model.
  F.Image.Sections.push_back(Section);
  Command->nsects = 1;
  Command->cmdsize += sizeof(section_64);
  Header->sizeofcmds += sizeof(section_64);
  auto *Raw = reinterpret_cast<section_64 *>(Command + 1);
  std::strcpy(Raw->segname, "__DATA");
  std::strcpy(Raw->sectname, "__data");
  Raw->addr = Raw->offset = 0x2000;
  Raw->size = 0x1000;
  F.Image.ObjCSourceReferences[0x2040] = {
      ObjCSourceReference::Kind::Class, 0x2040, 8, "NSAssertionHandler", {}};
  F.Image.ObjCSourceReferences[0x2048] = {
      ObjCSourceReference::Kind::Selector, 0x2048, 8, "currentHandler", {}};
  F.Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  F.Image.ImportPtrSlots[0x2080] = "_objc_msgSend";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(0x2080, "_objc_msgSend", 0,
                                         "/usr/lib/libobjc.A.dylib", false));
  auto Load = [](va_t Site, va_t Target, unsigned Register) {
    return 0x58000000u | (((Target - Site) / 4) << 5) | Register;
  };
  const uint32_t Body[] = {0xa9be7bfd,
                           0xa90153f3,
                           Load(F.Root + 8, 0x2040, 0),
                           F.branch(F.Root + 12),
                           0xaa1303e0,
                           Load(F.Root + 20, 0x2048, 1),
                           0x94000000u | ((0x1280 - (F.Root + 24)) / 4),
                           0xa94153f3,
                           0xa8c27bfd,
                           0xd65f03c0};
  for (unsigned I = 0; I < std::size(Body); ++I)
    F.word(F.Root + I * 4, Body[I]);
  F.word(F.Leaf, 0xaa0003f3);     // x19 receives class identity.
  F.word(F.Leaf + 4, 0xaa0103e0); // x0 is overwritten independently.
  F.word(0x1280, 0xb0000010);
  F.word(0x1284, 0xf9404210);
  F.word(0x1288, 0xd61f0200);
  F.run();
  ASSERT_EQ(F.med().RegisterCopyProjections.size(), 1U);
  const auto Hints = buildObjCSourceCallHints(F.Image, F.low());
  const auto Found = Hints.find(F.Root + 24);
  ASSERT_NE(Found, Hints.end());
  ASSERT_TRUE(Found->second.Receiver);
  EXPECT_EQ(Found->second.Receiver->ClassName, "NSAssertionHandler");
  EXPECT_EQ(Found->second.Selector, "currentHandler");
  EXPECT_EQ(Found->second.CallKind, SourceCallTypeHint::Kind::ObjCMessage);
  F.word(F.Leaf, 0xd503201f); // No longer an authenticated copy leaf.
  const auto Changed = buildObjCSourceCallHints(F.Image, F.low());
  EXPECT_TRUE(!Changed.count(F.Root + 24) || !Changed.at(F.Root + 24).Receiver);
}

TEST(SourceRegisterCopy, GeneratedCExecutesSequentialCopiesAndPreservesInputs) {
#ifndef NEVERD_TEST_CLANG
  GTEST_SKIP() << "clang is unavailable";
#else
  for (bool Swap : {false, true})
    for (bool Twice : {false, true}) {
      CopyFixture F(Swap, Twice);
      ASSERT_EQ(F.med().RegisterCopyProjections.size(), Twice ? 2U : 1U);
      auto High = F.high();
      High.Name = "copy_root";
      std::string Source;
      llvm::raw_string_ostream Stream(Source);
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.EmitComments = false;
      ASSERT_TRUE(HighCEmitter().emit({High}, Stream, Options));
      Source += R"C(
int main(void) {
  const uint64_t values[] = {0, 1, 17, 0x8000000000000000ULL,
                             0xffffffffffffffffULL, 0xabcdef0123456789ULL};
  for (unsigned i=0; i<6; ++i) for (unsigned j=0; j<6; ++j)
    for (unsigned k=0; k<6; ++k) {
      uint64_t a=values[i], b=values[j], c=values[k];
)C";
      Source += std::string("      uint64_t expected = ") +
                (Swap && !Twice ? "a - b + c;" : "b - a + c;") +
                "\n      if (copy_root(a,b,c) != expected) return 1;\n"
                "    }\n  return 0;\n}\n";
      llvm::SmallString<128> Directory;
      ASSERT_FALSE(
          llvm::sys::fs::createUniqueDirectory("neverd-copy-leaf", Directory));
      const std::filesystem::path Work(Directory.c_str());
      struct Cleanup {
        std::filesystem::path Work;
        ~Cleanup() {
          std::error_code Error;
          std::filesystem::remove_all(Work, Error);
        }
      } Cleanup{Work};
      const auto Path = (Work / "source.c").string();
      const auto Executable = (Work / "source").string();
      const auto ErrorPath = (Work / "stderr").string();
      std::ofstream(Path) << Source;
      for (const auto *Level : {"-O0", "-O2"}) {
        const std::string Compiler = NEVERD_TEST_CLANG;
        const std::vector<std::string> Arguments{
            Compiler, "-std=c11", Level, "-Werror", Path, "-o", Executable};
        const std::vector<llvm::StringRef> Refs(Arguments.begin(),
                                                Arguments.end());
        const std::optional<llvm::StringRef> Redirects[] = {
            std::nullopt, std::nullopt, ErrorPath};
        std::string Error;
        const auto Status = llvm::sys::ExecuteAndWait(
            Compiler, Refs, std::nullopt, Redirects, 60, 0, &Error);
        auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
        ASSERT_EQ(Status, 0)
            << Error << (Errors ? (*Errors)->getBuffer().str() : "") << Source;
        EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable},
                                            std::nullopt, Redirects, 30, 0,
                                            &Error),
                  0)
            << Error << Source;
      }
    }
#endif
}
} // namespace
