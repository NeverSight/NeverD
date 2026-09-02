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

#define DEBUG_TYPE "neverd-dwarf-loader"
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
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
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

  for (Record &Existing : Records) {
    if (Existing.Entry != Entry && !rangesOverlap(Existing.Ranges, New.Ranges))
      continue;
    Existing.Ambiguous = true;
    New.Ambiguous = true;
  }
  Records.push_back(std::move(New));
}

bool SubprogramExtentRegistry::isAmbiguous(va_t Entry) const {
  return std::any_of(Records.begin(), Records.end(), [&](const Record &Record) {
    return Record.Entry == Entry && Record.Ambiguous;
  });
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
      finish(Base::NonStack, 0);
    }
  } else if (Opcode == llvm::dwarf::DW_OP_regx && Operands.size() == 1) {
    if (IsFrameBase) {
      if (const auto Coordinate = coordinateForRegister(Operands[0]))
        finish(*Coordinate, 0);
    } else {
      finish(Base::NonStack, 0);
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

using dwarf_loader_detail::strictAttribute;
using dwarf_loader_detail::StrictAttributeLookup;

struct DWARFDebugContext::Impl {
  std::unique_ptr<llvm::MemoryBuffer> BinaryBuf;
  std::unique_ptr<llvm::MemoryBuffer> DSYMBuf;
  std::unique_ptr<llvm::DWARFContext> DwarfCtx;

  std::map<va_t, FunctionSym> FuncMap;
  std::vector<FunctionSym> FuncList;
  std::map<va_t, AuthenticatedReturnValueState> ReturnValueStates;

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
    std::vector<LocalObject> Objects;
  };

  std::map<va_t, FunctionInfo> Functions;
  dwarf_loader_detail::SubprogramExtentRegistry SubprogramExtents;
  std::vector<DataObjectSym> DataObjects;

  std::optional<unsigned> DwarfStackPointerReg;
  std::optional<unsigned> DwarfFramePointerReg;

  bool Loaded = false;
  bool AuthenticatedObjectExtents = false;

  struct ReturnTypeResolution {
    enum class Status : uint8_t { Absent, Found, Malformed };
    Status TheStatus = Status::Absent;
    llvm::DWARFDie TypeDie;
  };

  TypeRef convertDwarfType(llvm::DWARFDie Die, int Depth = 0);
  ReturnTypeResolution resolveReturnType(llvm::DWARFDie Die) const;
  AuthenticatedReturnValueState classifyReturnValue(llvm::DWARFDie Die,
                                                    int Depth = 0) const;
  std::vector<Location> decodeLocations(llvm::DWARFDie Die,
                                        llvm::dwarf::Attribute Attr,
                                        llvm::DWARFUnit *Unit,
                                        bool IsFrameBase) const;
  VariableExtentLookup lookupLocal(va_t FuncAddr, va_t UsePC, int64_t Offset,
                                   StackCoordinate Coordinate) const;
  void parseCompileUnits();
  void parseFunction(llvm::DWARFDie Die, llvm::DWARFUnit *Unit);
  void parseLocalVariable(llvm::DWARFDie Die, va_t FuncAddr, bool IsParam,
                          llvm::ArrayRef<AddressRange> ScopeRanges);
  void parseGlobalVariable(llvm::DWARFDie Die, llvm::DWARFUnit *Unit);
};

DWARFDebugContext::~DWARFDebugContext() = default;

static std::filesystem::path findDSYM(const std::filesystem::path &BinPath) {
  auto DSYMBundle = BinPath;
  DSYMBundle += ".dSYM";
  auto DwarfDir = DSYMBundle / "Contents" / "Resources" / "DWARF";
  if (std::filesystem::exists(DwarfDir)) {
    for (auto &Entry : std::filesystem::directory_iterator(DwarfDir)) {
      if (Entry.is_regular_file())
        return Entry.path();
    }
  }

  auto Parent = BinPath.parent_path();
  if (Parent.empty())
    Parent = ".";
  auto Stem = BinPath.stem().string();
  if (!std::filesystem::exists(Parent))
    return {};
  for (auto &Entry : std::filesystem::directory_iterator(Parent)) {
    if (Entry.path().extension() == ".dSYM" && Entry.is_directory()) {
      auto DSYMStem = Entry.path().stem().string();
      if (DSYMStem != Stem)
        continue;
      auto Inner = Entry.path() / "Contents" / "Resources" / "DWARF";
      if (std::filesystem::exists(Inner)) {
        for (auto &F : std::filesystem::directory_iterator(Inner))
          if (F.is_regular_file())
            return F.path();
      }
    }
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
  default:
    return std::nullopt;
  }
}

std::unique_ptr<DWARFDebugContext>
DWARFDebugContext::load(const std::filesystem::path &BinaryPath,
                        BinaryFormat Format, DWARFLoadTrust Trust,
                        std::span<const uint8_t> ExpectedImageBytes) {
  auto Ctx = std::unique_ptr<DWARFDebugContext>(new DWARFDebugContext());
  Ctx->PImpl = std::make_unique<Impl>();

  auto BufOr = llvm::MemoryBuffer::getFile(BinaryPath.string());
  if (!BufOr) {
    llvm::WithColor::warning()
        << "debug: cannot open " << BinaryPath.string() << "\n";
    return Ctx;
  }
  Ctx->PImpl->BinaryBuf = std::move(*BufOr);

  auto Obj = getObjectFromBuffer(*Ctx->PImpl->BinaryBuf, std::nullopt,
                                 ExpectedImageBytes);
  if (!Obj) {
    llvm::WithColor::warning()
        << "debug: cannot parse object file " << BinaryPath.string() << "\n";
    return Ctx;
  }

  const llvm::Triple::ArchType MainArch = Obj->getArch();
  const std::optional<std::array<uint8_t, 16>> MainUUID = machOUUID(*Obj);
  const bool SnapshotMatches = objectBytesEqual(*Obj, ExpectedImageBytes);
  Ctx->PImpl->DwarfStackPointerReg = dwarfStackPointerRegister(MainArch);
  Ctx->PImpl->DwarfFramePointerReg = dwarfFramePointerRegister(MainArch);

  Ctx->PImpl->DwarfCtx = llvm::DWARFContext::create(*Obj);
  if (!Ctx->PImpl->DwarfCtx) {
    llvm::WithColor::warning() << "debug: cannot create DWARFContext for "
                               << BinaryPath.string() << "\n";
    return Ctx;
  }

  bool HasDwarf = Ctx->PImpl->DwarfCtx->getNumCompileUnits() > 0;
  Ctx->PImpl->AuthenticatedObjectExtents =
      HasDwarf && Trust == DWARFLoadTrust::InImage && SnapshotMatches;

  if (!HasDwarf && Format == BinaryFormat::MachO) {
    auto DSYMPath = findDSYM(BinaryPath);
    if (!DSYMPath.empty()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "debug: loading dSYM from " << DSYMPath.string() << "\n");
      auto DSYMBufOr = llvm::MemoryBuffer::getFile(DSYMPath.string());
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
        }
      }
    }
  }

  if (!HasDwarf) {
    LLVM_DEBUG(llvm::dbgs() << "debug: no DWARF info found for "
                            << BinaryPath.string() << "\n");
    return Ctx;
  }

  LLVM_DEBUG(llvm::dbgs() << "debug: "
                          << Ctx->PImpl->DwarfCtx->getNumCompileUnits()
                          << " compile units found" << "\n");
  Ctx->PImpl->parseCompileUnits();
  Ctx->PImpl->Loaded = true;

  LLVM_DEBUG(llvm::dbgs() << "debug: loaded " << Ctx->PImpl->FuncList.size()
                          << " functions with debug info" << "\n");
  return Ctx;
}

TypeRef DWARFDebugContext::Impl::convertDwarfType(llvm::DWARFDie Die,
                                                  int Depth) {
  if (!Die.isValid() || Depth > 16)
    return NdType::makeInt(4);

  auto Tag = Die.getTag();
  switch (Tag) {
  case llvm::dwarf::DW_TAG_base_type: {
    auto SizeAttr = Die.find(llvm::dwarf::DW_AT_byte_size);
    auto Encoding = Die.find(llvm::dwarf::DW_AT_encoding);
    uint16_t Sz = SizeAttr ? static_cast<uint16_t>(
                                 SizeAttr->getAsUnsignedConstant().value_or(4))
                           : 4;
    if (Encoding) {
      auto Enc = Encoding->getAsUnsignedConstant().value_or(0);
      if (Enc == llvm::dwarf::DW_ATE_float)
        return NdType::makeFloat(Sz);
      bool IsSigned = (Enc == llvm::dwarf::DW_ATE_signed ||
                       Enc == llvm::dwarf::DW_ATE_signed_char);
      return NdType::makeInt(Sz, IsSigned);
    }
    return NdType::makeInt(Sz);
  }
  case llvm::dwarf::DW_TAG_pointer_type: {
    auto PointeeDie =
        Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    auto PT = PointeeDie.isValid() ? convertDwarfType(PointeeDie, Depth + 1)
                                   : NdType::makeVoid();
    return NdType::makePtr(PT);
  }
  case llvm::dwarf::DW_TAG_typedef:
  case llvm::dwarf::DW_TAG_const_type:
  case llvm::dwarf::DW_TAG_volatile_type:
  case llvm::dwarf::DW_TAG_restrict_type: {
    auto Ref = Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    return Ref.isValid() ? convertDwarfType(Ref, Depth + 1)
                         : NdType::makeVoid();
  }
  case llvm::dwarf::DW_TAG_array_type: {
    auto ElemDie =
        Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    auto Elem = ElemDie.isValid() ? convertDwarfType(ElemDie, Depth + 1)
                                  : NdType::makeInt(1, false);
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
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Array;
    T->ArrayCount = Count;
    T->ElemType = Elem;
    T->Size = static_cast<uint16_t>(Count * Elem->Size);
    return T;
  }
  case llvm::dwarf::DW_TAG_structure_type:
  case llvm::dwarf::DW_TAG_class_type:
  case llvm::dwarf::DW_TAG_union_type: {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Struct;
    auto Sz = Die.find(llvm::dwarf::DW_AT_byte_size);
    T->Size =
        Sz ? static_cast<uint16_t>(Sz->getAsUnsignedConstant().value_or(0)) : 0;
    return T;
  }
  case llvm::dwarf::DW_TAG_subroutine_type: {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Func;
    T->Size = 8;
    auto RetDie = Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    T->RetType = RetDie.isValid() ? convertDwarfType(RetDie, Depth + 1)
                                  : NdType::makeVoid();
    for (auto Child : Die.children()) {
      if (Child.getTag() == llvm::dwarf::DW_TAG_formal_parameter) {
        auto ParamDie =
            Child.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
        T->ParamTypes.push_back(ParamDie.isValid()
                                    ? convertDwarfType(ParamDie, Depth + 1)
                                    : NdType::makeInt(4));
      }
    }
    return T;
  }
  case llvm::dwarf::DW_TAG_enumeration_type: {
    auto Sz = Die.find(llvm::dwarf::DW_AT_byte_size);
    uint16_t S =
        Sz ? static_cast<uint16_t>(Sz->getAsUnsignedConstant().value_or(4)) : 4;
    return NdType::makeInt(S);
  }
  default:
    return NdType::makeInt(4);
  }
}

DWARFDebugContext::Impl::ReturnTypeResolution
DWARFDebugContext::Impl::resolveReturnType(llvm::DWARFDie Die) const {
  using Resolution = ReturnTypeResolution;
  std::vector<llvm::DWARFDie> Active;
  std::function<Resolution(llvm::DWARFDie, unsigned)> Resolve =
      [&](llvm::DWARFDie Current, unsigned Depth) -> Resolution {
    if (!Current.isValid() || Depth > 32)
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

AuthenticatedReturnValueState
DWARFDebugContext::Impl::classifyReturnValue(llvm::DWARFDie Die,
                                             int Depth) const {
  using Kind = AuthenticatedReturnKind;
  if (!Die.isValid() || Depth > 16)
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

} // namespace

void DWARFDebugContext::Impl::parseLocalVariable(
    llvm::DWARFDie Die, va_t FuncAddr, bool IsParam,
    llvm::ArrayRef<AddressRange> ScopeRanges) {
  auto TypeDie = Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
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
    Functions[FuncAddr].Objects.push_back(std::move(Object));
}

void DWARFDebugContext::Impl::parseGlobalVariable(llvm::DWARFDie Die,
                                                  llvm::DWARFUnit *Unit) {
  auto TypeDie = Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
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
  DataObjects.push_back(std::move(Object));
}

void DWARFDebugContext::Impl::parseFunction(llvm::DWARFDie Die,
                                            llvm::DWARFUnit *Unit) {
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
  const ReturnTypeResolution ReturnType = resolveReturnType(Die);
  if (ReturnType.TheStatus == ReturnTypeResolution::Status::Absent) {
    // DWARF encodes a source-level void return by omitting DW_AT_type.
    ReturnState = {AuthenticatedReturnKind::NoValue, 0};
    Sym.ReturnType = NdType::makeVoid();
  } else if (ReturnType.TheStatus == ReturnTypeResolution::Status::Found) {
    ReturnState = classifyReturnValue(ReturnType.TypeDie);
    Sym.ReturnType = convertDwarfType(ReturnType.TypeDie);
    if (!Sym.ReturnType || Sym.ReturnType->Kind == NdTypeKind::Unknown)
      ReturnState = {};
  } else {
    // A present but malformed reference is not evidence of void.
    Sym.ReturnType = std::make_shared<NdType>();
  }
  for (llvm::DWARFDie Child : Die.children()) {
    if (Child.getTag() != llvm::dwarf::DW_TAG_formal_parameter)
      continue;
    std::string Name;
    if (const char *RawName = Child.getName(llvm::DINameKind::ShortName))
      Name = RawName;
    auto ParamDie =
        Child.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    Sym.Params.emplace_back(std::move(Name), ParamDie.isValid()
                                                 ? convertDwarfType(ParamDie)
                                                 : NdType::makeInt(4));
  }

  FunctionInfo &Info = Functions[Addr];
  Info = FunctionInfo{};
  Info.Ranges = Ranges;
  Info.FrameBases =
      decodeLocations(Die, llvm::dwarf::DW_AT_frame_base, Unit, true);

  std::function<void(llvm::DWARFDie, const std::vector<AddressRange> &)> Walk =
      [&](llvm::DWARFDie Scope, const std::vector<AddressRange> &ScopeRanges) {
        for (llvm::DWARFDie Child : Scope.children()) {
          const auto Tag = Child.getTag();
          if (Tag == llvm::dwarf::DW_TAG_variable ||
              Tag == llvm::dwarf::DW_TAG_formal_parameter) {
            parseLocalVariable(Child, Addr,
                               Tag == llvm::dwarf::DW_TAG_formal_parameter,
                               ScopeRanges);
            continue;
          }
          if (Tag != llvm::dwarf::DW_TAG_lexical_block &&
              Tag != llvm::dwarf::DW_TAG_inlined_subroutine)
            continue;

          std::vector<AddressRange> ChildRanges = ScopeRanges;
          llvm::Expected<llvm::DWARFAddressRangesVector> RawChildRanges =
              Child.getAddressRanges();
          if (!RawChildRanges) {
            llvm::consumeError(RawChildRanges.takeError());
            LocalObject Poison;
            Poison.Variable.Type = NdType::makeInt(1, false);
            Poison.ScopeRanges = ScopeRanges;
            Poison.Locations.push_back(
                Location{std::nullopt, StackCoordinate::Malformed, 0});
            Info.Objects.push_back(std::move(Poison));
            continue;
          }
          if (!RawChildRanges->empty()) {
            ChildRanges.clear();
            for (const llvm::DWARFAddressRange &ChildRange : *RawChildRanges) {
              if (!ChildRange.valid() || ChildRange.LowPC >= ChildRange.HighPC)
                continue;
              for (const AddressRange &ParentRange : ScopeRanges) {
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
  Walk(Die, Ranges);

  SubprogramExtents.insert(Addr, Ranges);
  ReturnValueStates[Addr] = ReturnState;
  FuncMap[Addr] = Sym;
  FuncList.push_back(Sym);
}

void DWARFDebugContext::Impl::parseCompileUnits() {
  for (auto &CU : DwarfCtx->compile_units()) {
    auto Root = CU->getUnitDIE(false);
    if (!Root.isValid())
      continue;

    std::function<void(llvm::DWARFDie)> Walk = [&](llvm::DWARFDie Die) {
      if (!Die.isValid())
        return;
      if (Die.getTag() == llvm::dwarf::DW_TAG_subprogram) {
        parseFunction(Die, CU.get());
        return;
      }
      if (Die.getTag() == llvm::dwarf::DW_TAG_variable)
        parseGlobalVariable(Die, CU.get());
      for (auto Child : Die.children())
        Walk(Child);
    };
    Walk(Root);
  }

  std::sort(FuncList.begin(), FuncList.end(),
            [](const FunctionSym &A, const FunctionSym &B) {
              return A.Addr < B.Addr;
            });
}

VariableExtentLookup DWARFDebugContext::Impl::lookupLocal(
    va_t FuncAddr, va_t UsePC, int64_t Offset,
    StackCoordinate RequestedCoordinate) const {
  const FunctionInfo *Function = nullptr;
  if (auto It = Functions.find(FuncAddr); It != Functions.end()) {
    if (SubprogramExtents.isAmbiguous(It->first))
      return VariableExtentLookup::ambiguous();
    Function = &It->second;
  } else {
    for (const auto &[Entry, Candidate] : Functions) {
      if (!containsPC(FuncAddr, Candidate.Ranges))
        continue;
      if (SubprogramExtents.isAmbiguous(Entry))
        return VariableExtentLookup::ambiguous();
      if (Function)
        return VariableExtentLookup::ambiguous();
      Function = &Candidate;
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
  for (const LocalObject &Object : Function->Objects) {
    if (!containsPC(UsePC, Object.ScopeRanges))
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
    if (Coordinate == StackCoordinate::FrameBase) {
      std::optional<Location> FrameBase =
          selectLocation(Function->FrameBases, Ambiguous);
      int64_t AbsoluteBase = 0;
      if (Ambiguous || !FrameBase ||
          FrameBase->Coordinate == StackCoordinate::NonStack ||
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
  for (const auto &[Entry, Info] : PImpl->Functions) {
    if (!containsPC(Addr, Info.Ranges))
      continue;
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

std::optional<TypeSym> DWARFDebugContext::resolveType(uint64_t) const {
  return std::nullopt;
}

std::optional<SourceLoc> DWARFDebugContext::sourceLocation(va_t Addr) const {
  if (!PImpl || !PImpl->Loaded || !PImpl->DwarfCtx)
    return std::nullopt;

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

bool DWARFDebugContext::hasInfo() const { return PImpl && PImpl->Loaded; }

} // namespace neverd
