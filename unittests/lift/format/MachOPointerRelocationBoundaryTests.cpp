//===- MachOPointerRelocationBoundaryTests.cpp ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
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

class MedLLVMProvenanceTestPeer {
public:
  using WorkCounts = std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>;

  static WorkCounts addressProvenanceWork(const MedLLVMEmitter &Emitter) {
    const auto &Work = Emitter.AddressProvenanceWork;
    return {Work.EdgeClassifications, Work.RecurrenceProofs,
            Work.StableOffsetProofs, Work.IndexedBaseProofs};
  }

  static void resetAddressProvenanceWork(MedLLVMEmitter &Emitter) {
    Emitter.AddressProvenanceWork = {};
  }

  static bool edgeIsProven(MedLLVMEmitter &Emitter, const PhiNode &Phi,
                           int PredId) {
    return Emitter.classifyPhiIncomingEdge(Phi, PredId) ==
           MedLLVMEmitter::PhiEdgeFeasibility::ProvenFeasible;
  }

  static bool incomingIsRecurrent(MedLLVMEmitter &Emitter, const PhiNode &Phi,
                                  int PredId, const MedVar &Arg) {
    return Emitter.phiIncomingIsRecurrent(Phi, PredId, Arg);
  }

  static bool stableOffset(MedLLVMEmitter &Emitter, const MedVar &Value,
                           const MedVar *Forbidden) {
    return Emitter.valueIsStableAddressOffset(Value, Forbidden);
  }

  static bool indexedBase(MedLLVMEmitter &Emitter, const MedVar &Address) {
    uint64_t Base = 0;
    bool HaveBase = false;
    std::vector<MedVar> Terms;
    return Emitter.collectIndexedGlobalBase(Address, Base, HaveBase, Terms);
  }

  static bool symbolizesDataConstant(const MedLLVMEmitter &Emitter,
                                     uint64_t Value, uint16_t Size) {
    return Emitter.getVarSymbolizesDataConstant(Value, Size);
  }

  static bool mayRelocateConstant(const MedLLVMEmitter &Emitter, uint64_t Value,
                                  uint16_t Size) {
    return Emitter.getVarMayRelocateConstant(Value, Size);
  }

  static bool collectFrameReloadSources(MedLLVMEmitter &Emitter,
                                        const MedFunc &Func, Arch TargetArch,
                                        const MedOp &Load,
                                        std::vector<MedVar> &Sources) {
    Emitter.TargetArch = TargetArch;
    Emitter.CurMedFunc = &Func;
    return Emitter.collectFrameReloadSources(Load, Sources);
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

constexpr va_t SpilledConstTableVA = TextVA + 0x800;
constexpr va_t OtherSpilledConstTableVA = SpilledConstTableVA + 0x20;
constexpr va_t FarSpilledConstTableVA = TextVA + 0x3000;
constexpr va_t LowSpilledConstTableVA = 0x880;
constexpr va_t OtherLowSpilledConstTableVA = 0x8A0;
constexpr va_t SymbolizedSameRunTableVA = 0x1200;
constexpr va_t OtherSymbolizedSameRunTableVA = 0x1220;
constexpr va_t SignBitTableVA = 0x80001000;
constexpr va_t OtherSignBitTableVA = SignBitTableVA + 0x20;
constexpr va_t ScalarLiteralVA = TextVA + 0x5C0;

BinaryImage makeSpilledConstTableImage(Arch TargetArch) {
  BinaryImage Image;
  Image.Arch = TargetArch;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.Base = TextVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = TextVA;
  Text.Size = 0x1000;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  const uint16_t Values[] = {10, 20, 30, 40};
  std::memcpy(Text.Data.data() + (SpilledConstTableVA - TextVA), Values,
              sizeof(Values));
  const uint16_t OtherValues[] = {50, 60, 70, 80};
  std::memcpy(Text.Data.data() + (OtherSpilledConstTableVA - TextVA),
              OtherValues, sizeof(OtherValues));
  Image.Segments.push_back(std::move(Text));

  Section Code;
  Code.Name = "__text";
  Code.SegmentName = "__TEXT";
  Code.VA = CallerVA;
  Code.Size = 0x40;
  Code.FileSz = Code.Size;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Type = static_cast<uint32_t>(S_REGULAR) |
              static_cast<uint32_t>(S_ATTR_PURE_INSTRUCTIONS) |
              static_cast<uint32_t>(S_ATTR_SOME_INSTRUCTIONS);
  Image.Sections.push_back(std::move(Code));

  Section Const;
  Const.Name = "__const";
  Const.SegmentName = "__TEXT";
  Const.VA = SpilledConstTableVA;
  Const.Size = 0x40;
  Const.FileSz = Const.Size;
  Const.Flags = SegmentFlags::Readable;
  Const.Type = S_REGULAR;
  Image.Sections.push_back(std::move(Const));

  Symbol Table;
  Table.Name = "_spilled_const_table";
  Table.Addr = SpilledConstTableVA;
  Table.Size = sizeof(Values);
  Image.Symbols.push_back(std::move(Table));
  Symbol OtherTable;
  OtherTable.Name = "_other_spilled_const_table";
  OtherTable.Addr = OtherSpilledConstTableVA;
  OtherTable.Size = sizeof(OtherValues);
  Image.Symbols.push_back(std::move(OtherTable));
  return Image;
}

void addMachOPageZero(BinaryImage &Image) {
  Image.Base = 0;
  Segment PageZero;
  PageZero.Name = "__PAGEZERO";
  PageZero.VA = 0;
  PageZero.Size = TextVA;
  PageZero.Flags = SegmentFlags::None;
  Image.Segments.push_back(std::move(PageZero));
}

void addThresholdCrossingConstTableRun(BinaryImage &Image) {
  Segment LowData;
  LowData.Name = "__LOW_CONST";
  LowData.VA = 0x800;
  LowData.Size = 0xC00;
  LowData.FileSz = LowData.Size;
  LowData.Flags = SegmentFlags::Readable;
  LowData.Data.assign(LowData.Size, 0);
  const uint16_t FirstValues[] = {11, 22, 33, 44};
  const uint16_t SecondValues[] = {55, 66, 77, 88};
  const uint16_t SymbolizedValues[] = {99, 111, 123, 135};
  const uint16_t OtherSymbolizedValues[] = {147, 159, 171, 183};
  std::memcpy(LowData.Data.data() + (LowSpilledConstTableVA - LowData.VA),
              FirstValues, sizeof(FirstValues));
  std::memcpy(LowData.Data.data() + (OtherLowSpilledConstTableVA - LowData.VA),
              SecondValues, sizeof(SecondValues));
  std::memcpy(LowData.Data.data() + (SymbolizedSameRunTableVA - LowData.VA),
              SymbolizedValues, sizeof(SymbolizedValues));
  std::memcpy(LowData.Data.data() +
                  (OtherSymbolizedSameRunTableVA - LowData.VA),
              OtherSymbolizedValues, sizeof(OtherSymbolizedValues));
  Image.Segments.push_back(std::move(LowData));

  Section LowConst;
  LowConst.Name = "__low_const";
  LowConst.SegmentName = "__LOW_CONST";
  LowConst.VA = 0x800;
  LowConst.Size = 0xC00;
  LowConst.FileSz = LowConst.Size;
  LowConst.Flags = SegmentFlags::Readable;
  LowConst.Type = S_REGULAR;
  Image.Sections.push_back(std::move(LowConst));
}

void addAdjacentLowPointerTableSegments(BinaryImage &Image) {
  auto addSegment = [&](llvm::StringRef Name, va_t VA, va_t Target) {
    Segment Data;
    Data.Name = Name.str();
    Data.VA = VA;
    Data.Size = 0x20;
    Data.FileSz = Data.Size;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.assign(Data.Size, 0);
    writeObject(Data.Data, 0, Target);
    Image.Segments.push_back(std::move(Data));

    Section Const;
    Const.Name = (Name + "_const").str();
    Const.SegmentName = Name.str();
    Const.VA = VA;
    Const.Size = 0x20;
    Const.FileSz = Const.Size;
    Const.Flags = SegmentFlags::Readable;
    Const.Type = S_REGULAR;
    Image.Sections.push_back(std::move(Const));
    Image.DataPtrRelocSlots.insert(VA);
  };
  addSegment("__LOW_PTR_A", LowSpilledConstTableVA, SpilledConstTableVA);
  addSegment("__LOW_PTR_B", OtherLowSpilledConstTableVA,
             OtherSpilledConstTableVA);
}

void addSignBitConstTableRun(BinaryImage &Image) {
  Segment Data;
  Data.Name = "__SIGN_CONST";
  Data.VA = SignBitTableVA;
  Data.Size = 0x40;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.assign(Data.Size, 0);
  const uint16_t FirstValues[] = {13, 26, 39, 52};
  const uint16_t SecondValues[] = {65, 78, 91, 104};
  std::memcpy(Data.Data.data(), FirstValues, sizeof(FirstValues));
  std::memcpy(Data.Data.data() + (OtherSignBitTableVA - SignBitTableVA),
              SecondValues, sizeof(SecondValues));
  Image.Segments.push_back(std::move(Data));

  Section Const;
  Const.Name = "__sign_const";
  Const.SegmentName = "__SIGN_CONST";
  Const.VA = SignBitTableVA;
  Const.Size = 0x40;
  Const.FileSz = Const.Size;
  Const.Flags = SegmentFlags::Readable;
  Const.Type = S_REGULAR;
  Image.Sections.push_back(std::move(Const));
}

void addFarSpilledConstTable(BinaryImage &Image) {
  Segment Far;
  Far.Name = "__FAR_CONST";
  Far.VA = FarSpilledConstTableVA;
  Far.Size = 0x40;
  Far.FileSz = Far.Size;
  Far.Flags = SegmentFlags::Readable;
  Far.Data.assign(Far.Size, 0);
  const uint16_t Values[] = {90, 100, 110, 120};
  std::memcpy(Far.Data.data(), Values, sizeof(Values));
  Image.Segments.push_back(std::move(Far));

  Section Const;
  Const.Name = "__const_far";
  Const.SegmentName = "__FAR_CONST";
  Const.VA = FarSpilledConstTableVA;
  Const.Size = 0x40;
  Const.FileSz = Const.Size;
  Const.Flags = SegmentFlags::Readable;
  Const.Type = S_REGULAR;
  Image.Sections.push_back(std::move(Const));
}

MedFunc makeSpilledConstTableLookup(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = TargetArch == Arch::AArch64 ? "spilled_const_table_arm64"
                                          : "spilled_const_table_x86_64";
  Func.ReturnType = NdType::makeInt(2);
  Func.FrameSize = 16;

  MedVar Index = makeVar(MedVar::Param, 0, TRI.PointerSize);
  Index.RegOff = TRI.IntParamRegs.front();
  Func.Params.push_back(Index);

  MedVar SP = makeVar(MedVar::Reg, 100, TRI.PointerSize);
  SP.SSAVer = 0;
  SP.RegOff = TRI.StackPointer;
  MedVar Slot = makeVar(MedVar::Temp, 1, TRI.PointerSize);
  MedVar TableBase = makeVar(MedVar::Temp, 2, TRI.PointerSize);
  MedVar ReloadedBase = makeVar(MedVar::Temp, 3, TRI.PointerSize);
  MedVar ScaledIndex = makeVar(MedVar::Temp, 4, TRI.PointerSize);
  MedVar ElementAddr = makeVar(MedVar::Temp, 5, TRI.PointerSize);
  MedVar Element = makeVar(MedVar::Temp, 6, 2);

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;

  MedOp FormSlot;
  FormSlot.Opcode = NdOp::INT_ADD;
  FormSlot.Output = Slot;
  FormSlot.addInput(SP);
  FormSlot.addInput(MedVar::makeConst(uint64_t(-8), TRI.PointerSize));
  Block.Ops.push_back(std::move(FormSlot));

  MedOp MaterializeBase;
  MaterializeBase.Opcode = NdOp::COPY;
  MaterializeBase.Output = TableBase;
  MaterializeBase.addInput(
      MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  Block.Ops.push_back(std::move(MaterializeBase));

  MedOp Spill;
  Spill.Opcode = NdOp::STORE;
  Spill.addInput(Slot);
  Spill.addInput(TableBase);
  Block.Ops.push_back(std::move(Spill));

  MedOp Reload;
  Reload.Opcode = NdOp::LOAD;
  Reload.Output = ReloadedBase;
  Reload.addInput(Slot);
  Block.Ops.push_back(std::move(Reload));

  MedOp Scale;
  Scale.Opcode = NdOp::INT_LEFT;
  Scale.Output = ScaledIndex;
  Scale.addInput(Index);
  Scale.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Block.Ops.push_back(std::move(Scale));

  MedOp FormElementAddr;
  FormElementAddr.Opcode = NdOp::INT_ADD;
  FormElementAddr.Output = ElementAddr;
  FormElementAddr.addInput(ReloadedBase);
  FormElementAddr.addInput(ScaledIndex);
  Block.Ops.push_back(std::move(FormElementAddr));

  MedOp ReadElement;
  ReadElement.Opcode = NdOp::LOAD;
  ReadElement.Output = Element;
  ReadElement.addInput(ElementAddr);
  Block.Ops.push_back(std::move(ReadElement));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Element);
  Block.Ops.push_back(std::move(Return));
  Block.EndAddr = CallerVA + 0x20;
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeMaskedScalarPhiIndexLookup() {
  constexpr uint16_t PointerSize = 8;
  auto makeVar = [](MedVar::VarKind Kind, int Id, int SSAVer, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = Arch::AArch64;
    V.Id = Id;
    V.SSAVer = SSAVer;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "masked_scalar_phi_table_index_arm64";
  Func.ReturnType = NdType::makeVoid();
  Func.DoesNotReturn = true;

  MedVar ScalarIndex = makeVar(MedVar::Reg, 10, 1, PointerSize);
  ScalarIndex.RegOff = 10 * PointerSize;
  MedVar NextIndex = makeVar(MedVar::Reg, 10, 2, PointerSize);
  NextIndex.RegOff = ScalarIndex.RegOff;
  MedVar TableBase = makeVar(MedVar::Temp, 20, 1, PointerSize);
  MedVar MaskedIndex = makeVar(MedVar::Temp, 21, 1, PointerSize);
  MedVar ScaledIndex = makeVar(MedVar::Temp, 22, 1, PointerSize);
  MedVar ElementAddr = makeVar(MedVar::Temp, 23, 1, PointerSize);
  MedVar Element = makeVar(MedVar::Temp, 24, 1, 2);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 8;
  Entry.Succs = {1};
  MedOp MaterializeBase;
  MaterializeBase.Opcode = NdOp::COPY;
  MaterializeBase.Output = TableBase;
  MaterializeBase.addInput(MedVar::makeConst(SpilledConstTableVA, PointerSize));
  Entry.Ops.push_back(std::move(MaterializeBase));
  MedOp EnterLoop;
  EnterLoop.Opcode = NdOp::BRANCH;
  EnterLoop.addInput(MedVar::makeConst(CallerVA + 0x10, PointerSize));
  Entry.Ops.push_back(std::move(EnterLoop));

  MedBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = CallerVA + 0x10;
  Loop.EndAddr = CallerVA + 0x24;
  Loop.Preds = {0, 2};
  Loop.Succs = {2};
  PhiNode IndexPhi;
  IndexPhi.Output = ScalarIndex;
  IndexPhi.Args = {{0, MedVar::makeConst(0, PointerSize)}, {2, NextIndex}};
  Loop.Phis.push_back(std::move(IndexPhi));
  MedOp Mask;
  Mask.Opcode = NdOp::INT_AND;
  Mask.Output = MaskedIndex;
  Mask.addInput(ScalarIndex);
  Mask.addInput(MedVar::makeConst(0xffffffffULL, PointerSize));
  Loop.Ops.push_back(std::move(Mask));
  MedOp Scale;
  Scale.Opcode = NdOp::INT_LEFT;
  Scale.Output = ScaledIndex;
  Scale.addInput(MaskedIndex);
  Scale.addInput(MedVar::makeConst(4, PointerSize));
  Loop.Ops.push_back(std::move(Scale));
  MedOp FormElementAddr;
  FormElementAddr.Opcode = NdOp::INT_ADD;
  FormElementAddr.Output = ElementAddr;
  FormElementAddr.addInput(TableBase);
  FormElementAddr.addInput(ScaledIndex);
  Loop.Ops.push_back(std::move(FormElementAddr));
  MedOp ReadElement;
  ReadElement.Opcode = NdOp::LOAD;
  ReadElement.Output = Element;
  ReadElement.addInput(ElementAddr);
  Loop.Ops.push_back(std::move(ReadElement));
  MedOp Advance;
  Advance.Opcode = NdOp::BRANCH;
  Advance.addInput(MedVar::makeConst(CallerVA + 0x30, PointerSize));
  Loop.Ops.push_back(std::move(Advance));

  MedBlock Latch;
  Latch.Id = 2;
  Latch.StartAddr = CallerVA + 0x30;
  Latch.EndAddr = CallerVA + 0x38;
  Latch.Preds = {1};
  Latch.Succs = {1};
  MedOp Step;
  Step.Opcode = NdOp::INT_ADD;
  Step.Output = NextIndex;
  Step.addInput(ScalarIndex);
  Step.addInput(MedVar::makeConst(1, PointerSize));
  Latch.Ops.push_back(std::move(Step));
  MedOp BackEdge;
  BackEdge.Opcode = NdOp::BRANCH;
  BackEdge.addInput(MedVar::makeConst(Loop.StartAddr, PointerSize));
  Latch.Ops.push_back(std::move(BackEdge));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Latch)};
  return Func;
}

enum class ReachingStoreCase {
  DistinctPredecessorBases,
  BypassPredecessor,
  MalformedMissingBypassPredecessor,
  PartialOverlap,
  StoreAfterLoad,
};

enum class PhiTableBaseCase {
  DeadScalarClobber,
  SameBaseOnReachableEdges,
  AmbiguousRuntimeClobber,
  MalformedRuntimeClobber,
};

MedFunc makeRejectedFrameReloadLookup(Arch TargetArch, ReachingStoreCase Case) {
  MedFunc Func = makeSpilledConstTableLookup(TargetArch);
  Func.Name += "_rejected_" + std::to_string(static_cast<int>(Case));
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  std::vector<MedOp> Ops = Func.Blocks.front().Ops;

  if (Case == ReachingStoreCase::PartialOverlap) {
    MedVar PartialAddr;
    PartialAddr.Kind = MedVar::Temp;
    PartialAddr.TheArch = TargetArch;
    PartialAddr.Id = 20;
    PartialAddr.SSAVer = 1;
    PartialAddr.Size = TRI.PointerSize;
    MedOp FormPartial;
    FormPartial.Opcode = NdOp::INT_ADD;
    FormPartial.Output = PartialAddr;
    FormPartial.addInput(Ops[0].Output);
    FormPartial.addInput(MedVar::makeConst(4, TRI.PointerSize));
    MedOp PartialStore;
    PartialStore.Opcode = NdOp::STORE;
    PartialStore.addInput(PartialAddr);
    PartialStore.addInput(MedVar::makeConst(0, 4));
    Ops.insert(Ops.begin() + 3, std::move(FormPartial));
    Ops.insert(Ops.begin() + 4, std::move(PartialStore));
    Func.Blocks.front().Ops = std::move(Ops);
    return Func;
  }

  if (Case == ReachingStoreCase::StoreAfterLoad) {
    MedOp Spill = Ops[2];
    Ops.erase(Ops.begin() + 2);
    Ops.insert(Ops.begin() + 3, std::move(Spill));
    Func.Blocks.front().Ops = std::move(Ops);
    return Func;
  }

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 4;
  Entry.Succs = {1, 2};
  Entry.Ops.push_back(Ops[0]);
  MedOp Branch;
  Branch.Opcode = NdOp::COND_BR;
  Branch.Addr = CallerVA;
  Branch.addInput(MedVar::makeConst(CallerVA + 0x10, TRI.PointerSize));
  Branch.addInput(Func.Params.front());
  Entry.Ops.push_back(std::move(Branch));

  MedBlock FirstStore;
  FirstStore.Id = 1;
  FirstStore.StartAddr = CallerVA + 8;
  FirstStore.EndAddr = CallerVA + 0x10;
  FirstStore.Preds = {0};
  FirstStore.Succs = {3};
  FirstStore.Ops.push_back(Ops[1]);
  FirstStore.Ops.push_back(Ops[2]);

  MedBlock SecondStore;
  SecondStore.Id = 2;
  SecondStore.StartAddr = CallerVA + 0x10;
  SecondStore.EndAddr = CallerVA + 0x18;
  SecondStore.Preds = {0};
  SecondStore.Succs = {3};
  if (Case == ReachingStoreCase::DistinctPredecessorBases) {
    MedVar OtherBase = Ops[1].Output;
    OtherBase.Id = 21;
    MedOp MaterializeOther = Ops[1];
    MaterializeOther.Output = OtherBase;
    MaterializeOther.Inputs[0] =
        MedVar::makeConst(OtherSpilledConstTableVA, TRI.PointerSize);
    MedOp SpillOther = Ops[2];
    SpillOther.Inputs[1] = OtherBase;
    SecondStore.Ops.push_back(std::move(MaterializeOther));
    SecondStore.Ops.push_back(std::move(SpillOther));
  }

  MedBlock Merge;
  Merge.Id = 3;
  Merge.StartAddr = CallerVA + 0x18;
  Merge.EndAddr = CallerVA + 0x30;
  Merge.Preds = Case == ReachingStoreCase::MalformedMissingBypassPredecessor
                    ? std::vector<int>{1}
                    : std::vector<int>{1, 2};
  Merge.Ops.insert(Merge.Ops.end(), Ops.begin() + 3, Ops.end());

  Func.Blocks.clear();
  Func.Blocks.push_back(std::move(Entry));
  Func.Blocks.push_back(std::move(FirstStore));
  Func.Blocks.push_back(std::move(SecondStore));
  Func.Blocks.push_back(std::move(Merge));
  return Func;
}

MedFunc makeLateLiteralPoolSpillLookup(Arch TargetArch) {
  MedFunc Func = makeSpilledConstTableLookup(TargetArch);
  Func.Name = TargetArch == Arch::AArch64 ? "late_literal_pool_spill_arm64"
                                          : "late_literal_pool_spill_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  std::vector<MedOp> &Ops = Func.Blocks.front().Ops;
  MedVar TableBase = Ops[1].Output;
  MedVar PoolValue = TableBase;
  PoolValue.Id = 20;

  MedOp LoadPool;
  LoadPool.Opcode = NdOp::LOAD;
  LoadPool.Output = PoolValue;
  LoadPool.addInput(MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  Ops[1] = std::move(LoadPool);
  MedOp FormBase;
  FormBase.Opcode = NdOp::INT_ADD;
  FormBase.Output = TableBase;
  FormBase.addInput(PoolValue);
  FormBase.addInput(MedVar::makeConst(CallerVA, TRI.PointerSize));
  Ops.insert(Ops.begin() + 2, std::move(FormBase));

  // Move the matching STORE after the reload. It is a real same-slot store,
  // but cannot define the value already loaded above it.
  MedOp LateStore = std::move(Ops[3]);
  Ops.erase(Ops.begin() + 3);
  Ops.insert(Ops.begin() + 4, std::move(LateStore));
  return Func;
}

MedFunc makeLoopFrameReloadTableOffsetLookup(Arch TargetArch,
                                             bool InitializeInPreheader) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "loop_frame_reload_table_offset_arm64_"
                              : "loop_frame_reload_table_offset_x86_64_") +
              (InitializeInPreheader ? "initialized" : "post_store_only");
  Func.ReturnType = NdType::makeInt(2);
  Func.FrameSize = 16;

  MedVar Offset = makeVar(MedVar::Param, 0, TRI.PointerSize);
  Offset.RegOff = TRI.IntParamRegs[0];
  MedVar Continue = makeVar(MedVar::Param, 1, 1);
  Continue.RegOff = TRI.IntParamRegs[1];
  Func.Params = {Offset, Continue};

  MedVar SP = makeVar(MedVar::Reg, 100, TRI.PointerSize);
  SP.SSAVer = 0;
  SP.RegOff = TRI.StackPointer;
  MedVar Slot = makeVar(MedVar::Temp, 1, TRI.PointerSize);
  MedVar Reloaded = makeVar(MedVar::Temp, 2, TRI.PointerSize);
  MedVar Address = makeVar(MedVar::Temp, 3, TRI.PointerSize);
  MedVar Element = makeVar(MedVar::Temp, 4, 2);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 8;
  Entry.Succs = {1};
  MedOp FormSlot;
  FormSlot.Opcode = NdOp::INT_ADD;
  FormSlot.Output = Slot;
  FormSlot.addInput(SP);
  FormSlot.addInput(MedVar::makeConst(uint64_t(-8), TRI.PointerSize));
  Entry.Ops.push_back(std::move(FormSlot));
  if (InitializeInPreheader) {
    MedOp Initialize;
    Initialize.Opcode = NdOp::STORE;
    Initialize.addInput(Slot);
    Initialize.addInput(Offset);
    Entry.Ops.push_back(std::move(Initialize));
  }
  MedOp EnterLoop;
  EnterLoop.Opcode = NdOp::BRANCH;
  EnterLoop.addInput(MedVar::makeConst(CallerVA + 0x10, TRI.PointerSize));
  Entry.Ops.push_back(std::move(EnterLoop));

  MedBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = CallerVA + 0x10;
  Loop.EndAddr = CallerVA + 0x28;
  Loop.Preds = {0, 1};
  Loop.Succs = {1, 2};
  MedOp Reload;
  Reload.Opcode = NdOp::LOAD;
  Reload.Output = Reloaded;
  Reload.addInput(Slot);
  Loop.Ops.push_back(std::move(Reload));
  if (!InitializeInPreheader) {
    // This store can reach a later iteration but not the first LOAD reached
    // from the entry edge.  It must not manufacture an all-path proof.
    MedOp LateStore;
    LateStore.Opcode = NdOp::STORE;
    LateStore.addInput(Slot);
    LateStore.addInput(Offset);
    Loop.Ops.push_back(std::move(LateStore));
  }
  MedOp FormAddress;
  FormAddress.Opcode = NdOp::INT_ADD;
  FormAddress.Output = Address;
  FormAddress.addInput(MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  FormAddress.addInput(Reloaded);
  Loop.Ops.push_back(std::move(FormAddress));
  MedOp ReadElement;
  ReadElement.Opcode = NdOp::LOAD;
  ReadElement.Output = Element;
  ReadElement.addInput(Address);
  Loop.Ops.push_back(std::move(ReadElement));
  MedOp BackOrExit;
  BackOrExit.Opcode = NdOp::COND_BR;
  BackOrExit.addInput(MedVar::makeConst(Loop.StartAddr, TRI.PointerSize));
  BackOrExit.addInput(Continue);
  Loop.Ops.push_back(std::move(BackOrExit));

  MedBlock Exit;
  Exit.Id = 2;
  Exit.StartAddr = CallerVA + 0x30;
  Exit.EndAddr = CallerVA + 0x34;
  Exit.Preds = {1};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Element);
  Exit.Ops.push_back(std::move(Return));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Exit)};
  return Func;
}

MedFunc makeNarrowFrameReloadSecondTableBaseLookup(Arch TargetArch) {
  MedFunc Func = makeLoopFrameReloadTableOffsetLookup(
      TargetArch, /*InitializeInPreheader=*/true);
  Func.Name = TargetArch == Arch::AArch64
                  ? "narrow_frame_reload_second_table_base_arm64"
                  : "narrow_frame_reload_second_table_base_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);

  // Store only the low 32 bits of an independently relocatable table address,
  // reload them, then widen before adding them to a second table base.  The
  // narrow LOAD is not automatically a scalar index merely because it cannot
  // carry a complete native pointer: its reaching value still has address
  // provenance and must not be added to another relocated base.
  MedOp &Store = Func.Blocks[0].Ops[1];
  Store.Inputs[1] = MedVar::makeConst(OtherSpilledConstTableVA, 4);
  MedOp &Reload = Func.Blocks[1].Ops[0];
  Reload.Output.Size = 4;
  MedVar Narrow = Reload.Output;
  MedVar Wide = Narrow;
  Wide.Id = 5;
  Wide.Size = TRI.PointerSize;
  MedOp Widen;
  Widen.Opcode = NdOp::INT_ZEXT;
  Widen.Output = Wide;
  Widen.addInput(Narrow);
  Func.Blocks[1].Ops[1].Inputs[1] = Wide;
  Func.Blocks[1].Ops.insert(std::next(Func.Blocks[1].Ops.begin()),
                            std::move(Widen));
  return Func;
}

MedFunc makeFrameAliasedPointerTableRecurrence(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, int SSAVer, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = SSAVer;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = TargetArch == Arch::AArch64
                  ? "frame_aliased_pointer_table_recurrence_arm64"
                  : "frame_aliased_pointer_table_recurrence_x86_64";
  Func.ReturnType = NdType::makeInt(TRI.PointerSize);
  Func.FrameSize = 64;

  MedVar Continue = makeVar(MedVar::Param, 0, 0, 1);
  Continue.RegOff = TRI.IntParamRegs.front();
  Func.Params.push_back(Continue);

  MedVar EntrySP = makeVar(MedVar::Reg, 100, 0, TRI.PointerSize);
  EntrySP.RegOff = TRI.StackPointer;
  MedVar FrameSP = makeVar(MedVar::Reg, 100, 1, TRI.PointerSize);
  FrameSP.RegOff = TRI.StackPointer;
  MedVar FramePointer = makeVar(MedVar::Reg, 101, 1, TRI.PointerSize);
  FramePointer.RegOff = TRI.FramePointer;
  MedVar CounterSlot = makeVar(MedVar::Temp, 1, 1, TRI.PointerSize);
  MedVar DisjointSlot = makeVar(MedVar::Temp, 2, 1, TRI.PointerSize);
  MedVar Counter = makeVar(MedVar::Temp, 3, 1, TRI.PointerSize);
  MedVar Scaled = makeVar(MedVar::Temp, 4, 1, TRI.PointerSize);
  MedVar Address = makeVar(MedVar::Temp, 5, 1, TRI.PointerSize);
  MedVar Element = makeVar(MedVar::Temp, 6, 1, TRI.PointerSize);
  MedVar ScalarLiteral = makeVar(MedVar::Temp, 7, 1, TRI.PointerSize);
  MedVar Next = makeVar(MedVar::Temp, 8, 1, TRI.PointerSize);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0x18;
  Entry.Succs = {1};
  MedOp AllocateFrame;
  AllocateFrame.Opcode = NdOp::INT_SUB;
  AllocateFrame.Output = FrameSP;
  AllocateFrame.addInput(EntrySP);
  AllocateFrame.addInput(MedVar::makeConst(64, TRI.PointerSize));
  Entry.Ops.push_back(std::move(AllocateFrame));
  MedOp EstablishFramePointer;
  EstablishFramePointer.Opcode = NdOp::INT_ADD;
  EstablishFramePointer.Output = FramePointer;
  EstablishFramePointer.addInput(FrameSP);
  EstablishFramePointer.addInput(MedVar::makeConst(48, TRI.PointerSize));
  Entry.Ops.push_back(std::move(EstablishFramePointer));
  MedOp FormCounterSlot;
  FormCounterSlot.Opcode = NdOp::INT_ADD;
  FormCounterSlot.Output = CounterSlot;
  FormCounterSlot.addInput(FramePointer);
  FormCounterSlot.addInput(MedVar::makeConst(uint64_t(-16), TRI.PointerSize));
  Entry.Ops.push_back(std::move(FormCounterSlot));
  MedOp InitializeCounter;
  InitializeCounter.Opcode = NdOp::STORE;
  InitializeCounter.addInput(CounterSlot);
  InitializeCounter.addInput(MedVar::makeConst(0, TRI.PointerSize));
  Entry.Ops.push_back(std::move(InitializeCounter));
  MedOp FormDisjointSlot;
  FormDisjointSlot.Opcode = NdOp::INT_ADD;
  FormDisjointSlot.Output = DisjointSlot;
  FormDisjointSlot.addInput(FrameSP);
  FormDisjointSlot.addInput(MedVar::makeConst(8, TRI.PointerSize));
  Entry.Ops.push_back(std::move(FormDisjointSlot));
  MedOp InitializeDisjoint;
  InitializeDisjoint.Opcode = NdOp::STORE;
  InitializeDisjoint.addInput(DisjointSlot);
  InitializeDisjoint.addInput(MedVar::makeConst(0x55, TRI.PointerSize));
  Entry.Ops.push_back(std::move(InitializeDisjoint));
  MedOp EnterLoop;
  EnterLoop.Opcode = NdOp::BRANCH;
  EnterLoop.addInput(MedVar::makeConst(CallerVA + 0x20, TRI.PointerSize));
  Entry.Ops.push_back(std::move(EnterLoop));

  MedBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = CallerVA + 0x20;
  Loop.EndAddr = CallerVA + 0x48;
  Loop.Preds = {0, 1};
  Loop.Succs = {1, 2};
  MedOp ReloadCounter;
  ReloadCounter.Opcode = NdOp::LOAD;
  ReloadCounter.Output = Counter;
  ReloadCounter.addInput(CounterSlot);
  Loop.Ops.push_back(std::move(ReloadCounter));
  MedOp ScaleCounter;
  ScaleCounter.Opcode = NdOp::INT_LEFT;
  ScaleCounter.Output = Scaled;
  ScaleCounter.addInput(Counter);
  ScaleCounter.addInput(MedVar::makeConst(3, TRI.PointerSize));
  Loop.Ops.push_back(std::move(ScaleCounter));
  MedOp FormAddress;
  FormAddress.Opcode = NdOp::INT_ADD;
  FormAddress.Output = Address;
  FormAddress.addInput(MedVar::makeConst(DataVA + 16, TRI.PointerSize));
  FormAddress.addInput(Scaled);
  Loop.Ops.push_back(std::move(FormAddress));
  MedOp ReadElement;
  ReadElement.Opcode = NdOp::LOAD;
  ReadElement.Output = Element;
  ReadElement.addInput(Address);
  Loop.Ops.push_back(std::move(ReadElement));
  MedOp ReadScalarLiteral;
  ReadScalarLiteral.Opcode = NdOp::LOAD;
  ReadScalarLiteral.Output = ScalarLiteral;
  ReadScalarLiteral.addInput(
      MedVar::makeConst(ScalarLiteralVA, TRI.PointerSize));
  Loop.Ops.push_back(std::move(ReadScalarLiteral));
  MedOp AdvanceCounter;
  AdvanceCounter.Opcode = NdOp::INT_XOR;
  AdvanceCounter.Output = Next;
  AdvanceCounter.addInput(Counter);
  AdvanceCounter.addInput(ScalarLiteral);
  Loop.Ops.push_back(std::move(AdvanceCounter));
  MedOp StoreCounter;
  StoreCounter.Opcode = NdOp::STORE;
  StoreCounter.addInput(CounterSlot);
  StoreCounter.addInput(Next);
  Loop.Ops.push_back(std::move(StoreCounter));
  MedOp RewriteDisjoint;
  RewriteDisjoint.Opcode = NdOp::STORE;
  RewriteDisjoint.addInput(DisjointSlot);
  RewriteDisjoint.addInput(MedVar::makeConst(0xAA, TRI.PointerSize));
  Loop.Ops.push_back(std::move(RewriteDisjoint));
  MedOp BackOrExit;
  BackOrExit.Opcode = NdOp::COND_BR;
  BackOrExit.addInput(MedVar::makeConst(Loop.StartAddr, TRI.PointerSize));
  BackOrExit.addInput(Continue);
  Loop.Ops.push_back(std::move(BackOrExit));

  MedBlock Exit;
  Exit.Id = 2;
  Exit.StartAddr = CallerVA + 0x50;
  Exit.EndAddr = CallerVA + 0x54;
  Exit.Preds = {1};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Element);
  Exit.Ops.push_back(std::move(Return));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Exit)};
  return Func;
}

MedFunc makePhiConstTableLookup(Arch TargetArch, PhiTableBaseCase Case) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = (TargetArch == Arch::AArch64 ? "phi_const_table_arm64_"
                                           : "phi_const_table_x86_64_") +
              std::to_string(static_cast<int>(Case));
  Func.ReturnType = NdType::makeInt(2);

  MedVar Index = makeVar(MedVar::Param, 0, TRI.PointerSize);
  Index.RegOff = TRI.IntParamRegs[0];
  MedVar RuntimeCond = makeVar(MedVar::Param, 1, 1);
  RuntimeCond.RegOff = TRI.IntParamRegs[1];
  MedVar RuntimeClobber = makeVar(MedVar::Param, 2, 4);
  RuntimeClobber.RegOff = TRI.IntParamRegs[2];
  Func.Params = {Index, RuntimeCond, RuntimeClobber};

  MedVar Zero32 = makeVar(MedVar::Temp, 10, 4);
  MedVar WideZero = makeVar(MedVar::Temp, 11, TRI.PointerSize);
  MedVar LowZero = makeVar(MedVar::Temp, 12, 4);
  MedVar FoldedCond = makeVar(MedVar::Temp, 13, 1);
  MedVar BadWide = makeVar(MedVar::Temp, 14, TRI.PointerSize);
  MedVar PhiBase = makeVar(MedVar::Temp, 15, TRI.PointerSize);
  MedVar ScaledIndex = makeVar(MedVar::Temp, 16, TRI.PointerSize);
  MedVar ElementAddr = makeVar(MedVar::Temp, 17, TRI.PointerSize);
  MedVar Element = makeVar(MedVar::Temp, 18, 2);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0x10;
  Entry.Succs = {1, 2};

  if (Case == PhiTableBaseCase::DeadScalarClobber) {
    MedOp MaterializeZero;
    MaterializeZero.Opcode = NdOp::COPY;
    MaterializeZero.Output = Zero32;
    MaterializeZero.addInput(MedVar::makeConst(0, 4));
    Entry.Ops.push_back(std::move(MaterializeZero));

    MedOp WidenZero;
    WidenZero.Opcode = NdOp::INT_ZEXT;
    WidenZero.Output = WideZero;
    WidenZero.addInput(Zero32);
    Entry.Ops.push_back(std::move(WidenZero));

    MedOp SliceZero;
    SliceZero.Opcode = NdOp::SUBBYTES;
    SliceZero.Output = LowZero;
    SliceZero.addInput(WideZero);
    SliceZero.addInput(MedVar::makeConst(0, 4));
    Entry.Ops.push_back(std::move(SliceZero));

    MedOp CompareZero;
    CompareZero.Opcode = NdOp::INT_NOTEQUAL;
    CompareZero.Output = FoldedCond;
    CompareZero.addInput(LowZero);
    CompareZero.addInput(MedVar::makeConst(0, 4));
    Entry.Ops.push_back(std::move(CompareZero));
  }

  MedOp ConditionalBranch;
  ConditionalBranch.Opcode = NdOp::COND_BR;
  ConditionalBranch.Addr = CallerVA + 0xc;
  ConditionalBranch.addInput(
      MedVar::makeConst(CallerVA + 0x20, TRI.PointerSize));
  ConditionalBranch.addInput(
      Case == PhiTableBaseCase::DeadScalarClobber ? FoldedCond : RuntimeCond);
  Entry.Ops.push_back(std::move(ConditionalBranch));

  MedBlock Merge;
  Merge.Id = 1;
  Merge.StartAddr = CallerVA + 0x10;
  Merge.EndAddr = CallerVA + 0x20;
  Merge.Preds = Case == PhiTableBaseCase::MalformedRuntimeClobber
                    ? std::vector<int>{0, 99}
                    : std::vector<int>{0, 2};

  PhiNode BasePhi;
  BasePhi.Output = PhiBase;
  BasePhi.Args.push_back(
      {0, MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize)});
  if (Case == PhiTableBaseCase::SameBaseOnReachableEdges)
    BasePhi.Args.push_back(
        {2, MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize)});
  else
    BasePhi.Args.push_back(
        {Case == PhiTableBaseCase::MalformedRuntimeClobber ? 99 : 2, BadWide});
  Merge.Phis.push_back(std::move(BasePhi));

  MedOp Scale;
  Scale.Opcode = NdOp::INT_LEFT;
  Scale.Output = ScaledIndex;
  Scale.addInput(Index);
  Scale.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Merge.Ops.push_back(std::move(Scale));

  MedOp FormElementAddr;
  FormElementAddr.Opcode = NdOp::INT_ADD;
  FormElementAddr.Output = ElementAddr;
  FormElementAddr.addInput(PhiBase);
  FormElementAddr.addInput(ScaledIndex);
  Merge.Ops.push_back(std::move(FormElementAddr));

  MedOp ReadElement;
  ReadElement.Opcode = NdOp::LOAD;
  ReadElement.Output = Element;
  ReadElement.addInput(ElementAddr);
  Merge.Ops.push_back(std::move(ReadElement));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Element);
  Merge.Ops.push_back(std::move(Return));

  MedBlock Alternate;
  Alternate.Id = 2;
  Alternate.StartAddr = CallerVA + 0x20;
  Alternate.EndAddr = CallerVA + 0x28;
  Alternate.Preds = {0};
  Alternate.Succs = {1};

  MedOp WidenClobber;
  WidenClobber.Opcode = NdOp::INT_ZEXT;
  WidenClobber.Output = BadWide;
  WidenClobber.addInput(Case == PhiTableBaseCase::DeadScalarClobber
                            ? MedVar::makeConst(0, 4)
                            : RuntimeClobber);
  Alternate.Ops.push_back(std::move(WidenClobber));

  MedOp BranchToMerge;
  BranchToMerge.Opcode = NdOp::BRANCH;
  BranchToMerge.addInput(MedVar::makeConst(Merge.StartAddr, TRI.PointerSize));
  Alternate.Ops.push_back(std::move(BranchToMerge));

  Func.Blocks.push_back(std::move(Entry));
  Func.Blocks.push_back(std::move(Merge));
  Func.Blocks.push_back(std::move(Alternate));
  return Func;
}

MedFunc makeThreeWayPhiConstTableLookup(Arch TargetArch, bool ScalarArmDead) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name =
      std::string(TargetArch == Arch::AArch64 ? "three_way_phi_table_arm64_"
                                              : "three_way_phi_table_x86_64_") +
      (ScalarArmDead ? "dead" : "reachable");
  Func.ReturnType = NdType::makeInt(2);

  MedVar Index = makeVar(MedVar::Param, 0, TRI.PointerSize);
  Index.RegOff = TRI.IntParamRegs[0];
  MedVar GateCond = makeVar(MedVar::Param, 1, 1);
  GateCond.RegOff = TRI.IntParamRegs[1];
  MedVar TableCond = makeVar(MedVar::Param, 2, 1);
  TableCond.RegOff = TRI.IntParamRegs[2];
  Func.Params = {Index, GateCond, TableCond};

  MedVar PhiBase = makeVar(MedVar::Temp, 40, TRI.PointerSize);
  MedVar ScaledIndex = makeVar(MedVar::Temp, 41, TRI.PointerSize);
  MedVar ElementAddr = makeVar(MedVar::Temp, 42, TRI.PointerSize);
  MedVar Element = makeVar(MedVar::Temp, 43, 2);
  MedVar ProvenTableOnly = makeVar(MedVar::Temp, 44, 1);

  auto appendBranch = [&](MedBlock &Block, va_t Target) {
    MedOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.addInput(MedVar::makeConst(Target, TRI.PointerSize));
    Block.Ops.push_back(std::move(Branch));
  };

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0x10;
  Entry.Succs = {1, 2};
  if (ScalarArmDead) {
    MedOp FoldGate;
    FoldGate.Opcode = NdOp::INT_EQUAL;
    FoldGate.Output = ProvenTableOnly;
    FoldGate.addInput(MedVar::makeConst(3, 1));
    FoldGate.addInput(MedVar::makeConst(3, 1));
    Entry.Ops.push_back(std::move(FoldGate));
  }
  MedOp Gate;
  Gate.Opcode = NdOp::COND_BR;
  Gate.Addr = CallerVA + 0xc;
  Gate.addInput(MedVar::makeConst(CallerVA + 0x10, TRI.PointerSize));
  Gate.addInput(ScalarArmDead ? ProvenTableOnly : GateCond);
  Entry.Ops.push_back(std::move(Gate));

  MedBlock ChooseTable;
  ChooseTable.Id = 1;
  ChooseTable.StartAddr = CallerVA + 0x10;
  ChooseTable.EndAddr = CallerVA + 0x20;
  ChooseTable.Preds = {0};
  ChooseTable.Succs = {3, 4};
  MedOp Choose;
  Choose.Opcode = NdOp::COND_BR;
  Choose.Addr = CallerVA + 0x1c;
  Choose.addInput(MedVar::makeConst(CallerVA + 0x30, TRI.PointerSize));
  Choose.addInput(TableCond);
  ChooseTable.Ops.push_back(std::move(Choose));

  MedBlock Scalar;
  Scalar.Id = 2;
  Scalar.StartAddr = CallerVA + 0x20;
  Scalar.EndAddr = CallerVA + 0x28;
  Scalar.Preds = {0};
  Scalar.Succs = {5};
  appendBranch(Scalar, CallerVA + 0x50);

  MedBlock FirstTable;
  FirstTable.Id = 3;
  FirstTable.StartAddr = CallerVA + 0x30;
  FirstTable.EndAddr = CallerVA + 0x38;
  FirstTable.Preds = {1};
  FirstTable.Succs = {5};
  appendBranch(FirstTable, CallerVA + 0x50);

  MedBlock SecondTable;
  SecondTable.Id = 4;
  SecondTable.StartAddr = CallerVA + 0x40;
  SecondTable.EndAddr = CallerVA + 0x48;
  SecondTable.Preds = {1};
  SecondTable.Succs = {5};
  appendBranch(SecondTable, CallerVA + 0x50);

  MedBlock Merge;
  Merge.Id = 5;
  Merge.StartAddr = CallerVA + 0x50;
  Merge.EndAddr = CallerVA + 0x60;
  Merge.Preds = {2, 3, 4};
  PhiNode BasePhi;
  BasePhi.Output = PhiBase;
  BasePhi.Args = {
      {3, MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize)},
      {4, MedVar::makeConst(OtherSpilledConstTableVA, TRI.PointerSize)},
      {2, MedVar::makeConst(7, TRI.PointerSize)},
  };
  Merge.Phis.push_back(std::move(BasePhi));
  MedOp Scale;
  Scale.Opcode = NdOp::INT_LEFT;
  Scale.Output = ScaledIndex;
  Scale.addInput(Index);
  Scale.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Merge.Ops.push_back(std::move(Scale));
  MedOp FormElementAddr;
  FormElementAddr.Opcode = NdOp::INT_ADD;
  FormElementAddr.Output = ElementAddr;
  FormElementAddr.addInput(PhiBase);
  FormElementAddr.addInput(ScaledIndex);
  Merge.Ops.push_back(std::move(FormElementAddr));
  MedOp ReadElement;
  ReadElement.Opcode = NdOp::LOAD;
  ReadElement.Output = Element;
  ReadElement.addInput(ElementAddr);
  Merge.Ops.push_back(std::move(ReadElement));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Element);
  Merge.Ops.push_back(std::move(Return));

  Func.Blocks = {std::move(Entry),       std::move(ChooseTable),
                 std::move(Scalar),      std::move(FirstTable),
                 std::move(SecondTable), std::move(Merge)};
  return Func;
}

MedFunc makeMixedPhiIntegerCall(Arch TargetArch) {
  MedFunc Func =
      makeThreeWayPhiConstTableLookup(TargetArch, /*ScalarArmDead=*/false);
  Func.Name = TargetArch == Arch::AArch64 ? "mixed_phi_integer_call_arm64"
                                          : "mixed_phi_integer_call_x86_64";
  Func.ReturnType = NdType::makeVoid();
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedBlock &Merge = Func.Blocks[5];
  MedVar PhiValue = Merge.Phis.front().Output;
  Merge.Ops.clear();
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Merge.StartAddr;
  Call.addInput(MedVar::makeConst(ImportStubVA, TRI.PointerSize));
  Merge.Ops.push_back(std::move(Call));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Merge.StartAddr + 4;
  Merge.Ops.push_back(std::move(Return));

  MedCallInfo Info;
  Info.BlockId = Merge.Id;
  Info.OpIdx = 0;
  Info.TargetAddr = ImportStubVA;
  Info.TargetName = "_opaque_integer_sink";
  Info.Args.push_back(PhiValue);
  Func.CallInfos.push_back(std::move(Info));
  return Func;
}

MedFunc makeSpeculativeIntegerCallThenStrictLoad(Arch TargetArch) {
  MedFunc Func =
      makeThreeWayPhiConstTableLookup(TargetArch, /*ScalarArmDead=*/false);
  Func.Name = TargetArch == Arch::AArch64
                  ? "speculative_then_strict_table_arm64"
                  : "speculative_then_strict_table_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedBlock &Merge = Func.Blocks[5];
  MedVar ElementAddress = Merge.Ops[1].Output;

  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Merge.StartAddr + 4;
  Call.addInput(MedVar::makeConst(ImportStubVA, TRI.PointerSize));
  Merge.Ops.insert(Merge.Ops.begin() + 2, std::move(Call));

  MedCallInfo Info;
  Info.BlockId = Merge.Id;
  Info.OpIdx = 2;
  Info.TargetAddr = ImportStubVA;
  Info.TargetName = "_opaque_integer_sink";
  Info.Args.push_back(ElementAddress);
  Func.CallInfos.push_back(std::move(Info));
  return Func;
}

MedFunc makeRecurrentConstTableLookup(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = TargetArch == Arch::AArch64 ? "recurrent_const_table_arm64"
                                          : "recurrent_const_table_x86_64";
  Func.ReturnType = NdType::makeVoid();
  Func.DoesNotReturn = true;

  MedVar Index = makeVar(MedVar::Param, 0, TRI.PointerSize);
  Index.RegOff = TRI.IntParamRegs.front();
  Func.Params.push_back(Index);

  MedVar PhiBase = makeVar(MedVar::Temp, 30, TRI.PointerSize);
  MedVar NextBase = makeVar(MedVar::Temp, 31, TRI.PointerSize);
  MedVar Element = makeVar(MedVar::Temp, 32, 2);
  MedVar ScaledIndex = makeVar(MedVar::Temp, 33, TRI.PointerSize);
  MedVar ElementAddr = makeVar(MedVar::Temp, 34, TRI.PointerSize);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 4;
  Entry.Succs = {1};
  MedOp EnterLoop;
  EnterLoop.Opcode = NdOp::BRANCH;
  EnterLoop.addInput(MedVar::makeConst(CallerVA + 0x10, TRI.PointerSize));
  Entry.Ops.push_back(std::move(EnterLoop));

  MedBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = CallerVA + 0x10;
  Loop.EndAddr = CallerVA + 0x18;
  Loop.Preds = {0, 2};
  Loop.Succs = {2};
  PhiNode BasePhi;
  BasePhi.Output = PhiBase;
  BasePhi.Args = {{0, MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize)},
                  {2, NextBase}};
  Loop.Phis.push_back(std::move(BasePhi));
  MedOp Scale;
  Scale.Opcode = NdOp::INT_LEFT;
  Scale.Output = ScaledIndex;
  Scale.addInput(Index);
  Scale.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Loop.Ops.push_back(std::move(Scale));
  MedOp FormElementAddr;
  FormElementAddr.Opcode = NdOp::INT_ADD;
  FormElementAddr.Output = ElementAddr;
  FormElementAddr.addInput(PhiBase);
  FormElementAddr.addInput(ScaledIndex);
  Loop.Ops.push_back(std::move(FormElementAddr));
  MedOp ReadElement;
  ReadElement.Opcode = NdOp::LOAD;
  ReadElement.Output = Element;
  ReadElement.addInput(ElementAddr);
  Loop.Ops.push_back(std::move(ReadElement));
  MedOp Advance;
  Advance.Opcode = NdOp::BRANCH;
  Advance.addInput(MedVar::makeConst(CallerVA + 0x20, TRI.PointerSize));
  Loop.Ops.push_back(std::move(Advance));

  MedBlock Latch;
  Latch.Id = 2;
  Latch.StartAddr = CallerVA + 0x20;
  Latch.EndAddr = CallerVA + 0x28;
  Latch.Preds = {1};
  Latch.Succs = {1};
  MedOp Step;
  Step.Opcode = NdOp::INT_ADD;
  Step.Output = NextBase;
  Step.addInput(PhiBase);
  Step.addInput(MedVar::makeConst(2, TRI.PointerSize));
  Latch.Ops.push_back(std::move(Step));
  MedOp BackEdge;
  BackEdge.Opcode = NdOp::BRANCH;
  BackEdge.addInput(MedVar::makeConst(Loop.StartAddr, TRI.PointerSize));
  Latch.Ops.push_back(std::move(BackEdge));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Latch)};
  return Func;
}

MedFunc makeRepeatedRecurrentConstTableLookup(Arch TargetArch,
                                              unsigned LoadCount) {
  MedFunc Func = makeRecurrentConstTableLookup(TargetArch);
  Func.Name += "_repeated_" + std::to_string(LoadCount);
  MedBlock &Loop = Func.Blocks[1];
  auto LoadIt =
      std::find_if(Loop.Ops.begin(), Loop.Ops.end(),
                   [](const MedOp &Op) { return Op.Opcode == NdOp::LOAD; });
  EXPECT_NE(LoadIt, Loop.Ops.end());
  if (LoadIt == Loop.Ops.end())
    return Func;

  const MedOp LoadTemplate = *LoadIt;
  const auto InsertAt = Loop.Ops.erase(LoadIt);
  std::vector<MedOp> Loads;
  Loads.reserve(LoadCount);
  for (unsigned I = 0; I < LoadCount; ++I) {
    MedOp Load = LoadTemplate;
    Load.Output.Id = 1000 + static_cast<int>(I);
    Loads.push_back(std::move(Load));
  }
  Loop.Ops.insert(InsertAt, std::make_move_iterator(Loads.begin()),
                  std::make_move_iterator(Loads.end()));
  return Func;
}

void rebaseFunction(MedFunc &Func, va_t Delta) {
  const va_t OldEntry = Func.Entry;
  const va_t InternalEnd = OldEntry + 0x100;
  Func.Entry += Delta;
  for (MedBlock &Block : Func.Blocks) {
    if (Block.StartAddr != 0)
      Block.StartAddr += Delta;
    if (Block.EndAddr != 0)
      Block.EndAddr += Delta;
    for (MedOp &Op : Block.Ops) {
      if (Op.Addr != 0)
        Op.Addr += Delta;
      if (Op.Opcode != NdOp::BRANCH && Op.Opcode != NdOp::COND_BR)
        continue;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal >= OldEntry &&
            Op.Inputs[I].ConstVal < InternalEnd)
          Op.Inputs[I].ConstVal += Delta;
    }
  }
}

MedFunc makeRecurrentAbsolutePointerTableLoad() {
  MedFunc Func = makeRecurrentConstTableLookup(Arch::AArch64);
  Func.Name = "recurrent_absolute_pointer_table_load_arm64";
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::AArch64);

  MedVar LoadedBase;
  LoadedBase.Kind = MedVar::Temp;
  LoadedBase.TheArch = Arch::AArch64;
  LoadedBase.Id = 40;
  LoadedBase.SSAVer = 1;
  LoadedBase.Size = TRI.PointerSize;

  MedOp LoadBase;
  LoadBase.Opcode = NdOp::LOAD;
  LoadBase.Output = LoadedBase;
  LoadBase.addInput(MedVar::makeConst(DataVA + 16, TRI.PointerSize));
  Func.Blocks.front().Ops.insert(Func.Blocks.front().Ops.begin(),
                                 std::move(LoadBase));
  Func.Blocks[1].Phis.front().Args.front().second = LoadedBase;
  return Func;
}

MedFunc makeRecurrentPointerTableSlotWalk(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = TargetArch == Arch::AArch64
                  ? "recurrent_pointer_table_slot_walk_arm64"
                  : "recurrent_pointer_table_slot_walk_x86_64";
  Func.ReturnType = NdType::makeVoid();
  Func.DoesNotReturn = true;

  MedVar InitialSlot = makeVar(49, TRI.PointerSize);
  MedVar PhiSlot = makeVar(50, TRI.PointerSize);
  MedVar NextSlot = makeVar(51, TRI.PointerSize);
  MedVar LoadedTarget = makeVar(52, TRI.PointerSize);
  MedVar LoadedValue = makeVar(53, 4);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 4;
  Entry.Succs = {1};
  MedOp MaterializeInitialSlot;
  MaterializeInitialSlot.Opcode = NdOp::COPY;
  MaterializeInitialSlot.Output = InitialSlot;
  MaterializeInitialSlot.addInput(
      MedVar::makeConst(DataVA + 16, TRI.PointerSize));
  Entry.Ops.push_back(std::move(MaterializeInitialSlot));
  MedOp EnterLoop;
  EnterLoop.Opcode = NdOp::BRANCH;
  EnterLoop.addInput(MedVar::makeConst(CallerVA + 0x10, TRI.PointerSize));
  Entry.Ops.push_back(std::move(EnterLoop));

  MedBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = CallerVA + 0x10;
  Loop.EndAddr = CallerVA + 0x1c;
  Loop.Preds = {0, 2};
  Loop.Succs = {2};
  PhiNode SlotPhi;
  SlotPhi.Output = PhiSlot;
  SlotPhi.Args = {{0, InitialSlot}, {2, NextSlot}};
  Loop.Phis.push_back(std::move(SlotPhi));
  MedOp ReadTarget;
  ReadTarget.Opcode = NdOp::LOAD;
  ReadTarget.Output = LoadedTarget;
  ReadTarget.addInput(PhiSlot);
  Loop.Ops.push_back(std::move(ReadTarget));
  MedOp ReadValue;
  ReadValue.Opcode = NdOp::LOAD;
  ReadValue.Output = LoadedValue;
  ReadValue.addInput(LoadedTarget);
  Loop.Ops.push_back(std::move(ReadValue));
  MedOp Advance;
  Advance.Opcode = NdOp::BRANCH;
  Advance.addInput(MedVar::makeConst(CallerVA + 0x20, TRI.PointerSize));
  Loop.Ops.push_back(std::move(Advance));

  MedBlock Latch;
  Latch.Id = 2;
  Latch.StartAddr = CallerVA + 0x20;
  Latch.EndAddr = CallerVA + 0x28;
  Latch.Preds = {1};
  Latch.Succs = {1};
  MedOp Step;
  Step.Opcode = NdOp::INT_ADD;
  Step.Output = NextSlot;
  Step.addInput(PhiSlot);
  Step.addInput(MedVar::makeConst(24, TRI.PointerSize));
  Latch.Ops.push_back(std::move(Step));
  MedOp BackEdge;
  BackEdge.Opcode = NdOp::BRANCH;
  BackEdge.addInput(MedVar::makeConst(Loop.StartAddr, TRI.PointerSize));
  Latch.Ops.push_back(std::move(BackEdge));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Latch)};
  return Func;
}

enum class FalseRecurrenceCase {
  ControlOnlySelect,
  AnnihilatingMultiply,
  IndependentTableAdd,
  DuplicatePointerAdd,
  TableSubtrahend,
  UnknownNestedPhiArm,
  LoadedOffset,
};

MedFunc makeFalseRecurrentTableLookup(Arch TargetArch,
                                      FalseRecurrenceCase Case) {
  MedFunc Func = makeRecurrentConstTableLookup(TargetArch);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "false_recurrent_table_arm64_"
                              : "false_recurrent_table_x86_64_") +
              std::to_string(static_cast<int>(Case));
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };
  MedVar ScalarA = makeVar(MedVar::Param, 1, TRI.PointerSize);
  ScalarA.RegOff = TRI.IntParamRegs[1];
  MedVar ScalarB = makeVar(MedVar::Param, 2, TRI.PointerSize);
  ScalarB.RegOff = TRI.IntParamRegs[2];
  Func.Params.push_back(ScalarA);
  Func.Params.push_back(ScalarB);

  MedVar PhiBase = Func.Blocks[1].Phis.front().Output;
  MedVar NextBase = Func.Blocks[1].Phis.front().Args[1].second;
  if (Case == FalseRecurrenceCase::UnknownNestedPhiArm) {
    MedVar Nested = makeVar(MedVar::Temp, 41, TRI.PointerSize);
    PhiNode NestedPhi;
    NestedPhi.Output = Nested;
    NestedPhi.Args = {{2, NextBase}, {99, ScalarA}};
    Func.Blocks[1].Phis.push_back(std::move(NestedPhi));
    Func.Blocks[1].Phis.front().Args[1].second = Nested;
    return Func;
  }
  MedBlock &Latch = Func.Blocks[2];
  MedOp BackEdge = std::move(Latch.Ops.back());
  Latch.Ops.clear();
  if (Case == FalseRecurrenceCase::LoadedOffset) {
    MedVar Loaded = makeVar(MedVar::Temp, 40, TRI.PointerSize);
    MedOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = Loaded;
    Load.addInput(ScalarA);
    Latch.Ops.push_back(std::move(Load));
    MedOp Step;
    Step.Opcode = NdOp::INT_ADD;
    Step.Output = NextBase;
    Step.addInput(PhiBase);
    Step.addInput(Loaded);
    Latch.Ops.push_back(std::move(Step));
    Latch.Ops.push_back(std::move(BackEdge));
    return Func;
  }
  if (Case == FalseRecurrenceCase::ControlOnlySelect) {
    MedVar Cond = makeVar(MedVar::Temp, 40, 1);
    MedOp Compare;
    Compare.Opcode = NdOp::INT_NOTEQUAL;
    Compare.Output = Cond;
    Compare.addInput(PhiBase);
    Compare.addInput(MedVar::makeConst(0, TRI.PointerSize));
    Latch.Ops.push_back(std::move(Compare));
    MedOp Select;
    Select.Opcode = NdOp::SELECT;
    Select.Output = NextBase;
    Select.addInput(Cond);
    Select.addInput(ScalarA);
    Select.addInput(ScalarB);
    Latch.Ops.push_back(std::move(Select));
  } else if (Case == FalseRecurrenceCase::AnnihilatingMultiply) {
    MedOp Multiply;
    Multiply.Opcode = NdOp::INT_MULT;
    Multiply.Output = NextBase;
    Multiply.addInput(PhiBase);
    Multiply.addInput(MedVar::makeConst(0, TRI.PointerSize));
    Latch.Ops.push_back(std::move(Multiply));
  } else {
    MedVar OtherBase = makeVar(MedVar::Temp, 40, TRI.PointerSize);
    if (Case != FalseRecurrenceCase::DuplicatePointerAdd) {
      MedOp Copy;
      Copy.Opcode = NdOp::COPY;
      Copy.Output = OtherBase;
      Copy.addInput(
          MedVar::makeConst(OtherSpilledConstTableVA, TRI.PointerSize));
      Latch.Ops.push_back(std::move(Copy));
    }
    MedOp Step;
    Step.Opcode = Case == FalseRecurrenceCase::TableSubtrahend ? NdOp::INT_SUB
                                                               : NdOp::INT_ADD;
    Step.Output = NextBase;
    Step.addInput(PhiBase);
    Step.addInput(Case == FalseRecurrenceCase::DuplicatePointerAdd ? PhiBase
                                                                   : OtherBase);
    Latch.Ops.push_back(std::move(Step));
  }
  Latch.Ops.push_back(std::move(BackEdge));
  return Func;
}

enum class NestedRecurrentPhiCase {
  MutualSubregisterAlias,
  UnrelatedReachableCycle,
  UnrelatedDeadCycle,
};

MedFunc makeNestedRecurrentPhiTableLookup(Arch TargetArch,
                                          NestedRecurrentPhiCase Case) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = (TargetArch == Arch::AArch64 ? "nested_recurrent_table_arm64_"
                                           : "nested_recurrent_table_x86_64_") +
              std::to_string(static_cast<int>(Case));
  Func.ReturnType = NdType::makeInt(2);

  MedVar Index = makeVar(MedVar::Param, 0, TRI.PointerSize);
  Index.RegOff = TRI.IntParamRegs[0];
  MedVar RuntimeCond = makeVar(MedVar::Param, 1, 1);
  RuntimeCond.RegOff = TRI.IntParamRegs[1];
  MedVar RuntimeSeed = makeVar(MedVar::Param, 2, TRI.PointerSize);
  RuntimeSeed.RegOff = TRI.IntParamRegs[2];
  Func.Params = {Index, RuntimeCond, RuntimeSeed};

  MedVar RootPhi = makeVar(MedVar::Reg, 30, TRI.PointerSize);
  RootPhi.RegOff = TRI.IntReturnReg;
  const bool IsAlias = Case == NestedRecurrentPhiCase::MutualSubregisterAlias;
  MedVar NestedPhi = makeVar(MedVar::Reg, 31, IsAlias ? 4 : TRI.PointerSize);
  NestedPhi.RegOff = IsAlias ? RootPhi.RegOff : TRI.IntReturnReg2;
  MedVar NestedNext = makeVar(MedVar::Temp, 32, IsAlias ? 4 : TRI.PointerSize);
  MedVar RootNext = makeVar(MedVar::Temp, 33, TRI.PointerSize);
  MedVar ScaledIndex = makeVar(MedVar::Temp, 34, TRI.PointerSize);
  MedVar ElementAddr = makeVar(MedVar::Temp, 35, TRI.PointerSize);
  MedVar Element = makeVar(MedVar::Temp, 36, 2);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 8;
  Entry.Succs = {1};
  MedOp EnterLoop;
  EnterLoop.Opcode = NdOp::BRANCH;
  EnterLoop.addInput(MedVar::makeConst(CallerVA + 0x10, TRI.PointerSize));
  Entry.Ops.push_back(std::move(EnterLoop));

  MedBlock Header;
  Header.Id = 1;
  Header.StartAddr = CallerVA + 0x10;
  Header.EndAddr = CallerVA + 0x20;
  Header.Preds = {0, 2};
  Header.Succs = {2, 3};

  PhiNode Root;
  Root.Output = RootPhi;
  Root.Args = {{0, MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize)},
               {2, RootNext}};
  Header.Phis.push_back(std::move(Root));

  PhiNode Nested;
  Nested.Output = NestedPhi;
  Nested.Args = {
      {0, IsAlias
              ? MedVar::makeConst(static_cast<uint32_t>(SpilledConstTableVA), 4)
              : RuntimeSeed},
      {2, NestedNext}};
  Header.Phis.push_back(std::move(Nested));

  MedOp Scale;
  Scale.Opcode = NdOp::INT_LEFT;
  Scale.Output = ScaledIndex;
  Scale.addInput(Index);
  Scale.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Header.Ops.push_back(std::move(Scale));
  MedOp FormElementAddr;
  FormElementAddr.Opcode = NdOp::INT_ADD;
  FormElementAddr.Output = ElementAddr;
  FormElementAddr.addInput(RootPhi);
  FormElementAddr.addInput(ScaledIndex);
  Header.Ops.push_back(std::move(FormElementAddr));
  MedOp ReadElement;
  ReadElement.Opcode = NdOp::LOAD;
  ReadElement.Output = Element;
  ReadElement.addInput(ElementAddr);
  Header.Ops.push_back(std::move(ReadElement));
  MedOp ContinueOrExit;
  ContinueOrExit.Opcode = NdOp::COND_BR;
  ContinueOrExit.Addr = CallerVA + 0x1c;
  ContinueOrExit.addInput(MedVar::makeConst(CallerVA + 0x20, TRI.PointerSize));
  ContinueOrExit.addInput(Case == NestedRecurrentPhiCase::UnrelatedDeadCycle
                              ? MedVar::makeConst(0, 1)
                              : RuntimeCond);
  Header.Ops.push_back(std::move(ContinueOrExit));

  MedBlock Latch;
  Latch.Id = 2;
  Latch.StartAddr = CallerVA + 0x20;
  Latch.EndAddr = CallerVA + 0x30;
  Latch.Preds = {1};
  Latch.Succs = {1};
  MedOp StepNested;
  StepNested.Opcode = NdOp::INT_ADD;
  StepNested.Output = NestedNext;
  StepNested.addInput(NestedPhi);
  StepNested.addInput(MedVar::makeConst(2, IsAlias ? 4 : TRI.PointerSize));
  Latch.Ops.push_back(std::move(StepNested));
  MedOp FormRootNext;
  FormRootNext.Opcode = IsAlias ? NdOp::INT_ZEXT : NdOp::COPY;
  FormRootNext.Output = RootNext;
  FormRootNext.addInput(NestedNext);
  Latch.Ops.push_back(std::move(FormRootNext));
  MedOp BackEdge;
  BackEdge.Opcode = NdOp::BRANCH;
  BackEdge.addInput(MedVar::makeConst(Header.StartAddr, TRI.PointerSize));
  Latch.Ops.push_back(std::move(BackEdge));

  MedBlock Exit;
  Exit.Id = 3;
  Exit.StartAddr = CallerVA + 0x30;
  Exit.EndAddr = CallerVA + 0x38;
  Exit.Preds = {1};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Element);
  Exit.Ops.push_back(std::move(Return));

  Func.Blocks = {std::move(Entry), std::move(Header), std::move(Latch),
                 std::move(Exit)};
  return Func;
}

MedFunc makeDirectPhiTableLookup(Arch TargetArch, bool WrapInCopy,
                                 bool TableArmDead) {
  MedFunc Func = makePhiConstTableLookup(
      TargetArch, PhiTableBaseCase::AmbiguousRuntimeClobber);
  Func.Name =
      std::string(TargetArch == Arch::AArch64 ? "direct_phi_table_arm64_"
                                              : "direct_phi_table_x86_64_") +
      (WrapInCopy ? "copy_" : "bare_") +
      (TableArmDead ? "dead_table" : "ambiguous");

  MedBlock &Entry = Func.Blocks[0];
  if (TableArmDead)
    Entry.Ops.back().Inputs[1] = MedVar::makeConst(1, 1);

  MedBlock &Merge = Func.Blocks[1];
  MedVar DirectAddress = Merge.Phis.front().Output;
  if (WrapInCopy) {
    MedVar Wrapped = DirectAddress;
    Wrapped.Kind = MedVar::Temp;
    Wrapped.Id = 60;
    Wrapped.SSAVer = 1;
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = Wrapped;
    Copy.addInput(DirectAddress);
    Merge.Ops.insert(Merge.Ops.begin(), std::move(Copy));
    DirectAddress = Wrapped;
  }
  for (MedOp &Op : Merge.Ops)
    if (Op.Opcode == NdOp::LOAD && Op.Output.Size == 2) {
      Op.Inputs[0] = DirectAddress;
      break;
    }
  return Func;
}

MedFunc makeMixedRepresentationRecurrentTableLookup(Arch TargetArch,
                                                    bool RawArmFirst) {
  MedFunc Func = makeRecurrentConstTableLookup(TargetArch);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "mixed_recurrent_table_arm64_"
                              : "mixed_recurrent_table_x86_64_") +
              (RawArmFirst ? "raw_first" : "symbol_first");

  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedBlock &Entry = Func.Blocks[0];
  MedBlock &Loop = Func.Blocks[1];
  MedVar PhiBase = Loop.Phis.front().Output;
  MedVar NextBase = Loop.Phis.front().Args.back().second;
  MedVar ComputedBase = PhiBase;
  ComputedBase.Kind = MedVar::Temp;
  ComputedBase.Id = 60;
  ComputedBase.SSAVer = 1;

  constexpr int RawInitId = 10;
  constexpr int SymbolInitId = 11;
  constexpr va_t RawInitVA = CallerVA + 0x40;
  constexpr va_t SymbolInitVA = CallerVA + 0x50;

  Entry.Succs = {RawInitId, SymbolInitId};
  Entry.Ops.clear();
  MedOp ChooseInit;
  ChooseInit.Opcode = NdOp::COND_BR;
  ChooseInit.Addr = CallerVA;
  ChooseInit.addInput(MedVar::makeConst(RawInitVA, TRI.PointerSize));
  ChooseInit.addInput(Func.Params.front());
  Entry.Ops.push_back(std::move(ChooseInit));

  MedBlock RawInit;
  RawInit.Id = RawInitId;
  RawInit.StartAddr = RawInitVA;
  RawInit.EndAddr = RawInitVA + 8;
  RawInit.Preds = {0};
  RawInit.Succs = {Loop.Id};
  MedOp RawToLoop;
  RawToLoop.Opcode = NdOp::BRANCH;
  RawToLoop.addInput(MedVar::makeConst(Loop.StartAddr, TRI.PointerSize));
  RawInit.Ops.push_back(std::move(RawToLoop));

  MedBlock SymbolInit;
  SymbolInit.Id = SymbolInitId;
  SymbolInit.StartAddr = SymbolInitVA;
  SymbolInit.EndAddr = SymbolInitVA + 8;
  SymbolInit.Preds = {0};
  SymbolInit.Succs = {Loop.Id};
  MedOp MaterializeSymbol;
  MaterializeSymbol.Opcode = NdOp::COPY;
  MaterializeSymbol.Output = ComputedBase;
  MaterializeSymbol.addInput(
      MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  SymbolInit.Ops.push_back(std::move(MaterializeSymbol));
  MedOp SymbolToLoop;
  SymbolToLoop.Opcode = NdOp::BRANCH;
  SymbolToLoop.addInput(MedVar::makeConst(Loop.StartAddr, TRI.PointerSize));
  SymbolInit.Ops.push_back(std::move(SymbolToLoop));

  Loop.Preds = {RawInitId, SymbolInitId, 2};
  const std::pair<int, MedVar> RawArg = {
      RawInitId, MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize)};
  const std::pair<int, MedVar> SymbolArg = {SymbolInitId, ComputedBase};
  Loop.Phis.front().Args =
      RawArmFirst ? std::vector<std::pair<int, MedVar>>{RawArg,
                                                        SymbolArg,
                                                        {2, NextBase}}
                  : std::vector<std::pair<int, MedVar>>{
                        SymbolArg, RawArg, {2, NextBase}};
  for (MedOp &Op : Loop.Ops)
    if (Op.Opcode == NdOp::LOAD && Op.Output.Size == 2) {
      Op.Inputs[0] = PhiBase;
      break;
    }

  Func.Blocks.push_back(std::move(RawInit));
  Func.Blocks.push_back(std::move(SymbolInit));
  return Func;
}

MedFunc makeLiteralOffsetMixedRecurrentTableLookup(Arch TargetArch) {
  MedFunc Func =
      makeMixedRepresentationRecurrentTableLookup(TargetArch,
                                                  /*RawArmFirst=*/true);
  Func.Name = TargetArch == Arch::AArch64
                  ? "literal_offset_mixed_recurrent_arm64"
                  : "literal_offset_mixed_recurrent_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedBlock &Loop = Func.Blocks[1];
  MedVar ComputedBase;
  for (const auto &[Pred, Arg] : Loop.Phis.front().Args)
    if (Pred == 11)
      ComputedBase = Arg;

  MedBlock *SymbolInit = nullptr;
  for (MedBlock &Block : Func.Blocks)
    if (Block.Id == 11)
      SymbolInit = &Block;
  EXPECT_NE(SymbolInit, nullptr);
  if (!SymbolInit)
    return Func;

  MedVar LoadedOffset = ComputedBase;
  LoadedOffset.Id = 61;
  MedVar SymbolizedSibling = ComputedBase;
  SymbolizedSibling.Id = 62;

  MedOp ToSibling;
  ToSibling.Opcode = NdOp::COPY;
  ToSibling.Output = SymbolizedSibling;
  ToSibling.addInput(
      MedVar::makeConst(OtherSpilledConstTableVA, TRI.PointerSize));
  MedOp LoadOffset;
  LoadOffset.Opcode = NdOp::LOAD;
  LoadOffset.Output = LoadedOffset;
  LoadOffset.addInput(
      MedVar::makeConst(SpilledConstTableVA + 0x30, TRI.PointerSize));
  MedOp FormComputedBase;
  FormComputedBase.Opcode = NdOp::INT_ADD;
  FormComputedBase.Output = ComputedBase;
  FormComputedBase.addInput(LoadedOffset);
  FormComputedBase.addInput(SymbolizedSibling);

  MedOp ToLoop = std::move(SymbolInit->Ops.back());
  SymbolInit->Ops.clear();
  SymbolInit->Ops.push_back(std::move(ToSibling));
  SymbolInit->Ops.push_back(std::move(LoadOffset));
  SymbolInit->Ops.push_back(std::move(FormComputedBase));
  SymbolInit->Ops.push_back(std::move(ToLoop));
  return Func;
}

void replaceMedConstant(MedFunc &Func, uint64_t OldValue, uint64_t NewValue) {
  auto replace = [&](MedVar &V) {
    if (V.isConst() && V.ConstVal == OldValue)
      V.ConstVal = NewValue;
  };
  for (MedBlock &Block : Func.Blocks) {
    for (MedOp &Op : Block.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        replace(Op.Inputs[I]);
    for (PhiNode &Phi : Block.Phis)
      for (auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        replace(Arg);
      }
  }
}

MedFunc makeDualComputedRecurrentTableLookup(Arch TargetArch,
                                             uint64_t FirstBase,
                                             uint64_t SecondBase) {
  MedFunc Func = makeMixedRepresentationRecurrentTableLookup(
      TargetArch, /*RawArmFirst=*/true);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "dual_computed_recurrent_arm64_"
                              : "dual_computed_recurrent_x86_64_") +
              std::to_string(FirstBase) + "_" + std::to_string(SecondBase);
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedVar FirstComputed;
  FirstComputed.Kind = MedVar::Temp;
  FirstComputed.TheArch = TargetArch;
  FirstComputed.Id = 70;
  FirstComputed.SSAVer = 1;
  FirstComputed.Size = TRI.PointerSize;

  constexpr int FirstInitId = 10;
  constexpr int SecondInitId = 11;
  for (MedBlock &Block : Func.Blocks) {
    if (Block.Id == FirstInitId) {
      MedOp Materialize;
      Materialize.Opcode = NdOp::COPY;
      Materialize.Output = FirstComputed;
      Materialize.addInput(MedVar::makeConst(FirstBase, TRI.PointerSize));
      Block.Ops.insert(Block.Ops.begin(), std::move(Materialize));
    } else if (Block.Id == SecondInitId) {
      if (!Block.Ops.empty() && Block.Ops.front().Opcode == NdOp::COPY)
        Block.Ops.front().Inputs[0] =
            MedVar::makeConst(SecondBase, TRI.PointerSize);
    }
  }
  for (auto &[Pred, Arg] : Func.Blocks[1].Phis.front().Args)
    if (Pred == FirstInitId)
      Arg = FirstComputed;
  return Func;
}

MedFunc makeTwoBaseArithmeticPhiTableLookup(Arch TargetArch,
                                            NdOp ArithmeticOpcode) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name =
      std::string(TargetArch == Arch::AArch64 ? "two_base_arithmetic_arm64_"
                                              : "two_base_arithmetic_x86_64_") +
      std::to_string(static_cast<int>(ArithmeticOpcode));
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);

  MedVar FirstBase;
  FirstBase.Kind = MedVar::Temp;
  FirstBase.TheArch = TargetArch;
  FirstBase.Id = 60;
  FirstBase.SSAVer = 1;
  FirstBase.Size = TRI.PointerSize;
  MedVar SecondBase = FirstBase;
  SecondBase.Id = 61;
  MedVar Combined = FirstBase;
  Combined.Id = 62;

  MedBlock &FirstTable = Func.Blocks[3];
  MedOp Branch = std::move(FirstTable.Ops.back());
  FirstTable.Ops.clear();
  MedOp MaterializeFirst;
  MaterializeFirst.Opcode = NdOp::COPY;
  MaterializeFirst.Output = FirstBase;
  MaterializeFirst.addInput(
      MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  FirstTable.Ops.push_back(std::move(MaterializeFirst));
  MedOp MaterializeSecond;
  MaterializeSecond.Opcode = NdOp::COPY;
  MaterializeSecond.Output = SecondBase;
  MaterializeSecond.addInput(
      MedVar::makeConst(OtherSpilledConstTableVA, TRI.PointerSize));
  FirstTable.Ops.push_back(std::move(MaterializeSecond));
  MedOp Combine;
  Combine.Opcode = ArithmeticOpcode;
  Combine.Output = Combined;
  Combine.addInput(FirstBase);
  Combine.addInput(SecondBase);
  FirstTable.Ops.push_back(std::move(Combine));
  FirstTable.Ops.push_back(std::move(Branch));

  MedBlock &Merge = Func.Blocks[5];
  for (auto &[Pred, Arg] : Merge.Phis.front().Args)
    if (Pred == FirstTable.Id) {
      Arg = Combined;
      break;
    }
  return Func;
}

enum class InvalidPhiPointerExprCase {
  StandaloneAnd,
  DirectSecondBase,
  NumericOffset,
  PureArithmeticSameResult,
};

MedFunc makePhiPointerExpressionLookup(Arch TargetArch,
                                       InvalidPhiPointerExprCase Case) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name =
      std::string(TargetArch == Arch::AArch64 ? "phi_pointer_expr_arm64_"
                                              : "phi_pointer_expr_x86_64_") +
      std::to_string(static_cast<int>(Case));
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = TRI.PointerSize;
    return V;
  };

  if (Case == InvalidPhiPointerExprCase::NumericOffset) {
    replaceMedConstant(Func, SpilledConstTableVA, LowSpilledConstTableVA);
    replaceMedConstant(Func, OtherSpilledConstTableVA,
                       OtherLowSpilledConstTableVA);
  }

  auto replaceArm = [&](MedBlock &Block, MedVar Arm) {
    for (auto &[Pred, Arg] : Func.Blocks[5].Phis.front().Args)
      if (Pred == Block.Id) {
        Arg = Arm;
        return;
      }
    ADD_FAILURE() << "missing PHI arm for block " << Block.Id;
  };
  auto appendBeforeBranch = [](MedBlock &Block, MedOp Op) {
    Block.Ops.insert(std::prev(Block.Ops.end()), std::move(Op));
  };

  if (Case == InvalidPhiPointerExprCase::PureArithmeticSameResult) {
    for (int BlockId : {3, 4}) {
      MedBlock &Block = Func.Blocks[BlockId];
      MedVar Left = makeTemp(60 + BlockId * 3);
      MedVar Right = makeTemp(61 + BlockId * 3);
      MedVar Sum = makeTemp(62 + BlockId * 3);
      MedOp CopyLeft;
      CopyLeft.Opcode = NdOp::COPY;
      CopyLeft.Output = Left;
      CopyLeft.addInput(MedVar::makeConst(0x400, TRI.PointerSize));
      appendBeforeBranch(Block, std::move(CopyLeft));
      MedOp CopyRight;
      CopyRight.Opcode = NdOp::COPY;
      CopyRight.Output = Right;
      CopyRight.addInput(MedVar::makeConst(0x480, TRI.PointerSize));
      appendBeforeBranch(Block, std::move(CopyRight));
      MedOp Add;
      Add.Opcode = NdOp::INT_ADD;
      Add.Output = Sum;
      Add.addInput(Left);
      Add.addInput(Right);
      appendBeforeBranch(Block, std::move(Add));
      replaceArm(Block, Sum);
    }
    return Func;
  }

  MedBlock &FirstTable = Func.Blocks[3];
  MedVar Base = makeTemp(60);
  MedVar Result = makeTemp(61);
  uint64_t BaseVA = Case == InvalidPhiPointerExprCase::NumericOffset
                        ? LowSpilledConstTableVA
                        : SpilledConstTableVA;
  MedOp CopyBase;
  CopyBase.Opcode = NdOp::COPY;
  CopyBase.Output = Base;
  CopyBase.addInput(MedVar::makeConst(BaseVA, TRI.PointerSize));
  appendBeforeBranch(FirstTable, std::move(CopyBase));
  MedOp Arithmetic;
  Arithmetic.Opcode = Case == InvalidPhiPointerExprCase::StandaloneAnd
                          ? NdOp::INT_AND
                          : NdOp::INT_ADD;
  Arithmetic.Output = Result;
  Arithmetic.addInput(Base);
  if (Case == InvalidPhiPointerExprCase::StandaloneAnd)
    Arithmetic.addInput(Func.Params[0]);
  else if (Case == InvalidPhiPointerExprCase::DirectSecondBase)
    Arithmetic.addInput(
        MedVar::makeConst(OtherSpilledConstTableVA, TRI.PointerSize));
  else
    Arithmetic.addInput(MedVar::makeConst(4, TRI.PointerSize));
  appendBeforeBranch(FirstTable, std::move(Arithmetic));
  replaceArm(FirstTable, Result);
  return Func;
}

MedFunc makeIndependentPointerPhiAddressLookup(Arch TargetArch, NdOp Opcode) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "independent_pointer_phis_arm64_"
                              : "independent_pointer_phis_x86_64_") +
              std::to_string(static_cast<int>(Opcode));
  PhiNode &First = Func.Blocks[5].Phis.front();
  for (auto &[Pred, Arg] : First.Args)
    if (Pred == 3 || Pred == 4)
      Arg = MedVar::makeConst(SpilledConstTableVA, Arg.Size);
  const MedVar FirstOutput = First.Output;

  PhiNode Second = First;
  Second.Output.Id = 45;
  for (auto &[Pred, Arg] : Second.Args)
    if (Pred == 3 || Pred == 4)
      Arg = MedVar::makeConst(OtherSpilledConstTableVA, Arg.Size);
  MedVar SecondOutput = Second.Output;
  Func.Blocks[5].Phis.push_back(std::move(Second));

  for (MedOp &Op : Func.Blocks[5].Ops)
    if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2) {
      Op.Opcode = Opcode;
      Op.Inputs[0] = FirstOutput;
      Op.Inputs[1] = SecondOutput;
      break;
    }
  return Func;
}

MedFunc makeTruncatingPointerExpressionLookup(Arch TargetArch, bool TruncatePhi,
                                              NdOp Opcode = NdOp::INT_ADD) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "truncating_pointer_expr_arm64_"
                              : "truncating_pointer_expr_x86_64_") +
              (TruncatePhi ? "phi" : std::to_string(static_cast<int>(Opcode)));
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  PhiNode &Phi = Func.Blocks[5].Phis.front();
  if (TruncatePhi) {
    for (int BlockId : {3, 4}) {
      MedBlock &Block = Func.Blocks[BlockId];
      MedVar Materialized = Phi.Output;
      Materialized.Kind = MedVar::Temp;
      Materialized.Id = 70 + BlockId;
      Materialized.Size = TRI.PointerSize;
      MedOp Copy;
      Copy.Opcode = NdOp::COPY;
      Copy.Output = Materialized;
      Copy.addInput(MedVar::makeConst(BlockId == 3 ? SpilledConstTableVA
                                                   : OtherSpilledConstTableVA,
                                      TRI.PointerSize));
      Block.Ops.insert(std::prev(Block.Ops.end()), std::move(Copy));
      for (auto &[Pred, Arg] : Phi.Args)
        if (Pred == Block.Id)
          Arg = Materialized;
    }
    Phi.Output.Size = 4;
    MedVar WidePhi = Phi.Output;
    WidePhi.Kind = MedVar::Temp;
    WidePhi.Id = 46;
    WidePhi.Size = TRI.PointerSize;
    MedOp Widen;
    Widen.Opcode = NdOp::INT_ZEXT;
    Widen.Output = WidePhi;
    Widen.addInput(Phi.Output);
    Func.Blocks[5].Ops.insert(Func.Blocks[5].Ops.begin(), std::move(Widen));
    for (MedOp &Op : Func.Blocks[5].Ops)
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2 &&
          Op.Inputs[0].Id == Phi.Output.Id) {
        Op.Inputs[0] = WidePhi;
        break;
      }
    return Func;
  }

  for (MedOp &Op : Func.Blocks[5].Ops)
    if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2) {
      Op.Opcode = Opcode;
      Op.Output.Size = 4;
      for (MedOp &Consumer : Func.Blocks[5].Ops)
        for (uint8_t I = 0; I < Consumer.NumInputs; ++I)
          if (!Consumer.Inputs[I].isConst() &&
              Consumer.Inputs[I].Id == Op.Output.Id)
            Consumer.Inputs[I].Size = 4;
      break;
    }
  return Func;
}

MedFunc makeNoPhiSelectedTableLookup(Arch TargetArch, bool Masked);

MedFunc makeNoPhiMaskedArithmeticSelection(Arch TargetArch) {
  MedFunc Func = makeNoPhiSelectedTableLookup(TargetArch, /*Masked=*/false);
  Func.Name = TargetArch == Arch::AArch64 ? "no_phi_masked_arithmetic_arm64"
                                          : "no_phi_masked_arithmetic_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = TRI.PointerSize;
    return V;
  };
  MedVar Left = makeTemp(90);
  MedVar Right = makeTemp(91);
  MedOp MaskLeft;
  MaskLeft.Opcode = NdOp::INT_AND;
  MaskLeft.Output = Left;
  MaskLeft.addInput(MedVar::makeConst(LowSpilledConstTableVA, TRI.PointerSize));
  MaskLeft.addInput(Func.Params[1]);
  MedOp MaskRight;
  MaskRight.Opcode = NdOp::INT_AND;
  MaskRight.Output = Right;
  MaskRight.addInput(
      MedVar::makeConst(OtherLowSpilledConstTableVA, TRI.PointerSize));
  MaskRight.addInput(Func.Params[1]);

  MedBlock &Entry = Func.Blocks.front();
  auto Select = std::find_if(Entry.Ops.begin(), Entry.Ops.end(), [](MedOp &Op) {
    return Op.Opcode == NdOp::SELECT;
  });
  EXPECT_NE(Select, Entry.Ops.end());
  if (Select != Entry.Ops.end()) {
    Select->Inputs[1] = Left;
    Select->Inputs[2] = Right;
    Select = Entry.Ops.insert(Select, std::move(MaskLeft));
    Entry.Ops.insert(std::next(Select), std::move(MaskRight));
  }
  return Func;
}

MedFunc makeMalformedSelfRecurrentPhiTableLookup(Arch TargetArch) {
  MedFunc Func = makePhiConstTableLookup(
      TargetArch, PhiTableBaseCase::AmbiguousRuntimeClobber);
  Func.Name = TargetArch == Arch::AArch64
                  ? "malformed_self_recurrent_table_arm64"
                  : "malformed_self_recurrent_table_x86_64";
  MedBlock &Merge = Func.Blocks[1];
  Merge.Preds.push_back(99);
  Merge.Phis.front().Args.push_back({99, Merge.Phis.front().Output});
  return Func;
}

MedFunc makeMisassignedMaskedOrPhiTableLookup(Arch TargetArch) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name = TargetArch == Arch::AArch64 ? "misassigned_masked_or_phi_arm64"
                                          : "misassigned_masked_or_phi_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = TRI.PointerSize;
    return V;
  };

  MedVar A = makeTemp(60);
  MedVar B = makeTemp(61);
  MedVar NotA = makeTemp(62);
  MedVar NotB = makeTemp(63);
  MedVar Left = makeTemp(64);
  MedVar Right = makeTemp(65);
  MedVar Blend = makeTemp(66);

  MedBlock &FirstTable = Func.Blocks[3];
  MedOp Branch = std::move(FirstTable.Ops.back());
  FirstTable.Ops.clear();
  auto appendCopy = [&](MedVar Output, uint64_t Value) {
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = Output;
    Copy.addInput(MedVar::makeConst(Value, TRI.PointerSize));
    FirstTable.Ops.push_back(std::move(Copy));
  };
  appendCopy(A, SpilledConstTableVA);
  appendCopy(B, OtherSpilledConstTableVA);
  MedOp ComplementA;
  ComplementA.Opcode = NdOp::INT_NOT;
  ComplementA.Output = NotA;
  ComplementA.addInput(A);
  FirstTable.Ops.push_back(std::move(ComplementA));
  MedOp ComplementB;
  ComplementB.Opcode = NdOp::INT_NOT;
  ComplementB.Output = NotB;
  ComplementB.addInput(B);
  FirstTable.Ops.push_back(std::move(ComplementB));
  MedOp FormLeft;
  FormLeft.Opcode = NdOp::INT_AND;
  FormLeft.Output = Left;
  FormLeft.addInput(A);
  FormLeft.addInput(NotB);
  FirstTable.Ops.push_back(std::move(FormLeft));
  MedOp FormRight;
  FormRight.Opcode = NdOp::INT_AND;
  FormRight.Output = Right;
  FormRight.addInput(B);
  FormRight.addInput(NotA);
  FirstTable.Ops.push_back(std::move(FormRight));
  MedOp FormBlend;
  FormBlend.Opcode = NdOp::INT_OR;
  FormBlend.Output = Blend;
  FormBlend.addInput(Left);
  FormBlend.addInput(Right);
  FirstTable.Ops.push_back(std::move(FormBlend));
  FirstTable.Ops.push_back(std::move(Branch));

  for (auto &[Pred, Arg] : Func.Blocks[5].Phis.front().Args)
    if (Pred == FirstTable.Id) {
      Arg = Blend;
      break;
    }
  return Func;
}

MedFunc makeFoldedScalarAddPhiTableLookup(Arch TargetArch, bool WrapInCopy) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "folded_scalar_add_phi_arm64_"
                              : "folded_scalar_add_phi_x86_64_") +
              (WrapInCopy ? "copy" : "direct");
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = TRI.PointerSize;
    return V;
  };
  MedVar One = makeTemp(60);
  MedVar Rest = makeTemp(61);
  MedVar Sum = makeTemp(62);
  MedVar PhiArg = Sum;

  MedBlock &FirstTable = Func.Blocks[3];
  MedOp Branch = std::move(FirstTable.Ops.back());
  FirstTable.Ops.clear();
  MedOp MaterializeOne;
  MaterializeOne.Opcode = NdOp::COPY;
  MaterializeOne.Output = One;
  MaterializeOne.addInput(MedVar::makeConst(1, TRI.PointerSize));
  FirstTable.Ops.push_back(std::move(MaterializeOne));
  MedOp MaterializeRest;
  MaterializeRest.Opcode = NdOp::COPY;
  MaterializeRest.Output = Rest;
  MaterializeRest.addInput(
      MedVar::makeConst(SpilledConstTableVA - 1, TRI.PointerSize));
  FirstTable.Ops.push_back(std::move(MaterializeRest));
  MedOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Output = Sum;
  Add.addInput(One);
  Add.addInput(Rest);
  FirstTable.Ops.push_back(std::move(Add));
  if (WrapInCopy) {
    PhiArg = makeTemp(63);
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = PhiArg;
    Copy.addInput(Sum);
    FirstTable.Ops.push_back(std::move(Copy));
  }
  FirstTable.Ops.push_back(std::move(Branch));

  for (auto &[Pred, Arg] : Func.Blocks[5].Phis.front().Args)
    if (Pred == FirstTable.Id) {
      Arg = PhiArg;
      break;
    }
  return Func;
}

MedFunc makeUniformFoldedScalarAddPhiTableLookup(Arch TargetArch) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name = TargetArch == Arch::AArch64
                  ? "uniform_folded_scalar_add_phi_arm64"
                  : "uniform_folded_scalar_add_phi_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = TRI.PointerSize;
    return V;
  };
  auto replaceArm = [&](MedBlock &Block, uint64_t Target, int FirstId) {
    MedOp Branch = std::move(Block.Ops.back());
    Block.Ops.clear();
    MedVar Left = makeTemp(FirstId);
    MedVar Right = makeTemp(FirstId + 1);
    MedVar Sum = makeTemp(FirstId + 2);
    constexpr uint64_t Split = 0x80000000ULL;
    MedOp CopyLeft;
    CopyLeft.Opcode = NdOp::COPY;
    CopyLeft.Output = Left;
    CopyLeft.addInput(MedVar::makeConst(Split, TRI.PointerSize));
    Block.Ops.push_back(std::move(CopyLeft));
    MedOp CopyRight;
    CopyRight.Opcode = NdOp::COPY;
    CopyRight.Output = Right;
    CopyRight.addInput(MedVar::makeConst(Target - Split, TRI.PointerSize));
    Block.Ops.push_back(std::move(CopyRight));
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output = Sum;
    Add.addInput(Left);
    Add.addInput(Right);
    Block.Ops.push_back(std::move(Add));
    Block.Ops.push_back(std::move(Branch));
    for (auto &[Pred, Arg] : Func.Blocks[5].Phis.front().Args)
      if (Pred == Block.Id)
        Arg = Sum;
  };
  replaceArm(Func.Blocks[3], SpilledConstTableVA, 60);
  replaceArm(Func.Blocks[4], OtherSpilledConstTableVA, 70);
  return Func;
}

MedFunc makeComputedModelPhiTableLookup(Arch TargetArch, bool MixedModel,
                                        bool SameBase = false) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name =
      std::string(TargetArch == Arch::AArch64 ? "computed_model_phi_arm64_"
                                              : "computed_model_phi_x86_64_") +
      (MixedModel ? "mixed"
       : SameBase ? "uniform_single"
                  : "uniform_multi");
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = TRI.PointerSize;
    return V;
  };
  auto materialize = [&](MedBlock &Block, MedVar Output, uint64_t Value) {
    MedOp Branch = std::move(Block.Ops.back());
    Block.Ops.clear();
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = Output;
    Copy.addInput(MedVar::makeConst(Value, TRI.PointerSize));
    Block.Ops.push_back(std::move(Copy));
    Block.Ops.push_back(std::move(Branch));
  };

  MedVar A = makeTemp(60);
  materialize(Func.Blocks[3], A, SpilledConstTableVA);
  for (auto &[Pred, Arg] : Func.Blocks[5].Phis.front().Args)
    if (Pred == Func.Blocks[3].Id)
      Arg = A;

  if (!MixedModel) {
    MedVar B = makeTemp(61);
    materialize(Func.Blocks[4], B,
                SameBase ? SpilledConstTableVA : OtherSpilledConstTableVA);
    for (auto &[Pred, Arg] : Func.Blocks[5].Phis.front().Args)
      if (Pred == Func.Blocks[4].Id)
        Arg = B;
  }
  return Func;
}

MedFunc makeNarrowForwardedPhiTableLookup(Arch TargetArch) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name = TargetArch == Arch::AArch64 ? "narrow_forwarded_phi_table_arm64"
                                          : "narrow_forwarded_phi_table_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };
  auto materialize = [&](MedBlock &Block, int Id, uint64_t Value) {
    MedOp Branch = std::move(Block.Ops.back());
    Block.Ops.clear();
    MedVar Narrow = makeTemp(Id, 4);
    MedVar Wide = makeTemp(Id + 1, TRI.PointerSize);
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = Narrow;
    Copy.addInput(MedVar::makeConst(Value, 4));
    Block.Ops.push_back(std::move(Copy));
    MedOp Widen;
    Widen.Opcode = NdOp::INT_ZEXT;
    Widen.Output = Wide;
    Widen.addInput(Narrow);
    Block.Ops.push_back(std::move(Widen));
    Block.Ops.push_back(std::move(Branch));
    return Wide;
  };

  MedVar A = materialize(Func.Blocks[3], 60, SymbolizedSameRunTableVA);
  MedVar B = materialize(Func.Blocks[4], 62, OtherSymbolizedSameRunTableVA);
  for (auto &[Pred, Arg] : Func.Blocks[5].Phis.front().Args) {
    if (Pred == Func.Blocks[3].Id)
      Arg = A;
    else if (Pred == Func.Blocks[4].Id)
      Arg = B;
  }
  return Func;
}

MedFunc makeNestedAmbiguousPhiSelectTableLookup(Arch TargetArch) {
  MedFunc Func = makePhiConstTableLookup(
      TargetArch, PhiTableBaseCase::AmbiguousRuntimeClobber);
  Func.Name = TargetArch == Arch::AArch64
                  ? "nested_ambiguous_phi_select_arm64"
                  : "nested_ambiguous_phi_select_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedBlock &Merge = Func.Blocks[1];
  MedVar Selected = Merge.Phis.front().Output;
  Selected.Kind = MedVar::Temp;
  Selected.Id = 60;
  Selected.SSAVer = 1;
  MedOp Select;
  Select.Opcode = NdOp::SELECT;
  Select.Output = Selected;
  Select.addInput(Func.Params[1]);
  Select.addInput(Merge.Phis.front().Output);
  Select.addInput(MedVar::makeConst(OtherSpilledConstTableVA, TRI.PointerSize));
  Merge.Ops.insert(Merge.Ops.begin(), std::move(Select));
  for (MedOp &Op : Merge.Ops)
    if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2) {
      Op.Inputs[0] = Selected;
      break;
    }
  return Func;
}

MedFunc makeTwoRecurrentPointerTableLookup(Arch TargetArch,
                                           NdOp AddressOpcode) {
  MedFunc Func = makeRecurrentConstTableLookup(TargetArch);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "two_recurrent_pointer_table_arm64_"
                              : "two_recurrent_pointer_table_x86_64_") +
              std::to_string(static_cast<int>(AddressOpcode));
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = TRI.PointerSize;
    return V;
  };
  MedVar OtherPhi = makeTemp(40);
  MedVar OtherNext = makeTemp(41);
  PhiNode Other;
  Other.Output = OtherPhi;
  Other.Args = {
      {0, MedVar::makeConst(OtherSpilledConstTableVA, TRI.PointerSize)},
      {2, OtherNext}};
  Func.Blocks[1].Phis.push_back(std::move(Other));

  for (MedOp &Op : Func.Blocks[1].Ops)
    if (Op.Output.Id == 34) {
      Op.Opcode = AddressOpcode;
      Op.Inputs[1] = OtherPhi;
      break;
    }

  MedOp StepOther;
  StepOther.Opcode = NdOp::INT_ADD;
  StepOther.Output = OtherNext;
  StepOther.addInput(OtherPhi);
  StepOther.addInput(MedVar::makeConst(2, TRI.PointerSize));
  Func.Blocks[2].Ops.insert(Func.Blocks[2].Ops.begin(), std::move(StepOther));
  return Func;
}

MedFunc makeAmbiguousAliasSiblingTableLookup(Arch TargetArch,
                                             bool DifferentConst) {
  MedFunc Func = makeNestedRecurrentPhiTableLookup(
      TargetArch, NestedRecurrentPhiCase::MutualSubregisterAlias);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "ambiguous_alias_sibling_arm64_"
                              : "ambiguous_alias_sibling_x86_64_") +
              (DifferentConst ? "different" : "scalar");
  PhiNode &Sibling = Func.Blocks[1].Phis[1];
  if (!DifferentConst) {
    Sibling.Args.front().second = Func.Params[2];
    return Func;
  }

  MedVar Computed = Sibling.Output;
  Computed.Kind = MedVar::Temp;
  Computed.Id = 60;
  Computed.SSAVer = 1;
  MedOp Materialize;
  Materialize.Opcode = NdOp::COPY;
  Materialize.Output = Computed;
  Materialize.addInput(
      MedVar::makeConst(static_cast<uint32_t>(OtherSpilledConstTableVA), 4));
  Func.Blocks[0].Ops.insert(Func.Blocks[0].Ops.begin(), std::move(Materialize));
  Sibling.Args.front().second = Computed;
  return Func;
}

MedFunc makeMultiRawInitRecurrentTableLookup(Arch TargetArch, bool CrossRun,
                                             bool AlternateFirst) {
  MedFunc Func =
      makeMixedRepresentationRecurrentTableLookup(TargetArch, AlternateFirst);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "multi_raw_init_recurrent_arm64_"
                              : "multi_raw_init_recurrent_x86_64_") +
              (CrossRun ? "cross_" : "same_") +
              (AlternateFirst ? "alternate_first" : "primary_first");
  PhiNode &Phi = Func.Blocks[1].Phis.front();
  uint64_t OtherBase =
      CrossRun ? FarSpilledConstTableVA : OtherSpilledConstTableVA;
  for (auto &[Pred, Arg] : Phi.Args) {
    if (Pred == 10)
      Arg = MedVar::makeConst(AlternateFirst ? OtherBase : SpilledConstTableVA,
                              Arg.Size);
    else if (Pred == 11)
      Arg = MedVar::makeConst(AlternateFirst ? SpilledConstTableVA : OtherBase,
                              Arg.Size);
  }
  return Func;
}

enum class MixedWidthControlCase { Shift, Select, IntNot, IntNeg2 };

MedFunc makeMixedWidthControlPhiTableLookup(Arch TargetArch,
                                            MixedWidthControlCase Case) {
  MedFunc Func = makePhiConstTableLookup(
      TargetArch, PhiTableBaseCase::AmbiguousRuntimeClobber);
  const char *CaseName = Case == MixedWidthControlCase::Shift    ? "shift"
                         : Case == MixedWidthControlCase::Select ? "select"
                         : Case == MixedWidthControlCase::IntNot ? "not"
                                                                 : "neg2";
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "mixed_width_control_phi_table_arm64_"
                              : "mixed_width_control_phi_table_x86_64_") +
              CaseName;

  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };

  MedBlock &Entry = Func.Blocks[0];
  MedBlock &Merge = Func.Blocks[1];
  Entry.Ops.clear();

  MedVar FoldedCond = makeTemp(72, 1);
  if (Case == MixedWidthControlCase::Shift) {
    MedVar SmallOne = makeTemp(70, 1);
    MedVar Shifted = makeTemp(71, TRI.PointerSize);

    MedOp MaterializeOne;
    MaterializeOne.Opcode = NdOp::COPY;
    MaterializeOne.Output = SmallOne;
    MaterializeOne.addInput(MedVar::makeConst(1, 1));
    Entry.Ops.push_back(std::move(MaterializeOne));

    // emitOp first zero-extends the i8 value to the i64 shift-count width, so
    // this is 1 << 8 == 256, not an overshift of an eight-bit value.
    MedOp Shift;
    Shift.Opcode = NdOp::INT_LEFT;
    Shift.Output = Shifted;
    Shift.addInput(SmallOne);
    Shift.addInput(MedVar::makeConst(8, TRI.PointerSize));
    Entry.Ops.push_back(std::move(Shift));

    MedOp Compare;
    Compare.Opcode = NdOp::INT_NOTEQUAL;
    Compare.Output = FoldedCond;
    Compare.addInput(Shifted);
    Compare.addInput(MedVar::makeConst(0, TRI.PointerSize));
    Entry.Ops.push_back(std::move(Compare));
  } else if (Case == MixedWidthControlCase::Select) {
    MedVar WideScalar = makeTemp(70, TRI.PointerSize);
    MedOp WidenScalar;
    WidenScalar.Opcode = NdOp::INT_ZEXT;
    WidenScalar.Output = WideScalar;
    WidenScalar.addInput(Func.Params[2]);
    Entry.Ops.push_back(std::move(WidenScalar));

    // SELECT coerces its value arms to pointer width, then setVar truncates the
    // selected 0x100 back to FoldedCond's i8 output.  The branch condition is
    // therefore zero even though the selected wide arm is non-zero.
    MedOp Select;
    Select.Opcode = NdOp::SELECT;
    Select.Output = FoldedCond;
    Select.addInput(MedVar::makeConst(1, 1));
    Select.addInput(MedVar::makeConst(0x100, TRI.PointerSize));
    Select.addInput(MedVar::makeConst(0, 1));
    Entry.Ops.push_back(std::move(Select));

    Merge.Phis.front().Args = {
        {Entry.Id, WideScalar},
        {Func.Blocks[2].Id,
         MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize)}};
  } else {
    MedVar NarrowInput = makeTemp(70, 1);
    MedVar WideUnary = makeTemp(71, TRI.PointerSize);

    MedOp MaterializeInput;
    MaterializeInput.Opcode = NdOp::COPY;
    MaterializeInput.Output = NarrowInput;
    MaterializeInput.addInput(
        MedVar::makeConst(Case == MixedWidthControlCase::IntNot ? 0 : 1, 1));
    Entry.Ops.push_back(std::move(MaterializeInput));

    // Unary integer operations execute at the input width.  Both ~i8(0) and
    // -i8(1) are 255, which setVar then zero-extends to this i64 output.
    MedOp Unary;
    Unary.Opcode =
        Case == MixedWidthControlCase::IntNot ? NdOp::INT_NOT : NdOp::INT_NEG2;
    Unary.Output = WideUnary;
    Unary.addInput(NarrowInput);
    Entry.Ops.push_back(std::move(Unary));

    MedOp Compare;
    Compare.Opcode = NdOp::INT_EQUAL;
    Compare.Output = FoldedCond;
    Compare.addInput(WideUnary);
    Compare.addInput(MedVar::makeConst(255, TRI.PointerSize));
    Entry.Ops.push_back(std::move(Compare));
  }

  MedOp ConditionalBranch;
  ConditionalBranch.Opcode = NdOp::COND_BR;
  ConditionalBranch.Addr = CallerVA + 0xc;
  ConditionalBranch.addInput(
      MedVar::makeConst(Func.Blocks[2].StartAddr, TRI.PointerSize));
  ConditionalBranch.addInput(FoldedCond);
  Entry.Ops.push_back(std::move(ConditionalBranch));
  return Func;
}

MedFunc makeNoPhiSelectedTableLookup(Arch TargetArch, bool Masked) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "no_phi_selected_table_arm64_"
                              : "no_phi_selected_table_x86_64_") +
              (Masked ? "masked" : "select");
  Func.ReturnType = NdType::makeInt(2);
  MedVar CondSeed = makeVar(MedVar::Param, 0, 4);
  CondSeed.RegOff = TRI.IntParamRegs[0];
  MedVar Scalar = makeVar(MedVar::Param, 1, TRI.PointerSize);
  Scalar.RegOff = TRI.IntParamRegs[1];
  Func.Params = {CondSeed, Scalar};

  MedVar Cond = makeVar(MedVar::Temp, 80, 1);
  MedVar Selected = makeVar(MedVar::Temp, 81, TRI.PointerSize);
  MedVar Element = makeVar(MedVar::Temp, 82, 2);
  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0x20;

  MedOp Compare;
  Compare.Opcode = NdOp::INT_NOTEQUAL;
  Compare.Output = Cond;
  Compare.addInput(CondSeed);
  Compare.addInput(MedVar::makeConst(0, 4));
  Entry.Ops.push_back(std::move(Compare));

  if (!Masked) {
    MedOp Select;
    Select.Opcode = NdOp::SELECT;
    Select.Output = Selected;
    Select.addInput(Cond);
    Select.addInput(MedVar::makeConst(LowSpilledConstTableVA, TRI.PointerSize));
    Select.addInput(Scalar);
    Entry.Ops.push_back(std::move(Select));
  } else {
    MedVar WideCond = makeVar(MedVar::Temp, 83, TRI.PointerSize);
    MedVar Mask = makeVar(MedVar::Temp, 84, TRI.PointerSize);
    MedVar NotMask = makeVar(MedVar::Temp, 85, TRI.PointerSize);
    MedVar TableArm = makeVar(MedVar::Temp, 86, TRI.PointerSize);
    MedVar ScalarArm = makeVar(MedVar::Temp, 87, TRI.PointerSize);
    MedOp Widen;
    Widen.Opcode = NdOp::INT_ZEXT;
    Widen.Output = WideCond;
    Widen.addInput(Cond);
    Entry.Ops.push_back(std::move(Widen));
    MedOp Negate;
    Negate.Opcode = NdOp::INT_NEG2;
    Negate.Output = Mask;
    Negate.addInput(WideCond);
    Entry.Ops.push_back(std::move(Negate));
    MedOp Complement;
    Complement.Opcode = NdOp::INT_NOT;
    Complement.Output = NotMask;
    Complement.addInput(Mask);
    Entry.Ops.push_back(std::move(Complement));
    MedOp KeepTable;
    KeepTable.Opcode = NdOp::INT_AND;
    KeepTable.Output = TableArm;
    KeepTable.addInput(
        MedVar::makeConst(LowSpilledConstTableVA, TRI.PointerSize));
    KeepTable.addInput(Mask);
    Entry.Ops.push_back(std::move(KeepTable));
    MedOp KeepScalar;
    KeepScalar.Opcode = NdOp::INT_AND;
    KeepScalar.Output = ScalarArm;
    KeepScalar.addInput(Scalar);
    KeepScalar.addInput(NotMask);
    Entry.Ops.push_back(std::move(KeepScalar));
    MedOp Blend;
    Blend.Opcode = NdOp::INT_OR;
    Blend.Output = Selected;
    Blend.addInput(TableArm);
    Blend.addInput(ScalarArm);
    Entry.Ops.push_back(std::move(Blend));
  }

  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = Element;
  Load.addInput(Selected);
  Entry.Ops.push_back(std::move(Load));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Element);
  Entry.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Entry));
  return Func;
}

MedFunc makePointerTableNonValueRoleLookup(Arch TargetArch,
                                           bool TableOnlyInSelectCondition) {
  MedFunc Func = makeNoPhiSelectedTableLookup(TargetArch, /*Masked=*/false);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "pointer_table_non_value_role_arm64_"
                              : "pointer_table_non_value_role_x86_64_") +
              (TableOnlyInSelectCondition ? "control" : "annihilated");
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedBlock &Entry = Func.Blocks.front();
  auto Select = std::find_if(Entry.Ops.begin(), Entry.Ops.end(), [](MedOp &Op) {
    return Op.Opcode == NdOp::SELECT;
  });
  EXPECT_NE(Select, Entry.Ops.end());
  if (Select == Entry.Ops.end())
    return Func;

  if (TableOnlyInSelectCondition) {
    // ptrTableUniqueSegment's broad discovery walk sees this mapped constant,
    // but it is control-only: neither selected value carries table provenance.
    Select->Inputs[0] =
        MedVar::makeConst(LowSpilledConstTableVA, TRI.PointerSize);
    Select->Inputs[1] = Func.Params[1];
    Select->Inputs[2] = Func.Params[1];
  } else {
    // Multiplication is not pointer-preserving.  In particular, multiplying a
    // table address by zero must not let Path 2 manufacture a pointer into the
    // rebuilt table mirror merely because its discovery walk saw one operand.
    MedOp Annihilate;
    Annihilate.Opcode = NdOp::INT_MULT;
    Annihilate.Output = Select->Output;
    Annihilate.addInput(
        MedVar::makeConst(LowSpilledConstTableVA, TRI.PointerSize));
    Annihilate.addInput(MedVar::makeConst(0, TRI.PointerSize));
    *Select = std::move(Annihilate);
  }
  return Func;
}

MedFunc makeFoldedHighPointerTableLookup(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = TargetArch == Arch::AArch64 ? "folded_high_pointer_table_arm64"
                                          : "folded_high_pointer_table_x86_64";
  Func.ReturnType = NdType::makeInt(2);

  MedVar Left = makeTemp(90, TRI.PointerSize);
  MedVar Right = makeTemp(91, TRI.PointerSize);
  MedVar Address = makeTemp(92, TRI.PointerSize);
  MedVar Element = makeTemp(93, 2);
  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0x14;

  // Each leaf is an unmapped ordinary integer.  Their wrapping ADD result is
  // the 0x1200 pointer-table address; provenance must classify the values
  // getVar actually emits, not apply the address threshold to the separately
  // folded result.
  MedOp MaterializeLeft;
  MaterializeLeft.Opcode = NdOp::COPY;
  MaterializeLeft.Output = Left;
  MaterializeLeft.addInput(
      MedVar::makeConst(uint64_t(-0x1000), TRI.PointerSize));
  Entry.Ops.push_back(std::move(MaterializeLeft));
  MedOp MaterializeRight;
  MaterializeRight.Opcode = NdOp::COPY;
  MaterializeRight.Output = Right;
  MaterializeRight.addInput(MedVar::makeConst(0x2200, TRI.PointerSize));
  Entry.Ops.push_back(std::move(MaterializeRight));
  MedOp FormAddress;
  FormAddress.Opcode = NdOp::INT_ADD;
  FormAddress.Output = Address;
  FormAddress.addInput(Left);
  FormAddress.addInput(Right);
  Entry.Ops.push_back(std::move(FormAddress));
  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = Element;
  Load.addInput(Address);
  Entry.Ops.push_back(std::move(Load));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Element);
  Entry.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Entry));
  return Func;
}

MedFunc makePointerEndComparison(llvm::StringRef Name, Arch TargetArch,
                                 uint64_t Base, uint64_t End) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = Name.str();
  Func.ReturnType = NdType::makeInt(1);
  MedVar BaseValue = makeTemp(94, TRI.PointerSize);
  MedVar Byte = makeTemp(95, 1);
  MedVar BeforeEnd = makeTemp(96, 1);
  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0x10;
  MedOp MaterializeBase;
  MaterializeBase.Opcode = NdOp::COPY;
  MaterializeBase.Output = BaseValue;
  MaterializeBase.addInput(MedVar::makeConst(Base, TRI.PointerSize));
  Entry.Ops.push_back(std::move(MaterializeBase));
  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = Byte;
  Load.addInput(BaseValue);
  Entry.Ops.push_back(std::move(Load));
  MedOp Compare;
  Compare.Opcode = NdOp::INT_LESS;
  Compare.Output = BeforeEnd;
  Compare.addInput(BaseValue);
  Compare.addInput(MedVar::makeConst(End, TRI.PointerSize));
  Entry.Ops.push_back(std::move(Compare));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(BeforeEnd);
  Entry.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Entry));
  return Func;
}

MedFunc makeDeepForwardedLowTableLookup(Arch TargetArch) {
  MedFunc Func = makeNoPhiSelectedTableLookup(TargetArch, /*Masked=*/false);
  Func.Name = TargetArch == Arch::AArch64 ? "deep_forwarded_low_table_arm64"
                                          : "deep_forwarded_low_table_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedBlock &Entry = Func.Blocks.front();
  auto Select = std::find_if(Entry.Ops.begin(), Entry.Ops.end(), [](MedOp &Op) {
    return Op.Opcode == NdOp::SELECT;
  });
  auto Load = std::find_if(Entry.Ops.begin(), Entry.Ops.end(),
                           [](MedOp &Op) { return Op.Opcode == NdOp::LOAD; });
  EXPECT_NE(Select, Entry.Ops.end());
  EXPECT_NE(Load, Entry.Ops.end());
  if (Select == Entry.Ops.end() || Load == Entry.Ops.end())
    return Func;

  MedVar Current = Select->Output;
  MedOp Materialize;
  Materialize.Opcode = NdOp::COPY;
  Materialize.Output = Current;
  Materialize.addInput(
      MedVar::makeConst(LowSpilledConstTableVA, TRI.PointerSize));
  *Select = std::move(Materialize);

  std::vector<MedOp> Copies;
  for (int I = 0; I < 20; ++I) {
    MedVar Next = Current;
    Next.Id = 100 + I;
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = Next;
    Copy.addInput(Current);
    Copies.push_back(std::move(Copy));
    Current = Next;
  }
  Load = Entry.Ops.insert(Load, std::make_move_iterator(Copies.begin()),
                          std::make_move_iterator(Copies.end()));
  std::advance(Load, static_cast<long>(Copies.size()));
  Load->Inputs[0] = Current;
  return Func;
}

MedFunc makeDeepArithmeticLowTableLookup(Arch TargetArch) {
  MedFunc Func = makeDeepForwardedLowTableLookup(TargetArch);
  Func.Name = TargetArch == Arch::AArch64 ? "deep_arithmetic_low_table_arm64"
                                          : "deep_arithmetic_low_table_x86_64";
  MedBlock &Entry = Func.Blocks.front();
  for (MedOp &Op : Entry.Ops) {
    if (Op.Opcode != NdOp::COPY || Op.Output.Id < 100)
      continue;
    Op.Opcode = NdOp::INT_ADD;
    Op.addInput(MedVar::makeConst(0, Op.Output.Size));
  }
  return Func;
}

MedFunc makeDeepDynamicArithmeticLowTableLookup(Arch TargetArch) {
  MedFunc Func = makeDeepArithmeticLowTableLookup(TargetArch);
  Func.Name = TargetArch == Arch::AArch64
                  ? "deep_dynamic_arithmetic_low_table_arm64"
                  : "deep_dynamic_arithmetic_low_table_x86_64";
  for (MedOp &Op : Func.Blocks.front().Ops) {
    if (Op.Opcode != NdOp::INT_ADD || Op.Output.Id != 100)
      continue;
    EXPECT_GE(Func.Params.size(), 2u);
    if (Func.Params.size() >= 2)
      Op.Inputs[1] = Func.Params[1];
    break;
  }
  return Func;
}

MedFunc makeNoPhiInvalidFoldedSelection(Arch TargetArch,
                                        bool WithRuntimeIndex = false) {
  MedFunc Func = makeNoPhiSelectedTableLookup(TargetArch, /*Masked=*/false);
  Func.Name = TargetArch == Arch::AArch64
                  ? "no_phi_invalid_folded_selection_arm64"
                  : "no_phi_invalid_folded_selection_x86_64";
  if (WithRuntimeIndex)
    Func.Name += "_indexed";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = TRI.PointerSize;
    return V;
  };
  MedVar Shared = makeTemp(90);
  MedVar LeftOffset = makeTemp(91);
  MedVar RightOffset = makeTemp(92);
  MedVar Left = makeTemp(93);
  MedVar Right = makeTemp(94);

  std::vector<MedOp> Prefix;
  auto appendCopy = [&](MedVar Output, uint64_t Value) {
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = Output;
    Copy.addInput(MedVar::makeConst(Value, TRI.PointerSize));
    Prefix.push_back(std::move(Copy));
  };
  appendCopy(Shared, 0x400);
  appendCopy(LeftOffset, 0x480);
  appendCopy(RightOffset, 0x4a0);
  MedOp FormLeft;
  FormLeft.Opcode = NdOp::INT_ADD;
  FormLeft.Output = Left;
  FormLeft.addInput(Shared);
  FormLeft.addInput(LeftOffset);
  Prefix.push_back(std::move(FormLeft));
  MedOp FormRight;
  FormRight.Opcode = NdOp::INT_ADD;
  FormRight.Output = Right;
  FormRight.addInput(Shared);
  FormRight.addInput(RightOffset);
  Prefix.push_back(std::move(FormRight));

  MedBlock &Entry = Func.Blocks.front();
  auto Select = std::find_if(Entry.Ops.begin(), Entry.Ops.end(), [](MedOp &Op) {
    return Op.Opcode == NdOp::SELECT;
  });
  EXPECT_NE(Select, Entry.Ops.end());
  if (Select != Entry.Ops.end()) {
    Select->Inputs[1] = Left;
    Select->Inputs[2] = Right;
    Entry.Ops.insert(Select, std::make_move_iterator(Prefix.begin()),
                     std::make_move_iterator(Prefix.end()));
  }
  if (WithRuntimeIndex) {
    auto Load = std::find_if(Entry.Ops.begin(), Entry.Ops.end(),
                             [](MedOp &Op) { return Op.Opcode == NdOp::LOAD; });
    EXPECT_NE(Load, Entry.Ops.end());
    if (Load == Entry.Ops.end())
      return Func;
    MedVar Address = makeTemp(95);
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output = Address;
    Add.addInput(Load->Inputs[0]);
    Add.addInput(Func.Params[1]);
    Load->Inputs[0] = Address;
    Entry.Ops.insert(Load, std::move(Add));
  }
  return Func;
}

MedFunc makeTruncatingCopySelectTableLookup(Arch TargetArch) {
  MedFunc Func = makeNoPhiSelectedTableLookup(TargetArch, /*Masked=*/false);
  Func.Name = TargetArch == Arch::AArch64
                  ? "truncating_copy_select_table_arm64"
                  : "truncating_copy_select_table_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };
  MedVar Narrow = makeTemp(90, 4);
  MedVar Widened = makeTemp(91, TRI.PointerSize);
  MedOp Truncate;
  Truncate.Opcode = NdOp::COPY;
  Truncate.Output = Narrow;
  Truncate.addInput(MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  MedOp Widen;
  Widen.Opcode = NdOp::INT_ZEXT;
  Widen.Output = Widened;
  Widen.addInput(Narrow);

  MedBlock &Entry = Func.Blocks.front();
  auto Select = std::find_if(Entry.Ops.begin(), Entry.Ops.end(), [](MedOp &Op) {
    return Op.Opcode == NdOp::SELECT;
  });
  EXPECT_NE(Select, Entry.Ops.end());
  if (Select != Entry.Ops.end()) {
    Select->Inputs[1] = Widened;
    Select->Inputs[2] =
        MedVar::makeConst(OtherLowSpilledConstTableVA, TRI.PointerSize);
    Select = Entry.Ops.insert(Select, std::move(Truncate));
    Entry.Ops.insert(std::next(Select), std::move(Widen));
  }
  return Func;
}

MedFunc makeLoaderBasePlusTruncatingSelectOffsetLookup(Arch TargetArch) {
  MedFunc Func = makeNoPhiSelectedTableLookup(TargetArch, /*Masked=*/false);
  Func.Name = TargetArch == Arch::AArch64
                  ? "loader_base_truncating_select_offset_arm64"
                  : "loader_base_truncating_select_offset_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };

  MedBlock &Entry = Func.Blocks.front();
  auto Select = std::find_if(Entry.Ops.begin(), Entry.Ops.end(), [](MedOp &Op) {
    return Op.Opcode == NdOp::SELECT;
  });
  auto Load = std::find_if(Entry.Ops.begin(), Entry.Ops.end(),
                           [](MedOp &Op) { return Op.Opcode == NdOp::LOAD; });
  EXPECT_NE(Select, Entry.Ops.end());
  EXPECT_NE(Load, Entry.Ops.end());
  if (Select == Entry.Ops.end() || Load == Entry.Ops.end())
    return Func;

  MedVar Narrow = Select->Output;
  Narrow.Size = 4;
  Select->Output = Narrow;
  Select->Inputs[1] =
      MedVar::makeConst(OtherSpilledConstTableVA, TRI.PointerSize);

  MedVar Widened = makeTemp(90, TRI.PointerSize);
  MedVar Address = makeTemp(91, TRI.PointerSize);
  MedOp Widen;
  Widen.Opcode = NdOp::INT_ZEXT;
  Widen.Output = Widened;
  Widen.addInput(Narrow);
  MedOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Output = Address;
  Add.addInput(MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  Add.addInput(Widened);
  Load->Inputs[0] = Address;
  Load = Entry.Ops.insert(Load, std::move(Widen));
  Entry.Ops.insert(std::next(Load), std::move(Add));
  return Func;
}

MedFunc makeInvalidFoldedRecurrentTableLookup(Arch TargetArch) {
  MedFunc Func = makeRecurrentConstTableLookup(TargetArch);
  Func.Name = TargetArch == Arch::AArch64
                  ? "invalid_folded_recurrent_table_arm64"
                  : "invalid_folded_recurrent_table_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = TRI.PointerSize;
    return V;
  };
  MedVar First = makeTemp(80);
  MedVar Second = makeTemp(81);
  MedVar Folded = makeTemp(82);
  MedBlock &Entry = Func.Blocks[0];
  MedOp Branch = std::move(Entry.Ops.back());
  Entry.Ops.clear();
  MedOp CopyFirst;
  CopyFirst.Opcode = NdOp::COPY;
  CopyFirst.Output = First;
  CopyFirst.addInput(MedVar::makeConst(0x400, TRI.PointerSize));
  Entry.Ops.push_back(std::move(CopyFirst));
  MedOp CopySecond;
  CopySecond.Opcode = NdOp::COPY;
  CopySecond.Output = Second;
  CopySecond.addInput(MedVar::makeConst(0x480, TRI.PointerSize));
  Entry.Ops.push_back(std::move(CopySecond));
  MedOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Output = Folded;
  Add.addInput(First);
  Add.addInput(Second);
  Entry.Ops.push_back(std::move(Add));
  Entry.Ops.push_back(std::move(Branch));
  Func.Blocks[1].Phis.front().Args.front().second = Folded;
  return Func;
}

MedFunc makeRelocatableControlPhiTableLookup(Arch TargetArch) {
  MedFunc Func = makePhiConstTableLookup(
      TargetArch, PhiTableBaseCase::AmbiguousRuntimeClobber);
  Func.Name = TargetArch == Arch::AArch64 ? "reloc_control_phi_table_arm64"
                                          : "reloc_control_phi_table_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };
  MedVar Symbolized = makeTemp(80, TRI.PointerSize);
  MedVar RawNarrow = makeTemp(81, TRI.PointerSize);
  MedVar Cond = makeTemp(82, 1);
  MedBlock &Entry = Func.Blocks[0];
  MedOp Branch = std::move(Entry.Ops.back());
  Entry.Ops.clear();
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = Symbolized;
  Copy.addInput(MedVar::makeConst(SymbolizedSameRunTableVA, TRI.PointerSize));
  Entry.Ops.push_back(std::move(Copy));
  MedOp Widen;
  Widen.Opcode = NdOp::INT_ZEXT;
  Widen.Output = RawNarrow;
  Widen.addInput(MedVar::makeConst(SymbolizedSameRunTableVA, 4));
  Entry.Ops.push_back(std::move(Widen));
  MedOp Compare;
  Compare.Opcode = NdOp::INT_NOTEQUAL;
  Compare.Output = Cond;
  Compare.addInput(Symbolized);
  Compare.addInput(RawNarrow);
  Entry.Ops.push_back(std::move(Compare));
  Branch.Inputs[1] = Cond;
  Entry.Ops.push_back(std::move(Branch));
  return Func;
}

MedFunc makeRelocatableSubbytesControlPhiTableLookup(Arch TargetArch) {
  MedFunc Func = makePhiConstTableLookup(
      TargetArch, PhiTableBaseCase::AmbiguousRuntimeClobber);
  Func.Name = TargetArch == Arch::AArch64
                  ? "reloc_subbytes_control_phi_table_arm64"
                  : "reloc_subbytes_control_phi_table_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };
  MedVar Sliced = makeTemp(83, 4);
  MedVar Cond = makeTemp(84, 1);
  MedBlock &Entry = Func.Blocks[0];
  MedOp Branch = std::move(Entry.Ops.back());
  Entry.Ops.clear();

  // getVar turns this pointer-width table VA into ptrtoint(@global), so the
  // emitter does not see a ConstantInt byte offset and deliberately uses zero.
  // Control folding must make the same choice instead of shifting by the old
  // image VA and proving the opposite edge dead.
  MedOp Slice;
  Slice.Opcode = NdOp::SUBBYTES;
  Slice.Output = Sliced;
  Slice.addInput(MedVar::makeConst(0x12345678, 4));
  Slice.addInput(MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  Entry.Ops.push_back(std::move(Slice));
  MedOp Compare;
  Compare.Opcode = NdOp::INT_EQUAL;
  Compare.Output = Cond;
  Compare.addInput(Sliced);
  Compare.addInput(MedVar::makeConst(0x12345678, 4));
  Entry.Ops.push_back(std::move(Compare));
  Branch.Inputs[1] = Cond;
  Entry.Ops.push_back(std::move(Branch));
  return Func;
}

enum class NonPreservingForwardCase { Subbytes, SignExtend };

MedFunc makeNonPreservingForwardPhiTableLookup(Arch TargetArch,
                                               NonPreservingForwardCase Case) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name =
      std::string(TargetArch == Arch::AArch64
                      ? "nonpreserving_phi_table_arm64_"
                      : "nonpreserving_phi_table_x86_64_") +
      (Case == NonPreservingForwardCase::Subbytes ? "subbytes" : "sext");
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  uint64_t FirstBase = Case == NonPreservingForwardCase::Subbytes
                           ? LowSpilledConstTableVA
                           : SignBitTableVA;
  uint64_t SecondBase = Case == NonPreservingForwardCase::Subbytes
                            ? OtherLowSpilledConstTableVA
                            : OtherSignBitTableVA;
  replaceMedConstant(Func, SpilledConstTableVA, FirstBase);
  replaceMedConstant(Func, OtherSpilledConstTableVA, SecondBase);

  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };
  MedVar Input = makeTemp(
      80, Case == NonPreservingForwardCase::SignExtend ? 4 : TRI.PointerSize);
  MedVar Forwarded = makeTemp(81, TRI.PointerSize);
  MedBlock &FirstTable = Func.Blocks[3];
  MedOp Branch = std::move(FirstTable.Ops.back());
  FirstTable.Ops.clear();
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = Input;
  Copy.addInput(MedVar::makeConst(FirstBase, Input.Size));
  FirstTable.Ops.push_back(std::move(Copy));
  MedOp Forward;
  Forward.Opcode = Case == NonPreservingForwardCase::Subbytes ? NdOp::SUBBYTES
                                                              : NdOp::INT_SEXT;
  Forward.Output = Forwarded;
  Forward.addInput(Input);
  if (Case == NonPreservingForwardCase::Subbytes)
    Forward.addInput(MedVar::makeConst(1, TRI.PointerSize));
  FirstTable.Ops.push_back(std::move(Forward));
  FirstTable.Ops.push_back(std::move(Branch));
  for (auto &[Pred, Arg] : Func.Blocks[5].Phis.front().Args)
    if (Pred == FirstTable.Id)
      Arg = Forwarded;
  return Func;
}

enum class InvalidMaskedBlendCase {
  NonBooleanCondition,
  NarrowMask,
  NonBooleanBoolOp
};

MedFunc makeInvalidMaskedBlendTableLookup(Arch TargetArch,
                                          InvalidMaskedBlendCase Case) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };
  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name =
      std::string(TargetArch == Arch::AArch64
                      ? "invalid_masked_table_arm64_"
                      : "invalid_masked_table_x86_64_") +
      (Case == InvalidMaskedBlendCase::NarrowMask         ? "narrow"
       : Case == InvalidMaskedBlendCase::NonBooleanBoolOp ? "boolop"
                                                          : "nonboolean");
  Func.ReturnType = NdType::makeInt(2);
  MedVar Index = makeVar(MedVar::Param, 0, TRI.PointerSize);
  Index.RegOff = TRI.IntParamRegs[0];
  MedVar CondSeed = makeVar(MedVar::Param, 1, 4);
  CondSeed.RegOff = TRI.IntParamRegs[1];
  Func.Params = {Index, CondSeed};
  uint16_t MaskSize =
      Case == InvalidMaskedBlendCase::NarrowMask ? 4 : TRI.PointerSize;
  MedVar BoolCond = makeVar(MedVar::Temp, 80,
                            Case == InvalidMaskedBlendCase::NarrowMask ? 1 : 4);
  MedVar WideCond = makeVar(MedVar::Temp, 81, MaskSize);
  MedVar Mask = makeVar(MedVar::Temp, 82, MaskSize);
  MedVar NotMask = makeVar(MedVar::Temp, 83, MaskSize);
  MedVar Left = makeVar(MedVar::Temp, 84, TRI.PointerSize);
  MedVar Right = makeVar(MedVar::Temp, 85, TRI.PointerSize);
  MedVar Blend = makeVar(MedVar::Temp, 86, TRI.PointerSize);
  MedVar Scaled = makeVar(MedVar::Temp, 87, TRI.PointerSize);
  MedVar Address = makeVar(MedVar::Temp, 88, TRI.PointerSize);
  MedVar Element = makeVar(MedVar::Temp, 89, 2);
  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0x40;
  if (Case == InvalidMaskedBlendCase::NarrowMask) {
    MedOp Compare;
    Compare.Opcode = NdOp::INT_NOTEQUAL;
    Compare.Output = BoolCond;
    Compare.addInput(CondSeed);
    Compare.addInput(MedVar::makeConst(0, 4));
    Entry.Ops.push_back(std::move(Compare));
  } else if (Case == InvalidMaskedBlendCase::NonBooleanBoolOp) {
    MedOp BoolOr;
    BoolOr.Opcode = NdOp::BOOL_OR;
    BoolOr.Output = BoolCond;
    BoolOr.addInput(CondSeed);
    BoolOr.addInput(MedVar::makeConst(0, 4));
    Entry.Ops.push_back(std::move(BoolOr));
  }
  MedOp Widen;
  Widen.Opcode = NdOp::INT_ZEXT;
  Widen.Output = WideCond;
  Widen.addInput(Case == InvalidMaskedBlendCase::NonBooleanCondition
                     ? CondSeed
                     : BoolCond);
  Entry.Ops.push_back(std::move(Widen));
  MedOp Negate;
  Negate.Opcode = NdOp::INT_NEG2;
  Negate.Output = Mask;
  Negate.addInput(WideCond);
  Entry.Ops.push_back(std::move(Negate));
  MedOp Complement;
  Complement.Opcode = NdOp::INT_NOT;
  Complement.Output = NotMask;
  Complement.addInput(Mask);
  Entry.Ops.push_back(std::move(Complement));
  MedOp KeepLeft;
  KeepLeft.Opcode = NdOp::INT_AND;
  KeepLeft.Output = Left;
  KeepLeft.addInput(MedVar::makeConst(LowSpilledConstTableVA, TRI.PointerSize));
  KeepLeft.addInput(Mask);
  Entry.Ops.push_back(std::move(KeepLeft));
  MedOp KeepRight;
  KeepRight.Opcode = NdOp::INT_AND;
  KeepRight.Output = Right;
  KeepRight.addInput(
      MedVar::makeConst(OtherLowSpilledConstTableVA, TRI.PointerSize));
  KeepRight.addInput(NotMask);
  Entry.Ops.push_back(std::move(KeepRight));
  MedOp Or;
  Or.Opcode = NdOp::INT_OR;
  Or.Output = Blend;
  Or.addInput(Left);
  Or.addInput(Right);
  Entry.Ops.push_back(std::move(Or));
  MedOp Scale;
  Scale.Opcode = NdOp::INT_LEFT;
  Scale.Output = Scaled;
  Scale.addInput(Index);
  Scale.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Entry.Ops.push_back(std::move(Scale));
  MedOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Output = Address;
  Add.addInput(Blend);
  Add.addInput(Scaled);
  Entry.Ops.push_back(std::move(Add));
  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = Element;
  Load.addInput(Address);
  Entry.Ops.push_back(std::move(Load));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Element);
  Entry.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Entry));
  return Func;
}

MedFunc makeOneSidedMaskedOrPhiTableLookup(Arch TargetArch) {
  MedFunc Func = makeThreeWayPhiConstTableLookup(TargetArch, true);
  Func.Name = TargetArch == Arch::AArch64
                  ? "one_sided_masked_or_phi_table_arm64"
                  : "one_sided_masked_or_phi_table_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto makeTemp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = TRI.PointerSize;
    return V;
  };
  MedVar WideCond = makeTemp(90);
  MedVar Mask = makeTemp(91);
  MedVar NotMask = makeTemp(92);
  MedVar TableArm = makeTemp(93);
  MedVar ScalarArm = makeTemp(94);
  MedVar Blend = makeTemp(95);
  MedVar BoolCond = makeTemp(89);
  BoolCond.Size = 1;
  MedBlock &Merge = Func.Blocks[5];
  EXPECT_FALSE(Merge.Phis.empty());
  if (Merge.Phis.empty())
    return Func;
  MedVar PhiBase = Merge.Phis.front().Output;
  MedVar Cond = Func.Params[2];
  MedVar Scalar = Func.Params[0];

  std::vector<MedOp> Prefix;
  MedOp Compare;
  Compare.Opcode = NdOp::INT_NOTEQUAL;
  Compare.Output = BoolCond;
  Compare.addInput(Cond);
  Compare.addInput(MedVar::makeConst(0, Cond.Size));
  Prefix.push_back(std::move(Compare));
  MedOp Widen;
  Widen.Opcode = NdOp::INT_ZEXT;
  Widen.Output = WideCond;
  Widen.addInput(BoolCond);
  Prefix.push_back(std::move(Widen));
  MedOp Negate;
  Negate.Opcode = NdOp::INT_NEG2;
  Negate.Output = Mask;
  Negate.addInput(WideCond);
  Prefix.push_back(std::move(Negate));
  MedOp Complement;
  Complement.Opcode = NdOp::INT_NOT;
  Complement.Output = NotMask;
  Complement.addInput(Mask);
  Prefix.push_back(std::move(Complement));
  MedOp KeepTable;
  KeepTable.Opcode = NdOp::INT_AND;
  KeepTable.Output = TableArm;
  KeepTable.addInput(PhiBase);
  KeepTable.addInput(Mask);
  Prefix.push_back(std::move(KeepTable));
  MedOp KeepScalar;
  KeepScalar.Opcode = NdOp::INT_AND;
  KeepScalar.Output = ScalarArm;
  KeepScalar.addInput(Scalar);
  KeepScalar.addInput(NotMask);
  Prefix.push_back(std::move(KeepScalar));
  MedOp Or;
  Or.Opcode = NdOp::INT_OR;
  Or.Output = Blend;
  Or.addInput(TableArm);
  Or.addInput(ScalarArm);
  Prefix.push_back(std::move(Or));
  Merge.Ops.insert(Merge.Ops.begin(), std::make_move_iterator(Prefix.begin()),
                   std::make_move_iterator(Prefix.end()));
  auto Address =
      std::find_if(Merge.Ops.begin(), Merge.Ops.end(),
                   [](MedOp &Op) { return Op.Opcode == NdOp::INT_ADD; });
  EXPECT_NE(Address, Merge.Ops.end());
  if (Address != Merge.Ops.end())
    Address->Inputs[0] = Blend;
  return Func;
}

bool valueReferencesConstantGlobal(const llvm::Value *Root,
                                   std::set<const llvm::Value *> &Seen) {
  if (!Root || !Seen.insert(Root).second)
    return false;
  if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Root))
    return GV->isConstant() && GV->hasInitializer();
  const auto *User = llvm::dyn_cast<llvm::User>(Root);
  if (!User)
    return false;
  for (const llvm::Use &Operand : User->operands())
    if (valueReferencesConstantGlobal(Operand.get(), Seen))
      return true;
  return false;
}

bool valueReferencesTarget(const llvm::Value *Root, const llvm::Value *Target,
                           std::set<const llvm::Value *> &Seen) {
  if (!Root || !Seen.insert(Root).second)
    return false;
  if (Root == Target)
    return true;
  const auto *User = llvm::dyn_cast<llvm::User>(Root);
  if (!User)
    return false;
  for (const llvm::Use &Operand : User->operands())
    if (valueReferencesTarget(Operand.get(), Target, Seen))
      return true;
  return false;
}

const llvm::LoadInst *findVolatileI16Load(const llvm::Function &Function) {
  const llvm::LoadInst *Result = nullptr;
  for (const llvm::BasicBlock &Block : Function)
    for (const llvm::Instruction &Instruction : Block)
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
          Load && Load->getType()->isIntegerTy(16) && Load->isVolatile()) {
        EXPECT_EQ(Result, nullptr);
        Result = Load;
      }
  return Result;
}

const llvm::LoadInst *findNonAllocaI16Load(const llvm::Function &Function) {
  const llvm::LoadInst *Result = nullptr;
  for (const llvm::BasicBlock &Block : Function)
    for (const llvm::Instruction &Instruction : Block)
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
          Load && Load->getType()->isIntegerTy(16) &&
          !llvm::isa<llvm::AllocaInst>(
              Load->getPointerOperand()->stripPointerCasts())) {
        EXPECT_EQ(Result, nullptr);
        Result = Load;
      }
  return Result;
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

MedFunc makePhiPointerCaller() {
  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "data_phi_caller";
  Func.ReturnType = NdType::makeVoid();

  MedVar Cond;
  Cond.Kind = MedVar::Param;
  Cond.Id = 0;
  Cond.Size = 1;
  Cond.TheArch = Arch::AArch64;
  Func.Params.push_back(Cond);

  MedVar Selected;
  Selected.Kind = MedVar::Reg;
  Selected.Id = 1;
  Selected.SSAVer = 44;
  Selected.Size = 8;
  Selected.TheArch = Arch::AArch64;
  Selected.RegOff = getTargetRegInfo(Arch::AArch64).IntParamRegs[1];
  MedVar FirstPointer = Selected;
  FirstPointer.SSAVer = 43;
  MedVar SecondPointer = Selected;
  SecondPointer.SSAVer = 41;

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0x10;
  Entry.Succs = {1, 2};
  MedOp ConditionalBranch;
  ConditionalBranch.Opcode = NdOp::COND_BR;
  ConditionalBranch.Addr = CallerVA;
  ConditionalBranch.addInput(MedVar::makeConst(CallerVA + 0x20, 8));
  ConditionalBranch.addInput(Cond);
  Entry.Ops.push_back(std::move(ConditionalBranch));

  MedBlock First;
  First.Id = 1;
  First.StartAddr = CallerVA + 0x10;
  First.EndAddr = CallerVA + 0x18;
  First.Preds = {0};
  First.Succs = {3};
  MedOp FirstValue;
  FirstValue.Opcode = NdOp::COPY;
  FirstValue.Output = FirstPointer;
  FirstValue.addInput(MedVar::makeConst(CStringVA, 8));
  First.Ops.push_back(std::move(FirstValue));
  MedOp FirstBranch;
  FirstBranch.Opcode = NdOp::BRANCH;
  FirstBranch.addInput(MedVar::makeConst(CallerVA + 0x30, 8));
  First.Ops.push_back(std::move(FirstBranch));

  MedBlock Second;
  Second.Id = 2;
  Second.StartAddr = CallerVA + 0x20;
  Second.EndAddr = CallerVA + 0x28;
  Second.Preds = {0};
  Second.Succs = {3};
  MedOp SecondValue;
  SecondValue.Opcode = NdOp::COPY;
  SecondValue.Output = SecondPointer;
  SecondValue.addInput(MedVar::makeConst(CStringBVA, 8));
  Second.Ops.push_back(std::move(SecondValue));
  MedOp SecondBranch;
  SecondBranch.Opcode = NdOp::BRANCH;
  SecondBranch.addInput(MedVar::makeConst(CallerVA + 0x30, 8));
  Second.Ops.push_back(std::move(SecondBranch));

  MedBlock Merge;
  Merge.Id = 3;
  Merge.StartAddr = CallerVA + 0x30;
  Merge.EndAddr = CallerVA + 0x38;
  Merge.Preds = {1, 2};
  PhiNode PointerPhi;
  PointerPhi.Output = Selected;
  PointerPhi.Args = {{1, FirstPointer}, {2, SecondPointer}};
  Merge.Phis.push_back(std::move(PointerPhi));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = CallerVA + 0x30;
  Call.addInput(MedVar::makeConst(ImportStubVA + 4, 8));
  Merge.Ops.push_back(std::move(Call));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 0x34;
  Merge.Ops.push_back(std::move(Return));

  Func.Blocks = {std::move(Entry), std::move(First), std::move(Second),
                 std::move(Merge)};
  MedCallInfo Info;
  Info.BlockId = 3;
  Info.OpIdx = 0;
  Info.TargetAddr = ImportStubVA + 4;
  Info.TargetName = "_printf";
  Info.Args.push_back(Selected);
  Func.CallInfos.push_back(std::move(Info));
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

TEST(MachOLLVMDataPointerBoundary, SymbolizesExecutableCStringPhi) {
  BinaryImage Image = makeLLVMImage();
  addImport(Image, "_printf", ImportStubVA + 4);
  MedFunc Caller = makePhiPointerCaller();
  llvm::LLVMContext Context;
  testing::internal::CaptureStderr();
  auto Module = MedLLVMEmitter().emit(
      {Caller}, Context, "macho-data-pointer-phi", Arch::AArch64,
      {{ImportStubVA + 4, "_printf"}}, &Image, BinaryFormat::MachO);
  std::string Diagnostic = testing::internal::GetCapturedStderr();
  ASSERT_NE(Module, nullptr) << Diagnostic;
  expectValidModule(*Module);

  llvm::Function *Function = Module->getFunction(Caller.Name);
  ASSERT_NE(Function, nullptr);
  std::vector<llvm::CallInst *> Calls = callsIn(*Function);
  ASSERT_EQ(Calls.size(), 1u);
  llvm::Value *Argument = Calls.front()->getArgOperand(0);
  EXPECT_TRUE(Argument->getType()->isPointerTy());
  std::set<const llvm::Value *> Seen;
  EXPECT_TRUE(valueReferencesConstantGlobal(Argument, Seen));
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

TEST(MachOLLVMDataPointerBoundary,
     SpeculativePointerRewriteDoesNotFatalOnMixedIntegerPhi) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Caller = makeMixedPhiIntegerCall(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Caller}, Context, "macho-mixed-phi-integer-call", TargetArch,
        {{ImportStubVA, "_opaque_integer_sink"}}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    ASSERT_NE(Module, nullptr) << Diagnostic;
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Caller.Name);
    ASSERT_NE(Function, nullptr);
    std::vector<llvm::CallInst *> Calls = callsIn(*Function);
    ASSERT_EQ(Calls.size(), 1u);
    EXPECT_TRUE(Calls.front()->getArgOperand(0)->getType()->isIntegerTy());
  }
}

TEST(MachOLLVMDataPointerBoundary,
     CachedSpeculativeProofCannotBypassLaterStrictLoad) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Caller = makeSpeculativeIntegerCallThenStrictLoad(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Caller}, Context, "macho-cached-speculative-then-strict", TargetArch,
        {{ImportStubVA, "_opaque_integer_sink"}}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module, nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     SpeculativePointerRewriteRejectsNestedMixedPhiBesideInduction) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Caller =
        makeTwoRecurrentPointerTableLookup(TargetArch, NdOp::INT_ADD);
    Caller.Name = TargetArch == Arch::AArch64
                      ? "nested_mixed_phi_integer_call_arm64"
                      : "nested_mixed_phi_integer_call_x86_64";

    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    MedBlock &Loop = Caller.Blocks[1];
    ASSERT_EQ(Loop.Phis.size(), 2u);
    ASSERT_EQ(Loop.Phis[1].Args.size(), 2u);
    // The second address operand is a non-recurrent, live merge of another
    // table base and an opaque scalar.  The first operand remains a valid
    // recurrent table pointer.
    Loop.Phis[1].Args[1].second = Caller.Params.front();
    ASSERT_GE(Loop.Ops.size(), 3u);
    MedVar Address = Loop.Ops[1].Output;
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Loop.StartAddr + 4;
    Call.addInput(MedVar::makeConst(ImportStubVA, TRI.PointerSize));
    Loop.Ops[2] = std::move(Call);

    MedCallInfo Info;
    Info.BlockId = Loop.Id;
    Info.OpIdx = 2;
    Info.TargetAddr = ImportStubVA;
    Info.TargetName = "_opaque_integer_sink";
    Info.Args.push_back(Address);
    Caller.CallInfos.push_back(std::move(Info));

    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Caller}, Context, "macho-nested-mixed-phi-integer-call", TargetArch,
        {{ImportStubVA, "_opaque_integer_sink"}}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    ASSERT_NE(Module, nullptr) << Diagnostic;
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Caller.Name);
    ASSERT_NE(Function, nullptr);
    std::vector<llvm::CallInst *> Calls = callsIn(*Function);
    ASSERT_EQ(Calls.size(), 1u);
    EXPECT_FALSE(isPtrToIntValue(Calls.front()->getArgOperand(0)));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     SpeculativePointerRewriteRejectsMixedPhiUsedAsTableIndex) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Caller = makeMixedPhiIntegerCall(TargetArch);
    Caller.Name = TargetArch == Arch::AArch64
                      ? "mixed_phi_table_index_integer_call_arm64"
                      : "mixed_phi_table_index_integer_call_x86_64";

    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    MedBlock &Merge = Caller.Blocks[5];
    ASSERT_EQ(Merge.Phis.size(), 1u);
    ASSERT_FALSE(Merge.Ops.empty());
    MedVar Address = Merge.Phis.front().Output;
    Address.Kind = MedVar::Temp;
    Address.Id = 90;
    Address.SSAVer = 1;
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output = Address;
    Add.addInput(MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
    Add.addInput(Merge.Phis.front().Output);
    Merge.Ops.insert(Merge.Ops.begin(), std::move(Add));
    ASSERT_EQ(Caller.CallInfos.size(), 1u);
    Caller.CallInfos.front().OpIdx = 1;
    Caller.CallInfos.front().Args.front() = Address;

    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Caller}, Context, "macho-mixed-phi-table-index-integer-call",
        TargetArch, {{ImportStubVA, "_opaque_integer_sink"}}, &Image,
        BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    ASSERT_NE(Module, nullptr) << Diagnostic;
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Caller.Name);
    ASSERT_NE(Function, nullptr);
    std::vector<llvm::CallInst *> Calls = callsIn(*Function);
    ASSERT_EQ(Calls.size(), 1u);
    EXPECT_FALSE(isPtrToIntValue(Calls.front()->getArgOperand(0)));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     ResolvesReadOnlyTableBaseAcrossLocalFrameSpill) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeSpilledConstTableLookup(TargetArch);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Lookup}, Context, "macho-spilled-const-table",
                              TargetArch, {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = nullptr;
    for (const llvm::BasicBlock &Block : *Function)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
            Load && Load->getType()->isIntegerTy(16) && Load->isVolatile()) {
          ASSERT_EQ(TableLoad, nullptr);
          TableLoad = Load;
        }
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_TRUE(
        llvm::isa<llvm::GetElementPtrInst>(TableLoad->getPointerOperand()));
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RejectsUnreadablePageZeroMaskAsRelocatableConstant) {
  constexpr uint64_t Mask32 = 0xffffffffULL;
  constexpr uint16_t PointerSize = 8;

  BinaryImage PageZeroImage = makeSpilledConstTableImage(Arch::AArch64);
  addMachOPageZero(PageZeroImage);
  MedFunc PageZeroLookup = makeSpilledConstTableLookup(Arch::AArch64);
  llvm::LLVMContext PageZeroContext;
  MedLLVMEmitter PageZeroEmitter;
  auto PageZeroModule = PageZeroEmitter.emit(
      {PageZeroLookup}, PageZeroContext, "macho-pagezero-mask-classification",
      Arch::AArch64, {}, &PageZeroImage, BinaryFormat::MachO);
  ASSERT_NE(PageZeroModule, nullptr);
  expectValidModule(*PageZeroModule);
  EXPECT_FALSE(MedLLVMProvenanceTestPeer::symbolizesDataConstant(
      PageZeroEmitter, Mask32, PointerSize));
  EXPECT_FALSE(MedLLVMProvenanceTestPeer::mayRelocateConstant(
      PageZeroEmitter, Mask32, PointerSize));

  BinaryImage LowImage = makeSpilledConstTableImage(Arch::AArch64);
  addThresholdCrossingConstTableRun(LowImage);
  MedFunc LowLookup = makeSpilledConstTableLookup(Arch::AArch64);
  llvm::LLVMContext LowContext;
  MedLLVMEmitter LowEmitter;
  auto LowModule = LowEmitter.emit(
      {LowLookup}, LowContext, "macho-low-loader-proven-classification",
      Arch::AArch64, {}, &LowImage, BinaryFormat::MachO);
  ASSERT_NE(LowModule, nullptr);
  expectValidModule(*LowModule);
  EXPECT_FALSE(MedLLVMProvenanceTestPeer::symbolizesDataConstant(
      LowEmitter, LowSpilledConstTableVA, PointerSize));
  LowImage.RelocDataAddrs.insert(LowSpilledConstTableVA);
  EXPECT_TRUE(MedLLVMProvenanceTestPeer::symbolizesDataConstant(
      LowEmitter, LowSpilledConstTableVA, PointerSize));
  EXPECT_TRUE(MedLLVMProvenanceTestPeer::mayRelocateConstant(
      LowEmitter, LowSpilledConstTableVA, PointerSize));
}

TEST(MachOLLVMDataPointerBoundary,
     KeepsMaskedScalarIndexPhiOutOfTableProvenance) {
  constexpr uint64_t Mask32 = 0xffffffffULL;
  struct Scenario {
    bool AddPageZero;
    bool AddFalseLoaderEvidence;
    const char *Name;
  };
  const Scenario Scenarios[] = {
      {true, false, "pagezero"},
      {true, true, "pagezero-with-loader-evidence"},
      {false, true, "unmapped-with-loader-evidence"},
  };

  for (const Scenario &Case : Scenarios) {
    SCOPED_TRACE(Case.Name);
    BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
    if (Case.AddPageZero)
      addMachOPageZero(Image);
    Image.RelocDataAddrs.insert(SpilledConstTableVA);
    if (Case.AddFalseLoaderEvidence)
      Image.RelocDataAddrs.insert(Mask32);
    MedFunc Lookup = makeMaskedScalarPhiIndexLookup();
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context,
        std::string("macho-masked-scalar-phi-table-index-") + Case.Name,
        Arch::AArch64, {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    if (!Module) {
      EXPECT_NE(
          Diagnostic.find("ambiguous reachable read-only table-base PHI X10.1"),
          std::string::npos)
          << Diagnostic;
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
      ADD_FAILURE() << "scalar mask was promoted to address provenance";
      continue;
    }
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));

    bool SawIntegerMask = false;
    for (const llvm::BasicBlock &Block : *Function)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *And =
                llvm::dyn_cast<llvm::BinaryOperator>(&Instruction);
            And && And->getOpcode() == llvm::Instruction::And)
          for (const llvm::Value *Operand : And->operands())
            if (const auto *Integer =
                    llvm::dyn_cast<llvm::ConstantInt>(Operand))
              SawIntegerMask |= Integer->getZExtValue() == Mask32;
    EXPECT_TRUE(SawIntegerMask);
    EXPECT_EQ(Module->getNamedGlobal(makeNdDataSymbol(Mask32)), nullptr);
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RejectsUnmaterializableReadOnlyAndWritableRelocationEvidence) {
  constexpr uint64_t Mask32 = 0xffffffffULL;
  struct Scenario {
    bool AddPageZero;
    bool WritableEvidence;
    const char *Name;
  };
  const Scenario Scenarios[] = {
      {false, false, "unmapped-readonly"},
      {true, false, "pagezero-readonly"},
      {false, true, "unmapped-writable"},
      {true, true, "pagezero-writable"},
  };

  for (const Scenario &Case : Scenarios) {
    SCOPED_TRACE(Case.Name);
    BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
    if (Case.AddPageZero)
      addMachOPageZero(Image);
    if (Case.WritableEvidence)
      Image.WritableRelocDataAddrs.insert(Mask32);
    else
      Image.RelocDataAddrs.insert(Mask32);

    MedFunc Caller = makePointerCall(
        std::string("unmaterializable-loader-evidence-") + Case.Name,
        ImportStubVA, {MedVar::makeConst(Mask32, 8)});
    llvm::LLVMContext Context;
    MedLLVMEmitter Emitter;
    auto Module = Emitter.emit(
        {Caller}, Context,
        std::string("macho-unmaterializable-loader-evidence-") + Case.Name,
        Arch::AArch64, {{ImportStubVA, "_getopt_long"}}, &Image,
        BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Caller.Name);
    ASSERT_NE(Function, nullptr);
    std::vector<llvm::CallInst *> Calls = callsIn(*Function);
    ASSERT_EQ(Calls.size(), 1u);
    const auto *Argument =
        llvm::dyn_cast<llvm::ConstantInt>(Calls.front()->getArgOperand(0));
    ASSERT_NE(Argument, nullptr);
    EXPECT_EQ(Argument->getZExtValue(), Mask32);
    EXPECT_FALSE(MedLLVMProvenanceTestPeer::symbolizesDataConstant(
        Emitter, Mask32, /*Size=*/8));
    EXPECT_FALSE(MedLLVMProvenanceTestPeer::mayRelocateConstant(Emitter, Mask32,
                                                                /*Size=*/8));
    EXPECT_EQ(Module->getNamedGlobal(makeNdDataSymbol(Mask32)), nullptr);
  }

  // A readable segment range can be larger than its file-backed bytes.  A
  // scalar that merely lands in that sparse tail is still an integer: mapping
  // permission alone is not trusted pointer intent and must not turn the new
  // fail-closed contract into a regression.
  BinaryImage SparseImage = makeSpilledConstTableImage(Arch::AArch64);
  Segment Sparse;
  Sparse.Name = "__SPARSE_CONST";
  Sparse.VA = 0xfff00000ULL;
  Sparse.Size = 0x200000;
  Sparse.Flags = SegmentFlags::Readable;
  SparseImage.Segments.push_back(std::move(Sparse));
  MedFunc SparseCaller =
      makePointerCall("sparse_segment_scalar", ImportStubVA,
                      {MedVar::makeConst(Mask32, /*Size=*/8)});
  llvm::LLVMContext SparseContext;
  MedLLVMEmitter SparseEmitter;
  auto SparseModule = SparseEmitter.emit(
      {SparseCaller}, SparseContext, "macho-sparse-segment-scalar",
      Arch::AArch64, {{ImportStubVA, "_getopt_long"}}, &SparseImage,
      BinaryFormat::MachO);
  ASSERT_NE(SparseModule, nullptr);
  expectValidModule(*SparseModule);
  llvm::Function *SparseFunction = SparseModule->getFunction(SparseCaller.Name);
  ASSERT_NE(SparseFunction, nullptr);
  std::vector<llvm::CallInst *> SparseCalls = callsIn(*SparseFunction);
  ASSERT_EQ(SparseCalls.size(), 1u);
  const auto *SparseArgument =
      llvm::dyn_cast<llvm::ConstantInt>(SparseCalls.front()->getArgOperand(0));
  ASSERT_NE(SparseArgument, nullptr);
  EXPECT_EQ(SparseArgument->getZExtValue(), Mask32);
  EXPECT_FALSE(MedLLVMProvenanceTestPeer::mayRelocateConstant(
      SparseEmitter, Mask32, /*Size=*/8));
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForTrustedButUnmaterializableDataPointers) {
  {
    BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
    Image.RodataAnchorSeg[SpilledConstTableVA] = 0xdead0000;
    MedFunc Caller =
        makePointerCall("invalid_rodata_anchor", ImportStubVA,
                        {MedVar::makeConst(SpilledConstTableVA, /*Size=*/8)});
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Caller}, Context, "macho-invalid-rodata-anchor", Arch::AArch64,
        {{ImportStubVA, "_getopt_long"}}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module, nullptr);
    EXPECT_NE(Diagnostic.find("no global-data materialization route"),
              std::string::npos)
        << Diagnostic;
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }

  {
    constexpr uint64_t WritableBase = 0x200000000ULL;
    BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
    Segment Writable;
    Writable.Name = "__OVERSIZED_DATA";
    Writable.VA = WritableBase;
    Writable.Size = limits::kMaxSingleGlobalEmbedLen + 1;
    Writable.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(std::move(Writable));
    Image.WritableRelocDataAddrs.insert(WritableBase);
    MedFunc Caller =
        makePointerCall("oversized_writable_pointer", ImportStubVA,
                        {MedVar::makeConst(WritableBase, /*Size=*/8)});
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Caller}, Context, "macho-oversized-writable-pointer", Arch::AArch64,
        {{ImportStubVA, "_getopt_long"}}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module, nullptr);
    EXPECT_NE(Diagnostic.find("no global-data materialization route"),
              std::string::npos)
        << Diagnostic;
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }

  {
    constexpr uint64_t RodataBase = 0x300000000ULL;
    BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
    Segment Rodata;
    Rodata.Name = "__OVERSIZED_CONST";
    Rodata.VA = RodataBase;
    Rodata.Size = limits::kMaxSingleGlobalEmbedLen + 1;
    Rodata.FileSz = Rodata.Size;
    Rodata.Flags = SegmentFlags::Readable;
    Rodata.Data.assign(static_cast<size_t>(Rodata.Size), 0);
    const uint64_t RodataEnd = Rodata.VA + Rodata.Data.size();
    Image.Segments.push_back(std::move(Rodata));
    MedFunc Compare = makePointerEndComparison(
        "oversized_rodata_end", Arch::AArch64, RodataBase, RodataEnd);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module =
        MedLLVMEmitter().emit({Compare}, Context, "macho-oversized-rodata-end",
                              Arch::AArch64, {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module, nullptr);
    EXPECT_NE(Diagnostic.find("no global-data materialization route"),
              std::string::npos)
        << Diagnostic;
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }

  {
    constexpr uint64_t WritableBase = 0x210000000ULL;
    BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
    Segment Writable;
    Writable.Name = "__OVERSIZED_DIRECT_DATA";
    Writable.VA = WritableBase;
    Writable.Size = limits::kMaxSingleGlobalEmbedLen + 1;
    Writable.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    const uint64_t WritableEnd = Writable.VA + Writable.Size;
    Image.Segments.push_back(std::move(Writable));
    MedFunc Access = makePointerEndComparison(
        "oversized_writable_access", Arch::AArch64, WritableBase, WritableEnd);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Access}, Context, "macho-oversized-writable-access", Arch::AArch64, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module, nullptr);
    EXPECT_NE(Diagnostic.find("no global-data materialization route"),
              std::string::npos)
        << Diagnostic;
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedWhenInductionStringCannotUseOneCanonicalRun) {
  BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
  Segment &Rodata = Image.Segments.front();
  Rodata.Name = "__OVERSIZED_CONST";
  Rodata.Size = limits::kMaxSingleGlobalEmbedLen + 1;
  Rodata.FileSz = Rodata.Size;
  Rodata.Flags = SegmentFlags::Readable;
  Rodata.Data.assign(static_cast<size_t>(Rodata.Size), 0);
  constexpr char WalkedString[] = "walked-string";
  std::memcpy(Rodata.Data.data() + (SpilledConstTableVA - Rodata.VA),
              WalkedString, sizeof(WalkedString));

  MedFunc Lookup = makeRecurrentConstTableLookup(Arch::AArch64);
  llvm::LLVMContext Context;
  testing::internal::CaptureStderr();
  auto Module = MedLLVMEmitter().emit(
      {Lookup}, Context, "macho-oversized-induction-string", Arch::AArch64, {},
      &Image, BinaryFormat::MachO);
  std::string Diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_EQ(Module, nullptr);
  EXPECT_NE(Diagnostic.find("canonical read-only run"), std::string::npos)
      << Diagnostic;
  EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
            std::string::npos)
      << Diagnostic;
}

TEST(MachOLLVMDataPointerBoundary, AllowsSelfCopyScalarTableOffset) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeSpilledConstTableLookup(TargetArch);

    ASSERT_FALSE(Lookup.Params.empty());
    ASSERT_FALSE(Lookup.Blocks.empty());
    const MedVar Index = Lookup.Params.front();
    MedOp SelfCopy;
    SelfCopy.Opcode = NdOp::COPY;
    SelfCopy.Output = Index;
    SelfCopy.addInput(Index);
    Lookup.Blocks.front().Ops.insert(Lookup.Blocks.front().Ops.begin(),
                                     std::move(SelfCopy));

    const MedOp &Inserted = Lookup.Blocks.front().Ops.front();
    ASSERT_EQ(Inserted.Opcode, NdOp::COPY);
    ASSERT_EQ(Inserted.NumInputs, 1u);
    EXPECT_EQ(Inserted.Output, Inserted.Inputs[0]);
    EXPECT_EQ(Inserted.Output.TheArch, Inserted.Inputs[0].TheArch);
    EXPECT_EQ(Inserted.Output.Size, Inserted.Inputs[0].Size);
    EXPECT_EQ(Inserted.Output.RenameTag, Inserted.Inputs[0].RenameTag);
    EXPECT_EQ(Inserted.Output.RegOff, Inserted.Inputs[0].RegOff);

    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-self-copy-scalar-table-offset", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    ASSERT_NE(Module, nullptr) << Diagnostic;
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RejectsTwoNodeForwardingCycleBesideTableBase) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    ASSERT_GE(TRI.IntParamRegs.size(), 2u);
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeSpilledConstTableLookup(TargetArch);

    MedVar First = Lookup.Params.front();
    MedVar Second = First;
    Second.Id = 99;
    Second.RegOff = TRI.IntParamRegs[1];
    Lookup.Params.push_back(Second);

    MedOp FirstFromSecond;
    FirstFromSecond.Opcode = NdOp::COPY;
    FirstFromSecond.Output = First;
    FirstFromSecond.addInput(Second);
    MedOp SecondFromFirst;
    SecondFromFirst.Opcode = NdOp::COPY;
    SecondFromFirst.Output = Second;
    SecondFromFirst.addInput(First);
    MedBlock &Entry = Lookup.Blocks.front();
    Entry.Ops.insert(Entry.Ops.begin(), std::move(SecondFromFirst));
    Entry.Ops.insert(Entry.Ops.begin(), std::move(FirstFromSecond));

    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-two-node-scalar-forward-cycle", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     LeavesRelocatableCodeBaseOutsideDataTableAudit) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    ASSERT_TRUE(Image.isCodeAddress(CodeVA));

    MedFunc Lookup = makeSpilledConstTableLookup(TargetArch);
    Lookup.Name = TargetArch == Arch::AArch64
                      ? "code_base_scalar_offset_arm64"
                      : "code_base_scalar_offset_x86_64";
    replaceMedConstant(Lookup, SpilledConstTableVA, CodeVA);

    MedFunc CodeTarget;
    CodeTarget.Entry = CodeVA;
    CodeTarget.Name = TargetArch == Arch::AArch64 ? "code_target_arm64"
                                                  : "code_target_x86_64";
    CodeTarget.ReturnType = NdType::makeInt(2);
    MedBlock TargetBlock;
    TargetBlock.Id = 0;
    TargetBlock.StartAddr = CodeVA;
    TargetBlock.EndAddr = CodeVA + 4;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(MedVar::makeConst(0, 2));
    TargetBlock.Ops.push_back(std::move(Return));
    CodeTarget.Blocks.push_back(std::move(TargetBlock));

    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup, CodeTarget}, Context, "macho-code-base-scalar-offset",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    ASSERT_NE(Module, nullptr) << Diagnostic;
    expectValidModule(*Module);
    EXPECT_NE(Module->getFunction(Lookup.Name), nullptr);
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RejectsAmbiguousOrUninitializedFrameReloadProvenance) {
  const std::pair<ReachingStoreCase, const char *> Cases[] = {
      {ReachingStoreCase::BypassPredecessor, "bypass predecessor"},
      {ReachingStoreCase::MalformedMissingBypassPredecessor,
       "malformed missing bypass predecessor"},
      {ReachingStoreCase::PartialOverlap, "partial overlap"},
      {ReachingStoreCase::StoreAfterLoad, "store after load"},
  };
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (const auto &[Case, Label] : Cases) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(Label);
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      MedFunc Lookup = makeRejectedFrameReloadLookup(TargetArch, Case);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-rejected-frame-reload", TargetArch, {},
          &Image, BinaryFormat::MachO);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *Function = Module->getFunction(Lookup.Name);
      ASSERT_NE(Function, nullptr);
      const llvm::LoadInst *TableLoad = nullptr;
      for (const llvm::BasicBlock &Block : *Function)
        for (const llvm::Instruction &Instruction : Block)
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
              Load && Load->getType()->isIntegerTy(16) &&
              !llvm::isa<llvm::AllocaInst>(
                  Load->getPointerOperand()->stripPointerCasts())) {
            ASSERT_EQ(TableLoad, nullptr);
            TableLoad = Load;
          }
      ASSERT_NE(TableLoad, nullptr);
      std::set<const llvm::Value *> Seen;
      EXPECT_FALSE(
          valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
    }
}

TEST(MachOLLVMDataPointerBoundary,
     TreatsLiveInFramePointerAsExactButDistinctFrameOrigin) {
  auto useLiveInFramePointer = [](MedFunc &Func) {
    const TargetRegInfo &TRI = getTargetRegInfo(Arch::X86);
    ASSERT_FALSE(Func.Blocks.empty());
    ASSERT_FALSE(Func.Blocks.front().Ops.empty());
    MedOp &FormSlot = Func.Blocks.front().Ops.front();
    ASSERT_EQ(FormSlot.Opcode, NdOp::INT_ADD);
    ASSERT_GE(FormSlot.NumInputs, 1u);
    MedVar LiveInFP = FormSlot.Inputs[0];
    LiveInFP.Id += 1000;
    LiveInFP.RegOff = TRI.FramePointer;
    FormSlot.Inputs[0] = LiveInFP;
  };
  auto slotReload = [](const MedFunc &Func) -> const MedOp * {
    const MedVar &Slot = Func.Blocks.front().Ops.front().Output;
    for (const MedBlock &Block : Func.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
            Op.Inputs[0] == Slot)
          return &Op;
    return nullptr;
  };

  MedFunc Defined = makeSpilledConstTableLookup(Arch::X86);
  useLiveInFramePointer(Defined);
  const MedOp *DefinedReload = slotReload(Defined);
  ASSERT_NE(DefinedReload, nullptr);
  MedLLVMEmitter DefinedEmitter;
  std::vector<MedVar> Sources;
  ASSERT_TRUE(MedLLVMProvenanceTestPeer::collectFrameReloadSources(
      DefinedEmitter, Defined, Arch::X86, *DefinedReload, Sources));
  ASSERT_EQ(Sources.size(), 1u);
  EXPECT_EQ(Sources.front(), Defined.Blocks.front().Ops[1].Output);

  MedFunc Late = makeRejectedFrameReloadLookup(
      Arch::X86, ReachingStoreCase::StoreAfterLoad);
  useLiveInFramePointer(Late);
  const MedOp *LateReload = slotReload(Late);
  ASSERT_NE(LateReload, nullptr);
  MedLLVMEmitter LateEmitter;
  EXPECT_FALSE(MedLLVMProvenanceTestPeer::collectFrameReloadSources(
      LateEmitter, Late, Arch::X86, *LateReload, Sources));
}

TEST(MachOLLVMDataPointerBoundary,
     ReachingStoreProofUsesFunctionEntryNotBlockStorageOrder) {
  MedFunc Func = makeLoopFrameReloadTableOffsetLookup(
      Arch::X64, /*InitializeInPreheader=*/true);
  ASSERT_EQ(Func.Blocks.size(), 3u);
  const MedVar Slot = Func.Blocks[0].Ops.front().Output;
  std::rotate(Func.Blocks.begin(), std::next(Func.Blocks.begin()),
              Func.Blocks.end());

  const MedOp *Reload = nullptr;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 && Op.Inputs[0] == Slot)
        Reload = &Op;
  ASSERT_NE(Reload, nullptr);

  MedLLVMEmitter Emitter;
  std::vector<MedVar> Sources;
  ASSERT_TRUE(MedLLVMProvenanceTestPeer::collectFrameReloadSources(
      Emitter, Func, Arch::X64, *Reload, Sources));
  ASSERT_EQ(Sources.size(), 1u);
  EXPECT_EQ(Sources.front(), Func.Params.front());
}

TEST(MachOLLVMDataPointerBoundary,
     ProvesOnlyPreheaderInitializedLoopFrameReloadAsStableOffset) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.RelocDataAddrs.insert(SpilledConstTableVA);

    MedFunc Positive =
        makeLoopFrameReloadTableOffsetLookup(TargetArch, /*preheader=*/true);
    llvm::LLVMContext PositiveContext;
    auto PositiveModule = MedLLVMEmitter().emit(
        {Positive}, PositiveContext, "macho-loop-frame-stable-offset",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(PositiveModule, nullptr);
    expectValidModule(*PositiveModule);
    llvm::Function *PositiveFunction =
        PositiveModule->getFunction(Positive.Name);
    ASSERT_NE(PositiveFunction, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*PositiveFunction);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "tblptr");

    MedFunc Negative = makeLoopFrameReloadTableOffsetLookup(
        TargetArch, /*InitializeInPreheader=*/false);
    llvm::LLVMContext NegativeContext;
    testing::internal::CaptureStderr();
    auto NegativeModule = MedLLVMEmitter().emit(
        {Negative}, NegativeContext, "macho-loop-frame-uninitialized-offset",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(NegativeModule.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     CanonicalizesSPFPAliasesForPointerTableScalarRecurrence) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeLLVMImage();
    Image.Arch = TargetArch;
    constexpr uint64_t ScalarLiteral = 0x9E3779B185EBCA87ULL;
    writeObject(Image.Segments.front().Data,
                static_cast<size_t>(ScalarLiteralVA - TextVA), ScalarLiteral);
    MedFunc Lookup = makeFrameAliasedPointerTableRecurrence(TargetArch);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-frame-alias-pointer-table-recurrence",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    bool FoundPointerTableLoad = false;
    for (const llvm::BasicBlock &Block : *Function)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
            Load && Load->getPointerOperand()->getName() == "cptptr")
          FoundPointerTableLoad = true;
    EXPECT_TRUE(FoundPointerTableLoad);
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RelocatedExecutableLiteralCannotBecomeScalarTableOffset) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeLLVMImage();
    Image.Arch = TargetArch;
    constexpr uint64_t ScalarLiteral = 0x9E3779B185EBCA87ULL;
    writeObject(Image.Segments.front().Data,
                static_cast<size_t>(ScalarLiteralVA - TextVA), ScalarLiteral);
    RelocationEntry Relocation;
    Relocation.Address = ScalarLiteralVA;
    Relocation.Type = 1;
    Image.Relocations.push_back(std::move(Relocation));
    MedFunc Lookup = makeFrameAliasedPointerTableRecurrence(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-relocated-literal-table-offset", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     AuditsSelfLoopPostLoadStoreOnLaterIterations) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.RelocDataAddrs.insert(SpilledConstTableVA);
    MedFunc Lookup =
        makeLoopFrameReloadTableOffsetLookup(TargetArch, /*preheader=*/true);
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    MedOp LatePointerStore;
    LatePointerStore.Opcode = NdOp::STORE;
    LatePointerStore.addInput(Lookup.Blocks[0].Ops.front().Output);
    LatePointerStore.addInput(
        MedVar::makeConst(OtherSpilledConstTableVA, TRI.PointerSize));
    Lookup.Blocks[1].Ops.insert(std::next(Lookup.Blocks[1].Ops.begin()),
                                std::move(LatePointerStore));

    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-loop-frame-late-pointer-store", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RejectsNarrowFrameReloadOfIndependentTableBaseAsScalarOffset) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    Image.RelocDataAddrs.insert(SpilledConstTableVA);
    Image.RelocDataAddrs.insert(OtherSpilledConstTableVA);
    MedFunc Lookup = makeNarrowFrameReloadSecondTableBaseLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-narrow-frame-second-table-base", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    std::string IR;
    if (Module) {
      llvm::raw_string_ostream Stream(IR);
      Module->print(Stream, nullptr);
    }
    EXPECT_EQ(Module.get(), nullptr) << IR;
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForDistinctRawFrameReloadTableBases) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeRejectedFrameReloadLookup(
        TargetArch, ReachingStoreCase::DistinctPredecessorBases);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-distinct-frame-table-bases", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    std::string IR;
    if (Module) {
      llvm::raw_string_ostream Stream(IR);
      Module->print(Stream, nullptr);
      Stream.flush();
    }
    EXPECT_EQ(Module.get(), nullptr) << IR;
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     InductionScanIgnoresLiteralPoolStoreAfterReload) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    uint64_t Relative = OtherSpilledConstTableVA - CallerVA;
    writeObject(
        Image.Segments.front().Data,
        static_cast<size_t>(SpilledConstTableVA - Image.Segments.front().VA),
        Relative);
    MedFunc Lookup = makeLateLiteralPoolSpillLookup(TargetArch);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-late-literal-pool-spill", TargetArch, {},
        &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findNonAllocaI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    std::set<const llvm::Value *> Seen;
    EXPECT_FALSE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     ResolvesReadOnlyTableBaseAcrossReachablePhiInputs) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (PhiTableBaseCase Case : {PhiTableBaseCase::DeadScalarClobber,
                                  PhiTableBaseCase::SameBaseOnReachableEdges}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(Case == PhiTableBaseCase::DeadScalarClobber
                       ? "statically dead clobber"
                       : "same base on both reachable edges");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      MedFunc Lookup = makePhiConstTableLookup(TargetArch, Case);
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Lookup}, Context, "macho-phi-const-table",
                                TargetArch, {}, &Image, BinaryFormat::MachO);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *Function = Module->getFunction(Lookup.Name);
      ASSERT_NE(Function, nullptr);
      const llvm::LoadInst *TableLoad = nullptr;
      for (const llvm::BasicBlock &Block : *Function)
        for (const llvm::Instruction &Instruction : Block)
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
              Load && Load->getType()->isIntegerTy(16) && Load->isVolatile()) {
            ASSERT_EQ(TableLoad, nullptr);
            TableLoad = Load;
          }
      ASSERT_NE(TableLoad, nullptr);
      EXPECT_TRUE(
          llvm::isa<llvm::GetElementPtrInst>(TableLoad->getPointerOperand()));
      std::set<const llvm::Value *> Seen;
      EXPECT_TRUE(
          valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
    }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForAmbiguousReachablePhiTableBase) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (PhiTableBaseCase Case : {
             PhiTableBaseCase::AmbiguousRuntimeClobber,
             PhiTableBaseCase::MalformedRuntimeClobber,
         }) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(Case == PhiTableBaseCase::MalformedRuntimeClobber
                       ? "missing predecessor block"
                       : "reachable runtime clobber");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      MedFunc Lookup = makePhiConstTableLookup(TargetArch, Case);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-ambiguous-phi-const-table", TargetArch, {},
          &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary,
     KeepsSelfRecurrentTablePhiOnInductionResolverPath) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeRecurrentConstTableLookup(TargetArch);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Lookup}, Context, "macho-recurrent-const-table",
                              TargetArch, {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = nullptr;
    for (const llvm::BasicBlock &Block : *Function)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
            Load && Load->getType()->isIntegerTy(16) && Load->isVolatile()) {
          ASSERT_EQ(TableLoad, nullptr);
          TableLoad = Load;
        }
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_TRUE(
        llvm::isa<llvm::GetElementPtrInst>(TableLoad->getPointerOperand()));
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "indptr");
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     ReusesIndexedBaseProofForRepeatedAddressConsumers) {
  constexpr unsigned ManyLoadCount = 64;
  BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
  std::vector<MedFunc> OneFunction = {
      makeRepeatedRecurrentConstTableLookup(Arch::AArch64, 1)};
  std::vector<MedFunc> ManyFunctions = {
      makeRepeatedRecurrentConstTableLookup(Arch::AArch64, ManyLoadCount)};

  llvm::LLVMContext OneContext;
  MedLLVMEmitter OneEmitter;
  auto OneModule =
      OneEmitter.emit(OneFunction, OneContext, "macho-repeated-address-once",
                      Arch::AArch64, {}, &Image, BinaryFormat::MachO);
  ASSERT_NE(OneModule, nullptr);
  expectValidModule(*OneModule);
  const auto OneWork =
      MedLLVMProvenanceTestPeer::addressProvenanceWork(OneEmitter);

  llvm::LLVMContext ManyContext;
  MedLLVMEmitter ManyEmitter;
  auto ManyModule = ManyEmitter.emit(
      ManyFunctions, ManyContext, "macho-repeated-address-many", Arch::AArch64,
      {}, &Image, BinaryFormat::MachO);
  ASSERT_NE(ManyModule, nullptr);
  expectValidModule(*ManyModule);

  llvm::Function *Function =
      ManyModule->getFunction(ManyFunctions.front().Name);
  ASSERT_NE(Function, nullptr);
  unsigned SymbolizedLoads = 0;
  for (const llvm::BasicBlock &Block : *Function)
    for (const llvm::Instruction &Instruction : Block)
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
          Load && Load->getType()->isIntegerTy(16) && Load->isVolatile()) {
        ++SymbolizedLoads;
        EXPECT_TRUE(
            llvm::isa<llvm::GetElementPtrInst>(Load->getPointerOperand()));
        EXPECT_TRUE(Load->getPointerOperand()->getName().starts_with("indptr"));
        std::set<const llvm::Value *> Seen;
        EXPECT_TRUE(
            valueReferencesConstantGlobal(Load->getPointerOperand(), Seen));
      }
  EXPECT_EQ(SymbolizedLoads, ManyLoadCount);

  const auto ManyWork =
      MedLLVMProvenanceTestPeer::addressProvenanceWork(ManyEmitter);
  EXPECT_LE(std::get<0>(ManyWork), std::get<0>(OneWork) + 2);
  EXPECT_LE(std::get<1>(ManyWork), std::get<1>(OneWork) + 2);
  EXPECT_LE(std::get<2>(ManyWork), std::get<2>(OneWork) + 2);
  EXPECT_LE(std::get<3>(ManyWork), std::get<3>(OneWork) + 2)
      << "indexed-base proof work must stay constant when the same address "
         "expression feeds more loads";

  const MedVar &RecurrentAddress =
      ManyFunctions.front().Blocks[1].Ops[1].Output;
  MedLLVMProvenanceTestPeer::resetAddressProvenanceWork(ManyEmitter);
  for (unsigned I = 0; I < ManyLoadCount; ++I)
    EXPECT_FALSE(
        MedLLVMProvenanceTestPeer::indexedBase(ManyEmitter, RecurrentAddress));
  const auto DirectWork =
      MedLLVMProvenanceTestPeer::addressProvenanceWork(ManyEmitter);
  EXPECT_LE(std::get<3>(DirectWork), 1U)
      << "a completed negative indexed-base proof must be reused";
}

TEST(MachOLLVMDataPointerBoundary, ReusesExactPhiEdgeClassification) {
  BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
  std::vector<MedFunc> Functions = {
      makeRecurrentConstTableLookup(Arch::AArch64)};
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit(Functions, Context, "macho-repeated-phi-edge",
                             Arch::AArch64, {}, &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  const PhiNode &Phi = Functions.front().Blocks[1].Phis.front();
  MedLLVMProvenanceTestPeer::resetAddressProvenanceWork(Emitter);
  for (unsigned I = 0; I < 64; ++I)
    EXPECT_TRUE(MedLLVMProvenanceTestPeer::edgeIsProven(Emitter, Phi, 2));
  const auto Work = MedLLVMProvenanceTestPeer::addressProvenanceWork(Emitter);
  EXPECT_LE(std::get<0>(Work), 1U)
      << "an exact PHI/predecessor pair must be classified once";
}

TEST(MachOLLVMDataPointerBoundary, ReusesCompletedPhiRecurrenceProof) {
  BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
  std::vector<MedFunc> Functions = {
      makeRecurrentConstTableLookup(Arch::AArch64)};
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit(Functions, Context, "macho-repeated-recurrence",
                             Arch::AArch64, {}, &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  const PhiNode &Phi = Functions.front().Blocks[1].Phis.front();
  const MedVar &BackedgeValue = Phi.Args[1].second;
  MedLLVMProvenanceTestPeer::resetAddressProvenanceWork(Emitter);
  for (unsigned I = 0; I < 64; ++I)
    EXPECT_TRUE(MedLLVMProvenanceTestPeer::incomingIsRecurrent(Emitter, Phi, 2,
                                                               BackedgeValue));
  const auto Work = MedLLVMProvenanceTestPeer::addressProvenanceWork(Emitter);
  EXPECT_LE(std::get<1>(Work), 1U)
      << "a completed recurrence proof must be reused";
}

TEST(MachOLLVMDataPointerBoundary,
     ReusesStableOffsetProofWithoutMergingForbiddenKeys) {
  BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
  std::vector<MedFunc> Functions = {
      makeRecurrentConstTableLookup(Arch::AArch64)};
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit(Functions, Context, "macho-repeated-offset-proof",
                             Arch::AArch64, {}, &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  const MedVar &ScaledIndex = Functions.front().Blocks[1].Ops[0].Output;
  MedLLVMProvenanceTestPeer::resetAddressProvenanceWork(Emitter);
  for (unsigned I = 0; I < 64; ++I) {
    EXPECT_TRUE(
        MedLLVMProvenanceTestPeer::stableOffset(Emitter, ScaledIndex, nullptr));
    EXPECT_FALSE(MedLLVMProvenanceTestPeer::stableOffset(Emitter, ScaledIndex,
                                                         &ScaledIndex));
  }
  const auto Work = MedLLVMProvenanceTestPeer::addressProvenanceWork(Emitter);
  EXPECT_LE(std::get<2>(Work), 2U)
      << "stable-offset cache keys must preserve the optional forbidden value";
}

TEST(MachOLLVMDataPointerBoundary,
     ClearsProvenanceCachesAcrossFunctionsAndEmitterReuse) {
  {
    BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
    addThresholdCrossingConstTableRun(Image);
    MedFunc First =
        makeThreeWayPhiConstTableLookup(Arch::AArch64, /*ScalarArmDead=*/true);
    MedFunc Second = First;
    Second.Name += "_low_rebased";
    PhiNode &SecondPhi = Second.Blocks[5].Phis.front();
    SecondPhi.Args[0].second = MedVar::makeConst(LowSpilledConstTableVA, 8);
    SecondPhi.Args[1].second =
        MedVar::makeConst(OtherLowSpilledConstTableVA, 8);
    rebaseFunction(Second, 0x100);
    std::vector<MedFunc> Functions{std::move(First), std::move(Second)};

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        Functions, Context, "macho-two-provenance-generations", Arch::AArch64,
        {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    for (const MedFunc &Func : Functions) {
      llvm::Function *LLVMFunc = Module->getFunction(Func.Name);
      ASSERT_NE(LLVMFunc, nullptr);
      const llvm::LoadInst *TableLoad = findVolatileI16Load(*LLVMFunc);
      ASSERT_NE(TableLoad, nullptr);
      EXPECT_TRUE(
          llvm::isa<llvm::GetElementPtrInst>(TableLoad->getPointerOperand()));
      std::set<const llvm::Value *> Seen;
      EXPECT_TRUE(
          valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
    }
    llvm::GlobalVariable *LowRun =
        Module->getNamedGlobal("__nd_data_800.rodata");
    ASSERT_NE(LowRun, nullptr);
    llvm::Function *SecondFunction = Module->getFunction(Functions[1].Name);
    ASSERT_NE(SecondFunction, nullptr);
    const llvm::LoadInst *SecondLoad = findVolatileI16Load(*SecondFunction);
    ASSERT_NE(SecondLoad, nullptr);
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesTarget(SecondLoad->getPointerOperand(), LowRun, Seen));
  }

  BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64);
  std::vector<MedFunc> Functions = {
      makeThreeWayPhiConstTableLookup(Arch::AArch64,
                                      /*ScalarArmDead=*/true)};
  MedLLVMEmitter Emitter;
  {
    llvm::LLVMContext FirstContext;
    auto FirstModule = Emitter.emit(
        Functions, FirstContext, "macho-reused-emitter-provenance-first",
        Arch::AArch64, {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(FirstModule, nullptr);
    expectValidModule(*FirstModule);
  }

  MedOp *Gate = nullptr;
  for (MedOp &Op : Functions.front().Blocks.front().Ops)
    if (Op.Opcode == NdOp::COND_BR) {
      Gate = &Op;
      break;
    }
  ASSERT_NE(Gate, nullptr);
  ASSERT_GE(Gate->NumInputs, 2);
  Gate->Inputs[1] = Functions.front().Params[1];

  llvm::LLVMContext SecondContext;
  testing::internal::CaptureStderr();
  auto SecondModule = Emitter.emit(
      Functions, SecondContext, "macho-reused-emitter-provenance-second",
      Arch::AArch64, {}, &Image, BinaryFormat::MachO);
  std::string Diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_EQ(SecondModule, nullptr);
  EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
            std::string::npos)
      << Diagnostic;
}

TEST(MachOLLVMDataPointerBoundary,
     KeepsRebuiltPointerTableLoadOnSymbolizedInductionPath) {
  BinaryImage Image = makeLLVMImage();
  Image.DataPtrRelocSlots.erase(DataVA + 24);
  MedFunc Lookup = makeRecurrentAbsolutePointerTableLoad();
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Lookup}, Context, "macho-recurrent-pointer-table-load", Arch::AArch64,
      {}, &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::Function *Function = Module->getFunction(Lookup.Name);
  ASSERT_NE(Function, nullptr);
  const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
  ASSERT_NE(TableLoad, nullptr);
  EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "indrawptr");
  const std::string MirrorName =
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str();
  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(MirrorName);
  ASSERT_NE(Mirror, nullptr);
  EXPECT_TRUE(Mirror->hasInitializer());
}

TEST(MachOLLVMDataPointerBoundary,
     GenericPhiAuditDefersPureRelocatedPointerTableSlotInduction) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeLLVMImage();
    Image.Arch = TargetArch;
    ASSERT_EQ(Image.Segments[1].Name, section_names::macho::DataConstSeg);
    ASSERT_TRUE(Image.Segments[1].isWritable());
    writeObject(Image.Segments[1].Data, /*Off=*/40, CStringBVA);
    Image.DataPtrRelocSlots.clear();
    Image.DataPtrRelocSlots.insert(DataVA + 16);
    Image.DataPtrRelocSlots.insert(DataVA + 40);

    MedFunc Lookup = makeRecurrentPointerTableSlotWalk(TargetArch);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-recurrent-pointer-table-slot-walk",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *PointerLoad = nullptr;
    for (const llvm::BasicBlock &Block : *Function)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
            Load && Load->isVolatile() &&
            Load->getType()->isIntegerTy(
                getTargetRegInfo(TargetArch).PointerSize * 8)) {
          EXPECT_EQ(PointerLoad, nullptr);
          PointerLoad = Load;
        }
    ASSERT_NE(PointerLoad, nullptr);
    EXPECT_EQ(PointerLoad->getPointerOperand()->getName(), "indrawptr");
    const std::string MirrorName =
        (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str();
    llvm::GlobalVariable *Mirror = Module->getNamedGlobal(MirrorName);
    ASSERT_NE(Mirror, nullptr);
    bool FunctionReferencesMirror = false;
    bool RetainsRawSlotVA = false;
    for (const llvm::BasicBlock &Block : *Function)
      for (const llvm::Instruction &Instruction : Block) {
        std::set<const llvm::Value *> Seen;
        FunctionReferencesMirror |=
            valueReferencesTarget(&Instruction, Mirror, Seen);
        for (const llvm::Value *Operand : Instruction.operands())
          if (const auto *C = llvm::dyn_cast<llvm::Constant>(Operand))
            RetainsRawSlotVA |= constantContainsInteger(C, DataVA + 16);
      }
    EXPECT_TRUE(FunctionReferencesMirror);
    EXPECT_FALSE(RetainsRawSlotVA);
    EXPECT_EQ(
        Module->getNamedGlobal((kNdDataPrefix + llvm::utohexstr(DataVA)).str() +
                               section_names::elf::Rodata),
        nullptr);
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForUnmaterializableRelocationProvenTableTarget) {
  constexpr uint64_t Mask32 = 0xffffffffULL;
  BinaryImage Image = makeLLVMImage();
  addMachOPageZero(Image);
  writeObject(Image.Segments[1].Data, /*Off=*/16, Mask32);
  Image.DataPtrRelocSlots.erase(DataVA + 24);

  MedFunc Lookup = makeRecurrentAbsolutePointerTableLoad();
  llvm::LLVMContext Context;
  testing::internal::CaptureStderr();
  auto Module = MedLLVMEmitter().emit(
      {Lookup}, Context, "macho-unmaterializable-pointer-table-target",
      Arch::AArch64, {}, &Image, BinaryFormat::MachO);
  std::string Diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_EQ(Module, nullptr);
  EXPECT_NE(Diagnostic.find("relocation-proven data pointer"),
            std::string::npos)
      << Diagnostic;
  EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
            std::string::npos)
      << Diagnostic;
}

TEST(MachOLLVMDataPointerBoundary, PreservesNullRelocationProvenTableTarget) {
  BinaryImage Image = makeLLVMImage();
  const uint64_t NullTarget = 0;
  writeObject(Image.Segments[1].Data, /*Off=*/16, NullTarget);
  Image.DataPtrRelocSlots.erase(DataVA + 24);

  MedFunc Lookup = makeRecurrentAbsolutePointerTableLoad();
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Lookup}, Context, "macho-null-pointer-table-target", Arch::AArch64, {},
      &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);
  const std::string MirrorName =
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str();
  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(MirrorName);
  ASSERT_NE(Mirror, nullptr);
  ASSERT_TRUE(Mirror->hasInitializer());
  const auto *Initializer =
      llvm::dyn_cast<llvm::ConstantStruct>(Mirror->getInitializer());
  ASSERT_NE(Initializer, nullptr);
  ASSERT_GE(Initializer->getNumOperands(), 3u);
  const auto *NullField =
      llvm::dyn_cast<llvm::ConstantInt>(Initializer->getOperand(2));
  ASSERT_NE(NullField, nullptr);
  EXPECT_TRUE(NullField->isZero());
}

TEST(MachOLLVMDataPointerBoundary,
     KeepsOffsetRebuiltPointerTableLoadOnSymbolizedInductionPath) {
  BinaryImage Image = makeLLVMImage();
  Image.Segments[1].Flags = SegmentFlags::Readable;
  Image.DataPtrRelocSlots.erase(DataVA + 24);
  MedFunc Lookup = makeRecurrentAbsolutePointerTableLoad();
  MedVar LoadedBase = Lookup.Blocks.front().Ops.front().Output;
  MedVar OffsetBase = LoadedBase;
  OffsetBase.Id = 41;
  MedOp AddOffset;
  AddOffset.Opcode = NdOp::INT_ADD;
  AddOffset.Output = OffsetBase;
  AddOffset.addInput(LoadedBase);
  AddOffset.addInput(MedVar::makeConst(1, LoadedBase.Size));
  Lookup.Blocks.front().Ops.insert(std::next(Lookup.Blocks.front().Ops.begin()),
                                   std::move(AddOffset));
  Lookup.Blocks[1].Phis.front().Args.front().second = OffsetBase;

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Lookup}, Context, "macho-offset-pointer-table-load", Arch::AArch64, {},
      &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::Function *Function = Module->getFunction(Lookup.Name);
  ASSERT_NE(Function, nullptr);
  const llvm::LoadInst *TableLoad = findNonAllocaI16Load(*Function);
  ASSERT_NE(TableLoad, nullptr);
  EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "indrawptr");
}

TEST(MachOLLVMDataPointerBoundary,
     DoesNotTreatControlOrAnnihilatingUseAsPointerRecurrence) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (FalseRecurrenceCase Case : {FalseRecurrenceCase::ControlOnlySelect,
                                     FalseRecurrenceCase::AnnihilatingMultiply,
                                     FalseRecurrenceCase::IndependentTableAdd,
                                     FalseRecurrenceCase::DuplicatePointerAdd,
                                     FalseRecurrenceCase::TableSubtrahend,
                                     FalseRecurrenceCase::UnknownNestedPhiArm,
                                     FalseRecurrenceCase::LoadedOffset}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(static_cast<int>(Case));
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      MedFunc Lookup = makeFalseRecurrentTableLookup(TargetArch, Case);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-false-pointer-recurrence", TargetArch, {},
          &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary,
     KeepsMutualSubregisterRecurrentPhiOnInductionResolverPath) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeNestedRecurrentPhiTableLookup(
        TargetArch, NestedRecurrentPhiCase::MutualSubregisterAlias);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-mutual-alias-recurrent-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "indptr");
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedWhenReachablePhiArmOnlyContainsUnrelatedCycle) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeNestedRecurrentPhiTableLookup(
        TargetArch, NestedRecurrentPhiCase::UnrelatedReachableCycle);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-unrelated-recurrent-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module, nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     IgnoresStaticallyDeadUnrelatedCycleInPhiArm) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeNestedRecurrentPhiTableLookup(
        TargetArch, NestedRecurrentPhiCase::UnrelatedDeadCycle);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-dead-unrelated-recurrent-table", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    // With executable read-only data mirrored like direct global data, the
    // surviving entry-only PHI is owned by the all-arms merge resolver.
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "selmrgptr");
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForReachableScalarArmInMultiTablePhi) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Lookup = makeThreeWayPhiConstTableLookup(TargetArch, false);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-three-way-ambiguous-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module, nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     ResolvesMultiTablePhiWhenScalarArmIsStaticallyDead) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Lookup = makeThreeWayPhiConstTableLookup(TargetArch, true);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-three-way-dead-scalar-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "selmrgptr");
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForReachableScalarInDirectPhiAddress) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (bool WrapInCopy : {false, true}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(WrapInCopy ? "copy-wrapped" : "bare");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      MedFunc Lookup = makeDirectPhiTableLookup(TargetArch, WrapInCopy, false);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-direct-ambiguous-phi-table", TargetArch, {},
          &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary, IgnoresDeadTableArmInDirectPhiAddress) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeDirectPhiTableLookup(TargetArch, /*WrapInCopy=*/true,
                                              /*TableArmDead=*/true);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Lookup}, Context, "macho-direct-dead-table-phi",
                              TargetArch, {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = nullptr;
    for (const llvm::BasicBlock &Block : *Function)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
            Load && Load->getType()->isIntegerTy(16) &&
            !llvm::isa<llvm::AllocaInst>(
                Load->getPointerOperand()->stripPointerCasts())) {
          ASSERT_EQ(TableLoad, nullptr);
          TableLoad = Load;
        }
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "memptr");
    std::set<const llvm::Value *> Seen;
    EXPECT_FALSE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForMixedRawAndSymbolizedRecurrentPhiBases) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (bool RawArmFirst : {false, true}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(RawArmFirst ? "raw first" : "symbol first");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      Image.Segments.front().Flags = SegmentFlags::Readable;
      MedFunc Lookup =
          makeMixedRepresentationRecurrentTableLookup(TargetArch, RawArmFirst);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-mixed-recurrent-table", TargetArch, {},
          &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedWhenLiteralOffsetHidesSymbolizedRecurrentBase) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    const uint64_t Offset =
        uint64_t(0) - (OtherSpilledConstTableVA - SpilledConstTableVA);
    writeObject(Image.Segments.front().Data,
                SpilledConstTableVA + 0x30 - Image.Segments.front().VA, Offset);
    MedFunc Lookup = makeLiteralOffsetMixedRecurrentTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-literal-offset-mixed-recurrent", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    std::string IR;
    if (Module) {
      llvm::raw_string_ostream Stream(IR);
      Module->print(Stream, nullptr);
      Stream.flush();
    }
    EXPECT_EQ(Module.get(), nullptr) << IR;
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     ClassifiesRecurrentInitializationByEmittedAddressModel) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    auto expectAnchored = [&](MedFunc Lookup, llvm::StringRef ModuleName) {
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      addThresholdCrossingConstTableRun(Image);
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Lookup}, Context, ModuleName.str(), TargetArch,
                                {}, &Image, BinaryFormat::MachO);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);
      llvm::Function *Function = Module->getFunction(Lookup.Name);
      ASSERT_NE(Function, nullptr);
      const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
      ASSERT_NE(TableLoad, nullptr);
      EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "indptr");
    };

    for (bool RawArmFirst : {false, true}) {
      SCOPED_TRACE(RawArmFirst ? "raw first" : "computed first");
      MedFunc SameRawModel =
          makeMixedRepresentationRecurrentTableLookup(TargetArch, RawArmFirst);
      replaceMedConstant(SameRawModel, SpilledConstTableVA,
                         LowSpilledConstTableVA);
      expectAnchored(std::move(SameRawModel),
                     "macho-low-raw-computed-recurrent");
    }

    expectAnchored(
        makeDualComputedRecurrentTableLookup(TargetArch, LowSpilledConstTableVA,
                                             OtherLowSpilledConstTableVA),
        "macho-low-dual-computed-recurrent");

    BinaryImage MixedImage = makeSpilledConstTableImage(TargetArch);
    addThresholdCrossingConstTableRun(MixedImage);
    MedFunc MixedComputed = makeDualComputedRecurrentTableLookup(
        TargetArch, LowSpilledConstTableVA, SymbolizedSameRunTableVA);
    llvm::LLVMContext MixedContext;
    testing::internal::CaptureStderr();
    auto MixedModule = MedLLVMEmitter().emit(
        {MixedComputed}, MixedContext, "macho-mixed-computed-recurrent",
        TargetArch, {}, &MixedImage, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(MixedModule.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForTwoBaseArithmeticInMultiTablePhi) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (NdOp Opcode :
         {NdOp::INT_ADD, NdOp::INT_SUB, NdOp::INT_AND, NdOp::INT_OR}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(static_cast<int>(Opcode));
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      Image.Segments.front().Flags = SegmentFlags::Readable;
      MedFunc Lookup = makeTwoBaseArithmeticPhiTableLookup(TargetArch, Opcode);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-two-base-arithmetic-table", TargetArch, {},
          &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary, MalformedSelfEdgeCannotProvePhiRecurrence) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeMalformedSelfRecurrentPhiTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-malformed-recurrent-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForInvalidTablePointerProvenanceExpressions) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    auto expectRefused = [&](MedFunc Lookup, llvm::StringRef ModuleName,
                             bool AddLowRun = false) {
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      Image.Segments.front().Flags = SegmentFlags::Readable;
      if (AddLowRun)
        addThresholdCrossingConstTableRun(Image);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module =
          MedLLVMEmitter().emit({Lookup}, Context, ModuleName.str(), TargetArch,
                                {}, &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    };

    for (InvalidPhiPointerExprCase Case :
         {InvalidPhiPointerExprCase::StandaloneAnd,
          InvalidPhiPointerExprCase::DirectSecondBase,
          InvalidPhiPointerExprCase::PureArithmeticSameResult}) {
      SCOPED_TRACE(static_cast<int>(Case));
      expectRefused(makePhiPointerExpressionLookup(TargetArch, Case),
                    "macho-invalid-phi-pointer-expression",
                    Case ==
                        InvalidPhiPointerExprCase::PureArithmeticSameResult);
    }
    for (NdOp Opcode : {NdOp::INT_ADD, NdOp::INT_SUB}) {
      SCOPED_TRACE(static_cast<int>(Opcode));
      {
        SCOPED_TRACE("independent pointer phis");
        expectRefused(
            makeIndependentPointerPhiAddressLookup(TargetArch, Opcode),
            "macho-independent-pointer-phis");
      }
      {
        SCOPED_TRACE("truncating pointer arithmetic");
        expectRefused(makeTruncatingPointerExpressionLookup(
                          TargetArch, /*TruncatePhi=*/false, Opcode),
                      "macho-truncating-pointer-arithmetic");
      }
    }
    expectRefused(
        makeTruncatingPointerExpressionLookup(TargetArch, /*TruncatePhi=*/true),
        "macho-truncating-pointer-phi");
    expectRefused(makeNoPhiMaskedArithmeticSelection(TargetArch),
                  "macho-no-phi-masked-arithmetic-selection",
                  /*AddLowRun=*/true);
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForRecurrentPhiPlusLoaderProvenSecondBase) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.RelocDataAddrs.insert(OtherSpilledConstTableVA);
    MedFunc Lookup = makeRecurrentConstTableLookup(TargetArch);
    MedBlock &Loop = Lookup.Blocks[1];
    const MedVar PhiBase = Loop.Phis.front().Output;
    bool Replaced = false;
    for (MedOp &Op : Loop.Ops)
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2 &&
          !Op.Inputs[0].isConst() && Op.Inputs[0].Kind == PhiBase.Kind &&
          Op.Inputs[0].Id == PhiBase.Id &&
          Op.Inputs[0].SSAVer == PhiBase.SSAVer) {
        Op.Inputs[1] =
            MedVar::makeConst(OtherSpilledConstTableVA, PhiBase.Size);
        Replaced = true;
        break;
      }
    ASSERT_TRUE(Replaced);

    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-recurrent-plus-second-base", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedBeforePointerTableResolverForRecurrentSecondBase) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Segment PointerTable;
    PointerTable.Name = "__DATA_CONST";
    PointerTable.VA = FarSpilledConstTableVA;
    PointerTable.Size = 0x20;
    PointerTable.FileSz = PointerTable.Size;
    PointerTable.Flags = SegmentFlags::Readable;
    PointerTable.Data.assign(PointerTable.Size, 0);
    writeObject(PointerTable.Data, 0, SpilledConstTableVA);
    Image.Segments.push_back(std::move(PointerTable));
    Image.DataPtrRelocSlots.insert(FarSpilledConstTableVA);

    MedFunc Lookup = makeRecurrentConstTableLookup(TargetArch);
    MedBlock &Loop = Lookup.Blocks[1];
    const MedVar PhiBase = Loop.Phis.front().Output;
    bool Replaced = false;
    for (MedOp &Op : Loop.Ops)
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2 &&
          !Op.Inputs[0].isConst() && Op.Inputs[0].Kind == PhiBase.Kind &&
          Op.Inputs[0].Id == PhiBase.Id &&
          Op.Inputs[0].SSAVer == PhiBase.SSAVer) {
        Op.Inputs[1] = MedVar::makeConst(FarSpilledConstTableVA, PhiBase.Size);
        Replaced = true;
        break;
      }
    ASSERT_TRUE(Replaced);

    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-pointer-table-second-base", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary, KeepsProvenTableBasePlusNumericPhiOffset) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    addThresholdCrossingConstTableRun(Image);
    // Relocatable objects commonly place .text at VA zero.  The numeric +4
    // offset must stay an integer even though its value lands in that mapped
    // executable segment; only materialized code-pointer provenance would make
    // it a second address base.
    Segment LowText;
    LowText.Name = "__LOW_TEXT";
    LowText.VA = 0;
    LowText.Size = 0x100;
    LowText.FileSz = LowText.Size;
    LowText.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    LowText.Data.assign(LowText.Size, 0);
    Image.Segments.push_back(std::move(LowText));
    MedFunc Lookup = makePhiPointerExpressionLookup(
        TargetArch, InvalidPhiPointerExprCase::NumericOffset);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-table-base-numeric-phi-offset", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "selmrgptr");
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedWhenTablePointersOccupyMaskedOrMaskRoles) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Lookup = makeMisassignedMaskedOrPhiTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module =
        MedLLVMEmitter().emit({Lookup}, Context, "macho-misassigned-masked-or",
                              TargetArch, {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     DoesNotTreatFoldedScalarAddAsTableProvenance) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (bool WrapInCopy : {false, true}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(WrapInCopy ? "copy-wrapped" : "direct");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      Image.Segments.front().Flags = SegmentFlags::Readable;
      MedFunc Lookup =
          makeFoldedScalarAddPhiTableLookup(TargetArch, WrapInCopy);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-folded-scalar-add-phi", TargetArch, {},
          &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary,
     DoesNotClassifyUniformFoldedScalarAddsAsSymbolizedPointers) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Lookup = makeUniformFoldedScalarAddPhiTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-uniform-folded-scalar-add-phi", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    std::string IR;
    if (Module) {
      llvm::raw_string_ostream Stream(IR);
      Module->print(Stream, nullptr);
    }
    EXPECT_EQ(Module.get(), nullptr) << IR;
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     UsesCurrentPointerForUniformComputedMultiTablePhi) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Lookup = makeComputedModelPhiTableLookup(TargetArch, false);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-computed-model-multi-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "selmrgrawptr");
  }
}

TEST(MachOLLVMDataPointerBoundary,
     UsesCurrentPointerForUniformComputedSingleBasePhi) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Lookup = makeComputedModelPhiTableLookup(
        TargetArch, /*MixedModel=*/false, /*SameBase=*/true);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-computed-model-single-base", TargetArch, {},
        &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "selmrgrawptr");
  }
}

TEST(MachOLLVMDataPointerBoundary,
     UsesLeafWidthForForwardedMultiTablePhiAddressModel) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    addThresholdCrossingConstTableRun(Image);
    MedFunc Lookup = makeNarrowForwardedPhiTableLookup(TargetArch);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-narrow-forwarded-multi-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findNonAllocaI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "selmrgptr");
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForMixedRawAndComputedMultiTablePhi) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Lookup = makeComputedModelPhiTableLookup(TargetArch, true);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-mixed-model-multi-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForNestedAmbiguousPhiInSelectAddress) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeNestedAmbiguousPhiSelectTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-nested-ambiguous-phi-select", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForTwoIndependentRecurrentPointerBases) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (NdOp Opcode : {NdOp::INT_ADD, NdOp::INT_SUB}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(static_cast<int>(Opcode));
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      Image.Segments.front().Flags = SegmentFlags::Readable;
      MedFunc Lookup = makeTwoRecurrentPointerTableLookup(TargetArch, Opcode);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-two-recurrent-pointer-bases", TargetArch,
          {}, &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary, AuditsEveryAliasRecurrenceInitialization) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (bool DifferentConst : {false, true}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(DifferentConst ? "different" : "scalar");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      MedFunc Lookup =
          makeAmbiguousAliasSiblingTableLookup(TargetArch, DifferentConst);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-ambiguous-alias-sibling", TargetArch, {},
          &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary,
     RequiresMultiRawRecurrentBasesToShareRelocatableRun) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (bool AlternateFirst : {false, true}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(AlternateFirst ? "alternate first" : "primary first");

      BinaryImage CrossRunImage = makeSpilledConstTableImage(TargetArch);
      CrossRunImage.Segments.front().Flags = SegmentFlags::Readable;
      addFarSpilledConstTable(CrossRunImage);
      MedFunc CrossRun = makeMultiRawInitRecurrentTableLookup(
          TargetArch, /*CrossRun=*/true, AlternateFirst);
      llvm::LLVMContext CrossRunContext;
      testing::internal::CaptureStderr();
      auto CrossRunModule = MedLLVMEmitter().emit(
          {CrossRun}, CrossRunContext, "macho-cross-run-recurrent-bases",
          TargetArch, {}, &CrossRunImage, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(CrossRunModule.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;

      BinaryImage SameRunImage = makeSpilledConstTableImage(TargetArch);
      SameRunImage.Segments.front().Flags = SegmentFlags::Readable;
      MedFunc SameRun = makeMultiRawInitRecurrentTableLookup(
          TargetArch, /*CrossRun=*/false, AlternateFirst);
      llvm::LLVMContext SameRunContext;
      auto SameRunModule = MedLLVMEmitter().emit(
          {SameRun}, SameRunContext, "macho-same-run-recurrent-bases",
          TargetArch, {}, &SameRunImage, BinaryFormat::MachO);
      ASSERT_NE(SameRunModule, nullptr);
      expectValidModule(*SameRunModule);
      llvm::Function *Function = SameRunModule->getFunction(SameRun.Name);
      ASSERT_NE(Function, nullptr);
      const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
      ASSERT_NE(TableLoad, nullptr);
      EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "indptr");
    }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForScalarArmInNoPhiPointerSelection) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (bool Masked : {false, true}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(Masked ? "masked" : "select");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      addThresholdCrossingConstTableRun(Image);
      MedFunc Lookup = makeNoPhiSelectedTableLookup(TargetArch, Masked);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-no-phi-scalar-selection", TargetArch, {},
          &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForTableShapedInvalidRecurrentInitialization) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    addThresholdCrossingConstTableRun(Image);
    MedFunc Lookup = makeInvalidFoldedRecurrentTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-invalid-folded-recurrent-init", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedWhenNoPhiSelectionHasOnlyInvalidTableShapedArms) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (bool WithRuntimeIndex : {false, true}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(WithRuntimeIndex ? "indexed" : "direct");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      addThresholdCrossingConstTableRun(Image);
      MedFunc Lookup =
          makeNoPhiInvalidFoldedSelection(TargetArch, WithRuntimeIndex);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-no-phi-invalid-folded-selection",
          TargetArch, {}, &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary,
     RejectsTruncatingCopyAsSelectedTableProvenance) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    addThresholdCrossingConstTableRun(Image);
    MedFunc Lookup = makeTruncatingCopySelectTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-truncating-copy-table-selection", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary, RejectsTruncatingSelectAsLoaderTableOffset) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    Image.RelocDataAddrs.insert(SpilledConstTableVA);
    MedFunc Lookup = makeLoaderBasePlusTruncatingSelectOffsetLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-loader-base-truncating-select-offset",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedBeforePointerTableSelectForScalarArm) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    addThresholdCrossingConstTableRun(Image);
    auto LowRun = std::find_if(
        Image.Segments.begin(), Image.Segments.end(), [](const Segment &Seg) {
          return LowSpilledConstTableVA >= Seg.VA &&
                 LowSpilledConstTableVA < Seg.VA + Seg.Data.size();
        });
    ASSERT_NE(LowRun, Image.Segments.end());
    writeObject(LowRun->Data, LowSpilledConstTableVA - LowRun->VA,
                SpilledConstTableVA);
    Image.DataPtrRelocSlots.insert(LowSpilledConstTableVA);
    MedFunc Lookup = makeNoPhiSelectedTableLookup(TargetArch, /*Masked=*/false);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-pointer-table-select-scalar-arm", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RejectsPointerTableEvidenceOutsidePointerValueRoles) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (bool ControlOnly : {false, true}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(ControlOnly ? "select condition" : "multiply by zero");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      Image.Segments.front().Flags = SegmentFlags::Readable;
      addThresholdCrossingConstTableRun(Image);
      auto LowRun = std::find_if(
          Image.Segments.begin(), Image.Segments.end(), [](const Segment &Seg) {
            return LowSpilledConstTableVA >= Seg.VA &&
                   LowSpilledConstTableVA < Seg.VA + Seg.Data.size();
          });
      ASSERT_NE(LowRun, Image.Segments.end());
      writeObject(LowRun->Data, LowSpilledConstTableVA - LowRun->VA,
                  SpilledConstTableVA);
      Image.DataPtrRelocSlots.insert(LowSpilledConstTableVA);
      MedFunc Lookup =
          makePointerTableNonValueRoleLookup(TargetArch, ControlOnly);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-pointer-table-non-value-role", TargetArch,
          {}, &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      std::string IR;
      if (Module) {
        llvm::raw_string_ostream Stream(IR);
        Module->print(Stream, nullptr);
      }
      EXPECT_EQ(Module.get(), nullptr) << IR;
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary,
     ClassifiesActualLeavesOfFoldedHighPointerTableAddress) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    addThresholdCrossingConstTableRun(Image);
    auto LowRun = std::find_if(
        Image.Segments.begin(), Image.Segments.end(), [](const Segment &Seg) {
          return SymbolizedSameRunTableVA >= Seg.VA &&
                 SymbolizedSameRunTableVA < Seg.VA + Seg.Data.size();
        });
    ASSERT_NE(LowRun, Image.Segments.end());
    writeObject(LowRun->Data, SymbolizedSameRunTableVA - LowRun->VA,
                SpilledConstTableVA);
    Image.DataPtrRelocSlots.insert(SymbolizedSameRunTableVA);

    MedFunc Lookup = makeFoldedHighPointerTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-folded-high-pointer-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    ASSERT_NE(Module, nullptr) << Diagnostic;
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findNonAllocaI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    const std::string MirrorName =
        (kNdCodePtrPrefix + llvm::utohexstr(0x800)).str();
    llvm::GlobalVariable *Mirror = Module->getNamedGlobal(MirrorName);
    ASSERT_NE(Mirror, nullptr);
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesTarget(TableLoad->getPointerOperand(), Mirror, Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     ResolvesAdjacentPointerTableSegmentsThroughOneRelocationMirror) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    addAdjacentLowPointerTableSegments(Image);
    MedFunc Lookup = makeNoPhiSelectedTableLookup(TargetArch, /*Masked=*/false);
    MedOp *Select = nullptr;
    for (MedOp &Op : Lookup.Blocks.front().Ops)
      if (Op.Opcode == NdOp::SELECT) {
        Select = &Op;
        break;
      }
    ASSERT_NE(Select, nullptr);
    Select->Inputs[1] = MedVar::makeConst(
        LowSpilledConstTableVA, getTargetRegInfo(TargetArch).PointerSize);
    Select->Inputs[2] = MedVar::makeConst(
        OtherLowSpilledConstTableVA, getTargetRegInfo(TargetArch).PointerSize);

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-adjacent-distinct-pointer-table-segments",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "cptsel");
    EXPECT_NE(Module->getNamedGlobal("__nd_codeptr_880"), nullptr);
  }
}

TEST(MachOLLVMDataPointerBoundary,
     ResolvesDeepForwardingChainWithoutStaleAddressFallback) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    addThresholdCrossingConstTableRun(Image);
    MedFunc Lookup = makeDeepForwardedLowTableLookup(TargetArch);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-deep-forwarded-low-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    llvm::GlobalVariable *Rodata =
        Module->getNamedGlobal("__nd_data_800.rodata");
    ASSERT_NE(Rodata, nullptr);
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedWhenArithmeticDepthHidesRawTableBase) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    addThresholdCrossingConstTableRun(Image);
    MedFunc Lookup = makeDeepArithmeticLowTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-deep-arithmetic-low-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    std::string IR;
    if (Module) {
      llvm::raw_string_ostream Stream(IR);
      Module->print(Stream, nullptr);
    }
    EXPECT_EQ(Module.get(), nullptr) << IR;
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     DoesNotFallBackWhenDynamicArithmeticDepthHidesRawTableBase) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    addThresholdCrossingConstTableRun(Image);
    MedFunc Lookup = makeDeepDynamicArithmeticLowTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-deep-dynamic-arithmetic-low-table",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    if (!Module) {
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
      continue;
    }
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    std::string IR;
    llvm::raw_string_ostream Stream(IR);
    Module->print(Stream, nullptr);
    ASSERT_NE(TableLoad, nullptr) << IR;
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(
        valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RelocatableControlConstantsCannotPrunePhiEdges) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    addThresholdCrossingConstTableRun(Image);
    MedFunc Lookup = makeRelocatableControlPhiTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-relocatable-control-phi", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RelocatableSubbytesOffsetCannotPrunePhiEdges) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Lookup = makeRelocatableSubbytesControlPhiTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-relocatable-subbytes-control", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RejectsNonPreservingForwardersAsTableProvenance) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (NonPreservingForwardCase Case :
         {NonPreservingForwardCase::Subbytes,
          NonPreservingForwardCase::SignExtend}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(static_cast<int>(Case));
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      if (Case == NonPreservingForwardCase::Subbytes)
        addThresholdCrossingConstTableRun(Image);
      else
        addSignBitConstTableRun(Image);
      MedFunc Lookup = makeNonPreservingForwardPhiTableLookup(TargetArch, Case);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-nonpreserving-table-forwarder", TargetArch,
          {}, &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary,
     ResolvesMultiTablePhiWithCommutedAddressAdd) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Lookup = makeThreeWayPhiConstTableLookup(TargetArch, true);
    Lookup.Name += "_commuted";
    for (MedOp &Op : Lookup.Blocks[5].Ops)
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2) {
        std::swap(Op.Inputs[0], Op.Inputs[1]);
        break;
      }
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-commuted-multi-table-add", TargetArch, {},
        &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "selmrgptr");
  }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForInvalidMaskedPointerBlendSemantics) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (InvalidMaskedBlendCase Case :
         {InvalidMaskedBlendCase::NonBooleanCondition,
          InvalidMaskedBlendCase::NarrowMask,
          InvalidMaskedBlendCase::NonBooleanBoolOp}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(static_cast<int>(Case));
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      addThresholdCrossingConstTableRun(Image);
      MedFunc Lookup = makeInvalidMaskedBlendTableLookup(TargetArch, Case);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-invalid-masked-pointer-blend", TargetArch,
          {}, &Image, BinaryFormat::MachO);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(MachOLLVMDataPointerBoundary,
     FailsClosedForMaskedOrWithOnlyOneTablePointerArm) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    Image.Segments.front().Flags = SegmentFlags::Readable;
    MedFunc Lookup = makeOneSidedMaskedOrPhiTableLookup(TargetArch);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-one-sided-masked-or-table", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(MachOLLVMDataPointerBoundary,
     MixedWidthControlFoldingMatchesEmitterBeforePruningPhiEdges) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (MixedWidthControlCase Case :
         {MixedWidthControlCase::Shift, MixedWidthControlCase::Select,
          MixedWidthControlCase::IntNot, MixedWidthControlCase::IntNeg2}) {
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(static_cast<int>(Case));
      BinaryImage Image = makeSpilledConstTableImage(TargetArch);
      MedFunc Lookup = makeMixedWidthControlPhiTableLookup(TargetArch, Case);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "macho-mixed-width-control-phi", TargetArch, {},
          &Image, BinaryFormat::MachO);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *Function = Module->getFunction(Lookup.Name);
      ASSERT_NE(Function, nullptr);
      const llvm::LoadInst *TableLoad = findNonAllocaI16Load(*Function);
      ASSERT_NE(TableLoad, nullptr);
      EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "memptr");
      std::set<const llvm::Value *> Seen;
      EXPECT_FALSE(
          valueReferencesConstantGlobal(TableLoad->getPointerOperand(), Seen));
    }
}

} // namespace
