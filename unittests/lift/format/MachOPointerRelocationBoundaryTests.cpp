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
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
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

  static bool edgeIsInfeasible(MedLLVMEmitter &Emitter, const PhiNode &Phi,
                               int PredId) {
    return Emitter.classifyPhiIncomingEdge(Phi, PredId) ==
           MedLLVMEmitter::PhiEdgeFeasibility::Infeasible;
  }

  static bool edgeIsUnknown(MedLLVMEmitter &Emitter, const PhiNode &Phi,
                            int PredId) {
    return Emitter.classifyPhiIncomingEdge(Phi, PredId) ==
           MedLLVMEmitter::PhiEdgeFeasibility::Unknown;
  }

  static bool incomingIsRecurrent(MedLLVMEmitter &Emitter, const PhiNode &Phi,
                                  int PredId, const MedVar &Arg) {
    return Emitter.phiIncomingIsRecurrent(Phi, PredId, Arg);
  }

  static bool selfRecurrent(MedLLVMEmitter &Emitter, const PhiNode &Phi) {
    return Emitter.phiIsSelfRecurrent(Phi);
  }

  static void prepareFreshAnalysis(MedLLVMEmitter &Emitter, const MedFunc &Func,
                                   const BinaryImage &Image, Arch TargetArch,
                                   BinaryFormat Format) {
    Emitter.Img = &Image;
    Emitter.TargetArch = TargetArch;
    Emitter.TargetFormat = Format;
    Emitter.CurMedFunc = &Func;
  }

  static bool recoversAbsoluteDataPointerIdentity(MedLLVMEmitter &Emitter,
                                                  const MedVar &Value) {
    std::set<MedLLVMEmitter::DataAddressIdentity> Identities;
    return Emitter.recoverAbsoluteDataPointerLoadIdentities(Value, Identities);
  }

  static bool buildingEdgeQueryIsProvisional(MedLLVMEmitter &Emitter,
                                             const PhiNode &Phi, int PredId) {
    Emitter.FeasibleEdgesFor = Emitter.CurMedFunc;
    Emitter.FeasibleEdgeState =
        MedLLVMEmitter::FeasibleEdgeCacheState::Building;
    Emitter.FeasibleEdgeBuildSawReentrantQuery = false;
    Emitter.PhiEdgeClassCache.clear();
    const auto Result = Emitter.classifyPhiIncomingEdge(Phi, PredId);
    const bool IsProvisional =
        Result == MedLLVMEmitter::PhiEdgeFeasibility::Unknown &&
        Emitter.PhiEdgeClassCache.count(std::make_pair(&Phi, PredId)) == 0 &&
        Emitter.FeasibleEdgeBuildSawReentrantQuery;
    Emitter.FeasibleEdgesFor = nullptr;
    Emitter.FeasibleEdgeState = MedLLVMEmitter::FeasibleEdgeCacheState::Empty;
    Emitter.FeasibleEdgeBuildSawReentrantQuery = false;
    Emitter.invalidateFeasibleEdgeDependentCaches();
    return IsProvisional;
  }

  static void installTransactionalReentryProbe(MedLLVMEmitter &Emitter,
                                               const PhiNode &Phi, int PredId,
                                               const MedVar &Arg,
                                               bool &ObservedProvisionalQuery) {
    Emitter.FeasibleEdgeBuildTestHook = [&Emitter, &Phi, PredId, Arg,
                                         &ObservedProvisionalQuery] {
      // Model every dependent memo as if an analysis reached it while the
      // feasible graph was still private and incomplete. Publication must
      // discard both owner and content, not merely the edge-class cache.
      const auto ArgKey = MedLLVMEmitter::addressProvenanceVarKey(Arg);
      Emitter.PhiRecurrenceCacheFor = Emitter.CurMedFunc;
      Emitter.PhiRecurrenceCache[std::make_tuple(&Phi, PredId, ArgKey)] = false;
      Emitter.SelfRecurrenceCacheFor = Emitter.CurMedFunc;
      Emitter.SelfRecurrenceCache[&Phi] = false;
      Emitter.StableOffsetCacheFor = Emitter.CurMedFunc;
      const MedLLVMEmitter::AddressProvenanceVarKey EmptyForbidden{};
      Emitter
          .StableOffsetCache[std::make_tuple(ArgKey, false, EmptyForbidden)] =
          false;
      Emitter.IndexedGlobalBaseCacheFor = Emitter.CurMedFunc;
      Emitter.IndexedGlobalBaseCache[ArgKey] = {};
      Emitter.InductionBasesFor = Emitter.CurMedFunc;
      Emitter.InductionBaseVAs.insert(0x1234);
      Emitter.PtrTableUniqueSegCache[std::make_tuple(
          int64_t{1}, static_cast<int>(MedVar::Temp), false)] = 1;
      Emitter.PointerTableLoadRoleCacheFor = Emitter.CurMedFunc;
      Emitter.PointerTableLoadRoleCache[ArgKey] = {};
      Emitter.WritableDataSegCache[std::make_tuple(
          static_cast<int>(Arg.Kind), Arg.Id, Arg.SSAVer, false)] = 0x1234;
      Emitter.FrameDerivedCacheFor = Emitter.CurMedFunc;
      Emitter.FrameDerivedCache[{1, 1}] = true;
      Emitter.FrameAddressCacheFor = Emitter.CurMedFunc;
      Emitter.FrameAddressCache[{2, 2}] = true;

      const auto Result = Emitter.classifyPhiIncomingEdge(Phi, PredId);
      ObservedProvisionalQuery =
          Result == MedLLVMEmitter::PhiEdgeFeasibility::Unknown &&
          Emitter.PhiEdgeClassCache.count(std::make_pair(&Phi, PredId)) == 0 &&
          Emitter.FeasibleEdgeBuildSawReentrantQuery;
    };
  }

  static bool
  feasibleDependentCachesAreInvalidated(const MedLLVMEmitter &Emitter) {
    return Emitter.PhiRecurrenceCacheFor == nullptr &&
           Emitter.PhiRecurrenceCache.empty() &&
           Emitter.SelfRecurrenceCacheFor == nullptr &&
           Emitter.SelfRecurrenceCache.empty() &&
           Emitter.StableOffsetCacheFor == nullptr &&
           Emitter.StableOffsetCache.empty() &&
           Emitter.IndexedGlobalBaseCacheFor == nullptr &&
           Emitter.IndexedGlobalBaseCache.empty() &&
           Emitter.InductionBasesFor == nullptr &&
           Emitter.InductionBaseVAs.empty() &&
           Emitter.PtrTableUniqueSegCache.empty() &&
           Emitter.PointerTableLoadRoleCacheFor == nullptr &&
           Emitter.PointerTableLoadRoleCache.empty() &&
           Emitter.WritableDataSegCache.empty() &&
           Emitter.FrameDerivedCacheFor == nullptr &&
           Emitter.FrameDerivedCache.empty() &&
           Emitter.FrameAddressCacheFor == nullptr &&
           Emitter.FrameAddressCache.empty();
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

  static bool materializableDataAddress(const MedLLVMEmitter &Emitter,
                                        const MedVar &Value) {
    uint64_t Address = 0;
    return Emitter.resolveMaterializableDataAddress(Value, Address);
  }

  static bool controlConstantMayRelocate(const MedLLVMEmitter &Emitter,
                                         const MedVar &Value) {
    return Emitter.controlConstantMayRelocate(Value);
  }

  static std::optional<uint64_t> writableDataSegment(MedLLVMEmitter &Emitter,
                                                     const MedVar &Value,
                                                     bool RequireRelocBase) {
    return Emitter.writableDataSegOf(Value, RequireRelocBase);
  }

  static bool sameAddressProvenanceKey(const MedVar &A, const MedVar &B) {
    return MedLLVMEmitter::addressProvenanceVarKey(A) ==
           MedLLVMEmitter::addressProvenanceVarKey(B);
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
constexpr va_t SameFunctionInteriorVA = CallerVA + 0x20;
constexpr va_t ZeroWritableOwnerVA = 0;
constexpr va_t AdjacentWritableOwnerVA = 0x20;
constexpr va_t WritableOwnerSize = 0x20;
constexpr va_t ReadOnlyOwnerAVA = 0x2000;
constexpr va_t ReadOnlyOwnerBVA = ReadOnlyOwnerAVA + 0x20;
constexpr va_t ReadOnlyOwnerSize = 0x20;

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

bool constantContainsIntegerImpl(const llvm::Constant *Root, uint64_t Value,
                                 std::set<const llvm::Constant *> &Seen) {
  if (!Root || !Seen.insert(Root).second)
    return false;
  if (const auto *Int = llvm::dyn_cast<llvm::ConstantInt>(Root))
    return Int->getZExtValue() == Value;
  for (const llvm::Use &Operand : Root->operands())
    if (const auto *Child = llvm::dyn_cast<llvm::Constant>(Operand.get()))
      if (constantContainsIntegerImpl(Child, Value, Seen))
        return true;
  return false;
}

bool constantContainsInteger(const llvm::Constant *Root, uint64_t Value) {
  std::set<const llvm::Constant *> Seen;
  return constantContainsIntegerImpl(Root, Value, Seen);
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

const char *formatTraceName(BinaryFormat Format) {
  switch (Format) {
  case BinaryFormat::MachO:
    return "Mach-O";
  case BinaryFormat::ELF:
    return "ELF";
  case BinaryFormat::COFF:
    return "COFF";
  default:
    return "unknown";
  }
}

BinaryImage
makeSpilledConstTableImage(Arch TargetArch,
                           BinaryFormat Format = BinaryFormat::MachO,
                           bool PointerTable = false) {
  BinaryImage Image;
  Image.Arch = TargetArch;
  Image.Format = Format;
  Image.Bits = Bitness::Bits64;
  Image.Base = TextVA;
  const bool IsMachO = Format == BinaryFormat::MachO;
  const bool TableInText = IsMachO && !PointerTable;
  const uint64_t FirstTarget = SpilledConstTableVA + 0x10;
  const uint64_t SecondTarget = SpilledConstTableVA + 0x30;

  auto fillTables = [&](std::vector<uint8_t> &Bytes, size_t BaseOffset) {
    const uint16_t Values[] = {10, 20, 30, 40};
    const uint16_t OtherValues[] = {50, 60, 70, 80};
    if (!PointerTable) {
      std::memcpy(Bytes.data() + BaseOffset, Values, sizeof(Values));
      std::memcpy(Bytes.data() + BaseOffset +
                      (OtherSpilledConstTableVA - SpilledConstTableVA),
                  OtherValues, sizeof(OtherValues));
      return;
    }
    writeObject(Bytes, BaseOffset, FirstTarget);
    writeObject(Bytes, BaseOffset + 8, FirstTarget);
    writeObject(Bytes,
                BaseOffset + (OtherSpilledConstTableVA - SpilledConstTableVA),
                SecondTarget);
    writeObject(Bytes,
                BaseOffset + (OtherSpilledConstTableVA - SpilledConstTableVA) +
                    8,
                SecondTarget);
  };

  Segment Text;
  Text.Name = IsMachO ? "__TEXT" : ".text";
  Text.VA = TextVA;
  Text.Size = TableInText ? 0x1000 : SpilledConstTableVA - TextVA;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  if (TableInText)
    fillTables(Text.Data, SpilledConstTableVA - TextVA);
  Image.Segments.push_back(std::move(Text));

  if (!TableInText) {
    Segment Data;
    Data.Name = IsMachO                       ? "__DATA_CONST"
                : Format == BinaryFormat::ELF ? ".data.rel.ro"
                                              : ".rdata";
    Data.VA = SpilledConstTableVA;
    Data.Size = 0x40;
    Data.FileSz = Data.Size;
    Data.Flags = SegmentFlags::Readable;
    if (IsMachO || Format == BinaryFormat::ELF)
      Data.Flags = Data.Flags | SegmentFlags::Writable;
    Data.Data.assign(Data.Size, 0);
    fillTables(Data.Data, 0);
    Image.Segments.push_back(std::move(Data));
  }

  Section Code;
  Code.Name = IsMachO ? "__text" : ".text";
  Code.SegmentName = IsMachO ? "__TEXT" : ".text";
  Code.VA = CallerVA;
  // Cover every synthetic instruction address used by this fixture. Mach-O
  // code identity is section-authoritative, so CodeVA and ImportStubVA must
  // not rely on the enclosing __TEXT segment's coarse execute permission.
  Code.Size = 0x140;
  Code.FileSz = Code.Size;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (IsMachO)
    Code.Type = static_cast<uint32_t>(S_REGULAR) |
                static_cast<uint32_t>(S_ATTR_PURE_INSTRUCTIONS) |
                static_cast<uint32_t>(S_ATTR_SOME_INSTRUCTIONS);
  Image.Sections.push_back(std::move(Code));

  Section Const;
  Const.Name = IsMachO                       ? "__const"
               : Format == BinaryFormat::ELF ? ".data.rel.ro"
                                             : ".rdata";
  Const.SegmentName =
      IsMachO ? (TableInText ? "__TEXT" : "__DATA_CONST") : Const.Name;
  Const.VA = SpilledConstTableVA;
  Const.Size = 0x40;
  Const.FileSz = Const.Size;
  Const.Flags = SegmentFlags::Readable;
  if ((IsMachO && !TableInText) || Format == BinaryFormat::ELF)
    Const.Flags = Const.Flags | SegmentFlags::Writable;
  if (IsMachO)
    Const.Type = S_REGULAR;
  Image.Sections.push_back(std::move(Const));

  if (PointerTable) {
    for (uint64_t Slot :
         {SpilledConstTableVA, SpilledConstTableVA + 8,
          OtherSpilledConstTableVA, OtherSpilledConstTableVA + 8})
      Image.DataPtrRelocSlots.insert(Slot);
    Image.RelocDataAddrs.insert(FirstTarget);
    Image.RelocDataAddrs.insert(SecondTarget);
  }

  Symbol Table;
  Table.Name = "_spilled_const_table";
  Table.Addr = SpilledConstTableVA;
  Table.Size = PointerTable ? 16 : 8;
  Image.Symbols.push_back(std::move(Table));
  Symbol OtherTable;
  OtherTable.Name = "_other_spilled_const_table";
  OtherTable.Addr = OtherSpilledConstTableVA;
  OtherTable.Size = PointerTable ? 16 : 8;
  Image.Symbols.push_back(std::move(OtherTable));
  return Image;
}

BinaryImage makeWritablePointerTableImage(Arch TargetArch,
                                          BinaryFormat Format) {
  BinaryImage Image =
      makeSpilledConstTableImage(TargetArch, Format, /*PointerTable=*/true);
  for (Segment &Seg : Image.Segments) {
    if (!Seg.contains(SpilledConstTableVA))
      continue;
    Seg.Name = Format == BinaryFormat::MachO ? "__DATA" : ".data";
    Seg.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  }
  for (Section &Sec : Image.Sections) {
    if (!Sec.contains(SpilledConstTableVA))
      continue;
    Sec.Name = Format == BinaryFormat::MachO ? "__data" : ".data";
    Sec.SegmentName = Format == BinaryFormat::MachO ? "__DATA" : ".data";
    Sec.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  }
  return Image;
}

BinaryImage makeAdjacentWritableOwnerImage(Arch TargetArch,
                                           BinaryFormat Format) {
  BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
  const bool IsMachO = Format == BinaryFormat::MachO;
  auto addWritableOwner = [&](llvm::StringRef Name, va_t VA, bool IsBss) {
    Segment Data;
    Data.Name = Name.str();
    Data.VA = VA;
    Data.Size = WritableOwnerSize;
    Data.FileSz = IsBss ? 0 : Data.Size;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    if (!IsBss)
      Data.Data.assign(Data.Size, 0);
    Image.Segments.push_back(std::move(Data));

    Section Sec;
    Sec.Name = (Name + (IsBss ? "_bss" : "_data")).str();
    Sec.SegmentName = Name.str();
    Sec.VA = VA;
    Sec.Size = WritableOwnerSize;
    Sec.FileSz = IsBss ? 0 : Sec.Size;
    Sec.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    if (IsMachO)
      Sec.Type = IsBss ? S_ZEROFILL : S_REGULAR;
    if (!IsBss)
      Sec.Data.assign(Sec.Size, 0);
    Image.Sections.push_back(std::move(Sec));
  };
  addWritableOwner("__OWNER_A", ZeroWritableOwnerVA, /*IsBss=*/true);
  addWritableOwner("__OWNER_B", AdjacentWritableOwnerVA, /*IsBss=*/false);
  return Image;
}

BinaryImage makeAbsoluteSlotOwnerCollisionImage(Arch TargetArch,
                                                BinaryFormat Format) {
  BinaryImage Image =
      makeSpilledConstTableImage(TargetArch, Format, /*PointerTable=*/true);
  const va_t SlotVA = SpilledConstTableVA;
  for (Segment &Seg : Image.Segments)
    if (Seg.contains(SlotVA)) {
      writeObject(Seg.Data, static_cast<size_t>(SlotVA - Seg.VA),
                  ReadOnlyOwnerBVA);
      break;
    }
  Image.DataPtrRelocSlots.insert(SlotVA);
  Image.DataPtrRelocTargetOwners[SlotVA] = ReadOnlyOwnerAVA;

  const bool IsMachO = Format == BinaryFormat::MachO;
  auto addOwner = [&](llvm::StringRef Name, va_t VA, bool ExecutableSegment) {
    Segment Data;
    Data.Name = Name.str();
    Data.VA = VA;
    Data.Size = ReadOnlyOwnerSize;
    Data.FileSz = Data.Size;
    Data.Flags = SegmentFlags::Readable;
    if (ExecutableSegment)
      Data.Flags = Data.Flags | SegmentFlags::Executable;
    Data.Data.assign(Data.Size, 0x5a);
    Image.Segments.push_back(std::move(Data));

    Section Sec;
    Sec.Name = (Name + "_const").str();
    Sec.SegmentName = Name.str();
    Sec.VA = VA;
    Sec.Size = ReadOnlyOwnerSize;
    Sec.FileSz = Sec.Size;
    Sec.Flags = SegmentFlags::Readable;
    Sec.Data.assign(Sec.Size, 0x5a);
    if (IsMachO)
      Sec.Type = S_REGULAR;
    Image.Sections.push_back(std::move(Sec));
  };
  // A deliberately shares an executable load segment while its exact section
  // is non-code; B is a distinct read-only object beginning at A's one-past
  // address.  A value-only lookup of the relocated target therefore creates
  // B's global, while occurrence ownership must create A+sizeof(A).
  addOwner("__OWNER_RO_A", ReadOnlyOwnerAVA, /*ExecutableSegment=*/true);
  addOwner("__OWNER_RO_B", ReadOnlyOwnerBVA, /*ExecutableSegment=*/false);
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
  Loop.EndAddr = CallerVA + 0x1c;
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

MedFunc makeReentrantFeasibleEdgeRecurrentTableLookup(Arch TargetArch) {
  MedFunc Func = makeRecurrentConstTableLookup(TargetArch);
  Func.Name = TargetArch == Arch::AArch64
                  ? "reentrant_feasible_edge_table_arm64"
                  : "reentrant_feasible_edge_table_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);

  MedVar FoldedTrue;
  FoldedTrue.Kind = MedVar::Temp;
  FoldedTrue.TheArch = TargetArch;
  FoldedTrue.Id = 35;
  FoldedTrue.SSAVer = 1;
  FoldedTrue.Size = 1;

  MedBlock &Entry = Func.Blocks[0];
  MedBlock &Loop = Func.Blocks[1];
  Entry.Succs = {Loop.Id, 3};
  Entry.Ops.clear();

  MedOp Compare;
  Compare.Opcode = NdOp::INT_EQUAL;
  Compare.Output = FoldedTrue;
  Compare.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Compare.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Entry.Ops.push_back(std::move(Compare));

  MedOp EnterLoop;
  EnterLoop.Opcode = NdOp::COND_BR;
  EnterLoop.Addr = Entry.StartAddr;
  EnterLoop.addInput(MedVar::makeConst(Loop.StartAddr, TRI.PointerSize));
  EnterLoop.addInput(FoldedTrue);
  Entry.Ops.push_back(std::move(EnterLoop));

  MedBlock Dead;
  Dead.Id = 3;
  Dead.StartAddr = CallerVA + 0x30;
  Dead.EndAddr = CallerVA + 0x38;
  Dead.Preds = {Entry.Id};
  Dead.Succs = {Loop.Id};
  MedOp DeadToLoop;
  DeadToLoop.Opcode = NdOp::BRANCH;
  DeadToLoop.addInput(MedVar::makeConst(Loop.StartAddr, TRI.PointerSize));
  Dead.Ops.push_back(std::move(DeadToLoop));

  Loop.Preds.push_back(Dead.Id);
  Loop.Phis.front().Args.push_back(
      {Dead.Id, MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize)});
  Func.Blocks.push_back(std::move(Dead));
  return Func;
}

LowFunc makeRelocationSensitiveConstantFoldFunction(Arch TargetArch) {
  const uint16_t PointerSize =
      static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
  LowFunc Func;
  Func.Entry = CallerVA;
  Func.Name = TargetArch == Arch::AArch64 ? "relocation_sensitive_fold_arm64"
                                          : "relocation_sensitive_fold_x86_64";

  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 0x5C;

  auto addBinary = [&](NdOp Opcode, NdVar Output, NdVar A, NdVar B, va_t Addr) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = Addr;
    Op.addInput(A);
    Op.addInput(B);
    Block.Ops.push_back(std::move(Op));
  };

  const NdVar Page = NdVar::address(TextVA, PointerSize);
  const NdVar BaseA = NdVar::tmp(0x10, PointerSize);
  const NdVar BaseB = NdVar::tmp(0x20, PointerSize);
  addBinary(NdOp::INT_ADD, BaseA, Page, NdVar::cst(0x800, PointerSize),
            CallerVA);
  addBinary(NdOp::INT_ADD, BaseB, Page, NdVar::cst(0x820, PointerSize),
            CallerVA + 4);

  // The remaining operations consume those address-forming definitions. This
  // is the important closure boundary: once either ADD becomes COPY(const), a
  // later pass iteration must still know that its value is relocatable.
  addBinary(NdOp::INT_SUB, NdVar::tmp(0x30, PointerSize), BaseA, BaseB,
            CallerVA + 8);
  addBinary(NdOp::INT_SUB, NdVar::tmp(0x40, PointerSize), BaseA,
            NdVar::cst(8, PointerSize), CallerVA + 0xC);
  addBinary(NdOp::INT_ADD, NdVar::tmp(0x50, PointerSize),
            NdVar::cst(40, PointerSize), NdVar::cst(2, PointerSize),
            CallerVA + 0x10);
  addBinary(NdOp::INT_SUB, NdVar::tmp(0x60, PointerSize),
            NdVar::cst(40, PointerSize), NdVar::cst(2, PointerSize),
            CallerVA + 0x14);
  addBinary(NdOp::INT_ADD, NdVar::tmp(0x70, PointerSize),
            NdVar::cst(8, PointerSize), BaseA, CallerVA + 0x18);
  addBinary(NdOp::INT_SUB, NdVar::tmp(0x80, PointerSize),
            NdVar::cst(8, PointerSize), BaseA, CallerVA + 0x1C);
  addBinary(NdOp::INT_ADD, NdVar::tmp(0x90, PointerSize), BaseA, BaseB,
            CallerVA + 0x20);
  addBinary(NdOp::INT_AND, NdVar::tmp(0xA0, PointerSize), BaseA,
            NdVar::cst(~uint64_t{0xF}, PointerSize), CallerVA + 0x24);
  addBinary(NdOp::INT_MULT, NdVar::tmp(0xB0, PointerSize), BaseA,
            NdVar::cst(1, PointerSize), CallerVA + 0x28);
  addBinary(NdOp::INT_ADD, NdVar::tmp(0xC0, 4), BaseA,
            NdVar::cst(8, PointerSize), CallerVA + 0x2C);

  // Low-VA zero-fill storage deliberately collides with ordinary scalar
  // constants. Architectural address origin, not numeric segment membership,
  // determines which occurrence must retain relocation semantics.
  const NdVar LowBss = NdVar::address(0x400, PointerSize);
  const NdVar LowBssA = NdVar::tmp(0xD0, PointerSize);
  const NdVar LowBssB = NdVar::tmp(0xE0, PointerSize);
  const NdVar LowBssEnd = NdVar::tmp(0xF0, PointerSize);
  addBinary(NdOp::INT_ADD, LowBssA, LowBss, NdVar::cst(0x10, PointerSize),
            CallerVA + 0x30);
  addBinary(NdOp::INT_ADD, LowBssB, LowBss, NdVar::cst(0x30, PointerSize),
            CallerVA + 0x34);
  addBinary(NdOp::INT_SUB, NdVar::tmp(0x100, PointerSize), LowBssB, LowBssA,
            CallerVA + 0x38);
  addBinary(NdOp::INT_ADD, LowBssEnd, LowBss, NdVar::cst(0x80, PointerSize),
            CallerVA + 0x3C);
  addBinary(NdOp::INT_SUB, NdVar::tmp(0x110, PointerSize), LowBssEnd, LowBssA,
            CallerVA + 0x40);
  addBinary(NdOp::INT_SUB, NdVar::tmp(0x120, PointerSize),
            NdVar::cst(0x430, PointerSize), NdVar::cst(0x410, PointerSize),
            CallerVA + 0x44);

  // A truncating COPY and a directly narrow address marker both retain a
  // relocation-sensitive bit pattern, but neither can establish a complete
  // machine pointer when a later operation widens its output.
  const NdVar NarrowAddress = NdVar::tmp(0x130, 4);
  LowOp TruncateAddress;
  TruncateAddress.Opcode = NdOp::COPY;
  TruncateAddress.Output = NarrowAddress;
  TruncateAddress.Addr = CallerVA + 0x48;
  TruncateAddress.addInput(NdVar::address(0x410, PointerSize));
  Block.Ops.push_back(std::move(TruncateAddress));
  addBinary(NdOp::INT_ADD, NdVar::tmp(0x140, PointerSize), NarrowAddress,
            NdVar::cst(8, PointerSize), CallerVA + 0x4C);
  addBinary(NdOp::INT_ADD, NdVar::tmp(0x150, PointerSize),
            NdVar::address(0x410, 4), NdVar::cst(8, PointerSize),
            CallerVA + 0x50);

  // Exact reproduction of a low-VA ADRP+ADD collision: the page offset is a
  // narrow scalar by instruction role even when the loader also records the
  // same numeric value as a relocation target elsewhere in the image.
  addBinary(NdOp::INT_ADD, NdVar::tmp(0x160, PointerSize),
            NdVar::addressFragment(0, PointerSize), NdVar::scalar(0x248, 4),
            CallerVA + 0x54);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 0x58;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeMaterializedPointerTableInductionLookup(Arch TargetArch) {
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
  Func.Name = TargetArch == Arch::AArch64
                  ? "materialized_pointer_table_induction_arm64"
                  : "materialized_pointer_table_induction_x86_64";
  Func.ReturnType = NdType::makeVoid();
  Func.DoesNotReturn = true;

  MedVar InitialBase = makeVar(MedVar::Temp, 120, TRI.PointerSize);
  MedVar PhiBase = makeVar(MedVar::Temp, 121, TRI.PointerSize);
  MedVar LoadedPointer = makeVar(MedVar::Temp, 122, TRI.PointerSize);
  MedVar NextBase = makeVar(MedVar::Temp, 123, TRI.PointerSize);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 8;
  Entry.Succs = {1};
  MedOp MaterializeBase;
  MaterializeBase.Opcode = NdOp::COPY;
  MaterializeBase.Output = InitialBase;
  MaterializeBase.addInput(
      MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  Entry.Ops.push_back(std::move(MaterializeBase));
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
  BasePhi.Args = {{0, InitialBase}, {2, NextBase}};
  Loop.Phis.push_back(std::move(BasePhi));
  MedOp ReadPointer;
  ReadPointer.Opcode = NdOp::LOAD;
  ReadPointer.Output = LoadedPointer;
  ReadPointer.addInput(PhiBase);
  Loop.Ops.push_back(std::move(ReadPointer));
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
  Step.addInput(MedVar::makeConst(TRI.PointerSize, TRI.PointerSize));
  Latch.Ops.push_back(std::move(Step));
  MedOp BackEdge;
  BackEdge.Opcode = NdOp::BRANCH;
  BackEdge.addInput(MedVar::makeConst(Loop.StartAddr, TRI.PointerSize));
  Latch.Ops.push_back(std::move(BackEdge));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Latch)};
  return Func;
}

enum class WritableOwnerMergeCase {
  DeepMixedSelectLoad,
  DistinctOwnerPhiStore,
  NullableZeroSelectLoad,
  SharedOffsetDiamondLoad,
  AlgebraicCancellationLoad,
};

MedFunc makeWritableOwnerMerge(Arch TargetArch, WritableOwnerMergeCase Case) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };
  auto owned = [&](va_t Address, va_t Owner) {
    return MedVar::makeConst(Address, PointerSize,
                             ConstantAddressProvenance::DataAddress, Owner);
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = std::string(TargetArch == Arch::AArch64 ? "owned_merge_arm64_"
                                                      : "owned_merge_x86_64_") +
              std::to_string(static_cast<int>(Case));
  Func.ReturnType = NdType::makeVoid();

  MedVar Condition = makeVar(MedVar::Param, 0, 1);
  Condition.RegOff = TRI.IntParamRegs[0];
  Func.Params.push_back(Condition);

  auto appendCopies = [&](MedBlock &Block, MedVar Value, int FirstId) {
    for (int I = 0; I < 2; ++I) {
      MedVar Next = makeVar(MedVar::Temp, FirstId + I, PointerSize);
      MedOp Copy;
      Copy.Opcode = NdOp::COPY;
      Copy.Output = Next;
      Copy.addInput(Value);
      Block.Ops.push_back(std::move(Copy));
      Value = Next;
    }
    return Value;
  };
  auto appendLoad = [&](MedBlock &Block, const MedVar &Address, int Id) {
    MedOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = makeVar(MedVar::Temp, Id, 1);
    Load.addInput(Address);
    Block.Ops.push_back(std::move(Load));
  };
  auto appendStore = [&](MedBlock &Block, const MedVar &Value) {
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.addInput(owned(AdjacentWritableOwnerVA + 8, AdjacentWritableOwnerVA));
    Store.addInput(Value);
    Block.Ops.push_back(std::move(Store));
  };
  auto appendReturn = [](MedBlock &Block) {
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(std::move(Return));
  };

  if (Case == WritableOwnerMergeCase::DistinctOwnerPhiStore ||
      Case == WritableOwnerMergeCase::AlgebraicCancellationLoad) {
    MedVar Merged = makeVar(MedVar::Temp, 10, PointerSize);

    MedBlock Entry;
    Entry.Id = 0;
    Entry.StartAddr = CallerVA;
    Entry.EndAddr = CallerVA + 8;
    Entry.Succs = {1, 2};
    MedOp Split;
    Split.Opcode = NdOp::COND_BR;
    Split.addInput(MedVar::makeConst(CallerVA + 0x10, PointerSize));
    Split.addInput(Condition);
    Entry.Ops.push_back(std::move(Split));

    MedBlock Left;
    Left.Id = 1;
    Left.StartAddr = CallerVA + 0x10;
    Left.EndAddr = CallerVA + 0x18;
    Left.Preds = {0};
    Left.Succs = {3};
    MedOp LeftBranch;
    LeftBranch.Opcode = NdOp::BRANCH;
    LeftBranch.addInput(MedVar::makeConst(CallerVA + 0x30, PointerSize));
    Left.Ops.push_back(std::move(LeftBranch));

    MedBlock Right;
    Right.Id = 2;
    Right.StartAddr = CallerVA + 0x20;
    Right.EndAddr = CallerVA + 0x28;
    Right.Preds = {0};
    Right.Succs = {3};
    MedOp RightBranch;
    RightBranch.Opcode = NdOp::BRANCH;
    RightBranch.addInput(MedVar::makeConst(CallerVA + 0x30, PointerSize));
    Right.Ops.push_back(std::move(RightBranch));

    MedBlock Merge;
    Merge.Id = 3;
    Merge.StartAddr = CallerVA + 0x30;
    Merge.EndAddr = CallerVA + 0x40;
    Merge.Preds = {1, 2};
    PhiNode Phi;
    Phi.Output = Merged;
    if (Case == WritableOwnerMergeCase::DistinctOwnerPhiStore)
      Phi.Args = {{1, owned(AdjacentWritableOwnerVA, ZeroWritableOwnerVA)},
                  {2, owned(AdjacentWritableOwnerVA, AdjacentWritableOwnerVA)}};
    else
      Phi.Args = {{1, MedVar::makeConst(8, PointerSize)},
                  {2, MedVar::makeConst(0x10, PointerSize)}};
    Merge.Phis.push_back(std::move(Phi));
    if (Case == WritableOwnerMergeCase::DistinctOwnerPhiStore) {
      MedVar Forwarded = appendCopies(Merge, Merged, 11);
      appendStore(Merge, Forwarded);
    } else {
      MedVar Difference = makeVar(MedVar::Temp, 11, PointerSize);
      MedOp SubtractBase;
      SubtractBase.Opcode = NdOp::INT_SUB;
      SubtractBase.Output = Difference;
      SubtractBase.addInput(Merged);
      SubtractBase.addInput(owned(8, ZeroWritableOwnerVA));
      Merge.Ops.push_back(std::move(SubtractBase));

      MedVar Recovered = makeVar(MedVar::Temp, 12, PointerSize);
      MedOp AddBase;
      AddBase.Opcode = NdOp::INT_ADD;
      AddBase.Output = Recovered;
      AddBase.addInput(owned(8, ZeroWritableOwnerVA));
      AddBase.addInput(Difference);
      Merge.Ops.push_back(std::move(AddBase));
      appendLoad(Merge, Recovered, 13);
    }
    appendReturn(Merge);
    Func.Blocks = {std::move(Entry), std::move(Left), std::move(Right),
                   std::move(Merge)};
    return Func;
  }

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 0x80;
  MedVar Address = makeVar(MedVar::Temp, 10, PointerSize);
  if (Case == WritableOwnerMergeCase::SharedOffsetDiamondLoad) {
    MedVar Offset = makeVar(MedVar::Param, 1, PointerSize);
    Offset.RegOff = TRI.IntParamRegs[1];
    Func.Params.push_back(Offset);
    for (int I = 0; I < 28; ++I) {
      MedVar Next = makeVar(MedVar::Temp, 20 + I, PointerSize);
      MedOp Select;
      Select.Opcode = NdOp::SELECT;
      Select.Output = Next;
      Select.addInput(Condition);
      Select.addInput(Offset);
      Select.addInput(Offset);
      Block.Ops.push_back(std::move(Select));
      Offset = Next;
    }
    MedOp FormAddress;
    FormAddress.Opcode = NdOp::INT_ADD;
    FormAddress.Output = Address;
    FormAddress.addInput(owned(8, ZeroWritableOwnerVA));
    FormAddress.addInput(Offset);
    Block.Ops.push_back(std::move(FormAddress));
    appendLoad(Block, Address, 60);
  } else {
    MedOp Select;
    Select.Opcode = NdOp::SELECT;
    Select.Output = Address;
    Select.addInput(Condition);
    if (Case == WritableOwnerMergeCase::NullableZeroSelectLoad) {
      Select.addInput(owned(0, ZeroWritableOwnerVA));
      Select.addInput(MedVar::makeConst(0, PointerSize));
    } else {
      Select.addInput(owned(0x10, ZeroWritableOwnerVA));
      Select.addInput(MedVar::makeConst(8, PointerSize));
    }
    Block.Ops.push_back(std::move(Select));
    MedVar Forwarded = appendCopies(Block, Address, 11);
    if (Case == WritableOwnerMergeCase::NullableZeroSelectLoad)
      appendLoad(Block, Forwarded, 20);
    else
      appendLoad(Block, Forwarded, 20);
  }
  appendReturn(Block);
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeAbsoluteSlotOwnedIndexedRead(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  auto temp = [&](int Id, uint16_t Size) {
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
                  ? "absolute_slot_owned_indexed_read_arm64"
                  : "absolute_slot_owned_indexed_read_x86_64";
  Func.ReturnType = NdType::makeVoid();

  MedVar Offset;
  Offset.Kind = MedVar::Param;
  Offset.TheArch = TargetArch;
  Offset.Id = 0;
  Offset.SSAVer = 0;
  Offset.Size = PointerSize;
  Offset.RegOff = TRI.IntParamRegs.front();
  Func.Params.push_back(Offset);

  MedVar LoadedPointer = temp(1, PointerSize);
  MedVar IndexedAddress = temp(2, PointerSize);
  MedVar LoadedValue = temp(3, 2);

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 0x10;

  MedOp LoadPointer;
  LoadPointer.Opcode = NdOp::LOAD;
  LoadPointer.Output = LoadedPointer;
  LoadPointer.addInput(MedVar::makeConst(SpilledConstTableVA, PointerSize));
  Block.Ops.push_back(std::move(LoadPointer));

  MedOp AddIndex;
  AddIndex.Opcode = NdOp::INT_ADD;
  AddIndex.Output = IndexedAddress;
  AddIndex.addInput(LoadedPointer);
  AddIndex.addInput(Offset);
  Block.Ops.push_back(std::move(AddIndex));

  MedOp LoadValue;
  LoadValue.Opcode = NdOp::LOAD;
  LoadValue.Output = LoadedValue;
  LoadValue.addInput(IndexedAddress);
  Block.Ops.push_back(std::move(LoadValue));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeRematerializedRecurrentTableLookup(Arch TargetArch,
                                               uint16_t ElementSize = 2) {
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
  Func.Name = TargetArch == Arch::AArch64
                  ? "rematerialized_recurrent_table_arm64"
                  : "rematerialized_recurrent_table_x86_64";
  Func.ReturnType = NdType::makeVoid();
  Func.DoesNotReturn = true;

  MedVar Index = makeVar(MedVar::Param, 0, TRI.PointerSize);
  Index.RegOff = TRI.IntParamRegs[0];
  MedVar ResetCondition = makeVar(MedVar::Param, 1, 1);
  ResetCondition.RegOff = TRI.IntParamRegs[1];
  Func.Params = {Index, ResetCondition};

  MedVar InitialBase = makeVar(MedVar::Temp, 100, TRI.PointerSize);
  MedVar StableBase = makeVar(MedVar::Temp, 101, TRI.PointerSize);
  MedVar HeaderBase = makeVar(MedVar::Temp, 102, TRI.PointerSize);
  MedVar SelectedBase = makeVar(MedVar::Temp, 103, TRI.PointerSize);
  MedVar ScaledIndex = makeVar(MedVar::Temp, 104, TRI.PointerSize);
  MedVar FirstAddress = makeVar(MedVar::Temp, 105, TRI.PointerSize);
  MedVar FirstElement = makeVar(MedVar::Temp, 106, ElementSize);
  MedVar NextIndex = makeVar(MedVar::Temp, 107, TRI.PointerSize);
  MedVar NextScaledIndex = makeVar(MedVar::Temp, 108, TRI.PointerSize);
  MedVar SecondAddress = makeVar(MedVar::Temp, 109, TRI.PointerSize);
  MedVar SecondElement = makeVar(MedVar::Temp, 110, ElementSize);
  MedVar RematerializedBase = makeVar(MedVar::Temp, 111, TRI.PointerSize);
  MedVar JoinBase = makeVar(MedVar::Temp, 112, TRI.PointerSize);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0x10;
  Entry.Succs = {1};
  MedOp MaterializeInitial;
  MaterializeInitial.Opcode = NdOp::COPY;
  MaterializeInitial.Output = InitialBase;
  MaterializeInitial.addInput(
      MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  Entry.Ops.push_back(std::move(MaterializeInitial));
  MedOp MaterializeStable;
  MaterializeStable.Opcode = NdOp::COPY;
  MaterializeStable.Output = StableBase;
  MaterializeStable.addInput(
      MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  Entry.Ops.push_back(std::move(MaterializeStable));
  MedOp EnterLoop;
  EnterLoop.Opcode = NdOp::BRANCH;
  EnterLoop.addInput(MedVar::makeConst(CallerVA + 0x10, TRI.PointerSize));
  Entry.Ops.push_back(std::move(EnterLoop));

  MedBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = CallerVA + 0x10;
  Loop.EndAddr = CallerVA + 0x30;
  Loop.Preds = {0, 4};
  Loop.Succs = {2, 3};
  PhiNode HeaderPhi;
  HeaderPhi.Output = HeaderBase;
  HeaderPhi.Args = {{0, InitialBase}, {4, JoinBase}};
  Loop.Phis.push_back(std::move(HeaderPhi));
  MedOp SelectBase;
  SelectBase.Opcode = NdOp::SELECT;
  SelectBase.Output = SelectedBase;
  SelectBase.addInput(ResetCondition);
  SelectBase.addInput(HeaderBase);
  SelectBase.addInput(StableBase);
  Loop.Ops.push_back(std::move(SelectBase));
  MedOp ScaleIndex;
  ScaleIndex.Opcode = NdOp::INT_LEFT;
  ScaleIndex.Output = ScaledIndex;
  ScaleIndex.addInput(Index);
  ScaleIndex.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Loop.Ops.push_back(std::move(ScaleIndex));
  MedOp FormFirstAddress;
  FormFirstAddress.Opcode = NdOp::INT_ADD;
  FormFirstAddress.Output = FirstAddress;
  FormFirstAddress.addInput(SelectedBase);
  FormFirstAddress.addInput(ScaledIndex);
  Loop.Ops.push_back(std::move(FormFirstAddress));
  MedOp ReadFirst;
  ReadFirst.Opcode = NdOp::LOAD;
  ReadFirst.Output = FirstElement;
  ReadFirst.addInput(FirstAddress);
  Loop.Ops.push_back(std::move(ReadFirst));
  MedOp AdvanceIndex;
  AdvanceIndex.Opcode = NdOp::INT_ADD;
  AdvanceIndex.Output = NextIndex;
  AdvanceIndex.addInput(Index);
  AdvanceIndex.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Loop.Ops.push_back(std::move(AdvanceIndex));
  MedOp ScaleNextIndex;
  ScaleNextIndex.Opcode = NdOp::INT_LEFT;
  ScaleNextIndex.Output = NextScaledIndex;
  ScaleNextIndex.addInput(NextIndex);
  ScaleNextIndex.addInput(MedVar::makeConst(1, TRI.PointerSize));
  Loop.Ops.push_back(std::move(ScaleNextIndex));
  MedOp FormSecondAddress;
  FormSecondAddress.Opcode = NdOp::INT_ADD;
  FormSecondAddress.Output = SecondAddress;
  FormSecondAddress.addInput(SelectedBase);
  FormSecondAddress.addInput(NextScaledIndex);
  Loop.Ops.push_back(std::move(FormSecondAddress));
  MedOp ReadSecond;
  ReadSecond.Opcode = NdOp::LOAD;
  ReadSecond.Output = SecondElement;
  ReadSecond.addInput(SecondAddress);
  Loop.Ops.push_back(std::move(ReadSecond));
  MedOp ChooseReset;
  ChooseReset.Opcode = NdOp::COND_BR;
  ChooseReset.addInput(MedVar::makeConst(CallerVA + 0x30, TRI.PointerSize));
  ChooseReset.addInput(ResetCondition);
  Loop.Ops.push_back(std::move(ChooseReset));

  MedBlock Reset;
  Reset.Id = 2;
  Reset.StartAddr = CallerVA + 0x30;
  Reset.EndAddr = CallerVA + 0x38;
  Reset.Preds = {1};
  Reset.Succs = {4};
  MedOp MaterializeAgain;
  MaterializeAgain.Opcode = NdOp::COPY;
  MaterializeAgain.Output = RematerializedBase;
  MaterializeAgain.addInput(
      MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  Reset.Ops.push_back(std::move(MaterializeAgain));
  MedOp ResetToJoin;
  ResetToJoin.Opcode = NdOp::BRANCH;
  ResetToJoin.addInput(MedVar::makeConst(CallerVA + 0x50, TRI.PointerSize));
  Reset.Ops.push_back(std::move(ResetToJoin));

  MedBlock Bypass;
  Bypass.Id = 3;
  Bypass.StartAddr = CallerVA + 0x40;
  Bypass.EndAddr = CallerVA + 0x48;
  Bypass.Preds = {1};
  Bypass.Succs = {4};
  MedOp BypassToJoin;
  BypassToJoin.Opcode = NdOp::BRANCH;
  BypassToJoin.addInput(MedVar::makeConst(CallerVA + 0x50, TRI.PointerSize));
  Bypass.Ops.push_back(std::move(BypassToJoin));

  MedBlock Join;
  Join.Id = 4;
  Join.StartAddr = CallerVA + 0x50;
  Join.EndAddr = CallerVA + 0x58;
  Join.Preds = {2, 3};
  Join.Succs = {1};
  PhiNode JoinPhi;
  JoinPhi.Output = JoinBase;
  JoinPhi.Args = {{2, RematerializedBase}, {3, HeaderBase}};
  Join.Phis.push_back(std::move(JoinPhi));
  MedOp BackEdge;
  BackEdge.Opcode = NdOp::BRANCH;
  BackEdge.addInput(MedVar::makeConst(CallerVA + 0x10, TRI.PointerSize));
  Join.Ops.push_back(std::move(BackEdge));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Reset),
                 std::move(Bypass), std::move(Join)};
  return Func;
}

MedFunc
makeDifferentBaseRematerializedRecurrentTableLookup(Arch TargetArch,
                                                    uint16_t ElementSize = 2) {
  MedFunc Func =
      makeRematerializedRecurrentTableLookup(TargetArch, ElementSize);
  Func.Name = TargetArch == Arch::AArch64
                  ? "different_rematerialized_recurrent_table_arm64"
                  : "different_rematerialized_recurrent_table_x86_64";
  MedBlock &Reset = Func.Blocks[2];
  EXPECT_EQ(Reset.Ops.front().Opcode, NdOp::COPY);
  Reset.Ops.front().Inputs[0].ConstVal = OtherSpilledConstTableVA;
  return Func;
}

MedFunc makeComputedCoincidenceRecurrentTableLookup(Arch TargetArch,
                                                    uint16_t ElementSize = 2) {
  MedFunc Func =
      makeRematerializedRecurrentTableLookup(TargetArch, ElementSize);
  Func.Name = TargetArch == Arch::AArch64
                  ? "computed_coincidence_recurrent_table_arm64"
                  : "computed_coincidence_recurrent_table_x86_64";
  MedBlock &Reset = Func.Blocks[2];
  MedVar RematerializedBase = Reset.Ops.front().Output;
  constexpr uint64_t Mask = 0x55;
  MedOp ComputeCoincidence;
  ComputeCoincidence.Opcode = NdOp::INT_XOR;
  ComputeCoincidence.Output = RematerializedBase;
  ComputeCoincidence.addInput(
      MedVar::makeConst(SpilledConstTableVA ^ Mask, RematerializedBase.Size));
  ComputeCoincidence.addInput(MedVar::makeConst(Mask, RematerializedBase.Size));
  Reset.Ops.front() = std::move(ComputeCoincidence);
  return Func;
}

MedFunc makeNarrowRematerializedRecurrentTableLookup(Arch TargetArch,
                                                     uint16_t ElementSize = 2) {
  MedFunc Func =
      makeRematerializedRecurrentTableLookup(TargetArch, ElementSize);
  Func.Name = TargetArch == Arch::AArch64
                  ? "narrow_rematerialized_recurrent_table_arm64"
                  : "narrow_rematerialized_recurrent_table_x86_64";
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedBlock &Reset = Func.Blocks[2];
  MedVar RematerializedBase = Reset.Ops.front().Output;
  MedVar NarrowBase = RematerializedBase;
  NarrowBase.Id = 113;
  NarrowBase.Size = 4;

  MedOp SliceBase;
  SliceBase.Opcode = NdOp::SUBBYTES;
  SliceBase.Output = NarrowBase;
  SliceBase.addInput(MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize));
  SliceBase.addInput(MedVar::makeConst(0, TRI.PointerSize));
  MedOp WidenBase;
  WidenBase.Opcode = NdOp::INT_ZEXT;
  WidenBase.Output = RematerializedBase;
  WidenBase.addInput(NarrowBase);

  Reset.Ops.front() = std::move(SliceBase);
  Reset.Ops.insert(std::next(Reset.Ops.begin()), std::move(WidenBase));
  return Func;
}

MedFunc makeZeroExtendedNarrowConstantRematerializedTableLookup(
    Arch TargetArch, bool TruncatedHighAddress, uint16_t ElementSize = 2) {
  MedFunc Func =
      makeRematerializedRecurrentTableLookup(TargetArch, ElementSize);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "narrow_constant_rematerialized_arm64_"
                              : "narrow_constant_rematerialized_x86_64_") +
              (TruncatedHighAddress ? "truncated" : "exact");
  const uint64_t Base =
      TruncatedHighAddress ? SpilledConstTableVA : LowSpilledConstTableVA;
  const uint16_t PointerSize = getTargetRegInfo(TargetArch).PointerSize;
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops) {
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (!TruncatedHighAddress && Op.Inputs[I].isConst() &&
            Op.Inputs[I].ConstVal == SpilledConstTableVA)
          Op.Inputs[I].ConstVal = Base;
      if (Op.Opcode == NdOp::COPY && Op.Output.Size == PointerSize &&
          Op.NumInputs >= 1 && Op.Inputs[0].isConst() &&
          Op.Inputs[0].ConstVal == Base)
        Op.Inputs[0].Size = 4;
    }
  return Func;
}

MedFunc makeMixedModelRematerializedRecurrentTableLookup(
    Arch TargetArch, bool RawInitializerFirst, uint16_t ElementSize = 2) {
  MedFunc Func =
      makeRematerializedRecurrentTableLookup(TargetArch, ElementSize);
  Func.Name = std::string(TargetArch == Arch::AArch64
                              ? "mixed_model_rematerialized_table_arm64_"
                              : "mixed_model_rematerialized_table_x86_64_") +
              (RawInitializerFirst ? "raw_first" : "symbol_first");
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  PhiNode &HeaderPhi = Func.Blocks[1].Phis.front();
  for (auto &[Pred, Arg] : HeaderPhi.Args)
    if (Pred == 0)
      Arg = MedVar::makeConst(SpilledConstTableVA, TRI.PointerSize);
  if (!RawInitializerFirst)
    std::reverse(HeaderPhi.Args.begin(), HeaderPhi.Args.end());
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

bool valueReferencesMaterializedGlobal(const llvm::Value *Root,
                                       std::set<const llvm::Value *> &Seen) {
  if (!Root || !Seen.insert(Root).second)
    return false;
  if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Root))
    return GV->hasInitializer();
  // MedIR PHIs are lowered through edge stores into an alloca. Follow those
  // reaching values when a pointer expression loads the PHI slot; an ordinary
  // operand-only walk stops at the alloca and cannot see the symbolized reset
  // values that make the address relocation-safe.
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Root))
    if (const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(
            Load->getPointerOperand()->stripPointerCasts()))
      for (const llvm::User *User : Slot->users())
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
            Store && Store->getPointerOperand()->stripPointerCasts() == Slot &&
            valueReferencesMaterializedGlobal(Store->getValueOperand(), Seen))
          return true;
  const auto *User = llvm::dyn_cast<llvm::User>(Root);
  if (!User)
    return false;
  for (const llvm::Use &Operand : User->operands())
    if (valueReferencesMaterializedGlobal(Operand.get(), Seen))
      return true;
  return false;
}

bool valueReferencesSpecificGlobal(const llvm::Value *Root,
                                   const llvm::GlobalVariable *Target,
                                   std::set<const llvm::Value *> &Seen) {
  if (!Root || !Seen.insert(Root).second)
    return false;
  if (Root == Target)
    return true;
  // PHI values are represented by edge stores into an alloca. Follow those
  // stores so owner-identity assertions inspect every feasible incoming value,
  // not merely the load of the lowered PHI slot.
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Root))
    if (const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(
            Load->getPointerOperand()->stripPointerCasts()))
      for (const llvm::User *User : Slot->users())
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
            Store && Store->getPointerOperand()->stripPointerCasts() == Slot &&
            valueReferencesSpecificGlobal(Store->getValueOperand(), Target,
                                          Seen))
          return true;
  const auto *User = llvm::dyn_cast<llvm::User>(Root);
  if (!User)
    return false;
  for (const llvm::Use &Operand : User->operands())
    if (valueReferencesSpecificGlobal(Operand.get(), Target, Seen))
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

bool valueReferencesInteger(const llvm::Value *Root, uint64_t Value,
                            std::set<const llvm::Value *> &Seen) {
  if (!Root || !Seen.insert(Root).second)
    return false;
  if (const auto *Int = llvm::dyn_cast<llvm::ConstantInt>(Root))
    return Int->getZExtValue() == Value;
  const auto *User = llvm::dyn_cast<llvm::User>(Root);
  if (!User)
    return false;
  for (const llvm::Use &Operand : User->operands())
    if (valueReferencesInteger(Operand.get(), Value, Seen))
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

BinaryImage
makeMixedPointerRecordImage(Arch TargetArch,
                            BinaryFormat Format = BinaryFormat::MachO) {
  BinaryImage Image = makeLLVMImage();
  Image.Arch = TargetArch;
  Image.Format = Format;
  Image.DyldBindSlots.clear();
  Image.ImportPtrSlots.clear();
  Image.Imports.clear();
  Image.CodePtrRelocSlots.clear();
  Image.DataPtrRelocSlots.clear();
  Image.DataPtrRelocTargetOwners.clear();

  Segment &Data = Image.Segments[1];
  if (Format == BinaryFormat::MachO) {
    Data.Name = section_names::macho::DataConstSeg;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  } else if (Format == BinaryFormat::ELF) {
    Data.Name = section_names::elf::DataRelRo;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  } else {
    Data.Name = section_names::coff::Rdata;
    Data.Flags = SegmentFlags::Readable;
  }
  std::fill(Data.Data.begin(), Data.Data.end(), 0);

  constexpr uint64_t FirstLength = 4;
  constexpr uint64_t SecondLength = 5;
  writeObject(Data.Data, 0, FirstLength);
  writeObject(Data.Data, 8, CodeVA);
  writeObject(Data.Data, 16, CStringVA);
  writeObject(Data.Data, 24, SecondLength);
  writeObject(Data.Data, 32, CodeVA);
  writeObject(Data.Data, 40, CStringBVA);
  Image.CodePtrRelocSlots.insert(DataVA + 8);
  Image.CodePtrRelocSlots.insert(DataVA + 32);
  Image.DataPtrRelocSlots.insert(DataVA + 16);
  Image.DataPtrRelocSlots.insert(DataVA + 40);

  Section Records;
  Records.Name = Format == BinaryFormat::MachO ? section_names::macho::Const
                 : Format == BinaryFormat::ELF ? section_names::elf::DataRelRo
                                               : section_names::coff::Rdata;
  Records.SegmentName = Data.Name;
  Records.VA = DataVA;
  Records.Size = 48;
  Records.FileSz = Records.Size;
  Records.Flags = Data.Flags;
  Image.Sections.push_back(std::move(Records));
  return Image;
}

MedFunc makeMixedPointerRecordIndirectCaller(Arch TargetArch,
                                             uint64_t InitialOffset = 8,
                                             uint64_t RecordStride = 24) {
  const uint16_t PointerSize =
      static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.TheArch = TargetArch;
    Value.Id = Id;
    Value.SSAVer = 1;
    Value.Size = Size;
    return Value;
  };

  MedVar InitialSlot = makeTemp(200, PointerSize);
  MedVar CurrentSlot = makeTemp(201, PointerSize);
  MedVar LoadedTarget = makeTemp(202, PointerSize);
  MedVar NextSlot = makeTemp(203, PointerSize);

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "mixed_pointer_record_indirect_caller";
  Func.ReturnType = NdType::makeVoid();
  Func.DoesNotReturn = true;

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 8;
  Entry.Succs = {1};
  MedOp Materialize;
  Materialize.Opcode = NdOp::COPY;
  Materialize.Addr = CallerVA;
  Materialize.Output = InitialSlot;
  Materialize.addInput(MedVar::makeConst(DataVA + InitialOffset, PointerSize,
                                         ConstantAddressProvenance::Address));
  Entry.Ops.push_back(std::move(Materialize));
  MedOp Enter;
  Enter.Opcode = NdOp::BRANCH;
  Enter.Addr = CallerVA + 4;
  Enter.addInput(MedVar::makeConst(CallerVA + 0x10, PointerSize));
  Entry.Ops.push_back(std::move(Enter));

  MedBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = CallerVA + 0x10;
  Loop.EndAddr = CallerVA + 0x1c;
  Loop.Preds = {0, 2};
  Loop.Succs = {2};
  PhiNode SlotPhi;
  SlotPhi.Output = CurrentSlot;
  SlotPhi.Args = {{0, InitialSlot}, {2, NextSlot}};
  Loop.Phis.push_back(std::move(SlotPhi));
  MedOp LoadTarget;
  LoadTarget.Opcode = NdOp::LOAD;
  LoadTarget.Addr = CallerVA + 0x10;
  LoadTarget.Output = LoadedTarget;
  LoadTarget.addInput(CurrentSlot);
  Loop.Ops.push_back(std::move(LoadTarget));
  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = CallerVA + 0x14;
  Call.addInput(LoadedTarget);
  Loop.Ops.push_back(std::move(Call));
  MedOp Continue;
  Continue.Opcode = NdOp::BRANCH;
  Continue.Addr = CallerVA + 0x18;
  Continue.addInput(MedVar::makeConst(CallerVA + 0x20, PointerSize));
  Loop.Ops.push_back(std::move(Continue));

  MedBlock Latch;
  Latch.Id = 2;
  Latch.StartAddr = CallerVA + 0x20;
  Latch.EndAddr = CallerVA + 0x28;
  Latch.Preds = {1};
  Latch.Succs = {1};
  MedOp Step;
  Step.Opcode = NdOp::INT_ADD;
  Step.Addr = CallerVA + 0x20;
  Step.Output = NextSlot;
  Step.addInput(CurrentSlot);
  Step.addInput(MedVar::makeConst(RecordStride, PointerSize));
  Latch.Ops.push_back(std::move(Step));
  MedOp BackEdge;
  BackEdge.Opcode = NdOp::BRANCH;
  BackEdge.Addr = CallerVA + 0x24;
  BackEdge.addInput(MedVar::makeConst(Loop.StartAddr, PointerSize));
  Latch.Ops.push_back(std::move(BackEdge));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Latch)};
  return Func;
}

MedFunc makeMixedPointerRecordArgumentCaller(Arch TargetArch) {
  MedFunc Func = makeMixedPointerRecordIndirectCaller(TargetArch);
  Func.Name = "mixed_pointer_record_argument_caller";
  MedBlock &Loop = Func.Blocks[1];
  MedOp &Call = Loop.Ops[1];
  Call.Opcode = NdOp::CALL;
  Call.Inputs[0] =
      MedVar::makeConst(ImportStubVA, getTargetRegInfo(TargetArch).PointerSize,
                        ConstantAddressProvenance::CodeAddress);

  MedCallInfo Info;
  Info.BlockId = Loop.Id;
  Info.OpIdx = 1;
  Info.TargetAddr = ImportStubVA;
  Info.TargetName = "consume_handler";
  Info.Args.push_back(Loop.Ops[0].Output);
  Func.CallInfos.push_back(std::move(Info));
  return Func;
}

MedFunc makeExactMixedPointerSlotIndirectCaller(Arch TargetArch,
                                                uint64_t SlotOffset,
                                                uint64_t TargetAdjustment = 0) {
  const uint16_t PointerSize =
      static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
  auto makeTemp = [&](int Id) {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.TheArch = TargetArch;
    Value.Id = Id;
    Value.SSAVer = 1;
    Value.Size = PointerSize;
    return Value;
  };
  MedVar Slot = makeTemp(210);
  MedVar Target = makeTemp(211);
  MedVar AdjustedTarget = makeTemp(214);

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name =
      "exact_mixed_pointer_slot_indirect_caller_" + std::to_string(SlotOffset);
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + (TargetAdjustment != 0 ? 0x14 : 0x10);
  MedOp Materialize;
  Materialize.Opcode = NdOp::COPY;
  Materialize.Addr = CallerVA;
  Materialize.Output = Slot;
  Materialize.addInput(MedVar::makeConst(DataVA + SlotOffset, PointerSize,
                                         ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Materialize));
  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Addr = CallerVA + 4;
  Load.Output = Target;
  Load.addInput(Slot);
  Block.Ops.push_back(std::move(Load));
  if (TargetAdjustment != 0) {
    MedOp Adjust;
    Adjust.Opcode = NdOp::INT_ADD;
    Adjust.Addr = CallerVA + 8;
    Adjust.Output = AdjustedTarget;
    Adjust.addInput(Target);
    Adjust.addInput(MedVar::makeConst(TargetAdjustment, PointerSize,
                                      ConstantAddressProvenance::Scalar));
    Block.Ops.push_back(std::move(Adjust));
  }
  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = CallerVA + (TargetAdjustment != 0 ? 12 : 8);
  Call.addInput(TargetAdjustment != 0 ? AdjustedTarget : Target);
  Block.Ops.push_back(std::move(Call));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + (TargetAdjustment != 0 ? 16 : 12);
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeIndexedPointerTableIndirectCaller(Arch TargetArch,
                                              uint64_t TableVA) {
  const uint16_t PointerSize =
      static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
  MedFunc Func =
      makeExactMixedPointerSlotIndirectCaller(TargetArch, TableVA - DataVA);
  Func.Name = "indexed_pointer_table_indirect_caller";

  MedVar Index;
  Index.Kind = MedVar::Param;
  Index.TheArch = TargetArch;
  Index.Id = 0;
  Index.Size = PointerSize;
  Func.Params.push_back(Index);

  auto makeTemp = [&](int Id) {
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.TheArch = TargetArch;
    Value.Id = Id;
    Value.SSAVer = 1;
    Value.Size = PointerSize;
    return Value;
  };
  MedVar ScaledIndex = makeTemp(212);
  MedVar IndexedSlot = makeTemp(213);
  MedBlock &Block = Func.Blocks.front();
  MedOp Load = std::move(Block.Ops[1]);
  MedOp Call = std::move(Block.Ops[2]);
  MedOp Return = std::move(Block.Ops[3]);
  Block.Ops.resize(1);

  MedOp Scale;
  Scale.Opcode = NdOp::INT_MULT;
  Scale.Addr = CallerVA + 4;
  Scale.Output = ScaledIndex;
  Scale.addInput(Index);
  Scale.addInput(MedVar::makeConst(PointerSize, PointerSize,
                                   ConstantAddressProvenance::Scalar));
  Block.Ops.push_back(std::move(Scale));
  MedOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Addr = CallerVA + 8;
  Add.Output = IndexedSlot;
  Add.addInput(Block.Ops[0].Output);
  Add.addInput(ScaledIndex);
  Block.Ops.push_back(std::move(Add));
  Load.Addr = CallerVA + 12;
  Load.Inputs[0] = IndexedSlot;
  Block.Ops.push_back(std::move(Load));
  Call.Addr = CallerVA + 16;
  Block.Ops.push_back(std::move(Call));
  Return.Addr = CallerVA + 20;
  Block.Ops.push_back(std::move(Return));
  Block.EndAddr = CallerVA + 24;
  return Func;
}

MedFunc makeFrameReloadedDirectPhiPointerTableLookup(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedFunc Func = makePhiConstTableLookup(
      TargetArch, PhiTableBaseCase::SameBaseOnReachableEdges);
  Func.Name = TargetArch == Arch::AArch64
                  ? "frame_reloaded_raw_pointer_phi_arm64"
                  : "frame_reloaded_raw_pointer_phi_x86_64";
  Func.FrameSize = 16;

  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = TargetArch;
  SP.Id = 100;
  SP.SSAVer = 0;
  SP.Size = TRI.PointerSize;
  SP.RegOff = TRI.StackPointer;
  MedVar Slot;
  Slot.Kind = MedVar::Temp;
  Slot.TheArch = TargetArch;
  Slot.Id = 101;
  Slot.SSAVer = 1;
  Slot.Size = TRI.PointerSize;
  MedVar Reloaded = Slot;
  Reloaded.Id = 102;

  MedOp FormSlot;
  FormSlot.Opcode = NdOp::INT_ADD;
  FormSlot.Output = Slot;
  FormSlot.addInput(SP);
  FormSlot.addInput(MedVar::makeConst(uint64_t(-8), TRI.PointerSize));
  Func.Blocks.front().Ops.insert(Func.Blocks.front().Ops.begin(),
                                 std::move(FormSlot));

  MedBlock &Merge = Func.Blocks[1];
  EXPECT_FALSE(Merge.Phis.empty());
  if (Merge.Phis.empty())
    return Func;
  for (auto &[Pred, Arg] : Merge.Phis.front().Args) {
    (void)Pred;
    Arg = MedVar::makeConst(WritableVA, TRI.PointerSize);
  }
  MedOp Spill;
  Spill.Opcode = NdOp::STORE;
  Spill.addInput(Slot);
  Spill.addInput(Merge.Phis.front().Output);
  MedOp Reload;
  Reload.Opcode = NdOp::LOAD;
  Reload.Output = Reloaded;
  Reload.addInput(Slot);
  Merge.Ops.insert(Merge.Ops.begin(), std::move(Spill));
  Merge.Ops.insert(std::next(Merge.Ops.begin()), std::move(Reload));
  for (MedOp &Op : Merge.Ops)
    if (Op.Opcode == NdOp::LOAD && Op.Output.Size == 2) {
      Op.Inputs[0] = Reloaded;
      break;
    }
  return Func;
}

TEST(MachOLLVMDataPointerBoundary,
     UsesSymbolizedPointerTableBaseAfterLocalFrameReload) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    BinaryImage Image = makeLLVMImage();
    Image.Arch = TargetArch;
    writeObject(Image.Segments[2].Data, 0, CodeVA);
    Image.CodePtrRelocSlots.insert(WritableVA);
    Image.CodeRefTargets.insert(CodeVA);

    MedFunc Lookup = makeSpilledConstTableLookup(TargetArch);
    Lookup.Name = TargetArch == Arch::AArch64
                      ? "symbolized_spilled_pointer_table_arm64"
                      : "symbolized_spilled_pointer_table_x86_64";
    ASSERT_GE(Lookup.Blocks.front().Ops.size(), 7u);
    MedOp &MaterializeBase = Lookup.Blocks.front().Ops[1];
    ASSERT_EQ(MaterializeBase.Opcode, NdOp::COPY);
    MaterializeBase.Inputs[0] = MedVar::makeConst(WritableVA, TRI.PointerSize);
    const MedVar ReloadedBase = Lookup.Blocks.front().Ops[3].Output;
    MedOp &ReadElement = Lookup.Blocks.front().Ops[6];
    ASSERT_EQ(ReadElement.Opcode, NdOp::LOAD);
    ReadElement.Inputs[0] = ReloadedBase;

    MedFunc Target = makeReturnFunction("spilled_pointer_target", CodeVA);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup, Target}, Context, "macho-symbolized-spilled-pointer-table",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    ASSERT_NE(Module, nullptr) << Diagnostic;
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "cptsel.direct");
    EXPECT_NE(Module->getNamedGlobal(
                  (kNdCodePtrPrefix + llvm::utohexstr(WritableVA)).str()),
              nullptr);
  }
}

TEST(MachOLLVMDataPointerBoundary,
     RebasesRawDirectPhiPointerTableBaseAfterLocalFrameReload) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeLLVMImage();
    Image.Arch = TargetArch;
    writeObject(Image.Segments[2].Data, 0, CodeVA);
    Image.CodePtrRelocSlots.insert(WritableVA);
    Image.CodeRefTargets.insert(CodeVA);

    MedFunc Lookup = makeFrameReloadedDirectPhiPointerTableLookup(TargetArch);
    MedFunc Target = makeReturnFunction("raw_spilled_pointer_target", CodeVA);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup, Target}, Context, "macho-raw-spilled-pointer-table",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findVolatileI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "cptsel");
  }
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
     ResolvesReadOnlyTableBaseMinusStableRuntimeOffset) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeSpilledConstTableLookup(TargetArch);
    MedOp *Address = nullptr;
    for (MedOp &Op : Lookup.Blocks.front().Ops)
      if (Op.Opcode == NdOp::INT_ADD && Op.Output.Id == 5) {
        Address = &Op;
        break;
      }
    ASSERT_NE(Address, nullptr);
    Address->Opcode = NdOp::INT_SUB;
    Address->Inputs[1] = Lookup.Params.front();

    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Lookup}, Context, "macho-table-base-dynamic-sub",
                              TargetArch, {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *Function = Module->getFunction(Lookup.Name);
    ASSERT_NE(Function, nullptr);
    const llvm::LoadInst *TableLoad = findNonAllocaI16Load(*Function);
    ASSERT_NE(TableLoad, nullptr);
    EXPECT_EQ(TableLoad->getPointerOperand()->getName(), "tblptr");
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
     FailsClosedWhenCodeSymbolArithmeticFoldsToDataTableVA) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    const uint16_t PointerSize =
        static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    ASSERT_TRUE(Image.isCodeAddress(CodeVA));

    MedFunc Lookup = makeSpilledConstTableLookup(TargetArch);
    Lookup.Name = TargetArch == Arch::AArch64
                      ? "code_symbol_folded_to_table_arm64"
                      : "code_symbol_folded_to_table_x86_64";
    MedBlock &Entry = Lookup.Blocks.front();
    auto Materialize =
        std::find_if(Entry.Ops.begin(), Entry.Ops.end(), [](const MedOp &Op) {
          return Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                 Op.Inputs[0].isConst() &&
                 Op.Inputs[0].ConstVal == SpilledConstTableVA;
        });
    ASSERT_NE(Materialize, Entry.Ops.end());
    MedVar TableBase = Materialize->Output;
    MedVar CodeBase = TableBase;
    CodeBase.Id += 1000;
    Materialize->Output = CodeBase;
    Materialize->Inputs[0] = MedVar::makeConst(CodeVA, PointerSize);

    MedOp FoldToTable;
    FoldToTable.Opcode = NdOp::INT_ADD;
    FoldToTable.Output = TableBase;
    FoldToTable.addInput(CodeBase);
    FoldToTable.addInput(
        MedVar::makeConst(SpilledConstTableVA - CodeVA, PointerSize));
    Entry.Ops.insert(std::next(Materialize), std::move(FoldToTable));

    MedFunc CodeTarget = makeReturnFunction(TargetArch == Arch::AArch64
                                                ? "code_fold_target_arm64"
                                                : "code_fold_target_x86_64",
                                            CodeVA);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup, CodeTarget}, Context, "macho-code-symbol-folded-to-table",
        TargetArch, {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
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
     ResolvesDistinctRawFrameReloadTableBasesWithinOneRun) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeRejectedFrameReloadLookup(
        TargetArch, ReachingStoreCase::DistinctPredecessorBases);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-distinct-frame-table-bases", TargetArch, {},
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
     FailsClosedForDistinctRawFrameReloadTableBasesAcrossRuns) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    addFarSpilledConstTable(Image);
    MedFunc Lookup = makeRejectedFrameReloadLookup(
        TargetArch, ReachingStoreCase::DistinctPredecessorBases);
    replaceMedConstant(Lookup, OtherSpilledConstTableVA,
                       FarSpilledConstTableVA);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-cross-run-frame-table-bases", TargetArch, {},
        &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
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

TEST(LLVMDataPointerInvariantBoundary,
     PreservesSymbolizedWritablePointerTableInductionPhi) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeWritablePointerTableImage(TargetArch, Format);
      MedFunc Lookup = makeMaterializedPointerTableInductionLookup(TargetArch);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit({Lookup}, Context,
                                          "writable-pointer-table-induction",
                                          TargetArch, {}, &Image, Format);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      ASSERT_NE(Module, nullptr) << Diagnostic;
      expectValidModule(*Module);

      const llvm::GlobalVariable *Mirror = nullptr;
      for (const llvm::GlobalVariable &Global : Module->globals())
        if (Global.getName().starts_with("__nd_codeptr_")) {
          ASSERT_EQ(Mirror, nullptr);
          Mirror = &Global;
        }
      ASSERT_NE(Mirror, nullptr);
      EXPECT_FALSE(Mirror->isConstant());

      llvm::Function *Function = Module->getFunction(Lookup.Name);
      ASSERT_NE(Function, nullptr);
      const llvm::LoadInst *PointerLoad = nullptr;
      for (const llvm::BasicBlock &Block : *Function)
        for (const llvm::Instruction &Instruction : Block)
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
              Load && Load->getType()->isIntegerTy(64) && Load->isVolatile()) {
            ASSERT_EQ(PointerLoad, nullptr);
            PointerLoad = Load;
          }
      ASSERT_NE(PointerLoad, nullptr);
      EXPECT_EQ(PointerLoad->getPointerOperand()->getName(), "cptsel.direct");
      std::set<const llvm::Value *> Seen;
      EXPECT_TRUE(valueReferencesMaterializedGlobal(
          PointerLoad->getPointerOperand(), Seen));
      bool RetainsRawTableVA = false;
      for (const llvm::BasicBlock &Block : *Function)
        for (const llvm::Instruction &Instruction : Block)
          for (const llvm::Value *Operand : Instruction.operands())
            if (const auto *C = llvm::dyn_cast<llvm::Constant>(Operand))
              RetainsRawTableVA |=
                  constantContainsInteger(C, SpilledConstTableVA);
      EXPECT_FALSE(RetainsRawTableVA);
      EXPECT_EQ(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     PreservesOccurrenceOwnedWritablePointersAcrossNestedMerges) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64})
      for (WritableOwnerMergeCase Case : {
               WritableOwnerMergeCase::DeepMixedSelectLoad,
               WritableOwnerMergeCase::DistinctOwnerPhiStore,
               WritableOwnerMergeCase::NullableZeroSelectLoad,
               WritableOwnerMergeCase::SharedOffsetDiamondLoad,
               WritableOwnerMergeCase::AlgebraicCancellationLoad,
           }) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        SCOPED_TRACE(static_cast<int>(Case));
        BinaryImage Image = makeAdjacentWritableOwnerImage(TargetArch, Format);
        if (Case == WritableOwnerMergeCase::AlgebraicCancellationLoad) {
          Image.WritableRelocDataAddrs.insert(8);
          Image.WritableRelocDataAddrs.insert(0x10);
        }
        MedFunc Func = makeWritableOwnerMerge(TargetArch, Case);

        llvm::LLVMContext Context;
        testing::internal::CaptureStderr();
        auto Module = MedLLVMEmitter().emit({Func}, Context,
                                            "occurrence-owned-writable-merge",
                                            TargetArch, {}, &Image, Format);
        std::string Diagnostic = testing::internal::GetCapturedStderr();
        ASSERT_NE(Module, nullptr) << Diagnostic;
        expectValidModule(*Module);

        llvm::Function *Emitted = Module->getFunction(Func.Name);
        ASSERT_NE(Emitted, nullptr);
        const llvm::Value *ObservedValue = nullptr;
        if (Case == WritableOwnerMergeCase::DeepMixedSelectLoad ||
            Case == WritableOwnerMergeCase::NullableZeroSelectLoad ||
            Case == WritableOwnerMergeCase::SharedOffsetDiamondLoad ||
            Case == WritableOwnerMergeCase::AlgebraicCancellationLoad) {
          const llvm::LoadInst *ObservedLoad = nullptr;
          for (const llvm::BasicBlock &Block : *Emitted)
            for (const llvm::Instruction &Instruction : Block)
              if (const auto *Load =
                      llvm::dyn_cast<llvm::LoadInst>(&Instruction);
                  Load && Load->isVolatile() &&
                  Load->getType()->isIntegerTy(8)) {
                ASSERT_EQ(ObservedLoad, nullptr);
                ObservedLoad = Load;
              }
          ASSERT_NE(ObservedLoad, nullptr);
          ObservedValue = ObservedLoad->getPointerOperand();
          EXPECT_EQ(ObservedValue->getName(), "wrptr.mixed");
        } else {
          const llvm::StoreInst *ObservedStore = nullptr;
          for (const llvm::BasicBlock &Block : *Emitted)
            for (const llvm::Instruction &Instruction : Block)
              if (const auto *Store =
                      llvm::dyn_cast<llvm::StoreInst>(&Instruction);
                  Store && Store->isVolatile()) {
                ASSERT_EQ(ObservedStore, nullptr);
                ObservedStore = Store;
              }
          ASSERT_NE(ObservedStore, nullptr);
          ObservedValue = ObservedStore->getValueOperand();
        }
        ASSERT_NE(ObservedValue, nullptr);

        const llvm::GlobalVariable *OwnerA =
            Module->getNamedGlobal("__nd_data_0.data");
        ASSERT_NE(OwnerA, nullptr);
        std::set<const llvm::Value *> SeenA;
        EXPECT_TRUE(
            valueReferencesSpecificGlobal(ObservedValue, OwnerA, SeenA));

        const llvm::GlobalVariable *OwnerB =
            Module->getNamedGlobal("__nd_data_20.data");
        if (Case == WritableOwnerMergeCase::DistinctOwnerPhiStore) {
          ASSERT_NE(OwnerB, nullptr);
          std::set<const llvm::Value *> SeenB;
          EXPECT_TRUE(
              valueReferencesSpecificGlobal(ObservedValue, OwnerB, SeenB));
        } else if (OwnerB) {
          std::set<const llvm::Value *> SeenB;
          EXPECT_FALSE(
              valueReferencesSpecificGlobal(ObservedValue, OwnerB, SeenB));
        }

        if (Case == WritableOwnerMergeCase::NullableZeroSelectLoad) {
          std::set<const llvm::Value *> Seen;
          std::function<bool(const llvm::Value *)> HasNonNullGuard =
              [&](const llvm::Value *Value) {
                if (!Value || !Seen.insert(Value).second)
                  return false;
                if (const auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(Value);
                    Cmp && Cmp->getPredicate() == llvm::CmpInst::ICMP_NE)
                  for (const llvm::Value *Operand : Cmp->operands())
                    if (const auto *C =
                            llvm::dyn_cast<llvm::ConstantInt>(Operand);
                        C && C->isZero())
                      return true;
                const auto *User = llvm::dyn_cast<llvm::User>(Value);
                if (!User)
                  return false;
                for (const llvm::Value *Operand : User->operand_values())
                  if (HasNonNullGuard(Operand))
                    return true;
                return false;
              };
          std::string IR;
          llvm::raw_string_ostream Stream(IR);
          Module->print(Stream, nullptr);
          EXPECT_TRUE(HasNonNullGuard(ObservedValue)) << IR;
        }
      }
}

TEST(LLVMDataPointerInvariantBoundary,
     PreservesAbsoluteSlotOwnerAcrossIndexedRead) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image =
          makeAbsoluteSlotOwnerCollisionImage(TargetArch, Format);
      MedFunc Lookup = makeAbsoluteSlotOwnedIndexedRead(TargetArch);

      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit({Lookup}, Context,
                                          "absolute-slot-owned-indexed-read",
                                          TargetArch, {}, &Image, Format);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      ASSERT_NE(Module, nullptr) << Diagnostic;
      expectValidModule(*Module);

      llvm::Function *Function = Module->getFunction(Lookup.Name);
      ASSERT_NE(Function, nullptr);
      const llvm::LoadInst *TableLoad = findNonAllocaI16Load(*Function);
      ASSERT_NE(TableLoad, nullptr);

      const llvm::GlobalVariable *OwnerA =
          Module->getNamedGlobal("__nd_data_2000.rodata");
      ASSERT_NE(OwnerA, nullptr);
      std::set<const llvm::Value *> SeenA;
      EXPECT_TRUE(valueReferencesSpecificGlobal(TableLoad->getPointerOperand(),
                                                OwnerA, SeenA));

      if (const llvm::GlobalVariable *OwnerB =
              Module->getNamedGlobal("__nd_data_2020.rodata")) {
        std::set<const llvm::Value *> SeenB;
        EXPECT_FALSE(valueReferencesSpecificGlobal(
            TableLoad->getPointerOperand(), OwnerB, SeenB));
      }
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     PreservesPureRematerializedTableBaseRecurrence) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (bool PointerTable : {false, true})
      for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(PointerTable ? "pointer table" : "plain table");
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        const uint16_t ElementSize =
            PointerTable ? getTargetRegInfo(TargetArch).PointerSize : 2;
        BinaryImage Image =
            makeSpilledConstTableImage(TargetArch, Format, PointerTable);
        MedFunc Lookup =
            makeRematerializedRecurrentTableLookup(TargetArch, ElementSize);
        llvm::LLVMContext Context;
        testing::internal::CaptureStderr();
        auto Module = MedLLVMEmitter().emit({Lookup}, Context,
                                            "rematerialized-recurrent-table",
                                            TargetArch, {}, &Image, Format);
        std::string Diagnostic = testing::internal::GetCapturedStderr();
        ASSERT_NE(Module, nullptr) << Diagnostic;
        expectValidModule(*Module);

        llvm::Function *Function = Module->getFunction(Lookup.Name);
        ASSERT_NE(Function, nullptr);
        unsigned TableLoads = 0;
        for (const llvm::BasicBlock &Block : *Function)
          for (const llvm::Instruction &Instruction : Block) {
            if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
                Load && Load->getType()->isIntegerTy(ElementSize * 8) &&
                Load->isVolatile()) {
              ++TableLoads;
              std::set<const llvm::Value *> Seen;
              EXPECT_TRUE(valueReferencesMaterializedGlobal(
                  Load->getPointerOperand(), Seen));
            }
          }
        EXPECT_EQ(TableLoads, 2U);
      }
}

TEST(LLVMDataPointerInvariantBoundary,
     PreservesExactZeroExtendedNarrowConstantTableBase) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      addThresholdCrossingConstTableRun(Image);
      MedFunc Lookup = makeZeroExtendedNarrowConstantRematerializedTableLookup(
          TargetArch, /*TruncatedHighAddress=*/false);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit(
          {Lookup}, Context, "narrow-constant-rematerialized-table", TargetArch,
          {}, &Image, Format);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      ASSERT_NE(Module, nullptr) << Diagnostic;
      expectValidModule(*Module);

      llvm::Function *Function = Module->getFunction(Lookup.Name);
      ASSERT_NE(Function, nullptr);
      unsigned TableLoads = 0;
      for (const llvm::BasicBlock &Block : *Function)
        for (const llvm::Instruction &Instruction : Block)
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
              Load && Load->getType()->isIntegerTy(16) && Load->isVolatile()) {
            ++TableLoads;
            std::set<const llvm::Value *> Seen;
            EXPECT_TRUE(
                valueReferencesConstantGlobal(Load->getPointerOperand(), Seen));
          }
      EXPECT_EQ(TableLoads, 2U);
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsTruncatedHighNarrowConstantTableBase) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      MedFunc Lookup = makeZeroExtendedNarrowConstantRematerializedTableLookup(
          TargetArch, /*TruncatedHighAddress=*/true);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit({Lookup}, Context,
                                          "truncated-narrow-constant-table",
                                          TargetArch, {}, &Image, Format);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module.get(), nullptr);
      EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsDifferentRematerializedTableBaseRecurrence) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (bool PointerTable : {false, true})
      for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(PointerTable ? "pointer table" : "plain table");
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        const uint16_t ElementSize =
            PointerTable ? getTargetRegInfo(TargetArch).PointerSize : 2;
        BinaryImage Image =
            makeSpilledConstTableImage(TargetArch, Format, PointerTable);
        MedFunc Lookup = makeDifferentBaseRematerializedRecurrentTableLookup(
            TargetArch, ElementSize);
        llvm::LLVMContext Context;
        testing::internal::CaptureStderr();
        auto Module = MedLLVMEmitter().emit(
            {Lookup}, Context, "different-rematerialized-recurrent-table",
            TargetArch, {}, &Image, Format);
        std::string Diagnostic = testing::internal::GetCapturedStderr();
        EXPECT_EQ(Module.get(), nullptr);
        EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                  std::string::npos)
            << Diagnostic;
      }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsNumericCoincidenceAsRematerializedTableBase) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (bool PointerTable : {false, true})
      for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(PointerTable ? "pointer table" : "plain table");
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        const uint16_t ElementSize =
            PointerTable ? getTargetRegInfo(TargetArch).PointerSize : 2;
        BinaryImage Image =
            makeSpilledConstTableImage(TargetArch, Format, PointerTable);
        MedFunc Lookup = makeComputedCoincidenceRecurrentTableLookup(
            TargetArch, ElementSize);
        llvm::LLVMContext Context;
        testing::internal::CaptureStderr();
        auto Module = MedLLVMEmitter().emit(
            {Lookup}, Context, "computed-coincidence-recurrent-table",
            TargetArch, {}, &Image, Format);
        std::string Diagnostic = testing::internal::GetCapturedStderr();
        EXPECT_EQ(Module.get(), nullptr);
        EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                  std::string::npos)
            << Diagnostic;
      }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsNarrowRematerializedTableBaseRecurrence) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (bool PointerTable : {false, true})
      for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(PointerTable ? "pointer table" : "plain table");
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        const uint16_t ElementSize =
            PointerTable ? getTargetRegInfo(TargetArch).PointerSize : 2;
        BinaryImage Image =
            makeSpilledConstTableImage(TargetArch, Format, PointerTable);
        MedFunc Lookup = makeNarrowRematerializedRecurrentTableLookup(
            TargetArch, ElementSize);
        llvm::LLVMContext Context;
        testing::internal::CaptureStderr();
        auto Module = MedLLVMEmitter().emit(
            {Lookup}, Context, "narrow-rematerialized-recurrent-table",
            TargetArch, {}, &Image, Format);
        std::string Diagnostic = testing::internal::GetCapturedStderr();
        EXPECT_EQ(Module.get(), nullptr);
        EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                  std::string::npos)
            << Diagnostic;
      }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsMixedAddressModelsForRematerializedTableBase) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (bool PointerTable : {false, true})
      for (Arch TargetArch : {Arch::AArch64, Arch::X64})
        for (bool RawInitializerFirst : {false, true}) {
          SCOPED_TRACE(formatTraceName(Format));
          SCOPED_TRACE(PointerTable ? "pointer table" : "plain table");
          SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
          SCOPED_TRACE(RawInitializerFirst ? "raw first" : "symbol first");
          const uint16_t ElementSize =
              PointerTable ? getTargetRegInfo(TargetArch).PointerSize : 2;
          BinaryImage Image =
              makeSpilledConstTableImage(TargetArch, Format, PointerTable);
          if (Format == BinaryFormat::MachO && !PointerTable)
            Image.Segments.front().Flags = SegmentFlags::Readable;
          MedFunc Lookup = makeMixedModelRematerializedRecurrentTableLookup(
              TargetArch, RawInitializerFirst, ElementSize);
          llvm::LLVMContext Context;
          testing::internal::CaptureStderr();
          auto Module = MedLLVMEmitter().emit(
              {Lookup}, Context, "mixed-model-rematerialized-table", TargetArch,
              {}, &Image, Format);
          std::string Diagnostic = testing::internal::GetCapturedStderr();
          EXPECT_EQ(Module.get(), nullptr);
          EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                    std::string::npos)
              << Diagnostic;
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
     PointerTableOwnerClaimsPureRelocatedSlotInduction) {
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
    EXPECT_EQ(PointerLoad->getPointerOperand()->getName(), "cptsel.direct");
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

TEST(LLVMCodePointerInvariantBoundary,
     MixedPointerTableCodeLaneRemainsCallable) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
      SCOPED_TRACE(std::string(formatTraceName(Format)) + "/" +
                   std::to_string(static_cast<int>(TargetArch)));
      BinaryImage Image = makeMixedPointerRecordImage(TargetArch, Format);
      MedFunc Caller = makeMixedPointerRecordIndirectCaller(TargetArch);
      MedFunc Callee =
          makeReturnFunction("mixed_pointer_record_target", CodeVA);

      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                          "mixed-pointer-record-indirect-call",
                                          TargetArch, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
      ASSERT_NE(EmittedCaller, nullptr);
      std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
      ASSERT_EQ(Calls.size(), 1u);

      const std::string MirrorName =
          (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str();
      llvm::GlobalVariable *Mirror = Module->getNamedGlobal(MirrorName);
      ASSERT_NE(Mirror, nullptr);
      std::set<const llvm::Value *> SeenMirror;
      EXPECT_TRUE(valueReferencesSpecificGlobal(
          Calls.front()->getCalledOperand(), Mirror, SeenMirror));
      bool RetainsRawTargetVA = false;
      for (const llvm::BasicBlock &Block : *EmittedCaller)
        for (const llvm::Instruction &Instruction : Block)
          for (const llvm::Value *Operand : Instruction.operands())
            if (const auto *C = llvm::dyn_cast<llvm::Constant>(Operand))
              RetainsRawTargetVA |= constantContainsInteger(C, CodeVA);
      EXPECT_FALSE(RetainsRawTargetVA);
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     MixedPointerTableCodeLaneIsNotReclassifiedAsDataArgument) {
  BinaryImage Image = makeMixedPointerRecordImage(Arch::AArch64);
  MedFunc Caller = makeMixedPointerRecordArgumentCaller(Arch::AArch64);
  MedFunc Callee = makeReturnFunction("mixed_pointer_record_target", CodeVA);

  MedLLVMEmitter Classifier;
  MedLLVMProvenanceTestPeer::prepareFreshAnalysis(
      Classifier, Caller, Image, Arch::AArch64, BinaryFormat::MachO);
  EXPECT_FALSE(MedLLVMProvenanceTestPeer::recoversAbsoluteDataPointerIdentity(
      Classifier, Caller.Blocks[1].Ops[0].Output));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Caller, Callee}, Context, "mixed-pointer-record-code-argument",
      Arch::AArch64, {{ImportStubVA, "consume_handler"}}, &Image,
      BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
  ASSERT_NE(EmittedCaller, nullptr);
  std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
  ASSERT_EQ(Calls.size(), 1u);
  ASSERT_EQ(Calls.front()->arg_size(), 1u);
  const std::string MirrorName =
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str();
  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(MirrorName);
  ASSERT_NE(Mirror, nullptr);
  std::set<const llvm::Value *> Seen;
  EXPECT_TRUE(valueReferencesSpecificGlobal(Calls.front()->getArgOperand(0),
                                            Mirror, Seen));
}

TEST(LLVMCodePointerInvariantBoundary,
     ExactMixedPointerSlotsRequireAnExplicitCallableRole) {
  struct Case {
    uint64_t Offset;
    bool Callable;
  };
  for (const Case &C : {Case{0, false}, Case{8, true}, Case{16, false},
                        Case{24, false}, Case{32, true}, Case{40, false}}) {
    SCOPED_TRACE(C.Offset);
    BinaryImage Image = makeMixedPointerRecordImage(Arch::AArch64);
    MedFunc Caller =
        makeExactMixedPointerSlotIndirectCaller(Arch::AArch64, C.Offset);
    MedFunc Callee = makeReturnFunction("mixed_pointer_record_target", CodeVA);

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Caller, Callee}, Context, "exact-mixed-pointer-slot", Arch::AArch64,
        {}, &Image, BinaryFormat::MachO);
    if (!C.Callable) {
      EXPECT_EQ(Module, nullptr);
      continue;
    }
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
    ASSERT_NE(EmittedCaller, nullptr);
    std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
    ASSERT_EQ(Calls.size(), 1u);
    llvm::GlobalVariable *Mirror = Module->getNamedGlobal(
        (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str());
    ASSERT_NE(Mirror, nullptr);
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(valueReferencesSpecificGlobal(Calls.front()->getCalledOperand(),
                                              Mirror, Seen));
  }
}

TEST(LLVMCodePointerInvariantBoundary,
     MixedPointerTableImportLaneRemainsCallable) {
  BinaryImage Image = makeMixedPointerRecordImage(Arch::AArch64);
  Image.CodePtrRelocSlots.clear();
  Image.ImportPtrSlots[DataVA + 8] = "handler_one";
  Image.ImportPtrSlots[DataVA + 32] = "handler_two";
  addImport(Image, "handler_one", DataVA + 8);
  addImport(Image, "handler_two", DataVA + 32);
  MedFunc Caller = makeMixedPointerRecordIndirectCaller(Arch::AArch64);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Caller}, Context, "mixed-pointer-record-import-call", Arch::AArch64, {},
      &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
  ASSERT_NE(EmittedCaller, nullptr);
  std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
  ASSERT_EQ(Calls.size(), 1u);
  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str());
  ASSERT_NE(Mirror, nullptr);
  std::set<const llvm::Value *> Seen;
  EXPECT_TRUE(valueReferencesSpecificGlobal(Calls.front()->getCalledOperand(),
                                            Mirror, Seen));
  EXPECT_NE(Module->getNamedValue("handler_one"), nullptr);
  EXPECT_NE(Module->getNamedValue("handler_two"), nullptr);
}

TEST(LLVMCodePointerInvariantBoundary,
     CodePointerSlotWithPostLoadAdjustmentFailsClosed) {
  BinaryImage Image = makeMixedPointerRecordImage(Arch::AArch64);
  MedFunc Caller = makeExactMixedPointerSlotIndirectCaller(Arch::AArch64, 8, 4);
  MedFunc Callee = makeReturnFunction("mixed_pointer_record_target", CodeVA);

  llvm::LLVMContext Context;
  EXPECT_EQ(MedLLVMEmitter().emit({Caller, Callee}, Context,
                                  "adjusted-code-pointer-slot", Arch::AArch64,
                                  {}, &Image, BinaryFormat::MachO),
            nullptr);
}

TEST(LLVMCodePointerInvariantBoundary,
     IndexedPointerTableRequiresOneCompleteCallableDomain) {
  MedFunc Callee = makeReturnFunction("indexed_pointer_target", CodeVA);
  {
    BinaryImage Image = makeMixedPointerRecordImage(Arch::AArch64);
    Segment &Data = Image.Segments[1];
    writeObject(Data.Data, 0, CodeVA);
    writeObject(Data.Data, 8, CodeVA);
    Image.CodePtrRelocSlots = {DataVA, DataVA + 8};
    Image.DataPtrRelocSlots.clear();
    Section &Table = Image.Sections.back();
    Table.Size = 16;
    Table.FileSz = Table.Size;
    MedFunc Caller =
        makeIndexedPointerTableIndirectCaller(Arch::AArch64, DataVA);

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Caller, Callee}, Context, "indexed-code-pointer-table", Arch::AArch64,
        {}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
    ASSERT_NE(EmittedCaller, nullptr);
    std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
    ASSERT_EQ(Calls.size(), 1u);
    llvm::GlobalVariable *Mirror = Module->getNamedGlobal(
        (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str());
    ASSERT_NE(Mirror, nullptr);
    std::set<const llvm::Value *> Seen;
    EXPECT_TRUE(valueReferencesSpecificGlobal(Calls.front()->getCalledOperand(),
                                              Mirror, Seen));
  }
  {
    BinaryImage Image = makeMixedPointerRecordImage(Arch::AArch64);
    MedFunc Caller =
        makeIndexedPointerTableIndirectCaller(Arch::AArch64, DataVA);
    llvm::LLVMContext Context;
    EXPECT_EQ(MedLLVMEmitter().emit(
                  {Caller, Callee}, Context, "indexed-mixed-pointer-table",
                  Arch::AArch64, {}, &Image, BinaryFormat::MachO),
              nullptr);
  }
}

TEST(LLVMCodePointerInvariantBoundary,
     MixedPointerTableNonCodeLanesFailClosed) {
  struct Case {
    uint64_t InitialOffset;
    uint64_t Stride;
    const char *Name;
  };
  const Case Cases[] = {
      {0, 24, "scalar-lane"},
      {16, 24, "data-lane"},
      {8, 8, "mixed-lanes"},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    BinaryImage Image = makeMixedPointerRecordImage(Arch::AArch64);
    MedFunc Caller = makeMixedPointerRecordIndirectCaller(
        Arch::AArch64, C.InitialOffset, C.Stride);
    MedFunc Callee = makeReturnFunction("mixed_pointer_record_target", CodeVA);

    MedLLVMEmitter Classifier;
    MedLLVMProvenanceTestPeer::prepareFreshAnalysis(
        Classifier, Caller, Image, Arch::AArch64, BinaryFormat::MachO);
    EXPECT_EQ(MedLLVMProvenanceTestPeer::recoversAbsoluteDataPointerIdentity(
                  Classifier, Caller.Blocks[1].Ops[0].Output),
              C.InitialOffset == 16 && C.Stride == 24);

    llvm::LLVMContext Context;
    EXPECT_EQ(
        MedLLVMEmitter().emit({Caller, Callee}, Context,
                              std::string("mixed-pointer-record-") + C.Name,
                              Arch::AArch64, {}, &Image, BinaryFormat::MachO),
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
     ResolvesDirectRawTableSelectionWithinOneRun) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    MedFunc Lookup = makeNoPhiSelectedTableLookup(TargetArch, /*Masked=*/false);
    auto Select = std::find_if(
        Lookup.Blocks.front().Ops.begin(), Lookup.Blocks.front().Ops.end(),
        [](const MedOp &Op) { return Op.Opcode == NdOp::SELECT; });
    ASSERT_NE(Select, Lookup.Blocks.front().Ops.end());
    Select->Inputs[1] = MedVar::makeConst(
        SpilledConstTableVA, getTargetRegInfo(TargetArch).PointerSize);
    Select->Inputs[2] = MedVar::makeConst(
        OtherSpilledConstTableVA, getTargetRegInfo(TargetArch).PointerSize);

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-direct-raw-table-selection", TargetArch, {},
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
     FailsClosedForDirectRawTableSelectionAcrossRuns) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    BinaryImage Image = makeSpilledConstTableImage(TargetArch);
    addFarSpilledConstTable(Image);
    MedFunc Lookup = makeNoPhiSelectedTableLookup(TargetArch, /*Masked=*/false);
    auto Select = std::find_if(
        Lookup.Blocks.front().Ops.begin(), Lookup.Blocks.front().Ops.end(),
        [](const MedOp &Op) { return Op.Opcode == NdOp::SELECT; });
    ASSERT_NE(Select, Lookup.Blocks.front().Ops.end());
    Select->Inputs[1] = MedVar::makeConst(
        SpilledConstTableVA, getTargetRegInfo(TargetArch).PointerSize);
    Select->Inputs[2] = MedVar::makeConst(
        FarSpilledConstTableVA, getTargetRegInfo(TargetArch).PointerSize);

    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Lookup}, Context, "macho-cross-run-direct-table-selection", TargetArch,
        {}, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module.get(), nullptr);
    EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
              std::string::npos)
        << Diagnostic;
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
      if (ControlOnly) {
        ASSERT_NE(Module, nullptr) << Diagnostic;
        expectValidModule(*Module);
        llvm::Function *Function = Module->getFunction(Lookup.Name);
        ASSERT_NE(Function, nullptr);
        const llvm::LoadInst *TableLoad = findNonAllocaI16Load(*Function);
        ASSERT_NE(TableLoad, nullptr);
        std::set<const llvm::Value *> Seen;
        EXPECT_FALSE(valueReferencesConstantGlobal(
            TableLoad->getPointerOperand(), Seen));
        EXPECT_EQ(Diagnostic.find("refusing stale-address fallback"),
                  std::string::npos)
            << Diagnostic;
      } else {
        EXPECT_EQ(Module.get(), nullptr) << IR;
        EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                  std::string::npos)
            << Diagnostic;
      }
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

MedFunc makeExactAddressIndirectCaller(Arch TargetArch) {
  const uint16_t PointerSize =
      static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);

  MedVar Target;
  Target.Kind = MedVar::Temp;
  Target.TheArch = TargetArch;
  Target.Id = 1;
  Target.SSAVer = 1;
  Target.Size = PointerSize;

  MedFunc Func;
  Func.Name = "exact_address_indirect_caller";
  Func.Entry = CallerVA;
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 0xc;

  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Addr = CallerVA;
  Copy.Output = Target;
  Copy.addInput(MedVar::makeConst(CodeVA, PointerSize,
                                  ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Copy));

  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = CallerVA + 4;
  Call.addInput(Target);
  Block.Ops.push_back(std::move(Call));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 8;
  Block.Ops.push_back(std::move(Return));

  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc
makeExplicitConstantIndirectCaller(Arch TargetArch, va_t Target,
                                   ConstantAddressProvenance TargetProvenance) {
  const uint16_t PointerSize =
      static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);

  MedFunc Func;
  Func.Name = "explicit_constant_indirect_caller";
  Func.Entry = CallerVA;
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 8;

  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = CallerVA;
  Call.addInput(MedVar::makeConst(Target, PointerSize, TargetProvenance));
  Block.Ops.push_back(std::move(Call));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 4;
  Block.Ops.push_back(std::move(Return));

  Func.Blocks.push_back(std::move(Block));
  return Func;
}

enum class ExactCodeAddressValueSink { Return, Store, CallArgument };

MedFunc makeExactCodeAddressValueSink(Arch TargetArch,
                                      ExactCodeAddressValueSink Sink) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  auto makeVar = [&](MedVar::VarKind Kind, int Id) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = PointerSize;
    return V;
  };

  MedVar CodeAddress = makeVar(MedVar::Temp, 20);
  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = Sink == ExactCodeAddressValueSink::Return
                  ? "exact_code_address_return"
              : Sink == ExactCodeAddressValueSink::Store
                  ? "exact_code_address_store"
                  : "exact_code_address_call_argument";
  Func.ReturnType = Sink == ExactCodeAddressValueSink::Return
                        ? NdType::makeInt(PointerSize)
                        : NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 0x10;

  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Addr = CallerVA;
  Copy.Output = CodeAddress;
  Copy.addInput(MedVar::makeConst(CodeVA, PointerSize,
                                  ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Copy));

  if (Sink == ExactCodeAddressValueSink::Return) {
    MedVar ReturnReg = makeVar(MedVar::Reg, 21);
    ReturnReg.RegOff = TRI.IntReturnReg;
    MedOp WriteReturn;
    WriteReturn.Opcode = NdOp::COPY;
    WriteReturn.Addr = CallerVA + 4;
    WriteReturn.Output = ReturnReg;
    WriteReturn.addInput(CodeAddress);
    Block.Ops.push_back(std::move(WriteReturn));
  } else if (Sink == ExactCodeAddressValueSink::Store) {
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.Addr = CallerVA + 4;
    Store.addInput(MedVar::makeConst(SpilledConstTableVA, PointerSize,
                                     ConstantAddressProvenance::DataAddress));
    Store.addInput(CodeAddress);
    Block.Ops.push_back(std::move(Store));
  } else {
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = CallerVA + 4;
    Call.addInput(MedVar::makeConst(ImportStubVA, PointerSize));
    Block.Ops.push_back(std::move(Call));

    MedCallInfo Info;
    Info.BlockId = 0;
    Info.OpIdx = 1;
    Info.TargetAddr = ImportStubVA;
    Info.TargetName = "consume_code_address";
    Info.Args.push_back(CodeAddress);
    Func.CallInfos.push_back(std::move(Info));
  }

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 8;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

enum class ExactCodeAddressAtomicSink { Exchange, CompareExchange };

MedFunc makeExactCodeAddressAtomicSink(Arch TargetArch,
                                       ExactCodeAddressAtomicSink Sink) {
  const uint16_t PointerSize =
      static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);

  MedVar OldValue;
  OldValue.Kind = MedVar::Temp;
  OldValue.TheArch = TargetArch;
  OldValue.Id = 30;
  OldValue.SSAVer = 1;
  OldValue.Size = PointerSize;

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = Sink == ExactCodeAddressAtomicSink::Exchange
                  ? "exact_code_address_atomic_exchange"
                  : "exact_code_address_atomic_compare_exchange";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 8;

  MedOp Atomic;
  Atomic.Opcode = Sink == ExactCodeAddressAtomicSink::Exchange
                      ? NdOp::ATOMIC_XCHG
                      : NdOp::ATOMIC_CMPXCHG;
  Atomic.Addr = CallerVA;
  Atomic.MemoryOrdering = NdMemoryOrdering::Relaxed;
  Atomic.Output = OldValue;
  Atomic.addInput(MedVar::makeConst(SpilledConstTableVA, PointerSize,
                                    ConstantAddressProvenance::DataAddress));
  Atomic.addInput(MedVar::makeConst(CodeVA, PointerSize,
                                    ConstantAddressProvenance::Address));
  if (Sink == ExactCodeAddressAtomicSink::CompareExchange)
    Atomic.addInput(MedVar::makeConst(CodeVA, PointerSize,
                                      ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Atomic));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 4;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

enum class A64ExactCodeAddressAtomicIntrinsicSink { ScalarLSE, ExclusiveStore };

MedFunc makeA64ExactCodeAddressAtomicIntrinsicSink(
    A64ExactCodeAddressAtomicIntrinsicSink Sink) {
  constexpr Arch TargetArch = Arch::AArch64;
  constexpr uint16_t PointerSize = 8;

  MedVar Result;
  Result.Kind = MedVar::Temp;
  Result.TheArch = TargetArch;
  Result.Id = 35;
  Result.SSAVer = 1;
  Result.Size = Sink == A64ExactCodeAddressAtomicIntrinsicSink::ScalarLSE
                    ? PointerSize
                    : 4;

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = Sink == A64ExactCodeAddressAtomicIntrinsicSink::ScalarLSE
                  ? "exact_code_address_a64_lse"
                  : "exact_code_address_a64_exclusive_store";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 8;

  MedOp Atomic;
  Atomic.Opcode = NdOp::INTRINSIC;
  Atomic.Addr = CallerVA;
  Atomic.MemoryOrdering = NdMemoryOrdering::Relaxed;
  Atomic.Output = Result;
  const Intrinsic Id = Sink == A64ExactCodeAddressAtomicIntrinsicSink::ScalarLSE
                           ? Intrinsic::A64_AtomicOr
                           : Intrinsic::A64_Stxr;
  Atomic.addInput(MedVar::makeConst(static_cast<uint64_t>(Id), 2,
                                    ConstantAddressProvenance::Scalar));
  Atomic.addInput(MedVar::makeConst(CodeVA, PointerSize,
                                    ConstantAddressProvenance::Address));
  Atomic.addInput(MedVar::makeConst(SpilledConstTableVA, PointerSize,
                                    ConstantAddressProvenance::DataAddress));
  Atomic.addInput(
      MedVar::makeConst(PointerSize, 2, ConstantAddressProvenance::Scalar));
  if (Sink == A64ExactCodeAddressAtomicIntrinsicSink::ScalarLSE)
    Atomic.addInput(MedVar::makeConst(0, 1, ConstantAddressProvenance::Scalar));
  Block.Ops.push_back(std::move(Atomic));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 4;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeExactCodeAddressIdentityRelations(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };

  MedVar Offset;
  Offset.Kind = MedVar::Param;
  Offset.TheArch = TargetArch;
  Offset.Id = 40;
  Offset.SSAVer = 0;
  Offset.Size = PointerSize;
  Offset.RegOff = TRI.IntParamRegs.front();

  MedVar Difference = makeTemp(41, PointerSize);
  MedVar Equal = makeTemp(42, 1);
  MedVar NotEqual = makeTemp(43, 1);
  auto exactCodeAddress = [&] {
    return MedVar::makeConst(CodeVA, PointerSize,
                             ConstantAddressProvenance::Address);
  };

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "exact_code_address_identity_relations";
  Func.ReturnType = NdType::makeVoid();
  Func.Params.push_back(Offset);

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 0x10;

  MedOp Subtract;
  Subtract.Opcode = NdOp::INT_SUB;
  Subtract.Addr = CallerVA;
  Subtract.Output = Difference;
  Subtract.addInput(exactCodeAddress());
  Subtract.addInput(Offset);
  Block.Ops.push_back(std::move(Subtract));

  MedOp CompareEqual;
  CompareEqual.Opcode = NdOp::INT_EQUAL;
  CompareEqual.Addr = CallerVA + 4;
  CompareEqual.Output = Equal;
  CompareEqual.addInput(Difference);
  CompareEqual.addInput(exactCodeAddress());
  Block.Ops.push_back(std::move(CompareEqual));

  MedOp CompareNotEqual;
  CompareNotEqual.Opcode = NdOp::INT_NOTEQUAL;
  CompareNotEqual.Addr = CallerVA + 8;
  CompareNotEqual.Output = NotEqual;
  CompareNotEqual.addInput(Difference);
  CompareNotEqual.addInput(exactCodeAddress());
  Block.Ops.push_back(std::move(CompareNotEqual));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 0xc;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeFrameReloadedExactAddressIndirectCaller(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  auto makeVar = [&](MedVar::VarKind Kind, int Id) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = PointerSize;
    return V;
  };

  MedVar StackPointer = makeVar(MedVar::Reg, 50);
  StackPointer.SSAVer = 0;
  StackPointer.RegOff = TRI.StackPointer;
  MedVar Slot = makeVar(MedVar::Temp, 51);
  MedVar Reloaded = makeVar(MedVar::Temp, 52);

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "frame_reloaded_exact_address_indirect_caller";
  Func.ReturnType = NdType::makeVoid();
  Func.FrameSize = 16;

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 0x14;

  MedOp FormSlot;
  FormSlot.Opcode = NdOp::INT_ADD;
  FormSlot.Addr = CallerVA;
  FormSlot.Output = Slot;
  FormSlot.addInput(StackPointer);
  FormSlot.addInput(MedVar::makeConst(uint64_t(-8), PointerSize,
                                      ConstantAddressProvenance::Scalar));
  Block.Ops.push_back(std::move(FormSlot));

  MedOp Spill;
  Spill.Opcode = NdOp::STORE;
  Spill.Addr = CallerVA + 4;
  Spill.addInput(Slot);
  Spill.addInput(MedVar::makeConst(CodeVA, PointerSize,
                                   ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Spill));

  MedOp Reload;
  Reload.Opcode = NdOp::LOAD;
  Reload.Addr = CallerVA + 8;
  Reload.Output = Reloaded;
  Reload.addInput(Slot);
  Block.Ops.push_back(std::move(Reload));

  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = CallerVA + 0xc;
  Call.addInput(Reloaded);
  Block.Ops.push_back(std::move(Call));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 0x10;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeFrameReloadedInteriorAddressUse(Arch TargetArch,
                                            bool AsIndirectCall) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  auto makeVar = [&](MedVar::VarKind Kind, int Id) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = PointerSize;
    return V;
  };

  MedVar StackPointer = makeVar(MedVar::Reg, 60);
  StackPointer.SSAVer = 0;
  StackPointer.RegOff = TRI.StackPointer;
  MedVar InteriorAddress = makeVar(MedVar::Temp, 61);
  MedVar Slot = makeVar(MedVar::Temp, 62);
  MedVar Reloaded = makeVar(MedVar::Temp, 63);

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = AsIndirectCall ? "frame_reloaded_interior_indirect_call"
                             : "frame_reloaded_interior_return";
  Func.ReturnType =
      AsIndirectCall ? NdType::makeVoid() : NdType::makeInt(PointerSize);
  Func.FrameSize = 16;

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 4;
  Entry.Succs = {1};
  MedOp MaterializeInterior;
  MaterializeInterior.Opcode = NdOp::COPY;
  MaterializeInterior.Addr = CallerVA;
  MaterializeInterior.Output = InteriorAddress;
  MaterializeInterior.addInput(MedVar::makeConst(
      SameFunctionInteriorVA, PointerSize, ConstantAddressProvenance::Address));
  Entry.Ops.push_back(std::move(MaterializeInterior));

  MedBlock Interior;
  Interior.Id = 1;
  Interior.StartAddr = SameFunctionInteriorVA;
  Interior.EndAddr = SameFunctionInteriorVA + 0x14;
  Interior.Preds = {0};

  MedOp FormSlot;
  FormSlot.Opcode = NdOp::INT_ADD;
  FormSlot.Addr = SameFunctionInteriorVA;
  FormSlot.Output = Slot;
  FormSlot.addInput(StackPointer);
  FormSlot.addInput(MedVar::makeConst(uint64_t(-8), PointerSize,
                                      ConstantAddressProvenance::Scalar));
  Interior.Ops.push_back(std::move(FormSlot));

  MedOp Spill;
  Spill.Opcode = NdOp::STORE;
  Spill.Addr = SameFunctionInteriorVA + 4;
  Spill.addInput(Slot);
  Spill.addInput(InteriorAddress);
  Interior.Ops.push_back(std::move(Spill));

  MedOp Reload;
  Reload.Opcode = NdOp::LOAD;
  Reload.Addr = SameFunctionInteriorVA + 8;
  Reload.Output = Reloaded;
  Reload.addInput(Slot);
  Interior.Ops.push_back(std::move(Reload));

  if (AsIndirectCall) {
    MedOp Call;
    Call.Opcode = NdOp::INDIR_CALL;
    Call.Addr = SameFunctionInteriorVA + 0xc;
    Call.addInput(Reloaded);
    Interior.Ops.push_back(std::move(Call));
  } else {
    MedVar ReturnReg = makeVar(MedVar::Reg, 64);
    ReturnReg.RegOff = TRI.IntReturnReg;
    MedOp WriteReturn;
    WriteReturn.Opcode = NdOp::COPY;
    WriteReturn.Addr = SameFunctionInteriorVA + 0xc;
    WriteReturn.Output = ReturnReg;
    WriteReturn.addInput(Reloaded);
    Interior.Ops.push_back(std::move(WriteReturn));
  }

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = SameFunctionInteriorVA + 0x10;
  Interior.Ops.push_back(std::move(Return));

  Func.Blocks.push_back(std::move(Entry));
  Func.Blocks.push_back(std::move(Interior));
  return Func;
}

MedFunc makeInteriorAddressX86FlagRelation() {
  constexpr Arch TargetArch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };

  MedVar Offset = makeVar(MedVar::Param, 65, PointerSize);
  Offset.RegOff = TRI.IntParamRegs.front();
  MedVar Sum = makeVar(MedVar::Temp, 66, PointerSize);
  MedVar Low = makeVar(MedVar::Temp, 67, 4);
  MedVar Count = makeVar(MedVar::Temp, 68, 4);
  MedVar Masked = makeVar(MedVar::Temp, 69, 4);
  MedVar Parity = makeVar(MedVar::Temp, 70, 1);
  MedVar ReturnReg = makeVar(MedVar::Reg, 71, 1);
  ReturnReg.RegOff = TRI.IntReturnReg;

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "interior_address_x86_flag_relation";
  Func.ReturnType = NdType::makeInt(1);
  Func.Params.push_back(Offset);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 4;
  Entry.Succs = {1};
  MedOp EnterInterior;
  EnterInterior.Opcode = NdOp::BRANCH;
  EnterInterior.Addr = CallerVA;
  EnterInterior.addInput(
      MedVar::makeConst(SameFunctionInteriorVA, PointerSize));
  Entry.Ops.push_back(std::move(EnterInterior));

  MedBlock Interior;
  Interior.Id = 1;
  Interior.StartAddr = SameFunctionInteriorVA;
  Interior.EndAddr = SameFunctionInteriorVA + 0x18;
  Interior.Preds = {0};

  MedOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Addr = SameFunctionInteriorVA;
  Add.Output = Sum;
  Add.addInput(MedVar::makeConst(SameFunctionInteriorVA, PointerSize,
                                 ConstantAddressProvenance::Address));
  Add.addInput(Offset);
  Interior.Ops.push_back(std::move(Add));

  MedOp ExtractLow;
  ExtractLow.Opcode = NdOp::SUBBYTES;
  ExtractLow.Addr = SameFunctionInteriorVA + 4;
  ExtractLow.Output = Low;
  ExtractLow.addInput(Sum);
  ExtractLow.addInput(
      MedVar::makeConst(0, 4, ConstantAddressProvenance::Scalar));
  Interior.Ops.push_back(std::move(ExtractLow));

  MedOp PopulationCount;
  PopulationCount.Opcode = NdOp::POPCOUNT;
  PopulationCount.Addr = SameFunctionInteriorVA + 8;
  PopulationCount.Output = Count;
  PopulationCount.addInput(Low);
  Interior.Ops.push_back(std::move(PopulationCount));

  MedOp MaskParity;
  MaskParity.Opcode = NdOp::INT_AND;
  MaskParity.Addr = SameFunctionInteriorVA + 0xc;
  MaskParity.Output = Masked;
  MaskParity.addInput(Count);
  MaskParity.addInput(
      MedVar::makeConst(1, 4, ConstantAddressProvenance::Scalar));
  Interior.Ops.push_back(std::move(MaskParity));

  MedOp IsEven;
  IsEven.Opcode = NdOp::INT_EQUAL;
  IsEven.Addr = SameFunctionInteriorVA + 0x10;
  IsEven.Output = Parity;
  IsEven.addInput(Masked);
  IsEven.addInput(MedVar::makeConst(0, 4, ConstantAddressProvenance::Scalar));
  Interior.Ops.push_back(std::move(IsEven));

  MedOp WriteReturn;
  WriteReturn.Opcode = NdOp::COPY;
  WriteReturn.Addr = SameFunctionInteriorVA + 0x14;
  WriteReturn.Output = ReturnReg;
  WriteReturn.addInput(Parity);
  Interior.Ops.push_back(std::move(WriteReturn));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = SameFunctionInteriorVA + 0x18;
  Interior.Ops.push_back(std::move(Return));

  Func.Blocks = {std::move(Entry), std::move(Interior)};
  return Func;
}

MedFunc makeExactAndAdjustedCodeIdentitySelect(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };

  MedVar Condition = makeVar(MedVar::Param, 80, 1);
  Condition.RegOff = TRI.IntParamRegs[0];
  MedVar Offset = makeVar(MedVar::Param, 81, PointerSize);
  Offset.RegOff = TRI.IntParamRegs[1];
  MedVar Exact = makeVar(MedVar::Temp, 82, PointerSize);
  MedVar Adjusted = makeVar(MedVar::Temp, 83, PointerSize);
  MedVar Selected = makeVar(MedVar::Temp, 84, PointerSize);
  MedVar ReturnReg = makeVar(MedVar::Reg, 85, PointerSize);
  ReturnReg.RegOff = TRI.IntReturnReg;

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "exact_and_adjusted_code_identity_select";
  Func.ReturnType = NdType::makeInt(PointerSize);
  Func.Params = {Condition, Offset};

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 0x14;

  MedOp CopyExact;
  CopyExact.Opcode = NdOp::COPY;
  CopyExact.Addr = CallerVA;
  CopyExact.Output = Exact;
  CopyExact.addInput(MedVar::makeConst(CodeVA, PointerSize,
                                       ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(CopyExact));

  MedOp AddOffset;
  AddOffset.Opcode = NdOp::INT_ADD;
  AddOffset.Addr = CallerVA + 4;
  AddOffset.Output = Adjusted;
  AddOffset.addInput(Exact);
  AddOffset.addInput(Offset);
  Block.Ops.push_back(std::move(AddOffset));

  MedOp Select;
  Select.Opcode = NdOp::SELECT;
  Select.Addr = CallerVA + 8;
  Select.Output = Selected;
  Select.addInput(Condition);
  Select.addInput(Exact);
  Select.addInput(Adjusted);
  Block.Ops.push_back(std::move(Select));

  MedOp WriteReturn;
  WriteReturn.Opcode = NdOp::COPY;
  WriteReturn.Addr = CallerVA + 0xc;
  WriteReturn.Output = ReturnReg;
  WriteReturn.addInput(Selected);
  Block.Ops.push_back(std::move(WriteReturn));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 0x10;
  Block.Ops.push_back(std::move(Return));

  Func.Blocks.push_back(std::move(Block));
  return Func;
}

enum class LongRecurrentCodeArm { None, Infeasible, Feasible };

enum class LongRecurrentValueSink { SupportedAdd, MixedLayoutSelect };

constexpr unsigned LongRecurrentCopyDepth = 40;
constexpr unsigned SharedScalarDiamondDepth = 24;
constexpr va_t ExecutableLayoutOnlyVA = CallerVA + 0xc0;
constexpr va_t OtherRecurrentCodeVA = CodeVA + 0x40;

MedFunc makeLongRecurrentScalarCodeReturn(Arch TargetArch,
                                          LongRecurrentCodeArm CodeArm,
                                          LongRecurrentValueSink Sink) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  int NextId = 200;
  auto makeVar = [&](MedVar::VarKind Kind, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = NextId++;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };
  auto scalar = [&](uint64_t Value, uint16_t Size) {
    return MedVar::makeConst(Value, Size, ConstantAddressProvenance::Scalar);
  };
  auto address = [&](va_t Value) {
    return MedVar::makeConst(Value, PointerSize,
                             ConstantAddressProvenance::Address);
  };

  MedVar RuntimeInit = makeVar(MedVar::Param, PointerSize);
  RuntimeInit.RegOff = TRI.IntParamRegs[0];
  MedVar SelectCondition = makeVar(MedVar::Param, 1);
  SelectCondition.RegOff = TRI.IntParamRegs[1];
  MedVar FoldedTrue = makeVar(MedVar::Temp, 1);
  MedVar Recurrent = makeVar(MedVar::Temp, PointerSize);
  MedVar Result = makeVar(MedVar::Temp, PointerSize);
  MedVar ReturnReg = makeVar(MedVar::Reg, PointerSize);
  ReturnReg.RegOff = TRI.IntReturnReg;

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = Sink == LongRecurrentValueSink::MixedLayoutSelect
                  ? "long_recurrent_mixed_layout_select"
              : CodeArm == LongRecurrentCodeArm::None
                  ? "long_recurrent_supported_add"
              : CodeArm == LongRecurrentCodeArm::Infeasible
                  ? "long_recurrent_infeasible_code_arm"
                  : "long_recurrent_feasible_code_arm";
  Func.ReturnType = NdType::makeInt(PointerSize);
  Func.Params.push_back(RuntimeInit);
  if (Sink == LongRecurrentValueSink::MixedLayoutSelect)
    Func.Params.push_back(SelectCondition);

  constexpr int EntryId = 0;
  constexpr int OtherId = 1;
  constexpr int LoopId = 2;
  constexpr int ExitId = 3;
  constexpr va_t OtherAddr = CallerVA + 0x20;
  constexpr va_t LoopAddr = CallerVA + 0x40;
  constexpr va_t ExitAddr = CallerVA + 0x80;

  MedBlock Entry;
  Entry.Id = EntryId;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0xc;
  if (CodeArm == LongRecurrentCodeArm::None) {
    Entry.Succs = {LoopId};
    MedOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.Addr = CallerVA;
    Branch.addInput(address(LoopAddr));
    Entry.Ops.push_back(std::move(Branch));
  } else {
    Entry.Succs = {LoopId, OtherId};
    MedVar Condition = RuntimeInit;
    if (CodeArm == LongRecurrentCodeArm::Infeasible) {
      MedOp Compare;
      Compare.Opcode = NdOp::INT_EQUAL;
      Compare.Addr = CallerVA;
      Compare.Output = FoldedTrue;
      Compare.addInput(scalar(1, PointerSize));
      Compare.addInput(scalar(1, PointerSize));
      Entry.Ops.push_back(std::move(Compare));
      Condition = FoldedTrue;
    }
    MedOp Branch;
    Branch.Opcode = NdOp::COND_BR;
    Branch.Addr = CallerVA + 4;
    Branch.addInput(address(LoopAddr));
    Branch.addInput(Condition);
    Entry.Ops.push_back(std::move(Branch));
  }

  MedBlock Other;
  if (CodeArm != LongRecurrentCodeArm::None) {
    Other.Id = OtherId;
    Other.StartAddr = OtherAddr;
    Other.EndAddr = OtherAddr + 4;
    Other.Preds = {EntryId};
    Other.Succs = {LoopId};
    MedOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.Addr = OtherAddr;
    Branch.addInput(address(LoopAddr));
    Other.Ops.push_back(std::move(Branch));
  }

  MedBlock Loop;
  Loop.Id = LoopId;
  Loop.StartAddr = LoopAddr;
  Loop.EndAddr = LoopAddr + 0x30;
  Loop.Preds = {EntryId, LoopId};
  if (CodeArm != LongRecurrentCodeArm::None)
    Loop.Preds.insert(Loop.Preds.begin() + 1, OtherId);
  Loop.Succs = {LoopId, ExitId};

  std::vector<MedVar> BackedgeCopies;
  BackedgeCopies.reserve(LongRecurrentCopyDepth);
  for (unsigned I = 0; I < LongRecurrentCopyDepth; ++I)
    BackedgeCopies.push_back(makeVar(MedVar::Temp, PointerSize));
  MedVar BackedgeValue = makeVar(MedVar::Temp, PointerSize);

  PhiNode Phi;
  Phi.Output = Recurrent;
  Phi.Args.push_back({EntryId, RuntimeInit});
  if (CodeArm != LongRecurrentCodeArm::None)
    Phi.Args.push_back({OtherId, address(OtherRecurrentCodeVA)});
  Phi.Args.push_back({LoopId, BackedgeValue});
  Loop.Phis.push_back(std::move(Phi));

  MedVar Forwarded = Recurrent;
  for (unsigned I = 0; I < LongRecurrentCopyDepth; ++I) {
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Addr = LoopAddr + (I % 8) * 4;
    Copy.Output = BackedgeCopies[I];
    Copy.addInput(Forwarded);
    Loop.Ops.push_back(std::move(Copy));
    Forwarded = BackedgeCopies[I];
  }

  // Do not let the depth-independent pure-forward recurrence fast path
  // discharge this cycle: the scalar-zero relation forces the code proof's
  // residual Active cut while preserving the runtime value.
  MedOp CloseCycle;
  CloseCycle.Opcode = NdOp::INT_ADD;
  CloseCycle.Addr = LoopAddr;
  CloseCycle.Output = BackedgeValue;
  CloseCycle.addInput(Forwarded);
  CloseCycle.addInput(scalar(0, PointerSize));
  Loop.Ops.push_back(std::move(CloseCycle));

  MedVar Shared = Recurrent;
  for (unsigned I = 0; I < SharedScalarDiamondDepth; ++I) {
    MedVar Next = makeVar(MedVar::Temp, PointerSize);
    MedOp Diamond;
    Diamond.Opcode = NdOp::INT_XOR;
    Diamond.Addr = LoopAddr + (I % 8) * 4;
    Diamond.Output = Next;
    Diamond.addInput(Shared);
    Diamond.addInput(Shared);
    Loop.Ops.push_back(std::move(Diamond));
    Shared = Next;
  }

  if (Sink == LongRecurrentValueSink::SupportedAdd) {
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Addr = LoopAddr + 0x20;
    Add.Output = Result;
    Add.addInput(address(CodeVA));
    Add.addInput(Shared);
    Loop.Ops.push_back(std::move(Add));
  } else {
    MedVar ScalarRelation = makeVar(MedVar::Temp, PointerSize);
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Addr = LoopAddr + 0x20;
    Add.Output = ScalarRelation;
    Add.addInput(Shared);
    Add.addInput(address(ExecutableLayoutOnlyVA));
    Loop.Ops.push_back(std::move(Add));

    MedOp Select;
    Select.Opcode = NdOp::SELECT;
    Select.Addr = LoopAddr + 0x24;
    Select.Output = Result;
    Select.addInput(SelectCondition);
    Select.addInput(address(CodeVA));
    Select.addInput(ScalarRelation);
    Loop.Ops.push_back(std::move(Select));
  }

  MedOp Backedge;
  Backedge.Opcode = NdOp::COND_BR;
  Backedge.Addr = LoopAddr + 0x28;
  Backedge.addInput(address(LoopAddr));
  Backedge.addInput(RuntimeInit);
  Loop.Ops.push_back(std::move(Backedge));

  MedBlock Exit;
  Exit.Id = ExitId;
  Exit.StartAddr = ExitAddr;
  Exit.EndAddr = ExitAddr + 8;
  Exit.Preds = {LoopId};
  MedOp WriteReturn;
  WriteReturn.Opcode = NdOp::COPY;
  WriteReturn.Addr = ExitAddr;
  WriteReturn.Output = ReturnReg;
  WriteReturn.addInput(Result);
  Exit.Ops.push_back(std::move(WriteReturn));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = ExitAddr + 4;
  Exit.Ops.push_back(std::move(Return));

  Func.Blocks.push_back(std::move(Entry));
  if (CodeArm != LongRecurrentCodeArm::None)
    Func.Blocks.push_back(std::move(Other));
  Func.Blocks.push_back(std::move(Loop));
  Func.Blocks.push_back(std::move(Exit));
  return Func;
}

enum class LongExactIdentitySink { Return, IndirectCall };

MedFunc makeLongExactIdentityRecurrence(Arch TargetArch,
                                        LongExactIdentitySink Sink) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  int NextId = 400;
  auto makeVar = [&](MedVar::VarKind Kind) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = NextId++;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = PointerSize;
    return V;
  };
  auto address = [&](va_t Value) {
    return MedVar::makeConst(Value, PointerSize,
                             ConstantAddressProvenance::Address);
  };

  MedVar Continue = makeVar(MedVar::Param);
  Continue.RegOff = TRI.IntParamRegs[0];
  MedVar Recurrent = makeVar(MedVar::Temp);
  MedVar ReturnReg = makeVar(MedVar::Reg);
  ReturnReg.RegOff = TRI.IntReturnReg;

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = Sink == LongExactIdentitySink::Return
                  ? "long_exact_identity_recurrence_return"
                  : "long_exact_identity_recurrence_call";
  Func.ReturnType = Sink == LongExactIdentitySink::Return
                        ? NdType::makeInt(PointerSize)
                        : NdType::makeVoid();
  Func.Params.push_back(Continue);

  constexpr int EntryId = 0;
  constexpr int LoopId = 1;
  constexpr int ExitId = 2;
  constexpr va_t LoopAddr = CallerVA + 0x20;
  constexpr va_t ExitAddr = CallerVA + 0x60;

  MedBlock Entry;
  Entry.Id = EntryId;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 4;
  Entry.Succs = {LoopId};
  MedOp EnterLoop;
  EnterLoop.Opcode = NdOp::BRANCH;
  EnterLoop.Addr = CallerVA;
  EnterLoop.addInput(address(LoopAddr));
  Entry.Ops.push_back(std::move(EnterLoop));

  MedBlock Loop;
  Loop.Id = LoopId;
  Loop.StartAddr = LoopAddr;
  Loop.EndAddr = LoopAddr + 0x30;
  Loop.Preds = {EntryId, LoopId};
  Loop.Succs = {LoopId, ExitId};

  std::vector<MedVar> BackedgeCopies;
  BackedgeCopies.reserve(LongRecurrentCopyDepth);
  for (unsigned I = 0; I < LongRecurrentCopyDepth; ++I)
    BackedgeCopies.push_back(makeVar(MedVar::Temp));

  PhiNode Phi;
  Phi.Output = Recurrent;
  Phi.Args = {{EntryId, address(CodeVA)}, {LoopId, BackedgeCopies.back()}};
  Loop.Phis.push_back(std::move(Phi));

  MedVar Forwarded = Recurrent;
  for (unsigned I = 0; I < LongRecurrentCopyDepth; ++I) {
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Addr = LoopAddr + (I % 8) * 4;
    Copy.Output = BackedgeCopies[I];
    Copy.addInput(Forwarded);
    Loop.Ops.push_back(std::move(Copy));
    Forwarded = BackedgeCopies[I];
  }

  if (Sink == LongExactIdentitySink::IndirectCall) {
    MedOp Call;
    Call.Opcode = NdOp::INDIR_CALL;
    Call.Addr = LoopAddr + 0x20;
    Call.addInput(Recurrent);
    Loop.Ops.push_back(std::move(Call));
  }

  MedOp Backedge;
  Backedge.Opcode = NdOp::COND_BR;
  Backedge.Addr = LoopAddr + 0x24;
  Backedge.addInput(address(LoopAddr));
  Backedge.addInput(Continue);
  Loop.Ops.push_back(std::move(Backedge));

  MedBlock Exit;
  Exit.Id = ExitId;
  Exit.StartAddr = ExitAddr;
  Exit.EndAddr = ExitAddr + 8;
  Exit.Preds = {LoopId};
  if (Sink == LongExactIdentitySink::Return) {
    MedOp WriteReturn;
    WriteReturn.Opcode = NdOp::COPY;
    WriteReturn.Addr = ExitAddr;
    WriteReturn.Output = ReturnReg;
    WriteReturn.addInput(Recurrent);
    Exit.Ops.push_back(std::move(WriteReturn));
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = ExitAddr + 4;
  Exit.Ops.push_back(std::move(Return));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Exit)};
  return Func;
}

MedFunc makeRecurrentAdjustedCodeIdentityReturn(Arch TargetArch,
                                                bool DirectConstantArm) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
  auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
    MedVar V;
    V.Kind = Kind;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = Kind == MedVar::Param ? 0 : 1;
    V.Size = Size;
    return V;
  };
  auto scalar = [&](uint64_t Value) {
    return MedVar::makeConst(Value, PointerSize,
                             ConstantAddressProvenance::Scalar);
  };

  MedVar Iterations = makeVar(MedVar::Param, 90, PointerSize);
  Iterations.RegOff = TRI.IntParamRegs[0];
  MedVar Exact = makeVar(MedVar::Temp, 91, PointerSize);
  MedVar IsZero = makeVar(MedVar::Temp, 92, 1);
  MedVar Target = makeVar(MedVar::Temp, 93, PointerSize);
  MedVar Count = makeVar(MedVar::Temp, 94, PointerSize);
  MedVar NextTarget = makeVar(MedVar::Temp, 95, PointerSize);
  MedVar NextCount = makeVar(MedVar::Temp, 96, PointerSize);
  MedVar Continue = makeVar(MedVar::Temp, 97, 1);
  MedVar Result = makeVar(MedVar::Temp, 98, PointerSize);
  MedVar ReturnReg = makeVar(MedVar::Reg, 99, PointerSize);
  ReturnReg.RegOff = TRI.IntReturnReg;
  const MedVar EntryTarget =
      DirectConstantArm ? MedVar::makeConst(CodeVA, PointerSize,
                                            ConstantAddressProvenance::Address)
                        : Exact;

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "recurrent_adjusted_code_identity_return";
  Func.ReturnType = NdType::makeInt(PointerSize);
  Func.Params.push_back(Iterations);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 0xc;
  Entry.Succs = {1, 2};
  MedOp CopyExact;
  CopyExact.Opcode = NdOp::COPY;
  CopyExact.Addr = CallerVA;
  CopyExact.Output = Exact;
  CopyExact.addInput(MedVar::makeConst(CodeVA, PointerSize,
                                       ConstantAddressProvenance::Address));
  Entry.Ops.push_back(std::move(CopyExact));
  MedOp InitialZero;
  InitialZero.Opcode = NdOp::INT_EQUAL;
  InitialZero.Addr = CallerVA + 4;
  InitialZero.Output = IsZero;
  InitialZero.addInput(Iterations);
  InitialZero.addInput(scalar(0));
  Entry.Ops.push_back(std::move(InitialZero));
  MedOp SkipLoop;
  SkipLoop.Opcode = NdOp::COND_BR;
  SkipLoop.Addr = CallerVA + 8;
  SkipLoop.addInput(MedVar::makeConst(CallerVA + 0x30, PointerSize));
  SkipLoop.addInput(IsZero);
  Entry.Ops.push_back(std::move(SkipLoop));

  MedBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = CallerVA + 0x10;
  Loop.EndAddr = CallerVA + 0x24;
  Loop.Preds = {0, 1};
  Loop.Succs = {1, 2};
  PhiNode TargetPhi;
  TargetPhi.Output = Target;
  TargetPhi.Args = {{0, EntryTarget}, {1, NextTarget}};
  Loop.Phis.push_back(std::move(TargetPhi));
  PhiNode CountPhi;
  CountPhi.Output = Count;
  CountPhi.Args = {{0, Iterations}, {1, NextCount}};
  Loop.Phis.push_back(std::move(CountPhi));
  MedOp AdvanceTarget;
  AdvanceTarget.Opcode = NdOp::INT_ADD;
  AdvanceTarget.Addr = CallerVA + 0x10;
  AdvanceTarget.Output = NextTarget;
  AdvanceTarget.addInput(Target);
  AdvanceTarget.addInput(scalar(4));
  Loop.Ops.push_back(std::move(AdvanceTarget));
  MedOp Decrement;
  Decrement.Opcode = NdOp::INT_SUB;
  Decrement.Addr = CallerVA + 0x14;
  Decrement.Output = NextCount;
  Decrement.addInput(Count);
  Decrement.addInput(scalar(1));
  Loop.Ops.push_back(std::move(Decrement));
  MedOp KeepGoing;
  KeepGoing.Opcode = NdOp::INT_NOTEQUAL;
  KeepGoing.Addr = CallerVA + 0x18;
  KeepGoing.Output = Continue;
  KeepGoing.addInput(NextCount);
  KeepGoing.addInput(scalar(0));
  Loop.Ops.push_back(std::move(KeepGoing));
  MedOp BackEdge;
  BackEdge.Opcode = NdOp::COND_BR;
  BackEdge.Addr = CallerVA + 0x1c;
  BackEdge.addInput(MedVar::makeConst(Loop.StartAddr, PointerSize));
  BackEdge.addInput(Continue);
  Loop.Ops.push_back(std::move(BackEdge));

  MedBlock Exit;
  Exit.Id = 2;
  Exit.StartAddr = CallerVA + 0x30;
  Exit.EndAddr = CallerVA + 0x38;
  Exit.Preds = {0, 1};
  PhiNode ResultPhi;
  ResultPhi.Output = Result;
  ResultPhi.Args = {{0, EntryTarget}, {1, NextTarget}};
  Exit.Phis.push_back(std::move(ResultPhi));
  MedOp WriteReturn;
  WriteReturn.Opcode = NdOp::COPY;
  WriteReturn.Addr = Exit.StartAddr;
  WriteReturn.Output = ReturnReg;
  WriteReturn.addInput(Result);
  Exit.Ops.push_back(std::move(WriteReturn));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Exit.StartAddr + 4;
  Exit.Ops.push_back(std::move(Return));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Exit)};
  return Func;
}

MedFunc makeInfeasibleOtherExactAddressPhiIndirectCaller(
    Arch TargetArch, va_t LiveTarget, va_t InfeasibleTarget) {
  const uint16_t PointerSize =
      static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
  auto makeTemp = [&](int Id, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };
  auto exactAddress = [&](va_t Address) {
    return MedVar::makeConst(Address, PointerSize,
                             ConstantAddressProvenance::Address);
  };

  MedVar FoldedTrue = makeTemp(60, 1);
  MedVar Live = makeTemp(61, PointerSize);
  MedVar Infeasible = makeTemp(62, PointerSize);
  MedVar Merged = makeTemp(63, PointerSize);

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "infeasible_other_exact_address_phi_indirect_caller";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 8;
  Entry.Succs = {1, 2};
  MedOp Compare;
  Compare.Opcode = NdOp::INT_EQUAL;
  Compare.Addr = CallerVA;
  Compare.Output = FoldedTrue;
  Compare.addInput(
      MedVar::makeConst(1, PointerSize, ConstantAddressProvenance::Scalar));
  Compare.addInput(
      MedVar::makeConst(1, PointerSize, ConstantAddressProvenance::Scalar));
  Entry.Ops.push_back(std::move(Compare));
  MedOp ChooseLive;
  ChooseLive.Opcode = NdOp::COND_BR;
  ChooseLive.Addr = CallerVA + 4;
  ChooseLive.addInput(MedVar::makeConst(CallerVA + 0x10, PointerSize));
  ChooseLive.addInput(FoldedTrue);
  Entry.Ops.push_back(std::move(ChooseLive));

  MedBlock LiveBlock;
  LiveBlock.Id = 1;
  LiveBlock.StartAddr = CallerVA + 0x10;
  LiveBlock.EndAddr = CallerVA + 0x18;
  LiveBlock.Preds = {0};
  LiveBlock.Succs = {3};
  MedOp CopyLive;
  CopyLive.Opcode = NdOp::COPY;
  CopyLive.Addr = LiveBlock.StartAddr;
  CopyLive.Output = Live;
  CopyLive.addInput(exactAddress(LiveTarget));
  LiveBlock.Ops.push_back(std::move(CopyLive));
  MedOp LiveToMerge;
  LiveToMerge.Opcode = NdOp::BRANCH;
  LiveToMerge.Addr = LiveBlock.StartAddr + 4;
  LiveToMerge.addInput(MedVar::makeConst(CallerVA + 0x30, PointerSize));
  LiveBlock.Ops.push_back(std::move(LiveToMerge));

  MedBlock InfeasibleBlock;
  InfeasibleBlock.Id = 2;
  InfeasibleBlock.StartAddr = CallerVA + 0x20;
  InfeasibleBlock.EndAddr = CallerVA + 0x28;
  InfeasibleBlock.Preds = {0};
  InfeasibleBlock.Succs = {3};
  MedOp CopyInfeasible;
  CopyInfeasible.Opcode = NdOp::COPY;
  CopyInfeasible.Addr = InfeasibleBlock.StartAddr;
  CopyInfeasible.Output = Infeasible;
  CopyInfeasible.addInput(exactAddress(InfeasibleTarget));
  InfeasibleBlock.Ops.push_back(std::move(CopyInfeasible));
  MedOp InfeasibleToMerge;
  InfeasibleToMerge.Opcode = NdOp::BRANCH;
  InfeasibleToMerge.Addr = InfeasibleBlock.StartAddr + 4;
  InfeasibleToMerge.addInput(MedVar::makeConst(CallerVA + 0x30, PointerSize));
  InfeasibleBlock.Ops.push_back(std::move(InfeasibleToMerge));

  MedBlock Merge;
  Merge.Id = 3;
  Merge.StartAddr = CallerVA + 0x30;
  Merge.EndAddr = CallerVA + 0x38;
  Merge.Preds = {1, 2};
  PhiNode Phi;
  Phi.Output = Merged;
  Phi.Args = {{1, Live}, {2, Infeasible}};
  Merge.Phis.push_back(std::move(Phi));
  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = Merge.StartAddr;
  Call.addInput(Merged);
  Merge.Ops.push_back(std::move(Call));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Merge.StartAddr + 4;
  Merge.Ops.push_back(std::move(Return));

  Func.Blocks = {std::move(Entry), std::move(LiveBlock),
                 std::move(InfeasibleBlock), std::move(Merge)};
  return Func;
}

MedFunc makeSelfRecurrentExactAddressPhiIndirectCaller(Arch TargetArch) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);

  MedVar Target;
  Target.Kind = MedVar::Temp;
  Target.TheArch = TargetArch;
  Target.Id = 70;
  Target.SSAVer = 1;
  Target.Size = PointerSize;

  MedVar NextTarget = Target;
  NextTarget.Id = 71;
  MedVar FirstCompare = Target;
  FirstCompare.Id = 72;
  FirstCompare.Size = 1;
  MedVar SecondCompare = FirstCompare;
  SecondCompare.Id = 73;
  MedVar RuntimeValue;
  RuntimeValue.Kind = MedVar::Param;
  RuntimeValue.TheArch = TargetArch;
  RuntimeValue.Id = 74;
  RuntimeValue.Size = PointerSize;
  RuntimeValue.RegOff = TRI.IntParamRegs.front();

  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "self_recurrent_exact_address_phi_indirect_caller";
  Func.ReturnType = NdType::makeVoid();
  Func.DoesNotReturn = true;
  Func.Params.push_back(RuntimeValue);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 4;
  Entry.Succs = {1};
  MedOp EnterLoop;
  EnterLoop.Opcode = NdOp::BRANCH;
  EnterLoop.Addr = CallerVA;
  EnterLoop.addInput(MedVar::makeConst(CallerVA + 0x10, PointerSize));
  Entry.Ops.push_back(std::move(EnterLoop));

  MedBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = CallerVA + 0x10;
  Loop.EndAddr = CallerVA + 0x18;
  Loop.Preds = {0, 2};
  Loop.Succs = {2};
  PhiNode Phi;
  Phi.Output = Target;
  // Keep the recurrent arm first: a structural memo must not publish a false
  // result for NextTarget merely because its first DFS reaches Target while
  // Target is active. The entry arm still supplies the unique code identity.
  Phi.Args = {{2, NextTarget},
              {0, MedVar::makeConst(CodeVA, PointerSize,
                                    ConstantAddressProvenance::Address)}};
  Loop.Phis.push_back(std::move(Phi));
  MedOp CompareTarget;
  CompareTarget.Opcode = NdOp::INT_EQUAL;
  CompareTarget.Addr = Loop.StartAddr;
  CompareTarget.Output = FirstCompare;
  CompareTarget.addInput(Target);
  CompareTarget.addInput(RuntimeValue);
  Loop.Ops.push_back(std::move(CompareTarget));
  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = Loop.StartAddr + 4;
  Call.addInput(Target);
  Loop.Ops.push_back(std::move(Call));
  MedOp ToLatch;
  ToLatch.Opcode = NdOp::BRANCH;
  ToLatch.Addr = Loop.StartAddr + 8;
  ToLatch.addInput(MedVar::makeConst(CallerVA + 0x20, PointerSize));
  Loop.Ops.push_back(std::move(ToLatch));

  MedBlock Latch;
  Latch.Id = 2;
  Latch.StartAddr = CallerVA + 0x20;
  Latch.EndAddr = CallerVA + 0x2c;
  Latch.Preds = {1};
  Latch.Succs = {1};
  MedOp AdvanceTarget;
  AdvanceTarget.Opcode = NdOp::COPY;
  AdvanceTarget.Addr = Latch.StartAddr;
  AdvanceTarget.Output = NextTarget;
  AdvanceTarget.addInput(Target);
  Latch.Ops.push_back(std::move(AdvanceTarget));
  MedOp CompareNextTarget;
  CompareNextTarget.Opcode = NdOp::INT_EQUAL;
  CompareNextTarget.Addr = Latch.StartAddr + 4;
  CompareNextTarget.Output = SecondCompare;
  CompareNextTarget.addInput(NextTarget);
  CompareNextTarget.addInput(RuntimeValue);
  Latch.Ops.push_back(std::move(CompareNextTarget));
  MedOp BackEdge;
  BackEdge.Opcode = NdOp::BRANCH;
  BackEdge.Addr = Latch.StartAddr + 8;
  BackEdge.addInput(MedVar::makeConst(Loop.StartAddr, PointerSize));
  Latch.Ops.push_back(std::move(BackEdge));

  Func.Blocks = {std::move(Entry), std::move(Loop), std::move(Latch)};
  return Func;
}

enum class ExactAddressMergeKind { Phi, Select };

MedFunc makeMergedExactAddressIndirectCaller(ExactAddressMergeKind Kind,
                                             va_t FirstTarget,
                                             va_t SecondTarget) {
  constexpr Arch TargetArch = Arch::AArch64;
  const uint16_t PointerSize =
      static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
  auto temp = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = TargetArch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = PointerSize;
    return V;
  };
  auto exactAddress = [&](va_t Address) {
    return MedVar::makeConst(Address, PointerSize,
                             ConstantAddressProvenance::Address);
  };

  MedVar Condition;
  Condition.Kind = MedVar::Param;
  Condition.TheArch = TargetArch;
  Condition.Id = 0;
  Condition.SSAVer = 0;
  Condition.Size = 1;
  Condition.RegOff = getTargetRegInfo(TargetArch).IntParamRegs.front();

  MedFunc Func;
  Func.Name = Kind == ExactAddressMergeKind::Phi
                  ? "merged_exact_address_phi_indirect_caller"
                  : "merged_exact_address_select_indirect_caller";
  Func.Entry = CallerVA;
  Func.ReturnType = NdType::makeVoid();
  Func.Params.push_back(Condition);

  MedVar Merged = temp(10);
  auto addCallAndReturn = [&](MedBlock &Block, va_t CallAddress) {
    MedOp Call;
    Call.Opcode = NdOp::INDIR_CALL;
    Call.Addr = CallAddress;
    Call.addInput(Merged);
    Block.Ops.push_back(std::move(Call));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = CallAddress + 4;
    Block.Ops.push_back(std::move(Return));
  };

  if (Kind == ExactAddressMergeKind::Select) {
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = CallerVA;
    Block.EndAddr = CallerVA + 0xc;
    MedOp Select;
    Select.Opcode = NdOp::SELECT;
    Select.Addr = CallerVA;
    Select.Output = Merged;
    Select.addInput(Condition);
    Select.addInput(exactAddress(FirstTarget));
    Select.addInput(exactAddress(SecondTarget));
    Block.Ops.push_back(std::move(Select));
    addCallAndReturn(Block, CallerVA + 4);
    Func.Blocks.push_back(std::move(Block));
    return Func;
  }

  MedVar First = temp(11);
  MedVar Second = temp(12);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = CallerVA;
  Entry.EndAddr = CallerVA + 8;
  Entry.Succs = {1, 2};
  MedOp ConditionalBranch;
  ConditionalBranch.Opcode = NdOp::COND_BR;
  ConditionalBranch.Addr = CallerVA;
  ConditionalBranch.addInput(MedVar::makeConst(CallerVA + 0x20, PointerSize));
  ConditionalBranch.addInput(Condition);
  Entry.Ops.push_back(std::move(ConditionalBranch));

  MedBlock FirstBlock;
  FirstBlock.Id = 1;
  FirstBlock.StartAddr = CallerVA + 0x10;
  FirstBlock.EndAddr = CallerVA + 0x18;
  FirstBlock.Preds = {0};
  FirstBlock.Succs = {3};
  MedOp FirstCopy;
  FirstCopy.Opcode = NdOp::COPY;
  FirstCopy.Output = First;
  FirstCopy.addInput(exactAddress(FirstTarget));
  FirstBlock.Ops.push_back(std::move(FirstCopy));
  MedOp FirstBranch;
  FirstBranch.Opcode = NdOp::BRANCH;
  FirstBranch.addInput(MedVar::makeConst(CallerVA + 0x30, PointerSize));
  FirstBlock.Ops.push_back(std::move(FirstBranch));

  MedBlock SecondBlock;
  SecondBlock.Id = 2;
  SecondBlock.StartAddr = CallerVA + 0x20;
  SecondBlock.EndAddr = CallerVA + 0x28;
  SecondBlock.Preds = {0};
  SecondBlock.Succs = {3};
  MedOp SecondCopy;
  SecondCopy.Opcode = NdOp::COPY;
  SecondCopy.Output = Second;
  SecondCopy.addInput(exactAddress(SecondTarget));
  SecondBlock.Ops.push_back(std::move(SecondCopy));
  MedOp SecondBranch;
  SecondBranch.Opcode = NdOp::BRANCH;
  SecondBranch.addInput(MedVar::makeConst(CallerVA + 0x30, PointerSize));
  SecondBlock.Ops.push_back(std::move(SecondBranch));

  MedBlock MergeBlock;
  MergeBlock.Id = 3;
  MergeBlock.StartAddr = CallerVA + 0x30;
  MergeBlock.EndAddr = CallerVA + 0x38;
  MergeBlock.Preds = {1, 2};
  PhiNode Phi;
  Phi.Output = Merged;
  Phi.Args = {{1, First}, {2, Second}};
  MergeBlock.Phis.push_back(std::move(Phi));
  addCallAndReturn(MergeBlock, CallerVA + 0x30);

  Func.Blocks = {std::move(Entry), std::move(FirstBlock),
                 std::move(SecondBlock), std::move(MergeBlock)};
  return Func;
}

TEST(LLVMDataPointerInvariantBoundary,
     ReentrantFeasibleEdgeBuildPublishesStablePhiClasses) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      MedFunc Lookup =
          makeReentrantFeasibleEdgeRecurrentTableLookup(TargetArch);
      MedLLVMEmitter Emitter;
      MedLLVMProvenanceTestPeer::prepareFreshAnalysis(Emitter, Lookup, Image,
                                                      TargetArch, Format);

      const PhiNode &Phi = Lookup.Blocks[1].Phis.front();
      const MedVar &BackedgeValue = Phi.Args[1].second;

      MedLLVMEmitter BuildingEmitter;
      MedLLVMProvenanceTestPeer::prepareFreshAnalysis(
          BuildingEmitter, Lookup, Image, TargetArch, Format);
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::buildingEdgeQueryIsProvisional(
          BuildingEmitter, Phi, 2));

      // Force a query after the entry's constant condition has already
      // pruned 0->dead in the same build. The sticky transaction fallback
      // must rebuild from structure, restore that earlier edge, and discard
      // every dependent memo poisoned by the provisional query.
      MedLLVMEmitter TransactionalEmitter;
      MedLLVMProvenanceTestPeer::prepareFreshAnalysis(
          TransactionalEmitter, Lookup, Image, TargetArch, Format);
      bool ObservedProvisionalQuery = false;
      MedLLVMProvenanceTestPeer::installTransactionalReentryProbe(
          TransactionalEmitter, Phi, 2, BackedgeValue,
          ObservedProvisionalQuery);
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::edgeIsProven(TransactionalEmitter,
                                                          Phi, 2));
      EXPECT_TRUE(ObservedProvisionalQuery);
      EXPECT_TRUE(
          MedLLVMProvenanceTestPeer::feasibleDependentCachesAreInvalidated(
              TransactionalEmitter));
      EXPECT_TRUE(
          MedLLVMProvenanceTestPeer::edgeIsProven(TransactionalEmitter, Phi, 3))
          << "late re-entry must restore an edge pruned earlier in the build";
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::incomingIsRecurrent(
          TransactionalEmitter, Phi, 2, BackedgeValue));
      EXPECT_TRUE(
          MedLLVMProvenanceTestPeer::selfRecurrent(TransactionalEmitter, Phi));

      // The first query builds feasible edges. The second must observe the
      // same completed answer, not a transient classification memoized by a
      // provenance query that re-entered the build.
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::edgeIsProven(Emitter, Phi, 2));
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::edgeIsProven(Emitter, Phi, 2));
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::incomingIsRecurrent(
          Emitter, Phi, 2, BackedgeValue));
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::selfRecurrent(Emitter, Phi));

      // Preserve the three-state policy: the dead block still has a valid CFG
      // edge, while predecessor 99 has no structural edge at all.
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::edgeIsInfeasible(Emitter, Phi, 3));
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::edgeIsUnknown(Emitter, Phi, 99));
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     UntaggedZeroRemainsControlNullDespiteAddressTargetsAtVAZero) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image;
      Image.Arch = TargetArch;
      Image.Format = Format;
      Image.Bits = Bitness::Bits64;
      Image.WritableRelocDataAddrs.insert(0);
      Image.CodeRefTargets.insert(0);

      MedFunc Func;
      Func.Entry = CallerVA;
      Func.Name = "zero_control_relocation_boundary";
      MedLLVMEmitter Emitter;
      MedLLVMProvenanceTestPeer::prepareFreshAnalysis(Emitter, Func, Image,
                                                      TargetArch, Format);
      const uint16_t PointerSize =
          static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);

      EXPECT_FALSE(MedLLVMProvenanceTestPeer::controlConstantMayRelocate(
          Emitter, MedVar::makeConst(0, PointerSize)));
      EXPECT_FALSE(MedLLVMProvenanceTestPeer::controlConstantMayRelocate(
          Emitter, MedVar::makeConst(0, PointerSize,
                                     ConstantAddressProvenance::Scalar)));
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::controlConstantMayRelocate(
          Emitter,
          MedVar::makeConst(0, PointerSize,
                            ConstantAddressProvenance::DataAddress, 0)));
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::controlConstantMayRelocate(
          Emitter, MedVar::makeConst(0, PointerSize,
                                     ConstantAddressProvenance::CodeAddress)));
    }
}

TEST(LowToMedRelocationInvariantBoundary,
     ConstantPropagationPreservesRelocatableAddressAlgebraAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      Segment LowBss;
      LowBss.Name = ".bss";
      LowBss.VA = 0x400;
      LowBss.Size = 0x80;
      LowBss.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Image.Segments.push_back(std::move(LowBss));
      Segment LowRodata;
      LowRodata.Name = ".low.rodata";
      LowRodata.VA = 0x200;
      LowRodata.Size = 0x100;
      LowRodata.FileSz = LowRodata.Size;
      LowRodata.Flags = SegmentFlags::Readable;
      LowRodata.Data.assign(LowRodata.Size, 0);
      Image.Segments.push_back(std::move(LowRodata));
      Image.RelocDataAddrs.insert(0x248);
      LowToMedConverter Converter;
      Converter.setBinaryImage(&Image);
      MedFunc Func = Converter.convert(
          makeRelocationSensitiveConstantFoldFunction(TargetArch), TargetArch,
          Format);
      ASSERT_EQ(Func.Blocks.size(), 1U);

      auto opAt = [&](va_t Addr) -> const MedOp * {
        const auto &Ops = Func.Blocks.front().Ops;
        auto It = std::find_if(Ops.begin(), Ops.end(), [&](const MedOp &Op) {
          return Op.Addr == Addr;
        });
        EXPECT_NE(It, Ops.end());
        return It == Ops.end() ? nullptr : &*It;
      };
      auto expectCopyConstant = [&](va_t Addr, uint64_t Value,
                                    ConstantAddressProvenance Provenance =
                                        ConstantAddressProvenance::Unknown) {
        const MedOp *Op = opAt(Addr);
        ASSERT_NE(Op, nullptr);
        EXPECT_EQ(Op->Opcode, NdOp::COPY);
        ASSERT_EQ(Op->NumInputs, 1U);
        EXPECT_TRUE(Op->Inputs[0].isConst());
        EXPECT_EQ(Op->Inputs[0].ConstVal, Value);
        EXPECT_EQ(Op->Inputs[0].Provenance, Provenance);
      };
      auto expectBinary = [&](va_t Addr, NdOp Opcode) {
        const MedOp *Op = opAt(Addr);
        ASSERT_NE(Op, nullptr);
        EXPECT_EQ(Op->Opcode, Opcode);
        EXPECT_EQ(Op->NumInputs, 2U);
      };

      // Preserve canonical address construction and ordinary integer folding,
      // while retaining every operation that would otherwise freeze a
      // relation between rebuilt symbols or consume their pointer bit pattern.
      expectCopyConstant(CallerVA, SpilledConstTableVA,
                         ConstantAddressProvenance::Address);
      expectCopyConstant(CallerVA + 4, OtherSpilledConstTableVA,
                         ConstantAddressProvenance::Address);
      expectBinary(CallerVA + 8, NdOp::INT_SUB);
      expectCopyConstant(CallerVA + 0xC, SpilledConstTableVA - 8,
                         ConstantAddressProvenance::Address);
      expectCopyConstant(CallerVA + 0x10, 42);
      expectCopyConstant(CallerVA + 0x14, 38);
      expectCopyConstant(CallerVA + 0x18, SpilledConstTableVA + 8,
                         ConstantAddressProvenance::Address);
      expectBinary(CallerVA + 0x1C, NdOp::INT_SUB);
      expectBinary(CallerVA + 0x20, NdOp::INT_ADD);
      expectBinary(CallerVA + 0x24, NdOp::INT_AND);
      expectBinary(CallerVA + 0x28, NdOp::INT_MULT);
      expectBinary(CallerVA + 0x2C, NdOp::INT_ADD);
      expectCopyConstant(CallerVA + 0x30, 0x410,
                         ConstantAddressProvenance::Address);
      expectCopyConstant(CallerVA + 0x34, 0x430,
                         ConstantAddressProvenance::Address);
      expectBinary(CallerVA + 0x38, NdOp::INT_SUB);
      expectCopyConstant(CallerVA + 0x3C, 0x480,
                         ConstantAddressProvenance::Address);
      expectBinary(CallerVA + 0x40, NdOp::INT_SUB);
      expectCopyConstant(CallerVA + 0x44, 0x20);
      expectCopyConstant(CallerVA + 0x48, 0x410,
                         ConstantAddressProvenance::Address);
      expectBinary(CallerVA + 0x4C, NdOp::INT_ADD);
      expectBinary(CallerVA + 0x50, NdOp::INT_ADD);
      const MedOp *CompletedPageAddress = opAt(CallerVA + 0x54);
      ASSERT_NE(CompletedPageAddress, nullptr);
      EXPECT_EQ(CompletedPageAddress->Opcode, NdOp::COPY);
      ASSERT_EQ(CompletedPageAddress->NumInputs, 1U);
      EXPECT_TRUE(CompletedPageAddress->Inputs[0].isConst());
      EXPECT_EQ(CompletedPageAddress->Inputs[0].ConstVal, 0x248U);
      EXPECT_TRUE(
          isExactAddressProvenance(CompletedPageAddress->Inputs[0].Provenance));
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     ResolvesExactGenericAddressAtIndirectCallUseAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));

      MedFunc Caller = makeExactAddressIndirectCaller(TargetArch);
      MedFunc Callee = makeReturnFunction("exact_address_callee", CodeVA);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                          "exact-address-indirect-call",
                                          TargetArch, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
      llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
      ASSERT_NE(EmittedCaller, nullptr);
      ASSERT_NE(EmittedCallee, nullptr);
      std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
      ASSERT_EQ(Calls.size(), 1u);

      llvm::Value *CalledOperand = Calls.front()->getCalledOperand();
      std::set<const llvm::Value *> SeenTargets;
      EXPECT_TRUE(
          valueReferencesTarget(CalledOperand, EmittedCallee, SeenTargets));
      std::set<const llvm::Value *> SeenIntegers;
      EXPECT_FALSE(valueReferencesInteger(CalledOperand, CodeVA, SeenIntegers));
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     MaterializesExactGenericCodeAddressAtObservableValueSinksAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64})
      for (ExactCodeAddressValueSink Sink :
           {ExactCodeAddressValueSink::Return, ExactCodeAddressValueSink::Store,
            ExactCodeAddressValueSink::CallArgument}) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        SCOPED_TRACE(static_cast<unsigned>(Sink));
        BinaryImage Image = makeWritablePointerTableImage(TargetArch, Format);
        ASSERT_TRUE(Image.isCodeAddress(CodeVA));

        MedFunc Caller = makeExactCodeAddressValueSink(TargetArch, Sink);
        MedFunc Callee =
            makeReturnFunction("observable_code_address_callee", CodeVA);
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                            "observable-exact-code-address",
                                            TargetArch, {}, &Image, Format);
        ASSERT_NE(Module, nullptr);
        expectValidModule(*Module);

        llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
        llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
        ASSERT_NE(EmittedCaller, nullptr);
        ASSERT_NE(EmittedCallee, nullptr);

        llvm::Value *Observed = nullptr;
        if (Sink == ExactCodeAddressValueSink::Return) {
          for (llvm::BasicBlock &Block : *EmittedCaller)
            if (auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator()))
              if (Return->getReturnValue())
                Observed = Return->getReturnValue();
        } else if (Sink == ExactCodeAddressValueSink::Store) {
          for (llvm::BasicBlock &Block : *EmittedCaller)
            for (llvm::Instruction &Instruction : Block)
              if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
                Observed = Store->getValueOperand();
        } else {
          std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
          ASSERT_EQ(Calls.size(), 1u);
          ASSERT_EQ(Calls.front()->arg_size(), 1u);
          Observed = Calls.front()->getArgOperand(0);
        }
        ASSERT_NE(Observed, nullptr);
        std::set<const llvm::Value *> SeenTargets;
        EXPECT_TRUE(
            valueReferencesTarget(Observed, EmittedCallee, SeenTargets));
        std::set<const llvm::Value *> SeenIntegers;
        EXPECT_FALSE(valueReferencesInteger(Observed, CodeVA, SeenIntegers));
      }
}

TEST(LLVMCodePointerInvariantBoundary,
     ImportedObjectNameDoesNotClaimGenericDataAddressAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      const uint16_t PointerSize =
          static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      ASSERT_TRUE(Image.hasObjectDataProvenance(SpilledConstTableVA));
      ASSERT_FALSE(Image.isCodeAddress(SpilledConstTableVA));

      MedFunc Caller = makeExactCodeAddressValueSink(
          TargetArch, ExactCodeAddressValueSink::CallArgument);
      ASSERT_FALSE(Caller.Blocks.empty());
      ASSERT_FALSE(Caller.Blocks.front().Ops.empty());
      MedOp &Materialize = Caller.Blocks.front().Ops.front();
      ASSERT_EQ(Materialize.Opcode, NdOp::COPY);
      ASSERT_EQ(Materialize.NumInputs, 1u);
      Materialize.Inputs[0] = MedVar::makeConst(
          SpilledConstTableVA, PointerSize, ConstantAddressProvenance::Address);

      // A declaration with the same name must not let the mixed import-name
      // map reinterpret this data bind/IAT slot as that function's entry.
      MedFunc SameNamedFunction =
          makeReturnFunction("imported_typeinfo_object", CodeVA);
      const std::vector<std::pair<va_t, std::string>> Imports = {
          {SpilledConstTableVA, "imported_typeinfo_object"},
          {ImportStubVA, "consume_code_address"}};
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Caller, SameNamedFunction}, Context,
                                "imported-object-address-call-argument",
                                TargetArch, Imports, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
      ASSERT_NE(EmittedCaller, nullptr);
      std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
      ASSERT_EQ(Calls.size(), 1u);
      ASSERT_EQ(Calls.front()->arg_size(), 1u);
      llvm::Value *Argument = Calls.front()->getArgOperand(0);
      std::set<const llvm::Value *> SeenGlobals;
      EXPECT_TRUE(valueReferencesMaterializedGlobal(Argument, SeenGlobals));
      llvm::Function *FunctionWithSameName =
          Module->getFunction(SameNamedFunction.Name);
      ASSERT_NE(FunctionWithSameName, nullptr);
      std::set<const llvm::Value *> SeenFunctions;
      EXPECT_FALSE(
          valueReferencesTarget(Argument, FunctionWithSameName, SeenFunctions));
      std::set<const llvm::Value *> SeenIntegers;
      EXPECT_FALSE(
          valueReferencesInteger(Argument, SpilledConstTableVA, SeenIntegers));
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     MachOExecutableSegmentHeaderUsesDataIdentityAtObservableSink) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    const uint16_t PointerSize =
        static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
    BinaryImage Image =
        makeSpilledConstTableImage(TargetArch, BinaryFormat::MachO);
    ASSERT_NE(Image.getSegmentFor(TextVA), nullptr);
    ASSERT_EQ(Image.getSectionFor(TextVA), nullptr);
    EXPECT_FALSE(Image.isCodeAddress(TextVA));
    EXPECT_TRUE(Image.isCodeAddress(CallerVA));

    MedFunc Caller = makeExactCodeAddressValueSink(
        TargetArch, ExactCodeAddressValueSink::CallArgument);
    ASSERT_FALSE(Caller.Blocks.empty());
    ASSERT_FALSE(Caller.Blocks.front().Ops.empty());
    MedOp &Materialize = Caller.Blocks.front().Ops.front();
    ASSERT_EQ(Materialize.Opcode, NdOp::COPY);
    ASSERT_EQ(Materialize.NumInputs, 1u);
    Materialize.Inputs[0] = MedVar::makeConst(
        TextVA, PointerSize, ConstantAddressProvenance::Address);

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Caller}, Context, "macho-image-header-call-argument", TargetArch,
        {{ImportStubVA, "consume_code_address"}}, &Image, BinaryFormat::MachO);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
    ASSERT_NE(EmittedCaller, nullptr);
    std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
    ASSERT_EQ(Calls.size(), 1u);
    ASSERT_EQ(Calls.front()->arg_size(), 1u);
    llvm::Value *Argument = Calls.front()->getArgOperand(0);
    std::set<const llvm::Value *> SeenGlobals;
    EXPECT_TRUE(valueReferencesMaterializedGlobal(Argument, SeenGlobals));
    std::set<const llvm::Value *> SeenIntegers;
    EXPECT_FALSE(valueReferencesInteger(Argument, TextVA, SeenIntegers));

    BinaryImage Sectionless = Image;
    Sectionless.Sections.clear();
    EXPECT_TRUE(Sectionless.isCodeAddress(TextVA));
  }
}

TEST(LLVMCodePointerInvariantBoundary,
     ELFAndCOFFExecutableLoadSectionsKeepDataAndCodeIdentity) {
  for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      const uint16_t PointerSize =
          static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

      auto Text = std::find_if(
          Image.Segments.begin(), Image.Segments.end(),
          [](const Segment &Seg) { return Seg.contains(CallerVA); });
      auto Data = std::find_if(
          Image.Segments.begin(), Image.Segments.end(),
          [](const Segment &Seg) { return Seg.contains(SpilledConstTableVA); });
      ASSERT_NE(Text, Image.Segments.end());
      ASSERT_NE(Data, Image.Segments.end());
      ASSERT_NE(Text, Data);
      ASSERT_GE(SpilledConstTableVA, Text->VA);
      const size_t DataOffset =
          static_cast<size_t>(SpilledConstTableVA - Text->VA);
      const uint64_t CombinedSize =
          std::max<uint64_t>(Text->Size, DataOffset + Data->Size);
      const std::vector<uint8_t> DataBytes = Data->Data;
      Text->Size = CombinedSize;
      Text->FileSz = CombinedSize;
      Text->Data.resize(static_cast<size_t>(CombinedSize));
      ASSERT_LE(DataOffset + DataBytes.size(), Text->Data.size());
      std::copy(DataBytes.begin(), DataBytes.end(),
                Text->Data.begin() + static_cast<ptrdiff_t>(DataOffset));
      Image.Segments.erase(Data);

      ASSERT_EQ(Image.getSegmentFor(SpilledConstTableVA),
                Image.getSegmentFor(CodeVA));
      ASSERT_NE(Image.getSectionFor(SpilledConstTableVA), nullptr);
      ASSERT_NE(Image.getSectionFor(CodeVA), nullptr);
      ASSERT_FALSE(Image.isCodeAddress(SpilledConstTableVA));
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));
      ASSERT_TRUE(Image.hasObjectDataProvenance(SpilledConstTableVA));

      MedFunc DataCaller = makeExactCodeAddressValueSink(
          TargetArch, ExactCodeAddressValueSink::CallArgument);
      DataCaller.Name += "_rx_data_section";
      ASSERT_FALSE(DataCaller.Blocks.empty());
      ASSERT_FALSE(DataCaller.Blocks.front().Ops.empty());
      MedOp &MaterializeData = DataCaller.Blocks.front().Ops.front();
      ASSERT_EQ(MaterializeData.Opcode, NdOp::COPY);
      ASSERT_EQ(MaterializeData.NumInputs, 1u);
      MaterializeData.Inputs[0] = MedVar::makeConst(
          SpilledConstTableVA, PointerSize, ConstantAddressProvenance::Address);

      llvm::LLVMContext DataContext;
      auto DataModule = MedLLVMEmitter().emit(
          {DataCaller}, DataContext, "rx-data-section-observable-address",
          TargetArch, {{ImportStubVA, "consume_code_address"}}, &Image, Format);
      ASSERT_NE(DataModule, nullptr);
      expectValidModule(*DataModule);
      llvm::Function *EmittedDataCaller =
          DataModule->getFunction(DataCaller.Name);
      ASSERT_NE(EmittedDataCaller, nullptr);
      std::vector<llvm::CallInst *> DataCalls = callsIn(*EmittedDataCaller);
      ASSERT_EQ(DataCalls.size(), 1u);
      ASSERT_EQ(DataCalls.front()->arg_size(), 1u);
      llvm::Value *DataArgument = DataCalls.front()->getArgOperand(0);
      std::set<const llvm::Value *> SeenDataGlobals;
      EXPECT_TRUE(
          valueReferencesMaterializedGlobal(DataArgument, SeenDataGlobals));
      std::set<const llvm::Value *> SeenDataIntegers;
      EXPECT_FALSE(valueReferencesInteger(DataArgument, SpilledConstTableVA,
                                          SeenDataIntegers));

      MedFunc CodeCaller = makeExactCodeAddressValueSink(
          TargetArch, ExactCodeAddressValueSink::CallArgument);
      CodeCaller.Name += "_rx_code_section";
      MedFunc Callee = makeReturnFunction("rx_section_code_callee", CodeVA);
      llvm::LLVMContext CodeContext;
      auto CodeModule = MedLLVMEmitter().emit(
          {CodeCaller, Callee}, CodeContext,
          "rx-code-section-observable-address", TargetArch,
          {{ImportStubVA, "consume_code_address"}}, &Image, Format);
      ASSERT_NE(CodeModule, nullptr);
      expectValidModule(*CodeModule);
      llvm::Function *EmittedCodeCaller =
          CodeModule->getFunction(CodeCaller.Name);
      llvm::Function *EmittedCallee = CodeModule->getFunction(Callee.Name);
      ASSERT_NE(EmittedCodeCaller, nullptr);
      ASSERT_NE(EmittedCallee, nullptr);
      std::vector<llvm::CallInst *> CodeCalls = callsIn(*EmittedCodeCaller);
      ASSERT_EQ(CodeCalls.size(), 1u);
      ASSERT_EQ(CodeCalls.front()->arg_size(), 1u);
      llvm::Value *CodeArgument = CodeCalls.front()->getArgOperand(0);
      std::set<const llvm::Value *> SeenCodeTargets;
      EXPECT_TRUE(
          valueReferencesTarget(CodeArgument, EmittedCallee, SeenCodeTargets));
      std::set<const llvm::Value *> SeenCodeGlobals;
      EXPECT_FALSE(
          valueReferencesMaterializedGlobal(CodeArgument, SeenCodeGlobals));
      std::set<const llvm::Value *> SeenCodeIntegers;
      EXPECT_FALSE(
          valueReferencesInteger(CodeArgument, CodeVA, SeenCodeIntegers));
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     X86RipRelativeLeaUsesMachOSectionCodeAuthority) {
  BinaryImage Image =
      makeSpilledConstTableImage(Arch::X64, BinaryFormat::MachO);
  Section CString;
  CString.Name = "__cstring";
  CString.SegmentName = "__TEXT";
  CString.VA = CStringVA;
  CString.Size = 0x10;
  CString.FileSz = CString.Size;
  CString.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  CString.Type = llvm::MachO::S_CSTRING_LITERALS;
  Image.Sections.push_back(std::move(CString));
  ASSERT_FALSE(Image.isCodeAddress(CStringVA));
  ASSERT_TRUE(Image.isCodeAddress(CodeVA));

  std::vector<uint8_t> Bytes = {
      0x48, 0x8d, 0x05, 0, 0, 0, 0, // lea CStringVA(%rip), %rax
      0x48, 0x8d, 0x0d, 0, 0, 0, 0, // lea CodeVA(%rip), %rcx
      0xc3};                        // ret
  const int32_t CStringDisp =
      static_cast<int32_t>(CStringVA - (CallerVA + static_cast<va_t>(7)));
  const int32_t CodeDisp =
      static_cast<int32_t>(CodeVA - (CallerVA + static_cast<va_t>(14)));
  std::memcpy(Bytes.data() + 3, &CStringDisp, sizeof(CStringDisp));
  std::memcpy(Bytes.data() + 10, &CodeDisp, sizeof(CodeDisp));
  Segment *Text = nullptr;
  for (Segment &Seg : Image.Segments)
    if (Seg.contains(CallerVA)) {
      Text = &Seg;
      break;
    }
  ASSERT_NE(Text, nullptr);
  ASSERT_LE(CallerVA - Text->VA + Bytes.size(), Text->Data.size());
  std::copy(Bytes.begin(), Bytes.end(),
            Text->Data.begin() + static_cast<ptrdiff_t>(CallerVA - Text->VA));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64, InstructionMode::Default));
  CFGBuilder Builder;
  LowFunc Function = Builder.build(Image, Dec, CallerVA, "rip_lea_authority");
  EXPECT_EQ(std::find(Function.CodeRefTargets.begin(),
                      Function.CodeRefTargets.end(), CStringVA),
            Function.CodeRefTargets.end());
  EXPECT_NE(std::find(Function.CodeRefTargets.begin(),
                      Function.CodeRefTargets.end(), CodeVA),
            Function.CodeRefTargets.end());
}

TEST(LLVMCodePointerInvariantBoundary,
     AuthenticatedImportVeneerOwnsRelocatableCodeArithmetic) {
  for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
    const uint16_t PointerSize =
        static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
    BinaryImage Image =
        makeSpilledConstTableImage(TargetArch, BinaryFormat::MachO);
    for (Section &Sec : Image.Sections)
      if (Sec.Name == "__text") {
        Sec.Size = ImportStubVA - Sec.VA;
        Sec.FileSz = Sec.Size;
      }
    ASSERT_FALSE(Image.isCodeAddress(ImportStubVA));
    addImport(Image, "authenticated_import_veneer", SpilledConstTableVA);
    ASSERT_TRUE(Image.recordImportStub(ImportStubVA, Image.Imports.size() - 1));
    ASSERT_TRUE(Image.isImportStubAt(ImportStubVA));

    MedFunc Caller = makeExactCodeAddressValueSink(
        TargetArch, ExactCodeAddressValueSink::Return);
    ASSERT_GE(Caller.Blocks.front().Ops.size(), 2u);
    MedOp &Adjustment = Caller.Blocks.front().Ops.front();
    ASSERT_EQ(Adjustment.Opcode, NdOp::COPY);
    Adjustment.Opcode = NdOp::INT_ADD;
    Adjustment.NumInputs = 0;
    Adjustment.addInput(MedVar::makeConst(ImportStubVA, PointerSize,
                                          ConstantAddressProvenance::Address));
    Adjustment.addInput(
        MedVar::makeConst(4, PointerSize, ConstantAddressProvenance::Scalar));

    const std::vector<std::pair<va_t, std::string>> Imports = {
        {ImportStubVA, "authenticated_import_veneer"}};
    MedFunc FunctionOwner =
        makeReturnFunction("authenticated_import_veneer", CodeVA);
    llvm::LLVMContext ResolvedContext;
    auto Resolved =
        MedLLVMEmitter().emit({Caller, FunctionOwner}, ResolvedContext,
                              "authenticated-import-veneer-arithmetic",
                              TargetArch, Imports, &Image, BinaryFormat::MachO);
    ASSERT_NE(Resolved, nullptr);
    expectValidModule(*Resolved);
    llvm::Function *EmittedCaller = Resolved->getFunction(Caller.Name);
    llvm::Function *EmittedOwner = Resolved->getFunction(FunctionOwner.Name);
    ASSERT_NE(EmittedCaller, nullptr);
    ASSERT_NE(EmittedOwner, nullptr);
    bool SawFunctionIdentity = false;
    for (llvm::BasicBlock &Block : *EmittedCaller)
      for (llvm::Instruction &Instruction : Block) {
        std::set<const llvm::Value *> SeenTargets;
        SawFunctionIdentity |=
            valueReferencesTarget(&Instruction, EmittedOwner, SeenTargets);
        std::set<const llvm::Value *> SeenIntegers;
        EXPECT_FALSE(
            valueReferencesInteger(&Instruction, ImportStubVA, SeenIntegers));
      }
    EXPECT_TRUE(SawFunctionIdentity);

    llvm::LLVMContext UnresolvedContext;
    testing::internal::CaptureStderr();
    auto Unresolved = MedLLVMEmitter().emit(
        {Caller}, UnresolvedContext, "unresolved-import-veneer-arithmetic",
        TargetArch, Imports, &Image, BinaryFormat::MachO);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Unresolved, nullptr);
    EXPECT_NE(Diagnostic.find("no unique lifted code identity"),
              std::string::npos)
        << Diagnostic;

    MedFunc Unsupported = Caller;
    ASSERT_FALSE(Unsupported.Blocks.empty());
    ASSERT_FALSE(Unsupported.Blocks.front().Ops.empty());
    Unsupported.Blocks.front().Ops.front().Opcode = NdOp::FLOAT_ADD;
    llvm::LLVMContext UnsupportedContext;
    testing::internal::CaptureStderr();
    auto UnsupportedModule =
        MedLLVMEmitter().emit({Unsupported}, UnsupportedContext,
                              "unsupported-import-veneer-transform", TargetArch,
                              Imports, &Image, BinaryFormat::MachO);
    Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(UnsupportedModule, nullptr);
    EXPECT_NE(Diagnostic.find("no unique lifted code identity"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(LLVMCodePointerInvariantBoundary,
     MaterializesExactGenericCodeAddressAtAtomicValueSinksAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64})
      for (ExactCodeAddressAtomicSink Sink :
           {ExactCodeAddressAtomicSink::Exchange,
            ExactCodeAddressAtomicSink::CompareExchange}) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        SCOPED_TRACE(static_cast<unsigned>(Sink));
        BinaryImage Image = makeWritablePointerTableImage(TargetArch, Format);
        ASSERT_TRUE(Image.isCodeAddress(CodeVA));

        MedFunc Caller = makeExactCodeAddressAtomicSink(TargetArch, Sink);
        MedFunc Callee =
            makeReturnFunction("atomic_code_address_callee", CodeVA);
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                            "atomic-exact-code-address",
                                            TargetArch, {}, &Image, Format);
        ASSERT_NE(Module, nullptr);
        expectValidModule(*Module);

        llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
        llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
        ASSERT_NE(EmittedCaller, nullptr);
        ASSERT_NE(EmittedCallee, nullptr);

        std::vector<llvm::Value *> Observed;
        for (llvm::BasicBlock &Block : *EmittedCaller)
          for (llvm::Instruction &Instruction : Block) {
            if (auto *RMW = llvm::dyn_cast<llvm::AtomicRMWInst>(&Instruction))
              Observed.push_back(RMW->getValOperand());
            if (auto *CompareExchange =
                    llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&Instruction)) {
              Observed.push_back(CompareExchange->getCompareOperand());
              Observed.push_back(CompareExchange->getNewValOperand());
            }
          }
        ASSERT_EQ(Observed.size(),
                  Sink == ExactCodeAddressAtomicSink::Exchange ? 1u : 2u);
        for (llvm::Value *Value : Observed) {
          EXPECT_TRUE(isPtrToIntValue(Value));
          std::set<const llvm::Value *> SeenTargets;
          EXPECT_TRUE(valueReferencesTarget(Value, EmittedCallee, SeenTargets));
          std::set<const llvm::Value *> SeenIntegers;
          EXPECT_FALSE(valueReferencesInteger(Value, CodeVA, SeenIntegers));
        }
      }
}

TEST(
    LLVMCodePointerInvariantBoundary,
    MaterializesExactGenericCodeAddressAtA64AtomicIntrinsicValueSinksAcrossFormats) {
  constexpr Arch TargetArch = Arch::AArch64;
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (A64ExactCodeAddressAtomicIntrinsicSink Sink :
         {A64ExactCodeAddressAtomicIntrinsicSink::ScalarLSE,
          A64ExactCodeAddressAtomicIntrinsicSink::ExclusiveStore}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(static_cast<unsigned>(Sink));
      BinaryImage Image = makeWritablePointerTableImage(TargetArch, Format);
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));

      MedFunc Caller = makeA64ExactCodeAddressAtomicIntrinsicSink(Sink);
      MedFunc Callee = makeReturnFunction(
          "a64_atomic_intrinsic_code_address_callee", CodeVA);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                          "a64-atomic-exact-code-address",
                                          TargetArch, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
      llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
      ASSERT_NE(EmittedCaller, nullptr);
      ASSERT_NE(EmittedCallee, nullptr);

      llvm::Value *Observed = nullptr;
      for (llvm::BasicBlock &Block : *EmittedCaller)
        for (llvm::Instruction &Instruction : Block) {
          if (auto *RMW = llvm::dyn_cast<llvm::AtomicRMWInst>(&Instruction)) {
            ASSERT_EQ(Observed, nullptr);
            Observed = RMW->getValOperand();
          }
          if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
            if (const llvm::Function *Called = Call->getCalledFunction();
                Called && Called->getName().starts_with("llvm.aarch64.stxr")) {
              ASSERT_EQ(Observed, nullptr);
              ASSERT_GE(Call->arg_size(), 1u);
              Observed = Call->getArgOperand(0);
            }
        }
      ASSERT_NE(Observed, nullptr);
      EXPECT_TRUE(isPtrToIntValue(Observed));
      std::set<const llvm::Value *> SeenTargets;
      EXPECT_TRUE(valueReferencesTarget(Observed, EmittedCallee, SeenTargets));
      std::set<const llvm::Value *> SeenIntegers;
      EXPECT_FALSE(valueReferencesInteger(Observed, CodeVA, SeenIntegers));
    }
}

TEST(
    LLVMCodePointerInvariantBoundary,
    MaterializesExactGenericCodeAddressInPointerIdentityRelationsAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));

      MedFunc Caller = makeExactCodeAddressIdentityRelations(TargetArch);
      MedFunc Callee =
          makeReturnFunction("identity_code_address_callee", CodeVA);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                          "identity-exact-code-address",
                                          TargetArch, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
      llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
      ASSERT_NE(EmittedCaller, nullptr);
      ASSERT_NE(EmittedCallee, nullptr);

      const llvm::BinaryOperator *Subtract = nullptr;
      const llvm::ICmpInst *Equal = nullptr;
      const llvm::ICmpInst *NotEqual = nullptr;
      for (const llvm::BasicBlock &Block : *EmittedCaller)
        for (const llvm::Instruction &Instruction : Block) {
          if (const auto *Binary =
                  llvm::dyn_cast<llvm::BinaryOperator>(&Instruction);
              Binary && Binary->getOpcode() == llvm::Instruction::Sub) {
            ASSERT_EQ(Subtract, nullptr);
            Subtract = Binary;
          }
          if (const auto *Compare =
                  llvm::dyn_cast<llvm::ICmpInst>(&Instruction)) {
            if (Compare->getPredicate() == llvm::CmpInst::ICMP_EQ) {
              ASSERT_EQ(Equal, nullptr);
              Equal = Compare;
            } else if (Compare->getPredicate() == llvm::CmpInst::ICMP_NE) {
              ASSERT_EQ(NotEqual, nullptr);
              NotEqual = Compare;
            }
          }
        }
      ASSERT_NE(Subtract, nullptr);
      ASSERT_NE(Equal, nullptr);
      ASSERT_NE(NotEqual, nullptr);

      for (const llvm::Instruction *Relation :
           {static_cast<const llvm::Instruction *>(Subtract),
            static_cast<const llvm::Instruction *>(Equal),
            static_cast<const llvm::Instruction *>(NotEqual)}) {
        bool HasTargetPtrToIntOperand = false;
        for (const llvm::Use &Operand : Relation->operands()) {
          if (!isPtrToIntValue(Operand.get()))
            continue;
          std::set<const llvm::Value *> SeenOperandTargets;
          HasTargetPtrToIntOperand |= valueReferencesTarget(
              Operand.get(), EmittedCallee, SeenOperandTargets);
        }
        EXPECT_TRUE(HasTargetPtrToIntOperand);
        std::set<const llvm::Value *> SeenTargets;
        EXPECT_TRUE(
            valueReferencesTarget(Relation, EmittedCallee, SeenTargets));
        std::set<const llvm::Value *> SeenIntegers;
        EXPECT_FALSE(valueReferencesInteger(Relation, CodeVA, SeenIntegers));
      }
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     ResolvesFrameReloadedExactGenericAddressAtIndirectCallUseAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));

      MedFunc Caller = makeFrameReloadedExactAddressIndirectCaller(TargetArch);
      MedFunc Callee = makeReturnFunction("frame_code_address_callee", CodeVA);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                          "frame-exact-code-address",
                                          TargetArch, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
      llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
      ASSERT_NE(EmittedCaller, nullptr);
      ASSERT_NE(EmittedCallee, nullptr);
      std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
      ASSERT_EQ(Calls.size(), 1u);

      llvm::Value *CalledOperand = Calls.front()->getCalledOperand();
      std::set<const llvm::Value *> SeenTargets;
      EXPECT_TRUE(
          valueReferencesTarget(CalledOperand, EmittedCallee, SeenTargets));
      std::set<const llvm::Value *> SeenIntegers;
      EXPECT_FALSE(valueReferencesInteger(CalledOperand, CodeVA, SeenIntegers));
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     MaterializesFrameReloadedInteriorAddressAtOrdinaryReturnAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(formatTraceName(Format));
    BinaryImage Image = makeSpilledConstTableImage(Arch::X64, Format);
    ASSERT_TRUE(Image.isCodeAddress(SameFunctionInteriorVA));

    MedFunc Func = makeFrameReloadedInteriorAddressUse(
        Arch::X64, /*AsIndirectCall=*/false);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Func}, Context, "frame-interior-address-return",
                              Arch::X64, {}, &Image, Format);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *Emitted = Module->getFunction(Func.Name);
    ASSERT_NE(Emitted, nullptr);
    llvm::Value *Observed = nullptr;
    for (llvm::BasicBlock &Block : *Emitted)
      if (auto *Return =
              llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator()))
        if (Return->getReturnValue()) {
          ASSERT_EQ(Observed, nullptr);
          Observed = Return->getReturnValue();
        }
    ASSERT_NE(Observed, nullptr);
    ASSERT_TRUE(isPtrToIntValue(Observed));

    llvm::Value *PointerOperand = nullptr;
    if (auto *Cast = llvm::dyn_cast<llvm::PtrToIntInst>(Observed))
      PointerOperand = Cast->getPointerOperand();
    else if (auto *Cast = llvm::dyn_cast<llvm::ConstantExpr>(Observed))
      PointerOperand = Cast->getOperand(0);
    auto *InteriorIdentity =
        llvm::dyn_cast_or_null<llvm::BlockAddress>(PointerOperand);
    ASSERT_NE(InteriorIdentity, nullptr);
    EXPECT_EQ(InteriorIdentity->getFunction(), Emitted);
    std::set<const llvm::Value *> SeenIntegers;
    EXPECT_FALSE(
        valueReferencesInteger(Observed, SameFunctionInteriorVA, SeenIntegers));

    auto Clone = llvm::CloneModule(*Module);
    ASSERT_NE(Clone, nullptr);
    expectValidModule(*Clone);
  }
}

TEST(LLVMCodePointerInvariantBoundary,
     PreservesInteriorAddressThroughX86FlagTransformsAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(formatTraceName(Format));
    BinaryImage Image = makeSpilledConstTableImage(Arch::X64, Format);
    ASSERT_TRUE(Image.isCodeAddress(SameFunctionInteriorVA));

    MedFunc Func = makeInteriorAddressX86FlagRelation();
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Func}, Context, "interior-address-x86-flags",
                              Arch::X64, {}, &Image, Format);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);

    llvm::Function *Emitted = Module->getFunction(Func.Name);
    ASSERT_NE(Emitted, nullptr);
    bool SawBlockAddress = false;
    bool SawPopulationCount = false;
    for (llvm::BasicBlock &Block : *Emitted)
      for (llvm::Instruction &Instruction : Block) {
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
          if (const llvm::Function *Called = Call->getCalledFunction())
            SawPopulationCount |= Called->getName().starts_with("llvm.ctpop");
        for (const llvm::Use &Operand : Instruction.operands())
          if (const auto *Constant =
                  llvm::dyn_cast<llvm::Constant>(Operand.get()))
            SawBlockAddress |= constantContainsBlockAddress(Constant);
        std::set<const llvm::Value *> SeenIntegers;
        EXPECT_FALSE(valueReferencesInteger(
            &Instruction, SameFunctionInteriorVA, SeenIntegers));
      }
    EXPECT_TRUE(SawBlockAddress);
    EXPECT_TRUE(SawPopulationCount);

    auto Clone = llvm::CloneModule(*Module);
    ASSERT_NE(Clone, nullptr);
    expectValidModule(*Clone);
  }
}

TEST(LLVMCodePointerInvariantBoundary,
     PreservesExactIdentityThroughLongPureForwardRecurrenceAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64})
      for (LongExactIdentitySink Sink : {LongExactIdentitySink::Return,
                                         LongExactIdentitySink::IndirectCall}) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        SCOPED_TRACE(Sink == LongExactIdentitySink::Return ? "return"
                                                           : "indirect-call");
        BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
        ASSERT_TRUE(Image.hasExecutableCodeOwnerAt(CodeVA));

        MedFunc Caller = makeLongExactIdentityRecurrence(TargetArch, Sink);
        ASSERT_EQ(Caller.Blocks.size(), 3u);
        ASSERT_EQ(Caller.Blocks[1].Phis.size(), 1u);
        const PhiNode &RecurrentPhi = Caller.Blocks[1].Phis.front();
        ASSERT_EQ(RecurrentPhi.Args.size(), 2u);
        const auto Backedge =
            std::find_if(RecurrentPhi.Args.begin(), RecurrentPhi.Args.end(),
                         [](const auto &Arg) { return Arg.first == 1; });
        ASSERT_NE(Backedge, RecurrentPhi.Args.end());
        MedLLVMEmitter Probe;
        MedLLVMProvenanceTestPeer::prepareFreshAnalysis(Probe, Caller, Image,
                                                        TargetArch, Format);
        EXPECT_TRUE(MedLLVMProvenanceTestPeer::incomingIsRecurrent(
            Probe, RecurrentPhi, Backedge->first, Backedge->second))
            << "the pure-forward recurrence proof must not inherit the "
               "general 32-node depth limit";

        MedFunc Callee =
            makeReturnFunction("long_exact_identity_callee", CodeVA);
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                            "long-exact-identity-recurrence",
                                            TargetArch, {}, &Image, Format);
        ASSERT_NE(Module, nullptr);
        expectValidModule(*Module);

        llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
        llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
        ASSERT_NE(EmittedCaller, nullptr);
        ASSERT_NE(EmittedCallee, nullptr);

        bool SinkReferencesTarget = false;
        if (Sink == LongExactIdentitySink::IndirectCall) {
          std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
          ASSERT_EQ(Calls.size(), 1u);
          std::set<const llvm::Value *> SeenTargets;
          SinkReferencesTarget = valueReferencesTarget(
              Calls.front()->getCalledOperand(), EmittedCallee, SeenTargets);
        } else {
          const llvm::ReturnInst *ObservedReturn = nullptr;
          for (const llvm::BasicBlock &Block : *EmittedCaller)
            if (const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
                Return && Return->getReturnValue()) {
              ASSERT_EQ(ObservedReturn, nullptr);
              ObservedReturn = Return;
            }
          ASSERT_NE(ObservedReturn, nullptr);
          std::set<const llvm::Value *> SeenTargets;
          SinkReferencesTarget = valueReferencesTarget(
              ObservedReturn->getReturnValue(), EmittedCallee, SeenTargets);
        }
        EXPECT_TRUE(SinkReferencesTarget);

        for (const llvm::BasicBlock &Block : *EmittedCaller)
          for (const llvm::Instruction &Instruction : Block) {
            std::set<const llvm::Value *> SeenIntegers;
            EXPECT_FALSE(
                valueReferencesInteger(&Instruction, CodeVA, SeenIntegers));
          }
      }
}

TEST(LLVMCodePointerInvariantBoundary,
     BoundsLongSharedRecurrentScalarProofsAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64})
      for (LongRecurrentCodeArm CodeArm :
           {LongRecurrentCodeArm::None, LongRecurrentCodeArm::Infeasible,
            LongRecurrentCodeArm::Feasible}) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        SCOPED_TRACE(static_cast<unsigned>(CodeArm));
        BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
        ASSERT_TRUE(Image.hasExecutableCodeOwnerAt(CodeVA));
        ASSERT_TRUE(Image.hasExecutableCodeOwnerAt(OtherRecurrentCodeVA));

        MedFunc Caller = makeLongRecurrentScalarCodeReturn(
            TargetArch, CodeArm, LongRecurrentValueSink::SupportedAdd);
        MedFunc Callee = makeReturnFunction("long_recurrent_callee", CodeVA);
        MedFunc OtherCallee = makeReturnFunction("long_recurrent_other_callee",
                                                 OtherRecurrentCodeVA);

        const PhiNode *RecurrentPhi = nullptr;
        for (const MedBlock &Block : Caller.Blocks)
          if (!Block.Phis.empty()) {
            ASSERT_EQ(RecurrentPhi, nullptr);
            RecurrentPhi = &Block.Phis.front();
          }
        ASSERT_NE(RecurrentPhi, nullptr);
        auto Backedge =
            std::find_if(RecurrentPhi->Args.begin(), RecurrentPhi->Args.end(),
                         [](const auto &Arg) { return Arg.first == 2; });
        ASSERT_NE(Backedge, RecurrentPhi->Args.end());
        MedLLVMEmitter Probe;
        MedLLVMProvenanceTestPeer::prepareFreshAnalysis(Probe, Caller, Image,
                                                        TargetArch, Format);
        EXPECT_FALSE(MedLLVMProvenanceTestPeer::incomingIsRecurrent(
            Probe, *RecurrentPhi, Backedge->first, Backedge->second))
            << "the scalar-zero operation after >32 COPYs must exercise the "
               "residual SCC cut";

        llvm::LLVMContext Context;
        // The source graph is linear in SharedScalarDiamondDepth, but every
        // XOR names its child twice. An incomplete result from the residual
        // SCC cut therefore expands exponentially instead of being memoized.
        auto Module =
            MedLLVMEmitter().emit({Caller, Callee, OtherCallee}, Context,
                                  "long-recurrent-supported-code-add",
                                  TargetArch, {}, &Image, Format);
        ASSERT_NE(Module, nullptr);
        expectValidModule(*Module);

        llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
        llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
        llvm::Function *EmittedOther = Module->getFunction(OtherCallee.Name);
        ASSERT_NE(EmittedCaller, nullptr);
        ASSERT_NE(EmittedCallee, nullptr);
        ASSERT_NE(EmittedOther, nullptr);
        bool SawTargetDependentAdd = false;
        bool SawOtherTarget = false;
        for (const llvm::BasicBlock &Block : *EmittedCaller)
          for (const llvm::Instruction &Instruction : Block) {
            if (const auto *Binary =
                    llvm::dyn_cast<llvm::BinaryOperator>(&Instruction);
                Binary && Binary->getOpcode() == llvm::Instruction::Add) {
              std::set<const llvm::Value *> SeenTargets;
              SawTargetDependentAdd |=
                  valueReferencesTarget(Binary, EmittedCallee, SeenTargets);
            }
            std::set<const llvm::Value *> SeenOtherTargets;
            SawOtherTarget |= valueReferencesTarget(&Instruction, EmittedOther,
                                                    SeenOtherTargets);
            std::set<const llvm::Value *> SeenCodeIntegers;
            EXPECT_FALSE(
                valueReferencesInteger(&Instruction, CodeVA, SeenCodeIntegers));
            std::set<const llvm::Value *> SeenOtherIntegers;
            EXPECT_FALSE(valueReferencesInteger(
                &Instruction, OtherRecurrentCodeVA, SeenOtherIntegers));
          }
        EXPECT_TRUE(SawTargetDependentAdd);
        if (CodeArm == LongRecurrentCodeArm::Feasible)
          EXPECT_TRUE(SawOtherTarget);
        else if (CodeArm == LongRecurrentCodeArm::None)
          EXPECT_FALSE(SawOtherTarget);
        // MedLLVM emits edge-copy blocks for structurally present CFG edges,
        // including a proven-infeasible predecessor. Its dead edge may contain
        // a relocated @other store; the semantic requirement is that this arm
        // neither poisons the reachable proof nor leaves a raw original VA.
      }
}

TEST(LLVMCodePointerInvariantBoundary,
     RejectsLayoutOnlyAddressInScalarCycleSelectArmAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      ASSERT_TRUE(Image.hasExecutableCodeOwnerAt(CodeVA));
      ASSERT_TRUE(Image.hasExecutableCodeOwnerAt(ExecutableLayoutOnlyVA));
      ASSERT_EQ(Image.CodeRefTargets.count(ExecutableLayoutOnlyVA), 0u);
      ASSERT_NE(ExecutableLayoutOnlyVA, CallerVA);
      ASSERT_NE(ExecutableLayoutOnlyVA, CodeVA);

      MedFunc Caller = makeLongRecurrentScalarCodeReturn(
          TargetArch, LongRecurrentCodeArm::None,
          LongRecurrentValueSink::MixedLayoutSelect);
      for (const MedBlock &Block : Caller.Blocks)
        ASSERT_NE(Block.StartAddr, ExecutableLayoutOnlyVA);
      MedFunc Callee = makeReturnFunction("mixed_layout_select_callee", CodeVA);

      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                          "mixed-layout-scalar-cycle-select",
                                          TargetArch, {}, &Image, Format);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module, nullptr);
      EXPECT_NE(Diagnostic.find("no unique lifted code identity"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     PreservesExactAndAdjustedCodeIdentityAcrossSelectAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));

      MedFunc Caller = makeExactAndAdjustedCodeIdentitySelect(TargetArch);
      MedFunc Callee = makeReturnFunction("select_relation_callee", CodeVA);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                          "select-code-identity-relation",
                                          TargetArch, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
      llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
      ASSERT_NE(EmittedCaller, nullptr);
      ASSERT_NE(EmittedCallee, nullptr);
      const llvm::SelectInst *ObservedSelect = nullptr;
      const llvm::ReturnInst *ObservedReturn = nullptr;
      const llvm::StoreInst *SelectStore = nullptr;
      bool SawTargetDependentAdd = false;
      for (const llvm::BasicBlock &Block : *EmittedCaller)
        for (const llvm::Instruction &Instruction : Block) {
          if (const auto *Select =
                  llvm::dyn_cast<llvm::SelectInst>(&Instruction))
            ObservedSelect = Select;
          if (const auto *Return =
                  llvm::dyn_cast<llvm::ReturnInst>(&Instruction))
            ObservedReturn = Return;
          if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
              Store && Store->getValueOperand() == ObservedSelect)
            SelectStore = Store;
          if (const auto *Binary =
                  llvm::dyn_cast<llvm::BinaryOperator>(&Instruction);
              Binary && Binary->getOpcode() == llvm::Instruction::Add) {
            std::set<const llvm::Value *> SeenTargets;
            SawTargetDependentAdd |=
                valueReferencesTarget(Binary, EmittedCallee, SeenTargets);
          }
          std::set<const llvm::Value *> SeenIntegers;
          EXPECT_FALSE(
              valueReferencesInteger(&Instruction, CodeVA, SeenIntegers));
        }
      ASSERT_NE(ObservedSelect, nullptr);
      ASSERT_NE(ObservedReturn, nullptr);
      ASSERT_NE(ObservedReturn->getReturnValue(), nullptr);
      ASSERT_NE(SelectStore, nullptr);
      const auto *ReturnLoad =
          llvm::dyn_cast<llvm::LoadInst>(ObservedReturn->getReturnValue());
      ASSERT_NE(ReturnLoad, nullptr);
      const llvm::StoreInst *ReturnStore = nullptr;
      for (const llvm::BasicBlock &Block : *EmittedCaller)
        for (const llvm::Instruction &Instruction : Block)
          if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
              Store &&
              Store->getPointerOperand()->stripPointerCasts() ==
                  ReturnLoad->getPointerOperand()->stripPointerCasts() &&
              llvm::isa<llvm::LoadInst>(Store->getValueOperand()))
            ReturnStore = Store;
      ASSERT_NE(ReturnStore, nullptr);
      const auto *SelectedLoad =
          llvm::cast<llvm::LoadInst>(ReturnStore->getValueOperand());
      EXPECT_EQ(SelectedLoad->getPointerOperand()->stripPointerCasts(),
                SelectStore->getPointerOperand()->stripPointerCasts());
      std::set<const llvm::Value *> SeenTargets;
      EXPECT_TRUE(
          valueReferencesTarget(ObservedSelect, EmittedCallee, SeenTargets));
      EXPECT_TRUE(SawTargetDependentAdd);
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     PreservesExactCodeIdentityAcrossAdjustedRecurrentPhiAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64})
      for (bool DirectConstantArm : {false, true}) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        SCOPED_TRACE(DirectConstantArm ? "direct-constant-arm"
                                       : "forwarded-constant-arm");
        BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
        ASSERT_TRUE(Image.isCodeAddress(CodeVA));

        MedFunc Caller = makeRecurrentAdjustedCodeIdentityReturn(
            TargetArch, DirectConstantArm);
        MedFunc Callee =
            makeReturnFunction("recurrent_relation_callee", CodeVA);
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                            "recurrent-code-identity-relation",
                                            TargetArch, {}, &Image, Format);
        ASSERT_NE(Module, nullptr);
        expectValidModule(*Module);

        llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
        llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
        ASSERT_NE(EmittedCaller, nullptr);
        ASSERT_NE(EmittedCallee, nullptr);
        const llvm::ReturnInst *ObservedReturn = nullptr;
        const llvm::BinaryOperator *RecurrenceAdd = nullptr;
        const llvm::LoadInst *CurrentPhiLoad = nullptr;
        std::vector<const llvm::StoreInst *> Stores;
        for (const llvm::BasicBlock &Block : *EmittedCaller)
          for (const llvm::Instruction &Instruction : Block) {
            if (const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(&Instruction))
              ObservedReturn = Return;
            if (const auto *Binary =
                    llvm::dyn_cast<llvm::BinaryOperator>(&Instruction);
                Binary && Binary->getOpcode() == llvm::Instruction::Add) {
              for (unsigned I = 0; I < 2; ++I)
                if (const auto *Step = llvm::dyn_cast<llvm::ConstantInt>(
                        Binary->getOperand(I));
                    Step && Step->equalsInt(4)) {
                  RecurrenceAdd = Binary;
                  CurrentPhiLoad = llvm::dyn_cast<llvm::LoadInst>(
                      Binary->getOperand(1U - I));
                }
            }
            if (const auto *Store =
                    llvm::dyn_cast<llvm::StoreInst>(&Instruction))
              Stores.push_back(Store);
            std::set<const llvm::Value *> SeenIntegers;
            EXPECT_FALSE(
                valueReferencesInteger(&Instruction, CodeVA, SeenIntegers));
          }
        ASSERT_NE(ObservedReturn, nullptr);
        llvm::Value *ReturnValue = ObservedReturn->getReturnValue();
        ASSERT_NE(ReturnValue, nullptr);
        const auto *ReturnLoad = llvm::dyn_cast<llvm::LoadInst>(ReturnValue);
        ASSERT_NE(ReturnLoad, nullptr);
        ASSERT_NE(RecurrenceAdd, nullptr);
        ASSERT_NE(CurrentPhiLoad, nullptr);

        const llvm::StoreInst *AddResultStore = nullptr;
        for (const llvm::StoreInst *Store : Stores)
          if (Store->getValueOperand() == RecurrenceAdd)
            AddResultStore = Store;
        ASSERT_NE(AddResultStore, nullptr);

        const llvm::Value *CurrentPhiSlot =
            CurrentPhiLoad->getPointerOperand()->stripPointerCasts();
        const llvm::Value *AddResultSlot =
            AddResultStore->getPointerOperand()->stripPointerCasts();
        const llvm::Value *ExitPhiSlot = nullptr;
        bool SawCurrentPhiInitializer = false;
        bool SawBackedgeWrite = false;
        for (const llvm::StoreInst *Store : Stores) {
          const llvm::Value *Destination =
              Store->getPointerOperand()->stripPointerCasts();
          std::set<const llvm::Value *> SeenTargets;
          if (Destination == CurrentPhiSlot &&
              valueReferencesTarget(Store->getValueOperand(), EmittedCallee,
                                    SeenTargets))
            SawCurrentPhiInitializer = true;
          const auto *SourceLoad =
              llvm::dyn_cast<llvm::LoadInst>(Store->getValueOperand());
          if (!SourceLoad ||
              SourceLoad->getPointerOperand()->stripPointerCasts() !=
                  AddResultSlot)
            continue;
          if (Destination == CurrentPhiSlot)
            SawBackedgeWrite = true;
          else
            ExitPhiSlot = Destination;
        }
        EXPECT_TRUE(SawCurrentPhiInitializer);
        EXPECT_TRUE(SawBackedgeWrite);
        ASSERT_NE(ExitPhiSlot, nullptr);

        bool SawExitPhiInitializer = false;
        for (const llvm::StoreInst *Store : Stores) {
          if (Store->getPointerOperand()->stripPointerCasts() != ExitPhiSlot)
            continue;
          std::set<const llvm::Value *> SeenTargets;
          SawExitPhiInitializer |= valueReferencesTarget(
              Store->getValueOperand(), EmittedCallee, SeenTargets);
        }
        EXPECT_TRUE(SawExitPhiInitializer);

        bool ReturnLoadsExitPhi = false;
        for (const llvm::StoreInst *Store : Stores) {
          if (Store->getPointerOperand()->stripPointerCasts() !=
              ReturnLoad->getPointerOperand()->stripPointerCasts())
            continue;
          const auto *SourceLoad =
              llvm::dyn_cast<llvm::LoadInst>(Store->getValueOperand());
          ReturnLoadsExitPhi |=
              SourceLoad &&
              SourceLoad->getPointerOperand()->stripPointerCasts() ==
                  ExitPhiSlot;
        }
        EXPECT_TRUE(ReturnLoadsExitPhi);
      }
}

TEST(LLVMCodePointerInvariantBoundary,
     RejectsFrameReloadedInteriorAddressAtIndirectCallAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(formatTraceName(Format));
    BinaryImage Image = makeSpilledConstTableImage(Arch::X64, Format);
    ASSERT_TRUE(Image.isCodeAddress(SameFunctionInteriorVA));

    MedFunc Func =
        makeFrameReloadedInteriorAddressUse(Arch::X64, /*AsIndirectCall=*/true);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit({Func}, Context,
                                        "frame-interior-address-indirect-call",
                                        Arch::X64, {}, &Image, Format);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module, nullptr);
    EXPECT_NE(Diagnostic.find("unique lifted function"), std::string::npos)
        << Diagnostic;
  }
}

TEST(LLVMCodePointerInvariantBoundary,
     ResolvesLiveExactAddressAcrossProvenInfeasiblePhiArmAcrossFormats) {
  constexpr va_t InfeasibleCodeVA = CodeVA + 0x40;
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));
      ASSERT_TRUE(Image.isCodeAddress(InfeasibleCodeVA));

      MedFunc Caller = makeInfeasibleOtherExactAddressPhiIndirectCaller(
          TargetArch, CodeVA, InfeasibleCodeVA);
      MedFunc LiveCallee = makeReturnFunction("live_phi_code_callee", CodeVA);
      MedFunc InfeasibleCallee =
          makeReturnFunction("infeasible_phi_code_callee", InfeasibleCodeVA);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit(
          {Caller, LiveCallee, InfeasibleCallee}, Context,
          "infeasible-phi-exact-code-address", TargetArch, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
      llvm::Function *EmittedLive = Module->getFunction(LiveCallee.Name);
      llvm::Function *EmittedInfeasible =
          Module->getFunction(InfeasibleCallee.Name);
      ASSERT_NE(EmittedCaller, nullptr);
      ASSERT_NE(EmittedLive, nullptr);
      ASSERT_NE(EmittedInfeasible, nullptr);
      std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
      ASSERT_EQ(Calls.size(), 1u);

      llvm::Value *CalledOperand = Calls.front()->getCalledOperand();
      std::set<const llvm::Value *> SeenLiveTargets;
      EXPECT_TRUE(
          valueReferencesTarget(CalledOperand, EmittedLive, SeenLiveTargets));
      std::set<const llvm::Value *> SeenInfeasibleTargets;
      EXPECT_FALSE(valueReferencesTarget(CalledOperand, EmittedInfeasible,
                                         SeenInfeasibleTargets));
      std::set<const llvm::Value *> SeenLiveIntegers;
      EXPECT_FALSE(
          valueReferencesInteger(CalledOperand, CodeVA, SeenLiveIntegers));
      std::set<const llvm::Value *> SeenInfeasibleIntegers;
      EXPECT_FALSE(valueReferencesInteger(CalledOperand, InfeasibleCodeVA,
                                          SeenInfeasibleIntegers));
    }
}

TEST(
    LLVMCodePointerInvariantBoundary,
    ResolvesEntryExactAddressAcrossSelfRecurrentPhiAtIndirectCallUseAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));

      MedFunc Caller =
          makeSelfRecurrentExactAddressPhiIndirectCaller(TargetArch);
      MedFunc Callee = makeReturnFunction("recurrent_phi_code_callee", CodeVA);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                          "recurrent-phi-exact-code-address",
                                          TargetArch, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
      llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
      ASSERT_NE(EmittedCaller, nullptr);
      ASSERT_NE(EmittedCallee, nullptr);
      std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
      ASSERT_EQ(Calls.size(), 1u);

      llvm::Value *CalledOperand = Calls.front()->getCalledOperand();
      std::set<const llvm::Value *> SeenTargets;
      EXPECT_TRUE(
          valueReferencesTarget(CalledOperand, EmittedCallee, SeenTargets));
      std::set<const llvm::Value *> SeenIntegers;
      EXPECT_FALSE(valueReferencesInteger(CalledOperand, CodeVA, SeenIntegers));

      std::vector<const llvm::ICmpInst *> IdentityComparisons;
      for (const llvm::BasicBlock &Block : *EmittedCaller)
        for (const llvm::Instruction &Instruction : Block)
          if (const auto *Compare =
                  llvm::dyn_cast<llvm::ICmpInst>(&Instruction))
            IdentityComparisons.push_back(Compare);
      ASSERT_EQ(IdentityComparisons.size(), 2u);
      for (const llvm::ICmpInst *Compare : IdentityComparisons) {
        std::set<const llvm::Value *> SeenCompareTargets;
        EXPECT_TRUE(
            valueReferencesTarget(Compare, EmittedCallee, SeenCompareTargets));
        std::set<const llvm::Value *> SeenCompareIntegers;
        EXPECT_FALSE(
            valueReferencesInteger(Compare, CodeVA, SeenCompareIntegers));
      }
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     RejectsMixedExactCodeAddressAtIndirectCallUseAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(formatTraceName(Format));
    BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64, Format);
    ASSERT_TRUE(Image.isCodeAddress(CodeVA));

    MedFunc Caller = makeMergedExactAddressIndirectCaller(
        ExactAddressMergeKind::Select, CodeVA, CodeVA);
    ASSERT_FALSE(Caller.Blocks.empty());
    ASSERT_FALSE(Caller.Blocks.front().Ops.empty());
    MedOp &Select = Caller.Blocks.front().Ops.front();
    ASSERT_EQ(Select.Opcode, NdOp::SELECT);
    ASSERT_GE(Select.NumInputs, 3u);
    const uint16_t PointerSize =
        static_cast<uint16_t>(getTargetRegInfo(Arch::AArch64).PointerSize);
    Select.Inputs[2] =
        MedVar::makeConst(1, PointerSize, ConstantAddressProvenance::Scalar);

    MedFunc Callee = makeReturnFunction("mixed_code_address_callee", CodeVA);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit(
        {Caller, Callee}, Context, "mixed-exact-code-address-indirect-call",
        Arch::AArch64, {}, &Image, Format);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module, nullptr);
    EXPECT_NE(Diagnostic.find("no unique lifted function entry"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(LLVMCodePointerInvariantBoundary,
     RejectsExplicitNonCodeConstantAtIndirectCallUseAcrossFormats) {
  struct Case {
    const char *Name;
    va_t Value;
    ConstantAddressProvenance Provenance;
  };
  const Case Cases[] = {
      {"scalar", 1, ConstantAddressProvenance::Scalar},
      {"data-address", SpilledConstTableVA,
       ConstantAddressProvenance::DataAddress},
      {"generic-non-code-address", SpilledConstTableVA,
       ConstantAddressProvenance::Address},
      {"null", 0, ConstantAddressProvenance::Unknown},
  };

  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64, Format);
    ASSERT_FALSE(Image.isCodeAddress(SpilledConstTableVA));
    for (const Case &Current : Cases) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(Current.Name);
      MedFunc Caller = makeExplicitConstantIndirectCaller(
          Arch::AArch64, Current.Value, Current.Provenance);

      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module = MedLLVMEmitter().emit({Caller}, Context,
                                          "explicit-non-code-indirect-call",
                                          Arch::AArch64, {}, &Image, Format);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module, nullptr);
      EXPECT_NE(Diagnostic.find("no unique lifted function entry"),
                std::string::npos)
          << Diagnostic;
    }
  }
}

TEST(LLVMCodePointerInvariantBoundary,
     RejectsMixedExactCodeAddressAtObservableValueSinkAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(formatTraceName(Format));
    constexpr Arch TargetArch = Arch::AArch64;
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
    BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
    ASSERT_TRUE(Image.isCodeAddress(CodeVA));

    MedFunc Func = makeExactCodeAddressValueSink(
        TargetArch, ExactCodeAddressValueSink::Return);
    ASSERT_GE(Func.Blocks.front().Ops.size(), 2u);
    MedVar Condition;
    Condition.Kind = MedVar::Param;
    Condition.TheArch = TargetArch;
    Condition.Id = 90;
    Condition.Size = 1;
    Condition.RegOff = TRI.IntParamRegs.front();
    Func.Params.push_back(Condition);

    MedOp &Materialize = Func.Blocks.front().Ops.front();
    ASSERT_EQ(Materialize.Opcode, NdOp::COPY);
    const MedVar Merged = Materialize.Output;
    Materialize.Opcode = NdOp::SELECT;
    Materialize.NumInputs = 0;
    Materialize.addInput(Condition);
    Materialize.addInput(MedVar::makeConst(CodeVA, PointerSize,
                                           ConstantAddressProvenance::Address));
    Materialize.addInput(
        MedVar::makeConst(1, PointerSize, ConstantAddressProvenance::Scalar));
    ASSERT_EQ(Func.Blocks.front().Ops[1].Inputs[0], Merged);

    MedFunc Callee =
        makeReturnFunction("mixed_value_code_address_callee", CodeVA);
    llvm::LLVMContext Context;
    testing::internal::CaptureStderr();
    auto Module = MedLLVMEmitter().emit({Func, Callee}, Context,
                                        "mixed-exact-code-address-value",
                                        TargetArch, {}, &Image, Format);
    std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module, nullptr);
    EXPECT_NE(Diagnostic.find("no unique lifted code identity"),
              std::string::npos)
        << Diagnostic;
  }
}

TEST(LLVMCodePointerInvariantBoundary,
     MaterializesNullableExactCodeAddressAtObservableValueSinkAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
      const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));

      MedFunc Func = makeExactCodeAddressValueSink(
          TargetArch, ExactCodeAddressValueSink::Return);
      ASSERT_GE(Func.Blocks.front().Ops.size(), 2u);
      MedVar Condition;
      Condition.Kind = MedVar::Param;
      Condition.TheArch = TargetArch;
      Condition.Id = 100;
      Condition.Size = 1;
      Condition.RegOff = TRI.IntParamRegs.front();
      Func.Params.push_back(Condition);

      MedOp &Materialize = Func.Blocks.front().Ops.front();
      ASSERT_EQ(Materialize.Opcode, NdOp::COPY);
      Materialize.Opcode = NdOp::SELECT;
      Materialize.NumInputs = 0;
      Materialize.addInput(Condition);
      Materialize.addInput(MedVar::makeConst(
          CodeVA, PointerSize, ConstantAddressProvenance::Address));
      Materialize.addInput(
          MedVar::makeConst(0, PointerSize, ConstantAddressProvenance::Scalar));

      MedFunc Callee =
          makeReturnFunction("nullable_value_code_address_callee", CodeVA);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Func, Callee}, Context,
                                          "nullable-exact-code-address-value",
                                          TargetArch, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *Emitted = Module->getFunction(Func.Name);
      llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
      ASSERT_NE(Emitted, nullptr);
      ASSERT_NE(EmittedCallee, nullptr);
      const llvm::SelectInst *ObservedSelect = nullptr;
      bool SawSelectStore = false;
      for (const llvm::BasicBlock &Block : *Emitted)
        for (const llvm::Instruction &Instruction : Block) {
          if (const auto *Select =
                  llvm::dyn_cast<llvm::SelectInst>(&Instruction))
            ObservedSelect = Select;
          if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
            SawSelectStore |=
                ObservedSelect && Store->getValueOperand() == ObservedSelect;
          std::set<const llvm::Value *> SeenIntegers;
          EXPECT_FALSE(
              valueReferencesInteger(&Instruction, CodeVA, SeenIntegers));
        }
      ASSERT_NE(ObservedSelect, nullptr);
      std::set<const llvm::Value *> SeenTargets;
      EXPECT_TRUE(
          valueReferencesTarget(ObservedSelect, EmittedCallee, SeenTargets));
      EXPECT_TRUE(SawSelectStore);
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     ResolvesConvergedExactAddressPhiAndSelectAtIndirectCallUse) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (ExactAddressMergeKind Kind :
         {ExactAddressMergeKind::Phi, ExactAddressMergeKind::Select}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(Kind == ExactAddressMergeKind::Phi ? "phi" : "select");
      BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64, Format);
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));

      MedFunc Caller =
          makeMergedExactAddressIndirectCaller(Kind, CodeVA, CodeVA);
      MedFunc Callee =
          makeReturnFunction("merged_exact_address_callee", CodeVA);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Caller, Callee}, Context,
                                          "merged-exact-address-indirect-call",
                                          Arch::AArch64, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);

      llvm::Function *EmittedCaller = Module->getFunction(Caller.Name);
      llvm::Function *EmittedCallee = Module->getFunction(Callee.Name);
      ASSERT_NE(EmittedCaller, nullptr);
      ASSERT_NE(EmittedCallee, nullptr);
      std::vector<llvm::CallInst *> Calls = callsIn(*EmittedCaller);
      ASSERT_EQ(Calls.size(), 1u);
      std::set<const llvm::Value *> SeenTargets;
      EXPECT_TRUE(valueReferencesTarget(Calls.front()->getCalledOperand(),
                                        EmittedCallee, SeenTargets));
      std::set<const llvm::Value *> SeenIntegers;
      EXPECT_FALSE(valueReferencesInteger(Calls.front()->getCalledOperand(),
                                          CodeVA, SeenIntegers));
    }
}

TEST(LLVMCodePointerInvariantBoundary,
     RejectsDistinctExactAddressPhiAndSelectAtIndirectCallUse) {
  constexpr va_t OtherCodeVA = CodeVA + 0x40;
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (ExactAddressMergeKind Kind :
         {ExactAddressMergeKind::Phi, ExactAddressMergeKind::Select}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(Kind == ExactAddressMergeKind::Phi ? "phi" : "select");
      BinaryImage Image = makeSpilledConstTableImage(Arch::AArch64, Format);
      ASSERT_TRUE(Image.isCodeAddress(CodeVA));
      ASSERT_TRUE(Image.isCodeAddress(OtherCodeVA));

      MedFunc Caller =
          makeMergedExactAddressIndirectCaller(Kind, CodeVA, OtherCodeVA);
      MedFunc FirstCallee =
          makeReturnFunction("first_exact_address_callee", CodeVA);
      MedFunc SecondCallee =
          makeReturnFunction("second_exact_address_callee", OtherCodeVA);
      llvm::LLVMContext Context;
      testing::internal::CaptureStderr();
      auto Module =
          MedLLVMEmitter().emit({Caller, FirstCallee, SecondCallee}, Context,
                                "distinct-exact-address-indirect-call",
                                Arch::AArch64, {}, &Image, Format);
      std::string Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_EQ(Module, nullptr);
      EXPECT_NE(Diagnostic.find("no unique lifted function entry"),
                std::string::npos)
          << Diagnostic;
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     AddressAlgebraUsesOccurrenceProvenanceAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      const uint16_t PointerSize =
          static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

      auto temp = [&](int Id, uint16_t Size) {
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
      Func.Name = "occurrence_sensitive_address_algebra";
      MedBlock Block;
      Block.Id = 0;
      Block.StartAddr = CallerVA;
      Block.EndAddr = CallerVA + 0x10;

      const MedVar FullCopy = temp(200, PointerSize);
      MedOp Copy;
      Copy.Opcode = NdOp::COPY;
      Copy.Output = FullCopy;
      Copy.addInput(MedVar::makeConst(SpilledConstTableVA, PointerSize,
                                      ConstantAddressProvenance::Address));
      Block.Ops.push_back(std::move(Copy));

      const MedVar Narrow = temp(201, 4);
      MedOp Truncate;
      Truncate.Opcode = NdOp::COPY;
      Truncate.Output = Narrow;
      Truncate.addInput(MedVar::makeConst(SpilledConstTableVA, PointerSize,
                                          ConstantAddressProvenance::Address));
      Block.Ops.push_back(std::move(Truncate));

      const MedVar Widened = temp(202, PointerSize);
      MedOp Widen;
      Widen.Opcode = NdOp::COPY;
      Widen.Output = Widened;
      Widen.addInput(Narrow);
      Block.Ops.push_back(std::move(Widen));
      Func.Blocks.push_back(std::move(Block));

      MedLLVMEmitter Emitter;
      MedLLVMProvenanceTestPeer::prepareFreshAnalysis(Emitter, Func, Image,
                                                      TargetArch, Format);
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::materializableDataAddress(
          Emitter, MedVar::makeConst(SpilledConstTableVA, PointerSize,
                                     ConstantAddressProvenance::Address)));
      EXPECT_TRUE(MedLLVMProvenanceTestPeer::materializableDataAddress(
          Emitter, FullCopy));
      EXPECT_FALSE(MedLLVMProvenanceTestPeer::materializableDataAddress(
          Emitter, MedVar::makeConst(SpilledConstTableVA, PointerSize)));
      EXPECT_FALSE(MedLLVMProvenanceTestPeer::materializableDataAddress(
          Emitter, Widened));
      EXPECT_FALSE(MedLLVMProvenanceTestPeer::sameAddressProvenanceKey(
          MedVar::makeConst(SpilledConstTableVA, PointerSize,
                            ConstantAddressProvenance::Address),
          MedVar::makeConst(SpilledConstTableVA, PointerSize)))
          << "tagged and scalar occurrences must never share provenance memo";
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsNarrowExactAddressEscapeAfterWideningAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64})
      for (const auto &[Provenance, Address] :
           {std::pair{ConstantAddressProvenance::Address, SpilledConstTableVA},
            std::pair{ConstantAddressProvenance::DataAddress,
                      SpilledConstTableVA},
            std::pair{ConstantAddressProvenance::CodeAddress, CodeVA}}) {
        SCOPED_TRACE(formatTraceName(Format));
        SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
        SCOPED_TRACE(static_cast<unsigned>(Provenance));
        const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
        const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
        ASSERT_GT(PointerSize, 4u);
        BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

        auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
          MedVar V;
          V.Kind = Kind;
          V.TheArch = TargetArch;
          V.Id = Id;
          V.SSAVer = 1;
          V.Size = Size;
          return V;
        };

        MedFunc Func;
        Func.Entry = CallerVA;
        Func.Name = "narrow_exact_address_escape";
        Func.ReturnType = NdType::makeInt(PointerSize);
        MedBlock Block;
        Block.Id = 0;
        Block.StartAddr = CallerVA;
        Block.EndAddr = CallerVA + 8;

        const MedVar Narrow = makeVar(MedVar::Temp, 9000, 4);
        MedOp CopyNarrow;
        CopyNarrow.Opcode = NdOp::COPY;
        CopyNarrow.Output = Narrow;
        CopyNarrow.addInput(MedVar::makeConst(Address, 4, Provenance));
        Block.Ops.push_back(std::move(CopyNarrow));

        const MedVar Widened = makeVar(MedVar::Temp, 9001, PointerSize);
        MedOp Widen;
        Widen.Opcode = NdOp::INT_ZEXT;
        Widen.Output = Widened;
        Widen.addInput(Narrow);
        Block.Ops.push_back(std::move(Widen));

        MedVar ReturnReg = makeVar(MedVar::Reg, 9002, PointerSize);
        ReturnReg.RegOff = TRI.IntReturnReg;
        MedOp WriteReturn;
        WriteReturn.Opcode = NdOp::COPY;
        WriteReturn.Output = ReturnReg;
        WriteReturn.addInput(Widened);
        Block.Ops.push_back(std::move(WriteReturn));

        MedOp Return;
        Return.Opcode = NdOp::RETURN;
        Block.Ops.push_back(std::move(Return));
        Func.Blocks.push_back(std::move(Block));

        llvm::LLVMContext Context;
        testing::internal::CaptureStderr();
        auto Module = MedLLVMEmitter().emit({Func}, Context,
                                            "narrow-exact-address-escape",
                                            TargetArch, {}, &Image, Format);
        std::string Diagnostic = testing::internal::GetCapturedStderr();
        EXPECT_EQ(Module, nullptr);
        EXPECT_NE(Diagnostic.find("refusing stale-address fallback"),
                  std::string::npos)
            << Diagnostic;
      }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsDeepAddressFragmentEscapeAcrossFormats) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      const uint16_t PointerSize =
          static_cast<uint16_t>(getTargetRegInfo(TargetArch).PointerSize);
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

      auto temp = [&](int Id) {
        MedVar V;
        V.Kind = MedVar::Temp;
        V.TheArch = TargetArch;
        V.Id = Id;
        V.SSAVer = 1;
        V.Size = PointerSize;
        return V;
      };

      MedFunc Func;
      Func.Entry = CallerVA;
      Func.Name = "deep_fragment_escape";
      MedBlock Block;
      Block.Id = 0;
      Block.StartAddr = CallerVA;
      Block.EndAddr = CallerVA + 4;

      MedVar Previous = temp(1000);
      MedOp Seed;
      Seed.Opcode = NdOp::COPY;
      Seed.Output = Previous;
      Seed.addInput(MedVar::makeConst(
          TextVA, PointerSize, ConstantAddressProvenance::AddressFragment));
      Block.Ops.push_back(std::move(Seed));

      // The authoritative escape audit is a function-wide fixed-point closure;
      // depth must neither erase a real fragment nor depend on recursion
      // limits in one particular sink helper.
      for (int I = 0; I < 600; ++I) {
        MedVar Next = temp(1001 + I);
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output = Next;
        Copy.addInput(Previous);
        Block.Ops.push_back(std::move(Copy));
        Previous = Next;
      }

      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(MedVar::makeConst(DataVA, PointerSize));
      Store.addInput(Previous);
      Block.Ops.push_back(std::move(Store));
      Func.Blocks.push_back(std::move(Block));

      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Func}, Context, "fragment-escape",
                                          TargetArch, {}, &Image, Format);
      EXPECT_EQ(Module, nullptr);
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsDirectAndDeepFragmentSelectConditionsAcrossFormats) {
  constexpr Arch TargetArch = Arch::AArch64;
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);

  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (unsigned ForwardingDepth : {0u, 600u}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(ForwardingDepth == 0 ? "direct-condition"
                                        : "deep-condition");
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

      auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
        MedVar V;
        V.Kind = Kind;
        V.TheArch = TargetArch;
        V.Id = Id;
        V.SSAVer = 1;
        V.Size = Size;
        return V;
      };

      MedFunc Func;
      Func.Entry = CallerVA;
      Func.Name = ForwardingDepth == 0 ? "direct_fragment_select_condition"
                                       : "deep_fragment_select_condition";
      Func.ReturnType = NdType::makeInt(PointerSize);

      MedBlock Block;
      Block.Id = 0;
      Block.StartAddr = CallerVA;
      Block.EndAddr = CallerVA + 0x10;

      MedVar Condition = makeVar(MedVar::Temp, 6000, 1);
      MedOp Compare;
      Compare.Opcode = NdOp::INT_NOTEQUAL;
      Compare.Output = Condition;
      Compare.addInput(MedVar::makeConst(
          TextVA, PointerSize, ConstantAddressProvenance::AddressFragment));
      Compare.addInput(
          MedVar::makeConst(0, PointerSize, ConstantAddressProvenance::Scalar));
      Block.Ops.push_back(std::move(Compare));

      for (unsigned I = 0; I < ForwardingDepth; ++I) {
        MedVar Forwarded = makeVar(MedVar::Temp, 6001 + static_cast<int>(I), 1);
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output = Forwarded;
        Copy.addInput(Condition);
        Block.Ops.push_back(std::move(Copy));
        Condition = Forwarded;
      }

      MedVar Selected = makeVar(MedVar::Temp, 7000, PointerSize);
      MedOp Select;
      Select.Opcode = NdOp::SELECT;
      Select.Output = Selected;
      Select.addInput(Condition);
      Select.addInput(MedVar::makeConst(11, PointerSize,
                                        ConstantAddressProvenance::Scalar));
      Select.addInput(MedVar::makeConst(22, PointerSize,
                                        ConstantAddressProvenance::Scalar));
      Block.Ops.push_back(std::move(Select));

      MedVar ReturnReg = makeVar(MedVar::Reg, 7001, PointerSize);
      ReturnReg.RegOff = TRI.IntReturnReg;
      MedOp WriteReturn;
      WriteReturn.Opcode = NdOp::COPY;
      WriteReturn.Output = ReturnReg;
      WriteReturn.addInput(Selected);
      Block.Ops.push_back(std::move(WriteReturn));

      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Block.Ops.push_back(std::move(Return));
      Func.Blocks.push_back(std::move(Block));

      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Func}, Context, "fragment-select-condition",
                                TargetArch, {}, &Image, Format);
      EXPECT_EQ(Module, nullptr);
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     AtomicMemoryOwnerCompletesFragmentButValueEscapesAcrossFormats) {
  constexpr Arch TargetArch = Arch::AArch64;
  constexpr uint16_t AccessSize = 8;

  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(formatTraceName(Format));
    BinaryImage Image = makeWritablePointerTableImage(TargetArch, Format);

    auto makeTemp = [&](int Id, uint16_t Size) {
      MedVar V;
      V.Kind = MedVar::Temp;
      V.TheArch = TargetArch;
      V.Id = Id;
      V.SSAVer = 1;
      V.Size = Size;
      return V;
    };
    auto fragment = [] {
      return MedVar::makeConst(SpilledConstTableVA, AccessSize,
                               ConstantAddressProvenance::AddressFragment);
    };
    auto completeAddress = [] {
      return MedVar::makeConst(SpilledConstTableVA, AccessSize,
                               ConstantAddressProvenance::Address);
    };
    auto finishFunction = [](MedFunc &Func, MedBlock &Block) {
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Block.Ops.push_back(std::move(Return));
      Func.Blocks.push_back(std::move(Block));
    };

    MedFunc AddressOwner;
    AddressOwner.Entry = CallerVA;
    AddressOwner.Name = "fragment_atomic_address_owner";
    AddressOwner.ReturnType = NdType::makeVoid();
    MedBlock OwnerBlock;
    OwnerBlock.Id = 0;
    OwnerBlock.StartAddr = CallerVA;
    OwnerBlock.EndAddr = CallerVA + 8;
    MedOp AtomicAdd;
    AtomicAdd.Opcode = NdOp::ATOMIC_ADD;
    AtomicAdd.MemoryOrdering = NdMemoryOrdering::Relaxed;
    AtomicAdd.Output = makeTemp(8000, AccessSize);
    AtomicAdd.addInput(fragment());
    AtomicAdd.addInput(
        MedVar::makeConst(1, AccessSize, ConstantAddressProvenance::Scalar));
    OwnerBlock.Ops.push_back(std::move(AtomicAdd));
    finishFunction(AddressOwner, OwnerBlock);

    llvm::LLVMContext OwnerContext;
    auto OwnerModule = MedLLVMEmitter().emit({AddressOwner}, OwnerContext,
                                             "fragment-atomic-address-owner",
                                             TargetArch, {}, &Image, Format);
    ASSERT_NE(OwnerModule, nullptr);
    expectValidModule(*OwnerModule);
    llvm::Function *OwnerFunction = OwnerModule->getFunction(AddressOwner.Name);
    ASSERT_NE(OwnerFunction, nullptr);
    const llvm::AtomicRMWInst *EmittedRMW = nullptr;
    for (const llvm::BasicBlock &Block : *OwnerFunction)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *RMW =
                llvm::dyn_cast<llvm::AtomicRMWInst>(&Instruction)) {
          ASSERT_EQ(EmittedRMW, nullptr);
          EmittedRMW = RMW;
        }
    ASSERT_NE(EmittedRMW, nullptr);
    std::set<const llvm::Value *> SeenGlobals;
    EXPECT_TRUE(valueReferencesMaterializedGlobal(
        EmittedRMW->getPointerOperand(), SeenGlobals));
    std::set<const llvm::Value *> SeenIntegers;
    EXPECT_FALSE(valueReferencesInteger(EmittedRMW->getPointerOperand(),
                                        SpilledConstTableVA, SeenIntegers));

    MedFunc LSEAddressOwner;
    LSEAddressOwner.Entry = CallerVA;
    LSEAddressOwner.Name = "fragment_lse_address_owner";
    LSEAddressOwner.ReturnType = NdType::makeVoid();
    MedBlock LSEBlock;
    LSEBlock.Id = 0;
    LSEBlock.StartAddr = CallerVA;
    LSEBlock.EndAddr = CallerVA + 8;
    MedOp AtomicOr;
    AtomicOr.Opcode = NdOp::INTRINSIC;
    AtomicOr.MemoryOrdering = NdMemoryOrdering::Relaxed;
    AtomicOr.Output = makeTemp(8010, AccessSize);
    AtomicOr.addInput(
        MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_AtomicOr), 2,
                          ConstantAddressProvenance::Scalar));
    AtomicOr.addInput(
        MedVar::makeConst(1, AccessSize, ConstantAddressProvenance::Scalar));
    AtomicOr.addInput(fragment());
    AtomicOr.addInput(
        MedVar::makeConst(AccessSize, 2, ConstantAddressProvenance::Scalar));
    AtomicOr.addInput(
        MedVar::makeConst(0, 1, ConstantAddressProvenance::Scalar));
    LSEBlock.Ops.push_back(std::move(AtomicOr));
    finishFunction(LSEAddressOwner, LSEBlock);

    llvm::LLVMContext LSEContext;
    auto LSEModule = MedLLVMEmitter().emit({LSEAddressOwner}, LSEContext,
                                           "fragment-lse-address-owner",
                                           TargetArch, {}, &Image, Format);
    ASSERT_NE(LSEModule, nullptr);
    expectValidModule(*LSEModule);
    llvm::Function *LSEFunction = LSEModule->getFunction(LSEAddressOwner.Name);
    ASSERT_NE(LSEFunction, nullptr);
    const llvm::AtomicRMWInst *EmittedLSE = nullptr;
    for (const llvm::BasicBlock &Block : *LSEFunction)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *RMW =
                llvm::dyn_cast<llvm::AtomicRMWInst>(&Instruction)) {
          ASSERT_EQ(EmittedLSE, nullptr);
          EmittedLSE = RMW;
        }
    ASSERT_NE(EmittedLSE, nullptr);
    std::set<const llvm::Value *> SeenLSEGlobals;
    EXPECT_TRUE(valueReferencesMaterializedGlobal(
        EmittedLSE->getPointerOperand(), SeenLSEGlobals));
    std::set<const llvm::Value *> SeenLSEIntegers;
    EXPECT_FALSE(valueReferencesInteger(EmittedLSE->getPointerOperand(),
                                        SpilledConstTableVA, SeenLSEIntegers));

    MedFunc ExclusiveAddressOwner;
    ExclusiveAddressOwner.Entry = CallerVA;
    ExclusiveAddressOwner.Name = "fragment_exclusive_pair_address_owner";
    ExclusiveAddressOwner.ReturnType = NdType::makeVoid();
    MedBlock ExclusiveBlock;
    ExclusiveBlock.Id = 0;
    ExclusiveBlock.StartAddr = CallerVA;
    ExclusiveBlock.EndAddr = CallerVA + 8;
    MedOp ExclusiveLoad;
    ExclusiveLoad.Opcode = NdOp::INTRINSIC;
    ExclusiveLoad.MemoryOrdering = NdMemoryOrdering::Relaxed;
    ExclusiveLoad.Output = makeTemp(8020, 16);
    ExclusiveLoad.addInput(
        MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_Ldxp), 2,
                          ConstantAddressProvenance::Scalar));
    ExclusiveLoad.addInput(fragment());
    ExclusiveLoad.addInput(
        MedVar::makeConst(16, 2, ConstantAddressProvenance::Scalar));
    ExclusiveBlock.Ops.push_back(std::move(ExclusiveLoad));
    finishFunction(ExclusiveAddressOwner, ExclusiveBlock);

    llvm::LLVMContext ExclusiveContext;
    auto ExclusiveModule = MedLLVMEmitter().emit(
        {ExclusiveAddressOwner}, ExclusiveContext,
        "fragment-exclusive-address-owner", TargetArch, {}, &Image, Format);
    ASSERT_NE(ExclusiveModule, nullptr);
    expectValidModule(*ExclusiveModule);
    llvm::Function *ExclusiveFunction =
        ExclusiveModule->getFunction(ExclusiveAddressOwner.Name);
    ASSERT_NE(ExclusiveFunction, nullptr);
    const llvm::CallInst *EmittedExclusive = nullptr;
    for (const llvm::BasicBlock &Block : *ExclusiveFunction)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
          if (const llvm::Function *Called = Call->getCalledFunction();
              Called && Called->getName().starts_with("llvm.aarch64.ldxp")) {
            ASSERT_EQ(EmittedExclusive, nullptr);
            EmittedExclusive = Call;
          }
    ASSERT_NE(EmittedExclusive, nullptr);
    ASSERT_GE(EmittedExclusive->arg_size(), 1u);
    std::set<const llvm::Value *> SeenExclusiveGlobals;
    EXPECT_TRUE(valueReferencesMaterializedGlobal(
        EmittedExclusive->getArgOperand(0), SeenExclusiveGlobals));
    std::set<const llvm::Value *> SeenExclusiveIntegers;
    EXPECT_FALSE(valueReferencesInteger(EmittedExclusive->getArgOperand(0),
                                        SpilledConstTableVA,
                                        SeenExclusiveIntegers));

    MedFunc StoredValue;
    StoredValue.Entry = CallerVA;
    StoredValue.Name = "fragment_stored_value_escape";
    StoredValue.ReturnType = NdType::makeVoid();
    MedBlock StoreBlock;
    StoreBlock.Id = 0;
    StoreBlock.StartAddr = CallerVA;
    StoreBlock.EndAddr = CallerVA + 8;
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.addInput(completeAddress());
    Store.addInput(fragment());
    StoreBlock.Ops.push_back(std::move(Store));
    finishFunction(StoredValue, StoreBlock);

    llvm::LLVMContext StoreContext;
    auto StoreModule = MedLLVMEmitter().emit({StoredValue}, StoreContext,
                                             "fragment-stored-value",
                                             TargetArch, {}, &Image, Format);
    EXPECT_EQ(StoreModule, nullptr);

    MedFunc RMWValue;
    RMWValue.Entry = CallerVA;
    RMWValue.Name = "fragment_atomic_value_escape";
    RMWValue.ReturnType = NdType::makeVoid();
    MedBlock RMWBlock;
    RMWBlock.Id = 0;
    RMWBlock.StartAddr = CallerVA;
    RMWBlock.EndAddr = CallerVA + 8;
    MedOp EscapingRMW;
    EscapingRMW.Opcode = NdOp::ATOMIC_ADD;
    EscapingRMW.MemoryOrdering = NdMemoryOrdering::Relaxed;
    EscapingRMW.Output = makeTemp(8001, AccessSize);
    EscapingRMW.addInput(completeAddress());
    EscapingRMW.addInput(fragment());
    RMWBlock.Ops.push_back(std::move(EscapingRMW));
    finishFunction(RMWValue, RMWBlock);

    llvm::LLVMContext RMWContext;
    auto RMWModule =
        MedLLVMEmitter().emit({RMWValue}, RMWContext, "fragment-atomic-value",
                              TargetArch, {}, &Image, Format);
    EXPECT_EQ(RMWModule, nullptr);

    MedFunc LSEValue;
    LSEValue.Entry = CallerVA;
    LSEValue.Name = "fragment_lse_value_escape";
    LSEValue.ReturnType = NdType::makeVoid();
    MedBlock LSEValueBlock;
    LSEValueBlock.Id = 0;
    LSEValueBlock.StartAddr = CallerVA;
    LSEValueBlock.EndAddr = CallerVA + 8;
    MedOp EscapingLSE;
    EscapingLSE.Opcode = NdOp::INTRINSIC;
    EscapingLSE.MemoryOrdering = NdMemoryOrdering::Relaxed;
    EscapingLSE.Output = makeTemp(8030, AccessSize);
    EscapingLSE.addInput(
        MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_AtomicOr), 2,
                          ConstantAddressProvenance::Scalar));
    EscapingLSE.addInput(fragment());
    EscapingLSE.addInput(completeAddress());
    EscapingLSE.addInput(
        MedVar::makeConst(AccessSize, 2, ConstantAddressProvenance::Scalar));
    EscapingLSE.addInput(
        MedVar::makeConst(0, 1, ConstantAddressProvenance::Scalar));
    LSEValueBlock.Ops.push_back(std::move(EscapingLSE));
    finishFunction(LSEValue, LSEValueBlock);

    llvm::LLVMContext LSEValueContext;
    auto LSEValueModule =
        MedLLVMEmitter().emit({LSEValue}, LSEValueContext, "fragment-lse-value",
                              TargetArch, {}, &Image, Format);
    EXPECT_EQ(LSEValueModule, nullptr);
  }
}

TEST(LLVMDataPointerInvariantBoundary,
     DeepScalarForwardingDoesNotInventAddressFragmentTaint) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (Arch TargetArch : {Arch::AArch64, Arch::X64}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(TargetArch == Arch::AArch64 ? "arm64" : "x86_64");
      const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
      const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

      auto temp = [&](int Id) {
        MedVar V;
        V.Kind = MedVar::Temp;
        V.TheArch = TargetArch;
        V.Id = Id;
        V.SSAVer = 1;
        V.Size = PointerSize;
        return V;
      };

      MedFunc Func;
      Func.Entry = CallerVA;
      Func.Name = "deep_scalar_forwarding";
      Func.ReturnType = NdType::makeInt(PointerSize);
      MedBlock Block;
      Block.Id = 0;
      Block.StartAddr = CallerVA;
      Block.EndAddr = CallerVA + 4;

      MedVar Previous = temp(2000);
      MedOp Seed;
      Seed.Opcode = NdOp::COPY;
      Seed.Output = Previous;
      Seed.addInput(
          MedVar::makeConst(7, PointerSize, ConstantAddressProvenance::Scalar));
      Block.Ops.push_back(std::move(Seed));
      for (int I = 0; I < 600; ++I) {
        MedVar Next = temp(2001 + I);
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output = Next;
        Copy.addInput(Previous);
        Block.Ops.push_back(std::move(Copy));
        Previous = Next;
      }

      MedVar ReturnReg;
      ReturnReg.Kind = MedVar::Reg;
      ReturnReg.TheArch = TargetArch;
      ReturnReg.Id = 3000;
      ReturnReg.SSAVer = 1;
      ReturnReg.Size = PointerSize;
      ReturnReg.RegOff = TRI.IntReturnReg;
      MedOp WriteReturn;
      WriteReturn.Opcode = NdOp::COPY;
      WriteReturn.Output = ReturnReg;
      WriteReturn.addInput(Previous);
      Block.Ops.push_back(std::move(WriteReturn));
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Block.Ops.push_back(std::move(Return));
      Func.Blocks.push_back(std::move(Block));

      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Func}, Context, "deep-scalar",
                                          TargetArch, {}, &Image, Format);
      ASSERT_NE(Module, nullptr);
      expectValidModule(*Module);
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     AddressFragmentMaySpillLocallyButReloadStillCarriesTaint) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (bool EscapeReload : {false, true}) {
      SCOPED_TRACE(formatTraceName(Format));
      SCOPED_TRACE(EscapeReload ? "reload-escapes" : "local-spill-only");
      constexpr Arch TargetArch = Arch::AArch64;
      const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
      const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
      BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

      auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
        MedVar V;
        V.Kind = Kind;
        V.TheArch = TargetArch;
        V.Id = Id;
        V.SSAVer = 1;
        V.Size = Size;
        return V;
      };

      MedFunc Func;
      Func.Entry = CallerVA;
      Func.Name = EscapeReload ? "fragment_frame_reload_escape"
                               : "fragment_local_frame_spill";
      Func.ReturnType =
          EscapeReload ? NdType::makeInt(PointerSize) : NdType::makeVoid();
      Func.FrameSize = 16;

      MedVar SP = makeVar(MedVar::Reg, 5000, PointerSize);
      SP.SSAVer = 0;
      SP.RegOff = TRI.StackPointer;
      MedVar Slot = makeVar(MedVar::Temp, 5001, PointerSize);
      MedVar Reloaded = makeVar(MedVar::Temp, 5002, PointerSize);
      MedVar ReturnReg = makeVar(MedVar::Reg, 5003, PointerSize);
      ReturnReg.RegOff = TRI.IntReturnReg;

      MedBlock Block;
      Block.Id = 0;
      Block.StartAddr = CallerVA;
      Block.EndAddr = CallerVA + 4;
      MedOp FormSlot;
      FormSlot.Opcode = NdOp::INT_ADD;
      FormSlot.Output = Slot;
      FormSlot.addInput(SP);
      FormSlot.addInput(MedVar::makeConst(uint64_t(-8), PointerSize,
                                          ConstantAddressProvenance::Scalar));
      Block.Ops.push_back(std::move(FormSlot));

      MedOp Spill;
      Spill.Opcode = NdOp::STORE;
      Spill.addInput(Slot);
      Spill.addInput(MedVar::makeConst(
          TextVA, PointerSize, ConstantAddressProvenance::AddressFragment));
      Block.Ops.push_back(std::move(Spill));

      if (EscapeReload) {
        MedOp Reload;
        Reload.Opcode = NdOp::LOAD;
        Reload.Output = Reloaded;
        Reload.addInput(Slot);
        Block.Ops.push_back(std::move(Reload));
        MedOp WriteReturn;
        WriteReturn.Opcode = NdOp::COPY;
        WriteReturn.Output = ReturnReg;
        WriteReturn.addInput(Reloaded);
        Block.Ops.push_back(std::move(WriteReturn));
      }
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Block.Ops.push_back(std::move(Return));
      Func.Blocks.push_back(std::move(Block));

      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Func}, Context, "fragment-spill",
                                          TargetArch, {}, &Image, Format);
      if (EscapeReload) {
        EXPECT_EQ(Module, nullptr);
      } else {
        ASSERT_NE(Module, nullptr);
        expectValidModule(*Module);
      }
    }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsAddressFragmentReloadFromPartiallyOverlappingFrameSlot) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(formatTraceName(Format));
    constexpr Arch TargetArch = Arch::AArch64;
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
    BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

    auto makeVar = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
      MedVar V;
      V.Kind = Kind;
      V.TheArch = TargetArch;
      V.Id = Id;
      V.SSAVer = 1;
      V.Size = Size;
      return V;
    };

    MedFunc Func;
    Func.Entry = CallerVA;
    Func.Name = "fragment_partial_overlap_reload";
    Func.ReturnType = NdType::makeInt(PointerSize);
    Func.FrameSize = 16;

    MedVar SP = makeVar(MedVar::Reg, 5050, PointerSize);
    SP.SSAVer = 0;
    SP.RegOff = TRI.StackPointer;
    MedVar StoreSlot = makeVar(MedVar::Temp, 5051, PointerSize);
    MedVar LoadSlot = makeVar(MedVar::Temp, 5052, PointerSize);
    MedVar Reloaded = makeVar(MedVar::Temp, 5053, PointerSize);
    MedVar ReturnReg = makeVar(MedVar::Reg, 5054, PointerSize);
    ReturnReg.RegOff = TRI.IntReturnReg;

    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = CallerVA;
    Block.EndAddr = CallerVA + 0x18;

    MedOp FormStoreSlot;
    FormStoreSlot.Opcode = NdOp::INT_ADD;
    FormStoreSlot.Output = StoreSlot;
    FormStoreSlot.addInput(SP);
    FormStoreSlot.addInput(MedVar::makeConst(
        uint64_t(-8), PointerSize, ConstantAddressProvenance::Scalar));
    Block.Ops.push_back(std::move(FormStoreSlot));

    MedOp FormLoadSlot;
    FormLoadSlot.Opcode = NdOp::INT_ADD;
    FormLoadSlot.Output = LoadSlot;
    FormLoadSlot.addInput(SP);
    FormLoadSlot.addInput(MedVar::makeConst(uint64_t(-4), PointerSize,
                                            ConstantAddressProvenance::Scalar));
    Block.Ops.push_back(std::move(FormLoadSlot));

    MedOp Spill;
    Spill.Opcode = NdOp::STORE;
    Spill.addInput(StoreSlot);
    Spill.addInput(MedVar::makeConst(
        TextVA, PointerSize, ConstantAddressProvenance::AddressFragment));
    Block.Ops.push_back(std::move(Spill));

    // This pointer-width read starts four bytes into the spill.  Exact slot-key
    // equality is insufficient: its low half still contains fragment bytes.
    MedOp Reload;
    Reload.Opcode = NdOp::LOAD;
    Reload.Output = Reloaded;
    Reload.addInput(LoadSlot);
    Block.Ops.push_back(std::move(Reload));

    MedOp WriteReturn;
    WriteReturn.Opcode = NdOp::COPY;
    WriteReturn.Output = ReturnReg;
    WriteReturn.addInput(Reloaded);
    Block.Ops.push_back(std::move(WriteReturn));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(std::move(Return));
    Func.Blocks.push_back(std::move(Block));

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Func}, Context, "fragment-overlap",
                                        TargetArch, {}, &Image, Format);
    EXPECT_EQ(Module, nullptr);
  }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsAddressFragmentObservedAsAtomicOldFrameValue) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(formatTraceName(Format));
    constexpr Arch TargetArch = Arch::AArch64;
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
    BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

    auto makeVar = [&](MedVar::VarKind Kind, int Id) {
      MedVar V;
      V.Kind = Kind;
      V.TheArch = TargetArch;
      V.Id = Id;
      V.SSAVer = 1;
      V.Size = PointerSize;
      return V;
    };

    MedFunc Func;
    Func.Entry = CallerVA;
    Func.Name = "fragment_atomic_old_frame_value";
    Func.ReturnType = NdType::makeInt(PointerSize);
    Func.FrameSize = 16;

    MedVar SP = makeVar(MedVar::Reg, 5070);
    SP.SSAVer = 0;
    SP.RegOff = TRI.StackPointer;
    MedVar Slot = makeVar(MedVar::Temp, 5071);
    MedVar OldValue = makeVar(MedVar::Temp, 5072);
    MedVar ReturnReg = makeVar(MedVar::Reg, 5073);
    ReturnReg.RegOff = TRI.IntReturnReg;

    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = CallerVA;
    Block.EndAddr = CallerVA + 0x14;

    MedOp FormSlot;
    FormSlot.Opcode = NdOp::INT_ADD;
    FormSlot.Output = Slot;
    FormSlot.addInput(SP);
    FormSlot.addInput(MedVar::makeConst(uint64_t(-8), PointerSize,
                                        ConstantAddressProvenance::Scalar));
    Block.Ops.push_back(std::move(FormSlot));

    MedOp Spill;
    Spill.Opcode = NdOp::STORE;
    Spill.addInput(Slot);
    Spill.addInput(MedVar::makeConst(
        TextVA, PointerSize, ConstantAddressProvenance::AddressFragment));
    Block.Ops.push_back(std::move(Spill));

    // Atomic exchange returns the pre-write memory value.  Even though the new
    // value is scalar, the old value can expose the locally spilled fragment.
    MedOp Exchange;
    Exchange.Opcode = NdOp::ATOMIC_XCHG;
    Exchange.MemoryOrdering = NdMemoryOrdering::Relaxed;
    Exchange.Output = OldValue;
    Exchange.addInput(Slot);
    Exchange.addInput(
        MedVar::makeConst(0, PointerSize, ConstantAddressProvenance::Scalar));
    Block.Ops.push_back(std::move(Exchange));

    MedOp WriteReturn;
    WriteReturn.Opcode = NdOp::COPY;
    WriteReturn.Output = ReturnReg;
    WriteReturn.addInput(OldValue);
    Block.Ops.push_back(std::move(WriteReturn));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(std::move(Return));
    Func.Blocks.push_back(std::move(Block));

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Func}, Context, "fragment-atomic-old",
                                        TargetArch, {}, &Image, Format);
    EXPECT_EQ(Module, nullptr);
  }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsAddressFragmentReloadAfterBypassedLocalFrameSpill) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(formatTraceName(Format));
    constexpr Arch TargetArch = Arch::AArch64;
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
    BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

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
    Func.Name = "fragment_bypassed_frame_spill";
    Func.ReturnType = NdType::makeInt(PointerSize);
    Func.FrameSize = 16;

    MedVar Condition = makeVar(MedVar::Param, 5100, 1);
    Condition.RegOff = TRI.IntParamRegs[0];
    Func.Params.push_back(Condition);
    MedVar SP = makeVar(MedVar::Reg, 5101, PointerSize);
    SP.SSAVer = 0;
    SP.RegOff = TRI.StackPointer;
    MedVar Slot = makeVar(MedVar::Temp, 5102, PointerSize);
    MedVar Reloaded = makeVar(MedVar::Temp, 5103, PointerSize);
    MedVar ReturnReg = makeVar(MedVar::Reg, 5104, PointerSize);
    ReturnReg.RegOff = TRI.IntReturnReg;

    MedBlock Entry;
    Entry.Id = 0;
    Entry.StartAddr = CallerVA;
    Entry.EndAddr = CallerVA + 8;
    Entry.Succs = {1, 2};
    MedOp FormSlot;
    FormSlot.Opcode = NdOp::INT_ADD;
    FormSlot.Output = Slot;
    FormSlot.addInput(SP);
    FormSlot.addInput(MedVar::makeConst(uint64_t(-8), PointerSize,
                                        ConstantAddressProvenance::Scalar));
    Entry.Ops.push_back(std::move(FormSlot));
    MedOp ChoosePath;
    ChoosePath.Opcode = NdOp::COND_BR;
    ChoosePath.addInput(MedVar::makeConst(CallerVA + 0x10, PointerSize));
    ChoosePath.addInput(Condition);
    Entry.Ops.push_back(std::move(ChoosePath));

    MedBlock Spill;
    Spill.Id = 1;
    Spill.StartAddr = CallerVA + 8;
    Spill.EndAddr = CallerVA + 0x10;
    Spill.Preds = {0};
    Spill.Succs = {3};
    MedOp StoreFragment;
    StoreFragment.Opcode = NdOp::STORE;
    StoreFragment.addInput(Slot);
    StoreFragment.addInput(MedVar::makeConst(
        TextVA, PointerSize, ConstantAddressProvenance::AddressFragment));
    Spill.Ops.push_back(std::move(StoreFragment));
    MedOp SpillToJoin;
    SpillToJoin.Opcode = NdOp::BRANCH;
    SpillToJoin.addInput(MedVar::makeConst(CallerVA + 0x20, PointerSize));
    Spill.Ops.push_back(std::move(SpillToJoin));

    MedBlock Bypass;
    Bypass.Id = 2;
    Bypass.StartAddr = CallerVA + 0x10;
    Bypass.EndAddr = CallerVA + 0x18;
    Bypass.Preds = {0};
    Bypass.Succs = {3};
    MedOp BypassToJoin;
    BypassToJoin.Opcode = NdOp::BRANCH;
    BypassToJoin.addInput(MedVar::makeConst(CallerVA + 0x20, PointerSize));
    Bypass.Ops.push_back(std::move(BypassToJoin));

    MedBlock Join;
    Join.Id = 3;
    Join.StartAddr = CallerVA + 0x20;
    Join.EndAddr = CallerVA + 0x2c;
    Join.Preds = {1, 2};
    MedOp Reload;
    Reload.Opcode = NdOp::LOAD;
    Reload.Output = Reloaded;
    Reload.addInput(Slot);
    Join.Ops.push_back(std::move(Reload));
    MedOp WriteReturn;
    WriteReturn.Opcode = NdOp::COPY;
    WriteReturn.Output = ReturnReg;
    WriteReturn.addInput(Reloaded);
    Join.Ops.push_back(std::move(WriteReturn));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Join.Ops.push_back(std::move(Return));

    Func.Blocks = {std::move(Entry), std::move(Spill), std::move(Bypass),
                   std::move(Join)};

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Func}, Context, "fragment-bypass",
                                        TargetArch, {}, &Image, Format);
    EXPECT_EQ(Module, nullptr);
  }
}

TEST(LLVMDataPointerInvariantBoundary,
     RejectsAddressFragmentReturnedThroughSharedAArch64Epilogue) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(formatTraceName(Format));
    constexpr Arch TargetArch = Arch::AArch64;
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    const uint16_t PointerSize = static_cast<uint16_t>(TRI.PointerSize);
    BinaryImage Image = makeSpilledConstTableImage(TargetArch, Format);

    MedVar ReturnReg;
    ReturnReg.Kind = MedVar::Reg;
    ReturnReg.TheArch = TargetArch;
    ReturnReg.Id = 4000;
    ReturnReg.SSAVer = 1;
    ReturnReg.Size = PointerSize;
    ReturnReg.RegOff = TRI.IntReturnReg;

    MedFunc Func;
    Func.Entry = CallerVA;
    Func.Name = "shared_epilogue_fragment_return";
    Func.ReturnType = NdType::makeInt(PointerSize);

    MedBlock Producer;
    Producer.Id = 0;
    Producer.StartAddr = CallerVA;
    Producer.EndAddr = CallerVA + 8;
    Producer.Succs = {1};
    MedOp WriteReturn;
    WriteReturn.Opcode = NdOp::COPY;
    WriteReturn.Output = ReturnReg;
    WriteReturn.addInput(MedVar::makeConst(
        TextVA, PointerSize, ConstantAddressProvenance::AddressFragment));
    Producer.Ops.push_back(std::move(WriteReturn));
    MedOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.addInput(MedVar::makeConst(CallerVA + 0x10, PointerSize));
    Producer.Ops.push_back(std::move(Branch));

    MedBlock Epilogue;
    Epilogue.Id = 1;
    Epilogue.StartAddr = CallerVA + 0x10;
    Epilogue.EndAddr = CallerVA + 0x14;
    Epilogue.Preds = {0};
    MedVar LinkRegister;
    LinkRegister.Kind = MedVar::Reg;
    LinkRegister.TheArch = TargetArch;
    LinkRegister.Id = 4001;
    LinkRegister.SSAVer = 0;
    LinkRegister.Size = PointerSize;
    LinkRegister.RegOff = TRI.LinkRegister;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(LinkRegister);
    Epilogue.Ops.push_back(std::move(Return));
    Func.Blocks = {std::move(Producer), std::move(Epilogue)};

    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Func}, Context, "shared-epilogue-fragment",
                              TargetArch, {}, &Image, Format);
    EXPECT_EQ(Module, nullptr);
  }
}

} // namespace
