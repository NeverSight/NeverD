//===- MachOPointerRelocationBoundaryTests.cpp ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;
using namespace llvm::MachO;

constexpr va_t TextVA = 0x100000000ULL;
constexpr va_t DataVA = TextVA + 0x4000;
constexpr va_t WritableVA = DataVA + 0x1000;
constexpr va_t CStringVA = TextVA + 0x580;
constexpr va_t CStringBVA = TextVA + 0x590;
constexpr va_t CodeVA = TextVA + 0x500;
constexpr va_t ImportStubVA = TextVA + 0x540;
constexpr va_t CallerVA = TextVA + 0x420;

template <typename T>
void writeObject(std::vector<uint8_t> &Bytes, size_t Off, const T &Value) {
  ASSERT_LE(Off + sizeof(T), Bytes.size());
  std::memcpy(Bytes.data() + Off, &Value, sizeof(T));
}

BinaryImage makeChainedImage() {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs.push_back("/usr/lib/libSystem.B.dylib");

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = TextVA;
  Text.Size = 0x1000;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  std::memcpy(Text.Data.data() + (CStringVA - TextVA), "first\0", 6);
  std::memcpy(Text.Data.data() + (CStringBVA - TextVA), "second\0", 7);
  Image.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = "__DATA_CONST";
  Data.VA = DataVA;
  Data.Size = 0x100;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);

  dyld_chained_ptr_64_bind Bind{};
  Bind.ordinal = 1;
  Bind.addend = static_cast<uint8_t>(-2);
  Bind.next = 2;
  Bind.bind = 1;
  writeObject(Data.Data, 0, Bind);

  dyld_chained_ptr_64_rebase CStringRebase{};
  CStringRebase.target = CStringVA - TextVA;
  CStringRebase.next = 2;
  writeObject(Data.Data, 8, CStringRebase);

  dyld_chained_ptr_64_rebase CodeRebase{};
  CodeRebase.target = CodeVA - TextVA;
  CodeRebase.next = 2;
  writeObject(Data.Data, 16, CodeRebase);

  dyld_chained_ptr_64_rebase WritableRebase{};
  WritableRebase.target = WritableVA + 8 - TextVA;
  writeObject(Data.Data, 24, WritableRebase);
  Image.Segments.push_back(std::move(Data));

  Segment Writable;
  Writable.Name = "__DATA";
  Writable.VA = WritableVA;
  Writable.Size = 0x100;
  Writable.FileSz = Writable.Size;
  Writable.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Writable.Data.resize(Writable.Size);
  Image.Segments.push_back(std::move(Writable));

  Section TextSection;
  TextSection.Name = "__text";
  TextSection.SegmentName = "__TEXT";
  TextSection.VA = TextVA + 0x400;
  TextSection.Size = 0x180;
  TextSection.FileSz = TextSection.Size;
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  TextSection.Type = static_cast<uint32_t>(S_REGULAR) |
                     static_cast<uint32_t>(S_ATTR_PURE_INSTRUCTIONS) |
                     static_cast<uint32_t>(S_ATTR_SOME_INSTRUCTIONS);
  Image.Sections.push_back(std::move(TextSection));

  Section CStringSection;
  CStringSection.Name = "__cstring";
  CStringSection.SegmentName = "__TEXT";
  CStringSection.VA = CStringVA;
  CStringSection.Size = 0x40;
  CStringSection.FileSz = CStringSection.Size;
  CStringSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  CStringSection.Type = S_CSTRING_LITERALS;
  Image.Sections.push_back(std::move(CStringSection));
  return Image;
}

void expectValidModule(const llvm::Module &Module) {
  std::string Error;
  llvm::raw_string_ostream OS(Error);
  EXPECT_FALSE(llvm::verifyModule(Module, &OS)) << OS.str();
}

bool constantReferences(const llvm::Constant *Root,
                        const llvm::GlobalValue *Target) {
  if (Root == Target)
    return true;
  for (const llvm::Use &Operand : Root->operands())
    if (const auto *Child = llvm::dyn_cast<llvm::Constant>(Operand.get()))
      if (constantReferences(Child, Target))
        return true;
  return false;
}

bool isPtrToIntValue(const llvm::Value *Value) {
  if (const auto *Inst = llvm::dyn_cast<llvm::Instruction>(Value))
    return Inst->getOpcode() == llvm::Instruction::PtrToInt;
  if (const auto *Expr = llvm::dyn_cast<llvm::ConstantExpr>(Value))
    return Expr->getOpcode() == llvm::Instruction::PtrToInt;
  return false;
}

std::vector<llvm::CallInst *> callsIn(llvm::Function &Function) {
  std::vector<llvm::CallInst *> Calls;
  for (llvm::BasicBlock &Block : Function)
    for (llvm::Instruction &Inst : Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst))
        Calls.push_back(Call);
  return Calls;
}

MedFunc makePointerCall(llvm::StringRef Name, va_t Target,
                        std::vector<MedVar> Args) {
  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = Name.str();
  Func.ReturnType = NdType::makeVoid();
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = CallerVA;
  Call.addInput(MedVar::makeConst(Target, 8));
  Block.Ops.push_back(std::move(Call));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 4;
  Block.Ops.push_back(std::move(Return));
  Block.EndAddr = CallerVA + 8;
  Func.Blocks.push_back(std::move(Block));
  MedCallInfo Info;
  Info.BlockId = 0;
  Info.OpIdx = 0;
  Info.TargetAddr = Target;
  Info.TargetName = "_getopt_long";
  Info.Args = std::move(Args);
  Func.CallInfos.push_back(std::move(Info));
  return Func;
}

void addImport(BinaryImage &Image, llvm::StringRef Name, va_t Address) {
  Import Imported;
  Imported.Name = Name.str();
  Imported.IATAddr = Address;
  Image.Imports.push_back(std::move(Imported));
}

BinaryImage makeLLVMImage() {
  BinaryImage Image = makeChainedImage();
  Segment &Data = Image.Segments[1];
  constexpr uint64_t StdoutEncoding = 0x1111111111111111ULL;
  constexpr uint64_t GetoptEncoding = 0x2222222222222222ULL;
  writeObject(Data.Data, 0, StdoutEncoding);
  writeObject(Data.Data, 8, GetoptEncoding);
  writeObject(Data.Data, 16, CStringVA);
  writeObject(Data.Data, 24, WritableVA + 8);
  Image.DyldBindSlots[DataVA] = {"___stdoutp", 4};
  Image.DyldBindSlots[DataVA + 8] = {"_getopt_long", -8};
  Image.DataPtrRelocSlots.insert(DataVA + 16);
  Image.DataPtrRelocSlots.insert(DataVA + 24);
  addImport(Image, "___stdoutp", DataVA);
  addImport(Image, "_getopt_long", ImportStubVA);
  return Image;
}

std::vector<uint8_t> makeChainedBlob(macho_loader::ChainedFixupsInfo &Info) {
  constexpr size_t DataOff = 0x100;
  constexpr size_t DataSize = 0x180;
  constexpr uint32_t StartsOff = 0x20;
  constexpr uint32_t ImportsOff = 0x60;
  constexpr uint32_t SymbolsOff = 0x80;
  std::vector<uint8_t> Bytes(DataOff + DataSize, 0);
  Info.DataOff = DataOff;
  Info.DataSize = DataSize;

  dyld_chained_fixups_header Header{};
  Header.starts_offset = StartsOff;
  Header.imports_offset = ImportsOff;
  Header.symbols_offset = SymbolsOff;
  Header.imports_count = 2;
  Header.imports_format = DYLD_CHAINED_IMPORT_ADDEND64;
  writeObject(Bytes, DataOff, Header);

  dyld_chained_starts_in_image Starts{};
  Starts.seg_count = 1;
  Starts.seg_info_offset[0] = sizeof(uint32_t) * 2;
  writeObject(Bytes, DataOff + StartsOff, Starts);

  dyld_chained_starts_in_segment SegmentStarts{};
  SegmentStarts.size =
      offsetof(dyld_chained_starts_in_segment, page_start) + sizeof(uint16_t);
  SegmentStarts.page_size = 0x1000;
  SegmentStarts.pointer_format = DYLD_CHAINED_PTR_64_OFFSET;
  SegmentStarts.segment_offset = DataVA - TextVA;
  SegmentStarts.page_count = 1;
  SegmentStarts.page_start[0] = 0;
  writeObject(Bytes, DataOff + StartsOff + Starts.seg_info_offset[0],
              SegmentStarts);

  constexpr char Strings[] = "_unused\0___stdoutp\0";
  dyld_chained_import_addend64 Duplicate{};
  Duplicate.lib_ordinal = 1;
  Duplicate.name_offset = sizeof("_unused");
  Duplicate.addend = 99;
  writeObject(Bytes, DataOff + ImportsOff, Duplicate);
  dyld_chained_import_addend64 Bound{};
  Bound.lib_ordinal = 1;
  Bound.name_offset = sizeof("_unused");
  Bound.addend = 7;
  writeObject(Bytes, DataOff + ImportsOff + sizeof(Duplicate), Bound);
  std::memcpy(Bytes.data() + DataOff + SymbolsOff, Strings, sizeof(Strings));
  return Bytes;
}

TEST(MachOChainedPointerBoundary, RecordsBindAndFineGrainedRebases) {
  BinaryImage Image = makeChainedImage();
  macho_loader::ChainedFixupsInfo Info;
  std::vector<uint8_t> Binary = makeChainedBlob(Info);

  macho_loader::parseChainedFixupsImports(Binary.data(), Binary.size(), Info,
                                          Image);
  macho_loader::parseChainedFixupsRebases(Binary.data(), Binary.size(), Info,
                                          TextVA, Image);

  ASSERT_EQ(Image.DyldBindSlots.count(DataVA), 1u);
  EXPECT_EQ(Image.DyldBindSlots.at(DataVA).Name, "___stdoutp");
  // Equal names do not collapse the ordinal table: ordinal 1 carries addend 7,
  // not ordinal 0's addend 99; the pointer record contributes -2.
  EXPECT_EQ(Image.DyldBindSlots.at(DataVA).Addend, 5);

  auto Imported =
      std::find_if(Image.Imports.begin(), Image.Imports.end(),
                   [](const Import &I) { return I.Name == "___stdoutp"; });
  ASSERT_NE(Imported, Image.Imports.end());
  EXPECT_EQ(Imported->IATAddr, DataVA);

  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 8), 1u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 8), 0u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 16), 1u);
  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 24), 1u);
  EXPECT_FALSE(Image.isCodeAddress(CStringVA));
  EXPECT_TRUE(Image.isDataAddress(CStringVA));
  EXPECT_TRUE(Image.isCodeAddress(CodeVA));

  ASSERT_NE(Image.readVA(DataVA + 8, 8), nullptr);
  ASSERT_NE(Image.readVA(DataVA + 16, 8), nullptr);
  ASSERT_NE(Image.readVA(DataVA + 24, 8), nullptr);
  EXPECT_EQ(readPtr(Image.readVA(DataVA + 8, 8), true), CStringVA);
  EXPECT_EQ(readPtr(Image.readVA(DataVA + 16, 8), true), CodeVA);
  EXPECT_EQ(readPtr(Image.readVA(DataVA + 24, 8), true), WritableVA + 8);
}

TEST(MachOChainedPointerBoundary,
     RejectsMalformedOrdinalNameAndAddendOverflow) {
  // An out-of-range ordinal is local to that slot: it must not alias the last
  // valid record or partially join an Import.
  {
    BinaryImage Image = makeChainedImage();
    dyld_chained_ptr_64_bind Invalid{};
    Invalid.ordinal = 7;
    Invalid.bind = 1;
    writeObject(Image.Segments[1].Data, 0, Invalid);
    macho_loader::ChainedFixupsInfo Info;
    std::vector<uint8_t> Binary = makeChainedBlob(Info);
    macho_loader::parseChainedFixupsImports(Binary.data(), Binary.size(), Info,
                                            Image);
    macho_loader::parseChainedFixupsRebases(Binary.data(), Binary.size(), Info,
                                            TextVA, Image);
    EXPECT_TRUE(Image.DyldBindSlots.empty());
  }

  // Every symbol string must terminate inside the fixups blob.  Filling the
  // remaining string table removes that terminator without allowing a read
  // past DataSize.
  {
    BinaryImage Image = makeChainedImage();
    macho_loader::ChainedFixupsInfo Info;
    std::vector<uint8_t> Binary = makeChainedBlob(Info);
    std::fill(Binary.begin() + Info.DataOff + 0x80, Binary.end(), uint8_t{'x'});
    macho_loader::parseChainedFixupsImports(Binary.data(), Binary.size(), Info,
                                            Image);
    macho_loader::parseChainedFixupsRebases(Binary.data(), Binary.size(), Info,
                                            TextVA, Image);
    EXPECT_TRUE(Image.Imports.empty());
    EXPECT_TRUE(Image.DyldBindSlots.empty());
  }

  // The table and pointer addends are independently signed.  Their sum must
  // be checked before a binding becomes observable.
  {
    BinaryImage Image = makeChainedImage();
    dyld_chained_ptr_64_bind Overflow{};
    Overflow.ordinal = 1;
    Overflow.addend = 1;
    Overflow.bind = 1;
    writeObject(Image.Segments[1].Data, 0, Overflow);
    macho_loader::ChainedFixupsInfo Info;
    std::vector<uint8_t> Binary = makeChainedBlob(Info);
    dyld_chained_import_addend64 Bound{};
    Bound.lib_ordinal = 1;
    Bound.name_offset = sizeof("_unused");
    Bound.addend = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    writeObject(Binary,
                static_cast<size_t>(Info.DataOff) + 0x60 +
                    sizeof(dyld_chained_import_addend64),
                Bound);
    macho_loader::parseChainedFixupsImports(Binary.data(), Binary.size(), Info,
                                            Image);
    macho_loader::parseChainedFixupsRebases(Binary.data(), Binary.size(), Info,
                                            TextVA, Image);
    EXPECT_TRUE(Image.DyldBindSlots.empty());
    auto Imported =
        std::find_if(Image.Imports.begin(), Image.Imports.end(),
                     [](const Import &I) { return I.Name == "___stdoutp"; });
    ASSERT_NE(Imported, Image.Imports.end());
    EXPECT_EQ(Imported->IATAddr, 0u);
  }

  // Validate the complete import table before allocating ordinal storage. An
  // untrusted count cannot turn a tiny fixups blob into an enormous allocation.
  {
    BinaryImage Image = makeChainedImage();
    macho_loader::ChainedFixupsInfo Info;
    std::vector<uint8_t> Binary = makeChainedBlob(Info);
    dyld_chained_fixups_header Header{};
    std::memcpy(&Header, Binary.data() + Info.DataOff, sizeof(Header));
    Header.imports_count = std::numeric_limits<uint32_t>::max();
    writeObject(Binary, Info.DataOff, Header);
    macho_loader::parseChainedFixupsImports(Binary.data(), Binary.size(), Info,
                                            Image);
    EXPECT_TRUE(Image.Imports.empty());
  }
}

TEST(MachOClassicBindBoundary, RecordsSlotAndSignedAddend) {
  constexpr uint32_t BindOff = 0x20;
  std::vector<uint8_t> Binary(0x100, 0);
  std::vector<uint8_t> Stream = {
      static_cast<uint8_t>(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | 1),
      static_cast<uint8_t>(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM)};
  constexpr char Name[] = "___stderrp";
  Stream.insert(Stream.end(), Name, Name + sizeof(Name));
  Stream.insert(
      Stream.end(),
      {static_cast<uint8_t>(static_cast<uint8_t>(BIND_OPCODE_SET_TYPE_IMM) |
                            static_cast<uint8_t>(BIND_TYPE_POINTER)),
       static_cast<uint8_t>(BIND_OPCODE_SET_ADDEND_SLEB), 0x7d,
       static_cast<uint8_t>(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB), 0x18,
       static_cast<uint8_t>(BIND_OPCODE_DO_BIND),
       static_cast<uint8_t>(BIND_OPCODE_DONE)});
  std::memcpy(Binary.data() + BindOff, Stream.data(), Stream.size());

  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs.push_back("/usr/lib/libSystem.B.dylib");
  Segment Data;
  Data.Name = "__DATA_CONST";
  Data.VA = 0x2000;
  Data.Size = 0x100;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));
  Import Existing;
  Existing.Name = Name;
  Existing.IATAddr =
      0x1010; // an executable stub address must stay authoritative
  Image.Imports.push_back(std::move(Existing));

  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.BindOff = BindOff;
  DyldInfo.BindSize = Stream.size();
  macho_loader::parseBindStreams(Binary.data(), Binary.size(), DyldInfo, Image);

  ASSERT_EQ(Image.DyldBindSlots.count(0x2018), 1u);
  EXPECT_EQ(Image.DyldBindSlots.at(0x2018).Name, Name);
  EXPECT_EQ(Image.DyldBindSlots.at(0x2018).Addend, -3);
  ASSERT_EQ(Image.Imports.size(), 1u);
  EXPECT_EQ(Image.Imports.front().IATAddr, 0x1010u);
}

TEST(MachOClassicBindBoundary, RecordsEveryRepeatedPointerBinding) {
  constexpr uint32_t BindOff = 0x20;
  std::vector<uint8_t> Binary(0x100, 0);
  std::vector<uint8_t> Stream = {
      static_cast<uint8_t>(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM)};
  constexpr char Name[] = "_repeated";
  Stream.insert(Stream.end(), Name, Name + sizeof(Name));
  Stream.insert(
      Stream.end(),
      {static_cast<uint8_t>(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB), 0,
       static_cast<uint8_t>(BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB), 3, 8,
       static_cast<uint8_t>(BIND_OPCODE_DONE)});
  std::memcpy(Binary.data() + BindOff, Stream.data(), Stream.size());

  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Data;
  Data.Name = "__DATA_CONST";
  Data.VA = 0x2000;
  Data.Size = 0x100;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));
  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.BindOff = BindOff;
  DyldInfo.BindSize = Stream.size();
  macho_loader::parseBindStreams(Binary.data(), Binary.size(), DyldInfo, Image);

  ASSERT_EQ(Image.DyldBindSlots.size(), 3u);
  EXPECT_EQ(Image.DyldBindSlots.at(0x2000).Name, Name);
  EXPECT_EQ(Image.DyldBindSlots.at(0x2010).Name, Name);
  EXPECT_EQ(Image.DyldBindSlots.at(0x2020).Name, Name);

  // Reject the repeated operation as one unit when its last slot escapes the
  // segment; do not retain the valid-looking prefix of a malformed run.
  auto Repeat = std::find(
      Stream.begin(), Stream.end(),
      static_cast<uint8_t>(BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB));
  ASSERT_NE(Repeat, Stream.end());
  ASSERT_LT(Repeat + 1, Stream.end());
  *(Repeat + 1) = 17;
  std::fill(Binary.begin() + BindOff, Binary.end(), 0);
  std::memcpy(Binary.data() + BindOff, Stream.data(), Stream.size());
  Image.DyldBindSlots.clear();
  Image.Imports.clear();
  macho_loader::parseBindStreams(Binary.data(), Binary.size(), DyldInfo, Image);
  EXPECT_TRUE(Image.DyldBindSlots.empty());
  EXPECT_TRUE(Image.Imports.empty());
}

TEST(MachOLLVMImportPointerBoundary, SymbolizesMixedRelocationRun) {
  BinaryImage Image = makeLLVMImage();
  MedFunc Caller = makePointerCall(
      "mixed_pointer_caller", ImportStubVA,
      {MedVar::makeConst(DataVA + 16, 8), MedVar::makeConst(CStringVA, 8)});

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Caller}, Context, "macho-mixed-pointer-run", Arch::AArch64,
      {{ImportStubVA, "_getopt_long"}}, &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::Function *Getopt = Module->getFunction("getopt_long");
  ASSERT_NE(Getopt, nullptr);
  llvm::GlobalVariable *Stdout = Module->getNamedGlobal("__stdoutp");
  ASSERT_NE(Stdout, nullptr);
  EXPECT_TRUE(Stdout->isDeclaration());
  EXPECT_EQ(Module->getNamedGlobal("getopt_long"), nullptr);

  const std::string MirrorName =
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str();
  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(MirrorName);
  ASSERT_NE(Mirror, nullptr);
  ASSERT_TRUE(Mirror->hasInitializer());
  const auto *Init =
      llvm::dyn_cast<llvm::ConstantStruct>(Mirror->getInitializer());
  ASSERT_NE(Init, nullptr);
  ASSERT_GE(Init->getNumOperands(), 4u);
  EXPECT_TRUE(constantReferences(
      llvm::cast<llvm::Constant>(Init->getOperand(0)), Stdout));
  EXPECT_TRUE(constantReferences(
      llvm::cast<llvm::Constant>(Init->getOperand(1)), Getopt));
  EXPECT_FALSE(llvm::isa<llvm::ConstantInt>(Init->getOperand(2)));
  EXPECT_FALSE(llvm::isa<llvm::ConstantInt>(Init->getOperand(3)));

  const auto *StdoutAdd =
      llvm::dyn_cast<llvm::ConstantExpr>(Init->getOperand(0));
  ASSERT_NE(StdoutAdd, nullptr);
  ASSERT_EQ(StdoutAdd->getOpcode(), llvm::Instruction::Add);
  EXPECT_EQ(
      llvm::cast<llvm::ConstantInt>(StdoutAdd->getOperand(1))->getSExtValue(),
      4);
  const auto *GetoptAdd =
      llvm::dyn_cast<llvm::ConstantExpr>(Init->getOperand(1));
  ASSERT_NE(GetoptAdd, nullptr);
  ASSERT_EQ(GetoptAdd->getOpcode(), llvm::Instruction::Add);
  EXPECT_EQ(
      llvm::cast<llvm::ConstantInt>(GetoptAdd->getOperand(1))->getSExtValue(),
      -8);
}

TEST(MachOLLVMImportPointerBoundary, SymbolizesImportOnlyRun) {
  BinaryImage Image = makeLLVMImage();
  Segment &Data = Image.Segments[1];
  std::fill(Data.Data.begin(), Data.Data.end(), 0);
  constexpr uint64_t StderrEncoding = 0x3333333333333333ULL;
  constexpr uint64_t StdoutEncoding = 0x4444444444444444ULL;
  writeObject(Data.Data, 0, StderrEncoding);
  writeObject(Data.Data, 8, StdoutEncoding);
  Image.DataPtrRelocSlots.clear();
  Image.DyldBindSlots.clear();
  Image.ImportPtrSlots.clear();
  Image.Imports.clear();
  Image.DyldBindSlots[DataVA] = {"___stderrp", 0};
  Image.ImportPtrSlots[DataVA + 8] = "___stdoutp";
  addImport(Image, "_getopt_long", ImportStubVA);

  MedFunc Caller = makePointerCall("import_only_caller", ImportStubVA,
                                   {MedVar::makeConst(DataVA, 8)});
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Caller}, Context, "macho-import-only-run", Arch::AArch64,
      {{ImportStubVA, "_getopt_long"}}, &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::GlobalVariable *Stderr = Module->getNamedGlobal("__stderrp");
  llvm::GlobalVariable *Stdout = Module->getNamedGlobal("__stdoutp");
  ASSERT_NE(Stderr, nullptr);
  ASSERT_NE(Stdout, nullptr);
  const std::string MirrorName =
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str();
  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(MirrorName);
  ASSERT_NE(Mirror, nullptr);
  ASSERT_TRUE(Mirror->hasInitializer());
  EXPECT_TRUE(constantReferences(Mirror->getInitializer(), Stderr));
  EXPECT_TRUE(constantReferences(Mirror->getInitializer(), Stdout));
}

TEST(MachOLLVMImportPointerBoundary, ClearsPointerStateBetweenEmissions) {
  MedLLVMEmitter Emitter;
  const std::string MirrorName =
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str();

  BinaryImage FirstImage = makeLLVMImage();
  MedFunc FirstCaller = makePointerCall("first_pointer_emission", ImportStubVA,
                                        {MedVar::makeConst(DataVA + 16, 8)});
  llvm::LLVMContext FirstContext;
  auto FirstModule =
      Emitter.emit({FirstCaller}, FirstContext, "macho-first-pointer-emission",
                   Arch::AArch64, {{ImportStubVA, "_getopt_long"}}, &FirstImage,
                   BinaryFormat::MachO);
  ASSERT_NE(FirstModule, nullptr);
  expectValidModule(*FirstModule);
  llvm::GlobalVariable *FirstMirror = FirstModule->getNamedGlobal(MirrorName);
  ASSERT_NE(FirstMirror, nullptr);

  BinaryImage SecondImage = makeLLVMImage();
  MedFunc SecondCaller =
      makePointerCall("second_pointer_emission", ImportStubVA,
                      {MedVar::makeConst(DataVA + 24, 8)});
  llvm::LLVMContext SecondContext;
  auto SecondModule = Emitter.emit(
      {SecondCaller}, SecondContext, "macho-second-pointer-emission",
      Arch::AArch64, {{ImportStubVA, "_getopt_long"}}, &SecondImage,
      BinaryFormat::MachO);
  ASSERT_NE(SecondModule, nullptr);
  expectValidModule(*SecondModule);
  llvm::GlobalVariable *SecondMirror = SecondModule->getNamedGlobal(MirrorName);
  ASSERT_NE(SecondMirror, nullptr);
  EXPECT_NE(FirstMirror, SecondMirror);
  EXPECT_EQ(SecondMirror->getParent(), SecondModule.get());
}

MedFunc makeSelectPointerCaller(bool InvalidCodeArm, bool InvalidScalarArm) {
  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = InvalidCodeArm || InvalidScalarArm ? "rejected_select_caller"
                                                 : "data_select_caller";
  Func.ReturnType = NdType::makeVoid();
  MedVar Cond;
  Cond.Kind = MedVar::Param;
  Cond.Id = 0;
  Cond.Size = 8;
  Cond.TheArch = Arch::AArch64;
  Func.Params.push_back(Cond);

  MedVar Selected;
  Selected.Kind = MedVar::Temp;
  Selected.Id = 1;
  Selected.SSAVer = 1;
  Selected.Size = 8;
  Selected.TheArch = Arch::AArch64;

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;

  MedOp Direct;
  Direct.Opcode = NdOp::CALL;
  Direct.Addr = CallerVA;
  Direct.addInput(MedVar::makeConst(ImportStubVA, 8));
  Block.Ops.push_back(std::move(Direct));

  MedOp Select;
  Select.Opcode = NdOp::SELECT;
  Select.Addr = CallerVA + 4;
  Select.Output = Selected;
  Select.addInput(Cond);
  Select.addInput(MedVar::makeConst(CStringBVA, 8));
  Select.addInput(MedVar::makeConst(
      InvalidCodeArm ? CodeVA : (InvalidScalarArm ? 0x1234 : CStringVA), 8));
  Block.Ops.push_back(std::move(Select));

  MedOp SelectedCall;
  SelectedCall.Opcode = NdOp::CALL;
  SelectedCall.Addr = CallerVA + 8;
  SelectedCall.addInput(MedVar::makeConst(ImportStubVA, 8));
  Block.Ops.push_back(std::move(SelectedCall));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 12;
  Block.Ops.push_back(std::move(Return));
  Block.EndAddr = CallerVA + 16;
  Func.Blocks.push_back(std::move(Block));

  MedCallInfo DirectInfo;
  DirectInfo.BlockId = 0;
  DirectInfo.OpIdx = 0;
  DirectInfo.TargetAddr = ImportStubVA;
  DirectInfo.TargetName = "_getopt_long";
  DirectInfo.Args.push_back(MedVar::makeConst(CStringVA, 8));
  Func.CallInfos.push_back(std::move(DirectInfo));
  MedCallInfo SelectInfo;
  SelectInfo.BlockId = 0;
  SelectInfo.OpIdx = 2;
  SelectInfo.TargetAddr = ImportStubVA;
  SelectInfo.TargetName = "_getopt_long";
  SelectInfo.Args.push_back(Selected);
  Func.CallInfos.push_back(std::move(SelectInfo));
  return Func;
}

TEST(MachOLLVMDataPointerBoundary, SymbolizesDirectAndAllDataSelect) {
  BinaryImage Image = makeLLVMImage();
  MedFunc Caller = makeSelectPointerCaller(false, false);
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Caller}, Context, "macho-data-pointer-select", Arch::AArch64,
      {{ImportStubVA, "_getopt_long"}}, &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);
  llvm::Function *Function = Module->getFunction("data_select_caller");
  ASSERT_NE(Function, nullptr);
  std::vector<llvm::CallInst *> Calls = callsIn(*Function);
  ASSERT_EQ(Calls.size(), 2u);
  EXPECT_TRUE(isPtrToIntValue(Calls[0]->getArgOperand(0)));
  EXPECT_TRUE(isPtrToIntValue(Calls[1]->getArgOperand(0)));
  const auto *PointerSelect = llvm::dyn_cast<llvm::SelectInst>(
      llvm::cast<llvm::Instruction>(Calls[1]->getArgOperand(0))->getOperand(0));
  ASSERT_NE(PointerSelect, nullptr);
  EXPECT_TRUE(PointerSelect->getType()->isPointerTy());
}

TEST(MachOLLVMDataPointerBoundary, RejectsSelectWithCodeOrScalarArm) {
  for (const auto [CodeArm, ScalarArm] :
       {std::pair{true, false}, std::pair{false, true}}) {
    BinaryImage Image = makeLLVMImage();
    MedFunc Caller = makeSelectPointerCaller(CodeArm, ScalarArm);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Caller}, Context,
        CodeArm ? "macho-code-select" : "macho-scalar-select", Arch::AArch64,
        {{ImportStubVA, "_getopt_long"}}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction("rejected_select_caller");
    ASSERT_NE(Function, nullptr);
    std::vector<llvm::CallInst *> Calls = callsIn(*Function);
    ASSERT_EQ(Calls.size(), 2u);
    std::string IR;
    llvm::raw_string_ostream IRStream(IR);
    Module->print(IRStream, nullptr);
    IRStream.flush();
    EXPECT_FALSE(isPtrToIntValue(Calls[1]->getArgOperand(0))) << IR;
    EXPECT_TRUE(llvm::isa<llvm::LoadInst>(Calls[1]->getArgOperand(0)) ||
                llvm::isa<llvm::SelectInst>(Calls[1]->getArgOperand(0)))
        << IR;
    EXPECT_EQ(IR.find("second"), std::string::npos) << IR;
  }
}

} // namespace
