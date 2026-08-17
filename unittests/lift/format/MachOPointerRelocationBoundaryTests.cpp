//===- MachOPointerRelocationBoundaryTests.cpp ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

class PipelineTestPeer {
public:
  static bool requiresSerialLLVMEmission(const std::vector<MedFunc> &Funcs,
                                         const BinaryImage &Image) {
    return Pipeline::requiresSerialLLVMEmission(Funcs, Image);
  }
};

} // namespace neverd

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
  // Mach-O keeps __DATA_CONST writable while dyld applies fixups even though
  // the segment is immutable once loading completes.
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
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

bool constantContainsBlockAddress(const llvm::Constant *Root) {
  if (llvm::isa<llvm::BlockAddress>(Root))
    return true;
  for (const llvm::Use &Operand : Root->operands())
    if (const auto *Child = llvm::dyn_cast<llvm::Constant>(Operand.get()))
      if (constantContainsBlockAddress(Child))
        return true;
  return false;
}

bool constantContainsInteger(const llvm::Constant *Root, uint64_t Value) {
  if (const auto *Int = llvm::dyn_cast<llvm::ConstantInt>(Root))
    return Int->getZExtValue() == Value;
  for (const llvm::Use &Operand : Root->operands())
    if (const auto *Child = llvm::dyn_cast<llvm::Constant>(Operand.get()))
      if (constantContainsInteger(Child, Value))
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

MedFunc makeCleanupFunction(llvm::StringRef Name, va_t FunctionVA,
                            va_t MayThrowVA) {
  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = Name.str();
  Func.ReturnType = NdType::makeVoid();

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = FunctionVA;
  Protected.EndAddr = FunctionVA + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = FunctionVA + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(std::move(Call));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = FunctionVA + 8;
  Protected.Ops.push_back(std::move(Return));

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = FunctionVA + 0x20;
  Handler.EndAddr = FunctionVA + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = FunctionVA + 0x20;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x30};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;
  ItaniumCallSite Site;
  Site.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Site.LandingPadVA = FunctionVA + 0x20;
  EH.Itanium->CallSites.push_back(Site);
  Func.ExceptionMetadata = std::move(EH);
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

struct InteriorPointerFixture {
  BinaryImage Image;
  va_t Entry = 0;
  va_t End = 0;
  va_t SelectedSlot = 0;
  va_t SelectedTarget = 0;
  std::vector<va_t> InteriorTargets;
};

InteriorPointerFixture makeInteriorPointerFixture(Arch TargetArch) {
  InteriorPointerFixture Fixture;
  BinaryImage &Image = Fixture.Image;
  Image.Arch = TargetArch;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.Base = TextVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = TextVA;
  Text.Size = 0x600;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);

  va_t TableVA = 0;
  std::vector<uint8_t> Code;
  if (TargetArch == Arch::AArch64) {
    Fixture.Entry = TextVA + 0x4a0;
    Fixture.End = TextVA + 0x534;
    TableVA = DataVA + 8;
    Fixture.InteriorTargets = {TextVA + 0x4c8, TextVA + 0x4d4, TextVA + 0x4e0,
                               TextVA + 0x4ec};
    Code = {
        0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x02, 0xa9, 0xfd, 0x83, 0x00, 0x91,
        0xbf, 0xc3, 0x1f, 0xb8, 0xbf, 0x83, 0x1f, 0xb8, 0x28, 0x00, 0x00, 0x90,
        0x08, 0x21, 0x00, 0x91, 0x08, 0x05, 0x40, 0xf9, 0xe8, 0x0b, 0x00, 0xf9,
        0x1a, 0x00, 0x00, 0x14, 0x28, 0x00, 0x80, 0x52, 0xa8, 0x83, 0x1f, 0xb8,
        0x0a, 0x00, 0x00, 0x14, 0x48, 0x00, 0x80, 0x52, 0xa8, 0x83, 0x1f, 0xb8,
        0x07, 0x00, 0x00, 0x14, 0x68, 0x00, 0x80, 0x52, 0xa8, 0x83, 0x1f, 0xb8,
        0x04, 0x00, 0x00, 0x14, 0x88, 0x00, 0x80, 0x52, 0xa8, 0x83, 0x1f, 0xb8,
        0x01, 0x00, 0x00, 0x14, 0x28, 0x00, 0x00, 0x90, 0x0a, 0x05, 0x40, 0xf9,
        0xa8, 0x83, 0x5f, 0xb8, 0xe9, 0x03, 0x00, 0x91, 0x2a, 0x01, 0x00, 0xf9,
        0x28, 0x05, 0x00, 0xf9, 0x00, 0x00, 0x00, 0x90, 0x00, 0x6c, 0x15, 0x91,
        0x07, 0x00, 0x00, 0x94, 0x00, 0x00, 0x80, 0x52, 0xfd, 0x7b, 0x42, 0xa9,
        0xff, 0xc3, 0x00, 0x91, 0xc0, 0x03, 0x5f, 0xd6, 0xe8, 0x0b, 0x40, 0xf9,
        0x00, 0x01, 0x1f, 0xd6,
    };
  } else {
    Fixture.Entry = TextVA + 0x4b0;
    Fixture.End = TextVA + 0x51b;
    TableVA = TextVA + 0x1010;
    Fixture.InteriorTargets = {TextVA + 0x4d3, TextVA + 0x4dc, TextVA + 0x4e5,
                               TextVA + 0x4ee};
    Code = {
        0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x10, 0xc7, 0x45, 0xfc, 0x00,
        0x00, 0x00, 0x00, 0xc7, 0x45, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b,
        0x05, 0x4b, 0x0b, 0x00, 0x00, 0x48, 0x89, 0x45, 0xf0, 0xeb, 0x42, 0xc7,
        0x45, 0xf8, 0x01, 0x00, 0x00, 0x00, 0xeb, 0x19, 0xc7, 0x45, 0xf8, 0x02,
        0x00, 0x00, 0x00, 0xeb, 0x10, 0xc7, 0x45, 0xf8, 0x03, 0x00, 0x00, 0x00,
        0xeb, 0x07, 0xc7, 0x45, 0xf8, 0x04, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x35,
        0x14, 0x0b, 0x00, 0x00, 0x8b, 0x55, 0xf8, 0x48, 0x8d, 0x3d, 0x37, 0x00,
        0x00, 0x00, 0xb0, 0x00, 0xe8, 0x0f, 0x00, 0x00, 0x00, 0x31, 0xc0, 0x48,
        0x83, 0xc4, 0x10, 0x5d, 0xc3, 0x48, 0x8b, 0x45, 0xf0, 0xff, 0xe0,
    };
  }
  EXPECT_LE(Fixture.Entry - TextVA + Code.size(), Text.Data.size());
  std::copy(Code.begin(), Code.end(),
            Text.Data.begin() + static_cast<ptrdiff_t>(Fixture.Entry - TextVA));
  Image.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = "__DATA_CONST";
  Data.VA = TargetArch == Arch::AArch64 ? DataVA : TextVA + 0x1000;
  Data.Size = 0x60;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.assign(Data.Size, 0);
  for (size_t I = 0; I < Fixture.InteriorTargets.size(); ++I) {
    const va_t NameSlot = TableVA + I * 16;
    const va_t CodeSlot = NameSlot + 8;
    writeObject(Data.Data, static_cast<size_t>(NameSlot - Data.VA),
                TextVA + 0x540 + I * 8);
    writeObject(Data.Data, static_cast<size_t>(CodeSlot - Data.VA),
                Fixture.InteriorTargets[I]);
    Image.DataPtrRelocSlots.insert(NameSlot);
    Image.CodePtrRelocSlots.insert(CodeSlot);
  }
  Image.Segments.push_back(std::move(Data));
  Fixture.SelectedSlot = TableVA + 8;
  Fixture.SelectedTarget = Fixture.InteriorTargets.front();

  Image.Entry = Fixture.Entry;
  Image.KnownCodeRanges.push_back({Fixture.Entry, Fixture.End});
  ExceptionFunction Exception;
  Exception.Kind = RuntimeFunctionKind::Primary;
  Exception.Encoding = ExceptionEncoding::CompactUnwind;
  Exception.ParseStatus = ExceptionParseStatus::Complete;
  Exception.CodeRange = {Fixture.Entry, Fixture.End};
  Image.ExceptionMetadata.Functions.push_back(std::move(Exception));
  Image.ExceptionMetadata.rebuildIndex();
  return Fixture;
}

LowFunc buildInteriorPointerCFG(InteriorPointerFixture &Fixture,
                                std::set<va_t> Entries = std::set<va_t>{}) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Fixture.Image.Arch));
  CFGBuilder Builder;
  if (Entries.empty())
    Entries.insert(Fixture.Entry);
  Builder.setKnownFuncEntries(&Entries);
  return Builder.build(Fixture.Image, Dec, Fixture.Entry, "interior_owner");
}

std::set<va_t> blockStarts(const LowFunc &Func) {
  std::set<va_t> Result;
  for (const LowBlock &Block : Func.Blocks)
    Result.insert(Block.StartAddr);
  return Result;
}

bool containsOpcode(const LowFunc &Func, NdOp Opcode) {
  for (const LowBlock &Block : Func.Blocks)
    for (const LowOp &Op : Block.Ops)
      if (Op.Opcode == Opcode)
        return true;
  return false;
}

MedFunc makeReturnFunction(llvm::StringRef Name, va_t Entry,
                           std::optional<va_t> Interior = std::nullopt) {
  MedFunc Func;
  Func.Name = Name.str();
  Func.Entry = Entry;
  Func.ReturnType = NdType::makeVoid();
  auto AddReturnBlock = [&](int Id, va_t Address) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Address;
    Block.EndAddr = Address + 4;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Address;
    Block.Ops.push_back(std::move(Return));
    Func.Blocks.push_back(std::move(Block));
  };
  AddReturnBlock(0, Entry);
  if (Interior)
    AddReturnBlock(1, *Interior);
  return Func;
}

TEST(MachOInteriorCodePointerCFG,
     PreservesRelocatedInteriorRootsAndFoldsImmutableRelay) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    InteriorPointerFixture Fixture = makeInteriorPointerFixture(TargetArch);
    LowFunc Func = buildInteriorPointerCFG(Fixture);
    const std::set<va_t> Starts = blockStarts(Func);

    for (va_t Target : Fixture.InteriorTargets)
      EXPECT_EQ(Starts.count(Target), 1u)
          << "missing address-taken block 0x" << llvm::utohexstr(Target);

    bool HasSelectedDirectBranch = false;
    for (const LowBlock &Block : Func.Blocks)
      for (const LowOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::BRANCH && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst() &&
            Op.Inputs[0].Offset == Fixture.SelectedTarget)
          HasSelectedDirectBranch = true;
    EXPECT_TRUE(HasSelectedDirectBranch);
    EXPECT_FALSE(containsOpcode(Func, NdOp::INDIR_CALL));
  }
}

TEST(MachOInteriorCodePointerCFG, DoesNotFoldWritableOrUnprovenRelay) {
  {
    InteriorPointerFixture Fixture = makeInteriorPointerFixture(Arch::AArch64);
    Fixture.Image.Segments.back().Name = section_names::macho::DataSeg;
    Fixture.Image.Segments.back().Flags =
        Fixture.Image.Segments.back().Flags | SegmentFlags::Writable;
    LowFunc Func = buildInteriorPointerCFG(Fixture);
    EXPECT_TRUE(containsOpcode(Func, NdOp::INDIR_CALL));
  }
  {
    InteriorPointerFixture Fixture = makeInteriorPointerFixture(Arch::X64);
    Fixture.Image.CodePtrRelocSlots.clear();
    LowFunc Func = buildInteriorPointerCFG(Fixture);
    EXPECT_TRUE(containsOpcode(Func, NdOp::INDIR_CALL));
  }
}

TEST(MachOInteriorCodePointerCFG,
     RejectsOtherFunctionsAdjacentTargetsAndInstructionInteriors) {
  {
    InteriorPointerFixture Fixture = makeInteriorPointerFixture(Arch::X64);
    const va_t InstructionInterior = Fixture.Entry + 2;
    ASSERT_TRUE(
        Fixture.Image.patchPtr(Fixture.SelectedSlot, InstructionInterior));
    LowFunc Func = buildInteriorPointerCFG(Fixture);
    EXPECT_EQ(blockStarts(Func).count(InstructionInterior), 0u);
    EXPECT_TRUE(containsOpcode(Func, NdOp::INDIR_CALL));
  }
  {
    InteriorPointerFixture Fixture = makeInteriorPointerFixture(Arch::AArch64);
    const va_t OtherFunction = Fixture.InteriorTargets.front();
    std::set<va_t> Entries{Fixture.Entry, OtherFunction};
    LowFunc Func = buildInteriorPointerCFG(Fixture, Entries);
    EXPECT_EQ(blockStarts(Func).count(OtherFunction), 0u);
    EXPECT_TRUE(containsOpcode(Func, NdOp::INDIR_CALL));
  }
  {
    InteriorPointerFixture Fixture = makeInteriorPointerFixture(Arch::AArch64);
    const va_t Adjacent = Fixture.End;
    ASSERT_TRUE(Fixture.Image.patchPtr(Fixture.SelectedSlot, Adjacent));
    Segment &Text = Fixture.Image.Segments.front();
    const uint32_t Ret = 0xd65f03c0;
    writeObject(Text.Data, static_cast<size_t>(Adjacent - Text.VA), Ret);
    std::set<va_t> Entries{Fixture.Entry, Adjacent};
    LowFunc Func = buildInteriorPointerCFG(Fixture, Entries);
    EXPECT_EQ(blockStarts(Func).count(Adjacent), 0u);
    EXPECT_TRUE(containsOpcode(Func, NdOp::INDIR_CALL));
  }
}

TEST(MachOInteriorCodePointerCFG, RejectsPotentiallyAliasingRelayStore) {
  InteriorPointerFixture Fixture = makeInteriorPointerFixture(Arch::AArch64);
  Segment &Text = Fixture.Image.Segments.front();
  constexpr va_t NewDispatch = TextVA + 0x560;
  const uint32_t BranchToDispatch = 0x14000027; // b 0x100000560
  const uint32_t UnknownStore = 0xf9000149;     // str x9, [x10]
  const uint32_t Reload = 0xf9400be8;           // ldr x8, [sp, #16]
  const uint32_t IndirectBranch = 0xd61f0100;   // br x8
  writeObject(Text.Data, 0x4c4, BranchToDispatch);
  writeObject(Text.Data, static_cast<size_t>(NewDispatch - Text.VA),
              UnknownStore);
  writeObject(Text.Data, static_cast<size_t>(NewDispatch - Text.VA + 4),
              Reload);
  writeObject(Text.Data, static_cast<size_t>(NewDispatch - Text.VA + 8),
              IndirectBranch);
  Fixture.End = NewDispatch + 12;
  Fixture.Image.KnownCodeRanges.front().second = Fixture.End;
  Fixture.Image.ExceptionMetadata.Functions.front().CodeRange.End = Fixture.End;
  Fixture.Image.ExceptionMetadata.rebuildIndex();

  LowFunc Func = buildInteriorPointerCFG(Fixture);
  EXPECT_TRUE(containsOpcode(Func, NdOp::INDIR_CALL));
}

TEST(MachOInteriorCodePointerCFG, RejectsAmbiguousMultiplePredecessorRelay) {
  InteriorPointerFixture Fixture = makeInteriorPointerFixture(Arch::X64);
  const va_t Dispatch = Fixture.End - 6;
  Segment &Text = Fixture.Image.Segments.front();
  Text.Data[Fixture.SelectedTarget - Text.VA] = 0xe9; // jmp rel32
  const int32_t Displacement =
      static_cast<int32_t>(static_cast<int64_t>(Dispatch) -
                           static_cast<int64_t>(Fixture.SelectedTarget + 5));
  writeObject(Text.Data,
              static_cast<size_t>(Fixture.SelectedTarget - Text.VA + 1),
              Displacement);

  LowFunc Func = buildInteriorPointerCFG(Fixture);
  EXPECT_EQ(blockStarts(Func).count(Fixture.SelectedTarget), 1u);
  EXPECT_TRUE(containsOpcode(Func, NdOp::INDIR_CALL));
}

TEST(MachOLLVMInteriorCodePointerBoundary,
     EmitsFunctionAndBlockIdentityWhenConsumerPrecedesOwner) {
  constexpr va_t OwnerVA = TextVA + 0x700;
  constexpr va_t InteriorVA = TextVA + 0x720;
  constexpr va_t FunctionVA = TextVA + 0x780;
  BinaryImage Image = makeLLVMImage();
  Segment &Data = Image.Segments[1];
  writeObject(Data.Data, 32, FunctionVA);
  writeObject(Data.Data, 40, InteriorVA);
  Image.CodePtrRelocSlots.insert(DataVA + 32);
  Image.CodePtrRelocSlots.insert(DataVA + 40);

  MedFunc Consumer = makePointerCall("interior_pointer_consumer", ImportStubVA,
                                     {MedVar::makeConst(DataVA + 40, 8)});
  MedFunc Owner =
      makeReturnFunction("interior_pointer_owner", OwnerVA, InteriorVA);
  MedFunc FunctionTarget =
      makeReturnFunction("ordinary_function_target", FunctionVA);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Consumer, Owner, FunctionTarget}, Context, "macho-interior-code-pointer",
      Arch::AArch64, {{ImportStubVA, "_getopt_long"}}, &Image,
      BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str());
  ASSERT_NE(Mirror, nullptr);
  ASSERT_TRUE(Mirror->hasInitializer());
  llvm::Function *Function = Module->getFunction("ordinary_function_target");
  ASSERT_NE(Function, nullptr);
  EXPECT_TRUE(constantReferences(Mirror->getInitializer(), Function));
  EXPECT_TRUE(constantContainsBlockAddress(Mirror->getInitializer()));
  EXPECT_FALSE(constantContainsInteger(Mirror->getInitializer(), FunctionVA));
  EXPECT_FALSE(constantContainsInteger(Mirror->getInitializer(), InteriorVA));

  auto Clone = llvm::CloneModule(*Module);
  ASSERT_NE(Clone, nullptr);
  expectValidModule(*Clone);
}

TEST(MachOLLVMInteriorCodePointerBoundary,
     FailsClosedForUnresolvedOrMaskedInteriorTarget) {
  constexpr va_t OwnerVA = TextVA + 0x700;
  constexpr va_t InteriorVA = TextVA + 0x720;

  auto MakeInput = [&]() {
    BinaryImage Image = makeLLVMImage();
    writeObject(Image.Segments[1].Data, 40, InteriorVA);
    Image.CodePtrRelocSlots.insert(DataVA + 40);
    return Image;
  };
  MedFunc Consumer = makePointerCall("masked_interior_consumer", ImportStubVA,
                                     {MedVar::makeConst(DataVA + 40, 8)});
  MedFunc Owner =
      makeReturnFunction("masked_interior_owner", OwnerVA, InteriorVA);

  {
    BinaryImage Image = MakeInput();
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Consumer}, Context, "macho-unresolved-interior", Arch::AArch64,
        {{ImportStubVA, "_getopt_long"}}, &Image, BinaryFormat::MachO);
    EXPECT_EQ(Module, nullptr);
  }
  {
    BinaryImage Image = MakeInput();
    llvm::LLVMContext Context;
    const std::vector<char> BodyMask = {1, 0};
    auto Module = MedLLVMEmitter().emit(
        {Consumer, Owner}, Context, "macho-masked-interior", Arch::AArch64,
        {{ImportStubVA, "_getopt_long"}}, &Image, BinaryFormat::MachO,
        /*MergeableGlobals=*/false, &BodyMask);
    EXPECT_EQ(Module, nullptr);
  }
}

TEST(MachOLLVMInteriorCodePointerBoundary,
     ClearsBlockIdentityStateBetweenEmissions) {
  constexpr va_t OwnerVA = TextVA + 0x700;
  constexpr va_t InteriorVA = TextVA + 0x720;
  BinaryImage Image = makeLLVMImage();
  writeObject(Image.Segments[1].Data, 40, InteriorVA);
  Image.CodePtrRelocSlots.insert(DataVA + 40);
  MedFunc Consumer = makePointerCall("state_reset_consumer", ImportStubVA,
                                     {MedVar::makeConst(DataVA + 40, 8)});
  MedFunc Owner = makeReturnFunction("state_reset_owner", OwnerVA, InteriorVA);

  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto First = Emitter.emit(
      {Consumer, Owner}, Context, "macho-interior-state-first", Arch::AArch64,
      {{ImportStubVA, "_getopt_long"}}, &Image, BinaryFormat::MachO);
  ASSERT_NE(First, nullptr);
  expectValidModule(*First);

  auto Second = Emitter.emit({Consumer}, Context, "macho-interior-state-second",
                             Arch::AArch64, {{ImportStubVA, "_getopt_long"}},
                             &Image, BinaryFormat::MachO);
  EXPECT_EQ(Second, nullptr);
}

TEST(MachOLLVMInteriorCodePointerBoundary,
     UsesSerialEmissionOnlyForInteriorCodePointers) {
  constexpr va_t OwnerVA = TextVA + 0x700;
  constexpr va_t InteriorVA = TextVA + 0x720;
  BinaryImage Image = makeLLVMImage();
  writeObject(Image.Segments[1].Data, 40, InteriorVA);
  Image.CodePtrRelocSlots.insert(DataVA + 40);
  const std::vector<MedFunc> Functions{
      makeReturnFunction("shard_owner", OwnerVA, InteriorVA)};
  EXPECT_TRUE(PipelineTestPeer::requiresSerialLLVMEmission(Functions, Image));

  writeObject(Image.Segments[1].Data, 40, OwnerVA);
  EXPECT_FALSE(PipelineTestPeer::requiresSerialLLVMEmission(Functions, Image));
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
  Image.CodePtrRelocSlots.insert(DataVA);
  Image.DataPtrRelocSlots.insert(DataVA);
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
  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA), 0u);
  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA), 0u);

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

TEST(MachOClassicRebaseBoundary, CollectsDyldInfoRegion) {
  constexpr uint32_t RebaseOff = 0x100;
  constexpr uint32_t RebaseSize = 0x20;
  std::vector<uint8_t> Binary(0x200, 0);

  mach_header_64 Header{};
  Header.magic = MH_MAGIC_64;
  Header.cputype = CPU_TYPE_ARM64;
  Header.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
  Header.filetype = MH_EXECUTE;
  Header.ncmds = 1;
  Header.sizeofcmds = sizeof(dyld_info_command);
  writeObject(Binary, 0, Header);

  dyld_info_command Command{};
  Command.cmd = LC_DYLD_INFO_ONLY;
  Command.cmdsize = sizeof(Command);
  Command.rebase_off = RebaseOff;
  Command.rebase_size = RebaseSize;
  writeObject(Binary, sizeof(Header), Command);

  llvm::StringRef Bytes(reinterpret_cast<const char *>(Binary.data()),
                        Binary.size());
  auto ObjectOrError = llvm::object::ObjectFile::createMachOObjectFile(
      llvm::MemoryBufferRef(Bytes, "classic-rebase-dyld-info"));
  ASSERT_TRUE(static_cast<bool>(ObjectOrError))
      << llvm::toString(ObjectOrError.takeError());

  macho_loader::DyldInfoOffsets DyldInfo;
  macho_loader::parseDyldInfoLoadCommands(**ObjectOrError, DyldInfo);
  EXPECT_EQ(DyldInfo.RebaseOff, RebaseOff);
  EXPECT_EQ(DyldInfo.RebaseSize, RebaseSize);
}

TEST(MachOClassicRebaseBoundary, RecordsDataPointerSlot) {
  constexpr uint32_t RebaseOff = 0x20;
  std::vector<uint8_t> Binary(0x100, 0);
  const std::vector<uint8_t> Stream = {
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_POINTER),
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1), 8,
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1),
      static_cast<uint8_t>(REBASE_OPCODE_DONE)};
  std::memcpy(Binary.data() + RebaseOff, Stream.data(), Stream.size());

  BinaryImage Image = makeChainedImage();
  writeObject(Image.Segments[1].Data, 8, CStringVA);
  Image.CodePtrRelocSlots.clear();
  Image.DataPtrRelocSlots.clear();
  Image.CodePtrRelocSlots.insert(DataVA + 8);

  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.RebaseOff = RebaseOff;
  DyldInfo.RebaseSize = Stream.size();
  macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                  Image);

  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 8), 1u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 8), 0u);
}

TEST(MachOClassicRebaseBoundary, AddsExplicitAddressDelta) {
  constexpr uint32_t RebaseOff = 0x20;
  std::vector<uint8_t> Binary(0x100, 0);
  const std::vector<uint8_t> Stream = {
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_POINTER),
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1),
      8,
      static_cast<uint8_t>(REBASE_OPCODE_ADD_ADDR_ULEB),
      16,
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1),
      static_cast<uint8_t>(REBASE_OPCODE_DONE)};
  std::memcpy(Binary.data() + RebaseOff, Stream.data(), Stream.size());

  BinaryImage Image = makeChainedImage();
  writeObject(Image.Segments[1].Data, 24, CodeVA);
  Image.CodePtrRelocSlots.clear();
  Image.DataPtrRelocSlots.clear();
  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.RebaseOff = RebaseOff;
  DyldInfo.RebaseSize = Stream.size();

  macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                  Image);

  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 24), 1u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 8), 0u);
}

TEST(MachOClassicRebaseBoundary, AddsScaledImmediateAddressDelta) {
  constexpr uint32_t RebaseOff = 0x20;
  std::vector<uint8_t> Binary(0x100, 0);
  const std::vector<uint8_t> Stream = {
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_POINTER),
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1),
      8,
      static_cast<uint8_t>(REBASE_OPCODE_ADD_ADDR_IMM_SCALED | 2),
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1),
      static_cast<uint8_t>(REBASE_OPCODE_DONE)};
  std::memcpy(Binary.data() + RebaseOff, Stream.data(), Stream.size());

  BinaryImage Image = makeChainedImage();
  writeObject(Image.Segments[1].Data, 24, CodeVA);
  Image.CodePtrRelocSlots.clear();
  Image.DataPtrRelocSlots.clear();
  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.RebaseOff = RebaseOff;
  DyldInfo.RebaseSize = Stream.size();

  macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                  Image);

  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 24), 1u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 8), 0u);
}

TEST(MachOClassicRebaseBoundary, RecordsULEBCountAcrossTargetClasses) {
  constexpr uint32_t RebaseOff = 0x20;
  std::vector<uint8_t> Binary(0x100, 0);
  const std::vector<uint8_t> Stream = {
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_POINTER),
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1),
      8,
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_ULEB_TIMES),
      3,
      static_cast<uint8_t>(REBASE_OPCODE_DONE)};
  std::memcpy(Binary.data() + RebaseOff, Stream.data(), Stream.size());

  BinaryImage Image = makeChainedImage();
  writeObject(Image.Segments[1].Data, 8, CStringVA);
  writeObject(Image.Segments[1].Data, 16, CodeVA);
  writeObject(Image.Segments[1].Data, 24, WritableVA + 8);
  Image.CodePtrRelocSlots.clear();
  Image.DataPtrRelocSlots.clear();
  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.RebaseOff = RebaseOff;
  DyldInfo.RebaseSize = Stream.size();

  macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                  Image);

  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 8), 1u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 16), 1u);
  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 24), 1u);
}

TEST(MachOClassicRebaseBoundary, RebasesThenAddsULEBAddressDelta) {
  constexpr uint32_t RebaseOff = 0x20;
  std::vector<uint8_t> Binary(0x100, 0);
  const std::vector<uint8_t> Stream = {
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_POINTER),
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1),
      8,
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB),
      8,
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1),
      static_cast<uint8_t>(REBASE_OPCODE_DONE)};
  std::memcpy(Binary.data() + RebaseOff, Stream.data(), Stream.size());

  BinaryImage Image = makeChainedImage();
  writeObject(Image.Segments[1].Data, 8, CStringVA);
  writeObject(Image.Segments[1].Data, 24, CodeVA);
  Image.CodePtrRelocSlots.clear();
  Image.DataPtrRelocSlots.clear();
  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.RebaseOff = RebaseOff;
  DyldInfo.RebaseSize = Stream.size();

  macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                  Image);

  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 8), 1u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 24), 1u);
}

TEST(MachOClassicRebaseBoundary, RecordsULEBCountWithSkipping) {
  constexpr uint32_t RebaseOff = 0x20;
  std::vector<uint8_t> Binary(0x100, 0);
  const std::vector<uint8_t> Stream = {
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_POINTER),
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1),
      8,
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB),
      3,
      8,
      static_cast<uint8_t>(REBASE_OPCODE_DONE)};
  std::memcpy(Binary.data() + RebaseOff, Stream.data(), Stream.size());

  BinaryImage Image = makeChainedImage();
  writeObject(Image.Segments[1].Data, 8, CStringVA);
  writeObject(Image.Segments[1].Data, 24, CodeVA);
  writeObject(Image.Segments[1].Data, 40, WritableVA + 8);
  Image.CodePtrRelocSlots.clear();
  Image.DataPtrRelocSlots.clear();
  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.RebaseOff = RebaseOff;
  DyldInfo.RebaseSize = Stream.size();

  macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                  Image);

  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 8), 1u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 24), 1u);
  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 40), 1u);
}

TEST(MachOClassicRebaseBoundary, UsesImagePointerWidth) {
  constexpr va_t Text32VA = 0x1000;
  constexpr va_t Code32VA = Text32VA + 0x40;
  constexpr va_t CString32VA = Text32VA + 0x80;
  constexpr va_t Data32VA = 0x2000;
  constexpr uint32_t RebaseOff = 0x20;
  std::vector<uint8_t> Binary(0x100, 0);
  const std::vector<uint8_t> Stream = {
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_POINTER),
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1), 0,
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 2),
      static_cast<uint8_t>(REBASE_OPCODE_DONE)};
  std::memcpy(Binary.data() + RebaseOff, Stream.data(), Stream.size());

  BinaryImage Image;
  Image.Arch = Arch::ARM;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits32;
  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = Text32VA;
  Text.Size = 0x100;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));
  Segment Data;
  Data.Name = "__DATA_CONST";
  Data.VA = Data32VA;
  Data.Size = 0x40;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  writeObject(Data.Data, 0, static_cast<uint32_t>(CString32VA));
  writeObject(Data.Data, 4, static_cast<uint32_t>(Code32VA));
  Image.Segments.push_back(std::move(Data));
  Section Code;
  Code.Name = "__text";
  Code.SegmentName = "__TEXT";
  Code.VA = Text32VA;
  Code.Size = 0x80;
  Code.FileSz = Code.Size;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Type = static_cast<uint32_t>(S_REGULAR) |
              static_cast<uint32_t>(S_ATTR_PURE_INSTRUCTIONS);
  Image.Sections.push_back(std::move(Code));
  Section CString;
  CString.Name = "__cstring";
  CString.SegmentName = "__TEXT";
  CString.VA = CString32VA;
  CString.Size = 0x20;
  CString.FileSz = CString.Size;
  CString.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  CString.Type = S_CSTRING_LITERALS;
  Image.Sections.push_back(std::move(CString));

  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.RebaseOff = RebaseOff;
  DyldInfo.RebaseSize = Stream.size();
  macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                  Image);

  EXPECT_EQ(Image.DataPtrRelocSlots.count(Data32VA), 1u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(Data32VA + 4), 1u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(Data32VA + 8), 0u);
}

TEST(MachOClassicRebaseBoundary, AdvancesWithoutClassifyingTextRebaseTypes) {
  constexpr uint32_t RebaseOff = 0x20;
  std::vector<uint8_t> Binary(0x100, 0);
  const std::vector<uint8_t> Stream = {
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_TEXT_ABSOLUTE32),
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1),
      8,
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1),
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_TEXT_PCREL32),
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1),
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_POINTER),
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1),
      static_cast<uint8_t>(REBASE_OPCODE_DONE)};
  std::memcpy(Binary.data() + RebaseOff, Stream.data(), Stream.size());

  BinaryImage Image = makeChainedImage();
  writeObject(Image.Segments[1].Data, 8, CStringVA);
  writeObject(Image.Segments[1].Data, 16, CStringBVA);
  writeObject(Image.Segments[1].Data, 24, CodeVA);
  Image.CodePtrRelocSlots.clear();
  Image.DataPtrRelocSlots.clear();
  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.RebaseOff = RebaseOff;
  DyldInfo.RebaseSize = Stream.size();

  macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                  Image);

  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 8), 0u);
  EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 16), 0u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 24), 1u);
}

TEST(MachOClassicRebaseBoundary, RejectsMalformedStreamsAndPartialRuns) {
  constexpr uint32_t RebaseOff = 0x20;
  const uint8_t SetPointer = static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
                             static_cast<uint8_t>(REBASE_TYPE_POINTER);
  const uint8_t SetDataSegment =
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1);
  const uint8_t RebaseOne =
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1);

  auto ExpectRejected = [&](const char *Label,
                            const std::vector<uint8_t> &Stream,
                            uint32_t DeclaredOff = 0x20,
                            uint32_t DeclaredSize = 0) {
    SCOPED_TRACE(Label);
    std::vector<uint8_t> Binary(0x100, 0);
    if (rangeInBounds(DeclaredOff, Stream.size(), Binary.size()))
      std::memcpy(Binary.data() + DeclaredOff, Stream.data(), Stream.size());
    BinaryImage Image = makeChainedImage();
    writeObject(Image.Segments[1].Data, 8, CStringVA);
    writeObject(Image.Segments[1].Data, 16, CodeVA);
    writeObject(Image.Segments[1].Data, 0xf0, CStringVA);
    writeObject(Image.Segments[1].Data, 0xf8, CodeVA);
    Image.CodePtrRelocSlots.clear();
    Image.DataPtrRelocSlots.clear();
    macho_loader::DyldInfoOffsets DyldInfo;
    DyldInfo.RebaseOff = DeclaredOff;
    DyldInfo.RebaseSize = DeclaredSize ? DeclaredSize : Stream.size();
    macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                    Image);
    EXPECT_TRUE(Image.CodePtrRelocSlots.empty());
    EXPECT_TRUE(Image.DataPtrRelocSlots.empty());
  };

  ExpectRejected("metadata range outside file", {SetPointer}, 0xf8, 0x20);
  ExpectRejected("truncated ULEB", {SetPointer, SetDataSegment, 0x80});
  ExpectRejected("missing segment", {SetPointer, RebaseOne});
  ExpectRejected(
      "invalid segment cannot be overwritten",
      {SetPointer,
       static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 15), 0,
       SetDataSegment, 8, RebaseOne});
  ExpectRejected(
      "invalid offset cannot be overwritten",
      {SetPointer, SetDataSegment, 0x80, 0x02, SetDataSegment, 8, RebaseOne});
  ExpectRejected("invalid address addition cannot be overwritten",
                 {SetPointer, SetDataSegment, 0xf8, 0x01,
                  static_cast<uint8_t>(REBASE_OPCODE_ADD_ADDR_ULEB), 8,
                  SetDataSegment, 8, RebaseOne});
  ExpectRejected("unknown opcode",
                 {SetPointer, 0x90, SetDataSegment, 8, RebaseOne});
  ExpectRejected("unknown type",
                 {static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
                      static_cast<uint8_t>(15),
                  SetDataSegment, 8, RebaseOne});
  ExpectRejected("excessive count",
                 {SetPointer, SetDataSegment, 0,
                  static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_ULEB_TIMES),
                  0x81, 0x80, 0x80, 0x02});
  ExpectRejected("escaping repeated run is atomic",
                 {SetPointer, SetDataSegment, 0xf0, 0x01,
                  static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_ULEB_TIMES), 3});
}

TEST(MachOClassicRebaseBoundary, ImportBindingOwnsSlotInEitherParseOrder) {
  constexpr uint32_t RebaseOff = 0x20;
  constexpr uint32_t BindOff = 0x60;
  std::vector<uint8_t> Binary(0x100, 0);
  const std::vector<uint8_t> RebaseStream = {
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_POINTER),
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1), 8,
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1),
      static_cast<uint8_t>(REBASE_OPCODE_DONE)};
  std::vector<uint8_t> BindStream = {
      static_cast<uint8_t>(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM)};
  constexpr char Name[] = "_imported_pointer";
  BindStream.insert(BindStream.end(), Name, Name + sizeof(Name));
  BindStream.insert(
      BindStream.end(),
      {static_cast<uint8_t>(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1), 8,
       static_cast<uint8_t>(BIND_OPCODE_DO_BIND),
       static_cast<uint8_t>(BIND_OPCODE_DONE)});
  std::memcpy(Binary.data() + RebaseOff, RebaseStream.data(),
              RebaseStream.size());
  std::memcpy(Binary.data() + BindOff, BindStream.data(), BindStream.size());

  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.RebaseOff = RebaseOff;
  DyldInfo.RebaseSize = RebaseStream.size();
  DyldInfo.BindOff = BindOff;
  DyldInfo.BindSize = BindStream.size();

  auto ExpectImportOwnership = [&](bool BindFirst) {
    SCOPED_TRACE(BindFirst ? "bind before rebase" : "rebase before bind");
    BinaryImage Image = makeChainedImage();
    writeObject(Image.Segments[1].Data, 8, CStringVA);
    Image.CodePtrRelocSlots.clear();
    Image.DataPtrRelocSlots.clear();
    if (BindFirst) {
      macho_loader::parseBindStreams(Binary.data(), Binary.size(), DyldInfo,
                                     Image);
      macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                      Image);
    } else {
      macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                      Image);
      macho_loader::parseBindStreams(Binary.data(), Binary.size(), DyldInfo,
                                     Image);
    }

    ASSERT_EQ(Image.DyldBindSlots.count(DataVA + 8), 1u);
    EXPECT_EQ(Image.DyldBindSlots.at(DataVA + 8).Name, Name);
    EXPECT_EQ(Image.CodePtrRelocSlots.count(DataVA + 8), 0u);
    EXPECT_EQ(Image.DataPtrRelocSlots.count(DataVA + 8), 0u);
  };

  ExpectImportOwnership(false);
  ExpectImportOwnership(true);
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
  Image.DataPtrRelocSlots.clear();
  constexpr uint32_t RebaseOff = 0x10;
  const std::vector<uint8_t> RebaseStream = {
      static_cast<uint8_t>(REBASE_OPCODE_SET_TYPE_IMM) |
          static_cast<uint8_t>(REBASE_TYPE_POINTER),
      static_cast<uint8_t>(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | 1),
      16,
      static_cast<uint8_t>(REBASE_OPCODE_DO_REBASE_ULEB_TIMES),
      2,
      static_cast<uint8_t>(REBASE_OPCODE_DONE)};
  std::vector<uint8_t> Binary(0x40, 0);
  std::memcpy(Binary.data() + RebaseOff, RebaseStream.data(),
              RebaseStream.size());
  macho_loader::DyldInfoOffsets DyldInfo;
  DyldInfo.RebaseOff = RebaseOff;
  DyldInfo.RebaseSize = RebaseStream.size();
  macho_loader::parseRebaseStream(Binary.data(), Binary.size(), DyldInfo,
                                  Image);
  ASSERT_EQ(Image.DataPtrRelocSlots.count(DataVA + 16), 1u);
  ASSERT_EQ(Image.DataPtrRelocSlots.count(DataVA + 24), 1u);

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

TEST(MachOLLVMImportPointerBoundary,
     PromotesCanonicalPersonalityPlaceholderForNativeEH) {
  constexpr va_t EHFunctionVA = TextVA + 0x700;
  constexpr va_t MayThrowVA = TextVA + 0x780;
  BinaryImage Image = makeLLVMImage();
  Image.DyldBindSlots[DataVA] = {"___gxx_personality_v0", 0};

  MedFunc PointerCaller =
      makePointerCall("personality_pointer_caller", ImportStubVA,
                      {MedVar::makeConst(DataVA, 8)});
  MedFunc EHFunction =
      makeCleanupFunction("native_eh_caller", EHFunctionVA, MayThrowVA);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {PointerCaller, EHFunction}, Context, "macho-personality-placeholder",
      Arch::AArch64,
      {{ImportStubVA, "_getopt_long"}, {MayThrowVA, "may_throw"}}, &Image,
      BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::Function *Emitted = Module->getFunction(EHFunction.Name);
  ASSERT_NE(Emitted, nullptr);
  ASSERT_TRUE(Emitted->hasPersonalityFn());
  llvm::Function *Personality = Module->getFunction("__gxx_personality_v0");
  ASSERT_NE(Personality, nullptr);
  EXPECT_EQ(Emitted->getPersonalityFn()->stripPointerCasts(), Personality);
  EXPECT_EQ(Module->getNamedGlobal("__gxx_personality_v0"), nullptr);

  unsigned Invokes = 0;
  for (llvm::BasicBlock &Block : *Emitted)
    for (llvm::Instruction &Instruction : Block)
      Invokes += llvm::isa<llvm::InvokeInst>(Instruction);
  EXPECT_EQ(Invokes, 1u);

  const std::string MirrorName =
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str();
  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(MirrorName);
  ASSERT_NE(Mirror, nullptr);
  ASSERT_TRUE(Mirror->hasInitializer());
  EXPECT_TRUE(constantReferences(Mirror->getInitializer(), Personality));
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
