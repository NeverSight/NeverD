//===- DWARFLoader.cpp - DWARF debug info loader -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// DWARF debug information loading implementation.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/DWARFLoader.h"

#include "neverd/debug/CompressedSections.h"
#include "neverd/debug/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/ARM/ARMRegs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/support/FilePath.h"
#include "neverd/types/DWARFCallingConvention.h"

#define DEBUG_TYPE "neverd-dwarf-loader"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/DebugInfo/DWARF/DWARFAbbreviationDeclaration.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/DebugInfo/DWARF/DWARFLocationExpression.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/DebugInfo/DWARF/LowLevel/DWARFExpression.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <span>

namespace neverd {

namespace dwarf_loader_detail {

namespace {

struct StrictAttributeLookup {
  enum class Status : uint8_t { Absent, Found, Malformed };
  Status TheStatus = Status::Absent;
  llvm::DWARFFormValue Value;
};

bool hasCompleteAttributePayload(
    llvm::DWARFDie Die, const llvm::DWARFAbbreviationDeclaration &Abbrev) {
  const llvm::DWARFUnit *Unit = Die.getDwarfUnit();
  if (!Unit)
    return false;
  const llvm::DWARFDataExtractor Data = Unit->getDebugInfoExtractor();
  const uint64_t UnitEnd = Unit->getNextUnitOffset();
  const uint64_t DieOffset = Die.getOffset();
  if (DieOffset > UnitEnd || Abbrev.getCodeByteSize() > UnitEnd - DieOffset)
    return false;
  uint64_t ExpectedOffset = DieOffset + Abbrev.getCodeByteSize();
  const auto Specs = Abbrev.attributes();
  auto Spec = Specs.begin();
  const auto SpecEnd = Specs.end();
  for (const llvm::DWARFAttribute &Attribute : Die.attributes()) {
    if (Spec == SpecEnd || Attribute.Attr != Spec->Attr ||
        Attribute.Offset != ExpectedOffset)
      return false;
    const std::optional<int64_t> FixedSize = Spec->getByteSize(*Unit);
    if (FixedSize) {
      if (*FixedSize < 0 ||
          static_cast<uint64_t>(*FixedSize) != Attribute.ByteSize)
        return false;
    } else if (Attribute.ByteSize == 0) {
      return false;
    }
    if (ExpectedOffset > UnitEnd ||
        Attribute.ByteSize > UnitEnd - ExpectedOffset ||
        !Data.isValidOffsetForDataOfSize(ExpectedOffset, Attribute.ByteSize))
      return false;
    ExpectedOffset += Attribute.ByteSize;
    ++Spec;
  }
  return Spec == SpecEnd;
}

/// Complete source values may not inherit a guessed field from truncated or
/// duplicate attributes. Display-only conversion keeps its existing policy.
bool completeValueAttributes(llvm::DWARFDie Die) {
  if (!Die.isValid())
    return false;
  const auto *Abbrev = Die.getAbbreviationDeclarationPtr();
  if (!Abbrev || !hasCompleteAttributePayload(Die, *Abbrev))
    return false;
  llvm::SmallDenseSet<unsigned, 16> Seen;
  for (const auto &Spec : Abbrev->attributes())
    if (!Seen.insert(Spec.Attr).second)
      return false;
  return true;
}

StrictAttributeLookup strictAttribute(llvm::DWARFDie Die,
                                      llvm::dwarf::Attribute Wanted) {
  if (!Die.isValid())
    return {StrictAttributeLookup::Status::Malformed, {}};
  const llvm::DWARFAbbreviationDeclaration *Abbrev =
      Die.getAbbreviationDeclarationPtr();
  if (!Abbrev || !hasCompleteAttributePayload(Die, *Abbrev))
    return {StrictAttributeLookup::Status::Malformed, {}};

  unsigned Count = 0;
  for (const llvm::DWARFAbbreviationDeclaration::AttributeSpec &Spec :
       Abbrev->attributes())
    if (Spec.Attr == Wanted)
      ++Count;
  if (Count == 0)
    return {};
  if (Count != 1)
    return {StrictAttributeLookup::Status::Malformed, {}};

  const std::optional<llvm::DWARFFormValue> Value = Die.find(Wanted);
  return Value ? StrictAttributeLookup{StrictAttributeLookup::Status::Found,
                                       *Value}
               : StrictAttributeLookup{StrictAttributeLookup::Status::Malformed,
                                       {}};
}

bool rangesOverlap(std::span<const SubprogramRange> Left,
                   std::span<const SubprogramRange> Right) {
  for (const SubprogramRange &L : Left)
    for (const SubprogramRange &R : Right)
      if (L.first < R.second && R.first < L.second)
        return true;
  return false;
}

bool malformedRanges(std::span<const SubprogramRange> Ranges) {
  return Ranges.empty() || std::any_of(Ranges.begin(), Ranges.end(),
                                       [](const SubprogramRange &Range) {
                                         return Range.first >= Range.second;
                                       });
}

} // namespace

void SubprogramExtentRegistry::insert(va_t Entry,
                                      std::span<const SubprogramRange> Ranges) {
  Record New;
  New.Entry = Entry;
  New.Ranges.assign(Ranges.begin(), Ranges.end());
  New.Ambiguous = Entry == InvalidVA || malformedRanges(Ranges);

  Records.push_back(std::move(New));
  Dirty = true;
}

void SubprogramExtentRegistry::finalize() const {
  if (!Dirty)
    return;
  AmbiguousEntries.clear();
  std::unordered_set<va_t> Entries;
  struct Extent {
    va_t Begin, End;
    size_t Owner;
  };
  std::vector<Extent> Extents;
  std::vector<size_t> Malformed;
  for (size_t I = 0; I < Records.size(); ++I) {
    const Record &R = Records[I];
    if (R.Ambiguous || !Entries.insert(R.Entry).second)
      AmbiguousEntries.insert(R.Entry);
    for (const auto &[Begin, End] : R.Ranges) {
      if (Begin < End)
        Extents.push_back({Begin, End, I});
      else
        Malformed.push_back(I);
    }
  }
  std::sort(Extents.begin(), Extents.end(),
            [](const Extent &A, const Extent &B) {
              return std::tie(A.Begin, A.End, A.Owner) <
                     std::tie(B.Begin, B.End, B.Owner);
            });
  // Connected overlap components need only one sweep. Once a component
  // contains two DIEs, each of its owners overlaps a different owner. Ranges
  // of a single discontiguous DIE do not make that DIE ambiguous by itself.
  for (size_t First = 0; First < Extents.size();) {
    size_t Last = First + 1;
    va_t End = Extents[First].End;
    bool Different = false;
    for (; Last < Extents.size() && Extents[Last].Begin < End; ++Last) {
      End = std::max(End, Extents[Last].End);
      Different |= Extents[Last].Owner != Extents[First].Owner;
    }
    if (Different)
      for (size_t I = First; I < Last; ++I)
        AmbiguousEntries.insert(Records[Extents[I].Owner].Entry);
    First = Last;
  }
  // Retain the old conservative comparison for malformed extents. They are
  // never a trusted owner, but a containing valid DIE must stay ambiguous.
  for (size_t I : Malformed)
    for (const Record &R : Records)
      if (!AmbiguousEntries.contains(R.Entry) &&
          rangesOverlap(R.Ranges, Records[I].Ranges))
        AmbiguousEntries.insert(R.Entry);
  Dirty = false;
}

void SubprogramExtentRegistry::append(SubprogramExtentRegistry &&Other) {
  Records.insert(Records.end(), std::make_move_iterator(Other.Records.begin()),
                 std::make_move_iterator(Other.Records.end()));
  Dirty = true;
}

bool SubprogramExtentRegistry::isAmbiguous(va_t Entry) const {
  finalize();
  return AmbiguousEntries.contains(Entry);
}

ReturnTypeProvenanceStatus
mergeReturnTypeProvenance(ReturnTypeProvenanceStatus Current,
                          ReturnTypeProvenanceStatus Candidate,
                          bool SameConcreteType) {
  using Status = ReturnTypeProvenanceStatus;
  if (Current == Status::Malformed || Candidate == Status::Malformed)
    return Status::Malformed;
  if (Current == Status::Unspecified)
    return Candidate;
  if (Candidate == Status::Unspecified)
    return Current;
  if (Current != Candidate)
    return Status::Malformed;
  if (Current == Status::Value && !SameConcreteType)
    return Status::Malformed;
  return Current;
}

DecodedLocationExpression decodeLocationExpressionShape(
    uint8_t Opcode, std::span<const uint64_t> Operands, bool Complete,
    bool IsFrameBase, std::optional<unsigned> StackPointerRegister,
    std::optional<unsigned> FramePointerRegister) {
  using Base = LocationExpressionBase;
  DecodedLocationExpression Result;
  auto finish = [&](Base Coordinate, int64_t Offset) {
    Result.Base = Complete ? Coordinate : Base::Malformed;
    Result.Offset = Offset;
  };
  auto coordinateForRegister = [&](uint64_t Register) -> std::optional<Base> {
    if (StackPointerRegister && Register == *StackPointerRegister)
      return Base::StackPointer;
    if (FramePointerRegister && Register == *FramePointerRegister)
      return Base::FramePointer;
    return std::nullopt;
  };

  if (Opcode == llvm::dwarf::DW_OP_fbreg && Operands.size() == 1) {
    finish(Base::FrameBase, static_cast<int64_t>(Operands[0]));
  } else if (Opcode == llvm::dwarf::DW_OP_call_frame_cfa && Operands.empty()) {
    finish(Base::CFA, 0);
  } else if (Opcode >= llvm::dwarf::DW_OP_breg0 &&
             Opcode <= llvm::dwarf::DW_OP_breg31 && Operands.size() == 1) {
    const uint64_t Register = Opcode - llvm::dwarf::DW_OP_breg0;
    if (const auto Coordinate = coordinateForRegister(Register))
      finish(*Coordinate, static_cast<int64_t>(Operands[0]));
  } else if (Opcode == llvm::dwarf::DW_OP_bregx && Operands.size() == 2) {
    if (const auto Coordinate = coordinateForRegister(Operands[0]))
      finish(*Coordinate, static_cast<int64_t>(Operands[1]));
  } else if (Opcode >= llvm::dwarf::DW_OP_reg0 &&
             Opcode <= llvm::dwarf::DW_OP_reg31 && Operands.empty()) {
    const uint64_t Register = Opcode - llvm::dwarf::DW_OP_reg0;
    if (IsFrameBase) {
      if (const auto Coordinate = coordinateForRegister(Register))
        finish(*Coordinate, 0);
    } else {
      // A register location holds the value itself and therefore cannot
      // overlap a memory-backed stack object.
      finish(Base::Register, Register);
    }
  } else if (Opcode == llvm::dwarf::DW_OP_regx && Operands.size() == 1) {
    if (IsFrameBase) {
      if (const auto Coordinate = coordinateForRegister(Operands[0]))
        finish(*Coordinate, 0);
    } else {
      finish(Base::Register, Operands[0]);
    }
  } else if (!IsFrameBase && Opcode == llvm::dwarf::DW_OP_addr &&
             Operands.size() == 1) {
    // A complete fixed-address location is outside the stack coordinate
    // space.  Indexed addresses are resolved by DWARFExpression to the same
    // opcode/operand shape before this policy is applied.
    finish(Base::NonStack, 0);
  }
  return Result;
}

} // namespace dwarf_loader_detail

using dwarf_loader_detail::completeValueAttributes;
using dwarf_loader_detail::strictAttribute;
using dwarf_loader_detail::StrictAttributeLookup;

struct DWARFDebugContext::Impl {
  std::unique_ptr<llvm::MemoryBuffer> BinaryBuf;
  std::unique_ptr<llvm::MemoryBuffer> DSYMBuf;
  /// Held by the queries that read DwarfCtx after loading, whose lazy
  /// caches no two readers may fill at once.
  mutable std::mutex ContextMutex;
  /// The object's bytes with its compressed sections expanded, which the
  /// DWARF reader reads in its place (CompressedSections.h).
  std::vector<uint8_t> ExpandedBytes;
  /// The object DwarfCtx reads, which it resolves a relocatable object's
  /// relocations through whenever it reads a value: it outlives DwarfCtx.
  std::unique_ptr<llvm::object::ObjectFile> Object;
  std::unique_ptr<llvm::DWARFContext> DwarfCtx;

  std::map<va_t, FunctionSym> FuncMap;
  std::vector<FunctionSym> FuncList;
  std::map<va_t, AuthenticatedReturnValueState> ReturnValueStates;
  std::map<va_t, std::vector<AuthenticatedRecordParameter>> RecordParameters;
  llvm::Triple::ArchType MachineArch = llvm::Triple::UnknownArch;

  using AddressRange = std::pair<va_t, va_t>;

  using StackCoordinate = dwarf_loader_detail::LocationExpressionBase;

  struct Location {
    std::optional<AddressRange> Range;
    StackCoordinate Coordinate = StackCoordinate::Malformed;
    int64_t Offset = 0;
  };

  struct LocalObject {
    VariableSym Variable;
    std::vector<AddressRange> ScopeRanges;
    std::vector<Location> Locations;
  };

  struct FunctionInfo {
    std::vector<AddressRange> Ranges;
    std::vector<Location> FrameBases;
    /// The subprogram DIE, parsed again when a decompile asks for its locals.
    /// Keeping every function's locations resident is a second copy of the
    /// debug line and location lists.
    uint64_t LocalDieOffset = 0;
    unsigned LocalUnitIndex = 0;
    bool HasLocalDie = false;
  };

  std::map<va_t, FunctionInfo> Functions;
  /// Locals of the one function a decompile is reading. The unit that owns
  /// the next function replaces both the cache and the parsed unit.
  mutable std::unique_ptr<llvm::DWARFContext> LocalCtx;
  mutable llvm::DWARFUnit *LocalUnit = nullptr;
  mutable unsigned LocalUnitIndex = 0;
  mutable bool LocalUnitReady = false;
  mutable va_t CachedLocalEntry = 0;
  mutable bool CachedLocalValid = false;
  mutable std::vector<LocalObject> CachedLocals;
  /// Each range of each of Functions, by its start, and the greatest end of
  /// the ranges up to each, so that the functions whose ranges contain an
  /// address are found without reading every function (functionsContaining).
  struct FunctionRange {
    va_t Begin;
    va_t End;
    va_t Entry;
  };
  std::vector<FunctionRange> FunctionRanges;
  std::vector<va_t> FunctionRangeEndsSoFar;
  /// Reads FunctionRanges from Functions, which loading has filled.
  void indexFunctionRanges();
  /// The entries of the functions one of whose ranges contains \p PC, in
  /// entry order and each once: those of Functions containsPC finds.
  llvm::SmallVector<va_t, 4> functionsContaining(va_t PC) const;
  dwarf_loader_detail::SubprogramExtentRegistry SubprogramExtents;
  std::vector<DataObjectSym> DataObjects;
  /// Indexes of DataObjects in address order. Equal addresses keep the
  /// order they were parsed, which resolveDataObject walks.
  std::vector<size_t> DataObjectByAddress;
  void indexDataObjects();

  std::optional<unsigned> DwarfStackPointerReg;
  std::optional<unsigned> DwarfFramePointerReg;

  bool Loaded = false;
  bool AuthenticatedObjectExtents = false;

  /// The type a DIE declares, found on the DIE or on the declarations its
  /// DW_AT_specification and DW_AT_abstract_origin chain names.
  struct DeclaredTypeResolution {
    enum class Status : uint8_t { Absent, Found, Malformed };
    Status TheStatus = Status::Absent;
    llvm::DWARFDie TypeDie;
  };

  TypeRef convertDwarfType(llvm::DWARFDie Die, int Depth = 0,
                           bool CompleteValue = false) const;
  using RecordSignatureTypes =
      llvm::DenseMap<std::pair<const llvm::DWARFUnit *, uint64_t>, TypeRef>;
  TypeRef signatureType(llvm::DWARFDie Die,
                        RecordSignatureTypes &Records) const;
  std::optional<AuthenticatedRecordParameter>
  recordParameter(llvm::DWARFDie Die, va_t Entry) const;
  /// Where each formal parameter of the i386 function \p Die enters it at
  /// \p Entry, where one enters in a register: GCC passes a local function's
  /// first three there (regparm) and clang its first two (fastcc), where
  /// cdecl, the platform's convention, passes none.  Empty where none does.
  std::vector<SourceABIValueLocation>
  parameterEntryLocations(llvm::DWARFDie Die, va_t Entry) const;
  /// Where the formal parameter \p Param enters its function at \p Entry: the
  /// general register a location covering the entry names, or the one whose
  /// entry value another location takes; the caller's slots, where the
  /// covering location is memory the frame base or ESP addresses; else none.
  SourceABIValueLocation parameterEntryLocation(llvm::DWARFDie Param,
                                                va_t Entry) const;
  DeclaredTypeResolution resolveDeclaredType(llvm::DWARFDie Die) const;
  llvm::DWARFDie declaredTypeDie(llvm::DWARFDie Die) const;
  AuthenticatedReturnValueState classifyReturnValue(llvm::DWARFDie Die,
                                                    int Depth = 0) const;
  std::vector<Location> decodeLocations(llvm::DWARFDie Die,
                                        llvm::dwarf::Attribute Attr,
                                        llvm::DWARFUnit *Unit,
                                        bool IsFrameBase) const;
  VariableExtentLookup lookupLocal(va_t FuncAddr, va_t UsePC, int64_t Offset,
                                   StackCoordinate Coordinate) const;
  /// The subprogram's locals. The caller holds ContextMutex.
  const std::vector<LocalObject> &localsFor(va_t Entry,
                                            const FunctionInfo &Function) const;
  void parseCompileUnits(const llvm::object::ObjectFile &Object);
  void parseUnits(size_t Begin, size_t End);
  void parseFunction(llvm::DWARFDie Die, llvm::DWARFUnit *Unit,
                     unsigned UnitIndex, RecordSignatureTypes &Records);
  void parseFunctionLocals(llvm::DWARFDie Die,
                           llvm::ArrayRef<AddressRange> ScopeRanges,
                           std::vector<LocalObject> &Objects) const;
  void parseLocalVariable(llvm::DWARFDie Die, bool IsParam,
                          llvm::ArrayRef<AddressRange> ScopeRanges,
                          std::vector<LocalObject> &Objects) const;
  void parseGlobalVariable(llvm::DWARFDie Die, llvm::DWARFUnit *Unit);
};

DWARFDebugContext::~DWARFDebugContext() = default;

std::filesystem::path dSYMBundleFile(const std::filesystem::path &Bundle) {
  // A directory that cannot be read holds no bundle: the queries that ask
  // before a load must not throw.
  std::error_code EC;
  const auto DwarfDir = Bundle / "Contents" / "Resources" / "DWARF";
  if (!std::filesystem::exists(DwarfDir, EC))
    return {};
  for (std::filesystem::directory_iterator It(DwarfDir, EC), End;
       !EC && It != End; It.increment(EC))
    if (std::error_code FileEC; It->is_regular_file(FileEC))
      return It->path();
  return {};
}

std::filesystem::path findDSYMFile(const std::filesystem::path &BinaryPath) {
  std::error_code EC;
  auto Bundle = BinaryPath;
  Bundle += ".dSYM";
  if (auto File = dSYMBundleFile(Bundle); !File.empty())
    return File;

  auto Parent = BinaryPath.parent_path();
  if (Parent.empty())
    Parent = ".";
  const auto Stem = BinaryPath.stem();
  for (std::filesystem::directory_iterator It(Parent, EC), End;
       !EC && It != End; It.increment(EC)) {
    std::error_code EntryEC;
    if (It->path().extension() != ".dSYM" || !It->is_directory(EntryEC) ||
        It->path().stem() != Stem)
      continue;
    if (auto File = dSYMBundleFile(It->path()); !File.empty())
      return File;
  }
  return {};
}

static std::unique_ptr<llvm::object::ObjectFile> getObjectFromBuffer(
    llvm::MemoryBuffer &Buf,
    std::optional<llvm::Triple::ArchType> RequiredArch = std::nullopt,
    std::span<const uint8_t> PreferredBytes = {}) {
  auto hasBytes = [](const llvm::object::ObjectFile &Obj,
                     std::span<const uint8_t> Bytes) {
    const llvm::StringRef Data = Obj.getData();
    return Data.size() == Bytes.size() &&
           std::equal(Bytes.begin(), Bytes.end(),
                      reinterpret_cast<const uint8_t *>(Data.data()));
  };

  auto BinOr =
      llvm::object::ObjectFile::createObjectFile(Buf.getMemBufferRef());
  if (BinOr) {
    if (!RequiredArch || (*BinOr)->getArch() == *RequiredArch)
      return std::move(*BinOr);
    return nullptr;
  }
  llvm::consumeError(BinOr.takeError());

  auto UniOr =
      llvm::object::MachOUniversalBinary::create(Buf.getMemBufferRef());
  if (!UniOr) {
    llvm::consumeError(UniOr.takeError());
    return nullptr;
  }
  auto &Uni = *UniOr;
  std::unique_ptr<llvm::object::ObjectFile> Fallback;
  for (auto &ObjForArch : Uni->objects()) {
    auto ObjOr = ObjForArch.getAsObjectFile();
    if (ObjOr) {
      if (RequiredArch && (*ObjOr)->getArch() != *RequiredArch)
        continue;
      if (!PreferredBytes.empty() && hasBytes(**ObjOr, PreferredBytes))
        return std::move(*ObjOr);
      if (!Fallback)
        Fallback = std::move(*ObjOr);
      continue;
    }
    llvm::consumeError(ObjOr.takeError());
  }
  return Fallback;
}

/// \p Obj, or the copy of it whose compressed sections \p Expanded keeps
/// expanded, for a DWARF reader without decompressors.
static std::unique_ptr<llvm::object::ObjectFile>
withExpandedSections(std::unique_ptr<llvm::object::ObjectFile> Obj,
                     std::vector<uint8_t> &Expanded) {
  auto Bytes = expandCompressedSections(Obj->getData());
  if (!Bytes)
    return Obj;
  Expanded = std::move(*Bytes);
  auto Plain = llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(
      llvm::toStringRef(llvm::ArrayRef<uint8_t>(Expanded)),
      Obj->getFileName()));
  if (!Plain) {
    llvm::consumeError(Plain.takeError());
    Expanded.clear();
    return Obj;
  }
  return std::move(*Plain);
}

bool hasDWARFUnits(const std::filesystem::path &Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return false;
  auto Object = getObjectFromBuffer(**Buffer);
  if (!Object)
    return false;
  std::vector<uint8_t> Expanded;
  Object = withExpandedSections(std::move(Object), Expanded);
  const auto Context = llvm::DWARFContext::create(*Object);
  return Context && Context->getNumCompileUnits() > 0;
}

static bool objectBytesEqual(const llvm::object::ObjectFile &Obj,
                             std::span<const uint8_t> Bytes) {
  if (Bytes.empty())
    return false;
  const llvm::StringRef Data = Obj.getData();
  return Data.size() == Bytes.size() &&
         std::equal(Bytes.begin(), Bytes.end(),
                    reinterpret_cast<const uint8_t *>(Data.data()));
}

static std::optional<std::array<uint8_t, 16>>
machOUUID(const llvm::object::ObjectFile &Obj) {
  const auto *MachO = llvm::dyn_cast<llvm::object::MachOObjectFile>(&Obj);
  if (!MachO)
    return std::nullopt;
  llvm::ArrayRef<uint8_t> Bytes = MachO->getUuid();
  if (Bytes.size() != 16)
    return std::nullopt;
  std::array<uint8_t, 16> UUID{};
  std::copy(Bytes.begin(), Bytes.end(), UUID.begin());
  return UUID;
}

static std::optional<unsigned>
dwarfStackPointerRegister(llvm::Triple::ArchType Arch) {
  switch (Arch) {
  case llvm::Triple::x86:
    return 4;
  case llvm::Triple::x86_64:
    return 7;
  case llvm::Triple::arm:
  case llvm::Triple::armeb:
  case llvm::Triple::thumb:
  case llvm::Triple::thumbeb:
    return 13;
  case llvm::Triple::aarch64:
  case llvm::Triple::aarch64_be:
    return 31;
  case llvm::Triple::mips:
  case llvm::Triple::mipsel:
  case llvm::Triple::mips64:
  case llvm::Triple::mips64el:
    return 29;
  default:
    return std::nullopt;
  }
}

static std::optional<unsigned>
dwarfFramePointerRegister(llvm::Triple::ArchType Arch) {
  switch (Arch) {
  case llvm::Triple::x86:
    return 5;
  case llvm::Triple::x86_64:
    return 6;
  case llvm::Triple::arm:
  case llvm::Triple::armeb:
  case llvm::Triple::thumb:
  case llvm::Triple::thumbeb:
    return 11;
  case llvm::Triple::aarch64:
  case llvm::Triple::aarch64_be:
    return 29;
  case llvm::Triple::mips:
  case llvm::Triple::mipsel:
  case llvm::Triple::mips64:
  case llvm::Triple::mips64el:
    return 30;
  default:
    return std::nullopt;
  }
}

std::unique_ptr<DWARFDebugContext>
DWARFDebugContext::load(const std::filesystem::path &BinaryPath,
                        BinaryFormat Format, DWARFLoadTrust Trust,
                        std::span<const uint8_t> ExpectedImageBytes,
                        std::optional<llvm::Triple::ArchType> RequiredArch) {
  auto Ctx = std::unique_ptr<DWARFDebugContext>(new DWARFDebugContext());
  Ctx->PImpl = std::make_unique<Impl>();

  auto BufOr = llvm::MemoryBuffer::getFile(pathToUTF8(BinaryPath));
  if (!BufOr) {
    llvm::WithColor::warning()
        << "debug: cannot open " << pathToUTF8(BinaryPath) << "\n";
    return Ctx;
  }
  Ctx->PImpl->BinaryBuf = std::move(*BufOr);

  auto Obj = getObjectFromBuffer(*Ctx->PImpl->BinaryBuf, RequiredArch,
                                 ExpectedImageBytes);
  if (!Obj) {
    llvm::WithColor::warning()
        << "debug: cannot parse object file " << pathToUTF8(BinaryPath) << "\n";
    return Ctx;
  }

  const llvm::Triple::ArchType MainArch = Obj->getArch();
  const std::optional<std::array<uint8_t, 16>> MainUUID = machOUUID(*Obj);
  const bool SnapshotMatches = objectBytesEqual(*Obj, ExpectedImageBytes);
  Ctx->PImpl->DwarfStackPointerReg = dwarfStackPointerRegister(MainArch);
  Ctx->PImpl->MachineArch = MainArch;
  Ctx->PImpl->DwarfFramePointerReg = dwarfFramePointerRegister(MainArch);

  Obj = withExpandedSections(std::move(Obj), Ctx->PImpl->ExpandedBytes);
  Ctx->PImpl->DwarfCtx = llvm::DWARFContext::create(*Obj);
  if (!Ctx->PImpl->DwarfCtx) {
    llvm::WithColor::warning() << "debug: cannot create DWARFContext for "
                               << pathToUTF8(BinaryPath) << "\n";
    return Ctx;
  }

  bool HasDwarf = Ctx->PImpl->DwarfCtx->getNumCompileUnits() > 0;
  Ctx->PImpl->AuthenticatedObjectExtents =
      HasDwarf && Trust == DWARFLoadTrust::InImage && SnapshotMatches;

  if (!HasDwarf && Format == BinaryFormat::MachO) {
    auto DSYMPath = findDSYMFile(BinaryPath);
    if (!DSYMPath.empty()) {
      LLVM_DEBUG(llvm::dbgs() << "debug: loading dSYM from "
                              << pathToUTF8(DSYMPath) << "\n");
      auto DSYMBufOr = llvm::MemoryBuffer::getFile(pathToUTF8(DSYMPath));
      if (DSYMBufOr) {
        Ctx->PImpl->DSYMBuf = std::move(*DSYMBufOr);
        auto DSYMObj = getObjectFromBuffer(*Ctx->PImpl->DSYMBuf, MainArch);
        if (DSYMObj) {
          Ctx->PImpl->DwarfStackPointerReg =
              dwarfStackPointerRegister(DSYMObj->getArch());
          Ctx->PImpl->DwarfFramePointerReg =
              dwarfFramePointerRegister(DSYMObj->getArch());
          Ctx->PImpl->DwarfCtx = llvm::DWARFContext::create(*DSYMObj);
          HasDwarf = Ctx->PImpl->DwarfCtx &&
                     Ctx->PImpl->DwarfCtx->getNumCompileUnits() > 0;
          const std::optional<std::array<uint8_t, 16>> DSYMUUID =
              machOUUID(*DSYMObj);
          Ctx->PImpl->AuthenticatedObjectExtents =
              HasDwarf && Trust == DWARFLoadTrust::InImage && SnapshotMatches &&
              MainUUID && DSYMUUID && *MainUUID == *DSYMUUID;
          Obj = std::move(DSYMObj);
        }
      }
    }
  }

  if (!HasDwarf) {
    LLVM_DEBUG(llvm::dbgs() << "debug: no DWARF info found for "
                            << pathToUTF8(BinaryPath) << "\n");
    return Ctx;
  }

  LLVM_DEBUG(llvm::dbgs() << "debug: "
                          << Ctx->PImpl->DwarfCtx->getNumCompileUnits()
                          << " compile units found" << "\n");
  Ctx->PImpl->parseCompileUnits(*Obj);
  Ctx->PImpl->Object = std::move(Obj);
  Ctx->PImpl->Loaded = true;

  LLVM_DEBUG(llvm::dbgs() << "debug: loaded " << Ctx->PImpl->FuncList.size()
                          << " functions with debug info" << "\n");
  return Ctx;
}

std::vector<SourceABIValueLocation>
DWARFDebugContext::Impl::parameterEntryLocations(llvm::DWARFDie Die,
                                                 va_t Entry) const {
  std::vector<SourceABIValueLocation> Entries;
  if (MachineArch != llvm::Triple::x86)
    return Entries;
  bool InRegister = false;
  for (llvm::DWARFDie Child : Die.children()) {
    if (Child.getTag() != llvm::dwarf::DW_TAG_formal_parameter)
      continue;
    Entries.push_back(parameterEntryLocation(Child, Entry));
    InRegister |= Entries.back().Kind == SourceABICarrierKind::IntegerRegister;
  }
  if (!InRegister)
    Entries.clear();
  return Entries;
}

SourceABIValueLocation
DWARFDebugContext::Impl::parameterEntryLocation(llvm::DWARFDie Param,
                                                va_t Entry) const {
  auto Locations = Param.getLocations(llvm::dwarf::DW_AT_location);
  if (!Locations) {
    llvm::consumeError(Locations.takeError());
    return {};
  }
  // i386's DWARF numbers the general registers as its encoding does: EAX,
  // ECX, EDX, EBX, ESP, EBP, ESI, EDI.  No parameter enters in ESP.
  auto GeneralRegister = [](uint8_t Code) -> std::optional<uint64_t> {
    if (Code < llvm::dwarf::DW_OP_reg0 || Code > llvm::dwarf::DW_OP_reg7 ||
        Code == llvm::dwarf::DW_OP_reg4)
      return std::nullopt;
    return x86reg::generalReg(Code - llvm::dwarf::DW_OP_reg0);
  };
  std::optional<SourceABIValueLocation> Found;
  bool Conflict = false;
  auto Note = [&](SourceABICarrierKind Kind, uint64_t Register) {
    if (Found && (Found->Kind != Kind || Found->RegisterOffset != Register))
      Conflict = true;
    Found.emplace();
    Found->Kind = Kind;
    Found->RegisterOffset = Register;
  };
  auto *Unit = Param.getDwarfUnit();
  for (const auto &Location : *Locations) {
    llvm::StringRef Raw(reinterpret_cast<const char *>(Location.Expr.data()),
                        Location.Expr.size());
    llvm::DataExtractor Data(Raw, Unit->isLittleEndian(),
                             Unit->getAddressByteSize());
    llvm::DWARFExpression Expression(Data, Unit->getAddressByteSize(),
                                     Unit->getFormat());
    llvm::SmallVector<std::pair<uint8_t, uint64_t>, 4> Ops;
    bool Valid = true;
    for (const auto &Op : Expression) {
      if (Op.isError() || Ops.size() == 4) {
        Valid = false;
        break;
      }
      Ops.push_back({Op.getCode(),
                     Op.getNumOperands() ? Op.getRawOperand(0) : uint64_t()});
    }
    if (!Valid || Ops.empty())
      continue;
    // The value the register held at the entry, wherever the location
    // applies: DWARF 5's DW_OP_entry_value, GCC's DW_OP_GNU_entry_value.
    if (Ops.size() == 3 &&
        (Ops[0].first == llvm::dwarf::DW_OP_entry_value ||
         Ops[0].first == llvm::dwarf::DW_OP_GNU_entry_value) &&
        Ops[0].second == 1 && Ops[2].first == llvm::dwarf::DW_OP_stack_value) {
      if (const auto Register = GeneralRegister(Ops[1].first))
        Note(SourceABICarrierKind::IntegerRegister, *Register);
      continue;
    }
    if (Location.Range &&
        !(Location.Range->valid() && Location.Range->LowPC <= Entry &&
          Entry < Location.Range->HighPC))
      continue;
    if (Ops.size() != 1)
      continue;
    if (const auto Register = GeneralRegister(Ops[0].first))
      Note(SourceABICarrierKind::IntegerRegister, *Register);
    else if (Ops[0].first == llvm::dwarf::DW_OP_fbreg ||
             Ops[0].first == llvm::dwarf::DW_OP_breg4)
      Note(SourceABICarrierKind::Stack, 0);
  }
  if (!Found || Conflict)
    return {};
  return *Found;
}

std::optional<AuthenticatedRecordParameter>
DWARFDebugContext::Impl::recordParameter(llvm::DWARFDie Die, va_t Entry) const {
  llvm::DWARFDie Type = declaredTypeDie(Die);
  bool Pointer = false;
  unsigned Depth = 0;
  for (; Type &&
         Depth < ::neverd::limits::debug::
                     kDWARFLoaderDWARFDebugContextImplRecordParameterDepthLimit;
       ++Depth) {
    const auto Tag = Type.getTag();
    if (Tag == llvm::dwarf::DW_TAG_pointer_type) {
      if (Pointer || !Type.getDwarfUnit() ||
          Type.getDwarfUnit()->getAddressByteSize() != 8)
        return std::nullopt;
      auto Width = strictAttribute(Type, llvm::dwarf::DW_AT_byte_size);
      if (Width.TheStatus == StrictAttributeLookup::Status::Malformed ||
          (Width.TheStatus == StrictAttributeLookup::Status::Found &&
           Width.Value.getAsUnsignedConstant() != 8))
        return std::nullopt;
      Pointer = true;
    } else if (Tag != llvm::dwarf::DW_TAG_typedef &&
               Tag != llvm::dwarf::DW_TAG_const_type &&
               Tag != llvm::dwarf::DW_TAG_volatile_type &&
               Tag != llvm::dwarf::DW_TAG_restrict_type) {
      break;
    }
    auto Next = strictAttribute(Type, llvm::dwarf::DW_AT_type);
    if (Next.TheStatus != StrictAttributeLookup::Status::Found)
      return std::nullopt;
    Type = Type.getAttributeValueAsReferencedDie(Next.Value);
  }
  if (!Pointer || !Type ||
      Depth == ::neverd::limits::debug::kMaxDWARFUnwrapDepth ||
      (Type.getTag() != llvm::dwarf::DW_TAG_structure_type &&
       Type.getTag() != llvm::dwarf::DW_TAG_class_type))
    return std::nullopt;
  const auto Width = strictAttribute(Type, llvm::dwarf::DW_AT_byte_size);
  if (Width.TheStatus != StrictAttributeLookup::Status::Found)
    return std::nullopt;
  auto Bytes = Width.Value.getAsUnsignedConstant();
  if (!Bytes || *Bytes == 0 || *Bytes > (1u << 20) || !Type.getShortName())
    return std::nullopt;
  std::string Qualified;
  llvm::raw_string_ostream NameOS(Qualified);
  llvm::dumpTypeQualifiedName(Type, NameOS);
  if (Qualified.empty() ||
      Qualified.size() >
          ::neverd::limits::debug::
              kDWARFLoaderDWARFDebugContextImplRecordParameterQualifiedSizeLimit)
    return std::nullopt;

  auto Locations = Die.getLocations(llvm::dwarf::DW_AT_location);
  if (!Locations) {
    llvm::consumeError(Locations.takeError());
    return std::nullopt;
  }
  std::optional<unsigned> Register;
  for (const auto &Location : *Locations) {
    if (Location.Range &&
        !(Location.Range->valid() && Location.Range->LowPC <= Entry &&
          Entry < Location.Range->HighPC))
      continue;
    llvm::StringRef Raw(reinterpret_cast<const char *>(Location.Expr.data()),
                        Location.Expr.size());
    auto *Unit = Die.getDwarfUnit();
    llvm::DataExtractor Data(Raw, Unit->isLittleEndian(),
                             Unit->getAddressByteSize());
    llvm::DWARFExpression Expression(Data, Unit->getAddressByteSize(),
                                     Unit->getFormat());
    auto It = Expression.begin();
    if (It == Expression.end() || It->isError())
      return std::nullopt;
    const auto Code = It->getCode();
    std::optional<unsigned> Current;
    if (Code >= llvm::dwarf::DW_OP_reg0 && Code <= llvm::dwarf::DW_OP_reg31)
      Current = Code - llvm::dwarf::DW_OP_reg0;
    else if (Code == llvm::dwarf::DW_OP_regx &&
             It->getRawOperands().size() == 1 && It->getRawOperands()[0] <= 31)
      Current = static_cast<unsigned>(It->getRawOperands()[0]);
    ++It;
    if (!Current || It != Expression.end() || (Register && Register != Current))
      return std::nullopt;
    Register = Current;
  }
  if (!Register)
    return std::nullopt;
  uint64_t Physical = 0;
  if (MachineArch == llvm::Triple::aarch64 && *Register < 8) {
    Physical = getTargetRegInfo(Arch::AArch64).IntParamRegs[*Register];
  } else if (MachineArch == llvm::Triple::x86_64) {
    constexpr unsigned DwarfArgs[] = {5, 4, 1, 2, 8, 9};
    auto At = std::find(std::begin(DwarfArgs), std::end(DwarfArgs), *Register);
    if (At == std::end(DwarfArgs))
      return std::nullopt;
    Physical =
        getTargetRegInfo(Arch::X64).IntParamRegs[At - std::begin(DwarfArgs)];
  } else {
    return std::nullopt;
  }
  return AuthenticatedRecordParameter{Physical, std::move(Qualified),
                                      static_cast<uint32_t>(*Bytes)};
}

namespace {

/// The size and alignment of a value the debug information describes; the
/// alignment is 0 where the layout does not say it.
struct ValueShape {
  uint64_t Bytes = 0;
  uint64_t Align = 0;
};

/// The scalars of a value, as the debug information lays it out.
struct ValueLayout {
  std::vector<NdScalarLeaf> Leaves;
  /// False once a part's scalars are not described, or there are more than
  /// kMaxLayoutLeaves; the value's alignment may still be known.
  bool LeavesKnown = true;

  void add(uint64_t Offset, uint64_t Size, bool Floating) {
    if (!LeavesKnown ||
        Leaves.size() >= ::neverd::limits::debug::kDWARFLoaderMaxLayoutLeaves ||
        !Size || Offset + Size > UINT16_MAX) {
      LeavesKnown = false;
      return;
    }
    Leaves.push_back(
        {static_cast<uint16_t>(Offset), static_cast<uint16_t>(Size), Floating});
  }
};

// A scalar size does not imply an alignment above the native scalar ABI.
constexpr uint64_t MaxScalarAlignment = 16;

/// The alignment a scalar of \p Bytes has, or 0 when its size does not say
/// (the 12 bytes of an i386 long double).
uint64_t scalarAlignment(uint64_t Bytes) {
  return Bytes && Bytes <= MaxScalarAlignment && llvm::isPowerOf2_64(Bytes)
             ? Bytes
             : 0;
}

std::optional<uint64_t> constantAttribute(llvm::DWARFDie Die,
                                          llvm::dwarf::Attribute Attr) {
  if (auto Value = Die.find(Attr))
    return Value->getAsUnsignedConstant();
  return std::nullopt;
}

/// A subrange bound or count.  DW_FORM_sdata is signed and the other
/// constant forms are not, except that an all-ones upper bound, which
/// producers write for a zero-length array, is -1.
std::optional<int64_t> boundValue(const llvm::DWARFFormValue &Value) {
  if (Value.getForm() == llvm::dwarf::DW_FORM_sdata)
    return Value.getAsSignedConstant();
  const std::optional<uint64_t> Unsigned = Value.getAsUnsignedConstant();
  if (!Unsigned)
    return std::nullopt;
  if (*Unsigned == UINT64_MAX)
    return -1;
  if (*Unsigned > static_cast<uint64_t>(INT64_MAX))
    return std::nullopt;
  return static_cast<int64_t>(*Unsigned);
}

/// The elements of \p Array through all its dimensions; none when a bound is
/// not a constant or the elements are not contiguous.  A dimension without
/// bounds, a flexible array member, has none.
std::optional<uint64_t> arrayElements(llvm::DWARFDie Array) {
  if (Array.find(llvm::dwarf::DW_AT_byte_stride) ||
      Array.find(llvm::dwarf::DW_AT_bit_stride))
    return std::nullopt;
  uint64_t Count = 1;
  for (llvm::DWARFDie Child : Array.children()) {
    if (Child.getTag() != llvm::dwarf::DW_TAG_subrange_type)
      return std::nullopt;
    if (Child.find(llvm::dwarf::DW_AT_byte_stride) ||
        Child.find(llvm::dwarf::DW_AT_bit_stride))
      return std::nullopt;
    int64_t Extent = 0;
    if (auto Value = Child.find(llvm::dwarf::DW_AT_count)) {
      const std::optional<int64_t> Elements = boundValue(*Value);
      if (!Elements || *Elements < 0)
        return std::nullopt;
      Extent = *Elements;
    } else if (auto Upper = Child.find(llvm::dwarf::DW_AT_upper_bound)) {
      const std::optional<int64_t> Last = boundValue(*Upper);
      std::optional<int64_t> First = 0;
      if (auto Lower = Child.find(llvm::dwarf::DW_AT_lower_bound))
        First = boundValue(*Lower);
      if (!Last || !First || *Last < *First - 1)
        return std::nullopt;
      Extent = *Last - *First + 1;
    }
    if (static_cast<uint64_t>(Extent) >
        ::neverd::limits::debug::kDWARFLoaderMaxLayoutElements)
      return std::nullopt;
    Count *= static_cast<uint64_t>(Extent);
    if (Count > ::neverd::limits::debug::kDWARFLoaderMaxLayoutElements)
      return std::nullopt;
  }
  return Count;
}

/// The byte offset a DW_AT_data_member_location gives: a constant, or the
/// lone DW_OP_plus_uconst of older producers.  A virtual base's location is
/// an expression that depends on the object.
std::optional<uint64_t> memberLocation(const llvm::DWARFFormValue &Value) {
  if (auto Constant = Value.getAsUnsignedConstant())
    return *Constant;
  if (auto Block = Value.getAsBlock();
      Block && !Block->empty() &&
      (*Block)[0] == llvm::dwarf::DW_OP_plus_uconst) {
    unsigned Length = 0;
    const char *Error = nullptr;
    const uint64_t Offset = llvm::decodeULEB128(
        Block->data() + 1, &Length, Block->data() + Block->size(), &Error);
    if (!Error && 1 + Length == Block->size())
      return Offset;
  }
  return std::nullopt;
}

std::optional<ValueShape> layOut(llvm::DWARFDie Type, uint64_t Base, int Depth,
                                 ValueLayout &Layout);

/// Lays out the members and bases of \p Record at \p Base.
std::optional<ValueShape> layOutRecord(llvm::DWARFDie Record, uint64_t Base,
                                       int Depth, ValueLayout &Layout) {
  const std::optional<uint64_t> Bytes =
      constantAttribute(Record, llvm::dwarf::DW_AT_byte_size);
  if (!Bytes || Record.find(llvm::dwarf::DW_AT_declaration))
    return std::nullopt;
  uint64_t Align = 1;
  bool AlignKnown = true;
  for (llvm::DWARFDie Child : Record.children()) {
    const auto Tag = Child.getTag();
    // A variant part (a Rust enum's) overlays storage the members do not
    // list.
    if (Tag == llvm::dwarf::DW_TAG_variant_part) {
      Layout.LeavesKnown = AlignKnown = false;
      continue;
    }
    if ((Tag != llvm::dwarf::DW_TAG_member &&
         Tag != llvm::dwarf::DW_TAG_inheritance) ||
        Child.find(llvm::dwarf::DW_AT_external) ||
        Child.find(llvm::dwarf::DW_AT_declaration))
      continue;
    std::optional<uint64_t> Offset = 0;
    if (auto Location = Child.find(llvm::dwarf::DW_AT_data_member_location))
      Offset = memberLocation(*Location);
    const llvm::DWARFDie Type =
        Child.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    if (auto Width = constantAttribute(Child, llvm::dwarf::DW_AT_bit_size)) {
      if (!*Width)
        continue;
      // A bit field is an integer of the bytes that hold its bits; its
      // declared type aligns the record.
      ValueLayout Declared;
      const std::optional<ValueShape> Shape =
          layOut(Type, 0, Depth + 1, Declared);
      std::optional<uint64_t> First, Last;
      if (auto Bit =
              constantAttribute(Child, llvm::dwarf::DW_AT_data_bit_offset)) {
        First = *Bit / 8;
        Last = (*Bit + *Width - 1) / 8;
      } else if (Offset && Shape) {
        // DW_AT_bit_offset counts within the storage unit at the member's
        // location.
        First = *Offset;
        Last = *Offset +
               constantAttribute(Child, llvm::dwarf::DW_AT_byte_size)
                   .value_or(Shape->Bytes) -
               1;
      }
      if (!Shape || !First || !Last || *Last >= *Bytes) {
        Layout.LeavesKnown = AlignKnown = false;
        continue;
      }
      Layout.add(Base + *First, *Last - *First + 1, /*Floating=*/false);
      AlignKnown &= Shape->Align != 0;
      Align = std::max(Align, Shape->Align);
      continue;
    }
    if (!Offset) {
      Layout.LeavesKnown = AlignKnown = false;
      continue;
    }
    const std::optional<ValueShape> Shape =
        layOut(Type, Base + *Offset, Depth + 1, Layout);
    if (!Shape || *Offset + Shape->Bytes > *Bytes) {
      Layout.LeavesKnown = AlignKnown = false;
      continue;
    }
    // A member off its alignment is packed; the record's own alignment is
    // then not the largest of its members'.
    AlignKnown &= Shape->Align != 0 && *Offset % Shape->Align == 0;
    Align = std::max(Align, Shape->Align);
  }
  if (auto Explicit = constantAttribute(Record, llvm::dwarf::DW_AT_alignment))
    return ValueShape{*Bytes, *Explicit};
  return ValueShape{*Bytes, AlignKnown && *Bytes % Align == 0 ? Align : 0};
}

/// Lays out the value \p Type describes at \p Base: adds its scalars to
/// \p Layout and returns its size and alignment; none when its size is not
/// described.
std::optional<ValueShape> layOut(llvm::DWARFDie Type, uint64_t Base, int Depth,
                                 ValueLayout &Layout) {
  if (!Type.isValid() ||
      Depth > ::neverd::limits::debug::kDWARFLoaderMaxLayoutDepth) {
    Layout.LeavesKnown = false;
    return std::nullopt;
  }
  const std::optional<uint64_t> Bytes =
      constantAttribute(Type, llvm::dwarf::DW_AT_byte_size);
  switch (Type.getTag()) {
  case llvm::dwarf::DW_TAG_typedef:
  case llvm::dwarf::DW_TAG_const_type:
  case llvm::dwarf::DW_TAG_volatile_type:
  case llvm::dwarf::DW_TAG_restrict_type:
    return layOut(
        Type.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type), Base,
        Depth + 1, Layout);
  case llvm::dwarf::DW_TAG_base_type: {
    const std::optional<uint64_t> Encoding =
        constantAttribute(Type, llvm::dwarf::DW_AT_encoding);
    if (!Bytes || !Encoding)
      break;
    switch (*Encoding) {
    case llvm::dwarf::DW_ATE_float:
      Layout.add(Base, *Bytes, /*Floating=*/true);
      return ValueShape{*Bytes, scalarAlignment(*Bytes)};
    case llvm::dwarf::DW_ATE_complex_float:
      // The real part, then the imaginary part.
      if (*Bytes % 2)
        break;
      Layout.add(Base, *Bytes / 2, /*Floating=*/true);
      Layout.add(Base + *Bytes / 2, *Bytes / 2, /*Floating=*/true);
      return ValueShape{*Bytes, scalarAlignment(*Bytes / 2)};
    case llvm::dwarf::DW_ATE_address:
    case llvm::dwarf::DW_ATE_boolean:
    case llvm::dwarf::DW_ATE_signed:
    case llvm::dwarf::DW_ATE_signed_char:
    case llvm::dwarf::DW_ATE_unsigned:
    case llvm::dwarf::DW_ATE_unsigned_char:
    case llvm::dwarf::DW_ATE_signed_fixed:
    case llvm::dwarf::DW_ATE_unsigned_fixed:
    case llvm::dwarf::DW_ATE_UTF:
    case llvm::dwarf::DW_ATE_UCS:
    case llvm::dwarf::DW_ATE_ASCII:
      Layout.add(Base, *Bytes, /*Floating=*/false);
      return ValueShape{*Bytes, scalarAlignment(*Bytes)};
    default:
      // Decimal floating point and vendor encodings have no class here.
      break;
    }
    break;
  }
  case llvm::dwarf::DW_TAG_enumeration_type:
    if (!Bytes)
      break;
    Layout.add(Base, *Bytes, /*Floating=*/false);
    return ValueShape{*Bytes, scalarAlignment(*Bytes)};
  case llvm::dwarf::DW_TAG_pointer_type:
  case llvm::dwarf::DW_TAG_reference_type:
  case llvm::dwarf::DW_TAG_rvalue_reference_type: {
    const uint64_t Size =
        Bytes.value_or(Type.getDwarfUnit()->getAddressByteSize());
    Layout.add(Base, Size, /*Floating=*/false);
    return ValueShape{Size, scalarAlignment(Size)};
  }
  case llvm::dwarf::DW_TAG_structure_type:
  case llvm::dwarf::DW_TAG_class_type:
  case llvm::dwarf::DW_TAG_union_type:
    return layOutRecord(Type, Base, Depth, Layout);
  case llvm::dwarf::DW_TAG_array_type: {
    if (constantAttribute(Type, llvm::dwarf::DW_AT_GNU_vector).value_or(0))
      break; // Vector lanes are not independent scalar ABI members.
    const std::optional<uint64_t> Count = arrayElements(Type);
    ValueLayout Element;
    const std::optional<ValueShape> Shape =
        layOut(Type.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type),
               0, Depth + 1, Element);
    if (!Count || !Shape || !Shape->Bytes ||
        *Count > UINT64_MAX / Shape->Bytes ||
        (Bytes && *Bytes != *Count * Shape->Bytes))
      break;
    if (!Element.LeavesKnown ||
        *Count * Element.Leaves.size() >
            ::neverd::limits::debug::kDWARFLoaderMaxLayoutLeaves)
      Layout.LeavesKnown = false;
    for (uint64_t I = 0; Layout.LeavesKnown && I < *Count; ++I)
      for (const NdScalarLeaf &Leaf : Element.Leaves)
        Layout.add(Base + I * Shape->Bytes + Leaf.Offset, Leaf.Size,
                   Leaf.Floating);
    return ValueShape{*Count * Shape->Bytes, Shape->Align};
  }
  default:
    break;
  }
  // A value whose layout is not described here keeps its size alone.
  Layout.LeavesKnown = false;
  if (Bytes)
    return ValueShape{*Bytes, 0};
  return std::nullopt;
}

/// The DIE \p Type names through its typedefs and qualifiers.
llvm::DWARFDie unqualifiedType(llvm::DWARFDie Type) {
  for (int Depth = 0;
       Type.isValid() &&
       Depth <= ::neverd::limits::debug::kDWARFLoaderMaxLayoutDepth;
       ++Depth) {
    switch (Type.getTag()) {
    case llvm::dwarf::DW_TAG_typedef:
    case llvm::dwarf::DW_TAG_const_type:
    case llvm::dwarf::DW_TAG_volatile_type:
    case llvm::dwarf::DW_TAG_restrict_type:
      Type = Type.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
      continue;
    default:
      return Type;
    }
  }
  return {};
}

/// How a call passes the record \p Type describes, which is a record or a
/// complex number: C passes its records as their bytes; C++ says so for
/// each class (DW_AT_calling_convention), by reference for one with a
/// non-trivial copy constructor or destructor.  Rust's own ABI passes some
/// records as separate scalars where `extern "C"` passes their bytes, and
/// the debug information does not say which a function uses.
NdRecordPassing recordPassing(llvm::DWARFDie Type) {
  const std::optional<uint64_t> Language = Type.getLanguage();
  if (!Language)
    return NdRecordPassing::Unknown;
  const auto Source = static_cast<llvm::dwarf::SourceLanguage>(*Language);
  const bool CFamily = llvm::dwarf::isC(Source) ||
                       llvm::dwarf::isCPlusPlus(Source) ||
                       Source == llvm::dwarf::DW_LANG_ObjC_plus_plus;
  if (!CFamily)
    return NdRecordPassing::Unknown;
  if (Type.getTag() == llvm::dwarf::DW_TAG_base_type)
    return constantAttribute(Type, llvm::dwarf::DW_AT_encoding) ==
                   llvm::dwarf::DW_ATE_complex_float
               ? NdRecordPassing::Complex
               : NdRecordPassing::Unknown;
  if (llvm::dwarf::isC(Source))
    return NdRecordPassing::ByValue;
  const std::optional<uint64_t> Convention =
      constantAttribute(Type, llvm::dwarf::DW_AT_calling_convention);
  if (Convention == llvm::dwarf::DW_CC_pass_by_value)
    return NdRecordPassing::ByValue;
  if (Convention == llvm::dwarf::DW_CC_pass_by_reference)
    return NdRecordPassing::ByReference;
  return NdRecordPassing::Unknown;
}

/// Describes how a call passes the record argument or result \p Type, whose
/// type \p Die names: its scalars, alignment and passing.
void describeRecordPassing(llvm::DWARFDie Die, NdType &Type) {
  if (Type.Kind != NdTypeKind::Struct || Type.IsEnum)
    return;
  const llvm::DWARFDie Record = unqualifiedType(Die);
  if (!Record.isValid())
    return;
  ValueLayout Layout;
  const std::optional<ValueShape> Shape = layOut(Record, 0, 0, Layout);
  if (!Shape || Shape->Bytes != Type.Size)
    return;
  if (Layout.LeavesKnown && !Layout.Leaves.empty()) {
    std::stable_sort(Layout.Leaves.begin(), Layout.Leaves.end(),
                     [](const NdScalarLeaf &L, const NdScalarLeaf &R) {
                       return L.Offset < R.Offset;
                     });
    Type.ScalarLeaves = std::move(Layout.Leaves);
  }
  if (Shape->Align <= UINT16_MAX)
    Type.Alignment = static_cast<uint16_t>(Shape->Align);
  Type.Passing = recordPassing(Record);
}

/// Whether code in the language of \p Die's unit follows the platform's C
/// calling convention: C, C++ and Objective-C do, and Rust does for the
/// scalars it passes.
bool platformConvention(llvm::DWARFDie Die) {
  const std::optional<uint64_t> Language = Die.getLanguage();
  if (!Language)
    return false;
  const auto Source = static_cast<llvm::dwarf::SourceLanguage>(*Language);
  return llvm::dwarf::isC(Source) || llvm::dwarf::isCPlusPlus(Source) ||
         Source == llvm::dwarf::DW_LANG_ObjC_plus_plus ||
         Source == llvm::dwarf::DW_LANG_Rust;
}
} // namespace

TypeRef DWARFDebugContext::Impl::convertDwarfType(llvm::DWARFDie Die, int Depth,
                                                  bool CompleteValue) const {
  if (!Die.isValid() ||
      Depth > ::neverd::limits::debug::
                  kDWARFLoaderDWARFDebugContextImplConvertDwarfTypeDepthLimit)
    return CompleteValue ? nullptr : NdType::makeInt(4);

  if (CompleteValue && !completeValueAttributes(Die))
    return nullptr;

  auto Tag = Die.getTag();
  switch (Tag) {
  case llvm::dwarf::DW_TAG_base_type: {
    auto SizeAttr = Die.find(llvm::dwarf::DW_AT_byte_size);
    auto Encoding = Die.find(llvm::dwarf::DW_AT_encoding);
    if (CompleteValue) {
      const auto Bytes =
          SizeAttr ? SizeAttr->getAsUnsignedConstant() : std::nullopt;
      const auto Enc =
          Encoding ? Encoding->getAsUnsignedConstant() : std::nullopt;
      if (!Bytes || !Enc ||
          (*Bytes != 1 && *Bytes != 2 && *Bytes != 4 && *Bytes != 8) ||
          (*Enc != llvm::dwarf::DW_ATE_boolean &&
           *Enc != llvm::dwarf::DW_ATE_signed &&
           *Enc != llvm::dwarf::DW_ATE_signed_char &&
           *Enc != llvm::dwarf::DW_ATE_unsigned &&
           *Enc != llvm::dwarf::DW_ATE_unsigned_char &&
           *Enc != llvm::dwarf::DW_ATE_float) ||
          (*Enc == llvm::dwarf::DW_ATE_float && *Bytes < 4))
        return nullptr;
    }
    uint16_t Sz = SizeAttr ? static_cast<uint16_t>(
                                 SizeAttr->getAsUnsignedConstant().value_or(4))
                           : 4;
    if (Encoding) {
      auto Enc = Encoding->getAsUnsignedConstant().value_or(0);
      if (Enc == llvm::dwarf::DW_ATE_float)
        return NdType::makeFloat(Sz);
      if (Enc == llvm::dwarf::DW_ATE_complex_float && (Sz == 8 || Sz == 16) &&
          recordPassing(Die) == NdRecordPassing::Complex &&
          !llvm::StringRef(Die.getShortName() ? Die.getShortName() : "")
               .contains("long")) {
        auto T = NdType::makeComplex(Sz / 2);
        describeRecordPassing(Die, *T);
        return T;
      }
      // A complex number, decimal floating point and vendor encodings are
      // neither integers nor binary floating point: a record of their size,
      // which C cannot spell from it and conventions place by its parts.
      if (Enc == llvm::dwarf::DW_ATE_complex_float ||
          Enc == llvm::dwarf::DW_ATE_imaginary_float ||
          Enc == llvm::dwarf::DW_ATE_decimal_float ||
          Enc >= llvm::dwarf::DW_ATE_lo_user) {
        auto T = std::make_shared<NdType>();
        T->Kind = NdTypeKind::Struct;
        T->Size = Sz;
        return T;
      }
      bool IsSigned = (Enc == llvm::dwarf::DW_ATE_signed ||
                       Enc == llvm::dwarf::DW_ATE_signed_char);
      return NdType::makeInt(Sz, IsSigned);
    }
    return NdType::makeInt(Sz);
  }
  case llvm::dwarf::DW_TAG_pointer_type:
  case llvm::dwarf::DW_TAG_reference_type:
  case llvm::dwarf::DW_TAG_rvalue_reference_type: {
    auto PointeeDie =
        Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    auto PT = PointeeDie.isValid() ? convertDwarfType(PointeeDie, Depth + 1)
                                   : NdType::makeVoid();
    // A pointer is as wide as its unit's addresses unless it says otherwise:
    // four bytes on i386 and 32-bit ARM, where a parameter after it is the
    // next slot's.
    TypeRef Pointer = NdType::makePtr(PT);
    const auto Bytes = Die.find(llvm::dwarf::DW_AT_byte_size);
    const uint64_t Size =
        Bytes ? Bytes->getAsUnsignedConstant().value_or(0)
              : (Die.getDwarfUnit() ? Die.getDwarfUnit()->getAddressByteSize()
                                    : 0);
    if (CompleteValue) {
      if (Size != 4 && Size != 8)
        return nullptr;
      if (const auto AddressClass = Die.find(llvm::dwarf::DW_AT_address_class);
          AddressClass && AddressClass->getAsUnsignedConstant() != 0)
        return nullptr;
    }
    if (Size == 4 || Size == 8)
      Pointer->Size = static_cast<uint16_t>(Size);
    return Pointer;
  }
  case llvm::dwarf::DW_TAG_typedef:
  case llvm::dwarf::DW_TAG_const_type:
  case llvm::dwarf::DW_TAG_volatile_type:
  case llvm::dwarf::DW_TAG_restrict_type: {
    auto Ref = Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    return Ref.isValid()   ? convertDwarfType(Ref, Depth + 1, CompleteValue)
           : CompleteValue ? nullptr
                           : NdType::makeVoid();
  }
  case llvm::dwarf::DW_TAG_array_type: {
    const bool Vector =
        constantAttribute(Die, llvm::dwarf::DW_AT_GNU_vector).value_or(0);
    auto ElemDie =
        Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    auto Elem = ElemDie.isValid()
                    ? convertDwarfType(ElemDie, Depth + 1, CompleteValue)
                : CompleteValue ? nullptr
                                : NdType::makeInt(1, false);
    if (!Elem)
      return nullptr;
    if (CompleteValue) {
      bool HasDimension = false;
      for (const auto Child : Die.children())
        if (!completeValueAttributes(Child))
          return nullptr;
        else
          HasDimension |= Child.getTag() == llvm::dwarf::DW_TAG_subrange_type;
      const auto Count = arrayElements(Die);
      if (Vector || !HasDimension || !Count || !*Count || !Elem->Size ||
          *Count > UINT16_MAX / Elem->Size)
        return nullptr;
      const auto Bytes = static_cast<uint16_t>(*Count * Elem->Size);
      if (auto Stated = Die.find(llvm::dwarf::DW_AT_byte_size);
          Stated && Stated->getAsUnsignedConstant() != Bytes)
        return nullptr;
      return NdType::makeArray(Elem, static_cast<uint32_t>(*Count), Bytes);
    }
    uint32_t Count = 0;
    for (auto Child : Die.children()) {
      if (Child.getTag() == llvm::dwarf::DW_TAG_subrange_type) {
        auto UB = Child.find(llvm::dwarf::DW_AT_upper_bound);
        auto Cnt = Child.find(llvm::dwarf::DW_AT_count);
        if (Cnt)
          Count =
              static_cast<uint32_t>(Cnt->getAsUnsignedConstant().value_or(0));
        else if (UB)
          Count =
              static_cast<uint32_t>(UB->getAsUnsignedConstant().value_or(0)) +
              1;
      }
    }
    if (Vector) {
      const auto Lane = unqualifiedType(ElemDie);
      if (!Lane.isValid())
        return nullptr;
      const auto Encoding =
          constantAttribute(Lane, llvm::dwarf::DW_AT_encoding);
      if (Lane.getTag() != llvm::dwarf::DW_TAG_base_type || !Encoding ||
          (*Encoding != llvm::dwarf::DW_ATE_signed &&
           *Encoding != llvm::dwarf::DW_ATE_unsigned &&
           *Encoding != llvm::dwarf::DW_ATE_signed_char &&
           *Encoding != llvm::dwarf::DW_ATE_unsigned_char &&
           *Encoding != llvm::dwarf::DW_ATE_float))
        return nullptr;
      const auto Elements = arrayElements(Die);
      if (!Elements || !*Elements || *Elements > UINT32_MAX ||
          !ElemDie.isValid() ||
          (Elem->Kind != NdTypeKind::Int && Elem->Kind != NdTypeKind::Float) ||
          !llvm::isPowerOf2_64(*Elements) || !Elem->Size ||
          *Elements > UINT16_MAX / Elem->Size)
        return nullptr;
      Count = static_cast<uint32_t>(*Elements);
      if (auto Bytes = constantAttribute(Die, llvm::dwarf::DW_AT_byte_size);
          Bytes && *Bytes != uint64_t(Count) * Elem->Size)
        return nullptr;
    }
    const auto Bytes = static_cast<uint16_t>(Count * Elem->Size);
    return Vector ? NdType::makeVector(Elem, Count, Bytes)
                  : NdType::makeArray(Elem, Count, Bytes);
  }
  case llvm::dwarf::DW_TAG_structure_type:
  case llvm::dwarf::DW_TAG_class_type:
  case llvm::dwarf::DW_TAG_union_type: {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Struct;
    if (Die.getName(llvm::DINameKind::ShortName)) {
      llvm::raw_string_ostream Name(T->SourceName);
      llvm::dumpTypeQualifiedName(Die, Name);
    }
    auto Sz = Die.find(llvm::dwarf::DW_AT_byte_size);
    T->Size =
        Sz ? static_cast<uint16_t>(Sz->getAsUnsignedConstant().value_or(0)) : 0;
    if (CompleteValue) {
      if (Tag == llvm::dwarf::DW_TAG_union_type ||
          Die.find(llvm::dwarf::DW_AT_declaration) || !Sz ||
          Sz->getAsUnsignedConstant() != T->Size ||
          recordPassing(Die) != NdRecordPassing::ByValue)
        return nullptr;
      std::vector<TypeRef> Fields;
      std::vector<uint16_t> Offsets;
      for (const auto Child : Die.children()) {
        if (Child.getTag() == llvm::dwarf::DW_TAG_variant_part ||
            Child.getTag() == llvm::dwarf::DW_TAG_inheritance)
          return nullptr;
        if (Child.getTag() != llvm::dwarf::DW_TAG_member ||
            Child.find(llvm::dwarf::DW_AT_external) ||
            Child.find(llvm::dwarf::DW_AT_declaration))
          continue;
        if (!completeValueAttributes(Child) ||
            Child.find(llvm::dwarf::DW_AT_bit_size) ||
            Child.find(llvm::dwarf::DW_AT_bit_offset) ||
            Child.find(llvm::dwarf::DW_AT_data_bit_offset) ||
            Fields.size() >=
                ::neverd::limits::debug::kDWARFLoaderMaxLayoutElements)
          return nullptr;
        const auto Location =
            Child.find(llvm::dwarf::DW_AT_data_member_location);
        const auto Offset = Location ? memberLocation(*Location) : std::nullopt;
        if (!Offset || *Offset > T->Size)
          return nullptr;
        Fields.push_back(
            convertDwarfType(declaredTypeDie(Child), Depth + 1, true));
        Offsets.push_back(static_cast<uint16_t>(*Offset));
      }
      const unsigned PointerBytes = Die.getDwarfUnit()->getAddressByteSize();
      const auto Natural = NdType::makeStruct(std::move(Fields), PointerBytes);
      if (!Natural || Natural->Size != T->Size ||
          Natural->FieldOffsets != Offsets)
        return nullptr;
      if (const auto Align = Die.find(llvm::dwarf::DW_AT_alignment);
          Align && Align->getAsUnsignedConstant() != Natural->Alignment)
        return nullptr;
      T->Fields = std::move(Natural->Fields);
      T->FieldOffsets = std::move(Natural->FieldOffsets);
      T->Alignment = Natural->Alignment;
      T->Passing = NdRecordPassing::ByValue;
    }
    return T;
  }
  case llvm::dwarf::DW_TAG_subroutine_type: {
    const auto Return = resolveDeclaredType(Die);
    auto T = NdType::makeFunc(
        Return.TheStatus == DeclaredTypeResolution::Status::Found
            ? convertDwarfType(Return.TypeDie, Depth + 1)
        : Return.TheStatus == DeclaredTypeResolution::Status::Absent
            ? NdType::makeVoid()
            : nullptr);
    if (auto Convention = Die.find(llvm::dwarf::DW_AT_calling_convention))
      T->FunctionConvention = types::dwarfCallingConvention(
          Convention->getAsUnsignedConstant().value_or(0));
    for (auto Child : Die.children()) {
      if (Child.getTag() == llvm::dwarf::DW_TAG_unspecified_parameters) {
        T->FunctionVariadic = true;
        continue;
      }
      if (Child.getTag() == llvm::dwarf::DW_TAG_formal_parameter) {
        auto ParamDie = declaredTypeDie(Child);
        T->ParamTypes.push_back(ParamDie.isValid()
                                    ? convertDwarfType(ParamDie, Depth + 1)
                                    : nullptr);
      }
    }
    return T;
  }
  case llvm::dwarf::DW_TAG_enumeration_type: {
    if (CompleteValue)
      return nullptr;
    auto Sz = Die.find(llvm::dwarf::DW_AT_byte_size);
    uint16_t S =
        Sz ? static_cast<uint16_t>(Sz->getAsUnsignedConstant().value_or(4)) : 4;
    return NdType::makeInt(S);
  }
  default:
    return CompleteValue ? nullptr : NdType::makeInt(4);
  }
}

/// The declaration owns its mutable types, including nested member, pointer
/// and callback types. DWARF conversion bounds the graph before it gets here.
static TypeRef copySignatureType(const TypeRef &Type) {
  if (!Type)
    return nullptr;
  auto Copy = std::make_shared<NdType>(*Type);
  Copy->Pointee = copySignatureType(Type->Pointee);
  Copy->ElemType = copySignatureType(Type->ElemType);
  Copy->RetType = copySignatureType(Type->RetType);
  for (auto &Field : Copy->Fields)
    Field = copySignatureType(Field);
  for (auto &Param : Copy->ParamTypes)
    Param = copySignatureType(Param);
  return Copy;
}

TypeRef
DWARFDebugContext::Impl::signatureType(llvm::DWARFDie Die,
                                       RecordSignatureTypes &Records) const {
  const auto Record = unqualifiedType(Die);
  if (Record.isValid() &&
      (Record.getTag() == llvm::dwarf::DW_TAG_structure_type ||
       Record.getTag() == llvm::dwarf::DW_TAG_class_type)) {
    // The declared DIE includes the qualifier chain that complete conversion
    // validates. A referenced type may belong to a different unit.
    const auto Key = std::make_pair(Die.getDwarfUnit(), Die.getOffset());
    if (const auto It = Records.find(Key); It != Records.end())
      return copySignatureType(It->second);
    if (auto Complete = convertDwarfType(Die, 0, true)) {
      describeRecordPassing(Die, *Complete);
      Records.try_emplace(Key, Complete);
      return Complete;
    }
  }
  // A display type may retain a name and size without a complete value
  // graph. Source ABI owners must still reject that incomplete evidence.
  auto Type = convertDwarfType(Die);
  if (Type)
    describeRecordPassing(Die, *Type);
  return Type;
}

DWARFDebugContext::Impl::DeclaredTypeResolution
DWARFDebugContext::Impl::resolveDeclaredType(llvm::DWARFDie Die) const {
  using Resolution = DeclaredTypeResolution;
  std::vector<llvm::DWARFDie> Active;
  std::function<Resolution(llvm::DWARFDie, unsigned)> Resolve =
      [&](llvm::DWARFDie Current, unsigned Depth) -> Resolution {
    if (!Current.isValid() ||
        Depth >
            ::neverd::limits::debug::
                kDWARFLoaderDWARFDebugContextImplResolveDeclaredTypeResolveDepthLimit)
      return {Resolution::Status::Malformed, {}};
    if (std::find(Active.begin(), Active.end(), Current) != Active.end())
      return {Resolution::Status::Malformed, {}};
    Active.push_back(Current);

    Resolution Result;
    dwarf_loader_detail::ReturnTypeProvenanceStatus Merged =
        dwarf_loader_detail::ReturnTypeProvenanceStatus::Unspecified;
    const auto merge = [&](Resolution Candidate, bool FromProvenance) {
      using Provenance = dwarf_loader_detail::ReturnTypeProvenanceStatus;
      Provenance CandidateStatus = Provenance::Unspecified;
      if (Candidate.TheStatus == Resolution::Status::Malformed)
        CandidateStatus = Provenance::Malformed;
      else if (Candidate.TheStatus == Resolution::Status::Found)
        CandidateStatus = Provenance::Value;
      else if (FromProvenance)
        CandidateStatus = Provenance::NoValue;
      const bool SameType = Result.TheStatus != Resolution::Status::Found ||
                            Candidate.TheStatus != Resolution::Status::Found ||
                            Result.TypeDie == Candidate.TypeDie;
      Merged = dwarf_loader_detail::mergeReturnTypeProvenance(
          Merged, CandidateStatus, SameType);
      if (Merged == Provenance::Malformed) {
        Result = {Resolution::Status::Malformed, {}};
        return;
      }
      if (Candidate.TheStatus == Resolution::Status::Found &&
          Result.TheStatus == Resolution::Status::Absent)
        Result = std::move(Candidate);
    };

    const StrictAttributeLookup Type =
        strictAttribute(Current, llvm::dwarf::DW_AT_type);
    if (Type.TheStatus == StrictAttributeLookup::Status::Malformed) {
      Active.pop_back();
      return {Resolution::Status::Malformed, {}};
    }
    if (Type.TheStatus == StrictAttributeLookup::Status::Found) {
      const llvm::DWARFDie Referenced =
          Current.getAttributeValueAsReferencedDie(Type.Value);
      merge(Referenced.isValid()
                ? Resolution{Resolution::Status::Found, Referenced}
                : Resolution{Resolution::Status::Malformed, {}},
            false);
    }
    for (const llvm::dwarf::Attribute Provenance :
         {llvm::dwarf::DW_AT_specification,
          llvm::dwarf::DW_AT_abstract_origin}) {
      const StrictAttributeLookup Attribute =
          strictAttribute(Current, Provenance);
      if (Attribute.TheStatus == StrictAttributeLookup::Status::Malformed) {
        Active.pop_back();
        return {Resolution::Status::Malformed, {}};
      }
      if (Attribute.TheStatus == StrictAttributeLookup::Status::Absent)
        continue;
      const llvm::DWARFDie Referenced =
          Current.getAttributeValueAsReferencedDie(Attribute.Value);
      merge(Referenced.isValid()
                ? Resolve(Referenced, Depth + 1)
                : Resolution{Resolution::Status::Malformed, {}},
            true);
    }

    Active.pop_back();
    return Result;
  };
  return Resolve(Die, 0);
}

llvm::DWARFDie
DWARFDebugContext::Impl::declaredTypeDie(llvm::DWARFDie Die) const {
  // A DIE's own type stands.  The out-of-line copy of an inlined function
  // names its parameters only by DW_AT_abstract_origin, and a static data
  // member's definition its declaration only by DW_AT_specification.
  if (const llvm::DWARFDie Own =
          Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type))
    return Own;
  const DeclaredTypeResolution Declared = resolveDeclaredType(Die);
  return Declared.TheStatus == DeclaredTypeResolution::Status::Found
             ? Declared.TypeDie
             : llvm::DWARFDie();
}

AuthenticatedReturnValueState
DWARFDebugContext::Impl::classifyReturnValue(llvm::DWARFDie Die,
                                             int Depth) const {
  using Kind = AuthenticatedReturnKind;
  if (!Die.isValid() ||
      Depth >
          ::neverd::limits::debug::
              kDWARFLoaderDWARFDebugContextImplClassifyReturnValueDepthLimit)
    return {};

  const auto byteSize = [&](bool PointerDefault) -> std::optional<uint16_t> {
    const StrictAttributeLookup Attr =
        strictAttribute(Die, llvm::dwarf::DW_AT_byte_size);
    if (Attr.TheStatus == StrictAttributeLookup::Status::Malformed)
      return std::nullopt;
    if (Attr.TheStatus == StrictAttributeLookup::Status::Absent) {
      if (!PointerDefault || !Die.getDwarfUnit())
        return std::nullopt;
      const uint8_t AddressSize = Die.getDwarfUnit()->getAddressByteSize();
      return AddressSize == 0 ? std::nullopt
                              : std::optional<uint16_t>(AddressSize);
    }
    const std::optional<uint64_t> Size = Attr.Value.getAsUnsignedConstant();
    if (!Size || *Size == 0 || *Size > std::numeric_limits<uint16_t>::max())
      return std::nullopt;
    return static_cast<uint16_t>(*Size);
  };

  switch (Die.getTag()) {
  case llvm::dwarf::DW_TAG_base_type: {
    const StrictAttributeLookup Encoding =
        strictAttribute(Die, llvm::dwarf::DW_AT_encoding);
    if (Encoding.TheStatus != StrictAttributeLookup::Status::Found)
      return {};
    const std::optional<uint64_t> Enc = Encoding.Value.getAsUnsignedConstant();
    if (!Enc)
      return {};
    const std::optional<uint16_t> Size = byteSize(false);
    if (!Size)
      return {};
    if (*Enc == llvm::dwarf::DW_ATE_float)
      return {Kind::FloatingPoint, *Size};
    switch (*Enc) {
    case llvm::dwarf::DW_ATE_address:
    case llvm::dwarf::DW_ATE_boolean:
    case llvm::dwarf::DW_ATE_signed:
    case llvm::dwarf::DW_ATE_signed_char:
    case llvm::dwarf::DW_ATE_unsigned:
    case llvm::dwarf::DW_ATE_unsigned_char:
      return {Kind::Integer, *Size};
    default:
      return {};
    }
  }
  case llvm::dwarf::DW_TAG_pointer_type: {
    const std::optional<uint16_t> Size = byteSize(true);
    return Size ? AuthenticatedReturnValueState{Kind::Pointer, *Size}
                : AuthenticatedReturnValueState{};
  }
  case llvm::dwarf::DW_TAG_typedef:
  case llvm::dwarf::DW_TAG_const_type:
  case llvm::dwarf::DW_TAG_volatile_type:
  case llvm::dwarf::DW_TAG_restrict_type: {
    const StrictAttributeLookup Type =
        strictAttribute(Die, llvm::dwarf::DW_AT_type);
    if (Type.TheStatus != StrictAttributeLookup::Status::Found)
      return {};
    const llvm::DWARFDie Referenced =
        Die.getAttributeValueAsReferencedDie(Type.Value);
    return Referenced.isValid() ? classifyReturnValue(Referenced, Depth + 1)
                                : AuthenticatedReturnValueState{};
  }
  case llvm::dwarf::DW_TAG_array_type:
  case llvm::dwarf::DW_TAG_structure_type:
  case llvm::dwarf::DW_TAG_class_type:
  case llvm::dwarf::DW_TAG_union_type: {
    const std::optional<uint16_t> Size = byteSize(false);
    return Size ? AuthenticatedReturnValueState{Kind::Aggregate, *Size}
                : AuthenticatedReturnValueState{};
  }
  case llvm::dwarf::DW_TAG_enumeration_type: {
    const std::optional<uint16_t> Size = byteSize(false);
    return Size ? AuthenticatedReturnValueState{Kind::Integer, *Size}
                : AuthenticatedReturnValueState{};
  }
  default:
    // References, unspecified types, subroutines, and extensions require ABI
    // handling that this reader does not yet validate.  Keep them Unknown.
    return {};
  }
}

std::vector<DWARFDebugContext::Impl::Location>
DWARFDebugContext::Impl::decodeLocations(llvm::DWARFDie Die,
                                         llvm::dwarf::Attribute Attr,
                                         llvm::DWARFUnit *Unit,
                                         bool IsFrameBase) const {
  std::vector<Location> Result;
  llvm::Expected<llvm::DWARFLocationExpressionsVector> Locations =
      Die.getLocations(Attr);
  if (!Locations) {
    llvm::consumeError(Locations.takeError());
    Result.push_back(Location{std::nullopt, StackCoordinate::Malformed, 0});
    return Result;
  }

  for (const llvm::DWARFLocationExpression &Entry : *Locations) {
    Location Decoded;
    if (Entry.Range) {
      if (!Entry.Range->valid() || Entry.Range->LowPC >= Entry.Range->HighPC) {
        Result.push_back(Location{std::nullopt, StackCoordinate::Malformed, 0});
        continue;
      }
      Decoded.Range = AddressRange{Entry.Range->LowPC, Entry.Range->HighPC};
    }

    llvm::StringRef Bytes(reinterpret_cast<const char *>(Entry.Expr.data()),
                          Entry.Expr.size());
    llvm::DataExtractor Extractor(Bytes, Unit->isLittleEndian(),
                                  Unit->getAddressByteSize());
    llvm::DWARFExpression Expression(Extractor, Unit->getAddressByteSize(),
                                     Unit->getFormat());
    auto It = Expression.begin();
    if (It == Expression.end() || It->isError()) {
      Decoded.Coordinate = StackCoordinate::Malformed;
      Result.push_back(Decoded);
      continue;
    }

    const uint8_t Code = It->getCode();
    const std::vector<uint64_t> Operands(It->getRawOperands().begin(),
                                         It->getRawOperands().end());
    ++It;
    const bool Complete = It == Expression.end();
    const dwarf_loader_detail::DecodedLocationExpression Shape =
        dwarf_loader_detail::decodeLocationExpressionShape(
            Code, Operands, Complete, IsFrameBase, DwarfStackPointerReg,
            DwarfFramePointerReg);
    Decoded.Coordinate = Shape.Base;
    Decoded.Offset = Shape.Offset;
    Result.push_back(Decoded);
  }
  return Result;
}

namespace {

std::optional<uint64_t> declaredTypeSize(const TypeRef &Type,
                                         std::set<const NdType *> &Active) {
  if (!Type)
    return std::nullopt;
  if (Type->Kind != NdTypeKind::Array)
    return Type->Size == 0 ? std::nullopt : std::optional<uint64_t>(Type->Size);
  if (!Type->ElemType || !Active.insert(Type.get()).second)
    return std::nullopt;
  const std::optional<uint64_t> Element =
      declaredTypeSize(Type->ElemType, Active);
  Active.erase(Type.get());
  if (!Element || Type->ArrayCount == 0 ||
      *Element > std::numeric_limits<uint64_t>::max() / Type->ArrayCount)
    return std::nullopt;
  return *Element * Type->ArrayCount;
}

std::optional<uint64_t> declaredTypeSize(const TypeRef &Type) {
  std::set<const NdType *> Active;
  return declaredTypeSize(Type, Active);
}

bool containsPC(va_t PC, llvm::ArrayRef<std::pair<va_t, va_t>> AddressRanges) {
  return std::any_of(AddressRanges.begin(), AddressRanges.end(),
                     [&](const auto &Range) {
                       return PC >= Range.first && PC < Range.second;
                     });
}

std::optional<uint64_t> dwarfIntegerRegister(llvm::Triple::ArchType Arch,
                                             uint64_t Number) {
  if (Arch == llvm::Triple::aarch64 && Number <= 30)
    return a64reg::X0 + Number * (a64reg::X1 - a64reg::X0);
  if ((Arch == llvm::Triple::arm || Arch == llvm::Triple::thumb) && Number < 16)
    return armreg::R0 + Number * (armreg::R1 - armreg::R0);
  if (Arch == llvm::Triple::x86_64) {
    constexpr uint64_t Registers[] = {
        x86reg::RAX, x86reg::RDX, x86reg::RCX, x86reg::RBX,
        x86reg::RSI, x86reg::RDI, x86reg::RBP, x86reg::RSP,
        x86reg::R8,  x86reg::R9,  x86reg::R10, x86reg::R11,
        x86reg::R12, x86reg::R13, x86reg::R14, x86reg::R15};
    if (Number < std::size(Registers))
      return Registers[Number];
  }
  if (Arch == llvm::Triple::x86 && Number < 8)
    return x86reg::generalReg(Number);
  return std::nullopt;
}

} // namespace

void DWARFDebugContext::Impl::parseLocalVariable(
    llvm::DWARFDie Die, bool IsParam, llvm::ArrayRef<AddressRange> ScopeRanges,
    std::vector<LocalObject> &Objects) const {
  auto TypeDie = declaredTypeDie(Die);
  TypeRef Type =
      TypeDie.isValid() ? convertDwarfType(TypeDie) : NdType::makeInt(4);
  if (!declaredTypeSize(Type))
    return;

  LocalObject Object;
  if (const char *Name = Die.getName(llvm::DINameKind::ShortName))
    Object.Variable.Name = Name;
  Object.Variable.Type = std::move(Type);
  Object.Variable.IsParam = IsParam;
  Object.ScopeRanges.assign(ScopeRanges.begin(), ScopeRanges.end());
  Object.Locations = decodeLocations(Die, llvm::dwarf::DW_AT_location,
                                     Die.getDwarfUnit(), false);
  if (!Object.Locations.empty())
    Objects.push_back(std::move(Object));
}

void DWARFDebugContext::Impl::parseGlobalVariable(llvm::DWARFDie Die,
                                                  llvm::DWARFUnit *Unit) {
  auto TypeDie = declaredTypeDie(Die);
  TypeRef Type =
      TypeDie.isValid() ? convertDwarfType(TypeDie) : NdType::makeInt(4);
  const std::optional<uint64_t> Size = declaredTypeSize(Type);
  if (!Size)
    return;

  llvm::Expected<llvm::DWARFLocationExpressionsVector> Locations =
      Die.getLocations(llvm::dwarf::DW_AT_location);
  if (!Locations) {
    llvm::consumeError(Locations.takeError());
    return;
  }
  if (Locations->size() != 1 || Locations->front().Range)
    return;

  const llvm::DWARFLocationExpression &Location = Locations->front();
  llvm::StringRef Bytes(reinterpret_cast<const char *>(Location.Expr.data()),
                        Location.Expr.size());
  llvm::DataExtractor Extractor(Bytes, Unit->isLittleEndian(),
                                Unit->getAddressByteSize());
  llvm::DWARFExpression Expression(Extractor, Unit->getAddressByteSize(),
                                   Unit->getFormat());
  auto It = Expression.begin();
  if (It == Expression.end() || It->isError())
    return;
  const uint8_t Code = It->getCode();
  const std::vector<uint64_t> Operands(It->getRawOperands().begin(),
                                       It->getRawOperands().end());
  ++It;
  if (It != Expression.end() || Operands.size() != 1)
    return;

  std::optional<va_t> Address;
  if (Code == llvm::dwarf::DW_OP_addr) {
    Address = Operands[0];
  } else if (Code == llvm::dwarf::DW_OP_addrx &&
             Operands[0] <= std::numeric_limits<uint32_t>::max()) {
    if (auto Resolved =
            Unit->getAddrOffsetSectionItem(static_cast<uint32_t>(Operands[0])))
      Address = Resolved->Address;
  }
  if (!Address)
    return;

  DataObjectSym Object;
  if (const char *Name = Die.getName(llvm::DINameKind::ShortName))
    Object.Name = Name;
  Object.Addr = *Address;
  Object.Size = *Size;
  Object.IsBuffer = Type->Kind == NdTypeKind::Array;
  if (TypeDie.isValid())
    Object.Type = std::move(Type);
  DataObjects.push_back(std::move(Object));
}

void DWARFDebugContext::Impl::parseFunctionLocals(
    llvm::DWARFDie Die, llvm::ArrayRef<AddressRange> ScopeRanges,
    std::vector<LocalObject> &Objects) const {
  std::function<void(llvm::DWARFDie, const std::vector<AddressRange> &)> Walk =
      [&](llvm::DWARFDie Scope, const std::vector<AddressRange> &Ranges) {
        for (llvm::DWARFDie Child : Scope.children()) {
          const auto Tag = Child.getTag();
          if (Tag == llvm::dwarf::DW_TAG_variable ||
              Tag == llvm::dwarf::DW_TAG_formal_parameter) {
            parseLocalVariable(Child,
                               Tag == llvm::dwarf::DW_TAG_formal_parameter,
                               Ranges, Objects);
            continue;
          }
          if (Tag != llvm::dwarf::DW_TAG_lexical_block &&
              Tag != llvm::dwarf::DW_TAG_inlined_subroutine)
            continue;

          std::vector<AddressRange> ChildRanges = Ranges;
          llvm::Expected<llvm::DWARFAddressRangesVector> RawChildRanges =
              Child.getAddressRanges();
          if (!RawChildRanges) {
            llvm::consumeError(RawChildRanges.takeError());
            LocalObject Poison;
            Poison.Variable.Type = NdType::makeInt(1, false);
            Poison.ScopeRanges = Ranges;
            Poison.Locations.push_back(
                Location{std::nullopt, StackCoordinate::Malformed, 0});
            Objects.push_back(std::move(Poison));
            continue;
          }
          if (!RawChildRanges->empty()) {
            ChildRanges.clear();
            for (const llvm::DWARFAddressRange &ChildRange : *RawChildRanges) {
              if (!ChildRange.valid() || ChildRange.LowPC >= ChildRange.HighPC)
                continue;
              for (const AddressRange &ParentRange : Ranges) {
                const va_t Low = std::max(ChildRange.LowPC, ParentRange.first);
                const va_t High =
                    std::min(ChildRange.HighPC, ParentRange.second);
                if (Low < High)
                  ChildRanges.emplace_back(Low, High);
              }
            }
          }
          if (!ChildRanges.empty())
            Walk(Child, ChildRanges);
        }
      };
  Walk(Die, std::vector<AddressRange>(ScopeRanges.begin(), ScopeRanges.end()));
}

void DWARFDebugContext::Impl::parseFunction(llvm::DWARFDie Die,
                                            llvm::DWARFUnit *Unit,
                                            unsigned UnitIndex,
                                            RecordSignatureTypes &Records) {
  llvm::Expected<llvm::DWARFAddressRangesVector> DwarfRanges =
      Die.getAddressRanges();
  if (!DwarfRanges) {
    llvm::consumeError(DwarfRanges.takeError());
    return;
  }
  std::vector<AddressRange> Ranges;
  for (const llvm::DWARFAddressRange &Range : *DwarfRanges)
    if (Range.valid() && Range.LowPC < Range.HighPC)
      Ranges.emplace_back(Range.LowPC, Range.HighPC);
  if (Ranges.empty())
    return;
  std::sort(Ranges.begin(), Ranges.end());
  const va_t Addr = Ranges.front().first;
  const va_t High =
      std::max_element(Ranges.begin(), Ranges.end(),
                       [](const AddressRange &A, const AddressRange &B) {
                         return A.second < B.second;
                       })
          ->second;

  FunctionSym Sym;
  Sym.Addr = Addr;
  Sym.Size = High - Addr;
  if (auto Name = Die.getName(llvm::DINameKind::ShortName))
    Sym.Name = Name;
  else if (auto Link = Die.getName(llvm::DINameKind::LinkageName))
    Sym.Name = Link;
  else
    return;

  if (auto DeclFile = Die.find(llvm::dwarf::DW_AT_decl_file)) {
    if (auto Line = Die.find(llvm::dwarf::DW_AT_decl_line)) {
      Sym.DeclLoc.Line =
          static_cast<uint32_t>(Line->getAsUnsignedConstant().value_or(0));
      if (auto *LT = Unit->getContext().getLineTableForUnit(Unit)) {
        auto FileIdx = DeclFile->getAsUnsignedConstant().value_or(0);
        std::string FileName;
        if (LT->getFileNameByIndex(
                FileIdx, Unit->getCompilationDir(),
                llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath,
                FileName))
          Sym.DeclLoc.File = FileName;
      }
    }
  }

  AuthenticatedReturnValueState ReturnState;
  const DeclaredTypeResolution ReturnType = resolveDeclaredType(Die);
  if (ReturnType.TheStatus == DeclaredTypeResolution::Status::Absent) {
    // DWARF encodes a source-level void return by omitting DW_AT_type.
    ReturnState = {AuthenticatedReturnKind::NoValue, 0};
    Sym.ReturnType = NdType::makeVoid();
  } else if (ReturnType.TheStatus == DeclaredTypeResolution::Status::Found) {
    ReturnState = classifyReturnValue(ReturnType.TypeDie);
    Sym.ReturnType = signatureType(ReturnType.TypeDie, Records);
    if (!Sym.ReturnType || Sym.ReturnType->Kind == NdTypeKind::Unknown)
      ReturnState = {};
  } else {
    // A present but malformed reference is not evidence of void.
    Sym.ReturnType = std::make_shared<NdType>();
  }
  std::vector<AuthenticatedRecordParameter> EntryRecords;
  for (llvm::DWARFDie Child : Die.children()) {
    if (Child.getTag() == llvm::dwarf::DW_TAG_unspecified_parameters) {
      Sym.IsVariadic = true;
      continue;
    }
    if (Child.getTag() != llvm::dwarf::DW_TAG_formal_parameter)
      continue;
    if (auto Record = recordParameter(Child, Addr))
      EntryRecords.push_back(std::move(*Record));
    std::string Name;
    if (const char *RawName = Child.getName(llvm::DINameKind::ShortName))
      Name = RawName;
    auto ParamDie = declaredTypeDie(Child);
    TypeRef Type =
        ParamDie.isValid() ? signatureType(ParamDie, Records) : nullptr;
    Sym.Params.emplace_back(std::move(Name), std::move(Type));
  }
  Sym.PlatformConvention = platformConvention(Die);
  Sym.ParamEntryLocations = parameterEntryLocations(Die, Addr);

  RecordParameters[Addr] = std::move(EntryRecords);
  FunctionInfo &Info = Functions[Addr];
  Info = FunctionInfo{};
  Info.Ranges = Ranges;
  Info.FrameBases =
      decodeLocations(Die, llvm::dwarf::DW_AT_frame_base, Unit, true);
  Info.LocalDieOffset = Die.getOffset();
  Info.LocalUnitIndex = UnitIndex;
  Info.HasLocalDie = true;

  SubprogramExtents.insert(Addr, Ranges);
  ReturnValueStates[Addr] = ReturnState;
  FuncMap[Addr] = Sym;
  FuncList.push_back(Sym);
}

void DWARFDebugContext::Impl::parseUnits(size_t Begin, size_t End) {
  size_t Index = 0;
  for (auto &CU : DwarfCtx->compile_units()) {
    if (Index >= End)
      break;
    if (Index++ < Begin)
      continue;
    auto Root = CU->getUnitDIE(false);
    if (!Root.isValid())
      continue;
    const unsigned UnitIndex = static_cast<unsigned>(Index - 1);
    RecordSignatureTypes Records;

    std::function<void(llvm::DWARFDie)> Walk = [&](llvm::DWARFDie Die) {
      if (!Die.isValid())
        return;
      if (Die.getTag() == llvm::dwarf::DW_TAG_subprogram) {
        parseFunction(Die, CU.get(), UnitIndex, Records);
        return;
      }
      if (Die.getTag() == llvm::dwarf::DW_TAG_variable)
        parseGlobalVariable(Die, CU.get());
      for (auto Child : Die.children())
        Walk(Child);
    };
    Walk(Root);
  }
}

void DWARFDebugContext::Impl::parseCompileUnits(
    const llvm::object::ObjectFile &InputObject) {
  const auto ObjectData = InputObject.getMemoryBufferRef();
  const size_t Units = DwarfCtx->getNumCompileUnits();
  // Independent contexts would each decompress the entire debug section.
  // Keep compressed inputs serial rather than multiplying their peak memory.
  const bool Compressed =
      llvm::any_of(InputObject.sections(), [](auto Section) {
        if (Section.isCompressed())
          return true;
        auto Name = Section.getName();
        if (!Name) {
          llvm::consumeError(Name.takeError());
          return true;
        }
        return Name->starts_with(".zdebug_");
      });
  // A DWARFContext lazily fills cross-unit, abbreviation and line caches.
  // Each worker owns its object/context and output; sharing just the mapped
  // bytes avoids racing those caches. Small images stay on the calling thread.
  const unsigned Workers =
      !Compressed && Units >= 8 &&
              ObjectData.getBufferSize() >=
                  ::neverd::limits::debug::
                      kDWARFLoaderDWARFDebugContextImplParseCompileUnitsWorkersObjectDataGetBufferSizeLimit
          ? static_cast<unsigned>(
                std::min<size_t>(workerThreadCount(), Units / 4))
          : 1;
  bool Parsed = false;
  if (Workers > 1) {
    std::vector<std::unique_ptr<Impl>> Parts(Workers);
    std::atomic<unsigned> Next{0};
    auto Parse = [&] {
      const unsigned I = Next.fetch_add(1, std::memory_order_relaxed);
      auto Object = llvm::object::ObjectFile::createObjectFile(ObjectData);
      if (!Object) {
        llvm::consumeError(Object.takeError());
        return;
      }
      auto Part = std::make_unique<Impl>();
      Part->DwarfCtx = llvm::DWARFContext::create(**Object);
      if (!Part->DwarfCtx || Part->DwarfCtx->getNumCompileUnits() != Units)
        return;
      Part->MachineArch = MachineArch;
      Part->DwarfStackPointerReg = DwarfStackPointerReg;
      Part->DwarfFramePointerReg = DwarfFramePointerReg;
      Part->AuthenticatedObjectExtents = AuthenticatedObjectExtents;
      Part->parseUnits(Units * I / Workers, Units * (I + 1) / Workers);
      // No lazy LLVM reader escapes the worker. Published facts own their
      // names, types and locations; the main context remains the query reader.
      Part->DwarfCtx.reset();
      Parts[I] = std::move(Part);
    };
    runWithLargeStackThreads(Workers, Parse);
    Parsed = std::all_of(Parts.begin(), Parts.end(),
                         [](const auto &Part) { return bool(Part); });
    if (Parsed) {
      auto Merge = [](auto &Into, auto &From) {
        for (auto &[Key, Value] : From)
          Into.insert_or_assign(Key, std::move(Value));
      };
      // Source order, including duplicate entries, is identical to serial
      // ingestion. The global extent index sees every DIE before it decides
      // whether any object's bounds or return type can be trusted.
      for (auto &Part : Parts) {
        Merge(FuncMap, Part->FuncMap);
        Merge(ReturnValueStates, Part->ReturnValueStates);
        Merge(RecordParameters, Part->RecordParameters);
        Merge(Functions, Part->Functions);
        FuncList.insert(FuncList.end(),
                        std::make_move_iterator(Part->FuncList.begin()),
                        std::make_move_iterator(Part->FuncList.end()));
        DataObjects.insert(DataObjects.end(),
                           std::make_move_iterator(Part->DataObjects.begin()),
                           std::make_move_iterator(Part->DataObjects.end()));
        SubprogramExtents.append(std::move(Part->SubprogramExtents));
      }
    }
  }
  if (!Parsed)
    parseUnits(0, Units);

  SubprogramExtents.finalize();
  std::sort(FuncList.begin(), FuncList.end(),
            [](const FunctionSym &A, const FunctionSym &B) {
              return A.Addr < B.Addr;
            });
  indexFunctionRanges();
  indexDataObjects();
  // The units parsed above keep every DIE. Signatures and frame bases no
  // longer reference them, so the next query starts from the mapped file.
  if (auto Fresh = llvm::DWARFContext::create(InputObject))
    DwarfCtx = std::move(Fresh);
}

const std::vector<DWARFDebugContext::Impl::LocalObject> &
DWARFDebugContext::Impl::localsFor(va_t Entry,
                                   const FunctionInfo &Function) const {
  if (CachedLocalValid && CachedLocalEntry == Entry)
    return CachedLocals;
  CachedLocals.clear();
  CachedLocalEntry = Entry;
  CachedLocalValid = true;
  if (!Function.HasLocalDie || !Object)
    return CachedLocals;
  if (!LocalUnitReady || LocalUnitIndex != Function.LocalUnitIndex ||
      !LocalCtx || !LocalUnit) {
    LocalCtx = llvm::DWARFContext::create(*Object);
    LocalUnit = nullptr;
    LocalUnitReady = false;
    if (!LocalCtx)
      return CachedLocals;
    unsigned Seen = 0;
    for (auto &CU : LocalCtx->compile_units()) {
      if (Seen++ != Function.LocalUnitIndex)
        continue;
      LocalUnit = CU.get();
      LocalUnitIndex = Function.LocalUnitIndex;
      LocalUnitReady = true;
      break;
    }
  }
  if (!LocalUnit)
    return CachedLocals;
  const llvm::DWARFDie Die =
      LocalUnit->getDIEForOffset(Function.LocalDieOffset);
  if (!Die.isValid())
    return CachedLocals;
  parseFunctionLocals(Die, Function.Ranges, CachedLocals);
  return CachedLocals;
}

void DWARFDebugContext::Impl::indexDataObjects() {
  DataObjectByAddress.resize(DataObjects.size());
  for (size_t Index = 0; Index < DataObjects.size(); ++Index)
    DataObjectByAddress[Index] = Index;
  std::stable_sort(DataObjectByAddress.begin(), DataObjectByAddress.end(),
                   [&](size_t Left, size_t Right) {
                     return DataObjects[Left].Addr < DataObjects[Right].Addr;
                   });
  // Duplicate declarations may name one storage object. Agree once while
  // indexing, so every later name/type lookup sees the same evidence.
  for (size_t Begin = 0, End; Begin < DataObjectByAddress.size(); Begin = End) {
    End = Begin + 1;
    const auto &First = DataObjects[DataObjectByAddress[Begin]];
    TypeRef Type = First.Type;
    bool Conflict = false;
    while (End < DataObjectByAddress.size() &&
           DataObjects[DataObjectByAddress[End]].Addr == First.Addr) {
      const TypeRef &Other = DataObjects[DataObjectByAddress[End]].Type;
      if (Type && Other && !equalSourceTypes(Type, Other))
        Conflict = true;
      else if (!Type)
        Type = Other;
      ++End;
    }
    for (size_t I = Begin; I < End; ++I)
      DataObjects[DataObjectByAddress[I]].Type = Conflict ? nullptr : Type;
  }
}

void DWARFDebugContext::Impl::indexFunctionRanges() {
  FunctionRanges.clear();
  for (const auto &[Entry, Info] : Functions)
    for (const auto &[Begin, End] : Info.Ranges)
      if (Begin < End)
        FunctionRanges.push_back({Begin, End, Entry});
  std::sort(FunctionRanges.begin(), FunctionRanges.end(),
            [](const FunctionRange &A, const FunctionRange &B) {
              return A.Begin < B.Begin;
            });
  FunctionRangeEndsSoFar.resize(FunctionRanges.size());
  va_t Greatest = 0;
  for (size_t I = 0; I < FunctionRanges.size(); ++I)
    FunctionRangeEndsSoFar[I] = Greatest =
        std::max(Greatest, FunctionRanges[I].End);
}

llvm::SmallVector<va_t, 4>
DWARFDebugContext::Impl::functionsContaining(va_t PC) const {
  llvm::SmallVector<va_t, 4> Entries;
  // Of the ranges that start at or before PC, those that end after it
  // contain it; none does before the last range whose end so far is past PC.
  size_t I = std::upper_bound(FunctionRanges.begin(), FunctionRanges.end(), PC,
                              [](va_t Value, const FunctionRange &Range) {
                                return Value < Range.Begin;
                              }) -
             FunctionRanges.begin();
  while (I > 0 && FunctionRangeEndsSoFar[I - 1] > PC) {
    --I;
    if (FunctionRanges[I].End > PC)
      Entries.push_back(FunctionRanges[I].Entry);
  }
  std::sort(Entries.begin(), Entries.end());
  Entries.erase(std::unique(Entries.begin(), Entries.end()), Entries.end());
  return Entries;
}

VariableExtentLookup DWARFDebugContext::Impl::lookupLocal(
    va_t FuncAddr, va_t UsePC, int64_t Offset,
    StackCoordinate RequestedCoordinate) const {
  std::lock_guard<std::mutex> Guard(ContextMutex);
  const FunctionInfo *Function = nullptr;
  va_t LocalEntry = FuncAddr;
  if (auto It = Functions.find(FuncAddr); It != Functions.end()) {
    if (SubprogramExtents.isAmbiguous(It->first))
      return VariableExtentLookup::ambiguous();
    Function = &It->second;
  } else {
    for (const va_t Entry : functionsContaining(FuncAddr)) {
      if (SubprogramExtents.isAmbiguous(Entry))
        return VariableExtentLookup::ambiguous();
      if (Function)
        return VariableExtentLookup::ambiguous();
      Function = &Functions.at(Entry);
      LocalEntry = Entry;
    }
  }
  if (!Function)
    return VariableExtentLookup::notFound();
  if (UsePC == InvalidVA || !containsPC(UsePC, Function->Ranges))
    return VariableExtentLookup::ambiguous();

  auto selectLocation = [&](llvm::ArrayRef<Location> Locations,
                            bool &Ambiguous) -> std::optional<Location> {
    std::vector<const Location *> Selected;
    for (const Location &Location : Locations)
      if (Location.Range && UsePC >= Location.Range->first &&
          UsePC < Location.Range->second)
        Selected.push_back(&Location);
    if (Selected.empty()) {
      for (const Location &Location : Locations)
        if (!Location.Range)
          Selected.push_back(&Location);
    }
    if (Selected.empty())
      return std::nullopt;

    const Location &First = *Selected.front();
    if (First.Coordinate == StackCoordinate::Malformed) {
      Ambiguous = true;
      return std::nullopt;
    }
    for (const Location *Location : Selected) {
      if (Location->Coordinate == StackCoordinate::Malformed ||
          Location->Coordinate != First.Coordinate ||
          Location->Offset != First.Offset) {
        Ambiguous = true;
        return std::nullopt;
      }
    }
    return First;
  };

  struct ResolvedObject {
    VariableSym Variable;
    int64_t Base = 0;
    int64_t End = 0;
  };
  std::vector<ResolvedObject> Objects;
  std::optional<VariableSym> RegisterVariable;
  for (const LocalObject &Object : localsFor(LocalEntry, *Function)) {
    if (!containsPC(UsePC, Object.ScopeRanges))
      continue;
    // An entry-value expression, stack object or register fragment is not
    // a whole live-register declaration. Its unsupported stack-coordinate
    // shape must not poison an unrelated register's exact declaration.
    if (RequestedCoordinate == StackCoordinate::Register &&
        llvm::none_of(Object.Locations, [&](const Location &L) {
          return L.Coordinate == StackCoordinate::Register &&
                 dwarfIntegerRegister(MachineArch, L.Offset) ==
                     uint64_t(Offset) &&
                 (!L.Range ||
                  (L.Range->first <= UsePC && UsePC < L.Range->second));
        }))
      continue;

    bool Ambiguous = false;
    std::optional<Location> SelectedLocation =
        selectLocation(Object.Locations, Ambiguous);
    if (Ambiguous)
      return VariableExtentLookup::ambiguous();
    if (!SelectedLocation ||
        SelectedLocation->Coordinate == StackCoordinate::NonStack)
      continue;

    StackCoordinate Coordinate = SelectedLocation->Coordinate;
    int64_t Base = SelectedLocation->Offset;
    if (RequestedCoordinate == StackCoordinate::Register) {
      if (Coordinate != StackCoordinate::Register ||
          dwarfIntegerRegister(MachineArch, Base) != uint64_t(Offset))
        continue;
      if (RegisterVariable &&
          !equalSourceTypes(RegisterVariable->Type, Object.Variable.Type))
        return VariableExtentLookup::ambiguous();
      RegisterVariable = Object.Variable;
      continue;
    }
    if (Coordinate == StackCoordinate::FrameBase) {
      std::optional<Location> FrameBase =
          selectLocation(Function->FrameBases, Ambiguous);
      int64_t AbsoluteBase = 0;
      if (Ambiguous || !FrameBase ||
          FrameBase->Coordinate == StackCoordinate::NonStack ||
          FrameBase->Coordinate == StackCoordinate::Register ||
          FrameBase->Coordinate == StackCoordinate::FrameBase ||
          FrameBase->Coordinate == StackCoordinate::Malformed ||
          llvm::AddOverflow(FrameBase->Offset, Base, AbsoluteBase))
        return VariableExtentLookup::ambiguous();
      Coordinate = FrameBase->Coordinate;
      Base = AbsoluteBase;
    }
    if (Coordinate != RequestedCoordinate)
      continue;

    const std::optional<uint64_t> Size = declaredTypeSize(Object.Variable.Type);
    if (!Size ||
        *Size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return VariableExtentLookup::ambiguous();
    int64_t End = 0;
    if (llvm::AddOverflow(Base, static_cast<int64_t>(*Size), End))
      return VariableExtentLookup::ambiguous();

    ResolvedObject Resolved;
    Resolved.Variable = Object.Variable;
    Resolved.Variable.StackOffset = Base;
    Resolved.Base = Base;
    Resolved.End = End;
    Objects.push_back(std::move(Resolved));
  }

  if (RequestedCoordinate == StackCoordinate::Register)
    return RegisterVariable
               ? VariableExtentLookup::unique(std::move(*RegisterVariable))
               : VariableExtentLookup::notFound();

  std::vector<const ResolvedObject *> Candidates;
  for (const ResolvedObject &Object : Objects)
    if (Offset >= Object.Base && Offset <= Object.End)
      Candidates.push_back(&Object);
  if (Candidates.empty())
    return VariableExtentLookup::notFound();

  for (const ResolvedObject *Candidate : Candidates) {
    for (const ResolvedObject &Object : Objects) {
      const bool SameExtent =
          Candidate->Base == Object.Base && Candidate->End == Object.End;
      const bool ClosedOverlap =
          Candidate->Base <= Object.End && Object.Base <= Candidate->End;
      if (!SameExtent && ClosedOverlap)
        return VariableExtentLookup::ambiguous();
    }
  }

  const ResolvedObject *Chosen = Candidates.front();
  for (const ResolvedObject *Candidate : Candidates) {
    if (Candidate->Base != Chosen->Base || Candidate->End != Chosen->End)
      return VariableExtentLookup::ambiguous();
    if (!Candidate->Variable.Type ||
        Candidate->Variable.Type->Kind != NdTypeKind::Array)
      Chosen = Candidate;
  }
  return VariableExtentLookup::unique(Chosen->Variable);
}

// DebugContext interface

std::optional<FunctionSym> DWARFDebugContext::resolveFunction(va_t Addr) const {
  if (!PImpl || !PImpl->Loaded)
    return std::nullopt;
  auto It = PImpl->FuncMap.find(Addr);
  if (It != PImpl->FuncMap.end())
    return It->second;

  const FunctionSym *Resolved = nullptr;
  for (const va_t Entry : PImpl->functionsContaining(Addr)) {
    auto Sym = PImpl->FuncMap.find(Entry);
    if (Sym == PImpl->FuncMap.end() || Resolved)
      return std::nullopt;
    Resolved = &Sym->second;
  }
  return Resolved ? std::optional<FunctionSym>(*Resolved) : std::nullopt;
}

std::optional<VariableSym>
DWARFDebugContext::resolveVariable(va_t FuncAddr, int64_t Offset) const {
  if (!PImpl || !PImpl->Loaded)
    return std::nullopt;
  VariableExtentLookup Result = PImpl->lookupLocal(FuncAddr, FuncAddr, Offset,
                                                   Impl::StackCoordinate::CFA);
  return Result.Status == VariableExtentLookupStatus::Unique
             ? std::optional<VariableSym>(std::move(Result.Variable))
             : std::nullopt;
}

VariableExtentLookup
DWARFDebugContext::resolveVariableAt(va_t FuncAddr, va_t UsePC,
                                     int64_t Offset) const {
  if (!PImpl || !PImpl->Loaded)
    return VariableExtentLookup::notFound();
  return PImpl->lookupLocal(FuncAddr, UsePC, Offset,
                            Impl::StackCoordinate::CFA);
}

std::optional<VariableSym>
DWARFDebugContext::resolveStackPointerVariable(va_t FuncAddr,
                                               int64_t Offset) const {
  if (!PImpl || !PImpl->Loaded)
    return std::nullopt;
  VariableExtentLookup Result = PImpl->lookupLocal(
      FuncAddr, FuncAddr, Offset, Impl::StackCoordinate::StackPointer);
  return Result.Status == VariableExtentLookupStatus::Unique
             ? std::optional<VariableSym>(std::move(Result.Variable))
             : std::nullopt;
}

VariableExtentLookup
DWARFDebugContext::resolveStackPointerVariableAt(va_t FuncAddr, va_t UsePC,
                                                 int64_t Offset) const {
  if (!PImpl || !PImpl->Loaded)
    return VariableExtentLookup::notFound();
  return PImpl->lookupLocal(FuncAddr, UsePC, Offset,
                            Impl::StackCoordinate::StackPointer);
}

VariableExtentLookup
DWARFDebugContext::resolveFramePointerVariableAt(va_t FuncAddr, va_t UsePC,
                                                 int64_t Offset) const {
  if (!PImpl || !PImpl->Loaded)
    return VariableExtentLookup::notFound();
  return PImpl->lookupLocal(FuncAddr, UsePC, Offset,
                            Impl::StackCoordinate::FramePointer);
}

VariableExtentLookup
DWARFDebugContext::resolveRegisterVariableAt(va_t FuncAddr, va_t UsePC,
                                             uint64_t Register) const {
  if (!PImpl || !PImpl->Loaded ||
      Register > uint64_t(std::numeric_limits<int64_t>::max()))
    return VariableExtentLookup::notFound();
  return PImpl->lookupLocal(FuncAddr, UsePC, int64_t(Register),
                            Impl::StackCoordinate::Register);
}

std::optional<TypeSym> DWARFDebugContext::resolveType(uint64_t) const {
  return std::nullopt;
}

std::optional<SourceLoc> DWARFDebugContext::sourceLocation(va_t Addr) const {
  if (!PImpl || !PImpl->Loaded || !PImpl->DwarfCtx)
    return std::nullopt;

  std::lock_guard<std::mutex> Guard(PImpl->ContextMutex);
  auto Info = PImpl->DwarfCtx->getLineInfoForAddress(
      {Addr, llvm::object::SectionedAddress::UndefSection});
  if (!Info || (Info->FileName.empty() && Info->Line == 0))
    return std::nullopt;

  SourceLoc Loc;
  Loc.File = Info->FileName;
  Loc.Line = Info->Line;
  Loc.Col = Info->Column;
  return Loc;
}

llvm::Error DWARFDebugContext::withDWARFContext(
    llvm::function_ref<llvm::Error(llvm::DWARFContext &)> Use) const {
  if (!PImpl || !PImpl->Loaded || !PImpl->DwarfCtx)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "no DWARF was read");
  std::lock_guard<std::mutex> Guard(PImpl->ContextMutex);
  // A whole-unit walk populates every lazy DIE cache. Its consumer keeps
  // owned facts, so release that reader when the walk ends rather than
  // retaining the full debug graph beside the imported type library. The
  // line and local-variable query readers keep their independent caches.
  auto Reader = llvm::DWARFContext::create(*PImpl->Object);
  return Use(*Reader);
}

std::vector<FunctionSym> DWARFDebugContext::allFunctions() const {
  if (!PImpl || !PImpl->Loaded)
    return {};
  return PImpl->FuncList;
}

std::vector<DataObjectSym> DWARFDebugContext::allDataObjects() const {
  if (!PImpl || !PImpl->Loaded)
    return {};
  return PImpl->DataObjects;
}

std::optional<DataObjectSym>
DWARFDebugContext::resolveDataObject(va_t Addr) const {
  if (!PImpl || !PImpl->Loaded)
    return std::nullopt;
  const std::vector<DataObjectSym> &Objects = PImpl->DataObjects;
  const std::vector<size_t> &Order = PImpl->DataObjectByAddress;
  const auto Begin = std::lower_bound(
      Order.begin(), Order.end(), Addr,
      [&](size_t Index, va_t Value) { return Objects[Index].Addr < Value; });
  const auto End =
      std::upper_bound(Begin, Order.end(), Addr, [&](va_t Value, size_t Index) {
        return Value < Objects[Index].Addr;
      });
  for (auto Index = Begin; Index != End; ++Index)
    if (!Objects[*Index].Name.empty())
      return Objects[*Index];
  return std::nullopt;
}

std::pair<std::optional<va_t>, std::optional<va_t>>
DWARFDebugContext::dataSymbolsAround(va_t Addr) const {
  if (!PImpl || !PImpl->Loaded)
    return {};
  const std::vector<DataObjectSym> &Objects = PImpl->DataObjects;
  const std::vector<size_t> &Order = PImpl->DataObjectByAddress;
  const auto After = std::upper_bound(
      Order.begin(), Order.end(), Addr,
      [&](va_t Value, size_t Index) { return Value < Objects[Index].Addr; });
  std::optional<va_t> BeforeAddr;
  std::optional<va_t> AfterAddr;
  if (After != Order.begin())
    BeforeAddr = Objects[*std::prev(After)].Addr;
  if (After != Order.end())
    AfterAddr = Objects[*After].Addr;
  return {BeforeAddr, AfterAddr};
}

bool DWARFDebugContext::hasAuthenticatedFunctionSignatures() const {
  // Function declarations and object extents share the same exact-image / dSYM
  // UUID authentication gate.  Their semantic capabilities stay separate at
  // the DebugContext boundary so a future names-only or types-only provider
  // cannot accidentally borrow the other one.
  return PImpl && PImpl->Loaded && PImpl->AuthenticatedObjectExtents;
}

AuthenticatedReturnValueState
DWARFDebugContext::resolveAuthenticatedReturnValueState(va_t Entry) const {
  if (!PImpl || !PImpl->Loaded || !PImpl->AuthenticatedObjectExtents ||
      PImpl->SubprogramExtents.isAmbiguous(Entry))
    return {};
  const auto It = PImpl->ReturnValueStates.find(Entry);
  return It == PImpl->ReturnValueStates.end() ? AuthenticatedReturnValueState{}
                                              : It->second;
}

bool DWARFDebugContext::hasAuthenticatedObjectExtents() const {
  return PImpl && PImpl->Loaded && PImpl->AuthenticatedObjectExtents;
}

std::vector<AuthenticatedRecordParameter>
DWARFDebugContext::resolveAuthenticatedRecordParameters(va_t Entry) const {
  if (!PImpl || !PImpl->Loaded || !PImpl->AuthenticatedObjectExtents ||
      PImpl->SubprogramExtents.isAmbiguous(Entry))
    return {};
  auto It = PImpl->RecordParameters.find(Entry);
  if (It == PImpl->RecordParameters.end())
    return {};
  std::set<uint64_t> Registers;
  for (const auto &Parameter : It->second)
    if (!Registers.insert(Parameter.Register).second)
      return {};
  return It->second;
}

bool DWARFDebugContext::hasInfo() const { return PImpl && PImpl->Loaded; }

} // namespace neverd
